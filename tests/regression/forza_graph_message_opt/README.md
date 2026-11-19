# Message-path optimization experiments

This isolated app keeps the frozen core-combining graph workload, generator,
graph hash, owner-partitioned output, exact `int64` CPU reference, Vortex2
launch geometry, FORZA runtime, simulator driver, and hardware timing model.
It tests explicit software changes to `core_hw_fixed`; it does not assume
either change will beat the ordinary GPU atomic implementation.

All five new kernels cache immutable graph/topology scalars and pointers,
and load each CSR row's end before its inner edge loop. This avoids argument
and row-end reloads forced by the inline AMO's memory clobber in the frozen
implementation. These are joint source optimizations, rather than an isolated
measurement of queues alone.

`MODE=msg_local` retains the same shared scratchpad table and per-edge
`amoadd.d` fold. After the full-core fold barrier, each owner actor directly
adds its own core's contribution to its output. Only contributions from other
cores use FORZA CSR enqueue, hardware queues, connectivity, receive copying,
and the handler. Every remote vector is still sent densely, including zeros.
The publication, receive progress, unique-chunk checks, and outstanding-ACK
test run in one cooperative loop, rather than a send loop followed by another
completion loop. No DONE markers are exchanged. This uses the graph phase's
known incoming counts; it does not close a general asynchronous Selector.

With `C` cores, `P` actors per core, `N=C*P` actors, and `L=ceil(V/N)`, every
actor directly applies one local vector and receives `C-1` remote vectors.
Total local applications are `N`, while total transmitted remote vectors
are `C*(C-1)*P`. Each remote vector uses `ceil(L/6)` 56-byte typed chunks:
`begin`32, `source_core`16, `count`16, and six 64-bit contributions. Source
core and chunk-position masks reject local, duplicated, malformed, or missing
remote chunks. All lanes keep progressing until their whole warp has
published its assigned remote vectors, handled every expected remote chunk,
and has zero outstanding transport requests. A final global barrier closes
the one-shot phase.

`MODE=msg_private` uses the same local bypass and remote delivery loop, but
folds into physically interleaved `[output_word][local_sender]` scratchpad
columns. Each sender has its own logical dense vector; physical index is
`(owner*L + local_vertex)*P + local_sender`. This places hot contributions
from different lanes in different bank columns instead of aliasing every
power-of-two row stride into one bank. This removes shared-hot-word contention
between senders; it retains `amoadd.d` on private words. A plain private
load/add/store loop followed by a fence and CTA barrier would not be safe in
this model: posted local stores can remain in bank queues, while the fence
waits for pending loads/AMOs rather than ordinary-store completion. Private
AMOs keep acknowledged completion without changing hardware or runtime.

All private rows are zeroed with acknowledged swaps inside the timed phase.
After every fold AMO drains and the CTA barrier completes, each output word
has exactly one reducer. That reducer reads all `P` sender rows for the word
and writes its sum into sender column 0 using `amoswap.d`. The combined vector
stays strided by `P` during local application and payload gathering; it is
not compacted over another output word's still-needed inputs. No other output word depends
on that overwritten word. A fence and CTA barrier make column 0 stable before
local application and remote publishing. This in-place reduction avoids
allocating an additional combined table. Its cost is substantial: `P` times
the zeroing footprint and `O(P*V)` reduction work, which can outweigh reduced
contention. Bank conflicts can remain even for different private words. In
the fixed four-bank configuration, the combined column-0 addresses are
eight words apart, so final reduction swaps, local reads, and payload gathers
can still serialize in bank 0; interleaving does not eliminate that cost.

`MODE=msg_warp` uses the shared core table, local bypass, and remote hardware
delivery of `msg_local`, but assigns each source row to a whole warp. Global
warp rank is `warp_id*cores + core_id`, interleaving adjacent source rows
across cores. Warps take cyclic source rows, and their lanes take row edges
`begin + lane, begin + lane + threads, ...`. This reduces the effect of one
high-degree source row being assigned to one lane. The host checks this
different per-worker edge distribution exactly. Graph contents and hash are
unchanged; cross-core-edge counts can change with the source mapping.

`MODE=sw_warp` is a delivery control with exactly that same warp source
mapping, shared scratchpad fold, owner-local bypass, and remote dense vectors.
Its remote vectors are published to global memory with acknowledged swaps,
then the original software tree flags synchronize publishers before owners
reduce only other cores' global vectors. Each local actor publishes its
assigned remote-owner vectors. The full `C*N*L*8` partial grid is allocated,
but same-core entries remain unused: active bytes are `C*(N-P)*L*8`. Software
tree flags occupy `N*8` bytes. This removes a local-bypass advantage from the
hardware-vs-software delivery comparison.

`MODE=gpu_warp` uses exactly the same warp/lane source-edge assignment and
hoisted metadata, with direct `amoadd.d` into conventional contiguous
`y[destination]`. It has no combining table, Selector, FORZA enqueue/dequeue,
handler, tree flags, or DONE. Its final fence drains actual AMO responses and
a global barrier waits for every worker. This control ensures improved
message load balance is compared with improved direct-GPU load balance too.

Scratchpad bytes per core are `N*L*8` for `msg_local` and `P*N*L*8` for
`msg_private`. At the fixed 2-core, 8-actor/core geometry, V256 private rows
use exactly 16KiB, and larger allocations are rejected against device
capacity. Both modes allocate zero global partial-vector bytes and zero
software tree-flag bytes. The three hardware-message modes have these zero
global partial/tree footprints; `sw_warp` uses the global grid and flags
described above, while `gpu_warp` allocates no scratchpad. The graph, runtime stack/outbox, and final output
still use global memory.

The host checks every vertex, per-worker original edge counts, remote sent
vectors/chunks, `C` reduced vectors including the directly applied local
vector, zero DONE, zero outstanding requests, and completion sentinels.
`local_vectors_applied=N` and `partial_rows_per_core` make these differences
explicit in RESULT (`local_vectors_applied=0` for the direct GPU control).
The existing counters retain their meanings except
`max_reduce_cycles` now covers the fold-to-core barrier, optional private-row
reduction, and direct local application. Remote callback reduction overlaps
progress and is included in `max_publish_sync_cycles`. These independent
worker-phase maxima must not be added as one timeline. For `sw_warp`, the
reduction counter also includes remote-array reduction after the tree; for
`gpu_warp`, the fold counter covers direct edge traversal and atomic updates.

Runtime initialization and initial source-row preloads precede the timer.
All scratchpad zeroing, folding, reduction, local bypass, remote delivery,
and phase completion are included. Final Y updates remain ordinary owner
stores. The primary completed-output metric is core 0's
`device_cycles_post_readback_including_flush`, after both result readbacks
and simulator cache flush. Logical phase timestamps alone do not prove
global-store materialization. Each run is a fresh process and one graph
phase; no repeated-phase reuse or general actor-finish protocol is claimed.

Build only this new app with the unchanged matched L1-off/shared-L2 driver:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph_message_opt MODE=msg_local
env -u DEBUG make -C tests/regression/forza_graph_message_opt MODE=msg_private
env -u DEBUG make -C tests/regression/forza_graph_message_opt MODE=msg_warp
env -u DEBUG make -C tests/regression/forza_graph_message_opt MODE=sw_warp
env -u DEBUG make -C tests/regression/forza_graph_message_opt MODE=gpu_warp
```

Each mode saves a separate host binary and `kernel-MODE.vxbin`; measured
builds also save ELF and disassembly. All prior sources and measurements
remain frozen. Results describe this SimX model, including its existing
uncalibrated FORZA SRAM/CSR timing, rather than a physical GPU implementation.
