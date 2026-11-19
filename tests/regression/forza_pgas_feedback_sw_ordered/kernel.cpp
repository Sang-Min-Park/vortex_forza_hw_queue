#include "device.h"

#if MESSAGE_TRANSPORT == 0
__attribute__((noinline)) static void native_clients(args_t* __UNIFORM__ a,
                                                    result_t* out) {
  const uint32_t client = vx_core_id() * 4 + vx_thread_id();
  uint32_t key = initial_key(client, a->active_keys), step = 0, cookie = initial_cookie(client);
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t digest = 0, retries = 0;
  bool complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    if (step < a->steps) {
      bool acquired = true;
      if (a->kind == KIND_UPDATE) {
        acquired = swap64(reinterpret_cast<uint64_t*>(a->state)
                          + uint64_t(key) * 4 + 3, 1) == 0;
      }
      if (acquired) {
        uint64_t reply = handle_query<STATE_LOCAL>(a, key, client, step, cookie);
        if (a->kind == KIND_UPDATE) {
          vx_fence();
          if (swap64(reinterpret_cast<uint64_t*>(a->state)
                     + uint64_t(key) * 4 + 3, 0) != 1) out->error = 10;
        }
        uint64_t index = uint64_t(client) * a->steps + step;
        traces[index].key = trace_query(key, cookie);
        traces[index].reply = reply;
        digest += reply;
        key = reply_key(reply);
        cookie = next_cookie(reply);
        ++step;
      } else {
        ++retries;
      }
    }
    uint32_t __UNIFORM__ all_done = vx_vote_all(step == a->steps);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 11;
  out->processed = step;
  out->digest = digest;
  out->retries = retries;
}

#else
__attribute__((noinline)) static void message_clients(args_t* __UNIFORM__ a,
                                                     result_t* out) {
  const uint32_t client = vx_core_id() * 4 + vx_thread_id();
  uint32_t key = initial_key(client, a->active_keys), step = 0, cookie = initial_cookie(client);
  auto traces = reinterpret_cast<volatile trace_t*>(a->traces);
  uint64_t sent = 0, received = 0, retries = 0, digest = 0, credit = 0;
  bool waiting = false, done_sent = false, complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    if (!waiting && step < a->steps) {
      uint32_t core = key & (a->cores - 1);
      uint32_t pe = (key >> a->core_shift) & 3;
      uint64_t status = send_token(a, core, pe, 0, query_token(key, client, step, cookie));
      if (status == 0) { waiting = true; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 20;
    }
    if (waiting) {
      uint64_t reply = pop_token(a, 1, true);
      if (reply) {
        if (!(reply & 1) || reply_key(reply) >= a->keys) out->error = 21;
        uint64_t index = uint64_t(client) * a->steps + step;
        traces[index].key = trace_query(key, cookie);
        traces[index].reply = reply;
        digest += reply;
        key = reply_key(reply);
        cookie = next_cookie(reply);
        ++step; ++received;
        waiting = false;
      }
    }
    if (step == a->steps && !done_sent) {
      uint64_t status = send_token(a, 0, 0, 0, TOKEN_DONE | 1 | (uint64_t(client) << 16));
      if (status == 0) { done_sent = true; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 22;
    }
    bool done = false;
    if (done_sent) { credit = credits(); done = credit == 0; }
    uint32_t __UNIFORM__ all_done = vx_vote_all(done);
    if (all_done) { complete = true; break; }
  }
  if (!complete) out->error = 23;
  out->processed = step;
  out->sent = sent; out->received = received; out->retries = retries;
  out->outstanding = credit; out->digest = digest;
}

__attribute__((noinline)) static void message_servers(args_t* __UNIFORM__ a,
                                                     result_t* out) {
  const uint32_t core = vx_core_id(), lane = vx_thread_id();
  const uint32_t clients = a->cores * 4, servers = clients;
  const bool coordinator = core == 0 && lane == 0;
  uint32_t done_mask = 0, stop_cursor = 0;
  uint64_t pending_reply = 0, handled = 0, sent = 0, received = 0, retries = 0, credit = 0;
  uint32_t pending_client = 0;
  bool stopped = false, complete = false;
  for (uint32_t iteration = 0; iteration < a->limit; ++iteration) {
    if (pending_reply) {
      uint64_t status = send_token(a, pending_client / 4, 4 + pending_client % 4,
                                   1, pending_reply);
      if (status == 0) { pending_reply = 0; ++sent; }
      else if (status == 1) ++retries;
      else out->error = 30;
    }
    bool broadcasting = coordinator && stop_cursor && stop_cursor <= servers;
    if (broadcasting) {
      uint32_t endpoint_id = stop_cursor & (servers - 1);
      uint64_t status = send_token(a, endpoint_id / 4, endpoint_id % 4, 0, TOKEN_STOP | 1);
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
              || ((key >> a->core_shift) & 3) != lane) {
            out->error = 33;
          } else {
            pending_reply = handle_query<STATE_LOCAL>(a, key, client, step, query_cookie(token));
            pending_client = client;
            ++handled;
#if MESSAGE_TRANSPORT == 3
            uint64_t status = send_token(a, client / 4, 4 + client % 4, 1, pending_reply);
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
  uint32_t rank = rank_of(a);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  out->rank = rank;
#if MESSAGE_TRANSPORT != 0
  if (!register_endpoint()) out->error = 1;
#endif
#if STATE_LOCAL
  uint32_t local = vx_warp_id() * 4 + vx_thread_id();
  auto state = reinterpret_cast<const node_t*>(a->state);
  auto scratch = reinterpret_cast<uint64_t*>(__local_mem());
  uint32_t core = vx_core_id();
#if STATE_LOCAL == 1
  for (uint32_t offset = local; offset < (a->keys >> a->core_shift); offset += a->per_core) {
    uint32_t key = (offset << a->core_shift) | core;
    swap64(scratch + uint64_t(offset) * 3, state[key].next);
    swap64(scratch + uint64_t(offset) * 3 + 1, state[key].value);
    swap64(scratch + uint64_t(offset) * 3 + 2, state[key].version);
  }
#else
  (void)core;
  for (uint32_t key = local; key < a->keys; key += a->per_core) {
    swap64(scratch + uint64_t(key) * 2, state[key].next);
    swap64(scratch + uint64_t(key) * 2 + 1, state[key].value);
  }
#endif
#endif
  barrier(1);
  uint64_t start = vx_rdcycle();
  if (vx_warp_id() == 1) {
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
#endif
  barrier(3);
  out->start = start; out->end = vx_rdcycle(); out->complete = FEEDBACK_COMPLETE;
}
