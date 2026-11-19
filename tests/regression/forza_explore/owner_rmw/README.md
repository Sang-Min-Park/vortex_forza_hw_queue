# Owner-only ordinary RMW correctness litmus

This diagnostic uses the exact large-state `permute` input generator,
signed deltas, seed and input hash. The host stably buckets requests by the
unique owner of each state word; preprocessing is outside the kernel and
this app is not a performance baseline. Each lane processes only its own
bucket. There are no FORZA CSRs, payloads, queues or receive handlers.

`MODE=plain` performs volatile ordinary load/add/store. `MODE=ack` replaces
the store with `amoswap.d`, consumes its returned value and requires it to
equal the preceding load. That response dependency acknowledges the store
before the worker's next iteration. Both modes check every final state word,
owner update counts, and an independent checksum of the values read before
each update. Optional `-v1` additionally rereads each owner's state in the
guest after phase synchronization and checks a final-state checksum.

Defaults reproduce state size 262144, 65536 updates, seed17, permuted lines,
hash `47336e6e1d18e574`. Input, state, and result buffers are allocated in that
order as in the original app, with owner offsets allocated afterwards. Input
order in memory is deliberately changed by bucketing, so a passing result
cannot alone exonerate other memory/cache schedules. A failing ordinary RMW
case with no message path would directly show that messaging is unnecessary
to trigger the corruption.

All prior sources/models remain frozen. Build from the new experimental tree
with matching CONFIGS, protecting shared libraries with make `-o` as agreed.
Modes save separate `kernel-plain.vxbin` and `kernel-ack.vxbin` artifacts.
