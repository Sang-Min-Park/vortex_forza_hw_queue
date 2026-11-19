# Balanced one-word queue experiment

This is an isolated SimX benchmark, not a production FORZA runtime replacement.
It retains the same 8B update, ordinary global owner state, 64B message/slot,
dedicated mailbox aperture, network and ACK semantics as `large_state_fast`.
The only device scheduling change is one send attempt before each receive poll,
instead of a burst of up to eight. The input and all-state CPU validation are
unchanged. No simulator timing or cache code was modified.

The hot path relies on known valid inputs and this prototype's zero-on-empty
dequeue CSR. It writes the payload and nonblocking commit directly, copies the
received word before its read fence and slot release, and observes outstanding
zero after inputs and exact known receive counts are complete. Generic runtime
argument validation, fault reporting and actor DONE/future completion remain
separate interfaces; this is a bounded one-shot test.

From the configured RV64 build directory, build with the same CONFIGS used for
the driver, then run from this generated app directory:

```
make -C tests/regression/forza_explore/large_state_balanced
cd tests/regression/forza_explore/large_state_balanced
VORTEX_DRIVER=simx ./forza_large_msg_raw_balanced_compact -b2097152 -n65536 -ppermute -t600
```

The validated setup has one cluster, two cores, two warps/core, four lanes/warp,
native atomics, FORZA and a 1MiB shared L2. Use separate builds for L1 OFF and ON,
and ensure the generated runtime libraries are on `LD_LIBRARY_PATH`.

Every state word, per-worker input/flush/send/receive count, error and ACK-zero
condition must pass before accepting a timing. The two capacity cases touch
4,096 64B sectors sixteen times or 32,768 sectors twice. L2 lines are 128B;
the log's legacy `distinct_state_lines` field counts 64B sectors.

See [the full comparison](../../../../docs/proposals/forza_explore_optimization.md),
[matched results](../../../../docs/proposals/forza_explore_balanced_measurements.csv)
and [provenance](../../../../docs/proposals/forza_explore_balanced_manifest.json).
