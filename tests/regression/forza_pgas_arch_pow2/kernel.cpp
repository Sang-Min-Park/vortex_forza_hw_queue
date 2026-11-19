#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <forza_update_abi.h>
#include <forza_mmio_abi.h>
#include "common.h"

static inline void add(int64_t* p,int64_t v){__asm__ volatile("amoadd.d zero,%1,(%0)"::"r"(p),"r"(v):"memory");}
[[maybe_unused]]static inline void store(int64_t* p,int64_t v){__asm__ volatile("amoswap.d zero,%1,(%0)"::"r"(p),"r"(v):"memory");}
[[maybe_unused]]static inline uint64_t update(uint32_t core,uint32_t offset,int64_t delta){uint64_t result,address=FORZA_UPDATE_BASE+uint64_t(core)*FORZA_UPDATE_ZONE_STRIDE+uint64_t(offset)*8;__asm__ volatile("amoswap.d %0,%2,(%1)":"=r"(result):"r"(address),"r"(delta):"memory");return result;}
[[maybe_unused]]static inline uint64_t outstanding(){
 uint32_t actor=vx_warp_id()*vx_num_threads()+vx_thread_id();
 return *reinterpret_cast<volatile const uint64_t*>(forza_mmio_actor_address(actor,FORZA_MMIO_TX_COUNTS))&255;
}
[[maybe_unused]]static inline bool drain(uint64_t& remaining){
 for(uint32_t poll=0;poll<1000000;++poll){remaining=outstanding();uint32_t __UNIFORM__ all_done=vx_vote_all(remaining==0);if(all_done)return true;}
 return false;
}

__kernel void kernel_main(args_t* __UNIFORM__ a){
 uint32_t core=vx_core_id(),local=vx_warp_id()*a->threads+vx_thread_id(),rank=core*a->per_core+local,workers=a->cores*a->per_core;
 auto input=reinterpret_cast<const uint64_t*>(a->input_addr);auto global=reinterpret_cast<int64_t*>(a->state_addr);
 auto result=reinterpret_cast<result_t*>(a->result_addr)+rank;auto scratch=reinterpret_cast<int64_t*>(__local_mem());
#if defined(ARCH_GPU_LOCAL)||defined(ARCH_ENGINE_HYBRID)||defined(ARCH_ENGINE_COMB)
 uint32_t __UNIFORM__ core_mask=a->cores-1,core_shift=a->core_shift;
#endif
 [[maybe_unused]]auto partial=scratch;
#ifdef ARCH_ENGINE_COMB
 partial=scratch+a->words_per_core;
#endif
 result->rank=rank;uint64_t inputs=0,local_updates=0,remote_updates=0,attempts=0,retries=0,puts=0,applied=0,remaining=0;
 __syncthreads();vx_fence();vortex::gbarrier(1).arrive_and_wait();uint64_t start=vx_rdcycle();
#if defined(ARCH_GPU_LOCAL)||defined(ARCH_ENGINE_HYBRID)||defined(ARCH_ENGINE_COMB)
 for(uint32_t offset=local;offset<a->words_per_core;offset+=a->per_core)store(scratch+offset,0);
#endif
 vx_fence();__syncthreads();vortex::gbarrier(2).arrive_and_wait();uint64_t initialized=vx_rdcycle();
 for(uint32_t epoch=0;epoch<a->epochs;++epoch){
#if defined(ARCH_GPU_COMB)||defined(ARCH_ENGINE_COMB)
  for(uint32_t key=local;key<a->words;key+=a->per_core)store(partial+key,0);
  vx_fence();__syncthreads();
  for(uint32_t i=rank;i<a->updates;i+=workers){uint64_t raw=input[i];uint32_t key=uint32_t(raw);int32_t delta=int32_t(raw>>32);add(partial+key,delta);++inputs;}
  vx_fence();__syncthreads();
#ifdef ARCH_GPU_COMB
  for(uint32_t key=local;key<a->words;key+=a->per_core){int64_t value=partial[key];if(value){add(global+key,value);++puts;}}
  vx_fence();
#else
  for(uint32_t key=local;key<a->words;key+=a->per_core){int64_t value=partial[key];if(!value)continue;uint32_t owner=key&core_mask,offset=key>>core_shift;if(owner==core){add(scratch+offset,value);++local_updates;}else{bool accepted=false;for(uint32_t retry=0;retry<1000000;++retry){++attempts;uint64_t status=update(owner,offset,value);if(status==0){++remote_updates;accepted=true;break;}if(status!=1){result->error=2;break;}++retries;}if(!accepted)result->error=3;}}
  vx_fence();if(!drain(remaining))result->error=4;
#endif
#else
  for(uint32_t i=rank;i<a->updates;i+=workers){uint64_t raw=input[i];uint32_t key=uint32_t(raw);int32_t delta=int32_t(raw>>32);
#ifdef ARCH_GPU_GLOBAL
   add(global+key,delta);++remote_updates;
#else
   uint32_t owner=key&core_mask,offset=key>>core_shift;
   if(owner==core){add(scratch+offset,delta);++local_updates;}
   else{
#ifdef ARCH_GPU_LOCAL
    add(global+key,delta);++remote_updates;
#else
    bool accepted=false;for(uint32_t retry=0;retry<1000000;++retry){++attempts;uint64_t status=update(owner,offset,delta);if(status==0){++remote_updates;accepted=true;break;}if(status!=1){result->error=2;break;}++retries;}if(!accepted)result->error=3;
#endif
   }
#endif
   ++inputs;
  }
  vx_fence();
#ifdef ARCH_ENGINE_HYBRID
  if(!drain(remaining))result->error=4;
#endif
#endif
  vortex::gbarrier(4).arrive_and_wait();vx_fence();
 }
 uint64_t processed=vx_rdcycle();
#ifdef ARCH_GPU_LOCAL
 for(uint32_t offset=local;offset<a->words_per_core;offset+=a->per_core)scratch[offset]+=global[uint64_t(offset)*a->cores+core];
#endif
#if defined(ARCH_GPU_LOCAL)||defined(ARCH_ENGINE_HYBRID)||defined(ARCH_ENGINE_COMB)
 for(uint32_t offset=local;offset<a->words_per_core;offset+=a->per_core)global[uint64_t(offset)*a->cores+core]=scratch[offset];
#endif
 vx_fence();vortex::gbarrier(5).arrive_and_wait();vx_fence();
 result->inputs=inputs;result->local_updates=local_updates;result->remote_updates=remote_updates;result->send_attempts=attempts;result->send_retries=retries;result->partials_published=puts;result->partials_applied=applied;result->outstanding=remaining;result->start=start;result->initialized=initialized;result->processed=processed;result->end=vx_rdcycle();result->complete=ARCH_COMPLETE;
}
