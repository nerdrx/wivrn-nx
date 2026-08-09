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

The headset has its own *Automatic bitrate* toggle, next to the bitrate slider in its streaming
settings. Both switches must be enabled for the automatic control to run: turning either of them off
always uses the bitrate configured on the headset. Switching the headset toggle off while streaming
restores that bitrate immediately.

Can be a boolean, or an object:

### `enabled`
Default value: `true`

Set to `false` to always use the bitrate configured on the headset, whatever the headset toggle says.

### `min-bitrate`
Default value: `10000000` (10 Mbit/s)

Lower bound in bits per second, the automatic control never goes below it. If the headset asks for
less than this, the value requested by the headset is used instead.

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

### `codec`
Default value: best supported by both headset and encoder of `av1`, `h264`, `h265`.

One of `h264`, `h265`, `av1`, `raw`.

Not all encoders support every codec:
- `x264` encoder only supports `h264` codec
- `vulkan` encoder supports `h264` and `h265` codecs
- `raw` encoder only supports `raw` codec
- `nvenc` and `vaapi` support all codecs, except `raw`

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
