#ifndef FORZA_QUEUE_COMMON_H
#define FORZA_QUEUE_COMMON_H

#include <stdint.h>

enum {
  FORZA_TEST_MAX_THREADS = 64,
  FORZA_TEST_POLL_LIMIT = 200000,
  FORZA_TEST_DONE = 0x464f525a,
};

enum forza_test_error {
  FORZA_TEST_REGISTRATION = 1,
  FORZA_TEST_SEND_TIMEOUT,
  FORZA_TEST_RECEIVE_TIMEOUT,
  FORZA_TEST_ACK_TIMEOUT,
  FORZA_TEST_CONTROL,
  FORZA_TEST_PAYLOAD,
  FORZA_TEST_POINTER,
  FORZA_TEST_RELEASE,
  FORZA_TEST_ORDER,
  FORZA_TEST_MIXED_LOAD,
  FORZA_TEST_NEGATIVE_ABI,
};

typedef struct {
  uint32_t core;
  uint32_t warp;
  uint32_t lane;
  uint32_t local_pe;
  uint32_t logical_pe;
  uint32_t phase;
  uint32_t error;
  uint32_t sent;
  uint32_t received;
  uint32_t mixed_loads;
  uint32_t done;
  uint64_t expected;
  uint64_t actual;
  uint64_t final_count0;
  uint64_t final_count1;
} actor_result_t;

typedef struct {
  uint32_t num_actors;
  uint32_t actors_per_core;
  uint32_t num_cores;
  uint32_t num_threads;
  uint32_t rounds;
  uint32_t poll_limit;
  uint32_t error_mode;
  uint64_t results_addr;
  uint64_t scratch_addr;
} kernel_arg_t;

static inline uint64_t payload_word(uint32_t source, uint32_t sequence, uint32_t word) {
  return UINT64_C(0xcafef00d12345678)
       ^ (UINT64_C(0x0101010101010101) * (word + 1))
       ^ (static_cast<uint64_t>(source) << 32)
       ^ static_cast<uint64_t>(sequence);
}

#endif
