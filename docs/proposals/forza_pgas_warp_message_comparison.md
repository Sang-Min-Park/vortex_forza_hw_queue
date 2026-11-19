# Dedicated message instructions and warp-cooperative GPU handlers

Decoded SEND/RECV instructions, credit-aware waiting, larger software range
batches and warp-cooperative record handlers are implemented and measured.
None of these generic messaging variants beats the best native GPU control in
this bounded independent-bulk workload. The best observed messaging/native
ratio is2.35x slower on two-core single-hot input and3.90x slower on four cores.
This is a SimX result; no RTL/PPA or physical-GPU speedup is claimed.

## Implemented mechanisms and fixed resources

All modes run without UPDATE, RECORD or programmable VM arithmetic engines.
The actual two-core and four-core libraries were checked with `nm`; their
arithmetic service symbols are absent. The new ISA executes decoded custom
instructions rather than wrapping a memory-mapped operation. Its try and wait
forms are compared with the existing8-byte MMIO enqueue and combined POP/release.

The fabric, packet size, request queues, two receive slots/mailbox, SRAM,
per-cycle command budget and memory timing remain fixed within each machine.
`send_wait` waits for admission, not completion of the software handler. Waiting
producer warps have a separate service warp and do not prevent local receives.
All requests reference immutable input; no implicit release/acquire publication
guarantee is assumed for the new ISA.

Each core runs two four-lane warps. One produces work and one services that
core's owner shard in actual16KiB LocalMem. Ordinary GPU instructions execute
every count/sum/xor operation. All4096 old tuples are returned and individually
replayed by their per-key tickets, and final states and all work/message counts
are checked. Arithmetic is u64 modulo2^64 with full signed32 deltas.

The field control retains the previous four-lane/one-request handler. New warp
handlers load four requests together and compute same-key prefixes, with one
key cached across homogeneous quads. Mixed keys are initially processed one
group at a time. A separate follow-up loads/stores independent key leaders in
parallel and uses ordered existing-ISA shuffles with explicit store-response
dependencies. This is software optimization, not a new arithmetic opcode.

Ranges of8/32 inputs are represented by an opaque `(count,start+1)` token sent
to each owner represented in the range. Producer classification and receiver
filtering are timed. Cooperative-producer variants scan ranges across lanes
before a leader enqueues; they retain the same physical threads but change the
number of sending endpoints. Exact per-owner incoming counts provide finite
bulk completion, not general dynamic actor termination.

## Two-core comparison

These are final cumulative core0 MCYCLE values after output readback/cache
flush, with4,096 requests over64 records and seed17. Global L1D is disabled and
shared L2 enabled. Native minimum includes ordinary record locking, optimized
same-key grouped locking, and fully timed GPU histogram/prefix/scatter plus
warp-prefix ownership. Same-handler native owner controls are also retained.

| Implementation | Random | Eight-key hotset | Single hot record |
|---|---:|---:|---:|
| Best native GPU | 212,264 | 382,097 | 1,005,670 |
| Field handler, MMIO range8 | 2,838,931 | 2,619,747 | 2,923,574 |
| Quad handler, MMIO range32 | 5,250,588 | 4,616,711 | 2,384,874 |
| Quad handler, ISA try range32 | 5,245,376 | 4,614,109 | 2,381,027 |
| Quad handler, ISA wait range32 | 5,245,238 | 4,613,832 | 2,380,683 |
| Parallel leaders, MMIO range32 | 4,094,348 | 3,853,750 | 2,362,013 |
| Parallel leaders, ISA try range32 | 4,089,896 | 3,849,865 | 2,358,431 |
| Parallel leaders, ISA wait range32 | 4,090,193 | 3,849,686 | 2,358,461 |

The ISA wait hot result is only0.18% faster than matched quad-MMIO32. At range8,
try/wait removes or reduces repeated BUSY instructions but does not materially
change total time. For example, quad-MMIO8 takes2,854,586cycles with7378 BUSY
retries; ISA wait8 takes2,846,031 with zero BUSY retries. The service/handler
critical path can remain expensive while producer retries disappear; this is
not a measured causal cycle decomposition.

Parallel mixed-key leaders improve the quad random path about22%, but the
original field handler is still better on random/hotset. The same WarpHandler
behind GPU buckets takes2,166,793 hot cycles versus the field-handler control's
2,197,517, while incurring substantially more L2 reads. Register pressure and
generated shader control remain important. Cooperative producer variants do not
reverse the native-GPU result; all raw rows are retained.

Taking the minimum observed generic variant per input gives13.37x/6.86x/2.35x
native cycles for random/hotset/hot. This is a performance envelope, not an
implemented automatic algorithm selector.

## Four-core scaling

A separate release build changes only core count to4, retaining two warps/core,
four lanes/warp, same per-core memory and queue parameters, and matching guest
and host compilation. All inputs and exact final checksums match the two-core
cases. Additional physical cores and their resources are present in all modes.

| Implementation | Random | Eight-key hotset | Single hot record |
|---|---:|---:|---:|
| Best native GPU | 122,621 | 203,612 | 603,756 |
| Field handler, MMIO8 | 1,842,186 | 1,603,073 | 2,922,699 |
| Quad handler, ISA wait32 | 3,221,540 | 2,648,207 | 2,379,640 |
| Parallel leaders, ISA wait32 | 2,899,434 | 2,487,627 | 2,356,497 |
| Best observed messaging / native | 15.02x slower | 7.87x slower | 3.90x slower |

For random range32, four owners rescan16,384 descriptors for4,096 logical
requests, versus8,192 scans on two cores. A destination-specific bitmap token
could avoid filtering unowned indices, but that additional variant is not
implemented here. Logical owner assignment alone does not remove classification,
dispatch, stack traffic or the need to consume and persist every result.

## Verification, diagnostics and interpretation

**204/204 final regression executions pass:** main two-core120 (75 smokes,
45 primary), isolated parallel-leader48 (30 smokes,18 primary), four-core36
(18 smokes,18 primary). They include zero/one input,7/131 tails, random,
hotset and hot distributions. All source snapshots, four libraries per machine,
guest/host binaries, build logs and individual run logs are hash-verified.

An initial one-request quad variant stalled because its generated inner loop
did not update/check its induction. A structured single-exit handler and removal
of unnecessary mutable-uniform induction annotations corrected that path;
the failing source/binary and stopped-run log remain in build diagnostics.
The first helper ABI was also rebuilt before measurement to expose physical
source identity to compiler divergence analysis. No driver change was needed
for that header-only correction. A per-range helper candidate passed correctness
but slowed hot input and increased persistence work, and was not selected.

The four-core wrapper initially placed a log in an app directory cleared by
`make clean`; this failed before simulation. Logs now live outside that app,
and frozen component binaries were reused. No numerical test was weakened.

The current evidence rejects a large opcode-only speedup for these bulk cases.
It does not prove that dedicated instructions are useless for other contracts,
nor that dynamic feedback execution has been covered here. New hardware
notification/credit support must be assessed on its actual critical path; zero
BUSY retries is not itself an end-to-end win. The fixed record accelerator's
positive result remains a separate operation-acceleration result.

## Evidence

- [Main120 raw results](forza_pgas_warp_message_results.json)
- [Parallel48 raw results](forza_pgas_warp_message_parallel_results.json)
- [Four-core36 raw results](forza_pgas_warp_message_scale4_results.json)
- [Two-core168 verification](forza_pgas_warp_message_combined_freeze.json)
- [Four-core verification](forza_pgas_warp_message_scale4_freeze.json)
- Source directories: `tests/regression/forza_pgas_warp_message` and its
  `_parallel` sibling. The latter's `run_scale4.py` runs the bounded scaling set.
- Fixed operation accelerator comparison: [attribution report](forza_accelerator_attribution.md).

All current record state fits owner LocalMem and is far below L2. Larger-state,
feedback-dependent, programmable-memory-engine and shared-pool designs are
separate experiments and must not inherit these results without measurement.
