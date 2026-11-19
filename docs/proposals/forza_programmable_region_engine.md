# Experimental programmable region execution

This opt-in SimX engine tests whether destination execution can be useful beyond
one fixed arithmetic opcode. Software writes a program through a bounded command
channel and waits for configuration acknowledgements before submitting work.
Fixed UPDATE and RECORD engines must be disabled in this build.

Each core has four execution contexts with eight64-bit registers/context, a
32-entry request queue and a1KiB program store (four programs ×32instructions
×8bytes). Instructions are LD, acknowledged ST, ADD, XOR, AND, ADDI, immediate,
sign extension, conditional branches and return. Software supplies two32-bit
arguments. An invocation owns one64-byte region in the existing16KiB LocalMem;
out-of-region or unaligned accesses are rejected. A per-region hazard check
serializes conflicting invocations. This bounds the transaction footprint and
does not implement arbitrary RISC-V code or cross-region pointer traversal.
GPU threads initialize/export these regions outside the drained execution phase;
concurrent direct CPU/other-engine accesses are outside the ownership contract.
The VM hazard check does not interpose on ordinary CPU LocalMem accesses.

Program fetch has one read port and a registered fetch/execute boundary; at most
one instruction fetch and one instruction execution occur per core per cycle.
One actual additional LocalMem request client handles all engine LD/ST traffic,
with existing bank timing and priority. Each context waits for every memory
response, including store acknowledgements. The engine performs no host-map
read/modify/write and contains no special cases for installed program IDs.

Results are three software-selected registers plus caller identity. The same
physical64B frame transport and existing two64B result slots support actor-mailbox
or physical-source completion. Both hold the context until reply READY is
acknowledged, then return the original operation credit. The direct completion
form bypasses actor registration but still uses the same physical network; it
is not an LSU-register-return instruction. All buffers/channels remain finite.

`tests/regression/forza_pgas_vm` uploads an11-instruction count/sum/XOR program
and a different11-instruction count/min/max program with data-dependent branches
and conditional stores. Every old tuple and final state is checked. Native GPU
controls implement the same programs with lock, warp grouping and fully timed
GPU prefix ownership. Both associative programs receive legal GPU scan/group
optimizations. Native owner state uses the same64B stride and LocalMem capacity.
Configuration, initialization, completion, publication and export are timed.

The contribution being tested is a constrained programmable near-memory engine.
If both call forms win equally, that is an execution-engine benefit rather than
an actor-exclusive messaging benefit. Program installation and area/power costs
must accompany performance claims; this implementation provides no RTL/PPA
result. Component units use real LocalMem and check memory identity, withheld
store/result ACKs, region isolation, branch/fetch guards and bounded saturation.
