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
with `NXWARP_BUILD_REF=ON -DNXWARP_BUILD_TRANSPORT=ON` is enough.

### The e2e test

`wivrn-nxwarp-e2e` additionally needs nx-warp's **Vulkan decoder**, which a server-only
nxvc does not carry, plus spdlog. Build nx-warp once with both halves:

```sh
cmake -S ../nx-warp -B build/nxwarp \
    -DCMAKE_INSTALL_PREFIX=$NXWARP_INSTALL -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNXWARP_BUILD_REF=ON -DNXWARP_BUILD_TRANSPORT=ON \
    -DNXWARP_BUILD_VK=ON -DNXWARP_VK_SUBDIRS="common;decoder" \
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
	"encoders": [
		{
			"encoder": "nxwarp",
			"codec": "nxwarp",
			"options": { "qp": "26" }
		}
	]
}
```

`"codec": "nxwarp"` selects the same encoder on its own; both keys together are simply
explicit. The full option table is in `docs/configuration.md`; the one you will actually
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
```

`nxv-dec` must be on `PATH` (or pass `--nxv-dec /path/to/nxv-dec`) for the byte-identity
check. Other flags: `--nxv-out`, `--decoded-out`, `--seed`.

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

Measured on RADV (RX 7900 XTX), 320x240, QP 26:

| run | datagrams | published | byte-identical | PSNR vs source |
|---|---|---|---|---|
| clean, 12 frames | 83, 0 lost | 11 | 11/11 frames | mean 36.94 dB, worst 36.62 |
| 5 % loss, 40 frames | 277, 12 lost (4.3 %) | 30 | 30/30 frames | mean 36.89 dB, worst 36.62 |

The loss run is the interesting one: ten frames are lost outright and the PSNR of the
survivors does not move. That is this backend's concealment — a frame with a hole is
dropped, never half-decoded — and the stream does not stall.

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

* **Fixed QP, no rate control.** nx-warp's `rc/` component is not wired. The `qp` option is
  the whole quality control; the bitrate controller's number is logged and ignored. A
  scene that gets busier gets bigger frames, not worse ones — and if a frame outgrows the
  tile grid the server logs "raise QP" and drops it.
* **Chunk mapping, not one tile per tile.** The CPU reference codec's C ABI reports a
  tile's length but not its offset, so the frame bitstream is cut into MTU-sized chunks
  laid on the tile grid in raster order. Everything else is real, and the bytes round-trip
  exactly, but per-tile independence is lost: a chunk that never arrives costs the whole
  frame rather than one tile. This is why the 5 % loss run drops ten frames out of forty
  instead of blurring ten tiles. It becomes the identity mapping when the Vulkan encoder
  lands behind `nxwarp_codec`, and neither end changes when it does.
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
