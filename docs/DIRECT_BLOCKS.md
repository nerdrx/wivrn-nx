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

## Experimental tile-aware concealment (2026-09-22)

Android property `debug.wivrn.nx.partial_direct=1`, read when a direct stream
opens, enables a conservative missing-region experiment. Default is off.
It requires current view metadata and a complete current header/descriptor
table. Each current tile is used only if its entire referenced block range
arrived. Missing tiles reuse the same tile from a validated complete frame,
with offsets repacked so changes in mode/layout cannot reinterpret old bytes.

At most 10% of tiles may be retained. History must be at most 50 ms old, measured
from first packet arrival on the client clock; this does not bound source-image
age before arrival. Concealed output never becomes history. Raw holes still
count as network losses, lost-frame feedback remains sticky in the server's
bitrate controller, and concealed frames are not acknowledged as exact references.
The wire format and normal direct path remain unchanged.

Short live runs salvaged 97 incomplete frames at requested 400 Mbit/s and 18 at
500 Mbit/s. Helper work averaged 0.43 and 0.52 ms per attempted recovery,
respectively, excluding complete-history copies and upload/presentation. This
rescues isolated damage, not insufficient bandwidth: 500 remained unusable.
Retained tiles can visibly lag during motion and have no separate pose correction;
the experiment is not enabled for normal use. Clear the property and reconnect
to disable it.

[Evidence and plots](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/partial-recovery).

### Motion guard and cheaper refusal (2026-09-22)

The opt-in client now requires received neighbors around every retained tile to
match complete history exactly in mode and block content. Neighbor checks stay
inside each eye. If any observed neighbor changed, or none can be observed, the
patch is refused and the viewer keeps its prior complete picture. A changed
mode for the missing tile also refuses recovery. This avoids splicing stale
patches into known changing boundaries; motion entirely inside a missing region
remains unknowable. No universal edge or comfort guarantee is claimed.

A deterministic moving-edge test exercises the real recovery helper and a CPU
equivalent of the presentation shader's block indexing. Both comparison paths
receive identical complete frames and lose identical chunks. Guarded recovery
refuses every camera-pan repair in this test; the whole-frame fallback preserves
line continuity at the cost of temporal freshness. Synthetic animations compare
the current decoded target, whole-frame hold, unguarded and guarded recovery.

Excessive damage is now rejected before output allocation/copy. Three alternating
Pico CPU benchmark pairs measured 0.969 to 0.263 ms per refusal (73% less) on a
full-quality 2176×2176 stereo payload. Successful unguarded helper time was
1.478 versus 1.483 ms. This isolates helper work; it is not a live FPS result.

[Motion clips, limitations and measurements](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/recovery-motion).


## Optional LZ4 transport envelope

Set `"lz4":"true"` in direct encoder options on matching server/client builds.
Default is off. LZ4 1.10.0 compresses each independent NXDF unit in independent
64 KiB input chunks. If the complete envelope saves less than 5%, the original
NXDF bytes are sent instead. The bitrate controller continues to budget the
uncompressed representation; it does not spend savings on extra image detail.
Frame byte counters and packetization use the actual compressed/raw result.
The host copies the mapped GPU output into a reusable CPU buffer first: direct
LZ4 reads over uncached Vulkan memory caused a severe live regression.

Stream version 3 announces LZ4 with the original transport; version 4 combines
LZ4 with trusted-LAN CRC. Versions 1 and 2 retain their existing meanings.
Older clients reject the new versions. Within a stream, NXDF magic identifies
raw fallback and NXDL identifies the compressed envelope. The 16-byte NXDL
header contains magic, envelope version 1, original size and chunk count.
Each chunk has three little-endian uint32 fields: decoded size, stored size,
and flags (0 raw, 1 LZ4), followed by exactly that many stored bytes.

The client bounds original size against the negotiated geometry, validates all
chunk lengths before allocating, calls `LZ4_decompress_safe`, then validates the
restored NXDF descriptors before GPU upload. No renderer change is needed.
Partial history recovery is disabled for LZ4 streams, including raw bypass units:
missing compressed bytes cannot be treated as missing pixel blocks. Existing
FEC/retransmission may restore packets, otherwise incomplete units are dropped
and reported through normal loss feedback. This first integration waits for a
complete unit; chunks are not independently presented.

[Fixture savings and Pico CPU cost](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/lz4)
are not a live latency claim. LZ4 is fetched from its pinned upstream release
with a SHA-256 check, under the BSD 2-Clause license in `lib/LICENSE`.

The September 22 source profile also reduces full/half-detail base radii from
0.42/0.72 to 0.34/0.64. For coarse samples, host averaging footprints widen
gradually outside normalized radius 0.65 to twice their width at radius 1.0.
Sample reads per block and the headset shader remain unchanged. This softens
source detail; four-color block quantization can still produce visible edges.
