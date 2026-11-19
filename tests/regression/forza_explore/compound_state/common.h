#pragma once

#include <stdint.h>

enum { COMPOUND_COMPLETE = 0x43535458, COMPOUND_RESPONSE = 0x52535058, COMPOUND_MAX_ACTORS = 128 };

struct compound_request_t {
  uint32_t key;
  uint32_t id;
  uint64_t delta;
};

struct alignas(64) compound_record_t {
  uint64_t count;
  uint64_t sum;
  uint64_t xor_value;
  uint32_t lock;
  uint32_t reserved;
  uint64_t padding[4];
};

struct alignas(32) compound_response_t {
  uint64_t old_count;
  uint64_t old_sum;
  uint64_t old_xor;
  uint32_t complete;
  uint32_t reserved;
};

struct compound_result_t {
  uint32_t rank;
  uint32_t error;
  uint32_t complete;
  uint32_t reserved;
  uint64_t processed;
  uint64_t sent;
  uint64_t received;
  uint64_t outstanding;
  uint64_t retries;
  uint64_t start_cycle;
  uint64_t end_cycle;
  uint64_t cycles;
};

struct compound_arg_t {
  uint32_t keys;
  uint32_t requests;
  uint32_t actors;
  uint32_t cores;
  uint32_t actors_per_core;
  uint32_t threads;
  uint32_t iteration_limit;
  uint32_t stage;
  uint32_t bucket_capacity;
  uint32_t reserved;
  uint64_t requests_addr;
  uint64_t records_addr;
  uint64_t responses_addr;
  uint64_t results_addr;
  uint64_t incoming_addr;
  uint64_t buckets_addr;
  uint64_t bucket_counts_addr;
  uint64_t soa_count_addr;
  uint64_t soa_sum_addr;
  uint64_t soa_xor_addr;
  uint64_t soa_locks_addr;
};

static_assert(sizeof(compound_request_t) == 16, "request must use two payload words");
static_assert(sizeof(compound_record_t) == 64, "record and lock must share one cache line");
static_assert(sizeof(compound_response_t) == 32, "response slots may not overlap");

static inline uint64_t compound_token(uint32_t id) {
  uint64_t value = uint64_t(id) + UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}
