#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
#include "transport.h"

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  const uint32_t local = vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t rank = vx_core_id() * arg->per_core + local;
  const uint32_t actors = arg->actors, per_core = arg->per_core;
  const uint32_t limit = arg->limit;
  auto result = reinterpret_cast<result_t*>(arg->results) + rank;
  auto state = reinterpret_cast<uint64_t*>(__local_mem());
  result->rank = rank;
  if (!pgas_transport::init()) result->error = 1;
  if (limit == 0) {
    result->complete = MMIO_COMPLETE;
    return;
  }
  for (uint32_t offset = 0; offset < 4; ++offset)
    state[offset * per_core + local] = initial_state(rank, offset);
  __syncthreads();
  vx_fence();
  uint32_t sent = 0, received = 0;
  const uint32_t next = (rank + 1) % actors, previous = (rank + actors - 1) % actors;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    for (uint32_t burst = 0; burst < 8; ++burst) {
      if (sent == RING_MESSAGES) break;
      uint32_t status = pgas_transport::try_send(next / per_core, next % per_core,
                                                 ring_value(rank, sent));
      if (status == pgas_transport::EMPTY_OR_BUSY) { ++result->retries; break; }
      if (status != pgas_transport::READY_OR_ACCEPTED) { result->error = 2; break; }
      ++sent;
    }
    uint64_t control = 0, value = 0;
    uint32_t status = pgas_transport::try_receive(control, value);
    if (status == pgas_transport::READY_OR_ACCEPTED) {
      if (received >= RING_MESSAGES || value != ring_value(previous, received)) result->error = 3;
      ++received;
    } else if (status == pgas_transport::ERROR) result->error = 4;
    bool done = sent == RING_MESSAGES && received == RING_MESSAGES
             && pgas_transport::outstanding() == 0;
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) result->error = 5;
  result->sent = sent;
  result->received = received;
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();

  const uint32_t target = (rank + per_core) % actors;
  for (uint32_t phase = 0; phase < 4; ++phase) {
    const uint32_t offset = phase == 0 ? 0 : phase == 3 ? 2 : 1;
    const uint32_t op = phase == 1 ? 1 : 2;
    const uint32_t delta = phase == 1 ? rank + 1 : 0;
    uint64_t request = (uint64_t(op) << 56) | (uint64_t(rank) << 40)
                     | (uint64_t(offset) << 32) | delta;
    bool posted = false, serviced = false, replied = false, pending = false;
    uint64_t reply = 0;
    uint32_t destination = 0;
    complete = false;
    for (uint32_t iteration = 0; iteration < limit; ++iteration) {
      if (!posted) {
        uint32_t status = pgas_transport::try_send(target / per_core, target % per_core, request);
        if (status == pgas_transport::READY_OR_ACCEPTED) { posted = true; ++result->sent; }
        else if (status == pgas_transport::ERROR) result->error = 6;
      }
      if (pending) {
        uint32_t status = pgas_transport::try_send(destination / per_core, destination % per_core, reply, 1);
        if (status == pgas_transport::READY_OR_ACCEPTED) { pending = false; ++result->sent; }
        else if (status == pgas_transport::ERROR) result->error = 7;
      }
      uint64_t control = 0, value = 0;
      uint32_t status = pgas_transport::try_receive(control, value);
      if (status == pgas_transport::READY_OR_ACCEPTED) {
        uint32_t source = (value >> 40) & 65535;
        uint32_t position = (value >> 32) & 255;
        uint32_t operation = value >> 56;
        if (serviced || pending || source >= actors || position >= 4 || operation != op) {
          result->error = 8;
        } else {
          auto address = state + position * per_core + local;
          if (operation == 1) *address += int32_t(value);
          reply = *address;
          destination = source;
          pending = true;
          serviced = true;
        }
        ++result->received;
      } else if (status == pgas_transport::ERROR) result->error = 9;
      status = pgas_transport::try_receive(control, value, 1);
      if (status == pgas_transport::READY_OR_ACCEPTED) {
        uint64_t expected = initial_state(target, offset);
        if (phase == 1 || phase == 2) expected += rank + 1;
        if (replied || value != expected) result->error = 10;
        replied = true;
        ++result->received;
      } else if (status == pgas_transport::ERROR) result->error = 11;
      bool done = posted && serviced && replied && !pending
               && pgas_transport::outstanding(0) == 0 && pgas_transport::outstanding(1) == 0;
      uint32_t __UNIFORM__ all_done = vx_vote_all(done);
      if (all_done) { complete = true; break; }
    }
    if (!complete) result->error = 12;
    vx_fence();
    vortex::gbarrier(3 + phase).arrive_and_wait();
  }
  for (uint32_t offset = 0; offset < 4; ++offset)
    result->state[offset] = state[offset * per_core + local];
  if (!pgas_transport::healthy()) result->error = 13;
  result->complete = MMIO_COMPLETE;
}
