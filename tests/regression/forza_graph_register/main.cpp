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
#if defined(GRAPH_REGISTER_RANGE)
  const char* mode = "gpu_range";
  const char* kernel_file = "kernel-gpu_range.vxbin";
#elif defined(GRAPH_REGISTER_GPU)
  const char* mode = "gpu_cache";
  const char* kernel_file = "kernel-gpu_cache.vxbin";
#elif defined(GRAPH_REGISTER_SW)
  const char* mode = "sw_cache";
  const char* kernel_file = "kernel-sw_cache.vxbin";
#else
  const char* mode = "msg_cache";
  const char* kernel_file = "kernel-msg_cache.vxbin";
#endif
  uint32_t vertices = 64, edge_count = 256, seed = 17, iteration_limit = 200000;
  std::string shape = "skew";
  int option;
  while ((option = getopt(argc, argv, "k:v:e:g:s:p:h")) != -1) {
    switch (option) {
    case 'k': kernel_file = optarg; break;
    case 'v': vertices = std::strtoul(optarg, nullptr, 0); break;
    case 'e': edge_count = std::strtoul(optarg, nullptr, 0); break;
    case 'g': shape = optarg; break;
    case 's': seed = std::strtoul(optarg, nullptr, 0); break;
    case 'p': iteration_limit = std::strtoul(optarg, nullptr, 0); break;
    case 'h':
      std::printf("Usage: %s [-v vertices] [-e edges] [-g regular|skew|hub] "
                  "[-s seed] [-p progress limit] [-k kernel]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (vertices == 0 || vertices > 65536 || edge_count > 1048576 || seed == 0
      || iteration_limit == 0 || (shape != "regular" && shape != "skew" && shape != "hub")) {
    std::fprintf(stderr, "Invalid dimensions, seed, progress limit, or graph shape.\n");
    return EXIT_FAILURE;
  }
  std::signal(SIGALRM, deadline_expired);
  alarm(180);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));
  uint64_t cores, warps, threads, local_capacity;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &local_capacity));
  uint32_t actors = cores * warps * threads;
  if (actors == 0 || actors > GRAPH_MAX_ACTORS) {
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
  for (auto& value : cdf) value /= total;
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
  std::vector<uint32_t> rows(vertices + 1, 0), incoming(vertices, 0);
  std::vector<graph_edge_t> edges;
  edges.reserve(edge_count);
  std::vector<int64_t> x(vertices), reference(vertices, 0);
  std::vector<uint64_t> expected_edges(actors, 0);
  uint32_t maximum_outdegree = 0;
  uint64_t cross_core_edges = 0, graph_hash = UINT64_C(0xcbf29ce484222325);
  for (uint32_t v = 0; v < vertices; ++v) {
    x[v] = 1 + (v * 17 + 3) % 31;
    maximum_outdegree = std::max(maximum_outdegree, uint32_t(adjacency[v].size()));
    for (const auto& edge : adjacency[v]) {
      edges.push_back(edge);
      reference[edge.destination] += x[v] * edge.weight;
      ++incoming[edge.destination];
      graph_hash = (graph_hash ^ v) * UINT64_C(0x100000001b3);
      graph_hash = (graph_hash ^ edge.destination) * UINT64_C(0x100000001b3);
      graph_hash = (graph_hash ^ uint32_t(edge.weight)) * UINT64_C(0x100000001b3);
    }
    rows[v + 1] = edges.size();
  }
  std::vector<uint64_t> expected_flushes(actors, 0);
  for (uint32_t actor = 0; actor < actors; ++actor) {
    uint32_t core = actor / (warps * threads);
    uint32_t local = actor % (warps * threads);
    uint32_t global_warp = (local / threads) * cores + core;
    uint32_t total_warps = cores * warps;
    uint32_t lo = uint64_t(edge_count) * global_warp / total_warps;
    uint32_t hi = uint64_t(edge_count) * (global_warp + 1) / total_warps;
#ifndef GRAPH_REGISTER_RANGE
    uint32_t key0 = UINT32_MAX, key1 = UINT32_MAX;
    bool recent0 = false;
#endif
    for (uint32_t edge = lo + local % threads; edge < hi; edge += threads) {
      ++expected_edges[actor];
      uint32_t key = edges[edge].destination;
      cross_core_edges += core != (key % actors) / (warps * threads);
#ifdef GRAPH_REGISTER_RANGE
      ++expected_flushes[actor];
#else
      if (key == key0) {
        recent0 = true;
      } else if (key == key1) {
        recent0 = false;
      } else if (key0 == UINT32_MAX) {
        key0 = key; recent0 = true;
      } else if (key1 == UINT32_MAX) {
        key1 = key; recent0 = false;
      } else if (recent0) {
        ++expected_flushes[actor]; key1 = key; recent0 = false;
      } else {
        ++expected_flushes[actor]; key0 = key; recent0 = true;
      }
#endif
    }
#ifndef GRAPH_REGISTER_RANGE
    expected_flushes[actor] += (key0 != UINT32_MAX) + (key1 != UINT32_MAX);
#endif
  }
  graph_arg_t args{};
  args.vertices = vertices;
  args.edges = edge_count;
  args.actors = actors;
  args.cores = cores;
  args.actors_per_core = warps * threads;
  args.threads = threads;
  args.owned_stride = (vertices + actors - 1) / actors;
  args.iteration_limit = iteration_limit;
#if defined(GRAPH_REGISTER_GPU)
  uint32_t partial_rows = 0;
#else
  uint32_t partial_rows = 1;
#endif
  args.local_bytes = uint64_t(partial_rows) * actors * args.owned_stride * 8;
#ifdef GRAPH_REGISTER_SW
  args.global_vectors_bytes = uint64_t(cores) * actors * args.owned_stride * 8;
  uint64_t global_used_bytes = uint64_t(cores) * (actors - args.actors_per_core) * args.owned_stride * 8;
  uint32_t tree_flags_bytes = actors * 8;
#else
  uint64_t global_used_bytes = 0;
  uint32_t tree_flags_bytes = 0;
#endif
#ifdef GRAPH_REGISTER_GPU
  uint32_t local_vectors_applied = 0;
#else
  uint32_t local_vectors_applied = actors;
#endif
  if (args.local_bytes > local_capacity) {
    std::fprintf(stderr, "Mode %s needs %u scratchpad bytes/core, capacity=%" PRIu64 "\n",
                 mode, args.local_bytes, local_capacity);
    cleanup();
    return EXIT_FAILURE;
  }
  std::vector<int64_t> y(actors * args.owned_stride, 0);
  std::vector<graph_result_t> results(actors);
  uint32_t chunks_per_vector = (args.owned_stride + 5) / 6;
  if (chunks_per_vector > 64) {
    std::fprintf(stderr, "The per-core duplicate detector supports at most 64 chunks/vector.\n");
    cleanup();
    return EXIT_FAILURE;
  }
#if !defined(GRAPH_REGISTER_SW) && !defined(GRAPH_REGISTER_GPU)
  for (uint32_t actor = 0; actor < actors; ++actor) {
    results[actor].expected_received_chunks = (cores - 1) * chunks_per_vector;
  }
#endif
  vx_buffer_h rows_buffer, edges_buffer, x_buffer, y_buffer, results_buffer;
  RT_CHECK(create_buffer(rows.size() * 4, &rows_buffer, &args.rows_addr));
  RT_CHECK(create_buffer(std::max<size_t>(1, edges.size()) * sizeof(graph_edge_t), &edges_buffer, &args.edges_addr));
  RT_CHECK(create_buffer(x.size() * 8, &x_buffer, &args.x_addr));
  RT_CHECK(create_buffer(y.size() * 8, &y_buffer, &args.y_addr));
  RT_CHECK(create_buffer(results.size() * sizeof(graph_result_t), &results_buffer, &args.results_addr));
  RT_CHECK(vx_enqueue_write(queue, rows_buffer, 0, rows.data(), rows.size() * 4, 0, nullptr, nullptr));
  if (!edges.empty()) {
    RT_CHECK(vx_enqueue_write(queue, edges_buffer, 0, edges.data(), edges.size() * sizeof(graph_edge_t), 0, nullptr, nullptr));
  }
  RT_CHECK(vx_enqueue_write(queue, x_buffer, 0, x.data(), x.size() * 8, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, y_buffer, 0, y.data(), y.size() * 8, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), results.size() * sizeof(graph_result_t), 0, nullptr, nullptr));
#ifdef GRAPH_REGISTER_SW
  vx_buffer_h vectors_buffer, arrived_buffer, released_buffer;
  RT_CHECK(create_buffer(args.global_vectors_bytes, &vectors_buffer, &args.vectors_addr));
  RT_CHECK(create_buffer(actors * 4, &arrived_buffer, &args.arrived_addr));
  RT_CHECK(create_buffer(actors * 4, &released_buffer, &args.released_addr));
  std::vector<uint8_t> zero_vectors(args.global_vectors_bytes, 0);
  std::vector<uint32_t> zero_flags(actors, 0);
  RT_CHECK(vx_enqueue_write(queue, vectors_buffer, 0, zero_vectors.data(), zero_vectors.size(), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, arrived_buffer, 0, zero_flags.data(), actors * 4, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, released_buffer, 0, zero_flags.data(), actors * 4, 0, nullptr, nullptr));
#endif
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  std::printf("GRAPH mode=%s shape=%s vertices=%u edges=%u actors=%u seed=%u hash=%016" PRIx64
              " max_outdegree=%u max_indegree=%u cross_core_edges=%" PRIu64 " work_mapping=balanced_edge_ranges\n",
              mode, shape.c_str(), vertices, edge_count, actors, seed, graph_hash,
              maximum_outdegree, *std::max_element(incoming.begin(), incoming.end()), cross_core_edges);
  std::printf("MEMORY global_vectors_bytes=%" PRIu64 " global_active_vectors_bytes=%" PRIu64
              " local_bytes_per_core=%u local_capacity_per_core=%" PRIu64
              " tree_flags_bytes=%u owned_stride=%u partial_rows_per_core=%u one_shot_host_reset_excluded=1\n",
              args.global_vectors_bytes, global_used_bytes, args.local_bytes, local_capacity,
              tree_flags_bytes, args.owned_stride, partial_rows);
#ifdef GRAPH_REGISTER_GPU
  std::printf("GRAPH output_layout=contiguous completion=global_barrier\n");
#else
  std::printf("GRAPH output_layout=owner_partitioned completion=known_remote_counts_or_tree\n");
#endif
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = cores;
  launch.block_dim[0] = warps * threads;
  launch.lmem_size = args.local_bytes;
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, y.data(), y_buffer, 0, y.size() * 8, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, results.data(), results_buffer, 0, results.size() * sizeof(graph_result_t), 0, nullptr, &completed));
  RT_CHECK(vx_event_wait_value(completed, 1, UINT64_C(175000000000)));

  uint32_t errors = 0;
  for (uint32_t v = 0; v < vertices; ++v) {
#ifdef GRAPH_REGISTER_GPU
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
  uint64_t minimum_start = UINT64_MAX, maximum_start = 0, maximum_end = 0;
  uint64_t max_cycles = 0, maximum_data_end = 0, total_edges = 0;
  uint64_t total_flushes = 0, expected_total_flushes = 0;
  uint64_t total_vectors_published = 0, total_vectors_reduced = 0;
  uint64_t total_chunks_sent = 0, total_chunks_received = 0;
  uint64_t max_init = 0, max_fold = 0, max_sync = 0, max_reduce = 0;
  for (uint32_t actor = 0; actor < actors; ++actor) {
    const auto& r = results[actor];
    if (r.atomic_flushes != expected_flushes[actor]) {
      std::fprintf(stderr, "worker=%u atomic_flushes=%" PRIu64 "/%" PRIu64 "\n",
                   actor, r.atomic_flushes, expected_flushes[actor]);
    }
#ifdef GRAPH_REGISTER_GPU
    uint64_t expected_published = 0, expected_reduced = 0;
    uint64_t expected_chunks_sent = 0, expected_chunks_received = 0;
#else
    uint32_t local = actor % args.actors_per_core;
    uint64_t expected_published = (actors - 1 - local) / args.actors_per_core;
    uint64_t expected_reduced = cores;
#ifdef GRAPH_REGISTER_SW
    uint64_t expected_chunks_sent = 0, expected_chunks_received = 0;
#else
    uint64_t expected_chunks_sent = expected_published * chunks_per_vector;
    uint64_t expected_chunks_received = (cores - 1) * chunks_per_vector;
#endif
#endif
    uint32_t expected_done = 0;
    if (r.rank != actor || r.complete != GRAPH_COMPLETE || r.error != 0
        || r.edges_folded != expected_edges[actor] || r.atomic_flushes != expected_flushes[actor]
        || r.vectors_published != expected_published || r.vectors_reduced != expected_reduced
        || r.chunks_sent != expected_chunks_sent || r.chunks_received != expected_chunks_received
        || r.done_received != expected_done || r.outstanding != 0
        || r.data_end_cycle < r.start_cycle || r.end_cycle < r.data_end_cycle) {
      ++errors;
      std::fprintf(stderr, "worker=%u error=%u complete=%x edges=%" PRIu64 "/%" PRIu64
                          " vectors_published=%" PRIu64 "/%" PRIu64 " vectors_reduced=%" PRIu64 "/%" PRIu64
                          " chunks=%" PRIu64 "/%" PRIu64 " expected_chunks=%" PRIu64 "/%" PRIu64
                          " done=%u outstanding=%" PRIu64 " expected=%" PRIu64 " actual=%" PRIu64 "\n",
                          actor, r.error, r.complete, r.edges_folded, expected_edges[actor],
                          r.vectors_published, expected_published, r.vectors_reduced, expected_reduced,
                          r.chunks_sent, r.chunks_received, expected_chunks_sent, expected_chunks_received,
                          r.done_received, r.outstanding, r.expected, r.actual);
    }
    minimum_start = std::min(minimum_start, r.start_cycle);
    maximum_start = std::max(maximum_start, r.start_cycle);
    maximum_end = std::max(maximum_end, r.end_cycle);
    maximum_data_end = std::max(maximum_data_end, r.data_end_cycle);
    max_cycles = std::max(max_cycles, r.cycles);
    max_init = std::max(max_init, r.local_init_cycles);
    max_fold = std::max(max_fold, r.fold_cycles);
    max_sync = std::max(max_sync, r.publish_sync_cycles);
    max_reduce = std::max(max_reduce, r.reduce_cycles);
    total_edges += r.edges_folded;
    total_flushes += r.atomic_flushes;
    expected_total_flushes += expected_flushes[actor];
    total_vectors_published += r.vectors_published;
    total_vectors_reduced += r.vectors_reduced;
    total_chunks_sent += r.chunks_sent;
    total_chunks_received += r.chunks_received;
  }
  uint64_t memory_reads = 0, memory_writes = 0, retired = 0, device_cycles = 0;
  int reads_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_READS, 0, &memory_reads);
  int writes_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_WRITES, 0, &memory_writes);
  int retired_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MINSTRET, UINT32_MAX, &retired);
  int cycle_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MCYCLE, 0, &device_cycles);
  cleanup();
  if (errors != 0 || total_edges != edge_count) {
    std::fprintf(stderr, "FAILED: %u graph or worker errors, updates=%" PRIu64 "\n", errors, total_edges);
    return EXIT_FAILURE;
  }
  std::printf("PASSED: exact CPU SpMV reference for every vertex; int64 weighted scatter; "
              "updates=%" PRIu64 " published_vectors=%" PRIu64 " reduced_vectors=%" PRIu64 "\n",
              total_edges, total_vectors_published, total_vectors_reduced);
  std::printf("RESULT mode=%s graph=%s vertices=%u edges=%u seed=%u graph_hash=%016" PRIx64
              " input_edges=%u atomic_flushes=%" PRIu64 " expected_atomic_flushes=%" PRIu64
              " max_actor_cycles=%" PRIu64 " phase_span_cycles=%" PRIu64 " start_skew_cycles=%" PRIu64
              " data_span_cycles=%" PRIu64 " max_local_init_cycles=%" PRIu64 " max_fold_cycles=%" PRIu64
              " max_publish_sync_cycles=%" PRIu64 " max_reduce_cycles=%" PRIu64
              " vectors_published=%" PRIu64 " vectors_reduced=%" PRIu64
              " chunks_sent=%" PRIu64 " chunks_received=%" PRIu64
              " global_vectors_bytes=%" PRIu64 " global_active_vectors_bytes=%" PRIu64
              " local_bytes_per_core=%u tree_flags_bytes=%u local_vectors_applied=%u partial_rows_per_core=%u"
              " dram_reads_whole_kernel=%" PRIu64 " dram_writes_whole_kernel=%" PRIu64
              " retired_instructions_whole_kernel=%" PRIu64
              " device_cycles_post_readback_including_flush=%" PRIu64
              " counter_status=%d/%d/%d cycle_counter_status=%d\n",
              mode, shape.c_str(), vertices, edge_count, seed, graph_hash,
              edge_count, total_flushes, expected_total_flushes, max_cycles, maximum_end - minimum_start, maximum_start - minimum_start,
              maximum_data_end - minimum_start, max_init, max_fold, max_sync, max_reduce,
              total_vectors_published, total_vectors_reduced, total_chunks_sent, total_chunks_received,
              args.global_vectors_bytes, global_used_bytes, args.local_bytes, tree_flags_bytes, local_vectors_applied, partial_rows,
              memory_reads, memory_writes, retired, device_cycles,
              reads_status, writes_status, retired_status, cycle_status);
  return EXIT_SUCCESS;
}

