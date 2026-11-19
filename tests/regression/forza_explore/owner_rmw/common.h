#pragma once
#include <stdint.h>
struct request_t { uint64_t key; int64_t delta; };
struct alignas(64) result_t {
  uint64_t rank, complete, updates, mismatches;
  uint64_t old_checksum, device_checksum, start, end;
};
struct args_t {
  uint64_t requests, offsets, state, results, words, words_per_actor;
  uint32_t actors, per_core, threads, verify_device;
};
enum { OWNER_COMPLETE = 0x4f574e52 };
