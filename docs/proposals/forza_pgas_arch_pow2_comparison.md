# Power-of-two ownership optimization: measured follow-up

The isolated optimization removes runtime division/remainder and redundant host-proven address checks, but produces only modest improvements on this frozen SimX machine. It does not change the measured ranking: direct GPU global atomic remains the fastest of these five modes for all nine primary cases.

**49 exact passes and six capacity N/A cases** completed, including all five 128-input smokes, nine 4,096-input size/pattern combinations and 1 KiB/hot four-epoch replay. The original 91-run cohort and its source/binary/library hashes remain unchanged. New rows match the original numeric inputs and every final state checksum. The independent read-only source review found no correctness regression.

## What changed

The host requires a power-of-two core count, computes its shift once and passes it through a previously unused 64-bit argument. The kernel uses `owner = key & (cores - 1)` and `offset = key >> shift` in both native GPU-local and engine paths. Thus ownership, physical layout, capacity and work remain identical. For valid host-generated keys, the benchmark constructs the UPDATE address directly; the public checked helper and the hardware validator are unchanged. These two source optimizations form one bundle, so the cycle difference is not attributed solely to division.

All five new disassemblies contain **zero DIV/REM instructions**. GPU-global and GPU-core-combine guest binaries are byte-identical to the original snapshots. All **19 applicable control runs** for those two modes have exactly identical cycle counts. The same four frozen shared/kernel libraries serve both cohorts; no simulator source or timing parameter was changed.

## Whole-device cycles before → after

| Total state | Pattern | GPU direct | GPU local | GPU core combine | Engine hybrid | Engine combine |
|---|---|---:|---:|---:|---:|---:|
| 1 KiB | random | 50,532 → 50,532 | 116,467 → 114,065 | 56,463 → 56,463 | 274,130 → 271,363 | 75,086 → 73,547 |
| 1 KiB | skew | 51,919 → 51,919 | 97,547 → 95,319 | 56,838 → 56,838 | 264,902 → 263,598 | 74,249 → 72,695 |
| 1 KiB | hot | 52,262 → 52,262 | 97,688 → 95,482 | 55,118 → 55,118 | 264,902 → 263,598 | 60,269 → 59,846 |
| 8 KiB | random | 50,598 → 50,598 | 139,771 → 137,365 | 104,897 → 104,897 | 288,640 → 285,873 | 237,251 → 227,419 |
| 8 KiB | skew | 51,981 → 51,981 | 139,691 → 137,248 | 101,571 → 101,571 | 293,222 → 290,515 | 185,801 → 179,577 |
| 8 KiB | hot | 52,262 → 52,262 | 122,110 → 120,069 | 92,638 → 92,638 | 279,412 → 278,108 | 112,405 → 112,094 |
| 32 KiB | random | 50,881 → 50,881 | 219,832 → 217,492 | N/A | 338,523 → 335,807 | N/A |
| 32 KiB | skew | 50,998 → 50,998 | 222,939 → 220,811 | N/A | 343,105 → 340,449 | N/A |
| 32 KiB | hot | 52,338 → 52,338 | 205,881 → 204,469 | N/A | 329,295 → 328,042 | N/A |

For 1 KiB/random, GPU-local improves 116,467→114,065 cycles (2.06%), engine-hybrid 274,130→271,363 (1.01%), and engine-combine 75,086→73,547 (2.05%). For 1 KiB/hot, engine-combine improves 60,269→59,846 while direct GPU stays at 52,262. Engine retries remain zero throughout; the modest optimization does not expose a queue-backpressure bottleneck.

The previous register run-fold experiment remains separately frozen and was not retested by this follow-up. Its operation-count reduction is still valid, but its old engine/GPU-local mapping costs should not be interpreted as optimized power-of-two results. No cross-cohort hybrid performance number is fabricated.

All resource and model-scope caveats in [the original architecture report](forza_pgas_arch_comparison.md) apply: a real added LocalMem port is part of the engine; all local allocations share the same 16 KiB/core budget; 32 KiB dense-combine cases are N/A; LocalMem/LLC AMO timing differs in SimX; no RTL/PPA or universal messaging-performance claim follows.

## Evidence

- Source: `tests/regression/forza_pgas_arch_pow2` (original siblings untouched).
- [Full result JSON](forza_pgas_arch_pow2_results.json), [CSV](forza_pgas_arch_pow2_results.csv).
- [Verified final manifest](forza_pgas_arch_pow2_freeze.json), including old-source preservation, binary identity, exact controls and zero-DIV/REM proof.
