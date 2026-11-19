#include "field_handler.h"
#include "transport.h"

// Separate functions keep handler cache values out of producer retry scopes.
__attribute__((noinline)) static void produce(args_t* __UNIFORM__ a,result_t*out){
 const uint32_t lane=vx_thread_id(),core=vx_core_id(),n=a->requests,clients=a->cores*4,mask=a->cores-1,limit=a->limit;
 const uint64_t input=a->input,count_address=transport::endpoint(FORZA_MMIO_TX_COUNTS);
 uint32_t cursor=core*4+lane;uint64_t sent=0,retries=0,target=0,payload=0,credit=0;bool loaded=false,complete=false;
 for(uint32_t iteration=0;iteration<limit;++iteration){if(cursor<n){if(!loaded){uint32_t key=uint32_t(reinterpret_cast<const uint64_t*>(input)[uint64_t(cursor)*2]);target=FORZA_MMIO_BASE+(uint64_t(key&mask)<<17);payload=input+uint64_t(cursor)*16;loaded=true;}uint64_t status=swap64(reinterpret_cast<uint64_t*>(target),payload);if(status==0){cursor+=clients;++sent;loaded=false;}else if(status==1)++retries;else out->error=2;}
  bool done=false;if(cursor>=n){credit=transport::load(count_address)&255;done=credit==0;}uint32_t __UNIFORM__ all_done=vx_vote_all(done);if(all_done){complete=true;break;}
 }
 if(!complete)out->error=3;out->sent=sent;out->retries=retries;out->outstanding=credit;
}

__attribute__((noinline)) static void serve(args_t* __UNIFORM__ a,result_t*out){
 const uint32_t lane=vx_thread_id(),core=vx_core_id(),per_core=a->per_core,mask=a->cores-1,limit=a->limit;
 const uint64_t expected=reinterpret_cast<const uint64_t*>(a->counts)[core*per_core],input=a->input,bytes=uint64_t(a->requests)*16,pop_address=transport::endpoint(forza_mmio_pop_address(0));
 uint64_t handled=0;bool complete=false;FieldHandler handler(a);
 for(uint32_t iteration=0;iteration<limit;++iteration){if(handled==expected){complete=true;break;}uint64_t pointer=0;if(lane==0)pointer=swap64(reinterpret_cast<uint64_t*>(pop_address),0);uint64_t __UNIFORM__ descriptor=vx_shfl_idx(pointer,0,VX_QUAD_CVAL,VX_QUAD_MASK);
  if(descriptor){if(descriptor<input||descriptor-input>=bytes||((descriptor-input)&15))out->error=4;else{uint32_t id=handler.apply(descriptor);if(id==UINT32_MAX||(handler.key()&mask)!=core)out->error=5;}++handled;}
 }
 if(!complete&&handled==expected)complete=true;if(!complete)out->error=6;handler.flush();out->processed=lane==0?handled:0;out->received=lane==0?handled:0;
}

__kernel void kernel_main(args_t* __UNIFORM__ a){
 const uint32_t core=vx_core_id(),local=transport::local(),rank=rank_of(a),words=(a->keys>>a->core_shift)*3,per_core=a->per_core,shift=a->core_shift,records=a->keys>>a->core_shift;
 auto out=reinterpret_cast<result_t*>(a->results)+rank;auto scratch=reinterpret_cast<uint64_t*>(__local_mem());auto state=reinterpret_cast<record_t*>(a->state);out->rank=rank;
 if(!transport::init(1))out->error=1;for(uint32_t i=local;i<words;i+=per_core)swap64(scratch+i,0);barrier(1);uint64_t start=vx_rdcycle();
 if(vx_warp_id()==0)serve(a,out);else produce(a,out);
 if(!transport::healthy())out->error=7;barrier(2);uint64_t finish=vx_rdcycle();for(uint32_t offset=local;offset<records;offset+=per_core){uint32_t key=(offset<<shift)+core;state[key].count=scratch[offset*3];state[key].sum=scratch[offset*3+1];state[key].xor_value=scratch[offset*3+2];}barrier(3);out->start=start;out->initialized=start;out->processed_cycle=finish;out->end=vx_rdcycle();out->complete=RECORD_COMPLETE;
}
