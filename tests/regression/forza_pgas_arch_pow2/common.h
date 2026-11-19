#pragma once
#include <stdint.h>
struct request_t {uint32_t key;int32_t delta;};
struct result_t {
 uint64_t rank,inputs,local_updates,remote_updates,send_attempts,send_retries;
 uint64_t partials_published,partials_applied,outstanding,error,start,initialized,processed,end,complete;
};
struct args_t {
 uint64_t input_addr,state_addr,result_addr,core_shift;
 uint32_t words,words_per_core,updates,epochs,cores,per_core,threads,local_bytes;
};
enum {ARCH_COMPLETE=0x41524348};
