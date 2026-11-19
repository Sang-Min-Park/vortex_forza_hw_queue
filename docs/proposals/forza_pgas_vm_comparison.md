# Programmable owner execution: measured results

A constrained programmable processor improves hot-record execution for three different uploaded programs. The actor-mailbox form does not beat the same engine's direct completion form in the representative hot cases. These results support programmable owner execution, not an actor-exclusive benefit.

## Implementations and equal-work contract

P0 is an 11-instruction count/sum/XOR program. P1 is an 11-instruction count/min/max program with signed branches and conditional stores. P2 is a 17-instruction nonlinear program: increment count; depending on old sum's parity, add signed delta or XOR request ID into sum; add the new sum into the third field. All return the previous three fields, using u64 wrapping.

No engine change or program-ID arithmetic shortcut distinguishes these programs. Software uploads instructions, with configuration acknowledged before work. Four contexts execute at most one fetched and one executed instruction per core/cycle through registered stages. Every memory access uses the real LocalMem channels; stores must acknowledge before return.

Native controls include scalar locking, same-key warp grouping, and fully timed GPU bucket/prefix ownership. Both compact global records (32 B with lock) and 64 B records are measured. Compact owner state uses 24 B/key; VM regions use 64 B/key. Native and VM use the same physical 16 KiB/core LocalMem capacity. Installation, routing, initialization, result writes and export are included. The minimum native control is chosen per condition; this envelope is not an implemented automatic selector.

P0/P1 host validation replays every old tuple and checks all final records. P2 is noncommutative: each mode may have a different legal serial history. The host orders returned per-key count tickets, rejects gaps/duplicates, replays every old tuple and nonlinear transition, then checks all final records. Input identity matches across modes; P2 final checksums need not.

## Full primary performance matrix

All rows use 4,096 operations, seed 17, two cores, two four-lane warps/core, L1D disabled and shared L2 enabled. Values are cumulative core-0 MCYCLE after readback/flush. Speedup is best native cycles divided by engine cycles.

| Program | Keys | Pattern | Best native | Mailbox engine | Direct engine | Mailbox speedup | Direct speedup |
|---|---:|---|---:|---:|---:|---:|---:|
| branch | 64 | hot | 1,334,111 | 438,626 | 432,575 | 3.04x | 3.08x |
| branch | 64 | hotset | 421,989 | 436,372 | 432,050 | 0.97x | 0.98x |
| branch | 64 | random | 226,408 | 436,372 | 432,050 | 0.52x | 0.52x |
| branch | 256 | hot | 1,394,560 | 447,877 | 441,867 | 3.11x | 3.16x |
| branch | 256 | hotset | 417,433 | 445,789 | 442,624 | 0.94x | 0.94x |
| branch | 256 | random | 205,752 | 445,789 | 441,315 | 0.46x | 0.47x |
| minmax | 64 | hot | 1,101,501 | 418,023 | 413,087 | 2.64x | 2.67x |
| minmax | 64 | hotset | 424,432 | 417,320 | 412,219 | 1.02x | 1.03x |
| minmax | 64 | random | 229,657 | 417,320 | 412,219 | 0.55x | 0.56x |
| minmax | 256 | hot | 1,101,370 | 427,370 | 422,451 | 2.58x | 2.61x |
| minmax | 256 | hotset | 428,176 | 426,618 | 421,598 | 1.00x | 1.02x |
| minmax | 256 | random | 206,730 | 426,618 | 421,598 | 0.48x | 0.49x |
| sumxor | 64 | hot | 995,071 | 422,279 | 416,739 | 2.36x | 2.39x |
| sumxor | 64 | hotset | 378,600 | 421,275 | 414,714 | 0.90x | 0.91x |
| sumxor | 64 | random | 210,967 | 421,275 | 414,714 | 0.50x | 0.51x |
| sumxor | 256 | hot | 1,009,804 | 431,722 | 425,829 | 2.34x | 2.37x |
| sumxor | 256 | hotset | 372,690 | 430,447 | 424,035 | 0.87x | 0.88x |
| sumxor | 256 | random | 191,550 | 430,447 | 424,035 | 0.45x | 0.45x |

## Attribution and limits

Direct completion bypasses logical actor registration and the actor mailbox API while retaining the same bounded request/reply fabric and two-slot completion storage. It still transports packets; it is not an implemented GPU LSU instruction returning a result directly to a register. Thus similar wins establish that actor dispatch is unnecessary for this engine benefit, not that a newly designed LSU-attached engine has been measured.

The engine adds a 1 KiB instruction store, four contexts with eight 64-bit registers each, a 32-entry request queue, and one actual LocalMem client. Each invocation owns one 64 B region, with conflicting invocations serialized. The contract excludes concurrent ordinary GPU/other-engine accesses during execution. This is not arbitrary RISC-V execution, a full RZA, or cross-region pointer traversal. Context, arbitration, port timing and memory responses are modeled; RTL, achievable clock, area and power are not validated.

**248/248 accepted regression runs pass:** 100 P0/P1, 60 compact-native controls and 88 P2 (including eight seed-29 hot repeats). The repeats reproduce cycle counts with different input values. Separately, 36/36 real-LocalMem unit runs pass across release and ASAN/UBSAN, including withheld store/result ACKs, saturation, isolation and invalid instructions.

Earlier failed shader cohorts remain diagnostics. Ordered benchmark-local shuffle/vote wrappers prevent the compiler from sinking collective operations into partial-mask branches. P2 grouped reads execute for all lanes under one leader-held lock; only lock/commit is leader-only. The accepted cohorts use the corrected binaries and full checks, with no golden or latency change.

- [P0/P1 raw results](forza_pgas_vm_validated2_results.json)
- [Compact GPU controls](forza_pgas_vm_native_packed_validated_results.json)
- [P2 raw results](forza_pgas_vm_branch_ordered_results.json)
- [Verification and every compared mode](forza_pgas_vm_final_freeze.json)
- [Engine contract](forza_programmable_region_engine.md)
