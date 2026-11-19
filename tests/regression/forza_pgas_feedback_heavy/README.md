# Bounded heavy feedback controls

This followup raises only the host's accepted work bound from64 to512. It copies
the existing validated native, packed LR/SC, ISA wait global/local and corrected
software-owner queue guests byte-for-byte. Four frozen libraries are reused;
neither device source nor simulator is rebuilt. The packed-aware host otherwise
retains the same argument ABI, causal-history replay and final state checks.

Twenty cases use64 keys,128 dependent steps per each of8 clients, seed17,
work128/512 and hot/random placement. Each returned cookie influences the next
query. The work loop already takes a runtime parameter in the original guests.
The full device cycle counter includes initialization, termination and readback;
phase cycles are a separate metric. Mutable schedules may legitimately produce
different later keys, traces and final checksums; each history must independently
pass exact validation.

The host is compiled with the exact original recorded host compiler arguments,
changing only source/include/output paths. Guest hashes are checked against the
original accepted manifest before copying. All artifacts and shared libraries are
checked around every run. Results must also be compared with the separately
optimized grouped GPU baseline, whose immutable seed calculation can move outside
the lock; this followup does not establish the fastest possible GPU baseline.
