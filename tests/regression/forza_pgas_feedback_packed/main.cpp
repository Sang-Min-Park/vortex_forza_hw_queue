#include <vortex2.h>
#include <VX_types.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <numeric>
#include <string>
#include <vector>
#include <unistd.h>
#include "common.h"

static vx_device_h device = nullptr;
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h event_ = nullptr;
static std::vector<vx_buffer_h> buffers;
static void cleanup() {
  if (event_) vx_event_release(event_);
  if (kernel) vx_kernel_release(kernel);
  if (module_) vx_module_release(module_);
  if (queue) vx_queue_release(queue);
  for (auto buffer : buffers) vx_buffer_release(buffer);
  if (device) vx_device_release(device);
  alarm(0);
}
static void deadline(int) {
  static const char message[] = "FAILED: feedback deadline\n";
  (void)!write(2, message, sizeof(message) - 1);
  _exit(1);
}
#define CHECK(expr) do { int status = (expr); if (status) { \
  std::fprintf(stderr, "%s: %d\n", #expr, status); cleanup(); return 1; \
} } while (false)
static int allocate(uint64_t bytes, vx_buffer_h* buffer, uint64_t* address) {
  int status = vx_buffer_create(device, std::max<uint64_t>(bytes, 64),
                               VX_MEM_READ_WRITE, buffer);
  if (!status) { buffers.push_back(*buffer); status = vx_buffer_address(*buffer, address); }
  return status;
}
static uint64_t mix64(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

int main(int argc, char** argv) {
  const std::string mode = FEEDBACK_MODE;
  const bool messaging = mode.rfind("native", 0) != 0;
  const bool replicated = mode == "native_replicated_walk";
  const bool packed = mode == "native_packed";
  const bool software_queue = mode.rfind("gpu_queue", 0) == 0;
  const bool local = mode.find("_local") != std::string::npos;
  std::string kind = "walk", pattern = "random", kernel_file = "kernel-" + mode + ".vxbin";
  uint32_t keys = 64, steps = 64, work = 1, seconds = 600;
  uint64_t seed = 17;
  int option;
  while ((option = getopt(argc, argv, "n:s:w:o:p:r:t:k:h")) != -1) {
    switch (option) {
    case 'n': keys = std::strtoul(optarg, nullptr, 0); break;
    case 's': steps = std::strtoul(optarg, nullptr, 0); break;
    case 'w': work = std::strtoul(optarg, nullptr, 0); break;
    case 'o': kind = optarg; break;
    case 'p': pattern = optarg; break;
    case 'r': seed = std::strtoull(optarg, nullptr, 0); break;
    case 't': seconds = std::strtoul(optarg, nullptr, 0); break;
    case 'k': kernel_file = optarg; break;
    case 'h': std::puts("-n keys -s steps/client -w work -o walk|update -p random|hotset|hot -r seed -k kernel"); return 0;
    default: return 1;
    }
  }
  if (keys < 8 || keys > 1024 || (keys & (keys - 1)) || steps > 4096
      || work < 1 || work > 64 || !seconds || seconds > 3600
      || (kind != "walk" && kind != "update")
      || (pattern != "random" && pattern != "hotset" && pattern != "hot")) return 1;
  if (replicated && kind != "walk") return 77;
  if (packed && kind != "update") return 77;
  std::signal(SIGALRM, deadline); alarm(seconds); std::setvbuf(stdout, nullptr, _IOLBF, 0);
  CHECK(vx_device_open(0, &device));
  vx_queue_info_t info{sizeof(info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  CHECK(vx_queue_create(device, &info, &queue));
  uint64_t cores, warps, threads, capacity;
  CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &capacity));
  if (!cores || cores > 4 || (cores & (cores - 1)) || warps != 2 || threads != 4
      || keys % cores || cores * 4 * steps >= 65536) { cleanup(); return 1; }
  const uint32_t per_core = warps * threads, workers = cores * per_core, clients = cores * 4;
  const uint32_t active_keys = pattern == "hot" ? 1 : pattern == "hotset" ? 8 : keys;
  const uint64_t queries = uint64_t(clients) * steps;
  const uint64_t local_bytes = replicated ? uint64_t(keys) * 16
                             : local ? uint64_t(keys / cores) * 24 : 0;
  if (local_bytes > capacity) { cleanup(); return 77; }
  std::vector<node_t> initial(keys), actual(keys);
  std::vector<trace_t> traces(queries);
  std::vector<result_t> results(workers);
  const uint32_t queue_words = clients * clients + clients * 2;
  std::vector<uint64_t> queue_slots(queue_words), queue_ready(clients);
  uint64_t input_hash = UINT64_C(0xcbf29ce484222325);
  for (uint32_t key = 0; key < keys; ++key) {
    uint64_t random = mix64(key ^ mix64(seed));
    initial[key] = {uint32_t(random) & (active_keys - 1), uint32_t(random >> 32), 0, 0};
    input_hash = (input_hash ^ initial[key].next) * UINT64_C(0x100000001b3);
    input_hash = (input_hash ^ initial[key].value) * UINT64_C(0x100000001b3);
  }
  actual = initial;
  for (auto& trace : traces) trace = {UINT64_MAX, 0};
  args_t args{};
  args.keys = keys; args.active_keys = active_keys; args.steps = steps; args.work = work;
  args.kind = kind == "walk" ? KIND_WALK : KIND_UPDATE; args.cores = cores;
  args.per_core = per_core; args.threads = threads; args.core_shift = __builtin_ctzll(cores);
  args.limit = 2000000;
  vx_buffer_h state_buffer, trace_buffer, result_buffer, queue_buffer, ready_buffer;
  CHECK(allocate(actual.size() * sizeof(node_t), &state_buffer, &args.state));
  CHECK(allocate(traces.size() * sizeof(trace_t), &trace_buffer, &args.traces));
  CHECK(allocate(results.size() * sizeof(result_t), &result_buffer, &args.results));
  CHECK(allocate(queue_slots.size() * 8, &queue_buffer, &args.queue_slots));
  CHECK(allocate(queue_ready.size() * 8, &ready_buffer, &args.queue_ready));
  CHECK(vx_enqueue_write(queue, state_buffer, 0, actual.data(), actual.size() * sizeof(node_t), 0, nullptr, nullptr));
  if (queries) CHECK(vx_enqueue_write(queue, trace_buffer, 0, traces.data(), traces.size() * sizeof(trace_t), 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, result_buffer, 0, results.data(), results.size() * sizeof(result_t), 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, queue_buffer, 0, queue_slots.data(), queue_slots.size() * 8, 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, ready_buffer, 0, queue_ready.data(), queue_ready.size() * 8, 0, nullptr, nullptr));
  CHECK(vx_module_load_file(device, kernel_file.c_str(), &module_));
  CHECK(vx_module_get_kernel(module_, "main", &kernel));
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch); launch.kernel = kernel;
  launch.args_host = &args; launch.args_size = sizeof(args); launch.ndim = 1;
  launch.grid_dim[0] = cores; launch.block_dim[0] = per_core; launch.lmem_size = local_bytes;
  std::printf("INPUT mode=%s handler=%s keys=%u active_keys=%u steps=%u work=%u seed=%" PRIu64
              " input_hash=%016" PRIx64 " clients=%u logical_queries=%" PRIu64
              " cores=%" PRIu64 " warps=%" PRIu64 " threads=%" PRIu64
              " local_state_bytes_per_core=%" PRIu64 " lmem_capacity=%" PRIu64
              " state_location=%s software_queue_bytes=%u completion=client_done_then_coordinator_stop\n",
              mode.c_str(), kind.c_str(), keys, active_keys, steps, work, seed, input_hash,
              clients, queries, cores, warps, threads, local_bytes, capacity,
              replicated ? "replicated_localmem" : local ? "owner_localmem" : "global",
              software_queue ? (queue_words + clients) * 8 : 0);
  CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  CHECK(vx_enqueue_read(queue, actual.data(), state_buffer, 0, actual.size() * sizeof(node_t), 0, nullptr, nullptr));
  if (queries) CHECK(vx_enqueue_read(queue, traces.data(), trace_buffer, 0, traces.size() * sizeof(trace_t), 0, nullptr, nullptr));
  if (software_queue) {
    CHECK(vx_enqueue_read(queue, queue_slots.data(), queue_buffer, 0, queue_slots.size() * 8, 0, nullptr, nullptr));
    CHECK(vx_enqueue_read(queue, queue_ready.data(), ready_buffer, 0, queue_ready.size() * 8, 0, nullptr, nullptr));
  }
  CHECK(vx_enqueue_read(queue, results.data(), result_buffer, 0, results.size() * sizeof(result_t), 0, nullptr, &event_));
  CHECK(vx_event_wait_value(event_, 1, (uint64_t(seconds) + 5) * UINT64_C(1000000000)));
  uint32_t errors = 0;
  auto counter = [&](uint32_t cls, uint32_t csr, uint32_t core) {
    uint64_t value = 0;
    if (vx_device_mpm_query(device, cls, csr, core, &value)) ++errors;
    return value;
  };
  uint64_t cycles = counter(VX_DCR_MPM_CLASS_CORE, VX_CSR_MCYCLE, 0);
  uint64_t l2_reads = counter(VX_DCR_MPM_CLASS_L2CACHE, VX_CSR_MPM_L2CACHE_READS, 0);
  uint64_t l2_writes = counter(VX_DCR_MPM_CLASS_L2CACHE, VX_CSR_MPM_L2CACHE_WRITES, 0);
  uint64_t lmem_reads = 0, lmem_writes = 0;
  for (uint32_t core = 0; core < cores; ++core) {
    lmem_reads += counter(VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_LMEM_READS, core);
    lmem_writes += counter(VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_LMEM_WRITES, core);
  }
  auto fail = [&](const char* message, uint64_t index) {
    if (errors < 12) std::fprintf(stderr, "validation=%s index=%" PRIu64 "\n", message, index);
    ++errors;
  };
  if (software_queue) {
    for (uint32_t i = 0; i < queue_slots.size(); ++i) if (queue_slots[i]) fail("queue_not_drained", i);
    for (uint32_t i = 0; i < queue_ready.size(); ++i) if (queue_ready[i]) fail("doorbell_not_drained", i);
  }
  std::vector<std::vector<uint32_t>> per_key(keys), successors(queries);
  std::vector<uint32_t> indegree(queries), owner_work(workers);
  std::vector<uint64_t> client_digest(clients);
  for (uint32_t client = 0; client < clients; ++client) {
    uint32_t next = initial_key(client, active_keys), cookie = initial_cookie(client);
    for (uint32_t step = 0; step < steps; ++step) {
      uint32_t id = client * steps + step;
      const auto& trace = traces[id];
      uint32_t key = uint32_t(trace.key), recorded_cookie = trace.key >> 32;
      if (key != next || key >= active_keys || recorded_cookie != cookie || !(trace.reply & 1)
          || reply_key(trace.reply) >= active_keys) { fail("client_chain", id); continue; }
      next = reply_key(trace.reply);
      cookie = next_cookie(trace.reply);
      per_key[key].push_back(id);
      uint32_t owner = (key & (cores - 1)) * per_core
                     + ((key >> args.core_shift) & 3);
      ++owner_work[owner];
      client_digest[client] += trace.reply;
      if (step) { successors[id - 1].push_back(id); ++indegree[id]; }
    }
  }
  if (kind == "update") {
    for (auto& ids : per_key) {
      std::sort(ids.begin(), ids.end(), [&](uint32_t left, uint32_t right) {
        return reply_version(traces[left].reply) < reply_version(traces[right].reply);
      });
      for (uint32_t ticket = 0; ticket < ids.size(); ++ticket) {
        if (reply_version(traces[ids[ticket]].reply) != ticket) fail("state_ticket", ids[ticket]);
        if (ticket) { successors[ids[ticket - 1]].push_back(ids[ticket]); ++indegree[ids[ticket]]; }
      }
    }
  }
  std::deque<uint32_t> ready;
  for (uint32_t id = 0; id < queries; ++id) if (!indegree[id]) ready.push_back(id);
  std::vector<node_t> expected = initial;
  uint32_t replayed = 0;
  while (!ready.empty()) {
    uint32_t id = ready.front(); ready.pop_front();
    uint32_t client = id / steps, step = id % steps;
    const auto& trace = traces[id];
    if (uint32_t(trace.key) < keys) {
      uint32_t key = uint32_t(trace.key), cookie = trace.key >> 32;
      uint64_t wanted;
      if (kind == "walk") {
        uint32_t owner = key & (cores - 1), probe = key >> args.core_shift;
        uint32_t value = cookie, link = 0;
        for (uint32_t i = 0; i < work; ++i) {
          const auto& row = initial[(probe << args.core_shift) | owner];
          link = row.next; value = mix32(value ^ uint32_t(row.value) ^ link ^ i);
          probe = (link >> args.core_shift) & ((keys >> args.core_shift) - 1);
        }
        wanted = reply_token((link ^ value) & (active_keys - 1), 0, value);
      } else {
        auto& row = expected[key];
        wanted = reply_token(update_next(row.value, row.version, client, step,
                                         row.next, active_keys, cookie), row.version, row.value);
        row.value = updated_value(row.value, row.version, client, step, row.next, work, cookie);
        ++row.version;
      }
      if (trace.reply != wanted) fail("reply_replay", id);
    }
    ++replayed;
    for (uint32_t next : successors[id]) if (!--indegree[next]) ready.push_back(next);
  }
  if (replayed != queries) fail("causal_history_cycle", replayed);
  uint64_t state_checksum = UINT64_C(0xcbf29ce484222325);
  uint64_t trace_checksum = state_checksum;
  for (uint32_t key = 0; key < keys; ++key) {
    if (actual[key].next != expected[key].next || actual[key].value != expected[key].value
        || actual[key].version != expected[key].version || actual[key].lock) fail("final_state", key);
    state_checksum = (state_checksum ^ actual[key].next) * UINT64_C(0x100000001b3);
    state_checksum = (state_checksum ^ actual[key].value) * UINT64_C(0x100000001b3);
    state_checksum = (state_checksum ^ actual[key].version) * UINT64_C(0x100000001b3);
  }
  for (const auto& trace : traces) {
    trace_checksum = (trace_checksum ^ trace.key) * UINT64_C(0x100000001b3);
    trace_checksum = (trace_checksum ^ trace.reply) * UINT64_C(0x100000001b3);
  }
  uint64_t sent = 0, received = 0, retries = 0, phase_start = UINT64_MAX, phase_end = 0;
  for (uint32_t rank = 0; rank < workers; ++rank) {
    const auto& result = results[rank];
    uint32_t core = rank / per_core, local_rank = rank % per_core;
    bool client_role = local_rank >= 4;
    uint32_t client = core * 4 + local_rank % 4;
    uint64_t processed = client_role ? steps : messaging ? owner_work[rank] : 0;
    uint64_t want_sent = 0, want_received = 0;
    if (messaging) {
      want_sent = client_role ? steps + 1 : owner_work[rank] + (rank == 0 ? clients : 0);
      want_received = client_role ? steps : owner_work[rank] + 1 + (rank == 0 ? clients : 0);
    }
    if (result.rank != rank || result.error || result.outstanding
        || result.complete != FEEDBACK_COMPLETE || result.processed != processed
        || result.sent != want_sent || result.received != want_received
        || (client_role && result.digest != client_digest[client])
        || result.start > result.end) {
      fail("worker", rank);
      std::fprintf(stderr, "worker=%u error=%" PRIu64 " processed=%" PRIu64 "/%" PRIu64
                   " sent=%" PRIu64 "/%" PRIu64 " received=%" PRIu64 "/%" PRIu64
                   " outstanding=%" PRIu64 " complete=%" PRIx64 "\n", rank, result.error,
                   result.processed, processed, result.sent, want_sent, result.received,
                   want_received, result.outstanding, result.complete);
    }
    sent += result.sent; received += result.received; retries += result.retries;
    phase_start = std::min(phase_start, result.start); phase_end = std::max(phase_end, result.end);
  }
  cleanup();
  if (errors) { std::fprintf(stderr, "FAILED: %u exact feedback validation errors\n", errors); return 1; }
  std::printf("PASSED: all client response chains, causal state histories and final records\n");
  std::printf("RESULT mode=%s handler=%s keys=%u active_keys=%u steps=%u work=%u seed=%" PRIu64
              " input_hash=%016" PRIx64 " state_checksum=%016" PRIx64 " trace_checksum=%016" PRIx64
              " clients=%u logical_queries=%" PRIu64 " device_cycles_post_readback_including_flush=%" PRIu64
              " phase_cycles=%" PRIu64 " sent=%" PRIu64 " received=%" PRIu64 " retries=%" PRIu64
              " lmem_reads=%" PRIu64 " lmem_writes=%" PRIu64 " l2_reads=%" PRIu64 " l2_writes=%" PRIu64
              " causal_nodes_replayed=%u logical_handler_iterations=%" PRIu64 "\n",
              mode.c_str(), kind.c_str(), keys, active_keys, steps, work, seed, input_hash,
              state_checksum, trace_checksum, clients, queries, cycles, phase_end - phase_start,
              sent, received, retries, lmem_reads, lmem_writes, l2_reads, l2_writes,
              replayed, queries * work);
  return 0;
}
