#pragma once
#include <stdint.h>

struct request_t { uint32_t key; int32_t delta; };
static_assert(sizeof(request_t)==8,"one typed message word");
struct alignas(64) queue_slot_t { uint64_t sequence, payload; uint64_t padding[6]; };
static_assert(sizeof(queue_slot_t)==64,"publication and data share a cache sector");
struct result_t {
  uint64_t expected_receive, rank, inputs, sent, received, direct;
  uint64_t attempts, retries, polls, outstanding, local_amo, global_amo;
  uint64_t start, initialized, processed, end, error, complete;
};
struct args_t {
  uint64_t requests_addr, state_addr, results_addr, queues_addr;
  uint32_t words, words_per_actor, local_words_per_actor, local_bytes;
  uint32_t updates, actors, per_core, threads, queue_capacity, iteration_limit;
};
enum { PGAS_MAX_ACTORS=16, PGAS_COMPLETE=0x50474153 };

inline uint64_t pgas_queue_slot_index(uint32_t source,uint32_t destination,
                                      uint64_t position,uint32_t actors,uint32_t capacity) {
  uint32_t mixed=source^destination;
  uint32_t rotated=actors>1?((mixed<<1)&(actors-1))|(mixed>>(__builtin_ctz(actors)-1)):0;
  return (position&(capacity-1))*uint64_t(actors)*actors+uint64_t(source)*actors+rotated;
}
