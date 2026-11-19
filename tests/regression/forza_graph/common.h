#pragma once

#include <stdint.h>
#include <forza_software_abi.h>

enum {
  FORZA_GRAPH_COMPLETE = 0x46524750,
  FORZA_GRAPH_MAX_PES = 128,
};

struct graph_edge_t {
  uint32_t destination;
  int32_t weight;
};

struct graph_packet_t {
  uint32_t source;
  uint32_t destination;
  uint32_t edge;
  uint32_t reserved;
  int64_t contribution;
  uint64_t guard;
};

static_assert(sizeof(graph_packet_t) == 32, "graph message must fit the actor payload");

using graph_sw_queue_t = forza_sw_queue_t;
static_assert(sizeof(graph_sw_queue_t) == 64, "queue metadata occupies one cache line");

struct graph_actor_result_t {
  uint32_t rank;
  uint32_t error;
  uint32_t complete;
  uint32_t done_received;
  uint64_t sent;
  uint64_t received;
  uint64_t retries;
  uint64_t iterations;
  uint64_t outstanding;
  uint64_t start_cycle;
  uint64_t end_cycle;
  uint64_t cycles;
  uint64_t expected_received;
  uint64_t data_end_cycle;
  uint64_t expected;
  uint64_t actual;
};

struct graph_kernel_arg_t {
  uint32_t vertices;
  uint32_t edges;
  uint32_t actors;
  uint32_t cores;
  uint32_t actors_per_core;
  uint32_t threads;
  uint32_t owned_stride;
  uint32_t iteration_limit;
  uint64_t rows_addr;
  uint64_t edges_addr;
  uint64_t x_addr;
  uint64_t y_addr;
  uint64_t results_addr;
  uint64_t sw_state_addr;
  uint64_t sw_slots_addr;
  uint64_t sw_offsets_addr;
  uint32_t sw_queue_capacity;
  uint32_t reserved;
};

static inline uint64_t graph_packet_guard(uint32_t source, uint32_t destination,
                                         uint32_t edge, int64_t value) {
  return UINT64_C(0x4a17c39b86205ded) ^ (uint64_t(source) << 43)
       ^ (uint64_t(destination) << 19) ^ (uint64_t(edge) << 3) ^ uint64_t(value);
}
