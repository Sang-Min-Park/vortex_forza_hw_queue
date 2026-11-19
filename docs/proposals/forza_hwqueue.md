# FORZA hardware queues in SimX

Status: implemented SimX prototype. The extension is opt-in, RV64, and SimX-only.
The initial validation uses one simulator worker. `SIMX_FUNCTIONAL` is
unsupported because that mode disables the bounded-channel capacity contract.

## Purpose and topology

Model FORZA actor messages as bounded hardware queues, with explicit transport
backpressure and message SRAM, so software can exercise delivery and dequeue
semantics before an RTL implementation. Each Vortex core is one FORZA zone in a
single precinct. A persistent actor is a physical `(warp, lane)` pair, indexed
by `warp * threads_per_warp + lane`. Actor identity remains stable while its
warp remains active. Logical PE registration maps addresses to those physical
actors and eight mailboxes per actor; it need not equal the physical index.
The regression permutes logical PE assignments to verify this indirection.

The existing FORZA runtime cannot run unchanged: its Micro backend uses ECALLs,
older control-field shifts and CSR addresses, and one PE per ZAP. A Vortex
backend must supply CSR transport, physical actor topology, registration,
barriers, and the dequeue pointer contract. Preserve the runtime's public
Selector behavior where possible; changing the complete runtime is separate
work. An adapted device backend is now available in the isolated
`/export/nethomes/spark3100/forza-runtime-vortex` checkout; see
[runtime integration and team contract](forza_runtime_contract.md).
Fixed 56-byte payload assembly requires padding smaller application packets.

## ABI

Preserve the native message field layouts from
`forzartl_gt/hdl/include/{csr_intf,zxb_intf}.sv` and `zen_types.sv`, with the
address remapping below. Software must use `sw/common/forza_abi.h`.

| CSR | Native address | Vortex address | Meaning |
| --- | --- | --- | --- |
| ZENSTAT | `0x804` | `0x804` | Sender busy, error and enable status |
| ZQMSTAT | `0x805` | `0x805` | Mailbox ready `[7:0]`, errors `[15:8]` |
| ZQMMBOXREG | `0x806` | `0x806` | Logical PE and mailbox registration |
| ZQMDQ0..7 | `0x808..0x80f` | `0x808..0x80f` | Read pointer; write releases current message |
| ZENEQC / ZENEQD | `0x840 / 0x841` | `0x840 / 0x841` | Control / payload enqueue |
| ZENOMC | `0xcc3` | `0x850` | Eight outstanding counters, one byte per mailbox |
| ZQMAVAIL | `0xcc2` | `0x851` | Read alias for receive status |
| ZENTRY | — | `0x852` | Vortex-only atomic nonblocking commit; accepted / busy / invalid |
| ZQMMSG0..7 | `0xcc8..0xccf` | `0x858..0x85f` | Read aliases for dequeue pointers |

Vortex already assigns `0xcc2` to `CORE_ID` and `0xcc3` to `ACTIVE_WARPS`.
FORZA must never intercept these addresses: doing so changes hardware identity
and launch behavior. The Vortex mappings above avoid all 254 numeric CSR
definitions in generated `build64/sw/VX_types.h` (24 implemented FORZA
addresses checked). The remap is a required Vortex backend adaptation; native
FORZA binaries using the original counter/status addresses are incompatible.

The 64-bit control word contains local PE `[10:0]`, zone component `[14:11]`,
zone `[18:15]`, precinct `[31:19]`, mailbox `[34:32]`, AID `[39:36]`, opcode
`[47:40]`, and message ID `[60:48]`. The source control word is preserved,
including component, AID, opcode, and reserved fields. Routing and ACK
identity use separate packet metadata; the prototype does not synthesize
native transport fields into the stored control word. It transports arbitrary
opcodes for runtime interpretation, including direct and DONE messages.
Unsupported precincts or zones fail closed; AID, component, opcode, and
reserved fields are not validated.

ZENSTAT exposes mailbox busy `[7:0]`, mailbox errors `[15:8]`, message write
error `16`, available sequence count `[30:17]`, spawn busy `32`, spawn write
error `33`, and enable `63`. Spawn operations are outside this prototype.
Full counters and queues apply backpressure. Duplicate logical PE registration,
unregistered delivery, and invalid release report an error. A payload write
beyond seven words sets a sticky message write error; the following control
write is rejected. ZQM error bits remain zero because this SRAM model does not
inject receive faults. Reserved status bits read as zero.
The prototype's sequence-availability field reports
`255 - max(outstanding[0..7])`, rather than a native sequence-allocator pool.
If either shared TX FIFO is full, it conservatively marks every mailbox busy;
the status read does not yet know a future message's destination zone.

## Data path and storage

```text
actor CSR -> SFU -> ZEN per-actor assembler -> local[64] / external[64]
                                                    |
                                          ForzaFabric output RR
                                                    |
                   ZQM mailbox-index ingress[64] -> FREE -> FILLING -> READY
                                                    |                 |
actor LSU -> LocalMemSwitch -> ZQM SRAM <------------+       separate ACK plane
          load pointer/control/payload; fence; release CSR       -> source ZEN
```

Each core owns a ZEN with a seven-word payload assembler per physical actor.
The following control write commits one 64-byte packet containing the control
word and seven payload words. A short payload is allowed and its unwritten
words are zero-filled. Assembly state is isolated between actors. ZEN
has separate bounded local and external injection queues, each with capacity
64. A full queue applies backpressure through the CSR request/response path;
state advances exactly once when the request is accepted. A stalled sender
must not prevent other warps from polling or releasing receive mailboxes,
which would create a cycle between full data queues and blocked consumers.

The Vortex runtime uses `ZENTRY` rather than a blocking `ZENEQC` after a status
poll. Its CSRRW returns 0 on acceptance, 1 on busy, and 2 for invalid input.
A busy attempt leaves queues and outstanding counters unchanged and clears
only that actor's uncommitted assembler, so software can safely retry its
retained packet. The original blocking commit remains available. A unit test
checks two active lanes competing for the last TX FIFO entry: one is accepted
and the other returns busy without stalling the warp or leaking credit.

A shared `ForzaFabric` routes packets over SimChannels with round robin
arbitration per output. A blocked destination holds its packet until capacity
is available. Sender-owned registered stages bound every zone/fabric crossing
on both the data and ACK planes. Their local credits include deferred and
in-flight packets; a downstream pop returns credit after a registered delay.
This prevents a sender from overbooking a bounded sink before a deferred
cross-domain send becomes visible there.
The initial merged injection and fabric input FIFOs can cause
head-of-line blocking across destinations. ACKs travel on a separate channel
and cannot depend on capacity in a blocked data queue. Modules communicate
through channels; no module reaches into another module's state to enqueue,
acknowledge, or access memory.

Each core's ZQM provides eight mailboxes per registered actor and two 64-byte
slots per mailbox. Slots transition through free, filling, and READY states.
A READY slot remains stable until software releases its current dequeue
pointer. Receive ingress has one bounded FIFO per mailbox index, shared by
all destination actors. A blocked head remains at the front to preserve FIFO
order, so one full actor mailbox can delay other actors using that mailbox
index. Round robin arbitration still services other mailbox indices.
Message SRAM occupies the local-core aperture
`[0x70000000, 0x70100000)` (1 MiB). Each message is contiguous control plus
payload. Actor counts exceeding the aperture or native PE widths are rejected.

LSU loads to that aperture use a channel to the core's ZQM SRAM, including
appropriate load offsets and sizes. The aperture is load-only: stores and
atomics raise an explicit simulator error, as do loads beyond instantiated
SRAM. Software reads the READY dequeue pointer and finishes loading before
release; a caller must fence outstanding LSU loads before the release CSR.
Raw SRAM loads do not validate slot readiness. This prototype does not route
message storage through RZA or HBM.

ZQM generates the delivery ACK only after the complete packet is in SRAM and
its slot is READY. This is a deliberate prototype timing contract, distinct
from native hardware's earlier transport ACK. The ACK decrements the source
actor's originating mailbox outstanding counter. Delivery completion therefore
means the receiver can load the packet; it does not mean its handler has run or released the slot.
The first lossless prototype uses backpressure throughout and has no
NACK/retry machinery. No accepted valid packet may be dropped when a slot,
channel, or queue is full.

## Launch and progress constraints

Use the modern `vortex2` kernel entry and launch API with grid size equal to
the total core count, block size equal to `warps * threads`, and CTA cluster
size one. KMU round robin then places exactly one persistent CTA on each core,
and the CTA occupies all physical warps and lanes. Full-core block size alone
does not prevent later CTAs from reusing exited warps; this launch must not
contain extra CTAs or repurpose actors before their message state drains.

Actor state is per lane, but lanes are scheduled together in SIMT warps. A
blocking enqueue commit stalls its entire warp. A consumer lane in that same
warp cannot drain mailboxes while another lane remains blocked. The pressure
regression uses producer warp 0 and consumer warp 1. A runtime must organize
cooperative polling and bounded send batches around this constraint or assign
an independent service warp. The single-lane consumer uses a uniform warp
branch and an explicit one-lane thread mask, restoring all lanes before the
common barrier. Other lanes must not enter that barrier while the consumer
still needs to execute its divergent receive loop.

The initial tests use one Vortex cluster and hardware global barriers across
its cores. Ordinary cached RAM flag polling is unsuitable: private L1 caches
are noncoherent and `vx_fence()` does not invalidate them. Each participating
warp must execute the global barrier; no actor may exit early and leave peers
waiting. The supported envelope requires RV64, the timing simulation kernel,
one simulator worker, full-width SFU dispatch (`NUM_SFU_LANES == NUM_THREADS`),
VM disabled, at most 16 zones, and mailbox SRAM fitting the 1 MiB aperture
(at most 1024 physical actors per core). Reduced-width SFU dispatch can let
the last packet of a CSR instruction overtake a blocked earlier packet, so
per-instruction completion ordering needs a separate design before supporting
it. The current aperture is routed before the dcache MMU and does not implement
virtual-memory translation or protection.

## Validation and limits

Independent channel-level tests cover fabric output concurrency, arbitration
fairness, blocked-sink retention and ACK progress. The ZEN/ZQM test covers
per-lane registration, SRAM payloads, pointer lifetime and slot alternation,
assembly, delivery ACK identity, and outstanding-credit return. Its credit
stress holds 255 messages unacknowledged, checks that the 256th commit stalls,
services another warp's CSR meanwhile, returns one ACK, and verifies the
blocked commit completes exactly once before all credits drain to zero.
Short-payload zero filling is also checked.

The end-to-end configuration is two cores, two warps per core, and four lanes
per warp: 16 registered actors with reversed logical PE assignments. Each
positive run checks local and cross-core traffic, mailboxes 0 and 1, both SRAM
slots, all seven payload words loaded through the LSU, stable repeated peeks,
explicit release, per-sender ordering under shared-mailbox pressure, mixed
SRAM/cache loads in one instruction, and zero outstanding counters at exit.

| Check | Result | Saved output |
| --- | --- | --- |
| End-to-end `-n2` | PASS, 192 messages | `build64/tests/regression/forza_queue/validation-n2.log` |
| End-to-end `-n8` | PASS, 768 messages | `build64/tests/regression/forza_queue/validation-n8.log` |
| End-to-end `-n64` | PASS, 6,144 messages | `build64/tests/regression/forza_queue/validation-n64.log` |
| End-to-end `-n256` | PASS, 24,576 messages | `build64/tests/regression/forza_queue/validation-n256.log` |
| Invalid-zone, duplicate-registration, empty-release, and oversized-payload `-e` | PASS; no queued messages or leaked credits | `build64/tests/regression/forza_queue/validation-negative.log` |
| Fabric concurrency, fairness, FIFO, backpressure, independent ACKs | PASS | `build64-default/forza-fabric-test.log` |
| ZEN/ZQM CSR and SRAM, 255-credit saturation and cross-warp progress | PASS | `build64-default/forza-zone-test.log` |
| Extension disabled, `demo` | PASS | `build64-default/tests/regression/demo/demo-smoke.log` |
| Extension disabled, `sgemm -n16` | PASS | `build64-default/tests/regression/sgemm/sgemm-smoke.log` |

The queue tests above are transport tests. They do not run an actor handler or prove HClib
`finish`/DONE termination. The control opcode is transported for runtime
interpretation; no hardware DONE counter is provided.
Actual Selector handlers and software DONE are exercised separately by
`tests/regression/forza_runtime`; see the runtime contract for its results and
the distinction between Selector phase completion and full HClib finish.

Build and run from the isolated generated RV64 build tree. Reuse installed
tools and read-only dependency artifacts from the existing checkout; keep
original source and build artifacts unchanged. The initial regression uses
two cores, two warps per core, and four lanes per warp. The commands below
reproduce the prototype build and default positive test. Re-run
`configure` after source Makefile or TOML changes so the generated scripts and
headers are current. The app uses `KERNEL_LIB=vortex2`; the driver and app must
use identical feature and topology configuration:

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
mkdir -p build64
cd build64
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools \
  --osversion=ubuntu/focal --prefix="$PWD/install"
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C sw/kernel
env -u DEBUG make -C sw/runtime stub simx
env -u DEBUG make -C tests/regression/forza_queue
env -u DEBUG ./ci/blackbox.sh --driver=simx --app=forza_queue --cores=2 --warps=2 --threads=4
env -u DEBUG make -C tests/unittest/forza_fabric run
env -u DEBUG make -C tests/unittest/forza_zone run

(
  cd tests/regression/forza_queue
  export VORTEX_DRIVER=simx
  export LD_LIBRARY_PATH="$PWD/../../../sw/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ./forza_queue -n256
  ./forza_queue -e
)
```

The commands clear an inherited `DEBUG` value for the child process. This
workspace had `DEBUG=release`, whereas the simulator Makefile interprets a
defined `DEBUG` as a numeric trace level.

## Source locations

| File | Role |
| --- | --- |
| [sw/common/forza_abi.h](../../sw/common/forza_abi.h) | Shared CSR addresses, control packing and aperture constants |
| [sim/simx/forza/forza_zone.cpp](../../sim/simx/forza/forza_zone.cpp) | Per-core ZEN assembly, ZQM state, CSR arbitration, SRAM loads and ACKs |
| [sim/simx/forza/forza_fabric.cpp](../../sim/simx/forza/forza_fabric.cpp) | Data and ACK crossbar arbitration |
| [sim/simx/processor.cpp](../../sim/simx/processor.cpp) | Core/fabric bindings and registered credit stages |
| [sim/simx/mem/local_mem_switch.cpp](../../sim/simx/mem/local_mem_switch.cpp) | LSU routing and partial-response arbitration |
| [tests/regression/forza_queue/kernel.cpp](../../tests/regression/forza_queue/kernel.cpp) | Device-side CSR send/receive and stress test |
| [tests/regression/forza_queue/main.cpp](../../tests/regression/forza_queue/main.cpp) | Modern launch, result validation and timeout |
| [tests/unittest/forza_zone/main.cpp](../../tests/unittest/forza_zone/main.cpp) | Isolated CSR/SRAM and credit-saturation checks |
| [tests/unittest/forza_fabric/main.cpp](../../tests/unittest/forza_fabric/main.cpp) | Isolated connectivity checks |

The prototype establishes functional queue and backpressure behavior only.
It makes no RTL timing, synthesis, throughput, parallel-simulator, or
SimX/RTL parity claim. RTL, retry/recovery, multi-precinct routing, RZA/HBM
integration, and complete FORZA runtime adaptation require subsequent designs
and validation.
