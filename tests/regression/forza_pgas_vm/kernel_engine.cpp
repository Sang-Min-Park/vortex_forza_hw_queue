#include "device.h"
#include <forza_vm_abi.h>
#include "programs.h"
#include <forza_mmio_abi.h>

#if !defined(VX_CFG_FORZA_VM_ENABLE)
#error VM comparison requires the programmable destination engine
#endif

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  uint32_t __UNIFORM__ requests = a->requests, cores = a->cores;
  uint32_t __UNIFORM__ per_core = a->per_core, threads = a->threads;
  uint32_t __UNIFORM__ shift = a->core_shift, records = a->keys >> shift;
  uint32_t __UNIFORM__ workers = cores * per_core, limit = a->limit, window = a->window;
  uint32_t core = vx_core_id(), local = vx_warp_id() * threads + vx_thread_id();
  uint32_t rank = core * per_core + local, cursor = rank;
  auto input = reinterpret_cast<const request_t*>(a->input);
  auto state = reinterpret_cast<record_t*>(a->state);
  auto responses = reinterpret_cast<volatile response_t*>(a->responses);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  out->rank = rank;
#ifndef FORZA_VM_DIRECT_MODE
  uint64_t registration = forza_mmio_actor_address(local, FORZA_MMIO_REGISTER);
  if (swap64(reinterpret_cast<uint64_t*>(registration), (uint64_t(local) << 8) | 2) != 0)
    out->error = 1;
#endif
  if (local == 0 && !install_program()) out->error = 8;
  for (uint32_t i = local; i < records * 3; i += per_core)
    swap64(scratch + (i / 3) * 8 + i % 3, 0);
  barrier(1);
  out->start = out->initialized = vx_rdcycle();
  uint64_t sent = 0, received = 0, retries = 0, credits = 0, peak_inflight = 0;
  bool complete = false;
#ifdef FORZA_VM_DIRECT_MODE
  auto endpoint = reinterpret_cast<volatile uint64_t*>(
      forza_mmio_actor_address(local, FORZA_RECORD_CQ_ID));
  constexpr int count_index = (int64_t(FORZA_MMIO_TX_COUNTS) - int64_t(FORZA_RECORD_CQ_ID)) / 8;
  constexpr int release_index = (FORZA_RECORD_CQ_RELEASE - FORZA_RECORD_CQ_ID) / 8;
#else
  constexpr uint64_t peek_address = FORZA_RECORD_PEEK_BASE + 8;
  auto endpoint = reinterpret_cast<volatile uint64_t*>(
      forza_mmio_actor_address(local, peek_address));
  constexpr int count_index = (int64_t(FORZA_MMIO_TX_COUNTS) - int64_t(peek_address)) / 8;
  constexpr int release_index = (FORZA_RECORD_RELEASE_BASE + 8 - peek_address) / 8;
#endif
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    if (cursor < requests && sent - received < window) {
      request_t request = load_request(input, cursor);
      uint32_t owner = request.key & (cores - 1), offset = request.key >> shift;
#ifdef FORZA_VM_DIRECT_MODE
      uint64_t address = forza_vm_address(owner, offset, VM_PROGRAM, 1);
#else
      uint64_t address = forza_vm_address(owner, offset, VM_PROGRAM, 0);
#endif
      uint64_t status = swap64(reinterpret_cast<uint64_t*>(address),
                               forza_vm_pack(uint32_t(request.delta), request.id));
      if (status == FORZA_MMIO_ACCEPTED) {
        cursor += workers;
        ++sent;
        if (sent - received > peak_inflight)
          peak_inflight = sent - received;
      } else if (status == FORZA_MMIO_BUSY) {
        ++retries;
      } else {
        out->error = 2;
      }
    }
    uint64_t available = endpoint[0];
    if (available) {
#ifdef FORZA_VM_DIRECT_MODE
      uint64_t id = available - 1;
      uint64_t count = endpoint[1], sum = endpoint[2], value = endpoint[3];
      uint64_t token = available ^ count ^ sum ^ value;
#else
      auto message = reinterpret_cast<volatile const uint64_t*>(available);
      uint64_t control = message[0], id = message[1];
      uint64_t count = message[2], sum = message[3], value = message[4];
      uint64_t token = control ^ id ^ count ^ sum ^ value;
#endif
      if (swap64(endpoint + release_index, token) != FORZA_MMIO_ACCEPTED)
        out->error = 3;
      if (id >= requests || id % workers != rank) {
        out->error = 4;
      } else {
        respond(responses, id, count, sum, value);
        ++received;
      }
    }
    bool done = false;
    if (cursor >= requests && received == sent) {
      credits = endpoint[count_index] & 255;
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
  for (uint32_t offset = local; offset < records; offset += per_core) {
    uint32_t key = (offset << shift) + core;
    state[key].count = scratch[offset * 8];
    state[key].sum = scratch[offset * 8 + 1];
    state[key].xor_value = scratch[offset * 8 + 2];
  }
  barrier(3);
  out->processed = sent;
  out->sent = sent;
  out->received = received;
  out->retries = retries;
  out->outstanding = credits;
  out->processed_cycle = finish;
  out->end = vx_rdcycle();
  out->peak_inflight = peak_inflight;
  out->complete = RECORD_COMPLETE;
}
