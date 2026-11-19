#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"

#if __riscv_xlen != 64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "Coalesced graph kernels require RV64 with the A extension"
#endif
#ifndef VX_CFG_DCACHE_DISABLE
#error "Software publication flags require the shared-L2, private-D-cache-disabled configuration"
#endif

static inline void add_value(int64_t* address, int64_t value) {
  __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}

static inline void store_value(int64_t* address, int64_t value) {
  __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}

static inline void publish_flag(volatile uint32_t* address, uint32_t value) {
  __asm__ volatile ("amoswap.w zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}

static inline void tree_barrier(graph_arg_t* arg, uint32_t rank) {
  auto arrived = reinterpret_cast<volatile uint32_t*>(arg->arrived_addr);
  auto released = reinterpret_cast<volatile uint32_t*>(arg->released_addr);
  uint32_t left = 2 * rank + 1;
  uint32_t right = 2 * rank + 2;
  const uint32_t actors = arg->actors;
  uint32_t phase = 0;
  while (!vx_vote_all(phase == 2)) {
    if (phase == 0) {
      if ((left >= actors || arrived[left])
          && (right >= actors || arrived[right])) {
        publish_flag(arrived + rank, 1);
        vx_fence();
        phase = 1;
      }
    } else if (phase == 1) {
      if (rank == 0 || released[rank]) {
        if (left < actors) {
          publish_flag(released + left, 1);
        }
        if (right < actors) {
          publish_flag(released + right, 1);
        }
        vx_fence();
        phase = 2;
      }
    }
  }
}

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  const uint32_t actors = arg->actors;
  const uint32_t actors_per_core = arg->actors_per_core;
  const uint32_t threads = arg->threads;
  const uint32_t cores = arg->cores;
  const uint32_t vertices = arg->vertices;
  const uint32_t stride = arg->owned_stride;
  uint32_t local = vx_warp_id() * threads + vx_thread_id();
  uint32_t core = vx_core_id();
  uint32_t rank = core * actors_per_core + local;
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
  auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
  auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
  auto y = reinterpret_cast<int64_t*>(arg->y_addr) + uint64_t(rank) * stride;
  auto global = reinterpret_cast<int64_t*>(arg->vectors_addr);
  auto partial = reinterpret_cast<int64_t*>(__local_mem());
  uint64_t words = uint64_t(actors) * stride;
  uint32_t first_vertex = vx_warp_id() * cores + vx_core_id();
  uint32_t vertex_step = cores * vx_num_warps();
  uint32_t first_edge = first_vertex < vertices ? rows[first_vertex] : 0;
  int64_t first_value = first_vertex < vertices ? x[first_vertex] : 0;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  for (uint64_t word = local; word < words; word += actors_per_core) {
    store_value(partial + word, 0);
  }
  vx_fence();
  __syncthreads();
  uint64_t initialized = vx_rdcycle();
  uint64_t updates = 0;
  for (uint32_t vertex = first_vertex; vertex < vertices; vertex += vertex_step) {
    uint32_t begin = vertex == first_vertex ? first_edge : rows[vertex];
    int64_t value = vertex == first_vertex ? first_value : x[vertex];
    uint32_t end = rows[vertex + 1];
    for (uint32_t edge = begin + vx_thread_id(); edge < end; edge += threads) {
      graph_edge_t entry = edges[edge];
      uint32_t owner = entry.destination % actors;
      uint32_t index = entry.destination / actors;
      add_value(partial + uint64_t(owner) * stride + index, value * entry.weight);
      ++updates;
    }
  }
  vx_fence();
  uint64_t folded = vx_rdcycle();
  __syncthreads();
  for (uint32_t bucket = 0; bucket < stride; ++bucket) {
    y[bucket] += partial[uint64_t(rank) * stride + bucket];
  }
  uint64_t local_ready = vx_rdcycle();
  uint64_t published = 0;
  for (uint32_t owner = local; owner < actors; owner += actors_per_core) {
    if (owner / actors_per_core == core) {
      continue;
    }
    for (uint32_t bucket = 0; bucket < stride; ++bucket) {
      uint64_t word = uint64_t(owner) * stride + bucket;
      store_value(global + uint64_t(core) * words + word, partial[word]);
    }
    ++published;
  }
  vx_fence();
  tree_barrier(arg, rank);
  uint64_t synchronized = vx_rdcycle();
  for (uint32_t source = 0; source < cores; ++source) {
    if (source == core) {
      continue;
    }
    uint64_t base = (uint64_t(source) * actors + rank) * stride;
    for (uint32_t bucket = 0; bucket < stride; ++bucket) {
      y[bucket] += global[base + bucket];
    }
  }
  vx_fence();
  uint64_t data_end = vx_rdcycle();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t end = vx_rdcycle();
  result->edges_folded = updates;
  result->vectors_published = published;
  result->vectors_reduced = cores;
  result->start_cycle = start;
  result->data_end_cycle = data_end;
  result->end_cycle = end;
  result->cycles = end - start;
  result->local_init_cycles = initialized - start;
  result->fold_cycles = folded - initialized;
  result->publish_sync_cycles = synchronized - local_ready;
  result->reduce_cycles = local_ready - folded + data_end - synchronized;
  result->complete = GRAPH_COMPLETE;
}
