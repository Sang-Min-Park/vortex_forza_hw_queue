#include "device.h"

__attribute__((noinline)) static void grouped_clients(args_t* __UNIFORM__ a,
                                                     result_t* out) {
  uint32_t lane = vx_thread_id(), client = vx_core_id() * 4 + lane;
  uint32_t key = initial_key(client, a->active_keys), step = 0;
  uint32_t cookie = initial_cookie(client);
  auto state = reinterpret_cast<volatile node_t*>(a->state);
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t digest = 0, retries = 0;
  bool complete = false;
  if (a->kind != KIND_UPDATE) { out->error = 40; return; }
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    uint32_t __UNIFORM__ pending = grouped_ballot(step < a->steps);
    if (pending == 0) { complete = true; break; }
    uint64_t query = query_token(key, client, step, cookie);
    // next never changes: prepare independent work in parallel before locking.
    uint32_t link = uint32_t(state[key].next);
    uint32_t seed = update_seed(client, step, link, a->work, cookie);
    uint64_t queries[4] = {grouped_shuffle(query, 0), grouped_shuffle(query, 1),
                          grouped_shuffle(query, 2), grouped_shuffle(query, 3)};
    uint32_t seeds[4] = {uint32_t(grouped_shuffle(seed, 0)), uint32_t(grouped_shuffle(seed, 1)),
                         uint32_t(grouped_shuffle(seed, 2)), uint32_t(grouped_shuffle(seed, 3))};
    uint64_t reply = 0;
    for (uint32_t leader = 0; leader < 4; ++leader) {
      uint32_t __UNIFORM__ target = query_key(queries[leader]);
      uint32_t __UNIFORM__ group = grouped_ballot(step < a->steps && key == target);
      uint32_t __UNIFORM__ first = (group & (1u << leader))
                                  && !(group & ((1u << leader) - 1));
      if (!first) continue;
      uint64_t lock = 1;
      if (lane == leader) lock = swap64(&state[target].lock, 1);
      uint32_t __UNIFORM__ acquired = grouped_shuffle(lock, leader) == 0;
      if (!acquired) continue;
      // Full-warp identical-address loads keep the following work converged.
      uint32_t current_link = uint32_t(state[target].next);
      uint32_t value = uint32_t(state[target].value);
      uint32_t version = uint32_t(state[target].version);
      for (uint32_t member = 0; member < 4; ++member) {
        if (group & (1u << member)) {
          uint64_t member_query = queries[member];
          uint32_t member_client = query_client(member_query);
          uint32_t member_step = query_step(member_query);
          uint32_t member_cookie = query_cookie(member_query);
          uint32_t next = update_next(value, version, member_client, member_step,
                                      current_link, a->active_keys, member_cookie);
          uint64_t member_reply = reply_token(next, version, value);
          if (lane == member) reply = member_reply;
          uint32_t work = seeds[member];
          value = ((value ^ work ^ version) & 3)
                ? value + work : value ^ (work >> 1);
          ++version;
        }
      }
      if (lane == leader) {
        swap64(&state[target].value, value);
        swap64(&state[target].version, version);
        vx_fence();
        if (swap64(&state[target].lock, 0) != 1) out->error = 41;
      }
    }
    if (reply) {
      uint64_t index = uint64_t(client) * a->steps + step;
      traces[index].key = trace_query(key, cookie);
      traces[index].reply = reply;
      digest += reply;
      key = reply_key(reply);
      cookie = next_cookie(reply);
      ++step;
    } else if (step < a->steps) {
      ++retries;
    }
    uint32_t __UNIFORM__ done = grouped_all(step == a->steps);
    if (done) { complete = true; break; }
  }
  if (!complete) out->error = 42;
  out->processed = step; out->digest = digest; out->retries = retries;
}

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  uint32_t rank = rank_of(a);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  out->rank = rank;
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (vx_warp_id() == 1) grouped_clients(a, out);
  barrier(2);
  barrier(3);
  out->start = start; out->end = vx_rdcycle(); out->complete = FEEDBACK_COMPLETE;
}
