# Separate producer and service warps with generic transport

This isolated follow-up keeps the first generic record cohort unchanged. It
uses the same two cores/two warps/four lanes, with warp0 on each core serving
all core-owned keys and warp1 supplying requests (eight producers total).
Separate noinline functions give source retries and the GPU FieldHandler cache
separate register lifetimes. Every immutable launch field is copied at role
entry; the copied FieldHandler also caches its shape parameters rather than
reloading the argument buffer after assembly memory clobbers. Hardware performs
only opaque pointer delivery; all count/sum/XOR operations remain GPU code.

`gpu_field_core` fully times histogram/prefix/scatter and applies the same
cached FieldHandler to every core-owned key on warp0. Untouched keys are
explicitly zeroed. Other frozen native controls remain candidates for best GPU
performance; this pairing does not replace a faster native result. No input
prebucketing is performed by the host. Every original request and response is
unchanged; only the producer/owner placement changes for software_split.
