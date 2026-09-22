# Experimental direct-block Vulkan streaming

NXDB trades bandwidth for simple GPU sampling. The server packs NV12 compositor
images on the GPU; the client validates and uploads packed blocks, then samples
them inside its existing presentation pass. There is no separate full-frame
reconstruction pass. This is an opt-in experiment, not the default codec.

## Status: high-rate delivery is not ready

Live Pico testing on 2026-09-22 succeeded at a low saved bitrate (39 Mbit/s,
72 Hz), with visibly coarse peripheral blocks. A 90 Hz / 500 Mbit/s request did
**not** sustain fresh frames. The adaptive controller reduced the target, and
fixed-rate tests lost most complete frames. Whole-frame deadlines and retirement
of incomplete older frames did not solve this. The encoder averaged about
0.8 ms; receiver packet processing and wireless delivery need further work.

These are live observations, not proof of motion-to-photon latency. A 500 Mbit/s
request is a budget, not measured wire throughput. At that setting this test
packed 473,488 bytes/frame, or 340.9 Mbit/s at 90 frames/s before transport costs.

[Graphs and test limits](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/direct-live)

## Opt-in configuration

Use matching server and Android client builds; older clients do not understand
NXDB. Preserve the rest of the user's configuration and select:

```json
{"encoder":{"encoder":"nxwarp","codec":"nxwarp","options":{
  "backend":"direct","inter":"false","planar":"off",
  "snap-identity":"0","stereo-frame":"auto","rc":"auto","pace":"off"
}}}
```

The headset bitrate slider controls the packing budget. The normal range now
reaches 500 Mbit/s; the extended range reaches 800 Mbit/s. Reducing bitrate
progressively coarsens outer tiles, then uses solid colours. If even descriptors
exceed the budget, frame admission decreases rather than queueing old frames.
The packing planner reserves 25% for transport costs; this is not a hard wire
rate guarantee. Adaptive bitrate remains independently configurable.

Android development presets `debug.wivrn.test.bitrate_mbps` (1–800) and
`debug.wivrn.test.refresh_hz` (a supported headset rate) apply at startup and
save the setting. Clear each property after use to avoid overriding later UI
changes. Clearing a property does not restore the previous saved setting.

## Representation

Version 1 uses a 32-byte NXDB stream header and a 16-byte NXDF frame header,
followed by one 32-bit descriptor per 32×32 tile and packed block words.
Tiles select full, half, quarter resolution, or an inline RGB solid colour.
Each 8×8 block stores two RGB565 endpoints and 64 two-bit palette selectors
(20 bytes total). This path is not 10-bit. Geometry is aligned to 32 pixels.

Frames are independent. The transport uses one whole-frame band and retires
incomplete older frames when a newer complete frame arrives. A complete packed
frame is still required; partial-tile presentation is not implemented. This
makes loss of a few packets disproportionately damaging at high data rates.
There is no additional smoothing pass.

## Validation

- Wire parser: truncated data, invalid geometry/version, descriptor bounds and
  reserved bits checked under AddressSanitizer and UndefinedBehaviorSanitizer.
- Layout: mono/stereo geometry, 1–800 Mbit/s budgets, monotonic size and job bounds.
- Vulkan smoke test: NV12 stereo eye order and colour, solid and detailed modes;
  Vulkan 1.3 validation clean.
- Host server and Android release builds; matching APK installed on Pico.
- Live low-rate, adaptive 500/90 and fixed 500/90 runs with animated content.

Live reliability at 500/90 remains a failing test, not a release claim.

## Faster, smaller automatic bitrate adjustments

The AIMD controller now lowers bitrate by 10% for ordinary congestion or 20%
for severe congestion, with a 500 ms minimum between feedback-driven cuts.
Previously those cuts were 30% or 60%, with a 2-second minimum. Thus sustained
severe congestion can produce 500 → 400 → 320 → 256 Mbit/s instead of a single
500 → 200 Mbit/s cliff. Actual timing also requires enough new feedback; this
is not a promise of a switch every 500 ms.

Recovery waits for one second of healthy measurements, then uses 15% steps
with at least 500 ms of healthy measurements between subsequent steps, rather
than doubling. Ordinary upward probes use 2% of the ceiling (minimum 2 Mbit/s)
after one healthy second. Existing floor, ceiling, hysteresis, and rejection of
old in-flight measurements remain in effect. Radio-triggered AIMD drops are
also gentler; their separate timing and the bandwidth-estimation controller's
backoff timing remain unchanged.

These policy changes are locally tested; they do not establish that 500 Mbit/s
streaming is viable on the Pico or fix the underlying packet-delivery bottleneck.

Validation: 151 assertions across the NX Warp, radio, and bandwidth-estimator
controller tests passed, and the server rebuilt successfully. A virtual-clock
500 Mbit/s congestion case stepped to 450 then 405 Mbit/s, with 766 ms between
those cuts; the severe case first stepped to 400 Mbit/s. The recovery test
requires actual upward progress and bounds each increase. These are simulated
feedback results, not a new live Pico measurement.

## Trusted-LAN packet mode

Set `"trusted-lan":"true"` in the direct encoder options to replace the inner
SHA-256 counter-mode encryption and tag with plaintext plus CRC32. This is an
explicit speed/security tradeoff: CRC detects accidental corruption, but it is
not authentication and does not prevent a malicious sender from forging packets.
WiVRn's outer connection settings are unchanged. Never use this transport mode
as a standalone secure network protocol.

The NXDB stream header advertises version 2 for this mode; packed frame syntax
remains version 1. Updated clients accept versions 1 and 2, while older clients
reject version 2. Repeated headers cannot change the mode of an existing decoder.
All length, geometry, tile-index and output-buffer bounds checks remain enabled.
The default remains version 1 with the previous inner transport.

The receive path also retains payload scratch buffers across packets, reads tile
directories without allocating a temporary list, and avoids copying previous
frame metadata. Window eviction runs only when the newest frame advances.

The isolated Pico transport benchmark measured approximately 12.74 ms per frame
with legacy SHA framing versus 2.90 ms with trusted-LAN CRC framing (4.39×).
This excludes sockets, GPU work and display; live 500 Mbit/s delivery remains
unproven. [Workload, raw results and graph](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/packet-cost).

## Missing-prefix repair and freshest stereo selection (2026-09-22)

Fixed-chunk direct frames require chunk zero, contiguous indices, exact total
length and exact chunk sizes. A missing prefix previously allowed body data to
masquerade as a length and trigger false completion. Sparse/span framing keeps
its existing behavior. `tests/nxwarp_reassemble_test.cpp` covers both modes.

The viewer selects the newest complete direct stereo pair, avoiding retention
of older ready images due to predicted source timestamps. Short Pico runs
measured 44.6 ms receive-to-predicted-display at 200 Mbit/s versus 64.6 ms before;
83.6 fresh FPS after startup, 89.5 viewer submissions/s. This is not optical
photon latency. The 500 Mbit/s run still lost too many frames despite zero
direct-frame validation rejections.

[Measurements and graph](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/direct-freshness).

For explicit diagnostic capture only, Android property
`debug.wivrn.nx.capture_rejected=1` writes one rejected direct unit per decoder
to the app external data directory as `nxdb-rejected.bin`. Off by default;
clear the property after capture. Captures may contain image payload data.

## Experimental packet pacing (2026-09-22)

`"packet-window": "0.5"` spreads direct NX packets over half the configured
frame period (about 5.6 ms at 90 Hz), using the existing shard pacer. Accepted
range is 0 through 0.5; default 0 preserves burst sending. It is independent
of `pace`, which controls frame admission. Non-direct backends are unaffected.
The window covers NX payload bytes including transport parity, not outer
WiVRn/IP headers. Absolute sleeps prevent cumulative per-packet sleep drift;
a late sender does not sleep to catch up. This synchronous experiment can
consume encode-thread time and does not create extra network capacity.

It remains opt-in: short Pico testing did not establish an improvement at
500 Mbit/s. Use a sustainable bitrate instead of treating pacing as a cure.
[Short-run evidence](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/packet-pacing).
