#include <vortex2.h>
#include <VX_types.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <csignal>
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
  for (auto b : buffers) vx_buffer_release(b);
  if (device) vx_device_release(device);
  alarm(0);
}
static void deadline(int) { static const char s[] = "FAILED: large_state deadline\n"; (void)!write(2, s, sizeof(s)-1); _exit(1); }
#define CHECK(expr) do { int status = (expr); if (status) { std::fprintf(stderr, "%s: %d\n", #expr, status); cleanup(); return 1; } } while (false)
static int allocate(uint64_t bytes, vx_buffer_h* b, uint64_t* address) {
  int status = vx_buffer_create(device, bytes, VX_MEM_READ_WRITE, b);
  if (!status) { buffers.push_back(*b); status = vx_buffer_address(*b, address); }
  return status;
}
static uint64_t mix(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

int main(int argc, char** argv) {
#ifdef LARGE_MESSAGE
#ifdef LARGE_BATCH
  const char* mode = "msg_batch";
#elif defined(LARGE_RAW)
  const char* mode = "msg_raw_balanced_ordered";
#elif defined(LARGE_FULL_MESSAGE)
  const char* mode = "msg_full";
#elif defined(LARGE_NO_CACHE)
  const char* mode = "msg_plain";
#else
  const char* mode = "msg";
#endif
#elif defined(LARGE_GPU_PLAIN)
  const char* mode = "gpu";
#else
  const char* mode = "gpu_cache";
#endif
#ifdef LARGE_COMPACT
  const std::string compact_mode = std::string(mode) + "_compact";
  mode = compact_mode.c_str();
#endif
  uint64_t bytes = 2097152, seed = 17;
  uint32_t count = 4096, seconds = 600;
  std::string pattern = "random", kernel_file = std::string("kernel-") + mode + ".vxbin";
  int option;
  while ((option = getopt(argc, argv, "b:n:p:s:k:t:h")) != -1) {
    switch (option) {
      case 'b': bytes = std::strtoull(optarg, nullptr, 0); break;
      case 'n': count = std::strtoul(optarg, nullptr, 0); break;
      case 'p': pattern = optarg; break;
      case 's': seed = std::strtoull(optarg, nullptr, 0); break;
      case 'k': kernel_file = optarg; break;
      case 't': seconds = std::strtoul(optarg, nullptr, 0); break;
      case 'h': std::printf("-b state_bytes -n total_updates -p random|skew|reuse|permute -s seed -t seconds -k kernel\n"); return 0;
      default: return 1;
    }
  }
  if (bytes < 128 || bytes > UINT64_C(134217728) || (bytes & (bytes-1)) || count > 1048576
      || seconds == 0 || seconds > 3600 || (pattern != "random" && pattern != "skew" && pattern != "reuse" && pattern != "permute")) return 1;
  std::signal(SIGALRM, deadline); alarm(seconds); std::setvbuf(stdout, nullptr, _IOLBF, 0);
  CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi{sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  CHECK(vx_queue_create(device, &qi, &queue));
  uint64_t cores, warps, threads;
  CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &cores));
  CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  uint32_t actors = cores * warps * threads;
  uint64_t words = bytes / 8, lines = bytes / 64;
  if (actors == 0 || actors > 128 || words % actors) { cleanup(); return 1; }
  uint32_t scratch_bytes = 0;
#ifdef LARGE_BATCH
  scratch_bytes = uint64_t(warps * threads) * actors * 56;
  uint64_t scratch_capacity;
  CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &scratch_capacity));
  if (scratch_bytes > scratch_capacity) { cleanup(); return 1; }
#endif
  std::vector<uint32_t> permutation;
  if (pattern == "permute") {
    permutation.resize(lines);
    for (uint32_t i = 0; i < lines; ++i) permutation[i] = i;
    uint64_t rng = seed;
    for (uint64_t i = lines; i > 1; --i) { rng = mix(rng); std::swap(permutation[i-1], permutation[rng % i]); }
  }
  std::vector<request_t> requests(count);
  std::vector<int64_t> expected(words, 0), actual(words, 0);
  std::vector<uint8_t> touched(lines, 0);
  uint64_t hash = UINT64_C(0xcbf29ce484222325), distinct = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t h = mix(uint64_t(i) ^ mix(seed));
    uint64_t key = h & (words-1);
    if (pattern == "skew" && h % 10 != 0) key = mix(h) & (std::max<uint64_t>(1, words/128)-1);
    if (pattern == "reuse") {
      uint64_t round = i / actors, producer = i % actors;
      key = mix((round/8) * actors + producer + (round%8 < 6 ? 0 : UINT64_C(0x517cc1b727220a95)) + seed) & (words-1);
    }
    if (pattern == "permute") key = uint64_t(permutation[i % lines]) * 8;
    int64_t delta = int64_t((h >> 48) % 15) - 7;
    if (delta == 0) delta = 1;
    requests[i] = {static_cast<request_key_t>(key), static_cast<request_delta_t>(delta)};
    expected[key] += delta;
    if (!touched[key/8]) { touched[key/8] = 1; ++distinct; }
    hash = (hash ^ key) * UINT64_C(0x100000001b3);
    hash = (hash ^ uint64_t(delta)) * UINT64_C(0x100000001b3);
  }
  std::vector<uint64_t> want_inputs(actors), want_flushes(actors), want_sent(actors), want_local(actors), want_receive(actors);
  for (uint32_t actor = 0; actor < actors; ++actor) {
    [[maybe_unused]] cache_t c;
    auto emit = [&](uint64_t key, int64_t) {
      ++want_flushes[actor];
      uint32_t owner = key / (words/actors);
      if (owner == actor) ++want_local[actor];
      else { ++want_sent[actor]; ++want_receive[owner]; }
    };
    for (uint32_t i = actor; i < count; i += actors) {
      request_t r = requests[i]; ++want_inputs[actor];
#ifdef LARGE_NO_CACHE
      emit(r.key, r.delta);
#else
      if (r.key == c.key0) { c.sum0 += r.delta; c.recent0 = true; }
      else if (r.key == c.key1) { c.sum1 += r.delta; c.recent0 = false; }
      else if (c.key0 == UINT64_MAX) { c.key0 = r.key; c.sum0 = r.delta; c.recent0 = true; }
      else if (c.key1 == UINT64_MAX) { c.key1 = r.key; c.sum1 = r.delta; c.recent0 = false; }
      else if (c.recent0) { emit(c.key1,c.sum1); c.key1=r.key;c.sum1=r.delta;c.recent0=false; }
      else { emit(c.key0,c.sum0);c.key0=r.key;c.sum0=r.delta;c.recent0=true; }
#endif
    }
#ifndef LARGE_NO_CACHE
    if (c.key0 != UINT64_MAX) emit(c.key0,c.sum0);
    if (c.key1 != UINT64_MAX) emit(c.key1,c.sum1);
#endif
  }
  args_t arg{};
  arg.words=words;arg.words_per_actor=words/actors;arg.updates=count;arg.actors=actors;
  arg.per_core=warps*threads;arg.threads=threads;arg.iteration_limit=2000000;
  std::vector<result_t> results(actors);
#ifdef LARGE_MESSAGE
  for (uint32_t i=0;i<actors;++i) results[i].expected_receive=want_receive[i];
#endif
  vx_buffer_h input_buffer,state_buffer,result_buffer;
  CHECK(allocate(std::max<uint64_t>(16, count*sizeof(request_t)), &input_buffer, &arg.requests_addr));
  CHECK(allocate(bytes,&state_buffer,&arg.state_addr));
  CHECK(allocate(results.size()*sizeof(result_t),&result_buffer,&arg.results_addr));
  if (count) CHECK(vx_enqueue_write(queue,input_buffer,0,requests.data(),count*sizeof(request_t),0,nullptr,nullptr));
  CHECK(vx_enqueue_write(queue,state_buffer,0,actual.data(),bytes,0,nullptr,nullptr));
  CHECK(vx_enqueue_write(queue,result_buffer,0,results.data(),results.size()*sizeof(result_t),0,nullptr,nullptr));
  CHECK(vx_module_load_file(device,kernel_file.c_str(),&module_));
  CHECK(vx_module_get_kernel(module_,"main",&kernel));
  std::printf("INPUT mode=%s pattern=%s updates=%u actors=%u state_bytes=%" PRIu64 " input_bytes=%zu distinct_state_lines=%" PRIu64 " touched_line_bytes=%" PRIu64 " repeated_full_line_passes=%" PRIu64 " hash=%016" PRIx64 " completion=known_one_shot_counts\n",mode,pattern.c_str(),count,actors,bytes,requests.size()*sizeof(request_t),distinct,distinct*64,pattern=="permute"?count/lines:0,hash);
  vx_launch_info_t launch{};launch.struct_size=sizeof(launch);launch.kernel=kernel;launch.args_host=&arg;launch.args_size=sizeof(arg);
  launch.ndim=1;launch.grid_dim[0]=cores;launch.block_dim[0]=warps*threads;
  launch.lmem_size=scratch_bytes;
  CHECK(vx_enqueue_launch(queue,&launch,0,nullptr,nullptr));
  CHECK(vx_enqueue_read(queue,actual.data(),state_buffer,0,bytes,0,nullptr,nullptr));
  CHECK(vx_enqueue_read(queue,results.data(),result_buffer,0,results.size()*sizeof(result_t),0,nullptr,&event_));
  CHECK(vx_event_wait_value(event_,1,(uint64_t(seconds)+5)*UINT64_C(1000000000)));
  uint32_t errors=0;uint64_t checksum=UINT64_C(0xcbf29ce484222325);
  for(uint64_t i=0;i<words;++i){checksum=(checksum^uint64_t(actual[i]))*UINT64_C(0x100000001b3);if(actual[i]!=expected[i]){if(errors<8)std::fprintf(stderr,"state[%" PRIu64 "]=%" PRId64 " expected=%" PRId64 "\n",i,actual[i],expected[i]);++errors;}}
  uint64_t earliest=UINT64_MAX,latest=0,flushes=0,sent=0,received=0,locals=0,attempts=0,retries=0;
  for(uint32_t i=0;i<actors;++i){const auto&r=results[i];bool bad=r.rank!=i||r.complete!=LARGE_COMPLETE||r.error||r.inputs!=want_inputs[i]||r.flushes!=want_flushes[i]||r.outstanding||r.end<r.start;
#ifdef LARGE_MESSAGE
    bad=bad||r.sent!=want_sent[i]||r.local!=want_local[i]||r.received!=want_receive[i];
#else
    bad=bad||r.sent||r.local||r.received;
#endif
    if(bad){++errors;std::fprintf(stderr,"worker=%u error=%" PRIu64 " inputs=%" PRIu64 "/%" PRIu64 " flushes=%" PRIu64 "/%" PRIu64 " sent=%" PRIu64 " received=%" PRIu64 " outstanding=%" PRIu64 "\n",i,r.error,r.inputs,want_inputs[i],r.flushes,want_flushes[i],r.sent,r.received,r.outstanding);}
    earliest=std::min(earliest,r.start);latest=std::max(latest,r.end);flushes+=r.flushes;sent+=r.sent;received+=r.received;locals+=r.local;attempts+=r.attempts;retries+=r.retries;
  }
  auto counter=[&](uint32_t cls,uint32_t csr,uint32_t core){uint64_t v=0;int s=vx_device_mpm_query(device,cls,csr,core,&v);if(s){++errors;std::fprintf(stderr,"counter %x failed %d\n",csr,s);}return v;};
  uint64_t cycles=counter(VX_DCR_MPM_CLASS_CORE,VX_CSR_MCYCLE,0);
  uint64_t dram_r=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_READS,0),dram_w=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_WRITES,0);
  uint64_t l2r=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_READS,0),l2w=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_WRITES,0);
  uint64_t l2mr=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_R,0),l2mw=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_W,0),l2ev=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_EVICTS,0);
  uint64_t l1r=0,l1w=0,l1mr=0,l1mw=0;
  for(uint32_t c=0;c<cores;++c){l1r+=counter(VX_DCR_MPM_CLASS_DCACHE,VX_CSR_MPM_DCACHE_READS,c);l1w+=counter(VX_DCR_MPM_CLASS_DCACHE,VX_CSR_MPM_DCACHE_WRITES,c);l1mr+=counter(VX_DCR_MPM_CLASS_DCACHE,VX_CSR_MPM_DCACHE_MISS_R,c);l1mw+=counter(VX_DCR_MPM_CLASS_DCACHE,VX_CSR_MPM_DCACHE_MISS_W,c);}
  cleanup();
  if(errors){std::fprintf(stderr,"FAILED: %u validation errors\n",errors);return 1;}
  std::printf("PASSED: every state word and worker input/flush/receive count; checksum=%016" PRIx64 "\n",checksum);
#ifdef LARGE_FULL_MESSAGE
  const uint64_t payload_words = 7;
#else
  const uint64_t payload_words = (sizeof(request_t) + 7) / 8;
#endif
  uint64_t accepted_words = sent * payload_words;
#ifdef LARGE_BATCH
  accepted_words = sent + attempts - retries;
#endif
  std::printf("RESULT mode=%s pattern=%s state_bytes=%" PRIu64 " updates=%u hash=%016" PRIx64 " distinct_state_lines=%" PRIu64 " touched_line_bytes=%" PRIu64 " phase_cycles=%" PRIu64 " device_cycles_post_readback_including_flush=%" PRIu64 " atomic_or_message_flushes=%" PRIu64 " remote_sent=%" PRIu64 " remote_received=%" PRIu64 " local_applied=%" PRIu64 " send_attempts=%" PRIu64 " send_retries=%" PRIu64 " accepted_payload_words_derived=%" PRIu64 " physical_message_bytes=64 dram_reads=%" PRIu64 " dram_writes=%" PRIu64 " l2_reads=%" PRIu64 " l2_writes=%" PRIu64 " l2_read_misses=%" PRIu64 " l2_write_misses=%" PRIu64 " l2_evictions=%" PRIu64 " l1_reads=%" PRIu64 " l1_writes=%" PRIu64 " l1_read_misses=%" PRIu64 " l1_write_misses=%" PRIu64 " counter_window=post_readback_including_flush\n",mode,pattern.c_str(),bytes,count,hash,distinct,distinct*64,latest-earliest,cycles,flushes,sent,received,locals,attempts,retries,accepted_words,dram_r,dram_w,l2r,l2w,l2mr,l2mw,l2ev,l1r,l1w,l1mr,l1mw);
  return 0;
}
