#pragma once
#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
static inline uint64_t swap64(volatile uint64_t*p,uint64_t value){uint64_t old;__asm__ volatile("amoswap.d %0,%2,(%1)":"=r"(old):"r"(p),"r"(value):"memory");return old;}
static inline uint64_t add64(volatile uint64_t*p,uint64_t value){uint64_t old;__asm__ volatile("amoadd.d %0,%2,(%1)":"=r"(old):"r"(p),"r"(value):"memory");return old;}
static inline void respond(volatile response_t*p,uint32_t id,uint64_t count,uint64_t sum,uint64_t value){p[id].count=count;p[id].sum=sum;p[id].xor_value=value;p[id].complete=RESPONSE_COMPLETE;}
static inline uint32_t rank_of(args_t*a){return vx_core_id()*a->per_core+vx_warp_id()*a->threads+vx_thread_id();}
static inline void barrier(uint32_t id){vx_fence();vortex::gbarrier(id).arrive_and_wait();}
static inline request_t load_request(const request_t*input,uint32_t i){auto p=reinterpret_cast<const uint64_t*>(input)+uint64_t(i)*2;uint64_t pair=p[0],delta=p[1];return {uint32_t(pair),uint32_t(pair>>32),int32_t(uint32_t(delta)),0};}
static inline void store_request(request_t*output,uint64_t i,const request_t&r){auto p=reinterpret_cast<uint64_t*>(output)+i*2;p[0]=uint64_t(r.key)|(uint64_t(r.id)<<32);p[1]=uint32_t(r.delta);}
