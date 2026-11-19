# Experimental multiword destination transaction engine

This opt-in SimX prototype executes a serialized three-word record operation at its destination. It shares the existing 16KiB CPU LocalMem and the single UPDATE engine memory client. It does not add application-state SRAM, bypass LocalMem, alter bank timing, or implement RTL.

## Contract

`VX_CFG_FORZA_RECORD_ENABLE` requires the existing UPDATE, MMIO, and atomic extensions. The default is disabled. Each core holds up to 682 packed 24-byte records in its 16KiB LocalMem. The final 16 bytes are unused by records. A record contains three unsigned 64-bit fields: count, sum, xor.

For a request `(record, request_id32, delta32)`, the engine atomically returns the previous three fields and writes:

- count = old count + 1
- sum = old sum + sign-extended delta32
- xor = old xor XOR zero-extended request_id32

Addition wraps modulo 2^64. CPU code may initialize and export these records through its ordinary LocalMem path before and after the drained transaction phase. Concurrent CPU or ADD-engine accesses to these records are outside the contract. There is no lock protecting against such accesses.

## Instruction and wire interface

The shared ABI is `sw/common/forza_record_abi.h`. An AMOSWAP.D targets `0x71500000 + zone * 0x4000 + record * 8`. Its operand is `(uint64_t(request_id) << 32) | uint32_t(delta)`. `forza_record_address` and `forza_record_pack` construct these values. The return value is ACCEPTED, BUSY, or INVALID. The encoded record index selects a physical LocalMem byte offset of `record * 24`, not `record * 8`.

The source must register mailbox 1 before issuing a request. The hardware captures its registered logical PE. The request is a normal 64-byte physical frame with opcode 0x81, mailbox 0 credit, record index in the destination PE field, packed operand in payload word 0, and the captured return PE in payload word 1. This use of the destination PE field denotes a record only for opcode 0x81.

The reply is another 64-byte frame, opcode 0x82, addressed to the captured source PE on mailbox 1. Its first four payload words are request ID, old count, old sum, old xor. All unused words are zero. The reply uses the existing two-slot mailbox SRAM and normal readiness/fill path. It is not stored in a separate ideal response memory.

The new MMIO endpoint offsets are PEEK at 0x140 + mailbox*8 and RELEASE at 0x180 + mailbox*8, relative to the actor endpoint base. The physical actor stride remains 0x200. As with other per-actor MMIO operations, `forza_mmio_actor_address` must encode the physical actor in the address so compiler divergence analysis sees different lane endpoints.

PEEK is an 8-byte load returning zero for no ready message, otherwise the address of its control word. Payload starts at pointer + 8. RELEASE is an AMOSWAP.D, returning ACCEPTED for a ready slot and INVALID for an empty one. The release operand is semantically ignored by the device, but software must use an actual register dependency on the control and every copied payload word, preserving the ordered receive lifetime rule. A fence with no such dependency does not by itself stop a younger SFU CSR from issuing in this SimX pipeline.

## Bounded execution resources

The destination has a 32-entry request queue and four transaction contexts. Each context holds request identity and three old/new 64-bit values. A scoreboard serializes the same record; different records may use separate contexts. The request queue remains FIFO, so a locked head may delay other records.

Each transaction issues exactly three separate 8-byte LD requests and three separate acknowledged 8-byte ST requests with `strsp=1`. Loads extract only their addressed word from the functional 64-byte response backing. Stores enable only eight bytes. All six accesses pass through the actual LocalMem crossbar and banks. A tag, synthetic engine hart, and per-operation UUID validate every response; malformed, duplicate, or stale responses cannot retire another field.

There is at most one memory request per core per cycle shared between ADD and record execution, with alternating first choice. There is at most one registered compute stage per core per cycle, assuming two 64-bit adders and one XOR. Compute occurs after all three load responses. The record lock is released only after all three store responses. Existing LocalMem capacity, bank occupancy, priority, response queues, and channel delays remain unchanged. Six memory issue opportunities plus the compute stage are a lower bound, not the observed total transaction latency.

A context retains its old snapshot until its reply has reached a READY mailbox slot and its delivery ACK returns. Only then does the engine send the original request ACK. Therefore mailbox-0 outstanding zero means the record is committed and the reply is READY; it does not mean software consumed the reply. Engine replies use a distinct synthetic source actor equal to the configured actors/core, with a separate bounded sequence-to-context map.

## Reply virtual channel

Requests and recursive replies cannot safely share only one blocked FIFO: a full request queue can otherwise hide replies needed to release all contexts. The prototype uses independent bounded reply channels through zone and fabric boundaries. Request and reply virtual channels jointly arbitrate the same physical data input/output: at most one total 64-byte frame per physical input and output per cycle. The ACK plane is unchanged. Additional reply buffers and virtual-channel arbitration are explicit new resources; there is no added physical link bandwidth.

The unit suite floods both cores until each source reaches BUSY, without consuming any replies, then drains the complete transaction set. This tests reply escape under simultaneous saturated request queues. Ordinary mailbox head-of-line blocking and the LocalMem client's lowest priority remain limitations. Programs must cooperatively receive while retrying BUSY; blocking all receiver execution can still prevent progress.

## Verification and interpretation

`tests/unittest/forza_record` binds the engine to the actual LocalMem implementation. It checks three LD plus three acknowledged ST accesses per transaction, exactly eight enabled bytes each, signed/wrapping arithmetic, the last record, old snapshots, same-record serialization, withheld store responses, withheld reply delivery, finite queue backpressure, exact source-credit recovery, and malformed response identities. A second fixture uses the actual jointly arbitrated ForzaFabric for symmetric two-core saturation. Normal and ASAN/UBSAN runs are recorded under the fresh `build64-forza-record/tests/unittest/forza_record` directory.

Performance comparisons must include strong global/local GPU lock controls and an efficient owner scheduling or prefix/bucket control for independent bulk input. Returning an atomic old tuple is the same required semantic in every mode. This engine is a new resource/operation proposal, not a tuning change to old GPU latency or proof that message passing is always faster.
