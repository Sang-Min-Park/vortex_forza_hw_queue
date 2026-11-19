# Nonlinear programmable region transaction

This installs a third program on the unchanged generic VM hardware/library.
It captures the old64-bit count/sum/value, increments count, updates sum by
`old_sum&1 ? old_sum+signext(delta32) : old_sum^id32`, and adds the new sum to
value. Seventeen ordinary VM instructions implement all math/branches/memory.
The operation is noncommutative and does not become a constant min/max state.

The GPU controls are scalar cooperative locking, one lock for four same-key
requests applied sequentially, and fully timed GPU bucketing followed by four
cooperative input loads and sequential state transitions per key. Global records
are tested at both32B and64B stride. Native owner SRAM is packed24B/record while
the engine reserves64B regions; all use the same16KiB/core capacity. Direct and
mailbox calls use identical uploaded code and bounded engine resources.

Request inputs are identical. Legal concurrent execution orders can produce
different final checksums. Host validation sorts each key by returned count,
checks every old tuple, replays its nonlinear transition, then checks every
final state against that complete serial history. No mode is required to match
the input-index order, and no result is accepted merely by a checksum.
