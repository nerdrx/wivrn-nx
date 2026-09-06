# Configurable items

Configuration is done on server side.
Files are read from
- `/usr/share/wivrn/config.json` (where `/usr` is selected at configure time with `CMAKE_INSTALL_PREFIX`)
- `/etc/wivrn/config.json`
- `$XDG_CONFIG_HOME/wivrn/config.json` or if `$XDG_CONFIG_HOME` is not set, `$HOME/.config/wivrn/config.json`.

Files later in the list replace top-level values from previous ones.

If you installed WiVRn from a flatpack, the config is in `$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json`.

All elements are optional and have default values.

## `bit-depth`
Default value: `8` (bits)

Bit depth of the video. 8-bit is supported by all encoders. 10-bit is supported by `vaapi` and `nvenc` encoders using `h265` or `av1`.

## `bitrate-auto`
Default value: `true`

Automatically adapts the video bitrate to the quality of the wireless link, so that walking away
from the router degrades image quality instead of the connection.

The bitrate configured on the headset is used as the ceiling, the server never goes above it. The
server measures, for every frame, how much of a frame period the headset spent receiving it, plus
the frames that never arrived, and lowers the bitrate when the link is saturated. Once the link
measures healthy again it climbs back up towards the ceiling. Every change is logged at info level
with its reason.

The headset has a *Bitrate control* selector next to the bitrate slider in its streaming settings,
with three entries: *Manual*, *Adaptive* and *Adaptive v2 (experimental)*. Both switches must be
enabled for the automatic control to run: *Manual* on the headset, or `false` here, always uses the
bitrate configured on the headset. Switching the headset selector to *Manual* while streaming
restores that bitrate immediately.

Can be a boolean, or an object:

### `enabled`
Default value: `true`

Set to `false` to always use the bitrate configured on the headset, whatever the headset selector
says.

### `min-bitrate`
Default value: `10000000` (10 Mbit/s)

Lower bound in bits per second, the automatic control never goes below it. If the headset asks for
less than this, the value requested by the headset is used instead.

### `mode`
Default value: `"aimd"`

Which control law the automatic bitrate runs. Two are available:

* `"aimd"` — the original one, described above: a sliding window of per-frame link utilisation
  drives a multiplicative decrease and a slow additive probe back up, with a deep drop and a fast
  rebound on an acute lag spike. It never learns how big the link is, so it has to walk back up
  blind after every decrease.
* `"bbr"` — **experimental.** Estimates the delivered bandwidth directly, dividing the bytes the
  server put on the wire for a frame by the time the headset says it spent receiving it, and keeps a
  ten second running maximum of that. The bitrate is a gain times that estimate: 1.25 while the
  estimate is still growing, 0.85 once it settles, 1.10 for one short probe every eight seconds, and
  0.7 for one interval after loss. It converges on 0.85 of the measured link and gets back there in
  a second or two after an interruption instead of climbing blind. Frames too small to have loaded
  the link are ignored, the way BBR ignores application-limited samples, so a static scene cannot
  talk the estimate up.

**Precedence: the headset wins.** This key is only the default for a headset that has never chosen.
Picking either *Adaptive* or *Adaptive v2* in the headset settings pins the control law for that
headset and overrides this key; a headset that has only ever used *Manual*, or has never touched the
selector at all, follows whatever is configured here. So setting `"mode": "bbr"` on the server is
enough to try v2 without touching any headset — but a headset that once selected *Adaptive* keeps
AIMD until it selects something else. Note that such a headset still *displays* *Adaptive*: the
entry it shows is derived from what it last chose, not from what the server is running.

Changing the control law mid-session (either end) resets the controller to the full ceiling and
starts over: the two laws do not measure the same quantity, and measurements taken under one say
nothing about the other. Every decision either law takes is logged at info level with its reason,
its estimate and its state.

### Examples
```json
{
	"bitrate-auto": false
}
```
Disable automatic bitrate, always use the bitrate configured on the headset.

```json
{
	"bitrate-auto": {
		"enabled": true,
		"min-bitrate": 25000000
	}
}
```
Enable automatic bitrate, but never go below 25 Mbit/s.

```json
{
	"bitrate-auto": {
		"mode": "bbr"
	}
}
```
Default every headset that has not chosen for itself to the experimental bandwidth estimating
control law.

## `pacing`
Default value: `true`

Spreads each video frame's packets evenly over part of a frame period instead of handing them to the
socket as fast as the kernel accepts them.

Without it, a frame is a single burst: at 150 Mbit/s and 90 fps that is a few hundred kilobytes
arriving at the access point every 11 ms, which is what overflows its buffer and produces the lag
spike it then takes seconds to recover from. Pacing costs nothing in latency as long as the window
stays small, and keeps that buffer shallow. Packets are sent in micro-bursts of about 12 kB, so the
wakeup rate stays low and Wi-Fi frame aggregation is unaffected.

Pacing is skipped for whatever does not ride the UDP stream socket: the control socket, which carries
the IDRs and their parameter sets, is never delayed, and neither is video while the multipath
failover has it on the USB path.

The headset has its own *Smooth packet pacing* toggle in its streaming settings. Both switches must
be enabled. Every change is logged at info level.

Can be a boolean, or an object:

### `enabled`
Default value: `true`

Set to `false` to never pace, whatever the headset toggle says.

### `window`
Default value: `0.4`

Fraction of a frame period a frame's packets are spread over. Clamped to `0.5`: the automatic bitrate
reads link utilisation as the fraction of a frame period a frame took to arrive, so a window near its
`0.60` probe-up threshold would stop it ever raising the bitrate again. A frame never takes longer
than its window, and when the encoder delivers late the window shrinks by the overrun so that
completion is never pushed into the next frame.

### Examples
```json
{
	"pacing": false
}
```
Never pace, send each frame as one burst.

```json
{
	"pacing": {
		"window": 0.25
	}
}
```
Pace, but over a quarter of a frame period rather than the default 40%.

## `encoder-failover`
Default value: `true`

Hands a video stream to the software encoder when its hardware encoder dies or stops answering in
the middle of a session, instead of leaving that stream frozen until the headset reconnects.

Encoder sessions do fail while streaming: a driver reset, a suspend/resume, a GPU that was reset
under another application, or simply a bug. Until now such a stream stopped producing pictures for
good — one eye frozen, one line in the log per frame — because nothing on the server treated an
encode error as anything but a frame to drop. The server now watches each stream: one hard error, or
three half-second windows in which frames went in and no picture came out, and that stream is written
off. A software (x264) encoder is built with the same resolution, framerate, bitrate and text clarity
setting, swapped in, and starts on a keyframe, so the picture is back within a frame or two. The
other streams are untouched — a failure on one eye never moves the other.

The swap is only possible **within one codec**, and therefore only for H.264 streams. The headset's
decoder is created once, from the codec in the stream description, and there is no way to change it
that does not tear down every decoder on the headset — something that only happens on a reconnect.
A stream that was encoding H.265 or AV1 is therefore written off with one explanatory line in the log
and stays down until the headset reconnects, which also rebuilds every encoder. The same applies to a
10-bit session: the software encoder only reads 8-bit images. If having the fallback available
matters more than the compression H.265 buys, set the `codec` of the [`encoder`](#encoder) key to
`h264`.

The hardware encoder is never tried again during the session: a driver that has just failed an encode
is not to be trusted with the next one. Expect a noticeably higher CPU load while a stream is on
x264; the bitrate is left exactly where the automatic control had walked it to, since the problem is
CPU, not bandwidth. The resolution is not lowered.

The headset has its own *Encoder failover* toggle in its streaming settings. Both switches must be
enabled. Turning either off stops the server acting on a failure; it never undoes a swap that already
happened.

### Example
```json
{
	"encoder-failover": false
}
```
Never fall back to software encoding: a failing hardware encoder freezes its stream, as it did before.

## `intra-refresh`
Default value: `true`

Repairs loss that the error correction and the retransmissions could not with a rolling column of
intra-coded blocks sweeping across the picture over the next half second, instead of asking the
encoder for a full keyframe.

A keyframe is the largest frame there is, and the request goes out exactly when the link has just
proved it cannot carry the traffic it already had. Worse, the stream is held silent until the
headset acknowledges that keyframe, and every frame skipped in the meantime is another frame the
headset did not receive — which asks for another keyframe. Sweeping intra blocks across the picture
instead repairs it gradually at a near-constant bitrate, with no frame skipped and nothing large
ever sent.

The keyframes that are **not** loss recovery stay real keyframes either way: the first frame of a
session, and the one after a reconnect, a bitrate reconfiguration or an encoder failover swap. In
all of those the headset's decoder holds nothing to repair, so only a keyframe will do.

Supported on **x264** (`b_intra_refresh`, plus `x264_encoder_intra_refresh()` on demand) and on
**NVENC** (`enableIntraRefresh`, plus `forceIntraRefreshWithFrameCnt` on demand). The Vulkan
encoders do not need it — they recover by encoding against the newest reference the headset
acknowledged, so they never asked for a recovery keyframe at all. The FFmpeg VAAPI encoders expose
no intra refresh control and keep using keyframes; each logs one line saying so at startup.

The headset has its own *Intra-refresh recovery* toggle in its streaming settings. Both switches
must be enabled. The refresh mechanism is part of the encode session's configuration, so turning
the feature **on** takes effect on the next connection; turning it off applies immediately.

See [intra-refresh.md](intra-refresh.md) for the sweep length, the failure handling and the
per-encoder details.

### Example
```json
{
	"intra-refresh": false
}
```
Recover from unrecoverable loss with a full keyframe, as it did before.

## `ref-invalidation`
Default value: `true`

Repairs loss one rung cheaper than the sweep above: instead of anything rolling across the picture,
the encoder is told which frame the headset never received, and predicts the next one from an older
frame the headset *did* acknowledge.

The repair is a single ordinary P frame and it lands on the very next frame. Nothing large is sent,
no frame is skipped, no bit budget is diverted into intra blocks, and the picture is whole again
about as fast as the feedback round trip allows. It is free in a way neither of the other two rungs
is, which is why it is tried first.

Recovery is therefore a ladder, climbed only by failure:

1. **invalidate** — one P frame predicted from an older reference;
2. **refresh** — the rolling sweep, when the loss is out of reach or the invalidation was itself
   lost;
3. **IDR** — the full keyframe, when three sweeps in a row are spoiled or there is no sweep to fall
   to.

"Out of reach" is the one real limit. Invalidating a frame invalidates everything predicted from it,
so it only helps while something older survives in the encoder's decoded picture buffer. A loss
reported later than the DPB is deep would take the whole chain with it and force exactly the
keyframe this avoids, so the ladder skips that rung instead. Escalation belongs to one recovery and
not to the stream: a fresh, independent loss always starts at the bottom again.

Supported on **NVENC** (`NvEncInvalidateRefFrames`). Two things change there when this is enabled:
the picture timestamp now carries the frame index, since a timestamp is the only handle the
invalidation call has on a reference, and the DPB is raised from the driver default to four frames
so that there is something older to fall back on. Four is a deliberate compromise — deeper would
make older losses repairable, but every reference is also a frame the *headset's* decoder must hold,
and H.264/HEVC bound that by level, so a deep DPB at a large eye size is a stream a headset may
legitimately refuse. Four is about 44 ms at 90 Hz, comfortably more than a LAN feedback round trip.
Prediction quality is untouched: `numRefL0` is left alone, so the encoder still predicts from a
single reference — the newest valid one — exactly as it did with a DPB of one. The cost is reference
memory at both ends, not bitrate.

The **Vulkan** encoders already behave this way and need no switch: a DPB slot only becomes eligible
as a reference once the headset has acknowledged it, so a lost frame is never predicted from in the
first place. **x264** and the FFmpeg **VAAPI** encoders have no per-frame reference control to reach
— x264's `x264_picture_t` has no reference-list field (only `i_frame_reference`/`i_dpb_size`, which
say how many references exist and never which one a frame may use), and libavcodec fills VAAPI's
reference lists from its own reference management, below any application interface. Both log one
line saying so at startup and keep to the rungs above.

The headset has its own *Smart loss recovery* toggle in its streaming settings. Both switches must
be enabled. The deeper reference buffer is part of the encode session's configuration, so turning
the feature **on** takes effect on the next connection; turning it off applies immediately.

### Example
```json
{
	"ref-invalidation": false
}
```
Go straight to the intra refresh or the keyframe on a lost frame, as it did before.

## `emergency-framerate`
Default value: `true`

The last automatic resort for a struggling link, the rung below the bitrate floor. When the
automatic bitrate is already pinned at its minimum and the connection is **still** losing frames —
so the error correction, the retransmissions, the intra-refresh recovery and the bitrate drops have
all failed to stabilise the picture — the server halves the stream framerate, which halves the
bandwidth at once. It does not change resolution and needs no reconnect: only the encode and pacing
rate is halved, exactly like the manual *Half framerate mode*, and the panel refresh rate reported
to the running application is unchanged.

It is detected from the same delivery window the automatic bitrate already keeps: it engages after
the bitrate has sat at the floor with sustained severe loss for about three seconds, and restores
the full framerate once the link has been clean for a five-second hysteresis window. The transitions
are rate-limited so a link hovering at the edge cannot oscillate between the two rates, and every
transition is logged. It is independent of the manual *Half framerate mode*: either can be in force
without the other.

The headset has its own *Emergency framerate drop* toggle in its streaming settings. Both switches
must be enabled. The detection is live both ways — turning it off while it is engaged restores the
full framerate immediately.

### Example
```json
{
	"emergency-framerate": false
}
```
Never automatically drop the framerate, even when the link is failing at the minimum bitrate.

## `stream_scale`
Default value: `1.0`

Server-side ceiling on the size the eye images are **encoded** at, as a linear fraction of the
per-eye stream size the headset asked for. A value in `]0, 1]`; `1.0` leaves the headset in charge.

This is the sharpness/decode-cost dial. The NX Warp decoder's work scales with the pixel count of
the encoded image, and the headset's GPU runs the two eyes one after the other, so the decoded
frame rate is set by that pixel count. `0.8` linear is `0.64` of the pixels, `0.7` is about half.
The headset keeps its full display resolution either way and upscales what it decoded (bilinear, or
sharp with FSR), so the cost is peak sharpness, not field of view or geometry.

Both dimensions are scaled and then **rounded up to a multiple of 64**, the tile grid the foveation
shader and the NX Warp encoder work in (which also satisfies 4:2:0 chroma siting). For a headset
asking 1088x1088 per eye: `0.8` gives 896x896 (14x14 = 196 tiles instead of 17x17 = 289), `0.7`
gives 768x768 (12x12 = 144 tiles). The result is never smaller than one tile.

The headset has a *reduced resolution* slider of its own (`render_scale`) that means the same thing.
The two compose as the **smaller of the two**, not as a product: this setting is a cap, so a headset
that already asked for less than it keeps what it asked for, and two moderate values never multiply
into a blurry one. The effective scale is also what the foveation guardrail reads, so the periphery
cannot collapse further just because the encode was shrunk here.

Read when the headset connects, like the encoder settings it belongs to: changing it takes effect on
the next connection, not on the running session. When it is below `1.0` the resulting size is logged
at info level, for example:

```
nxwarp: stream 0 encodes 896x896 per eye (stream_scale 0.8, headset asked 1088x1088)
```

`stream-scale` is accepted as a spelling of the same key.

### Example
```json
{
	"stream_scale": 0.8
}
```
Encode 896x896 per eye when the headset asks for 1088x1088, trading peak sharpness for about a
third less decode work per frame.

## `edge_bleed`
Default value: `{ "overscan": 0.05, "extension": "fade" }`

The fix for the black band at the edge of the view at low frame rates.

When the application cannot produce a frame for every display refresh, the headset's compositor
reprojects the last frame it has to the newest head pose. Where that frame's field of view runs out
it has nothing to show, so a black band sweeps in at the edge of the view — wider the later the
frame and the faster the head. It is the most legible symptom of lag there is, because it is the one
artefact that is not part of the picture.

Two independent halves, and only one of them ever runs in a given session.

### `overscan`
A fraction in `[0, 0.5]`, default `0.05`. `0` disables it.

The server widens the field of view it hands the application by this fraction of each side's
**tangent**: at `0.05` every edge of the projection plane moves outward by 5 % of its own half
extent, so the application renders, and the encoder encodes, about 5 % more picture beyond each
edge. Those are real pixels, decoded on the headset like any other, so a reprojection has content to
move into and nothing has to be invented.

It is a fraction of the plane rather than a number of degrees because that is the quantity the
pixels are uniform in. A projection layer is a flat image: 5 % of the plane is 5 % more pixels on
that side whatever the field of view happens to be, while "5 degrees" is a wildly different number
of pixels at the centre of a wide field of view than at its edge. The headset's HUD converts it back
to degrees for display.

**The trade.** The encoded size does not change — a Pico 4 still streams 1088x1088 per eye — so the
same pixels now cover a wider angle and the picture is correspondingly less sharp. At `0.05` that is
about 4.5 % of the linear resolution, and about 9 % of the encoded area falls outside the panel and
is only ever seen during a reprojection. At `0.10` it is about 9 % and 17 %. This is the same
currency `stream_scale` spends, and the two compose: overscan at `0.05` with `stream_scale` at `0.8`
is about 24 % less linear resolution than neither.

Applied where the runtime asks the driver for the view field of view, which is the single point the
whole feature acts at: the application renders it, the compositor squashes and foveates it, the
encoder encodes it, and the headset gets it back per frame and hands it to its own compositor as the
projection layer's field of view. Nothing downstream has to know the setting exists. It takes effect
on the next frame, and the resulting margin is logged at info level:

```
edge bleed: rendering 5.0% overscan, 0.06 deg left / 0.06 deg up on eye 0 (same encoded size, so about 4.8% less sharp)
```

The **foveation** and the **NX Warp tile grid** are unaffected, and deliberately so: both are
derived from the encoded size and from a gaze direction, not from the field of view. A wider field
of view at the same encoded size moves the foveal region to a slightly different normalised
coordinate — the correct behaviour, since the gaze still points where it pointed — and leaves the
tile grid at exactly the same 17x17 it was.

### `extension`
`"none"`, `"clamp"` or `"fade"`, default `"fade"`.

What the **headset** does when `overscan` is `0`. This is the fallback that keeps "never black"
true with no encoded margin at all, and it costs no resolution because it produces no new picture:
the headset widens its own projection layer by `overscan_fallback` and fills the invented ring out
of the picture's own edge.

  * `none` — nothing. The pre-feature behaviour, and a black band whenever a reprojection outruns
    the frame. Useful for seeing what the setting is for.
  * `clamp` — the outermost row and column of the picture are stretched outward across the ring.
    Exact at one pixel out, and increasingly obviously a smear the further out it goes.
  * `fade` — clamp, and then past `fade_distance` of the ring the stretched colour decays into that
    edge's own averaged colour. A stretch of a hard edge is a streak; a stretch that decays into the
    colour it came from reads as the picture continuing off the side of the view.

Nothing is decoded for this ring. It is invented, and it is a smear if you go looking for it — but
it is a smear of the right colour, and it is only ever used where the alternative is black.

When `overscan` is above `0` the ring is off whatever this says, because the margin is then real
decoded pixels arriving inside the picture and there is nothing to invent. Exactly one of the two
halves applies.

### `overscan_fallback`
A fraction in `[0, 0.5]`, default `0.05`. The width of the ring the headset invents, in the same
units as `overscan`. Only read when `overscan` is `0` and `extension` is not `"none"`.

### `fade_distance`
A fraction in `[0, 0.25]`, default `0.02`. How far into the invented ring the stretch survives
before the fade to the edge colour begins, as a fraction of the ring. `"fade"` only.

Sent to the headset once, with the rest of the stream geometry, so a change takes effect on the next
connection. `edge-bleed` is accepted as a spelling of the same key, and every member is optional;
an out-of-range value is clamped and an unknown `extension` spelling is logged and ignored rather
than refused, because the whole object is cosmetic and a session that starts with a silly margin is
a better outcome than one that does not start.

Both controls have a GUI: **dashboard → Settings → Edge bleed**. The headset's HUD shows the margin
in play on the "Shown … pose age" line, beside the pose age on purpose — the pose age is how late
the frame on the panel is, and the margin is how much of that lateness the picture can absorb.

### Example
```json
{
	"edge_bleed": {
		"overscan": 0.08,
		"extension": "fade"
	}
}
```
Render 8 % wider than the panel shows, spending about 7 % of the linear resolution to buy a margin
a reprojection can move into. The extension mode is inert here, and is what would take over if the
overscan were turned back to `0`.

## `encoder`
The encoder to use, either a single string or object applied to all streams, or a list of string or objects with values for left, right and alpha.
When a string it is used, it is equivalent to the `encoder` item of the object.

WiVRn encodes each eye separately, and the alpha channel as one for both eyes. Each stream is processed independently, this may use resources more effectively and reduce latency.

### `encoder`
Default value: `nvenc` if Nvidia GPU and compiled with nvenc, `vaapi` for all other GPU when compiled with ffmpeg, else `x264`.

Identifier of the encoder, one of
* `x264`: software encoding
* `nvenc`: Nvidia hardware encoding
* `vaapi`: AMD/Intel hardware encoding
* `vulkan`: experimental, for any GPU that supports vulkan video encode
* `nxwarp`: NX Warp, the pure-compute codec of [nx-warp](https://github.com/nerdrx/nx-warp).
  Only built when the server was configured with `-DWIVRN_USE_NXWARP=ON`, and never chosen
  automatically — it runs on the CPU reference codec today and would lose to any hardware
  encoder on the machine. See below.

### `codec`
Default value: best supported by both headset and encoder of `av1`, `h264`, `h265`.

One of `h264`, `h265`, `av1`, `raw`, `nxwarp`.

Not all encoders support every codec:
- `x264` encoder only supports `h264` codec
- `vulkan` encoder supports `h264` and `h265` codecs
- `raw` encoder only supports `raw` codec
- `nxwarp` encoder only supports `nxwarp` codec
- `nvenc` and `vaapi` support all codecs, except `raw` and `nxwarp`

### NX Warp

`"encoder": "nxwarp"` (or `"codec": "nxwarp"`, which selects the same encoder) streams
[NX Warp](https://github.com/nerdrx/nx-warp) instead of a hardware codec. It is experimental
and it is a **protocol break**: adding the codec to the enumeration changes WiVRn's protocol
version hash, so a client and a server that know NX Warp only talk to each other. The headset
side is the other half of the switch — the codec is negotiated through the headset's
`supported_codecs` list, so a client that does not offer `nxwarp` cannot be given it, and the
headset's own toggle works by re-ordering or removing that list rather than by a new setting.

The encoder ignores WiVRn's shard FEC, retransmission and packet pacing: NX Warp brings its own
transport, which does its own tile runs, forward error correction and per-band feedback. It also
opts out of the encoder watchdog's failover, because a silent swap to VAAPI mid-session with an
NX Warp decoder on the other end is a black screen and the codec cannot be renegotiated without
a reconnect.

Per-stream `options` (all optional):

| key | default | meaning |
|---|---|---|
| `backend` | `ref` | which codec produces the bytes. `ref` is the CPU reference encoder (`nxvc_ref`), correct and far too slow — hundreds of milliseconds per eye. `vk` is the Vulkan compute encoder (`nxvc_vk_encoder`), which runs on the server's own GPU and is roughly **55x faster**, at the cost of the tools it does not implement (see below). An unrecognised value is an error, not a fallback |
| `qp` | `28` | fixed quantiser, 0..63. There is no rate control yet: the bitrate controller's number is logged and ignored |
| `inter` | `off` | inter prediction — the pose warp, per-tile motion vectors and the reference ring. `off` is all-intra, which is the safe bring-up default |
| `intra-period` | `180` | rolling intra refresh period in frames; `1` forces every tile every frame |
| `intra-dir` | `on` | directional intra prediction (tool 17). It is most of the CPU encoder's time at headset resolutions; `off` codes the DC-plane predictor only, for more bits and a much faster encode |
| `effort` | `1` | how hard the encoder looks for the cheapest way to say each frame. `1` adds the **integer requantiser**: a coefficient quantised to ±1 whose squared error is worth less than the bits it saves is dropped. Measured on RADV at 2 × 1088×1088, it is **−1.5 % BD-rate on rANS and −3.6 % on Lite for no measurable encode time** (9.12 → 9.18 ms a frame, inside the run-to-run spread). `0` is the plain dead-zone quantiser, which is what the encoder did before this option existed. It changes which levels are coded and nothing about how they decode — the stream carries no tool bit for it and no decoder can tell the levels apart — and both backends honour it. There is no level 2: nxvc refuses one, because a wider motion search measures −0.05 % for +12 % encode time and its own trellis RDOQ cannot run on a GPU. Out of range is an error, not a fallback |
| `snap-identity` | `0` | **snap still tiles to a copy**, in 1/16 luma samples; 0 = off. When the head has barely moved -- every tile corner displaced by less than this -- the encoder sends the frame as if it had not moved at all, and the headset decodes those tiles as a straight copy instead of a filtered warp. On a Pico 4 that warp is 8.25 of 13.7 ms of Pass B per pair. The error introduced is at most **half the setting**: half a sample at 16, which is the rounding the motion search already accepts. Measured: **below 16 nothing ever snaps** (a head at rest still drifts ~0.57 samples a frame), at 16 about a third of still frames qualify for -0.05 dB and slightly FEWER bytes, and at ordinary head speeds nothing snaps at all. Needs `"backend": "vk"` and `"inter": "on"`; above 32 is refused, because the unit is sixteenths and past two samples the tool discards motion rather than rounding it |
| `preset` | `1` | nxvc effort preset: `0` medium, `1` fast, `2` slow. Encoder-side only |
| `threads` | `0` | encoder worker threads for the tile pool: `0` uses every core (capped at 16), `1` is the serial path. Byte-identical either way |
| `pace` | `auto` | send pacing: `auto` follows the rate the headset reports it can decode at, `off` sends every composited frame, a number is a fixed frame rate. See below |
| `entropy` | `auto` | entropy coder: `rans` spends headset decode time to make the stream smaller, `lite` spends bitrate to make it cheaper to decode, `auto` picks from the tools the headset advertises. An unrecognised value is an error, not a fallback |
| `coded-vectors` | `default` | whether motion vectors are coded into the stream (`default`), left for the decoder to re-derive (`none`), or fixed (`static`). An unrecognised value is an error, not a fallback |
| `band-rows` | `6` | tile rows per transport band, which is the unit of pacing and of feedback |
| `tile-map` | `auto` | how a frame's bytes are laid on the transport's tile grid. `auto` uses per-tile spans when the codec reports them and every coded tile fits a transport slot, and the fixed-chunk mapping otherwise, decided per frame; `chunks` never uses spans; `spans` is `auto` under a different name, kept so a measurement run can say which it meant. See below |
| `mtu` | `1280` | transport MTU. Leaves room for WiVRn's own packet envelope inside a 1400-byte datagram |
| `lens-mask` | `on` | do not spend bits on the 64x64 tiles the headset's optics cannot show. See below |
| `lens-mask-margin` | `1` | the ring of tiles around the visible region left coded anyway, in tiles. `0` masks right up to the boundary |
| `lens-mask-skip` | `on` | also hand the mask to the codec as nxvc's skip map, where the backend has one. Separable from the mask itself so the two halves can be measured apart |

**`"lens-mask"`: the tiles no lens can show.** A headset shows a round region through each
lens; the encoded picture is a rectangle. The tiles in the corners are encoded, sent,
decoded and never seen by an eye.

The geometry is `client/utils/view_geometry.h`, the same header edge bleed uses, and the two
share one tangent-space model rather than two that agree today: `visible_region()`'s growth
factor and `overscan_angle()`'s tangent scaling are the same number, and
`tests/lens_mask_test.cpp` asserts

    visible_region(display_fov, f) == visible_region(widen(display_fov, f), 0)

**Edge bleed does not change the mask by one tile.** The overscan widens the FOV and leaves
the encoded size alone, so the tangent rectangle and the protected ellipse scale together
and every tile lands in the same place relative to the region. The test asserts that too,
tile for tile, which is how the ring edge bleed paid for is kept: the mask cannot eat it.
It also means nothing downstream has to be told the setting exists -- the encoder and the
compositor pass a margin of 0 because the FOV that reaches them has already been widened at
`wivrn_hmd::get_view_poses()`.

`is_maskable()` in that header is the RECTANGULAR question -- is a region disjoint from the
panel's bounding box -- and it is what the reprojection pass asks. The lens mask asks a
different one, and a stricter one: the four corners of that bounding box are inside it and
outside the round lens, and masking them is the whole point. The two are therefore not
ANDed; at overscan 0 the panel rectangle IS the whole image and an AND would mask nothing.
Measured on the 100 deg case at 5 % overscan: all 12 masked tiles overlap the panel
rectangle and none is wholly inside the ring.

The visible region is
modelled as an ellipse in TANGENT space, centred on the lens axis, with a half-extent on
each axis of the LARGER of that axis's two FOV half-angles. Sizing it from the larger half
is the conservative choice and it is what makes an asymmetric FOV matter: the region then
covers the narrow side of the picture entirely and only the far corners of the wide side
can be masked.

A tile is masked only when its whole source footprint -- destination pixels mapped back
through the foveation runs -- falls outside that region, AND so does every tile within
`lens-mask-margin` of it. At the default 1 the entire ring of tiles that touches the
boundary is still coded, which is the guard against everything the model does not carry:
lens tolerances, an eye off the optical axis, a runtime reporting a FOV slightly larger
than the optics, and the resampling the headset's defoveator does at a tile edge.

Two mechanisms, and they are not the same:

* the masked tiles are **filled with a flat mid grey** before they are encoded -- by the
  compositor's foveation pass on the image path, and by the encoder itself on the host-plane
  path. A tile that is the same constant every frame has no residual and no displacement
  error, so the mode search picks WARP_SKIP for it, keeps picking it, and never trips the
  bound that would force it back to INTRA. This half works on every backend and on every
  codec;
* where the backend has an input for it, the codec is **also told** (`nxvc_encoder_set_skip_map`,
  which the CPU reference encoder has and the Vulkan one does not). nxvc still overrides the
  request wherever a coded tile is required -- rolling intra refresh, no eligible reference,
  an alpha plane -- so this is a request, not an assertion.

The flattening is the half that matters, and that is a measurement rather than a
preference: asking nxvc to skip a tile whose PICTURE is still moving asks it to reconstruct
a moving picture by warping, which drifts into a forced refresh, and on a rotating clip the
skip map ALONE made the stream larger. With the pixels already flat there is nothing to
drift and the request is free.

The server says once per stream what it got:

```
nxwarp: stream 0 lens mask: 12 of 289 tiles per eye masked (margin 1 tile, fov -52.5/52.5/52.5/-52.5 deg); never coded (nxvc skip map)
```

or `flattened only: this backend has no skip-map input ...` on the Vulkan backend.

**What it is worth, measured.** Small, and the reason is worth knowing before turning it
off. A masked tile does not become free: an INTRA tile has a per-tile floor of about 32
bytes of header and minimum payload however flat it is, so flattening recovers only what
the tile cost above that floor. Measured at 1088x1088 QP 30 (docs/NXWARP-E2E.md has the
runs): −0.12 % on an inter clip, −0.33 % all-intra, −0.26 % all-intra on uniformly detailed
content — against a 2.1 % tile share. On an inter stream the corner tiles of a static scene
are already skipping anyway; the configuration where it pays is a live session with head
motion, where the corners carry detail that keeps being re-coded. The mechanism is exact and
costs nothing to keep on, but do not expect the tile share in bytes. Removing the floor
would need a way to tell the codec a tile is ABSENT rather than flat, which nxvc does not
have — a skip map pins a mode, and `row_present` is row-granular over the atlas.

**Zero masked tiles is a real answer, not a failure.** The mask is computed on the ENCODED
picture, after foveation, and foveation compresses the periphery hard: at 1088x1088 encoded
from a 2176x2176 render, one corner tile covers about a seventh of the source width and
reaches into the visible region, so nothing is entirely outside it any more. Unfoveated, the
same geometry masks 12 of 289 tiles per eye (40 at margin 0). The dashboard's headset
statistics page shows the pair, next to the encode size.

**Settings → Skip invisible tiles** is the switch, on by default.

**`"tile-map"`, and when to move it.** With spans, a codec tile's own bytes travel at its
own tile index, so a lost datagram costs the tiles it carried instead of the whole frame,
and the per-tile receipt map the encoder predicts from finally names real tiles. It is the
default and it is what you want. `chunks` exists for two reasons: it is the A/B for
anything the new mapping is suspected of costing — same clip, same QP, same frames, two
mappings — and it is somewhere to stand if a live session regresses. The server logs which
one it used every two seconds:

```
nxwarp: stream 0 tile mapping: 178 frame(s) with per-tile spans, 0 with the fixed-chunk fallback ("tile-map": "auto")
```

A frame with any tile larger than a transport slot cannot be carried at one tile per slot
and falls back whole, so an all-fallback line under `auto` means the quantiser is low
enough that tiles are outgrowing the MTU — raise `qp`, or `mtu` if the link allows it.

There is no need to read the log for it. **Settings → Tile mapping** sets it, and the
dashboard's headset statistics page reports what actually happened over the last window
("`128 frames on per-tile spans, 4 on fixed chunks`") — counts rather than the setting,
because the choice is per frame and a mix is a normal answer. The in-headset HUD says it
too, on the stream's geometry line, told from what arrived rather than from anything on
the wire: under fixed chunks a frame carries one transport tile per MTU-sized piece of its
bitstream and under spans it carries every tile it coded, which on a paired 1088x1088 is 45
against 578.

**`"backend": "vk"` is intra-only, and that is not free.** It implements the DC-plane
intra half of the v1 bitstream and nothing else: no inter prediction (`"inter": "on"`
together with it is refused at startup rather than silently ignored), no directional
intra, no chroma-from-luma, no 4x4 transform split, no custom probability tables. The
reference backend has all of those on by default, and they are worth real bitrate — at
1088x1088 and the same QP the GPU backend spends about **1.9x the bytes** for about
**3 dB less** PSNR. It is the right trade when the alternative is under 2 fps, and the
wrong one if you were getting frames out of the reference already.

`set_view()` and the transport's per-tile feedback are still plumbed to the GPU backend
and are accepted and ignored by it, because an all-intra frame has no reference to warp
and no prediction a lost tile can corrupt. The transport itself still conceals normally.

The backend needs an `nxvc` built with the Vulkan encoder
(`-DNXWARP_BUILD_VK=ON -DNXWARP_VK_SUBDIRS="common;encoder;decoder"`). Configure prints
`NX Warp Vulkan encoder: ON` when it found one; without it, asking for `"vk"` is an
error at encoder construction and `"ref"` still works.

#### Rate control

NX Warp codes a whole frame at one quantiser and has no rate control inside the codec, so
bytes per frame are the only output variable — and on the headset that one number is two
things at once. It is the load on the link, and it is the frame rate: NX Warp decodes at
roughly a millisecond per kilobyte on a Pico 4, so a 12 KB frame decodes inside a 90 Hz
budget and a 52 KB frame arrives at about ten. The quantiser is therefore the knob for a
steady session, and `"rc": "auto"` (the default) turns it, frame by frame, to hold the
bytes per frame the session's bitrate ceiling implies.

The controller takes the ceiling this stream was given — already its share of the link,
and already net of the FEC parity overhead — divides by eight and by the compositor frame
rate, and steps the quantiser one QP per frame toward the result, two while more than a
factor of two out, with a 5% dead band so it stops rather than dithering. It re-reads the
ceiling every frame, so automatic bitrate mode is followed live. `min-qp` and `max-qp`
bound it: below `min-qp` the frames cost frame rate on the headset however much link there
is, and above `max-qp` the picture is not worth sending. The two-second encode report
prints the applied QP next to the bytes it bought and the budget they were aimed at.

`"rc": "fixed"` is the old behaviour — this encoder's `qp` for the whole session, whatever
the link is doing — and the ceiling is then ignored, which the log says once.

#### Send pacing

The compositor produces 90 frames a second. A Pico 4's NX Warp decoder takes 15–17 ms per
eye and keeps a queue of one, so it decodes about one frame in four — and dropping the
other three is not free. The headset reports every frame it does not reconstruct, and the
server's answer is an all-intra frame: three times the size, slower to decode, so more get
dropped. Measured on a live session, that loop held inter prediction off entirely, at 614
frames dropped per two seconds per stream.

`"pace": "auto"` breaks it by not sending what cannot be decoded. The headset puts its own
measured decode cost on every feedback packet; the encoder keeps an interval derived from
it — the decode time plus a tenth of it plus a millisecond — and a composited frame that
arrives sooner than that since the last frame it **sent** is dropped before anything is
spent on it: no encode, no bytes, no transport state, no frame id. The interval slews a
twentieth of the way to the target every frame and jumps five percent slower on every
frame the headset reports it dropped by its decode stride, clamped to 90…15 fps. The
asymmetry is the point: being too slow costs frame rate, being too fast costs an intra
frame *and* the frame rate.

Frame ids on the wire count sent frames, not composited ones, so a paced gap never reaches
the client as a gap — a hole in the sequence is loss to its reassembler and to the delivery
reports WiVRn's automatic bitrate reads, and a paced frame is not lost.

A headset that has not reported a decode cost is not paced at all, so the first frames of a
session, and a client too old to carry the field, behave exactly as they did before.

The two-second encode report carries the pace, the decode figure it came from, and how many
composited frames were not sent.

| option | default | |
| --- | --- | --- |
| `pace` | `auto` | follow the headset's reported decode cost |
| | `off` | send every composited frame — the behaviour before pacing existed |
| | `30` | a fixed frame rate, honoured exactly whatever the headset reports |

| option | default | |
| --- | --- | --- |
| `rc` | `auto` | `auto` honours the bitrate ceiling, `fixed` pins `qp` |
| `qp` | `28` | the quantiser, 0..63; with `auto`, where the controller starts |
| `min-qp` | `20` | the finest quantiser the controller may reach |
| `max-qp` | `44` | the coarsest |

```json
{
	"encoders": [
		{
			"encoder": "nxwarp",
			"options": { "qp": "26", "inter": "on" }
		}
	]
}
```

### Watching it run

The encoder's two-second report is on the bus as well as in the log, as `NxwarpStats` on
`io.github.wivrn.Server`: a JSON array with one object per active stream, replaced whole every
time a stream reports, and empty when nothing is streaming NX Warp. The schema is
`common/nxwarp_stats.h`.

```
busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server \
        io.github.wivrn.Server NxwarpStats
```

The dashboard renders it on the **Headset statistics** page, one card per stream: frames sent and
the rate they are paced to, the headset's own decode time, frames the pacer held back, encode
time, frame size against the controller's target, the quantiser and its band, the bitrate the
controller allows, what the headset failed to reconstruct and the reason that accounts for most of
it, the encoded size and tile count, the effort level, how many tiles the headset can decode as a copy,
and the negotiated entropy coder. Each line has a one-sentence
note on what it means. Nothing there requires reading the log.

### In the dashboard

None of this needs a text editor. Selecting **NX Warp** in the encoder drop-down on the
dashboard's settings page reveals an **NX Warp encoder** section carrying the settings above that
are worth changing: [`stream_scale`](#stream_scale) (with the per-eye size and tile count it will
produce, from the size the last connected headset asked for), `entropy`, `pace` with its fixed
frame rate, `rc` with its `min-qp`/`max-qp` band, `coded-vectors`, `effort` (as **Extra encoder
effort**, on by default), `snap-identity` (as **Snap still tiles**, off), and
`inter` with `intra-period`. Each carries a one-line note on what it trades away.

The remaining options — `backend`, `qp`, `intra-dir`, `preset`, `threads`, `band-rows`, `mtu` —
are bring-up and debugging controls rather than things to tune, and stay in the file. The
dashboard leaves any option it does not show exactly as it found it, and writes an option out only
when it differs from the default above, so a configuration edited by hand keeps its shape.

`wivrn-nxwarp-loopback`, built alongside the server, runs the whole encoder path — codec,
packetizer, transport, receiver, reference decoder — on synthetic frames with no headset, no GPU
and no socket, and writes a `.nxv` that `nxv-dec` can decode.

If `nvenc` encoder is in use, you can refer to [nvidia website](https://developer.nvidia.com/video-encode-decode-support-matrix) to make sure that your GPU supports encoding with the desired codec.

### Examples
1. Simple configuration
```json
{
	"encoder": {
		"encoder": "vaapi",
		"codec": "h265"
	}
}
```
Use vaapi hardware encoding, h265 video codec (HEVC).

2. Hardware + software encoder
```json
{
	"encoder": [
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
		{
			"encoder": "x264",
			"codec": "h264",
		},
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
	]
}
```
Creates a hardware encoder for left eye and transparency, and a software encoder for right eye.

### `device`, only for vaapi
Default value: unset

Manually specify the device for encoding, can be used to offload encode to an iGPU. Device shall be in the form "/dev/dri/renderD128".


### `options` (very advanced), only for vaapi
Default value: unset

Json object of additional options to pass directly to ffmpeg `avcodec_open2`'s `option` parameter.

## `mirror`
Default value: `false`

Only available when built with `WIVRN_USE_PIPEWIRE`.

Publishes a desktop mirror of the headset view as a PipeWire video source node, named
`wivrn-headset-view` and described as *WiVRn Headset View*, with media class `Video/Source`, so
that it can be displayed or recorded by any PipeWire consumer.

The frames are taken in the compositor, from the left eye, after the layers have been composited
but before foveation and before encoding: what is published is the clean undistorted view, not
the foveated image that is sent to the headset.

**Disabled by default, because it is not free**: every captured frame costs a compute resample of
the eye view, a copy back to system memory and a copy into a PipeWire buffer. Nothing at all is
recorded when no consumer is connected to the node, so the cost is only paid while something is
actually watching. Frames are captured at most at the configured rate, and a frame is skipped
rather than waited on if the previous capture has not been read yet.

The node exists for the duration of a headset session: it appears when a headset connects and
disappears when it disconnects. The configuration is read when the session starts, so changing it
requires reconnecting the headset.

Can be a boolean, or an object:

### `enabled`
Default value: `false`

Whether the mirror node is published.

### `fps`
Default value: `30`

Rate at which frames are captured, in frames per second. Capped to the compositor frame rate: the
mirror never captures more than once per composited frame.

### `scale`
Default value: `0.5`

Resolution of the published video, as a fraction of the per-eye render resolution. Halving it
divides the readback and copy cost by four, which is why it is the default.

### Example
```json
{
	"mirror": true
}
```
Publish the mirror at half the per-eye resolution, 30 fps.

```json
{
	"mirror": {
		"enabled": true,
		"fps": 60,
		"scale": 1.0
	}
}
```
Publish the mirror at the full per-eye resolution, 60 fps.

### Viewing it
```sh
gst-launch-1.0 pipewiresrc target-object=wivrn-headset-view ! videoconvert ! autovideosink
```
In OBS, add a *PipeWire Camera* source (OBS 30 or later) and pick *WiVRn Headset View*. Any other
PipeWire client works too, `pw-cat`, `qpwgraph` and Wireplumber's `wpctl status` list the node
under *Video/Source*.

## `application`
Default value: unset

An application to start when connection with the headset is established, can be a string or an array of strings if parameters need to be provided.

### Example
```json
{
	"application": ["steam", "steam://launch/275850/VR"]
}
```
Launch No Man's Sky in VR mode on Steam when connection with headset is established.

## `tcp-only`
Default value: `false`

Only use TCP for communications with the client, this may have increased latency.
If `false` or unset, WiVRn will use both TCP and UDP.

### Example
```json
{
	"tcp-only": true
}
```

## `publish-service`
Default value: `avahi`

How to publish the service over the network, `avahi` or null.

If set to null, service will not be published and address has to be entered manually on the headset.

## `openvr-compat-path`
Default value: unset

Provides the path to the directory of an OpenVR compatibility tool (such as OpenComposite).

If unset, WiVRn will autodetect the path of such a tool as usual (see [the SteamVR guide](./steamvr.md)).

If set to an null, WiVRn will not manage the OpenVR configuration.

## `hid-forwarding`
Default value: `false`

Only available when the `uinput` kernel module is loaded and the user has write access.

Mirrors input devices forwarded from the headset (keyboard, mouse, gamepad) to `uinput`
devices, for applications that do not read them through OpenXR. Which devices are forwarded is
chosen on the headset.

A forwarded gamepad is also exposed as a native OpenXR gamepad at `/user/gamepad`, which needs
no permission and is always available. Only the digested controller state is forwarded
(buttons, two sticks, analog triggers and a d-pad), so device specific features such as gyro,
adaptive triggers or the touchpad are not available.

## `debug-gui`
Default value: `false`

Only available when built with `WIVRN_FEATURE_DEBUG_GUI`.

Enables the Monado debug gui.

## `use-steamvr-lh`
Default value: `false`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`

Enables the driver to load SteamVR Lighthouse devices.

## `lh-stick-deadzone`
Default value: `0`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`

Applies a deadzone to joysticks on SteamVR controllers (e.g. Index).

## `port`
Default value: `9757`

Change the TCP/UDP port used for the connection.

## `hostname`
Default value: unset

If set, overrides the name displayed in the server list.
