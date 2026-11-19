# FORZA CSR queue handoff

This repository preserves the Vortex SimX FORZA queue prototype, its optional
experiments, and the recorded results. Start with the CSR path. This is a C++
hardware-behavior model; the FORZA extension has not been integrated and
validated in Vortex RTL. The original FORZA RTL and assembly tests live in a
separate project and are not included here.

## Read the implementation

| File | Purpose |
| --- | --- |
| [forza_zone.h](../sim/simx/forza/forza_zone.h) | Per-actor assembly, mailbox state, shared TX/RX queues, SRAM |
| [forza_zone.cpp](../sim/simx/forza/forza_zone.cpp) | CSR handling, receive/load/release, ACKs and cycle progression |
| [forza_fabric.cpp](../sim/simx/forza/forza_fabric.cpp) | Output arbitration and separate message/ACK channels |
| [forza_abi.h](../sw/common/forza_abi.h) | CSR addresses and packet layout |
| [forza_zone_train.cpp](../sim/simx/forza/forza_zone_train.cpp) | Reading-only CSR extraction; never compile alongside the original |
| [forza_hwqueue.md](proposals/forza_hwqueue.md) | Detailed queue contract and original validation |
| [forza_runtime_contract.md](proposals/forza_runtime_contract.md) | Original FORZA vs Vortex adaptation, lifecycle and progress limits |

One core is one zone. A physical `(warp, lane)` is an actor, indexed by
`warp * threads_per_warp + lane`; logical PE registration maps addresses to
those physical endpoints. Each actor has eight mailboxes with two 64-byte
slots each. Payloads contain a 64-bit control word plus seven payload words.

The receive path is fabric -> registered channel boundary -> `net_rx` ->
mailbox-index ingress FIFO -> destination actor's SRAM slot. The SRAM is
additional core-local message storage. It is not the original FORZA RZA/HBM
storage path. `RegSlice`/`net_rx` are SimX integration choices, not a literal
copy of the original FORZA fabric.

## Prepare dependencies and run the CSR baseline

Use a compatible Vortex toolchain; the pinned prebuilt toolchain revision is
in [VERSION](../VERSION). See [installation](install_vortex.md) for host and
toolchain setup. Keep hardware configuration identical across kernel library,
simulator driver and application. The baseline requires RV64, VM off, one
SimX worker and full-width SFU dispatch.

```bash
git submodule update --init --recursive
make -C third_party softfloat ramulator
export TOOLDIR="$HOME/tools"
bash ci/run_forza_csr.sh
```

The Vortex-only FORZA runtime backend is bundled under
`sw/forza_runtime`; no sibling checkout or authenticated upstream clone is
required for the CSR tests. The backend was adapted against upstream FORZA
runtime commit `138df12fbe56d2bc248c596841ac23703217e6d8`. The
[adapter patch](forza_runtime_adapter.patch) preserves the original split-checkout
modifications for provenance. `FORZA_RUNTIME_HOME` can select another compatible
runtime, but the bundled backend is the default for the FORZA regression Makefiles.

Existing dependency builds may be reused through `THIRD_PARTY_DIR` and
`FORZA_RUNTIME_HOME`. The default smoke script creates a separate generated
build directory, builds matching artifacts and checks the queue, actor runtime
and drained physical-endpoint reuse. Logs are under that build's `diagnostics`.

To test eight lanes without mixing build configurations:

```bash
FORZA_THREADS=8 bash ci/run_forza_csr.sh "$PWD/build64-forza-csr-8t"
```

## Handoff checkout validation

On 2026-11-19, the prepared handoff checkout was freshly configured and built
with the bundled runtime using `ci/run_forza_csr.sh`. At 2 cores, 2 warps/core,
4 threads/warp and 4 SFU lanes, it passed 768 raw queue messages, 384 Selector
handler calls, and 128 drained self-messages across four full-core CTAs.
[Validation and logs](forza_evidence/handoff-smoke/validation.json) are included.
The existing 64-actor runtime and registration limits were not changed.

## Recorded results and their limits

These results were recorded before this handoff. Source hashes and individual
logs are preserved; they are not claims that every configuration passes today.

| Cores | Warps/core | Threads/warp | Actors | Raw CSR queue | Selector runtime |
| ---: | ---: | ---: | ---: | --- | --- |
| 2 | 2 | 4 | 16 | PASS | PASS |
| 2 | 2 | 8 | 32 | PASS | PASS |
| 4 | 2 | 4 | 32 | PASS | PASS |
| 4 | 2 | 8 | 64 | PASS | Host timeout |
| 2 | 4 | 8 | 64 | PASS | Host timeout |

The four-core 64-actor runtime also failed to complete with a 300-second event
wait using the unchanged device binary. This does not establish the cause or
prove deadlock. The runtime checks ring, fan-in and handler-generated replies;
all lanes are actors, with no dedicated service warp.

For the 16-physical-endpoint lifetime probe, two and four sets of full-core
CTAs passed when retaining physical PE identities and draining all traffic.
New logical PE identities on reused endpoints were rejected: 16 registration
errors for four blocks and 48 for eight blocks. These are actual failed probes,
not silently classified as successful tests. To reproduce the latter:

```bash
cd build64-forza-csr/tests/regression/forza_csr_generalize
LD_LIBRARY_PATH=../../../sw/runtime VORTEX_DRIVER=simx \
  ./forza_csr_generalize -w2 -m logical -n4
```

Expect a nonzero exit and registration errors. Passing physical reuse tests
only drained self-messaging; it does not establish arbitrary cross-block
communication, delivery to nonresident blocks or logical-actor virtualization.

- [Full recorded results and hashes](proposals/forza_csr_generalization_results.json)
- [Published evidence index](forza_evidence/manifest.json)
- [Lifecycle probe](../tests/regression/forza_csr_generalize/README.md)
- [Original runtime regression](../tests/regression/forza_runtime/README.md)

Archived documents contain original absolute paths and build-directory links.
Those are provenance records, not portable setup commands. The evidence index
maps copied CSR logs and scripts to their original paths. Earlier exploratory
reports, optional ISA/MMIO/update/record/VM sources and their experiments are
preserved, but the default configuration keeps those optional paths disabled.

## Next investigations

1. Locate the 64-actor runtime's lack of completion; separate queue, runtime,
   compiler and test-protocol causes before changing semantics.
2. Define actor registration teardown, draining and generation/ownership rules
   before allowing a physical endpoint to acquire a new logical identity.
3. Define how messages and execution progress work for nonresident receivers.
4. Compare against a software queue with matched capacity, resources, ordering
   and completion semantics. Account for added SRAM and transport buffers.

Hardware thread migration, RZA operations, `spawn`, `quit` and the original
FORZA instruction encodings are outside the validated CSR queue contract.
The upstream Vortex history and licenses are retained. The handoff baseline
also contains an earlier local DOT8 snapshot at commit `1ab680880`.
