# Constant-time shard completion

`shard_set` counts successful unique insertions. `complete()` checks that count
against the slot-vector length and retains the existing last-shard timing check.
`missing_shards()` avoids walking a vector already known to have no holes, while
preserving inferred older-frame tails and parity filtering.

The slot vector is private; `shards()` exposes only const reads. `reset()` clears
the count. Parity reconstruction can resize the vector but increments the count
only through a successful insertion. Duplicates and rejected indices do not
increment it. Move operations transfer all frame state and reset their source,
which remains reusable; self-move does nothing. Copy assignment uses a temporary
to preserve the original invariant if allocation throws.

The change is internal and always active in source. Packet bytes, repair policy,
FEC rules, frame retirement and codec settings do not change. It requires a new
client build to affect an installed headset. Synthetic CPU query timings are
not decoder throughput, displayed FPS or motion-to-photon measurements.

Tests cover duplicate/out-of-order insertion, parity, failed reconstruction after
resize, copy/move/reset reuse and index limits. The independent differential
check compares completeness and missing lists against the original scanning
rules after 50,000 deterministic mutations.
