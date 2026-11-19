#pragma once

#include "device.h"

class WarpHandler {
public:
  __attribute__((always_inline)) explicit WarpHandler(args_t* args)
    : keys_(args->keys), requests_(args->requests), shift_(args->core_shift)
    , lane_(vx_thread_id())
    , scratch_(reinterpret_cast<volatile uint64_t*>(__local_mem()))
    , responses_(reinterpret_cast<volatile response_t*>(args->responses)) {
  }

  __attribute__((always_inline)) uint32_t apply(const request_t& request,
                                               bool valid) {
    uint32_t __UNIFORM__ mask = vx_vote_ballot(valid);
    uint32_t __UNIFORM__ bad = vx_vote_any(valid && (request.key >= keys_ || request.id >= requests_));
    if (mask && !bad) {
    uint32_t __UNIFORM__ first = __builtin_ctz(mask);
    uint32_t __UNIFORM__ key = vx_shfl_idx(request.key, first, VX_QUAD_CVAL, VX_QUAD_MASK);
    uint32_t __UNIFORM__ same = vx_vote_all(!valid || request.key == key);
    if (same) {
      if (!cached_ || cached_key_ != key) {
        flush();
        uint64_t count = 0, sum = 0, value = 0;
        if (lane_ == 0) {
          auto p = scratch_ + uint64_t(key >> shift_) * 3;
          count = p[0]; sum = p[1]; value = p[2];
        }
        count_ = vx_shfl_idx(count, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        sum_ = vx_shfl_idx(sum, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        xor_ = vx_shfl_idx(value, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        cached_key_ = key;
        cached_ = true;
      }
      apply_group(request, valid, mask, lane_, responses_, count_, sum_, xor_);
    } else {
      flush();
      apply_mixed(request, valid, shift_, lane_, scratch_, responses_);
    }
    }
    return bad ? UINT32_MAX : __builtin_popcount(mask);
  }

  __attribute__((always_inline)) void flush() {
    if (cached_ && lane_ == 0) {
      auto p = scratch_ + uint64_t(cached_key_ >> shift_) * 3;
      swap64(p, count_); swap64(p + 1, sum_); swap64(p + 2, xor_);
    }
    cached_ = false;
  }

private:
  __attribute__((noinline)) static void apply_mixed(const request_t& request, bool valid,
      uint32_t shift, uint32_t lane, volatile uint64_t* scratch,
      volatile response_t* responses) {
    for (uint32_t leader = 0; leader < 4; ++leader) {
      uint32_t __UNIFORM__ key = vx_shfl_idx(request.key, leader, VX_QUAD_CVAL, VX_QUAD_MASK);
      uint32_t __UNIFORM__ group = vx_vote_ballot(valid && request.key == key);
      if ((group & (1u << leader)) && !(group & ((1u << leader) - 1))) {
        uint64_t count = 0, sum = 0, value = 0;
        if (lane == 0) {
          auto p = scratch + uint64_t(key >> shift) * 3;
          count = p[0]; sum = p[1]; value = p[2];
        }
        count = vx_shfl_idx(count, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        sum = vx_shfl_idx(sum, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        value = vx_shfl_idx(value, 0, VX_QUAD_CVAL, VX_QUAD_MASK);
        apply_group(request, valid && request.key == key, group, lane, responses, count, sum, value);
        if (lane == 0) {
          auto p = scratch + uint64_t(key >> shift) * 3;
          swap64(p, count); swap64(p + 1, sum); swap64(p + 2, value);
        }
      }
    }
  }

  __attribute__((always_inline)) static void apply_group(const request_t& request, bool member,
       uint32_t group, uint32_t lane, volatile response_t* responses,
       uint64_t& count, uint64_t& sum, uint64_t& value) {
    uint64_t own_sum = member ? uint64_t(int64_t(request.delta)) : 0;
    uint64_t own_xor = member ? request.id : 0;
    uint64_t inclusive_sum = own_sum, inclusive_xor = own_xor;
    for (uint32_t step = 1; step < 4; step <<= 1) {
      uint64_t other_sum = vx_shfl_up(inclusive_sum, step, VX_QUAD_CVAL, VX_QUAD_MASK);
      uint64_t other_xor = vx_shfl_up(inclusive_xor, step, VX_QUAD_CVAL, VX_QUAD_MASK);
      inclusive_sum += lane >= step ? other_sum : 0;
      inclusive_xor ^= lane >= step ? other_xor : 0;
    }
    uint64_t total_sum = vx_shfl_idx(inclusive_sum, 3, VX_QUAD_CVAL, VX_QUAD_MASK);
    uint64_t total_xor = vx_shfl_idx(inclusive_xor, 3, VX_QUAD_CVAL, VX_QUAD_MASK);
    uint32_t prior = __builtin_popcount(group & ((1u << lane) - 1));
    uint32_t total = __builtin_popcount(group);
    if (member) respond(responses, request.id, count + prior,
                        sum + inclusive_sum - own_sum,
                        value ^ inclusive_xor ^ own_xor);
    count += total; sum += total_sum; value ^= total_xor;
  }

  uint32_t keys_, requests_, shift_, lane_;
  volatile uint64_t* scratch_;
  volatile response_t* responses_;
  bool cached_ = false;
  uint32_t cached_key_ = 0;
  uint64_t count_ = 0, sum_ = 0, xor_ = 0;
};
