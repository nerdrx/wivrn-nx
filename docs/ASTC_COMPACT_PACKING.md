# Independent compact ASTC packets

This experiment is **off by default**. It removes fixed mode bits from the two
ASTC 8×8 block layouts emitted by the PC shader, then uses the existing Zstd
level 3 compressor. The client restores exactly the same standard ASTC blocks
before uploading them to its existing hardware-sampled texture. There is no new
GPU reconstruction pass, temporal reference, motion warp or spatial downscale.

## Configuration and compatibility

Set the private per-encoder option `"_wivrn_astc_compact": "1"` on both native
ASTC eye encoders to try it. A client with v4 support is required: older clients
reject the compact payload. Leave the option absent for the unchanged v1/v2
selection. If the independent motion-packing option is also enabled, motion
packing takes priority and compact records are not used.

The encoder runs **one** Zstd attempt per frame using compact bytes when all
blocks have supported modes. Unknown modes retain ordinary ASTC input. LZ4/raw
fallback and the existing threshold for selecting Zstd remain available.
It does not recompress the original layout to compare every frame: doing so
would add another compression call for a modest wire saving. Consequently,
the experiment does not guarantee smaller packets for every scene or quality.

## Format

Each 16-byte ASTC block becomes a 14-byte record: one mode selector plus the
111 variable bits. The known low-17-bit headers are `0x100f3` and `0x10442`.
Other headers are rejected by the packing helper. A NAST v4 packet retains the
24-byte independent header and reports the full 16-byte-per-block texture size;
encoding value 5 means Zstd-compressed compact records. The declared Zstd
content size must match exactly 14 bytes per block, with no trailing frames.

The client decompresses into the beginning of its existing CPU scratch buffer,
then expands blocks backwards in place. Overlapping bytes within an expansion
use `memmove`; the hardware texture remains byte-identical. There is no extra
frame-sized scratch allocation or ACK/history dependency. Parser limits and
legacy v1–v3 behavior are retained.

## Evidence and limits

At fixed q6 on two native 2176² photo fixtures, compressed payload shrinks about
**5.0–5.8%**. Across the earlier 36-frame 512² camera/object scene, aggregate
Zstd payload shrinks about **4.9%**. This modest saving is not a claim of halving
VR bandwidth, improving source quality, or matching HEVC.

Host measurements of a single selected compact encode pass include field
packing and Zstd3. Some single-eye runs offset the packing cost; a separate stereo batch probe
was slower with compact records. This is a bandwidth experiment, not a guaranteed
PC speedup. Recompressing both layouts is explicitly avoided. Pico CPU tests cover complete strict payload
decode plus in-place expansion with byte-exact verification. They exclude
staging copies, GPU upload/render, Wi-Fi, presentation and photon latency.
The headset was asleep with display off, and clocks were not locked.

Native moving-scene live smoothness and whole-pipeline net benefit are still
unverified. This source addition does not install an APK, enable the option, or
change an active live profile.

## Validation

`tests/nxastc_compact_test.cpp` checks randomized variable bits, backward
expansion overlap, unknown mode rejection, shape/size bounds, malformed/trailing
Zstd frames, undeclared/wrong content sizes and legacy packets. Existing
`tests/nxastc_motion_test.cpp` covers v3 deltas and recovery separately.

```sh
g++ -O2 -std=c++20 tests/nxastc_compact_test.cpp -llz4 -lzstd -o /tmp/nxastc-compact-test
/tmp/nxastc-compact-test
/tmp/nxastc-compact-test --bench input.astc
/tmp/nxastc-compact-test --bench-decode input.astc
```

The decode benchmark alternates baseline/candidate order each iteration,
excludes verification from the timed interval, and verifies both outputs after
each decode. Host sanitizer checks and standalone ARM64 checks pass. Host/server
and Android client builds pass; these checks do not establish live VR quality.
