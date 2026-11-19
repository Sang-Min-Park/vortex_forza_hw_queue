#include "grouped_device.h"

__attribute__((noinline)) static void native_walk_clients(args_t* __UNIFORM__ a,
                                                         result_t* out) {
  uint32_t core = vx_core_id(), worker = physical_local() - GROUPED_WORKERS;
  uint32_t keys[GROUPED_CHAINS], steps[GROUPED_CHAINS], cookies[GROUPED_CHAINS];
  for (uint32_t slot = 0; slot < GROUPED_CHAINS; ++slot) {
    uint32_t client = logical_client(a, core, worker, slot);
    keys[slot] = initial_key(client, a->active_keys);
    cookies[slot] = initial_cookie(client); steps[slot] = 0;
  }
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t digest = 0, processed = 0;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    uint32_t slot = iteration % GROUPED_CHAINS;
    uint32_t client = logical_client(a, core, worker, slot);
    if (steps[slot] < a->steps) {
      uint64_t reply = handle_query<0>(a, keys[slot], client, steps[slot], cookies[slot]);
      uint64_t index = uint64_t(client) * a->steps + steps[slot];
      traces[index].key = trace_query(keys[slot], cookies[slot]);
      traces[index].reply = reply;
      digest += reply; ++processed; ++steps[slot];
      keys[slot] = reply_key(reply); cookies[slot] = next_cookie(reply);
    }
    bool local_done = true;
    for (uint32_t slot = 0; slot < GROUPED_CHAINS; ++slot)
      local_done &= steps[slot] == a->steps;
    uint32_t __UNIFORM__ done = scalar_group_all(local_done);
    if (done) { complete = true; break; }
  }
  if (!complete) out->error = 50;
  out->processed = processed; out->digest = digest;
}

__attribute__((noinline)) static void native_grouped_clients(args_t* __UNIFORM__ a,
                                                            result_t* out) {
  uint32_t lane = vx_thread_id(), core = vx_core_id();
  uint32_t worker = physical_local() - GROUPED_WORKERS;
  uint32_t keys[GROUPED_CHAINS], steps[GROUPED_CHAINS], cookies[GROUPED_CHAINS];
  uint32_t seeds[GROUPED_CHAINS]{};
  bool prepared[GROUPED_CHAINS]{};
  for (uint32_t slot = 0; slot < GROUPED_CHAINS; ++slot) {
    uint32_t client = logical_client(a, core, worker, slot);
    keys[slot] = initial_key(client, a->active_keys);
    cookies[slot] = initial_cookie(client); steps[slot] = 0;
  }
  auto state = reinterpret_cast<volatile node_t*>(a->state);
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t digest = 0, retries = 0, processed = 0;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    uint32_t slot = iteration % GROUPED_CHAINS;
    uint32_t key = keys[slot], step = steps[slot], cookie = cookies[slot];
    uint32_t client = logical_client(a, core, worker, slot);
    uint32_t __UNIFORM__ pending = scalar_group_ballot(step < a->steps);
    if (pending) {
      uint64_t query = query_token(key, client, step, cookie);
      // next is immutable; this seed stays valid until this logical client advances.
      seeds[slot] = scalar_group_seed(client, step, uint32_t(state[key].next),
                                     a->work, cookie, prepared[slot], seeds[slot]);
      prepared[slot] = true;
      uint64_t queries[VX_CFG_NUM_THREADS];
      uint32_t work_seeds[VX_CFG_NUM_THREADS];
      for (uint32_t member = 0; member < VX_CFG_NUM_THREADS; ++member) {
        queries[member] = scalar_group_shuffle(query, member);
        work_seeds[member] = uint32_t(scalar_group_shuffle(seeds[slot], member));
      }
      uint64_t reply = 0;
      for (uint32_t leader = 0; leader < VX_CFG_NUM_THREADS; ++leader) {
        uint32_t __UNIFORM__ target = query_key(queries[leader]);
        uint32_t __UNIFORM__ group = scalar_group_ballot(step < a->steps && key == target);
        uint32_t __UNIFORM__ first = (group & (1u << leader))
                                    && !(group & ((1u << leader) - 1));
        if (!first) continue;
        uint64_t lock = 1;
        if (lane == leader) lock = swap64(&state[target].lock, 1);
        uint32_t __UNIFORM__ acquired = scalar_group_shuffle(lock, leader) == 0;
        if (!acquired) continue;
        // Identical-address reads by every lane keep subsequent work converged.
        uint32_t link = uint32_t(state[target].next);
        uint32_t value = uint32_t(state[target].value);
        uint32_t version = uint32_t(state[target].version);
        for (uint32_t member = 0; member < VX_CFG_NUM_THREADS; ++member) {
          if (group & (1u << member)) {
            uint64_t member_query = queries[member];
            uint32_t member_client = query_client(member_query);
            uint32_t next = update_next(value, version, member_client,
                                        query_step(member_query), link,
                                        a->active_keys, query_cookie(member_query));
            uint64_t member_reply = reply_token(next, version, value, member_client);
            if (lane == member) reply = member_reply;
            uint32_t work = work_seeds[member];
            value = ((value ^ work ^ version) & 3)
                  ? value + work : value ^ (work >> 1);
            ++version;
          }
        }
        if (lane == leader) {
          swap64(&state[target].value, value);
          swap64(&state[target].version, version);
          vx_fence();
          if (swap64(&state[target].lock, 0) != 1) out->error = 51;
        }
      }
      if (reply) {
        uint64_t index = uint64_t(client) * a->steps + step;
        traces[index].key = trace_query(key, cookie);
        traces[index].reply = reply;
        digest += reply; ++processed;
        keys[slot] = reply_key(reply); cookies[slot] = next_cookie(reply);
        steps[slot] = step + 1; prepared[slot] = false;
      } else if (step < a->steps) {
        ++retries;
      }
    }
    bool local_done = true;
    for (uint32_t slot = 0; slot < GROUPED_CHAINS; ++slot)
      local_done &= steps[slot] == a->steps;
    uint32_t __UNIFORM__ done = scalar_group_all(local_done);
    if (done) { complete = true; break; }
  }
  if (!complete) out->error = 52;
  out->processed = processed; out->digest = digest; out->retries = retries;
}

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  uint32_t rank = rank_of(a);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  out->rank = rank;
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (physical_local() >= GROUPED_WORKERS) {
    if (a->kind == KIND_WALK) native_walk_clients(a, out);
    else native_grouped_clients(a, out);
  }
  barrier(2); barrier(3);
  out->start = start; out->end = vx_rdcycle(); out->complete = FEEDBACK_COMPLETE;
}
