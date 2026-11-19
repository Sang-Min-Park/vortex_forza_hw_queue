# Vortex FORZA runtime backend

This is the header-only Vortex adaptation of the FORZA actor/Selector runtime.
It is adapted against `forza-project/forza-runtime` commit
`138df12fbe56d2bc248c596841ac23703217e6d8` and was previously maintained in the
local sibling checkout `forza-runtime-vortex`. It is bundled here so that the
CSR regressions can be rebuilt from this repository without that checkout.

- `inc/selector.forza.h` selects the Vortex backend.
- `inc/selector.vortex.h` implements cooperative progress and staged mailbox close.
- `inc/forza_message_lib.vortex.h` uses CSR transport and mailbox SRAM loads.
- `inc/forza_message_lib.software.h` is the software-queue experimental transport.
- `vortex.mk` supplies include paths, flags and header dependencies, including
  the compiler divergence-conversion budget required by the inlined runtime.

The three backend headers and `vortex.mk` retain their source contents from the
tested sibling checkout. The small selector entry header intentionally selects
only the bundled backend. The original native FORZA runtime, assembly tests,
RTL and RZA implementation are separate projects and are not bundled here.

The historical adapter diff is preserved in
[`docs/forza_runtime_adapter.patch`](../../docs/forza_runtime_adapter.patch).
See the [handoff](../../docs/forza_handoff.md) for the scope and known limits.
