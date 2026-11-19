#pragma once
#include <stdint.h>
struct smoke_result_t {
  uint32_t rank, errors, received, done, complete;
};
struct smoke_arg_t {
  uint64_t results;
  uint32_t actors, per_core, threads;
};
enum { SMOKE_COMPLETE = 0x51554954 };
