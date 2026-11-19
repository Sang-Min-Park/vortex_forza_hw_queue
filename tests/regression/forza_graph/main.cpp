#include <vortex2.h>
#include <VX_types.h>
#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include "common.h"

static vx_device_h device = nullptr;
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h completed = nullptr;
static std::vector<vx_buffer_h> buffers;

static void deadline_expired(int) {
  static const char message[] = "FAILED: graph benchmark exceeded its 180s deadline\n";
  ssize_t count = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)count;
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
    for (auto buffer : buffers) {
      vx_buffer_release(buffer);
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

static uint32_t random_word(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static uint32_t zipf_vertex(uint32_t& state, const std::vector<double>& cdf) {
  double sample = double(random_word(state)) / 4294967296.0;
  return std::lower_bound(cdf.begin(), cdf.end(), sample) - cdf.begin();
}

static int create_buffer(uint64_t bytes, vx_buffer_h* buffer, uint64_t* address) {
  int status = vx_buffer_create(device, bytes, VX_MEM_READ_WRITE, buffer);
  if (status == 0) {
    buffers.push_back(*buffer);
    status = vx_buffer_address(*buffer, address);
  }
  return status;
}

int main(int argc, char** argv) {
#if defined(FORZA_GRAPH_GPU_EDGE)
  const char* mode = "gpu_edge";
  const char* kernel_file = "kernel-gpu_edge.vxbin";
#elif defined(FORZA_GRAPH_GPU)
  const char* mode = "gpu_vertex";
  const char* kernel_file = "kernel-gpu.vxbin";
#elif defined(FORZA_GRAPH_SOFTWARE)
  const char* mode = "software";
  const char* kernel_file = "kernel-sw.vxbin";
#else
  const char* mode = "hardware";
  const char* kernel_file = "kernel-hw.vxbin";
#endif
  uint32_t vertices = 64;
  uint32_t edge_count = 256;
  uint32_t seed = 17;
  uint32_t iteration_limit = 200000;
  uint32_t queue_capacity = 16;
  std::string shape = "skew";
  int option;
  while ((option = getopt(argc, argv, "k:v:e:g:s:p:q:h")) != -1) {
    switch (option) {
    case 'k': kernel_file = optarg; break;
    case 'v': vertices = std::strtoul(optarg, nullptr, 0); break;
    case 'e': edge_count = std::strtoul(optarg, nullptr, 0); break;
    case 'g': shape = optarg; break;
    case 's': seed = std::strtoul(optarg, nullptr, 0); break;
    case 'p': iteration_limit = std::strtoul(optarg, nullptr, 0); break;
    case 'q': queue_capacity = std::strtoul(optarg, nullptr, 0); break;
    case 'h':
      std::printf("Usage: %s [-v vertices] [-e edges] [-g regular|skew|hub] "
                  "[-s seed] [-p progress limit] [-q software queue slots] [-k kernel]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (vertices == 0 || vertices > 65536 || edge_count > 1048576 || seed == 0
      || iteration_limit == 0 || queue_capacity < 2 || queue_capacity > 4096
      || (shape != "regular" && shape != "skew" && shape != "hub")) {
    std::fprintf(stderr, "Invalid dimensions, seed, progress limit, or graph shape.\n");
    return EXIT_FAILURE;
  }
  std::signal(SIGALRM, deadline_expired);
  alarm(180);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));
  uint64_t cores, warps, threads;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  uint32_t actors = cores * warps * threads;
  if (actors == 0 || actors > FORZA_GRAPH_MAX_PES) {
    std::fprintf(stderr, "Unsupported actor count: %u\n", actors);
    cleanup();
    return EXIT_FAILURE;
  }

  std::vector<double> cdf(vertices);
  double total = 0;
  for (uint32_t v = 0; v < vertices; ++v) {
    total += 1.0 / std::pow(double(v + 1), 1.25);
    cdf[v] = total;
  }
  for (auto& value : cdf) {
    value /= total;
  }
  cdf.back() = 1.0;
  std::vector<std::vector<graph_edge_t>> adjacency(vertices);
  uint32_t state = seed;
  for (uint32_t e = 0; e < edge_count; ++e) {
    uint32_t source = shape == "skew" ? zipf_vertex(state, cdf) : e % vertices;
    uint32_t destination;
    if (shape == "hub" && random_word(state) % 10 != 0) {
      destination = 0;
    } else {
      destination = shape == "skew" ? zipf_vertex(state, cdf) : random_word(state) % vertices;
    }
    int32_t weight = 1 + random_word(state) % 7;
    adjacency[source].push_back(graph_edge_t{destination, weight});
  }
  std::vector<uint32_t> rows(vertices + 1, 0);
  std::vector<graph_edge_t> edges;
  edges.reserve(edge_count);
  std::vector<int64_t> x(vertices);
  std::vector<int64_t> reference(vertices, 0);
  std::vector<uint32_t> incoming(vertices, 0);
  std::vector<uint64_t> expected_sent(actors, 0), expected_received(actors, 0);
  std::vector<graph_sw_queue_t> software_queues(actors * actors);
  uint32_t maximum_outdegree = 0;
  uint64_t cross_core_edges = 0;
  uint64_t graph_hash = UINT64_C(0xcbf29ce484222325);
  for (uint32_t v = 0; v < vertices; ++v) {
    x[v] = 1 + (v * 17 + 3) % 31;
    maximum_outdegree = std::max(maximum_outdegree, uint32_t(adjacency[v].size()));
    for (const auto& edge : adjacency[v]) {
      edges.push_back(edge);
      reference[edge.destination] += x[v] * edge.weight;
      ++incoming[edge.destination];
      uint32_t producer = v % actors;
      uint32_t consumer = edge.destination % actors;
      ++expected_sent[producer];
      ++expected_received[consumer];
      cross_core_edges += producer / (warps * threads) != consumer / (warps * threads);
      graph_hash = (graph_hash ^ v) * UINT64_C(0x100000001b3);
      graph_hash = (graph_hash ^ edge.destination) * UINT64_C(0x100000001b3);
      graph_hash = (graph_hash ^ uint32_t(edge.weight)) * UINT64_C(0x100000001b3);
    }
    rows[v + 1] = edges.size();
  }
  uint64_t total_slots = 0;
  for (auto& meta : software_queues) {
    meta.slots_capacity = queue_capacity;
    meta.slots_offset = total_slots * 64;
    total_slots += meta.slots_capacity;
  }

  graph_kernel_arg_t args{};
  args.vertices = vertices;
  args.edges = edge_count;
  args.actors = actors;
  args.cores = cores;
  args.actors_per_core = warps * threads;
  args.threads = threads;
  args.owned_stride = (vertices + actors - 1) / actors;
  args.iteration_limit = iteration_limit;
  args.sw_queue_capacity = queue_capacity;
  std::vector<int64_t> y(actors * args.owned_stride, 0);
  std::vector<graph_actor_result_t> results(actors);
  for (uint32_t actor = 0; actor < actors; ++actor) {
    results[actor].expected_received = expected_received[actor];
  }
  vx_buffer_h rows_buffer, edges_buffer, x_buffer, y_buffer, results_buffer;
  RT_CHECK(create_buffer(rows.size() * sizeof(rows[0]), &rows_buffer, &args.rows_addr));
  RT_CHECK(create_buffer(std::max<size_t>(1, edges.size()) * sizeof(graph_edge_t), &edges_buffer, &args.edges_addr));
  RT_CHECK(create_buffer(x.size() * sizeof(x[0]), &x_buffer, &args.x_addr));
  RT_CHECK(create_buffer(y.size() * sizeof(y[0]), &y_buffer, &args.y_addr));
  RT_CHECK(create_buffer(results.size() * sizeof(results[0]), &results_buffer, &args.results_addr));
  RT_CHECK(vx_enqueue_write(queue, rows_buffer, 0, rows.data(), rows.size() * sizeof(rows[0]), 0, nullptr, nullptr));
  if (!edges.empty()) {
    RT_CHECK(vx_enqueue_write(queue, edges_buffer, 0, edges.data(), edges.size() * sizeof(edges[0]), 0, nullptr, nullptr));
  }
  RT_CHECK(vx_enqueue_write(queue, x_buffer, 0, x.data(), x.size() * sizeof(x[0]), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, y_buffer, 0, y.data(), y.size() * sizeof(y[0]), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), results.size() * sizeof(results[0]), 0, nullptr, nullptr));
#ifdef FORZA_GRAPH_SOFTWARE
  vx_buffer_h software_state_buffer, software_slots_buffer;
  RT_CHECK(create_buffer(software_queues.size() * sizeof(software_queues[0]),
                         &software_state_buffer, &args.sw_state_addr));
  RT_CHECK(create_buffer(total_slots * 64, &software_slots_buffer, &args.sw_slots_addr));
  RT_CHECK(vx_enqueue_write(queue, software_state_buffer, 0, software_queues.data(),
                            software_queues.size() * sizeof(software_queues[0]), 0, nullptr, nullptr));
  std::vector<uint8_t> slots(total_slots * 64, 0);
  for (uint32_t pair = 0; pair < actors * actors; ++pair) {
    for (uint32_t slot = 0; slot < queue_capacity; ++slot) {
      uint64_t sequence = slot;
      std::memcpy(slots.data() + (uint64_t(pair) * queue_capacity + slot) * 64,
                   &sequence, sizeof(sequence));
    }
  }
  RT_CHECK(vx_enqueue_write(queue, software_slots_buffer, 0, slots.data(), slots.size(), 0, nullptr, nullptr));
#endif
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  std::printf("GRAPH mode=%s shape=%s vertices=%u edges=%u actors=%u seed=%u hash=%016" PRIx64
              " max_outdegree=%u max_indegree=%u cross_core_edges=%" PRIu64 " software_queue_slots=%u\n",
              mode, shape.c_str(), vertices, edge_count, actors, seed, graph_hash,
              maximum_outdegree, *std::max_element(incoming.begin(), incoming.end()), cross_core_edges,
              queue_capacity);
#ifdef FORZA_GRAPH_GPU
  std::printf("GRAPH output_layout=contiguous completion=global_barrier\n");
#else
  std::printf("GRAPH output_layout=owner_partitioned completion=actor_done\n");
#endif
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = cores;
  launch.block_dim[0] = warps * threads;
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, y.data(), y_buffer, 0, y.size() * sizeof(y[0]), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, results.data(), results_buffer, 0, results.size() * sizeof(results[0]), 0, nullptr, &completed));
  RT_CHECK(vx_event_wait_value(completed, 1, UINT64_C(175000000000)));

  uint32_t errors = 0;
  for (uint32_t v = 0; v < vertices; ++v) {
#ifdef FORZA_GRAPH_GPU
    int64_t actual = y[v];
#else
    int64_t actual = y[(v % actors) * args.owned_stride + v / actors];
#endif
    if (actual != reference[v]) {
      if (errors < 8) {
        std::fprintf(stderr, "vertex=%u expected=%" PRId64 " actual=%" PRId64 "\n", v, reference[v], actual);
      }
      ++errors;
    }
  }
  uint64_t maximum_cycles = 0, minimum_start = UINT64_MAX, maximum_start = 0;
  uint64_t maximum_end = 0, total_received = 0, total_sent = 0, retries = 0;
  uint64_t maximum_data_end = 0;
  for (uint32_t actor = 0; actor < actors; ++actor) {
    const auto& result = results[actor];
    uint64_t expected_updates = expected_sent[actor];
#ifdef FORZA_GRAPH_GPU_EDGE
    expected_updates = actor < edge_count ? (edge_count - 1 - actor) / actors + 1 : 0;
#endif
#ifdef FORZA_GRAPH_GPU
    uint64_t expected_handlers = 0;
    uint32_t expected_done = 0;
#else
    uint64_t expected_handlers = expected_received[actor];
    uint32_t expected_done = actors;
#endif
    if (result.rank != actor || result.complete != FORZA_GRAPH_COMPLETE || result.error != 0
        || result.sent != expected_updates || result.received != expected_handlers
        || result.done_received != expected_done || result.outstanding != 0
        || result.data_end_cycle < result.start_cycle) {
      ++errors;
      std::fprintf(stderr, "actor=%u error=%u complete=%x sent=%" PRIu64 "/%" PRIu64
                          " received=%" PRIu64 "/%" PRIu64 " done=%u outstanding=%" PRIu64
                          " expected=%" PRIu64 " actual=%" PRIu64 "\n", actor, result.error,
                          result.complete, result.sent, expected_updates, result.received,
                          expected_handlers, result.done_received, result.outstanding,
                          result.expected, result.actual);
    }
    maximum_cycles = std::max(maximum_cycles, result.cycles);
    minimum_start = std::min(minimum_start, result.start_cycle);
    maximum_start = std::max(maximum_start, result.start_cycle);
    maximum_end = std::max(maximum_end, result.end_cycle);
    maximum_data_end = std::max(maximum_data_end, result.data_end_cycle);
    total_sent += result.sent;
    total_received += result.received;
    retries += result.retries;
  }
  uint64_t memory_reads = 0, memory_writes = 0, retired = 0, device_cycles = 0;
  int reads_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_READS, 0, &memory_reads);
  int writes_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_WRITES, 0, &memory_writes);
  int retired_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MINSTRET, UINT32_MAX, &retired);
  int cycle_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MCYCLE, 0, &device_cycles);
  cleanup();
#ifdef FORZA_GRAPH_GPU
  const uint64_t expected_total_handlers = 0;
#else
  const uint64_t expected_total_handlers = edge_count;
#endif
  if (errors != 0 || total_sent != edge_count || total_received != expected_total_handlers) {
    std::fprintf(stderr, "FAILED: %u graph or actor errors, sent=%" PRIu64 " received=%" PRIu64 "\n",
                 errors, total_sent, total_received);
    return EXIT_FAILURE;
  }
#ifdef FORZA_GRAPH_GPU
  std::printf("PASSED: exact CPU SpMV reference for every vertex; direct shared-memory "
              "64-bit atomics; updates=%" PRIu64 " handlers=0 DONE=0 completion=global_barrier\n",
              total_sent);
#else
  std::printf("PASSED: exact CPU SpMV reference for every vertex; owner-computes actor partition; "
              "sent=%" PRIu64 " received=%" PRIu64 " DONE=%u outstanding=0\n", total_sent, total_received, actors * actors);
#endif
  std::printf("RESULT mode=%s graph=%s vertices=%u edges=%u seed=%u graph_hash=%016" PRIx64
              " max_actor_cycles=%" PRIu64 " phase_span_cycles=%" PRIu64 " start_skew_cycles=%" PRIu64
              " data_span_cycles=%" PRIu64 " software_queue_slots=%u"
              " retries=%" PRIu64 " edges_per_cycle=%.9f dram_reads_whole_kernel=%" PRIu64
              " dram_writes_whole_kernel=%" PRIu64 " retired_instructions_whole_kernel=%" PRIu64
              " device_cycles_post_readback_including_flush=%" PRIu64
              " counter_status=%d/%d/%d cycle_counter_status=%d\n", mode, shape.c_str(), vertices, edge_count, seed,
              graph_hash, maximum_cycles, maximum_end - minimum_start, maximum_start - minimum_start,
              maximum_data_end - minimum_start, queue_capacity,
              retries, double(edge_count) / double(maximum_cycles), memory_reads, memory_writes,
              retired, device_cycles, reads_status, writes_status, retired_status, cycle_status);
  return EXIT_SUCCESS;
}
