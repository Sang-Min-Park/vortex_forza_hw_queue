# Programmable region transactions

This experiment replaces a fixed arithmetic engine with an opt-in eight-register
microprogram executor. Software uploads one of two eleven-instruction programs:
count/sum/XOR or count/signed-min/signed-max with conditional stores. Hardware
implements only LD, acknowledged ST, scalar integer operations, branches and
return. Both programs return all three old fields and increment a per-key ticket.

The same four-context engine, actual LocalMem port/banks, 64-byte reserved region,
program fetch/execute scheduling and bounded request queue serve mailbox and
physical-source completion calls. No per-program timing or math shortcut exists.
Every memory access must remain within its invocation's 64-byte region. This is
a small programmable transaction engine, not a full RISC-V core or arbitrary
cross-region RZA implementation. The default is disabled; fixed UPDATE/RECORD
engines are forbidden in this build.

Native controls include cooperative global locks, same-key warp grouping and
fully timed GPU histogram/prefix/scatter ownership. Both associative programs
receive legal prefix/grouping optimizations. Native owner state has the same
64-byte stride in the same 16KiB/core LocalMem; no new data capacity is added.
Program upload, SRAM initialization, response consumption, completion and export
are included in final cumulative device cycles. Full signed32 inputs, every old
tuple prefix and every final record are checked. This finite bulk experiment
does not claim dynamic actor termination or feedback-chain performance.
