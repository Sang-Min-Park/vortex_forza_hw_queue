# Feedback-dependent generic messaging versus GPU execution

This report compares18 primary conditions using two ordinary GPU handlers, actual SEND/RECV and wait-any notification, and strong direct/owner GPU controls. Wins and losses are tabulated below. These are SimX results, not RTL or physical GPU measurements.

## Contract and controls

The machine is two cores, two warps/core and four lanes/warp, with L1D disabled and shared L2 enabled. UPDATE and RECORD arithmetic engines are disabled. Eight persistent client chains have one outstanding query each. A returned key and a16-bit cookie derived from the previous result determine the next request; even single-hot-key payloads cannot be prepared in advance. Walk executes1/8/32 dependent owner-shard probes. Update conditionally changes a32-bit value and its version using ordinary GPU instructions. Every mode executes the same client program and committed work count.

MMIO, ISAtry and ISAwait use identical generic queues/fabric/two-slot mailboxes. Wait servers use receive-wait-any to allow inactive endpoints. Each physical server lane exclusively owns a key stripe. No host per-owner incoming counts are supplied. Eight client DONEs reach one coordinator, which sends eight STOPs, itself last; all endpoint credits drain before completion. All replies are self-contained8-byte tokens in64-byte physical frames.

The update response and next key depend on the actual return values of both state AMOSWAP operations. Therefore both state stores commit before a response can issue through SFU or MMIO. No pointer-notification publication shortcut is used.

Native direct control runs the same eight chains persistently, using ordinary global loads for walk and cooperative record locking for update. The stronger native walk control replicates immutable next/value data in existing LocalMem, including timed initialization. The packed update control uses one64-bit value/version word with LR.D/SC.D retries, including GPU packing/unpacking and export. LR/SC retries include write invalidation and shared-line reservation interference; they are not assumed equivalent to ideal CAS.

Ordinary GPU software owner queues provide the same owner placement and handler without the new message hardware: client/server SPSC request slots, one reply slot/client, coordinator STOP slots and ready-bit masks, all in global memory. ACKed payload AMOSWAP precedes ready AMOOR; consumption clears slot and readiness before reply. At eight clients these queues occupy704B global memory. All slots/masks must be empty at completion.

Owner-local variants use actual packed24B records in the existing16KiB/core LocalMem, initialized/exported inside the timed GPU work. Global-state controls are retained. Replicated immutable state uses16B/key/core, within the same physical capacity. Thus a locality effect cannot silently be credited to hardware messaging alone.

## Primary results

Each row compares matching handler, chain depth, work/probe count, input and seed17. Lower cycles are better. GPU direct is the minimum of global and replicated/packed controls as applicable; software owner is the best measured ordinary-memory queue mode. HW is the best measured MMIO/ISA mode. Extension cases retain the strongest controls and ISAWait global/local rather than exhaustively rerunning every interface.

| Handler | Steps/client | Work | Pattern | GPU direct | GPU software owner | HW queue best | HW / best standard GPU |
|---|---:|---:|---|---:|---:|---:|---:|
| update | 128 | 1 | hot | 652,742 | 3,631,100 | 3,217,559 | 4.93x |
| update | 128 | 1 | random | 198,710 | 1,146,739 | 895,494 | 4.51x |
| update | 128 | 8 | hot | 1,909,068 | 4,584,101 | 4,056,207 | 2.12x |
| update | 128 | 8 | random | 386,678 | 1,380,342 | 1,089,741 | 2.82x |
| update | 128 | 32 | hot | 4,784,460 | 7,460,130 | 6,931,600 | 1.45x |
| update | 128 | 32 | random | 773,167 | 2,301,928 | 1,904,051 | 2.46x |
| update | 512 | 1 | hot | 2,595,017 | 14,436,604 | 12,794,903 | 4.93x |
| update | 512 | 1 | random | 805,516 | 4,518,124 | 3,520,716 | 4.37x |
| update | 512 | 8 | hot | 7,622,607 | 18,241,075 | 16,149,519 | 2.12x |
| walk | 128 | 1 | hot | 132,468 | 3,103,132 | 2,679,867 | 20.23x |
| walk | 128 | 1 | random | 132,569 | 984,507 | 766,646 | 5.78x |
| walk | 128 | 8 | hot | 317,940 | 4,839,496 | 4,170,823 | 13.12x |
| walk | 128 | 8 | random | 318,017 | 1,510,130 | 1,133,355 | 3.56x |
| walk | 128 | 32 | hot | 953,844 | 10,590,549 | 9,282,624 | 9.73x |
| walk | 128 | 32 | random | 953,920 | 3,132,092 | 2,535,351 | 2.66x |
| walk | 512 | 1 | hot | 522,243 | 12,325,057 | 10,644,411 | 20.38x |
| walk | 512 | 1 | random | 522,344 | 3,987,077 | 2,980,407 | 5.71x |
| walk | 512 | 8 | hot | 1,264,131 | 19,263,197 | 16,608,199 | 13.14x |

Hardware messaging wins **0/18** primary conditions. Across matched quick cases, MMIO/ISAWait cycle ratios range from1.045x to1.138x. This is an implementation comparison including instruction/control/register-allocation differences, not an isolated count of polling cycles removed.

The feedback protocol has at most eight live queries and reports no enqueue BUSY in these tests. It exercises dependent execution and receive wakeup; it is not the saturated producer-pressure test used by the separate bulk benchmark. Walk traces and state checksums match exactly across all modes. Mutable update schedules may differ: requests and final values depend on prior replies. All modes have equal initial state, client count and committed iteration count, but cache paths can vary with legal interleavings.

## Exact verification and provenance

**200/200 accepted executions pass:**147 unchanged native/hardware rows,40 corrected software-owner-queue rows and13 isolated packed-control rows. All eight unaffected modes were rebuilt with byte-identical host binary, ELF, vxbin and disassembly (32 identity checks) and identical four libraries before reusing those147 runs. The old40 software queue rows remain diagnostic and are excluded. Every query key/cookie, reply and final record is checked. For update, the CPU constructs client-order and per-key-version edges, rejects causal cycles and replays all operations. Retry recomputation is timed; for packed mode each failed SC repeats the arithmetic work and is reported as a retry.

Source, host/guest binaries, four shared libraries, build logs and every run log are frozen and reverified. Pre-identity/pre-cookie exploratory builds and the earlier pre-publication-dependency cohort remain diagnostic only. The latter was stopped after identifying that ignored AMOSWAP results did not guarantee state-before-reply ordering; its source snapshots, binaries, completed logs and cause JSON are preserved. A second software-queue-only correction makes token validity depend on the old bit returned by AMOAND, ensuring ready-mask clear commits before a response allows that producer to publish again. Those40 cases were rerun in an isolated sibling source. No changed-mode diagnostic timing is reused in this final table.

- Native/hardware source: `tests/regression/forza_pgas_feedback` (software queue branch superseded).
- Corrected software queues: `tests/regression/forza_pgas_feedback_sw_ordered`.
- Packed control source: `tests/regression/forza_pgas_feedback_packed`.
- [Ordered raw results](forza_pgas_feedback_ordered_results.json).
- [Corrected software queue raw results](forza_pgas_feedback_sw_ordered_validated_results.json).
- [Unchanged-mode binary identity proof](forza_pgas_feedback_binary_identity.json).
- [Packed raw results](forza_pgas_feedback_packed_validated_results.json).
- [Final verification manifest](forza_pgas_feedback_final_freeze.json).

This study does not prove every native implementation is optimal or every messaging architecture loses. State is small and resident. Additional cache-aware scheduling, larger distributed memory, actor virtualization, variable message sizes and physical area/power require separate models. A measured reduction versus software owner queues or MMIO is not a native-GPU performance win.
