#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_message_lib.vortex.h>
#include "common.h"

static_assert(sizeof(request_t) == 8, "fast enqueue consumes one payload word");

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  const uint32_t rank = vx_core_id() * arg->per_core
                      + vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t actors = arg->actors;
  const uint32_t owner_shift = __builtin_ctzll(arg->words_per_actor);
  const uint32_t zone_shift = __builtin_ctz(arg->per_core);
  const uint32_t local_mask = arg->per_core - 1;
  auto output = reinterpret_cast<result_t*>(arg->results_addr) + rank;
  auto input = reinterpret_cast<const uint64_t*>(arg->requests_addr);
  auto state = reinterpret_cast<int64_t*>(arg->state_addr);
  const uint64_t expected = output->expected_receive;
  output->rank = rank;
  if (forza_vortex_init() != SUCCESS) output->error = 1;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint32_t index = rank;
  uint64_t sent = 0, received = 0, local = 0, attempts = 0, retries = 0, outstanding = 0;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
    for (uint32_t burst = 0; burst < 1; ++burst) {
      if (index >= arg->updates) break;
      uint64_t word = input[index];
      uint32_t key = uint32_t(word);
      uint32_t owner = key >> owner_shift;
      if (owner == rank) {
        state[key] += int32_t(word >> 32);
        ++local;
      } else {
        ++attempts;
        csr_write(FORZA_CSR_ZENEQD, word);
        uint64_t control = uint64_t(owner >> zone_shift) << FORZA_DEST_ZONE_SHIFT
                         | (owner & local_mask);
        uint64_t result;
        __asm__ volatile ("csrrw %0, %1, %2" : "=r"(result)
                          : "i"(FORZA_CSR_ZENTRY), "r"(control) : "memory");
        if (result == FORZA_TRY_BUSY) {
          ++retries;
          break;
        }
        if (result != FORZA_TRY_ACCEPTED) {
          output->error = 2;
          break;
        }
        ++sent;
      }
      index += actors;
    }
    // This prototype returns zero for an empty dequeue; no separate status poll is needed.
    uint64_t pointer = csr_read(FORZA_CSR_ZQMDQ0);
    if (pointer != 0) {
      auto message = reinterpret_cast<volatile const uint64_t*>(pointer);
      uint64_t control = message[0];
      uint64_t word = message[1];
      __asm__ volatile ("fence r, rw" ::: "memory");
      // CSR service can pass an LSU fence; its source must depend on both loads.
      csr_write(FORZA_CSR_ZQMDQ0, control ^ word);
      uint32_t key = uint32_t(word);
      if (((control >> FORZA_CONTROL_OPCODE_SHIFT) & 255) != DIRECT_OP
          || key >= arg->words || (key >> owner_shift) != rank) {
        output->error = 3;
      } else {
        state[key] += int32_t(word >> 32);
        ++received;
      }
    }
    bool ready = false;
    if (index >= arg->updates && received == expected) {
      outstanding = csr_read(FORZA_CSR_ZENCOUNT) & 255;
      ready = outstanding == 0;
    }
    uint32_t __UNIFORM__ all_ready = vx_vote_all(ready);
    if (all_ready) {
      complete = true;
      break;
    }
  }
  uint64_t tx_status = csr_read(FORZA_CSR_ZENSTAT);
  uint64_t rx_status = csr_read(FORZA_CSR_ZQMSTAT);
  if (!complete || !(tx_status >> 63) || (tx_status & (0x1ffull << 8))
      || (rx_status & (255ull << 8))) output->error = 4;
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  output->start = start;
  output->end = vx_rdcycle();
  output->inputs = sent + local;
  output->flushes = sent + local;
  output->local = local;
  output->sent = sent;
  output->received = received;
  output->attempts = attempts;
  output->retries = retries;
  output->outstanding = outstanding;
  output->complete = LARGE_COMPLETE;
}
