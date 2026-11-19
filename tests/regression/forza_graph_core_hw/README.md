# Core combining followed by FORZA hardware queues

This app combines contributions from every actor on a core in one scratchpad
table, then delivers the finished core partials through the FORZA Selector
runtime and hardware message queues. It is separate from the frozen
`forza_graph` and `forza_graph_coalesced` apps and reuses their graph generator,
hash, `int64` CPU reference, owner output layout, and shared result/argument ABI.

The first stage follows Mitul Tandon's core-combining histogram algorithm in
`vortex-actor`, commit `7ef9d3acf9d34c0df1c8a257fe3bf67759081472`,
`tests/actor/actor_histo_agg_comb/kernel.cpp`. Every lane is an actor; one
persistent full-core CTA shares `[destination_owner][local_vertex]` partials.
Cooperative acknowledged `amoswap.d` zeroing, a fence, and a local barrier
precede the graph traversal. Source vertices are cyclically distributed across
all actors. Each weighted edge uses `amoadd.d` into its core's shared table.
Every worker fences its fold operations, then the CTA barrier waits until all
of that core's partial contributions are complete.

The next stage replaces the software variant's global partial-array flush,
software tree barrier, and global-array reduction. Local actor `i` publishes
owners `i, i+P, i+2P, ...`, where `P` is actors per core. Each finished owner
vector is read directly from scratchpad and sent as 56-byte typed payloads:
an 8-byte header (`begin`32, `source_core`16, `count`16) followed by up to six
64-bit contributions. Each receiver tracks one chunk-position bitset per
source core, rejects duplicate chunks, and checks that every core's mask is
complete. The host limits vectors to 64 chunks; a full 64-bit mask is handled
without a shift by 64.
Every core sends every owner's dense vector, including zeros. For `C` cores
and `N` actors, this is `C*N` vectors and `C*N*ceil(L/6)` data messages, with
`L=ceil(vertices/N)` values per vector. There is no intermediate global
partial-vector array and no software tree-flag allocation.

Every actor keeps calling `Selector.progress()` while its warp has pending
publishers. The recipient handler adds each chunk into its own output slice.
`Selector.done()` then sends the normal `N*N` DONE markers and drains all
mailboxes and outstanding transport acknowledgements. A final fence and
global barrier close the logical phase. This retains the current runtime's
all-to-all DONE cost, unlike the software tree-barrier variants. Same-core
and cross-core combined vectors both use the hardware message path.

`MODE=core_hw_fixed` keeps exactly the same shared-core fold, owner assignment,
payloads, queues, and handler. It uses the known one-shot graph-phase counts:
every owner must receive exactly `C*ceil(L/6)` chunks, and each worker waits
until all its assigned vectors were published, all expected incoming chunks
were handled exactly once, and its transport outstanding count is zero.
All lanes continue progressing until their whole warp satisfies these
conditions, then the final global barrier completes the phase. This mode
does not call `request_done()` or `done()`, sends zero DONE markers, and does
not claim `Selector.is_done()` or generic actor-runtime closure. It is a
one-shot application-specific completion control, not a repeated-epoch or
general asynchronous-actor termination protocol.

The host checks every output vertex, each worker's original edge count,
distributed vector/chunk counts, `C` completed vectors at every recipient,
DONE from all actors in normal mode (zero in fixed-count mode), zero
outstanding requests, and completion sentinels.
Scratchpad allocation is `N*L*8` bytes per core and is checked against the
device capacity with explicit Vortex2 `launch.lmem_size`. The configured
two-core, eight-actor/core launch sends 32 vectors, 32 data chunks at V64,
and 96 chunks at V256, plus 256 DONE markers.

Runtime initialization and the first source-row preload precede the timer.
Scratchpad zeroing, graph traversal, local atomic contention, publishing,
handler work, DONE, and final synchronization are included. `max_fold_cycles`
ends after that worker's fold AMOs drain; the subsequent core barrier belongs
to `max_publish_sync_cycles`. Handler reductions overlap progress, so
`max_reduce_cycles=0` does not mean the reductions cost nothing. Separate
worker-phase maxima must not be added together.

Final output updates use ordinary owner-only stores, just like the actor and
software-combining variants. A fence does not by itself prove global-memory
store completion. The primary comparison is
`device_cycles_post_readback_including_flush`, queried from core 0 after all
output/result readbacks and simulator cache flush. The phase timestamps are
logical-completion diagnostics. Global output reset occurs before launch and
is excluded from the phase; scratchpad reset is included. Zero
`global_vectors_bytes` means that no partial-vector array is allocated, not
that the graph, runtime stack/outbox, or final output avoid global memory.

Build with the same existing A/FORZA-enabled L1-off, shared-L2 SimX driver:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph_core_hw
env -u DEBUG make -C tests/regression/forza_graph_core_hw MODE=core_hw_fixed
cd tests/regression/forza_graph_core_hw
VORTEX_DRIVER=simx LD_LIBRARY_PATH=../../../sw/runtime ./forza_graph_core_hw -k kernel-core_hw.vxbin -v64 -e256 -gskew -s17
```

The app saves `kernel-core_hw.vxbin` and `kernel-core_hw_fixed.vxbin` with
separate host binaries; measured builds also snapshot their ELF
and disassembly. All comparisons use fresh processes and unchanged driver
libraries. These SimX results are relative to the current model, whose FORZA
SRAM ports, banking, area, and physical RTL timing have not been validated.
