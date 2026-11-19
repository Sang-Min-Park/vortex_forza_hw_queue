#include <vortex2.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <vector>
#include "common.h"

static vx_device_h device = nullptr;
static vx_buffer_h results_buffer = nullptr;
static vx_buffer_h scratch_buffer = nullptr;
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h completed = nullptr;

static void deadline_expired(int) {
  static const char message[] = "FAILED: FORZA queue test exceeded the 125s deadline\n";
  ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)written;
  _exit(EXIT_FAILURE);
}

static void cleanup() {
  if (device != nullptr) {
    if (completed != nullptr) {
      vx_event_release(completed);
    }
    if (kernel != nullptr) {
      vx_kernel_release(kernel);
    }
    if (module_ != nullptr) {
      vx_module_release(module_);
    }
    if (queue != nullptr) {
      vx_queue_release(queue);
    }
    if (scratch_buffer != nullptr) {
      vx_buffer_release(scratch_buffer);
    }
    if (results_buffer != nullptr) {
      vx_buffer_release(results_buffer);
    }
    vx_device_release(device);
    device = nullptr;
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

static const char* error_name(uint32_t error) {
  switch (error) {
  case 0: return "incomplete";
  case FORZA_TEST_REGISTRATION: return "registration/status";
  case FORZA_TEST_SEND_TIMEOUT: return "enqueue timeout";
  case FORZA_TEST_RECEIVE_TIMEOUT: return "receive timeout";
  case FORZA_TEST_ACK_TIMEOUT: return "ACK timeout";
  case FORZA_TEST_CONTROL: return "control word";
  case FORZA_TEST_PAYLOAD: return "payload/order";
  case FORZA_TEST_POINTER: return "peek pointer";
  case FORZA_TEST_RELEASE: return "release";
  case FORZA_TEST_ORDER: return "sender FIFO";
  case FORZA_TEST_MIXED_LOAD: return "mixed memory load";
  case FORZA_TEST_NEGATIVE_ABI: return "negative ABI status";
  default: return "unknown";
  }
}

int main(int argc, char** argv) {
  const char* kernel_file = "kernel.vxbin";
  uint32_t rounds = 8;
  uint32_t poll_limit = FORZA_TEST_POLL_LIMIT;
  uint32_t error_mode = 0;
  int option;
  while ((option = getopt(argc, argv, "ek:n:p:h")) != -1) {
    switch (option) {
    case 'k': kernel_file = optarg; break;
    case 'n': rounds = std::strtoul(optarg, nullptr, 0); break;
    case 'p': poll_limit = std::strtoul(optarg, nullptr, 0); break;
    case 'e': error_mode = 1; break;
    case 'h':
      std::printf("Usage: %s [-k kernel.vxbin] [-n rounds] [-p poll limit] [-e negative ABI]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (rounds < 2 || rounds > 1024 || poll_limit == 0) {
    std::fprintf(stderr, "Use 2..1024 rounds and a nonzero poll limit.\n");
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
  if (cores < 2 || cores > 16 || (cores & 1) != 0 || warps < 2
   || threads < 2 || threads > FORZA_TEST_MAX_THREADS || warps * threads > 2048) {
    std::fprintf(stderr, "forza_queue requires an even 2..16 cores, >=2 warps, "
                         "2..64 threads and <=2048 actors/core.\n");
    cleanup();
    return EXIT_FAILURE;
  }

  kernel_arg_t args = {};
  if (error_mode != 0 && cores == 16) {
    std::fprintf(stderr, "Negative ABI mode requires fewer than 16 cores for an invalid zone.\n");
    cleanup();
    return EXIT_FAILURE;
  }
  args.num_cores = cores;
  args.num_threads = threads;
  args.actors_per_core = warps * threads;
  args.num_actors = cores * args.actors_per_core;
  args.rounds = rounds;
  args.poll_limit = poll_limit;
  args.error_mode = error_mode;
  std::vector<actor_result_t> results(args.num_actors);
  std::vector<uint64_t> scratch(args.num_actors, 0);
  uint64_t results_size = results.size() * sizeof(results[0]);
  uint64_t scratch_size = scratch.size() * sizeof(scratch[0]);
  RT_CHECK(vx_buffer_create(device, results_size, VX_MEM_READ_WRITE, &results_buffer));
  RT_CHECK(vx_buffer_address(results_buffer, &args.results_addr));
  RT_CHECK(vx_buffer_create(device, scratch_size, VX_MEM_READ_WRITE, &scratch_buffer));
  RT_CHECK(vx_buffer_address(scratch_buffer, &args.scratch_addr));
  RT_CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), results_size, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, scratch_buffer, 0, scratch.data(), scratch_size, 0, nullptr, nullptr));
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  std::printf("FORZA queues: %" PRIu64 " cores, %" PRIu64 " warps, %" PRIu64
              " lanes, %u rounds, %u actors\n", cores, warps, threads, rounds, args.num_actors);
  vx_launch_info_t launch = {};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = args.num_cores;
  launch.block_dim[0] = args.actors_per_core;
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, results.data(), results_buffer, 0, results_size, 0, nullptr, &completed));
  RT_CHECK(vx_event_wait_value(completed, 1, UINT64_C(120000000000)));

  uint32_t errors = 0;
  uint64_t sent = 0;
  uint64_t received = 0;
  for (uint32_t actor = 0; actor < args.num_actors; ++actor) {
    const auto& result = results[actor];
    uint32_t core = actor / args.actors_per_core;
    uint32_t local_pe = actor % args.actors_per_core;
    uint32_t warp = local_pe / args.num_threads;
    uint32_t lane = local_pe % args.num_threads;
    uint32_t expected_sent = error_mode ? 0 : rounds * 4 + (warp == 0 ? rounds * 4 : 0);
    uint32_t expected_received = error_mode ? 0 : rounds * 4
                               + (local_pe == args.num_threads ? rounds * 4 * args.num_threads : 0);
    uint32_t expected_mixed_loads = error_mode ? 0 : rounds * 4;
    bool valid = result.done == FORZA_TEST_DONE && result.error == 0
              && result.core == core && result.warp == warp && result.lane == lane
              && result.local_pe == local_pe && result.logical_pe == args.actors_per_core - 1 - local_pe
              && result.sent == expected_sent
              && result.received == expected_received && result.mixed_loads == expected_mixed_loads
              && result.final_count0 == 0 && result.final_count1 == 0;
    if (!valid) {
      ++errors;
      std::fprintf(stderr, "actor %u (core=%u warp=%u lane=%u): %s, phase=%u "
                           "expected=0x%016" PRIx64 " actual=0x%016" PRIx64
                           " sent=%u/%u received=%u/%u done=0x%x\n",
                   actor, core, warp, lane, error_name(result.error), result.phase,
                   result.expected, result.actual, result.sent, expected_sent,
                   result.received, expected_received, result.done);
    }
    sent += result.sent;
    received += result.received;
  }
  cleanup();
  if (errors != 0 || sent != received) {
    std::fprintf(stderr, "FAILED: %u actor errors, sent=%" PRIu64 " received=%" PRIu64 "\n",
                 errors, sent, received);
    return EXIT_FAILURE;
  }
  if (error_mode) {
    std::printf("PASSED: invalid zone, registration collision, empty release, oversized payload; "
                "no queued messages or leaked credits\n");
  } else {
    std::printf("PASSED: %" PRIu64 " messages; two-slot FIFO, shared mailbox pressure, "
                "seven-word payloads, explicit release, mixed memory loads, ACK counts zero\n", sent);
  }
  return EXIT_SUCCESS;
}
