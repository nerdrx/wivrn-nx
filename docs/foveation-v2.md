# Fixed foveation v2 — "sharper center" (WiVRn NX)

Status: implemented (levers 1 + 2 + the render_scale/FSR guardrail). Headset toggle
**"Sharper center (foveation)"** plus a strength slider on the Streaming settings page, off by
default (strength 0 = the pre-v2 curve, behaviour unchanged). Two sub-toggles: **"Adaptive
foveation"** and **"Protect foveal quality (NVENC/x264)"**. Lever 3 (foveal ROI QP) is stubbed —
see the last section.

## Problem

WiVRn's foveation is a variable-rate *resample*, not a QP trick: the server warps each eye so
the fovea is 1:1 and the periphery is progressively downsampled, encodes the smaller warped
image, and sends the client a span list (`to_headset::foveation_parameter`) describing the
pixel-ratio bands so it can undo the warp. Pre-v2 there was a single knob, `stream_scale`, which
set **both** the encoded size **and** the periphery steepness at once, and the fovea was 1:1 at a
single point. On a headset with no eye tracking (Pico 4) the sharp region is fixed at the center,
so we want to spend the bit budget on a wider, genuinely full-resolution center — *without*
changing the encoded size (which cannot change live and interacts with the encoder, FEC and the
bitrate controller).

## Lever 1 — decouple periphery steepness from encode size (the core win)

`server/compositor/foveation.cpp` generalises the warp curve from a single tan to a **central
1:1 plateau** flanked by two independent tan periphery branches. `defoveate(x)` maps foveated
(encoded, destination) coordinates back to source (full) coordinates:

```
defoveate(x) = c + λ·(x - xc)                       for |x - xc| <= p    (1:1 plateau)
             = (c + λp) + λ/aR·tan(aR·(x - xc - p))  for  x > xc + p      (right periphery)
             = (c - λp) + λ/aL·tan(aL·(x - xc + p))  for  x < xc - p      (left periphery)
```

- `λ = foveated_dim / source_dim` (the encode-size ratio, **unchanged** by this feature).
- `c` is the fovea center in source coordinates; `xc` is its destination coordinate, taken from
  the original coupled solver `solve_foveation` so that at strength 0 everything collapses back
  to the pre-v2 single tan.
- `p` is the half-width of the 1:1 plateau in destination coordinates — this is what strength
  widens.
- `aL`, `aR` are each solved by `solve_branch(λ, d, Δ)`, the root of `tan(a·d) = a·Δ/λ`, where
  `d` is the branch's destination length (junction→edge) and `Δ` its source length.

The plateau is **exactly 1:1 in pixels**: its slope `λ` in normalised coords is
`λ · source_dim/foveated_dim = 1` source pixel per destination pixel, so the span quantiser
(`fill_param_2d`) emits a 1:1 band across the whole plateau. The periphery joins the plateau
**C¹** (slope `λ` at `xc ± p`) and each branch is solved so the **edge still lands exactly**
(`defoveate(±1) = ±1`). That edge-matching is the property the solver must preserve: get the
plateau wrong and either the periphery is discontinuous or the branch solve fails to converge.
Verified numerically for centered and off-center `c` at several strengths — edges hit to 1e-6,
fovea slope stays `λ`, and strength 0 reproduces the original edge steepness.

Because only the curve *shape* changes and never `λ`, the encoded image is the same size; more
source pixels are simply kept 1:1 in the center, paid for by a steeper periphery. The wire format
(the span list) and the client defoveator are **untouched** — the client already consumes
arbitrary span distributions (that is how it handles the eye-tracked gaze path), it just receives
a different distribution.

Strength maps to `p = strength · 0.6 · room`, where `room = min(1-xc, 1+xc)` is the destination
distance to the nearer edge (before the guardrail below trims it).

## Plumbing and live-vs-reconnect

`float foveation_strength` (and `bool foveation_adaptive`, `bool foveation_foveal_qp`) were added
to `from_headset::settings_changed` (`common/wivrn_packets.h`), serialised automatically by
boost::pfr like every other field. The client stores `sharper_center` + `foveation_strength` in
its configuration, sends them in the headset info packet and in every live `settings_changed`
packet (`effective_foveation_strength()` = 0 when the toggle is off). The server reads them in the
compositor **per frame** (`compositor::update_foveation_shape`) and calls `foveation::set_shape`.

Unlike `render_scale`, which the server only reads when it *builds* the encoders (encode size
cannot change live), the curve shape is recomputed inside `update_ubo` whenever it changes:
`shape.strength` / `shape.render_scale` are part of the dirty-set fingerprint, so a changed
strength re-quantises the spans on the very next frame with **no renegotiation and no reconnect**.
This is confirmed by construction — nothing in the encode path or the negotiated stream depends
on the strength.

## Lever 2 — adaptive foveation (bitrate coupling)

When **Adaptive foveation** is on and the automatic bitrate is active (`bitrate_auto`),
`update_foveation_shape` reads the bitrate controller's snapshot
(`wivrn_session::bitrate_status()` → `bitrate_controller::snapshot()`) and steepens the curve as
the controller backs off its ceiling:

```
ratio   = bitrate_bps / ceiling_bps          # 1 while riding the ceiling
degrade = clamp((0.85 - ratio) / 0.85, 0, 1) # 0.85 dead zone leaves steady-state headroom alone
target  = clamp(base + degrade·(1 - base), 0, 1)
```

So a link that holds the ceiling changes nothing; a Wi-Fi dip that forces the controller down
raises the strength toward 1, compressing the periphery instead of dropping global quality. The
bitrate itself and the encode size are **never** touched — only the curve shape, recomputed per
frame.

**Hysteresis / rate-limit:** `target` feeds a first-order lag (`foveation_adaptive_state`) with a
~2 s rise and ~4 s fall time constant, and the value actually pushed to `set_shape` is snapped to
0.02 steps so a slow drift does not re-quantise the spans every frame. The result cannot pop
frame-to-frame; a real change of one step still applies live on the next frame. Gated entirely on
`bitrate_auto_active()` — with a fixed bitrate there is no controller state to read.

## Lever guardrail — composing with render_scale + FSR

`render_scale` (reduced-resolution streaming) uniformly shrinks the *whole* eye, fovea included,
and the client's FSR pass reconstructs the center. Foveation shrinks the *periphery* on top. The
two form a multiplicative chain: the combined peripheral factor is
`render_scale × (1 / periphery_ratio)`, and if both go low the periphery collapses into a blocky
FSR upscale.

`build_curve` therefore bounds the added periphery steepness **relative to the neutral curve**,
tightening the bound as `render_scale` falls:

```
cap = neutral_edge_ratio × (1 + 2 · render_scale)   # 3× neutral at full res, 2× at render_scale 0.5
```

`edge_ratio` (the steepest source-pixels-per-destination-pixel at either edge) is monotone in `p`,
so if the strength-requested plateau would exceed the cap, `p` is bisected down until it fits. At
strength 0 this is inert (`p = 0`, neutral curve). Role split, documented so the three do not
fight: **render_scale owns central sharpness, foveation owns the peripheral taper, FSR
reconstructs.**

## Lever 3 — foveal ROI QP (stubbed)

Because fixed foveation makes the central region a *static* rectangle, an encoder that exposes a
per-region QP map could bias QP down over it for extra central protection. This is **stubbed**,
deliberately:

- The setting (`foveation_foveal_qp`, toggle "Protect foveal quality (NVENC/x264)") exists and is
  plumbed to the encoders (`encoder_settings::foveation_foveal_qp`).
- **NVENC** (`video_encoder_nvenc.cpp`) and **x264** (`video_encoder_x264.cpp`) carry a `TODO`
  at the exact place the map would be filled (`NV_ENC_PIC_PARAMS.qpDeltaMap` /
  `pic.prop.quant_offsets`). Wiring it needs the foveal rectangle threaded from the compositor
  into the per-frame encode path, which is the risky, unverifiable part.
- **VAAPI** and **Vulkan** encoders have no portable per-region QP path and **log once** that they
  cannot honour it.

The target hardware here is VAAPI/Vulkan, where lever 3 would never engage, so the effort went
into making levers 1 + 2 + the guardrail solid rather than half-wiring a QP map that cannot be
tested. The curve reshaping is the actual win and applies to every encoder.

## On-device validation (needs a headset — not verifiable here)

1. **Lever 1:** connect, enable *Sharper center*, raise the strength slider. The image center
   should get visibly sharper and the extreme periphery softer, with **no change** in encoded
   size, bitrate, or the Defoveate/Statistics meters' size figures — and it should change
   **immediately, without reconnecting**.
2. **Guardrail:** turn on reduced-resolution streaming at a low streaming resolution, then push
   strength to 100%. The periphery should soften but **not** collapse into large blocky FSR
   blocks; compare against the same strength at full streaming resolution (periphery allowed to go
   steeper).
3. **Lever 2:** enable *Adaptive foveation* with the automatic bitrate on, then force a Wi-Fi dip
   (move away from the AP / add contention). The center should hold while the periphery visibly
   softens over a second or two as the controller backs off, and recover slowly when the link does
   — no frame-to-frame popping.
