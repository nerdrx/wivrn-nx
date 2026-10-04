# Parallel native ASTC eye encoding

An experimental PC-only path overlaps the independent eye encoders. It is off
by default. Start the server with `WIVRN_ASTC_PARALLEL_EYES=1` to enable it.
It applies only when stream 0 and stream 1 are ASTC encoder instances and all
auxiliary encoder slots are empty. Other configurations keep serial encoding.

The compositor takes one shared-pointer snapshot for the frame, launches the
right eye with `std::async(std::launch::async)`, encodes the left on its existing
worker, then gets the future before releasing the input image or taking another
frame. Each eye owns its compression context, readback slots and codec state;
shared sender queue insertion is mutex protected. Worker creation failure falls
back to serial execution. Existing per-eye errors retain watchdog handling.
There is no additional Pico decoder, GPU pass or stream representation.

## CPU evidence

Two different native 2176×2176 q6 ASTC 8×8 fixtures, 20 warmups and 200 interleaved
samples, nonzero host load on 32 logical CPUs:

| Independent format | Serial batch p50/p95 | Right async + caller left | Persistent workers |
|---|---:|---:|---:|
| Ordinary Zstd 3 |3.516/4.096 ms|2.130/2.474 ms|2.172/2.742 ms|
| Compact + one Zstd 3 |4.059/4.558 ms|2.461/3.142 ms|2.433/3.265 ms|

Async launch p50/p95 is 0.0186/0.0303ms ordinary and 0.0458/0.0646ms compact.
Every mode produces byte-identical packets and verifies decompression against
the original ASTC blocks. This is a compression/packet CPU probe, not a live
compositor frame-time, Wi-Fi throughput, viewer FPS or photon-latency result.
Changing host load means close async/persistent differences are not significant
comparisons. GPU fence waits and sender backpressure remain in the real path.

## Acceptance limits

Source build and ownership review pass. Hot replacement retains the captured
encoders; shutdown drains the current frame before returning. Per-eye output
is ordered because the next frame does not start until both calls finish.
Sender insertion order can change, so stereo delivery and pacing must still be
checked in live motion before making this the default. Perfetto scopes share
an encoder track; overlapping slices should not be read as serial CPU time.
The current live server/profile is not restarted or modified by this addition.
