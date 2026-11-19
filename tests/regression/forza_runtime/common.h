#pragma once

#include <stdint.h>

enum {
  FORZA_RUNTIME_EPOCHS = 3,
  FORZA_RUNTIME_MAILBOXES = 2,
  FORZA_RUNTIME_MAX_PES = 128,
  FORZA_RUNTIME_COMPLETE = 0x4652544d,
};

enum forza_runtime_error {
  FORZA_RUNTIME_IDENTITY = 1,
  FORZA_RUNTIME_SEND,
  FORZA_RUNTIME_TIMEOUT,
  FORZA_RUNTIME_PAYLOAD,
  FORZA_RUNTIME_ORDER,
  FORZA_RUNTIME_SOURCE,
  FORZA_RUNTIME_COMPLETION,
};

struct runtime_packet_t {
  uint32_t source;
  uint32_t destination;
  uint32_t sequence;
  uint32_t mailbox;
  uint64_t cookie;
  uint64_t guard;
};

static_assert(sizeof(runtime_packet_t) == 32, "exercise a payload smaller than 56 bytes");

struct runtime_epoch_result_t {
  uint32_t sent;
  uint32_t handled[FORZA_RUNTIME_MAILBOXES];
  uint32_t done_received[FORZA_RUNTIME_MAILBOXES];
  uint32_t retries;
  uint32_t iterations;
  uint32_t responses_at_close;
  uint64_t checksum[FORZA_RUNTIME_MAILBOXES];
  uint64_t outstanding[FORZA_RUNTIME_MAILBOXES];
};

struct runtime_actor_result_t {
  uint32_t core;
  uint32_t warp;
  uint32_t lane;
  uint32_t rank;
  uint32_t phase;
  uint32_t error;
  uint32_t done;
  uint32_t reserved;
  uint64_t expected;
  uint64_t actual;
  runtime_epoch_result_t epoch[FORZA_RUNTIME_EPOCHS];
};

struct runtime_kernel_arg_t {
  uint32_t num_actors;
  uint32_t actors_per_core;
  uint32_t num_cores;
  uint32_t num_threads;
  uint32_t rounds;
  uint32_t iteration_limit;
  uint64_t results_addr;
};

static inline uint64_t runtime_cookie(uint32_t source, uint32_t destination,
                                     uint32_t sequence, uint32_t mailbox,
                                     uint32_t epoch) {
  return UINT64_C(0x9e3779b97f4a7c15)
       ^ (uint64_t(source + 1) * UINT64_C(0x100000001b3))
       ^ (uint64_t(destination) << 41)
       ^ (uint64_t(sequence) << 9)
       ^ (uint64_t(mailbox) << 5)
       ^ uint64_t(epoch);
}

static inline uint64_t runtime_guard(uint64_t cookie) {
  return ~cookie ^ UINT64_C(0x56b0cafe1234abcd);
}
