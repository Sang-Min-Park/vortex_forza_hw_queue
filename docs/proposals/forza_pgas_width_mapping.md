# Message service width and PGAS ownership

All **112 runs passed**: 42 scalar/contiguous, 42 vector/contiguous, and 28
vector/striped. Each run checked every state word, per-actor work, transport and
AMO counts, plus software queue tickets where applicable. Independent analysis
reconstructed the input hash and full state checksum from the frozen generator.

The experiments separate two changes:

1. **Width only:** one MMIO command lane/cycle becomes four independent 64-bit
   command lanes/cycle with a full-mask response. CSR already services up to four
   active lanes/tick in this configuration. The seven guest binaries remain
   byte-identical; all 30 unaffected native/CSR/software runs have identical cycles.
2. **Ownership only:** on that same vector model, contiguous ownership becomes
   `owner = key & 15`, `offset = key >> 4`. Backing access/export reconstructs
   `key = offset * 16 + owner`. Inputs, logical output, transport, SRAM capacity
   and local bank interleave remain unchanged. The native-global guest binary and
   all four of its repeated cases have identical cycles.

Four command ports are an architectural resource change. They do not increase
NoC bandwidth, queue capacity, cache capacity or local SRAM capacity, and do not
establish equal physical area, frequency or energy to CSR hardware.

## Width-only result

Both datasets have 4,096 updates and 32KiB state. Values are whole-device cycles
after state/results/queue readback and cache flush.

| MMIO placement | Random, 1 lane | Random, 4 lanes | Skew, 1 lane | Skew, 4 lanes |
| --- | ---: | ---: | ---: | ---: |
| Global | 628,498 | 622,324 | 5,175,858 | 5,158,563 |
| Local SRAM | 846,574 | 870,100 | 6,657,897 | 6,629,703 |

The wider command service gives no large end-to-end improvement here. The
effect is not equivalent to changing packet size or making the entire connection
network four times wider. Busy retries, per-actor progress and handler execution
remain part of the measured path.

## Ownership and the hot actor

With contiguous 256-word regions, all 32 hot keys belong to actor 0. Striping puts
two hot keys on each of 16 actors. The input graph and answer are unchanged.

| Skewed input | Contiguous | Striped |
| --- | ---: | ---: |
| Minimum/maximum updates handled by one owner | 20 / 3,712 | 210 / 293 |
| Minimum/maximum incoming messages | 17 / 3,484 | 195 / 278 |
| Updates targeting core 0 / core 1 owners | 3,902 / 194 | 2,037 / 2,059 |
| Total transmitted messages | 3,841 | 3,851 |

Messages were not eliminated to obtain the improvement. On the vector model,
skewed CSR/local falls from 5,871,490 to 746,689 cycles (**7.86x**); MMIO/local
falls from 6,629,703 to 863,831 (**7.67x**); software/local falls from 14,833,119
to 1,320,772 (**11.23x**). The earlier large skew penalty was strongly affected
by the chosen ownership policy, not an intrinsic fixed cost of message passing.
Mapping also changes bank/core placement and export coalescing; these gains are
not attributed solely to one component. Striping is not universally better:
random MMIO/local increases from 870,100 to 921,674 cycles.

## Final seven-mode result: vector interface, striped ownership

| Mode | Random cycles | Skewed cycles |
| --- | ---: | ---: |
| Native global | 50,351 | 50,594 |
| Native local | 183,173 | 183,599 |
| CSR/global | 668,657 | 661,964 |
| CSR/local | 750,992 | 746,689 |
| MMIO/global | 798,816 | 781,972 |
| MMIO/local | 921,674 | 863,831 |
| Software owner/local | 1,267,488 | 1,320,772 |

For the same local-state actor algorithm, hardware CSR queues beat software
queues by **1.69x / 1.77x** on random/skewed input; MMIO queues beat software by
**1.38x / 1.53x**. Native global remains fastest. Even after balancing ownership,
CSR/local takes 14.92x / 14.76x native-global cycles; MMIO/local takes
18.30x / 17.07x. The native control was neither weakened nor changed between
ownership policies.

## What local SRAM establishes

Live local state uses the actual `__local_mem()` scratchpad path: 16KiB/core,
32KiB total, interleaved as `offset * 8 + local_actor`. The local owner variants
record 8,192 local reads and 8,192 local writes for 4,096 updates, including
initialization/export. Larger 128KiB functional cases explicitly retain 32KiB
locally and spill 96KiB to global backing. All variants retain a global snapshot
allocation for host verification; native/local also uses its prefix for remote
partials. These allocations are reported separately.

The resident working set is only 32KiB against a 1MiB shared L2. This experiment
cannot demonstrate a greater-than-L2 working set that also fits the available
owner SRAM. Local placement also does not remove message processing costs:
striped CSR/local processing spans 709,950 / 705,647 cycles versus native-global
48,650 / 48,962, before local initialization and export. Native/local has its own
routing, local atomics, partial pull and export costs; its processing spans are
122,822 / 122,880 cycles. These are measured limits of this implementation and
workload, not a universal claim that scratchpad or actor applications are slower.

The software PGAS handle is `{owner, offset}` with active-message operations.
Ordinary loads do not access another core's local SRAM. These SimX experiments
do not establish RTL implementation, PPA or physical GPU performance.

## Frozen evidence

- [Combined verification, ownership distributions and ratios](forza_pgas_cohort_analysis.json)
- [All 112 enriched rows](forza_pgas_cohorts.csv)
- [Final source/binary/log freeze](forza_pgas_final_freeze.json)
- [Scalar/contiguous commands and artifacts](forza_pgas_l1_off.json)
- [Vector/contiguous commands and artifacts](forza_pgas_vector_l1_off.json)
- [Vector/striped commands and artifacts](forza_pgas_striped_l1_off.json)
- [Original benchmark](../../tests/regression/forza_pgas_explore/README.md)
- [Isolated striped benchmark](../../tests/regression/forza_pgas_striped/README.md)
