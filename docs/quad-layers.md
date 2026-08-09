# Separate quad-layer streaming (WiVRn NX)

Status: v1 implemented — one opaque quad layer is promoted to stream 3 and composited
on the headset as a real `XrCompositionLayerQuad`. Untested on hardware. See
"Implementation (v1)" at the end for what was built, what it deviates from, and what
is still open.

## Problem

Overlay UIs (wlx-overlay-s / WayVR panels, SteamVR dashboards) are composited server-side into
the eye images and then travel through the lossy world video (foveation + 4:2:0 + quantization
+ reprojection resampling). Text that would be pixel-sharp rendered natively on the headset
arrives soft. Goal: stream selected quad layers as their own high-quality, low-framerate
streams and composite them on the client at display time — sharp and reprojection-stable.

Explicit non-goal: VRChat's in-world menus (baked into the projection layer by the app; no
layer to separate).

## Decisions (from recon)

1. **Layer intake: viable.** Overlay clients (XR_EXTX_overlay is enabled; xrizer overlays are
   ordinary OpenXR clients) reach `compositor::layer_commit` as distinct quad layers via
   Monado's multi-client compositor; flattening happens in `layer_squasher::do_layers` — that
   is the promotion point. Bonus: any quad currently disables the single-projection fast path
   and can grow the encoded FOV (off-axis quads cost world resolution); promoting quads out
   restores both.
2. **Stream multiplicity: the framing is ready, the geometry model is not.** stream_item_idx
   is a uint8 and the client drops unknown stream indices gracefully; the "3" is mechanical in
   ~8 places. The real work: quads need their own encode image (not layer 4 of the shared
   arrayLayers=3 image) and per-stream {width, height, codec} in video_stream_description.
   The alpha stream is the working precedent for per-stream codec/bitrate/gating — but not
   for independent resolution or cadence.
3. **Quad encode format: a video encoder per quad** (hardware decode, existing shard path),
   high bitrate weight, v1 at lockstep cadence. There is NO damage tracking anywhere today —
   a static quad re-encodes at 90 fps; v2 adds emit-on-change using the (swapchain ptr,
   image_index, rect, pose) tuple as the damage proxy, which requires the pacing/IDR
   bookkeeping (common_frame's exact-index join, idr_handler frame windows) to tolerate
   sparse frame indices — the second-hardest part of the project.
4. **Client composition: reuse the existing quad machinery.** The client already submits its
   GUI as XrCompositionLayerQuad via scene::add_quad_layer with pooled swapchains; the
   streamed quad is decode → blit (immutable YCbCr sampler, same pattern as the defoveator) →
   add_quad_layer with the server-supplied pose. The runtime then timewarps the panel at
   90 Hz — the pose-stability win that motivates the feature. Pose must be resolvable
   client-side including view-space (head-locked) quads and recentering offsets.
5. **Layer attribution is the ugliest part**: OpenXR gives no stable cross-frame layer
   identity and provenance is erased before WiVRn sees the stack; robust selection likely
   needs a small Monado patch (patches/monado/ already carries nine), with
   swapchain-pointer heuristics as the cheap v1.

## Minimal viable slice

One opaque quad layer, fixed max resolution, content updated at ≤ 30 fps or on-change,
composited client-side with live pose. Multiple quads, alpha blending, cylinder layers later.

## UX

Headset toggle: "Sharp overlay layers", default ON when available (it strictly improves
quality); per the NX rule, in the headset streaming settings. Server config mirror with a
max-layer count and per-layer resolution cap (bandwidth guard).

## Interactions

- Bandwidth: quad streams participate in the bitrate controller's ceiling (they are small and
  low-fps; account them, don't let them starve the world stream on a deep AIMD drop —
  world stream has priority, quads degrade to lower update rate first).
- Multipath: quad/control-channel content is a natural fit for the reliable path.

## Test plan

wlx-overlay-s / WayVR on the Linux client setup; A/B screenshots of a text panel at fixed
size; verify pose stability of a static panel during head motion (the reprojection win).

## Implementation (v1)

### What it does

On every `layer_commit` the compositor may pick one quad layer out of the stack, leave
it out of the squash entirely, resample it into an image of its own and encode it as
stream 3. The headset decodes it, copies it into a swapchain of its own and submits it
as an `XrCompositionLayerQuad` with the pose the server took it out of the stack with.
The runtime then holds the panel still at display rate, and the panel never goes
through foveation, eye-image scaling or reprojection.

Two things fall out of taking the layer out of the stack: the single-projection fast
path comes back when the only other layer is a projection layer, and the encoded field
of view is no longer widened by an off-axis quad.

### Selection heuristic

Candidates are quad layers that are visible in both eyes, carry no colour transform,
have a positive size, and are not asking for their alpha to be blended (see below).
Among them the largest by **on-screen solid angle** wins: area, foreshortened by how
obliquely the panel is seen, over the square of the distance from the head to it —
computed in the space the layer lives in, so a head-locked layer is measured against
the origin. It is the criterion that matches "the panel the user is looking at", and
it moves smoothly, so the choice does not flicker between frames. On top of that the
layer promoted on the previous commit gets a 25% bonus: switching costs a key frame,
so a challenger has to be clearly bigger, not just bigger.

### Opacity: what wlx panels actually submit — unverified

**This is the open question of the slice.** It was not possible to observe what
wlx-overlay-s / WayVR set for `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
without the hardware, so v1 takes the conservative side: a layer that asks for source
alpha blending is **not** promoted and stays baked, because stream 3 carries colour
only and a translucent panel would come out opaque — a visible regression, whereas not
promoting is exactly today's behaviour.

If wlx does set the flag on panels that are in fact solid rectangles (which is likely:
overlay toolkits tend to set it unconditionally), the feature does nothing until the
server configuration says otherwise:

```json
"quad-layers": { "max-size": 1024, "allow-blended": true }
```

Deciding this properly needs either a GPU reduction over the layer's alpha channel (an
extra pass and a readback, one frame late) or a hint from the overlay through a Monado
patch. Both are v2.

### Wire format

`video_stream_description` gains `codec[3]` for the quad stream plus its own
`quad_width` / `quad_height`, and a `stream_size(index)` accessor that every decoder
now uses instead of deriving its size from the eye size. Zero size means "no quad
stream", and it is what tells the headset not to create a decoder for it. The eye and
alpha streams keep deriving their geometry from the single `width` / `height` pair, so
nothing about them changed on the wire.

The placement travels on the first shard of stream 3, as
`view_info_t::quad_info_t`: pose, size in meters, a `head_locked` flag saying whether
the pose is in the world or the view space, and the source rectangle — the part of the
encode image that holds picture. The encode image is a fixed square (default 1024,
`quad-layers.max-size`); the panel fills the largest sub-rectangle of it that keeps its
aspect ratio and does not upscale the source, and the rest is black. That is how a
panel can be resized at any time without reallocating anything on either side.

`tests/quad_stream_test.cpp` covers both round trips.

### Bitrate

The quad stream's pixels are weighted like eye pixels (factor 1.0, versus 0.05 for the
passthrough alpha plane): it exists to keep text legible, so discounting it would
defeat the purpose. At the default cap against two 2000x2000 eyes that is about 11% of
the ceiling. Because that share is reserved for the whole session, the encoder is only
created when the headset asked for the feature at connection time — a headset with the
toggle off pays nothing, exactly as before. Turning the toggle off mid-session stops
promotion immediately; turning it on takes effect on the next connection.

The AIMD bitrate controller still only looks at streams 0-2: the quad stream is silent
whenever no layer is promoted, and the feedback the headset sends for the frames it
skipped would be read as packet loss.

### Falls back to baking when

- the headset's "Sharp overlay layers" toggle is off (per frame), or was off when the
  session was set up (no encoder, no bitrate share)
- `quad-layers` is disabled or `max-size` is 0 in the server configuration
- the layer stack contains no quad layer
- the only quad layers are alpha-blended (unless `allow-blended`), single-eye, colour
  transformed, or degenerate
- anything throws on the headset while composing the panel (the pass and swapchain are
  dropped and the frame is shown without the layer)

In all of those the compositor takes exactly the path it took before this feature
existed.

### Deviations from the plan above

- **Cadence.** v1 is strictly lockstep, as planned: the quad stream uses the same frame
  indices as the eyes and re-encodes at stream rate. Emit-on-change is v2. The client
  side of the gap handling is in place already (the stream is joined by exact frame
  index and its absence never holds the eyes back), and a latent truncation in
  `shard_accumulator` that would have misfiled a frame after a gap of exactly 256 was
  fixed along the way.
- **Colour space.** The layer is sampled through the same view the squasher uses and
  re-encoded to sRGB before the YCbCr conversion, which assumes the application's
  swapchain is sRGB. That is the assumption the existing single-projection fast path
  already makes.
- **Encoder array width.** All four streams share the encoder machinery; the array
  entry for a stream the session does not use is simply disabled, and every loop over
  encoders skips the null. `video_encoder` gained a `src_layer` because the three eye
  streams read their own layer of the shared image while the quad stream has an image
  of its own and reads layer 0.

### Still open

- multiple simultaneous panels (one stream each, or an atlas)
- alpha, and with it wlx panels that really are translucent
- emit-on-change, which is where the bandwidth win is
- cylinder layers, which wlx also uses for curved panels
