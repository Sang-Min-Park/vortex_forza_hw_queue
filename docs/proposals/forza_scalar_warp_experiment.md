# Physical single-thread warp experiment

The experiment tests whether independent warp PCs and waits help owner messaging
enough to offset the loss of SIMD execution. It does not replace the previously
frozen message experiments or claim equal silicon area between configurations.

Four isolated RV64 SimX machines use two cores, shared L2, disabled global L1D,
the same existing 16 KiB LocalMem/core, four LocalMem banks/core, and eight LSU
pending entries. Fixed UPDATE/RECORD and programmable VM engines are disabled.

| Machine | Warps/core | Threads/warp | Physical threads/core | Message commands/core/cycle |
|---|---:|---:|---:|---:|
| simt2w4t | 2 | 4 | 8 | 4 |
| scalar8w1t | 8 | 1 | 8 | 1 |
| simt2w4t_port1 | 2 | 4 | 8 | 1 |
| scalar2w1t | 2 | 1 | 2 | 1 |

The primary comparison retains eight physical thread contexts/core. The port-one
SIMT control separates command-service width from physical warp width. The
two-warp scalar machine also reduces physical contexts; it is a secondary
resource-reduction comparison, not a pure independent-PC intervention. ALU,
LSU and SFU lane counts follow physical thread width; instruction issue,
scoreboards, operand contexts and compiler output also differ. The bank count
is explicit because its default otherwise falls from four to one at width one.

The new standalone benchmark retains eight logical response-dependent client
chains, four per core, in every configuration. Half the physical endpoints
serve requests and half run clients. A client endpoint may multiplex several
logical chains. Each logical chain has one outstanding query and its next key
and cookie depend on its previous reply. Replies include the logical client ID.
All operations use the same initial data, 64 keys, seed and iteration counts;
mutable schedules may have different legal serial histories.

Controls include direct native GPU execution, packed LR/SC updates, a cooperative
native control where applicable, actual SEND/RECV try/wait with global or
owner-local state, and ordinary-memory software owner queues. Native controls
may perform legal independent work outside locks. No hardware arithmetic
operation is added. The CPU reconstructs client and key-version ordering,
replays every response and checks every final record. Shutdown must drain
logical-client DONEs, physical-server STOPs, transport credits and software queues.

The bounded primary set covers random immutable lookup, random mutable update,
single-hot update, and a heavier single-hot update. Zero/one/seven-step smokes
must pass on all machines before primary measurements. Whole-device core-zero
MCYCLE after readback/flush is the performance metric, not host wall time.
Sources, exact host/guest binaries, generated configuration, libraries and
per-run logs are captured in separate manifests. Physical 64 B frames, queue
depths and mailbox slots retain their existing semantics. Total mailbox data
capacity scales with physical endpoints and is disclosed for the small machine.

The completed experiment passes all 168 runs (72 smokes and 96 primary cases).
With eight physical threads/core retained, single-thread warps improve the best
message path by about 1.41x for random lookup and 1.38x for random updates, but
barely change single-hot updates. Messaging does not beat the strongest measured
native control in any primary condition. See the
[full comparison](forza_pgas_scalar_warp_comparison.md),
[raw executions](forza_pgas_scalar_warp_validated_results.json), and
[independent artifact verification](forza_pgas_scalar_warp_analysis.json).
