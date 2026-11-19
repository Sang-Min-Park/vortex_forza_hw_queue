# Graph scatter with software combining

This app ports the three combining algorithms in Mitul Tandon's tracked
`vortex-actor` commit `7ef9d3acf9d34c0df1c8a257fe3bf67759081472`
(`Update actor kernels and add README`, 2026-10-06):

- `tests/actor/actor_histo_agg_amo`: one dense vector for each sender/owner pair.
- `tests/actor/actor_histo_agg_amo_lmem`: same-core vectors in scratchpad and
  cross-core vectors in global memory.
- `tests/actor/actor_histo_agg_comb`: one shared scratchpad vector per core,
  then a global partial-result vector per core.

The source repository is `https://github.com/tandonmitul27/vortex-actor`.
These are arithmetic-combining graph algorithms, rather than an unmodified
generic HClib/Conveyors batching runtime. All inputs and results are identical
to the frozen `forza_graph` benchmark: the same deterministic CSR generator,
seeds, graph hash, and exact per-vertex `int64` weighted-scatter CPU reference.
Every worker enumerates source vertices `rank + k*N`. Each edge contributes
`weight * x[source]`; the owner remains `destination % N`. Output uses the
same owner-partitioned global-memory layout as the actor implementation.

`MODE=agg_amo` folds each edge with `amoadd.d` into its sender-private vector.
Its dynamic CAP=1 ring is a 16-byte header (`head`, `tail`, `sender`, padding)
followed by `L=ceil(V/N)` 64-bit values. A nonzero owner vector is published
once, and that owner reads it once after the software tree barrier.
`MODE=agg_lmem` folds into an on-chip `[local_sender][local_receiver][L]`
grid for same-core destinations and a global `[sender][receiver][L]` grid
for other cores. Every owner reduces all sender vectors, including zeros.
`MODE=agg_comb` folds into one shared `[owner][L]` scratchpad array per core.
Workers cooperatively flush that core's array, and each owner sums its slice
across the cores. This last algorithm has no actor messages.

`MODE=hw_agg` is a control that applies the same sender-private global-vector
folding as `agg_amo`, then sends the combined values through the actual FORZA
Selector and hardware queues. A 56-byte typed packet has an 8-byte index/count
header and six 64-bit contributions. Every nonzero owner vector sends all
`ceil(L/6)` chunks; an entirely zero vector sends none. The owner applies the
chunks and completes the normal all-to-all software DONE protocol. Its
`publish_sync_cycles` includes overlapped handler work; `reduce_cycles=0`
does not imply that reduction was free.

The ports use one persistent CTA per core and one lane per actor, including
the original `agg_amo` variant that previously launched one-thread blocks.
They use the Vortex2 launch API and explicit `launch.lmem_size`; the host
rejects scratchpad allocations above the device's per-core capacity. The
graph reference needs 64-bit rather than the original 32-bit histogram values.
There are no per-edge cycle snapshots (`BD_FINE=0` equivalent).

The software tree algorithm is retained: every actor waits for its binary
tree children, publishes arrival, then waits for release and releases its
children. All lanes keep executing the loop until `vx_vote_all(phase==2)`.
The matched configuration disables private D-cache and enables shared L2,
so volatile flag polling does not depend on unsupported L1 coherence.

Publication differs deliberately from the old plain-store code: tree flags,
CAP=1 header publication/release, and combined cross-core flushes use
acknowledged `amoswap.w`/`amoswap.d` plus fences. Scratchpad zeroing also uses
acknowledged 64-bit swaps and a local barrier. Every fold AMO response drains
before its completion is published. These changes prevent a posted ordinary
store from being mistaken for completed cross-core publication. Scratchpad
zeroing is inside the timed phase. Global vectors, flags, and output are
zeroed by the host before launch; their one-shot reset cost is excluded from
the timed phase, as in the original benchmark. Repeated iterations would
need an explicit reset/reuse protocol.

Logical phase timestamps cover local initialization, edge traversal/folding,
publication and tree synchronization, owner reduction, and a final global
barrier. Final owner output stores remain ordinary stores, as in the source
algorithm and actor baseline. A fence alone does not prove they reached
global memory. Use `device_cycles_post_readback_including_flush` as the primary
completed-result comparison: it is core 0's cycle counter after both result
readbacks and simulator cache flush. It includes launch/setup and output
materialization. The phase counters are diagnostic; maxima of separate
worker phases must not be added as if they formed one worker's timeline.

Host verification checks every vertex, every worker's edge count, published
and reduced vector counts, completion sentinel, and hardware chunks/DONE/
outstanding counts where applicable. Footprints print allocated global bytes,
active global vector bytes, scratchpad bytes per core, and software tree flags.
For `agg_lmem`, same-core entries of the allocated global grid remain unused.
For `hw_agg`, allocated tree flags are unused and footprint output should be
read alongside the hardware model's separate SRAM/runtime allocation.

Build only this app in the existing matched L1-off/A-enabled build:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph_coalesced MODE=agg_amo
env -u DEBUG make -C tests/regression/forza_graph_coalesced MODE=agg_lmem
env -u DEBUG make -C tests/regression/forza_graph_coalesced MODE=agg_comb
env -u DEBUG make -C tests/regression/forza_graph_coalesced MODE=hw_agg
```

Each mode saves its own host binary and `kernel-MODE.vxbin`. Use explicit
`-k kernel-MODE.vxbin` and a fresh host process for every measurement. The
old `forza_graph` files, results, and binary snapshots remain separate.
These are SimX model comparisons; the new FORZA hardware model has not been
validated for physical SRAM ports, banking, area, or RTL timing.
