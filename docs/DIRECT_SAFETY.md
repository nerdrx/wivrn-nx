# Native safety prefix (experimental)

NXDS carries a small independent image before the detailed image in each native direct frame. It bridges short transport stalls while automatic bitrate responds. It does not use a second HEVC decoder or motion-vector extrapolation.

## Behavior

- Enable `"safety": "true"` alongside `"backend": "direct"` and optionally `"lz4": "true"`. Default is off; matched server/client builds are required.
- Applies to the paired-eye view, not quad layers. Each safety dimension is one quarter of the source, rounded up to 32 pixels: 2176×2176 becomes 544×544 per eye.
- Reserve `min(20 Mbit/s, total bitrate / 4)` from the existing total budget. Detail receives the remainder. Plan admission includes both images and transport allowance. At very low rates the format floor can lower frame admission; this does not guarantee 90 updates/s at every slider value.
- The client can publish a fully received safety prefix while detail is still arriving. It retains safety separately from the main frame ring.
- Keep the detailed image for two display refreshes without an update. Then choose a newer complete safety image. Return immediately to newer detail, or detail with the same timestamp as the displayed safety image. Never rewind source time.
- Loss feedback still reports missing detail, so fallback does not hide congestion from automatic bitrate. Safety presentations do not masquerade as complete detail feedback.

The sender places safety first **within a frame**. This is not an independently prioritized network channel and cannot overtake already queued older detail. If the safety prefix is lost too, the last complete image remains. A total radio outage cannot be hidden indefinitely.

## Wire and rendering

NXDB versions 5/6 advertise safety support and optional LZ4; version 6 uses trusted-LAN CRC. Older clients reject these versions. Existing versions 1–4 remain accepted.

The NXDS header is eight little-endian 32-bit words: magic `0x5344584e`, version 1, safety width, safety height, eye count, safety byte count, detail byte count, reserved zero. Independently raw-or-LZ4 safety bytes precede independently raw-or-LZ4 detail bytes. The existing outer transport length covers the entire envelope.

Only a contiguous complete safety prefix is accepted. Geometry, length, descriptor offsets and LZ4 bounds are validated before GPU reads. The fragment shader samples the small block geometry using normalized native-image coordinates; both eyes keep their original pose/FOV mapping. No extra full-screen reconstruction or blur pass is introduced. Two additional snapshot buffers are allocated only for safety-enabled streams.

## Validation

`tests/direct_safety_test.cpp` covers missing safety/detail chunks, malformed lengths/geometry, truncation and stream versions under AddressSanitizer/UBSan. `tests/nx_safety_selection_test.cpp` covers two-refresh boundaries, stale returns and equal-timestamp quality upgrades. `tests/direct_blocks_gpu_test.cpp` validates actual Vulkan output and red/blue eye separation in both segments; compile with `-DNX_DIRECT_TEST_WIDTH=2176` for the full-size fixture.

The opt-in Android diagnostic `debug.wivrn.nx.safety_loss_test=1` discards detail-only chunks for 30 of every 180 source frames. It is disabled by default and must be cleared after testing. This deliberately favorable loss pattern tests switching, not resilience to arbitrary radio loss.

[Pico evidence and limitations](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/safety-prefix).
