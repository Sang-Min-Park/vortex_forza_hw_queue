#pragma once
#include <stdint.h>
struct MockDim { uint32_t x, y, z; };
inline MockDim gridDim{2, 1, 1}, blockDim{8, 1, 1};
inline int vx_num_cores() { return 2; }
inline int vx_num_warps() { return 2; }
inline int vx_num_threads() { return 4; }
inline int vx_core_id() { return 0; }
inline int vx_warp_id() { return 0; }
inline int vx_thread_id() { return 0; }
inline int get_cluster_size() { return 1; }
inline bool vx_vote_all(int value) { return value; }
inline bool vx_vote_any(int value) { return value; }
uint64_t csr_read(uint32_t address);
void csr_write(uint32_t address, uint64_t value);
#define __syncthreads() ((void)0)
