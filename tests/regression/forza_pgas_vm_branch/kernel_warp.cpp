#include "branch_quad.h"

__kernel void kernel_main(args_t* __UNIFORM__ a) {
  uint32_t lane = vx_thread_id(), rank = rank_of(a);
  uint32_t workers = a->cores * a->per_core, cursor = rank;
  auto input = reinterpret_cast<const request_t*>(a->input);
  auto state = reinterpret_cast<volatile record_t*>(a->state);
  auto responses = reinterpret_cast<volatile response_t*>(a->responses);
  auto out = reinterpret_cast<result_t*>(a->results) + rank;
  out->rank = rank;
  barrier(1);
  uint64_t start = vx_rdcycle(), processed = 0, retries = 0;
  bool complete = false;
  for (uint32_t round = 0; round < a->limit; ++round) {
    bool pending = cursor < a->requests;
    request_t request{};
    if (pending) request = load_request(input, cursor);
    bool groups_complete = true;
    for (uint32_t leader = 0; leader < 4; ++leader) {
      uint32_t __UNIFORM__ key = vx_shfl_idx(request.key, leader, VX_QUAD_CVAL, VX_QUAD_MASK);
      uint32_t __UNIFORM__ group = vx_vote_ballot(pending && request.key == key);
      uint32_t __UNIFORM__ first = (group & (1u << leader)) && !(group & ((1u << leader) - 1));
      if (!first) continue;
      bool accepted = false;
      for (uint32_t attempt = 0; attempt < 4096; ++attempt) {
        uint64_t lock = 1;
        if (lane == leader) lock = swap64(&state[key].lock, 1);
        uint32_t __UNIFORM__ acquired = vx_shfl_idx(lock, leader, VX_QUAD_CVAL, VX_QUAD_MASK) == 0;
        if (acquired) {
          // Every lane reads the same locked record.  A leader-only load followed
          // by shuffles lets this compiler duplicate the collectives into the
          // divergent load branches, where the source lane may be inactive.
          uint64_t count = state[key].count;
          uint64_t sum = state[key].sum;
          uint64_t value = state[key].xor_value;
          apply_quad(request, group, count, sum, value, responses);
          if (lane == leader) {
            state[key].count = count; state[key].sum = sum; state[key].xor_value = value;
            vx_fence(); swap64(&state[key].lock, 0);
          }
          if ((group >> lane) & 1) { cursor += workers; ++processed; }
          accepted = true;
          break;
        }
        if ((group >> lane) & 1) ++retries;
      }
      if (!accepted) { groups_complete = false; break; }
    }
    if (!groups_complete) { out->error = 2; break; }
    uint32_t __UNIFORM__ done = vx_vote_all(cursor >= a->requests);
    if (done) { complete = true; break; }
  }
  if (!complete) out->error = 1;
  barrier(2);
  out->processed = processed; out->retries = retries;
  out->start = out->initialized = start;
  out->processed_cycle = out->end = vx_rdcycle();
  out->complete = RECORD_COMPLETE;
}
