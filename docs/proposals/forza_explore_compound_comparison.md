# Compound transactions with message ownership and direct GPU execution

The message path did **not** beat the strongest measured GPU implementation in
these tests. Making the operation a linearizable three-field transaction, or
making the touched state exceed the 1 MiB L2, did not reverse the result. This is
a result for the current SimX implementation and the tested inputs, not a proof
that message passing cannot win on another workload or architecture.

## What the operation requires

Each 16-byte request supplies `(key, request_id, delta)`. It must return one
consistent old `(count, sum, xor)` snapshot, then update all three fields. A key's
requests may execute in any order, but every returned snapshot must belong to a
valid serial order. Three independent field atomics alone do not satisfy this
contract.

The CPU checks every response: sort each key's requests by their returned count,
require unique consecutive tickets, reconstruct the exact sum/xor prefix in that
order, and check every final record and lock. All **30 official runs passed**:
12 primary measurements and 18 uniform/hot/zero-input smokes. Counters, worker
counts, and output snapshots are checked, rather than just message totals. Three independently recorded L1-off hot probes
also reproduce every exported RESULT field exactly for all three implementations;
the preserved original and official MP device images are byte-identical.

## Matched implementations

- **MP** uses the dedicated hardware queues with one actor per lane and deterministic
  `key % 16` ownership. The optimized test backend sends two payload words,
  polls once per receive attempt, bypasses self-owned requests, and uses known
  one-shot incoming counts instead of all-to-all DONE messages. Send, receive,
  and ACK progress are interleaved. The owner caches its last record and flushes
  on key changes.
- **GPU lock** operates directly on 64-byte records. A warp performs one lock
  attempt per pending lane per cooperative round; winners finish and release
  before the next round. There is no divergent spin-acquisition loop. The lock
  and fields share a cache sector; acquire/release ordering was audited for both
  measured cache settings. Old snapshots are written after releasing the lock.
- **GPU owner buckets** is a stronger ownership control than comparing only
  against locks. The first GPU kernel routes requests into producer-private,
  per-owner buckets. A second kernel drains them using the **same owner-state
  helper as MP**. GPU routing, both launches, and their cache flushes are timed.
  There is no CPU pre-bucketing or free routing stage.

Every implementation writes responses into the same unique global output slots;
MP is not charged a reply packet absent from the GPU controls. The GPU-owner
implementation reserves `16 × requests × 16 B` for buckets but touches only
GPU-produced prefixes. Reserved capacity and actually touched sectors are
reported separately in the data files.

## Results

All entries are cumulative device cycles queried after output readback and cache
flush; smaller is better. For GPU owner buckets this includes **both kernels**.
The separate `final_stage_phase_span_cycles` field describes only its apply
kernel and is not used as its total-time comparison.

| Cache setting | Input | MP | GPU lock | GPU owner buckets | MP / best measured GPU |
|---|---|---:|---:|---:|---:|
| L1 off, shared L2 | 32 keys, 4,096 hot requests | 8,467,958 | **1,405,128** | 3,018,393 | 6.03× slower |
| L1 on, shared L2 | Same hot input | 8,429,160 | **1,427,978** | 3,028,020 | 5.90× slower |
| L1 off, shared L2 | 32,768 keys, 65,536 uniform requests | 22,038,048 | **3,388,063** | 7,217,509 | 6.50× slower |
| L1 on, shared L2 | Same uniform input | 22,385,949 | **3,478,101** | 7,321,988 | 6.44× slower |

The capacity input allocates **2,097,152 B of state** and actually addresses
**1,817,344 B of distinct 64-byte state sectors**: it is a genuine touched-state
footprint above the 1,048,576 B L2. It also reads 1 MiB of request input and writes
2 MiB of response output. The bucket control reserves 16 MiB of bucket payload
capacity and touches 1,054,912 B of bucket sectors, plus 2,048 B of lengths. These
are footprint measurements, not claimed network bytes or cache-miss counts.

The hot input sends about 90% of requests to key zero; it is not a uniform
contention benchmark. Its exact hash is `29f18d4ecbe0ce03`; the capacity input's
hash is `2ae09b2c68c57d34`. Every mode in a matched row uses the same requests,
positive deltas, IDs, record layout, and CPU semantic checks.

## What the measurements support

The native GPU lock path wins all four primary comparisons, despite doing a
compound transaction. MP also remains about three times slower than the
GPU-owner bucket control. Thus the result is not obtained by leaving out an
ownership-based native GPU alternative.

There is evidence of instruction/progress cost beyond memory traffic. In the
L1-off capacity case, MP retires 6,768,467 instructions versus 1,066,029 for GPU
lock, about 6.35 times as many. MP reports 102,580/85,899 DRAM reads/writes versus
106,118/89,617 for GPU lock; modestly fewer requests did not offset the instruction
and queue/progress cost. Those counters cover the stated all-stage post-readback
window; they are not pure handler-stage traffic.

Hot-key ownership funnels work through one receiving actor and its small final
mailbox. In that case MP records 37,603 failed-ready send attempts for 3,852
non-self messages. This is direct evidence of queue pressure; it does not isolate
exact cycle contributions or make a send retry equivalent to a lock retry.

Most importantly, ownership here still uses the **same shared global record
memory and shared L2** as native execution. Messages do not place owner data in
private DRAM or make a remote access physically local. Exceeding cache capacity
therefore does not automatically create FORZA-style NUMA locality benefits.

A future bulk request interface, actual owner-local memory architecture, or a
more expensive state operation could change the tradeoff. Those possibilities
are unmeasured here. Packet batching would be a concrete way to reduce the
current per-request CSR/progress overhead; this report does not assign it an
invented speedup or claim an inevitable win.

## Sensitivities and excluded diagnostic

An optional completed-lane gating policy was tested and preserved separately.
For the L1-off hot primary input it increased MP cycles from 8,467,958 to
9,681,777; the same extra branch also regressed the two initial small nonzero
smokes. The official default therefore uses the simpler progress loop for all
cases and cache settings, rather than selecting a policy per case.

The optional separate-array `gpu_soa` control has not completed its isolated
single-request test within the diagnostic budget. The same AoS single request
passes in 2,390 cycles and about 0.02 seconds. A correctly reconverged field
completion fence also failed to complete the SoA single request within 30 seconds.
These diagnostics remain preserved; SoA has **no valid performance entry** and no
particular model bug is asserted from a timeout. No timing model was modified.

## Scope and reproduction

Both fresh builds enable native atomics and FORZA, use two cores, two warps per
core and four lanes per warp, and retain the 1 MiB shared L2. The actual simulator
C++ constants give four L2 banks and four local-memory banks. L1-off adds only
`VX_CFG_DCACHE_DISABLE`; L1-on retains the write-through private L1. Standalone
host configuration expansion is not used to infer the simulator's bank count.

MP's known incoming counts are a one-shot completion contract, not generic
actor `finish`. Plain output stores are checked after the runtime's automatic
cache flush; a device fence alone is not claimed to drain all posted stores.
The state helper passed these precise prefix and final-state tests; this is not
a general proof of all owner-memory access patterns.

- [Benchmark and exact commands](../../tests/regression/forza_explore/compound_state/README.md)
- [Measurement CSV](forza_explore_compound_measurements.csv)
- [Raw records, pairings, and counters](forza_explore_compound_measurements.json)
- [Sources, compiler, configuration, binaries, commands, and log hashes](forza_explore_compound_manifest.json)
