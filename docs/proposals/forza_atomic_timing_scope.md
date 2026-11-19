# Atomic timing scope of the PGAS experiments

This is a source-only audit of the current checkout, dated 2026-10-27. No
simulation was run and no cache, SRAM, atomic, or queue timing was changed.
There is a pre-existing difference between SimX's LocalMem and LLC atomic
service models, including LLC RTL mechanisms absent from the SimX commit path.
This establishes a timing-model limitation, not a measured speedup correction.

## What the sources establish

| Path | Source behavior and evidence |
| --- | --- |
| SimX LocalMem | An atomic consumes its acceptance tick and two subsequent bank-busy ticks. A following read/atomic to the same bank word waits an additional tick: nominal bank initiation intervals are three ticks for different words and four for the same word, before other contention/backpressure. See [occupancy definition](../../sim/simx/mem/local_mem.cpp#L37), [busy/RDW checks](../../sim/simx/mem/local_mem.cpp#L116), and [busy assignment](../../sim/simx/mem/local_mem.cpp#L204). |
| Local-memory RTL | The single-port bank performs read, capture, then writeback; `amo_busy` gates acceptance, followed by a same-address read-during-write check. This supports the LocalMem model's architectural rationale. See [three-stage datapath](../../hw/rtl/mem/VX_local_mem.sv#L327), [busy state](../../hw/rtl/mem/VX_local_mem.sv#L425), and [RDW/ready gating](../../hw/rtl/mem/VX_local_mem.sv#L575). This audit does not establish exact waveform parity. |
| SimX LLC | At most one pipeline-head request is processed per bank/tick. `commitAmo` computes and merges the new cached word and sends its response in that service call. Its explicit stalls are response capacity and, for write-through, downstream capacity. It has no LocalMem-style atomic occupancy or separate atomic compute/writeback state. See [bank tick](../../sim/simx/mem/cache.cpp#L721), [commit/stall checks](../../sim/simx/mem/cache.cpp#L1140), [merge](../../sim/simx/mem/cache.cpp#L1193), and [response](../../sim/simx/mem/cache.cpp#L1219). Thus one hot-hit commit per bank/tick is a model upper bound, not guaranteed observed throughput or one-tick end-to-end latency. |
| LLC RTL | Atomics have a registered compute stage, queued synthetic writeback, and forwarding/settling state. `commit_busy` blocks new core admissions through the commit window; same-line replays have `chain_stall`. These mechanisms are not explicitly represented by SimX's inline `commitAmo`. See [writeback/compute structure](../../hw/rtl/cache/VX_cache_amo.sv#L173), [compute/writeback updates](../../hw/rtl/cache/VX_cache_amo.sv#L492), [busy/chain gating](../../hw/rtl/cache/VX_cache_amo.sv#L631), and [bank admission/writeback arbitration](../../hw/rtl/cache/VX_cache_bank.sv#L361). |

Cache and scratchpad banks need not have identical microarchitectures. The
specific parity concern is the missing LLC atomic timing mechanisms relative
to this checkout's RTL, rather than a requirement to make global and local
atomics equally slow. Misses, MSHR chains, routing, response congestion and
instruction issue still affect SimX LLC performance; it is not an infinite
atomic-throughput model.

The destination ADD engine submits real AMOs to the existing LocalMem client
crossbar and inherits its bank rules. It does not bypass them through a host
array: see [client binding](../../sim/simx/core.cpp#L150) and
[operation issue](../../sim/simx/forza/forza_zone.cpp#L721).
Consequently cached native GPU AMOs and engine SRAM AMOs exercise different
existing service assumptions. All benchmark modes retain that same model;
the measurements remain reproducible SimX comparisons. Their ratios alone do
not establish physical GPU/engine throughput, equal-area superiority, or a
universal architecture ranking. Neither an RTL initiation interval nor a
numerical correction to prior results has been measured here. Changing the
LLC model requires a separate parity investigation and new comparison cohort,
not retuning a frozen baseline until an engine wins.

## Existing verification paths and available tools

No ready-to-run standalone `cache_amo` bank-throughput test was found. The
existing [cache wrapper](../../hw/unittest/cache/VX_cache_top.sv#L64) exposes
AMO-capable LLC parameters, but its [C++ main](../../hw/unittest/cache/main.cpp#L33)
only calls `Verilated::commandArgs` and returns without a clock or traffic.
The local-memory unit main is also a stub. Running their `make run` targets
would not measure an initiation interval.

The useful existing application is [tests/regression/amo](../../tests/regression/amo/Makefile):
`-tamoadd` hammers one global word and `-tamoadd_lmem` hammers one local word
per workgroup. Both perform `.W` updates, including in an RV64 build; they
are not a direct `.D` engine throughput measurement. See the
[global loop](../../tests/regression/amo/kernel.cpp#L25) and
[local loop](../../tests/regression/amo/kernel.cpp#L273).
[amo.yaml](../../ci/testcases/amo.yaml#L18) runs functional cases on both drivers,
but does not declare an atomic `check: model_parity` case. The general parity
harness compares retired instructions and whole-application cycles;
[its check cases are pinned to RV32](../../ci/testcase.py#L89), so it does not
automatically certify these RV64 experiments.

Read-only tool discovery found Verilator at
`/export/nethomes/spark3100/tools/verilator/bin/verilator`:
`Verilator 5.046 2026-02-28 rev v5.046-55-g1264184fb`. It is not on the default
PATH; generated `config.mk` resolves it through `TOOLDIR`. `make`, Python,
the Vortex Clang binary and RV64 objdump are present. Neither `iverilog` nor
`pytest` was found on the default PATH. No RTL build was attempted.

A bounded follow-up using existing sources is the following, from a separately
configured RV64 build, after regenerating that build as required by AGENTS.md:

```bash
CONFIGS='-DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE' \
  ./ci/blackbox.sh --driver=simx --app=amo --cores=2 --warps=2 --threads=4 \
  --l2cache --args='-tamoadd -n64' --debug=3 --log=amoadd-simx.log
CONFIGS='-DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE' \
  ./ci/blackbox.sh --driver=rtlsim --app=amo --cores=2 --warps=2 --threads=4 \
  --l2cache --args='-tamoadd -n64' --debug=3 --log=amoadd-rtl.log
```

Repeat with `-tamoadd_lmem` and distinct log names for the local path. Use an
isolated build with FORZA/MMIO/UPDATE disabled; those SimX features have no RTL
implementation. These proposed commands have not been executed. They can
compare functional results and whole-kernel cycles and inspect bank-event
spacing in the existing cache traces. CPU issue gaps, warmup and response
backpressure prevent interpreting every observed gap as the minimum bank
initiation interval. A saturated, warmed bank-level stimulus with monitored
accept/commit events would be needed to measure that quantity directly; the
existing standalone stub is not such a stimulus. No new simulator or testbench
is introduced by this audit.

## Subsequently executed focused probe

The parent ran the existing atomic application in a separate native-only RV64
build, `build64-amo-audit`, with two cores, two warps/core, four lanes/warp,
L1D disabled, shared L2, and A enabled. FORZA/MMIO/UPDATE were disabled. The
exact same compiled guest was run on SimX and RTL, selecting `-tamoadd -n64`
or `-tamoadd_lmem -n64`. All four executions passed their existing functional
checks. No timing source was changed.

| Existing test | Retired instructions, both drivers | SimX cycles | RTL cycles | RTL / SimX |
| --- | ---: | ---: | ---: | ---: |
| Global contended AMOADD.W | 1,164 | 5,093 | 8,581 | 1.685 |
| Local contended AMOADD.W | 1,258 | 4,839 | 4,956 | 1.024 |

These are whole-application measurements including startup and memory-system
effects. The global case has a materially larger timing discrepancy than the
local control, consistent with investigating the source-level LLC gap. This
probe does not isolate every cause, measure saturated bank initiation interval,
or establish a correction factor for `.D` scatter, arbitrary workloads, or
the new engine (which still has no RTL). Do not multiply the architecture-search
results by these ratios or declare that an engine has therefore beaten RTL.

The [four-run probe manifest](forza_atomic_rtl_probe.json) records commands,
same-guest hash, logs, instruction/cycle counts, 21 source/library/build artifacts,
and the exact configuration. The earlier source-only observations remain
separate from this subsequent measured evidence.

The parent also ran the original frozen **AMOADD.D scatter guest itself**,
unchanged, on both native-only drivers: 1KiB state, 4,096 single-hot inputs,
seed17, one epoch. Both passed every result word and worker count, with input
hash `b7b852f54b3eb463` and state checksum `a1fca5c23162987b`. Whole-device
cycles after readback/flush were **52,262 SimX versus 80,727 RTL**. Processing
spans were 50,891 versus 62,636; startup, barriers, output and flush also differ.
This is the actual `.D` workload, not a correction inferred from the `.W` probe.
Its original binary hash, commands, two logs and exact output fields are nested
under `scatter_doubleword_probe` in the same manifest. Optional RTL memory
counters are zero in this build and provide no traffic evidence.

The new engine is still SimX-only. Comparing its 59,846 SimX cycles against the
native workload's 80,727 RTL cycles would mix models and does **not** prove an
engine win. This measured discrepancy instead makes LLC timing parity a concrete
prerequisite for a physically meaningful architecture ranking.
