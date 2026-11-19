# SimX bit-manipulation regression

Validation on 2026-11-19 passed all 52 positive programs (4,450 RV32 vectors and
5,226 RV64 vectors) and all 52 intentionally failing negative programs. The CSR
queue, Selector runtime and drained endpoint-reuse baseline also passed after
integration. [Results, simulator hashes and logs](../../../docs/forza_evidence/bitmanip-integration/summary.json)
are preserved.

The implementation was recovered from the separate `vortex-forza-step12`
working tree and integrated with the FORZA queue decoder. It covers these
26 RV32/RV64 instruction spellings:

```
andn bclr bclri bext bexti binv binvi bset bseti
clz cpop ctz max maxu min minu orc.b orn rev8
rol ror rori sh1add sh2add sh3add xnor
```

This is a subset of Zba/Zbb/Zbs. It does not claim complete extensions,
RV64 word/`.uw` variants, `sext.b`/`sext.h`/`zext.h`, Zbc, or RTL support.
The simulator recognizes the listed encodings without a new configuration flag.
Latency follows the existing two-cycle simple ALU model and is not RTL timing
validation. `sw/common/bitmanip.h` is a host helper header and is distinct from
this instruction decode/execution support.

`run.py` generates self-checking standalone assembly and compares execution
against independent Python reference values. Cases include zero, high bits,
signed/unsigned comparisons, shift boundaries, all immediate bit indices,
deterministic random operands, destination/source aliases and `x0`. Negative
programs deliberately corrupt the first expected value and must exit 1; positive
programs must exit 0. The HTIF completion word is placed in the existing uncached
IO range by `link.ld` to avoid cached completion polling.

After installing the Vortex toolchain and building SoftFloat/Ramulator:

```bash
export TOOLDIR="$HOME/tools"
bash ci/run_bitmanip.sh 32
bash ci/run_bitmanip.sh 64
```

To verify coexistence with the FORZA CSR queues in one configuration:

```bash
bash ci/run_forza_csr.sh
```

This builds FORZA-enabled RV64 SimX, runs all 26 positive and 26 negative
bit-manipulation programs, then checks the CSR queue, Selector runtime and
drained physical-endpoint reuse.

Each command configures a separate generated build directory, builds standalone
SimX, and runs positive and negative programs for every listed mnemonic. Results,
assembly, ELF, disassembly and logs remain in that build directory under
`tests/unittest/bitmanip/results/`. Binaries are not committed.

For selected instructions in an already configured build:

```bash
make -C tests/unittest/bitmanip run BITMANIP_TEST_ARGS='--ops andn orn xnor --negative'
```

The prior FORZA assembly filtering and `dscratch0`/`time` compatibility work are
separate from these bit-manipulation changes. FORZA-only atomic, migration,
PZOP, spawn and quit encodings are not enabled by this implementation.
