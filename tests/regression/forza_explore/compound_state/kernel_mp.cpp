#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_message_lib.vortex.h>
#include "state.h"

__kernel void kernel_main(compound_arg_t* __UNIFORM__ arg) {
  const uint32_t actors = arg->actors;
  const uint32_t per_core = arg->actors_per_core;
  const uint32_t rank = vx_core_id() * per_core + vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t end = uint64_t(arg->requests) * (rank + 1) / actors;
  uint32_t cursor = uint64_t(arg->requests) * rank / actors;
  const uint32_t limit = arg->iteration_limit;
  auto requests = reinterpret_cast<const compound_request_t*>(arg->requests_addr);
  auto incoming = reinterpret_cast<const uint64_t*>(arg->incoming_addr);
  auto result = reinterpret_cast<compound_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  OwnerState state(arg, result);
  uint32_t initialized = forza_vortex_init();
  if (initialized != SUCCESS) result->error = initialized;
  uint64_t expected = incoming[rank];
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t sent = 0, received = 0, retries = 0, outstanding = 0;
  compound_request_t request{};
  bool loaded = false;
  bool locally_complete = false;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
#ifdef COMPOUND_ACTIVE_GATING
    if (!locally_complete) {
#endif
      if (cursor < end) {
        if (!loaded) {
          request = requests[cursor];
          loaded = true;
        }
        uint32_t owner = request.key % actors;
        if (owner == rank) {
          state.apply(request);
          ++cursor;
          loaded = false;
        } else {
          uint32_t status = forza_send_sized(0, &request, sizeof(request), 0,
                                             owner / per_core, owner % per_core);
          if (status == SUCCESS) {
            ++cursor;
            ++sent;
            loaded = false;
          } else if (status == ZEN_NOT_READY) {
            ++retries;
          } else {
            result->error = status;
          }
        }
      }
      uint32_t available = forza_message_available(0);
      if (available == SUCCESS) {
        compound_request_t incoming_request{};
        uint32_t status = forza_message_receive_ready(0, reinterpret_cast<uint64_t*>(&incoming_request), sizeof(incoming_request));
        if (status == SUCCESS) {
          state.apply(incoming_request);
          ++received;
        } else {
          result->error = status;
        }
      } else if (available != NO_MSG) {
        result->error = available;
      }
      uint32_t status = forza_get_outstanding_msg_count(0, &outstanding);
      if (status != SUCCESS) result->error = status;
      locally_complete = cursor == end && state.processed() == expected && outstanding == 0;
#ifdef COMPOUND_ACTIVE_GATING
    }
#endif
    uint32_t __UNIFORM__ all_ready = vx_vote_all(locally_complete);
    if (all_ready) {
      complete = true;
      break;
    }
  }
  state.finish();
  if (!complete) result->error = 16;
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t end_cycle = vx_rdcycle();
  result->processed = state.processed();
  result->sent = sent;
  result->received = received;
  result->outstanding = outstanding;
  result->retries = retries;
  result->start_cycle = start;
  result->end_cycle = end_cycle;
  result->cycles = end_cycle - start;
  result->complete = COMPOUND_COMPLETE;
}
