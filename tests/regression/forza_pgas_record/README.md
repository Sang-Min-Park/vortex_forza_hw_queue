# Resident record fetch-update with exact replies

Independent bulk operations return a linearizable old `(count,sum,xor)` snapshot
and commit `(count+1,sum+signext(delta32),xor^request_id32)`. All fields use
unsigned 64-bit arithmetic. The primary input uses full signed32 deltas including
INT32_MIN/MAX, so the API is not silently replaced by a narrow packed integer.
Every response is replayed in per-key ticket order against the original inputs;
every final record, lock, input count, received reply and outstanding credit is
checked. Requests overlap; they do not adapt to prior replies.

Controls are cooperative global record locking, warp same-key grouped locking
with per-lane prefix replies, and fully timed histogram/prefix/scatter routing
followed by a serial-per-key owner apply kernel. Owner apply retains each key in
registers, writes the same packed24B owner SRAM, and exports it. Its O(N+K)
routing scratch is initialized and populated on the device. Route and apply are
separate launches; routing completion/cache flush, all scratch work and both
launches count in final cumulative post-readback MCYCLE. Core cycle counters survive
launches; route cycles are a secondary snapshot and are not added again. No host
sorting/bucketing is supplied to a device algorithm.

The transaction engine uses actual LocalMem accesses and serializes each key.
Requests pack id32 and delta32; mailbox1 replies contain id and all three old
fields. PEEK returns a message pointer; RELEASE consumes the XOR of control and
every copied payload word. This is a distinct optional hardware transaction
primitive, not generic software actor performance. The added LocalMem port,
contexts, queues, message SRAM and physical service width must accompany claims.
All modes use the same frozen machine, two cores/two warps/four lanes, and no
mode-specific timing changes. Each engine/owner core's packed state fits its real
16KiB LocalMem. Native lock records are 32B including the lock; engine/owner use
24B SRAM and the common 32B global export layout.

Run `python3 run_matrix.py --build-dir /absolute/build64-forza-record --stage all`.
The runner pins shared libraries, snapshots every guest/host, verifies hashes
before/after runs, and executes at most one simulation at a time. A small-delta
input switch exists for separate companion experiments; primary comparisons
remain wide32 and make no claim about a packed-CAS implementation.
