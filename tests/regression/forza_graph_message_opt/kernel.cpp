#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <selector.forza.h>
#include "common.h"

#if __riscv_xlen != 64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "Core combining requires RV64 with the A extension"
#endif

struct vector_chunk_t {
  uint32_t begin;
  uint16_t source_core;
  uint16_t count;
  int64_t values[6];
};

static_assert(sizeof(vector_chunk_t) == 56, "a chunk must fit the FORZA payload");

static void fail(graph_result_t* result, uint32_t error,
                 uint64_t expected, uint64_t actual) {
  if (result->error == 0) {
    result->error = error;
    result->expected = expected;
    result->actual = actual;
  }
}

class MessageSelector : public hclib::Selector<1, vector_chunk_t> {
public:
  MessageSelector(graph_arg_t* arg, graph_result_t* result)
    : hclib::Selector<1, vector_chunk_t>(0), arg_(arg), result_(result) {
    mb[0].process = [this](vector_chunk_t packet, int source) { accumulate(packet, source); };
  }

  bool all_remote_chunks_seen() const {
    uint32_t chunks = (arg_->owned_stride + 5) / 6;
    uint64_t mask = chunks == 64 ? UINT64_MAX : (UINT64_C(1) << chunks) - 1;
    for (uint32_t core = 0; core < arg_->cores; ++core) {
      if (core != uint32_t(vx_core_id()) && seen_[core] != mask) {
        return false;
      }
    }
    return true;
  }

private:
  void accumulate(vector_chunk_t packet, int source) {
    if (source != -1 || uint32_t(packet.source_core) == uint32_t(vx_core_id()) || packet.source_core >= arg_->cores
        || packet.source_core >= VX_CFG_NUM_CLUSTERS * VX_CFG_NUM_CORES
        || packet.begin >= arg_->owned_stride || packet.begin % 6 != 0
        || packet.count == 0 || packet.count > 6
        || packet.count > arg_->owned_stride - packet.begin) {
      fail(result_, 1, arg_->owned_stride, packet.begin);
      return;
    }
    uint32_t chunk = packet.begin / 6;
    if (chunk >= 64 || (seen_[packet.source_core] & (UINT64_C(1) << chunk)) != 0) {
      fail(result_, 6, 0, chunk);
      return;
    }
    seen_[packet.source_core] |= UINT64_C(1) << chunk;
    uint32_t remaining = arg_->owned_stride - packet.begin;
    if (packet.count != (remaining < 6 ? remaining : 6)) {
      fail(result_, 2, remaining < 6 ? remaining : 6, packet.count);
      return;
    }
    auto y = reinterpret_cast<int64_t*>(arg_->y_addr)
           + uint64_t(result_->rank) * arg_->owned_stride;
    for (uint32_t bucket = 0; bucket < packet.count; ++bucket) {
      y[packet.begin + bucket] += packet.values[bucket];
    }
    ++result_->chunks_received;
    uint32_t chunks = (arg_->owned_stride + 5) / 6;
    uint64_t mask = chunks == 64 ? UINT64_MAX : (UINT64_C(1) << chunks) - 1;
    if (seen_[packet.source_core] == mask) {
      ++result_->vectors_reduced;
    }
    if (result_->chunks_received == result_->expected_received_chunks) {
      result_->data_end_cycle = vx_rdcycle();
    }
  }

  graph_arg_t* arg_;
  graph_result_t* result_;
  uint64_t seen_[VX_CFG_NUM_CLUSTERS * VX_CFG_NUM_CORES]{};
};

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  uint32_t local = vx_warp_id() * arg->threads + vx_thread_id();
  uint32_t rank = vx_core_id() * arg->actors_per_core + local;
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
    MessageSelector selector(arg, result);
    selector.start();
    auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
    auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
    auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
    auto partial = reinterpret_cast<int64_t*>(__local_mem());
    const uint32_t actors = arg->actors;
    const uint32_t actors_per_core = arg->actors_per_core;
#ifdef GRAPH_WARP
    const uint32_t threads = arg->threads;
    const uint32_t cores = arg->cores;
#endif
    const uint32_t vertices = arg->vertices;
    const uint32_t stride = arg->owned_stride;
    const uint32_t iteration_limit = arg->iteration_limit;
    uint64_t words = uint64_t(actors) * stride;
#ifdef GRAPH_WARP
    uint32_t first_vertex = vx_warp_id() * cores + vx_core_id();
    uint32_t vertex_step = cores * vx_num_warps();
#else
    uint32_t first_vertex = rank;
    uint32_t vertex_step = actors;
#endif
    uint32_t first_edge = first_vertex < vertices ? rows[first_vertex] : 0;
    int64_t first_value = first_vertex < vertices ? x[first_vertex] : 0;
    __syncthreads();
    vx_fence();
    vortex::gbarrier(2).arrive_and_wait();
    uint64_t start = vx_rdcycle();
    result->start_cycle = start;

#ifdef GRAPH_MSG_PRIVATE
    const uint32_t partial_stride = actors_per_core;
    uint64_t zero_begin = 0;
    uint64_t zero_step = 1;
#else
    const uint32_t partial_stride = 1;
    uint64_t zero_begin = local;
    uint64_t zero_step = actors_per_core;
#endif
    for (uint64_t word = zero_begin; word < words; word += zero_step) {
#ifdef GRAPH_MSG_PRIVATE
      int64_t* target = partial + word * partial_stride + local;
#else
      int64_t* target = partial + word;
#endif
      int64_t value = 0;
      __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(target), "r"(value) : "memory");
    }
    vx_fence();
    __syncthreads();
    uint64_t initialized = vx_rdcycle();
    uint64_t updates = 0;
    for (uint32_t vertex = first_vertex; vertex < vertices; vertex += vertex_step) {
      uint32_t begin = vertex == first_vertex ? first_edge : rows[vertex];
      int64_t value = vertex == first_vertex ? first_value : x[vertex];
      uint32_t end = rows[vertex + 1];
#ifdef GRAPH_WARP
      uint32_t edge_begin = begin + vx_thread_id();
      uint32_t edge_step = threads;
#else
      uint32_t edge_begin = begin;
      uint32_t edge_step = 1;
#endif
      for (uint32_t edge = edge_begin; edge < end; edge += edge_step) {
        graph_edge_t entry = edges[edge];
        uint32_t owner = entry.destination % actors;
        uint32_t index = entry.destination / actors;
#ifdef GRAPH_MSG_PRIVATE
        uint64_t sender_offset = local;
#else
        uint64_t sender_offset = 0;
#endif
        uint64_t word = uint64_t(owner) * stride + index;
        int64_t* target = partial + word * partial_stride + sender_offset;
        int64_t contribution = value * entry.weight;
        __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(target), "r"(contribution) : "memory");
        ++updates;
      }
    }
    vx_fence();
    uint64_t folded = vx_rdcycle();
    __syncthreads();
#ifdef GRAPH_MSG_PRIVATE
    for (uint64_t word = local; word < words; word += actors_per_core) {
      int64_t sum = 0;
      for (uint32_t sender = 0; sender < actors_per_core; ++sender) {
        sum += partial[word * partial_stride + sender];
      }
      int64_t* target = partial + word * partial_stride;
      __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(target), "r"(sum) : "memory");
    }
    vx_fence();
    __syncthreads();
#endif
    auto y = reinterpret_cast<int64_t*>(arg->y_addr) + uint64_t(rank) * stride;
    for (uint32_t bucket = 0; bucket < stride; ++bucket) {
      y[bucket] += partial[(uint64_t(rank) * stride + bucket) * partial_stride];
    }
    uint64_t local_ready = vx_rdcycle();
    result->vectors_reduced = 1;
    result->edges_folded = updates;
    result->local_init_cycles = initialized - start;
    result->fold_cycles = folded - initialized;
    result->reduce_cycles = local_ready - folded;
    if (result->expected_received_chunks == 0) {
      result->data_end_cycle = local_ready;
    }

    uint32_t owner = local;
    if (owner / actors_per_core == uint32_t(vx_core_id())) {
      owner += actors_per_core;
    }
    uint32_t begin = 0;
    bool completed = false;
    for (uint32_t iteration = 0; iteration < iteration_limit; ++iteration) {
      if (owner < actors) {
        vector_chunk_t packet{};
        packet.begin = begin;
        packet.source_core = vx_core_id();
        uint32_t remaining = stride - begin;
        packet.count = remaining < 6 ? remaining : 6;
        for (uint32_t bucket = 0; bucket < packet.count; ++bucket) {
          uint64_t word = uint64_t(owner) * stride + begin + bucket;
          packet.values[bucket] = partial[word * partial_stride];
        }
        if (selector.try_send(0, packet, owner)) {
          ++result->chunks_sent;
          begin += packet.count;
          if (begin == stride) {
            ++result->vectors_published;
            owner += actors_per_core;
            if (owner / actors_per_core == uint32_t(vx_core_id())) {
              owner += actors_per_core;
            }
            begin = 0;
          }
        }
      }
      selector.progress();
      if (!selector.ok()) {
        fail(result, 3, 0, selector.error());
      }
      bool ready = owner >= actors
                && result->chunks_received == result->expected_received_chunks
                && selector.outstanding(0) == 0 && selector.all_remote_chunks_seen();
      uint32_t __UNIFORM__ all_complete = vx_vote_all(ready);
      if (all_complete) {
        completed = true;
        break;
      }
    }
    result->outstanding = selector.outstanding(0);
    if (!completed || !selector.ok() || result->outstanding != 0) {
      fail(result, 8, result->expected_received_chunks, result->chunks_received);
    }
    uint64_t drained = vx_rdcycle();
    result->publish_sync_cycles = drained - local_ready;
    vx_fence();
    vortex::gbarrier(3).arrive_and_wait();
    vx_fence();
    result->end_cycle = vx_rdcycle();
    result->cycles = result->end_cycle - start;
  });
  result->complete = GRAPH_COMPLETE;
}
