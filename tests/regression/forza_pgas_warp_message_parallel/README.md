# Parallel mixed-key owner handling follow-up

This isolated follow-up retains the same frozen ISA machine, input/old-tuple
contract, source producer mapping and range32 transport as the main warp study.
Its only handler changes are ordered shader shuffles and a mixed-key path that
loads/stores independent key leaders concurrently across lanes instead of
serializing each distinct key. Equal-key requests still receive lane-prefix
old tuples, each key is committed by exactly one leader, and one-key cache
behavior remains unchanged for homogeneous quads.

The shuffle wrappers emit the existing ISA encodings using volatile asm with
memory clobbers. This prevents compiler sinking a cross-lane read into a
predicate that deactivates a required source lane; no hardware timing is changed.

The bounded matrix compares MMIO32, ISA try32 and ISA wait32 with freshly built
native global-lock, grouped-lock and timed bucket/prefix controls. All complete
per-key prefix checks, state/counter checks, source snapshots and binary/library
hash checks remain enabled. Earlier source/binaries/results are unchanged.

Store response values feed a final ordered shuffle so every leader's three
acknowledged updates complete before the next quad can read that key from a
different lane. Cache flush similarly consumes all three acknowledgments.
