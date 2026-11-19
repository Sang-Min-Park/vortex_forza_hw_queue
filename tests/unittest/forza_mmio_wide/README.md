# Generic wide mailbox contract

This unit requires the generic FORZA MMIO transport and deliberately rejects builds that enable UPDATE or RECORD. It exercises opaque message contents; no arithmetic operation or destination execution engine participates.

The generic receive endpoints are `FORZA_MMIO_RX_PEEK_BASE` and `FORZA_MMIO_RX_RELEASE_BASE`. `forza_mmio_peek_address(mailbox)` selects the current ready slot without consuming it and returns its control-word address, or zero if empty. `forza_mmio_release_address(mailbox)` accepts an AMOSWAP.D to release that slot. Both must be qualified with `forza_mmio_actor_address(physical_actor, endpoint)` so hardware identity and compiler divergence agree.

Software loads control and each required payload word using ordinary loads from the existing mailbox SRAM aperture. The RELEASE operand must retain a real register dependency on every copied word; its numeric value is ignored by hardware. The test XORs copied words into the operand. Slot capacity, delivery scheduling, network bandwidth, and message math remain unchanged.

The contract covers all eight mailboxes, two registered actors, opaque 32-byte and 56-byte payload copies, zero and high-bit values, two-slot advance/reuse, physical actor address checking, invalid mailbox helpers, and empty or incorrect command types. The original RECORD names remain aliases at their unchanged addresses for source/ABI compatibility.

Run from a freshly configured out-of-tree build with matching FORZA, A, MMIO, core, warp, and lane flags. The optional existing vector-MMIO flag selects the already modeled command resource; this interface change adds no port or bandwidth.
