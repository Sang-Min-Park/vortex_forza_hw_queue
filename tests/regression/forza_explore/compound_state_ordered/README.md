# Compound transactions with data-dependent mailbox release

This is a separate corrected cohort of the frozen `compound_state` MP benchmark.
Its main/kernel/state/ABI sources are copied unchanged. Only the included runtime
changes to `runtime_ordered`, whose dequeue-release CSR operand depends on the
control word and every loaded payload word. A device LSU fence alone did not
order the separate CSR unit in this SimX model. The driver and timing model are
unchanged.

The same per-response ticket-prefix and all-record checks remain mandatory.
Use `forza_compound_ordered_mp -k kernel-ordered-mp.vxbin` explicitly. Reuse the
frozen native GPU lock and two-stage owner results only after verifying identical
requests, cache policy, drivers, and their host/kernel artifact hashes.

The old cohort and its passing and failed diagnostics remain preserved. Passing
old inputs do not remove the latent early-release issue. Corrected MP cycles
must be reported separately, rather than replacing the old recorded numbers.
