# Actor graph scatter / integer SpMV

This benchmark computes one sparse graph scatter step:

```text
y[destination] += edge_weight(source, destination) * x[source]
```

The graph is stored as CSR rows. Each actor owns source vertices `rank + k*P`
and enumerates their irregular adjacency lists. Every edge sends one 32-byte
typed message through the actual FORZA Selector runtime; the destination
actor handles the message and updates its owned output vertex. This is an
integer sparse matrix-vector scatter step, not a ring message microbenchmark
or a complete BFS/PageRank implementation.

`owner(v) = v % P` is an application-level owner-computes rule. Owned output
values occupy disjoint contiguous actor slices at
`y[owner(v) * owned_stride + v/P]`. The global memory remains shared and
addressable; this benchmark does not add hardware protection or a general
PGAS allocation/one-sided-access API.

The host deterministically generates three graph shapes:

- `regular`: balanced source degrees and uniform random destinations.
- `skew`: Zipf-distributed source degrees and destination popularity.
- `hub`: balanced source degrees, with about 90% of edges targeting vertex 0.

Self edges and duplicate edges are valid and counted. The graph hash, degree
extrema, seed, dimensions, and cross-core edge count are printed so transport
comparisons can verify identical inputs. Every output vertex is compared
exactly with a CPU reference. The host also validates each actor's sent and
handled counts, DONE from all peers, and zero outstanding messages.

Both transports compile the same graph traversal, typed packet, handler,
Selector, producer progress loop, and completion method. Only the transport
backend changes. `TRANSPORT=hw` uses CSR-controlled ZEN/ZQM SRAM. `TRANSPORT=sw`
selects the separate software backend with bounded SPSC global-memory queues
per producer/consumer pair. `-q` sets each software pair's slot capacity;
default 16 and a two-slot sensitivity run are supported. Queue bytes and
buffering are not identical to the hardware model and must be reported.

All lane actors cooperatively progress until their warp has submitted its
edges, then `Selector::done()` continues progress until the phase closes.
The handler does no blocking work and produces no additional graph messages.

Device cycle snapshots exclude allocation, uploads, registration, initial
graph-state setup, and the initial global barrier. `max_actor_cycles` and
`phase_span_cycles` include graph traversal, payload handling, and DONE
completion. `data_span_cycles` ends when the last actual graph update is
handled, not merely enqueued. Every run exchanges an additional `P*P` DONE
markers. `start_skew_cycles` makes timestamp alignment visible. Whole-kernel
DRAM requests and retired instructions additionally include setup and result
flush; they are not restricted to the timed phase. Global memory counters
are queried once from core 0 rather than incorrectly summed across cores.

The primary comparison uses identical RV64 SimX configurations with shared
L2 and private D-cache disabled for both transports. This makes the software
queue's control and slot publication visible through the shared cache rather
than relying on unsupported private-L1 coherence. It is a controlled model
comparison, not a claim about a physical GPU or a synthesized FORZA design.
In particular, the current hardware-queue SRAM and crossbar model has not
been calibrated for bank conflicts, physical port counts, area, or RTL timing.

From a configured graph build tree:

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph TRANSPORT=hw
env -u DEBUG make -C tests/regression/forza_graph TRANSPORT=sw
cd tests/regression/forza_graph
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH="$PWD/../../../sw/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./forza_graph_hw -v64 -e256 -gskew -s17
./forza_graph_sw -v64 -e256 -gskew -s17 -q16
./forza_graph_sw -v64 -e256 -gskew -s17 -q2
```

The saved host binaries and `kernel-hw.vxbin` / `kernel-sw.vxbin` allow either
transport to be run after the other has been built. Use a fresh process per
measurement. `-p` bounds producer progress; a 180-second host deadline bounds
an incorrect completion path.

## Ordinary GPU atomic baselines

`TRANSPORT=gpu` builds `forza_graph_gpu` and `kernel-gpu.vxbin`. It assigns CSR
source rows cyclically to the same physical threads, multiplies each edge
weight by `x[source]`, and directly executes `amoadd.d` into a conventional
contiguous `y[destination]` array. `TRANSPORT=gpu_edge` builds the corresponding
`forza_graph_gpu_edge` and `kernel-gpu_edge.vxbin`; it assigns global edge IDs
cyclically to threads to reduce high-degree-row imbalance. Each edge finds
its source with a binary search of the original CSR row offsets inside the
timed region. No host-provided edge-source preprocessing array is used.

These kernels contain no Selector, mailbox, message handler, software DONE,
or FORZA enqueue/dequeue instructions. They need `VX_CFG_EXT_A_ENABLE` for
64-bit global atomic addition. Both modes validate the same per-vertex CPU
reference; direct kernels also validate the number of atomic updates issued
by each source-row/edge worker. Global destination writes are not restricted
to the conceptual actor owner. This is an algorithm comparison with ordinary
GPU execution, rather than a pure transport substitution.

Every direct thread fences after its atomic loop; the following global
barrier waits for all cores before the end timestamp. The data timestamp is
taken after the AMO-draining fence and before that final barrier. The vertex
mode preloads its first row offset and source value before the initial timed
barrier, as the actor kernel does. The edge mode includes its source lookup
cost in the measured interval. Graph initialization and uploads remain
excluded in every mode. For a matched comparison, enable the A extension in
the actor build and the direct build and use the identical simulator driver;
do not compare new direct results against older A-disabled actor results.

New actor builds also fence after `Selector.done()` before their end timestamp.
An ordinary posted-store fence alone does not prove that every actor output
store has drained to global memory. The direct AMO fence waits for actual
read-modify-write responses. To expose this distinction, every mode also
reports `device_cycles_post_readback_including_flush`: core 0's cycle counter
queried after the host has read all output and result buffers, including the
simulator cache flush. This secondary metric includes launch and initialization,
unlike the phase timestamps. `cycle_counter_status=0` indicates a successful
query; the existing three-part `counter_status` retains its original meaning.

```bash
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C tests/regression/forza_graph TRANSPORT=gpu
env -u DEBUG make -C tests/regression/forza_graph TRANSPORT=gpu_edge
env -u DEBUG make -C tests/regression/forza_graph TRANSPORT=hw
```
