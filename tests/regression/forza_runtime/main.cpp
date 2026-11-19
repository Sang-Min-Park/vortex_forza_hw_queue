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
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h completed = nullptr;

static void deadline_expired(int) {
  static const char message[] = "FAILED: FORZA runtime test exceeded the 125s deadline\n";
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
  case FORZA_RUNTIME_IDENTITY: return "runtime identity";
  case FORZA_RUNTIME_SEND: return "runtime send";
  case FORZA_RUNTIME_TIMEOUT: return "runtime progress timeout";
  case FORZA_RUNTIME_PAYLOAD: return "callback payload";
  case FORZA_RUNTIME_ORDER: return "callback sender order";
  case FORZA_RUNTIME_SOURCE: return "callback source";
  case FORZA_RUNTIME_COMPLETION: return "runtime completion";
  default: return "unknown";
  }
}

int main(int argc, char** argv) {
  const char* kernel_file = "kernel.vxbin";
  uint32_t rounds = 8;
  uint32_t iteration_limit = 200000;
  int option;
  while ((option = getopt(argc, argv, "k:n:p:h")) != -1) {
    switch (option) {
    case 'k': kernel_file = optarg; break;
    case 'n': rounds = std::strtoul(optarg, nullptr, 0); break;
    case 'p': iteration_limit = std::strtoul(optarg, nullptr, 0); break;
    case 'h':
      std::printf("Usage: %s [-k kernel.vxbin] [-n rounds] [-p progress limit]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (rounds == 0 || rounds > 1024 || iteration_limit == 0) {
    std::fprintf(stderr, "Use 1..1024 rounds and a nonzero progress limit.\n");
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
  uint64_t actors = cores * warps * threads;
  if (cores < 2 || warps < 2 || actors > FORZA_RUNTIME_MAX_PES) {
    std::fprintf(stderr, "forza_runtime requires >=2 cores, >=2 warps and <=%u actors.\n",
                 FORZA_RUNTIME_MAX_PES);
    cleanup();
    return EXIT_FAILURE;
  }

  runtime_kernel_arg_t args = {};
  args.num_cores = cores;
  args.num_threads = threads;
  args.actors_per_core = warps * threads;
  args.num_actors = actors;
  args.rounds = rounds;
  args.iteration_limit = iteration_limit;
  std::vector<runtime_actor_result_t> results(args.num_actors);
  uint64_t results_size = results.size() * sizeof(results[0]);
  RT_CHECK(vx_buffer_create(device, results_size, VX_MEM_READ_WRITE, &results_buffer));
  RT_CHECK(vx_buffer_address(results_buffer, &args.results_addr));
  RT_CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), results_size, 0, nullptr, nullptr));
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  std::printf("FORZA runtime: %" PRIu64 " cores, %" PRIu64 " warps, %" PRIu64
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
  uint32_t closed_with_reply_pending = 0;
  uint64_t total_handled = 0;
  uint64_t total_retries = 0;
  for (uint32_t actor = 0; actor < args.num_actors; ++actor) {
    const auto& result = results[actor];
    uint32_t local = actor % args.actors_per_core;
    bool valid = result.done == FORZA_RUNTIME_COMPLETE && result.error == 0
              && result.core == actor / args.actors_per_core
              && result.warp == local / args.num_threads
              && result.lane == local % args.num_threads && result.rank == actor;
    for (uint32_t epoch = 0; epoch < FORZA_RUNTIME_EPOCHS; ++epoch) {
      const auto& stats = result.epoch[epoch];
      total_retries += stats.retries;
      valid = valid && stats.sent == rounds * FORZA_RUNTIME_MAILBOXES;
      for (uint32_t mailbox = 0; mailbox < FORZA_RUNTIME_MAILBOXES; ++mailbox) {
        uint32_t expected_count = epoch == 1 ? (actor == 0 ? rounds * args.num_actors : 0) : rounds;
        uint64_t expected_sum = 0;
        uint32_t source_begin = epoch == 1 ? 0 :
          (epoch == 2 && mailbox == 1 ? (actor + 1) % args.num_actors
                                     : (actor + args.num_actors - 1) % args.num_actors);
        uint32_t source_end = epoch == 1 ? args.num_actors : source_begin + 1;
        if (expected_count != 0) {
          for (uint32_t source = source_begin; source < source_end; ++source) {
            for (uint32_t sequence = 0; sequence < rounds; ++sequence) {
              expected_sum += runtime_cookie(source, actor, sequence, mailbox, epoch);
            }
          }
        }
        valid = valid && stats.handled[mailbox] == expected_count
                      && stats.checksum[mailbox] == expected_sum
                      && stats.done_received[mailbox] == args.num_actors
                      && stats.outstanding[mailbox] == 0;
        total_handled += stats.handled[mailbox];
      }
      if (epoch == 2 && stats.responses_at_close < rounds) {
        ++closed_with_reply_pending;
      }
    }
    if (!valid) {
      ++errors;
      std::fprintf(stderr, "actor %u (core=%u warp=%u lane=%u): %s, phase=%u "
                           "expected=0x%016" PRIx64 " actual=0x%016" PRIx64 " done=0x%x\n",
                   actor, result.core, result.warp, result.lane, error_name(result.error), result.phase,
                   result.expected, result.actual, result.done);
      for (uint32_t epoch = 0; epoch < FORZA_RUNTIME_EPOCHS; ++epoch) {
        const auto& stats = result.epoch[epoch];
        std::fprintf(stderr, " epoch%u sent=%u handled=%u/%u DONE=%u/%u outstanding=%" PRIu64
                             "/%" PRIu64 " retries=%u iterations=%u responses_at_close=%u\n", epoch, stats.sent,
                     stats.handled[0], stats.handled[1], stats.done_received[0], stats.done_received[1],
                     stats.outstanding[0], stats.outstanding[1], stats.retries, stats.iterations, stats.responses_at_close);
      }
    }
  }
  cleanup();
  if (errors != 0 || closed_with_reply_pending == 0) {
    std::fprintf(stderr, "FAILED: %u actor runtime errors\n", errors);
    return EXIT_FAILURE;
  }
  std::printf("PASSED: %" PRIu64 " handler invocations; global-PE ring and fan-in, two mailboxes, "
              "short payloads, per-sender FIFO, software DONE from all peers, ACK counts zero; "
              "%u actors closed before their final handler-generated reply; "
              "backpressure retries=%" PRIu64 "\n",
              total_handled, closed_with_reply_pending, total_retries);
  return EXIT_SUCCESS;
}
