#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
#include <selector.forza.h>

#if __riscv_xlen != 64
#error "forza_graph requires RV64"
#endif

static void fail(graph_actor_result_t* result, uint32_t error,
                 uint64_t expected, uint64_t actual) {
  if (result->error == 0) {
    result->error = error;
    result->expected = expected;
    result->actual = actual;
  }
}

class GraphSelector : public hclib::Selector<1, graph_packet_t> {
public:
  GraphSelector(graph_kernel_arg_t* arg, graph_actor_result_t* result)
    : hclib::Selector<1, graph_packet_t>(0)
    , arg_(arg)
    , result_(result) {
    mb[0].process = [this](graph_packet_t packet, int source) { accumulate(packet, source); };
  }

private:
  void accumulate(graph_packet_t packet, int source) {
    if (source != -1 || packet.source >= arg_->vertices
        || packet.destination >= arg_->vertices || packet.edge >= arg_->edges) {
      fail(result_, 1, arg_->vertices, packet.destination);
      return;
    }
    if (packet.destination % arg_->actors != result_->rank) {
      fail(result_, 2, result_->rank, packet.destination % arg_->actors);
      return;
    }
    uint64_t expected = graph_packet_guard(packet.source, packet.destination,
                                            packet.edge, packet.contribution);
    if (packet.guard != expected) {
      fail(result_, 3, expected, packet.guard);
      return;
    }
    auto y = reinterpret_cast<int64_t*>(arg_->y_addr);
    uint64_t local = uint64_t(result_->rank) * arg_->owned_stride
                   + packet.destination / arg_->actors;
    y[local] += packet.contribution;
    ++result_->received;
    if (result_->received == result_->expected_received) {
      result_->data_end_cycle = vx_rdcycle();
    }
  }

  graph_kernel_arg_t* arg_;
  graph_actor_result_t* result_;
};

__kernel void kernel_main(graph_kernel_arg_t* __UNIFORM__ arg) {
  uint32_t rank = vx_core_id() * arg->actors_per_core
                + vx_warp_id() * arg->threads + vx_thread_id();
  auto result = reinterpret_cast<graph_actor_result_t*>(arg->results_addr) + rank;
  result->rank = rank;
#ifdef FORZA_GRAPH_SOFTWARE
  forza_graph_bind_transport(arg->sw_state_addr, arg->sw_slots_addr,
                             arg->sw_offsets_addr, arg->sw_queue_capacity, arg->actors);
#endif
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
    GraphSelector selector(arg, result);
    selector.start();
    auto rows = reinterpret_cast<const uint32_t*>(arg->rows_addr);
    auto edges = reinterpret_cast<const graph_edge_t*>(arg->edges_addr);
    auto x = reinterpret_cast<const int64_t*>(arg->x_addr);
    uint32_t vertex = rank;
    uint32_t edge = vertex < arg->vertices ? rows[vertex] : 0;
    int64_t x_value = vertex < arg->vertices ? x[vertex] : 0;
    vx_fence();
    vortex::gbarrier(2).arrive_and_wait();
    result->start_cycle = vx_rdcycle();
    if (result->expected_received == 0) {
      result->data_end_cycle = result->start_cycle;
    }
    bool submitted = false;
    for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
      while (vertex < arg->vertices && edge == rows[vertex + 1]) {
        vertex += arg->actors;
        if (vertex < arg->vertices) {
          edge = rows[vertex];
          x_value = x[vertex];
        }
      }
      if (vertex < arg->vertices) {
        graph_edge_t entry = edges[edge];
        int64_t contribution = x_value * entry.weight;
        graph_packet_t packet = {vertex, entry.destination, edge, 0, contribution,
          graph_packet_guard(vertex, entry.destination, edge, contribution)};
        uint32_t destination = entry.destination % arg->actors;
        if (selector.try_send(0, packet, destination)) {
          ++edge;
          ++result->sent;
        } else {
          ++result->retries;
        }
      }
      selector.progress();
      if (!selector.ok()) {
        fail(result, 4, 0, selector.error());
      }
      result->iterations = iteration + 1;
      uint32_t __UNIFORM__ all_submitted = vx_vote_all(vertex >= arg->vertices);
      if (all_submitted) {
        submitted = true;
        break;
      }
    }
    if (!submitted) {
      fail(result, 5, arg->edges, result->sent);
    }
    selector.done();
    vx_fence();
    result->end_cycle = vx_rdcycle();
    result->cycles = result->end_cycle - result->start_cycle;
    result->done_received = selector.done_received(0);
    result->outstanding = selector.outstanding(0);
    if (!selector.ok()) {
      fail(result, 6, 0, selector.error());
    }
    if (!selector.is_done() || result->done_received != arg->actors
        || result->outstanding != 0) {
      fail(result, 7, arg->actors, result->done_received);
    }
  });
  result->complete = FORZA_GRAPH_COMPLETE;
}
