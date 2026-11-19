#pragma once
#include <stdint.h>

#ifdef LARGE_COMPACT
using request_key_t = uint32_t;
using request_delta_t = int32_t;
#else
using request_key_t = uint64_t;
using request_delta_t = int64_t;
#endif
struct request_t { request_key_t key; request_delta_t delta; };
static_assert(sizeof(request_t) == 8 || sizeof(request_t) == 16, "bounded short request size");
struct result_t {
  uint64_t expected_receive;
  uint64_t inputs, flushes, local, sent, received, attempts, retries;
  uint64_t outstanding, start, end, error, rank, complete;
};
struct args_t {
  uint64_t requests_addr, state_addr, results_addr;
  uint64_t words, words_per_actor;
  uint32_t updates, actors, per_core, threads, iteration_limit;
};
enum { LARGE_COMPLETE = 0x4c415247 };

struct cache_t {
  uint64_t key0 = UINT64_MAX, key1 = UINT64_MAX;
  int64_t sum0 = 0, sum1 = 0;
  bool recent0 = false;
};
