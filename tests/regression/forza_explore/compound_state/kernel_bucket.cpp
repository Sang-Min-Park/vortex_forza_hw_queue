#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "state.h"

__kernel void kernel_main(compound_arg_t* __UNIFORM__ arg) {
  const uint32_t rank = vx_core_id() * arg->actors_per_core
                      + vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t actors = arg->actors;
  const uint32_t capacity = arg->bucket_capacity;
  auto requests = reinterpret_cast<const compound_request_t*>(arg->requests_addr);
  auto buckets = reinterpret_cast<compound_request_t*>(arg->buckets_addr);
  auto counts = reinterpret_cast<uint64_t*>(arg->bucket_counts_addr);
  auto result = reinterpret_cast<compound_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t processed = 0;
  if (arg->stage == 0) {
    uint32_t cursors[COMPOUND_MAX_ACTORS];
    for (uint32_t owner = 0; owner < actors; ++owner) cursors[owner] = 0;
    uint32_t begin = uint64_t(arg->requests) * rank / actors;
    uint32_t end = uint64_t(arg->requests) * (rank + 1) / actors;
    for (uint32_t index = begin; index < end; ++index) {
      compound_request_t request = requests[index];
      if (request.key >= arg->keys || request.id >= arg->requests) {
        result->error = 17;
        continue;
      }
      uint32_t owner = request.key % actors;
      uint32_t slot = cursors[owner]++;
      if (slot >= capacity) {
        result->error = 18;
        continue;
      }
      uint64_t bucket = uint64_t(rank) * actors + owner;
      buckets[bucket * capacity + slot] = request;
      ++processed;
    }
    for (uint32_t owner = 0; owner < actors; ++owner)
      counts[uint64_t(rank) * actors + owner] = cursors[owner];
    result->sent = processed;
  } else {
    OwnerState state(arg, result);
    for (uint32_t source = 0; source < actors; ++source) {
      uint64_t bucket = uint64_t(source) * actors + rank;
      uint64_t count = counts[bucket];
      if (count > capacity) {
        result->error = 18;
        continue;
      }
      for (uint32_t slot = 0; slot < count; ++slot) {
        compound_request_t request = buckets[bucket * capacity + slot];
        if (request.key >= arg->keys || request.id >= arg->requests
            || request.key % actors != rank) {
          result->error = 17;
          continue;
        }
        state.apply(request);
      }
    }
    state.finish();
    processed = state.processed();
    result->received = processed;
  }
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t finish = vx_rdcycle();
  result->processed = processed;
  result->start_cycle = start;
  result->end_cycle = finish;
  result->cycles = finish - start;
  result->complete = COMPOUND_COMPLETE;
}
