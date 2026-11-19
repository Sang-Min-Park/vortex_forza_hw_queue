#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  uint32_t rank = vx_core_id() * arg->per_core + vx_warp_id() * arg->threads + vx_thread_id();
  auto requests = reinterpret_cast<const request_t*>(arg->requests);
  auto offsets = reinterpret_cast<const uint32_t*>(arg->offsets);
  auto state = reinterpret_cast<volatile int64_t*>(arg->state);
  auto result = reinterpret_cast<result_t*>(arg->results) + rank;
  uint32_t begin = offsets[rank], end = offsets[rank + 1];
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle(), checksum = 0, mismatches = 0;
  for (uint32_t index = begin; index < end; ++index) {
    request_t request = requests[index];
    int64_t old = state[request.key];
    checksum ^= uint64_t(old);
#ifdef OWNER_ACK_STORE
    int64_t returned;
    int64_t value = old + request.delta;
    auto address = const_cast<int64_t*>(state + request.key);
    __asm__ volatile ("amoswap.d %0, %2, (%1)" : "=r"(returned) : "r"(address), "r"(value) : "memory");
    if (returned != old) ++mismatches;
#else
    state[request.key] = old + request.delta;
#endif
  }
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t finish = vx_rdcycle(), device_checksum = UINT64_C(0xcbf29ce484222325);
  if (arg->verify_device) {
    uint64_t first = uint64_t(rank) * arg->words_per_actor;
    for (uint64_t word = 0; word < arg->words_per_actor; ++word) {
      device_checksum = (device_checksum ^ uint64_t(state[first + word])) * UINT64_C(0x100000001b3);
    }
  }
  result->rank = rank;
  result->updates = end - begin;
  result->mismatches = mismatches;
  result->old_checksum = checksum;
  result->device_checksum = device_checksum;
  result->start = start;
  result->end = finish;
  result->complete = OWNER_COMPLETE;
}
