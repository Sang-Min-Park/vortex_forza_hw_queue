# CSR endpoint lifetime experiment

This experiment uses only FORZA CSRs and mailbox SRAM loads. Each block occupies
all warps on one core. Every thread registers mailbox 0, sends a self-addressed
64-byte packet with seven checked payload words, loads it, releases the slot,
and drains delivery ACKs before finishing. There is no cross-block barrier and
no dependency on another block becoming resident.

`-w N` launches `core_count * N` blocks, requiring reuse of physical warp/lane
positions when N exceeds one. All blocks synchronize locally before exiting.

- `-m physical` registers the physical `(warp,lane)` actor number as PE. Later
  blocks retain that address and test reuse after complete message drain.
- `-m logical` registers `blockIdx.x * actors_per_core + threadIdx.x` as PE.
  Later blocks request a new logical identity on a previously used physical
  endpoint. This is a probe of registration lifetime, not an implementation of
  endpoint migration or virtualization. Registration rejection is reported as
  failure with the previous/new registration and ZEN status; it is not hidden
  as an expected-success result.

Use `-w1 -m logical` and `-w1 -m physical` as controls, then compare `-w2` or
`-w4`. `-n` sets messages per thread and `-p` bounds polls. The host enforces a
125-second outer deadline. Every process opens a fresh device, so separate
runs start with reset queue state.

Build and run from a configured RV64 build tree with the same FORZA feature,
core, warp and thread settings for the device library and the application.
For example, after configuring a build tree:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
make -C sw/kernel
make -C sw/runtime stub simx
make -C tests/regression/forza_csr_generalize
./ci/blackbox.sh --driver=simx --app=forza_csr_generalize --cores=2 --warps=2 --threads=4 --args='-w2 -m physical -n4'
```

Passing physical reuse does not establish general-grid communication: only
self-messages are tested, all old messages are drained, the PE remains tied to
its physical lane, and no messages target nonresident blocks. The existing
`forza_runtime` test provides all-actor ring, fan-in and request/reply coverage.
