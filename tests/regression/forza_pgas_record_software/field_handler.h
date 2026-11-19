#pragma once

#include "device.h"

class FieldHandler {
public:
  __attribute__((always_inline)) explicit FieldHandler(args_t* args,
                                                       bool first_visit_zero = false)
    : args_(args)
    , scratch_(reinterpret_cast<volatile uint64_t*>(__local_mem()))
    , responses_(reinterpret_cast<volatile uint64_t*>(args->responses))
    , lane_(vx_thread_id())
    , first_visit_zero_(first_visit_zero) {
  }

  // All lanes of one four-lane owner warp call with the same descriptor pointer.
  __attribute__((always_inline)) uint32_t apply(uint64_t request_pointer) {
    auto words = reinterpret_cast<const uint64_t*>(request_pointer);
    uint64_t pair = words[0];
    uint64_t tail = words[1];
    uint32_t key = uint32_t(pair);
    uint32_t id = uint32_t(pair >> 32);
    if (key >= args_->keys || id >= args_->requests) {
      return UINT32_MAX;
    }
    if (!valid_ || key != key_) {
      flush();
      key_ = key;
      value_ = 0;
      if (!first_visit_zero_ && lane_ < 3) {
        value_ = scratch_[uint64_t(key >> args_->core_shift) * 3 + lane_];
      }
      valid_ = true;
    }

    uint64_t old_value = value_;
    uint64_t increment = lane_ == 0 ? 1
                       : lane_ == 1 ? uint64_t(int64_t(int32_t(uint32_t(tail)))) : 0;
    uint64_t xor_operand = lane_ == 2 ? uint64_t(id) : 0;
    value_ = (value_ + increment) ^ xor_operand;
    // Bulk callers observe completion only after the drained phase and output flush.
    responses_[uint64_t(id) * 4 + lane_] = lane_ < 3 ? old_value : RESPONSE_COMPLETE;
    return id;
  }

  __attribute__((always_inline)) void flush() {
    if (valid_ && lane_ < 3) {
      swap64(scratch_ + uint64_t(key_ >> args_->core_shift) * 3 + lane_, value_);
    }
    valid_ = false;
  }

  __attribute__((always_inline)) uint32_t key() const {
    return key_;
  }

private:
  args_t* args_;
  volatile uint64_t* scratch_;
  volatile uint64_t* responses_;
  uint32_t lane_;
  bool first_visit_zero_;
  bool valid_ = false;
  uint32_t key_ = 0;
  uint64_t value_ = 0;
};
