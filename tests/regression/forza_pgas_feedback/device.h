#pragma once
#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_mmio_abi.h>
#include <forza_isa_abi.h>
#include "common.h"

#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_UPDATE_ENABLE)
#error "Feedback tests require ordinary GPU handlers, without arithmetic engines"
#endif
static inline uint64_t swap64(volatile uint64_t* address, uint64_t value) {
  uint64_t old;
  __asm__ volatile("amoswap.d %0,%2,(%1)" : "=r"(old)
                   : "r"(address), "r"(value) : "memory");
  return old;
}
static inline uint64_t or64(volatile uint64_t* address, uint64_t value) {
  uint64_t old;
  __asm__ volatile("amoor.d %0,%2,(%1)" : "=r"(old)
                   : "r"(address), "r"(value) : "memory");
  return old;
}
static inline uint64_t and64(volatile uint64_t* address, uint64_t value) {
  uint64_t old;
  __asm__ volatile("amoand.d %0,%2,(%1)" : "=r"(old)
                   : "r"(address), "r"(value) : "memory");
  return old;
}
static inline uint32_t rank_of(const args_t* a) {
  return vx_core_id() * a->per_core + vx_warp_id() * a->threads + vx_thread_id();
}
static inline void barrier(uint32_t id) {
  vx_fence();
  vortex::gbarrier(id).arrive_and_wait();
}
static inline uint64_t endpoint(uint64_t address) {
  return address + uint64_t(vx_warp_id() * 4 + vx_thread_id()) * FORZA_MMIO_ACTOR_STRIDE;
}
static inline uint64_t identity() { return vx_warp_id() * 4 + vx_thread_id(); }
static inline uint64_t send_token(const args_t* a, uint32_t core, uint32_t pe,
                                   uint32_t mailbox, uint64_t token) {
#if MESSAGE_TRANSPORT == 4
  uint32_t clients = a->cores * 4, server = core * 4 + pe;
  auto slots = reinterpret_cast<volatile uint64_t*>(a->queue_slots);
  auto ready = reinterpret_cast<volatile uint64_t*>(a->queue_ready);
  if (mailbox == 1) {
    uint32_t client = core * 4 + pe - 4;
    auto slot = slots + uint64_t(clients) * clients + client;
    if (*slot) return FORZA_MMIO_BUSY;
    return swap64(slot, token) == 0 ? FORZA_MMIO_ACCEPTED : FORZA_MMIO_INVALID;
  }
  uint32_t source = vx_core_id() * 4 + vx_thread_id();
  bool stop = bool(token & TOKEN_STOP);
  auto slot = stop ? slots + uint64_t(clients) * clients + clients + server
                   : slots + uint64_t(server) * clients + source;
  uint64_t bit = uint64_t(1) << (stop ? clients : source);
  if (*slot) return FORZA_MMIO_BUSY;
  if (swap64(slot, token) != 0) return FORZA_MMIO_INVALID;
  or64(ready + server, bit);
  return FORZA_MMIO_ACCEPTED;
#elif MESSAGE_TRANSPORT == 2
  (void)a;
  return forza_isa_send_try(forza_isa_route(core, pe, mailbox), token, identity());
#elif MESSAGE_TRANSPORT == 3
  (void)a;
  return forza_isa_send_wait(forza_isa_route(core, pe, mailbox), token, identity());
#else
  (void)a;
  uint64_t address = FORZA_MMIO_BASE + (uint64_t(core) << 17)
                   + (uint64_t(pe) << 6) + (uint64_t(mailbox) << 3);
  return swap64(reinterpret_cast<uint64_t*>(address), token);
#endif
}
static inline uint64_t pop_token(const args_t* a, uint32_t mailbox, bool wait) {
#if MESSAGE_TRANSPORT == 4
  (void)wait;
  uint32_t clients = a->cores * 4, local_id = vx_core_id() * 4 + vx_thread_id();
  auto slots = reinterpret_cast<volatile uint64_t*>(a->queue_slots);
  if (mailbox == 1) {
    auto slot = slots + uint64_t(clients) * clients + local_id;
    return *slot ? swap64(slot, 0) : 0;
  }
  auto ready = reinterpret_cast<volatile uint64_t*>(a->queue_ready) + local_id;
  uint64_t available = *ready;
  if (!available) return 0;
  uint32_t source = __builtin_ctzll(available);
  auto slot = source == clients ? slots + uint64_t(clients) * clients + clients + local_id
                               : slots + uint64_t(local_id) * clients + source;
  uint64_t token = swap64(slot, 0);
  and64(ready, ~(uint64_t(1) << source));
  return token;
#elif MESSAGE_TRANSPORT == 2
  (void)a;
  (void)wait;
  return forza_isa_recv_try(mailbox, identity());
#elif MESSAGE_TRANSPORT == 3
  (void)a;
  return wait ? forza_isa_recv_wait_any(mailbox, identity()) : forza_isa_recv_try(mailbox, identity());
#else
  (void)a;
  (void)wait;
  return swap64(reinterpret_cast<uint64_t*>(endpoint(forza_mmio_pop_address(mailbox))), 0);
#endif
}
static inline uint64_t credits() {
#if MESSAGE_TRANSPORT == 4
  return 0;
#elif MESSAGE_TRANSPORT >= 2
  return forza_isa_credits(identity()) & 65535;
#else
  return *reinterpret_cast<volatile const uint64_t*>(endpoint(FORZA_MMIO_TX_COUNTS)) & 65535;
#endif
}
static inline bool register_endpoint() {
#if MESSAGE_TRANSPORT == 4
  return true;
#else
  uint64_t registration = (uint64_t(vx_warp_id() * 4 + vx_thread_id()) << 8) | 3;
#if MESSAGE_TRANSPORT >= 2
  return forza_isa_register(registration, identity()) == 0;
#else
  return swap64(reinterpret_cast<uint64_t*>(endpoint(FORZA_MMIO_REGISTER)), registration) == 0;
#endif
#endif
}
static inline bool healthy() {
#if MESSAGE_TRANSPORT == 4
  return true;
#else
  uint64_t tx = *reinterpret_cast<volatile const uint64_t*>(endpoint(FORZA_MMIO_TX_STATUS));
  uint64_t rx = *reinterpret_cast<volatile const uint64_t*>(endpoint(FORZA_MMIO_RX_STATUS));
  return (tx >> 63) && !(tx & (UINT64_C(0x1ff) << 8)) && !(rx & (UINT64_C(255) << 8));
#endif
}
template <uint32_t Layout> static inline volatile uint64_t* state_word(
    const args_t* a, uint32_t key, uint32_t field) {
  if constexpr (Layout == 2) {
    return reinterpret_cast<volatile uint64_t*>(__local_mem()) + uint64_t(key) * 2 + field;
  } else if constexpr (Layout == 1) {
    return reinterpret_cast<volatile uint64_t*>(__local_mem())
         + uint64_t(key >> a->core_shift) * 3 + field;
  } else {
    return reinterpret_cast<volatile uint64_t*>(a->state) + uint64_t(key) * 4 + field;
  }
}
template <uint32_t Layout> static inline uint64_t handle_query(
    const args_t* a, uint32_t key, uint32_t client, uint32_t step, uint32_t cookie) {
  if (a->kind == KIND_WALK) {
    uint32_t owner = key & (a->cores - 1), probe = key >> a->core_shift;
    uint32_t value = cookie, link = 0;
    for (uint32_t i = 0; i < a->work; ++i) {
      uint32_t current = (probe << a->core_shift) | owner;
      link = uint32_t(*state_word<Layout>(a, current, 0));
      uint32_t salt = uint32_t(*state_word<Layout>(a, current, 1));
      value = mix32(value ^ salt ^ link ^ i);
      probe = (link >> a->core_shift) & ((a->keys >> a->core_shift) - 1);
    }
    return reply_token((link ^ value) & (a->active_keys - 1), 0, value);
  }
  uint32_t link = uint32_t(*state_word<Layout>(a, key, 0));
  uint32_t old_value = uint32_t(*state_word<Layout>(a, key, 1));
  uint32_t version = uint32_t(*state_word<Layout>(a, key, 2));
  uint32_t value = updated_value(old_value, version, client, step, link, a->work, cookie);
  // Both committed-store responses must be available before publishing the reply.
  old_value = uint32_t(swap64(state_word<Layout>(a, key, 1), value));
  version = uint32_t(swap64(state_word<Layout>(a, key, 2), version + 1));
  return reply_token(update_next(old_value, version, client, step, link, a->active_keys, cookie),
                     version, old_value);
}
