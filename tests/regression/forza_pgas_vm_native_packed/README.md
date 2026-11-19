# Packed native controls for programmable destination execution

This isolated follow-up removes the VM engine's 64-byte reservation layout
from the native GPU controls. Global records use 32 bytes including their lock;
GPU owner records use packed 24-byte state in the existing LocalMem. The VM
engine and its driver are unchanged and are not invoked by these kernels.

Both software operations remain identical: old count/sum/xor plus signed32 sum
and XOR update, or old count/min/max plus signed32 min/max update, initially zero.
Every old tuple prefix, final record and worker count is checked. Ordered warp
shuffle helpers preserve full-lane execution for grouping and prefix operations.

Global lock, same-key grouped lock and timed histogram/prefix/scatter owner
controls all run again on the same four frozen programmable-build libraries.
The owner includes GPU routing, LocalMem persistence and export costs; it gets
the benefit of retaining a key in registers. No host buckets are supplied.

Run the generated out-of-tree Makefile/runner after configure. The default
matrix has 24 smoke cases plus 36 primary cases at 64/256 records, 4096 inputs
and random/hotset/hot distributions. No engine speedup should be claimed by
forcing a 64-byte native layout when a legal packed implementation is faster.
