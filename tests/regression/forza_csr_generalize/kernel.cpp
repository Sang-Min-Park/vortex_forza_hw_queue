#include <vx_spawn2.h>
#include <forza_abi.h>
#include "common.h"

#if __riscv_xlen != 64
#error "CSR queue experiment requires RV64"
#endif

static bool fail(csr_generalize_result_t* result, uint32_t error,
                 uint64_t expected, uint64_t actual) {
  result->error = error;
  result->expected = expected;
  result->actual = actual;
  return false;
}

static bool run_actor(csr_generalize_arg_t* arg, csr_generalize_result_t* result) {
  uint64_t registration = (uint64_t(result->pe) << 8) | 1;
  result->registration_before = csr_read(FORZA_CSR_ZQMMBOXREG);
  csr_write(FORZA_CSR_ZQMMBOXREG, registration);
  result->registration_after = csr_read(FORZA_CSR_ZQMMBOXREG);
  result->status = csr_read(FORZA_CSR_ZENSTAT);
  if (result->registration_after != registration
      || (result->status & (UINT64_C(0x1ff) << 8)) != 0) {
    return fail(result, CSR_GENERALIZE_REGISTRATION, registration,
                result->registration_after);
  }
  uint64_t control = forza_message_control(result->core, result->pe, 0, 0);
  for (uint32_t sequence = 0; sequence < arg->rounds; ++sequence) {
    bool accepted = false;
    for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
      for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
        csr_write(FORZA_CSR_ZENEQD,
                  csr_generalize_payload(result->block, result->thread, sequence, word));
      }
      uint64_t attempt = csr_swap(FORZA_CSR_ZENTRY, control);
      if (attempt == FORZA_TRY_ACCEPTED) {
        accepted = true;
        break;
      }
      if (attempt != FORZA_TRY_BUSY) {
        return fail(result, CSR_GENERALIZE_SEND, FORZA_TRY_ACCEPTED, attempt);
      }
      ++result->retries;
    }
    if (!accepted) {
      return fail(result, CSR_GENERALIZE_SEND, 1, 0);
    }
    ++result->sent;
    bool ready = false;
    for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
      if (csr_read(FORZA_CSR_ZQMSTAT) & 1) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      return fail(result, CSR_GENERALIZE_RECEIVE, 1, csr_read(FORZA_CSR_ZQMSTAT));
    }
    uint64_t address = csr_read(FORZA_CSR_ZQMDQ0);
    uint64_t first = FORZA_MAILBOX_BASE + uint64_t(result->actor) * 1024;
    if (address != first && address != first + FORZA_MESSAGE_BYTES) {
      return fail(result, CSR_GENERALIZE_POINTER, first, address);
    }
    auto words = reinterpret_cast<volatile uint64_t*>(address);
    if (words[0] != control) {
      return fail(result, CSR_GENERALIZE_PAYLOAD, control, words[0]);
    }
    for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
      uint64_t expected = csr_generalize_payload(result->block, result->thread, sequence, word);
      uint64_t actual = words[word + 1];
      if (actual != expected) {
        return fail(result, CSR_GENERALIZE_PAYLOAD, expected, actual);
      }
    }
    __asm__ volatile ("fence r, rw" ::: "memory");
    csr_write(FORZA_CSR_ZQMDQ0, 0);
    ++result->received;
    bool drained = false;
    for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
      if (csr_read(FORZA_CSR_ZENCOUNT) == 0) {
        drained = true;
        break;
      }
    }
    if (!drained || csr_read(FORZA_CSR_ZQMSTAT) != 0) {
      return fail(result, CSR_GENERALIZE_DRAIN, 0, csr_read(FORZA_CSR_ZENCOUNT));
    }
  }
  result->status = csr_read(FORZA_CSR_ZENSTAT);
  if ((result->status & (UINT64_C(0x1ff) << 8)) != 0) {
    return fail(result, CSR_GENERALIZE_DRAIN, 0, result->status);
  }
  return true;
}

__kernel void kernel_main(csr_generalize_arg_t* __UNIFORM__ arg) {
  uint32_t block = blockIdx.x;
  uint32_t thread = threadIdx.x;
  auto result = reinterpret_cast<csr_generalize_result_t*>(arg->results_addr)
              + block * arg->actors_per_core + thread;
  result->block = block;
  result->thread = thread;
  result->core = vx_core_id();
  result->warp = vx_warp_id();
  result->lane = vx_thread_id();
  result->actor = result->warp * arg->threads_per_warp + result->lane;
  result->pe = arg->logical_pe ? block * arg->actors_per_core + thread : result->actor;
  bool success = run_actor(arg, result);
  __syncthreads();
  result->done = success ? CSR_GENERALIZE_DONE : 0;
}
