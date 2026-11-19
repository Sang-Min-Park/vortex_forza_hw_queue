#pragma once
#include <vx_spawn2.h>
#include <vx_barrier.h>
#include "common.h"
// Cross-lane operations must remain above lane-predicate control flow.
static inline __attribute__((always_inline)) uint64_t ordered_shfl_up(uint64_t value,int b,int c,int mask) {
 uint64_t result;uint64_t control=(mask<<12)|(c<<6)|b;
 __asm__ volatile(".insn r 0x0b,4,1,%0,%1,%2":"=r"(result):"r"(value),"r"(control):"memory");return result;
}
static inline __attribute__((always_inline)) uint64_t ordered_shfl_idx(uint64_t value,int b,int c,int mask) {
 uint64_t result;uint64_t control=(mask<<12)|(c<<6)|b;
 __asm__ volatile(".insn r 0x0b,7,1,%0,%1,%2":"=r"(result):"r"(value),"r"(control):"memory");return result;
}
static inline __attribute__((always_inline)) uint64_t ordered_vote_ballot(int predicate) {
 uint64_t result;__asm__ volatile(".insn r 0x0b,3,1,%0,%1,x0":"=r"(result):"r"(uint64_t(predicate)):"memory");return result;
}
static inline __attribute__((always_inline)) uint64_t ordered_vote_all(int predicate) {
 uint64_t result;__asm__ volatile(".insn r 0x0b,0,1,%0,%1,x0":"=r"(result):"r"(uint64_t(predicate)):"memory");return result;
}
#define vx_shfl_up ordered_shfl_up
#define vx_shfl_idx ordered_shfl_idx
#define vx_vote_ballot ordered_vote_ballot
#define vx_vote_all ordered_vote_all
static inline uint64_t swap64(volatile uint64_t*p,uint64_t value){uint64_t old;__asm__ volatile("amoswap.d %0,%2,(%1)":"=r"(old):"r"(p),"r"(value):"memory");return old;}
static inline uint64_t add64(volatile uint64_t*p,uint64_t value){uint64_t old;__asm__ volatile("amoadd.d %0,%2,(%1)":"=r"(old):"r"(p),"r"(value):"memory");return old;}
static inline void respond(volatile response_t*p,uint32_t id,uint64_t count,uint64_t sum,uint64_t value){p[id].count=count;p[id].sum=sum;p[id].xor_value=value;p[id].complete=RESPONSE_COMPLETE;}
static inline uint32_t rank_of(args_t*a){return vx_core_id()*a->per_core+vx_warp_id()*a->threads+vx_thread_id();}
static inline void barrier(uint32_t id){vx_fence();vortex::gbarrier(id).arrive_and_wait();}
static inline request_t load_request(const request_t*input,uint32_t i){auto p=reinterpret_cast<const uint64_t*>(input)+uint64_t(i)*2;uint64_t pair=p[0],delta=p[1];return {uint32_t(pair),uint32_t(pair>>32),int32_t(uint32_t(delta)),0};}
static inline void store_request(request_t*output,uint64_t i,const request_t&r){auto p=reinterpret_cast<uint64_t*>(output)+i*2;p[0]=uint64_t(r.key)|(uint64_t(r.id)<<32);p[1]=uint32_t(r.delta);}

static inline uint64_t identity_a() { return VM_PROGRAM == 0 ? 0 : uint64_t(INT64_MAX); }
static inline uint64_t identity_b() { return VM_PROGRAM == 0 ? 0 : uint64_t(INT64_MIN); }
static inline uint64_t combine_a(uint64_t a, uint64_t b) {
 return VM_PROGRAM == 0 ? a+b : uint64_t(int64_t(a)<int64_t(b)?int64_t(a):int64_t(b));
}
static inline uint64_t combine_b(uint64_t a, uint64_t b) {
 return VM_PROGRAM == 0 ? a^b : uint64_t(int64_t(a)>int64_t(b)?int64_t(a):int64_t(b));
}
static inline uint64_t value_b(const request_t& r) { return VM_PROGRAM == 0 ? r.id : uint64_t(int64_t(r.delta)); }

static inline void branch_update(uint64_t& count,uint64_t& sum,uint64_t& value,int32_t delta,uint32_t id) {
 ++count;sum=(sum&1)?sum+uint64_t(int64_t(delta)):sum^uint64_t(id);value+=sum;
}
