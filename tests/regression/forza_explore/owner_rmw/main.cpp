#include <vortex2.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <vector>
#include <unistd.h>
#include "common.h"

static uint64_t mix(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}
static void deadline(int) { static const char s[] = "FAILED: owner RMW timeout\n"; (void)!write(2, s, sizeof(s)-1); _exit(1); }
#define CHECK(expr) do { int status = (expr); if (status) { std::fprintf(stderr, "%s: %d\n", #expr, status); return 1; } } while (false)

int main(int argc, char** argv) {
#ifdef OWNER_ACK_STORE
  const char* mode = "ack";
#else
  const char* mode = "plain";
#endif
  uint64_t bytes = 262144, seed = 17;
  uint32_t count = 65536, seconds = 600, verify_device = 0;
  std::string kernel_file = std::string("kernel-") + mode + ".vxbin";
  int option;
  while ((option = getopt(argc, argv, "b:n:s:k:t:v:p:")) != -1) {
    switch (option) {
    case 'b': bytes = std::strtoull(optarg, nullptr, 0); break;
    case 'n': count = std::strtoul(optarg, nullptr, 0); break;
    case 's': seed = std::strtoull(optarg, nullptr, 0); break;
    case 'k': kernel_file = optarg; break;
    case 't': seconds = std::strtoul(optarg, nullptr, 0); break;
    case 'v': verify_device = std::strtoul(optarg, nullptr, 0); break;
    case 'p': if (std::string(optarg) != "permute") return 1; break;
    default: return 1;
    }
  }
  if (bytes < 128 || bytes > UINT64_C(134217728) || (bytes & (bytes-1)) || count > 1048576 || !seconds) return 1;
  std::signal(SIGALRM, deadline); alarm(seconds); std::setvbuf(stdout, nullptr, _IOLBF, 0);
  vx_device_h device;
  vx_queue_h queue;
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
  uint32_t actors = cores * warps * threads;
  uint64_t words = bytes / 8, lines = bytes / 64;
  if (!actors || words % actors) return 1;
  std::vector<uint32_t> permutation(lines);
  for (uint32_t i = 0; i < lines; ++i) permutation[i] = i;
  uint64_t rng = seed;
  for (uint64_t i = lines; i > 1; --i) { rng = mix(rng); std::swap(permutation[i-1], permutation[rng % i]); }
  std::vector<std::vector<request_t>> buckets(actors);
  std::vector<int64_t> expected(words, 0), actual(words, 0);
  std::vector<uint64_t> old_checksums(actors, 0), device_checksums(actors, UINT64_C(0xcbf29ce484222325));
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t h = mix(uint64_t(i) ^ mix(seed));
    uint64_t key = uint64_t(permutation[i % lines]) * 8;
    int64_t delta = int64_t((h >> 48) % 15) - 7;
    if (!delta) delta = 1;
    uint32_t owner = key / (words / actors);
    old_checksums[owner] ^= uint64_t(expected[key]);
    expected[key] += delta;
    buckets[owner].push_back(request_t{key, delta});
    hash = (hash ^ key) * UINT64_C(0x100000001b3);
    hash = (hash ^ uint64_t(delta)) * UINT64_C(0x100000001b3);
  }
  std::vector<request_t> requests;
  std::vector<uint32_t> offsets(actors + 1, 0);
  for (uint32_t rank = 0; rank < actors; ++rank) {
    requests.insert(requests.end(), buckets[rank].begin(), buckets[rank].end());
    offsets[rank + 1] = requests.size();
    for (uint64_t i = uint64_t(rank) * (words / actors); i < uint64_t(rank + 1) * (words / actors); ++i) {
      device_checksums[rank] = (device_checksums[rank] ^ uint64_t(expected[i])) * UINT64_C(0x100000001b3);
    }
  }
  args_t args{};
  args.words = words; args.words_per_actor = words / actors;
  args.actors = actors; args.per_core = warps * threads; args.threads = threads; args.verify_device = verify_device;
  std::vector<result_t> results(actors);
  vx_buffer_h input_buffer, offsets_buffer, state_buffer, results_buffer;
  CHECK(vx_buffer_create(device, std::max<size_t>(16, requests.size() * sizeof(request_t)), VX_MEM_READ_WRITE, &input_buffer));
  CHECK(vx_buffer_create(device, bytes, VX_MEM_READ_WRITE, &state_buffer));
  CHECK(vx_buffer_create(device, results.size() * sizeof(result_t), VX_MEM_READ_WRITE, &results_buffer));
  CHECK(vx_buffer_create(device, offsets.size() * 4, VX_MEM_READ_WRITE, &offsets_buffer));
  CHECK(vx_buffer_address(input_buffer, &args.requests));
  CHECK(vx_buffer_address(offsets_buffer, &args.offsets));
  CHECK(vx_buffer_address(state_buffer, &args.state));
  CHECK(vx_buffer_address(results_buffer, &args.results));
  if (count) CHECK(vx_enqueue_write(queue, input_buffer, 0, requests.data(), requests.size() * sizeof(request_t), 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, offsets_buffer, 0, offsets.data(), offsets.size() * 4, 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, state_buffer, 0, actual.data(), bytes, 0, nullptr, nullptr));
  CHECK(vx_enqueue_write(queue, results_buffer, 0, results.data(), results.size() * sizeof(result_t), 0, nullptr, nullptr));
  CHECK(vx_module_load_file(device, kernel_file.c_str(), &module));
  CHECK(vx_module_get_kernel(module, "main", &kernel));
  std::printf("INPUT diagnostic=owner_rmw mode=%s state_bytes=%" PRIu64 " updates=%u actors=%u hash=%016" PRIx64 " device_verify=%u preprocessing=stable_host_owner_buckets_excluded\n", mode, bytes, count, actors, hash, verify_device);
  vx_launch_info_t launch{};
  launch.struct_size = sizeof(launch); launch.kernel = kernel; launch.args_host = &args; launch.args_size = sizeof(args);
  launch.ndim = 1; launch.grid_dim[0] = cores; launch.block_dim[0] = warps * threads;
  CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, nullptr));
  CHECK(vx_enqueue_read(queue, actual.data(), state_buffer, 0, bytes, 0, nullptr, nullptr));
  CHECK(vx_enqueue_read(queue, results.data(), results_buffer, 0, results.size() * sizeof(result_t), 0, nullptr, &completed));
  CHECK(vx_event_wait_value(completed, 1, (uint64_t(seconds) + 5) * UINT64_C(1000000000)));
  uint32_t errors = 0;
  for (uint64_t word = 0; word < words; ++word) {
    if (actual[word] != expected[word]) {
      if (errors < 8) std::fprintf(stderr, "state[%" PRIu64 "]=%" PRId64 " expected=%" PRId64 "\n", word, actual[word], expected[word]);
      ++errors;
    }
  }
  for (uint32_t rank = 0; rank < actors; ++rank) {
    const auto& r = results[rank];
    if (r.rank != rank || r.complete != OWNER_COMPLETE || r.updates != buckets[rank].size() || r.mismatches
        || r.old_checksum != old_checksums[rank] || (verify_device && r.device_checksum != device_checksums[rank])) {
      ++errors;
      std::fprintf(stderr, "worker=%u updates=%" PRIu64 " mismatches=%" PRIu64 " old_checksum=%016" PRIx64 "/%016" PRIx64 " device_checksum=%016" PRIx64 "/%016" PRIx64 "\n", rank, r.updates, r.mismatches, r.old_checksum, old_checksums[rank], r.device_checksum, device_checksums[rank]);
    }
  }
  CHECK(vx_event_release(completed)); CHECK(vx_kernel_release(kernel)); CHECK(vx_module_release(module));
  CHECK(vx_buffer_release(input_buffer)); CHECK(vx_buffer_release(offsets_buffer)); CHECK(vx_buffer_release(state_buffer)); CHECK(vx_buffer_release(results_buffer));
  CHECK(vx_queue_release(queue)); CHECK(vx_device_release(device)); alarm(0);
  if (errors) { std::fprintf(stderr, "FAILED: %u owner-RMW validation errors\n", errors); return 1; }
  std::printf("PASSED: no FORZA, every state word, owner update counts, pre-update checksum%s; mode=%s hash=%016" PRIx64 "\n", verify_device ? ", guest final-state checksum" : "", mode, hash);
}
