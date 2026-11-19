#include <vortex2.h>
#include <VX_types.h>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <csignal>
#include <unistd.h>
#include <vector>
#include <string>
#include "common.h"

static vx_device_h device;
static vx_queue_h queue;
static vx_buffer_h buffer;
static vx_module_h module_;
static vx_kernel_h kernel;
static vx_event_h event_;

static void cleanup() {
  if (event_) vx_event_release(event_);
  if (kernel) vx_kernel_release(kernel);
  if (module_) vx_module_release(module_);
  if (buffer) vx_buffer_release(buffer);
  if (queue) vx_queue_release(queue);
  if (device) vx_device_release(device);
  alarm(0);
}
static void deadline(int) {
  const char message[] = "FAILED: packet/PGAS smoke deadline\n";
  (void)!write(2, message, sizeof(message) - 1);
  _exit(1);
}
#define CHECK(expr) do { int status = (expr); if (status) { \
  std::fprintf(stderr, "%s failed: %d\n", #expr, status); cleanup(); return 1; \
} } while (false)

int main(int argc, char** argv) {
  std::string binary = argc > 1 ? argv[1] : std::string("kernel-") + TRANSPORT_NAME + ".vxbin";
  std::signal(SIGALRM, deadline); alarm(180);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi{sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  CHECK(vx_queue_create(device, &qi, &queue));
  uint64_t cores, warps, threads, local_capacity;
  CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &local_capacity));
  uint32_t per_core = warps * threads, actors = cores * per_core;
  if (cores < 2 || actors > 128 || uint64_t(per_core) * 32 > local_capacity) {
    std::fprintf(stderr, "Unsupported smoke geometry\n"); cleanup(); return 1;
  }
  std::vector<result_t> results(actors);
  CHECK(vx_buffer_create(device, results.size() * sizeof(result_t), VX_MEM_READ_WRITE, &buffer));
  args_t arg{};
  CHECK(vx_buffer_address(buffer, &arg.results));
  arg.actors = actors; arg.per_core = per_core; arg.threads = threads;
  arg.limit = argc > 2 ? std::strtoul(argv[2], nullptr, 0) : 100000;
  CHECK(vx_enqueue_write(queue, buffer, 0, results.data(), results.size() * sizeof(result_t), 0, nullptr, nullptr));
  CHECK(vx_module_load_file(device, binary.c_str(), &module_));
  CHECK(vx_module_get_kernel(module_, "main", &kernel));
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch); launch.kernel = kernel;
  launch.args_host = &arg; launch.args_size = sizeof(arg);
  launch.ndim = 1; launch.grid_dim[0] = cores; launch.block_dim[0] = per_core;
  launch.lmem_size = per_core * 32;
  CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  CHECK(vx_enqueue_read(queue, results.data(), buffer, 0, results.size() * sizeof(result_t), 0, nullptr, &event_));
  CHECK(vx_event_wait_value(event_, 1, UINT64_C(180000000000)));
  uint64_t errors = 0, retries = 0;
  for (uint32_t rank = 0; rank < actors; ++rank) {
    const auto& r = results[rank];
    bool bad = r.rank != rank || r.error || r.complete != MMIO_COMPLETE
            || (arg.limit != 0 && (r.sent != RING_MESSAGES + 8 || r.received != RING_MESSAGES + 8));
    uint32_t source = (rank + actors - per_core) % actors;
    for (uint32_t offset = 0; offset < 4 && arg.limit != 0; ++offset) {
      uint64_t expected = initial_state(rank, offset);
      if (offset == 1) expected += source + 1;
      if (r.state[offset] != expected) {
        std::fprintf(stderr, "rank=%u state%u=%016" PRIx64 " expected=%016" PRIx64 "\n",
                     rank, offset, r.state[offset], expected);
        bad = true;
      }
    }
    if (bad) {
      ++errors;
      std::fprintf(stderr, "rank=%u error=%" PRIu64 " complete=%" PRIx64
          " sent=%" PRIu64 " received=%" PRIu64 "\n",rank,r.error,r.complete,r.sent,r.received);
    }
    retries += r.retries;
  }
  cleanup();
  if (errors) { std::fprintf(stderr, "FAILED: %" PRIu64 " actors\n", errors); return 1; }
  if (arg.limit == 0) {
    std::printf("PASSED: %s initialization only, %u actors\n", TRANSPORT_NAME, actors);
    return 0;
  }
  std::printf("PASSED: %s transport, %u actors, %u ring messages/actor, full64 including zero/high bits, "
              "PGAS remote GET/UPDATE/reply on real core-local SRAM, errors0, send_retries=%" PRIu64 "\n",
              TRANSPORT_NAME,actors,RING_MESSAGES,retries);
  return 0;
}
