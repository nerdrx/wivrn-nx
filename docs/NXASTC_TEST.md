# NXVC ASTC: native texture streaming experiment

The explicit `nxastc` encoder sends independent ASTC 8x8 texture blocks, using
LZ4 only when its output is smaller than the raw block payload. The Pico uploads
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
and motion smoothing off, without rewriting the user's saved profile. Unsupported
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

## Validation and current limits

The native PC server and matching OpenXR runtime build successfully. The Android
API 29 arm64 client builds with warnings treated as errors. Standalone packet
tests and shader compilation pass. A same-signature update was installed on the
connected Pico on 2026-10-03; the matching server is listening. At launch, the
Pico Guardian activity blocked the application switch, so live image quality,
delivered frame rate, and latency remain unverified until a real scene is running.

This first integration uses fixed block quality: the bitrate slider does not
change ASTC dimensions or endpoint precision. Compressed byte rate depends on
scene content. The experiment needs live network and presentation measurements
before it can claim a sustained 90 FPS result.

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
