#pragma once
#include "device.h"

static inline __attribute__((always_inline)) void apply_quad(
    const request_t& request, uint32_t mask, uint64_t& count,
    uint64_t& sum, uint64_t& value, volatile response_t* responses) {
  uint32_t lane = vx_thread_id();
  uint32_t ids[4];
  int32_t deltas[4];
  ids[0] = vx_shfl_idx(request.id, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
  deltas[0] = int32_t(vx_shfl_idx(uint32_t(request.delta), 0, VX_QUAD_CVAL, VX_QUAD_MASK));
  ids[1] = vx_shfl_idx(request.id, 1, VX_QUAD_CVAL, VX_QUAD_MASK);
  deltas[1] = int32_t(vx_shfl_idx(uint32_t(request.delta), 1, VX_QUAD_CVAL, VX_QUAD_MASK));
  ids[2] = vx_shfl_idx(request.id, 2, VX_QUAD_CVAL, VX_QUAD_MASK);
  deltas[2] = int32_t(vx_shfl_idx(uint32_t(request.delta), 2, VX_QUAD_CVAL, VX_QUAD_MASK));
  ids[3] = vx_shfl_idx(request.id, 3, VX_QUAD_CVAL, VX_QUAD_MASK);
  deltas[3] = int32_t(vx_shfl_idx(uint32_t(request.delta), 3, VX_QUAD_CVAL, VX_QUAD_MASK));
  // Snapshot each member's prefix while all lanes advance the same state;
  // publish once after the uniform serial loop rather than inside its branches.
  uint64_t old_count = 0, old_sum = 0, old_value = 0;
  for (uint32_t member = 0; member < 4; ++member) {
    uint32_t id = ids[member];
    int32_t delta = deltas[member];
    if (mask & (1u << member)) {
      if (lane == member) {
        old_count = count; old_sum = sum; old_value = value;
      }
      branch_update(count, sum, value, delta, id);
    }
  }
  if ((mask >> lane) & 1)
    respond(responses, request.id, old_count, old_sum, old_value);
}
