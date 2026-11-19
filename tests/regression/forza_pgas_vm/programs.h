#pragma once
#include <forza_vm_abi.h>

static inline uint64_t program_word(uint32_t pc) {
#if VM_PROGRAM == 0
  switch (pc) {
  case 0: return forza_vm_instruction(FORZA_VM_LD, 3, 0, 0, 0);
  case 1: return forza_vm_instruction(FORZA_VM_LD, 4, 0, 0, 8);
  case 2: return forza_vm_instruction(FORZA_VM_LD, 5, 0, 0, 16);
  case 3: return forza_vm_instruction(FORZA_VM_ADDI, 6, 3, 0, 1);
  case 4: return forza_vm_instruction(FORZA_VM_ST, 6, 0, 0, 0);
  case 5: return forza_vm_instruction(FORZA_VM_SEXT32, 7, 1, 0, 0);
  case 6: return forza_vm_instruction(FORZA_VM_ADD, 6, 4, 7, 0);
  case 7: return forza_vm_instruction(FORZA_VM_ST, 6, 0, 0, 8);
  case 8: return forza_vm_instruction(FORZA_VM_XOR, 6, 5, 2, 0);
  case 9: return forza_vm_instruction(FORZA_VM_ST, 6, 0, 0, 16);
  default: return forza_vm_instruction(FORZA_VM_RET, 3, 4, 5, 0);
  }
#else
  switch (pc) {
  case 0: return forza_vm_instruction(FORZA_VM_LD, 3, 0, 0, 0);
  case 1: return forza_vm_instruction(FORZA_VM_LD, 4, 0, 0, 8);
  case 2: return forza_vm_instruction(FORZA_VM_LD, 5, 0, 0, 16);
  case 3: return forza_vm_instruction(FORZA_VM_ADDI, 6, 3, 0, 1);
  case 4: return forza_vm_instruction(FORZA_VM_ST, 6, 0, 0, 0);
  case 5: return forza_vm_instruction(FORZA_VM_SEXT32, 7, 1, 0, 0);
  case 6: return forza_vm_instruction(FORZA_VM_BGE, 0, 7, 4, 8);
  case 7: return forza_vm_instruction(FORZA_VM_ST, 7, 0, 0, 8);
  case 8: return forza_vm_instruction(FORZA_VM_BGE, 0, 5, 7, 10);
  case 9: return forza_vm_instruction(FORZA_VM_ST, 7, 0, 0, 16);
  default: return forza_vm_instruction(FORZA_VM_RET, 3, 4, 5, 0);
  }
#endif
}

static inline bool install_program() {
  for (uint32_t pc = 0; pc < 11; ++pc) {
    bool accepted = false;
    for (uint32_t attempt = 0; attempt < 10000; ++attempt) {
      uint64_t status = swap64(reinterpret_cast<uint64_t*>(forza_vm_config_address(VM_PROGRAM, pc)),
                               program_word(pc));
      if (status == FORZA_MMIO_ACCEPTED) { accepted = true; break; }
      if (status != FORZA_MMIO_BUSY) return false;
    }
    if (!accepted) return false;
  }
  for (uint32_t attempt = 0; attempt < 10000; ++attempt)
    if (*reinterpret_cast<volatile const uint64_t*>(FORZA_VM_CONFIG_PENDING) == 0) return true;
  return false;
}
