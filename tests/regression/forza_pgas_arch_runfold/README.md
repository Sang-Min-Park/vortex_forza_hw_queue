# One-entry producer run combining

This isolated follow-up preserves the five-mode cohort. `gpu_runfold` and
`engine_runfold` each retain one last key and signed 64-bit sum per producer
in registers, flush nonzero runs when the key changes, and flush the final run
at every epoch tail. GPU flushes use direct global AMOADD64; engine flushes use
local SRAM AMOADD64 or the destination ADD aperture with commit ACK. Inputs,
physical geometry, core ownership, state capacity and completion measurement
match the parent experiment. No full key-indexed table is allocated. The host
independently reconstructs every producer stream and verifies exact flush
counts, every final word, ACK drain, and logical input count. Zero-input and
single-input tail cases are included. Random key changes can make this slower;
GPU receives the same combining opportunity. This is a SimX-model comparison,
with the same LocalMem/LLC timing caveats as the parent experiment.
