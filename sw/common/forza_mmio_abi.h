#pragma once

#include <stdint.h>

#define FORZA_MMIO_BASE             0x71000000ULL
#define FORZA_MMIO_APERTURE         0x00400000ULL
#define FORZA_MMIO_TX_BYTES         0x00200000ULL
#define FORZA_MMIO_ENDPOINT_BASE    (FORZA_MMIO_BASE + FORZA_MMIO_TX_BYTES)
#define FORZA_MMIO_ACTOR_STRIDE     0x200ULL
#define FORZA_MMIO_MAX_ACTORS       4096U
#define FORZA_MMIO_REGISTER         (FORZA_MMIO_BASE + 0x00200000ULL)
#define FORZA_MMIO_RX_STATUS        (FORZA_MMIO_BASE + 0x00200040ULL)
#define FORZA_MMIO_TX_STATUS        (FORZA_MMIO_BASE + 0x00200080ULL)
#define FORZA_MMIO_TX_COUNTS        (FORZA_MMIO_BASE + 0x002000c0ULL)
#define FORZA_MMIO_RX_POP_BASE      (FORZA_MMIO_BASE + 0x00200100ULL)
#define FORZA_MMIO_RX_PEEK_BASE     (FORZA_MMIO_ENDPOINT_BASE + 0x140ULL)
#define FORZA_MMIO_RX_RELEASE_BASE  (FORZA_MMIO_ENDPOINT_BASE + 0x180ULL)
#define FORZA_MMIO_INVALID_ADDRESS  (FORZA_MMIO_BASE + FORZA_MMIO_APERTURE - 8)

#define FORZA_MMIO_ACCEPTED 0ULL
#define FORZA_MMIO_BUSY     1ULL
#define FORZA_MMIO_INVALID  2ULL

static inline uint64_t forza_mmio_actor_address(uint32_t actor, uint64_t endpoint) {
  if (actor >= FORZA_MMIO_MAX_ACTORS || endpoint < FORZA_MMIO_ENDPOINT_BASE
      || endpoint - FORZA_MMIO_ENDPOINT_BASE >= FORZA_MMIO_ACTOR_STRIDE) {
    return FORZA_MMIO_INVALID_ADDRESS;
  }
  return endpoint + (uint64_t)actor * FORZA_MMIO_ACTOR_STRIDE;
}

static inline uint64_t forza_mmio_tx_address(uint32_t zone, uint32_t local_pe,
                                             uint32_t mailbox) {
  if (zone >= 16 || local_pe >= 2048 || mailbox >= 8) {
    return FORZA_MMIO_INVALID_ADDRESS;
  }
  return FORZA_MMIO_BASE + ((uint64_t)zone << 17)
       + ((uint64_t)local_pe << 6) + ((uint64_t)mailbox << 3);
}

static inline uint64_t forza_mmio_pop_address(uint32_t mailbox) {
  return mailbox < 8 ? FORZA_MMIO_RX_POP_BASE + (uint64_t)mailbox * 8
                     : FORZA_MMIO_INVALID_ADDRESS;
}

static inline uint64_t forza_mmio_peek_address(uint32_t mailbox) {
  return mailbox < 8 ? FORZA_MMIO_RX_PEEK_BASE + (uint64_t)mailbox * 8
                     : FORZA_MMIO_INVALID_ADDRESS;
}

static inline uint64_t forza_mmio_release_address(uint32_t mailbox) {
  return mailbox < 8 ? FORZA_MMIO_RX_RELEASE_BASE + (uint64_t)mailbox * 8
                     : FORZA_MMIO_INVALID_ADDRESS;
}

static inline uint64_t forza_mmio_pack_update(uint32_t key, int32_t delta) {
  return (uint64_t)key | ((uint64_t)(uint32_t)delta << 32);
}
