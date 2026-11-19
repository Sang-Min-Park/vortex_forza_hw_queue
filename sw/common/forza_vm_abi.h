#pragma once

#include <stdint.h>
#include <forza_record_abi.h>

#define FORZA_VM_BASE              0x71600000ULL
#define FORZA_VM_DIRECT_BASE       0x71700000ULL
#define FORZA_VM_CONFIG_BASE       0x71800000ULL
#define FORZA_VM_APERTURE          0x00100000ULL
#define FORZA_VM_ZONE_STRIDE       0x00010000ULL
#define FORZA_VM_PROGRAM_STRIDE    0x00002000ULL
#define FORZA_VM_CONFIG_BYTES      0x00001000ULL
#define FORZA_VM_CONFIG_PENDING    (FORZA_VM_CONFIG_BASE + 0x800ULL)
#define FORZA_VM_PROGRAMS          4U
#define FORZA_VM_PROGRAM_WORDS     32U
#define FORZA_VM_REGISTERS         8U
#define FORZA_VM_CONTEXTS          4U
#define FORZA_VM_QUEUE_DEPTH       32U
#define FORZA_VM_STEP_LIMIT        256U
#define FORZA_VM_REGION_BYTES      64U
#define FORZA_VM_REGIONS           256U
#define FORZA_VM_REQUEST_OPCODE    0x85U
#define FORZA_VM_REPLY_OPCODE      0x86U
#define FORZA_VM_DIRECT_REQUEST_OPCODE 0x87U
#define FORZA_VM_DIRECT_REPLY_OPCODE   0x88U

enum forza_vm_opcode {
  FORZA_VM_LD = 1, FORZA_VM_ST = 2, FORZA_VM_ADD = 3,
  FORZA_VM_XOR = 4, FORZA_VM_AND = 5, FORZA_VM_ADDI = 6,
  FORZA_VM_IMM = 7, FORZA_VM_SEXT32 = 8, FORZA_VM_BGE = 9,
  FORZA_VM_BEQ = 10, FORZA_VM_RET = 11,
};

static inline uint64_t forza_vm_instruction(uint32_t op, uint32_t dst,
                                           uint32_t a, uint32_t b, int32_t imm) {
  return op | (uint64_t(dst) << 8) | (uint64_t(a) << 12)
       | (uint64_t(b) << 16) | (uint64_t(uint32_t(imm)) << 32);
}

static inline uint64_t forza_vm_address(uint32_t zone, uint32_t region,
                                       uint32_t program, uint32_t direct) {
  if (zone >= 16 || region >= FORZA_VM_REGIONS || program >= FORZA_VM_PROGRAMS)
    return FORZA_VM_BASE + FORZA_VM_APERTURE - 8;
  return (direct ? FORZA_VM_DIRECT_BASE : FORZA_VM_BASE)
       + uint64_t(zone) * FORZA_VM_ZONE_STRIDE
       + uint64_t(program) * FORZA_VM_PROGRAM_STRIDE + uint64_t(region) * 8;
}

static inline uint64_t forza_vm_config_address(uint32_t program, uint32_t pc) {
  return FORZA_VM_CONFIG_BASE
       + (uint64_t(program) * FORZA_VM_PROGRAM_WORDS + pc) * 8;
}

// Registers start with region address, arg0:u32, arg1:u32, then zeroes.
static inline uint64_t forza_vm_pack(uint32_t arg0, uint32_t arg1) {
  return uint64_t(arg0) | (uint64_t(arg1) << 32);
}
