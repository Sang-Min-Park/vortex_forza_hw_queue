# Bounded register combining and balanced edge ranges

This isolated experiment compares four algorithms on the same deterministic
CSR graphs, integer weighted scatter, exact per-vertex CPU reference, and
unchanged SimX driver/configuration. Earlier apps and measurements remain
frozen. The experiment tests a specific candidate; it does not establish that
message passing must beat an optimized ordinary GPU algorithm.

All four modes use balanced contiguous edge ranges per warp. With global
warp `g=warp_id*cores+core_id` and `W=warps_per_core*cores`, the range is
`[floor(E*g/W), floor(E*(g+1)/W))`. Lane `i` processes `lo+i, lo+i+T, ...`.
The host validates each worker with those explicit ranges, including cases
where `E<W` and the final warp owns the only edge. No CPU edge-source array
is generated. Each lane binary-searches CSR row offsets once at its first
edge, then advances its cached source/end through row boundaries and empty
rows. Search, source advancement, and `x[source]` loads are timed.

`msg_cache`, `sw_cache`, and `gpu_cache` share the same noinline fold helper
and two-entry LRU combiner. The cache consists of named scalar tags, sums,
and a most-recently-used bit. A hit adds the weighted contribution in the
cache. A full-cache miss flushes its least-recently-used entry, and the two
remaining valid entries flush at the end. Every valid flush uses `amoadd.d`,
including a valid entry whose sum is zero. `UINT32_MAX` is an invalid tag;
destination zero and zero sums remain valid. The helper takes immutable
pointers/scalars by value to keep its live state separate from the Selector.
The compiled streaming edge/cache loop has no stack loads/stores: tags, sums,
and MRU state remain in registers. The three cached helper instruction
streams are byte-identical. One-time callee-save prologue/epilogue traffic
still occurs and is measured; this is not a claim that the whole kernel
avoids stack memory. The build's `compiled-fold-proof.txt` records the evidence.

`gpu_range` uses the identical range/source traversal without the combiner:
it issues one global `amoadd.d` per input edge. `gpu_cache` gives the ordinary
GPU the same LRU arithmetic optimization as the message variants and flushes
directly into conventional contiguous `y[destination]`. Both GPU modes fence
their actual AMO responses and wait at the final global barrier. They contain
no Selector, FORZA enqueue/dequeue, partial table, handler, or DONE protocol.

`msg_cache` and `sw_cache` flush cache entries into the same contiguous
per-core scratchpad partial array indexed by global destination. The table
is cooperatively reset with acknowledged swaps inside the phase. Fold AMOs
drain before the CTA barrier makes all core partials stable. Each owner
directly applies its own core's contribution once. Only other cores' dense
owner vectors require delivery. Publication gathers vertex positions
`owner + bucket*N`; owner/index arithmetic is outside the edge fold.

`msg_cache` sends remote vectors through the unchanged FORZA Selector, CSR
enqueue/dequeue, hardware queues, and connectivity. It retains the existing
56-byte typed chunks with source-core/chunk-position duplicate detection,
single cooperative send/progress/ACK loop, and known-count one-shot phase
completion. There are no DONE markers or claims of generic Selector closure.
`sw_cache` publishes the same remote vectors with acknowledged global swaps,
uses the same software tree flags as the earlier software control, and then
owners reduce remote partial arrays. Its allocated grid is `C*N*L*8` bytes,
of which `C*(N-P)*L*8` are active; flags use `N*8` bytes. Message delivery has
no global partial array or software tree flags. Both combining variants use
`N*L*8` scratchpad bytes/core, with `L=ceil(V/N)` and `P` actors/core.

For two cores and sixteen actors, the combining variants directly apply
sixteen local vectors and publish sixteen remote vectors. Message mode sends
sixteen chunks at V64 or forty-eight at V256; software mode has zero hardware
chunks. Every owner reduces two vectors including the local contribution.
These dense delivery counts do not depend on cache hit rate. GPU modes have
zero vectors/chunks/local applications and zero scratchpad allocation.

`atomic_flushes` counts actual fold RMW operations caused by cache evictions
and final valid entries, summed across workers. It excludes scratchpad reset,
software publication, tree flags, and final output materialization.
`gpu_range` must report exactly `input_edges`; the three cached modes must
report the same count for the same graph. The host independently replays
each worker's tag/LRU sequence and verifies both its original edge count and
flush count, in addition to every output vertex and all delivery/completion
counters. This is a diagnostic oracle, not preprocessing supplied to kernels.

Runtime initialization precedes timing. Local reset, initial CSR searches,
traversal, cache branches, flushes, local application, remote delivery and
completion are included. `max_reduce_cycles` includes the fold-to-core
barrier and local application; software mode also includes its remote-array
reduction. Message callback reduction overlaps progress and belongs to
`max_publish_sync_cycles`. Independent worker-phase maxima must not be added
as one timeline. Final output owner stores remain ordinary stores in the
combining variants, so the primary completed-output comparison is core 0's
`device_cycles_post_readback_including_flush`, after all output/result
readbacks and simulator cache flush. Phase timestamps are diagnostic.

Build the new app only, with the same existing L1-off/shared-L2 driver:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph_register MODE=msg_cache
env -u DEBUG make -C tests/regression/forza_graph_register MODE=sw_cache
env -u DEBUG make -C tests/regression/forza_graph_register MODE=gpu_cache
env -u DEBUG make -C tests/regression/forza_graph_register MODE=gpu_range
```

Use separate saved binaries and `kernel-MODE.vxbin` in fresh processes.
Performance is relative to the current model, not proof about physical GPUs
or calibrated FORZA RTL. Per-case comparisons against the best measured
ordinary GPU variant are retrospective selections and must be labeled so.
