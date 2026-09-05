# NX Warp end to end

The `nx-warp-e2e` branch is where the server encoder and the headset decoder meet. This
file is the runbook: what to build, how to run a live session, what the in-process test
proves, and — the important half — what still does not work.

`docs/nxwarp.md` is the reference for the wire format and the codec integration. This file
assumes it.

---

## 1. What is new on this branch

Two things, on top of the merge of `nx-warp-server` and `nx-warp-client`.

**`view_info` is on the wire.** `to_headset::nxwarp_datagram` carries an optional
`view_info` — display time, per-eye pose and fov, the foveation runs, the alpha flag — on
the first datagram of each frame and no other, which is the rule
`video_stream_data_shard` already followed. The server fills it from the `view_info_t`
that `present_image` receives; the headset publishes the decoded frame with it instead of
a default-constructed one. Before this, NX Warp frames decoded but reprojected from a zero
pose, which on a headset means the picture does not track your head.

**`wivrn-nxwarp-e2e`.** The real `video_encoder_nxwarp` and the real `nxwarp_decoder` in
one process, joined by the real packet types over a link that drops datagrams. Section 4.

---

## 2. Building

### Server

```sh
cmake -S . -B build/server \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$TOOLS/local;$NXWARP_INSTALL" \
    -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_CLIENT=OFF \
    -DWIVRN_USE_NXWARP=ON \
    -DGIT_DESC="$(git describe --tags --always)" -DGIT_COMMIT="$(git rev-parse HEAD)"
cmake --build build/server -j4
```

`GIT_DESC`/`GIT_COMMIT` are only needed when the build directory sits **outside** the
source tree: `cmake -P` runs the version script with the build directory as its working
directory, and `git describe` there finds no repository and fails the build. An in-tree
`build/` needs neither.

`WIVRN_USE_NXWARP=ON` requires an `nxvc` package. For the server alone, an nx-warp built
with `NXWARP_BUILD_REF=ON -DNXWARP_BUILD_TRANSPORT=ON` is enough — but that gets you the
CPU reference codec only. The GPU encoder behind `"backend": "vk"` needs `encoder` in
`NXWARP_VK_SUBDIRS` as well; configure prints `NX Warp Vulkan encoder: ON` when it found
one, and the server builds and runs normally when it did not.

On a box with no system Vulkan SDK the encoder directory takes `NXVC_VK_HEADERS_DIR`
like the rest of `vk/` does.

### The e2e test

`wivrn-nxwarp-e2e` additionally needs nx-warp's **Vulkan decoder**, which a server-only
nxvc does not carry, plus spdlog. Build nx-warp once with both halves:

```sh
cmake -S ../nx-warp -B build/nxwarp \
    -DCMAKE_INSTALL_PREFIX=$NXWARP_INSTALL -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNXWARP_BUILD_REF=ON -DNXWARP_BUILD_TRANSPORT=ON \
    -DNXWARP_BUILD_VK=ON -DNXWARP_VK_SUBDIRS="common;encoder;decoder" \
    -DNXVC_VK_HEADERS_DIR=$TOOLS/local/include -DNXWARP_VK_REQUIRED=ON
cmake --build build/nxwarp -j4 --target install
```

`NXVC_VK_HEADERS_DIR` is not optional on a box without a system Vulkan SDK: when
nx-warp's configure finds no Vulkan headers it prints a warning, **skips the Vulkan
runtime and still succeeds**, and the install then leaves whatever `libnxvc_vk_decoder.a`
was already in the prefix. `NXWARP_VK_REQUIRED=ON` turns that skip into a configure
failure, which is what an install prefix built for a client wants.

Point the server build's `CMAKE_PREFIX_PATH` at that prefix and the target appears. When
it is missing, configure prints

```
	NX Warp e2e test: OFF (needs an nxvc with the Vulkan decoder, and spdlog)
```

and the rest of the server builds normally — this is not a configure error.

### Linux client

```sh
cmake -S . -B build/client \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$TOOLS/local" \
    -DWIVRN_BUILD_CLIENT=ON -DWIVRN_BUILD_SERVER=OFF \
    -DWIVRN_USE_NXWARP=ON \
    -DWIVRN_USE_SYSTEM_LIBKTX=OFF -DWIVRN_USE_SYSTEM_BOOST=OFF
cmake --build build/client -j4
```

`ktx` must be on `PATH`. The client builds its own nx-warp through `ExternalProject`, from
`../nx-warp`; `WIVRN_NXWARP_DIR` overrides that path.

**`WIVRN_USE_NXWARP=ON` is required on the client too.** Without it the client builds and
runs, but with no NX Warp decoder in it at all — it will simply never offer the codec.

### APK

```sh
JAVA_HOME=$JDK21 ANDROID_HOME=$ANDROID_SDK \
./gradlew assembleRelease \
    -Psuffix=.warp \
    -Pwivrn_version=nx-warp-e2e \
    -Pwivrn_app_name="WiVRn NX Warp" \
    --max-workers=2
```

NDK 29.0.14206865, JDK 21, `ktx` on `PATH`. `-Psuffix=.warp` gives applicationId
`org.meumeu.wivrn.nx.warp`, so this build installs **alongside** the everyday
`org.meumeu.wivrn.nx` rather than replacing it, and `-Pwivrn_app_name` labels it
distinctly in the headset's app list. Both matter: this is an experimental build and it
should not be the one you reach for by accident.

Installing it, **on your own** — nothing here has been installed on a headset:

```sh
adb install -r build/outputs/apk/release/wivrn-release.apk
```

Substitute the actual filename; unless a signing key is configured
(`signingKeyPassword` in `gradle.properties` plus `ks.keystore`) the release APK is
**unsigned** and `adb install` will refuse it. Sign it, or build the `oculus` variant with
your own key.

---

## 3. Running a live session

### Server

NX Warp is never chosen automatically — it runs on the CPU reference codec and would lose
to any hardware encoder on the machine. Ask for it in
`$XDG_CONFIG_HOME/wivrn/config.json`:

```json
{
	"encoder": { "encoder": "nxwarp", "codec": "nxwarp", "options": { "qp": "26" } }
}
```

The top-level key is the singular **`encoder`** in this fork (an object for every stream,
or an array with one entry per stream); a plural `encoders` key is silently ignored and the
server falls back to its default hardware encoder, which is exactly what a first live
session did. `"codec": "nxwarp"` selects the same encoder on its own; both keys together
are simply explicit. `wivrn-server -f <file>` takes the file directly, so the everyday
`config.json` need not change. The full option table is in `docs/configuration.md`; the one you will actually
reach for is `qp`, the **fixed** quantiser (0..63, default 28) — lower is better quality
and more bits. There is no rate control, so this is the only quality knob, and the
bitrate the controller computes is logged and ignored.

Then start the server as usual.

### Headset

**Settings → Advanced → "NX Warp codec"**. On, `nxwarp` moves to the front of the
headset's `supported_codecs`; off, it is removed. There is no protocol field for it — the
existing codec negotiation does the work, and a server with no NX Warp encoder just picks
the next codec in the list. It needs a new encode session, so it takes effect on
reconnect, not live.

### Checking it took

The server logs the stream geometry and the fixed QP on the first frame; the headset logs

```
nxwarp[0]: 320x240 on <device>, 5 x 4 tiles, 1166 bytes per tile
```

once the codec stream header arrives. If you see neither, the negotiation picked another
codec — check that **both** ends were built with `WIVRN_USE_NXWARP=ON`.

### A protocol break

Adding the codec to the enumeration changes WiVRn's protocol version hash, and so does the
new `view_info` field on `nxwarp_datagram`. **A client and a server from this branch talk
only to each other.** An older NX Warp client will not connect to this server, and vice
versa. Update both ends together.

---

## 4. The in-process test

```sh
ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=30:duration=0.5" \
       -pix_fmt yuv420p -f rawvideo src.yuv

wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --loss 0.05 --seed 7

# the same two runs on the GPU encoder
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --loss 0.05 --seed 7 --backend vk
```

`--backend ref|vk` picks the codec and `--qp N` the quantiser (default 26). Every
assertion below holds for both backends; the run prints the encoder's own measurement of
the interval around `codec->encode()` at the end, which is the number that decides
whether a backend can hold a frame budget.

`nxv-dec` must be on `PATH` (or pass `--nxv-dec /path/to/nxv-dec`) for the byte-identity
check. Other flags: `--nxv-out`, `--decoded-out`, `--seed`.

### Rate control

`--rc auto` turns on the encoder's rate controller and `--bitrate N` gives it a
whole-link ceiling in bit/s, exactly as the session's own controller does — the run calls
`video_encoder::set_bitrate`, so this stream's share and the FEC parity overhead are taken
out of it on the real path. `--bitrate2 N` moves the ceiling halfway through the run,
which is what WiVRn's automatic bitrate mode does mid-session and the only way to see the
controller follow rather than merely converge. `--min-qp` / `--max-qp` mirror the encoder
options of the same name so the assertions can tell "did not reach the ceiling" from
"reached the end of the band".

`--rc fixed` (the default here, and *not* the server's default) pins `--qp` for the whole
run: every assertion about the transport is written against frames that are the same size
every time, and a moving quantiser would make the loss pattern irreproducible.

With `--bitrate` set the run prints a per-frame trace of the applied QP against the byte
budget, then the tail average, and asserts that bytes per frame stay within 25% above the
budget, come in under it only at the bottom of the QP band, and that the quantiser settles
into a band of at most 2 QP. A representative run at 1088x1088, 90 Hz, GPU backend,
starting at QP 28 with the ceiling doubled at 2.5 s:

| window | QP | B/frame | budget | error |
| --- | --- | --- | --- | --- |
| 0.0–0.5 s | 28→42 | 22372 | 20833 | +7.4% |
| 0.5–2.5 s | 40–42 | 20301 | 20833 | −2.6% |
| 2.5–3.0 s | 41→22 | 34782 | 41667 | −16.5% |
| 3.0–5.0 s | 22–24 | 38655 | 41667 | −7.2% |

It reaches the first band in about 35 frames and the second in about 16, the difference
being the double step the controller takes while more than a factor of two out.

It drives the shipping `video_encoder_nxwarp` with a real `vk::Image` in the compositor's
two-plane 4:2:0 layout, and the shipping `nxwarp_decoder` adopting the same Vulkan device.
Every packet goes through WiVRn's real serializer and is read back out of the bytes, so a
field that does not serialize does not arrive. It asserts:

* frames decode, and a clean run publishes every frame presented but the one still in
  flight;
* the published `view_info` is **bit-identical** to the presented one, field by field, and
  is not a default that happens to compare equal;
* the decoder's band deadlines produce feedback at roughly frame rate and the encoder
  accepts all of it into its client shadow;
* the GPU decoder's output is **byte-identical** to `nxv-dec`'s over the same `.nxv`
  stream — captured through `nxwarp_host::on_frame_unit`, so it is the stream the decoder
  was actually fed, not a re-derivation;
* every published frame is within PSNR of its source, aligned through `view_info` rather
  than by position, because under loss a dropped frame simply never appears.

Measured on RADV (RX 7900 XTX), 320x240, QP 26, `--backend ref`:

| run | datagrams | published | byte-identical | PSNR vs source |
|---|---|---|---|---|
| clean, 12 frames | 83, 0 lost | 11 | 11/11 frames | mean 36.94 dB, worst 36.62 |
| 5 % loss, 40 frames | 277, 12 lost (4.3 %) | 30 | 30/30 frames | mean 36.89 dB, worst 36.62 |

The loss run is the interesting one: ten frames are lost outright and the PSNR of the
survivors does not move. That is this backend's concealment — a frame with a hole is
dropped, never half-decoded — and the stream does not stall.

The same two runs on `--backend vk` pass every assertion, including the byte-identity
one: 11/11 and 28/28 frames identical to `nxv-dec`. The GPU encoder's streams are larger
and about 2.5 dB worse at the same QP (36.54 dB against 39.03 on the clean run), which is
the intra-only trade described in section 5, not a defect.

**What it does not cover:** reordering. The harness calls `push_datagram` straight from the
encode loop, so packets arrive in send order. `nxwarp-loopback --loss` covers the
reassembly path under reordering.

---

## 5. What works, and what does not

Works, end to end, in process:

* the codec, the packetizer, the transport, the AEAD, the class-A FEC, the band deadlines,
  the feedback and the client shadow;
* pose, fov and foveation reaching the headset with their frame;
* concealment under loss.

Does **not** work yet, and none of it is a bug to be filed:

* **The GPU encoder is intra-only, and it costs bitrate.** `"backend": "vk"` implements
  the DC-plane intra half of the v1 bitstream: no inter, no directional intra, no
  chroma-from-luma, no 4x4 split, no custom tables. The reference backend has all of
  those on by default. Measured at 1088x1088 on an RX 7900 XTX, 12 frames of `testsrc2`,
  timed around `codec->encode()` inside `encode()`:

  | backend | QP | mean | worst | bytes/frame |
  |---|---|---|---|---|
  | `ref` | 26 | 973.41 ms | 1037.82 ms | 34302 |
  | `ref` | 30 | 937.06 ms | 1027.54 ms | 27720 |
  | `vk`  | 30, host planes | 14.07 ms | 17.23 ms | 50152 |
  | `vk`  | 30, image direct | **3.80 ms** | 6.84 ms | 50152 |

  About 250x faster for about 1.9x the bytes. That is the whole trade, and it is the
  right one at 937 ms a frame. The two `vk` rows are the same encoder over the same
  picture and produce the same file byte for byte; the difference is the readback the
  next item used to describe.

* ~~One readback the GPU path should not need.~~ **Done.** The `vk` backend reads the
  compositor's image on the GPU: E0 binds the two planes as `R8_UINT` / `R8G8_UINT`
  storage views and writes the tile-major planes E3 consumes, and no pixel of the
  picture touches host memory. What it took, all of it now in the tree:

  * `image_formats()` in `server/compositor/compositor.cpp` and in
    `server/compositor/quad_converter.cpp` names the two UINT formats as well as the
    `_UNORM` plane formats, so the plane views are legal ones for that image. The
    planar format stays last, because callers take it as `formats.back()`. RADV
    accepted both without complaint; nothing had to be dropped.
  * `video_encoder_nxwarp`'s `target_queue` is the graphics/compute family for this
    backend rather than the transfer one — `nxvc_vk_encoder` submits its passes on the
    queue it adopted, so a picture released to the transfer family would have to be
    acquired back for a copy nobody makes any more. It is chosen from the `backend`
    option before the base class is constructed, since `target_queue` is `const`.
  * `present_image` copies nothing for this backend and allocates no readback buffer
    per slot; the submission it makes exists only to carry the compositor semaphore to
    the fence `encode()` waits on. The image is safe to read until `encode()` returns,
    which the slot state machine in `video_encoder::present_image` is what guarantees.
  * `nxwarp_codec` grew `accepts_image()` / `encode_image()`, and nx-warp grew
    `nxvc_vk_encoder_encode_image()` behind it. `nxwarp_e2e`'s own source image is now
    created the way the compositor's is — mutable format, extended usage, storage
    usage, the same format list — so the in-process test exercises the shipping path
    rather than the fallback.

  Two host costs went with it, both inside nx-warp and both byte-neutral: the
  coefficient readback is no longer copied out of its host-cached mapping before the
  table-set choice reads it once, and that choice hoists its `log2` into a table built
  with the probability tables and runs tile-parallel on a small pool. 14.07 ms a frame
  became 3.80 ms, and the `.nxv` is identical to the byte.

  The image entry point is pinned the same way the plane one is:
  `tests/vk-encoder/api_acid.cmake` runs with `-DIMAGE=ON` as `vk.encoder.acid.api.image`
  and requires the stream out of a VkImage to be byte-identical to `nxv-enc`'s.

* **Fixed QP, no rate control.** nx-warp's `rc/` component is not wired. The `qp` option is
  the whole quality control; the bitrate controller's number is logged and ignored. A
  scene that gets busier gets bigger frames, not worse ones — and if a frame outgrows the
  tile grid the server logs "raise QP" and drops it.
* **Chunk mapping, not one tile per tile — still.** The frame bitstream is cut into
  MTU-sized chunks laid on the tile grid in raster order, so a chunk that never arrives
  costs the whole frame rather than one tile. This is why the 5 % loss run drops frames
  instead of blurring tiles.

  This was expected to fix itself when the Vulkan encoder landed, and it did not, though
  the blocker has moved. `nxvc_vke_tile` **does** report each tile's byte offset and
  length — that half is solved, and `nxwarp_codec_vk` has the numbers in hand. What is
  left is above the codec: `nxwarp_tile_desc` has nowhere to put them, `nxwarp_send_frame`
  slices by `t * chunk_bytes`, and the client's `nxwarp_reassemble` actively *rejects* a
  frame with a hole and requires every non-last chunk to be exactly `chunk_bytes`. Making
  it the identity mapping is a change to the packetizer and to the client's reassembly,
  and the client does change, contrary to what `nxwarp_packetize.h` promises.
* **No per-tile pose warp.** Every tile of a frame is reprojected from the frame's pose, as
  with every other codec. The per-tile warp of nx-warp PAPER 4.3 is the point of the codec
  and it is not connected.
* **Arrival-driven band deadlines.** A band is closed when a datagram for a later band or a
  later frame arrives, not on the runtime's predicted display time, because the decoder
  does not have one. A band the sender goes quiet on is only closed by the next frame.
* **No hybrid mode, no client-side reference ring**, and inter prediction across frames is
  untested from the headset side.
* **Never run on a headset.** Both ends build, the APK builds, and the two halves have now
  been connected in process — but no live server-to-headset session has been run. The first
  one may well find something this test cannot: real packet reordering, real jitter, a real
  display-time clock.
