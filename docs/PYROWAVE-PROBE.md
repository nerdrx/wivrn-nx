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

The [headless Pico 4 test](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-03/pyrowave-pico)
found and fixed a decoder correctness bug: the CPU-filled block-offset buffer
can reside in noncoherent memory on Adreno and must be flushed before the GPU
reads it. Before the fix, repeated decodes of the same packet produced
different bands. After the fix, native luma mean absolute error against the
desktop decoder fell from 24.46 to 0.29 levels, and repeated Pico captures
were byte-identical. Six successive full-image, 4:2:0 moving frames also
matched the desktop decode to about 0.15 luma levels mean absolute error.
These are synthetic pans, not a live viewer test.

With correct output, native 4352 × 2176 stereo took 20.35 ms GPU p50 /
20.66 ms p95 after two warmups, including 7.46 ms dequantization and
12.88 ms inverse wavelet/output at p50. A uniformly scaled, nonfoveated
2688 × 1344 image took 8.75 / 8.77 ms GPU, while its synchronous CPU call
took 12.66 / 23.45 ms. A separate 60-decode cycle of six synthetic-pan
frames at that size took 8.59 / 9.23 ms GPU and 19.96 / 23.77 ms for the
synchronous CPU call after six warmups. Neither proves 90 fresh frames/s
in the viewer;
the native decode exceeds an 11.11 ms frame budget by itself. Lowering the
native packet target fivefold from 500 to 100 Mbit/s saved only about 0.91 ms
GPU in paired short runs. Reconstruction cost dominates compressed byte count.
The desktop timing is not a Pico speed proxy.

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
benchmarks linked above. The Pico's Vulkan 1.1 driver exposes subgroup-size
control through `VK_EXT_subgroup_size_control`, so the port now queries and
enables that extension instead of assuming Vulkan 1.3. Its fragment edge
barrier uses Vulkan 1.1 commands; a 1920 × 1080 headless decode exercised
that path. A signed APK was installed in place for an initial connection
probe, then the original app was restored in place with its data preserved.
Live PyroWave transport, picture fidelity, motion quality, and sustained
viewer thermals remain unverified.

The ported PyroWave codec files retain Hans-Kristian Arntzen's MIT SPDX
notices. WiVRn integration remains under WiVRn's GPL terms.
