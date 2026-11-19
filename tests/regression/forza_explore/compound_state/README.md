# Compound owner-state transactions

This isolated experiment asks whether dedicated message queues help when an update
requires more than a single GPU atomic instruction. It leaves all earlier graph
experiments, drivers, cache models, and message hardware unchanged.

Each input is a 16-byte `(key, request_id, delta)` request. A transaction returns a
consistent old `(count, sum, xor)` tuple and then increments count, adds delta, and
xors a deterministic function of request_id. The three values cannot be updated
independently while satisfying this old-snapshot contract. Execution order between
requests for the same key is deliberately unspecified.

The host validates every returned tuple. For each key it sorts requests by the
returned old count, requires unique consecutive tickets, reconstructs the exact
sum/xor prefix in that order, and checks every final record and lock. Merely
receiving the expected number of messages is insufficient.

## Implementations

- `mp`: one persistent lane per actor; key modulo actor count chooses the owner.
  The optimized short-message interface sends exactly two payload words, polls
  availability once per receive attempt, and uses a fixed expected request count
  instead of all-to-all DONE messages. A self-owned request bypasses the queue.
  The owner caches its most recently used record, flushing on key change. Runtime
  progress interleaves one send attempt with one receive attempt and checks ACKs.
- `gpu_lock`: a 64-byte record contains the three state fields and a 32-bit lock.
  Each pending lane makes one acquisition attempt per cooperative warp round.
  Winners complete the critical section and unlock before all lanes reconverge;
  there is no divergent lock-acquisition spin loop. The record fits within one
  cache sector. Cache acquire/release ordering was audited for the configured
  write-through private L1 and shared L2; all final outputs are also CPU-checked.
  Captured old tuples are stored after releasing the lock to shorten the lock hold.
- `gpu_owner`: a strong GPU ownership control with no per-record locks. A first
  GPU kernel routes input into producer-private, per-owner global buckets and
  writes their lengths. A second GPU kernel drains those buckets and uses the
  exact same owner-state helper as MP. Both kernels, their cache flushes, and GPU
  routing are included in the primary cycle count. There is no CPU pre-bucketing
  or supplied per-channel count oracle for this implementation.
- `gpu_soa`: optional separate-array lock experiment with acknowledged field
  AMOs. It has not completed even the isolated one-request diagnostic under the
  tested deadline, so it is excluded from performance comparisons. Preserved
  failed binaries/logs and the converged-fence experiment remain available for
  diagnosis. A deadline is not evidence of a particular hardware failure cause.

All implementations write each response to the same unique global output slot;
MP is not charged a reply message that the GPU paths avoid. MP's expected incoming
counts are precomputed for one-shot completion. This contract does not implement
general dynamic actor termination. `ACTIVE_PROGRESS=1` enables an optional
completed-lane gating experiment; the official default keeps the simpler progress
loop after the extra branch regressed the initial tested cases. Saved sensitivity
binaries are separate from official artifacts.

## Reproducing the measurements

Build from a configured build tree's matching
`tests/regression/forza_explore/compound_state` directory. Keep the existing driver
and kernel library fixed when building each mode:

```sh
make MODE=mp -o /absolute/build/sw/runtime/libvortex.so \
  -o /absolute/build/sw/kernel/libvortex2.a CONFIGS='the matching driver flags'
```

Save each host, vxbin, ELF, and dump in `snapshots/MODE/` before building another
mode: the common clean target removes root-level kernel artifacts and logs.
The runner deliberately refuses to overwrite logs and hashes the driver and all
host/device inputs before and after every run.

```sh
python3 tests/regression/forza_explore/compound_state/run_comparison.py \
  --build build64-forza-explore --cache l1_off --cases capacity,hot
python3 tests/regression/forza_explore/compound_state/run_comparison.py \
  --build build64-forza-explore-l1 --cache l1_on --cases capacity,hot
python3 tests/regression/forza_explore/compound_state/analyze.py \
  --off build64-forza-explore --on build64-forza-explore-l1 \
  --output docs/proposals/forza_explore_compound_measurements
```

The matched driver flags enable FORZA, native atomics, shared L2, 2 cores, 2 warps
per core, and 4 lanes per warp. L1-off adds `VX_CFG_DCACHE_DISABLE`; L1-on removes
that flag. Both use the unchanged 1 MiB shared L2 with four banks. Source and
binary provenance must accompany any reported numbers.

## Measurement limits

The primary metric is core-0 cumulative device cycles queried after output
readback and automatic cache flush. GPU-owner cycles include both launches;
`final_stage_phase_span_cycles` describes only its apply kernel and is not a
fair total-time substitute. DRAM counters reset per launch, so routing counters
are captured at its completion and added to the final apply/readback counters.
Retired instructions sum all cores and survive launches in this model.

`state_alloc_bytes` is reserved capacity. `state_touched_sector_bytes` is the
number of distinct 64-byte state sectors addressed by the actual requests, not
traffic or an inferred cache miss count. A capacity run must demonstrate a touched
state footprint above 1 MiB; allocating a large unused array is insufficient.
Request/response and routing footprints are reported separately. Random accesses,
responses, initialization policy, queue capacities, and lock contention all affect
results. MP's owner state still lives in shared global memory: this experiment
creates no physically nearer private owner DRAM or NUMA placement.

This evaluates one concrete SimX implementation, not physical GPU performance
or a proof that messages can never outperform native execution. Lock-based and
bucket-based GPU controls are both measured; neither is claimed globally optimal.
