#pragma once

#include "common.h"

static inline void compound_respond_to(volatile compound_response_t* responses,
                                       const compound_request_t& request,
                                       uint64_t count, uint64_t sum, uint64_t xor_value) {
  auto response = responses + request.id;
  response->old_count = count;
  response->old_sum = sum;
  response->old_xor = xor_value;
  response->complete = COMPOUND_RESPONSE;
}

class OwnerState {
public:
  OwnerState(compound_arg_t* arg, compound_result_t* result)
    : records_(reinterpret_cast<volatile compound_record_t*>(arg->records_addr)),
      responses_(reinterpret_cast<volatile compound_response_t*>(arg->responses_addr)) {
    (void)result;
  }

  void apply(const compound_request_t& request) {
    if (key_ != request.key) {
      finish();
      key_ = request.key;
      auto record = records_ + key_;
      count_ = record->count;
      sum_ = record->sum;
      xor_ = record->xor_value;
    }
    compound_respond_to(responses_, request, count_, sum_, xor_);
    ++count_;
    sum_ += request.delta;
    xor_ ^= compound_token(request.id);
    ++processed_;
  }

  void finish() {
    if (key_ != UINT32_MAX) {
      auto record = records_ + key_;
      record->count = count_;
      record->sum = sum_;
      record->xor_value = xor_;
      key_ = UINT32_MAX;
    }
  }

  uint64_t processed() const { return processed_; }

private:
  volatile compound_record_t* records_;
  volatile compound_response_t* responses_;
  uint32_t key_ = UINT32_MAX;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t xor_ = 0;
  uint64_t processed_ = 0;
};
