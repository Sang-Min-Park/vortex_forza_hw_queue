#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"

#if __riscv_xlen != 64
#error "forza_graph GPU atomic baselines require RV64"
#endif
#ifndef VX_CFG_EXT_A_ENABLE
#error "forza_graph GPU atomic baselines require VX_CFG_EXT_A_ENABLE"
#endif

static inline void accumulate(int64_t* address, int64_t value) {
  // The result is intentionally unused, but the LSU still waits for the AMO
  // response before the subsequent fence can complete.
  __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}

__kernel void kernel_main(graph_kernel_arg_t* __UNIFORM__ arg) {
  uint32_t rank = vx_core_id() * arg->actors_per_core
                + vx_warp_id() * arg->threads + vx_thread_id();
  auto result = reinterpret_cast<graph_actor_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
  auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
  auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
  auto y = reinterpret_cast<int64_t*>(arg->y_addr);
#ifndef FORZA_GRAPH_GPU_EDGE
  uint32_t first_edge = rank < arg->vertices ? rows[rank] : 0;
  int64_t first_value = rank < arg->vertices ? x[rank] : 0;
#endif
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t updates = 0;
#ifdef FORZA_GRAPH_GPU_EDGE
  for (uint32_t edge = rank; edge < arg->edges; edge += arg->actors) {
    // Upper-bound search handles empty CSR rows without a host edge-source
    // preprocessing array; its loads and branches are inside the timer.
    uint32_t low = 0;
    uint32_t high = arg->vertices;
    while (low + 1 < high) {
      uint32_t middle = low + (high - low) / 2;
      if (rows[middle] <= edge) {
        low = middle;
      } else {
        high = middle;
      }
    }
    graph_edge_t entry = edges[edge];
    accumulate(y + entry.destination, x[low] * entry.weight);
    ++updates;
  }
#else
  for (uint32_t vertex = rank; vertex < arg->vertices; vertex += arg->actors) {
    uint32_t begin = vertex == rank ? first_edge : rows[vertex];
    int64_t value = vertex == rank ? first_value : x[vertex];
    uint32_t end = rows[vertex + 1];
    for (uint32_t edge = begin; edge < end; ++edge) {
      graph_edge_t entry = edges[edge];
      accumulate(y + entry.destination, value * entry.weight);
      ++updates;
    }
  }
#endif
  vx_fence();
  uint64_t data_end = vx_rdcycle();
  vortex::gbarrier(3).arrive_and_wait();
  uint64_t end = vx_rdcycle();
  result->sent = updates;
  result->received = 0;
  result->done_received = 0;
  result->outstanding = 0;
  result->start_cycle = start;
  result->data_end_cycle = data_end;
  result->end_cycle = end;
  result->cycles = end - start;
  result->iterations = updates;
  result->complete = FORZA_GRAPH_COMPLETE;
}
