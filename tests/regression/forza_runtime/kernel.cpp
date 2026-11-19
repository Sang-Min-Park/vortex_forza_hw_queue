#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <selector.forza.h>
#include "common.h"

#if __riscv_xlen != 64
#error "forza_runtime requires RV64"
#endif

static void fail(runtime_actor_result_t* result, uint32_t code,
                 uint64_t expected, uint64_t actual) {
  if (result->error == 0) {
    result->error = code;
    result->expected = expected;
    result->actual = actual;
  }
}

static void barrier() {
  vx_fence();
  vortex::gbarrier(1).arrive_and_wait();
}

class RuntimeSelector : public hclib::Selector<FORZA_RUNTIME_MAILBOXES, runtime_packet_t> {
public:
  RuntimeSelector(runtime_kernel_arg_t* arg, runtime_actor_result_t* result, uint32_t epoch)
    : hclib::Selector<FORZA_RUNTIME_MAILBOXES, runtime_packet_t>(epoch)
    , arg_(arg)
    , result_(result)
    , epoch_(epoch) {
    mb[0].process = [this](runtime_packet_t packet, int source) { handle(0, packet, source); };
    mb[1].process = [this](runtime_packet_t packet, int source) { handle(1, packet, source); };
  }

private:
  void handle(uint32_t mailbox, runtime_packet_t packet, int source) {
    if (source != -1 || packet.source >= arg_->num_actors) {
      fail(result_, FORZA_RUNTIME_SOURCE, arg_->num_actors, packet.source);
      return;
    }
    uint32_t rank = result_->rank;
    uint32_t expected_source = epoch_ == 2 && mailbox == 1
      ? (rank + 1) % arg_->num_actors : (rank + arg_->num_actors - 1) % arg_->num_actors;
    if ((epoch_ != 1 && packet.source != expected_source)
        || packet.destination != rank || packet.mailbox != mailbox) {
      fail(result_, FORZA_RUNTIME_SOURCE, rank, packet.destination);
    }
    uint64_t cookie = runtime_cookie(packet.source, rank, packet.sequence, mailbox, epoch_);
    if (packet.cookie != cookie || packet.guard != runtime_guard(cookie)) {
      fail(result_, FORZA_RUNTIME_PAYLOAD, cookie, packet.cookie);
    }
    uint32_t expected_sequence = next_[mailbox][packet.source];
    if (packet.sequence != expected_sequence) {
      fail(result_, FORZA_RUNTIME_ORDER, expected_sequence, packet.sequence);
    }
    ++next_[mailbox][packet.source];
    auto& stats = result_->epoch[epoch_];
    ++stats.handled[mailbox];
    stats.checksum[mailbox] += packet.cookie;
    if (epoch_ == 2 && mailbox == 0) {
      uint64_t reply_cookie = runtime_cookie(rank, packet.source, packet.sequence, 1, epoch_);
      runtime_packet_t reply = {rank, packet.source, packet.sequence, 1,
                                reply_cookie, runtime_guard(reply_cookie)};
      if (send(1, reply, packet.source)) {
        ++stats.sent;
      } else {
        fail(result_, FORZA_RUNTIME_SEND, 1, 0);
      }
    }
  }

  runtime_kernel_arg_t* arg_;
  runtime_actor_result_t* result_;
  uint32_t epoch_;
  uint32_t next_[FORZA_RUNTIME_MAILBOXES][FORZA_RUNTIME_MAX_PES]{};
};

static bool run_epoch(runtime_kernel_arg_t* arg, runtime_actor_result_t* result, uint32_t epoch) {
  RuntimeSelector selector(arg, result, epoch);
  auto& stats = result->epoch[epoch];
  selector.start();
  uint32_t next_send = 0;
  uint32_t rank = result->rank;
  uint32_t destination = epoch == 0 ? (rank + 1) % arg->num_actors : 0;
  uint32_t expected_received = epoch == 0 ? arg->rounds
    : (rank == 0 ? arg->rounds * arg->num_actors : 0);
  bool data_complete = false;
  result->phase = epoch * 10 + 1;
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
    if (next_send < arg->rounds * FORZA_RUNTIME_MAILBOXES) {
      uint32_t mailbox = next_send % FORZA_RUNTIME_MAILBOXES;
      uint32_t sequence = next_send / FORZA_RUNTIME_MAILBOXES;
      uint64_t cookie = runtime_cookie(rank, destination, sequence, mailbox, epoch);
      runtime_packet_t packet = {rank, destination, sequence, mailbox, cookie, runtime_guard(cookie)};
      if (selector.try_send(mailbox, packet, destination)) {
        ++next_send;
        ++stats.sent;
      } else {
        ++stats.retries;
      }
    }
    selector.progress();
    if (!selector.ok()) {
      fail(result, FORZA_RUNTIME_SEND, 0, selector.error());
    }
    bool local_complete = next_send == arg->rounds * FORZA_RUNTIME_MAILBOXES
                       && stats.handled[0] == expected_received
                       && stats.handled[1] == expected_received;
    uint32_t __UNIFORM__ all_complete = vx_vote_all(local_complete);
    stats.iterations = iteration + 1;
    if (all_complete) {
      data_complete = true;
      break;
    }
  }
  if (!data_complete) {
    fail(result, FORZA_RUNTIME_TIMEOUT, expected_received, stats.handled[0]);
  }
  // Every actor has finished its application work before closing this epoch.
  // The global barrier also keeps one warp's lanes from entering the next
  // phase while another lane still needs to poll and run callbacks.
  barrier();
  result->phase = epoch * 10 + 2;
  selector.request_done();
  bool done_complete = false;
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
    selector.progress();
    if (!selector.ok()) {
      fail(result, FORZA_RUNTIME_COMPLETION, 0, selector.error());
    }
    uint32_t __UNIFORM__ all_complete = vx_vote_all(selector.is_done());
    if (all_complete) {
      done_complete = true;
      break;
    }
  }
  if (!done_complete) {
    fail(result, FORZA_RUNTIME_TIMEOUT, 1, 0);
  }
  for (uint32_t mailbox = 0; mailbox < FORZA_RUNTIME_MAILBOXES; ++mailbox) {
    stats.done_received[mailbox] = selector.done_received(mailbox);
    stats.outstanding[mailbox] = selector.outstanding(mailbox);
    if (stats.done_received[mailbox] != arg->num_actors || stats.outstanding[mailbox] != 0) {
      fail(result, FORZA_RUNTIME_COMPLETION, arg->num_actors, stats.done_received[mailbox]);
    }
  }
  barrier();
  return data_complete && done_complete && selector.ok();
}

static bool run_request_reply(runtime_kernel_arg_t* arg, runtime_actor_result_t* result) {
  constexpr uint32_t epoch = 2;
  RuntimeSelector selector(arg, result, epoch);
  auto& stats = result->epoch[epoch];
  selector.start();
  uint32_t next_send = 0;
  uint32_t rank = result->rank;
  uint32_t destination = (rank + 1) % arg->num_actors;
  bool submitted = false;
  result->phase = 21;
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
    // A reply enables the next request. This bounds each actor's application
    // reply demand while preserving callbacks that send on another mailbox.
    if (next_send < arg->rounds && stats.handled[1] == next_send) {
      uint64_t cookie = runtime_cookie(rank, destination, next_send, 0, epoch);
      runtime_packet_t packet = {rank, destination, next_send, 0, cookie, runtime_guard(cookie)};
      if (selector.send(0, packet, destination)) {
        ++next_send;
        ++stats.sent;
      } else {
        ++stats.retries;
      }
    }
    uint32_t __UNIFORM__ all_submitted = vx_vote_all(next_send == arg->rounds);
    stats.iterations = iteration + 1;
    if (all_submitted) {
      submitted = true;
      break;
    }
    selector.progress();
    if (!selector.ok()) {
      fail(result, FORZA_RUNTIME_SEND, 0, selector.error());
    }
  }
  if (!submitted) {
    fail(result, FORZA_RUNTIME_TIMEOUT, arg->rounds, next_send);
  }
  result->phase = 22;
  stats.responses_at_close = stats.handled[1];
  selector.done();
  for (uint32_t mailbox = 0; mailbox < FORZA_RUNTIME_MAILBOXES; ++mailbox) {
    stats.done_received[mailbox] = selector.done_received(mailbox);
    stats.outstanding[mailbox] = selector.outstanding(mailbox);
    if (stats.handled[mailbox] != arg->rounds
        || stats.done_received[mailbox] != arg->num_actors || stats.outstanding[mailbox] != 0) {
      fail(result, FORZA_RUNTIME_COMPLETION, arg->rounds, stats.handled[mailbox]);
    }
  }
  if (!selector.ok()) {
    fail(result, FORZA_RUNTIME_COMPLETION, 0, selector.error());
  }
  barrier();
  return submitted && selector.is_done() && selector.ok();
}

__kernel void kernel_main(runtime_kernel_arg_t* __UNIFORM__ arg) {
  uint32_t core = vx_core_id();
  uint32_t warp = vx_warp_id();
  uint32_t lane = vx_thread_id();
  uint32_t local = warp * arg->num_threads + lane;
  uint32_t actor = core * arg->actors_per_core + local;
  auto result = reinterpret_cast<runtime_actor_result_t*>(arg->results_addr) + actor;
  result->core = core;
  result->warp = warp;
  result->lane = lane;
  result->rank = actor;
  result->phase = 100;
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
    uint64_t my_pe = hclib::forza_my_pe();
    uint64_t pes = hclib::forza_n_pes();
    if (my_pe != actor || pes != arg->num_actors) {
      fail(result, FORZA_RUNTIME_IDENTITY, actor, my_pe);
    }
    for (uint32_t epoch = 0; epoch < 2; ++epoch) {
      hclib::finish([&] { run_epoch(arg, result, epoch); });
    }
    hclib::finish([&] { run_request_reply(arg, result); });
  });
  result->phase = 200;
  result->done = FORZA_RUNTIME_COMPLETE;
}
