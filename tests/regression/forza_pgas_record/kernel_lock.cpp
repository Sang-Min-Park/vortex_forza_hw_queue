#include "device.h"
__kernel void kernel_main(args_t* __UNIFORM__ a){
 uint32_t rank=rank_of(a),workers=a->cores*a->per_core,cursor=rank;auto input=reinterpret_cast<const request_t*>(a->input);auto state=reinterpret_cast<volatile record_t*>(a->state);auto responses=reinterpret_cast<volatile response_t*>(a->responses);auto out=reinterpret_cast<result_t*>(a->results)+rank;
 out->rank=rank;barrier(1);uint64_t start=vx_rdcycle(),processed=0,retries=0;bool loaded=false,complete=false;request_t r{};
 for(uint32_t iteration=0;iteration<a->limit;++iteration){if(cursor<a->requests){if(!loaded){r=load_request(input,cursor);loaded=true;}auto p=state+r.key;if(swap64(&p->lock,1)==0){uint64_t count=p->count,sum=p->sum,value=p->xor_value;p->count=count+1;p->sum=sum+uint64_t(int64_t(r.delta));p->xor_value=value^r.id;vx_fence();swap64(&p->lock,0);respond(responses,r.id,count,sum,value);cursor+=workers;++processed;loaded=false;}else ++retries;}uint32_t __UNIFORM__ done=vx_vote_all(cursor>=a->requests);if(done){complete=true;break;}}
 if(!complete)out->error=1;barrier(2);out->processed=processed;out->retries=retries;out->start=start;out->initialized=start;out->processed_cycle=vx_rdcycle();out->end=vx_rdcycle();out->complete=RECORD_COMPLETE;
}
