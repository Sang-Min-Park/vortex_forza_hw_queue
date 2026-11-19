# LSU message command aperture

This opt-in SimX prototype adds an acknowledged 8-byte message interface to
the existing per-core FORZA zone. It requires RV64, `VX_CFG_EXT_A_ENABLE`,
`VX_CFG_EXT_FORZA_ENABLE`, and `VX_CFG_FORZA_MMIO_ENABLE`. The new flag is off
by default. The original CSR path and all frozen builds remain available.

An ordinary posted store cannot tell the caller that a bounded queue rejected
its packet. Holding such a store until receiver space appears could also block
that core's receive operations. The prototype instead defines device-command
semantics for `amoswap.d` at a dedicated uncached aperture. The actual instruction
passes through the LSU, local-memory switch, bounded device channels, and LSU
response/writeback path. It is not a host call or an ordinary RAM RMW.

## Interface

The shared C ABI is `sw/common/forza_mmio_abi.h`. The aperture is
`0x71000000..0x713fffff`, local to the issuing core's device.
The table lists actor-zero endpoints; REGISTER/status/count/POP must be
translated with `forza_mmio_actor_address` for the issuing physical actor.

| Endpoint | Instruction | Operand and result |
| --- | --- | --- |
| TX, first 2 MiB | `amoswap.d` | Address encodes destination zone/local PE/mailbox; rs2 is all 64 payload bits; rd is ACCEPTED=0, BUSY=1, INVALID=2 |
| REGISTER, base+0x200000 | `amoswap.d` | rs2 is `(local_pe << 8) \| mailbox_mask`; rd is ACCEPTED or INVALID |
| REGISTER | `ld` | Current actor registration |
| RX_STATUS, base+0x200040 | `ld` | Mailbox-ready bitmap, same meaning as ZQM status |
| TX_STATUS, base+0x200080 | `ld` | Sender busy/error status, same meaning as ZEN status |
| TX_COUNTS, base+0x2000c0 | `ld` | Eight outstanding delivery counts, one byte per mailbox |
| RX_POP, base+0x200100+8*mailbox | `amoswap.d` | rs2 ignored; captures the current slot's first payload word into the response and atomically frees that slot |

TX address bits 20:6 contain `(zone << 11) | local_pe`; bits 5:3 contain the
mailbox. `forza_mmio_tx_address` and `forza_mmio_pop_address` construct these
addresses and return an invalid-command address for out-of-range fields. The
source actor is selected by the request's issuing warp and original thread ID.
REGISTER, status, count, and POP addresses additionally encode that physical actor:
`forza_mmio_actor_address(actor, endpoint)` adds `actor * 0x200` to an actor-zero
endpoint. The device rejects addresses encoding a different actor. The endpoint
half of the aperture holds 4096 actors. TX destination addresses are unchanged.

The explicit actor bits also make per-thread status loads visibly divergent to
the SIMT compiler. A single constant endpoint address returning hidden
actor-dependent values would violate LLVM's uniform-load assumption and could
make a whole warp skip POP even while some lanes have ready messages.

Each successful enqueue creates a DIRECT message with one payload word and
zeroes the other six words. It shares the existing bounded local/external TX
queues, outstanding-credit accounting, crossbar, ACKs, receive queues, and two
mailbox slots. BUSY causes no enqueue, sequence allocation, or credit change.
The caller retains the packet, performs receive/progress work, and retries.
Delivery ACK errors remain visible through sender status.

RX is a checked, single-consumer protocol:

```text
ld ready, actor_endpoint(RX_STATUS)
if ready & (1 << mailbox):
    amoswap.d payload, zero, actor_endpoint(RX_POP[mailbox])
    process(payload)
```

The status check does not consume a message. With one consumer per actor and
mailbox, a ready slot remains available until that consumer pops it. All 64-bit
payload values, including zero, are valid. An unchecked pop of an empty mailbox
returns zero without consuming anything, so that return alone is not a valid
availability test. The payload is copied into an independent response block
before the slot becomes reusable; there is no subsequent payload load or
separate release instruction to order.

Posted stores are explicitly unsupported. Unsupported AMO operations, AMOs of
the wrong width, malformed endpoints, and uninstantiated destination zones
return INVALID without enqueueing. Registration retains the existing one-shot
mapping rules. This interface does not add a generic actor finish protocol or
support messages wider than eight bytes.

## Service model and limits

The command decoder has one shared 64-bit service port per zone. It round-robins
LSU blocks and processes one active lane per cycle, returning a one-lane LSU
response fragment with the original tag, CID, UUID, and lane mask. Bounded
response-channel capacity is checked before a command mutates device state.
Blocked outputs backpressure ingress; queue-full TX attempts promptly return
BUSY so they do not wait for receiver processing. Normal mailbox SRAM loads
retain their existing path. Mixed SRAM/MMIO load packets also return correct
lane fragments.

The existing CSR model can handle all active lanes of a CSR packet in a tick;
the new single-port command service is a deliberately explicit, different
throughput assumption. No physical message compression is claimed: frames,
slots, and fabric transfers remain 64 bytes. This experiment changes the
instruction interface, not the fabric width or the placement of application
state. Native GPU AMOs to ordinary global memory retain their existing meaning.

`VX_CFG_FORZA_MMIO_VECTOR_ENABLE`, off by default and requiring MMIO/A, selects
a separate resource assumption: up to `VX_CFG_NUM_LSU_LANES` independent 64-bit
command services act on one selected LSU request in a cycle. With four lanes
this means four command/state ports and a multi-enqueue TX FIFO, comparable to
the existing CSR model's all-lane service. All active lane results return in one
full-mask response. One vector is selected across all LSU blocks per zone per
cycle; servicing it prevents the following legacy SRAM request from also being
processed in that cycle. An insufficient TX FIFO can accept some lanes and
return BUSY for the others. Response capacity is checked before any lane acts.

This option adds no queue slots, cache capacity, application SRAM capacity, or
fabric bandwidth. The fabric still transfers at most one message from a zone
per cycle. The four command ports are an explicit additional resource assumption,
not a claim of the same hardware area or a PPA result. Scalar mode and native
memory operations retain their existing cycle behavior.

`tests/unittest/forza_mmio` checks channel-only device behavior: fragmented
multi-lane responses, sparse masks, thread identity, address byte offsets,
registration, zero payloads, slot reuse, invalid commands, bounded TX rejection,
receive progress while TX is full, FIFO order, and ACK credit return. Actual
guest tests must additionally verify LSU instruction completion and masked
multi-lane kernel execution against a CPU reference.
