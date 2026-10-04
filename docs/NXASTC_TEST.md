# NXVC ASTC: native texture streaming experiment

The explicit `nxastc` encoder sends independent ASTC 8x8 texture blocks, using
LZ4 or Zstd when their output is smaller than the raw block payload. The Pico uploads
those blocks into a compressed Vulkan image. Its existing presentation shader
samples the texture; there is no full-frame software ASTC decode or extra RGBA
reconstruction pass.

The PC shader samples the compositor's YCbCr planes, reconstructs RGB, and writes
one fixed ASTC mode using three fitting iterations and six-bit colour endpoints.
Flat blocks use a constant-colour shortcut. This is a fast, approximate encoder,
not a general-purpose high-quality ASTC compressor.

## Live test configuration

Select `"encoder": "nxastc"` in an isolated server configuration and run the
matching server and client builds. Both eye streams use the rendered eye extent
at scale 1.0, with a neutral spatial foveation curve. The server does not
automatically select this experimental encoder.

For a non-persistent Android test override:

```sh
adb shell setprop debug.wivrn.nx.test_codec nxastc
```

Restart the client after setting this property. It requests the device's native
recommended eye extent, 90 Hz, one source frame per refresh, eight-bit colour,
and motion smoothing off, without rewriting the user's saved profile. The
experimental profile is opaque VR: passthrough alpha is disabled on both ends.
Unsupported
ASTC devices reject this override. Clear it with:

```sh
adb shell setprop debug.wivrn.nx.test_codec ''
```

Existing optional presentation filters remain user settings. For an unfiltered
timing comparison, turn those filters off in the headset UI. Do not compare a
filtered live stream against an isolated decoder microbenchmark.

## Bounds and latency policy

- Packet headers carry version, dimensions, exact raw block length, and payload
  length. Invalid or truncated packets are dropped.
- LZ4 decompression is bounded and must produce exactly the expected block size.
- Pending complete frames are limited to two; newer frames displace older queued
  frames. The image pool is bounded, and exhausted pools drop frames.
- Decoder completion telemetry uses the same frame's actual post-upload timestamp.
- ASTC texture support and linear sampling support are checked before advertising
  the codec. The normal headset pose reprojection remains in place.
- Asynchronous uploads and presentation use the same graphics queue. The
  transfer-to-fragment barrier orders texture reads without a CPU post-submit
  fence wait or an additional decoder timeline semaphore. Host handoff is not
  GPU completion. The tested Pico driver rejected that redundant semaphore with
  `vkCreateSemaphore: Incomplete`.

## Validation and current limits

The native PC server and matching OpenXR runtime build successfully. The Android
API 29 arm64 client builds with warnings treated as errors. Standalone packet
tests and shader compilation pass. On 2026-10-04, same-signature Pico updates
resolved decoder creation and the unused alpha decoder. WayVR starts against the
matching native runtime and delivered textures reach the headset presentation pass.

LZ4 now decompresses into reusable cached CPU memory before copying blocks to the
upload buffer. Its backreferences read previously written bytes, so decompressing
directly into sequential-write GPU staging memory was unnecessarily expensive.

The user's isolated live test runs with the built-in `--no-encrypt` server option.
Short stationary WayVR checks on the updated Pico client reached approximately
89–90 fresh updates/s, but the final restart also included a window with 164 fresh
updates across 180 submitted layers. This is **not sustained or moving-scene
90 FPS proof**. Neither timestamp sums nor runtime estimates are optical
photon-latency measurements.

Neutral presentation now defaults to specialization mode 2, removing inactive
post-processing and bleed branches from the existing shader. Enabling an effect
restores the general path, which remains selected for that defoveator's lifetime
to avoid per-frame pipeline rebuilds. Explicit
`debug.wivrn.nx.static_post=0` (or `WIVRN_NX_STATIC_POST=0` on desktop) retains the
general shader for comparisons. Two short Pico A/B runs reported this app's own
GPU pass at 3.7–4.0 ms for mode 0 and 1.3–2.0 ms for mode 2 after startup.
Those numbers exclude the system compositor, display scanout and photon latency.

ASTC quality responds to the controller/slider budget using measured packet bytes.
The seven rungs retain native dimensions: q6/q5/q4 use six/five/four-bit endpoints;
q3 uses four-bit endpoints and four weight levels; q2 uses three-bit endpoints and
four weight levels; q1 uses two-bit endpoints and constant weights; q0 sends coarse
flat block means. q1/q0 are emergency quality levels with severe block artifacts.
Size estimates expire after 30 encoded frames so a simpler scene can recover its
detail. Current-frame overruns also trigger downshifts; overruns above 150% can
skip two unmeasured rungs. An abrupt complexity increase invalidates stale
lower-rung estimates. The warmed simple-to-complex regression reaches q2 after
two significant overruns (q6 then q4); q2's 3.6% overrun is inside the existing
10% tolerance. This is a controller model, not a two-frame transport guarantee.
The controller uses hysteresis and chooses previously measured fitting rungs
rather than continually toggling between two overloaded settings. This is
adaptive quality, not a strict byte ceiling: a new scene or the q0 floor can still
exceed the budget.

Each encoder worker reuses a Zstd context and scratch buffer. Zstd level 3 is used
only when it saves at least 10% against the smaller raw/LZ4 candidate. It preserves
ASTC blocks exactly and uses independent frames, so it adds no reference recovery
chain. Packet version 1 retains raw/LZ4; version 2 carries Zstd. **Update both
server and client before testing this experimental profile.** Older clients reject
version 2. The decoder verifies a single complete Zstd frame, declared raw size,
and exact bounded output before uploading the texture.

Offline 4352x2176 dark/forest screenshot fixtures measured q2 plus Zstd at
251,029/133,680 bytes, versus the same-source q6 plus LZ4 at 508,605/318,924:
50.6%/58.1% less payload. RGB PSNR fell from 31.74/40.04 to 27.41/28.80 dB;
the quality loss is visible. These are synthetic stereo image fixtures, not live
motion or Pico frame-rate proof. Smoother 3x3 weight fields saved only about 20%
with Zstd and were not integrated. Native dimensions do not imply native detail.

Additional per-eye fixtures measured q2 plus Zstd3 at 243,333/130,834 bytes for
2176x2176 dark/forest, versus q6 plus LZ4 at 501,248/311,230: 51.5%/58.0% saved.
A dense 2176x800 crowd crop saved only 39.2%, so halving bytes is not universal.
Contrast-gated and luma-preserving endpoint variants increased bytes and were
rejected. Zstd levels 6/9 saved somewhat more but did not halve the crowd payload;
level 9 cost 3.9–9.7 ms per q2 host compression, so level 3 remains selected.

A standalone Pico CPU benchmark of one 2176x2176 eye measured 30 decompressions:
q6 LZ4 median/p95 0.433/0.442 ms, q6 Zstd 1.052/1.293 ms; four-level/three-bit
ASTC LZ4 0.400/0.407 ms, Zstd 1.019/1.215 ms. All output bytes matched. This
excludes upload, presentation and network handling. Debug builds now optimize
only the hot bundled compression libraries while retaining symbols/assertions;
the previous unoptimized Zstd cost up to 15 ms per eye on the PC.

The BBR acute-loss path is capped to reduce its previous budget. A bursty delivery
estimate previously allowed a nominal backoff to increase the budget during loss;
a regression test reproduces that case and verifies the cap.

Captured server and headset packets proved the former encrypted recovery bug: the
server packet decompressed correctly, while the received packet differed across
one shard and produced too few decoded bytes. UDP encryption mutates borrowed
payload spans; parity and retransmission history must snapshot their plaintext
before either socket sends them. The recovery path now uses that pre-send snapshot.

## Native Vulkan header pin

The C Monado compositor and C++ WiVRn server must compile against the same
Vulkan header version. The system headers were 1.4.357 while the local Vulkan
SDK headers were 1.4.309; mixing them changed embedded `vk_bundle`/`comp_base`
layouts by eight bytes and left Monado's swapchain command-pool mutex
uninitialized. Configure both targets with the local headers and dependency
prefix, then build the server and matching runtime:

```sh
cmake --preset server \
  -DWIVRN_USE_SYSTEM_BOOST=OFF \
  -DVulkan_INCLUDE_DIR=/run/media/nerdrx/Lex/claude/tools/local/include \
  -DCMAKE_PREFIX_PATH='/run/media/nerdrx/Lex/claude/nx-scratch/nxwarp-atlas-live-build/install;/run/media/nerdrx/Lex/claude/tools/local'
cmake --build build-server --target wivrn-server openxr_wivrn -j12
```

An optional, default-off v3 lossless motion-packing path is documented in
[ASTC_MOTION_PACKING.md](ASTC_MOTION_PACKING.md). It requires matched builds and
has standalone Pico CPU evidence, but no live-motion acceptance result yet.
