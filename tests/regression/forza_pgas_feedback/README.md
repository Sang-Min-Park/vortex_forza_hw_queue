# Feedback-dependent owner execution

Two ordinary GPU handlers run over the same generic transport. Arithmetic
UPDATE/RECORD engines are forbidden at compile time. The initial machine is
two cores, two four-lane warps/core, shared L2 and disabled global L1D.

Warp1 runs four independent client chains/core. A client has one outstanding
query and uses its returned next key in the following query. Warp0 has four
server lanes/core; key owner is `{key % cores, (key / cores) % 4}`. Each server
lane exclusively owns its stripe, so it does not need record locks. Native GPU
clients run persistently with the same eight client chains. Walk uses normal
global loads; mutable state uses cooperative one-attempt record-lock rounds,
with no divergent spin loop that can hold a warp's lock owner inactive.

- **walk:** follow a configurable number of dependent probes within the target
  owner's shard, computing a value and the next global query key.
- **update:** read link/value/version, compute a conditional value update, then
  commit value and incremented version. The reply's next key depends on old
  state, client and step. Work iterations add ordinary GPU computation.

Queries and replies are self-contained 64-bit tokens in 64-byte physical
messages. Replies carry a valid bit, next key, old version and old value (walk
returns its computed value). No global response store is published by a message.
Every mutable server store is an acknowledged AMOSWAP before a reply can issue.
The reply fields and next key use the old values actually returned by both
state AMOSWAPs, creating data dependencies on both committed-store responses.
A16-bit cookie derived from the preceding reply also changes the next query's
computation. Therefore even the single-hot-key workload cannot precompute its
request payloads before replies. Trace key words contain key and this cookie;
the CPU checks both against the previous reply.

Transport modes are MMIO, actual SEND/RECV try, and actual SEND/RECV wait.
Wait servers use recv_wait_any so idle endpoint lanes do not block active ones.
Each mode has global and owner LocalMem state variants. Global rows are32B
including the native lock; local rows are24B and use actual shared16KiB/core
LocalMem. Local initialization/export are timed. A local-state speedup is a
placement-plus-execution result; the global-state messaging controls isolate
it from a claim that messaging alone creates locality.

No host per-owner incoming counts are supplied. After consuming its last reply,
each client sends one DONE to core0/PE0. That coordinator receives all clients'
DONEs, then sends one STOP to each server endpoint, itself last. This is
O(clients+servers), not all-to-all DONE. During STOP publication it uses a
nonblocking receive so it cannot wait for its own not-yet-sent STOP. All endpoints
drain their actual delivery credits before the final global barrier.

The CPU validates every `(key,reply)` trace. Walk traces are deterministic and
must match across modes for equal input. Update interleavings may change later
query keys and final state: the CPU builds client-order and per-key-version
edges, checks an acyclic history, replays every operation, and checks every
reply and final record. Same initial state/program/client concurrency/logical
operation count is compared; no identical adaptive trace or final checksum is
claimed for update modes. Cache locality can differ with those legal schedules.

Native and messaging use the same8 client chains; server contexts are idle in
the native mode because direct client operations do not require a GPU service
thread. This does not establish the best possible native implementation.
Every mode records actual handled counts, sent/received messages, retries,
LocalMem accesses, L2 accesses and final post-readback device cycles.

`native_replicated_walk` is a stronger immutable-state control: it copies the
entire next/value table into each core's existing LocalMem, then directly runs
the client chains from that replica. Initialization is timed and capacity is
checked (16B/key/core); it adds no SRAM. Mutable update is intentionally not
run in this mode because unsynchronized replicas would change its semantics.

`gpu_queue_global/local` use only ordinary global-memory GPU operations for
transport. Each client/server pair has one SPSC request slot, each client one
reply slot, and each server one coordinator STOP slot. A per-server ready mask
avoids scanning every empty request slot: acknowledged payload AMOSWAP precedes
AMOOR notification; consumption clears the payload and then AMOANDs its bit.
One outstanding request/client and ACKed queue operations make reply-slot reuse
safe. All slots and masks are checked empty after completion. At8clients these
queues use704B of global memory. No hardware mailbox registration, credit CSR,
or new messaging instruction is used. The owner handler and termination protocol
are identical to the hardware-queue variants.

Configure a fresh out-of-tree build, then build matching ISA-enabled generic-only
libraries. `run_matrix.py --build-dir <build> --stage build` snapshots all sources,
four libraries, host binaries and guests. `--stage smoke` runs zero/one/partial
chains; `--stage run --suite quick` adds the first primary cases. `--suite full`
adds bounded work/probe and chain-depth sensitivity. All runs have deadlines,
exact CPU validation and before/after artifact hash checks.
