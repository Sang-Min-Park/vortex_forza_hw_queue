#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
#ifdef LARGE_MESSAGE
#include <selector.forza.h>
#endif
#if __riscv_xlen != 64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "large_state requires RV64 and A"
#endif

#ifdef LARGE_MESSAGE
class StateSelector : public hclib::Selector<1, request_t> {
public:
  StateSelector(args_t* arg, result_t* result) : arg_(arg), result_(result) {
    mb[0].process = [this](request_t r, int source) {
      if (source != -1 || r.key >= arg_->words || r.key / arg_->words_per_actor != result_->rank) {
        result_->error = 1;
        return;
      }
      // Unique append slots avoid making the diagnostic digest another RMW.
      if (result_->received >= arg_->trace_stride) {
        result_->error = 7;
        return;
      }
      reinterpret_cast<request_t*>(arg_->trace_addr)[uint64_t(result_->rank) * arg_->trace_stride + result_->received] = r;
      reinterpret_cast<int64_t*>(arg_->state_addr)[r.key] += r.delta;
      ++result_->received;
    };
  }
private:
  args_t* arg_;
  result_t* result_;
};
#endif

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  const uint32_t rank = vx_core_id() * arg->per_core + vx_warp_id() * arg->threads + vx_thread_id();
  auto output = reinterpret_cast<result_t*>(arg->results_addr) + rank;
  output->rank = rank;
  auto input = reinterpret_cast<const request_t*>(arg->requests_addr);
  auto state = reinterpret_cast<int64_t*>(arg->state_addr);
  const uint32_t actors = arg->actors, count = arg->updates;
#ifdef LARGE_MESSAGE
  const char* deps[] = {"system", "bale_actor"};
  hclib::launch(deps, 2, [&] {
  StateSelector selector(arg, output);
  selector.start();
#endif
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t inputs = 0, flushes = 0, locals = 0, sent = 0, attempts = 0, retries = 0;
  auto flush = [&](uint64_t key, int64_t delta) {
#ifdef LARGE_MESSAGE
    if (key / arg->words_per_actor == rank) {
      state[key] += delta;
      ++locals;
    } else {
      ++attempts;
      if (!selector.try_send(0, request_t{static_cast<request_key_t>(key), static_cast<request_delta_t>(delta)}, key / arg->words_per_actor)) {
        ++retries;
        return false;
      }
      ++sent;
    }
#else
    int64_t* address = state + key;
    __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(address), "r"(delta) : "memory");
#endif
    ++flushes;
    return true;
  };
  [[maybe_unused]] cache_t cache;
  uint32_t index = rank;
  bool finished = false;
#ifdef LARGE_MESSAGE
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
#else
  while (!finished) {
#endif
    // A bounded burst amortizes polling and never waits inside a divergent lane.
    for (uint32_t step = 0; step < 8; ++step) {
      if (index < count) {
        request_t r = input[index];
#ifdef LARGE_NO_CACHE
        if (!flush(r.key, r.delta)) break;
#else
        if (r.key == cache.key0) { cache.sum0 += r.delta; cache.recent0 = true; }
        else if (r.key == cache.key1) { cache.sum1 += r.delta; cache.recent0 = false; }
        else if (cache.key0 == UINT64_MAX) { cache.key0 = r.key; cache.sum0 = r.delta; cache.recent0 = true; }
        else if (cache.key1 == UINT64_MAX) { cache.key1 = r.key; cache.sum1 = r.delta; cache.recent0 = false; }
        else if (cache.recent0) {
          if (!flush(cache.key1, cache.sum1)) break;
          cache.key1 = r.key; cache.sum1 = r.delta; cache.recent0 = false;
        } else {
          if (!flush(cache.key0, cache.sum0)) break;
          cache.key0 = r.key; cache.sum0 = r.delta; cache.recent0 = true;
        }
#endif
        index += actors;
        ++inputs;
      } else {
#ifndef LARGE_NO_CACHE
        if (cache.key0 != UINT64_MAX) {
          if (!flush(cache.key0, cache.sum0)) break;
          cache.key0 = UINT64_MAX;
        }
        if (cache.key1 != UINT64_MAX) {
          if (!flush(cache.key1, cache.sum1)) break;
          cache.key1 = UINT64_MAX;
        }
#endif
        finished = true;
        break;
      }
    }
#ifdef LARGE_MESSAGE
    selector.progress();
    bool ready = finished && output->received == output->expected_receive && selector.outstanding(0) == 0;
    uint32_t __UNIFORM__ all_complete = vx_vote_all(ready);
    if (all_complete) break;
    if (!selector.ok()) output->error = 2;
#endif
  }
  vx_fence();
#ifdef LARGE_MESSAGE
  output->outstanding = selector.outstanding(0);
  if (!finished || output->received != output->expected_receive || output->outstanding || !selector.ok()) output->error = 3;
#endif
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  output->start = start;
  output->end = vx_rdcycle();
  output->inputs = inputs; output->flushes = flushes; output->local = locals;
  output->sent = sent; output->attempts = attempts; output->retries = retries;
  output->complete = LARGE_COMPLETE;
#ifdef LARGE_MESSAGE
  });
#endif
}
