# Corrected compact queue experiment

This is the final isolated scatter experiment. It uses the typed 8B request and
one-send/one-receive schedule from `large_state_balanced`, with a required slot
lifetime correction: the DQ-release CSR's source register depends on control
and every payload word read. The LSU read fence remains. This prevents a release
on the separate SFU path from overtaking pending payload loads. The model and
all prior benchmark artifacts are unchanged.

Owner state is ordinary globally addressed memory; mailbox slots are dedicated
SRAM in the SimX model. Packets and slots remain 64B. Dequeue-pointer polling
relies on this prototype's empty-return-zero contract. Original FORZA software
should keep ZQMSTAT polling unless its hardware promises that extra behavior.

Build from a freshly configured RV64 build with the matching two-core, two-warp,
four-lane, FORZA/A/L2 driver configuration. Use separate OFF/ON builds:

```
make -C tests/regression/forza_explore/large_state_balanced_ordered
cd tests/regression/forza_explore/large_state_balanced_ordered
VORTEX_DRIVER=simx ./forza_large_msg_raw_balanced_ordered_compact -b2097152 -n65536 -ppermute -t600
```

Set `LD_LIBRARY_PATH` to that build's `sw/runtime`. Preserve `CONFIGS` from the
driver when building the app. Every state word, worker count and ACK-zero check
must pass before a cycle result is accepted. The final comparison covers 256KiB
and 2MiB sector working sets with L1 OFF/ON and the same numeric native GPU inputs.

- [Final experiment report](../../../../docs/proposals/forza_explore_optimization.md)
- [Validated measurements](../../../../docs/proposals/forza_explore_balanced_ordered_measurements.csv)
- [Source, binary and command provenance](../../../../docs/proposals/forza_explore_balanced_ordered_manifest.json)
- [Receive-ordering diagnosis](../../../../docs/proposals/forza_explore_ordering_bug.md)
