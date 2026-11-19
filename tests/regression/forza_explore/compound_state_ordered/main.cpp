#include <vortex2.h>
#include <VX_types.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include "common.h"

static vx_device_h device = nullptr;
static vx_queue_h queue = nullptr;
static vx_module_h module_ = nullptr;
static vx_kernel_h kernel = nullptr;
static vx_event_h completed = nullptr;
static vx_event_h routed = nullptr;
static std::vector<vx_buffer_h> buffers;

static void deadline_expired(int) {
  static const char message[] = "FAILED: compound-state deadline exceeded\n";
  ssize_t count = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)count;
  _exit(EXIT_FAILURE);
}

static void cleanup() {
  if (device != nullptr) {
    if (completed != nullptr) vx_event_release(completed);
    if (routed != nullptr) vx_event_release(routed);
    if (kernel != nullptr) vx_kernel_release(kernel);
    if (module_ != nullptr) vx_module_release(module_);
    if (queue != nullptr) vx_queue_release(queue);
    for (auto buffer : buffers) vx_buffer_release(buffer);
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

static int create_buffer(uint64_t bytes, vx_buffer_h* buffer, uint64_t* address) {
  int status = vx_buffer_create(device, std::max<uint64_t>(bytes, 64), VX_MEM_READ_WRITE, buffer);
  if (status == 0) {
    buffers.push_back(*buffer);
    status = vx_buffer_address(*buffer, address);
  }
  return status;
}

static uint32_t random_word(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

int main(int argc, char** argv) {
  const std::string mode = COMPOUND_MODE;
  std::string kernel_file = "kernel-" + mode + ".vxbin";
  uint32_t keys = 64, updates = 128, seed = 17, active_keys = 256;
  uint32_t iteration_limit = 10000000, seconds = 600;
  std::string shape = "uniform";
  int option;
  while ((option = getopt(argc, argv, "k:n:u:g:s:a:p:t:h")) != -1) {
    switch (option) {
    case 'k': kernel_file = optarg; break;
    case 'n': keys = std::strtoul(optarg, nullptr, 0); break;
    case 'u': updates = std::strtoul(optarg, nullptr, 0); break;
    case 'g': shape = optarg; break;
    case 's': seed = std::strtoul(optarg, nullptr, 0); break;
    case 'a': active_keys = std::strtoul(optarg, nullptr, 0); break;
    case 'p': iteration_limit = std::strtoul(optarg, nullptr, 0); break;
    case 't': seconds = std::strtoul(optarg, nullptr, 0); break;
    case 'h':
      std::printf("Usage: %s [-n keys] [-u requests] [-g uniform|hot|hotset] [-s seed] "
                  "[-a hotset keys] [-p progress limit] [-t deadline seconds] [-k kernel]\n", argv[0]);
      return EXIT_SUCCESS;
    default: return EXIT_FAILURE;
    }
  }
  if (keys == 0 || keys > 1048576 || updates > 1048576 || seed == 0 || active_keys == 0
      || iteration_limit == 0 || seconds == 0
      || (shape != "uniform" && shape != "hot" && shape != "hotset")) {
    std::fprintf(stderr, "Invalid dimensions, distribution or limits.\n");
    return EXIT_FAILURE;
  }
  std::signal(SIGALRM, deadline_expired);
  alarm(seconds);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));
  uint64_t cores, warps, threads;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  uint32_t actors = cores * warps * threads;
  if (actors == 0 || actors > COMPOUND_MAX_ACTORS) {
    std::fprintf(stderr, "Unsupported actor count: %u\n", actors);
    cleanup();
    return EXIT_FAILURE;
  }
  std::vector<compound_request_t> requests(updates);
  std::vector<uint64_t> expected_count(keys, 0), expected_sum(keys, 0), expected_xor(keys, 0);
  std::vector<uint64_t> incoming(actors, 0), expected_sent(actors, 0), expected_received(actors, 0);
  std::vector<uint64_t> channel_counts(uint64_t(actors) * actors, 0);
  std::unordered_set<uint32_t> touched_keys;
  uint32_t rng = seed;
  uint64_t input_hash = UINT64_C(1469598103934665603), cross_core = 0;
  auto hash_word = [&](uint64_t word) {
    for (uint32_t byte = 0; byte < 8; ++byte) {
      input_hash ^= uint8_t(word >> (8 * byte));
      input_hash *= UINT64_C(1099511628211);
    }
  };
  for (uint32_t producer = 0; producer < actors; ++producer) {
    uint32_t lo = uint64_t(updates) * producer / actors;
    uint32_t hi = uint64_t(updates) * (producer + 1) / actors;
    for (uint32_t id = lo; id < hi; ++id) {
      uint32_t key;
      if (shape == "hot") key = random_word(rng) % 10 != 0 ? 0 : random_word(rng) % keys;
      else key = random_word(rng) % (shape == "hotset" ? std::min(keys, active_keys) : keys);
      uint64_t delta = 1 + random_word(rng) % 31;
      requests[id] = {key, id, delta};
      hash_word((uint64_t(id) << 32) | key);
      hash_word(delta);
      ++expected_count[key];
      expected_sum[key] += delta;
      expected_xor[key] ^= compound_token(id);
      touched_keys.insert(key);
      uint32_t owner = key % actors;
      ++incoming[owner];
      ++channel_counts[uint64_t(producer) * actors + owner];
      if (owner != producer) {
        ++expected_sent[producer];
        ++expected_received[owner];
      }
      cross_core += owner / (warps * threads) != producer / (warps * threads);
    }
  }
  std::vector<compound_record_t> records(keys);
  std::vector<compound_response_t> responses(updates);
  for (auto& response : responses) response.old_count = UINT64_MAX;
  std::vector<compound_result_t> results(uint64_t(actors) * 2);
  compound_arg_t args{};
  args.keys = keys;
  args.requests = updates;
  args.actors = actors;
  args.cores = cores;
  args.actors_per_core = warps * threads;
  args.threads = threads;
  args.iteration_limit = iteration_limit;
  args.bucket_capacity = (updates + actors - 1) / actors;
  uint64_t input_bytes = uint64_t(updates) * sizeof(compound_request_t);
  uint64_t response_bytes = uint64_t(updates) * sizeof(compound_response_t);
  uint64_t state_bytes = mode == "gpu_soa" ? uint64_t(keys) * 28 : uint64_t(keys) * sizeof(compound_record_t);
  uint64_t bucket_bytes = mode == "gpu_owner"
      ? uint64_t(actors) * actors * args.bucket_capacity * sizeof(compound_request_t) : 0;
  vx_buffer_h request_buffer, record_buffer = nullptr, response_buffer, result_buffer, incoming_buffer;
  vx_buffer_h buckets_buffer = nullptr, counts_buffer = nullptr;
  vx_buffer_h soa_count_buffer = nullptr, soa_sum_buffer = nullptr, soa_xor_buffer = nullptr, soa_lock_buffer = nullptr;
  RT_CHECK(create_buffer(input_bytes, &request_buffer, &args.requests_addr));
  RT_CHECK(create_buffer(response_bytes, &response_buffer, &args.responses_addr));
  RT_CHECK(create_buffer(results.size() * sizeof(results[0]), &result_buffer, &args.results_addr));
  RT_CHECK(create_buffer(incoming.size() * 8, &incoming_buffer, &args.incoming_addr));
  if (mode == "gpu_soa") {
    RT_CHECK(create_buffer(uint64_t(keys) * 8, &soa_count_buffer, &args.soa_count_addr));
    RT_CHECK(create_buffer(uint64_t(keys) * 8, &soa_sum_buffer, &args.soa_sum_addr));
    RT_CHECK(create_buffer(uint64_t(keys) * 8, &soa_xor_buffer, &args.soa_xor_addr));
    RT_CHECK(create_buffer(uint64_t(keys) * 4, &soa_lock_buffer, &args.soa_locks_addr));
    std::vector<uint64_t> zero64(keys, 0);
    std::vector<uint32_t> zero32(keys, 0);
    RT_CHECK(vx_enqueue_write(queue, soa_count_buffer, 0, zero64.data(), zero64.size() * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_enqueue_write(queue, soa_sum_buffer, 0, zero64.data(), zero64.size() * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_enqueue_write(queue, soa_xor_buffer, 0, zero64.data(), zero64.size() * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_enqueue_write(queue, soa_lock_buffer, 0, zero32.data(), zero32.size() * 4, 0, nullptr, nullptr));
  } else {
    RT_CHECK(create_buffer(uint64_t(keys) * sizeof(records[0]), &record_buffer, &args.records_addr));
    RT_CHECK(vx_enqueue_write(queue, record_buffer, 0, records.data(), records.size() * sizeof(records[0]), 0, nullptr, nullptr));
  }
  if (mode == "gpu_owner") {
    RT_CHECK(create_buffer(bucket_bytes, &buckets_buffer, &args.buckets_addr));
    RT_CHECK(create_buffer(channel_counts.size() * 8, &counts_buffer, &args.bucket_counts_addr));
    std::vector<uint64_t> zeros(channel_counts.size(), 0);
    RT_CHECK(vx_enqueue_write(queue, counts_buffer, 0, zeros.data(), zeros.size() * 8, 0, nullptr, nullptr));
  }
  if (updates != 0) {
    RT_CHECK(vx_enqueue_write(queue, request_buffer, 0, requests.data(), input_bytes, 0, nullptr, nullptr));
    RT_CHECK(vx_enqueue_write(queue, response_buffer, 0, responses.data(), response_bytes, 0, nullptr, nullptr));
  }
  RT_CHECK(vx_enqueue_write(queue, result_buffer, 0, results.data(), results.size() * sizeof(results[0]), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, incoming_buffer, 0, incoming.data(), incoming.size() * 8, 0, nullptr, nullptr));
  std::unordered_set<uint64_t> state_lines;
  for (uint32_t key : touched_keys) {
    if (mode == "gpu_soa") {
      state_lines.insert((args.soa_count_addr + uint64_t(key) * 8) / 64);
      state_lines.insert((args.soa_sum_addr + uint64_t(key) * 8) / 64);
      state_lines.insert((args.soa_xor_addr + uint64_t(key) * 8) / 64);
      state_lines.insert((args.soa_locks_addr + uint64_t(key) * 4) / 64);
    } else state_lines.insert((args.records_addr + uint64_t(key) * 64) / 64);
  }
  uint64_t bucket_touched_lines = 0;
  if (mode == "gpu_owner") {
    std::unordered_set<uint64_t> lines;
    for (uint64_t channel = 0; channel < channel_counts.size(); ++channel) {
      for (uint64_t entry = 0; entry < channel_counts[channel]; ++entry) {
        lines.insert((args.buckets_addr + (channel * args.bucket_capacity + entry) * 16) / 64);
      }
    }
    bucket_touched_lines = lines.size();
  }
  std::printf("WORKLOAD mode=%s keys=%u requests=%u shape=%s seed=%u hash=%016" PRIx64
              " actors=%u cross_core_requests=%" PRIu64 " touched_keys=%zu\n",
              mode.c_str(), keys, updates, shape.c_str(), seed, input_hash, actors, cross_core, touched_keys.size());
  std::printf("MEMORY state_alloc_bytes=%" PRIu64 " state_touched_sector_bytes=%zu input_bytes=%" PRIu64
              " response_bytes=%" PRIu64 " bucket_alloc_bytes=%" PRIu64 " bucket_touched_sector_bytes=%" PRIu64
              " bucket_counts_bytes=%zu record_bytes=%zu response_record_bytes=%zu\n",
              state_bytes, state_lines.size() * 64, input_bytes, response_bytes, bucket_bytes,
              bucket_touched_lines * 64, mode == "gpu_owner" ? channel_counts.size() * 8 : 0,
              mode == "gpu_soa" ? size_t(28) : sizeof(compound_record_t), sizeof(compound_response_t));
  RT_CHECK(vx_module_load_file(device, kernel_file.c_str(), &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = cores;
  launch.block_dim[0] = warps * threads;
  uint64_t route_reads = 0, route_writes = 0;
  int route_reads_status = 0, route_writes_status = 0;
  uint64_t result_base = args.results_addr;
  if (mode == "gpu_owner") {
    args.stage = 0;
    RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, &routed));
    RT_CHECK(vx_event_wait_value(routed, 1, uint64_t(seconds) * UINT64_C(1000000000)));
    route_reads_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_READS, 0, &route_reads);
    route_writes_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_WRITES, 0, &route_writes);
    args.stage = 1;
    args.results_addr = result_base + actors * sizeof(compound_result_t);
  }
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  if (updates != 0) RT_CHECK(vx_enqueue_read(queue, responses.data(), response_buffer, 0, response_bytes, 0, nullptr, nullptr));
  if (mode == "gpu_soa") {
    std::vector<uint64_t> values(keys);
    std::vector<uint32_t> locks(keys);
    RT_CHECK(vx_enqueue_read(queue, values.data(), soa_count_buffer, 0, uint64_t(keys) * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_queue_finish(queue, uint64_t(seconds) * UINT64_C(1000000000)));
    for (uint32_t key = 0; key < keys; ++key) records[key].count = values[key];
    RT_CHECK(vx_enqueue_read(queue, values.data(), soa_sum_buffer, 0, uint64_t(keys) * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_queue_finish(queue, uint64_t(seconds) * UINT64_C(1000000000)));
    for (uint32_t key = 0; key < keys; ++key) records[key].sum = values[key];
    RT_CHECK(vx_enqueue_read(queue, values.data(), soa_xor_buffer, 0, uint64_t(keys) * 8, 0, nullptr, nullptr));
    RT_CHECK(vx_enqueue_read(queue, locks.data(), soa_lock_buffer, 0, uint64_t(keys) * 4, 0, nullptr, nullptr));
    RT_CHECK(vx_queue_finish(queue, uint64_t(seconds) * UINT64_C(1000000000)));
    for (uint32_t key = 0; key < keys; ++key) {
      records[key].xor_value = values[key];
      records[key].lock = locks[key];
    }
  } else RT_CHECK(vx_enqueue_read(queue, records.data(), record_buffer, 0, records.size() * sizeof(records[0]), 0, nullptr, nullptr));
  std::vector<uint64_t> actual_channels(channel_counts.size());
  if (mode == "gpu_owner") RT_CHECK(vx_enqueue_read(queue, actual_channels.data(), counts_buffer, 0, actual_channels.size() * 8, 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_read(queue, results.data(), result_buffer, 0, results.size() * sizeof(results[0]), 0, nullptr, &completed));
  RT_CHECK(vx_event_wait_value(completed, 1, uint64_t(seconds) * UINT64_C(1000000000)));
  uint64_t reads = 0, writes = 0, instructions = 0, cycles = 0;
  int reads_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_READS, 0, &reads);
  int writes_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_MEM_WRITES, 0, &writes);
  int instruction_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MINSTRET, UINT32_MAX, &instructions);
  int cycle_status = vx_device_mpm_query(device, VX_DCR_MPM_CLASS_CORE, VX_CSR_MCYCLE, 0, &cycles);
  reads += route_reads;
  writes += route_writes;
  uint64_t errors = 0;
  for (uint32_t key = 0; key < keys; ++key) {
    const auto& record = records[key];
    if (record.count != expected_count[key] || record.sum != expected_sum[key]
        || record.xor_value != expected_xor[key] || record.lock != 0) {
      if (errors < 8) std::fprintf(stderr, "record key=%u count=%" PRIu64 "/%" PRIu64
          " sum=%" PRIu64 "/%" PRIu64 " xor=%016" PRIx64 "/%016" PRIx64 " lock=%u\n",
          key, record.count, expected_count[key], record.sum, expected_sum[key],
          record.xor_value, expected_xor[key], record.lock);
      ++errors;
    }
  }
  std::vector<uint32_t> order(updates);
  for (uint32_t id = 0; id < updates; ++id) order[id] = id;
  std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
    if (requests[left].key != requests[right].key) return requests[left].key < requests[right].key;
    return responses[left].old_count < responses[right].old_count;
  });
  uint32_t previous_key = UINT32_MAX;
  uint64_t ticket = 0, sum = 0, xor_value = 0;
  for (uint32_t id : order) {
    const auto& request = requests[id];
    const auto& response = responses[id];
    if (request.key != previous_key) {
      previous_key = request.key;
      ticket = sum = xor_value = 0;
    }
    if (response.complete != COMPOUND_RESPONSE || response.old_count != ticket
        || response.old_sum != sum || response.old_xor != xor_value) {
      if (errors < 8) std::fprintf(stderr, "snapshot id=%u key=%u ticket=%" PRIu64 "/%" PRIu64
          " sum=%" PRIu64 "/%" PRIu64 " xor=%016" PRIx64 "/%016" PRIx64 " complete=%x\n",
          id, request.key, response.old_count, ticket, response.old_sum, sum,
          response.old_xor, xor_value, response.complete);
      ++errors;
    }
    ++ticket;
    sum += request.delta;
    xor_value ^= compound_token(id);
  }
  uint32_t result_offset = mode == "gpu_owner" ? actors : 0;
  uint64_t total_processed = 0, sent = 0, received = 0, retries = 0;
  uint64_t min_start = UINT64_MAX, max_end = 0;
  for (uint32_t rank = 0; rank < actors; ++rank) {
    const auto& result = results[result_offset + rank];
    uint64_t expected = mode == "mp" || mode == "gpu_owner" ? incoming[rank]
        : uint64_t(updates) * (rank + 1) / actors - uint64_t(updates) * rank / actors;
    bool valid = result.rank == rank && result.error == 0 && result.complete == COMPOUND_COMPLETE
              && result.processed == expected && result.outstanding == 0
              && result.end_cycle >= result.start_cycle;
    if (mode == "mp") valid = valid && result.sent == expected_sent[rank] && result.received == expected_received[rank];
    if (mode == "gpu_owner") {
      valid = valid && result.received == incoming[rank];
      const auto& route = results[rank];
      uint64_t route_expected = uint64_t(updates) * (rank + 1) / actors - uint64_t(updates) * rank / actors;
      valid = valid && route.rank == rank && route.error == 0 && route.complete == COMPOUND_COMPLETE
            && route.processed == route_expected && route.sent == route_expected;
    }
    if (!valid) {
      if (errors < 8) std::fprintf(stderr, "worker=%u processed=%" PRIu64 "/%" PRIu64
          " sent=%" PRIu64 " received=%" PRIu64 " error=%u complete=%x\n",
          rank, result.processed, expected, result.sent, result.received, result.error, result.complete);
      ++errors;
    }
    total_processed += result.processed;
    sent += result.sent;
    received += result.received;
    retries += result.retries;
    min_start = std::min(min_start, result.start_cycle);
    max_end = std::max(max_end, result.end_cycle);
  }
  if (mode == "gpu_owner" && actual_channels != channel_counts) ++errors;
  if (total_processed != updates) ++errors;
  cleanup();
  if (errors != 0) {
    std::fprintf(stderr, "FAILED: compound linearizability/final-state/worker errors=%" PRIu64 "\n", errors);
    return EXIT_FAILURE;
  }
  std::printf("PASSED: every old snapshot matches per-key returned-ticket prefix; all final fields/locks/workers valid; "
              "transactions=%u responses=%u\n", updates, updates);
  std::printf("RESULT mode=%s keys=%u requests=%u shape=%s seed=%u input_hash=%016" PRIx64
              " device_cycles_post_readback_including_flush=%" PRIu64
              " final_stage_phase_span_cycles=%" PRIu64 " transactions=%" PRIu64
              " messages_sent=%" PRIu64 " messages_received=%" PRIu64 " retries=%" PRIu64
              " dram_reads_all_stages_post_readback=%" PRIu64 " dram_writes_all_stages_post_readback=%" PRIu64
              " retired_instructions_post_readback=%" PRIu64 " stages=%u"
              " counter_status=%d/%d/%d cycle_counter_status=%d route_counter_status=%d/%d\n",
              mode.c_str(), keys, updates, shape.c_str(), seed, input_hash, cycles, max_end - min_start,
              total_processed, sent, received, retries, reads, writes, instructions, mode == "gpu_owner" ? 2 : 1,
              reads_status, writes_status, instruction_status, cycle_status, route_reads_status, route_writes_status);
  return EXIT_SUCCESS;
}
