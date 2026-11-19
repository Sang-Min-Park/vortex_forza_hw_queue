# FORZA queue optimization and L2-capacity experiments

These are opt-in SimX experiments, not measurements of a physical GPU or RTL.
All comparisons use the same unchanged queue/cache/memory model, two cores,
two warps per core and four lanes per warp. One thread represents one actor and
one core represents one zone. Runtime experiments are isolated under
`tests/regression/forza_explore`; the original runtime is unchanged.

## Final results after correcting receive-slot lifetime

The corrected queue path did not beat native GPU execution. All final entries
below pass every state word, worker count and ACK-zero check. Transaction cases
also validate every returned old tuple. The scatter guest binary is identical between
the two cache configurations; the matching driver/model fingerprints are frozen.

| Workload, 65,536 updates | L1 | Corrected queue cycles | Best measured native GPU cycles | Queue / GPU |
| --- | --- | ---: | ---: | ---: |
| 256KiB scatter working set | OFF | 12,753,853 | 790,537 | 16.13x |
| 2MiB scatter working set | OFF | 13,028,563 | 964,288 | 13.51x |
| 256KiB scatter working set | ON | 12,795,101 | 803,460 | 15.93x |
| 2MiB scatter working set | ON | 13,379,175 | 990,244 | 13.51x |
| Multi-field transaction, >L2 touched state | OFF | 22,151,660 | 3,388,063 | 6.54x |
| Multi-field transaction, >L2 touched state | ON | 22,648,758 | 3,478,101 | 6.51x |

For scatter the final path is the compact typed one-send/one-receive kernel in
`large_state_balanced_ordered`. For transactions it is the isolated
`compound_state_ordered` using `runtime_ordered`. These are the recommended
review targets. Earlier constant-zero-release variants below are retained as
**exploratory pre-correction measurements**, including individually passing
cases; they are not the final receive-ordering contract.

## What exceeding L2 means here

The two cores share a 1MiB cluster L2. Vortex also supports multiple clusters,
so this is not a claim that every possible Vortex configuration has one L2.
The actual model has four L2 banks, 128B lines and 64B sectors. The application
log field `distinct_state_lines` counts **64B sectors**, despite its legacy name.

The capacity scatter case performs 65,536 signed integer updates over 2MiB of
owner state. A shuffled permutation touches all 32,768 sectors twice. Merely
allocating more than L2 would not establish this property. The native and
message implementations use the same numeric requests and physical state
indices. Owner state remains in ordinary global memory with shared L2;
software ownership does not place those data in a core-local SRAM. Only the
message slots use the dedicated SRAM path.

Miss and eviction counters cover the whole kernel and cache flush, including
input/runtime traffic. Cache miss events include MSHR/chained events and must
not be divided naively by admission-read events to report a miss percentage.
Whole device cycles are queried after result/state readback and cache flush.
Host request generation, uploads and CPU validation are outside this metric.

## Optimizations actually implemented

- Typed runtime messages write and copy only the required payload words.
  Full and short variants also differ in receive polling/copy work; this is a
  bundled optimization, not an isolated measurement of CSR writes alone.
- Compact scatter requests use an 8B `{uint32 key, int32 delta}` representation.
  The benchmark bounds guarantee a key below 2^24 and an absolute combined
  delta at most 7 * 2^20. Global state still uses signed 64-bit integers.
- Raw transport removes the generic Selector callback/outbox path.
- A batch carries an 8B count and up to six compact updates. Producer-private
  scratchpad buffers cost 7,168B per core in this configuration. Buffering,
  packing and draining are included in whole device cycles.
- The active-lane variant stops polling only after the actor has sent its
  inputs, received its exact known incoming count and observed ACK zero.
  This is one-shot benchmark completion, not general actor termination.
- The typed fast variant hoists topology work, directly writes one payload
  CSR and the nonblocking commit, polls the dequeue pointer once, and checks
  outstanding counts only when application work is complete. It retains the
  payload-read fence before releasing a slot. It relies on the prototype's
  zero-on-empty dequeue behavior and known valid input; it is not a replacement
  for the generic runtime's argument/error interface.
- The balanced variant changes only the typed fast producer burst from eight
  attempts to one before receiving. This tests send/receive scheduling rather
  than changing queue depth or network bandwidth.

No queue timing, atomic latency or cache capacity was changed to improve a
result. Even compact requests still occupy a **64B physical packet and slot**.
The fabric transfers packet objects; these results do not model a narrower
wire, flit serialization or physical SRAM bank/port costs.

## Exploratory optimization breakdown before the ordering correction

All rows below individually passed every state-word check, per-worker input/flush/delivery
counts and outstanding-zero checks. The seed is 17; the numeric-input hash is
`85beaff86ad00ad4`. Lower cycles are better.

| Implementation | Whole device cycles | Relative to compact native GPU |
| --- | ---: | ---: |
| Compact native GPU atomic | 964,288 | 1.00x |
| Native GPU with two-entry register combiner, 16B input | 3,270,017 | 3.39x |
| Compact raw message | 36,214,153 | 37.56x |
| Compact batch of up to six | 33,302,346 | 34.54x |
| Compact raw with completed-lane gating | 35,686,065 | 37.01x |
| Compact typed fast path | 27,920,583 | 28.96x |
| Compact typed fast, one send / one receive | 12,996,595 | 13.48x |

The batched path reduced 61,388 remote updates to 10,315 accepted packets,
averaging 5.95 updates per packet, but improved cycles by only 8.0% versus raw.
It increased ordinary runtime memory traffic. The typed fast path reduced
cycles by 22.9% versus raw. Balancing send and receive then improved it by
another 2.15x and reduced failed enqueue attempts from 100,364 to 31,849 for
the same 61,388 remote updates. This makes the tested queue path 2.79x faster
than the initial raw version, but still 13.48x slower than direct GPU updates.
Short payloads alone do not remove receive, commit, retry, dispatch and ordinary
owner-memory work.

The matched compact native GPU takes 790,537 cycles on the 256KiB repeated
working set, versus 12,920,310 for the balanced queue. The latter passes all
sixteen complete sector visits; its input hash is `47336e6e1d18e574`.

The typed fast path was also measured with L1 ON: 27,797,810 cycles at 256KiB
and 27,353,669 at 2MiB, versus compact GPU 803,460 and 990,244. L1 reduces runtime
L2 traffic but did not reverse the performance result. These numbers refer to
the eight-send fast variant, not the later balanced sensitivity.

The pre-correction balanced path also passed all four matched capacity checks. The guest
binary is byte-identical across L1 policies; no cache-model parameter is changed
between implementations within a comparison.

| L1 | Working set | Balanced queue cycles | Compact GPU cycles | Queue / GPU |
| --- | ---: | ---: | ---: | ---: |
| OFF | 256KiB | 12,920,310 | 790,537 | 16.34x |
| OFF | 2MiB | 12,996,595 | 964,288 | 13.48x |
| ON | 256KiB | 12,891,263 | 803,460 | 16.04x |
| ON | 2MiB | 13,096,218 | 990,244 | 13.22x |

This scheduling improvement is substantial relative to the previous queue
implementation, but none of these cases beats the native GPU control.

## Multi-field transactions

The second family atomically fetches an old `(count,sum,xor)` tuple, then updates
all three fields. Every returned snapshot is checked by sorting per-key tickets
and reconstructing its exact prefix, followed by every final field and lock.
Native controls include cooperative lock acquisition and a fully timed two-stage
GPU owner-bucketing implementation. The latter has no CPU pre-routing shortcut.

At 32,768 records and 65,536 requests, 1,817,344B of distinct 64B state sectors
were actually touched, exceeding L2. The L1-OFF message implementation took
22,151,660 cycles versus 3,388,063 for the direct GPU lock implementation
(6.54x slower). A 90%-hot 32-record case with 4,096 transactions took 8,485,122
message cycles versus 1,405,128 GPU lock cycles. Thus this stronger operation
also does not establish a message-queue speedup in this model.

## Ordering bug found and corrected

The original Selector/register-combiner scatter at 256KiB and 65,536 updates
reported three incorrect state words despite correct message counts and ACK
zero. Its timing is excluded. An unchanged-guest full-trace reproduction showed
three incoming payload substitutions, including words from two received entries
later. Independently folding those recorded values exactly reconstructed all
five wrong state words in the instrumented execution. Native owner-RMW tests
passed, so no atomic/fence workaround was added to ordinary owner state updates.

The source ordering hole was specific: the LSU fence gates LSU dispatch, while
the separate SFU can issue a slot-release CSR whose operand is the constant
zero. The release could therefore free a two-slot buffer before pending payload
loads completed. An assembly compiler barrier and a CSR's own fetch stall do
not provide that missing dependency on older LSU work.

`runtime_ordered` keeps the existing fence and XORs the loaded control and every
loaded payload word into the actual release-CSR source register. The source
register dependency waits for all those loads. Both the prototype and the
inspected original FORZA RTL ignore DQ write data, so the release operation is
preserved. No simulator timing was changed.

The exact formerly failing 16B Selector case now passes all 65,536 updates,
61,584 remote deliveries, all state words and ACK zero. Full/short ASAN/UBSAN
contracts and actual CSR smoke tests also pass. The final scatter capacities and
ten corrected transaction runs were then revalidated. See the separate ordering
report for frozen traces, compiled dependency chains and original-vs-corrected
provenance.

The fast dequeue-pointer polling rule is specific to this Vortex prototype:
empty returns zero. The inspected original FORZA RTL continuously computes its
DQ address even when invalid; the generic ordered runtime still polls ZQMSTAT
first. A runtime/hardware team must retain that distinction in its interface.

The optional SoA atomic-field transaction control also stalled, including a
single-request case with a reconverged fence. It has no accepted timing and is
excluded from speed comparisons. The validated AoS GPU lock is the stronger
measured native control; a stalled control is never counted as a messaging win.

These are irregular scatter and transaction microbenchmarks, not full BFS,
SSSP, graph traversal or a claim about every irregular GPU application.
An online request/reply workload where future requests depend on returned
state would test a different contract; it has not been implemented here.

## Artifacts

- [Final corrected scatter results](forza_explore_balanced_ordered_measurements.csv)
- [Final corrected scatter provenance](forza_explore_balanced_ordered_manifest.json)
- [Corrected transaction results](forza_explore_compound_ordered_comparison.md)
- [Corrected transaction measurements](forza_explore_compound_ordered_measurements.csv)
- [Corrected transaction provenance](forza_explore_compound_ordered_manifest.json)
- [Ordering bug and causal verification](forza_explore_ordering_bug.md)
- [Scatter comparisons](forza_explore_large_priority.csv)
- [Matched L1-ON scatter comparisons](forza_explore_large_selected_l1_on.csv)
- [Typed fast sources, hashes and logs](forza_explore_fast_manifest.json)
- [Balanced path matched results](forza_explore_balanced_measurements.csv)
- [Balanced path provenance](forza_explore_balanced_manifest.json)
- [Payload-trace diagnostic](forza_explore_large_digest.json)
- [Native owner-RMW diagnostic](../../build64-forza-explore/tests/regression/forza_explore/owner_rmw/owner-rmw-manifest.json)
