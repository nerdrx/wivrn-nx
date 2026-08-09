# Separate quad-layer streaming (WiVRn NX)

Status: recon complete, decisions below.

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
