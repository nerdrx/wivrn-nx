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
