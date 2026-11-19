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

#if defined(GRAPH_AGG_LMEM) || defined(GRAPH_AGG_COMB)
static inline void store_value(int64_t* address, int64_t value) {
  __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}
#endif

static inline void publish_flag(volatile uint32_t* address, uint32_t value) {
  __asm__ volatile ("amoswap.w zero, %1, (%0)" :: "r"(address), "r"(value) : "memory");
}

static inline void tree_barrier(graph_arg_t* arg, uint32_t rank) {
  auto arrived = reinterpret_cast<volatile uint32_t*>(arg->arrived_addr);
  auto released = reinterpret_cast<volatile uint32_t*>(arg->released_addr);
  uint32_t left = 2 * rank + 1;
  uint32_t right = 2 * rank + 2;
  uint32_t phase = 0;
  while (!vx_vote_all(phase == 2)) {
    if (phase == 0) {
      if ((left >= arg->actors || arrived[left])
          && (right >= arg->actors || arrived[right])) {
        publish_flag(arrived + rank, 1);
        vx_fence();
        phase = 1;
      }
    } else if (phase == 1) {
      if (rank == 0 || released[rank]) {
        if (left < arg->actors) {
          publish_flag(released + left, 1);
        }
        if (right < arg->actors) {
          publish_flag(released + right, 1);
        }
        vx_fence();
        phase = 2;
      }
    }
  }
}

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  uint32_t local = vx_warp_id() * arg->threads + vx_thread_id();
  uint32_t core = vx_core_id();
  uint32_t rank = core * arg->actors_per_core + local;
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
  auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
  auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
  auto y = reinterpret_cast<int64_t*>(arg->y_addr) + uint64_t(rank) * arg->owned_stride;
#if defined(GRAPH_AGG_LMEM) || defined(GRAPH_AGG_COMB)
  auto global = reinterpret_cast<int64_t*>(arg->vectors_addr);
  auto scratch = reinterpret_cast<int64_t*>(__local_mem());
#endif
  uint32_t first_edge = rank < arg->vertices ? rows[rank] : 0;
  int64_t first_value = rank < arg->vertices ? x[rank] : 0;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();

#ifdef GRAPH_AGG_LMEM
  uint64_t row_words = uint64_t(arg->actors_per_core) * arg->owned_stride;
  for (uint64_t word = 0; word < row_words; ++word) {
    store_value(scratch + uint64_t(local) * row_words + word, 0);
  }
  vx_fence();
  __syncthreads();
#elif defined(GRAPH_AGG_COMB)
  uint64_t words = uint64_t(arg->actors) * arg->owned_stride;
  for (uint64_t word = local; word < words; word += arg->actors_per_core) {
    store_value(scratch + word, 0);
  }
  vx_fence();
  __syncthreads();
#endif
  uint64_t initialized = vx_rdcycle();
  uint64_t updates = 0;
  for (uint32_t vertex = rank; vertex < arg->vertices; vertex += arg->actors) {
    uint32_t begin = vertex == rank ? first_edge : rows[vertex];
    int64_t value = vertex == rank ? first_value : x[vertex];
    for (uint32_t edge = begin; edge < rows[vertex + 1]; ++edge) {
      graph_edge_t entry = edges[edge];
      uint32_t owner = entry.destination % arg->actors;
      uint32_t index = entry.destination / arg->actors;
      int64_t contribution = value * entry.weight;
#ifdef GRAPH_AGG_AMO
      uint64_t channel = uint64_t(rank) * arg->actors + owner;
      auto counts = reinterpret_cast<int64_t*>(arg->vectors_addr
                    + channel * arg->vector_stride_bytes + sizeof(graph_ring_header_t));
      add_value(counts + index, contribution);
#elif defined(GRAPH_AGG_LMEM)
      if (owner / arg->actors_per_core == core) {
        uint32_t receiver = owner % arg->actors_per_core;
        uint64_t channel = uint64_t(local) * arg->actors_per_core + receiver;
        add_value(scratch + channel * arg->owned_stride + index, contribution);
      } else {
        uint64_t channel = uint64_t(rank) * arg->actors + owner;
        add_value(global + channel * arg->owned_stride + index, contribution);
      }
#else
      add_value(scratch + uint64_t(owner) * arg->owned_stride + index, contribution);
#endif
      ++updates;
    }
  }
  vx_fence();
  uint64_t folded = vx_rdcycle();
  uint64_t published = 0;
#ifdef GRAPH_AGG_AMO
  for (uint32_t owner = 0; owner < arg->actors; ++owner) {
    uint64_t channel = uint64_t(rank) * arg->actors + owner;
    auto header = reinterpret_cast<volatile graph_ring_header_t*>(arg->vectors_addr
                    + channel * arg->vector_stride_bytes);
    auto counts = reinterpret_cast<const int64_t*>(arg->vectors_addr
                    + channel * arg->vector_stride_bytes + sizeof(graph_ring_header_t));
    int64_t any = 0;
    for (uint32_t bucket = 0; bucket < arg->owned_stride; ++bucket) {
      any |= counts[bucket];
    }
    if (any != 0) {
      publish_flag(&header->sender, rank);
      vx_fence();
      publish_flag(&header->tail, 1);
      ++published;
    }
  }
  vx_fence();
#elif defined(GRAPH_AGG_LMEM)
  __syncthreads();
  published = arg->actors;
#else
  __syncthreads();
  uint64_t flush_words = uint64_t(arg->actors) * arg->owned_stride;
  for (uint64_t word = local; word < flush_words; word += arg->actors_per_core) {
    store_value(global + uint64_t(core) * flush_words + word, scratch[word]);
  }
  vx_fence();
  published = local == 0 ? arg->actors : 0;
#endif
  tree_barrier(arg, rank);
  uint64_t synchronized = vx_rdcycle();
  uint64_t reduced = 0;
#ifdef GRAPH_AGG_AMO
  for (uint32_t source = 0; source < arg->actors; ++source) {
    uint64_t channel = uint64_t(source) * arg->actors + rank;
    auto header = reinterpret_cast<volatile graph_ring_header_t*>(arg->vectors_addr
                    + channel * arg->vector_stride_bytes);
    if (header->head != header->tail) {
      auto counts = reinterpret_cast<const int64_t*>(arg->vectors_addr
                    + channel * arg->vector_stride_bytes + sizeof(graph_ring_header_t));
      for (uint32_t bucket = 0; bucket < arg->owned_stride; ++bucket) {
        y[bucket] += counts[bucket];
      }
      publish_flag(&header->head, header->tail);
      ++reduced;
    }
  }
#elif defined(GRAPH_AGG_LMEM)
  for (uint32_t source = 0; source < arg->actors_per_core; ++source) {
    uint64_t channel = uint64_t(source) * arg->actors_per_core + local;
    for (uint32_t bucket = 0; bucket < arg->owned_stride; ++bucket) {
      y[bucket] += scratch[channel * arg->owned_stride + bucket];
    }
    ++reduced;
  }
  for (uint32_t source = 0; source < arg->actors; ++source) {
    if (source / arg->actors_per_core == core) {
      continue;
    }
    uint64_t channel = uint64_t(source) * arg->actors + rank;
    for (uint32_t bucket = 0; bucket < arg->owned_stride; ++bucket) {
      y[bucket] += global[channel * arg->owned_stride + bucket];
    }
    ++reduced;
  }
#else
  for (uint32_t bucket = 0; bucket < arg->owned_stride; ++bucket) {
    int64_t sum = 0;
    for (uint32_t source = 0; source < arg->cores; ++source) {
      uint64_t base = (uint64_t(source) * arg->actors + rank) * arg->owned_stride;
      sum += global[base + bucket];
    }
    y[bucket] = sum;
  }
  reduced = arg->cores;
#endif
  vx_fence();
  uint64_t data_end = vx_rdcycle();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  uint64_t end = vx_rdcycle();
  result->edges_folded = updates;
  result->vectors_published = published;
  result->vectors_reduced = reduced;
  result->start_cycle = start;
  result->data_end_cycle = data_end;
  result->end_cycle = end;
  result->cycles = end - start;
  result->local_init_cycles = initialized - start;
  result->fold_cycles = folded - initialized;
  result->publish_sync_cycles = synchronized - folded;
  result->reduce_cycles = data_end - synchronized;
  result->complete = GRAPH_COMPLETE;
}
