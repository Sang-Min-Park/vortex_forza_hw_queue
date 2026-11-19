# Incoming payload diagnostic

This copies the frozen 16B Selector/LRU experiment into an isolated diagnostic.
Before applying a received update, its handler appends `{key,delta}` into a unique
per-owner sequence slot. The host checks two independent commutative hashes and
the exact sorted multiset against the original CPU-generated remote LRU flush
events. The original full-state and per-worker checks are retained, including
owner-local updates. An incoming-trace match with a state mismatch narrows the
problem to state processing or the untraced local bypass. A trace mismatch means
recorded payloads differ; it alone cannot distinguish transport, stack copies,
or trace-store problems.

This is a correctness diagnostic. Extra trace stores, readback, memory footprint
and compiled code change timing; a diagnostic pass cannot erase a failure in the
original frozen binary. The trace does not maintain a per-message hash accumulator
via another global read/modify/write. Original sources and artifacts are unchanged.

The target reproduction is `-b262144 -n65536 -ppermute -s17 -t600` with L1 disabled.
Frozen log key `distinct_state_lines` counts 64B sectors; the shared L2 has 128B
lines with 64B sectors and a total capacity of 1MiB.
