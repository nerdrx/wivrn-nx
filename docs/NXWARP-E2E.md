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

### Where the headset's decode time goes

The HUD's decode line reports the two halves of the GPU decode:

```
decode 34.2 ms · GPU 30.7 (A 7.6 / B 23.1)
```

`B` there is an **envelope**, not a kernel. nxvc measures it from the end of Pass A to the
end of Pass B, so it contains the predictor dispatch and all three reconstruction
segments. Reading it as "the reconstruct kernel costs 23 ms" is the wrong conclusion, and
for a while it was the only one the line supported. The next line breaks it up:

```
B 23.1 = warp 3.1 · skip 19.4 · coded 4.2 · dir 1.6 · other 2.4 ms · tiles 241/39/9
```

  * **warp** — Pass W, the predictor dispatch.
  * **skip** — `WARP_SKIP` tiles. These run the normative integer pose warp themselves,
    and on a settled inter stream they are usually most of the envelope. A stream that
    codes almost nothing can still be expensive to decode, which is why a large share
    here means lowering the bitrate will not help.
  * **coded** — every other non-INTRA tile.
  * **dir** — INTRA tiles, on the directional-intra wavefront.
  * **other** — the pipeline drain between those dispatches. The four segments are
    timestamps taken around dispatches, so the gap between them belongs to none of them;
    it is what the segmentation itself costs. It is shown rather than folded into the
    parts, so the equation balances without any of the terms being wrong.

The tile counts are what make a zero segment legible: no tiles means there was nothing to
do, tiles with no time means the device cannot timestamp that dispatch.

All of it is measured around **eye pass 0**, the convention nxvc already uses for
`pass_w_ms`: a paired stream runs the segments once per eye and these cover the first.

The same numbers reach the dashboard's headset statistics card, carried by
`from_headset::nxwarp_decode_profile` — a packet of its own rather than another field on
`nxwarp_feedback`, because it is a two-second profile window and the feedback goes out
several times a frame. Nothing on the server acts on it; a lost one costs a stale card.
They appear in the `NxwarpStats` D-Bus property as the `client_pass_*` fields, with
`client_pass_b_other_ms` published for convenience and recomputed on the way in rather
than trusted.

The split needs an nxvc with `NXVC_VK_DECODER_PASSB_SEGMENTS`. Built against an older
prefix the client still compiles, reports `segments_known = false`, and both the HUD line
and the dashboard rows are absent rather than showing zeroes.

### Decoupled display

The reprojection pass used to be submitted with a wait on the decoder's semaphore, so it
sat behind the whole decode on the queue, and `render()` then blocked on the previous
submission's fence for as long as that took. Measured on a clean live pair: a 16.7 ms
decode per eye pair in front of a display pass costing one or two, a loop turning at
**43/s against an 11.1 ms refresh**, and a **94 ms** old pose on the panel.

With `decoupled_display` (on by default, and a headset setting) a frame is handed over
only once its decode has actually FINISHED, and the display pass carries no wait at all.
Every refresh warps and presents the newest complete frame with the freshest pose,
whether or not a decode is running.

It is not new code. A pool item with no semaphore already meant "the decode thread waits
its own fence and publishes a finished frame", which is the path the Pico's Adreno driver
forces on this client anyway — it advertises timeline semaphores and then refuses to
create one. The option chooses that path deliberately rather than only as a fallback, so
the better-tested of the two becomes the default.

What it costs is the one frame of CPU-side pipelining the semaphore bought. On Adreno
that is not a loss: every queue is the same hardware ring, so there was no GPU overlap to
give up. The win is that the display pass is no longer serialised behind the decode from
the CPU side, and the compositor gets a frame every refresh.

Pose age stays meaningful without any change: it is measured against the frame the
refresh actually chose, and under decoupling every frame in the buffer is a completed
one, so "the frame drawn" and "the latest complete frame" are the same statement.

`client/utils/frame_ring.h` is the lock-free hand-over the render thread reads before it
takes any lock — a sequence-locked ring with an atomic latest-complete index, so the
render loop can know what is ready without waiting on the decoders. Its payload is
constrained to trivially copyable types on purpose: a seqlock copies a slot
speculatively and discards it if it was being written, and a `shared_ptr` copied
mid-write would touch a reference count before the check could reject it. Tested by
`tests/frame_ring_test.cpp`:

```sh
g++ -std=c++23 -O2 -Iclient -o frame_ring_test tests/frame_ring_test.cpp -pthread && ./frame_ring_test
```

which races a writer against a reader for millions of hand-overs and asserts no torn
frame, and is clean under `-fsanitize=thread`.

### A protocol break

Adding the codec to the enumeration changes WiVRn's protocol version hash, and so does the
new `view_info` field on `nxwarp_datagram`, and so does
`from_headset::nxwarp_decode_profile`. **A client and a server from this branch talk
only to each other.** An older NX Warp client will not connect to this server, and vice
versa. Update both ends together.

---

## 4. The in-process test

```sh
ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=30:duration=0.5" \
       -pix_fmt yuv420p -f rawvideo src.yuv

wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --loss 0.05 --seed 7

# reordering, and the 16-bit frame id wrap
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --reorder 0.05 --seed 7
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --first-frame 65500

# the per-tile span mapping, and what one lost datagram costs under it
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk --qp 32
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk --qp 32 \
    --drop-datagram 20

# the arrival-order atlas model, against the frame-complete one
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk --qp 32 \
    --tile-stream
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --backend vk --qp 32 \
    --tile-stream --reorder 0.3 --reorder-depth 8 --seed 7

# the same runs on the GPU encoder
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 40 --loss 0.05 --seed 7 --backend vk

# total blackout: the link delivers nothing at all
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk --loss 3
```

**Run each harness invocation in its own directory.** It writes `e2e.nxv` and
`e2e.ref.yuv` under those fixed names into the working directory, so two runs sharing one
directory overwrite each other's capture and the byte-identity check then compares a
picture against somebody else's stream — which shows up as a nonsense count like
"decoded every unit this decoder consumed (400/6)" rather than as a plain failure.

**`--tile-map auto|spans|chunks`** forces the encoder's mapping of frame bytes onto the
tile grid, which is the A/B for anything the per-tile span mapping is suspected of
costing: same clip, same QP, same frames, two mappings. Without it the mapping can only be
moved by moving the quantiser, and then the frames are not the same frames.

### What the span mapping cost the client, and where it was not

The first live stereo session on the Pico after spans went live showed the client's stage
line move from `submit 0.6–0.8` to `submit 7.0` ms, and the decode figure the server paces
to from 14.6 to 23.7 ms — with the GPU unchanged (`fence-post 12.9 = gpu 12.3 + copy 0.3 +
queue 0.1`). So ~7 ms of *host* time per frame.

`submit` is now split, because the two halves fail for opposite reasons and their sum
cannot tell them apart:

```
stage: ... | submit 0.6 (qlock 0.5 + codec 0.1) | ...
```

* `codec` is the time inside `nxvc_vk_decode_frame_ex`;
* `qlock` is the wait for the one Vulkan queue every submitter in the process shares.

Measured at 2176x1088 — a 34x17 grid of 578 tiles, the geometry of the Pico's stereo pair
— `codec` is **0.1 ms under both mappings**. The codec is not slower. A queue lock is not
a cost in itself either; it is a symptom of a thread not being scheduled, and the thread
in front of it is the network one.

Two lines were added there too, since it had no numbers of its own:

```
net: 0.011 ms per datagram over 7943 datagrams (44.24 ms of every second)
reassembly (network thread): 0.01 ms/frame over 125 frames, buffer 51 kB held for 51 kB of unit (1.0x)
```

That last ratio is what the span mapping actually broke. 200 frames, 2176x1088, QP 34:

| | chunks | spans, before | spans, after |
|---|---|---|---|
| transport tiles per frame | 45 | 578 | 578 |
| unit buffer reserved | 51 kB | **658 kB** | 51 kB |
| unit bytes held | 51 kB | 51 kB | 51 kB |
| ratio | 1.0x | **12.9x** | 1.0x |

The reassembler reserved `(highest + 1) * chunk_bytes` — the whole grid at one full
transport slot each. Under the chunk mapping a frame *was* a prefix of the grid, so that
was the frame's own size to within a slot; under spans the same 51 kB frame spans all 578
tiles. 658 kB allocated and freed per frame at the headset's frame rate is, on a
phone-class allocator, an mmap and an munmap per frame — and the cost of an munmap does
not land on the thread that paid it, it lands wherever the TLB shootdown catches.

Three other things there were O(grid) work that a 45-tile prefix had hidden:

* "are this frame's bytes whole" walked the grid on **every datagram, for every entry of
  the window** — 578 x 3 x every datagram. It is now a comparison of two running totals
  maintained where tiles are placed. Not measurable on a desktop (0.011 against 0.012 ms
  per datagram, inside the noise); it is 87k vector probes per frame the headset's little
  cores no longer make.
* three grid walks became one — `is_complete` scanned twice and `reassemble` called it and
  then scanned a third time before concatenating;
* the four-byte length prefix is skipped as the frame is copied rather than erased
  afterwards, which was a memmove of the whole unit, every frame.

None of it changes what comes out: byte-identical to `nxv-dec` over every published frame,
on both mappings, at 320x240, 960x544 and 2176x1088.

**What this does not claim.** The 7 ms is a Pico number and the desktop reproduces the
*shape* (a 12.9x buffer, an O(grid) walk per datagram) but not the magnitude — a 7900 XTX
with a fast allocator absorbs both. Whether the fix returns `submit` to under a
millisecond is for the live pair to say, and the `qlock`/`codec` split is what will make
that answerable in one line either way.

`--loss 3` is not a typo and not a probability that got away: the draw is
`uniform(0,1) < loss`, so anything at or above 1 drops **every** datagram. It is a case
worth a line of its own, because the server must survive a client that hears nothing —
keep coding, place nothing, invent nothing, and end — and because it is the shape of a
headset that walked out of radio range rather than one on a bad link.

The run asserts what a blackout should look like rather than what a lossy run should:
the encoder keeps coding, the transport places no tiles and marks none late, nothing
reassembles, nothing is published, and no feedback comes back — a receiver that hears
nothing has no frame id and no band structure to report *on*, so silence is the correct
answer and the encoder is required to make progress without it.

It used to report **six failures** on a working tree, which is the worst answer a test
can give: the code was right and six checks were asking a question the input had made
meaningless ("did the frames that arrived decode", "is the published pose real"), so a
real regression here would have been indistinguishable from the noise.

`--reorder P` holds back a P fraction of datagrams by one to three datagram slots: a
datagram held at slot `s` with delay `d` is released while slot `s+d+1` is handled, so
exactly `d` datagrams that were behind it on the wire go out in front of it. At the six or
seven datagrams per frame of a 320x240 stream that crosses the frame boundary whenever the
held datagram was near the end of its own frame, which is the case a one-frame reassembler
cannot survive. `--first-frame N` starts the frame counter at N; the encoder puts
`uint16_t(frame_id)` on the wire, so `--first-frame 65500` walks the stream through the
16-bit wrap — about twelve minutes into a 90 fps session.

`--pace`, `--client-decode-ms`, `--present-hz` and `--feedback-delay` are below, under
*Send pacing, and a slow client*.

`--backend ref|vk` picks the codec and `--qp N` the quantiser (default 26). Every
assertion below holds for both backends; the run prints the encoder's own measurement of
the interval around `codec->encode()` at the end, which is the number that decides
whether a backend can hold a frame budget.

`nxv-dec` must be on `PATH` (or pass `--nxv-dec /path/to/nxv-dec`) for the byte-identity
check. Other flags: `--nxv-out`, `--decoded-out`, `--seed`.

### The automatic bitrate

`--aimd N` runs WiVRn's own `bitrate_controller` inside the harness, with `N` as the
whole-link ceiling in bit/s, fed the `from_headset::feedback` this run's decoder actually
produces and applying its answer to the live encoder with `set_bitrate`. Encoder,
transport, real decoder, real delivery reports, real control law, one process.

It runs on a **virtual clock**, one frame period per presented frame. Every threshold in
that class is a duration — a two second window, a 250 ms evaluation interval, a five
second hold before each increase — and the harness encodes as fast as the GPU allows, so
on wall time the controller would see a whole session inside one window and decide
nothing. One frame period per frame is the cadence a headset produces, and it makes the
run repeatable.

With a clean link it asserts the ceiling is held and that no frame is reported
undelivered; with `--loss` it asserts the opposite — that the undelivered frames are
reported *and* that the controller backs off. Both directions matter: this path used to
report neither, and "the bitrate did not move" was the symptom.

### What a lossy run may and may not assert

"A lossy run drops the frames with holes rather than inventing them" is a statement about
frames that HAVE holes, and at a few percent loss the class-A parity often recovers every
one — which is the FEC succeeding, not the decoder failing. Asserting it unconditionally
made the verdict a property of the seed: over twelve seeds of `--backend ref --frames 12
--inter on --loss 0.05`, seeds 3 and 5 lost 9 and 4 datagrams, reassembled 12 of 12, and
were reported as failures.

So the invariant that always holds is asserted always — `published <= reassembled <= sent`,
at any loss rate, which is the whole of "invents nothing" — and the stronger claim runs
only when a frame actually failed to reassemble. Seven of those twelve seeds still reach
it, so the check did not become vacuous; it stopped being a coin toss. When the parity
covered everything the run says so in a note, rather than passing silently.

`tests/bitrate_nxwarp_test.cpp` is the other half, and the one that isolates the cause.
It drives the control law directly against two simulated eye decoders and separates the
two changes that had to be made — the frame numbering and the loss report — so each can
be shown to matter on its own. Build and run it the way its header comment says.

### Encoder effort

`--effort 0|1` is the encoder's `"effort"` option, and the harness defaults to **1** as the
server does: a harness that ran a configuration nobody ships would be measuring something
else. Level 1 is the integer requantiser — a coefficient quantised to ±1 whose squared error
is worth less than the bits it saves is dropped — and `--effort 0` is how a run reaches the
pre-effort bitstream, which is what makes the two comparable in one binary.

| run (320x240, `--backend vk --qp 32`, 12 frames) | B/frame | PSNR | decoder check |
| --- | --- | --- | --- |
| `--effort 0` | 5198 | 32.26 dB | byte-identical to `nxv-dec` on every published frame |
| `--effort 1` | **4804** (−7.6 %) | 31.35 dB | byte-identical to `nxv-dec` on every published frame |
| `--entropy lite --client-tools all --effort 0` | 6054 | 32.20 dB | byte-identical |
| `--entropy lite --client-tools all --effort 1` | **5386** (−11.0 %) | 31.34 dB | byte-identical |

Read the byte column, not the size of the `.nxv` the run writes: that file holds the units
the GPU decoder actually consumed, and the bounded worker queue decides how many those are.

**Both levels are the same bitstream.** The two streams above carry the identical tool mask
(`0x0000000006600045` from `nxv-info`), because the level leaves no tool bit — it changes
which levels are coded and nothing about how they decode. That is why nothing on the client
had to change for this and why the headset's `nxvc_tools` handshake advertises nothing new.

`"effort": "2"` is refused at startup rather than clamped, as nxvc refuses it: a wider motion
search measures −0.05 % BD-rate for +12 % encoder time, and the reference's own trellis RDOQ
cannot run on a GPU at all. On `--backend ref` the level reaches the non-directional path
only — the scope nxvc pins byte-identical against the GPU encoder — so with `intra-dir` on,
that backend's default, it changes nothing and the encoder logs a warning saying so.

### Send pacing, and a slow client

A desktop GPU decodes a 320x240 NX Warp frame in about a millisecond, so nothing in this
harness reproduced the thing that actually goes wrong on a Pico 4: a decoder that cannot
keep up, a bounded worker queue, a decode stride, and the flood of
`from_headset::nxwarp_frame_not_held` reports that follows. Four flags put that case in
reach, and all four are off by default.

| flag | |
| --- | --- |
| `--client-decode-ms N` | the shipping decoder waits N ms on its worker thread for every frame. Everything downstream of the wait — the stride, the queue bound, the not-held reports, the decode figure it sends the server — is the real thing reacting to a real cost |
| `--pace auto\|off\|N` | the encoder's `"pace"` option. **`off` here, unlike the server, whose default is `auto`**: the pace is a wall-clock decision, so leaving it on would make the frame count of every other test depend on how fast this machine encodes |
| `--present-hz N` | present composited frames at N Hz instead of as fast as the GPU allows. Pacing tests need it: a loop presenting a thousand frames a second makes the pace drop 97% of them and says nothing about a compositor at 90 Hz |
| `--feedback-delay N` | hold each not-held report back N presented frames. Zero is the harness's own behaviour — the report arrives in the same loop iteration that produced it, which no network does — and it is what hides the case the encoder's `last_resync_id` rule exists for |

Under pacing the harness counts **sent** frames rather than presented ones, since those are
no longer the same number, and it rebuilds the wire frame ids itself so that the check that
each published picture lines up with the frame id it carries still means something. It also
counts the resync notices on path `0xFE`, which is the direct measure of whether inter
prediction is engaging at all: bytes per frame depend on the clip, that number does not.

A representative pair, 320x240, `vk` backend, `--inter on`, 1800 frames at 90 Hz, a
simulated 31 ms client (37 ms as the client measures it — this harness reads the picture
back inside the measured window) and a three-frame control-socket delay:

```
--pace off    1800 sent at 90.0 fps, 1765 not held (1209 by the decode stride),
              434 all-intra resyncs (24.1% of sent frames), 31 frames published
--pace auto    460 sent at 23.0 fps,   11 not held (0 by the decode stride),
                3 all-intra resyncs (0.7%),                449 frames published
```

Both runs are byte-identical to `nxv-dec` over every published frame. The second sends a
quarter of the frames and shows 14 times as many of them.

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
  than by position, because under loss a dropped frame simply never appears;
* every frame presented reassembles whole when the link lost nothing, **however the
  datagrams were ordered** — judged on `nxwarp_host::on_frame_unit`, not on published
  frames, because a frame that reassembled and was then discarded as stale by the bounded
  worker queue is not a reassembly loss;
* frames reach the worker and are published in frame order;
* the transport's `tiles_late` is under 10 % of `tiles_placed` — tiles that arrived and
  were reported to the encoder as not having arrived;
* no frame is published with a default pose on a link that lost nothing (see below).

How many frames get *published* is deliberately **not** asserted: the worker keeps at most
`kMaxQueuedFrames` and discards the rest as stale ("late is worse than missing"), so the
count moves with the machine's load and the resolution. The run prints an accounting line
instead — presented, reassembled, published, discarded as stale — and the byte-identity
check aligns published pictures to `nxv-dec`'s by `feedback::frame_index` rather than by
position, so one frame dropped late no longer makes every later frame look different.

### What `--reorder` found

Measured on RADV (RX 7900 XTX), 40 frames, QP 26, `--backend ref`. "Reassembled" is frames
that came back whole out of 40 presented; "late tiles" is the transport's `tiles_late` over
`tiles_placed`.

| run | one-frame reassembler (before) | window of 3 (after) |
|---|---|---|
| clean, 320x240 | 40/40, 80.1 % late tiles | 40/40, **0.0 %** late tiles |
| `--reorder 0.05 --seed 7`, 320x240 | **37/40**, 80.0 % late | **40/40**, 0.0 % late |
| `--reorder 0.05 --seed 11`, 320x240 | **37/40**, 80.0 % late | **40/40**, 0.0 % late |
| `--reorder 0.15 --seed 7`, 320x240 | **28/40**, 79.7 % late | **40/40**, 0.0 % late |
| `--loss 0.05 --seed 7`, 320x240 | 29/40, 78.9 % late | **39/40**, 0.0 % late |
| `--reorder 0.05 --seed 7`, 960x544 | 40/40, 93.4 % late | 40/40, 0.0 % late |
| `--first-frame 65500 --reorder 0.05`, 960x544 | — | 40/40, 0.0 % late |

Every "after" run is byte-identical to `nxv-dec` over every frame it published, on both
`--backend ref` and `--backend vk`.

Three things fall out of this table.

* **Reordering across a frame boundary cost frames, and at 15 % it cost a third of them.**
  The window loses none, at any rate tried, at either resolution, and across the 16-bit
  frame id wrap.
* **The loss run improves too** (29 → 39 of 40), which is not the window: it is the band
  deadline fix. The old deadlines closed a band on its own first datagram, so FEC groups
  were closed before their parity could arrive and repairs that were available were never
  made.
* **80 to 93 % of tiles that arrived were being reported to the encoder as lost.** The
  harness reproduces, on a link with nothing wrong with it, what the live headset counters
  showed (13629 late of 13991 placed, 97 %). It is 0.0 % now. See `docs/nxwarp.md` 2.2.

The "before" column also fails the byte-identity check on a *clean* run, comparing only 20
of 40 published frames: the one-frame reassembler reopened every frame a second time on its
trailing parity datagram, so it stamped two `frame_index` values per frame. That defect was
invisible until the harness started aligning by index.

Earlier measurements, 320x240, QP 26, `--backend ref`, before any of this:

| run | datagrams | published | byte-identical | PSNR vs source |
|---|---|---|---|---|
| clean, 12 frames | 83, 0 lost | 11 | 11/11 frames | mean 36.94 dB, worst 36.62 |
| 5 % loss, 40 frames | 277, 12 lost (4.3 %) | 30 | 30/30 frames | mean 36.89 dB, worst 36.62 |

`--backend vk` passes every assertion too. The GPU encoder's streams are larger and about
2.5 dB worse at the same QP, which is the intra-only trade described in section 5, not a
defect.

**A case worth knowing:** under loss the harness sometimes prints `1 published frame had no
view_info`. The transport's FEC rebuilt the tiles of a lost first datagram, but `view_info`
rides the WiVRn packet around them, so the frame arrives whole with no pose and is published
with a default one. It cannot happen on a link that lost nothing, and the harness asserts
that.

**What it still does not cover:** real jitter and real sockets. `--reorder` permutes the
delivery order, but the harness still calls `push_datagram` synchronously from the encode
loop, so the datagrams of one frame arrive microseconds apart while whole frames are tens of
milliseconds apart. The clock half of the band deadline policy therefore only ever fires
between frames, never inside one — which the 0.0 % late-tile figure confirms it should not,
but a link with real jitter would exercise it properly and this does not.

---

### `--deterministic`: two runs, one file

`--nxv-out` is not reproducible by default, and the reason is not the encoder. The file is
built from the frame units the decoder **consumed**, and the decoder's worker keeps a backlog
one frame deep: a frame that arrives while the previous one is still on the GPU is dropped as
late. Whether that happens is a race between the network thread and the worker. Three
consecutive twelve-frame runs on one machine consumed nine, nine and eight frames and wrote
31242, 36381 and 46749 bytes — the same encoder, the same input, three different files. So
the flag exists because comparing two `.nxv` files was measuring the machine, not the code.

```sh
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 \
    --backend vk --qp 32 --deterministic --nxv-out a.nxv
wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 \
    --backend vk --qp 32 --deterministic --nxv-out b.nxv
md5sum a.nxv b.nxv        # identical
```

Three things change, and all three are wall clocks:

* **The decoder is drained after every frame.** The queue is empty before the next frame is
  pushed, so the backlog drop cannot fire and every unit that closes is decoded, in order.
  The wait is stated in the decoder's own counters — every closed frame decoded, dropped for
  a hole, refused by the codec or withheld — so a lossy leg, where a frame legitimately never
  decodes, settles rather than hanging.
* **The worker's backlog is unbounded** (`set_unbounded_queue`), which is what the drain needs
  to be sufficient rather than merely likely: several units can close inside one `send_stream`
  call when datagrams were held back, and the second would otherwise evict the first before
  the drain ever ran.
* **The clock counts frames.** `e2e_host::now()` returns the presented-frame index at a
  nominal 90 Hz instead of `steady_clock`. Every timestamp this host produces ends up back in
  the encoder — `publish()` stamps `blitted` and `displayed`, and those ride the feedback the
  controller and the client shadow are driven by.
* **The decode stride is pinned to 1** (`nxwarp_pin_decode_stride`). It is otherwise derived
  from measured decode time against measured arrival period, and a stride above 1 throws
  frames away.

What it refuses, rather than quietly ignoring — a flag that is read and does nothing is worse
than one that is rejected, because the run still writes a file and the file still looks like
an answer:

| refused | why |
|---|---|
| `--aimd` | the bitrate controller reacts to delivery timing |
| `--rc` other than `fixed` | the rate controller's window is a wall-clock window |
| `--pace` other than `off` | send admission is a `steady_clock` deadline inside the encoder |
| `--client-decode-ms` | a real sleep on the worker, whose purpose is to make timing paths fire |
| `--present-hz` | a compositor cadence measured against `steady_clock` |

`--loss`, `--reorder`, `--drop-datagram` and `--seed` stay legal: the link's `mt19937` is
seeded, so the drop pattern is already a function of `--seed` alone. Verified byte-identical
across two runs each at 320x240 and 1088x1088, on the reference and GPU backends, with inter,
with the stereo pair, with the pair *and* inter, under 5 % loss, under reordering, under
`--drop-datagram`, under `--tile-stream`, and across the 16-bit frame-id wrap.

**The trade is real.** A `--deterministic` run does not exercise the backlog drop, the stride
adaptation, the pacing or the automatic bitrate at all. It is for byte identity — "did this
change alter the bitstream" — and nothing else. Ordinary runs are where those paths are
tested, and they still are.

---

### Under the Vulkan validation layers

The harness links the shipping client decoder and the real encoder against one device, which
makes it the only place either Vulkan path can be checked without a headset. With
`vulkan-validation-layers` installed:

```sh
mkdir -p run && cd run          # the harness writes fixed filenames into cwd
ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=30:duration=0.5" \
       -pix_fmt yuv420p -f rawvideo src.yuv
VK_LOADER_LAYERS_ENABLE='*validation*' \
    wivrn-nxwarp-e2e --yuv src.yuv --width 320 --height 240 --frames 12 --backend vk
```

Two findings are expected and are **not** bugs:

* `VUID-VkImageMemoryBarrier-oldLayout-01212` and
  `VUID-vkCmdCopyImageToBuffer-srcImage-00186`, both on the image named `nxwarp image`.
  The decoder's pool images are created `SAMPLED | TRANSFER_DST` because that is all the
  headset needs; the harness reads one back with `vkCmdCopyImageToBuffer`, which wants
  `TRANSFER_SRC`. Adding that usage to satisfy a test would change what the shipping client
  allocates, so the harness carries the finding instead.

Everything else must be silent. What the device has to enable for that to be true is not
obvious, because the party that needs it is not the party that creates the device: nxvc's
decoder runs on the VkDevice it is handed, so its requirements land on
`server/utils/wivrn_vk_bundle.cpp` here and on `client/application.cpp` on the headset, and
both have to ask for the same three things.

* `shaderInt16` and `storageBuffer16BitAccess` — Pass A decodes into int16 coefficients and
  writes them to a storage buffer, so its SPIR-V declares both capabilities. Without them
  `vkCreateShaderModule` is undefined behaviour that happens to work
  (`VUID-VkShaderModuleCreateInfo-pCode-08740`,
  `VUID-RuntimeSpirv-storageBuffer16BitAccess-11161`).
* `samplerYcbcrConversion` — the decoder's pool is `G8_B8R8_2PLANE_420_UNORM` and every view
  of one carries a conversion object (`VUID-vkCreateSamplerYcbcrConversion-None-01648`).

All three are asked for only where the device offers them and reported when it does not: a
GPU without them can still run every other encoder, so it is a reason to refuse NX Warp and
not a reason to refuse to start.

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
* ~~Chunk mapping, not one tile per tile.~~ **Done on the GPU backend, per frame.** A
  codec tile's own bytes now go at its own transport tile index whenever the codec
  reports byte spans (`nxwarp_codec::reports_tile_spans`) *and* every tile fits a
  transport slot (`nxwarp_spans_fit`); the fixed-chunk mapping is the fallback for
  everything else, which today means the whole of the reference backend — its C ABI
  (`nxvc_tile_info`) has a length but no offset and cannot report one.

  It is decided **per frame**, not per session, because it is a property of the frame's
  tiles. A frame with one tile larger than the ~1162-byte slot takes the fallback whole
  rather than half. That is not a corner case: `testsrc2` at 320x240 QP 26 takes it on 7
  frames of 12, and at 960x544 QP 26 on all 20; at QP 32 both run entirely on spans.
  The run prints which:

  ```
  tile mapping: 20 frame(s) sent with per-tile spans (2700 coded tiles), 0 with the fixed-chunk fallback
    ok   every coded tile got exactly one transport slot (2700 on the wire, 2700 coded)
  ```

  That second line is the identity claim itself and is asserted whenever a run took one
  mapping throughout: as many transport slots on the wire as the codec coded tiles, one
  each, at its own index.

  The client's `nxwarp_reassemble` changed with it, and had to. Its two loss tests — no
  hole in the index run from 0, no short tile but the last — were the chunk mapping's way
  of noticing a loss, and under real spans both are *wrong*: a tile the frame did not code
  carries nothing and is indistinguishable, from the tile list alone, from one that was
  lost, and a coded tile is as long as its own content. The length prefix notices the same
  loss by better evidence — bytes that did not arrive make the total fall short of what the
  frame declared itself to be — and that test is exact under both mappings, which is why
  nothing on the wire says which mapping produced a frame. The prefix now rides the
  **lowest tile that carries bytes** rather than tile 0, since a frame that did not code
  tile 0 puts its leading bytes on the first tile it did.

  **What one lost datagram costs, measured.** `--drop-datagram N[:K]` takes the Nth *data*
  datagram off the link, together with the parity of whatever FEC group it was in — the
  parity goes not to make the loss worse (a parity datagram carries no tile) but to make it
  real, since class-A parity rebuilds a single lost datagram outright and the question here
  is what a loss the FEC did *not* cover costs. The claim is then checked as a **set**,
  against the headset's own receipt map as the encoder's `nxt::ClientShadow` holds it, per
  tile:

  | 20 frames, 960x544, QP 32 | spans (`vk`) | chunks (`ref`) |
  |---|---|---|
  | transport tiles offered | 2700 (135/frame) | 262 (13/frame) |
  | tiles the dropped datagram carried | 8 | 1 |
  | tiles placed | 2692 | 261 |
  | receipts lost outside that datagram | 0 | 0 |
  | tiles of that datagram reported held | 0 | 0 |

  Both halves are asserted and neither alone is the claim. "Nothing else" fails under a
  mapping where a datagram is a slice of the byte stream and the loss spreads; "exactly its
  tiles" fails on a run where the FEC quietly repaired everything. Under spans the eight
  tiles are 6 % of that frame's 135; under chunks the one "tile" is a thirteenth of the
  frame's *bytes* and the tile index it sits at names no codec tile at all, which is the
  difference the table cannot show and the receipt map exists to make usable.

  What has **not** changed is the client: it still waits for a whole frame, so both runs
  above lose one frame of twenty. Applying tiles as they arrive is step 4 of
  `docs/NXWARP-TILESTREAM.md` and waits on the decoder API.

* **The arrival-order model, and what it proves.** `--tile-stream` runs the atlas
  bookkeeping of SYNTAX 13.12 — `src_frame`, `gen`, and `C` as the ordered sequence of `H`
  steps folded into it — beside the real decode, over the tile runs the link actually
  delivered, twice: once applied **in arrival order** with the lazy advance and the
  `src_frame` monotonicity rule of `docs/NXWARP-TILESTREAM.md` section 2, and once applied
  **in frame order** with the eager advance, which is what a whole-frame decode call does.
  The proof obligation is that the two end in the same state, and that is what is asserted.

  It observes only. Every other figure a `--tile-stream` run prints is the figure the same
  run without it prints, including the byte-identity against `nxv-dec`.

  The interesting direction needs reordering deep enough to cross a frame at the *same tile
  position*, which the historical `--reorder` depth of three datagram slots never reaches —
  at six or seven datagrams a frame, a datagram has to fall a whole frame behind.
  `--reorder-depth D` is that, and 8 reaches it:

  | 40 frames, 320x240 | runs delivered | applied | superseded | atlases agree |
  |---|---|---|---|---|
  | `vk` QP 32, clean | 71 | 71 runs / 240 tiles | 0 | yes |
  | `vk` QP 32, `--reorder 0.3 --reorder-depth 8` | 237 | 221 runs / 764 tiles | 16 runs / 36 tiles | yes |
  | `vk` QP 32, same, across the 16-bit wrap | 237 | 226 runs / 772 tiles | 11 runs / 28 tiles | yes |
  | `ref`, same | 213 | 197 runs / 197 tiles | 16 runs / 16 tiles | yes |

  The superseded count is not reported, it is **asserted**: a run of frame `N` is superseded
  exactly when every position it covers has already had a tile of some frame `> N`
  delivered, which is computable from the link's own delivery order, and the model's count
  must equal it. The wrap row is what pins the unwrapping of the 16-bit frame id — compared
  as a raw `uint16_t`, every frame after the wrap looks older than the atlas and every tile
  is reported superseded.

  That the two paths agree *even where tiles were superseded* is the content of the proof
  rather than an exception to it: applying frame `N` then `N+1` at a position, and applying
  `N+1` and dropping `N`, both leave `src_frame = N+1` and `C = I` there, because a coded
  tile resets `C`. A superseded tile is not a loss, and this is why.

  What the model is fed is what the link handed the decoder, so tiles the FEC rebuilt
  inside the receiver do not appear in it. It is a statement about ORDER, and both paths see
  the same set.

  **Found by the deeper reordering, and left as it stands:** at `--reorder-depth 8` on a
  clean link the `ref` backend publishes three frames of forty with a default pose. This is
  the failure the note under *What `--reorder` found* describes for loss — `view_info` rides
  the WiVRn packet around the frame's first datagram and nothing else carries it — reached
  by a second input: a first datagram held back past its own frame's close is, to the
  reassembler, exactly as absent as one that was dropped. The run says so in a note, and
  the "no default pose on a clean link" assertion is gated on the reorder depth rather than
  quietly weakened.

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

## 6. Edge bleed

At a low application frame rate the headset's compositor reprojects the last frame it has to
the newest head pose. Where that frame's field of view runs out it has nothing to show, so a
black band sweeps in at the edge of the view — wider the later the frame and the faster the
head. It is the most legible symptom of lag there is, precisely because it is the one
artefact that is not part of the picture: everything else degrades, and this appears.

Two halves. Exactly one of them runs in any given session.

### Overscan — the real fix

The server widens the field of view it hands the application by a fraction of each side's
tangent (`edge_bleed.overscan`, default 5 %). The application renders it, the compositor
foveates it, the encoder encodes it, and the headset gets it back per frame in `view_info`
and hands it to its own compositor as the projection layer's field of view. So the ring
beyond the panel is **real decoded pixels** from end to end, and the reprojection moves into
picture instead of into nothing.

It acts at exactly one place, `wivrn_hmd::get_view_poses()`, where the runtime asks the
driver what field of view the views have. Nothing downstream knows the setting exists, which
is the reason it composes with everything: the squash, the foveation, the quad promotion, the
NX Warp stereo pairing.

**The trade, stated plainly.** The encoded size does not change — 1088x1088 per eye on a Pico
4, with or without — so the same pixels cover a wider angle and the picture is that much less
sharp. At 5 % that is about 4.5 % of the linear resolution, and about 9 % of the encoded area
is spent outside the panel, seen only during a reprojection. It is the same currency
`stream_scale` spends, and the two multiply.

**What it does not disturb.** The foveation remap and the nxvc tile grid are both derived from
the *encoded size* and a gaze direction, never from the field of view. A wider field of view
at 1088x1088 leaves the grid at 17x17 tiles, byte for byte, and moves the foveal rectangle to a
slightly different normalised coordinate — which is correct, because the gaze still points
where it pointed. Checked with `nxv-info` on a captured stream: same geometry, same tile count.

### Edge extension — the fallback

With `edge_bleed.overscan` at 0 there are no real pixels to move into, and the guarantee has to
come from somewhere else. The headset then widens **its own** projection layer by
`edge_bleed.overscan_fallback` and fills the invented ring out of the picture's own edge, in the
reprojection pass, at no resolution cost because it produces no new picture.

The ring is made out of the existing defoveation grid rather than out of a second draw: the
vertex shader moves every interior vertex inward by `1 / (1 + margin)` and leaves the vertices
already on the border where they are, so the outermost band of grid cells — and only that band —
is stretched across the margin. No new geometry, no extra pass, and the interior is a uniform
rescale of what it always was. A varying carries "how far into the ring am I", zero everywhere
else, so the fragment branch is coherent and the interior pays nothing.

Inside the ring:

  * `clamp` pulls the sample to the outermost texel of the side being extended, smoothstepped
    from the border outward so the join has a zero derivative and no visible crease;
  * `fade` does that and then, past `edge_bleed.fade_distance` of the ring, blends toward that
    edge's own averaged colour — three taps spread *along* the edge, so the result is a colour
    field rather than a mirror of whatever detail sat at one texel.

Both are per axis, so a corner does the right thing on both at once. The alpha stream that
carries passthrough transparency is never touched, so a transparent periphery stays transparent
instead of being painted over with a smear.

None of this is decoded. It is invented, and it is a smear if you go looking for it — but it is a
smear of the right colour, and it is only ever used where the alternative is black.

### Where the numbers live

`client/utils/view_geometry.h` is the one place the geometry is computed: the tangent widening,
the visible sub-rectangle of an overscanned image, the extension enum and its wire values. The
server includes it (the repository root is on `wivrn-common-base`'s public include path), the
client includes it, the dashboard includes it, and `tests/view_geometry_test.cpp` checks it.

It is shared on purpose. The **lens mask** skips tiles that fall entirely outside the visible
lens region — and the overscan margin is exactly the part of the image that is outside the panel
*and must not be skipped*, since being pulled into view later is the whole reason it exists.
`view_geometry::is_maskable()` is the question both features have to ask, and asking it in one
place is what keeps them from disagreeing by a tile.

### Settings

Dashboard → Settings → **Edge bleed**: an overscan margin slider (with a live readout of what the
margin costs in sharpness and encoded area) and an edge extension combo box. Server side they are
the `edge_bleed` object in `config.json`; `docs/configuration.md` has the full key reference.

The headset's HUD shows the margin in play on the first line, beside the pose age:

```
Shown 45 · decoded 45 fps · loop 90/s · period 11.1 ms · pose age 31.4 ms · bleed 5.0% encoded
```

or, when the headset is inventing the margin rather than decoding it:

```
... · pose age 31.4 ms · bleed 5.0% fade
```

Beside the pose age deliberately: the pose age is how late the frame on the panel is, and the
margin is how much of that lateness the picture can absorb before a band appears. Reading one
without the other is how you conclude the bleed is not working when the margin is simply smaller
than the head is fast.

### Proving it without a headset

`tests/edge_bleed_test.cpp` renders the **real** `client/shaders/reprojection.glsl` — compiled at
test time with `glslangValidator`, on a headless Vulkan device, with the same descriptor layout
and the same 192-byte push constant block the client uses — over a synthetic picture, with a
motion-field displacement large enough to push the sampling well outside the source. The target
is cleared to pure black, and the test asserts that after the pass **no pixel anywhere is the
clear colour**, in `clamp` and in `fade` and at a margin more than twice the default.

The "before" leg is the same render with the grid shrunk and the ring left undrawn, which is
exactly the situation the feature exists for: a layer wider than the picture with nothing filling
the margin. That leg is *required to fail* the same assertion — a proof that nothing can be black
is worth nothing if the arrangement could not produce black in the first place.

It also writes PNGs of each configuration, plus a 3x crop of the left border region, so the
result can be looked at rather than only asserted. Those are checked in under `docs/assets/`
and laid out side by side in [`docs/GALLERY.md`](GALLERY.md), which is the shortest way to
see what the setting does.

```sh
g++ -std=c++23 -O2 -I. -o edge_bleed_test tests/edge_bleed_test.cpp -lvulkan -lz
./edge_bleed_test client/shaders/reprojection.glsl <output dir>
```

`tests/view_geometry_test.cpp` is the arithmetic half and needs nothing but a compiler:

```sh
g++ -std=c++23 -O2 -I. -o view_geometry_test tests/view_geometry_test.cpp && ./view_geometry_test
```

It asserts the widening scales the *tangent* and not the angle (on a Pico 4's half field of view
those differ by degrees), that `visible_rect()` agrees with a hand re-derivation from the widened
angles, that an asymmetric field of view keeps asymmetric margins in pixels, and that the wire
values of the extension enum are 0/1/2 and are not free to change.

### A protocol break

`to_headset::video_stream_description` grew four fields, so this is another change to the
protocol version hash. A client and a server from this branch talk only to each other — which was
already true on `nx-warp-e2e`, and is now true for one more reason.

## 7. The atlas coding mode, wired

[SYN] 13.12's ATLAS mode (tool bit 31) replaces the reference: instead of the previous
decoded picture, each tile position holds the pixels of the most recent frame that CODED
it, plus the composed warp from this frame's pose back to that one. A skipped tile then
produces no reference pixels and touches nothing, which is where the decoder's whole skip
warp goes. ATLAS_REBASE (bit 34) puts a per-frame mode switch on top: each frame is either
an ATLAS frame or a PICTURE frame, decoded by the ordinary process against one coherent
picture assembled from the atlas, whose reconstruction then becomes the atlas.

This branch is the WiVRn side of it, wired so the headset build is ready the moment the
nxvc branches merge. Nothing here turns it on by default.

### The private prefix

Built from nxvc `origin/main` merged with `origin/atlas-encoder`. One conflict,
`vk/decoder/atlas/atlas_layout.h`, add/add, and not a divergence: the file was introduced
on each side by the same patch applied twice, so git has no common ancestor for the path.
Resolved to main's side, verified a strict superset -- all 96 lines of the encoder side's
version appear verbatim and in order in main's 280, none unique to the encoder side.

`ctest` on that prefix: **139/139 passed**, one skip (`python.pytest`, no numpy venv).
Atlas legs green on both ICDs -- `vk.atlas.compose`, `.state`, `.vs_ref`, `.gpu_vs_cpu`
(and `_radv`, `_lavapipe`), `ref.atlas`, `vk.encoder.atlas.acid.rans`/`.lite`/`.layout`/
`.mode`/`.host`, `vk.decoder.conformance` (+ lavapipe).

`nxvc_vk_decoder_tools_supported()` there is `0x00000005ff7a1fff`: bits 31 and 34 present,
**bit 35 (planar) absent**, as it will be until lowpoly-gpu lands. The client sends this
mask through unchanged, so nothing forces a tool the decoder has not got.

### The guards

The atlas ABI arrives in pieces, so every use of it is behind a feature test, and the
tests are decided in one place rather than rediscovered per site.

| macro | what it gates | how it is decided |
| --- | --- | --- |
| `NXVC_VK_DECODER_TOOLS_FOR` | `nxvc_vk_decoder_tools_for()` | upstream macro |
| `NXVC_VK_DECODER_ATLAS_STATS` | `frame_mode`, `picture_frames`, `atlas_entries_valid`, `tiles_assembled`, `tiles_warped_skip`, `tiles_identity_seg` | upstream macro |
| `WIVRN_NXVC_ATLAS_DECODE` | `nxvc_vk_decoder_set_atlas_view()`, `nxvc_vk_decoder_atlas_images()` | this tree's probe |
| `WIVRN_NXVC_ATLAS_ENCODE` | `nxvc_vke_create_info::atlas`, `::atlas_mode`, `::atlas_picture_d` | this tree's probe |
| `WIVRN_NXVC_ATLAS_STATS` | `nxvc_vkd_stats::tiles_superseded` | this tree's probe |

The upstream macros are used directly; a macro is the only thing that can be asked about
an appended struct field. The rest have no upstream feature test, so
`cmake/NxvcAtlasFeatures.cmake` compiles the smallest program that uses each one. That
works for the server, which builds against an installed prefix. It cannot work for the
client, which BUILDS nxvc -- at configure time its include directory is an empty directory
ExternalProject has not filled yet -- so `client/CMakeLists.txt` reads the same answer out
of the source header that is about to be installed. Same file, one copy earlier.

Configure prints what it found:

```
-- NX Warp atlas: decoder ON, encoder ON, stats ON, tools_for ON
-- NX Warp atlas (client): decoder ON, stats ON, tools_for ON
```

### Server

`"atlas": "off" | "auto"` and `"atlas-picture-threshold"`, reaching
`nxvc_vke_create_info::atlas`, `::atlas_mode` and `::atlas_picture_d`.

`"auto"` sets BOTH tool bits. There is deliberately no setting for bit 31 alone: leaving
the mode switch clear makes every frame an ATLAS frame, which is the configuration whose
reference goes stale under fast head motion, and shipping it as a reachable choice would
be offering a worse operating point that nothing outside the encoder could see had been
chosen.

The threshold is `D` from 13.12.11.1, in LUMA SAMPLES: a PICTURE frame is coded when the
worst corner displacement in the atlas, this frame's advance included, exceeds it. 0 asks
nxvc for its own default, which is 8.

Default off until it is measured on a headset. The atlas removes the 8.8 ms an eye that
skipped tiles cost in the picture model, but the figure on the nx-warp side is a desktop
measurement with a 1.57x caveat that has not been reproduced on an Adreno, and a default
that is faster on a workstation and slower on the target is worse than no default.

`"atlas"` with `"inter": false` is refused in `video_encoder_nxwarp.cpp` rather than at
nxvc's `create()`, so the message can name the two options the person wrote. Asking for it
against an nxvc without the encoder half is refused too, not silently ignored.

### Client

The advertised tool mask already came from `nxvc_vk_decoder_tools_for()`, so bits 31 and
34 flow through the existing handshake with no change at all.

The display path asks for the ATLAS display view (13.12.5) on an atlas stream. Under the
atlas the picture `nxvc_vk_decoder_images()` returns is DERIVED and explicitly not
normative -- conformance compares the atlas -- and what a display pass wants is a sampler
over the atlas, which nxvc produces beside it on request. R8 where the stream allows it:
NV12-shaped, one luma tap plus one chroma tap, 1.086 ms a pair on a Pico 4 against 2.124
for the three-plane R16 form. R8 is refused on a stream with a colour transform, because
under CT_YCOCGR the chroma planes carry an extra bit an 8-bit UNORM cannot hold, so the
transform is checked here rather than the refusal being caught.

**One thing a desktop cannot check.** The view's formats are `R8_UNORM` / `R8G8_UNORM`
where the derived picture is `r8ui` / `rg8ui`. They carry the same numbers, but a sampler
reading a UNORM returns them scaled to 0..1, so the display pass's shader has to agree
with whichever it is bound. That is the first thing to look at if the atlas path shows a
washed-out or blown-out picture on a headset.

### HUD

```
atlas: 3 PICTURE / 87 ATLAS frames (3% PICTURE) · warps/frame 144 · assembled 578 · identity 12
```

Absent entirely on a stream without the atlas -- a mode that is not in use has no rate to
report. Against a prefix with only `tiles_superseded` it degrades to the frame counts plus
that one number; against a prefix with neither it says

```
atlas: on, per-frame counters not in this nxvc
```

which is a different statement from a row of zeroes and has to look different.

### Dashboard

**Settings → NX Warp encoder → Atlas coding** (Off / Auto) and **PICTURE threshold**, the
latter shown only when the mode is Auto. Both erase their key when left at the value the
server applies for an absent option -- the threshold especially, because 0 means "the
codec's own default" and pinning it would freeze a number upstream is free to move.
`tests/dashboard_nxwarp_settings_test.cpp` covers the round trip, the erase-at-default and
the clamp: 251 checks.

### The legs, and what they do not prove

vrroom, 2176x1088 side by side (1088x1088 per eye, `--eyes 2`), QP 30, 32 frames, the
Vulkan backend, inter on, `--atlas off` against `--atlas auto`:

| clip / mode | B/frame | PSNR | stream tools | decoder check |
| --- | --- | --- | --- | --- |
| rest / off | 67920 | 34.33 dB | `0x…06600c45` | byte-identical to `nxv-dec` |
| rest / auto | 74456 | 39.54 dB | `0x…486600c45` | byte-identical to `nxv-dec` |
| mid / off | 68958 | 34.13 dB | `0x…06600c45` | byte-identical to `nxv-dec` |
| mid / auto | 74800 | 39.51 dB | `0x…486600c45` | byte-identical to `nxv-dec` |
| still / off | 65187 | 39.51 dB | `0x…06600c45` | byte-identical to `nxv-dec` |
| still / auto | 73998 | 39.51 dB | `0x…486600c45` | byte-identical to `nxv-dec` |

All six pass. The tool masks are the point of the table: `0x4_8000_0000` over the baseline
is bits 31 and 34, so the `auto` legs really are atlas streams and the `off` legs really
are not, and the GPU decoder reproduces `nxv-dec` byte for byte on both.

**The PICTURE share is 0 % on every leg, and that is not a measurement.** It is 0 by
construction: `nxv-info` shows one distinct pose across every frame of every capture,
because the harness has no pose track and writes a zero pose. The mode trigger is a
DISPLACEMENT threshold, so with no head motion it can never fire, and the ATLAS/PICTURE
switch -- the entire point of bit 34 -- is untested here.

The fixtures already carry the missing half: `rest.poses.json`, `mid.poses.json` and
`still.poses.json` sit beside the YUV, and `rest` / `mid` / `still` differ precisely in how
the head moves. What is needed is a `--poses` flag on `wivrn-nxwarp-e2e` to feed them into
`view_info`. Until then the A/B above measures the atlas as a REFERENCE MODEL and says
nothing about the mode switch.

Read the byte column from the run log, not from the `.nxv`: that file holds the units the
GPU decoder actually consumed, which the bounded worker queue decides -- 3 to 7 of the 32
frames here.

## Live session with nobody wearing the headset

The Pico keeps its display on and its compositor at 90 Hz with the proximity
sensor uncovered, and the WiVRn client streams in `XR_SESSION_STATE_VISIBLE`,
so a live nxwarp session needs no person in the headset:

1. Start the server on the desktop (`wivrn-server -f <config.json>`); the
   `active_runtime.json` symlink under `~/.config/openxr/1/` appears once a
   client connects, so point OpenXR apps at the build's manifest directly.
2. Connect the test client by intent (about 15 s after the app launches):
   `adb shell am start -a android.intent.action.VIEW -d wivrn://<server-ip> org.meumeu.wivrn.nx.warp`
3. Drive frames with `hello_xr`, keeping its stdin open (it exits on EOF):
   `tail -f /dev/null | XR_RUNTIME_JSON=<build>/openxr_wivrn-dev.json hello_xr -g Vulkan2`
4. Read `adb logcat -d | grep -E 'nxwarp\[[0-9]\]'` for the client's per-stream
   decode lines and the server log for the encode/pacing/not-held lines.

`pkill -x hello_xr` ends the session. Absolute numbers taken this way sit
above a worn session's (SLAM tracking keeps the GPU at 490 MHz and warm), so
compare pairs taken minutes apart, not against cold rows.
