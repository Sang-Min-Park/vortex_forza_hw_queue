#pragma once

#include <stdint.h>

enum { GRAPH_COMPLETE = 0x434f414c, GRAPH_MAX_ACTORS = 128 };

struct graph_edge_t {
  uint32_t destination;
  int32_t weight;
};

struct graph_result_t {
  uint32_t rank;
  uint32_t error;
  uint32_t complete;
  uint32_t done_received;
  uint64_t edges_folded;
  uint64_t vectors_published;
  uint64_t vectors_reduced;
  uint64_t chunks_sent;
  uint64_t chunks_received;
  uint64_t outstanding;
  uint64_t expected_received_chunks;
  uint64_t start_cycle;
  uint64_t data_end_cycle;
  uint64_t end_cycle;
  uint64_t cycles;
  uint64_t local_init_cycles;
  uint64_t fold_cycles;
  uint64_t publish_sync_cycles;
  uint64_t reduce_cycles;
  uint64_t expected;
  uint64_t actual;
  uint64_t atomic_flushes;
};

struct graph_arg_t {
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
  uint64_t vectors_addr;
  uint64_t arrived_addr;
  uint64_t released_addr;
  uint64_t vector_stride_bytes;
  uint64_t global_vectors_bytes;
  uint32_t local_bytes;
  uint32_t reserved;
};

struct graph_ring_header_t {
  uint32_t head;
  uint32_t tail;
  uint32_t sender;
  uint32_t reserved;
};

static_assert(sizeof(graph_ring_header_t) == 16, "dynamic CAP=1 ring header layout");
