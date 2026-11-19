#include "warp_handler.h"
#include "transport.h"
#ifdef MESSAGE_USE_ISA
#include <forza_isa_abi.h>
#endif

static_assert(MESSAGE_BATCH_SIZE == 8 || MESSAGE_BATCH_SIZE == 32, "bounded range batch");

static inline uint64_t send_token(uint32_t destination, uint64_t token) {
#ifdef MESSAGE_USE_ISA
  uint64_t route = forza_isa_route(destination, 0, 0);
#ifdef MESSAGE_WAIT
  return forza_isa_send_wait(route, token, transport::local());
#else
  return forza_isa_send_try(route, token, transport::local());
#endif
#else
  return transport::send(destination, 0, 0, token);
#endif
}

static inline uint64_t pop_token() {
#ifdef MESSAGE_USE_ISA
#ifdef MESSAGE_WAIT
  return forza_isa_recv_wait(0, transport::local());
#else
  return forza_isa_recv_try(0, transport::local());
#endif
#else
  return transport::pop_descriptor();
#endif
}

static inline uint64_t source_credits() {
#ifdef MESSAGE_USE_ISA
  return forza_isa_credits(transport::local());
#else
  return transport::credits();
#endif
}

#ifndef MESSAGE_COOPERATIVE_PRODUCER
__attribute__((noinline)) static void produce(args_t* __UNIFORM__ a, result_t* out) {
  const uint32_t lane = vx_thread_id(), core = vx_core_id();
  const uint32_t clients = a->cores * 4, requests = a->requests;
  const uint32_t owner_mask = a->cores - 1, limit = a->limit;
  const uint64_t input = a->input;
  uint32_t cursor = (core * 4 + lane) * MESSAGE_BATCH_SIZE;
  uint32_t pending_owners = 0;
  uint64_t token = 0, sent = 0, retries = 0, credit = 0, scanned = 0;
  bool loaded = false, complete = false;
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (cursor < requests) {
      if (!loaded) {
        uint32_t count = requests - cursor;
        if (count > MESSAGE_BATCH_SIZE) count = MESSAGE_BATCH_SIZE;
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t key = uint32_t(reinterpret_cast<const uint64_t*>(input)[uint64_t(cursor + i) * 2]);
          pending_owners |= uint32_t(1) << (key & owner_mask);
          ++scanned;
        }
        token = (uint64_t(count) << 32) | uint64_t(cursor + 1);
        loaded = true;
      }
      uint32_t destination = __builtin_ctz(pending_owners);
      uint64_t status = send_token(destination, token);
      if (status == FORZA_MMIO_ACCEPTED) {
        pending_owners &= pending_owners - 1;
        ++sent;
        if (!pending_owners) {
          cursor += clients * MESSAGE_BATCH_SIZE;
          loaded = false;
        }
      } else if (status == FORZA_MMIO_BUSY) ++retries;
      else out->error = 2;
    }
    bool done = false;
    if (cursor >= requests) {
      credit = source_credits() & 255;
      done = credit == 0;
    }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 3;
  out->sent = sent; out->retries = retries; out->outstanding = credit; out->scanned = scanned;
}

#endif

#ifdef MESSAGE_COOPERATIVE_PRODUCER
__attribute__((noinline)) static void produce_cooperative(args_t* __UNIFORM__ a, result_t* out) {
  const uint32_t lane = vx_thread_id(), core = vx_core_id();
  const uint32_t cores = a->cores, requests = a->requests, limit = a->limit;
  const uint64_t input = a->input;
  uint32_t cursor = core * MESSAGE_BATCH_SIZE;
  uint64_t sent = 0, retries = 0, credit = 0, scanned = 0;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (cursor < requests) {
      uint32_t count = requests - cursor;
      if (count > MESSAGE_BATCH_SIZE) count = MESSAGE_BATCH_SIZE;
      uint32_t owners = 0;
      for (uint32_t block = 0; block < count; block += 4) {
        uint32_t index = block + lane;
        if (index < count) {
          uint32_t key = uint32_t(reinterpret_cast<const uint64_t*>(input)[uint64_t(cursor + index) * 2]);
          owners |= uint32_t(1) << (key & (cores - 1));
        }
      }
      owners |= vx_shfl_bfly(owners, 1, VX_QUAD_CVAL, VX_QUAD_MASK);
      owners |= vx_shfl_bfly(owners, 2, VX_QUAD_CVAL, VX_QUAD_MASK);
      uint32_t pending = vx_shfl_idx(owners, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
      uint64_t token = (uint64_t(count) << 32) | uint64_t(cursor + 1);
      for (uint32_t attempt = 0; pending && attempt < limit; ++attempt) {
        uint64_t status = 0;
        if (lane == 0) status = send_token(__builtin_ctz(pending), token);
        uint64_t __UNIFORM__ result = vx_shfl_idx(status, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        if (result == FORZA_MMIO_ACCEPTED) { pending &= pending - 1; ++sent; }
        else if (result == FORZA_MMIO_BUSY) ++retries;
        else { out->error = 2; break; }
      }
      if (pending) { out->error = 3; break; }
      scanned += count;
      cursor += cores * MESSAGE_BATCH_SIZE;
    }
    bool done = false;
    if (cursor >= requests) {
      if (lane == 0) credit = source_credits() & 255;
      uint64_t __UNIFORM__ current = vx_shfl_idx(credit, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
      done = current == 0;
    }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 3;
  out->sent = lane == 0 ? sent : 0; out->retries = lane == 0 ? retries : 0;
  out->outstanding = lane == 0 ? credit : 0; out->scanned = lane == 0 ? scanned : 0;
}
#endif

__attribute__((noinline)) static void serve(args_t* __UNIFORM__ a, result_t* out) {
  const uint32_t lane = vx_thread_id(), core = vx_core_id();
  const uint32_t per_core = a->per_core, owner_mask = a->cores - 1;
  const uint32_t requests = a->requests, workers = a->cores * per_core, limit = a->limit;
  const uint64_t input = a->input;
  auto counts = reinterpret_cast<const uint64_t*>(a->counts);
  const uint64_t expected_work = counts[core * per_core];
  const uint64_t expected_messages = counts[workers + core * per_core];
  uint64_t handled = 0, received = 0, scanned = 0;
  bool complete = false;
  WarpHandler handler(a);
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (received == expected_messages) { complete = true; break; }
    uint64_t token = 0;
    if (lane == 0) token = pop_token();
    uint64_t __UNIFORM__ batch = vx_shfl_idx(token, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
    if (batch) {
      uint32_t __UNIFORM__ first = uint32_t(batch) - 1;
      uint32_t __UNIFORM__ count = uint32_t(batch >> 32);
      if (!uint32_t(batch) || !count || count > MESSAGE_BATCH_SIZE
          || first >= requests || count > requests - first) out->error = 4;
      else {
        uint32_t matched = 0;
        for (uint32_t block = 0; block < count; block += 4) {
          uint32_t index = block + lane;
          request_t request{};
          if (index < count) request = load_request(reinterpret_cast<const request_t*>(input), first + index);
          bool valid = index < count && (request.key & owner_mask) == core;
          uint32_t work = handler.apply(request, valid);
          if (work == UINT32_MAX) out->error = 5;
          else { handled += work; matched += work; }
        }
        if (!matched) out->error = 8;
        scanned += count;
      }
      ++received;
    }
  }
  if (!complete && received == expected_messages) complete = true;
  if (!complete || handled != expected_work) out->error = 6;
  handler.flush();
  out->processed = lane == 0 ? handled : 0;
  out->received = lane == 0 ? received : 0;
  out->scanned = lane == 0 ? scanned : 0;
}

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  const uint32_t core = vx_core_id(), local = transport::local(), rank = rank_of(a);
  const uint32_t words = (a->keys >> a->core_shift) * 3, per_core = a->per_core;
  const uint32_t shift = a->core_shift, records = a->keys >> a->core_shift;
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  auto state = reinterpret_cast<record_t*>(a->state);
  out->rank = rank;
#ifdef MESSAGE_USE_ISA
  if (forza_isa_register((uint64_t(local) << 8) | 1, local) != FORZA_MMIO_ACCEPTED) out->error = 1;
#else
  if (!transport::init(1)) out->error = 1;
#endif
  if (a->batch_size != MESSAGE_BATCH_SIZE) out->error = 1;
  for (uint32_t i = local; i < words; i += per_core) swap64(scratch + i, 0);
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (vx_warp_id() == 0) serve(a, out);
  else {
#ifdef MESSAGE_COOPERATIVE_PRODUCER
    produce_cooperative(a, out);
#else
    produce(a, out);
#endif
  }
  if (!transport::healthy()) out->error = 7;
  barrier(2);
  uint64_t finish = vx_rdcycle();
  for (uint32_t offset = local; offset < records; offset += per_core) {
    uint32_t key = (offset << shift) + core;
    state[key].count = scratch[offset * 3]; state[key].sum = scratch[offset * 3 + 1]; state[key].xor_value = scratch[offset * 3 + 2];
  }
  barrier(3);
  out->start = start; out->initialized = start; out->processed_cycle = finish;
  out->end = vx_rdcycle(); out->complete = RECORD_COMPLETE;
}
