#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <selector.forza.h>
#include "common.h"

#if __riscv_xlen != 64
#error "graph aggregation requires RV64"
#endif
#ifndef VX_CFG_EXT_A_ENABLE
#error "graph aggregation requires VX_CFG_EXT_A_ENABLE"
#endif

struct vector_chunk_t {
  uint32_t begin;
  uint32_t count;
  int64_t values[6];
};

static_assert(sizeof(vector_chunk_t) == 56, "six values and a chunk header fit one payload");

static void fail(graph_result_t* result, uint32_t error,
                 uint64_t expected, uint64_t actual) {
  if (result->error == 0) {
    result->error = error;
    result->expected = expected;
    result->actual = actual;
  }
}

static int64_t* vector_at(graph_arg_t* arg, uint32_t source, uint32_t owner) {
  auto base = reinterpret_cast<uint8_t*>(arg->vectors_addr);
  uint64_t pair = uint64_t(source) * arg->actors + owner;
  return reinterpret_cast<int64_t*>(base + pair * arg->vector_stride_bytes
                                   + sizeof(graph_ring_header_t));
}

class VectorSelector : public hclib::Selector<1, vector_chunk_t> {
public:
  VectorSelector(graph_arg_t* arg, graph_result_t* result)
    : hclib::Selector<1, vector_chunk_t>(0), arg_(arg), result_(result) {
    mb[0].process = [this](vector_chunk_t packet, int source) { accumulate(packet, source); };
  }

private:
  void accumulate(vector_chunk_t packet, int source) {
    if (source != -1 || packet.begin >= arg_->owned_stride
        || packet.begin % 6 != 0 || packet.count == 0 || packet.count > 6
        || packet.count > arg_->owned_stride - packet.begin) {
      fail(result_, 1, arg_->owned_stride, packet.begin);
      return;
    }
    uint32_t remaining = arg_->owned_stride - packet.begin;
    if (packet.count != (remaining < 6 ? remaining : 6)) {
      fail(result_, 2, remaining < 6 ? remaining : 6, packet.count);
      return;
    }
    auto y = reinterpret_cast<int64_t*>(arg_->y_addr)
           + uint64_t(result_->rank) * arg_->owned_stride;
    for (uint32_t b = 0; b < packet.count; ++b) {
      y[packet.begin + b] += packet.values[b];
    }
    ++result_->chunks_received;
    if (packet.begin + packet.count == arg_->owned_stride) {
      ++result_->vectors_reduced;
    }
    if (result_->chunks_received == result_->expected_received_chunks) {
      result_->data_end_cycle = vx_rdcycle();
    }
  }

  graph_arg_t* arg_;
  graph_result_t* result_;
};

__kernel void kernel_main(graph_arg_t* __UNIFORM__ arg) {
  uint32_t rank = vx_core_id() * arg->actors_per_core
                + vx_warp_id() * arg->threads + vx_thread_id();
  auto result = reinterpret_cast<graph_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
    VectorSelector selector(arg, result);
    selector.start();
    auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
    auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
    auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
    __syncthreads();
    vx_fence();
    vortex::gbarrier(2).arrive_and_wait();
    result->start_cycle = vx_rdcycle();
    if (result->expected_received_chunks == 0) {
      result->data_end_cycle = result->start_cycle;
    }
    uint64_t updates = 0;
    for (uint32_t vertex = rank; vertex < arg->vertices; vertex += arg->actors) {
      int64_t value = x[vertex];
      for (uint32_t edge = rows[vertex]; edge < rows[vertex + 1]; ++edge) {
        graph_edge_t entry = edges[edge];
        auto target = vector_at(arg, rank, entry.destination % arg->actors)
                    + entry.destination / arg->actors;
        int64_t contribution = value * entry.weight;
        __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(target), "r"(contribution) : "memory");
        ++updates;
      }
    }
    vx_fence();
    result->edges_folded = updates;
    uint64_t folded = vx_rdcycle();
    result->fold_cycles = folded - result->start_cycle;

    uint32_t owner = 0;
    uint32_t begin = 0;
    bool selected = false;
    bool submitted = false;
    for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
      while (owner < arg->actors && !selected) {
        auto values = vector_at(arg, rank, owner);
        bool any = false;
        for (uint32_t b = 0; b < arg->owned_stride; ++b) {
          any |= values[b] != 0;
        }
        if (any) {
          selected = true;
        } else {
          ++owner;
        }
      }
      if (owner < arg->actors) {
        vector_chunk_t packet{};
        packet.begin = begin;
        uint32_t remaining = arg->owned_stride - begin;
        packet.count = remaining < 6 ? remaining : 6;
        auto values = vector_at(arg, rank, owner);
        for (uint32_t b = 0; b < packet.count; ++b) {
          packet.values[b] = values[begin + b];
        }
        if (selector.try_send(0, packet, owner)) {
          ++result->chunks_sent;
          begin += packet.count;
          if (begin == arg->owned_stride) {
            ++result->vectors_published;
            ++owner;
            begin = 0;
            selected = false;
          }
        }
      }
      // Finished senders must keep servicing their inbox until every active
      // lane has published; otherwise fan-in can fill the two receive slots.
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
    selector.done();
    result->done_received = selector.done_received(0);
    result->outstanding = selector.outstanding(0);
    if (!selector.ok() || !selector.is_done()
        || result->done_received != arg->actors || result->outstanding != 0) {
      fail(result, 5, arg->actors, result->done_received);
    }
    uint64_t drained = vx_rdcycle();
    result->publish_sync_cycles = drained - folded;
    vx_fence();
    vortex::gbarrier(3).arrive_and_wait();
    result->end_cycle = vx_rdcycle();
    result->cycles = result->end_cycle - result->start_cycle;
  });
  result->complete = GRAPH_COMPLETE;
}
