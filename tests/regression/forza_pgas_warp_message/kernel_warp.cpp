#include "device.h"
__kernel void kernel_main(args_t* __UNIFORM__ a){
 uint32_t lane=vx_thread_id(),rank=rank_of(a),workers=a->cores*a->per_core,cursor=rank;auto input=reinterpret_cast<const request_t*>(a->input);auto state=reinterpret_cast<volatile record_t*>(a->state);auto responses=reinterpret_cast<volatile response_t*>(a->responses);auto out=reinterpret_cast<result_t*>(a->results)+rank;
 out->rank=rank;barrier(1);uint64_t start=vx_rdcycle(),processed=0,retries=0;bool complete=false;
 for(uint32_t round=0;round<a->limit;++round){bool pending=cursor<a->requests;request_t r{};if(pending)r=load_request(input,cursor);bool groups_complete=true;
  for(uint32_t leader=0;leader<4;++leader){uint32_t __UNIFORM__ key=vx_shfl_idx(r.key,leader,VX_QUAD_CVAL,VX_QUAD_MASK);uint32_t __UNIFORM__ group=vx_vote_ballot(pending&&r.key==key);uint32_t __UNIFORM__ first=(group&(uint32_t(1)<<leader))&&!(group&((uint32_t(1)<<leader)-1));if(!first)continue;bool accepted=false;
   uint64_t own_sum=((group>>lane)&1)?uint64_t(int64_t(r.delta)):0,own_xor=((group>>lane)&1)?r.id:0;
   uint64_t inclusive_sum=own_sum,inclusive_xor=own_xor;
   for(uint32_t step=1;step<4;step<<=1){uint64_t other_sum=vx_shfl_up(inclusive_sum,step,VX_QUAD_CVAL,VX_QUAD_MASK),other_xor=vx_shfl_up(inclusive_xor,step,VX_QUAD_CVAL,VX_QUAD_MASK);inclusive_sum+=(lane>=step)?other_sum:0;inclusive_xor^=(lane>=step)?other_xor:0;}
   uint64_t total_sum=vx_shfl_idx(inclusive_sum,3,VX_QUAD_CVAL,VX_QUAD_MASK),total_xor=vx_shfl_idx(inclusive_xor,3,VX_QUAD_CVAL,VX_QUAD_MASK),prefix_sum=inclusive_sum-own_sum,prefix_xor=inclusive_xor^own_xor;
   uint32_t count=__builtin_popcount(group),prefix_count=__builtin_popcount(group&((uint32_t(1)<<lane)-1));
   for(uint32_t attempt=0;attempt<4096;++attempt){uint64_t oldlock=1;if(lane==leader)oldlock=swap64(&state[key].lock,1);uint32_t __UNIFORM__ acquired=vx_shfl_idx(oldlock,leader,VX_QUAD_CVAL,VX_QUAD_MASK)==0;
   if(acquired){
    uint64_t oldcount=0,oldsum=0,oldxor=0;if(lane==leader){oldcount=state[key].count;oldsum=state[key].sum;oldxor=state[key].xor_value;state[key].count=oldcount+count;state[key].sum=oldsum+total_sum;state[key].xor_value=oldxor^total_xor;vx_fence();swap64(&state[key].lock,0);}oldcount=vx_shfl_idx(oldcount,leader,VX_QUAD_CVAL,VX_QUAD_MASK);oldsum=vx_shfl_idx(oldsum,leader,VX_QUAD_CVAL,VX_QUAD_MASK);oldxor=vx_shfl_idx(oldxor,leader,VX_QUAD_CVAL,VX_QUAD_MASK);
    if((group>>lane)&1){respond(responses,r.id,oldcount+prefix_count,oldsum+prefix_sum,oldxor^prefix_xor);cursor+=workers;++processed;}accepted=true;break;
   }else if((group>>lane)&1)++retries;
   }if(!accepted){groups_complete=false;break;}
  }
  if(!groups_complete){out->error=2;break;}uint32_t __UNIFORM__ done=vx_vote_all(cursor>=a->requests);if(done){complete=true;break;}
 }
 if(!complete)out->error=1;barrier(2);out->processed=processed;out->retries=retries;out->start=start;out->initialized=start;out->processed_cycle=vx_rdcycle();out->end=vx_rdcycle();out->complete=RECORD_COMPLETE;
}
