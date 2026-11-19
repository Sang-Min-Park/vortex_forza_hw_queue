# Isolated FORZA receive ordering correction

This sibling preserves the frozen `runtime` directory and corrects its slot
release ordering. In the current SimX pipeline, a memory fence waits in the
LSU but does not prevent an independent younger CSR instruction from issuing
through the SFU. A release using `x0` can therefore recycle a receive slot
before all of that slot's payload loads return.

`forza_vortex_release(mailbox, loaded_token)` now consumes the XOR of the raw
control word and every payload word actually read. Its CSR source register
therefore depends on every such load. The device ignores the release write's
numeric value; even a token whose runtime value is zero retains the register
dependency. The existing read fence remains. There is no one-argument release
API: raw consumers must explicitly provide a token covering their loads.
Invalid opcodes release with the control word alone and do not read payload.
No hardware timing, CSR numbering, queue layout, or model source changes.

This directory copies the Vortex adapter from `forza-runtime-vortex` without
changing that frozen runtime or any earlier benchmark. It retains the same
Selector API, CSR addresses, hardware queue model, routing, ACK protocol,
physical 64-byte message frame, and 64-byte receive slots.

Include this directory's `vortex.mk` from a benchmark. With no option, the
Selector keeps its full-width baseline behavior: seven payload CSR writes
for DATA and DONE, and the existing checked receive path. Build with Make
`FORZA_VORTEX_SHORT_MESSAGES=1` (or define that macro in device CFLAGS) to
enable both optimizations:

- DATA transmission writes only `ceil(sizeof(T)/8)` payload words. The final
  partial word is explicitly zero padded without reading beyond the object.
  DONE transmits exactly two words, sender PE and epoch.
- Selector progress checks mailbox availability once, then calls a READY-only
  receive helper. It reads control plus the words needed by `T` for DATA, or
  two payload words for DONE. It copies those words before the existing load
  fence and slot release, then invokes the handler on its local copy.

Unused words in the local Selector payload remain zero initialized. The
hardware already resets/zero pads the assembler on accepted or BUSY commits;
every retry rewrites every used payload word. Overflow and sticky status
errors remain errors. This optimization reduces CSR instructions and receive
loads, not physical network-frame bytes or mailbox-slot allocation. It does
not introduce zero-copy handlers or variable-size physical frames. The receive
ordering correction above applies to both full and short builds.

The backend adds `forza_send_sized(mailbox, pointer, bytes, precinct, zone, pe)`
and `forza_message_receive_ready(mailbox, payload, data_bytes)`. The latter
requires the caller to have observed a successful availability check and to
be the mailbox's sole consumer. Its output buffer must hold at least
`max(round_up(data_bytes,8),16)` bytes. DATA size must be 1..56; DONE reads
two words regardless of DATA size. Unused output words are left untouched,
so Selector provides a zero-initialized 56-byte payload. These explicit new
backend APIs always honor their sizes; the compile flag selects their use
inside Selector. Existing `forza_send` and checked `forza_message_receive`
retain their full 56-byte contract for legacy callers and host mocks.

DONE sender deduplication and epoch checks, bounded outbox behavior, staged
handler replies, and outstanding-request completion are unchanged. A READY
helper never substitutes for an availability/error check and does not support
multiple simultaneous consumers of one HART mailbox.

Host contracts exercise the actual backend using mocked CSR functions and
commit/read-fence hooks. Tests cover byte sizes 1, 8, 16, 40, and 56; exact
CSR write counts; partial-word and unused-word padding; BUSY reset/retry;
overflow/error reporting; full legacy receives; single versus double status
polling; copied-before-release lifetime by poisoning the released slot;
two-word DONE, duplicate DONE, epoch rejection, bounded outboxes, and replies.
Both full and short tests run with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
make -f /absolute/path/to/runtime_ordered/test/Makefile BUILD_DIR=/tmp/forza-runtime-ordered-contract run
```

`test/simx` additionally runs the actual CSR/LSU path with sixteen actors
across two cores: five typed sizes, staged 40-byte replies, repeated Selector
epochs/DONE, slot reuse, and a raw one-byte send checked through the legacy
full-width receive API to verify physical zero padding. From a newly
configured experimental build, use the same driver CONFIGS and:

```bash
make -C tests/regression/forza_explore/runtime_ordered/test/simx SHORT=0
make -C tests/regression/forza_explore/runtime_ordered/test/simx SHORT=1
```

Benchmarks should report accepted payload words as a derived value and label
it so. A BUSY commit can write words that are later retried, while an early
busy hint writes none; accepted-message count alone is not an actual CSR-write
counter. The contract tests measure actual mocked CSR writes, including retries.
