# Intra-refresh loss recovery

## The problem

Video streaming to a headset is one long chain of P frames: every frame is coded as a difference
against the one before it. That is what makes the bitrate small enough to fit over Wi-Fi, and it
is also why a single lost frame does not stay lost — every frame after it predicts from something
the headset does not have, and the picture stays wrong until the chain is broken.

The way it has always been broken is a keyframe. When the headset reports a frame it never
received whole (`sent_to_decoder` unset in its feedback), `default_idr_handler` asks the encoder
for an IDR: a frame coded from nothing at all, which the decoder can start from cleanly. It works,
and on a good link it is invisible.

On a bad link it is the wrong answer, for three compounding reasons:

* **An IDR is the largest frame there is.** Ten to thirty times a P frame is normal. The request
  goes out precisely when the link has just proved it cannot carry the traffic it already had.
* **The IDR itself gets lost or delayed.** It rides the control (TCP) socket, so it is not
  *dropped*, but a fat retransmitting TCP frame on a struggling link arrives late, and the whole
  stream is *skipped* until the headset acknowledges it — `should_skip` returns true the entire
  time. The headset sees nothing at all for as long as that takes, and it gives up on a stream
  that has shown it nothing for a second.
* **Every frame that was skipped is a frame the headset did not get,** which is more evidence of
  loss, which asks for another IDR. Loss → IDR → more loss → another IDR.

## The mechanism

Intra refresh replaces the recovery keyframe with a rolling column of intra-coded blocks that
sweeps across the picture over the next few dozen frames. Each frame carries a slice of the
picture coded from nothing, and the encoder restricts motion vectors so that the already-refreshed
side never predicts from the not-yet-refreshed side. After one full sweep every block has been
coded from nothing at some point, so the picture is whole again — without any single frame ever
being large.

The result is recovery at a near-constant bitrate. Nothing spikes, nothing is skipped, and the
headset keeps receiving decodable frames throughout: it sees the damaged region shrink from one
edge to the other rather than seeing nothing at all and then a perfect frame.

## Which keyframes stay keyframes

Not all keyframes are loss recovery, and the ones that are not **must** stay real IDRs. The split
lives in `default_idr_handler`:

| Situation | What it produces |
|---|---|
| First frame of a session | Real IDR |
| `reset()` — reconnect, bitrate/framerate reconfiguration, encoder failover swap | Real IDR |
| Headset reports a lost reference frame, intra refresh available and enabled | Intra refresh sweep |
| Headset reports a lost reference frame, otherwise | Real IDR (unchanged behaviour) |
| Several sweeps in a row spoiled by further loss | Real IDR |

The reasoning for the first two rows is the same: the headset's decoder holds nothing this encoder
produced, so there is no partially-correct picture to repair. A sweep predicted from nothing
decodes to nothing. Those are the cases where a keyframe is not a cost but the only option.

## The state machine

`default_idr_handler` gained two states next to the ones it always had:

* **`need_refresh`** — a loss was reported, the next frame encoded starts a sweep.
* **`refreshing`** — a sweep is crossing the picture. `should_skip` returns **false** throughout:
  the intra blocks that repair the picture ride these frames, so skipping them would be skipping
  the repair. This is the opposite of the `wait_idr_feedback` state, which skips everything.

Two decisions inside `refreshing` are worth spelling out.

**Further loss during a sweep never restarts it.** A frame that goes missing mid-sweep took a
slice of intra blocks with it, so that column was not refreshed and the sweep has failed — but
restarting on the report would move the column back to the left edge, and a link that is still
dropping frames would keep doing that forever, leaving the picture permanently half repaired. The
loss is recorded and the verdict is passed once, when the sweep ends.

**Failures are bounded.** A sweep that ended with loss inside it starts another one, up to three
attempts. A link bad enough to spoil three sweeps in a row is not one a gentle repair can fix, so
the fourth attempt is a real IDR. A sweep that completes cleanly clears the tally, so the count is
about the link right now and not about the session.

## Per-encoder support

| Encoder | Support | How |
|---|---|---|
| **x264** | Full, native | `param.b_intra_refresh = 1` with `i_keyint_max` set to the sweep length, and `x264_encoder_intra_refresh()` on demand |
| **NVENC** | Full, native | `enableIntraRefresh` / `intraRefreshPeriod` / `intraRefreshCnt` in the codec config, and `forceIntraRefreshWithFrameCnt` in the per-frame codec picture parameters, on demand |
| **Vulkan video encode** | Not needed | Recovers by encoding against the newest reference the headset acknowledged, so it never asked for a recovery keyframe in the first place |
| **VAAPI (FFmpeg)** | Not available | libavcodec exposes no intra refresh control on `h264_vaapi` / `hevc_vaapi` / `av1_vaapi`; loss still asks for a keyframe |

### x264

The sweep speed is not its own parameter. x264 advances the refresh column by
`max((mb_width - 1) / i_keyint_max, 1)` macroblocks per frame, so the keyframe interval is what
sets it. Left at `X264_KEYINT_MAX_INFINITE` — what WiVRn used before — the clamp gives the minimum
of one macroblock column per frame: 120 frames to cross a 1920-wide eye, well over a second, far
too slow to repair a loss with. Setting `i_keyint_max` to the sweep length gives a sweep of about
that many frames at any width.

The side effect is that x264 also starts a sweep of its own every `i_keyint_max` frames. That is
welcome rather than not: it bounds how long an error the headset never reported can persist, and it
does not *add* bitrate — the rate control has the same budget either way and simply spends a little
of it on intra blocks.

Forcing an IDR still produces a real IDR under `b_intra_refresh` (verified directly against
libx264: the forced frame carries SPS, PPS and IDR slices), so the keyframes in the table above
are unaffected.

### NVENC

NVENC runs intra refresh continuously once enabled. `intraRefreshCnt` is the sweep length,
`intraRefreshPeriod` how often a sweep starts on its own; the period has to be the larger of the
two, and is set to twice the sweep length. On top of that, a loss asks for a sweep of its own
through `forceIntraRefreshWithFrameCnt` in `NV_ENC_PIC_PARAMS_H264` / `_HEVC` / `_AV1`.

Support is queried with `NV_ENC_CAPS_SUPPORT_INTRA_REFRESH`; a GPU that says no falls back to
keyframe recovery with one line in the log.

### Vulkan and VAAPI

The Vulkan encoders use `dpb_state` rather than `default_idr_handler`: they keep every reference
slot the headset has acknowledged and encode the next frame against the newest of them. Loss
therefore costs a slightly older reference instead of a keyframe, and the doom loop this feature
exists to break never starts. Adding a rolling refresh would need per-block intra control the
Vulkan video encode extension WiVRn targets does not expose, and would buy nothing.

The FFmpeg VAAPI encoders have no intra refresh AVOption at all — the only keyframe-related
option any of them exposes is `idr_interval`, which controls how often a *whole* IDR is emitted.
VAAPI itself has an intra refresh parameter on some drivers, but nothing in libavcodec plumbs it
through, so honouring the setting would mean going around the encoder FFmpeg owns. Both log one
line at encoder creation saying what actually happens.

## Defaults and the sweep length

The sweep is **48 frames**: half a second at 90 Hz, 0.4 s at 120 Hz.

The trade is quality against recovery time. A short sweep spends more of the bit budget on intra
blocks and leaves less for the picture; a long one recovers slowly and, worse, leaves a wider
window in which a second loss lands inside the sweep and spoils it. 48 frames is comfortably
inside the one second of no decoded output after which the headset gives up on a stream, with room
for two sweeps before that becomes a risk.

## Bitrate

Intra refresh raises the steady-state cost of the stream slightly: every frame of a sweep carries
some intra-coded blocks, and with the periodic sweep x264 performs there is usually a sweep in
progress. No accounting change is needed anywhere. The rate control has a fixed budget and
allocates within it, so the effect is a small quality cost rather than extra bytes on the wire —
and it is repaid many times over by not sending recovery keyframes on a link that cannot carry
them.

## Interaction with the rest of the transport

Forward error correction and shard retransmission come first: they repair loss without the encoder
ever hearing about it, and every loss they absorb is a sweep that never happens. Intra refresh is
the fallback for the loss they could not repair — the case that used to produce a keyframe.

Encoder failover interacts the other way round. When a hardware encoder is written off and a
software (x264) one takes its stream, the replacement is built from the same settings and
therefore configures intra refresh the same way, and is handed the live state of the switch. The
keyframe it starts on is a real IDR, as it must be: the headset holds nothing the new encoder
produced.

## Configuration

Server, in `~/.config/wivrn/config.json`:

```json
{
	"intra-refresh": false
}
```

Default `true`. Headset: *Intra-refresh recovery* in the streaming settings, default on.

Both switches must be enabled, like `encoder-failover`. The refresh mechanism is part of the
encode session's configuration on every encoder that has one, so it can only be set up when the
encoder is created: **turning the feature on takes effect on the next connection**, while turning
it off applies immediately.

## Tests

`tests/intra_refresh_test.cpp` drives the handler frame by frame with no encoder and no headset,
covering the start-versus-recovery split, the absence of skipping during a sweep, the
no-restart-on-loss rule, the bounded escalation to an IDR, the failure tally, the live switch and
non-reference frames.

```
g++ -std=c++23 -I server -I common -I build-server/common \
    -I build-server/_deps/monado-src/src/xrt/include \
    -I build-server/_deps/monado-src/src/xrt/auxiliary \
    -I build-server/_deps/monado-src/src/external/openxr_includes \
    -isystem external -isystem build-server/_deps/boost-src/libs/pfr/include \
    -o intra_refresh_test tests/intra_refresh_test.cpp \
    server/encoder/idr_handler.cpp common/smp.cpp -lcrypto
./intra_refresh_test
```
