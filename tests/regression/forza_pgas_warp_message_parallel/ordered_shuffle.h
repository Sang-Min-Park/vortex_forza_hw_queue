#pragma once

#include <stdint.h>

static inline uint64_t ordered_shfl_idx(uint64_t value, int source, int limit, int mask) {
  uint64_t result, control = (uint64_t(mask) << 12) | (uint64_t(limit) << 6) | uint32_t(source);
  __asm__ volatile(".insn r 0x0b, 7, 1, %0, %1, %2"
                   : "=r"(result) : "r"(value), "r"(control) : "memory");
  return result;
}

static inline uint64_t ordered_shfl_up(uint64_t value, int distance, int limit, int mask) {
  uint64_t result, control = (uint64_t(mask) << 12) | (uint64_t(limit) << 6) | uint32_t(distance);
  __asm__ volatile(".insn r 0x0b, 4, 1, %0, %1, %2"
                   : "=r"(result) : "r"(value), "r"(control) : "memory");
  return result;
}
