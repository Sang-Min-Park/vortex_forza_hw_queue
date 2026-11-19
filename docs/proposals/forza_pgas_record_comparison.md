# Resident record transactions: a repeatable destination-engine win

On the frozen SimX machine, the new destination-side record engine is **2.39–2.66× faster than the best of five tested native GPU implementations for 4,096 requests to a single hot record**. Both representative sizes retain that advantage with a second input seed, and exact same-seed repeats match every recorded result field. This is a win for a specialized multiword transaction engine attached to owner SRAM, not a claim that hardware mailboxes alone accelerate generic actor software.

Random requests still favor ordinary GPU record locking. The full primary matrix, including these losses, is retained below. The eight-key hotset has a small win only at 64 allocated records and losses at the larger allocations. No general irregular-graph or universal PGAS speedup follows.

## Operation and correctness contract

Each independent bulk request `fetch_update(key, delta, id)` returns the old 64-bit `(count,sum,xor)` tuple and commits `(count+1, sum+signext(delta32), xor^id32)`. All arithmetic is unsigned modulo 2^64. Primary deltas exercise full signed32 values, including INT32_MIN/MAX, rather than a narrow counter that can simply be packed with ticket/XOR into one 64-bit word. A small-delta CLI exists, but no narrow packed-CAS comparison was measured or claimed.

The CPU sorts every key's replies by returned ticket, requires contiguous tickets, and replays the exact original delta and request ID to verify every old sum/XOR prefix. Every final record and lock is checked. Worker inputs, received replies and outstanding credits are checked separately. Thus these overlapping independent requests admit one consistent per-key serial history; this test is not a proof of arbitrary real-time ordering, adaptive clients, crash recovery or general API linearizability. No subsequent request depends on an earlier response.

## Matched primary cycles

Two cores × two warps/core × four lanes/warp; global L1D disabled and shared L2 enabled. Each row uses the same 4,096 input records and numeric hash. Whole-device cycles are core 0 MCYCLE after output readback/cache flushing. The counter survives kernel launches, so the final cumulative value includes routing exactly once. Routing has its own secondary snapshot; it is not added again.

| Keys | Pattern | GPU lock | GPU grouped lock | GPU serial owner | GPU grouped lock opt | GPU warp-prefix owner | Engine | Best native / engine |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 64 | random | 212,261 | 2,083,236 | 313,614 | 2,051,440 | 392,225 | 376,847 | 0.563× |
| 64 | hotset | 392,019 | 1,950,137 | 433,721 | 1,859,100 | 382,647 | 376,847 | 1.015× |
| 64 | hot | 1,119,808 | 1,650,593 | 2,171,937 | 1,004,156 | 1,010,604 | 376,847 | 2.665× |
| 256 | random | 196,248 | 2,101,031 | 368,054 | 2,075,749 | 470,997 | 386,109 | 0.508× |
| 256 | hotset | 377,299 | 1,956,898 | 469,601 | 1,859,330 | 434,602 | 386,109 | 0.977× |
| 256 | hot | 1,150,001 | 1,661,563 | 2,207,794 | 1,006,767 | 1,062,329 | 386,109 | 2.607× |
| 1024 | random | 189,200 | 2,103,227 | 548,483 | 2,080,644 | 774,800 | 422,781 | 0.448× |
| 1024 | hotset | 394,710 | 1,955,138 | 613,107 | 1,859,560 | 641,389 | 422,781 | 0.934× |
| 1024 | hot | 1,245,298 | 1,632,146 | 2,351,187 | 1,010,043 | 1,268,907 | 422,781 | 2.389× |

A ratio above one favors the engine. At 64-key single-hot input, the best native result is the optimized grouped lock at 1,004,156 cycles versus 376,847 for the engine. At 1,024 keys it is 1,010,043 versus 422,781. Native lock remains strongest for random input: 212,261/196,248/189,200 cycles across 64/256/1,024 keys, while the engine takes 376,847/386,109/422,781.

## Native controls receive the same legal opportunities

- **Global record lock:** cooperative one-attempt rounds, cached pending request, full tuple snapshot/update, fence before same-LSU unlock and complete per-request output. A 32B record includes its lock and keeps all fields in one sector.
- **Same-key grouped lock:** warp lanes sharing a key acquire one lock and reconstruct each request's prefix response. The optimized sibling computes tuple totals and lane prefixes before acquiring the lock, shortening the critical section. All four lanes participate in the 64-bit shuffles, including partial work tails.
- **Owner route/apply:** device histogram, prefix offsets, atomic scatter and separate apply launch, using O(N+K) scratch. All routing, scratch initialization, intermediate completion/cache flush, owner state persistence and outputs are timed. The optimized apply assigns a warp per key, processes four routed requests at once and performs tuple-prefix scans. The GPU can retain a key in registers while applying it and commits its final tuple to the same packed24B LocalMem before export. No host buckets, counts or sorted input are provided to the algorithm.

The comparison takes the minimum across all five native implementations for each input; it does not select only the lock baseline. These are implemented and measured controls, not proof that no more advanced GPU algorithm or better compiler could improve them. In particular this is a batchable prefix problem, and the warp-prefix control explicitly uses that fact.

## Repeatability and validation

**90 fresh-process runs pass exact validation:** 52 in the initial four-mode matrix, 26 in the optimized native-control matrix and 12 representative repeats. Smokes cover zero requests, one request, 128 random and 128 hot requests. The two primary matrices cover all three key counts and all three distributions.

| Second seed (29), single-hot | Best measured native | Engine | Speedup |
|---|---:|---:|---:|
| 64 keys | 1,004,156 | 376,847 | 2.665× |
| 1024 keys | 1,010,043 | 422,781 | 2.389× |

The new seed changes numeric input hashes and the CPU-replayed sums; every method still matches the same final checksum for that seed. Cycle counts happen to remain identical on this branch-independent single-hot workload. An exact seed17/64-key repeat matches **all RESULT fields** for normal lock, optimized grouped lock, optimized owner and engine. These deterministic SimX repeats do not estimate physical-device timing variance.

## What hardware was added and why the scope matters

The destination engine reads and writes the actual owner LocalMem through the existing extra hardware update port. It has four transaction contexts, a bounded 32-entry request queue, a per-key hazard check, a compute stage and three acknowledged memory reads plus three acknowledged writes per transaction. Completion credit is not returned until the state is committed and the reply is READY at the source. Each reply carries ID and all three old words through mailbox1; CPU PEEK/load/RELEASE uses a dependency on control and all four payload words.

The dedicated reply virtual channel adds bounded credits/buffering to avoid request/reply deadlock. Fabric arbitration still grants at most one data packet per input/output per cycle across request and reply classes; this is not doubled link bandwidth. The engine shares the added LocalMem request port with the earlier ADD engine. Its contexts, control/compute logic, request storage, reply credits and mailbox SRAM are real extra architectural resources and have no RTL area/power estimate here.

Packed owner state is 24B/record: 768B, 3,072B or 12,288B per core at the tested sizes, within the same 16KiB/core LocalMem. The native owner control uses that same packed storage. Engine mailbox SRAM is separately allocated (eight actors × eight mailboxes × two 64B slots = 8KiB/core); registering only mailbox1 does not erase that prototype allocation. The common global export uses 32B/record. No state is silently spilled and no mode receives extra hidden LocalMem capacity.

For example, 64 keys and 4,096 engine operations produce **12,480 actual LocalMem reads and 12,480 writes** including initialization/export, matching three record-field accesses per transaction plus state setup/export. Requests and replies are 64B physical messages even though the semantic reply uses four payload words. The apparent engine win is therefore not an instantaneous update to a host-side C++ map.

The likely benefit is moving hot-key serialization and tuple update sequencing into the destination device: CPU lock retries and multi-instruction tuple protocols are replaced by queued hardware service. GPU owner algorithms avoid locks but pay device routing and prefix/output work. Instruction count, register spills and these model's different LocalMem/LLC timing paths also affect the observed ranking; no isolated experiment assigns a causal fraction to each. All four shared libraries are frozen and no per-mode latency is tuned. These are SimX architectural results, not RTL/PPA or physical-GPU-optimality results. The largest record state is far below the 1MiB L2, so this is not a demonstration of a larger-than-L2 capacity win.

## Reproduction, preserved failure and provenance

- Original new workload: `tests/regression/forza_pgas_record`; optimized native sibling: `tests/regression/forza_pgas_record_gpu_opt`; repeat script: `tests/regression/forza_pgas_record_repeats/run_repeats.py`.
- [First matrix JSON](forza_pgas_record_results.json) and [CSV](forza_pgas_record_results.csv); [optimized controls JSON](forza_pgas_record_gpu_opt_results.json) and [CSV](forza_pgas_record_gpu_opt_results.csv); [repeat JSON](forza_pgas_record_repeats.json) and [CSV](forza_pgas_record_repeats.csv).
- The first mutable-mask grouped-lock N=1 smoke failed to retire. Its source, compiled guest and logs are preserved under `build64-forza-record/tests/regression/forza_pgas_record/pre-warp-control-fix`. The successful kernel uses fixed leader indices, retains same-key grouping, and passes all partial-work smokes. That diagnostic run is excluded from the 90-pass cohort. No driver change was needed.
- [Final source/library/binary/log verification manifest](forza_pgas_record_final_freeze.json); matching shared libraries: `build64-forza-record/library-freeze.json`. Earlier ADD, run-fold, PGAS and large-state cohorts remain untouched.
