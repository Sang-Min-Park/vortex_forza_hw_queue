# Warp-cooperative messaging and dedicated instruction comparison

This isolated SimX experiment preserves the count/sum/xor old-tuple operation,
full signed32 input, per-key prefix replay, actual owner LocalMem, and strong
native GPU controls. No workload-specific arithmetic engine is enabled.

`field_mmio8` retains the previous four-lane/one-request GPU handler as a
control. Warp modes load four original requests concurrently, group equal keys
within the quad, reconstruct every old tuple using lane prefixes, and commit
each key once. A one-key register cache handles repeated homogeneous quads;
mixed-key quads flush that cache and use actual acknowledged LocalMem stores.
The same WarpHandler is also invoked by a GPU-generated bucket owner control.

Range sizes8 and32 use an opaque `(count,start+1)` token and original immutable
input. Producers scan ranges to identify destination cores. Receivers scan and
filter their owned requests; ranges touching two owners are scanned twice.
Logical updates and old tuple responses are never eliminated. Classification,
tails, initialization, slot handling, completion, state persistence and exports
are timed. Two four-lane warps/core retain one producer and one service warp.

`warp_mmio*`, `warp_isa*`, and `warp_wait*` share handler and batching code. They
select current MMIO, dedicated SEND/RECV try instructions, or dedicated blocking
admission/receive instructions. Waiting producers have a separate service warp
so they cannot prevent local receives from executing. ISA wait is an admission
contract, not operation completion. Inputs are immutable, and no automatic
release/acquire semantics are inferred from the new opcode.

Each mode uses the same bounded fabric and mailbox SRAM. Batching operations
inside an already received range does not assume a magical RECV4 port. The
finite bulk test supplies exact per-owner work/message counts; this is not a
dynamic actor termination detector. Native global lock, grouped lock and timed
GPU bucket/prefix controls all run on the same frozen machine.

`warp_coop_*32` is an additional producer-mapping intervention: each producer
warp scans one contiguous range cooperatively and lane zero sends its tokens.
The same eight physical producer lanes remain active for key scanning, but
there are two source endpoints rather than eight on the two-core machine.
Destination selection, scanning, retries and drained completion remain timed;
source counts and logical scans are checked against this mapping.

Before the official cohort, a separate per-range helper was evaluated to reduce
register pressure. It passed exact tail/prefix checks but slowed hot MMIO8 by
flushing the cache at each range. That diagnostic candidate remains under the
build diagnostics; the official code retains the structured inlined cache.
