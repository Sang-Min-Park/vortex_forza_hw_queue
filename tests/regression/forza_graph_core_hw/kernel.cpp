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

class CoreVectorSelector : public hclib::Selector<1, vector_chunk_t> {
public:
  CoreVectorSelector(graph_arg_t* arg, graph_result_t* result)
    : hclib::Selector<1, vector_chunk_t>(0), arg_(arg), result_(result) {
    mb[0].process = [this](vector_chunk_t packet, int source) { accumulate(packet, source); };
  }

  bool all_chunks_seen() const {
    uint32_t chunks = (arg_->owned_stride + 5) / 6;
    uint64_t mask = chunks == 64 ? UINT64_MAX : (UINT64_C(1) << chunks) - 1;
    for (uint32_t core = 0; core < arg_->cores; ++core) {
      if (seen_[core] != mask) {
        return false;
      }
    }
    return true;
  }

private:
  void accumulate(vector_chunk_t packet, int source) {
    if (source != -1 || packet.source_core >= arg_->cores
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
    CoreVectorSelector selector(arg, result);
    selector.start();
    auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
    auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
    auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
    auto partial = reinterpret_cast<int64_t*>(__local_mem());
    uint64_t words = uint64_t(arg->actors) * arg->owned_stride;
    uint32_t first_edge = rank < arg->vertices ? rows[rank] : 0;
    int64_t first_value = rank < arg->vertices ? x[rank] : 0;
    __syncthreads();
    vx_fence();
    vortex::gbarrier(2).arrive_and_wait();
    uint64_t start = vx_rdcycle();
    result->start_cycle = start;

    for (uint64_t word = local; word < words; word += arg->actors_per_core) {
      int64_t* target = partial + word;
      int64_t value = 0;
      __asm__ volatile ("amoswap.d zero, %1, (%0)" :: "r"(target), "r"(value) : "memory");
    }
    vx_fence();
    __syncthreads();
    uint64_t initialized = vx_rdcycle();
    uint64_t updates = 0;
    for (uint32_t vertex = rank; vertex < arg->vertices; vertex += arg->actors) {
      uint32_t begin = vertex == rank ? first_edge : rows[vertex];
      int64_t value = vertex == rank ? first_value : x[vertex];
      for (uint32_t edge = begin; edge < rows[vertex + 1]; ++edge) {
        graph_edge_t entry = edges[edge];
        uint32_t owner = entry.destination % arg->actors;
        uint32_t index = entry.destination / arg->actors;
        int64_t* target = partial + uint64_t(owner) * arg->owned_stride + index;
        int64_t contribution = value * entry.weight;
        __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(target), "r"(contribution) : "memory");
        ++updates;
      }
    }
    vx_fence();
    uint64_t folded = vx_rdcycle();
    __syncthreads();
    result->edges_folded = updates;
    result->local_init_cycles = initialized - start;
    result->fold_cycles = folded - initialized;

    uint32_t owner = local;
    uint32_t begin = 0;
    bool submitted = false;
    for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
      if (owner < arg->actors) {
        vector_chunk_t packet{};
        packet.begin = begin;
        packet.source_core = vx_core_id();
        uint32_t remaining = arg->owned_stride - begin;
        packet.count = remaining < 6 ? remaining : 6;
        auto values = partial + uint64_t(owner) * arg->owned_stride;
        for (uint32_t bucket = 0; bucket < packet.count; ++bucket) {
          packet.values[bucket] = values[begin + bucket];
        }
        if (selector.try_send(0, packet, owner)) {
          ++result->chunks_sent;
          begin += packet.count;
          if (begin == arg->owned_stride) {
            ++result->vectors_published;
            owner += arg->actors_per_core;
            begin = 0;
          }
        }
      }
      selector.progress();
      if (!selector.ok()) {
        fail(result, 3, 0, selector.error());
      }
      uint32_t __UNIFORM__ all_submitted = vx_vote_all(owner >= arg->actors);
      if (all_submitted) {
        submitted = true;
        break;
      }
    }
    if (!submitted) {
      fail(result, 4, arg->actors, owner);
    }
#ifdef GRAPH_CORE_HW_FIXED
    bool completed = false;
    for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
      selector.progress();
      bool ready = owner >= arg->actors
                && result->chunks_received == result->expected_received_chunks
                && selector.outstanding(0) == 0 && selector.all_chunks_seen();
      uint32_t __UNIFORM__ all_complete = vx_vote_all(ready);
      if (all_complete) {
        completed = true;
        break;
      }
      if (!selector.ok()) {
        fail(result, 7, 0, selector.error());
      }
    }
    result->outstanding = selector.outstanding(0);
    if (!completed || !selector.ok() || result->outstanding != 0) {
      fail(result, 8, result->expected_received_chunks, result->chunks_received);
    }
#else
    selector.done();
    result->done_received = selector.done_received(0);
    result->outstanding = selector.outstanding(0);
    if (!selector.ok() || !selector.is_done()
        || result->done_received != arg->actors || result->outstanding != 0
        || !selector.all_chunks_seen()) {
      fail(result, 5, arg->actors, result->done_received);
    }
#endif
    uint64_t drained = vx_rdcycle();
    result->publish_sync_cycles = drained - folded;
    vx_fence();
    vortex::gbarrier(3).arrive_and_wait();
    vx_fence();
    result->end_cycle = vx_rdcycle();
    result->cycles = result->end_cycle - start;
  });
  result->complete = GRAPH_COMPLETE;
}
