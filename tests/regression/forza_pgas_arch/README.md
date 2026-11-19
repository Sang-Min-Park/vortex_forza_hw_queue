# Core-owned SRAM and destination ADD experiment

This bounded SimX experiment fixes the physical machine at two cores, two warps
per core and four lanes per warp. A key maps to core `key % cores` and owner
offset `key / cores`. Each request is one aligned 8-byte `{key32,delta32}` load.
Every mode computes exactly the same signed weighted scatter, with optional
replays of the same input stream. The host checks every output word and every
worker's input, local, remote and combined-publication counts.

`gpu_global` uses direct global AMOADD64. `gpu_local` uses owner-core SRAM for
local updates and global atomic partials for foreign updates, then owner pull.
`gpu_core_comb` combines in a full source-core SRAM table and publishes only
nonzero values with global AMOADD64. `engine_hybrid` performs local SRAM atomic
updates directly and foreign updates through the destination ADD device.
`engine_comb` uses the same combining front end and destination device.

All local state and source partial tables count against the same 16 KiB/core
LocalMem capacity. The engine combining mode reserves owner state plus its
source table. Capacity failures return 77 and are recorded as N/A. There is no
silent spill or extra private SRAM. Global output storage is initialized by the
host for all modes; SRAM initialization and combining-table clearing are timed.
One initialization/export is paid per launch, while each replay epoch performs
the same input work and combines independently. This is a throughput experiment,
not a claim that replaying a commutative sum is the best possible application.

The engine is an architectural addition: one actual extra LocalMem request port,
bounded queue/inflight storage, and completion ACK after destination AMO commit.
Its resource cost must accompany performance comparisons. All measurements use
the same frozen SimX machine. SimX's LocalMem and LLC AMO timing paths differ;
these results rank this model and do not establish a physical implementation's
optimum. No latency parameter is changed for an individual mode.

Run `python3 run_matrix.py --build-dir ../../../build64-forza-arch --stage all`
from this directory, or pass an absolute build directory. Builds pin shared
libraries and snapshot each mode. The primary metric is core 0 MCYCLE after all
output readback/cache flushing; kernel timestamps also expose initialization,
processing and export windows. Runs are sequential and have finite deadlines.
