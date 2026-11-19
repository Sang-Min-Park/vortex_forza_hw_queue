# Generic GPU handlers: arithmetic engines disabled

With both fixed-operation engines disabled, the generic GPU-handler implementations are correct but do **not** beat the fastest measured native GPU controls on this record workload. The previous 2.66× result belongs to the specialized destination transaction engine and is not evidence for generic message passing.

**56/56 runs pass exact validation**: 42 in the first generic cohort and 14 in the isolated role-split follow-up. All sources, host/guest snapshots, four shared libraries and 56 run logs were verified again. The tested libsimx has neither `ForzaZone::tick_record` nor `ForzaZone::tick_update`; the benchmark also fails compilation if either hardware feature is enabled.

## Same semantic work and native controls

The input is unchanged: 64 records, 4,096 independent bulk requests, full signed32 deltas including INT32_MIN/MAX, old `(count,sum,xor)` returned and `(count+1,sum+signext(delta),xor^id)` committed with u64 wrap. Every response ticket prefix and every final record is CPU-checked, along with per-source/per-handler counts and delivery credits. All eight implementations use the same input hash and final checksum within each distribution.

Two cores × two warps × four lanes remain fixed. Global L1D is disabled and shared L2 enabled. Primary cycles are final cumulative core0 MCYCLE after output readback/cache flush; routing is included once. All native algorithms are rerun on this same engine-free machine.

| Pattern | GPU lock | GPU grouped lock | GPU prefix owner | GPU field owner | GPU field core | Mixed bulk | Split bulk | Explicit RPC | Best bulk / best GPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| random | 212,261 | 2,051,440 | 392,225 | 665,718 | 1,195,047 | 3,144,679 | 3,030,751 | 1,889,205 | 14.28× slower |
| hotset | 392,019 | 1,859,100 | 382,647 | 652,827 | 1,176,386 | 2,809,664 | 2,792,861 | 2,712,589 | 7.30× slower |
| hot | 1,119,808 | 1,004,156 | 1,010,604 | 2,069,032 | 2,193,349 | 8,324,688 | 4,244,847 | 11,304,564 | 4.23× slower |

The fastest native result is selected across every control, including global locks, same-key warp grouping, timed histogram/prefix/scatter plus warp-prefix apply, and both field-parallel owner variants. A paired field-core control is not substituted for a faster native implementation.

## What runs on the GPU

[`FieldHandler`](../../tests/regression/forza_pgas_record_roles/field_handler.h) is compiled GPU code. Lanes0/1/2 hold count/sum/XOR and lane3 writes completion. It caches one key per owner warp and persists packed24B records in actual per-core LocalMem. The transport sees only an opaque pointer; it does not decode a record opcode or perform arithmetic. Native field-owner controls call the same handler.

The first bulk implementation mixes producers and receiving/handling in each warp. The follow-up dedicates warp0 of each core to service and warp1 to production, with separate noinline functions. It retains the same 16 physical contexts and uses eight producer lanes rather than 16. Immutable launch parameters and the handler shape are copied into scalars before loops; source target/pointer is retained across BUSY. No host prebucket or sorted input is provided to the message variants.

For single-hot input, this combined software change reduces mixed-bulk cycles **8,324,688→4,244,847** and L2 reads **372,086→70,030**. It is a useful improvement, but the best native grouped-lock control takes **1,004,156 cycles**, so the best generic bulk remains **4.23× slower**. The experiment does not isolate the individual contribution of role separation, scalar caching and register-pressure changes.

## Bulk completion and explicit RPC are different guarantees

- **Bulk:** one opaque request descriptor per input, normal GPU response stores, no reply notification. Completion requires the host-provided exact per-owner handled count, zero source delivery credits, a global barrier and final readback/flush. Delivery ACK alone is insufficient. This finite phase contract is the same kind of independent-bulk completion available to the native bucket controls; it is not a general actor termination detector.
- **Explicit RPC:** a thread handler publishes all three response fields and completion with four acknowledged ordinary AMOSWAP stores, fences, then sends an opaque notification through mailbox1. The source actually loads the four response words, checks completion and accumulates a digest verified against host readback. A four-request window per producer limits 16 producers to 64 active chains. Reply receiving continues while an outgoing reply is pending. No arithmetic reply VC is present.

The RPC path offers an additional per-source publication/consumption guarantee and different thread-owner placement; it is shown separately rather than silently replacing the bulk comparison. Its faster random result still loses to native locking. The source and handler count checks, full prefix replay and consumed-response digest all pass.

## Hardware and interpretation

Only generic MMIO transport, mailbox/FIFO storage and lossless routing remain enabled. The driver was built without UPDATE/RECORD flags, and [the recorded symbol check](../../build64-forza-generic/engine-absence-proof.json) verifies both arithmetic service methods are absent from the actual shared library. No per-mode latency or hardware resource parameter is changed.

At64 records, packed owner state is768B/core within the existing16KiB/core LocalMem. Readonly request descriptors and response output slots are global memory, not hidden SRAM. Generic mailbox SRAM remains a separate prototype resource. These results use one small resident state and do not establish larger-than-L2 behavior or physical RTL/PPA performance.

The result is narrower than “messaging cannot win”: this known bulk input admits strong GPU regrouping and prefix processing. The tests establish that these generic-handler implementations have not produced a win for this workload. They do not justify retaining the earlier specialized-engine speedup as a generic PGAS claim.

## Evidence

- Sources: `tests/regression/forza_pgas_record_software` and `tests/regression/forza_pgas_record_roles`.
- [First generic JSON](forza_pgas_record_software_results.json), [CSV](forza_pgas_record_software_results.csv); [role-split JSON](forza_pgas_record_roles_results.json), [CSV](forza_pgas_record_roles_results.csv).
- [Final verification manifest](forza_pgas_record_software_final_freeze.json), including source/shared/binary hashes, every run log hash and the engine-absence proof.
- Smokes cover0/1/128random/128hot inputs; performance covers64-key random/eight-hotset/single-hot input. No further state sizes or unmeasured wins are inferred.
