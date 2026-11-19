#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_abi.h>

#if defined(VX_CFG_NUM_CLUSTERS) && VX_CFG_NUM_CLUSTERS != 1
#error "FORZA Vortex runtime currently requires one cluster for its global barriers"
#endif

enum MSG_OP { DIRECT_OP, INDIRECT_OP, DONE_OP };
enum RET_STATUS {
  SUCCESS, DONE_MSG, NO_MSG, INVALID_NUM_MBOX, INVALID_MBOX, INVALID_LPE,
  INVALID_PRECINCT, INVALID_ZONE, ZEN_NOT_READY, INVALID_PTR, INVALID_PKT_SZ,
  MALLOC_FAIL, TRANSPORT_ERROR, PROTOCOL_ERROR
};

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

inline uint64_t forza_get_num_precincts() { return 1; }
inline uint64_t forza_get_zones_per_precinct() { return vx_num_cores(); }
inline uint64_t forza_get_my_precinct() { return 0; }
inline uint64_t forza_get_my_zone() { return vx_core_id(); }

inline uint32_t forza_vortex_init() {
  if (uint32_t(gridDim.x) != uint32_t(vx_num_cores())
      || uint32_t(gridDim.y) != 1 || uint32_t(gridDim.z) != 1
      || uint32_t(blockDim.x) != forza_vortex_pes_per_zone()
      || uint32_t(blockDim.y) != 1 || uint32_t(blockDim.z) != 1
      || get_cluster_size() != 1) {
    return INVALID_LPE;
  }
  csr_write(FORZA_CSR_ZQMMBOXREG, (forza_vortex_local_pe() << 8) | 0xff);
  __syncthreads();
  vortex::gbarrier registration(1);
  registration.arrive_and_wait();
  uint64_t status = csr_read(FORZA_CSR_ZENSTAT);
  return ((status >> 63) && !(status & (0x1ffull << 8))) ? SUCCESS : TRANSPORT_ERROR;
}

inline uint32_t forza_get_outstanding_msg_count(uint64_t mailbox, uint64_t* count) {
  if (mailbox >= 8) {
    return INVALID_MBOX;
  }
  if (count == nullptr) {
    return INVALID_PTR;
  }
  *count = (csr_read(FORZA_CSR_ZENCOUNT) >> (8 * mailbox)) & 255;
  return SUCCESS;
}

inline uint32_t forza_message_available(uint64_t mailbox) {
  if (mailbox >= 8) {
    return INVALID_MBOX;
  }
  uint64_t status = csr_read(FORZA_CSR_ZQMSTAT);
  if (status & (1ull << (8 + mailbox))) {
    return TRANSPORT_ERROR;
  }
  return (status & (1ull << mailbox)) ? SUCCESS : NO_MSG;
}

inline uint64_t forza_vortex_dequeue(uint64_t mailbox) {
  switch (mailbox) {
  case 0: return csr_read(FORZA_CSR_ZQMDQ0 + 0);
  case 1: return csr_read(FORZA_CSR_ZQMDQ0 + 1);
  case 2: return csr_read(FORZA_CSR_ZQMDQ0 + 2);
  case 3: return csr_read(FORZA_CSR_ZQMDQ0 + 3);
  case 4: return csr_read(FORZA_CSR_ZQMDQ0 + 4);
  case 5: return csr_read(FORZA_CSR_ZQMDQ0 + 5);
  case 6: return csr_read(FORZA_CSR_ZQMDQ0 + 6);
  case 7: return csr_read(FORZA_CSR_ZQMDQ0 + 7);
  default: return 0;
  }
}

inline void forza_vortex_release(uint64_t mailbox) {
  // A CSR write does not by itself order preceding LSU payload loads.
  __asm__ volatile ("fence r, rw" ::: "memory");
  switch (mailbox) {
  case 0: csr_write(FORZA_CSR_ZQMDQ0 + 0, 0); break;
  case 1: csr_write(FORZA_CSR_ZQMDQ0 + 1, 0); break;
  case 2: csr_write(FORZA_CSR_ZQMDQ0 + 2, 0); break;
  case 3: csr_write(FORZA_CSR_ZQMDQ0 + 3, 0); break;
  case 4: csr_write(FORZA_CSR_ZQMDQ0 + 4, 0); break;
  case 5: csr_write(FORZA_CSR_ZQMDQ0 + 5, 0); break;
  case 6: csr_write(FORZA_CSR_ZQMDQ0 + 6, 0); break;
  case 7: csr_write(FORZA_CSR_ZQMDQ0 + 7, 0); break;
  }
}

inline uint32_t forza_message_receive(uint64_t mailbox, uint64_t* packet, size_t size) {
  if (packet == nullptr) {
    return INVALID_PTR;
  }
  if (size != FORZA_PAYLOAD_WORDS * sizeof(uint64_t)) {
    return INVALID_PKT_SZ;
  }
  uint32_t available = forza_message_available(mailbox);
  if (available != SUCCESS) {
    return available;
  }
  uint64_t pointer = forza_vortex_dequeue(mailbox);
  if (pointer == 0) {
    return TRANSPORT_ERROR;
  }
  auto words = reinterpret_cast<volatile const uint64_t*>(pointer);
  uint32_t opcode = (words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255;
  for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
    packet[word] = words[word + 1];
  }
  forza_vortex_release(mailbox);
  return opcode == DIRECT_OP ? SUCCESS : opcode == DONE_OP ? DONE_MSG : PROTOCOL_ERROR;
}

inline uint32_t forza_vortex_send_packet(uint64_t mailbox, const void* packet,
                                        uint64_t precinct, uint64_t zone,
                                        uint64_t logical_pe, uint32_t opcode) {
  if (mailbox >= 8) {
    return INVALID_MBOX;
  }
  if (precinct != 0) {
    return INVALID_PRECINCT;
  }
  if (zone >= uint64_t(vx_num_cores())) {
    return INVALID_ZONE;
  }
  if (logical_pe >= forza_vortex_pes_per_zone()) {
    return INVALID_LPE;
  }
  if (packet == nullptr) {
    return INVALID_PTR;
  }
  uint64_t sender_status = csr_read(FORZA_CSR_ZENSTAT);
  if (!(sender_status >> 63) || (sender_status & (0x1ffull << 8))) {
    return TRANSPORT_ERROR;
  }
  // Busy is a hint only. ZENTRY atomically checks and commits every lane.
  if (sender_status & (1ull << mailbox)) {
    return ZEN_NOT_READY;
  }
  auto bytes = reinterpret_cast<const uint8_t*>(packet);
  for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
    uint64_t value;
    memcpy(&value, bytes + word * 8, 8);
    csr_write(FORZA_CSR_ZENEQD, value);
  }
  uint64_t control = forza_message_control(zone, logical_pe, mailbox, opcode);
  uint64_t result;
  __asm__ volatile ("csrrw %0, %1, %2"
                    : "=r"(result) : "i"(FORZA_CSR_ZENTRY), "r"(control) : "memory");
  return result == FORZA_TRY_ACCEPTED ? SUCCESS
       : result == FORZA_TRY_BUSY ? ZEN_NOT_READY : TRANSPORT_ERROR;
}

inline uint32_t forza_send(uint64_t mailbox, const void* packet, uint64_t precinct,
                           uint64_t zone, uint64_t logical_pe) {
  return forza_vortex_send_packet(mailbox, packet, precinct, zone, logical_pe, DIRECT_OP);
}

inline uint32_t forza_send_done_epoch(uint64_t mailbox, uint64_t precinct,
                                      uint64_t zone, uint64_t logical_pe, uint64_t epoch) {
  uint64_t packet[FORZA_PAYLOAD_WORDS] = {forza_vortex_my_pe(), epoch, 0, 0, 0, 0, 0};
  return forza_vortex_send_packet(mailbox, packet, precinct, zone, logical_pe, DONE_OP);
}
