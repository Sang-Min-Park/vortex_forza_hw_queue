// Experimental native FORZA message ABI for the SimX queue prototype.
#pragma once

#define FORZA_CSR_ZENSTAT    0x804
#define FORZA_CSR_ZQMSTAT    0x805
#define FORZA_CSR_ZQMMBOXREG 0x806
#define FORZA_CSR_ZQMDQ0     0x808
#define FORZA_CSR_ZQMDQ1     0x809
#define FORZA_CSR_ZENEQC     0x840
#define FORZA_CSR_ZENEQD     0x841
// Native 0xcc2/0xcc3 collide with Vortex core ID / active warp CSRs.
#define FORZA_CSR_ZENCOUNT   0x850
#define FORZA_CSR_ZQMAVAIL   0x851
// Vortex extension: atomic nonblocking commit. Busy discards the assembly.
#define FORZA_CSR_ZENTRY     0x852
#define FORZA_TRY_ACCEPTED   0
#define FORZA_TRY_BUSY       1
#define FORZA_TRY_INVALID    2
#define FORZA_CSR_ZQMMSG0    0x858

#define FORZA_MAILBOX_BASE     0x70000000ULL
#define FORZA_MAILBOX_APERTURE 0x100000ULL
#define FORZA_MESSAGE_BYTES 64
#define FORZA_PAYLOAD_WORDS 7
#define FORZA_NUM_MAILBOXES 8
#define FORZA_MAILBOX_SLOTS 2
#define FORZA_DEST_ZONE_SHIFT 15
#define FORZA_CONTROL_MBOX_SHIFT 32
#define FORZA_CONTROL_OPCODE_SHIFT 40

#ifndef __ASSEMBLER__
#include <stdint.h>

static inline uint64_t forza_message_control(uint32_t zone, uint32_t local_pe,
                                              uint32_t mailbox, uint32_t opcode) {
  return ((uint64_t)(zone & 15) << FORZA_DEST_ZONE_SHIFT)
       | (local_pe & 0x7ff)
       | ((uint64_t)(mailbox & 7) << FORZA_CONTROL_MBOX_SHIFT)
       | ((uint64_t)(opcode & 255) << FORZA_CONTROL_OPCODE_SHIFT);
}
#endif
