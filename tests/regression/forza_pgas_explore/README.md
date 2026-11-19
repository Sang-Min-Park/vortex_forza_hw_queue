# Owner-local state and two message interfaces

This is a weighted in-degree / irregular scatter experiment. Each input edge has
source `edge_index % actors`, destination `key`, and signed weight `delta`; the
answer is `state[key] += delta`. Every mode consumes the identical 8B input edge
array and is checked against an uncombined CPU sum over every destination word.
It is a bounded one-shot phase with known incoming counts, not a complete graph
runtime or a distributed termination algorithm.

A software PGAS handle is `{owner, offset}`. The global logical key maps to
`owner = key / words_per_actor` and `offset = key % words_per_actor`. An actor
updates its own region directly; an update to another owner is an active message
whose 8B payload contains `{offset32,delta32}`. This does **not** add an ordinary
global load/store address that can access another core's local memory. Remote
get/reply operations require an explicit protocol; the separate MMIO functional
test exercises that protocol.

For local placement, the state uses the real `__local_mem()` LSU/scratchpad path.
Within a core, word `offset` for local actor `pe` resides at:

```
__local_mem() + 8 * (offset * actors_per_core + pe)
```

The interleaving distributes adjacent actors across banks. Every owner has one
software writer; its local state update needs ordinary local loads/stores.
Scratchpad initialization uses acknowledged swaps and a barrier. Results are
copied to a global snapshot only after processing completes so the host can
check them. That snapshot is not the live local state.

With 8 actors/core and 16KiB local capacity, each actor can keep 256 int64 words
locally. Two cores therefore keep 32KiB of logical state in local SRAM. Above
that size, each actor retains its first 256 words locally and explicitly places
the remainder in the global cache/DRAM-backed allocation. Logs report resident
bytes, spill bytes, and how many input updates target each part. Spilled accesses
may hit the normal cache hierarchy; global backing is not a forced DRAM miss.
There is no claim that arbitrarily large state fits scratchpad.
Every mode still allocates a full global state-sized snapshot/backing buffer for
host verification. For resident message state its prefix is an output snapshot;
for `gpu_local` that prefix also holds remote partials during processing. The
`MEMORY` line reports this allocation separately from live spill bytes, rather
than claiming that local placement removes every global allocation.

| Mode | State and work placement |
| --- | --- |
| `hw_owner`, `TRANSPORT=csr`, `STATE=global/local` | Ordered CSR transport; single owner applies updates in the selected state placement. |
| `hw_owner`, `TRANSPORT=mmio`, `STATE=global/local` | Same owner algorithm and loop, with root-owned memory-interface transport. |
| `sw_owner` | Same local owner handler, global SPSC queues with four 64B slots per source/destination pair by default. |
| `gpu_local` | Same local SRAM layout: same-core resident updates use local AMOs; other updates use global AMOs. After a barrier, owners pull global resident partials into local state. |
| `gpu_global` | Direct global AMO per input, retained as a strong native control. |

The four hardware cases form a 2x2 interface/placement ablation. The best native
control is the faster of `gpu_local` and `gpu_global`; using a particular memory
placement is not assumed to improve performance. `gpu_local` can update other
actors' locations in **its own core's** local memory with atomics, and never
addresses another core's local memory.

The hardware mailbox SRAM is separate from the 16KiB local-state allocation.
The existing prototype instantiates 8 mailboxes with two 64B slots per actor;
logs report that resource separately. A short request does not change the 64B
physical message format.

Software queues have one producer and one consumer per channel. A 64B-aligned
slot holds its sequence and payload in the same 64B sector. Sequence polling uses
acknowledged AMO reads, also invalidating a stale private line when L1 is enabled.
The producer writes payload, then publishes the sequence via an acknowledged
same-sector AMO. The consumer reads payload and uses an LSU fence before the
sequence-release AMO. Both operations remain in the LSU ordering domain. This is
different from the previously corrected LSU-read/SFU-CSR release race. Per-thread
producer/consumer cursor arrays use ordinary private stack storage, not additional
local-state SRAM. Their logical size and global ring allocation are reported.
The host also validates every final queue sequence against the expected number
of messages per source/destination channel.
Slots use a bijective source/destination permutation and slot-major layout to
spread active lane polling over L2 banks instead of striding every channel onto
the same bank. The host checks that mapping has no aliases. Logical channels and
total ring bytes are unchanged.

Global backing/output and software queues are initialized on the host before the
launch; that setup and transfers are excluded. Local state must be initialized
inside the kernel, and its initialization is reported separately. Primary device
cycles are queried after full state/results/queue readback and cache flush.
Logical initialization, processing and output spans are secondary. LMEM, L2 and
DRAM event counters cover the whole kernel and flush, including input, stack,
transport and snapshot traffic. No physical GPU timing or synthesized SRAM area
is inferred from these uncalibrated SimX measurements.

Build modes in an already configured matching build tree:

```
make MODE=hw_owner TRANSPORT=csr STATE=local
make MODE=hw_owner TRANSPORT=mmio STATE=local
make MODE=hw_owner TRANSPORT=mmio STATE=global
make MODE=sw_owner
make MODE=gpu_local
make MODE=gpu_global
```

Use `-b32768` for the full local-state case and `-b131072` for an explicit spill
case. `-p random|skew|reuse|permute` selects the edge pattern, `-n` the edge count,
and `-q` the software ring capacity. Input hash, exact state checksum, per-worker
work/transport/AMO counts, and zero outstanding messages are checked.
