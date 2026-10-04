# Reuse retained UDP receive batches

`UDP::receive_raw` previously allocated a new 20×2048-byte batch whenever a
packet still held the current backing storage. Video shards retain that storage
until reassembly releases them. The receive path now keeps 32 slots, including
the active buffer, and reuses only cached buffers with no external packet owner.
The active buffer is reused directly when unshared. When all slots are busy,
only a cache reference is evicted; held packets keep their bytes alive.

The cache owns at most 1.25 MiB of batch storage. Externally held evicted batches
can exceed that amount, as they could before. The cache is populated lazily.
Packet ordering, 2048-byte limits, truncated-datagram drops, AES processing and
single-thread receiving semantics are unchanged. No protocol or profile option
is needed.

Separately, a one-shard decoder handoff uses a local span instead of allocating
a vector. Multi-shard contiguous runs keep the vector path. Every current
backend consumes the outer span synchronously, matching the previous temporary
vector lifetime. Frame-completion and missing-view checks are unchanged.

## Validation

The real UDP loopback test `tests/udp_buffer_test.cpp` holds more than 32 batches,
checks payload immutability, released-buffer reuse, pending-message order, move
construction, oversized datagrams and short encrypted datagrams. Normal and
ASan/UBSan runs pass. Android native-client library and host/server builds pass.
These are source/build checks; no updated APK was installed for this work.

A matched PC production-API probe used 10 ABBA cycles, 20 runs per treatment,
400 batches/run, 20×1400-byte datagrams/batch and 16 retained batch owners. Both
socket implementations used matching `-O2 -std=c++20 -D_GNU_SOURCE` flags. It
counted allocation requests in the measured 40 KiB size range and checked bytes
and retained lifetimes outside timing. Allocation counts were 376→16 on every
run, 95.7% less. Median per-batch p50 was 1.965→1.940 µs; median per-run p95 was
2.560 µs for both. Timing is effectively unchanged in this local loopback probe.
An earlier Debug-versus-O2 comparison was invalid and discarded.

The allocation result does not measure Pico CPU time, Wi-Fi throughput, live
viewer FPS or photon latency. The published evidence is in NX Warp's
`bench/results/90fps-2026-10-04/overnight-recovery/udp-reuse` report.
