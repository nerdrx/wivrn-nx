# Lossless motion packing for native ASTC

This optional v3 packet mode reduces transmitted texture-block bytes without changing any encoded ASTC block or reconstructed pixel. It is compression prediction, not display-frame extrapolation: inaccurate motion choices increase residual bytes, rather than bend edges or shift objects incorrectly. The existing hardware sampler, upload and presentation passes stay unchanged.

## Admission and recovery

The PC compares each block with nine neighboring blocks in a recently decoded reference, records a one-byte selector and XOR residual, then compresses the selector prefix and residuals with Zstd3. It sends this representation only when its payload is at least 15% smaller than the independent Zstd/raw anchor. A rejected trial skips the next three prediction attempts while independent pictures continue. Quality control accounts for the actual 32-byte v3 header and chosen payload.

Only feedback with a nonzero `received_from_decoder` advances the acknowledged reference; receipt or submission alone cannot do so. The reference must exactly match one of the server's 16 cached frames and be 1–8 frame IDs old. Reordered feedback cannot rewind the acknowledgement. Missing, expired or unacknowledged references cause an independent anchor immediately. There is no wait for feedback. The client retains the 16 newest distinct decoded v3 frames, reuses their CPU buffers and rejects missing references or malformed selectors. Decoder loss cannot cause silent pixel drift: a missing reference fails that packet, and independent anchors restore decoding. This is not a guarantee of network recovery within two display frames.

An anchor uses v3 Zstd or raw data with `reference_frame = UINT64_MAX`; a delta names its decoded reference. Existing v1 raw/LZ4 and v2 Zstd formats remain unchanged. Each eye has its own cache, so prediction never crosses the eye boundary. At 2176×2176, 16 cached block buffers occupy about 18.1 MiB per eye, plus temporary output/residual storage. No cache block buffers are allocated by the ordinary v1/v2 path. The opt-in server also holds 16 raw frames per eye.

## Experimental configuration

Update both ends before using v3. This mode defaults off. The string value `"1"` enables the private encoder option; other values or absence disable it:

```json
{
  "encoder": {
    "encoder": "nxastc",
    "options": { "_wivrn_astc_motion_delta": "1" }
  }
}
```

This has not replaced the normal test profile. A sustained moving-scene Wi-Fi comparison remains required before enabling it by default. Ordinary fixed 8×8 image quality remains below the user's HEVC expectation; lossless packing does not itself fix block colour fidelity.

## Measurements and limits

Synthetic 1920×1080 wrapped pans saved 19–24% at one pixel and 7–11% at four pixels. The four-pixel candidates do not pass the 15% admission rule. Exactly block-aligned eight-pixel pans saved 95–97%, an unusually favorable case that should not be generalized to VR. Unrelated random frames became larger and fall back to independent anchors.

On a Pico A8110, the production packet decoder reconstructed a tiled, photo-derived 2176×2176 single-eye fixture exactly: 211,559 independent payload bytes became 161,864 delta bytes (23.49% saved). Median/p95 CPU decode was 0.908/1.086 ms for delta versus 0.559/0.590 ms for independent Zstd, with five warmups and thirty samples. This excludes cache rotation, staging copy, GPU upload, presentation, networking and photons. It ran while XR was idle, with no CPU frequency or affinity lock. It does not establish viewer FPS or live latency savings.

The native C++ prototype's stereo geometry was explicitly corrected from an erroneous 4×4-footprint-sized grid to 544×272 **8×8** blocks (4352×2176 pixels). Only corrected results are valid. Stronger independent Zstd levels saved about 2–5% while adding PC time; spatial XOR predictors increased packet bytes and were rejected.

## Moving-scene follow-up

A separate prerecorded 512×512 3D camera/object-motion clip does not reproduce the favorable wrapped-photo result. At a one-frame reference gap, 9/35 pairs pass the 15% admission rule, saving 4.34% aggregate payload; replaying the three-frame losing-probe cooldown reduces that to 1.55%. No pair passes at reference gaps of two, four or eight source frames. Wider search adds PC cost without fixing older-reference admission, so it is not integrated. These are offline fixed-gap fixtures, not live ACK/transport or native-headset measurements. The mode stays default off. See the NX Warp `motion-packing/stress` report for fixture bytes, graphs and rejected alternatives.

## Runnable checks

```sh
c++ -O2 -std=c++20 tests/nxastc_motion_test.cpp -lzstd -llz4 -o /tmp/nxastc-motion-test
/tmp/nxastc-motion-test
c++ -O2 -std=c++20 tests/nxastc_packet_decode_test.cpp -lzstd -llz4 -o /tmp/nxastc-packet-test
/tmp/nxastc-packet-test
```

These check compatibility, successful anchor/delta reconstruction, positive decode acknowledgements, reordered feedback, reference age, selector bounds, packet truncation and malformed compression. The same packet tests passed on the Pico. They do not exercise the complete live sender/receiver under Wi-Fi loss.
