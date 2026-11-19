# Actual local state, CSR/MMIO messaging, and native GPU controls

All 42 runs passed exact state and per-actor checks. Hardware owner queues were
faster than the controlled software owner queues, but neither message interface
beat direct native GPU atomics. Moving live owner state to the real local SRAM
also did not improve this particular workload in the current SimX model.

## Implemented behavior

Each actor owns an int64 array. A logical address is `{owner, offset}`. Its owner
can read/update it directly; another actor sends an active update message carrying
`{offset32, delta32}`. Core equals zone, and each hardware thread equals actor.
This is software PGAS naming plus active messages. It does not make another
core's local SRAM accessible by an ordinary program load/store.

The local variants use `__local_mem()` through Vortex's actual scratchpad LSU
path. This is per-core shared SRAM with private actor partitions, rather than a
per-thread stack variable backed by global memory. With eight actors/core:

```
byte_address = __local_mem() + 8 * (offset * 8 + local_actor)
```

Every core keeps 256 int64 words/actor: exactly 16,384 bytes/core, or 32KiB over
two cores. A larger logical owner array keeps that prefix in SRAM and explicitly
spills the remaining offsets to global cache/DRAM backing. The physical end of
the allocated local region is byte 16,383; it is not an unbounded local store.

The experiment implements seven variants:

| Variant | Live state and update path |
| --- | --- |
| CSR/global | Owner handles CSR-delivered messages in global state. |
| CSR/local | Identical owner algorithm with a local SRAM prefix. |
| MMIO/global | Same owner algorithm, memory-interface enqueue/pop, global state. |
| MMIO/local | Same memory interface, local SRAM prefix. |
| Software owner/local | Same local owner handler; global SPSC ring per producer/destination pair. |
| Native local | Same local layout: same-core resident updates use LMEM AMOs; other updates use global AMOs. Owners pull resident remote partials after a barrier, then export. |
| Native global | One global AMO per input; retained as a strong native control. |

The four hardware variants form a 2x2 interface/placement ablation. Native local
never accesses another core's scratchpad. Native global is permitted to use the
native programming model for the same mathematical result; the comparison does
not force it through a message runtime.

The root-owned MMIO transport carries one opaque 64-bit (8B) payload per
operation, including zero and high-bit values. The physical message/slot format
remains 64B. Its actor-specific endpoint address makes
per-lane state visible to LLVM divergence analysis; see the
[interface definition](forza_mmio.md) and
[uniformity correction](forza_mmio_uniformity.md).
CSR receives use the [ordered release dependency](forza_explore_ordering_bug.md).
The two interfaces do not change the state operation or logical ownership.

## Workload, checks, and capacity

The workload is weighted in-degree / irregular scatter: each edge has source
`edge_index % 16`, destination `key`, and signed weight `delta`; it contributes
`state[key] += delta`. It is one finite phase, not a full graph application or
generic termination benchmark. Input requests are the same 8B array in every
variant, and every final word, including untouched zeros, is checked against an
uncombined CPU reference.

The 28 functional runs comprise seven variants at 0, 1 and 128 updates with
32KiB state, plus seven 256-update runs with 128KiB state. The spill runs have
32KiB resident and 96KiB globally backed state. For example, the MMIO/local spill
run routes 65 updates to resident offsets and 191 to spilled offsets; both parts
pass the same exact CPU check. These are functional spill checks, not claims of
DRAM capacity thrashing.

Per-worker input, send, receive, direct and native-AMO counts are checked, as are
zero outstanding messages. Software rings additionally validate every final
sequence against its exact source/destination message count. Input hashes and
final checksums agree across all seven variants of each dataset. All 45 captured
source, shared-library and binary artifacts remained unchanged during the matrix.
The [independent analysis](forza_pgas_analysis.json) hashes all 42 logs and
rechecks dataset agreement.

Local placement still has a full global snapshot/backing allocation for host
verification. Its resident prefix is an output snapshot for owner messaging; for
native local it also stores remote partials while processing. It is not hidden
or subtracted from memory usage:

| Resource for the 32KiB state case | Allocated/used size |
| --- | ---: |
| Logical state | 32KiB |
| Live local state in local variants | 16KiB/core, 32KiB total |
| Live spill in local variants | 0 |
| Global state snapshot/backing allocation in every variant | 32KiB |
| Native-local global remote-partial prefix | 32KiB within that allocation |
| Input edges for 4,096 updates | 32KiB |
| Per-actor results | 2,304B |
| Software SPSC rings, capacity four | 64KiB global allocation |
| Software private cursor arrays | 4KiB logical stack storage |
| Hardware mailbox SRAM | 8KiB/core, separate from state LMEM |

Queue slots are 64B aligned, with payload and sequence in the same 64B sector.
A bijective slot/channel permutation avoids putting every active queue poll on
the same L2 bank. Sequence AMOs provide coherent polling. Same-sector payload
publication and LSU-fenced sequence release stay within the memory ordering
domain. The software baseline uses no global producer lock or MPSC reservation
atomic. It is an optimized per-message SPSC baseline, not the earlier Mitul
coalescing implementation.

## Measured cycles

Configuration is RV64, two cores, two warps/core, four lanes/warp, private D-cache
disabled, shared 1MiB L2, 16KiB LMEM/core, and the same enabled hardware-queue/MMIO
model for every variant. Both performance datasets have 4,096 edges and 4,096
destination words. Random input hash is `3fb6d8f59f4ebfd7`; the skewed input hash
is `7bb356562c5a27fe` and concentrates 90% of destinations in a small hot subset.

Cycles below are core-0 device cycles after full state/results/queue readback and
cache flush. Host initialization/transfers are excluded. Required local-state
initialization, owner processing, and final snapshot export are included.

| Variant | Random cycles | Skewed cycles |
| --- | ---: | ---: |
| Native global | 50,351 | 50,594 |
| Native local | 199,254 | 183,866 |
| CSR/global | 624,552 | 5,224,915 |
| CSR/local | 756,765 | 5,871,490 |
| MMIO/global | 628,498 | 5,175,858 |
| MMIO/local | 846,574 | 6,657,897 |
| Software owner/local | 1,183,247 | 14,833,119 |

With identical local state, CSR hardware queues are **1.56x faster** than
software owner queues on random input and **2.53x faster** on skewed input.
MMIO hardware queues are **1.40x** and **2.23x faster** than that software control.
The best native implementation remains native global: CSR/local takes 15.03x and
116.05x its cycles; MMIO/local takes 16.81x and 131.59x.

MMIO/global and CSR/global are within about 1% on these points, with no consistent
MMIO win. MMIO/local takes 11.9% and 13.4% more cycles than CSR/local. These values
do not support a claim that replacing CSR access with this MMIO interface alone
improves throughput.

Actual local-memory event counts verify the placement: each local owner variant
records 8,192 LMEM reads and 8,192 LMEM writes for 4,096 updates, including state
initialization and export. Global owner variants record zero LMEM data accesses.
The changes did not merely rename a global-memory array.

## Interpretation and limits

The two-core resident state totals only 32KiB, far below the 1MiB shared L2. A
working set larger than L2 that also fits all owner SRAM is impossible at this
capacity/core count. This experiment therefore tests transport and local-state
placement for already-L2-fitting state. The spill case reports global backing
honestly; it does not turn spilled state into SRAM or force cache misses.

The skewed case exposes owner congestion: CSR/local has 36,224 failed send
attempts and 75,495 receive polls for 3,841 delivered messages; software owner
queues have 52,979 retries and 216,542 polls. These measurements demonstrate
substantial queue/runtime overhead. They do not by themselves isolate a single
cycle-level bottleneck.

The mandated interleave spreads actors across four banks, and the owner variants
have zero measured LMEM bank stalls. Different offsets of one actor retain the
same bank. Native-local hot destinations can therefore alias one bank (1,842
measured stalls); its result is not a universal claim about every possible local
memory layout. The strong native-global control remains available and fastest.

Global backing is host-zeroed before launch; local SRAM must be initialized in
the kernel. Native local also pays its real partial-pull/export costs. Individual
initialization, processing and export spans are logged as secondary metrics.
L2/DRAM counters cover input, stacks, protocol, partials and output together,
rather than isolated application-state misses. The model's cache/atomic timing
and device queue ports are not calibrated physical GPU predictions. No RTL
implementation, synthesized area, or physical performance is established here.

## Reproduction artifacts

- [Benchmark source and protocol](../../tests/regression/forza_pgas_explore/README.md)
- [Commands, configurations, hashes and all 42 runs](forza_pgas_l1_off.json)
- [Original per-run CSV](forza_pgas_l1_off.csv)
- [Enriched CSV with pattern and state checksum](forza_pgas_enriched.csv)
- [Independent hash/checksum verification and ratios](forza_pgas_analysis.json)
- [Read-only source review](forza_pgas_source_audit.json)

The frozen executable, guest, ELF and disassembly for every mode are under
`build64-forza-pgas/tests/regression/forza_pgas_explore/artifacts/`.
