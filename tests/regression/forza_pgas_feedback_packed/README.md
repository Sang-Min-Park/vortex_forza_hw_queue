# Packed native control for conditional feedback updates

This isolated follow-up uses the exact client program, initial state, 16-bit
reply-dependent cookie, conditional update and causal-history verification from
`forza_pgas_feedback`. It supplies no actor registration, mailbox, generic
messaging instruction or GPU service thread.

The mutable value is32 bits and its version is bounded well below32 bits, so
both fit in one64-bit word. The immutable next link remains separate. GPU code
packs this representation before the initial barrier, uses ordinary global
LR.D/SC.D attempts, and recomputes from a new observed word after failure. A
client advances and publishes its response trace only after successful SC.
All lane identities are explicit compiler dependencies of the asm results.
After the common completion barrier, the GPU unpacks the state for identical
CPU validation. Packing, retries, unpacking and export are timed.

The same eight dependent client chains run persistently. The CPU checks every
query key/cookie and old-value/version response, constructs client-order and
per-key ticket edges, rejects cyclic histories and replays every final record.
Legal adaptive interleavings can produce different query paths and final state
from other modes; no identical trace is claimed for mutable updates.

Only the update handler is measured. Walk already has both global-memory and
replicated-LocalMem native controls in the primary cohort. The machine and four
runtime/kernel libraries are identical to the ISA messaging experiment; the
new messaging hardware remains unused in this control. LR/SC is supplied by the
existing A extension. There is no new fixed arithmetic opcode or timing change.

Run the generated out-of-tree Makefile and `run_matrix.py` with a fresh cohort.
The source, guest, host, four libraries and individual logs are fingerprinted.
Primary cases match the conditional-update cases in the original feedback
cohort so best native controls can be selected without weakening its semantics.
