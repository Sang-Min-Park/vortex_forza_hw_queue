# Destination accelerator attribution: isolated source optimization

This follow-up preserves the first attribution cohort and all shared libraries. Only GPU source code changes. Both response modes retain window2, the same input, record semantics, exact returned tuple replay, owner state, engine resources and fabric.

The shared kernel caches immutable launch shape, uses one response endpoint base pointer with constant offsets, and removes the pending request address/operand cache. On BUSY it reloads the same immutable descriptor next iteration; it still advances only on ACCEPTED and services completions every iteration. End-of-source completion checks independently counted received==sent after the input cursor reaches the end, plus zero delivery credits. The host still verifies both counts against exact expected source totals.

Mailbox1 uses PEEK/control/ID/three tuple fields/RELEASE. Direct uses physical-source completion ID/three tuple fields/RELEASE, without logical actor registration. Both read all returned data before dependent RELEASE and export the same response array. Direct completion still traverses the same64B request/reply fabric and reuses the same two64B physical mailbox1 slots. This does not create a zero-latency LSU response path or remove hardware communication.

The first cohort's three native GPU controls are retained as separate frozen data. This sibling builds only the paired response modes. It is one bounded software optimization experiment, not a search over arbitrary latencies or hardware resources.
