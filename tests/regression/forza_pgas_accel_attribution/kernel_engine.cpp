#include "device.h"
#include <forza_record_abi.h>
#include <forza_mmio_abi.h>

#if !defined(VX_CFG_FORZA_RECORD_ENABLE) || !defined(VX_CFG_FORZA_UPDATE_ENABLE)
#error Record attribution requires the same enabled destination engine in all modes
#endif

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  uint32_t rank = rank_of(a);
  uint32_t local = vx_warp_id() * a->threads + vx_thread_id();
  uint32_t workers = a->cores * a->per_core;
  uint32_t core = vx_core_id();
  uint32_t cursor = rank;
  auto input = reinterpret_cast<const request_t*>(a->input);
  auto state = reinterpret_cast<record_t*>(a->state);
  auto responses = reinterpret_cast<volatile response_t*>(a->responses);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  out->rank = rank;
#ifndef FORZA_RECORD_DIRECT_MODE
  uint64_t registration = forza_mmio_actor_address(local, FORZA_MMIO_REGISTER);
  if (swap64(reinterpret_cast<uint64_t*>(registration), (uint64_t(local) << 8) | 2) != 0)
    out->error = 1;
#endif
  for (uint32_t i = local; i < (a->keys >> a->core_shift) * 3; i += a->per_core)
    swap64(scratch + i, 0);
  barrier(1);
  uint64_t start = vx_rdcycle();
  uint64_t sent = 0, received = 0, retries = 0, credits = 0, peak_inflight = 0;
  uint64_t expected = (a->requests + workers - 1 - rank) / workers;
  bool complete = false, pending = false;
  uint64_t pending_address = 0, pending_operand = 0;
  uint64_t count_address = forza_mmio_actor_address(local, FORZA_MMIO_TX_COUNTS);
#ifdef FORZA_RECORD_DIRECT_MODE
  uint64_t poll_address = forza_mmio_actor_address(local, FORZA_RECORD_CQ_ID);
  uint64_t count_result = forza_mmio_actor_address(local, FORZA_RECORD_CQ_COUNT);
  uint64_t sum_result = forza_mmio_actor_address(local, FORZA_RECORD_CQ_SUM);
  uint64_t xor_result = forza_mmio_actor_address(local, FORZA_RECORD_CQ_XOR);
  uint64_t release = forza_mmio_actor_address(local, FORZA_RECORD_CQ_RELEASE);
#else
  uint64_t poll_address = forza_mmio_actor_address(local, forza_record_peek_address(1));
  uint64_t release = forza_mmio_actor_address(local, forza_record_release_address(1));
#endif
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    if (cursor < a->requests && (!a->window || sent - received < a->window)) {
      if (!pending) {
        request_t request = load_request(input, cursor);
        uint32_t owner = request.key & (a->cores - 1);
        uint32_t offset = request.key >> a->core_shift;
#ifdef FORZA_RECORD_DIRECT_MODE
        pending_address = forza_record_direct_address(owner, offset);
#else
        pending_address = forza_record_address(owner, offset);
#endif
        pending_operand = forza_record_pack(request.id, request.delta);
        pending = true;
      }
      uint64_t status = swap64(reinterpret_cast<uint64_t*>(pending_address), pending_operand);
      if (status == FORZA_MMIO_ACCEPTED) {
        cursor += workers;
        ++sent;
        pending = false;
        if (sent - received > peak_inflight)
          peak_inflight = sent - received;
      } else if (status == FORZA_MMIO_BUSY) {
        ++retries;
      } else {
        out->error = 2;
      }
    }
    uint64_t available = *reinterpret_cast<volatile const uint64_t*>(poll_address);
    if (available) {
#ifdef FORZA_RECORD_DIRECT_MODE
      uint64_t id = available - 1;
      uint64_t count = *reinterpret_cast<volatile const uint64_t*>(count_result);
      uint64_t sum = *reinterpret_cast<volatile const uint64_t*>(sum_result);
      uint64_t value = *reinterpret_cast<volatile const uint64_t*>(xor_result);
      uint64_t token = available ^ count ^ sum ^ value;
#else
      auto message = reinterpret_cast<volatile const uint64_t*>(available);
      uint64_t control = message[0], id = message[1];
      uint64_t count = message[2], sum = message[3], value = message[4];
      uint64_t token = control ^ id ^ count ^ sum ^ value;
#endif
      if (swap64(reinterpret_cast<uint64_t*>(release), token) != FORZA_MMIO_ACCEPTED)
        out->error = 3;
      if (id >= a->requests || id % workers != rank) {
        out->error = 4;
      } else {
        respond(responses, id, count, sum, value);
        ++received;
      }
    }
    bool done = false;
    if (cursor >= a->requests && received == expected) {
      credits = *reinterpret_cast<volatile const uint64_t*>(count_address) & 255;
      done = credits == 0;
    }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) {
      complete = true;
      break;
    }
  }
  if (!complete)
    out->error = 5;
  barrier(2);
  uint64_t finish = vx_rdcycle();
  for (uint32_t offset = local; offset < (a->keys >> a->core_shift); offset += a->per_core) {
    uint32_t key = (offset << a->core_shift) + core;
    state[key].count = scratch[offset * 3];
    state[key].sum = scratch[offset * 3 + 1];
    state[key].xor_value = scratch[offset * 3 + 2];
  }
  barrier(3);
  out->processed = sent;
  out->sent = sent;
  out->received = received;
  out->retries = retries;
  out->outstanding = credits;
  out->start = start;
  out->initialized = start;
  out->processed_cycle = finish;
  out->end = vx_rdcycle();
  out->peak_inflight = peak_inflight;
  out->complete = RECORD_COMPLETE;
}
