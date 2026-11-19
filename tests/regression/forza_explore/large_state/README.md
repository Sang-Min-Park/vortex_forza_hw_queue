# Large owner state: direct GPU versus short messages

This isolated experiment performs irregular signed updates `state[key] += delta`.
It is a random graph-scatter-style update workload, not a complete graph algorithm.
The state is one globally addressable array, divided into contiguous owner ranges.
The same physical index and input request array are used by every mode. There is
no dense per-core partial table or assumption that state fits scratchpad.

Modes are `msg`, `msg_plain`, `msg_full`, `gpu_cache`, and `gpu`. `msg` and
`gpu_cache` use the same two-entry per-worker LRU sum combiner, retaining a victim
until its flush succeeds. `msg_plain` emits one short message/local operation per
input, `msg_full` does the same with the full-word runtime path, and `gpu` emits
one direct global `amoadd.d` per input. Compare the best message mode against the
faster native control rather than assuming the combiner helps.
Inputs are striped cyclically across the same physical lane workers. The message
implementation directly applies an owner's own flushes; remote flushes use the
isolated FORZA runtime with 16B `{key64, delta64}` payloads. Receivers alone update
their owner ranges with ordinary loads/stores. All workers continue receiving
while their warp sends and waits for exact known incoming counts and ACK zero.
This is one-shot application-specific completion, not generic actor termination.

Short mode performs two payload CSR writes for an accepted 16B message, while
the physical packet and receive slot remain 64B. `accepted_payload_words_derived`
is `remote_sent*2`; it excludes payload writes from failed commit attempts and is
explicitly not an instrumented count of every CSR write. Attempts and retries are
measured separately. The isolated runtime's contract tests count the actual word
operations, including busy cases. Full mode writes seven words. The short/full
comparison bundles reduced CSR payload assembly with the short receive path's
single poll and smaller copy; it does not isolate a single source of savings.
The simulator and original runtime are unchanged.

`COMPACT=1` adds separate `_compact` host/kernel artifacts with an 8B request
(`uint32 key`, `int32 delta`). The CLI limits state to 128MiB, hence key < 2^24,
and input count to 2^20 with each |delta| <= 7. Even a combined flush has absolute
value <= 7*2^20 and fits int32. Casts occur explicitly after these bounds; state
and CPU sums remain int64. Compact message and native controls use the same 8B
input representation and numeric input hash. Short injection needs one payload
word instead of two; physical packets remain 64B. Default 16B artifacts persist.

Two additional compact controls use the short backend directly: `msg_raw` sends
one request per packet, while `msg_batch` sends up to six requests plus one 8B
count word in a packet. Both retain owner-local bypass and the same known-count
completion. Batch buffers are private to each producer and destination, occupying
7,168B of local memory per core with the fixed 8-producer/16-owner configuration.
Their initialization is included in whole-device cycles. Sent/received/flush
counters count requests; attempts/retries count packets. Derived accepted payload
words are `sent` for raw and `sent + attempts - retries` for batched messages.

`-b` selects state bytes (suggested 262144, 2097152, 8388608, 33554432), and `-n`
selects total input updates. Patterns are uniform `random`, `skew` (90% within a
small hot subset), per-worker two-key `reuse`, and `permute` (a seeded shuffled
permutation of every cache line, repeated as needed). For example:

```
./forza_large_msg -b2097152 -n65536 -ppermute -t600
./forza_large_gpu_cache -b2097152 -n65536 -ppermute -t600
./forza_large_gpu -b2097152 -n65536 -ppermute -t600
```

The 2MiB permutation case touches all 32,768 distinct 64B lines twice, a genuinely
reused set larger than the configured 1MiB L2. Merely allocating a large array is
not evidence of capacity pressure: logs report distinct touched lines, their byte
footprint, and completed full-line passes. Small update-count runs are useful
functional/latency checks but must not be described as exhausting L2 capacity.
Graph state is reset and requests/permutations/known receive counts are prepared
on the host before the timed launch; their cost and uploads are excluded.

Every state word is checked against an independent uncombined CPU reference,
including untouched zeros; a checksum is printed. Per-worker input and combiner
flush counts, local/remote deliveries, errors and zero outstanding are checked.
Input hash and signed deltas make the cross-mode comparison reproducible.

The primary cycle metric is read after all result/state downloads and cache
flush. Phase timestamps are logical completion only; owner plain stores can be
posted. Cache and DRAM counters cover the whole kernel and flush, including input,
runtime and result accesses; they are not isolated state-only misses. Host memory
copies themselves are not modeled PCIe/DMA time. L2 and processor-wide counters
are read once; L1 socket counters are summed over cores in the fixed socket-size-1
setup. L1 ON is valid here because mutable state has one software owner in message
mode and native kernels access shared mutable state only via global atomics.
There are no global-memory software mailbox polling flags in this workload.

Build only the new app against an existing matching driver:

```
make MODE=msg
make MODE=msg_plain
make MODE=msg_full
make MODE=gpu_cache
make MODE=gpu
```

Use the same driver/configuration in each comparison, and test both L1 OFF/shared
L2 and L1 ON/shared L2 separately. These results describe the current uncalibrated
SimX model, not physical GPU or synthesized queue performance.
