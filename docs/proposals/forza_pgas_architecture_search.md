# PGAS and asynchronous operation execution

The programming requirement is owner-addressed data plus asynchronous messaging.
An actor need not be a physical thread, and a fixed-size operation need not run a
GPU receive handler. This experiment explores those choices without changing
the native cache or atomic timing to manufacture a speedup.

## Candidates

1. Native global AMOs remain a strong baseline for commutative updates.
2. A core-cooperative scratchpad combine/reduce is a software baseline where its
   state fits the same physical local-memory capacity.
3. Software message handlers may own a shard cooperatively at thread, warp, or
   core granularity. Logical ownership and physical queue endpoint identity are
   separate. ADD permits parallel processing of independent keys; this does not
   permit arbitrary stateful actor handlers to execute concurrently.
4. A destination ADD engine consumes owner-addressed requests and submits real
   `MemReq` atomics through an additional client of the existing `LocalMem`.
   Its hybrid variant performs same-core updates directly and sends remote ones.

The fixed initial machine is two cores, two four-lane warps/core, four SRAM banks
and 16KiB SRAM/core. A logical actor grouping change does not increase physical
ALU lanes, thread contexts, local-memory banks/capacity, or fabric bandwidth.
This is not a physical warp-width sweep. Such a sweep would have to account for
the parameters derived from `VX_CFG_NUM_THREADS` and the current FORZA full-width
SFU requirement.

## Destination engine

`VX_CFG_FORZA_UPDATE_ENABLE` is default-off and requires the existing opt-in MMIO
and A extensions. A new address window starts at `0x71400000`. Each 16KiB zone
stride represents 2,048 int64 owner words; the operand is a full 64-bit addend.
`amoswap.d` submits a request and returns ACCEPTED/BUSY/INVALID. It does not
return the previous target value. Unsupported widths and reserved addresses are
rejected.

Accepted requests retain the existing bounded TX/fabric and sender credit path.
Opcode `0x80` uses the destination field as a local word offset, not a registered
mailbox PE. The destination has a 32-entry operation queue and at most 32
in-flight tags. It issues at most one actual LocalMem AMO request per cycle.
No software mailbox registration, ready polling, payload POP, or receive handler
is required for these operations.

The engine reaches local SRAM only through `update_req_out`/`update_rsp_in`
channels. A new LocalMem crossbar client is appended after existing clients;
CPU requests and engine requests compete for the same banks. The existing
LocalMem atomic occupancy, read-after-write rules, capacity, and data path are
unchanged. This adds a crossbar input and control/queue hardware, not SRAM banks
or capacity. It is a SimX design experiment with no synthesized PPA claim.

An ACK is generated only after the LocalMem operation response. Therefore
draining these operation credits establishes operation completion. This differs
from the existing generic message ACK, which only establishes READY delivery.
An update-only phase can drain all sources and barrier before exporting results.
That finite reduction protocol does not implement generic FaBSP `finish` for
handlers that may generate more messages or replies.

## Comparison contract

All modes must consume identical input records and produce identical checked
int64 results. Initialization, combining, partial publication, required barriers,
and final export must be included in device timing. Whole-device cycles after
readback/cache flush are primary; host wall time is not GPU speedup.

Local state and sender combine tables share the same 16KiB/core. A case that
cannot fit must be rejected or explicitly use timed global backing, rather than
silently allocating additional local SRAM. Best-native comparisons retain both
direct AMOs and legal combining. Uniform, skewed, and single-hot-key inputs test
different contention; seeds and state capacity must be visible in results.

The previous 112-run matrices and their binaries remain frozen. Pre-engine
model sources are preserved at
`build64-forza-arch/snapshot/base_model/manifest.json`. The new feature uses a
fresh out-of-tree build and must not overwrite old driver libraries.

## Relation to FaBSP and other PGAS systems

The local [FaBSP definition](../../../hclib-actor/doc/markdowns/details/fabsp.md)
interleaves computation, messaging, and handlers. The
[actor/PGAS paper](https://arxiv.org/abs/2107.05516) motivates automatic aggregation
and termination rather than requiring a particular hardware thread mapping.
The reduction engine is a specialized fast path within this design space, not
a port of that complete runtime.

Owner-addressed remote atomics and cooperative communication are also valid
PGAS interface choices: see the primary
[NVSHMEM atomic API](https://docs.nvidia.com/nvshmem/api/latest/gen/api/amo.html)
and [cooperative device API guidance](https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/apis.html).
These references motivate alternatives; their NVIDIA performance is not used to
predict Vortex results.

## Completed bounded search

The [measured comparison](forza_pgas_arch_comparison.md) includes seven
implementations: direct GPU global AMOs, native local/global hybrid, native
core combining, destination-engine hybrid, destination-engine core combining,
and paired GPU/engine one-entry register run combining. The physical machine
remains two four-lane warps/core. Logical ownership is now one data shard/core;
the engine does not need a software actor or a receive thread at each address.
No physical warp-size sweep or generic cooperative actor runtime was claimed.

There were 91 fresh-process cases: **85 exact passes and six capacity N/A**.
All six N/A cases are the two dense-combine modes at 32KiB total state, where
the required local arrays exceed the existing SRAM budget. The paired register
fold needs no dense table and remains valid at that state size. CPU references
check every result word, per-producer work/publication counts, and completion.
The [freeze manifest](forza_pgas_arch_final_freeze.json) verifies all captured
sources, shared libraries and binaries remained unchanged during measurement.
The destination engine's [separate contract tests](forza_update_engine.md) pass
normal and address/undefined-behavior sanitizer runs against real LocalMem.

For 1KiB single-hot state and 4,096 inputs, direct native GPU takes 52,262
cycles; core-combined GPU takes 55,118; engine hybrid takes 264,902; combined
engine takes 60,269. Register folding lowers 4,096 inputs to 16 flushes, but
takes 72,495 cycles on GPU and 84,316 on the engine. Reducing operation count
does not automatically offset comparison/branch/scan and state export costs.
At 32KiB/hot, register folding improves engine 329,295 to 149,025 cycles, while
direct native GPU takes 52,338. Native direct AMOs remain best in all nine
primary size/pattern cases in this fixed SimX model.

The previous per-thread CPU mailbox handler is no longer required for the new
ADD path. The existing message/fabric implementation is reused as transport;
unused generic mailbox SRAM is still instantiated in this prototype. Therefore
this experiment does not claim the minimum possible hardware footprint.

## Design judgment and limits

The supported evidence favors an operation-aware PGAS interface: use direct
native remote-memory operations for simple legal atomics, and retain messages
for arbitrary handlers, replies, or operations for which an atomic is
insufficient. A specialized SRAM engine and source combining are selectable
backends, rather than a requirement that every ADD visit a software mailbox.
This is a proposed runtime policy, not a completed automatic backend selector.

Generic handlers must preserve their state and ordering contract. Combining is
legal only for a declared reduction. General FaBSP termination must also account
for active handlers, software partial batches and handler-generated messages;
the update-only ACK drain here is not that protocol.

An [independent source audit](forza_atomic_timing_scope.md) found that the SimX
LLC atomic commit path does not model all compute/writeback/admission stages
present in the current RTL. The SRAM engine inherits the existing LocalMem
bank occupancy. No timing was changed or result multiplied by a correction
factor. These results rank the tested implementations in the frozen SimX
model, not a universal or physically calibrated optimum. Broader architecture
decisions require the LLC parity investigation and workload-specific evidence
for more than this cached additive scatter.

The subsequent [same-guest native RTL probe](forza_atomic_rtl_probe.json)
passed both global/local atomic checks on both drivers. Global AMOADD.W took
5,093 SimX versus 8,581 RTL cycles with identical 1,164 instructions; local
AMOADD.W took 4,839 versus 4,956 cycles with identical 1,258 instructions.
This confirms a material global-path timing discrepancy in that small test,
without measuring an engine RTL speedup or a transferable correction factor.
The original full64 MMIO message/remote GET/UPDATE smoke was also rerun against
the new UPDATE-enabled driver and passed with all 16 actors, proving that the
existing generic message path remains usable beside the operation engine.

## Owner-address arithmetic follow-up

A final compiled-code check found a dynamic `divuw` in per-record core routing.
The [isolated power-of-two follow-up](forza_pgas_arch_pow2_comparison.md) keeps
the exact owner mapping but precomputes a shift and uses mask/shift addressing.
It also removes redundant software address checks after host bounds validation;
hardware validation is unchanged. Its 55 cases give 49 exact passes and six
capacity N/A. All 19 applicable native control cases have identical binaries
and exact cycles. No new dump contains DIV/REM, but the bundle only improves
the engine by roughly 1–2% in representative cases: 1KiB/hot combined engine
is 59,846 cycles versus the unchanged native 52,262. The source-level division
was therefore not the large measured bottleneck in this SimX model.

Across all three new cohorts there are **134 exact passes and 12 capacity N/A
out of 146 cases**. Sources and binaries for the earlier cohorts remain frozen.
The actual native `.D` scatter guest was additionally run unchanged on RTL:
52,262 SimX versus 80,727 RTL cycles, with identical checked input/output.
This is documented in the timing audit; it is not permission to compare an
engine SimX result directly to native RTL and claim a win.
