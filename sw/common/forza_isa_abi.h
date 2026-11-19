#pragma once

#include <stdint.h>
#include <forza_abi.h>

#define FORZA_ISA_OPCODE 0x0bU
#define FORZA_ISA_FUNCT7 0x20U
#define FORZA_ISA_SEND_TRY 0U
#define FORZA_ISA_RECV_TRY 1U
#define FORZA_ISA_SEND_WAIT 2U
#define FORZA_ISA_RECV_WAIT 3U
#define FORZA_ISA_CREDITS 4U
#define FORZA_ISA_REGISTER 5U
#define FORZA_ISA_STATUS 6U
#define FORZA_ISA_RECV_WAIT_ANY 7U

static inline uint64_t forza_isa_route(uint32_t zone, uint32_t pe, uint32_t mailbox) {
  if (zone >= 16 || pe >= 2048 || mailbox >= 8) {
    return UINT64_MAX;
  }
  return forza_message_control(zone, pe, mailbox, 0);
}

#if defined(__riscv)
#define FORZA_ISA_BINARY(name, func) \
static inline uint64_t name(uint64_t first, uint64_t second, uint64_t identity) { \
  uint64_t result; \
  __asm__ volatile (".insn r 0x0b, " #func ", 0x20, %0, %1, %2" \
                    : "=r"(result) : "r"(first), "r"(second), "r"(identity) : "memory"); \
  return result; \
}

FORZA_ISA_BINARY(forza_isa_send_try, 0)
FORZA_ISA_BINARY(forza_isa_recv_try_raw, 1)
FORZA_ISA_BINARY(forza_isa_send_wait, 2)
FORZA_ISA_BINARY(forza_isa_recv_wait_raw, 3)
FORZA_ISA_BINARY(forza_isa_credits_raw, 4)
FORZA_ISA_BINARY(forza_isa_register_raw, 5)
FORZA_ISA_BINARY(forza_isa_status_raw, 6)
FORZA_ISA_BINARY(forza_isa_recv_wait_any_raw, 7)
#undef FORZA_ISA_BINARY

static inline uint64_t forza_isa_recv_try(uint64_t mailbox, uint64_t identity) {
  return forza_isa_recv_try_raw(mailbox, identity, identity);
}
static inline uint64_t forza_isa_recv_wait(uint64_t mailbox, uint64_t identity) {
  return forza_isa_recv_wait_raw(mailbox, identity, identity);
}
static inline uint64_t forza_isa_recv_wait_any(uint64_t mailbox, uint64_t identity) {
  return forza_isa_recv_wait_any_raw(mailbox, identity, identity);
}
static inline uint64_t forza_isa_credits(uint64_t identity) {
  return forza_isa_credits_raw(identity, identity, identity);
}
static inline uint64_t forza_isa_register(uint64_t registration, uint64_t identity) {
  return forza_isa_register_raw(registration, identity, identity);
}
static inline uint64_t forza_isa_status(uint64_t identity) {
  return forza_isa_status_raw(identity, identity, identity);
}
#endif
