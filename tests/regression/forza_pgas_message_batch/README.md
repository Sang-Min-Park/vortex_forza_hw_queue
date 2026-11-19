# Generic message range batching

This isolated SimX experiment keeps the arithmetic UPDATE and RECORD engines
disabled. It reuses the generic eight-byte MMIO transport, fixed 64-byte physical
frames, and the validated GPU FieldHandler. No new hardware operation is added.

Each core has two four-lane warps. Warp zero services that core's owner shard;
warp one produces requests. A producer owns immutable contiguous input ranges
of 1, 4, or 8 records, advancing by eight producers times the range length.
It scans every range's keys, constructs a destination-core bitmap, and sends
one token to each represented destination. The token is
`(uint64_t(count) << 32) | (start_index + 1)`. Zero remains the no-message value.
No mutable descriptor or host-built destination buckets are used.

The receiver scans the original immutable range, filters records by destination
core, and calls the ordinary compiled GPU handler once for each owned request.
It preserves every returned old `(count, sum, xor)` tuple; there is no reduction
or elimination of logical operations. A range sent to both cores is scanned
at both receivers. Producer and receiver scan counts are checked and reported.

All grouping, filtering, final partial ranges, initialization, SRAM flush,
completion barriers and output export are timed. Source assignment changes
with range length and is disclosed rather than described as an identical
instruction-stream intervention. Each owner has a single service warp, so
its register-cached record state has no concurrent writer.

Completion uses exact per-core request and message counts derived from input,
as in the earlier finite bulk experiment. These counts do not provide a
generic dynamic actor finish detector. Every producer drains its delivery
credits and every service warp consumes its expected ranges before a common
global barrier and export. Results are visible at phase completion; no
per-request reply-notification publication is claimed.

Controls include native global locks, grouped locks, timed GPU bucket/prefix
ownership, and the same FieldHandler reached through GPU-generated buckets.
All modes use identical input and exact per-key response-prefix validation.
The existing 16KiB/core LocalMem contains packed 24-byte owner records.
Request descriptors and response output arrays use ordinary global memory.

Run `run_matrix.py --build-dir <fresh-build> --stage build` only after configuring
and building matching generic-only libraries. The runner freezes source,
library and guest/host hashes, builds each mode into its own snapshot, and
then runs zero/one/request-tail smokes and 64-record random/hotset/hot cases.
This tests software batching over generic hardware queues. A gain would not
establish a new arithmetic accelerator or a messaging-exclusive advantage.
