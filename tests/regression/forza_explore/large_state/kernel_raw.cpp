#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_message_lib.vortex.h>
#include "common.h"

struct batch_t {
  uint64_t count;
  request_t requests[6];
};
static_assert(sizeof(request_t) == 8, "raw scatter uses compact requests");
static_assert(sizeof(batch_t) == 56, "batch must fit one payload");

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  const uint32_t local = vx_warp_id() * arg->threads + vx_thread_id();
  const uint32_t rank = vx_core_id() * arg->per_core + local;
  const uint32_t actors = arg->actors;
  auto output = reinterpret_cast<result_t*>(arg->results_addr) + rank;
  auto input = reinterpret_cast<const request_t*>(arg->requests_addr);
  auto state = reinterpret_cast<int64_t*>(arg->state_addr);
  output->rank = rank;
#ifdef LARGE_BATCH
  if (uint64_t(arg->per_core) * actors * sizeof(batch_t) > (UINT64_C(1) << VX_CFG_LMEM_LOG_SIZE)) {
    output->error = 4;
    return;
  }
  auto batches = reinterpret_cast<batch_t*>(__local_mem()) + uint64_t(local) * actors;
  for (uint32_t owner = 0; owner < actors; ++owner) batches[owner].count = 0;
#endif
  uint32_t initialized = forza_vortex_init();
  if (initialized != SUCCESS) output->error = initialized;
  __syncthreads();
  vx_fence();
  vortex::gbarrier(2).arrive_and_wait();
  uint64_t start = vx_rdcycle();
  uint64_t inputs = 0, flushes = 0, locals = 0, sent = 0, attempts = 0, retries = 0, received = 0;
  uint64_t outstanding = 0;
  uint32_t index = rank;
#ifdef LARGE_BATCH
  uint32_t drain_owner = 0;
  auto send_batch = [&](uint32_t owner) {
    uint32_t count = batches[owner].count;
    if (count == 0) return true;
    ++attempts;
    uint32_t status = forza_send_sized(0, batches + owner, 8 + count * 8, 0,
                                       owner / arg->per_core, owner % arg->per_core);
    if (status == ZEN_NOT_READY) {
      ++retries;
      return false;
    }
    if (status != SUCCESS) {
      output->error = status;
      return false;
    }
    sent += count;
    flushes += count;
    batches[owner].count = 0;
    return true;
  };
#endif
  bool finished = false, complete = false;
  for (uint32_t iteration = 0; iteration < arg->iteration_limit; ++iteration) {
    for (uint32_t step = 0; step < 8; ++step) {
      if (index < arg->updates) {
        request_t request = input[index];
        uint32_t owner = request.key / arg->words_per_actor;
        if (owner == rank) {
          state[request.key] += request.delta;
          ++locals;
          ++flushes;
        } else {
#ifdef LARGE_BATCH
          if (batches[owner].count == 6 && !send_batch(owner)) break;
          uint32_t slot = batches[owner].count;
          batches[owner].requests[slot] = request;
          batches[owner].count = slot + 1;
#else
          ++attempts;
          uint32_t status = forza_send_sized(0, &request, sizeof(request), 0,
                                             owner / arg->per_core, owner % arg->per_core);
          if (status == ZEN_NOT_READY) {
            ++retries;
            break;
          }
          if (status != SUCCESS) {
            output->error = status;
            break;
          }
          ++sent;
          ++flushes;
#endif
        }
        index += actors;
        ++inputs;
      } else {
#ifdef LARGE_BATCH
        if (drain_owner < actors) {
          if (!send_batch(drain_owner)) break;
          ++drain_owner;
        } else {
          finished = true;
          break;
        }
#else
        finished = true;
        break;
#endif
      }
    }
    uint32_t available = forza_message_available(0);
    if (available == SUCCESS) {
      uint64_t words[7];
#ifdef LARGE_BATCH
      uint32_t status = forza_message_receive_ready(0, words, 56);
      uint32_t count = status == SUCCESS ? uint32_t(words[0]) : 0;
      if (status == SUCCESS && count > 0 && count <= 6) {
        for (uint32_t entry = 0; entry < count; ++entry) {
          request_t request;
          memcpy(&request, words + entry + 1, sizeof(request));
          if (request.key >= arg->words || request.key / arg->words_per_actor != rank) {
            output->error = 5;
          } else {
            state[request.key] += request.delta;
            ++received;
          }
        }
      } else output->error = status == SUCCESS ? 5 : status;
#else
      uint32_t status = forza_message_receive_ready(0, words, 8);
      if (status != SUCCESS) output->error = status;
      else {
        request_t request;
        memcpy(&request, words, sizeof(request));
        if (request.key >= arg->words || request.key / arg->words_per_actor != rank) output->error = 5;
        else {
          state[request.key] += request.delta;
          ++received;
        }
      }
#endif
    } else if (available != NO_MSG) output->error = available;
    uint32_t status = forza_get_outstanding_msg_count(0, &outstanding);
    if (status != SUCCESS) output->error = status;
    uint32_t __UNIFORM__ all_complete = vx_vote_all(finished && received == output->expected_receive && outstanding == 0);
    if (all_complete) {
      complete = true;
      break;
    }
  }
  if (!complete) output->error = 6;
  vx_fence();
  vortex::gbarrier(3).arrive_and_wait();
  vx_fence();
  output->start = start;
  output->end = vx_rdcycle();
  output->inputs = inputs;
  output->flushes = flushes;
  output->local = locals;
  output->sent = sent;
  output->received = received;
  output->attempts = attempts;
  output->retries = retries;
  output->outstanding = outstanding;
  output->complete = LARGE_COMPLETE;
}
