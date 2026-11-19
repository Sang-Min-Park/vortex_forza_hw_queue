#pragma once
#include <stdint.h>
struct request_t {uint32_t key,id;int32_t delta;uint32_t reserved;};
struct record_t {uint64_t count,sum,xor_value,lock;};
struct response_t {uint64_t count,sum,xor_value,complete;};
struct result_t {uint64_t rank,processed,sent,received,retries,error,outstanding,complete,start,initialized,processed_cycle,end,peak_inflight;};
struct args_t {
 uint64_t input,state,responses,results,counts,offsets,cursors,buckets;
 uint32_t keys,requests,cores,per_core,threads,stage,core_shift,limit,window;
};
enum {RECORD_COMPLETE=0x52454344,RESPONSE_COMPLETE=0x52535044};
static_assert(sizeof(request_t)==16,"request ABI");
static_assert(sizeof(record_t)==32,"record ABI");

#ifndef VM_PROGRAM
#define VM_PROGRAM 0
#endif
#if VM_PROGRAM == 0
#define VM_PROGRAM_NAME "sumxor"
#else
#define VM_PROGRAM_NAME "minmax"
#endif
