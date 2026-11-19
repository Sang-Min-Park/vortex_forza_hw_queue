# Ordered receive regression

`kernel.cpp`, `main.cpp`, and `common.h` are byte-for-byte copies of the frozen
`large_state` Selector benchmark. Only the build target and runtime include
path differ. `MODE=msg` retains the two-entry LRU, 16-byte request, cyclic
input assignment, local-owner bypass, and fixed-count completion.

This app includes the sibling `runtime_ordered`, which makes receive-slot
release consume a token depending on every control/payload load. The original
runtime, failed binaries, graph benchmarks, simulator, and driver stay unchanged.

From the generated L1-off experimental build, use the same A/FORZA/L2 flags
and protect the existing shared libraries with `make -o` when building:

```bash
make MODE=msg
VORTEX_DRIVER=simx LD_LIBRARY_PATH=../../../../sw/runtime \
  ./forza_large_ordered_msg -k kernel-msg.vxbin \
  -b262144 -n65536 -ppermute -s17 -t600
```

The exact former failing input hashes to `47336e6e1d18e574`. The host verifies
every final state word and every worker's input, flush, local/remote, receive,
completion, and outstanding counts. The old failed measurement is not replaced
or retrospectively treated as correct. This regression tests a runtime ordering
correction; it does not change hardware timing or prove other configurations.
