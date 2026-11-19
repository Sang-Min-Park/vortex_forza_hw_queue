#pragma once

#include <stdint.h>

#define FORZA_UPDATE_BASE             0x71400000ULL
#define FORZA_UPDATE_APERTURE         0x00080000ULL
#define FORZA_UPDATE_ZONE_STRIDE      0x00004000ULL
#define FORZA_UPDATE_MAX_ZONES        16U
#define FORZA_UPDATE_WORDS_PER_ZONE   2048U
#define FORZA_UPDATE_VALID_BYTES      (FORZA_UPDATE_ZONE_STRIDE * FORZA_UPDATE_MAX_ZONES)
#define FORZA_UPDATE_INVALID_ADDRESS  (FORZA_UPDATE_BASE + FORZA_UPDATE_APERTURE - 8)
#define FORZA_UPDATE_OPCODE           0x80U
#define FORZA_UPDATE_MAILBOX          0U
#define FORZA_UPDATE_QUEUE_DEPTH      32U
#define FORZA_UPDATE_INFLIGHT         32U

static inline uint64_t forza_update_address(uint32_t zone, uint32_t word_offset) {
  if (zone >= FORZA_UPDATE_MAX_ZONES || word_offset >= FORZA_UPDATE_WORDS_PER_ZONE) {
    return FORZA_UPDATE_INVALID_ADDRESS;
  }
  return FORZA_UPDATE_BASE + (uint64_t)zone * FORZA_UPDATE_ZONE_STRIDE
       + (uint64_t)word_offset * 8;
}
