#pragma once

#include <stdint.h>

// Global-memory SPSC queues for controlled transport comparisons.
struct alignas(64) forza_sw_queue_t {
  uint64_t published;
  uint64_t consumed;
  uint64_t slots_offset;
  uint64_t slots_capacity;
  uint64_t reserved[4];
};

struct alignas(64) forza_sw_slot_t {
  uint64_t words[8];
};

static_assert(sizeof(forza_sw_queue_t) == 64, "SPSC metadata is one cache line");
static_assert(sizeof(forza_sw_slot_t) == 64, "SPSC sequence and payload share one line");
