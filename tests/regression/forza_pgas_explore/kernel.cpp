#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
#if __riscv_xlen!=64 || !defined(VX_CFG_EXT_A_ENABLE)
#error "PGAS experiments require RV64 and A"
#endif
#ifdef PGAS_HW
#include "transport.h"
#endif

[[maybe_unused]] static inline void store_ack(uint64_t* p,uint64_t value) {
  __asm__ volatile("amoswap.d zero,%1,(%0)"::"r"(p),"r"(value):"memory");
}
[[maybe_unused]] static inline void add_ack(int64_t* p,int64_t value) {
  __asm__ volatile("amoadd.d zero,%1,(%0)"::"r"(p),"r"(value):"memory");
}
[[maybe_unused]] static inline uint64_t poll_sequence(uint64_t* p) {
  uint64_t value;
  __asm__ volatile("amoadd.d %0,zero,(%1)":"=r"(value):"r"(p):"memory");
  return value;
}

__kernel void kernel_main(args_t* __UNIFORM__ arg) {
  const uint32_t lane=vx_thread_id(),core=vx_core_id();
  const uint32_t local=vx_warp_id()*arg->threads+lane;
  const uint32_t rank=core*arg->per_core+local;
  const uint32_t actors=arg->actors,owned=arg->words_per_actor;
  const uint32_t owner_shift=__builtin_ctz(owned);
  [[maybe_unused]] const uint32_t zone_shift=__builtin_ctz(arg->per_core);
  [[maybe_unused]] const uint32_t local_mask=arg->per_core-1;
#if defined(PGAS_STATE_GLOBAL)||defined(PGAS_GPU_GLOBAL)
  const uint32_t resident=0;
#else
  const uint32_t resident=arg->local_words_per_actor;
#endif
  auto input=reinterpret_cast<const uint64_t*>(arg->requests_addr);
  auto global=reinterpret_cast<int64_t*>(arg->state_addr);
  auto scratch=reinterpret_cast<int64_t*>(__local_mem());
  [[maybe_unused]] auto mine=scratch+local;
  auto out=reinterpret_cast<result_t*>(arg->results_addr)+rank;
  [[maybe_unused]] uint64_t expected=out->expected_receive;
  out->rank=rank;
#ifdef PGAS_SW
  uint64_t produced[PGAS_MAX_ACTORS]{},consumed[PGAS_MAX_ACTORS]{};
  uint32_t cursor=0;
  auto slots=reinterpret_cast<queue_slot_t*>(arg->queues_addr);
#endif
#ifdef PGAS_HW
  if(!pgas_transport::init())out->error=1;
#endif
  __syncthreads();vx_fence();vortex::gbarrier(2).arrive_and_wait();
  uint64_t start=vx_rdcycle();
#ifndef PGAS_GPU_GLOBAL
  for(uint32_t offset=0;offset<resident;++offset)store_ack(reinterpret_cast<uint64_t*>(mine+uint64_t(offset)*arg->per_core),0);
#endif
  vx_fence();__syncthreads();vortex::gbarrier(3).arrive_and_wait();
  uint64_t initialized=vx_rdcycle();
  uint64_t inputs=0,sent=0,received=0,direct=0,attempts=0,retries=0,polls=0,outstanding=0,local_amo=0,global_amo=0;
  [[maybe_unused]] auto apply=[&](uint32_t offset,int64_t delta) {
    if(offset<resident)mine[uint64_t(offset)*arg->per_core]+=delta;
    else global[uint64_t(rank)*owned+offset]+=delta;
  };
#if defined(PGAS_HW)||defined(PGAS_SW)
  uint32_t index=rank;bool complete=false,lane_complete=false;
  for(uint32_t iteration=0;iteration<arg->iteration_limit;++iteration) {
    if(!lane_complete) {
      if(index<arg->updates) {
        uint64_t word=input[index];uint32_t key=uint32_t(word);
        uint32_t owner=key>>owner_shift,offset=key&(owned-1);
        bool accepted=true;
        if(owner==rank){apply(offset,int32_t(word>>32));++direct;}
        else {
          uint64_t payload=(word&UINT64_C(0xffffffff00000000))|offset;
          ++attempts;
#ifdef PGAS_HW
          uint32_t status=pgas_transport::try_send(owner>>zone_shift,owner&local_mask,payload);
          accepted=status==1;
          if(status>1)out->error=2;
#else
          uint64_t position=produced[owner];
          auto slot=slots+pgas_queue_slot_index(rank,owner,position,actors,arg->queue_capacity);
          accepted=poll_sequence(&slot->sequence)==position;
          if(accepted){
            slot->payload=payload;
            vx_fence();store_ack(&slot->sequence,position+1);vx_fence();
            produced[owner]=position+1;
          }
#endif
          if(accepted)++sent;else ++retries;
        }
        if(accepted){index+=actors;++inputs;}
      }
      bool available=false;uint64_t payload=0;
#ifdef PGAS_HW
      ++polls;uint64_t control=0;
      uint32_t status=pgas_transport::try_receive(control,payload);
      if(status>1)out->error=3;
      available=status==1;
#else
      for(uint32_t probe=0;probe<4;++probe){
        uint32_t source=cursor;cursor=(cursor+1==actors)?0:cursor+1;
        if(source==rank)continue;
        uint64_t position=consumed[source];
        auto slot=slots+pgas_queue_slot_index(source,rank,position,actors,arg->queue_capacity);
        ++polls;
        if(poll_sequence(&slot->sequence)==position+1){
          payload=*reinterpret_cast<volatile uint64_t*>(&slot->payload);
          vx_fence();store_ack(&slot->sequence,position+arg->queue_capacity);vx_fence();
          consumed[source]=position+1;available=true;break;
        }
      }
#endif
      if(available){uint32_t offset=uint32_t(payload);if(offset>=owned)out->error=4;else{apply(offset,int32_t(payload>>32));++received;}}
      if(index>=arg->updates&&received==expected){
#ifdef PGAS_HW
        outstanding=pgas_transport::outstanding();
#endif
        lane_complete=outstanding==0;
      }
    }
    uint32_t __UNIFORM__ ready=vx_vote_all(lane_complete);
    if(ready){complete=true;break;}
  }
  if(!complete)out->error=5;
#ifdef PGAS_HW
  if(!pgas_transport::healthy())out->error=6;
#endif
#else
  for(uint32_t index=rank;index<arg->updates;index+=actors){
    uint64_t word=input[index];uint32_t key=uint32_t(word);
    [[maybe_unused]] uint32_t owner=key>>owner_shift,offset=key&(owned-1);
#ifdef PGAS_GPU_LOCAL
    if((owner>>zone_shift)==core&&offset<resident){add_ack(scratch+uint64_t(offset)*arg->per_core+(owner&local_mask),int32_t(word>>32));++local_amo;}
    else
#endif
    {add_ack(global+key,int32_t(word>>32));++global_amo;}
    ++inputs;
  }
#endif
  vx_fence();vortex::gbarrier(4).arrive_and_wait();vx_fence();
  uint64_t processed=vx_rdcycle();
#ifdef PGAS_GPU_LOCAL
  for(uint32_t offset=0;offset<resident;++offset)mine[uint64_t(offset)*arg->per_core]+=global[uint64_t(rank)*owned+offset];
#endif
#ifndef PGAS_GPU_GLOBAL
  for(uint32_t offset=0;offset<resident;++offset)global[uint64_t(rank)*owned+offset]=mine[uint64_t(offset)*arg->per_core];
#endif
  vx_fence();vortex::gbarrier(5).arrive_and_wait();vx_fence();
  out->inputs=inputs;out->sent=sent;out->received=received;out->direct=direct;
  out->attempts=attempts;out->retries=retries;out->polls=polls;out->outstanding=outstanding;
  out->local_amo=local_amo;out->global_amo=global_amo;
  out->start=start;out->initialized=initialized;out->processed=processed;out->end=vx_rdcycle();out->complete=PGAS_COMPLETE;
}
