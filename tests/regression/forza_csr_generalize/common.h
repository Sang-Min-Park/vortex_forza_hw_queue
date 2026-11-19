#pragma once

#include <stdint.h>

enum {
  CSR_GENERALIZE_DONE = 0x43535247,
  CSR_GENERALIZE_REGISTRATION = 1,
  CSR_GENERALIZE_SEND,
  CSR_GENERALIZE_RECEIVE,
  CSR_GENERALIZE_POINTER,
  CSR_GENERALIZE_PAYLOAD,
  CSR_GENERALIZE_DRAIN,
};

struct csr_generalize_result_t {
  uint32_t block;
  uint32_t thread;
  uint32_t core;
  uint32_t warp;
  uint32_t lane;
  uint32_t actor;
  uint32_t pe;
  uint32_t error;
  uint32_t sent;
  uint32_t received;
  uint32_t retries;
  uint32_t done;
  uint64_t registration_before;
  uint64_t registration_after;
  uint64_t status;
  uint64_t expected;
  uint64_t actual;
};

struct csr_generalize_arg_t {
  uint32_t actors_per_core;
  uint32_t threads_per_warp;
  uint32_t rounds;
  uint32_t poll_limit;
  uint32_t logical_pe;
  uint64_t results_addr;
};

static inline uint64_t csr_generalize_payload(uint32_t block, uint32_t thread,
                                              uint32_t sequence, uint32_t word) {
  return UINT64_C(0x7ac104b900000000) ^ (uint64_t(block) << 40)
       ^ (uint64_t(thread) << 24) ^ (uint64_t(sequence) << 8) ^ word;
}
