# PyroWave probe (experimental)

PyroWave is an optional, all-GPU wavelet codec path for the custom WiVRn NX
server and viewer. It is selected with `"encoder": "pyrowave"`; NX Warp remains
the default preference. Both ends must be built from this branch because the
codec value changes the protocol hash. No live Pico result is claimed here.

This is a speed experiment, not a replacement justified by image quality.
The [matched full-image comparison](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-03/pyrowave-full-image)
used the same stereo source and actual encoded bytes. At approximately
500 Mbit/s and 90 Hz, NXVC was **2.24 dB better** on the forest image at
650,000 bytes versus PyroWave at 694,380 bytes; on the dark image NXVC was
**3.67 dB better** at 671,320 bytes versus PyroWave at 694,328 bytes. Near
65–90 kB per frame, the two were close. The initial larger PyroWave advantage
was a comparison against *foveated* NXVC and should not be used as evidence.

On the desktop RX 7900 XTX, the PyroWave benchmark reported a median 0.173 ms
for its GPU dequantization and inverse-wavelet stages. NXVC's separate Vulkan
tool reported 6.665 ms for Pass A and Pass B on a roughly matched frame. Those
tools time different scopes, so the ratio is **not** end-to-end speedup, Pico
decode time or motion-to-photon latency. The architecture is worth a headset
test because PyroWave's independent coefficient work is highly parallel and
avoids NXVC's comparatively costly entropy path.

## Wiring

- The server reads the compositor's 8-bit 4:2:0 planes, submits PyroWave encode
  work to the existing Vulkan queue, then sends packetized bytes through the
  existing WiVRn video shards. Its byte target follows bitrate and frame-rate
  changes. A 4 MiB per-frame target ceiling keeps the allocated buffers bounded.
- The client has a manual PyroWave codec choice and decodes into a three-plane
  Vulkan image sampled by the normal presentation path. Automatic selection
  still favors NX Warp. The server does not attempt incompatible mid-session
  hardware-codec failover.
- For a full-image comparison, use render scale 1, server `stream_scale` 1,
  foveation strength 0, and adaptive foveation off. Otherwise the encoder can
  still receive a previously downsampled or foveated image.

## Validation boundary

The PyroWave library, WiVRn server and Linux viewer compile in an isolated
build. The Android arm64 client library also links for API 29 with NDK 29.
The codec's offline reference CLI completed image round-trips for the
benchmarks linked above. APK packaging, live Pico transport/decode, motion
quality and headset thermals remain separate gates. The active streamer was
not restarted or changed during this probe.

The ported PyroWave codec files retain Hans-Kristian Arntzen's MIT SPDX
notices. WiVRn integration remains under WiVRn's GPL terms.
