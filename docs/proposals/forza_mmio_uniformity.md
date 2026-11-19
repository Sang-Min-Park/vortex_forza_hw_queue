# MMIO actor-address uniformity correction

The first MMIO ABI returned actor-dependent status from one constant address.
The device selected the actor from the request's warp/thread metadata. That
hidden distinction conflicted with the SIMT compiler's treatment of a load
through a uniform pointer: the receive-ready branch was compiled without lane
splitting. When some lanes had a ready message and another lane did not, the
whole warp could skip POP. Full slots then blocked the incoming FIFO.

This was an interface/compilation-contract error. The device continued servicing
commands; it was not waiting on a full response channel or an unfinished AMO.
No LLVM pass, LSU pipeline, queue depth, latency, or fabric bandwidth was changed.

## Evidence before correction

The preserved original guest is in
`build64-forza-pgas/tests/regression/forza_mmio/snapshots/mmio-initial`.
Its receive path contains:

```text
0x180000408  ld a3, 0(a5)       # constant per-core RX_STATUS address
0x180000414  bnez a6, ...       # error branch
0x180000424  beqz a3, ...       # ready branch
0x180000428  amoswap.d ...      # POP
```

There is no `vx_split` between the status load and POP. The trace in
`smoke-mmio-commands.log` contains 6,620 status reads with different ready values
across lanes. POP stops after cycle 82,739, while command processing continues
through cycle 604,277. The bounded guest also stalls after exactly 200 delivered
messages. Initialization alone passes, and the same ring/PGAS program using the
ordered CSR backend passes.

The original model, ABI, and actual simulator DSO are preserved in
`build64-forza-pgas/snapshots/mmio-uniform-address-model`.

## Corrected contract

Each physical actor owns an explicit 0x200-byte endpoint region:

```text
actor = physical_warp * threads_per_warp + physical_lane
address = actor_zero_endpoint + actor * 0x200
```

`forza_mmio_actor_address` constructs the address. REGISTER, status, count, and
POP accesses use it. The device verifies that the encoded actor equals the
request's warp/thread metadata, then normalizes the endpoint offset. A different
actor's endpoint returns INVALID. The physical endpoint actor is independent of
the logical PE number registered for message routing. TX addresses continue to
encode the destination zone, logical PE, and mailbox.

The corrected guest computes actor address bits with `slliw ..., 0x9` at
0x1800000a4. Its receive path now contains:

```text
0x180000498  ld a1, 0(t1)       # explicit actor-dependent RX_STATUS address
0x1800004a4  vx_split_n ...     # error branch mask
0x1800004c0  vx_split ...       # ready branch mask
0x1800004cc  amoswap.d ...      # POP only for ready lanes
```

The Makefile explicitly tracks both the transport and shared ABI headers; the
validated guest was rebuilt after those dependencies were corrected.

## Validation

Normal and ASAN/UBSAN channel contracts pass for fragmented lanes, sparse masks,
actor identity and wrong-actor rejection, all mailbox byte offsets, zero/full64
payloads, slot reuse, bounded BUSY, receive progress under TX backpressure, ACK
credit return, mixed SRAM/MMIO packets, and a held-full response sink. The
existing CSR/SRAM/255-credit zone regression also passes with MMIO enabled.

The rebuilt actual guest passes with 16 actors, 64 ring messages per actor,
zero and high-bit payloads, and cross-core GET/UPDATE/reply operations on real
core-local SRAM. All actor counts, state values, and error fields match the host
reference. Model and unit provenance is recorded separately in
`build64-forza-pgas/tests/unittest/forza_mmio/mmio-actor-address-manifest.json`.
