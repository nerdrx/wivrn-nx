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
| `preset` | `1` | nxvc effort preset: `0` medium, `1` fast, `2` slow. Encoder-side only |
| `threads` | `0` | encoder worker threads for the tile pool: `0` uses every core (capped at 16), `1` is the serial path. Byte-identical either way |
| `band-rows` | `6` | tile rows per transport band, which is the unit of pacing and of feedback |
| `mtu` | `1280` | transport MTU. Leaves room for WiVRn's own packet envelope inside a 1400-byte datagram |

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
