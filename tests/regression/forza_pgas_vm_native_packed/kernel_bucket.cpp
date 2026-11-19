#include "device.h"
__kernel void kernel_main(args_t* __UNIFORM__ a){
 uint32_t rank=rank_of(a),workers=a->cores*a->per_core,core=vx_core_id(),local=vx_warp_id()*a->threads+vx_thread_id();auto input=reinterpret_cast<const request_t*>(a->input);auto buckets=reinterpret_cast<request_t*>(a->buckets);auto counts=reinterpret_cast<uint64_t*>(a->counts);auto offsets=reinterpret_cast<uint64_t*>(a->offsets);auto cursors=reinterpret_cast<uint64_t*>(a->cursors);auto state=reinterpret_cast<record_t*>(a->state);auto responses=reinterpret_cast<volatile response_t*>(a->responses);auto out=reinterpret_cast<result_t*>(a->results)+rank;out->rank=rank;
 barrier(1);uint64_t start=vx_rdcycle(),processed=0;
 if(a->stage==0){for(uint32_t k=rank;k<a->keys;k+=workers)swap64(counts+k,0);barrier(2);for(uint32_t i=rank;i<a->requests;i+=workers){request_t r=load_request(input,i);add64(counts+r.key,1);}barrier(3);if(rank==0){uint64_t position=0;for(uint32_t k=0;k<a->keys;++k){swap64(offsets+k,position);swap64(cursors+k,position);position+=counts[k];}}barrier(4);
  for(uint32_t i=rank;i<a->requests;i+=workers){request_t r=load_request(input,i);uint64_t p=add64(cursors+r.key,1);if(p>=a->requests){out->error=1;continue;}store_request(buckets,p,r);++processed;}
 }else{auto scratch=reinterpret_cast<uint64_t*>(__local_mem());uint32_t lane=vx_thread_id(),warp=vx_warp_id();
  for(uint32_t k=(warp<<a->core_shift)+core;k<a->keys;k+=a->cores*VX_CFG_NUM_WARPS){uint64_t count=0,sum=0,value=0,begin=offsets[k],n=counts[k];
   for(uint64_t block=0;block<n;block+=4){uint64_t position=block+lane;bool valid=position<n;request_t r{};if(valid){r=load_request(buckets,begin+position);if(r.key!=k||r.id>=a->requests)out->error=2;}
    uint64_t own_sum=valid?uint64_t(int64_t(r.delta)):identity_a(),own_xor=valid?value_b(r):identity_b(),inclusive_sum=own_sum,inclusive_xor=own_xor;
    for(uint32_t step=1;step<4;step<<=1){uint64_t other_sum=vx_shfl_up(inclusive_sum,step,VX_QUAD_CVAL,VX_QUAD_MASK),other_xor=vx_shfl_up(inclusive_xor,step,VX_QUAD_CVAL,VX_QUAD_MASK);inclusive_sum=combine_a(inclusive_sum,(lane>=step)?other_sum:identity_a());inclusive_xor=combine_b(inclusive_xor,(lane>=step)?other_xor:identity_b());}
    uint64_t total_sum=vx_shfl_idx(inclusive_sum,3,VX_QUAD_CVAL,VX_QUAD_MASK),total_xor=vx_shfl_idx(inclusive_xor,3,VX_QUAD_CVAL,VX_QUAD_MASK);
    uint64_t prefix_sum=inclusive_sum-own_sum,prefix_xor=inclusive_xor^own_xor;
#if VM_PROGRAM != 0
    prefix_sum=vx_shfl_up(inclusive_sum,1,VX_QUAD_CVAL,VX_QUAD_MASK);prefix_xor=vx_shfl_up(inclusive_xor,1,VX_QUAD_CVAL,VX_QUAD_MASK);uint64_t keep=uint64_t(0)-uint64_t(lane!=0);prefix_sum=(prefix_sum&keep)|(identity_a()&~keep);prefix_xor=(prefix_xor&keep)|(identity_b()&~keep);
#endif
    if(valid){respond(responses,r.id,count+lane,combine_a(sum,prefix_sum),combine_b(value,prefix_xor));++processed;}sum=combine_a(sum,total_sum);value=combine_b(value,total_xor);count+=(n-block<4)?n-block:4;
   }
   if(lane==0){uint32_t offset=(k>>a->core_shift)*3;swap64(scratch+offset,count);swap64(scratch+offset+1,sum);swap64(scratch+offset+2,value);}
  }
  barrier(6);for(uint32_t k=(local<<a->core_shift)+core;k<a->keys;k+=workers){uint32_t offset=(k>>a->core_shift)*3;state[k].count=scratch[offset];state[k].sum=scratch[offset+1];state[k].xor_value=scratch[offset+2];}
 }

 barrier(5);out->processed=processed;out->start=start;out->initialized=start;out->processed_cycle=vx_rdcycle();out->end=vx_rdcycle();out->complete=RECORD_COMPLETE;
}
