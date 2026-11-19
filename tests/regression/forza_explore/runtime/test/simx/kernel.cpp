#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <selector.forza.h>
#include "common.h"

template <unsigned N> struct Packet { unsigned char bytes[N]; };
template <unsigned N> static Packet<N> make_packet(uint32_t rank, uint32_t stage) {
  Packet<N> packet{};
  for (unsigned i = 0; i < N; ++i) packet.bytes[i] = (rank * 13 + stage * 7 + i) & 255;
  return packet;
}

template <unsigned N>
static __attribute__((noinline)) void stage(smoke_arg_t* arg, smoke_result_t* result,
                                           uint32_t rank, uint32_t peer, uint32_t epoch) {
  hclib::Selector<1, Packet<N>> selector(epoch);
  selector.mb[0].process = [&](Packet<N> packet, int source) {
    auto expected = make_packet<N>(peer, epoch);
    if (source != -1 || memcmp(&packet, &expected, N) != 0) ++result->errors;
    ++result->received;
  };
  auto packet = make_packet<N>(rank, epoch);
  bool sent = false;
  while (!vx_vote_all(sent)) {
    if (!sent) sent = selector.try_send(0, packet, peer);
    selector.progress();
  }
  selector.done();
  result->done += selector.done_received(0);
  if (!selector.ok() || !selector.is_done() || selector.done_received(0) != arg->actors
      || selector.outstanding(0) != 0) ++result->errors;
  vortex::gbarrier(2).arrive_and_wait();
}

static __attribute__((noinline)) void replies(smoke_arg_t* arg, smoke_result_t* result,
                                             uint32_t rank, uint32_t peer) {
  hclib::Selector<2, Packet<40>> selector(6);
  selector.mb[0].process = [&](Packet<40> packet, int source) {
    auto expected = make_packet<40>(peer, 6);
    if (source != -1 || memcmp(&packet, &expected, 40) != 0) ++result->errors;
    ++result->received;
    auto reply = make_packet<40>(rank, 7);
    if (!selector.send(1, reply, peer)) ++result->errors;
  };
  selector.mb[1].process = [&](Packet<40> packet, int source) {
    auto expected = make_packet<40>(peer, 7);
    if (source != -1 || memcmp(&packet, &expected, 40) != 0) ++result->errors;
    ++result->received;
  };
  auto packet = make_packet<40>(rank, 6);
  bool sent = false;
  while (!vx_vote_all(sent)) {
    if (!sent) sent = selector.try_send(0, packet, peer);
    selector.progress();
  }
  selector.done();
  result->done += selector.done_received(0) + selector.done_received(1);
  if (!selector.ok() || !selector.is_done() || selector.done_received(0) != arg->actors
      || selector.done_received(1) != arg->actors) ++result->errors;
  vortex::gbarrier(2).arrive_and_wait();
}

__kernel void kernel_main(smoke_arg_t* __UNIFORM__ arg) {
  uint32_t rank = vx_core_id() * arg->per_core + vx_warp_id() * arg->threads + vx_thread_id();
  uint32_t peer = rank < arg->per_core ? rank + arg->per_core : rank - arg->per_core;
  auto result = reinterpret_cast<smoke_result_t*>(arg->results) + rank;
  result->rank = rank;
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
    stage<1>(arg, result, rank, peer, 1);
    stage<8>(arg, result, rank, peer, 2);
    stage<16>(arg, result, rank, peer, 3);
    stage<40>(arg, result, rank, peer, 4);
    stage<56>(arg, result, rank, peer, 5);
    replies(arg, result, rank, peer);
    uint8_t byte = rank + 1;
    bool sent = false;
    while (!vx_vote_all(sent)) {
      if (!sent) sent = forza_send_sized(0, &byte, 1, 0, peer / arg->per_core,
                                         peer % arg->per_core) == SUCCESS;
    }
    uint64_t packet[7]{};
    while (forza_message_available(0) == NO_MSG) {}
    if (forza_message_receive(0, packet, 56) != SUCCESS || packet[0] != peer + 1) ++result->errors;
    for (unsigned word = 1; word < 7; ++word) if (packet[word] != 0) ++result->errors;
    ++result->received;
    uint64_t outstanding = 1;
    do { forza_get_outstanding_msg_count(0, &outstanding); } while (outstanding != 0);
    vortex::gbarrier(2).arrive_and_wait();
  });
  result->complete = SMOKE_COMPLETE;
}
