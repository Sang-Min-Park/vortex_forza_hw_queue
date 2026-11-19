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

static vx_device_h device=nullptr;static vx_queue_h queue=nullptr;static vx_module_h module_=nullptr;static vx_kernel_h kernel=nullptr;static vx_event_h event_=nullptr;static std::vector<vx_buffer_h> buffers;
static void cleanup(){if(event_)vx_event_release(event_);if(kernel)vx_kernel_release(kernel);if(module_)vx_module_release(module_);if(queue)vx_queue_release(queue);for(auto b:buffers)vx_buffer_release(b);if(device)vx_device_release(device);alarm(0);}
static void deadline(int){static const char s[]="FAILED: architecture deadline\n";(void)!write(2,s,sizeof(s)-1);_exit(1);}
#define CHECK(e) do{int s=(e);if(s){std::fprintf(stderr,"%s: %d\n",#e,s);cleanup();return 1;}}while(false)
static int allocate(uint64_t bytes,vx_buffer_h* b,uint64_t* addr){int s=vx_buffer_create(device,bytes,VX_MEM_READ_WRITE,b);if(!s){buffers.push_back(*b);s=vx_buffer_address(*b,addr);}return s;}
static uint64_t mix(uint64_t x){x+=UINT64_C(0x9e3779b97f4a7c15);x=(x^(x>>30))*UINT64_C(0xbf58476d1ce4e5b9);x=(x^(x>>27))*UINT64_C(0x94d049bb133111eb);return x^(x>>31);}
int main(int argc,char**argv){
#ifdef ARCH_GPU_GLOBAL
 const char* mode="gpu_global";
#elif defined(ARCH_GPU_LOCAL)
 const char* mode="gpu_local";
#elif defined(ARCH_GPU_COMB)
 const char* mode="gpu_core_comb";
#elif defined(ARCH_ENGINE_HYBRID)
 const char* mode="engine_hybrid";
#else
 const char* mode="engine_comb";
#endif
 uint64_t bytes=1024,seed=17;uint32_t updates=4096,epochs=1,seconds=600;std::string pattern="random",kernel_file=std::string("kernel-")+mode+".vxbin";
 int opt;while((opt=getopt(argc,argv,"b:n:e:p:s:t:k:h"))!=-1){switch(opt){case'b':bytes=std::strtoull(optarg,nullptr,0);break;case'n':updates=std::strtoul(optarg,nullptr,0);break;case'e':epochs=std::strtoul(optarg,nullptr,0);break;case'p':pattern=optarg;break;case's':seed=std::strtoull(optarg,nullptr,0);break;case't':seconds=std::strtoul(optarg,nullptr,0);break;case'k':kernel_file=optarg;break;case'h':std::puts("-b state_bytes -n updates_per_epoch -e epochs -p random|skew|hot -s seed -t seconds -k kernel");return 0;default:return 1;}}
 if(bytes<128||bytes>32768||(bytes&(bytes-1))||!epochs||epochs>64||uint64_t(updates)*epochs>1048576||!seconds||seconds>3600||(pattern!="random"&&pattern!="skew"&&pattern!="hot"))return 1;
 std::signal(SIGALRM,deadline);alarm(seconds);std::setvbuf(stdout,nullptr,_IOLBF,0);
 CHECK(vx_device_open(0,&device));vx_queue_info_t qi{sizeof(qi),nullptr,VX_QUEUE_PRIORITY_NORMAL,0};CHECK(vx_queue_create(device,&qi,&queue));
 uint64_t cores,warps,threads,capacity;CHECK(vx_device_query(device,VX_CAPS_NUM_CORES,&cores));CHECK(vx_device_query(device,VX_CAPS_NUM_WARPS,&warps));CHECK(vx_device_query(device,VX_CAPS_NUM_THREADS,&threads));CHECK(vx_device_query(device,VX_CAPS_LOCAL_MEM_SIZE,&capacity));
 uint32_t per_core=warps*threads,workers=cores*per_core,words=bytes/8;if(!cores||cores>16||(cores&(cores-1))||!workers||words%cores){cleanup();return 1;}
 uint64_t local_bytes=0;
#if defined(ARCH_GPU_LOCAL)||defined(ARCH_ENGINE_HYBRID)
 local_bytes=bytes/cores;
#elif defined(ARCH_GPU_COMB)
 local_bytes=bytes;
#elif defined(ARCH_ENGINE_COMB)
 local_bytes=bytes+bytes/cores;
#endif
 if(local_bytes>capacity){std::printf("SKIPPED: mode=%s state_bytes=%" PRIu64 " required_local_bytes=%" PRIu64 " capacity=%" PRIu64 " reason=same_per_core_SRAM_budget\n",mode,bytes,local_bytes,capacity);cleanup();return 77;}
 std::vector<request_t> input(updates);std::vector<int64_t> expected(words),actual(words),partial(uint64_t(cores)*words);std::vector<result_t> results(workers);
 std::vector<uint64_t> want_inputs(workers),want_local(workers),want_remote(workers),want_puts(workers);uint64_t hash=UINT64_C(0xcbf29ce484222325),distinct=0;std::vector<uint8_t> touched(words);
 for(uint32_t i=0;i<updates;++i){uint64_t h=mix(uint64_t(i)^mix(seed));uint32_t key=h&(words-1);if(pattern=="skew"&&h%10!=0)key=mix(h)&(std::max<uint32_t>(1,words/128)-1);if(pattern=="hot")key=0;int32_t delta=int32_t((h>>48)%15)-7;if(!delta)delta=1;input[i]={key,delta};expected[key]+=int64_t(delta)*epochs;uint32_t rank=i%workers,source=rank/per_core;++want_inputs[rank];partial[uint64_t(source)*words+key]+=delta;
#ifdef ARCH_GPU_GLOBAL
 ++want_remote[rank];
#elif defined(ARCH_GPU_LOCAL)||defined(ARCH_ENGINE_HYBRID)
 if(key%cores==source)++want_local[rank];else ++want_remote[rank];
#endif
 hash=(hash^key)*UINT64_C(0x100000001b3);hash=(hash^uint64_t(int64_t(delta)))*UINT64_C(0x100000001b3);if(!touched[key]){touched[key]=1;++distinct;}}
#if defined(ARCH_GPU_COMB)||defined(ARCH_ENGINE_COMB)
 for(uint32_t c=0;c<cores;++c)for(uint32_t key=0;key<words;++key)if(partial[uint64_t(c)*words+key]){uint32_t rank=c*per_core+key%per_core;
#ifdef ARCH_GPU_COMB
 ++want_puts[rank];
#else
 if(key%cores==c)++want_local[rank];else ++want_remote[rank];
#endif
 }
#endif
 for(uint32_t r=0;r<workers;++r){want_inputs[r]*=epochs;want_local[r]*=epochs;want_remote[r]*=epochs;want_puts[r]*=epochs;}
 args_t a{};a.core_shift=__builtin_ctzll(cores);a.words=words;a.words_per_core=words/cores;a.updates=updates;a.epochs=epochs;a.cores=cores;a.per_core=per_core;a.threads=threads;a.local_bytes=local_bytes;vx_buffer_h ib,sb,rb;
 CHECK(allocate(std::max<uint64_t>(8,input.size()*8),&ib,&a.input_addr));CHECK(allocate(bytes,&sb,&a.state_addr));CHECK(allocate(results.size()*sizeof(result_t),&rb,&a.result_addr));if(updates)CHECK(vx_enqueue_write(queue,ib,0,input.data(),input.size()*8,0,nullptr,nullptr));CHECK(vx_enqueue_write(queue,sb,0,actual.data(),bytes,0,nullptr,nullptr));CHECK(vx_enqueue_write(queue,rb,0,results.data(),results.size()*sizeof(result_t),0,nullptr,nullptr));
 CHECK(vx_module_load_file(device,kernel_file.c_str(),&module_));CHECK(vx_module_get_kernel(module_,"main",&kernel));
 std::printf("INPUT mode=%s state_bytes=%" PRIu64 " updates=%u epochs=%u logical_updates=%" PRIu64 " pattern=%s seed=%" PRIu64 " hash=%016" PRIx64 " distinct_keys=%" PRIu64 " cores=%" PRIu64 " warps=%" PRIu64 " threads=%" PRIu64 " owner_mapping=key_mod_cores owner_words=%u lmem_bytes_per_core=%" PRIu64 " lmem_capacity_per_core=%" PRIu64 " input_bytes=%zu global_state_bytes=%" PRIu64 " global_partial_bytes=0 host_reset_excluded=1\n",mode,bytes,updates,epochs,uint64_t(updates)*epochs,pattern.c_str(),seed,hash,distinct,cores,warps,threads,a.words_per_core,local_bytes,capacity,input.size()*8,bytes);
 vx_launch_info_t launch{};launch.struct_size=sizeof(launch);launch.kernel=kernel;launch.args_host=&a;launch.args_size=sizeof(a);launch.ndim=1;launch.grid_dim[0]=cores;launch.block_dim[0]=per_core;launch.lmem_size=local_bytes;CHECK(vx_enqueue_launch(queue,&launch,0,nullptr,nullptr));CHECK(vx_enqueue_read(queue,actual.data(),sb,0,bytes,0,nullptr,nullptr));CHECK(vx_enqueue_read(queue,results.data(),rb,0,results.size()*sizeof(result_t),0,nullptr,&event_));CHECK(vx_event_wait_value(event_,1,(uint64_t(seconds)+5)*UINT64_C(1000000000)));
 uint32_t errors=0;uint64_t checksum=UINT64_C(0xcbf29ce484222325);for(uint32_t k=0;k<words;++k){checksum=(checksum^uint64_t(actual[k]))*UINT64_C(0x100000001b3);if(actual[k]!=expected[k]){if(errors<8)std::fprintf(stderr,"state[%u]=%" PRId64 " expected=%" PRId64 "\n",k,actual[k],expected[k]);++errors;}}
 uint64_t total_local=0,total_remote=0,total_attempts=0,total_retries=0,total_puts=0,start=UINT64_MAX,init_min=UINT64_MAX,init_max=0,process_min=UINT64_MAX,process_max=0,end=0;
 for(uint32_t rank=0;rank<workers;++rank){auto&r=results[rank];bool bad=r.rank!=rank||r.inputs!=want_inputs[rank]||r.local_updates!=want_local[rank]||r.remote_updates!=want_remote[rank]||r.partials_published!=want_puts[rank]||r.partials_applied||r.error||r.outstanding||r.complete!=ARCH_COMPLETE||r.start>r.initialized||r.initialized>r.processed||r.processed>r.end;
#if defined(ARCH_ENGINE_HYBRID)||defined(ARCH_ENGINE_COMB)
 bad|=r.send_attempts!=r.remote_updates+r.send_retries;
#else
 bad|=r.send_attempts||r.send_retries;
#endif
 if(bad){++errors;std::fprintf(stderr,"worker=%u error=%" PRIu64 " input=%" PRIu64 "/%" PRIu64 " local=%" PRIu64 "/%" PRIu64 " remote=%" PRIu64 "/%" PRIu64 " puts=%" PRIu64 "/%" PRIu64 " outstanding=%" PRIu64 " complete=%" PRIx64 "\n",rank,r.error,r.inputs,want_inputs[rank],r.local_updates,want_local[rank],r.remote_updates,want_remote[rank],r.partials_published,want_puts[rank],r.outstanding,r.complete);}total_local+=r.local_updates;total_remote+=r.remote_updates;total_attempts+=r.send_attempts;total_retries+=r.send_retries;total_puts+=r.partials_published;start=std::min(start,r.start);init_min=std::min(init_min,r.initialized);init_max=std::max(init_max,r.initialized);process_min=std::min(process_min,r.processed);process_max=std::max(process_max,r.processed);end=std::max(end,r.end);}
 auto counter=[&](uint32_t cls,uint32_t csr,uint32_t core){uint64_t value=0;if(vx_device_mpm_query(device,cls,csr,core,&value)){++errors;}return value;};uint64_t cycles=counter(VX_DCR_MPM_CLASS_CORE,VX_CSR_MCYCLE,0),dr=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_READS,0),dw=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_MEM_WRITES,0),l2r=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_READS,0),l2w=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_WRITES,0),l2mr=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_R,0),l2mw=counter(VX_DCR_MPM_CLASS_L2CACHE,VX_CSR_MPM_L2CACHE_MISS_W,0),lmread=0,lmwrite=0,lmstall=0;for(uint32_t c=0;c<cores;++c){lmread+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_READS,c);lmwrite+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_WRITES,c);lmstall+=counter(VX_DCR_MPM_CLASS_MEM,VX_CSR_MPM_LMEM_BANK_ST,c);}
 cleanup();if(errors){std::fprintf(stderr,"FAILED: %u exact validation errors\n",errors);return 1;}std::printf("PASSED: every state word and worker count checksum=%016" PRIx64 "\n",checksum);
 std::printf("RESULT mode=%s state_bytes=%" PRIu64 " updates=%u epochs=%u pattern=%s seed=%" PRIu64 " hash=%016" PRIx64 " checksum=%016" PRIx64 " device_cycles_post_readback_including_flush=%" PRIu64 " phase_cycles=%" PRIu64 " init_span_cycles=%" PRIu64 " process_span_cycles=%" PRIu64 " output_span_cycles=%" PRIu64 " local_updates=%" PRIu64 " remote_updates=%" PRIu64 " partials_published=%" PRIu64 " send_attempts=%" PRIu64 " send_retries=%" PRIu64 " lmem_bytes_per_core=%" PRIu64 " lmem_reads=%" PRIu64 " lmem_writes=%" PRIu64 " lmem_bank_stalls=%" PRIu64 " dram_reads=%" PRIu64 " dram_writes=%" PRIu64 " l2_reads=%" PRIu64 " l2_writes=%" PRIu64 " l2_read_misses=%" PRIu64 " l2_write_misses=%" PRIu64 "\n",mode,bytes,updates,epochs,pattern.c_str(),seed,hash,checksum,cycles,end-start,init_max-start,process_max-init_min,end-process_min,total_local,total_remote,total_puts,total_attempts,total_retries,local_bytes,lmread,lmwrite,lmstall,dr,dw,l2r,l2w,l2mr,l2mw);return 0;
}
