#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "fold.h"

#if __riscv_xlen != 64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "The GPU register controls require RV64 with the A extension"
#endif

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  const uint32_t cores = arg->cores, threads = arg->threads;
  const uint32_t vertices = arg->vertices, edge_count = arg->edges;
  uint32_t core = vx_core_id();
  uint32_t rank = core * arg->actors_per_core + vx_warp_id() * threads + vx_thread_id();
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
  auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
  auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
  auto y = reinterpret_cast<int64_t*>(arg->y_addr);
  uint32_t global_warp = vx_warp_id() * cores + core;
  uint32_t total_warps = cores * vx_num_warps();
  uint32_t lo = uint64_t(edge_count) * global_warp / total_warps;
  uint32_t hi = uint64_t(edge_count) * (global_warp + 1) / total_warps;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  fold_stats_t stats = fold_range(rows, edges, x, y, vertices,
                                  lo + vx_thread_id(), hi, threads);
  vx_fence();
  uint64_t data_end = vx_rdcycle();
  vortex::gbarrier(3).arrive_and_wait();
  uint64_t end = vx_rdcycle();
  result->edges_folded = stats.edges;
  result->atomic_flushes = stats.flushes;
  result->start_cycle = start;
  result->data_end_cycle = data_end;
  result->end_cycle = end;
  result->cycles = end - start;
  result->fold_cycles = data_end - start;
  result->complete = GRAPH_COMPLETE;
}
