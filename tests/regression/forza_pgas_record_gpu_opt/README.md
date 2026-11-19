# Optimized native record controls

This sibling leaves the first four-mode record cohort unchanged. Warp locking
computes subgroup totals and lane prefixes before acquisition, shortening the
critical section. Its sums/XORs use two quad shuffle scan steps. Owner routing
is unchanged and fully timed; owner apply now assigns a warp per key, processes
four consecutive routed requests in parallel, performs tuple prefix scans, and
returns each original request's exact snapshot. All four lanes remain active
for shuffles, including a partial final batch. Acknowledged three-word commits
persist each final record in the same packed24B LocalMem before export.

The host checks all final values, every ticket prefix and exact lane work counts.
All inputs, resource capacities, completion metrics and frozen libraries match
the first cohort. These are strengthened native GPU controls, not a new message
primitive or a change to model timing.
