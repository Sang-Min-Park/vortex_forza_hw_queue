#include "device.h"

#if MESSAGE_TRANSPORT == 0
__attribute__((noinline)) static void native_clients(args_t* __UNIFORM__ a,
                                                    result_t* out) {
  const uint32_t worker = physical_local() - role_workers(a);
  const uint32_t chains = chains_per_worker(a), core = vx_core_id();
  uint32_t key[4], step[4] = {}, cookie[4];
  for (uint32_t chain = 0; chain < chains; ++chain) {
    const uint32_t client = logical_client(a, core, worker, chain);
    key[chain] = initial_key(client, a->active_keys);
    cookie[chain] = initial_cookie(client);
  }
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t digest = 0, retries = 0, processed = 0;
  bool complete = false;
  uint32_t cursor = 0;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    const uint32_t chain = cursor;
    cursor = (cursor + 1) % chains;
    if (step[chain] < a->steps) {
      const uint32_t client = logical_client(a, core, worker, chain);
      uint64_t reply = 0;
      if (a->kind == KIND_UPDATE) {
        auto row = reinterpret_cast<volatile node_t*>(a->state) + key[chain];
        const uint64_t packed = load_reserved(&row->value);
        const uint32_t old_value = uint32_t(packed), version = packed >> 32;
        const uint32_t link = row->next;
        const uint32_t value = updated_value(old_value, version, client, step[chain],
                                             link, a->work, cookie[chain]);
        if (store_conditional(&row->value,
            uint64_t(value) | (uint64_t(version + 1) << 32)) == 0) {
          reply = reply_token(update_next(old_value, version, client, step[chain],
              link, a->active_keys, cookie[chain]), version, old_value, client);
        } else {
          ++retries;
        }
      } else {
        reply = handle_query<0>(a, key[chain], client, step[chain], cookie[chain]);
      }
      if (reply) {
        const uint64_t index = uint64_t(client) * a->steps + step[chain];
        traces[index].key = trace_query(key[chain], cookie[chain]);
        traces[index].reply = reply;
        digest += reply;
        key[chain] = reply_key(reply); cookie[chain] = next_cookie(reply);
        ++step[chain]; ++processed;
      }
    }
    bool done = true;
    for (uint32_t chain = 0; chain < chains; ++chain) done &= step[chain] == a->steps;
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 11;
  out->processed = processed; out->digest = digest; out->retries = retries;
}
#else
__attribute__((noinline)) static void message_clients(args_t* __UNIFORM__ a,
                                                     result_t* out) {
  const uint32_t workers = role_workers(a), worker = physical_local() - workers;
  const uint32_t chains = chains_per_worker(a), core = vx_core_id();
  uint32_t key[4], step[4] = {}, cookie[4];
  bool waiting[4] = {}, done_sent[4] = {};
  for (uint32_t chain = 0; chain < chains; ++chain) {
    const uint32_t client = logical_client(a, core, worker, chain);
    key[chain] = initial_key(client, a->active_keys);
    cookie[chain] = initial_cookie(client);
  }
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t sent = 0, received = 0, retries = 0, digest = 0, credit = 0, processed = 0;
  bool complete = false;
  uint32_t cursor = 0;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    const uint32_t chain = cursor;
    cursor = (cursor + 1) % chains;
    const uint32_t client = logical_client(a, core, worker, chain);
    if (!waiting[chain] && step[chain] < a->steps) {
      const uint32_t target_core = key[chain] & (a->cores - 1);
      const uint32_t pe = (key[chain] >> a->core_shift) & (workers - 1);
      uint64_t status = send_token(a, target_core, pe, 0,
          query_token(key[chain], client, step[chain], cookie[chain]));
      if (status == 0) { waiting[chain] = true; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 20;
    }
    if (step[chain] == a->steps && !done_sent[chain]) {
      uint64_t status = send_token(a, 0, 0, 0,
          TOKEN_DONE | 1 | (uint64_t(client) << 16));
      if (status == 0) { done_sent[chain] = true; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 22;
    }
    bool any_waiting = false, all_waiting = true;
    for (uint32_t slot = 0; slot < chains; ++slot) {
      any_waiting |= waiting[slot];
      all_waiting &= waiting[slot] || step[slot] == a->steps;
    }
    if (any_waiting) {
      // Populate independent virtual chains before parking this physical warp.
      uint64_t reply = pop_token(a, 1, all_waiting);
      if (reply) {
        const uint32_t source = reply_client(reply), local_client = source % 4;
        const uint32_t slot = local_client / workers;
        if (!(reply & 1) || reply_key(reply) >= a->active_keys
            || source / 4 != core || local_client % workers != worker
            || slot >= chains || !waiting[slot]) {
          out->error = 21;
        } else {
          const uint64_t index = uint64_t(source) * a->steps + step[slot];
          traces[index].key = trace_query(key[slot], cookie[slot]);
          traces[index].reply = reply;
          digest += reply;
          key[slot] = reply_key(reply); cookie[slot] = next_cookie(reply);
          ++step[slot]; ++received; ++processed; waiting[slot] = false;
        }
      }
    }
    bool done = true;
    for (uint32_t slot = 0; slot < chains; ++slot) done &= done_sent[slot];
    if (done) { credit = credits(); done = credit == 0; }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 23;
  out->processed = processed; out->sent = sent; out->received = received;
  out->retries = retries; out->outstanding = credit; out->digest = digest;
}

__attribute__((noinline)) static void message_servers(args_t* __UNIFORM__ a,
                                                     result_t* out) {
  const uint32_t core = vx_core_id(), worker = physical_local();
  const uint32_t workers = role_workers(a), clients = 8, servers = a->cores * workers;
  const bool coordinator = core == 0 && worker == 0;
  uint32_t done_mask = 0, stop_cursor = 0, pending_client = 0;
  uint64_t pending_reply = 0, handled = 0, sent = 0, received = 0, retries = 0, credit = 0;
  bool stopped = false, complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    if (pending_reply) {
      uint64_t status = send_token(a, pending_client / 4,
          workers + (pending_client % 4) % workers, 1, pending_reply);
      if (status == 0) { pending_reply = 0; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 30;
    }
    bool broadcasting = coordinator && stop_cursor && stop_cursor <= servers;
    if (broadcasting) {
      uint32_t endpoint_id = stop_cursor & (servers - 1);
      uint64_t status = send_token(a, endpoint_id / workers, endpoint_id % workers,
                                   0, TOKEN_STOP | 1);
      if (status == 0) { ++stop_cursor; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 31;
    }
    uint32_t __UNIFORM__ any_broadcast = vx_vote_any(broadcasting);
    if (!stopped && !pending_reply) {
      uint64_t token = pop_token(a, 0, !any_broadcast);
      if (token) {
        ++received;
        if (token & TOKEN_STOP) {
          stopped = true;
        } else if (token & TOKEN_DONE) {
          uint32_t client = query_client(token);
          if (!coordinator || client >= clients || (done_mask & (1u << client))) {
            out->error = 32;
          } else {
            done_mask |= 1u << client;
            if (done_mask == (1u << clients) - 1) stop_cursor = 1;
          }
        } else {
          uint32_t key = query_key(token), client = query_client(token), step = query_step(token);
          if (!(token & 1) || key >= a->keys || client >= clients || step >= a->steps
              || (key & (a->cores - 1)) != core
              || ((key >> a->core_shift) & (workers - 1)) != worker) {
            out->error = 33;
          } else {
            pending_reply = handle_query<STATE_LOCAL>(a, key, client, step, query_cookie(token));
            pending_client = client; ++handled;
#if MESSAGE_TRANSPORT == 3
            uint64_t status = send_token(a, client / 4,
                workers + (client % 4) % workers, 1, pending_reply);
            if (status == 0) { pending_reply = 0; ++sent; }
            else out->error = 34;
#endif
          }
        }
      }
    }
    bool done = false;
    if (stopped && !pending_reply) { credit = credits(); done = credit == 0; }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 35;
  if (coordinator && (done_mask != (1u << clients) - 1 || stop_cursor != servers + 1))
    out->error = 36;
  out->processed = handled; out->sent = sent; out->received = received;
  out->retries = retries; out->outstanding = credit;
}
#endif

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  const uint32_t rank = rank_of(a), local = physical_local(), core = vx_core_id();
  (void)local; (void)core;
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  out->rank = rank;
#if MESSAGE_TRANSPORT != 0
  if (!register_endpoint()) out->error = 1;
#else
  // Native update stores the entire mutable tuple in one ordinary 64-bit LR/SC word.
  if (a->kind == KIND_UPDATE) {
    auto state = reinterpret_cast<volatile node_t*>(a->state);
    for (uint32_t key = rank; key < a->keys; key += a->cores * a->per_core)
      swap64(&state[key].value, uint32_t(state[key].value) | (state[key].version << 32));
  }
#endif
#if STATE_LOCAL == 1
  auto state = reinterpret_cast<const node_t*>(a->state);
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  for (uint32_t offset = local; offset < (a->keys >> a->core_shift); offset += a->per_core) {
    uint32_t key = (offset << a->core_shift) | core;
    swap64(scratch + uint64_t(offset) * 3, state[key].next);
    swap64(scratch + uint64_t(offset) * 3 + 1, state[key].value);
    swap64(scratch + uint64_t(offset) * 3 + 2, state[key].version);
  }
#endif
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (vx_warp_id() >= VX_CFG_NUM_WARPS / 2) {
#if MESSAGE_TRANSPORT == 0
    native_clients(a, out);
#else
    message_clients(a, out);
#endif
  } else {
#if MESSAGE_TRANSPORT != 0
    message_servers(a, out);
#endif
  }
#if MESSAGE_TRANSPORT != 0
  if (!healthy()) out->error = 2;
#endif
  barrier(2);
#if STATE_LOCAL == 1
  auto exported = reinterpret_cast<node_t*>(a->state);
  for (uint32_t offset = local; offset < (a->keys >> a->core_shift); offset += a->per_core) {
    uint32_t key = (offset << a->core_shift) | core;
    exported[key].next = scratch[uint64_t(offset) * 3];
    exported[key].value = scratch[uint64_t(offset) * 3 + 1];
    exported[key].version = scratch[uint64_t(offset) * 3 + 2];
  }
#elif MESSAGE_TRANSPORT == 0
  if (a->kind == KIND_UPDATE) {
    auto state = reinterpret_cast<volatile node_t*>(a->state);
    for (uint32_t key = rank; key < a->keys; key += a->cores * a->per_core) {
      uint64_t packed = state[key].value;
      state[key].value = uint32_t(packed); state[key].version = packed >> 32;
    }
  }
#endif
  barrier(3);
  out->start = start; out->end = vx_rdcycle(); out->complete = FEEDBACK_COMPLETE;
}
