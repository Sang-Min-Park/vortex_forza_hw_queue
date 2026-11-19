#include <vortex2.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <vector>
#include "common.h"

static vx_device_h device = nullptr;
static vx_buffer_h results_buffer = nullptr;
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h completed = nullptr;

static void deadline_expired(int) {
  static const char message[] = "FAILED: CSR generalization test exceeded 125s deadline\n";
  ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)written;
  _exit(EXIT_FAILURE);
}

static void cleanup() {
  if (device != nullptr) {
    if (completed != nullptr) { vx_event_release(completed); }
    if (kernel != nullptr) { vx_kernel_release(kernel); }
    if (module_ != nullptr) { vx_module_release(module_); }
    if (queue != nullptr) { vx_queue_release(queue); }
    if (results_buffer != nullptr) { vx_buffer_release(results_buffer); }
    vx_device_release(device);
  }
  alarm(0);
}

#define RT_CHECK(expr) do { \
  int status = (expr); \
  if (status != 0) { \
    std::fprintf(stderr, "%s failed: %d\n", #expr, status); \
    cleanup(); \
    return EXIT_FAILURE; \
  } \
} while (false)

int main(int argc, char** argv) {
  const char* kernel_file = "kernel.vxbin";
  uint32_t waves = 1;
  uint32_t rounds = 4;
  uint32_t polls = 20000;
  uint32_t logical_pe = 0;
  int option;
  while ((option = getopt(argc, argv, "w:n:p:m:k:h")) != -1) {
    switch (option) {
    case 'w': waves = std::strtoul(optarg, nullptr, 0); break;
    case 'n': rounds = std::strtoul(optarg, nullptr, 0); break;
    case 'p': polls = std::strtoul(optarg, nullptr, 0); break;
    case 'k': kernel_file = optarg; break;
    case 'm':
      if (std::strcmp(optarg, "logical") == 0) {
        logical_pe = 1;
      } else if (std::strcmp(optarg, "physical") != 0) {
        std::fprintf(stderr, "Mode must be physical or logical.\n");
        return EXIT_FAILURE;
      }
      break;
    case 'h':
      std::printf("Usage: %s [-w CTA waves] [-m physical|logical] [-n messages/thread] "
                  "[-p poll limit] [-k kernel.vxbin]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (waves == 0 || waves > 32 || rounds == 0 || rounds > 1024 || polls == 0) {
    std::fprintf(stderr, "Use 1..32 waves, 1..1024 rounds, and nonzero poll limit.\n");
    return EXIT_FAILURE;
  }
  std::signal(SIGALRM, deadline_expired);
  alarm(125);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t queue_info = {sizeof(queue_info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &queue_info, &queue));
  uint64_t cores, warps, threads;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  uint64_t actors_per_core = warps * threads;
  uint64_t blocks = cores * waves;
  uint64_t total_threads = blocks * actors_per_core;
  if (cores == 0 || cores > 16 || actors_per_core == 0 || actors_per_core > 1024
      || (logical_pe && total_threads > 2048)) {
    std::fprintf(stderr, "Unsupported dimensions: require <=16 cores, <=1024 actors/core, "
                         "and <=2048 logical thread IDs.\n");
    cleanup();
    return EXIT_FAILURE;
  }
  std::vector<csr_generalize_result_t> results(total_threads);
  uint64_t bytes = results.size() * sizeof(results[0]);
  csr_generalize_arg_t args = {};
  args.actors_per_core = actors_per_core;
  args.threads_per_warp = threads;
  args.rounds = rounds;
  args.poll_limit = polls;
  args.logical_pe = logical_pe;
  RT_CHECK(vx_buffer_create(device, bytes, VX_MEM_READ_WRITE, &results_buffer));
  RT_CHECK(vx_buffer_address(results_buffer, &args.results_addr));
  RT_CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), bytes, 0, nullptr, nullptr));
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  std::printf("FORZA CSR generalize: %" PRIu64 " cores, %" PRIu64 " warps/core, %" PRIu64
              " lanes/warp; %u waves, %" PRIu64 " blocks, %" PRIu64 " logical threads; "
              "PE mode=%s, rounds=%u\n", cores, warps, threads, waves, blocks, total_threads,
              logical_pe ? "logical" : "physical", rounds);
  vx_launch_info_t launch = {};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = blocks;
  launch.block_dim[0] = actors_per_core;
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, results.data(), results_buffer, 0, bytes, 0, nullptr, &completed));
  RT_CHECK(vx_event_wait_value(completed, 1, UINT64_C(120000000000)));
  uint32_t errors = 0;
  uint32_t registration_errors = 0;
  uint64_t sent = 0;
  uint64_t received = 0;
  uint64_t retries = 0;
  std::vector<uint32_t> completed_by_block(blocks, 0);
  for (uint64_t index = 0; index < total_threads; ++index) {
    const auto& result = results[index];
    uint32_t block = index / actors_per_core;
    uint32_t thread = index % actors_per_core;
    bool valid = result.done == CSR_GENERALIZE_DONE && result.error == 0
              && result.block == block && result.thread == thread
              && result.sent == rounds && result.received == rounds;
    if (!valid) {
      ++errors;
      if (result.error == CSR_GENERALIZE_REGISTRATION) { ++registration_errors; }
      if (errors <= 12) {
        std::fprintf(stderr, "block=%u thread=%u physical=(%u,%u,%u) actor=%u PE=%u "
                             "error=%u before=0x%" PRIx64 " after=0x%" PRIx64
                             " status=0x%" PRIx64 " expected=0x%" PRIx64 " actual=0x%" PRIx64
                             " sent=%u received=%u done=0x%x\n",
                     block, thread, result.core, result.warp, result.lane, result.actor,
                     result.pe, result.error, result.registration_before, result.registration_after,
                     result.status, result.expected, result.actual, result.sent,
                     result.received, result.done);
      }
    } else {
      ++completed_by_block[block];
    }
    sent += result.sent;
    received += result.received;
    retries += result.retries;
  }
  for (uint32_t block = 0; block < blocks; ++block) {
    std::printf("block=%u completed=%u/%" PRIu64 "\n", block,
                completed_by_block[block], actors_per_core);
  }
  cleanup();
  std::printf("%s: errors=%u registration_errors=%u sent=%" PRIu64
              " received=%" PRIu64 " retries=%" PRIu64 "\n",
              errors == 0 ? "PASSED" : "FAILED", errors, registration_errors,
              sent, received, retries);
  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
