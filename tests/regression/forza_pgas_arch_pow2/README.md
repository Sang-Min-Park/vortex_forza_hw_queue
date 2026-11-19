# Power-of-two owner mapping optimization

This isolated sibling preserves the original five-mode sources and artifacts.
The host requires a power-of-two core count and supplies its precomputed shift
through the unused 64-bit ABI word. Kernel owner mapping uses `key & (cores-1)`
and `key >> shift`, mathematically identical to modulo/division. Both native
GPU-local and engine paths receive this optimization. Engine update addresses
use validated owner/offset bounds directly; the destination hardware continues
to validate each request. General-purpose public ABI helpers remain unchanged.
The GPU-global input/AMO path is unchanged and its binary/cycles are checked
against the original cohort. Algorithms, SRAM capacity, resource counts,
initialization/export and exact host validation are unchanged. No model or
library is rebuilt. The matrix runs 128-input smokes, nine 4096-input state/pattern
points and 1KiB/hot four-epoch replay, recording capacity N/A for dense combine
modes at 32KiB. This is a software optimization on the same frozen SimX model.
