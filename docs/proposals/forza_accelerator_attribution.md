# Separating destination acceleration from actor messaging

The fixed record accelerator helps the tested single-hot-record workload even
when the source does not register an actor or receive actor-mailbox messages.
Its speedup is not exclusive to actor messaging. Generic software batching also
helps the messaging implementation, but does not beat the strongest native GPU
control in the measured cases.

These are SimX architectural experiments on Vortex, not physical-GPU or RTL/PPA
measurements. No cache, bank, fabric or memory latency was adjusted per mode.

## Same accelerator, two completion interfaces

The operation is unchanged: capture the old unsigned64 `(count,sum,xor)` tuple
and commit `(count+1,sum+signext(delta32),xor^request_id32)`, modulo 2^64,
publishing the captured tuple only after all store acknowledgments. Inputs
exercise the full signed32 range, including INT32_MIN/MAX. Every returned tuple
is replayed in per-key ticket order, and every final record is checked. These are
independent bulk requests; no subsequent request depends on a previous reply.

Both paths use exactly the same four-context destination engine, 32-entry
request queue, per-record hazard rule, three 8-byte reads, compute stage, and
three acknowledged 8-byte writes to actual LocalMem. Both use the same 64-byte
request/reply frames, shared physical fabric bandwidth, two 64-byte result slots
per source, reservation/fill pipeline and ACK sequence. ACK follows result READY
and does not mean software consumed the result. Source kernels enforce the same
two-request `sent - consumed` window and consume all returned fields.

```text
actor-mailbox completion:
GPU MMIO command -> fabric -> RECORD engine -> fabric
  -> logical PE lookup -> mailbox1 -> PEEK / payload loads / RELEASE

direct completion queue:
GPU MMIO command -> fabric -> SAME RECORD engine -> SAME fabric
  -> physical source -> completion queue -> ID / field loads / RELEASE
```

The direct queue aliases the existing mailbox1 SRAM and its two slots. It adds
no result capacity or link bandwidth. It does not register a logical PE; the
physical executing source identifies the completion queue. Direct mode is pinned
until reset and excludes mailbox1 registration/release APIs. Other raw read-only
SRAM aliases remain accessible under the prototype's existing memory model;
this is not an isolation/protection feature.

The direct endpoint is a proposed nonblocking completion interface. It is **not**
a blocking LSU instruction that returns three architectural registers, and does
not use an independently implemented alternative GPU cache/interconnect path.
Remote packets and their queues still exist. This experiment isolates actor
addressing and completion API overhead while holding physical transport fixed.

## Matched results

The machine has two cores, two warps/core and four lanes/warp; global L1D is off,
shared L2 is on, and each core retains the existing 16KiB LocalMem. Primary
measurements use 4,096 requests and seed17. Whole-device cycles are final core0
MCYCLE after output readback/cache flushing. Native routing, SRAM setup,
completion and exports are included. Native minimum selects global locking,
optimized same-key grouped locking, or timed GPU bucket/prefix ownership.

| Records | Distribution | Best native GPU | Mailbox + same engine, optimized | Direct CQ + same engine, optimized |
|---|---|---:|---:|---:|
| 64 | random | 212,264 | 430,248 | 425,801 |
| 64 | eight-key hotset | 382,097 | 430,248 | 425,801 |
| 64 | single hot record | 1,005,670 | 430,248 | 425,801 |
| 1024 | single hot record | 1,010,182 | 463,400 | 458,808 |

For 64-record single-hot input, both accelerated paths win: **2.34x mailbox and
2.36x direct**. Direct is only about 1.04% faster than mailbox. The accelerator
benefit is therefore available without actor mailbox handling. Both accelerated
paths lose to the best native implementation on 64-record random and hotset
inputs. The 1024-record hot case retains approximately 2.18x/2.20x speedup.

The initial shared-source implementation completed all distributions at both
record counts. Its 64-record mailbox/direct results were 487,215/495,160 cycles:
the direct shader kept separately spilled field addresses. One isolated source
follow-up uses contiguous endpoint pointers and reduces other live state for
both protocols. It changes neither hardware nor shared libraries. The small
protocol ranking reverses after this cleanup; it is not evidence of an inherent
mailbox advantage. Initial and optimized cohorts are preserved separately.

The unwindowed mailbox reference matches its window-two sibling in the initial
cohort. Observed peak `sent - consumed` is one in these performance cases even
though the cap is two. These are end-to-end shader/engine measurements, not a
saturated-link throughput limit. Saturation/backpressure is separately checked
by the unit fixtures.

All engine performance runs perform `3*N + 3*K` LocalMem reads and the same
number of writes including setup/export: 12,480 each at 64 records and 15,360
each at 1024. No per-request arithmetic or memory work disappears in direct mode.
Seed29 changes input hashes/checksums and repeats the 64-record hot pair with
identical cycles. This deterministic replay is not physical timing variation.

## Generic batching without an arithmetic accelerator

A separate matching build disables both UPDATE and RECORD. The actual library
has neither `ForzaZone::tick_update` nor `ForzaZone::tick_record`. Each core runs
one producer warp and one service warp. The GPU FieldHandler performs all math.

For batch sizes 1/4/8, each producer scans an immutable contiguous input range,
builds its destination-core bitmap and sends one opaque token to each represented
core. The token encodes only `(start_index+1,count)`. Receivers scan the original
range, filter their owned keys and execute every original operation. There is
no reduction, host prebucket, mutable descriptor publication, or new hardware.
Grouping, filtering, partial tails, flushes and exports are timed. Source
assignment changes with batch size. Random ranges sent to both cores cause
duplicated receiver scans; those costs and message counts are checked.

| Distribution, 64 records | Best native GPU | Batch1 | Batch4 | Batch8 | Batch1 / Batch8 |
|---|---:|---:|---:|---:|---:|
| random | 212,264 | 3,654,832 | 3,236,138 | 2,838,931 | 1.29x |
| eight-key hotset | 382,097 | 3,406,334 | 3,002,657 | 2,619,747 | 1.30x |
| single hot record | 1,005,670 | 5,462,774 | 3,387,357 | 2,923,574 | 1.87x |

Single-hot messages fall from 4096 to 1024 to 512 while all 4096 old tuples and
updates remain. The best batch still takes **2.91x native cycles**. Random Batch8
uses 1023 messages and 8184 receiver key scans, versus Batch1's 4096 messages and
4096 scans; it remains 13.37x slower than native. Batch8 hotset remains 6.86x
slower. The same FieldHandler reached through GPU-generated buckets also beats
Batch8, including on single-hot input (2,197,517 cycles).

This is generic **software batching over existing queue hardware**, not a new
arithmetic or DMA accelerator. It establishes an implementation improvement,
not messaging-exclusive acceleration. Native controls already receive legal
grouping/prefix opportunities. Known per-owner work/message counts are supplied
for finite bulk completion; no dynamic actor termination detector or immediate
per-request reply notification is claimed.

## What is general, and what the experiments establish

| Mechanism | Applicability | Evidence and limit |
|---|---|---|
| Remote 64-bit ADD / UPDATE | Standard primitive for additive updates; not arbitrary handlers | Earlier matched scatter experiments lost to native GPU in all nine primary cases. |
| RECORD transaction engine | Fixed count/sum/xor update and old-tuple return | Wins on a contended record, including without actor mailbox APIs; not a programmable RZA implementation. |
| Queue/MMIO service | Payload-independent communication | Earlier hardware queues improved the same actor software over software queues; native GPU still won. Wider command service alone gave little benefit. |
| Range batching | Payload-independent transport amortization for batchable input | New software experiment improves messaging 1.29–1.87x but still loses to native GPU. |

No tested accelerator has established a benefit unique to actor messaging.
Destination execution and serialization are useful architectural candidates,
but they must also be available to the non-actor control when attributing gains.
A future programmable near-memory handler would broaden supported operations;
its instruction cost, memory conflicts, concurrency and direct invocation path
would need their own implementation and measurements. Existing fixed-function
numbers do not predict that result.

## Verification and reproduction

- **144/144 regression runs passed:** initial attribution70, isolated optimized
  pair18, and validated generic batching56. These include zero/one input,
  partial tails, random/hotset/hot distributions and second-seed hot repeats.
- **20/20 unit cases passed:** release and ASAN/UBSAN, including symmetric
  saturation, two-slot reuse, withheld store/reply ACKs, malformed identities,
  full32 request IDs, mode exclusion and exact six memory accesses/transaction.
- An initial batch zero-input smoke found a host async-copy buffer lifetime bug.
  Its sources/binaries/logs remain under
  `build64-forza-batch-generic/diagnostics/pre-count-lifetime-fix`. The host count
  buffer now lives until completion. Device expectations and handler code were
  not weakened; the successful cohort has a separate manifest and artifacts.
- The first attribution build inherited `DEBUG=release` and failed compilation;
  its log is preserved. Final libraries and measurements explicitly unset DEBUG
  and use release compilation. Older record/generic frozen libraries were not
  overwritten.

Sources: `tests/regression/forza_pgas_accel_attribution`, its `_opt` sibling,
`tests/regression/forza_pgas_message_batch`, and `tests/unittest/forza_record`.
Use their generated out-of-tree Makefiles and `run_matrix.py` runners. The
direct ABI is in `sw/common/forza_record_abi.h`; routing/model changes are in
`sim/simx/types.h` and `sim/simx/forza/forza_zone.{h,cpp}`.

- [Initial attribution raw results](forza_pgas_accel_attribution_results.json)
- [Optimized pair raw results](forza_pgas_accel_attribution_opt_results.json)
- [Generic batching raw results](forza_pgas_message_batch_results.json)
- [Final source/library/binary/log verification](forza_accelerator_attribution_final_freeze.json)
- [Unit manifest](../../build64-forza-attribution/tests/unittest/forza_record/forza-record-direct-unit-manifest.json)
- [Earlier generic-handler comparison](forza_pgas_record_software_comparison.md)
- [Earlier remote ADD comparison](forza_pgas_arch_comparison.md)
- [Earlier generic queue/service-width comparison](forza_pgas_width_mapping.md)

All tested record state fits the existing owner SRAM and is far smaller than
the shared L2. These experiments do not establish larger-than-L2 behavior,
general graph speedup, adaptive RPC performance, equal area/power, or that either
completion protocol is optimal.
