#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "state.h"

static inline uint32_t swap32(volatile uint32_t* address, uint32_t value) {
  uint32_t old;
  __asm__ volatile ("amoswap.w %0, %2, (%1)" : "=r"(old) : "r"(address), "r"(value) : "memory");
  return old;
}

#ifdef COMPOUND_SOA
static inline uint64_t read64(uint64_t* address) {
  uint64_t old;
  __asm__ volatile ("amoadd.d %0, zero, (%1)" : "=r"(old) : "r"(address) : "memory");
  return old;
}

static inline void write64(uint64_t* address, uint64_t value) {
  __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}
#endif

__kernel void kernel_main(compound_arg_t* __UNIFORM__ arg) {
  const uint32_t rank = vx_core_id() * arg->actors_per_core
                      + vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t actors = arg->actors;
  uint32_t cursor = uint64_t(arg->requests) * rank / actors;
  const uint32_t end = uint64_t(arg->requests) * (rank + 1) / actors;
  const uint32_t limit = arg->iteration_limit;
  auto requests = reinterpret_cast<const compound_request_t*>(arg->requests_addr);
  auto responses = reinterpret_cast<volatile compound_response_t*>(arg->responses_addr);
  auto result = reinterpret_cast<compound_result_t*>(arg->results_addr) + rank;
#ifdef COMPOUND_SOA
  auto counts = reinterpret_cast<uint64_t*>(arg->soa_count_addr);
  auto sums = reinterpret_cast<uint64_t*>(arg->soa_sum_addr);
  auto xors = reinterpret_cast<uint64_t*>(arg->soa_xor_addr);
  auto locks = reinterpret_cast<volatile uint32_t*>(arg->soa_locks_addr);
#else
  auto records = reinterpret_cast<volatile compound_record_t*>(arg->records_addr);
#endif
  result->rank = rank;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t processed = 0, retries = 0;
  compound_request_t request{};
  uint64_t token = 0;
  bool loaded = false;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < limit; ++iteration) {
    bool pending = cursor < end;
#ifdef COMPOUND_SOA
    bool held = false;
    uint64_t old_count = 0, old_sum = 0, old_xor = 0;
    if (pending) {
      if (!loaded) {
        request = requests[cursor];
        token = compound_token(request.id);
        loaded = true;
      }
      if (swap32(locks + request.key, 1) == 0) {
        old_count = read64(counts + request.key);
        old_sum = read64(sums + request.key);
        old_xor = read64(xors + request.key);
        write64(counts + request.key, old_count + 1);
        write64(sums + request.key, old_sum + request.delta);
        write64(xors + request.key, old_xor ^ token);
        held = true;
      } else {
        ++retries;
      }
    }
    // All lanes drain acknowledged field operations before any held lock is released.
    vx_fence();
    if (held) {
      swap32(locks + request.key, 0);
      compound_respond_to(responses, request, old_count, old_sum, old_xor);
      ++cursor;
      ++processed;
      loaded = false;
    }
#else
    bool pending = cursor < end;
    if (pending) {
      if (!loaded) {
        request = requests[cursor];
        token = compound_token(request.id);
        loaded = true;
      }
#ifdef COMPOUND_SOA
      auto lock = locks + request.key;
#else
      auto record = records + request.key;
      auto lock = &record->lock;
#endif
      uint32_t old_lock = swap32(lock, 1);
      if (old_lock == 0) {
#ifdef COMPOUND_SOA
        uint64_t old_count = read64(counts + request.key);
        uint64_t old_sum = read64(sums + request.key);
        uint64_t old_xor = read64(xors + request.key);
        write64(counts + request.key, old_count + 1);
        write64(sums + request.key, old_sum + request.delta);
        write64(xors + request.key, old_xor ^ token);
        vx_fence();
#else
        uint64_t old_count = record->count;
        uint64_t old_sum = record->sum;
        uint64_t old_xor = record->xor_value;
        record->count = old_count + 1;
        record->sum = old_sum + request.delta;
        record->xor_value = old_xor ^ token;
#endif
        swap32(lock, 0);
        compound_respond_to(responses, request, old_count, old_sum, old_xor);
        ++cursor;
        ++processed;
        loaded = false;
      } else {
        ++retries;
      }
    }
#endif
    uint32_t __UNIFORM__ all_complete = vx_vote_all(cursor == end);
    if (all_complete) {
      complete = true;
      break;
    }
  }
  if (!complete) result->error = 16;
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t finish = vx_rdcycle();
  result->processed = processed;
  result->retries = retries;
  result->start_cycle = start;
  result->end_cycle = finish;
  result->cycles = finish - start;
  result->complete = COMPOUND_COMPLETE;
}
