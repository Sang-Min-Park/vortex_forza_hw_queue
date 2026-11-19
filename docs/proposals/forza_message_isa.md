# Generic message ISA experiment

The opt-in `VX_CFG_FORZA_ISA_ENABLE` SimX prototype compares a decoded GPU
message interface with existing MMIO, without changing arithmetic handlers,
packet bandwidth, queue depths, or mailbox capacity. It has no matching RTL yet.

The intended benefit is reduced address/LSU command overhead and eliminating
shader retry/poll instructions while a warp waits for queue state. These are
testable architectural hypotheses, not a prediction of native-GPU speedup.

## Encoding and software contract

`CUSTOM0` opcode `0x0b`, funct7 `0x20`, and funct3 select the operation. It is an
R-type instruction with integer rs1/rs2 and integer rd. Source and destination
values are independent for every active lane; inactive lanes have no effects.

| funct3 | Operation | rs1 | rs2 | rd |
|---|---|---|---|---|
| 0 | SEND.TRY | route | opaque nonzero64 token | ACCEPTED0 / BUSY1 / INVALID2 |
| 1 | RECV.TRY | mailbox0..7 | ignored | token, or0 if empty |
| 2 | SEND.WAIT | route | opaque nonzero64 token | ACCEPTED0 / INVALID2 |
| 3 | RECV.WAIT | mailbox0..7 | ignored | token; waits for every active endpoint |
| 4 | MSG.CREDITS | ignored | ignored | eight packed8-bit delivery counters |
| 5 | MSG.REGISTER | `(logical_PE<<8) | mailbox_mask` | ignored | ACCEPTED0 / INVALID2 |
| 6 | MSG.STATUS | ignored | ignored | existing ZEN status/error encoding |
| 7 | RECV.WAIT.ANY | mailbox0..7 | ignored | tokens where available,0 elsewhere; waits for any active endpoint |

The route uses existing control fields: zone bits15..18, zone-local PE bits0..10
and mailbox bits32..34; all other bits must be zero. The helper validates field
bounds. The hardware additionally validates the actual instantiated zone count.
Zero is reserved by the opaque-token receive ABI and SEND rejects it.

Receive copies the eight-byte token and releases the slot. It does not return
a borrowed pointer into mailbox SRAM. `WAIT.ANY` permits servers whose active
lanes have uneven incoming work; `WAIT` requires eventual traffic to every
active endpoint and is appropriate only when software guarantees that progress.
Malformed receive arguments complete with0 and set the existing sticky error
status, rather than parking forever. Register collision semantics are unchanged.

## Bounded execution and ordering

The decoder parks only the issuing warp. SFU submits the original instruction
trace through its existing ForzaZone request/response channels. The bounded
work queue round-robins parked instructions, retaining completed-lane state so
neither accepted sends nor consumed tokens are repeated. Queue state changes
enable another attempt without issuing shader retry/poll instructions. SFU is
not held while a trace is parked; other warps can receive or return space.

ISA and MMIO share a command budget. With vector MMIO it is at most the
configured LSU lane count per core per cycle; otherwise one. Alternating first
choice prevents a busy MMIO producer from permanently excluding an ISA receiver
or vice versa. Partial MMIO response masks retain the existing LSU response
identity and advance only processed lanes. Network injection, incoming queues,
one-slot reservation/fill per cycle, two slots/mailbox and ACK timing are
unchanged. Existing normal CSR semantics retain their separate control path.

The new pending state is bounded by the existing work-queue capacity
`2 * NUM_WARPS`, plus the existing channel capacities, and adds completed-lane
masks/cursors to retained traces. Fetch stalling prevents an actual warp from
issuing another such instruction before completion. There is no unbounded
per-message host queue or unmodeled bypass into another SimObject.

The inline assembly has a compiler memory clobber. This is **not** a hardware
release/acquire guarantee for preceding GPU stores. Descriptor publishers must
use the supported explicit visibility protocol, or send immutable descriptors/
inline tokens. Receive does not invalidate unrelated application caches. Delivery
credit zero does not mean a software handler or application phase completed.

## Validation

`tests/unittest/forza_isa` exercises try/send/receive, exact token bytes, credits,
bounded TX backpressure, send-wait wakeup, all-lane and any-lane receive waits,
inactive lanes, preexisting arrivals, completed-lane retention, other-warp
registration and receive progress, and shared MMIO/ISA command arbitration.
End-to-end regression kernels exercise the actual decoder, operand collection,
SFU channel and GPU writeback path using the helpers in
`sw/common/forza_isa_abi.h`.

All performance modes must use matching compile flags, the same frozen driver
and same resources. A WAIT path that blocks every possible receiver is a
software scheduling deadlock; additional service capacity cannot be assumed
for free when comparing execution layouts.
