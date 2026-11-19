#pragma once
#include <stdint.h>

struct node_t { uint64_t next, value, version, lock; };
struct trace_t { uint64_t key, reply; };
struct result_t {
  uint64_t rank, processed, sent, received, retries, error, outstanding;
  uint64_t digest, complete, start, end;
};
struct args_t {
  uint64_t state, traces, results, queue_slots, queue_ready;
  uint32_t keys, active_keys, steps, work, kind, cores, per_core, threads, core_shift, limit;
};
enum { FEEDBACK_COMPLETE = 0x4644424b, KIND_WALK = 0, KIND_UPDATE = 1 };
static constexpr uint64_t TOKEN_DONE = UINT64_C(1) << 62;
static constexpr uint64_t TOKEN_STOP = UINT64_C(1) << 63;
static inline uint32_t mix32(uint32_t x) {
  x ^= x >> 16; x *= UINT32_C(0x7feb352d);
  x ^= x >> 15; x *= UINT32_C(0x846ca68b);
  return x ^ (x >> 16);
}
static inline uint32_t initial_key(uint32_t client, uint32_t keys) {
  return mix32(client + 17) & (keys - 1);
}
static inline uint32_t initial_cookie(uint32_t client) { return mix32(client + 5) & 65535; }
static inline uint64_t query_token(uint32_t key, uint32_t client, uint32_t step,
                                   uint32_t cookie) {
  return 1 | (uint64_t(key) << 1) | (uint64_t(client) << 16)
           | (uint64_t(step) << 24) | (uint64_t(cookie) << 40);
}
static inline uint32_t query_key(uint64_t token) { return (token >> 1) & 32767; }
static inline uint32_t query_client(uint64_t token) { return (token >> 16) & 255; }
static inline uint32_t query_step(uint64_t token) { return (token >> 24) & 65535; }
static inline uint32_t query_cookie(uint64_t token) { return (token >> 40) & 65535; }
static inline uint64_t reply_token(uint32_t next, uint32_t version, uint32_t value,
                                   uint32_t client) {
  return 1 | (uint64_t(next) << 1) | (uint64_t(client) << 7) | (uint64_t(version) << 16)
           | (uint64_t(value) << 32);
}
static inline uint32_t reply_key(uint64_t token) { return (token >> 1) & 63; }
static inline uint32_t reply_client(uint64_t token) { return (token >> 7) & 7; }
static inline uint32_t reply_version(uint64_t token) { return (token >> 16) & 65535; }
static inline uint32_t reply_value(uint64_t token) { return token >> 32; }
static inline uint32_t next_cookie(uint64_t token) {
  return (reply_value(token) ^ reply_version(token)) & 65535;
}
static inline uint64_t trace_query(uint32_t key, uint32_t cookie) {
  return uint64_t(key) | (uint64_t(cookie) << 32);
}
static inline uint32_t update_seed(uint32_t client, uint32_t step,
                                   uint32_t link, uint32_t work, uint32_t cookie) {
  uint32_t value = mix32(client * 65537 + step + link + cookie * 257);
  for (uint32_t i = 0; i < work; ++i) value = mix32(value + i + link);
  return value;
}
static inline uint32_t updated_value(uint32_t old_value, uint32_t version,
                                     uint32_t client, uint32_t step,
                                     uint32_t link, uint32_t work, uint32_t cookie) {
  uint32_t value = update_seed(client, step, link, work, cookie);
  return ((old_value ^ value ^ version) & 3)
       ? old_value + value : old_value ^ (value >> 1);
}
static inline uint32_t update_next(uint32_t old_value, uint32_t version,
                                   uint32_t client, uint32_t step,
                                   uint32_t link, uint32_t keys, uint32_t cookie) {
  return mix32(old_value ^ (version * 257) ^ (client * 65537) ^ step ^ link ^ cookie)
       & (keys - 1);
}
static_assert(sizeof(node_t) == 32, "state ABI");
static_assert(sizeof(trace_t) == 16, "trace ABI");

static constexpr uint32_t LOGICAL_CLIENTS_PER_CORE = 4;
static inline uint32_t role_workers(const args_t* a) { return a->per_core / 2; }
static inline uint32_t chains_per_worker(const args_t* a) { return 4 / role_workers(a); }
static inline uint32_t logical_client(const args_t* a, uint32_t core,
                                      uint32_t worker, uint32_t chain) {
  return core * 4 + worker + chain * role_workers(a);
}
