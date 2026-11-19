#include <vortex2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "common.h"

#define CHECK(expr) do { int status = (expr); if (status) { std::fprintf(stderr, "%s: %d\n", #expr, status); return 1; } } while (false)

int main(int argc, char** argv) {
#ifdef RUNTIME_SHORT
  const char* file = argc > 1 ? argv[1] : "kernel-short.vxbin";
#else
  const char* file = argc > 1 ? argv[1] : "kernel-full.vxbin";
#endif
  vx_device_h device;
  vx_queue_h queue;
  vx_buffer_h buffer;
  vx_module_h module;
  vx_kernel_h kernel;
  vx_event_h completed;
  CHECK(vx_device_open(0, &device));
  vx_queue_info_t info{sizeof(info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  CHECK(vx_queue_create(device, &info, &queue));
  uint64_t cores, warps, threads;
  CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  if (cores != 2) return 1;
  smoke_arg_t args{};
  args.actors = cores * warps * threads;
  args.per_core = warps * threads;
  args.threads = threads;
  std::vector<smoke_result_t> result(args.actors);
  CHECK(vx_buffer_create(device, result.size() * sizeof(result[0]), VX_MEM_READ_WRITE, &buffer));
  CHECK(vx_buffer_address(buffer, &args.results));
  CHECK(vx_enqueue_write(queue, buffer, 0, result.data(), result.size() * sizeof(result[0]), 0, nullptr, nullptr));
  CHECK(vx_module_load_file(device, file, &module));
  CHECK(vx_module_get_kernel(module, "main", &kernel));
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = cores;
  launch.block_dim[0] = warps * threads;
  CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  CHECK(vx_enqueue_read(queue, result.data(), buffer, 0, result.size() * sizeof(result[0]), 0, nullptr, &completed));
  CHECK(vx_event_wait_value(completed, 1, UINT64_C(175000000000)));
  uint32_t errors = 0;
  for (uint32_t rank = 0; rank < args.actors; ++rank) {
    const auto& r = result[rank];
    if (r.rank != rank || r.errors || r.complete != SMOKE_COMPLETE || r.received != 8 || r.done != 7 * args.actors) {
      ++errors;
      std::fprintf(stderr, "actor=%u errors=%u received=%u done=%u complete=%x\n", rank, r.errors, r.received, r.done, r.complete);
    }
  }
  CHECK(vx_event_release(completed));
  CHECK(vx_kernel_release(kernel));
  CHECK(vx_module_release(module));
  CHECK(vx_buffer_release(buffer));
  CHECK(vx_queue_release(queue));
  CHECK(vx_device_release(device));
  if (errors) return 1;
  std::printf("PASSED: actual SimX CSR short/full runtime, %u actors, sizes1/8/16/40/56, replies, epochs, DONE, raw1B zero-padding and repeated slot reuse\n", args.actors);
}
