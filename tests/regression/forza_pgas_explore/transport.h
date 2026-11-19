#pragma once

#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_abi.h>
#include <forza_mmio_abi.h>
#include <forza_message_lib.vortex.h>

namespace pgas_transport {
enum { EMPTY_OR_BUSY = 0, READY_OR_ACCEPTED = 1, ERROR = 2 };

static inline __attribute__((always_inline)) uint64_t endpoint(uint64_t base) {
  uint32_t actor = uint32_t(vx_warp_id()) * VX_CFG_NUM_THREADS + uint32_t(vx_thread_id());
  return forza_mmio_actor_address(actor, base);
}

static inline __attribute__((always_inline)) uint64_t swap(uint64_t address, uint64_t value) {
  uint64_t result;
  __asm__ volatile ("amoswap.d %0, %2, (%1)" : "=r"(result)
                    : "r"(address), "r"(value) : "memory");
  return result;
}

static inline __attribute__((always_inline)) uint64_t load(uint64_t address) {
  return *reinterpret_cast<volatile const uint64_t*>(address);
}

static inline bool healthy() {
#ifdef PGAS_TRANSPORT_MMIO
  uint64_t tx = load(endpoint(FORZA_MMIO_TX_STATUS));
  uint64_t rx = load(endpoint(FORZA_MMIO_RX_STATUS));
#else
  uint64_t tx = csr_read(FORZA_CSR_ZENSTAT);
  uint64_t rx = csr_read(FORZA_CSR_ZQMSTAT);
#endif
  return (tx >> 63) && !(tx & (UINT64_C(0x1ff) << 8)) && !(rx & (UINT64_C(255) << 8));
}

static inline bool init() {
#ifdef PGAS_TRANSPORT_MMIO
  uint64_t pe = uint64_t(vx_warp_id()) * vx_num_threads() + vx_thread_id();
  uint64_t result = swap(endpoint(FORZA_MMIO_REGISTER), (pe << 8) | 255);
  __syncthreads();
  vx_fence();
  vortex::gbarrier(1).arrive_and_wait();
  return result == FORZA_MMIO_ACCEPTED && healthy();
#else
  return forza_vortex_init() == SUCCESS;
#endif
}

static inline __attribute__((always_inline)) uint32_t try_send(
    uint32_t zone, uint32_t local_pe, uint64_t payload, uint32_t mailbox = 0) {
  uint64_t result;
#ifdef PGAS_TRANSPORT_MMIO
  result = swap(forza_mmio_tx_address(zone, local_pe, mailbox), payload);
#else
  csr_write(FORZA_CSR_ZENEQD, payload);
  uint64_t control = forza_message_control(zone, local_pe, mailbox, DIRECT_OP);
  __asm__ volatile ("csrrw %0, %1, %2" : "=r"(result)
                    : "i"(FORZA_CSR_ZENTRY), "r"(control) : "memory");
#endif
  return result == FORZA_MMIO_ACCEPTED ? READY_OR_ACCEPTED
       : result == FORZA_MMIO_BUSY ? EMPTY_OR_BUSY : ERROR;
}

static inline __attribute__((always_inline)) uint32_t try_receive(
    uint64_t& control, uint64_t& payload, uint32_t mailbox = 0) {
#ifdef PGAS_TRANSPORT_MMIO
  uint64_t status = load(endpoint(FORZA_MMIO_RX_STATUS));
  if (status & (UINT64_C(1) << (8 + mailbox))) return ERROR;
  if (!(status & (UINT64_C(1) << mailbox))) return EMPTY_OR_BUSY;
  payload = swap(endpoint(forza_mmio_pop_address(mailbox)), 0);
  control = 0;
#else
  uint64_t pointer = forza_vortex_dequeue(mailbox);
  if (pointer == 0) return EMPTY_OR_BUSY;
  auto words = reinterpret_cast<volatile const uint64_t*>(pointer);
  control = words[0];
  payload = words[1];
  forza_vortex_release(mailbox, control ^ payload);
  if (((control >> FORZA_CONTROL_OPCODE_SHIFT) & 255) != DIRECT_OP) return ERROR;
#endif
  return READY_OR_ACCEPTED;
}

static inline __attribute__((always_inline)) uint64_t outstanding(uint32_t mailbox = 0) {
#ifdef PGAS_TRANSPORT_MMIO
  uint64_t counts = load(endpoint(FORZA_MMIO_TX_COUNTS));
#else
  uint64_t counts = csr_read(FORZA_CSR_ZENCOUNT);
#endif
  return (counts >> (8 * mailbox)) & 255;
}
}
