# FORZA runtime integration regression

This regression compiles the Vortex backend of the actual FORZA runtime fork
at `FORZA_RUNTIME_HOME`, includes `selector.forza.h`, and executes
`hclib::launch`, `hclib::finish`, `Selector`, callback handlers, and the
runtime's send/progress/DONE methods on the SimX hardware-queue extension.
The default runtime path is the sibling checkout `forza-runtime-vortex`.
Its `vortex.mk` supplies the required compiler flags and header dependencies.
In particular, the inlined actor entry exceeds LLVM's default 100-basic-block
SIMT conversion guard. The build raises this guard to 512; skipping that
conversion produces unmasked per-lane branches and corrupts busy/retry logic.

The tested launch has two cores, two warps per core, four lanes per warp, one
cluster, and one persistent full-core CTA per core. All 16 lanes are actors.
The device must use RV64, full-width SFU dispatch, and VM disabled.
There is one live Selector per actor; the three epochs reuse mailbox IDs only
after the previous Selector completes. No service warp is reserved: all lanes
remain actors and cooperate in progress.

Three successive epochs use 32-byte trivially copyable application packets
and two mailboxes:

1. Every actor sends to the next global PE. Callbacks verify destination,
   source, per-mailbox sender order, and payload guards. This covers local and
   cross-core routing through the runtime's global-PE translation.
2. Every actor sends to global PE zero. The warp containing PE zero keeps all
   lanes in a collective progress loop until the consumer handles every
   packet. Busy sends retry without blocking the consumer lane.
3. Every actor queues requests on mailbox zero. Its request callback queues
   replies on mailbox one. Each actor allows one application request at a
   time, bounding reply demand. The application calls `Selector::done()` as
   soon as its final request is queued, before its final reply arrives.
   Completion must keep dispatching callbacks and accept their later-mailbox
   replies while closing earlier mailboxes.

The host checks callback counts and checksums independently. Every mailbox
must receive DONE from all peers and end with zero hardware outstanding
messages. The final epoch also requires that at least one actor entered
completion while a reply was still pending. `-n R` produces `96 * R` data
handler invocations; each run additionally exchanges 1,536 DONE messages.
`finish` retains the upstream adapter's callback-scope behavior; this test
does not establish full HClib future or dynamic task semantics.

All lanes in a warp call progress until `vx_vote_all` permits a uniform exit.
The first two epochs use an application data-complete barrier before closing;
the request/reply epoch deliberately has no such barrier before `done()`.
The runtime's staged mailbox closure is suitable for the tested
mailbox-zero-request to mailbox-one-reply graph. This regression does not
prove termination for arbitrary cycles of handlers producing new messages.
Handlers enqueue replies and return. They must not block inside `done()`,
`receive(true)`, or recursive progress while the current callback is running.

From the generated build tree, after configuring it and building the matching
SimX driver:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
env -u DEBUG make -C tests/regression/forza_runtime
env -u DEBUG make -C tests/regression/forza_runtime run-simx OPTS='-n32 -p4000'
```

`FORZA_RUNTIME_HOME` may override the external runtime checkout. Payload size,
handler calls, software DONE, and transport ACKs are checked separately.
The host has a 125-second hard deadline, and `-p` bounds application progress
loops. The original blocking `done()` API is bounded by the host deadline.
