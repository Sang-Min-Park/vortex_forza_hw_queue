#pragma once

#include <stdint.h>
#include <forza_mmio_abi.h>

#define FORZA_RECORD_BASE             0x71500000ULL
#define FORZA_RECORD_DIRECT_BASE      0x71580000ULL
#define FORZA_RECORD_APERTURE         0x00080000ULL
#define FORZA_RECORD_ZONE_STRIDE      0x00004000ULL
#define FORZA_RECORD_MAX_ZONES        16U
#define FORZA_RECORD_BYTES            24U
#define FORZA_RECORDS_PER_ZONE        (FORZA_RECORD_ZONE_STRIDE / FORZA_RECORD_BYTES)
#define FORZA_RECORD_VALID_BYTES      (FORZA_RECORD_ZONE_STRIDE * FORZA_RECORD_MAX_ZONES)
#define FORZA_RECORD_INVALID_ADDRESS  (FORZA_RECORD_BASE + FORZA_RECORD_APERTURE - 8)
#define FORZA_RECORD_DIRECT_INVALID_ADDRESS (FORZA_RECORD_DIRECT_BASE + FORZA_RECORD_APERTURE - 8)
#define FORZA_RECORD_REQUEST_OPCODE   0x81U
#define FORZA_RECORD_REPLY_OPCODE     0x82U
#define FORZA_RECORD_DIRECT_REQUEST_OPCODE 0x83U
#define FORZA_RECORD_DIRECT_REPLY_OPCODE   0x84U
#define FORZA_RECORD_REQUEST_MAILBOX  0U
#define FORZA_RECORD_REPLY_MAILBOX    1U
#define FORZA_RECORD_QUEUE_DEPTH      32U
#define FORZA_RECORD_CONTEXTS         4U
#define FORZA_RECORD_PEEK_BASE        FORZA_MMIO_RX_PEEK_BASE
#define FORZA_RECORD_RELEASE_BASE     FORZA_MMIO_RX_RELEASE_BASE
#define FORZA_RECORD_CQ_ID            (FORZA_MMIO_ENDPOINT_BASE + 0x1c0ULL)
#define FORZA_RECORD_CQ_COUNT         (FORZA_MMIO_ENDPOINT_BASE + 0x1c8ULL)
#define FORZA_RECORD_CQ_SUM           (FORZA_MMIO_ENDPOINT_BASE + 0x1d0ULL)
#define FORZA_RECORD_CQ_XOR           (FORZA_MMIO_ENDPOINT_BASE + 0x1d8ULL)
#define FORZA_RECORD_CQ_RELEASE       (FORZA_MMIO_ENDPOINT_BASE + 0x1e0ULL)

static inline uint64_t forza_record_address(uint32_t zone, uint32_t record) {
  if (zone >= FORZA_RECORD_MAX_ZONES || record >= FORZA_RECORDS_PER_ZONE) {
    return FORZA_RECORD_INVALID_ADDRESS;
  }
  return FORZA_RECORD_BASE + (uint64_t)zone * FORZA_RECORD_ZONE_STRIDE
       + (uint64_t)record * 8;
}

static inline uint64_t forza_record_pack(uint32_t request_id, int32_t delta) {
  return ((uint64_t)request_id << 32) | (uint32_t)delta;
}

static inline uint64_t forza_record_direct_address(uint32_t zone, uint32_t record) {
  if (zone >= FORZA_RECORD_MAX_ZONES || record >= FORZA_RECORDS_PER_ZONE) {
    return FORZA_RECORD_DIRECT_INVALID_ADDRESS;
  }
  return FORZA_RECORD_DIRECT_BASE + (uint64_t)zone * FORZA_RECORD_ZONE_STRIDE
       + (uint64_t)record * 8;
}

static inline uint64_t forza_record_peek_address(uint32_t mailbox) {
  return forza_mmio_peek_address(mailbox);
}

static inline uint64_t forza_record_release_address(uint32_t mailbox) {
  return forza_mmio_release_address(mailbox);
}
