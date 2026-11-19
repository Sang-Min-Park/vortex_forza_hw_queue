#pragma once
#include <stdint.h>
struct args_t {
  uint64_t results;
  uint32_t actors, per_core, threads, limit;
};
struct result_t {
  uint64_t rank, error, complete, sent, received, retries;
  uint64_t state[4];
};
enum { MMIO_COMPLETE = 0x4d4d494f, RING_MESSAGES = 64 };
static inline uint64_t ring_value(uint32_t rank, uint32_t index) {
  if (index == 0) return 0;
  if (index == 1) return UINT64_MAX;
  if (index == 2) return UINT64_C(0x8000000000000000) | rank;
  uint64_t v = (uint64_t(rank) << 32) | index;
  v ^= v >> 30; v *= UINT64_C(0xbf58476d1ce4e5b9);
  v ^= v >> 27; v *= UINT64_C(0x94d049bb133111eb);
  return v ^ (v >> 31);
}
static inline uint64_t initial_state(uint32_t rank, uint32_t offset) {
  if (offset == 0) return 0;
  if (offset == 1) return uint64_t(rank) * 1000 + 1;
  if (offset == 2) return UINT64_C(0x8000000000000000) | (uint64_t(rank) << 8) | 2;
  return ~uint64_t(rank * 32 + offset);
}
