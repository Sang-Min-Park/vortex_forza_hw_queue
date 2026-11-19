#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_abi.h>
#include "common.h"
#ifdef FORZA_TEST_DIAGNOSTICS
#include <vx_print.h>
#endif

#if __riscv_xlen != 64
#error "forza_queue requires an RV64 build"
#endif

static bool fail(actor_result_t* result, uint32_t code, uint64_t expected, uint64_t actual) {
  if (result->error == 0) {
    result->error = code;
    result->expected = expected;
    result->actual = actual;
#ifdef FORZA_TEST_DIAGNOSTICS
    vx_printf("FORZA failure core=%u warp=%u lane=%u phase=%u error=%u expected=%lx actual=%lx\n",
              result->core, result->warp, result->lane, result->phase, code, expected, actual);
#endif
  }
  return false;
}

static uint64_t count(uint32_t mailbox) {
  return (csr_read(FORZA_CSR_ZENCOUNT) >> (mailbox * 8)) & 0xff;
}

static bool wait_send(kernel_arg_t* arg, actor_result_t* result, uint32_t mailbox) {
  uint64_t status = 0;
  for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
    status = csr_read(FORZA_CSR_ZENSTAT);
    if ((status & (UINT64_C(1) << 63)) == 0 || (status & (UINT64_C(0x1ff) << 8))) {
      return fail(result, FORZA_TEST_REGISTRATION, UINT64_C(1) << 63, status);
    }
    if ((status & (UINT64_C(1) << mailbox)) == 0) {
      return true;
    }
  }
  return fail(result, FORZA_TEST_SEND_TIMEOUT, 0, status);
}

static bool wait_count(kernel_arg_t* arg, actor_result_t* result, uint32_t mailbox) {
  uint64_t outstanding = 0;
  for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
    outstanding = count(mailbox);
    if (outstanding == 0) {
      return true;
    }
  }
  return fail(result, FORZA_TEST_ACK_TIMEOUT, 0, outstanding);
}

static uint64_t control(uint32_t core, uint32_t local_pe, uint32_t mailbox) {
  return forza_message_control(core, local_pe, mailbox, 0);
}

static bool send(kernel_arg_t* arg, actor_result_t* result, uint32_t source,
                 uint32_t target_core, uint32_t target_pe, uint32_t mailbox,
                 uint32_t sequence) {
  if (!wait_send(arg, result, mailbox)) {
    return false;
  }
  for (uint32_t word = 0; word < 7; ++word) {
    csr_write(FORZA_CSR_ZENEQD, payload_word(source, sequence, word));
  }
  csr_write(FORZA_CSR_ZENEQC, control(target_core, target_pe, mailbox));
  ++result->sent;
  return true;
}

static uint64_t dequeue(uint32_t mailbox) {
  return mailbox == 0 ? csr_read(FORZA_CSR_ZQMDQ0) : csr_read(FORZA_CSR_ZQMDQ1);
}

static void release(uint32_t mailbox) {
  __asm__ volatile ("fence r, rw" ::: "memory");
  if (mailbox == 0) {
    csr_write(FORZA_CSR_ZQMDQ0, 0);
  } else {
    csr_write(FORZA_CSR_ZQMDQ1, 0);
  }
}

static bool wait_receive(kernel_arg_t* arg, actor_result_t* result, uint32_t mailbox) {
  uint64_t status = 0;
  for (uint32_t poll = 0; poll < arg->poll_limit; ++poll) {
    status = csr_read(FORZA_CSR_ZQMSTAT);
    if (status & (UINT64_C(0xff) << 8)) {
      return fail(result, FORZA_TEST_REGISTRATION, 0, status);
    }
    if (status & (UINT64_C(1) << mailbox)) {
      return true;
    }
  }
  return fail(result, FORZA_TEST_RECEIVE_TIMEOUT, UINT64_C(1) << mailbox, status);
}

static bool check_pointer(actor_result_t* result, uint64_t address, uint32_t mailbox) {
  uint64_t first = FORZA_MAILBOX_BASE
                 + result->local_pe * FORZA_NUM_MAILBOXES * FORZA_MAILBOX_SLOTS * FORZA_MESSAGE_BYTES
                 + mailbox * FORZA_MAILBOX_SLOTS * FORZA_MESSAGE_BYTES;
  if (address != first && address != first + FORZA_MESSAGE_BYTES) {
    return fail(result, FORZA_TEST_POINTER, first, address);
  }
  return true;
}

static bool check_message(actor_result_t* result, uint64_t address, uint32_t source,
                          uint32_t sequence, uint32_t mailbox) {
  if (!check_pointer(result, address, mailbox)) {
    return false;
  }
  auto words = reinterpret_cast<volatile uint64_t*>(address);
  uint64_t mask = (UINT64_C(0xf) << FORZA_DEST_ZONE_SHIFT)
                | (UINT64_C(7) << FORZA_CONTROL_MBOX_SHIFT)
                | UINT64_C(0x7ff) | (UINT64_C(0xff) << FORZA_CONTROL_OPCODE_SHIFT);
  uint64_t expected = control(result->core, result->logical_pe, mailbox);
  uint64_t actual = words[0] & mask;
  if (actual != expected) {
    return fail(result, FORZA_TEST_CONTROL, expected, actual);
  }
  for (uint32_t word = 0; word < 7; ++word) {
    expected = payload_word(source, sequence, word);
    actual = words[word + 1];
    if (actual != expected) {
      return fail(result, FORZA_TEST_PAYLOAD, expected, actual);
    }
  }
  return true;
}

static void barrier() {
  vx_fence();
  vortex::gbarrier global(1);
  global.arrive_and_wait();
}

static bool private_mailboxes(kernel_arg_t* arg, actor_result_t* result, uint32_t actor, bool local) {
  uint32_t peer_core = local ? result->core : result->core ^ 1;
  uint32_t source = peer_core * arg->actors_per_core + result->local_pe;
  auto scratch = reinterpret_cast<volatile uint64_t*>(arg->scratch_addr);
#ifdef FORZA_TEST_DIAGNOSTICS
  if (result->local_pe == 0) {
    vx_printf("FORZA private entry core=%u local=%u\n", result->core, local);
  }
#endif
  for (uint32_t round = 0; round < arg->rounds; ++round) {
    uint32_t mailbox = round & 1;
    uint32_t first_sequence = round * 2 + (local ? 0x8000 : 0);
    result->phase = (local ? 500 : 100) + round;
    if (!send(arg, result, actor, peer_core, result->logical_pe, mailbox, first_sequence)
     || !send(arg, result, actor, peer_core, result->logical_pe, mailbox, first_sequence + 1)) {
      return false;
    }
#ifdef FORZA_TEST_DIAGNOSTICS
    if (result->local_pe == 0 && round == 0) {
      vx_printf("FORZA two sends complete core=%u\n", result->core);
    }
#endif
    if (!wait_count(arg, result, mailbox)) {
      return false;
    }
#ifdef FORZA_TEST_DIAGNOSTICS
    if (result->local_pe == 0 && round == 0) {
      vx_printf("FORZA two ACKs complete core=%u\n", result->core);
    }
#endif
    uint64_t first_address = 0;
    for (uint32_t slot = 0; slot < 2; ++slot) {
      if (!wait_receive(arg, result, mailbox)) {
        return false;
      }
      uint64_t address = dequeue(mailbox);
      if (dequeue(mailbox) != address) {
        return fail(result, FORZA_TEST_POINTER, address, dequeue(mailbox));
      }
      if (slot == 0) {
        first_address = address;
      } else if (address == first_address) {
        return fail(result, FORZA_TEST_RELEASE, first_address + 64, address);
      }
      uint32_t sequence = first_sequence + slot;
      if (!check_message(result, address, source, sequence, mailbox)) {
        return false;
      }
      uint64_t expected = payload_word(source, sequence, 6);
      scratch[actor] = expected;
      vx_fence();
      uint64_t lane_mask = UINT64_C(0) - (result->lane & 1);
      uint64_t selected = ((address + 56) & lane_mask)
                        | (reinterpret_cast<uint64_t>(&scratch[actor]) & ~lane_mask);
      uint64_t actual;
      // One instruction spans the aperture and normal memory in the same warp.
      __asm__ volatile ("ld %0, 0(%1)" : "=r"(actual) : "r"(selected) : "memory");
      if (actual != expected) {
        return fail(result, FORZA_TEST_MIXED_LOAD, expected, actual);
      }
      ++result->mixed_loads;
      release(mailbox);
      ++result->received;
    }
    uint64_t status = csr_read(FORZA_CSR_ZQMSTAT);
    if (status & (UINT64_C(1) << mailbox)) {
      return fail(result, FORZA_TEST_RELEASE, 0, status);
    }
    barrier();
  }
  return true;
}

static bool shared_mailbox(kernel_arg_t* arg, actor_result_t* result, uint32_t actor) {
  uint32_t peer_core = result->core ^ 1;
  uint32_t messages = arg->rounds * 4;
  uint32_t __UNIFORM__ warp = vx_warp_id();
  result->phase = 1000;
  // Separate warps let the consumer drain a full mailbox while producers run.
  if (warp == 0) {
    for (uint32_t sequence = 0; sequence < messages; ++sequence) {
      uint32_t target_pe = arg->actors_per_core - 1 - arg->num_threads;
      if (!send(arg, result, actor, peer_core, target_pe, 0, 0x10000 + sequence)) {
        return false;
      }
    }
    if (!wait_count(arg, result, 0)) {
      return false;
    }
  } else if (warp == 1) {
    vx_tmc_one();
    uint32_t next[FORZA_TEST_MAX_THREADS] = {};
    for (uint32_t message = 0; message < messages * arg->num_threads; ++message) {
      if (!wait_receive(arg, result, 0)) {
        return false;
      }
      uint64_t address = dequeue(0);
      if (!check_pointer(result, address, 0)) {
        return false;
      }
      auto words = reinterpret_cast<volatile uint64_t*>(address);
      uint32_t source = static_cast<uint32_t>((words[1] ^ payload_word(0, 0, 0)) >> 32);
      uint32_t source_base = peer_core * arg->actors_per_core;
      if (source < source_base || source >= source_base + arg->num_threads) {
        return fail(result, FORZA_TEST_ORDER, source_base, source);
      }
      uint32_t sender = source - source_base;
      uint32_t sequence = 0x10000 + next[sender];
      if (!check_message(result, address, source, sequence, 0)) {
        return false;
      }
      ++next[sender];
      release(0);
      ++result->received;
    }
    for (uint32_t sender = 0; sender < arg->num_threads; ++sender) {
      if (next[sender] != messages) {
        return fail(result, FORZA_TEST_ORDER, messages, next[sender]);
      }
    }
    vx_tmc(-1);
  }
  barrier();
  return true;
}

static bool negative_abi(kernel_arg_t* arg, actor_result_t* result) {
  uint32_t scenario = result->local_pe % 4;
  uint64_t expected_status;
  result->phase = 3000 + scenario;
  if (scenario == 0) {
    for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
      csr_write(FORZA_CSR_ZENEQD, payload_word(result->local_pe, 0, word));
    }
    csr_write(FORZA_CSR_ZENEQC, control(arg->num_cores, result->logical_pe, 0));
    expected_status = UINT64_C(1) << 8;
  } else if (scenario == 1) {
    csr_write(FORZA_CSR_ZQMMBOXREG, (static_cast<uint64_t>(arg->actors_per_core - 1) << 8) | 3);
    expected_status = UINT64_C(1) << 16;
  } else if (scenario == 2) {
    release(0);
    expected_status = UINT64_C(1) << 16;
  } else {
    for (uint32_t word = 0; word <= FORZA_PAYLOAD_WORDS; ++word) {
      csr_write(FORZA_CSR_ZENEQD, payload_word(result->local_pe, 0, word));
    }
    csr_write(FORZA_CSR_ZENEQC, control(result->core, result->logical_pe, 0));
    expected_status = (UINT64_C(1) << 16) | (UINT64_C(1) << 8);
  }
  uint64_t actual_status = csr_read(FORZA_CSR_ZENSTAT) & (UINT64_C(0x1ff) << 8);
  if (actual_status != expected_status) {
    return fail(result, FORZA_TEST_NEGATIVE_ABI, expected_status, actual_status);
  }
  uint64_t outstanding = csr_read(FORZA_CSR_ZENCOUNT);
  if (outstanding != 0) {
    return fail(result, FORZA_TEST_NEGATIVE_ABI, 0, outstanding);
  }
  uint64_t registration = (static_cast<uint64_t>(result->logical_pe) << 8) | 3;
  uint64_t actual_registration = csr_read(FORZA_CSR_ZQMMBOXREG);
  if (actual_registration != registration) {
    return fail(result, FORZA_TEST_NEGATIVE_ABI, registration, actual_registration);
  }
  uint64_t status = csr_read(FORZA_CSR_ZQMSTAT);
  if (status != 0) {
    return fail(result, FORZA_TEST_NEGATIVE_ABI, 0, status);
  }
  result->final_count0 = count(0);
  result->final_count1 = count(1);
  result->done = FORZA_TEST_DONE;
  return true;
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t core = vx_core_id();
  uint32_t warp = vx_warp_id();
  uint32_t lane = vx_thread_id();
  uint32_t local_pe = warp * arg->num_threads + lane;
  uint32_t actor = core * arg->actors_per_core + local_pe;
  auto result = reinterpret_cast<actor_result_t*>(arg->results_addr) + actor;
  result->core = core;
  result->warp = warp;
  result->lane = lane;
  result->local_pe = local_pe;
  result->logical_pe = arg->actors_per_core - 1 - local_pe;
  result->phase = 1;
  uint64_t registration = (static_cast<uint64_t>(result->logical_pe) << 8) | 3;
  csr_write(FORZA_CSR_ZQMMBOXREG, registration);
  uint64_t actual_registration = csr_read(FORZA_CSR_ZQMMBOXREG);
  if (actual_registration != registration) {
    fail(result, FORZA_TEST_REGISTRATION, registration, actual_registration);
    return;
  }
  __syncthreads();
  barrier();
  if (arg->error_mode) {
    negative_abi(arg, result);
    return;
  }
  if (!private_mailboxes(arg, result, actor, false)) {
    return;
  }
#ifdef FORZA_TEST_DIAGNOSTICS
  if (local_pe == 0) {
    vx_printf("FORZA cross-core complete core=%u\n", core);
  }
#endif
  if (!private_mailboxes(arg, result, actor, true)) {
    return;
  }
#ifdef FORZA_TEST_DIAGNOSTICS
  if (local_pe == 0) {
    vx_printf("FORZA local complete core=%u\n", core);
  }
#endif
  if (!shared_mailbox(arg, result, actor)) {
    return;
  }
#ifdef FORZA_TEST_DIAGNOSTICS
  if (local_pe == 0) {
    vx_printf("FORZA shared complete core=%u\n", core);
  }
#endif
  result->phase = 2000;
  if (!wait_count(arg, result, 0) || !wait_count(arg, result, 1)
   || !wait_send(arg, result, 0) || !wait_send(arg, result, 1)) {
    return;
  }
  result->final_count0 = count(0);
  result->final_count1 = count(1);
  result->done = FORZA_TEST_DONE;
}
