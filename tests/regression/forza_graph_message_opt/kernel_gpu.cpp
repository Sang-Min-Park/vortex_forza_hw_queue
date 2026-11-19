#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"

#if __riscv_xlen != 64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "The warp GPU control requires RV64 with the A extension"
#endif

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  const uint32_t cores = arg->cores;
  const uint32_t threads = arg->threads;
  const uint32_t vertices = arg->vertices;
  uint32_t rank = vx_core_id() * arg->actors_per_core
                + vx_warp_id() * threads + vx_thread_id();
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
  auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
  auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
  auto y = reinterpret_cast<int64_t*>(arg->y_addr);
  uint32_t first_vertex = vx_warp_id() * cores + vx_core_id();
  uint32_t vertex_step = cores * vx_num_warps();
  uint32_t first_edge = first_vertex < vertices ? rows[first_vertex] : 0;
  int64_t first_value = first_vertex < vertices ? x[first_vertex] : 0;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t updates = 0;
  for (uint32_t vertex = first_vertex; vertex < vertices; vertex += vertex_step) {
    uint32_t begin = vertex == first_vertex ? first_edge : rows[vertex];
    int64_t value = vertex == first_vertex ? first_value : x[vertex];
    uint32_t end = rows[vertex + 1];
    for (uint32_t edge = begin + vx_thread_id(); edge < end; edge += threads) {
      graph_edge_t entry = edges[edge];
      int64_t* target = y + entry.destination;
      int64_t contribution = value * entry.weight;
      __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(target), "r"(contribution) : "memory");
      ++updates;
    }
  }
  vx_fence();
  uint64_t data_end = vx_rdcycle();
  vortex::gbarrier(3).arrive_and_wait();
  uint64_t end = vx_rdcycle();
  result->edges_folded = updates;
  result->start_cycle = start;
  result->data_end_cycle = data_end;
  result->end_cycle = end;
  result->cycles = end - start;
  result->fold_cycles = data_end - start;
  result->complete = GRAPH_COMPLETE;
}
