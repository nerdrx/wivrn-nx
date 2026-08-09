# Motion-vector frame extrapolation (WiVRn NX)

Status: recon complete, decisions below.

## Problem

When the application renders far below display rate (e.g. a CPU-bound VRChat instance at
10 fps on a 90 Hz headset), head *rotation* stays smooth (the client reprojects the last frame
against fresh head poses) but position, animation and hands judder at app rate. Goal:
synthesize plausible intermediate frames on the client by warping the last decoded frame along
the scene's motion, so perceived motion approaches display rate.

## Approach

The server's video encoder already performs motion estimation on every frame. Export a coarse
motion-vector field (block grid, e.g. 16x16, quantized), send it as small per-frame metadata
beside the video stream, and let the client warp the last frame along scaled vectors for each
display refresh until the next real frame arrives.

Not in scope: app-supplied motion vectors (XR_FB_space_warp — VRChat does not provide them),
client-side optical flow (Adreno budget), and any change to the video bitstream itself.

## Decisions (from recon)

1. **MV source: we compute it ourselves, server-side.** Encoder MVs are ruled out: x264's
   public API exports none, Vulkan Video has no ME output, VAAPI's FEI/Stats entrypoints are
   Intel-only (WiVRn's VAAPI users are AMD) and unreachable through FFmpeg, and NVENC's
   ME-only mode — the single working API — would gate the feature to NVIDIA *and* still
   requires an explicit N-vs-N-9 pass, because at low app fps the encoder sees duplicate
   frames and its in-encode vectors are ~zero. Instead: a compute pass in the compositor
   (which already runs compute and holds current + previous composited images), block
   matching across the *real app frame boundary* — detected via duplicate layer content /
   unchanged app pose in view_info. Universal across all encoders. Run it only when the app
   is actually below stream rate (the client's `times_displayed` feedback and duplicate
   detection make this cheap to gate).
2. **Transport: a new `to_headset` packet** keyed by frame_idx (protocol hash bumps — fine
   for this fork). Grid at 64 px blocks ≈ 3.5 KB both eyes ≈ 2.5 Mbit/s at 90 Hz; a dropped
   MV packet degrades to "no extrapolation this frame". Riding inside view_info would shrink
   shard 0's video payload — rejected.
3. **Warp insertion: extend the defoveation pass.** The client submits its layer with the
   server's render pose and lets the runtime timewarp; on repeat vsyncs Pico already re-runs
   defoveation (discard_frame=false), so the warp is incremental work there. The decoded
   image is an external-format YCbCr texture (sample-only) — the warp is a raster pass
   displacing the existing vertex grid / UVs by t-scaled vectors, not a compute pass (the
   client currently has zero compute plumbing). On Quest-family HMDs repeat vsyncs currently
   cost nothing (frame discard) — extrapolation re-enables submission there, a real added
   cost to document.
4. **The existing "Application SpaceWarp" toggle is not spacewarp** — it only sets
   fps_divider=2 (server encodes at half rate; no synthesis anywhere in WiVRn; the runtime
   never synthesizes because no motion/depth layers are submitted). Independently of this
   feature: rename/re-describe that setting honestly. The new "Motion smoothing" toggle is a
   separate setting.

## Client warp sketch (independent of decisions)

- Maintain last decoded frame + its MV grid + its render pose.
- Per display refresh without a fresh frame: t = elapsed / frame_interval; sample the frame
  with UVs displaced by t-scaled motion vectors (inpainting disocclusions by clamped stretch —
  accepted artifact), then the normal head-pose reprojection runs on top.
- Reset warp accumulation on every real frame and on IDR.
- Cap extrapolation (e.g. max 3 synthesized frames per real frame at 10 fps → effective 40;
  beyond that artifacts outweigh smoothness — tunable).

## UX

Headset toggle (streaming settings): "Motion smoothing", default OFF initially (artifacts are
content-dependent), plus an intensity/frame-cap setting if cheap. Per the NX rule, fully
controllable from the headset.

## Staging

- Stage 1: server exports MVs from the easiest encoder (likely x264) + protocol field +
  client-side debug visualization of the field (no warp yet) — proves the data end to end.
- Stage 2: client warp pass + toggle.
- Stage 3: additional encoders / fallback ME, tuning.

## Test plan

Linux client + `WIVRN_DUMP`-style capture; force the app to a low fps (e.g. a test OpenXR
app with a frame limiter) and compare judder with/without; MV field visual debug overlay.
