#pragma once
#include <stdint.h>
struct request_t {uint32_t key,id;int32_t delta;uint32_t reserved;};
struct record_t {uint64_t count,sum,xor_value,lock;
#ifdef VM_PADDED
uint64_t padding[4];
#endif
};
struct response_t {uint64_t count,sum,xor_value,complete;};
struct result_t {uint64_t rank,processed,sent,received,retries,error,outstanding,complete,start,initialized,processed_cycle,end,peak_inflight;};
struct args_t {
 uint64_t input,state,responses,results,counts,offsets,cursors,buckets;
 uint32_t keys,requests,cores,per_core,threads,stage,core_shift,limit,window;
};
enum {RECORD_COMPLETE=0x52454344,RESPONSE_COMPLETE=0x52535044};
static_assert(sizeof(request_t)==16,"request ABI");
#ifdef VM_PADDED
static_assert(sizeof(record_t)==64,"record ABI");
#define VM_GLOBAL_STRIDE "64"
#else
static_assert(sizeof(record_t)==32,"record ABI");
#define VM_GLOBAL_STRIDE "32"
#endif

#ifndef VM_PROGRAM
#define VM_PROGRAM 2
#endif
#define VM_PROGRAM_NAME "branch"
