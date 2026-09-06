# NX Warp hybrid mode: the idle HEVC ASIC as an atlas patch source

Scoping report and prototype measurements, 2026-09-06.
Branch `nx-warp-hybrid` (worktree of `wivrn-nx-e2e` @ 6e4611c4).
Prototype code: [`hybrid-proto/`](../hybrid-proto/).

Companion to nx-warp `docs/adr/0029-atlas-reference.md` (branch `atlas`, 0b162e5),
whose **Cheat 7** this report exists to price, and to nx-warp `docs/HYBRID.md`,
which described the *previous* (layered, whole-frame-prediction) hybrid that
ADR-0022 rejected. **This is not that design.** Under the atlas model the base
layer is not a prediction hypothesis for the whole picture; it is a second
*source of patches* for individual atlas tiles, competing per tile with an nxvc
coded tile. That change is what makes the question worth reopening.

Every number below was taken on the attached device. Nothing is estimated from
literature; where a quantity was not measured it says so.

---

## 0. Summary

**Recommendation: GO for a Phase-1 prototype, on the strength of three
measurements and against one unmeasured risk.**

| Question | Answer | Confidence |
|---|---|---|
| Is the Pico's HEVC decoder bit-exact with a conforming software decoder? | **Yes. 180/180 frames byte-identical** across three bitrates. | Measured, decisive |
| Is the AHardwareBuffer → Vulkan → integer-YCoCg-R path bit-exact? | **Yes. 0 of 1 183 744 samples differ** from a CPU computation on the reference decode. | Measured, decisive |
| What does a base-sourced atlas patch cost on the GPU? | **1.9 µs per 64×64 tile.** An nxvc coded tile is ~41 µs; the old normative skip warp was 34 µs. | Measured |
| Does the base layer cost the GPU anything to decode? | **No.** It runs on the ASIC. Two eyes at 1088² sustain 721 fps aggregate. | Measured |
| Is MediaCodec latency really 8–20 ms? | **No. 2.76 ms mean / 5.6 ms p99** at 9.6 Mbit with the Qualcomm vendor key. | Measured |
| Can two WiVRn encoders cover the same region today? | **No** — but the blocker is on the client, and the server's partition already permits it. | Read the code |
| Does the base actually improve quality per bit as a patch source? | **Not measured.** This is the open risk. | — |

The one-line case: **a full-frame base refresh of both eyes costs 1.1 ms of
GPU; coding the same 578 tiles in nxvc would cost 23.7 ms.** Hybrid does not
buy more coded tiles — it buys a refresh floor, which is precisely the cost
ADR-0029 Cheat 3 (foveated refresh) currently pays in periphery staleness.

---

## 1. The existing H.265 path

### 1.1 Client (`client/decoder/android/`)

`wivrn::android::decoder` (`android_decoder.h:63`) handles all three hardware
codecs; there is no HEVC-specific class. The path is already exactly the one
hybrid mode needs, which is the single biggest reason this is cheap to build.

* **Configuration** (`android_decoder.cpp:139-147`): MIME, width, height,
  `KEY_OPERATING_RATE = ceil(frame_rate)`, `KEY_PRIORITY = 0`. Notably
  **`KEY_LOW_LATENCY` is never set**, and the Qualcomm vendor key is present
  but commented out at line 141:
  ```cpp
  // AMediaFormat_setInt32(format.get(), "vendor.qti-ext-dec-low-latency.enable", 1); // Qualcomm low latency mode
  ```
  Section 2.2 measures what that comment costs: **0.97 ms of mean latency.**
* **Output**: an `AImageReader` created with `AIMAGE_FORMAT_PRIVATE`,
  `CPU_READ_NEVER | CPU_WRITE_NEVER | GPU_SAMPLED_IMAGE`, `maxImages = 7`
  (`android_decoder.cpp:121-133`), whose `ANativeWindow` is handed to
  `AMediaCodec_configure` (`:171`). Output buffers are always
  `releaseOutputBuffer(..., true)` — rendered, never copied (`:538`).
* **Import** (`map_hardware_buffer`, `:403-503`): `AImage_getHardwareBuffer` →
  `getAndroidHardwareBufferPropertiesANDROID` → `VkImage` with
  `format = eUndefined` + `VkExternalFormatANDROID` → dedicated
  `VkImportAndroidHardwareBufferInfoANDROID` allocation → `VkImageView` +
  `VkSamplerYcbcrConversion`. **The import is cached by `AHardwareBuffer*`**
  (`android_decoder.h:115`), so in steady state it is a hash lookup.
* **Colorimetry**: the driver's suggestions are discarded and BT.709 / full
  range is hardcoded (`:370-372`) with the comment that the decoder's metadata
  "is garbage". Chroma offsets and **the swizzle** are taken from the driver.
  Section 3.3 shows the swizzle is load-bearing and not identity.
* **Sync**: none. `VK_KHR_external_semaphore_fd` is not used anywhere in the
  client; `android_blit_handle` leaves `semaphore` null and relies on
  `acquireLatestImage` plus holding the `AImage` for the handle's lifetime.
* **To the compositor**: one layout transition to `eGeneral`, then the YCbCr
  image view + the decoder's sampler are bound as an **immutable** combined
  image sampler in `stream_defoveator` and sampled directly by
  `reprojection.frag`. No blit, no copy.
* **Instrumentation**: there is **no decode-begin/decode-end stamp**. The
  server infers decode cost as `received_from_decoder - sent_to_decoder` from
  `from_headset::feedback` (`common/wivrn_packets.h:903`), where
  `sent_to_decoder` is set equal to `received_last_packet`
  (`shard_accumulator.cpp:363`) — i.e. it is "frame complete", not a decoder
  measurement. Only the nxwarp decoder reports real decode cost
  (`nxwarp_feedback::decode_us`).

### 1.2 Server (`server/encoder/`)

`video_encoder` (`video_encoder.h:56`) with backends selected by name
(`video_encoder.h:49-54`: `nvenc, vaapi, x264, vulkan, raw, nxwarp`).
H.265-capable backends:

| Backend | File | Rate control | IDR / refresh |
|---|---|---|---|
| FFmpeg VAAPI | `ffmpeg/video_encoder_va.cpp` | `bit_rate`, `gop_size = INT_MAX` (`:320-322`) | IDR only; **intra-refresh and ref-invalidation both unsupported**, logged once at `:273-292` |
| NVENC | `video_encoder_nvenc.cpp` | CBR, `vbvBufferSize = bitrate/fps*2` (`:136-146`) | `FORCEIDR`, `nvEncInvalidateRefFrames` (`:698`), `forceIntraRefreshWithFrameCnt` (`:723-725`) |
| Vulkan Video | `video_encoder_vulkan_h265.cpp` | `VideoEncodeRateControlLayerInfoKHR` (`:235-260`) | no IDR at all in steady state — `dpb_state` re-references the newest *acked* slot (`:34-127`) |
| x264 | `video_encoder_x264.cpp` | ABR | h264 only |

All IDR policy funnels through `idr_handler` (`idr_handler.h:55`) whose
`frame_type` enum is `{i, p, refresh, invalidate}` — the escalation ladder
hybrid's loss handling needs (section 5.3) already exists.

### 1.3 Can two encoders cover the same region? — the important finding

**Not today, and the reason is not what the task assumed.** Upstream WiVRn
partitions a frame among encoders by rectangle. **This fork has removed that
entirely.** There is no `offset_x`, no `offset_y`, no per-encoder rect anywhere
in the tree. Instead:

```cpp
// server/encoder/encoder_settings.h:86-89
inline constexpr size_t num_streams = 4;   // left, right, alpha, quad
inline constexpr uint8_t quad_stream_idx = 3;
```

Everything is `std::array<..., num_streams>`, one encoder per fixed slot, and
geometry is *derived from the slot index*, never configured
(`encoder_settings.cpp:390-398`; `wivrn_packets.h:1337-1348`
`stream_size(stream_index)`). `stream_idx` is the only demux key on the wire
(`video_encoder.cpp:661`) and the client's only routing key
(`stream_network.cpp:230-239`). Two encoders sharing an index would interleave
shards into one `shard_accumulator` and corrupt each other.

**But the server's partition is by Vulkan array layer, not by rect, and nothing
enforces layer uniqueness.** `src_layer` is read in exactly one place — the
barrier at `compositor.cpp:970-976` — and the present loop hands each encoder
the *whole* image (`compositor.cpp:1043-1058`). Two encoders with the same
`src_layer` would both receive it, and two read-only barriers on the same layer
to the same layout are legal. So:

> The server side of "two encoders on the same region" is close to free. The
> blocker is entirely client-side: `decoder_count = 4` with a hard-wired
> mapping from stream index to compositing role (`stream.cpp:1165-1200`).

The extension is therefore: `num_streams 4 → 6`, indices 4 and 5 being the base
layer for eye 0 and eye 1 with `src_layer = 0` and `1` — aliasing streams 0 and
1 deliberately — plus a client-side rule that streams 4/5 do **not** become
views but feed the nxwarp decoder's atlas. Section 6 sizes that work.

Two further server-side consequences worth naming now:
* `split_bitrate()` (`encoder_settings.cpp:59-97`) weights by `width*height`, so
  adding two full-size streams halves everyone's share unless the base is given
  an explicit weight. The base's share must be a policy, not an accident.
* `prober::select_encoder()` (`:246`) never picks `nxwarp` unless explicitly
  named, so a mixed nxwarp+HEVC configuration is already expressible in config.

---

## 2. Measurements: MediaCodec HEVC decode on the Pico 4

### 2.1 Method

Standalone native harness [`hybrid-proto/src/nxhevcbench.c`](../hybrid-proto/src/nxhevcbench.c),
built for arm64 against NDK 29.0.14206865 (API 29) and run from `adb shell` in
`/data/local/tmp`. **No app, no activity, no window** — `AImageReader` provides
a producer surface without a display. It never touches `wivrn-server` or the
installed `org.meumeu.wivrn.nx.warp` package.

It reproduces `android_decoder.cpp`'s configuration exactly (same
`AImageReader_newWithUsage` arguments, same `maxImages = 7`, same
`OPERATING_RATE`/`PRIORITY`, same `releaseOutputBuffer(render = true)`), so the
numbers transfer.

*Harness note:* `AMediaCodec_setAsyncNotifyCallback` is accepted by
`OMX.qcom.video.decoder.hevc` on this device but **never delivers an
input-buffer callback** (0 callbacks in 5 s, reproduced). The harness therefore
uses the synchronous dequeue API with a drain thread. Worth knowing before
anyone refactors the client onto async NDK callbacks.

**Content.** nx-warp corpus `vr-mixed-1024-v2` (head-rotation class), left eye
cropped and Lanczos-scaled 1024²→1088², looped to 288 frames. Encoded with
x265 in the shape a WiVRn base layer would have: `bframes=0 ref=1 scenecut=0
rc-lookahead=0 keyint=90 repeat-headers=1 slices=1 **ctu=64**`. CTB 64 is
deliberate — HEVC CTB boundaries then coincide with the nxvc 64×64 tile grid.
Generator: [`hybrid-proto/gen-streams.sh`](../hybrid-proto/gen-streams.sh).

| stream | mean AU | actual rate at 90 fps |
|---|---|---|
| `base_1088_5m` | 6 534 B | 4.70 Mbit/s |
| `base_1088_10m` | 13 283 B | 9.56 Mbit/s |
| `base_1088_50m` | 69 480 B | 50.03 Mbit/s |

Two things are measured separately because they answer different questions:

* **latency** — one access unit in flight. Submit → the instant an
  `AHardwareBuffer` is in hand. This is what an atlas refresh actually waits for.
* **throughput** — codec kept fed. Steady-state output interval, i.e. how much
  of the ASIC one stream occupies.

### 2.2 Results — `OMX.qcom.video.decoder.hevc`, 1088×1088

Serial submit→AHardwareBuffer latency, milliseconds, n = 200 after 40 warm-up:

| stream | knob | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|
| 4.70 Mbit | none (as the client is today) | 3.377 | 3.024 | 5.225 | 11.353 | 13.808 |
| 4.70 Mbit | `low-latency` (API 30 key) | 3.340 | 2.905 | 5.776 | 10.224 | 17.854 |
| 4.70 Mbit | **`vendor.qti-ext-dec-low-latency.enable`** | **2.825** | 2.535 | 4.600 | 6.596 | 6.888 |
| 9.56 Mbit | none | 3.723 | 3.406 | 5.763 | 7.110 | 11.990 |
| 9.56 Mbit | `low-latency` | 3.691 | 3.315 | 5.444 | 11.079 | 14.286 |
| 9.56 Mbit | **`vendor.qti-ext-dec-low-latency.enable`** | **2.756** | 2.505 | 3.983 | 5.627 | 6.195 |
| 50.03 Mbit | none | 6.749 | 6.450 | 8.525 | 13.745 | 17.720 |
| 50.03 Mbit | **`vendor.qti-ext-dec-low-latency.enable`** | **5.164** | 5.065 | 7.206 | 8.174 | 12.667 |

Sustained throughput (ASIC occupancy per 1088² frame):

| stream | knob | mean interval | sustained | occupancy |
|---|---|---|---|---|
| 4.70 Mbit | none | 1.287 ms | 716.4 fps | 1.40 ms |
| 9.56 Mbit | none | 1.301 ms | 713.2 fps | **1.40 ms** |
| 9.56 Mbit | qti | 1.349 ms | 707.4 fps | 1.41 ms |
| 50.03 Mbit | none | 3.717 ms | 255.7 fps | 3.91 ms |
| 50.03 Mbit | qti | 2.629 ms | 360.7 fps | 2.77 ms |

**Two eyes concurrently** (two independent decoder instances, 9.56 Mbit, qti key):

| | interval mean | sustained | latency mean | latency p99 |
|---|---|---|---|---|
| eye L | 2.634 ms | 358.9 fps | 2.804 ms | 4.582 ms |
| eye R | 2.609 ms | 362.3 fps | 2.753 ms | 4.528 ms |
| **aggregate** | | **721.2 fps** | | |

### 2.3 What these numbers change

1. **The 8–20 ms figure in `HYBRID.md` §3.1 and ADR-0029 Cheat 7 is wrong for
   this device by a factor of three to seven.** `HYBRID.md` §6 lists that number
   as "a literature number, not ours … the weakest number in this document" and
   asks for exactly this measurement. It is **2.76 ms mean, 5.63 ms p99** at
   the operating point that matters, and the p99 matters more than the mean
   because the deadline is fixed. That is inside one 90 Hz frame with room to
   spare, and inside two 240 Hz frames.
2. **The Qualcomm vendor key is worth setting and the API-30 key is not.**
   `low-latency` moved the mean by 0.03–0.04 ms (noise) and made the tail
   *worse* at 4.70 Mbit. `vendor.qti-ext-dec-low-latency.enable` removed
   0.55–1.59 ms of mean and, more importantly, cut p99 from 11.4 ms to 6.6 ms
   and max from 17.9 ms to 6.9 ms. **This is a free win for the plain HEVC path
   today, independent of everything else in this report** — it is the
   commented-out line at `android_decoder.cpp:141`, and INTEGRATION.md item 10
   and ADR-0027 §3 already ask for it.
3. **Two eyes do not contend.** 721 fps aggregate for two instances versus
   713 fps for one: the ASIC is the shared resource and it is nowhere near
   saturated. Two eyes at 240 Hz needs 480 decodes/s against 721 available —
   1.5× headroom at 9.56 Mbit. Per-eye latency was unchanged by the second
   instance (2.76 → 2.80 ms).
4. **Bitrate buys latency, steeply.** 50 Mbit costs 1.9× the latency and 2.8×
   the ASIC occupancy of 9.56 Mbit. A base layer belongs at the low end, which
   is where the design wants it anyway.

---

## 3. Measurements: AHardwareBuffer → Vulkan → atlas tile

Harness [`hybrid-proto/src/nxahbvk.c`](../hybrid-proto/src/nxahbvk.c) +
shader [`atlas_patch.comp`](../hybrid-proto/src/atlas_patch.comp). Same decode
path as above, then per frame: import the AHB (as `map_hardware_buffer` does,
cached by pointer) and dispatch a compute pass that converts a list of 64×64
tiles into the codec's coded sample domain and writes them into an atlas image
laid out per ADR-0029 §1 (full-extent Y plane `R8_UINT` 1088², Co/Cg plane
`R16G16_SINT` 544²). Timed with `vkCmdWriteTimestamp` on the compute queue.

Device: **Adreno (TM) 650**, Vulkan 1.1.128, `timestampPeriod` 52.083 ns,
`timestampValidBits` 48.

### 3.1 Import cost

| | measured |
|---|---|
| cold import (`vkCreateImage` + `vkAllocateMemory` + view + descriptor set) | **0.80–1.09 ms**, ×15 |
| cached lookup, steady state | **0.0007 ms** (0.7 µs) |

15 cold imports is the `AImageReader` pool turning over once (`maxImages = 7`,
plus the driver's own). **This is a one-off cost of about 13 ms at stream
start**, then free — exactly the behaviour `android_decoder.cpp`'s
`hardware_buffer_map` was written for. It is not a per-frame cost and must not
be modelled as one.

*Caveat:* `android_decoder.cpp:415-422` clears that cache whenever the AHB
format properties change, and the `TODO` at `:421` notes the defoveator's
immutable-sampler pipelines are *not* rebuilt. A hybrid client adds a second
consumer of that cache and makes the TODO load-bearing.

### 3.2 Conversion cost — the number the budget turns on

Tile count swept, 100 measured frames each, 9.56 Mbit stream, luma + 4:2:0
chroma:

| tiles | pixels | mean | p50 | p95 | **per tile** |
|---|---|---|---|---|---|
| 289 (full eye) | 1 183 744 | 0.5345 ms | 0.3946 | 1.5930 | 1.8 µs |
| 144 | 589 824 | 0.2531 ms | 0.2000 | 1.1043 | 1.8 µs |
| 72 | 294 912 | 0.1916 ms | 0.1041 | 1.1974 | 2.7 µs |
| 36 | 147 456 | 0.0685 ms | 0.0562 | 0.0586 | 1.9 µs |
| 18 | 73 728 | 0.0328 ms | 0.0324 | 0.0352 | 1.8 µs |
| 9 | 36 864 | 0.0202 ms | 0.0202 | 0.0209 | 2.2 µs |

**A base-sourced atlas patch costs 1.9 µs per 64×64 tile and scales linearly.**
Against ADR-0029's own per-tile figures on the same GPU:

| operation | per tile | ratio |
|---|---|---|
| **base-sourced patch (this work)** | **1.9 µs** | 1× |
| normative integer skip warp (the thing ADR-0029 removes) | 34 µs | 18× |
| nxvc coded tile: Lite Pass A 16 µs + Pass B ~25 µs | ~41 µs | 22× |
| nxvc intra tile | ~12 µs | 6× |

### 3.3 Bit-exactness — the load-bearing measurement

ADR-0029 Cheat 7 prefers **Option B**: the encoder runs a conforming HEVC
decoder, so its shadow atlas is exact, and the residual risk is the NV12→atlas
conversion, made a normative integer transform. That rests on a claim about
this specific silicon. Two independent checks:

**(a) Is the Pico's HEVC decoder bit-exact with FFmpeg's?**
Decoded each stream to ByteBuffer output on the device
(`COLOR_FormatYUV420SemiPlanar32m`, stride 1536, slice-height 1536), de-strided
to planar yuv420p, pulled, and compared byte-for-byte against
`ffmpeg -f hevc -pix_fmt yuv420p`:

| stream | frames compared | identical |
|---|---|---|
| 4.70 Mbit | 60 | **60 / 60** |
| 9.56 Mbit | 60 | **60 / 60** |
| 50.03 Mbit | 60 | **60 / 60** |

**180 of 180 frames byte-identical.** HEVC's decoding process is normatively
bit-exact and this device honours it.

**(b) Is the GPU path from AHardwareBuffer to atlas sample bit-exact?**
This is the step the spec does *not* guarantee. The shader reads the imported
AHB through a `VkSamplerYcbcrConversion` configured `RGB_IDENTITY` + `NEAREST`
+ full range — so the sampler performs no colour conversion and no chroma
filtering, and the YCbCr→R'G'B'→YCoCg-R step is ours, in integer arithmetic.

First, dumping each raw sampler channel and matching it against every reference
frame and plane:

```
channel .r: closest = frame 44 plane Cr, differing 0/1183744 (0.000%)
channel .g: closest = frame 44 plane Y,  differing 0/1183744 (0.000%)
channel .b: closest = frame 44 plane Cb, differing 0/1183744 (0.000%)
```

Then the full conversion, GPU output versus a CPU computation of the identical
integer transform applied to FFmpeg's decode:

```
atlas Y (full integer YCbCr->RGB->YCoCg-R): frame 44, differing 0/1183744
```

**The entire chain — bitstream → Pico ASIC → AHardwareBuffer → Vulkan sampler →
integer conversion → atlas — is bit-exact.** ADR-0029 Option B is empirically
supported on this device, and Option A (drift-tolerant `base_sourced` patches)
is not needed here.

**One finding that must go into the normative clause.** The driver's
`samplerYcbcrConversionComponents` is **not identity** on this device: the
sampled `.r/.g/.b` come back as **`(Cr, Y, Cb)`** (`externalFormat = 506`,
`suggestedYcbcrModel = 2`, `suggestedYcbcrRange = 1`, chroma offsets
`(1,1)` = midpoint). Assuming `(Y, Cb, Cr)` produced a *plausible-looking* image
that was wrong in 99.8 % of samples with a max delta of 176 — it did not crash,
it did not look obviously broken, it was simply wrong. A normative conversion
clause **must** consume the reported swizzle rather than assume identity, and
the conformance vectors must include a device whose swizzle is non-identity.
This is the single most likely way to ship a silently broken hybrid decoder.

---

## 4. The design

### 4.1 Shape

Layer 0 is an ordinary HEVC elementary stream at the enhancement layer's own
geometry (1088² per eye, CTB 64 so CTB boundaries coincide with the nxvc tile
grid). The ASIC decodes it. Its picture is a **patch source**, not a
prediction of the frame.

Per atlas tile position, per frame, a tile's pixels come from one of:

| source | flags | GPU cost | who decides |
|---|---|---|---|
| nothing (skip) | unchanged | **0** | encoder |
| base frame F, tile rect | `base_sourced` (bit 2) | **1.9 µs** | encoder |
| nxvc coded tile | — | ~41 µs | encoder |
| nxvc residual over the base-decoded tile | `base_sourced` + coded | ~41 µs | encoder |

The atlas update rule of ADR-0029 §2 is unchanged: a base-sourced patch is a
*coded* tile for the purposes of step 3 — it writes pixels, sets `C := I`,
`src_frame := N`, `gen := 0`, `valid := 1` — and additionally sets flags bit 2.
No change to the composition arithmetic, no new normative geometry.

Because the atlas stores the **coded sample domain at full tile extent**
(ADR-0029 §1, "the atlas pixel layout never depends on any per-tile choice"), a
base-sourced patch is indistinguishable from an nxvc-coded one once written.
That is the property that makes this cheap: there is no second code path in the
display warp, in the reference logic, or in conformance.

### 4.2 The GPU cost model, with numbers

Budget: **4.2 ms per displayed frame pair** (both eyes), ADR-0029.
Per eye, 17×17 = 289 tiles; both eyes, 578.

Cost of refreshing *every* tile of *both* eyes, each frame:

| via | per tile | 578 tiles |
|---|---|---|
| **base-sourced patches** | 1.9 µs | **1.10 ms** (measured p50: 0.88 ms) |
| nxvc coded tiles | 41 µs | 23.7 ms |
| old model's skip warp (not even a refresh) | 34 µs | 19.7 ms |

Two scenarios against the 4.2 ms budget. Both charge an *unmeasured* display
warp; see §7.

**A — nxvc-only atlas (ADR-0029 as written).**

| | per pair |
|---|---|
| Pass A entropy, 2 × ~1 ms | 2.0 ms |
| Pass B coded tiles, 2 × ~1 ms (≈39 tiles/eye) | 2.0 ms |
| atlas metadata advance, 2 × 0.05 ms (budgeted, unmeasured) | 0.1 ms |
| display warp (unmeasured) | ? |
| **total** | **4.1 ms + display warp** |

At budget with nothing to spare, and the other ~250 tiles per eye are *not
refreshed at all* — they hold whatever they last had. ADR-0029 Cheat 3 names
that cost explicitly: "periphery detail and periphery temporal fidelity".

**B — hybrid.**

| | per pair |
|---|---|
| base decode | **0 ms GPU** (1.4 ms/eye of ASIC, in parallel) |
| full-frame base refresh, 578 tiles | 1.10 ms |
| atlas metadata advance | 0.1 ms |
| display warp (unmeasured) | ? |
| **remaining for nxvc coded tiles** | **3.0 ms − display warp** |
| → coded tiles affordable at 41 µs | **~37 per eye** *(if display warp ≈ 1 ms)* |

So hybrid affords roughly the same number of nxvc coded tiles as scenario A —
**and additionally refreshes all 578 tiles from the base every frame.**

> **This is the whole argument.** Hybrid does not buy more coded tiles. It buys
> a *refresh floor* for 1.10 ms/pair that would cost 23.7 ms/pair in nxvc — and
> that floor is exactly what ADR-0029 Cheat 3 currently trades away.

The corollary is a demand on the encoder: the base layer is only worth its
1.10 ms if base-sourced patches are *better than the stale atlas content they
replace*. That is the unmeasured risk of §7.

### 4.3 Bitrate

Measured, at a full 90 Hz refresh of 1088² per eye:

| base | per eye | both eyes |
|---|---|---|
| `5m` | 4.70 Mbit/s | 9.4 Mbit/s |
| `10m` | 9.56 Mbit/s | 19.1 Mbit/s |

Against a WiVRn session at 50–100 Mbit total, a 10–19 Mbit/s base is 10–38 % of
budget. Two levers reduce it further, and both are free:

* **The base need not run at the display rate.** It is a refresh source, not a
  frame source; the atlas holds pixels between refreshes. A 45 Hz base halves
  the bitrate *and* halves ASIC occupancy, at the cost of tiles being at most
  22 ms stale before nxvc corrects them.
* **`split_bitrate()` must be told.** Its `width*height` weighting
  (`encoder_settings.cpp:59-97`) would give the base an equal share with the
  eye streams, which is wrong by roughly 5×. The base's share is a policy
  number and belongs in `encoder_settings`.

### 4.4 Tile-aligned refresh policy

The ASIC decodes a whole picture or nothing — HEVC has no partial decode, and
MediaCodec will not accept less than an access unit. So the ASIC cost per base
frame is **fixed** (1.40 ms of ASIC at 9.56 Mbit) and only the *GPU* cost varies
with how many tiles are copied. The policy question is therefore purely "which
tiles does the encoder mark `base_sourced`", and it is free to be aggressive.

Recommended v1 policy, in order of increasing ambition:

1. **Full refresh, every base frame.** 289 tiles/eye, 1.10 ms/pair. Simplest,
   and the budget affords it. Start here — it makes the base a strict addition
   and isolates the quality question.
2. **Striped refresh.** Refresh ⌈289/k⌉ tiles per base frame on a rotating
   schedule; every tile sees the base every k base frames. At k = 4 that is
   73 tiles/eye = 0.28 ms/pair. Pairs naturally with an intra-refresh base
   (§5.3) whose refresh wave sweeps the same stripes, so a stripe is copied
   on the frame its CTB column was just intra-coded.
3. **Eccentricity-weighted refresh.** The inverse of Cheat 3: the *periphery*
   is refreshed from the base (cheap, and where nxvc's coded-tile budget cannot
   reach), the fovea from nxvc coded tiles (accurate). This is the
   configuration the two mechanisms were shaped for, and it is the one worth
   measuring first for quality.

CTB alignment is what makes any of these clean: with `ctu=64` the HEVC
quantisation and deblocking boundaries land on nxvc tile boundaries, so a
copied tile rect never straddles a CTB and a base-sourced tile has no
seam that a neighbouring nxvc tile disagrees about.

### 4.5 Synchronisation between the two streams

One counter. The base AU carries the nxvc frame id (`frame_id16`, already on the
wire — `wivrn_packets.h`, `nxwarp_datagram`), and a per-tile `base_sourced`
patch names the base frame it comes from via the atlas's existing `src_frame`
field. No new sync primitive.

The ordering is far more forgiving than the old layered design, because of
ADR-0029 Cheat 1 (no whole-frame requirement):

* Enhancement tiles never wait for the base. They are applied to the atlas as
  they arrive, exactly as they are today.
* A base picture, when it arrives, is applied to the atlas as a batch of
  patches. If it arrives late, it is applied late — the atlas is persistent
  storage, and a patch is just as valid one frame later, because its `C` is
  advanced by the same rule as everything else.
* There is **no deadline coupling at all**, which is the single largest
  structural difference from `HYBRID.md` §2, where the enhancement layer had to
  be buffered against the base and a missing base forced `wgt = 0` across the
  whole frame.

The one hard rule: **a base-sourced patch may only be applied if the client
actually decoded that base picture**, and the encoder must know whether it did.
That is a per-base-frame acknowledgement, and the nxwarp transport already
carries per-frame acks (`set_frame_held`, `ref_confirm` — `nx-warp` @ 5516d82).

### 4.6 Base-layer loss

The old design's worst property disappears. Under `HYBRID.md` §4.3 a lost base
AU cost a whole frame and escalated to an IDR. Under the atlas:

1. A lost base picture means **no base-sourced patches this frame**. The atlas
   keeps the pixels it had; every tile stays `valid`; nothing is concealed and
   nothing is extrapolated.
2. nxvc refreshes the affected tiles instead, on its own schedule and at its
   own per-tile granularity — which is precisely ADR-0006's "acknowledged
   neighbourhood references, no IDR".
3. HEVC's own reference chain still breaks, so the *base* needs recovery. Three
   options, in preference order:
   * **Intra refresh** (`idr_handler`'s `refresh` frame type). NVENC supports it
     (`forceIntraRefreshWithFrameCnt`), x264 supports it, **VAAPI does not**
     (`video_encoder_va.cpp:273-292`). A refresh wave costs no visible IDR spike
     and pairs with striped refresh (§4.4 policy 2).
   * **Drop the base for a few frames** and let nxvc carry everything. The
     budget affords this: scenario A is the fallback, and it is a mode the
     decoder already runs.
   * IDR, last. Expensive and visible — but now merely a bitrate spike in a
     stream nothing is blocked on, not a lost frame.

The asymmetry `HYBRID.md` §4.3 demanded — base AUs as the *highest* FEC
priority class, above foveal tiles — is **no longer justified**. A lost base
costs a delayed refresh; a lost nxvc tile costs a stale tile the encoder is
tracking. Foveal tiles should keep priority.

---

## 5. Determinism, restated against ADR-0029

| link in the chain | guarantee | evidence |
|---|---|---|
| HEVC bitstream → decoded YCbCr | normative in the HEVC spec | **measured, 180/180 frames identical to FFmpeg** |
| decoded YCbCr → AHardwareBuffer | vendor-internal | opaque, but §3.3(b) shows the samples survive |
| AHB → Vulkan sampler (`RGB_IDENTITY`+`NEAREST`) | *not* guaranteed by Vulkan | **measured, 0/1183744 samples differ** |
| sampler output → atlas (integer YCbCr→RGB→YCoCg-R) | ours, must be normative | **measured, 0/1183744 samples differ** |
| encoder's shadow atlas | Option B: encoder runs a conforming HEVC decoder | supported by row 1 |

Conditions the normative clause must state, all of which this prototype
discovered the hard way:

1. `ycbcrModel = RGB_IDENTITY`, `ycbcrRange = ITU_FULL`, `chromaFilter =
   NEAREST`. Anything else puts a float colour conversion inside the normative
   path, which is exactly what `HYBRID.md` §2.1 warns is not bit-exact across
   vendors.
2. **The driver-reported `samplerYcbcrConversionComponents` swizzle is
   consumed, not assumed.** Measured non-identity on the Pico 4.
3. Chroma siting is taken from `suggestedXChromaOffset`/`suggestedYChromaOffset`
   (measured: midpoint/midpoint) and the 4:2:0 → tile-grid upsample is
   specified. The base is 4:2:0 and the atlas is not.
4. Conformance gains a hybrid vector whose base is a fixed HEVC AU committed to
   the repo, plus at least one device with a non-identity swizzle.

Note this is *stronger* than `HYBRID.md` §2.1 hoped for. That section
recommended pinning the conversion while conceding the sampler shortcut might
be tenable. The measurement says: pin it, and the pinned version costs nothing
and is exactly reproducible.

---

## 6. What building it costs

| # | work | where | size |
|---|---|---|---|
| 1 | set `vendor.qti-ext-dec-low-latency.enable` | `android_decoder.cpp:141` | **uncomment one line** — do this regardless |
| 2 | `num_streams` 4 → 6; base streams 4/5 aliasing `src_layer` 0/1 | `encoder_settings.h:87`, `.cpp:390-410` | small |
| 3 | base share in `split_bitrate` | `encoder_settings.cpp:59-97` | small |
| 4 | `video_stream_description` carries base geometry + codec for 4/5 | `wivrn_packets.h:1318-1349` | small, wire-compatible bump |
| 5 | client: `decoder_count` 4 → 6, and streams 4/5 route to nxwarp, not to a view | `stream.h:80-82`, `stream.cpp:1165-1200`, `stream_network.cpp:230-239` | **the real work** |
| 6 | nxwarp decoder consumes a base blit handle; atlas patch pass | `nxwarp_decoder.cpp`, new shader | medium; prototype exists in `hybrid-proto/` |
| 7 | encoder: base-sourced patch decision + shadow HEVC decode | `video_encoder_nxwarp.cpp`, nx-warp `ref/` | **the other real work** |
| 8 | `base_sourced` flag bit 2 semantics + normative conversion clause | nx-warp `docs/SYNTAX.md`, conformance | spec work |

Items 1–4 are hours. Item 5 is the client's fixed stream-role assumption, which
is the structural blocker identified in §1.3. Items 6–7 are the substance.

The prototype in `hybrid-proto/` already contains a working, verified version of
the hardest mechanical part of item 6: AHB import, ycbcr conversion setup,
tile-list dispatch, and the integer conversion, all bit-exact.

---

## 7. What was NOT measured

Stated plainly, because the recommendation depends on it.

1. **Quality. Nothing in this report measures whether a 5–10 Mbit base is a
   *good* patch source.** It measures that patches are cheap and exact. Whether
   a base-sourced tile beats the stale atlas tile it replaces — and by how much,
   and where in the visual field — is unmeasured and is the one thing that can
   still sink this. `hybrid/RESULTS.md`'s 68-point sweep answers a different
   question (whole-frame prediction quality, the design ADR-0022 rejected) and
   does not transfer.
2. **The display warp.** ADR-0029's display step is not built, so the "3.0 ms
   remaining" in §4.2 scenario B is 3.0 ms *minus an unknown*. Both scenarios
   are charged equally, so the comparison holds, but the absolute headroom does
   not until that is measured.
3. **The atlas metadata advance.** 0.05 ms/eye is ADR-0029's budget, not a
   measurement, and the ADR says so.
4. **Live session behaviour.** Everything here ran standalone. A real session
   has wivrn-server, the nxwarp compute decoder, the compositor and two HEVC
   decoders contending for the same Adreno 650 and the same memory bandwidth.
   The ASIC does not contend with the GPU for shader cores, but it does contend
   for bandwidth, and §3.2's p95 (1.59 ms against a 0.39 ms p50) already shows
   this dispatch is bandwidth-sensitive. **This needs a scheduled loop.**
5. **Thermals.** No sustained run. All measurements are ≤ 30 s.
6. **Other silicon.** One device, one driver. The swizzle finding of §3.3 is
   precisely the kind of thing that differs elsewhere, and the Quest 3's
   decoder is a different block.

---

## 8. Recommendation

**GO, scoped as follows.**

*Do now, independent of hybrid:* uncomment `android_decoder.cpp:141`. It is one
line and it is worth 0.55–1.59 ms of mean decode latency and roughly half the
p99 on the HEVC path that ships today.

*Do next, before any hybrid implementation:* measure item 7.1. Take the atlas
model's own fixture, and compare a base-sourced patch against the stale tile it
would replace, eccentricity-weighted. That is a `ref/`-side experiment needing
no client work, and it is the only remaining question whose answer could be
"no".

*Then build,* in this order: server streams 4/5 (§6 items 2–4), client routing
(item 5), the atlas patch pass (item 6, prototype already working), the encoder
decision (item 7). Start with policy 1 — full refresh, every frame — because it
makes the base a strict addition and isolates the quality question from the
scheduling question.

*Record against ADR-0029:* Cheat 7's preferred Option B is **empirically
validated on the Pico 4**; Option A is not needed on this device. Cheat 7's
latency figure should be corrected from "10–20 ms" to **2.76 ms mean, 5.63 ms
p99 at 9.6 Mbit with the vendor low-latency key**, and its claim that this is
"the compatibility and bulk-refresh path and not the low-latency one" is too
pessimistic for this silicon — at 2.76 ms the base is inside a 240 Hz frame
pair's budget, not outside it.

The strategic point is worth stating separately. ADR-0022 rejected hybrid
because, as a whole-frame prediction layer, it was dominated by plain HEVC on
both quality and latency. Under the atlas that comparison no longer applies:
the base is not competing with nxvc to code the picture, it is doing the one
job nxvc is worst at — **refreshing 578 tiles that are mostly unchanged, for
1.10 ms instead of 23.7 ms.** That is a different argument from the one
ADR-0022 refused, and the measurements support it.

---

## Appendix: reproducing

```sh
cd hybrid-proto
./gen-streams.sh 5m 10m 50m     # needs ffmpeg + libx265; ~4 min
./build.sh                      # NDK 29.0.14206865 -> /data/local/tmp
adb shell mkdir -p /data/local/tmp/nxhybrid
for r in 5m 10m 50m; do adb push streams/base_1088_$r.{hevc,idx,csd} /data/local/tmp/nxhybrid/; done
adb push build/atlas_patch.spv /data/local/tmp/nxhybrid/

./run-bench.sh                  # the §2.2 table -> results/*.json

# §3.2 tile sweep
adb shell '/data/local/tmp/nxahbvk --stream /data/local/tmp/nxhybrid/base_1088_10m.hevc ...  --sweep'
```

All CPU-side work runs under `chrt -i 0 taskset -c 0-7 nice -n 19` via
`nx-warp/scripts/cpu-discipline.sh`.

Files:

| path | what |
|---|---|
| `hybrid-proto/gen-streams.sh` | test streams, AU index, csd-0, conforming-decoder reference |
| `hybrid-proto/src/nxhevcbench.c` | MediaCodec latency / throughput / ByteBuffer dump |
| `hybrid-proto/src/nxahbvk.c` | AHB import + Vulkan atlas patch, timestamped |
| `hybrid-proto/src/atlas_patch.comp` | the integer NV12 → YCoCg-R atlas conversion |
| `hybrid-proto/build.sh`, `run-bench.sh` | build/push, sweep |
| `hybrid-proto/results/` | raw JSON and the dumped planes |
