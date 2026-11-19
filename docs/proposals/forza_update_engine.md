# Destination LocalMem UPDATE engine

`VX_CFG_FORZA_UPDATE_ENABLE` is a default-off SimX experiment requiring the
FORZA/MMIO/A extensions and the existing 16KiB core-local memory. It adds one
request/response client to that same banked LocalMem. Capacity, bank count,
bank arbitration, and atomic latency are unchanged. The engine never reads or
modifies application state directly in C++.

`sw/common/forza_update_abi.h` defines an AMOSWAP.D command aperture:

```text
address = 0x71400000 + destination_zone * 16384 + destination_word_offset * 8
rs2 = full 64-bit add operand
rd = ACCEPTED / BUSY / INVALID
```

Word offsets are 0..2047. The routed aperture is 512KiB; its first 256KiB holds
16 possible zone windows and the rest is reserved for invalid-command handling.
An uninstantiated zone or invalid offset is rejected. The result reports enqueue
acceptance, not the old value of the target word.

Accepted commands use the existing bounded local/external TX queues, sequence
ownership, source mailbox-0 credits, and 64-byte message fabric. Opcode 0x80
identifies UPDATE. For this opcode the destination field contains a LocalMem
word offset rather than a logical actor PE; payload word 1 contains the add
operand. Destination HART registration and software mailbox slots are unused.

The destination queues at most 32 requests and tracks at most 32 in-flight
LocalMem transactions. It issues at most one real `MemOp::AMO_ADD` per cycle
through its single added port. The existing LocalMem bank handles the RMW and
returns its normal response. The engine verifies tag, dedicated synthetic hart
ID, UUID, and response data before acknowledging completion. It sends no ACK
merely for arriving at the destination or entering the engine queue.

Source mailbox-0 outstanding count reaching zero therefore means its UPDATEs
have committed, provided that count contains only UPDATE traffic. Ordinary
messages retain their delivery-ACK meaning. A program must drain outstanding
UPDATEs before ending the phase; this is not a generic actor finish protocol.
INIT and READ use normal CPU accesses to `__local_mem()`, and software must
publish initialization before remote updates begin.

The new port shares existing banks and has the priority assigned by the current
LocalMem crossbar. It does not add a dedicated ideal state SRAM. Same-word atomic
conflicts, finite engine buffers, shared ACK bandwidth, and head-of-line blocking
still apply. Mixed traffic and fixed-priority memory arbitration do not provide
a new starvation-freedom guarantee. No RTL or PPA equivalence is claimed.

`tests/unittest/forza_update` binds the engine to the actual LocalMem model and
uses a separate CPU memory client for initialization and validation. Normal and
ASAN/UBSAN tests cover signed/full64 additions, last-word bounds, response-held
completion, finite queue/in-flight/ACK backpressure and exact recovery. Separate
fault cases verify that a matching tag with a wrong UUID or hart is rejected
before ACK or pending-tag release.
