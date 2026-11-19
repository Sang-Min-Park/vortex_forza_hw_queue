#include "field_handler.h"
#include "transport.h"

static_assert(MESSAGE_BATCH_SIZE == 1 || MESSAGE_BATCH_SIZE == 4
              || MESSAGE_BATCH_SIZE == 8, "bounded message batch");

__attribute__((noinline)) static void produce(args_t* __UNIFORM__ a,
                                             result_t* out) {
  const uint32_t lane = vx_thread_id();
  const uint32_t core = vx_core_id();
  const uint32_t clients = a->cores * 4;
  const uint32_t requests = a->requests;
  const uint32_t owner_mask = a->cores - 1;
  const uint32_t limit = a->limit;
  const uint64_t input = a->input;
  const uint64_t count_address = transport::endpoint(FORZA_MMIO_TX_COUNTS);
  uint32_t cursor = (core * 4 + lane) * MESSAGE_BATCH_SIZE;
  uint32_t pending_owners = 0;
  uint64_t token = 0;
  uint64_t sent = 0, retries = 0, credit = 0, scanned = 0;
  bool loaded = false, complete = false;

  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (cursor < requests) {
      if (!loaded) {
        uint32_t remaining = requests - cursor;
        uint32_t count = remaining < MESSAGE_BATCH_SIZE
                       ? remaining : MESSAGE_BATCH_SIZE;
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t key = uint32_t(reinterpret_cast<const uint64_t*>(input)
                                 [uint64_t(cursor + i) * 2]);
          pending_owners |= uint32_t(1) << (key & owner_mask);
          ++scanned;
        }
        token = (uint64_t(count) << 32) | uint64_t(cursor + 1);
        loaded = true;
      }
      uint32_t destination = __builtin_ctz(pending_owners);
      uint64_t target = FORZA_MMIO_BASE + (uint64_t(destination) << 17);
      uint64_t status = swap64(reinterpret_cast<uint64_t*>(target), token);
      if (status == FORZA_MMIO_ACCEPTED) {
        pending_owners &= pending_owners - 1;
        ++sent;
        if (!pending_owners) {
          cursor += clients * MESSAGE_BATCH_SIZE;
          loaded = false;
        }
      } else if (status == FORZA_MMIO_BUSY) {
        ++retries;
      } else {
        out->error = 2;
      }
    }
    bool done = false;
    if (cursor >= requests) {
      credit = transport::load(count_address) & 255;
      done = credit == 0;
    }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) {
      complete = true;
      break;
    }
  }
  if (!complete) out->error = 3;
  out->sent = sent;
  out->retries = retries;
  out->outstanding = credit;
  out->scanned = scanned;
}

__attribute__((noinline)) static void serve(args_t* __UNIFORM__ a,
                                           result_t* out) {
  const uint32_t lane = vx_thread_id();
  const uint32_t core = vx_core_id();
  const uint32_t per_core = a->per_core;
  const uint32_t owner_mask = a->cores - 1;
  const uint32_t requests = a->requests;
  const uint32_t workers = a->cores * per_core;
  const uint32_t limit = a->limit;
  const uint64_t input = a->input;
  auto counts = reinterpret_cast<const uint64_t*>(a->counts);
  const uint64_t expected_work = counts[core * per_core];
  const uint64_t expected_messages = counts[workers + core * per_core];
  const uint64_t pop_address = transport::endpoint(forza_mmio_pop_address(0));
  uint64_t handled = 0, received = 0, scanned = 0;
  bool complete = false;
  FieldHandler handler(a);

  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (received == expected_messages) {
      complete = true;
      break;
    }
    uint64_t token = 0;
    if (lane == 0) {
      token = swap64(reinterpret_cast<uint64_t*>(pop_address), 0);
    }
    uint64_t __UNIFORM__ batch = vx_shfl_idx(token, 0, VX_QUAD_CVAL,
                                            VX_QUAD_MASK);
    if (batch) {
      uint32_t __UNIFORM__ first = uint32_t(batch) - 1;
      uint32_t __UNIFORM__ count = uint32_t(batch >> 32);
      if (!uint32_t(batch) || !count || count > MESSAGE_BATCH_SIZE
          || first >= requests || count > requests - first) {
        out->error = 4;
      } else {
        uint32_t matched = 0;
        for (uint32_t i = 0; i < count; ++i) {
          uint64_t pointer = input + uint64_t(first + i) * sizeof(request_t);
          uint32_t key = uint32_t(*reinterpret_cast<const uint64_t*>(pointer));
          ++scanned;
          if ((key & owner_mask) == core) {
            uint32_t id = handler.apply(pointer);
            if (id == UINT32_MAX || (handler.key() & owner_mask) != core) {
              out->error = 5;
            }
            ++handled;
            ++matched;
          }
        }
        if (!matched) out->error = 8;
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
  const uint32_t core = vx_core_id();
  const uint32_t local = transport::local();
  const uint32_t rank = rank_of(a);
  const uint32_t words = (a->keys >> a->core_shift) * 3;
  const uint32_t per_core = a->per_core;
  const uint32_t shift = a->core_shift;
  const uint32_t records = a->keys >> a->core_shift;
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  auto state = reinterpret_cast<record_t*>(a->state);
  out->rank = rank;
  if (!transport::init(1) || a->batch_size != MESSAGE_BATCH_SIZE) out->error = 1;
  for (uint32_t i = local; i < words; i += per_core) swap64(scratch + i, 0);
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (vx_warp_id() == 0) serve(a, out);
  else produce(a, out);
  if (!transport::healthy()) out->error = 7;
  barrier(2);
  uint64_t finish = vx_rdcycle();
  for (uint32_t offset = local; offset < records; offset += per_core) {
    uint32_t key = (offset << shift) + core;
    state[key].count = scratch[offset * 3];
    state[key].sum = scratch[offset * 3 + 1];
    state[key].xor_value = scratch[offset * 3 + 2];
  }
  barrier(3);
  out->start = start;
  out->initialized = start;
  out->processed_cycle = finish;
  out->end = vx_rdcycle();
  out->complete = RECORD_COMPLETE;
}
