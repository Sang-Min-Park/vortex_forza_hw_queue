#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_software_abi.h>

#if !defined(VX_CFG_DCACHE_DISABLE)
#error "The controlled software mailbox baseline requires private D-cache disabled"
#endif
#if !defined(VX_CFG_L2_ENABLE)
#error "The controlled software mailbox baseline requires shared L2 enabled"
#endif

enum MSG_OP { DIRECT_OP, INDIRECT_OP, DONE_OP };
enum RET_STATUS {
  SUCCESS, DONE_MSG, NO_MSG, INVALID_NUM_MBOX, INVALID_MBOX, INVALID_LPE,
  INVALID_PRECINCT, INVALID_ZONE, ZEN_NOT_READY, INVALID_PTR, INVALID_PKT_SZ,
  MALLOC_FAIL, TRANSPORT_ERROR, PROTOCOL_ERROR
};

struct ForzaSoftwareContext {
  uint64_t metadata;
  uint64_t slots;
  uint32_t capacity;
  uint32_t actors;
  uint32_t rank;
  uint32_t cursor;
  uint32_t selected;
};

static __thread ForzaSoftwareContext forza_software_context;
static constexpr uint64_t FORZA_SW_DONE = 1ull << 63;
static constexpr uint64_t FORZA_SW_SEQUENCE = ~FORZA_SW_DONE;

inline uint64_t forza_vortex_pes_per_zone() {
  return uint64_t(vx_num_warps()) * vx_num_threads();
}
inline uint64_t forza_vortex_n_pes() {
  return uint64_t(vx_num_cores()) * forza_vortex_pes_per_zone();
}
inline uint64_t forza_vortex_local_pe() {
  return uint64_t(vx_warp_id()) * vx_num_threads() + vx_thread_id();
}
inline uint64_t forza_vortex_my_pe() {
  return uint64_t(vx_core_id()) * forza_vortex_pes_per_zone() + forza_vortex_local_pe();
}

inline void forza_graph_bind_transport(uint64_t metadata, uint64_t slots,
                                       uint64_t offsets, uint32_t capacity,
                                       uint32_t actors) {
  (void)offsets;
  forza_software_context = {metadata, slots, capacity, actors,
                            uint32_t(forza_vortex_my_pe()), 0, actors};
}

inline uint32_t forza_vortex_init() {
  const auto& ctx = forza_software_context;
  if (ctx.metadata == 0 || ctx.slots == 0 || ctx.capacity < 2
      || ctx.actors != forza_vortex_n_pes()
      || uint32_t(gridDim.x) != uint32_t(vx_num_cores())
      || uint32_t(gridDim.y) != 1 || uint32_t(gridDim.z) != 1
      || uint32_t(blockDim.x) != forza_vortex_pes_per_zone()
      || uint32_t(blockDim.y) != 1 || uint32_t(blockDim.z) != 1
      || get_cluster_size() != 1) {
    return INVALID_LPE;
  }
  __syncthreads();
  vortex::gbarrier(1).arrive_and_wait();
  return SUCCESS;
}

inline uint32_t forza_get_outstanding_msg_count(uint64_t mailbox, uint64_t* count) {
  if (mailbox != 0) {
    return INVALID_MBOX;
  }
  if (count == nullptr) {
    return INVALID_PTR;
  }
  // A published SPSC slot is already receiver-visible. Per-channel ordering
  // keeps its DONE marker behind its data; there is no transport ACK plane.
  *count = 0;
  return SUCCESS;
}

inline volatile forza_sw_queue_t* forza_sw_queue(uint32_t source, uint32_t destination) {
  auto& ctx = forza_software_context;
  auto queues = reinterpret_cast<volatile forza_sw_queue_t*>(ctx.metadata);
  return queues + uint64_t(source) * ctx.actors + destination;
}

inline volatile uint64_t* forza_sw_slot(uint32_t source, uint32_t destination,
                                       uint64_t position) {
  auto& ctx = forza_software_context;
  uint64_t channel = uint64_t(source) * ctx.actors + destination;
  uint64_t index = channel * ctx.capacity + position % ctx.capacity;
  return reinterpret_cast<volatile uint64_t*>(ctx.slots + index * 64);
}

inline uint32_t forza_message_available(uint64_t mailbox) {
  if (mailbox != 0) {
    return INVALID_MBOX;
  }
  auto& ctx = forza_software_context;
  if (ctx.selected < ctx.actors) {
    return SUCCESS;
  }
  for (uint32_t offset = 0; offset < ctx.actors; ++offset) {
    uint32_t source = (ctx.cursor + offset) % ctx.actors;
    uint64_t position = forza_sw_queue(source, ctx.rank)->consumed;
    uint64_t sequence = forza_sw_slot(source, ctx.rank, position)[0];
    if ((sequence & FORZA_SW_SEQUENCE) == position + 1) {
      ctx.selected = source;
      return SUCCESS;
    }
  }
  return NO_MSG;
}

inline uint32_t forza_message_receive(uint64_t mailbox, uint64_t* packet, size_t size) {
  if (packet == nullptr) {
    return INVALID_PTR;
  }
  if (size != 56) {
    return INVALID_PKT_SZ;
  }
  uint32_t available = forza_message_available(mailbox);
  if (available != SUCCESS) {
    return available;
  }
  auto& ctx = forza_software_context;
  uint32_t source = ctx.selected;
  auto queue = forza_sw_queue(source, ctx.rank);
  uint64_t position = queue->consumed;
  auto slot = forza_sw_slot(source, ctx.rank, position);
  uint64_t sequence = slot[0];
  __asm__ volatile ("fence r, rw" ::: "memory");
  for (uint32_t word = 0; word < 7; ++word) {
    packet[word] = slot[word + 1];
  }
  __asm__ volatile ("fence r, rw" ::: "memory");
  slot[0] = position + ctx.capacity;
  queue->consumed = position + 1;
  ctx.cursor = (source + 1) % ctx.actors;
  ctx.selected = ctx.actors;
  return (sequence & FORZA_SW_DONE) ? DONE_MSG : SUCCESS;
}

inline uint32_t forza_sw_send_packet(uint64_t mailbox, const void* packet,
                                    uint64_t precinct, uint64_t zone,
                                    uint64_t local_pe, bool done) {
  if (mailbox != 0) {
    return INVALID_MBOX;
  }
  if (precinct != 0) {
    return INVALID_PRECINCT;
  }
  if (zone >= uint64_t(vx_num_cores())) {
    return INVALID_ZONE;
  }
  if (local_pe >= forza_vortex_pes_per_zone()) {
    return INVALID_LPE;
  }
  if (packet == nullptr) {
    return INVALID_PTR;
  }
  auto& ctx = forza_software_context;
  uint32_t destination = zone * forza_vortex_pes_per_zone() + local_pe;
  auto queue = forza_sw_queue(ctx.rank, destination);
  uint64_t position = queue->published;
  auto slot = forza_sw_slot(ctx.rank, destination, position);
  if (slot[0] != position) {
    return ZEN_NOT_READY;
  }
  auto words = reinterpret_cast<const uint64_t*>(packet);
  for (uint32_t word = 0; word < 7; ++word) {
    slot[word + 1] = words[word];
  }
  // The publication word shares the payload's cache line. With private L1
  // disabled, both endpoints observe the same shared-L2 line and bank order.
  __asm__ volatile ("fence rw, w" ::: "memory");
  slot[0] = (position + 1) | (done ? FORZA_SW_DONE : 0);
  queue->published = position + 1;
  return SUCCESS;
}

inline uint32_t forza_send(uint64_t mailbox, const void* packet, uint64_t precinct,
                           uint64_t zone, uint64_t local_pe) {
  return forza_sw_send_packet(mailbox, packet, precinct, zone, local_pe, false);
}

inline uint32_t forza_send_done_epoch(uint64_t mailbox, uint64_t precinct,
                                      uint64_t zone, uint64_t local_pe, uint64_t epoch) {
  uint64_t packet[7] = {forza_vortex_my_pe(), epoch, 0, 0, 0, 0, 0};
  return forza_sw_send_packet(mailbox, packet, precinct, zone, local_pe, true);
}
