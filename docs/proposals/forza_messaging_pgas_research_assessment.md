# Research assessment: generic messaging and PGAS on Vortex

This review synthesizes existing measurements and checks source-level costs.
It adds no new ISA implementation, simulation result, or hardware speedup claim.

## Research question and current answer

The question is whether bounded, generic communication and owner execution can
improve end-to-end GPU performance, with explicit resource cost and completion
semantics. Reproducing FORZA's component names or exposing a PGAS API is not a
performance result.

Current evidence does not establish a native-GPU win for generic messaging.
The specialized count/sum/xor destination engine wins on single-hot records,
including when called without actor registration or mailbox APIs. This supports
that limited operation accelerator, not an actor-exclusive advantage. Generic
queues improve the same actor algorithm relative to software queues, but those
are different claims.

| Evidence | Observed outcome | Interpretation |
|---|---|---|
| Fixed RECORD engine, hot64 | Native1,005,670; mailbox430,248; direct425,801 cycles | Roughly2.3x destination-engine win; available without actor mailbox handling. |
| Generic Batch8, hot64 | 2,923,574 vs native1,005,670 | Still2.91x slower despite1.87x improvement over Batch1. |
| Generic Batch8, random64 | 2,838,931 vs native212,264 | Still13.37x slower. |
| Standard remote ADD engine | Best native wins all9 primary cases | A common primitive alone is insufficient on this scatter/machine. |
| Hardware vs software queues, same actor algorithm | Hardware1.38–1.77x faster in striped PGAS | A real queue implementation improvement; not a native-GPU win. |
| Corrected2MiB scatter, L1off | Queue13,028,563 vs native964,288 | Exceeding sharedL2 alone did not rescue messaging. |
| Corrected >L2 touched-state transaction | Queue22,151,660 vs native3,388,063 | A stronger operation and large touched state also lost in the prior global-state design. |

Sources: [accelerator attribution](forza_accelerator_attribution.md),
[remote ADD](forza_pgas_arch_comparison.md),
[queue service/ownership](forza_pgas_width_mapping.md), and
[corrected capacity experiments](forza_explore_optimization.md).

The capacity tests genuinely touched more than L2; they were not merely large
allocations. Their owner state remained in shared global memory. The recent
resident-state tests use actual owner LocalMem but fit comfortably below L2.
Neither fact should be substituted for the other when arguing physical locality.

## What a SEND/RECV instruction would have to change

The current fast path already submits an eight-byte token with one
`AMOSWAP.D`. RX_POP returns the token and releases its slot with one instruction.
LSU address classification routes these commands directly to ForzaZone, bypassing
the global cache atomic path. Seven payload CSR writes and an HBM atomic round
trip are not the present fast-path cost.

Relevant code is `tests/regression/forza_pgas_message_batch/{device,transport}.h`,
`kernel_batch.cpp`, `sim/simx/mem/local_mem_switch.cpp`, and
`sim/simx/forza/forza_zone.cpp`.

An opcode can reduce address generation, LSU command routing/response overhead,
live registers and status handling. Wider MMIO service1→4 lanes gave little
end-to-end improvement with identical guest binaries, but that experiment did
not implement a fused SEND/RECV ISA and does not settle the ISA question.

Batch8 hot accepts512 messages but executes7911 BUSY retries. Producer retry
instructions can compete with service warps. A credit-aware SEND that parks a
producer warp until admission, and a receive-ready event that avoids empty
polling, are more specific hypotheses than renaming the MMIO instruction.
Waiting must not occupy a shared unit in a way that prevents the receiver from
running; runnable service capacity and a reply escape path are required.

| Batch8 case | Cycles to match native that must be removed | Fraction of total time |
|---|---:|---:|
| hot | 1,917,904 | 65.60% |
| hotset | 2,237,650 | 85.41% |
| random | 2,626,667 | 92.52% |

These are break-even requirements, not measured ISA-attributable costs. Retry
counts cannot be multiplied by an assumed latency to obtain critical-path time.
Dedicated SEND/RECV has not yet been measured, so no speedup is predicted here.

The handler mapping matters too: the current four-lane FieldHandler applies
one request, distributing its three fields and completion over lanes. Native
warp-prefix owner processing applies four same-key requests together. The same
FieldHandler reached through GPU buckets still takes2,197,517 hot cycles. That
is evidence that changing only enqueue is insufficiently motivated, not a hard
lower bound on every possible messaging handler. Its difference from Batch8 is
not a measured pure communication cost: routing, ordering, filtering and code
generation differ.

An ISA experiment should separate these changes:

1. SEND/RECV versus current MMIO with identical queues, arbitration, payload,
   handler, result contract and physical transport.
2. Credit wait/wakeup versus shader retry/polling, with bounded pending state
   and guaranteed receive progress.
3. Warp-cooperative receive/dispatch and handler batching versus one-request
   handling. Any deeper queues or additional read ports count as new resources.
4. Release/acquire publication support versus the current explicit protocol,
   preserving visibility before notification. A pointer enqueue alone does not
   make preceding descriptor/result stores visible.

The existing two-slot mailbox and one-reservation/fill-per-cycle path cannot
magically supply four independent queued messages in one cycle. RECV_BATCH
needs a multicycle gather, a shared pool, or explicitly costed extra capacity/
ports. Several logical operations can also be represented by a range token and
processed cooperatively without adding a new dequeue port.

## FORZA ideas to keep, adapt, and defer

| Idea | Reason to evaluate it | GPU adaptation |
|---|---|---|
| Owner-addressed execution | May replace several dependent remote accesses with one request and local work | Tie `{owner,offset}` to real data placement; compare with equally legal native placement/RMA. |
| Generic asynchronous queues and credits | Removes software queue synchronization and enables overlap | Bound all buffers, retain backpressure, distinguish delivery from operation completion and phase termination. |
| Near endpoint notification | Can reduce repeated status polling | Ready events or wait/wakeup with a lost-wakeup-safe contract; keep service warps schedulable. |
| Direct and indirect payload SEND | Supports small inline operations and larger descriptors | Count gather/copy/publication costs; choose payload size independently of operation semantics. |
| Destination aggregation | Amortizes headers and injection/dispatch work | Batch opaque work, flush on limits or timeouts; preserve ordering and avoid waiting forever for a full batch. |
| Programmable near-memory execution | Could perform useful multi-access handlers without a fixed record opcode | A separate unimplemented hypothesis; compare identical programmable execution through direct invocation too. |

The2025 FORZA design explicitly describes ZIP aggregation, direct/indirect SEND,
credits and scratchpad notifications in
`forza/FORZA-POG-2025/chapters/part1_arch_spec/07_on-chip_fabric.tex` (outside this
repository). These are design-document features; they are not all verified as
implemented in the older inspected RTL. The ZIP aggregation discussion concerns
precinct/system networking and does not predict an on-chip GPU speedup.

The CSR ring, fixed64B frames, two slots, actor=physical-thread mapping,
thread migration and CXL are not prerequisites for the research question.
Current mailbox allocation is8 mailboxes×2slots×64B=1024B per physical endpoint.
Naively applying it to32warps×32lanes would require1MiB/core just for mailboxes.
Logical actors should be virtualized over a bounded warp/core queue or buffer
pool; the resulting dispatch and metadata costs must be included.

## Workloads that can distinguish the hypotheses

Existing independent bulk inputs legitimately permit GPU regrouping and prefix
processing. Those strong controls should remain. They do not test the full case
where a reply determines the next request or handlers dynamically create work.

A bounded next workload pair is:

- **Feedback lookup/walk:** the returned record determines the next key. Run
  multiple independent client chains and vary chain length and client count.
- **Conditional state update:** a software handler checks state and performs a
  conditional multi-field update, returning information used in the next request.
  The same queue hardware must run both handlers without new arithmetic opcodes.

Compare with persistent native GPU implementations, legal asynchronous work
queues and frontier/batched algorithms where semantics allow them. Match client
concurrency, inputs, ordering requirements and observable completion. Do not
force a host interaction or kernel launch at every GPU request. Count all owner
routing, initialization, publication, queue handling, termination and export.
The fixed host-derived incoming counts used by the current bulk tests do not
apply to general dynamic handler-generated traffic. Track actual requests,
replies, active work and partial batches.

Separate shared-global versus actual owner-local state, request footprint,
handler work, dependency depth, contention and core count. Extra owner SRAM must
be offered/accounted for in controls; declaring an owner in software alone does
not create a faster memory path. A single-GPU on-chip conclusion and a
multi-GPU/network conclusion require separate systems and baselines.

## Decision criteria

A useful positive result would be reproducible end-to-end wins for more than
one GPU-code handler on the same generic communication hardware, with realistic
bounded resources and strong native controls. Record explicit regressions and
the workload region where the mechanism helps. If a standard persistent GPU
work queue benefits from the same primitive, the contribution can be generic
asynchronous GPU execution support; exclusivity to actor syntax is unnecessary.

If opcode-only changes are small, do not keep enlarging the fabric without
evidence. If gains appear only after adding a new fixed arithmetic opcode,
classify them as operation acceleration. If queue, wakeup and cooperative
dispatch still fail on the dependency-sensitive workloads, reject this
implementation's generic-messaging performance hypothesis or narrow its target.
The present evidence is insufficient for the project's generic GPU-speedup
claim, but it identifies specific mechanisms and falsifiable next tests.

## Related primary sources and scope

[A Scalable Actor-based Programming System for PGAS Runtimes](https://arxiv.org/abs/2107.05516)
frames fine-grained asynchronous actor communication, aggregation and termination
within a superstep. Its cluster-PGAS measurements are not evidence for Vortex
GPU hardware speedup.

[NVSHMEM's device API guide](https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/apis.html)
already emphasizes transport awareness, cooperative warp/block communication,
coalescing and aggregation. These must be legal capabilities in GPU controls.

[Atos: Scalable Irregular Parallelism with GPUs](https://escholarship.org/uc/item/9j6337h7)
studies GPU-driven scheduling, one-sided communication and aggregation on
multi-GPU/multi-node systems. Its results establish a distinct research setting,
not a transferable speedup or proof for the current two-core on-chip prototype.
