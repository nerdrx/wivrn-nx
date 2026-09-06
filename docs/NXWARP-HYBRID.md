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

**One finding that must go into the normative clause.** A `RGB_IDENTITY`
sampler over this buffer returns `.r/.g/.b` = **`(Cr, Y, Cb)`**, not
`(Y, Cb, Cr)` (`externalFormat = 506`, `suggestedYcbcrModel = 2`,
`suggestedYcbcrRange = 1`, chroma offsets `(1,1)` = midpoint). Assuming
`(Y, Cb, Cr)` produced a *plausible-looking* image that was wrong in 99.8 % of
samples with a max delta of 176 — it did not crash, it did not look obviously
broken, it was simply wrong.

Part II corrects the *reason*, which changes how the clause should be written.
This was first read here as a vendor quirk of the Adreno driver. It is not: the
same `(Cr, Y, Cb)` order was afterwards measured on **RADV** (Part II §12.1),
and it follows from the format itself — `G8_B8R8_2PLANE_420_UNORM` carries luma
in **G**, Cb in **B** and Cr in **R**, so a channel-identity sampler returns
them in that order on any conforming implementation. The rule is therefore not
"query the driver and hope"; it is **normative and knowable in advance**, and an
implementation that hardcodes `(Y, Cb, Cr)` is wrong everywhere rather than
wrong on some devices. The conformance vector still wants it pinned.

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
2. **The channel order is `(Cr, Y, Cb)`, from the format, not assumed
   identity.** Measured on the Pico 4 and on RADV; see Part II §9.2.
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

---

# Part II — Design: the base layer in atlas v1

Written after the gate passed ([`NXWARP-HYBRID-GATE.md`](NXWARP-HYBRID-GATE.md))
and the ADR owner took `base_sourced` into ADR-0029 / SYNTAX 13.12. This part
is the design; §12 says what is implemented on this branch and what is not.

Two documents are load-bearing here and neither is mine:
`nx-warp` `docs/adr/0029-atlas-reference.md` @ `0b162e5` (branch `atlas`) is
normative, and `docs/ATLAS-DECODER.md` @ `cf1df26` (branch `passw-bypass`) is
the decoder agent's. Where they leave something open I have priced it rather
than chosen it — see §11.

## 9. The finding that shapes everything: the conversion is the identity

Part I built the base→atlas kernel around an integer YCbCr→R'G'B'→YCoCg-R
transform, on the assumption that the atlas holds YCoCg-R because ADR-0012 and
ADR-0029 §1 say "the coded sample domain". **For a WiVRn NX stream that
assumption is wrong, and the real answer is much better.**

Reading the two ends of the actual pipeline:

* `server/compositor/shaders/foveation.comp:45-54` writes **BT.709 full-range**
  YCbCr — `Y = 0.2126R + 0.7152G + 0.0722B`, chroma biased by `+0.5`, with no
  16/235 or 16/240 scaling — into the `eG8B8R82Plane420Unorm` compositor image
  (`compositor.cpp:209`).
* `server/encoder/nxwarp_codec_vk.cpp:137-139` configures nxvc with
  `ci.chroma = 0` (4:2:0), `ci.bit_depth = 8`, and **leaves `color_transform`
  at its default `NXVC_CT_NONE`** — "planes are coded as given (YUV in, YUV
  out)", `nxvc.h:96`. `nxvc.h:101-103` says so explicitly: *"A YCbCr source
  (WiVRn's Linux capture path is already
  VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) is coded as-is with NXVC_CT_NONE."*

So **the nxvc coded sample domain, for the streams this fork actually produces,
is BT.709 full-range YCbCr 4:2:0 at 8 bits** — not YCoCg-R. YCoCg-R appears
only when the input is RGB (`NXVC_CS_RGB` requires `NXVC_CT_YCOCGR`), which
WiVRn never sends.

Three consequences, in increasing order of importance:

1. **The base patch kernel has no colour matrix.** If the HEVC base is encoded
   full-range BT.709, its decoded samples *are* the atlas's numbers. The kernel
   is a swizzle, an 8→16-bit widen, and a store. Part I §3.2's 1.9 µs/tile was
   measuring a harder kernel than the one that is needed.
2. **The normative conversion clause shrinks to a swizzle clause.** ADR-0029
   Cheat 7 Option B says the residual risk "is not the HEVC decode but the
   NV12-to-atlas conversion, which we therefore define as a normative integer
   transform". With `CT_NONE` that transform is the identity, and the only
   thing left to specify normatively is the channel order:
   `G8_B8R8_2PLANE_420_UNORM` carries luma in **G**, Cb in **B** and Cr in
   **R**, so a channel-identity sampler yields `.r/.g/.b` = `(Cr, Y, Cb)`.
   Measured identically on the Pico 4's Adreno 650 through an AHardwareBuffer
   external format and on RADV through the plain format (§12.1), so it is a
   property of the format and not of any driver. Assuming `(Y, Cb, Cr)` gives a
   plausible, wholly wrong picture. **The channel order is the whole clause.**
3. **It reopens the decoder agent's atlas-format question in our favour.**
   `ATLAS-DECODER.md`'s open question rules out an 8-bit UNORM atlas because
   "a 9-bit YCoCg-R chroma plane does not fit 8-bit UNORM". Under `CT_NONE`
   **there is no 9-bit plane** — chroma is 8-bit YCbCr — so R8_UNORM is
   available, and `bench/README.md`'s measurement that an integer storage image
   costs ~3× a UNORM one on this part is then a reason to take it. §11 prices
   all three.

**Requirement this places on the base encoder:** the HEVC base must be
full-range BT.709 (`video_full_range_flag = 1`, `colour_primaries = 1`,
`matrix_coeffs = 1`). WiVRn's Vulkan H.265 encoder already sets
`video_full_range_flag = 1` (`video_encoder_vulkan_h265.cpp:73`); x265 needs
`--range full --colorprim bt709 --colormatrix bt709`, and FFmpeg's VAAPI
encoder needs `-color_range pc`. If that is got wrong the base is 16/235 and
every base-sourced patch is washed out by a fixed offset — a failure that looks
like a gamma bug and is not one.

## 10. Stream and role layout

### 10.1 The constraint

This fork has no rect partition. `num_streams = 4` and the four slots are
`left, right, alpha, quad` (`encoder_settings.h:87-90`), geometry is derived
from the slot index, and `stream_idx` is the only demux key on the wire and the
only routing key on the client. Part I §1.3 has the detail.

### 10.2 The opening `nx-warp-stereo` creates

Branch `nx-warp-stereo` (`ca1e29c1`, merged into this branch) codes **both eyes
as one nxvc stereo frame on stream 0**: `res[0].eyes = 2`, `src_layer = 0`,
`src_layer_right = 1`, and **`res[1].enabled = false`** — "the right-eye stream
keeps its entry in the description; what it loses is its encoder".

**Stream 1's encoder slot is therefore already vacant whenever NX Warp is
running paired.** That is where the base layer goes.

### 10.3 The layout

| stream | when NX Warp is paired | `eyes` | `src_layer` |
|---|---|---|---|
| 0 | nxvc enhancement, both eyes, one stereo frame | 2 | 0 (+1 right) |
| **1** | **HEVC base, both eyes, one side-by-side picture** | **2** | **0 (+1 right)** |
| 2 | alpha, unchanged | 1 | 2 |
| 3 | quad, unchanged | 1 | own image |

This is deliberately *not* the "nxwarp on 0/1, base on 2/3" the task sketched:
2 and 3 are alpha and quad and taking them would break both. Putting the base
on 1 needs **no new stream index, no `num_streams` change, and no protocol
version bump for the stream count** — the description entry for stream 1 already
exists, and what changes is what it says about itself.

Consequences, each of which is a real decision:

* **The base layer requires the stereo pairing.** If the pair is not paired —
  a mixed pair, or a per-eye width that is not a multiple of 64 — stream 1 has
  the right eye's encoder and there is no slot. That is an acceptable
  precondition rather than a limitation: both features are pair-wide, both need
  the 64-multiple width, and the alternative (going to `num_streams = 6`) buys
  nothing that the vacated slot does not already give.
* **One pair-compose, shared.** `nxwarp_codec_vk.cpp` already builds a scratch
  image `eyes * width` wide and does one `vkCmdCopyImage` per eye per plane into
  its half, timed and reported through `compose_ms()`. The base encoder wants
  *exactly the same* side-by-side picture, so that scratch image and its copy
  should be produced once per frame by the compositor and consumed by both
  encoders, not built twice. This is the one piece of the server work that is a
  refactor rather than an addition.
* **CTB 64 lands on the tile grid for both eyes.** The pairing already refuses a
  per-eye width that is not a multiple of 64, so the side-by-side seam is on a
  64-boundary; with `ctu=64` every CTB boundary in the pair picture coincides
  with an nxvc tile boundary, in both eyes, with no per-eye offset correction.
* **`split_bitrate()` must be told.** It weights by `width*height`
  (`encoder_settings.cpp:59-97`), which would hand the pair-wide base an equal
  share with the pair-wide enhancement stream. The gate measured the base
  saturating at 3–5 Mbit/s per eye for head-rotation content and 10–15 Mbit/s
  for dense static UI, so the base's share is a policy number in that range,
  expressed as a `bitrate_multiplier` and not derived from area.

### 10.4 Making the client's role mapping data-driven

Today the client maps stream index to compositing role by position
(`stream.cpp`, `decoder_count = view_count + 2`). Stream 1 becoming a base layer
breaks that, and papering over it with a second positional rule would make the
next change worse. The change is to say the role on the wire.

**Correction to the sketch this section first carried.** It proposed adding the
field to `video_stream_description::item`. There is no such type:
`video_stream_description` is a FLAT struct with parallel per-stream arrays
(`std::array<video_codec, 4> codec`) and scalars whose geometry the client
derives. The change follows that shape instead, and the enum belongs to
namespace `wivrn` beside `video_codec`, not to `to_headset` — a mistake worth
recording because it is exactly the kind that compiles nowhere and wastes a
build:

```cpp
// common/wivrn_packets.h, namespace wivrn, beside video_codec
enum class stream_role : uint8_t {
    view  = 0,   // a view's picture; `paired_eyes` says how many it carries
    alpha = 1,
    quad  = 2,
    base  = 3,   // an atlas patch source; not composited on its own
};

// in video_stream_description
std::array<stream_role, 4> role = {view, view, alpha, quad};  // = the old rule
std::array<uint8_t, 4> serves_stream = {0xff, 0xff, 0xff, 0xff};
constexpr stream_role role_of(uint8_t stream_index) const;
```

The defaults reproduce the positional rule exactly, so a description built
without touching them behaves as it did before — which is the property that
made the client rewrite safe to do in one pass.

`stream_size()` becomes role-driven rather than a switch on the index, and a
`base` stream reports its **coded** size, which is pair-wide. That is a real
asymmetry with the view streams, which report per-eye, and it exists because
the nxvc stream carries its eye count in a header of its own
(`nxvc_vkd_stream_info::eyes`) and an HEVC SPS does not: it just states a
width, so the number the decoder is created with has to be the true one.

The client then routes by `role`, not by index: `view` streams go to the
defoveator as today, `base` streams go to the nxwarp decoder's atlas as patch
sources and are never presented on their own — never collected as a view blit
handle, never in the reprojection path, never counted in "views ready" — while
still getting a decoder and still sending feedback like any other stream.
`serves_stream` is what lets a base stream name the enhancement stream whose
atlas it fills, so the pairing is explicit rather than "stream 1 fills stream 0
because it is stream 1".

This is a protocol change and it needs the `protocol_version` hash to move. It
does so **on its own**: `serialization_traits` derives the hash from the type,
and feeds every enumerator name and value of an enum into it, so adding the
field and the enum moves the version with no manual revision bump. Client and
server ship together, as they already must for `video_codec::nxwarp`.

It is also the change that makes the *next* stream-shaped feature cheap, which
is worth the one-time cost.

### 10.5 Sync and loss, concretely

Frame identity is the existing `frame_id16`. A base picture carries the frame
number it was composed from; a base-sourced patch sets `src_frame` to it, and
the atlas's `C` advance (ADR-0029 step 1) then carries that tile forward from
that pose like any other. There is no second clock.

Because the atlas is persistent storage and ADR-0029 Cheat 1 removes the
whole-frame requirement, a late base picture is applied late rather than
dropped, and a lost one simply produces no patches that frame. The escalation
ladder is Part I §4.6 and it is unchanged by this layout.

## 11. The base patch import component

### 11.1 What is pinned and what is not

Pinned by `ATLAS-DECODER.md`, and what this component is written against:

* **Atlas pixels** follow `nxvw_ring_layout()`
  (`vk/decoder/inter/inter_layout.h:257`) for ONE slot: per plane, row stride
  `(planeW * eyes + 1) & ~1` u16 elements, planes concatenated, and **eye `e`
  begins at column `e * planeW[p]`**.
* **The per-tile table** is 64 B per entry, and its index is **eye-minor**:
  `n = row*cols + eye*cols_per_eye + col`. The doc flags this as
  "two eye conventions in the same feature, which is the kind of thing that gets
  confused once and then stays confused" — pixels are per-eye sub-pictures,
  the table is interleaved. This component addresses pixels by `(eye, x, y)` and
  the table by `n`, and takes both from the caller rather than deriving one from
  the other.
* **Table semantics** on a write-back (ADR-0029 step 3): `C := I`,
  `src_frame := N`, `gen := 0`, `static := 0`, `valid := 1`, `res_level := 0`,
  and here additionally **flags bit 2 `base_sourced`**. `C`'s rows 0–1 are
  Q10.21 and row 2 is Q2.29, so identity is `{1<<21,0,0, 0,1<<21,0, 0,0,1<<29}`.

**Not pinned:** the atlas image format. `ATLAS-DECODER.md`'s open questions say
it "needs pricing before it is chosen". The component therefore compiles three
storage variants from one source and the choice is a parameter, not a
commitment:

| variant | storage | note |
|---|---|---|
| `STORE=0` | SSBO, u16 packed two per uint | today's reference ring, byte-for-byte |
| `STORE=1` | `R16_UINT` storage image | sampleable by the client's display pass |
| `STORE=2` | `R8_UNORM` storage image | **available only because `CT_NONE` means 8-bit chroma** (§9.3) |

All three write the same logical layout, so their readbacks are byte-comparable
and the format decision can be made on cost alone.

### 11.2 The pricing, measured — and the answer is the SSBO

`hybrid-proto/src/test_base_patch.c --size 1088x1088 --bench 50`, on the part
that decides it. Byte-exactness against the CPU model passes on all three
variants at this geometry, on both devices, so these are three ways of writing
the identical bytes and the choice really is cost alone.

| variant | Adreno 650 (Pico 4) | per tile | RX 7900 XTX (RADV) | per tile |
|---|---|---|---|---|
| `STORE=0` SSBO u16 pairs | **1.205 ms** | **2.08 µs** | 0.152 ms | 0.26 µs |
| `STORE=1` R16_UINT image | 1.944 ms | 3.36 µs | **0.025 ms** | **0.04 µs** |
| `STORE=2` R8_UNORM image | 2.065 ms | 3.57 µs | 0.025 ms | 0.04 µs |

578 patches — every tile of both eyes at 1088² per eye — 50 iterations, three
runs agreeing to 0.4 %, on an idle headset with no session running.

**Take the SSBO.** Three things follow, and the first is the one that matters
most:

1. **The ordering is inverted between the two parts.** The storage images beat
   the SSBO by 6× on the desktop GPU and lose to it by 1.6× on the Adreno. This
   question therefore cannot be settled on a host, and any host measurement of
   it — including `bench/README.md`'s "an integer storage image costs ~3× a
   UNORM one on this part", which is a desktop number — is evidence about the
   wrong device. The atlas lives on the headset.
2. **R8_UNORM buys nothing here.** §9.3 argued that `CT_NONE` makes an 8-bit
   atlas *legal* by removing the 9-bit YCoCg-R chroma plane, and that is still
   true and still worth having as a footprint argument — half the atlas memory
   and half its bandwidth. But it is the **slowest** of the three on the store
   path, so it must be justified on footprint and not on speed.
3. **The full-frame refresh is 1.20 ms per pair, not the 1.07 ms projected.**
   The gate's cost table (`NXWARP-HYBRID-GATE.md` §4) derived that figure from
   Part I §3.2's 1.9 µs/tile, measured on the *harder* kernel that still had a
   colour matrix in it. Measured directly on the identity kernel with the
   per-tile table write included, it is 2.08 µs/tile. The conclusion does not
   move — 1.20 ms/pair is 29 % of the 4.2 ms budget against nxvc-only's ~4.00 ms
   — but the number in that table should be read as 1.20, and the reason the
   simpler kernel is *dearer* than the harder one is not explained by anything
   measured here and is left standing as an oddity rather than rationalised.

**What this does not cover.** The base picture here is a plain
`G8_B8R8_2PLANE_420_UNORM` image, not an `AHardwareBuffer` imported under an
external format. The sampler configuration is identical, but an Adreno external
format may not sample at the same cost, so the *absolute* numbers could move.
The *ordering* is a property of the three store paths, which are the only thing
that differs between the variants, and that is what the decision rests on.

### 12. Status: what is implemented on this branch

| item | state |
|---|---|
| §9 colour finding | read out of both ends of the pipeline; the kernel is written to it |
| `base_patch.comp`, three storage variants | **written**, one source, `-DSTORE=0/1/2` |
| `base_patch_layout.h`: layout math + CPU model | **written** |
| Host byte-exactness test (RADV) | **written and PASSING**, §12.1, also at 1088² |
| Encoder-side FFmpeg shadow decode + test | **written and PASSING**, §12.2 |
| Device leg (Adreno) byte-exactness | **PASSING** on the Pico 4, §12.1 |
| Atlas format pricing | **measured, and it picks the SSBO**, §11.2 |
| §10.3 stream layout in `encoder_settings` | **implemented**: base on stream 1, opt-in, HEVC-only, CTB 64, full-range |
| §10.4 `stream_role` on the wire | **implemented** on both ends; hash moves on its own |
| Shared pair-compose refactor | **implemented** as `wivrn::pair_compose`, shared and composed once per frame |
| Base encoder fed the composed pair | **implemented** in `video_encoder::present_image`, waiting on the compositor timeline |
| Paired path under the e2e harness | **implemented**: `wivrn-nxwarp-e2e --eyes 2`, with a negative control |
| Encoder-side base-sourced patches into the encoder's atlas | **not implemented**: needs the encoder agent's call shape, §12.3 |
| Client-side atlas import of a base frame | **stub**: `handle_base_frame()`, waiting on `nxvc_vk_atlas_write_tiles` |
| §13 corpus capture | route identified and written as a runbook, §13.4; **needs a live-session slot** |

Two things the paired e2e leg is worth reading for, beyond "it passes".

**It has a negative control.** The right eye is the left one rotated half a
picture with its luma inverted (`255 - y == y` has no integer solution, so every
luma sample differs), and the two halves are scored separately. Filling layer 1
with the left eye instead drops the right-eye score from 41.24 dB to 12.59 dB
and fails the run. An assertion that cannot fail is not evidence, and this one
can.

**It found a real defect that was not mine.** On a paired encoder,
`nxvc_vk_encoder_set_view()` is refused — the library wants one view per eye,
because the two eyes have different poses and each gets its own warp record —
so `video_encoder_nxwarp`, which called the single-view form unconditionally,
gave a paired stream **no pose on either eye**. It was invisible for as long as
the paired path was only ever exercised all-intra, where the library documents
that it accepts and ignores views because there is no reference to warp. On a
paired inter stream it is not invisible and it does not fail loudly: the
predictor warps confidently from nothing. Fixed here by adding the pair form
(`nxwarp_codec::set_views`) and calling it with both eyes. This is a defect in
the `nx-warp-stereo` feature, not in the base layer, and nothing but the paired
harness would have found it.

**What the harness still does not prove.** No Vulkan validation layer is
installed on this machine, so the widened barriers in `pair_compose` are shown
to produce correct output on RADV but are not shown to be *formally* sufficient.
Installing `vulkan-validation-layers` and re-running `--eyes 2` is the cheap way
to close that, and it is a system package rather than something this branch can
do for itself.

### 12.3 The interface the encoder-side shadow decode needs

Option B has the encoder run a conforming HEVC decoder (FFmpeg) over its own
base bitstream, so that the encoder's model of the atlas contains the same
base-sourced patches the headset's does — without which the encoder cannot know
which tiles are already covered and the skip decision is guesswork. The decode
half exists and passes (§12.2). What does not exist is the call that turns a
decoded base picture into patches in the ENCODER's atlas, because that atlas
belongs to the encoder agent's component and its shape is theirs to state.

This is the interface this side needs. It is deliberately the mirror of the
decoder's `nxvc_vk_atlas_write_tiles(dec, eye, first_tile, count, src,
src_frame, flags)`, so that both ends describe a base patch the same way:

```c
/* Write `count` tiles of `eye`'s sub-picture, starting at the eye-minor linear
 * tile index `first_tile`, from a decoded base picture already in the atlas
 * layout, into the ENCODER's shadow atlas.
 *
 *   src         three 8-bit planes at the PER-EYE geometry, Y/Cb/Cr, in the
 *               coded sample domain -- which under CT_NONE is BT.709
 *               full-range YCbCr 4:2:0, i.e. the identity (see 9). Host
 *               memory: this is FFmpeg's output, not a VkImage.
 *   src_stride  per plane, in samples.
 *   src_frame   the frame number the base picture was composed from; it goes
 *               into each tile's record verbatim, exactly as 11.1 says.
 *   flags       must include the base_sourced bit (SYNTAX 13.12.1 bit 2), and
 *               the encoder must be able to read it back per tile -- knowing a
 *               tile is base-sourced is the point of the shadow.
 *
 * Returns the number of tiles actually written, so a partial write is visible
 * rather than silent. */
int nxvc_enc_atlas_write_tiles(nxvc_encoder * enc,
                               uint32_t eye,
                               uint32_t first_tile,
                               uint32_t count,
                               const uint8_t * const src[3],
                               const int src_stride[3],
                               uint32_t src_frame,
                               uint32_t flags);
```

Three questions for the encoder agent that this side cannot answer, and which
the call shape depends on:

1. **Host or device?** The decoder's form takes device memory because its
   pixels arrive from a hardware decoder. FFmpeg's arrive in host memory, so a
   device-side form would make this side stage them itself, which is only worth
   doing if the encoder's atlas is a GPU resource. Which is it?
2. **Is `count` over a contiguous run, or is a tile list wanted?** A full-frame
   base refresh — which §7 of the gate recommends, since the gate passes 100 %
   of tiles — is one contiguous run per eye, so the run form is enough for the
   policy actually proposed. A tile list only becomes necessary if the refresh
   goes partial.
3. **Does the encoder's skip decision read `base_sourced` back, or does the
   caller keep its own record?** This side can keep the record, but if the
   encoder's own tile state already has a flags byte then storing it twice is
   how the two drift apart.

Until this lands, the encoder computes its skip decisions with no knowledge of
the base layer, which is safe (it codes tiles the base already covers, so the
picture is right and the bitrate is wasted) but throws away the whole saving the
gate measured.

### 12.1 The host byte-exactness test

`hybrid-proto/src/test_base_patch.c`, against the CPU model in
`base_patch_layout.h`. The base picture is a real
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image — the compositor's own format —
sampled through the same `RGB_IDENTITY` / `NEAREST` / full-range conversion the
Android path uses on the imported AHardwareBuffer. So everything except the AHB
import itself is exercised, on any desktop GPU, with no headset.

```
device: AMD Radeon RX 7900 XTX (RADV NAVI31)
atlas: 147456 u16 (3 planes), image 512x384, grid 4x3 per eye, cols 8
  STORE=0  pixels ok (0/147456 differ)   table ok
  STORE=1  pixels ok (0/147456 differ)   table ok
  STORE=2  pixels ok (0/147456 differ)   table ok
  swizzle is consumed: ok
PASS
```

Four checks, and the last two are the ones that catch real bugs: all three
variants reproduce the model bit for bit; all three agree with each other,
which is what makes the atlas format a free choice; the per-tile table matches
across **all 64 bytes**, including the 20 reserved bytes a v1 decoder must zero
and conformance still compares; and a deliberately permuted channel order must
change the output, so a kernel that ignored it could not pass. That last check
is what found the correction in §9.2 — the first run failed with 146880 of
147456 samples differing and `got 90 want 0`, which is Cr where Y belonged.

### 12.2 The shadow decode test

`hybrid-proto/src/test_base_shadow.cpp` feeds the same access units the device
harness fed the Pico and compares against FFmpeg's own decode:

```
== full-range base (correct configuration) ==
shadow decode: 24 frames, 0 mismatched, 0 out of order
colour check : ok (full range, BT.709, yuv420p)
PASS
== limited-range base (the mistake the guard exists for) ==
shadow decode: 24 frames, 0 mismatched, 0 out of order
colour check : range is not full (pc/JPEG);
PASS
```

Two properties beyond "the pixels match". **Order**: the decoder is opened
`thread_count = 1` with `AV_CODEC_FLAG_LOW_DELAY`, because a frame-threaded
decoder buffers several pictures before emitting the first, and that reordering
would put the shadow behind the encoder's own reference tracking. The test
asserts one-in-one-out on every AU. **Colour**: `check_colour()` is the guard
for §9's requirement, and the run above shows it firing on a limited-range base
— the failure that otherwise looks like a gamma bug and is not one.

## 13. The corpus: materialising real captures

The gate ([`NXWARP-HYBRID-GATE.md`](NXWARP-HYBRID-GATE.md) §6.1) named this as
the follow-up that could still move its answer off 100 %, so it is worth being
precise about the route.

### 13.1 The mirror is the wrong tool

`docs/configuration.md` §`mirror` publishes the headset view as a PipeWire
`Video/Source` node, and it is tempting because it needs no code. It is not
usable for corpus material, for four independent reasons:

* **Left eye only.** `mirror.comp` resamples *one* eye
  (`pipewire_mirror.cpp`, `src_extent` from a single view).
* **Pre-foveation, and resampled.** The docs say so explicitly: frames are
  taken "after the layers have been composited but before foveation and before
  encoding", then scaled by `scale` (default 0.5). `corpus/README.md` requires
  the *opposite* — a specific configuration in which the foveation pass
  degenerates to an identity, so that what is recorded is what the encoder sees.
* **Wrong colour and format.** It publishes `SPA_VIDEO_FORMAT_RGBx` from an
  `eR8G8B8A8Unorm` image; the corpus wants `yuv420p`/`yuv444p` in the coded
  domain. Converting back would introduce a rounding step that is exactly what
  §9 spends its effort removing.
* **No pose.** Every `wivrn-capture` entry declares a `pose_log`, and the
  mirror node carries no head pose at all. Nothing downstream of it could
  reconstruct one.

### 13.2 The route that already exists

`corpus/README.md` and `tools/quality/README.md` §1c already specify it, and
this fork already has both halves:

* `WIVRN_DUMP_VIDEO` is implemented at `server/encoder/video_encoder.cpp:299`
  and writes one file per stream, `<prefix>-<stream_idx>.<ext>`;
* the `raw` encoder (`server/encoder/video_encoder_raw.cpp`) makes that `.yuv`
  — the encoder's actual input, post-foveation and post-colour-conversion;
* `tools/quality/capture/wivrn_capture.py` has `convert` (to the corpus's
  pixel formats) and `poses` (from `timings.csv` + `head.csv`).

So the work is a session, not code:

```sh
WIVRN_DUMP_VIDEO=/path/dump wivrn-server        # with encoder: raw
python3 tools/quality/capture/wivrn_capture.py convert --in dump-0.yuv \
    --w 1440 --h 1440 --out $NXW_CORPUS --name wivrn-vrchat-1440 --pix yuv420p,yuv444p
python3 tools/quality/capture/wivrn_capture.py poses --timings timings.csv \
    --head head.csv --out $NXW_CORPUS/wivrn-vrchat-1440.poses.json
python3 corpus/fetch.py --record --only wivrn-vrchat-1440
```

**Three cautions specific to this branch**, none of which is in the existing
docs because they predate the stereo pairing:

1. **Capture with `stereo-frame` off.** With the pairing on, stream 0 is the
   eye *pair* and a `raw` dump of it is `2*width x height` side by side, not the
   `1440x1440` per-eye picture the manifest declares. Either turn the pairing
   off for the capture or split the dump; turning it off is safer, because the
   manifest's `resolution` field is what every downstream tool trusts.
2. **The identity-foveation configuration is mandatory**, and it is easy to get
   silently wrong: stream extent ≥ render extent, `render_scale` 1.0,
   `foveation_strength` 0, adaptive foveation off, FSR1 off, motion smoothing
   off. `corpus/README.md` is blunt that frames captured without it "are not
   comparable with anything".
3. **`raw` needs the bitrate for it.** An uncompressed 1440² 4:2:0 stream at
   90 fps is ~280 MB/s; this is a local-loopback or short-run exercise, not
   something to attempt over Wi-Fi.

These are private recordings of someone's session: never committed, never
published, never attached to a bug report.

### 13.3 What is needed

**A live-session slot.** Recording is a headset session with a specific
configuration, which is the coordinator's to schedule. The three sequences
`corpus/MANIFEST.json` declares with `"frames": 0` are `wivrn-vrchat-1440`
(social VR, the bitrate floor), `wivrn-beatsaber-1440` (sustained fast head
motion, the best case for pose warp) and `wivrn-alyx-1440` (near-field hands at
large parallax). For re-running the patch-source gate, **`wivrn-vrchat-1440` is
the one that matters**: it is the content class expected to set the bitrate
floor, and therefore the one most likely to push a base tile below the 35 dB
gate boundary the gate found.

### 13.4 The capture procedure, exactly

Runnable as written. Derived from `tools/quality/capture/wivrn_capture.py plan`
and `tools/quality/README.md` §1c, which are the authority; the additions here
are the three things that document does not know about this branch (the eye
pairing, the `stream_scale` cap, and the arithmetic for a take that fits).

A person must be in the headset: there is no headless or fake-driver mode
(`XRT_BUILD_DRIVER_SIMULATED OFF` in `server/CMakeLists.txt`), the compositor
path only runs with a real client connected and an OpenXR app submitting
layers, and the content has to be played.

**Step 1 — server configuration.** `$HOME/.config/wivrn/config.json` (or
`$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json` for the
flatpak). Back up the existing file first; this is a capture configuration and
is not what anyone wants to stream with.

```json
{
    "encoder": "raw",
    "stream_scale": 1.0,
    "foveation_strength": 0,
    "foveation_adaptive": false,
    "mirror": false,
    "bit-depth": 8
}
```

* `"encoder": "raw"` is the uncompressed encoder (`video_encoder_raw.cpp`,
  8-bit only). It is what makes the dump a frame rather than a bitstream.
* **`"encoder": "raw"` also settles the eye pairing**, and this is the caution
  the upstream recipe cannot contain: `encoder_settings.cpp`'s `stereo-frame`
  pairs the eyes only when **both** eye streams are `video_codec::nxwarp`
  (`both_nxwarp`), so with the raw encoder the pairing declines on its own and
  the dump is per-eye. If you capture with an nxwarp encoder instead, stream 0
  is the **pair**, `dump-0.yuv` is `2*width x height` side by side, and the
  `--w/--h` you pass to `convert` below would be wrong. With `raw` there is
  nothing to remember.
* `stream_scale 1.0` plus `render_scale 1.0` and `foveation_strength 0` is what
  degenerates the foveation LUT to an identity — `foveation::compute_params`
  emits a 1:1 LUT when the encode extent is at least the source view extent.
  Frames captured without this "are not comparable with anything"
  (`corpus/README.md`).

**Step 2 — headset-side toggles**, in the headset's own settings, not the
server config: `render_scale` 1.0, **FSR1 off**, **motion smoothing off**.
Motion smoothing synthesises frames (`motion_warp_commit`) and a synthesised
frame is not a render target; it must not enter a codec test.

**Step 3 — run the server with the three dump variables.**

```sh
S=/run/media/nerdrx/Lex/claude/nx-scratch/capture
mkdir -p "$S"
WIVRN_DUMP_VIDEO="$S/dump" \
WIVRN_DUMP_TIMINGS="$S/timings.csv" \
WIVRN_DUMP_HEAD="$S/head.csv" \
  wivrn-server
```

`WIVRN_DUMP=list` prints the device names the build can dump, if
`WIVRN_DUMP_HEAD` turns out to be the wrong suffix. This produces
`dump-0.yuv` and `dump-1.yuv` (left and right eye), each a sequence of **NV12**
frames at the encode resolution, plus the two CSVs.

**Step 4 — connect, run VRChat, play for the take.** Then stop the server so
the files are closed.

**Step 5 — expected sizes, so the take is chosen deliberately.** At 1440×1440
NV12 a frame is `1440*1440*3/2` = **3 110 400 B ≈ 3.11 MB**, so per eye at
90 fps:

| take | per eye | both eyes |
|---|---|---|
| 10 s | 2.8 GB | 5.6 GB |
| **30 s** | **8.4 GB** | **16.8 GB** |
| 60 s | 16.8 GB | 33.6 GB |

`nx-scratch` is on the Lex NVMe with ~366 GB free (it is *not* tmpfs), so any
of these fits. **30 s of one eye is enough to re-run the patch-source gate** —
that gate's whole evaluation window was 12 frames — and 60 s is what
PAPER 2.11 item 1 asks for if the recording is to serve the parity kill test
too. The frames also traverse the network at ~280 MB/s per eye, so this is a
loopback or short-run exercise.

**Step 6 — convert, with the resolution the server actually logged.** Do not
assume 1440; read it from the server's startup log (`stream 0 encodes WxH per
eye`) or from `ls -l dump-0.yuv` divided by `frames * 1.5`.

```sh
cd /run/media/nerdrx/Lex/claude/nx-warp/tools/quality
export NXW_CORPUS=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus
python3 capture/wivrn_capture.py convert --in "$S/dump-0.yuv" --w 1440 --h 1440 \
    --out "$NXW_CORPUS" --name wivrn-vrchat-1440 --pix yuv420p,yuv444p
python3 capture/wivrn_capture.py poses --timings "$S/timings.csv" \
    --head "$S/head.csv" --out "$NXW_CORPUS/wivrn-vrchat-1440.poses.json"
cd /run/media/nerdrx/Lex/claude/nx-warp
python3 corpus/fetch.py --record --only wivrn-vrchat-1440
```

`convert` de-interleaves NV12 into planar 4:2:0 and replicates chroma for a
4:4:4 copy. `poses` joins the frame-indexed timing CSV to the pose track by
timestamp — `head.csv` is sampled at its own rate, not one row per frame — and
warns when the nearest pose sample for a frame is more than a frame time away.
`fetch.py --record` writes the frame count and hashes into `MANIFEST.json`,
which is what turns the entry's `"frames": 0` into a real one.

**Step 7 — restore the configuration.** `raw` is uncompressed and
`foveation_strength 0` is not a streaming setting.

Independent confirmation of §9 worth noting: `tools/quality/README.md` §1c
states the dumped frame is "BT.709 **full range** with an sRGB transfer already
applied", reached from the WiVRn side without reference to this work. That is
the same conclusion §9 draws from `foveation.comp`, and it is why the base
layer must be encoded full-range.

**Then re-run the gate**, which needs no further capture work:

```sh
cd /run/media/nerdrx/Lex/claude/wivrn-nx-hybrid/hybrid-proto/gate
# point the fixtures at the new sequence and re-run mkbase2.sh / final.sh / table2.py
```

The number to watch is the gate pass fraction at QP 22. On the two synthetic
fixtures it was 100.0 % with the worst tile 1.9 dB clear of the boundary; the
gate bites near 35 dB, and VRChat is the content class expected to reach it.
