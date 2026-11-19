# Core-owned SRAM and destination ADD: bounded SimX comparison

The destination ADD engine and register combining are functional, but none of the tested variants beats the best native GPU control on this frozen SimX machine. Direct global AMOADD64 remains fastest for all nine 4,096-input cases. This establishes a result for this additive scatter and modeled machine, not an impossibility result for messaging or PGAS.

Both cohorts completed: **85 exact passes and six capacity N/A cases** across 91 fresh-process runs. The first cohort has 59 passes and six N/A; the paired register-fold follow-up has 26 passes. Every result word, worker input count, nonzero publication count and engine ACK drain is checked. Input hashes and full-state checksums match across applicable modes. All captured sources, four shared libraries and guest/host snapshots were verified again after the final run.

## Matched whole-device cycles

Primary cycles are core 0 MCYCLE after full output readback/cache flushing. Kernels also record initialization, processing and export spans. Input upload and host output reset precede the launch in every mode. The machine is two cores × two warps × four lanes; global L1D is disabled, shared L2 enabled. All records use one aligned 64-bit input load.

| Total state | Pattern | GPU direct | GPU local | GPU core combine | Engine hybrid | Engine combine | GPU run fold | Engine run fold |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1 KiB | random | 50,532 | 116,467 | 56,463 | 274,130 | 75,086 | 112,556 | 351,579 |
| 1 KiB | skew | 51,919 | 97,547 | 56,838 | 264,902 | 74,249 | 98,151 | 227,264 |
| 1 KiB | hot | 52,262 | 97,688 | 55,118 | 264,902 | 60,269 | 72,495 | 84,316 |
| 8 KiB | random | 50,598 | 139,771 | 104,897 | 288,640 | 237,251 | 112,591 | 366,035 |
| 8 KiB | skew | 51,981 | 139,691 | 101,571 | 293,222 | 185,801 | 114,596 | 366,193 |
| 8 KiB | hot | 52,262 | 122,110 | 92,638 | 279,412 | 112,405 | 72,495 | 98,882 |
| 32 KiB | random | 50,881 | 219,832 | N/A | 338,523 | N/A | 112,854 | 416,120 |
| 32 KiB | skew | 50,998 | 222,939 | N/A | 343,105 | N/A | 112,958 | 418,959 |
| 32 KiB | hot | 52,338 | 205,881 | N/A | 329,295 | N/A | 72,545 | 149,025 |

The best engine/native ratios are 1KiB random: 1.49×, 1KiB skew: 1.43×, 1KiB hot: 1.15×, 8KiB random: 4.69×, 8KiB skew: 3.57×, 8KiB hot: 1.89×, 32KiB random: 6.65×, 32KiB skew: 6.73×, 32KiB hot: 2.85×. A ratio above one means more cycles. No retries occurred in the measured engine cohorts, so queue backpressure is not the explanation for this ranking.

## What changed and what the counters show

`engine_hybrid` maps a key to core `key % 2` and word `key / 2`. Producers update their local owner state through ordinary LocalMem AMOADD64; foreign updates enter the hardware destination ADD path. The device updates the same real LocalMem and acknowledges after AMO completion. No CPU receive handler, software mailbox, logical PE registration, or per-thread actor ownership is required. Physical source credits remain per executing lane.

`gpu_local` is a strong native alternative: local updates use the same owner SRAM; foreign contributions use global atomic sums, which each owner pulls after all updates complete. `gpu_core_comb` and `engine_comb` share the same dense source-core SRAM fold and sparse nonzero flush. The GPU gets the identical combining opportunity. Both include table clear and scan work.

The paired run-fold modes keep one key and signed 64-bit sum per producer, emitting only nonzero runs at key changes and epoch tails. They avoid a dense table and remain valid at 32 KiB total state. For 1 KiB single-hot input, both reduce 4,096 operations to 16 flushes; the engine performs eight local and eight foreign flushes. Whole cycles are still 72,495 for GPU run fold and 84,316 for engine run fold, versus 52,262 for direct GPU and 60,269 for engine core combine. The compiled GPU streaming loop keeps its state in registers and has no stack load/store in that loop. Fewer atomics alone therefore did not make this implementation faster.

At 32 KiB single-hot state, run folding reduces engine cycles from 329,295 to 149,025; direct GPU takes 52,338. The engine owns and exports the full state, including untouched words, and this cost grows with state size. Random inputs provide little one-entry reuse; their extra key comparison/control work can cost more than the saved AMOs. Phase counters expose these costs, but no isolated timing intervention was performed to assign an exact fraction to each cause.

Four-epoch 1 KiB/hot replay (16,384 logical updates, initialization/export once): GPU direct 188,174; GPU local 354,188; GPU core combine 199,064; engine hybrid 1,025,540; engine combine 208,085; GPU run fold 265,296; engine run fold 302,947. Each epoch is folded and flushed independently; all final words match four times the single-epoch CPU sum. This is throughput replay, not a claim that repeated additive work could not be algebraically simplified by an application.

## Resource and scope limits

Each core has the same 16 KiB LocalMem budget. Direct owner state consumes half the total state per core. GPU dense combine consumes the entire total state per core. Engine dense combine consumes owner state plus that full source table. Thus the two dense combine modes correctly report N/A at 32 KiB total state; no hidden SRAM or silent global spill is used. Register folding adds no dense local table. All modes also allocate the same global result array for validation/export.

The engine is an added architectural resource: a LocalMem request port and bounded queue/inflight state, in addition to the message fabric. It is not a zero-cost instruction substitution or an RTL/PPA result. Current SimX LocalMem and LLC AMO paths have different service models; the LLC timing path omits some stages present in current RTL. No timing knob was modified between modes. See [the independent timing audit](forza_atomic_timing_scope.md).

The largest state is 32 KiB, which fits the existing shared L2; these cases do not prove behavior for a working set larger than L2. Inputs are bounded commutative integer scatter, and the engine supports ADD rather than arbitrary handlers. Core-owner `{owner,offset}` addressing is explicit PGAS routing; this does not expose another core's SRAM through an ordinary load/store.

## Reproduction and evidence

- First cohort source: `tests/regression/forza_pgas_arch`; [result JSON](forza_pgas_arch_results.json), [CSV](forza_pgas_arch_results.csv).
- Paired run-fold source: `tests/regression/forza_pgas_arch_runfold`; [result JSON](forza_pgas_arch_runfold_results.json), [CSV](forza_pgas_arch_runfold_results.csv).
- Both runners use `build64-forza-arch`, pin the frozen runtime/kernel libraries and save host, guest binary, ELF, disassembly, build commands and individual logs.
- [Final verification manifest](forza_pgas_arch_final_freeze.json) records the source/library/binary fingerprints and verified run totals.
