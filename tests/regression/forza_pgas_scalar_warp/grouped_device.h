#pragma once
#include "device.h"

static constexpr uint32_t GROUPED_WORKERS = VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS / 2;
static constexpr uint32_t GROUPED_CHAINS = LOGICAL_CLIENTS_PER_CORE / GROUPED_WORKERS;
static_assert(GROUPED_WORKERS == 1 || GROUPED_WORKERS == 4, "supported physical client count");
static_assert(VX_CFG_NUM_THREADS == 1 || VX_CFG_NUM_THREADS == 4, "supported warp width");

static inline __attribute__((always_inline)) uint64_t scalar_group_shuffle(
    uint64_t value, uint32_t source) {
  if constexpr (VX_CFG_NUM_THREADS == 1) return value;
  uint64_t result, control = (VX_QUAD_MASK << 12) | (VX_QUAD_CVAL << 6) | source;
  __asm__ volatile(".insn r 0x0b,7,1,%0,%1,%2" : "=r"(result)
                   : "r"(value), "r"(control) : "memory");
  return result;
}
static inline __attribute__((always_inline)) uint32_t scalar_group_ballot(bool value) {
  if constexpr (VX_CFG_NUM_THREADS == 1) return value;
  uint64_t result;
  __asm__ volatile(".insn r 0x0b,3,1,%0,%1,x0" : "=r"(result)
                   : "r"(uint64_t(value)) : "memory");
  return uint32_t(result);
}
static inline __attribute__((always_inline)) bool scalar_group_all(bool value) {
  if constexpr (VX_CFG_NUM_THREADS == 1) return value;
  uint64_t result;
  __asm__ volatile(".insn r 0x0b,0,1,%0,%1,x0" : "=r"(result)
                   : "r"(uint64_t(value)) : "memory");
  return result;
}

// A call boundary keeps later warp collectives outside cache-hit divergence.
__attribute__((noinline)) static uint32_t scalar_group_seed(
    uint32_t client, uint32_t step, uint32_t link, uint32_t work,
    uint32_t cookie, bool cached, uint32_t seed) {
  return cached ? seed : update_seed(client, step, link, work, cookie);
}
