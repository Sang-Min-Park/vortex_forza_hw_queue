# Mailbox release ordering: reproduction, evidence, and correction

A long 16B Selector test exposed a receive-lifetime error: a mailbox slot could
be returned before its payload loads completed. A source-level `fence r,rw`
followed by `csrw zqmdq0,x0` did not establish the required dependency in this
SimX prototype. Received values, rather than the owner's arithmetic alone,
explain the observed incorrect state.

The correction consumes every copied word in the register operand of the release
CSR. It preserves the mailbox format, physical 64B slots, and simulator timing
parameters. The formerly failing 16B stress case now passes every state and worker
check with the register-dependent release.

## Required lifetime and the missing ordering edge

The receive buffer belongs to the receiver until all its payload loads have
captured their values. Releasing it permits ZQM to overwrite it with another
message. The original sequence was effectively:

```asm
ld    key,   8(pointer)
ld    delta,16(pointer)
fence r,rw
csrw  zqmdq0,zero
```

In the inspected model, FENCE enters the LSU without a warp fetch stall
([decode.cpp](../../sim/simx/decode.cpp#L635)); fetch suspension is selected from
`instr->is_wstall()` ([core.cpp](../../sim/simx/core.cpp#L497)). The fence waits for
the LSU's pending table and gates subsequent LSU dispatch
([lsu_unit.cpp](../../sim/simx/lsu_unit.cpp#L331),
[FenceController](../../sim/simx/lsu_unit.h#L46)). FORZA CSR requests travel through
the SFU into a separate channel without that LSU drain check
([sfu_unit.cpp](../../sim/simx/sfu_unit.cpp#L279)). The SFU's explicit LSU drain
check applies to BAR, not the release CSR
([sfu_unit.cpp](../../sim/simx/sfu_unit.cpp#L423)).

Consequently, the release's constant-zero source can be ready while the earlier
payload loads are still pending. The software fence does not connect the two
execution paths in this prototype.

```mermaid
sequenceDiagram
    participant CPU as Receiver
    participant LSU
    participant ZQM
    participant SRAM as Two mailbox slots
    CPU->>LSU: Issue payload loads
    CPU->>LSU: Issue fence
    CPU->>ZQM: Release CSR with constant zero
    Note over LSU,ZQM: Separate paths; release has no loaded-data dependency
    ZQM->>SRAM: Return and reuse the slot
    LSU->>SRAM: Delayed load reaches reused slot
    SRAM-->>CPU: New or mixed payload values
```

## Reproduction and independent observation

Configuration: RV64, 2 cores/zones, 2 warps/core, 4 lanes/warp, 16 actors, L1
disabled, shared 1MiB L2. The failing input performs 65,536 signed updates to a
256KiB owner-partitioned array. A shuffled permutation visits 4,096 distinct
64B sectors 16 times; numeric input hash is `47336e6e1d18e574`. The L2 has 128B
lines and 64B sectors. The compatibility log field `distinct_state_lines` counts
64B sectors.

The original frozen Selector/LRU binary returned incorrect values at keys
26352, 30960, and 31736 while every worker's input, flush, receive, and zero-ACK
checks passed. Its timing is excluded from performance results.

The diagnostic guest appends each received `{key,delta}` to a unique sequence
slot for its owner before applying the update. This avoids introducing a second
global hash read/modify/write into the observer. The host checks two commutative
hashes and the exact payload multiset. Added trace traffic changes timing and
cache schedules, so this guest is a diagnostic rather than a performance control.

The first diagnostic found five incorrect state words. A second host executable
saved all payloads, requests, state, and worker results while using **the exact
same diagnostic guest and driver**. It reproduced the same hashes and five state
errors. Every previously captured artifact was hash-checked before and after;
the host-only recording source and executable were new files.

An independent Python reconstruction starts with CPU-computed owner-local
contributions and folds the actual received trace into the state. It matches
**every actual state word**, but differs from the correct CPU state at five keys.
There are no invalid keys, wrong-owner entries, or zero trace entries. Exactly
three incoming event substitutions account for those five errors:

| Owner | Missing event | Excess event |
| --- | --- | --- |
| 11 | `(22920,+1)` | `(22856,+1)` |
| 15 | `(30776,-1)` | `(31424,-5)` |
| 15 | `(30936,-6)` | `(30936,+2)` |

The chronological trace has the expected two-slot reuse signature:

| Owner | Earlier received index | Value | Index two entries later | Value |
| --- | ---: | --- | ---: | --- |
| 11 | 119 | `(22856,+1)` | 121 | `(22856,+1)` |
| 15 | 2914 | `(31424,-5)` | 2916 | `(31424,-5)` |
| 15 | 3152 | `(30936,+2)`; expected delta `-6` | 3154 | `(30968,+2)` |

The first two pairs duplicate an entire same-parity slot value. In the third,
the earlier message keeps its key but contains the later message's delta.
Together with the separate execution paths, this supports premature slot reuse
as the cause. Sorted-position mismatch counts of 78 and 738 in the earlier log
are **not** counts of corrupted messages; the exact multiset has three substitutions.

Assembly inspection of the original failing binary shows TX and RX payload words
remain in registers through the callback. The particular theory of a payload
stack copy being reloaded incorrectly is therefore ruled out for that binary.
Native owner-only plain and acknowledged RMW diagnostics both passed. Their host
bucketing changes scheduling, so they do not establish universal memory-order
correctness; the trace provides the more specific evidence here.

## Dependency correction and ABI compatibility

The ordered runtime computes a token from the actual loaded values:

```cpp
uint64_t loaded_token = control;
for (each payload word being copied) {
    uint64_t value = mailbox_words[word];
    copied_words[word] = value;
    loaded_token ^= value;
}
// Existing fence remains; the CSR now also consumes the loaded-data dependency.
csr_write(zqmdq, loaded_token);
```

The source register cannot be ready until every included load has completed.
The token's numerical value is irrelevant, including when it happens to be zero.
This is a dependency operand, not a message checksum or new on-wire field.
Non-immediate CSR instructions declare `rs1` as a source
([decode.cpp](../../sim/simx/decode.cpp#L692)); the scoreboard checks every source
register before issue ([scoreboard.cpp](../../sim/simx/scoreboard.cpp#L40)). The
ordered 16B dump contains an XOR chain from control, key, and delta into the
release's register operand; the old constant-zero release compiles to an
immediate CSR write. Thus the corrected dependency reaches the actual instruction
stream and the simulator's issue mechanism.
Both checked-full and already-ready short receive APIs include every word they
actually load. Invalid-opcode handling depends on the loaded control word.

The prototype's DQ write handler ignores the write value and returns the current
slot ([forza_zone.cpp](../../sim/simx/forza/forza_zone.cpp#L184)). The inspected
original FORZA RTL agrees: `zqm_ring_reg_top.sv:137` sets
`zqm_free_msg = w_csr_write` for DQ0 and likewise for DQ1 through DQ7, without
testing `w_csr.data`; `zqm_mbox_top.sv:480` consumes that boolean. The zero/nonzero
data test in `zqm_ring_reg_top.sv:129` belongs to mailbox registration and
unregistration, a different CSR. Thus this operand preserves the inspected DQ
release semantics. No simulator or RTL timing changes were made.

Ordered code is isolated in
[runtime_ordered](../../tests/regression/forza_explore/runtime_ordered/inc/forza_message_lib.vortex.h)
and
[large_state_balanced_ordered](../../tests/regression/forza_explore/large_state_balanced_ordered/kernel.cpp).
Old sources and binaries remain available for reproduction.

The runtime-team contract is:

- Selector send/progress/handler APIs remain unchanged; the ordered backend
  constructs the dependency internally.
- Every raw receive path must keep the slot until all copied control/payload
  values have returned. Its release source must depend on all those loads,
  including DONE and error paths to the extent those paths read data.
- Use an unconditional CSR write (`CSRW`/`CSRRW`) with the computed register
  operand. A conditional CSR set/clear operation is unsuitable when the token's
  runtime value is zero.
- DQ write data is an ignored value in the inspected interfaces. Registration's
  zero-value unregister rule applies to a different CSR and must remain separate.
- A fence instruction without this cross-path dependency is insufficient for
  this prototype. This correction does not redefine general memory/I/O fencing.

## Validation status and timing provenance

The corrected direct compact path has passed the 2MiB, 65,536-update permutation
case with every CPU state word, per-worker count, and zero-outstanding check:
13,028,563 whole-device cycles. Its source, header, guest, and driver hashes were
unchanged before and after the run. The disassembly uses the loaded
`control ^ payload` register as the release source. See the
[first ordered run manifest](../../build64-forza-explore/tests/regression/forza_explore/large_state_balanced_ordered/ordered-first-runs.json).
The formerly failing **16B Selector** workload also passes the exact original
65,536-update/256KiB input, numeric hash `47336e6e1d18e574`, with all state words,
worker counts and zero outstanding correct; final state checksum is
`c680e15126d4dc1a`. The application's kernel, host, and common ABI source files
are byte-identical to the failed application; the runtime include selects the
ordered backend. The simulator and driver were unchanged.
Its 78,024,427 whole-device cycles belong to the generic Selector path, not the
compact fast performance path. See
[ordered stress runs](../../build64-forza-explore/tests/regression/forza_explore/large_state_ordered/ordered-runs.json)
and the
[verified source freeze](../../build64-forza-explore/tests/regression/forza_explore/large_state_ordered/ordered-source-freeze.json).

ASan/UBSan host contracts pass for FULL and SHORT receive APIs, all eight
mailboxes, sizes 1/8/16/40/56, exact loaded-token operands, invalid opcodes,
DATA/DONE, BUSY/overflow, poisoning, replies and deduplication. These synchronous
mocks validate backend contracts; the long actual SimX regression supplies the
asynchronous lifetime check. Actual FULL and SHORT CSR smoke tests also pass.
The
[ordered contract manifest](../../build64-forza-explore/tests/regression/forza_explore/large_state_ordered/ordered-contract-manifest.json)
records 18 new source hashes, 22 ordered test/build artifacts, host contracts,
both actual CSR smokes, and the exact long regression. It also verifies that 13
frozen original runtime files and its six captured OFF/ON runtime/kernel-library hashes remain
unchanged.
An [independent verification manifest](forza_explore_ordering_verification.json)
also rechecks both model DSOs, all eight original OFF/ON shared prerequisites,
the unchanged diagnostic guest/dumps, and the ordered sources and test artifacts.
The [corrected compound-state cohort](forza_explore_compound_ordered_comparison.md)
also passes all ten ordered cases; its unchanged native controls are tracked
separately in that report's provenance.

All previously passed raw, batch, fast, balanced, and compound timings that used
constant-zero release are **pre-correction exploratory results**. A pass on one
input does not establish safe slot lifetime on every schedule. The final
performance comparison must identify the ordered implementation and its own
validated artifacts. Native GPU controls do not use this release path and may be
reused when their input, configuration, and binary hashes match.

The typed fast/balanced benchmark also uses this prototype's zero-pointer result
for an empty dequeue. The inspected original RTL continuously computes its DQ
address from the mailbox base/current slot; it does not establish that empty
dequeue convention. The generic ordered runtime still checks `zqmstat` first.
Treat single-dequeue polling as a prototype-specific software/hardware agreement,
separate from the write-value compatibility of the ordering correction.

The primary cycle window remains core-0 device cycles after result/state readback
and cache flush. Logical phase timestamps are secondary. Cache counters cover
input, runtime, result, and state traffic together; they are not state-only miss
counts. The simulator is uncalibrated, and these checks do not establish RTL
implementation or physical hardware performance.

## Artifacts

| Evidence | Location |
| --- | --- |
| Original stress failure and exploratory controls | [priority manifest](forza_explore_large_priority.json) |
| Diagnostic guest, two hashes, and exact multiset checks | [diagnostic manifest](forza_explore_large_digest.json) |
| Unchanged-guest host-only repeat, command, hashes, and binary dumps | [record manifest](forza_explore_large_trace_record.json) |
| Independent Python state fold and three event substitutions | [analysis manifest](forza_explore_large_trace_analysis.json) |
| Corrected runtime contracts, both actual CSR modes, and exact stress regression | [ordered contract manifest](../../build64-forza-explore/tests/regression/forza_explore/large_state_ordered/ordered-contract-manifest.json) |
| Independent final source, model, library, dump and test-artifact hash checks | [verification manifest](forza_explore_ordering_verification.json) |
| Corrected compound-state validation and native-control reuse | [ordered compound comparison](forza_explore_compound_ordered_comparison.md) |
| Diagnostic source and reproduction runners | [large_state_digest](../../tests/regression/forza_explore/large_state_digest/README.md) |
| Frozen original performance source and build matrix | [large_state](../../tests/regression/forza_explore/large_state/README.md) |

The record manifest names the full trace, requests, actual and expected state,
worker results, and reconstructed state dumps with SHA-256 hashes. Their run
directory is `build64-forza-explore/tests/regression/forza_explore/large_state_digest/runs/host-record-n65536-permute`.
