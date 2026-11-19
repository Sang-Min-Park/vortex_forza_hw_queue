# General GPU-handler record messages, with no arithmetic engine

This experiment uses only opaque generic MMIO message transport. UPDATE and
RECORD hardware flags are absent. The count/sum/XOR handler is ordinary compiled
GPU code. Semantic operation and wide signed32 inputs match the previous record
experiment; all old snapshots remain unchanged.

`software_bulk` assigns each key to an owner warp. Lane0 dequeues an opaque
request pointer and broadcasts it; lanes0/1/2 own count/sum/XOR and lane3 writes
the response-complete field, using the shared FieldHandler. A register cache
retains one key. Actual packed24B LocalMem is initialized and persisted. There
are no reply notifications: host-provided exact handler counts, delivery credits,
a global barrier and final output flush complete this independent bulk phase.
This phase contract also permits native GPU owner/bucket controls.

`software_rpc` assigns each key to one handler thread. It computes in ordinary
GPU code, publishes all three response fields and completion using acknowledged
generic AMOSWAP stores, fences, then sends an opaque notification. The source
actually reads its response and accumulates a host-checked digest. Each producer
has a window of four unreturned requests (at most64 globally). Pending replies
have priority and reply receives continue while one outgoing reply is pending.
No fixed arithmetic reply channel or special record opcode is enabled.

Native controls include normal global locking, optimized same-key warp grouping,
fully timed warp-prefix owner bucketing, and `gpu_field_owner`, which uses the
same field-parallel FieldHandler after device histogram/prefix/scatter. Sorted
field owner can start each first-visited key from known zero registers; untouched
keys are explicitly persisted as zero. All routing, scratch setup, handler math,
publication/copy, local state persistence and global exports are timed.

The host verifies every bulk reply ticket/sum/XOR prefix, every final record,
per-source/per-handler work, notification consumption (RPC), and zero delivery
credits. Exact incoming counts are a benchmark phase-completion contract, not a
general actor finish implementation. Source/guest/library snapshots and logs
are preserved by run_matrix.py, with primary cumulative MCYCLE after readback.
