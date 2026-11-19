# Compound transactions after ordering mailbox release

After correcting the receive-slot release dependency, **all ten new compound MP
runs pass** their full response-prefix and final-state checks. The corrected MP
path is still **5.93–6.54 times slower** than the best measured native GPU control
on the four primary comparisons. This is a separate corrected cohort; all older
source, logs, and recorded measurements remain unchanged.

## Why the receive runtime changed

In this SimX implementation a fence is dispatched to the LSU without stalling
warp fetch. It drains the LSU path, but a younger slot-release CSR uses the
separate SFU path. The old `csrw zqmdq0, zero` has no register dependency on the
payload loads and can execute while they remain pending. Releasing the slot then
permits its SRAM contents to be replaced before the last load captures its data.
The FORZA CSR's own fetch stall prevents subsequent instructions from overtaking
that CSR; it does not drain preceding work from another functional unit.

The isolated `runtime_ordered` keeps the fence and passes a token into the actual
release CSR source register. The token xors the loaded control word and **every
loaded payload word**. Scoreboard RAW dependencies therefore require all these
loads to complete before release. The hardware ignores the written value. An
accidentally zero runtime token is safe because the instruction dependencies
remain; a compiler-eliminated zero idiom would not be safe.

For both cache builds, the actual compiled two-word receive path is:

```asm
ld    s2, 0(s7)       # control
ld    s3, 8(s7)       # first payload word
ld    a3, 16(s7)      # second payload word
fence r, rw
xor   a4, s3, s2
xor   a4, a3, a4
csrw  0x808, a4       # release depends on all three loads
```

The invalid-opcode path releases using the loaded control value. The runtime's
full path covers all seven payload words; its short path covers the rounded
requested size, with two words for DONE. The compound operation itself never
uses DONE packets. [Compiled proof](../../tests/regression/forza_explore/compound_state_ordered/compiled-release-proof.txt)
records the actual code and dump hashes for both builds.

This is specifically a receive-lifetime ordering fix. It is not a claim that
ordinary owner-memory updates caused the earlier failing scatter, or that a
fence alone drains all posted output stores. Driver, cache, fabric, and memory
latency models are unchanged.

## What was rerun and what was reused

The compound main, ABI, owner-state helper, and MP kernel are byte-identical
copies of their frozen originals. Only the runtime include changes. Ten MP runs
were executed anew: hot and capacity primary inputs plus uniform, hot, and zero
smokes under each cache setting. Every returned old `(count,sum,xor)` snapshot
must reconstruct a valid per-key ticket prefix, and every final record, lock,
worker count, and outstanding count must pass.

Twenty frozen GPU reference rows are reused: cooperative record locking and a
fully timed two-kernel GPU owner-bucket control for those same inputs and cache
settings. Their hosts, device kernels, drivers, input hashes, and log hashes were
verified unchanged. They do not consume message slots and therefore do not
require this receive fix. They were not silently rerun or replaced.

## Corrected results

Whole device cycles are queried after output readback and cache flush. The GPU
owner control includes both its routing and apply kernels. Smaller is better.

| Cache setting | Input | Corrected MP | GPU lock | GPU owner buckets | MP / best native GPU |
|---|---|---:|---:|---:|---:|
| L1 off | 32 keys, 4,096 hot requests | 8,485,122 | **1,405,128** | 3,018,393 | 6.04× slower |
| L1 on | Same hot input | 8,463,175 | **1,427,978** | 3,028,020 | 5.93× slower |
| L1 off | 32,768 keys, 65,536 uniform requests | 22,151,660 | **3,388,063** | 7,217,509 | 6.54× slower |
| L1 on | Same uniform input | 22,648,758 | **3,478,101** | 7,321,988 | 6.51× slower |

The capacity input still touches **1,817,344 B of distinct state sectors**, above
the 1 MiB shared L2, from a 2 MiB state allocation. Requests and responses add
1 MiB and 2 MiB respectively. Ownership does not give MP a private, physically
nearer state memory; all record implementations use shared global memory.

The corrected cycles are 0.20–1.17% above their corresponding older passing MP
runs. The ordering fix does not change the performance conclusion in this
family. Both native controls still outperform MP, and the lock control remains
the strongest measured baseline. This is a measured outcome for these inputs
and this simulator, not a universal impossibility theorem for message passing.

## Reproduction and provenance

Both builds retain native atomics and FORZA with two cores, two warps per core,
and four lanes per warp. The shared L2 is 1 MiB with four banks; only the private
L1 policy differs. No queue, cache, or atomic timing was changed.

- [New benchmark and commands](../../tests/regression/forza_explore/compound_state_ordered/README.md)
- [Corrected CSV](forza_explore_compound_ordered_measurements.csv)
- [Corrected records, native references, counters, and pairings](forza_explore_compound_ordered_measurements.json)
- [Source identity, runtime freeze, compiled proof, fixed native controls, binaries, and log hashes](forza_explore_compound_ordered_manifest.json)
- [Preserved original compound report](forza_explore_compound_comparison.md)
