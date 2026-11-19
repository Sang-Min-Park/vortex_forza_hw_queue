#include "field_handler.h"
__kernel void kernel_main(args_t* __UNIFORM__ a){
 uint32_t rank=rank_of(a),workers=a->cores*a->per_core,core=vx_core_id(),local=vx_warp_id()*a->threads+vx_thread_id();auto input=reinterpret_cast<const request_t*>(a->input);auto buckets=reinterpret_cast<request_t*>(a->buckets);auto counts=reinterpret_cast<uint64_t*>(a->counts);auto offsets=reinterpret_cast<uint64_t*>(a->offsets);auto cursors=reinterpret_cast<uint64_t*>(a->cursors);auto state=reinterpret_cast<record_t*>(a->state);auto out=reinterpret_cast<result_t*>(a->results)+rank;out->rank=rank;
 barrier(1);uint64_t start=vx_rdcycle(),processed=0;
 if(a->stage==0){for(uint32_t k=rank;k<a->keys;k+=workers)swap64(counts+k,0);barrier(2);for(uint32_t i=rank;i<a->requests;i+=workers){request_t r=load_request(input,i);add64(counts+r.key,1);}barrier(3);if(rank==0){uint64_t position=0;for(uint32_t k=0;k<a->keys;++k){swap64(offsets+k,position);swap64(cursors+k,position);position+=counts[k];}}barrier(4);
  for(uint32_t i=rank;i<a->requests;i+=workers){request_t r=load_request(input,i);uint64_t p=add64(cursors+r.key,1);if(p>=a->requests){out->error=1;continue;}store_request(buckets,p,r);++processed;}
 }else{auto scratch=reinterpret_cast<uint64_t*>(__local_mem());uint32_t lane=vx_thread_id(),warp=vx_warp_id();FieldHandler handler(a,true);
  for(uint32_t k=(warp<<a->core_shift)+core;k<a->keys;k+=a->cores*VX_CFG_NUM_WARPS){uint64_t begin=offsets[k],n=counts[k];if(n==0&&lane<3)swap64(scratch+uint64_t(k>>a->core_shift)*3+lane,0);
   for(uint64_t i=0;i<n;++i){uint32_t id=handler.apply(a->buckets+(begin+i)*sizeof(request_t));if(id==UINT32_MAX)out->error=2;if(lane==0)++processed;}
  }
  handler.flush();barrier(6);for(uint32_t k=(local<<a->core_shift)+core;k<a->keys;k+=workers){uint32_t offset=(k>>a->core_shift)*3;state[k].count=scratch[offset];state[k].sum=scratch[offset+1];state[k].xor_value=scratch[offset+2];}
 }

 barrier(5);out->processed=processed;out->start=start;out->initialized=start;out->processed_cycle=vx_rdcycle();out->end=vx_rdcycle();out->complete=RECORD_COMPLETE;
}
