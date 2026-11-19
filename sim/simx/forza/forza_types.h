#pragma once

#include <array>
#include "types.h"
#include "instr_trace.h"
#include <forza_abi.h>

namespace vortex {

struct ForzaCsrRequest {
  instr_trace_t* trace;
  uint32_t block;
};

struct ForzaMessage {
  uint32_t source_zone = 0;
  uint32_t source_actor = 0;
  uint32_t dest_zone = 0;
  uint32_t dest_pe = 0;
  uint32_t mailbox = 0;
  uint64_t sequence = 0;
  std::array<uint64_t, 8> words{};
};

struct ForzaAck {
  uint32_t dest_zone = 0;
  uint32_t actor = 0;
  uint32_t mailbox = 0;
  uint64_t sequence = 0;
  bool error = false;
};

inline bool is_forza_csr(uint32_t addr) {
  return addr == FORZA_CSR_ZENSTAT || addr == FORZA_CSR_ZQMSTAT
      || addr == FORZA_CSR_ZQMMBOXREG
      || (addr >= FORZA_CSR_ZQMDQ0 && addr < FORZA_CSR_ZQMDQ0 + 8)
      || addr == FORZA_CSR_ZENEQC || addr == FORZA_CSR_ZENEQD
      || addr == FORZA_CSR_ZENCOUNT || addr == FORZA_CSR_ZQMAVAIL
      || addr == FORZA_CSR_ZENTRY
      || (addr >= FORZA_CSR_ZQMMSG0 && addr < FORZA_CSR_ZQMMSG0 + 8);
}

}
