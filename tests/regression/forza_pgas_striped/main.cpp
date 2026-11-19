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

static vx_device_h device=nullptr;static vx_queue_h queue=nullptr;
static vx_module_h module_=nullptr;static vx_kernel_h kernel=nullptr;static vx_event_h event_=nullptr;
static std::vector<vx_buffer_h> buffers;
static void cleanup(){if(event_)vx_event_release(event_);if(kernel)vx_kernel_release(kernel);if(module_)vx_module_release(module_);if(queue)vx_queue_release(queue);for(auto b:buffers)vx_buffer_release(b);if(device)vx_device_release(device);alarm(0);}
static void deadline(int){static const char s[]="FAILED: PGAS deadline\n";(void)!write(2,s,sizeof(s)-1);_exit(1);}
#define CHECK(e) do{int s=(e);if(s){std::fprintf(stderr,"%s: %d\n",#e,s);cleanup();return 1;}}while(false)
static int allocate(uint64_t bytes,vx_buffer_h* b,uint64_t* addr){int s=vx_buffer_create(device,bytes,VX_MEM_READ_WRITE,b);if(!s){buffers.push_back(*b);s=vx_buffer_address(*b,addr);}return s;}
static uint64_t mix(uint64_t x){x+=UINT64_C(0x9e3779b97f4a7c15);x=(x^(x>>30))*UINT64_C(0xbf58476d1ce4e5b9);x=(x^(x>>27))*UINT64_C(0x94d049bb133111eb);return x^(x>>31);}

int main(int argc,char** argv){
#ifdef PGAS_HW
#ifdef PGAS_TRANSPORT_MMIO
  const char* transport="mmio";
#else
  const char* transport="csr";
#endif
#ifdef PGAS_STATE_GLOBAL
  const char* placement="global";
#else
  const char* placement="local";
#endif
  const std::string label=std::string("hw_owner_")+transport+"_"+placement;
#elif defined(PGAS_SW)
  const char* transport="software_spsc",*placement="local";const std::string label="sw_owner";
#elif defined(PGAS_GPU_LOCAL)
  const char* transport="native",*placement="local";const std::string label="gpu_local";
#else
  const char* transport="native",*placement="global";const std::string label="gpu_global";
#endif
  uint64_t bytes=32768,seed=17;uint32_t updates=4096,capacity=4,seconds=600;
  std::string pattern="random",kernel_file="kernel-forza_pgas_striped_"+label+".vxbin";
  int opt;while((opt=getopt(argc,argv,"b:n:p:s:q:t:k:h"))!=-1){switch(opt){case'b':bytes=std::strtoull(optarg,nullptr,0);break;case'n':updates=std::strtoul(optarg,nullptr,0);break;case'p':pattern=optarg;break;case's':seed=std::strtoull(optarg,nullptr,0);break;case'q':capacity=std::strtoul(optarg,nullptr,0);break;case't':seconds=std::strtoul(optarg,nullptr,0);break;case'k':kernel_file=optarg;break;case'h':std::puts("-b state_bytes -n updates -p random|skew|reuse|permute -s seed -q queue_capacity -t seconds -k kernel");return 0;default:return 1;}}
  if(bytes<128||bytes>UINT64_C(16777216)||(bytes&(bytes-1))||updates>1048576||capacity<2||capacity>64||(capacity&(capacity-1))||!seconds||seconds>3600||(pattern!="random"&&pattern!="skew"&&pattern!="reuse"&&pattern!="permute"))return 1;
  std::signal(SIGALRM,deadline);alarm(seconds);std::setvbuf(stdout,nullptr,_IOLBF,0);
  CHECK(vx_device_open(0,&device));vx_queue_info_t qi{sizeof(qi),nullptr,VX_QUEUE_PRIORITY_NORMAL,0};CHECK(vx_queue_create(device,&qi,&queue));
  uint64_t cores,warps,threads,lmem_capacity;CHECK(vx_device_query(device,VX_CAPS_NUM_CORES,&cores));CHECK(vx_device_query(device,VX_CAPS_NUM_WARPS,&warps));CHECK(vx_device_query(device,VX_CAPS_NUM_THREADS,&threads));CHECK(vx_device_query(device,VX_CAPS_LOCAL_MEM_SIZE,&lmem_capacity));
  uint32_t actors=cores*warps*threads,per_core=warps*threads,words=bytes/8;
  if(!actors||actors>PGAS_MAX_ACTORS||words%actors||(actors&(actors-1))||(per_core&(per_core-1))){std::fprintf(stderr,"Unsupported PGAS geometry\n");cleanup();return 1;}
  uint32_t owned=words/actors,resident=std::min<uint64_t>(owned,lmem_capacity/(per_core*8));
#if defined(PGAS_STATE_GLOBAL)||defined(PGAS_GPU_GLOBAL)
  resident=0;
#endif
  uint32_t local_bytes=per_core*resident*8;
  std::vector<uint32_t> permutation;if(pattern=="permute"){permutation.resize(bytes/64);for(uint32_t i=0;i<permutation.size();++i)permutation[i]=i;uint64_t rng=seed;for(size_t i=permutation.size();i>1;--i){rng=mix(rng);std::swap(permutation[i-1],permutation[rng%i]);}}
  std::vector<request_t> input(updates);std::vector<int64_t> expected(words,0),actual(words,0);
  std::vector<uint64_t> want_inputs(actors),want_direct(actors),want_sent(actors),want_receive(actors),want_lamo(actors),want_gamo(actors);
  std::vector<uint64_t> want_channels(uint64_t(actors)*actors);
  std::vector<uint8_t> touched(bytes/64,0);uint64_t hash=UINT64_C(0xcbf29ce484222325),distinct=0,resident_updates=0,spill_updates=0;
  for(uint32_t i=0;i<updates;++i){uint64_t h=mix(uint64_t(i)^mix(seed));uint32_t key=h&(words-1);if(pattern=="skew"&&h%10!=0)key=mix(h)&(std::max<uint32_t>(1,words/128)-1);if(pattern=="reuse"){uint64_t round=i/actors,producer=i%actors;key=mix((round/8)*actors+producer+(round%8<6?0:UINT64_C(0x517cc1b727220a95))+seed)&(words-1);}if(pattern=="permute")key=permutation[i%permutation.size()]*8;int32_t delta=int32_t((h>>48)%15)-7;if(!delta)delta=1;input[i]={key,delta};expected[key]+=delta;hash=(hash^key)*UINT64_C(0x100000001b3);hash=(hash^uint64_t(int64_t(delta)))*UINT64_C(0x100000001b3);if(!touched[key/8]){touched[key/8]=1;++distinct;}
    uint32_t worker=i%actors,owner=key&(actors-1),offset=key/actors;++want_inputs[worker];if(owner==worker)++want_direct[worker];else{++want_sent[worker];++want_receive[owner];++want_channels[uint64_t(worker)*actors+owner];}
    if(offset<resident)++resident_updates;else ++spill_updates;
    if(owner/per_core==worker/per_core&&offset<resident)++want_lamo[worker];else ++want_gamo[worker];
  }
  args_t arg{};arg.words=words;arg.words_per_actor=owned;arg.local_words_per_actor=resident;arg.local_bytes=local_bytes;arg.updates=updates;arg.actors=actors;arg.per_core=per_core;arg.threads=threads;arg.queue_capacity=capacity;arg.iteration_limit=2000000;
  std::vector<result_t> result(actors);
#if defined(PGAS_HW)||defined(PGAS_SW)
  for(uint32_t i=0;i<actors;++i)result[i].expected_receive=want_receive[i];
#endif
  vx_buffer_h ib,sb,rb;CHECK(allocate(std::max<uint64_t>(8,uint64_t(updates)*8),&ib,&arg.requests_addr));CHECK(allocate(bytes,&sb,&arg.state_addr));CHECK(allocate(result.size()*sizeof(result_t),&rb,&arg.results_addr));
  if(updates){CHECK(vx_enqueue_write(queue,ib,0,input.data(),input.size()*8,0,nullptr,nullptr));}
  CHECK(vx_enqueue_write(queue,sb,0,actual.data(),bytes,0,nullptr,nullptr));CHECK(vx_enqueue_write(queue,rb,0,result.data(),result.size()*sizeof(result_t),0,nullptr,nullptr));
  uint64_t software_queue_bytes=0;
#ifdef PGAS_SW
  std::vector<queue_slot_t> slots(uint64_t(actors)*actors*capacity);std::vector<uint8_t> mapped(slots.size(),0);
  for(uint32_t source=0;source<actors;++source)for(uint32_t destination=0;destination<actors;++destination)for(uint32_t j=0;j<capacity;++j){uint64_t index=pgas_queue_slot_index(source,destination,j,actors,capacity);if(index>=slots.size()||mapped[index]){std::fprintf(stderr,"Invalid software queue mapping\n");cleanup();return 1;}mapped[index]=1;slots[index].sequence=j;}
  software_queue_bytes=slots.size()*sizeof(queue_slot_t);vx_buffer_h qb;CHECK(allocate(software_queue_bytes,&qb,&arg.queues_addr));CHECK(vx_enqueue_write(queue,qb,0,slots.data(),software_queue_bytes,0,nullptr,nullptr));
#endif
  CHECK(vx_module_load_file(device,kernel_file.c_str(),&module_));CHECK(vx_module_get_kernel(module_,"main",&kernel));
  uint64_t min_owner_work=UINT64_MAX,max_owner_work=0;
  for(uint32_t i=0;i<actors;++i){uint64_t work=want_receive[i]+want_direct[i];min_owner_work=std::min(min_owner_work,work);max_owner_work=std::max(max_owner_work,work);}
  std::printf("MAPPING owner_mapping=striped min_owner_updates=%" PRIu64 " max_owner_updates=%" PRIu64 " min_owner_incoming=%" PRIu64 " max_owner_incoming=%" PRIu64 "\n",min_owner_work,max_owner_work,*std::min_element(want_receive.begin(),want_receive.end()),*std::max_element(want_receive.begin(),want_receive.end()));
  uint64_t remote_partial_bytes=0;
#ifdef PGAS_GPU_LOCAL
  remote_partial_bytes=uint64_t(local_bytes)*cores;
#endif
  std::printf("MEMORY global_state_allocation_bytes=%" PRIu64 " global_remote_partial_bytes=%" PRIu64 " result_bytes=%zu\n",bytes,remote_partial_bytes,result.size()*sizeof(result_t));
  std::printf("INPUT owner_mapping=striped mode=%s transport=%s placement=%s algorithm=weighted_in_degree graph_vertices=%u graph_edges=%u source_streams=%u state_bytes=%" PRIu64 " actors=%u cores=%" PRIu64 " owner_words=%u lmem_capacity_per_core=%" PRIu64 " lmem_state_bytes_per_core=%u resident_state_bytes_total=%" PRIu64 " dram_backing_state_bytes=%" PRIu64 " resident_target_updates=%" PRIu64 " spill_target_updates=%" PRIu64 " software_queue_bytes=%" PRIu64 " software_cursor_logical_bytes=%u hw_mailbox_sram_bytes_per_core=%u input_bytes=%zu distinct_state_sectors=%" PRIu64 " touched_sector_bytes=%" PRIu64 " hash=%016" PRIx64 " pgas=owner_offset_active_update host_reset_excluded=1 completion=known_one_shot_counts\n",label.c_str(),transport,placement,words,updates,actors,bytes,actors,cores,owned,lmem_capacity,local_bytes,uint64_t(local_bytes)*cores,bytes-uint64_t(local_bytes)*cores,resident_updates,spill_updates,software_queue_bytes,
#ifdef PGAS_SW
  actors*PGAS_MAX_ACTORS*16,
#else
  0u,
#endif
#ifdef PGAS_HW
  per_core*8*2*64,
#else
  0u,
#endif
  input.size()*sizeof(request_t),distinct,distinct*64,hash);
  vx_launch_info_t launch{};launch.struct_size=sizeof(launch);launch.kernel=kernel;launch.args_host=&arg;launch.args_size=sizeof(arg);launch.ndim=1;launch.grid_dim[0]=cores;launch.block_dim[0]=per_core;launch.lmem_size=local_bytes;
  CHECK(vx_enqueue_launch(queue,&launch,0,nullptr,nullptr));CHECK(vx_enqueue_read(queue,actual.data(),sb,0,bytes,0,nullptr,nullptr));
#ifdef PGAS_SW
  CHECK(vx_enqueue_read(queue,slots.data(),qb,0,software_queue_bytes,0,nullptr,nullptr));
#endif
  CHECK(vx_enqueue_read(queue,result.data(),rb,0,result.size()*sizeof(result_t),0,nullptr,&event_));CHECK(vx_event_wait_value(event_,1,(uint64_t(seconds)+5)*UINT64_C(1000000000)));
  uint32_t errors=0;uint64_t checksum=UINT64_C(0xcbf29ce484222325);for(uint32_t i=0;i<words;++i){checksum=(checksum^uint64_t(actual[i]))*UINT64_C(0x100000001b3);if(actual[i]!=expected[i]){if(errors<8)std::fprintf(stderr,"state[%u]=%" PRId64 " expected=%" PRId64 "\n",i,actual[i],expected[i]);++errors;}}
#ifdef PGAS_SW
  for(uint32_t source=0;source<actors;++source)for(uint32_t destination=0;destination<actors;++destination)for(uint32_t j=0;j<capacity;++j){uint64_t channel=uint64_t(source)*actors+destination,count=want_channels[channel],seq=j;if(count>j)seq=j+((count-1-j)/capacity+1)*capacity;uint64_t index=pgas_queue_slot_index(source,destination,j,actors,capacity);if(slots[index].sequence!=seq){if(errors<8)std::fprintf(stderr,"queue channel=%" PRIu64 " slot=%u sequence=%" PRIu64 " expected=%" PRIu64 "\n",channel,j,slots[index].sequence,seq);++errors;}}
#endif
  uint64_t sent=0,received=0,direct=0,attempts=0,retries=0,polls=0,lamo=0,gamo=0,start=UINT64_MAX,init_min=UINT64_MAX,init_max=0,process_min=UINT64_MAX,process_max=0,end=0;
  for(uint32_t i=0;i<actors;++i){auto&r=result[i];bool bad=r.rank!=i||r.inputs!=want_inputs[i]||r.error||r.outstanding||r.complete!=PGAS_COMPLETE||r.start>r.initialized||r.initialized>r.processed||r.processed>r.end;
#if defined(PGAS_HW)||defined(PGAS_SW)
    bad=bad||r.sent!=want_sent[i]||r.direct!=want_direct[i]||r.received!=want_receive[i]||r.local_amo||r.global_amo;
#elif defined(PGAS_GPU_LOCAL)
    bad=bad||r.local_amo!=want_lamo[i]||r.global_amo!=want_gamo[i]||r.sent||r.received;
#else
    bad=bad||r.local_amo||r.global_amo!=want_inputs[i]||r.sent||r.received;
#endif
    if(bad){++errors;std::fprintf(stderr,"actor=%u error=%" PRIu64 " inputs=%" PRIu64 "/%" PRIu64 " sent=%" PRIu64 " recv=%" PRIu64 "/%" PRIu64 " localamo=%" PRIu64 " globalamo=%" PRIu64 "\n",i,r.error,r.inputs,want_inputs[i],r.sent,r.received,want_receive[i],r.local_amo,r.global_amo);}sent+=r.sent;received+=r.received;direct+=r.direct;attempts+=r.attempts;retries+=r.retries;polls+=r.polls;lamo+=r.local_amo;gamo+=r.global_amo;start=std::min(start,r.start);init_min=std::min(init_min,r.initialized);init_max=std::max(init_max,r.initialized);process_min=std::min(process_min,r.processed);process_max=std::max(process_max,r.processed);end=std::max(end,r.end);}
  auto counter=[&](uint32_t cls,uint32_t csr,uint32_t core){uint64_t x=0;int s=vx_device_mpm_query(device,cls,csr,core,&x);if(s){++errors;std::fprintf(stderr,"counter%x failed%d\n",csr,s);}return x;};
  uint64_t cycles=counter(VX_DCR_MPM_CLASS_CORE,VX_CSR_MCYCLE,0),dr=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_READS,0),dw=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_WRITES,0),l2r=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_READS,0),l2w=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_WRITES,0),l2mr=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_R,0),l2mw=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_W,0);
  uint64_t lmread=0,lmwrite=0,lmstalls=0;for(uint32_t c=0;c<cores;++c){lmread+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_READS,c);lmwrite+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_WRITES,c);lmstalls+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_BANK_ST,c);}
  cleanup();if(errors){std::fprintf(stderr,"FAILED: %u exact validation errors\n",errors);return 1;}
  std::printf("PASSED: all state words and peractor work/transport/AMO counts checksum=%016" PRIx64 "\n",checksum);
  std::printf("RESULT owner_mapping=striped mode=%s transport=%s placement=%s state_bytes=%" PRIu64 " updates=%u hash=%016" PRIx64 " device_cycles_post_readback_including_flush=%" PRIu64 " phase_cycles=%" PRIu64 " init_span_cycles=%" PRIu64 " process_span_cycles=%" PRIu64 " output_span_cycles=%" PRIu64 " sent=%" PRIu64 " received=%" PRIu64 " direct=%" PRIu64 " send_attempts=%" PRIu64 " send_retries=%" PRIu64 " receive_polls=%" PRIu64 " local_amo_updates=%" PRIu64 " global_amo_updates=%" PRIu64 " lmem_state_bytes_per_core=%u dram_spill_bytes=%" PRIu64 " software_queue_bytes=%" PRIu64 " lmem_reads=%" PRIu64 " lmem_writes=%" PRIu64 " lmem_bank_stalls=%" PRIu64 " dram_reads=%" PRIu64 " dram_writes=%" PRIu64 " l2_reads=%" PRIu64 " l2_writes=%" PRIu64 " l2_read_misses=%" PRIu64 " l2_write_misses=%" PRIu64 " counter_window=whole_kernel_and_flush\n",label.c_str(),transport,placement,bytes,updates,hash,cycles,end-start,init_max-start,process_max-init_min,end-process_min,sent,received,direct,attempts,retries,polls,lamo,gamo,local_bytes,bytes-uint64_t(local_bytes)*cores,software_queue_bytes,lmread,lmwrite,lmstalls,dr,dw,l2r,l2w,l2mr,l2mw);
  return 0;
}
