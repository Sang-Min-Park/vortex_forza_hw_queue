#pragma once
#include "device.h"
struct snapshot_t {uint64_t count,sum,xor_value;};
// One executing GPU thread owns every key mapped to its fixed handler lane.
// The hardware sees opaque pointers and performs no record arithmetic.
class RecordHandler {
public:
 explicit RecordHandler(uint32_t core_shift):shift_(core_shift),memory_(reinterpret_cast<volatile uint64_t*>(__local_mem())){}
 snapshot_t apply(const request_t&r){if(key_!=r.key){flush();key_=r.key;uint32_t offset=(key_>>shift_)*3;count_=memory_[offset];sum_=memory_[offset+1];xor_=memory_[offset+2];}snapshot_t old{count_,sum_,xor_};++count_;sum_+=uint64_t(int64_t(r.delta));xor_^=r.id;return old;}
 void flush(){if(key_!=UINT32_MAX){uint32_t offset=(key_>>shift_)*3;memory_[offset]=count_;memory_[offset+1]=sum_;memory_[offset+2]=xor_;vx_fence();key_=UINT32_MAX;}}
private:
 uint32_t shift_,key_=UINT32_MAX;volatile uint64_t*memory_;uint64_t count_=0,sum_=0,xor_=0;
};
