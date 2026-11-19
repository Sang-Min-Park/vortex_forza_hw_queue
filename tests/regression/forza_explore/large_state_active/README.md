# Active worker completion control

This sibling experiment copies the frozen `../large_state` compact raw and batch
controls and stops a worker's receive/status polling once it has submitted all
its own requests, received its exact known one-shot incoming count, and observed
zero outstanding messages. The completed worker still joins the warp vote and
final barrier. This relies on known finite work and does not implement reusable
epochs or generic actor termination. Original source and binaries are retained.

Requests, input generation, state layout, signed arithmetic, owner-local bypass,
full-state CPU reference and host counter windows are unchanged. Modes are
`msg_raw_compact_active` and `msg_batch_compact_active`. Batch buffers remain
7,168B/core in the fixed two-core/eight-worker-per-core configuration. A 64B
physical packet carries either one compact 8B request or a count plus up to six
requests. Attempts count packets; sent/received count requests.

The permutation measurement touches every 64B sector in 2MiB twice: 32,768
distinct sectors, or 16,384 distinct 128B L2 lines. The frozen compatibility key
`distinct_state_lines` counts 64B sectors. The shared L2 is 1MiB. Counters cover
the entire kernel plus flush, including input/runtime/result traffic, rather
than isolated state accesses. No simulator timing is changed.
