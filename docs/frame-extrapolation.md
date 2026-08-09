# Motion smoothing (WiVRn NX)

Status: implemented. Headset toggle "Motion smoothing" on the streaming settings page,
off by default. Server-side estimator, `to_headset::motion_field` packet, client-side
warp folded into the defoveation pass.

## Problem

When the application renders far below display rate (e.g. a CPU-bound VRChat instance at
10 fps on a 90 Hz headset), head *rotation* stays smooth (the client reprojects the last frame
against fresh head poses) but position, animation and hands judder at app rate. Goal:
synthesize plausible intermediate frames on the client by warping the last decoded frame along
the scene's motion, so perceived motion approaches display rate.

## Approach

The server computes a coarse motion field between two consecutive *distinct* application
frames with a compute pass in the compositor, sends it as a small packet beside the video
stream, and the client warps the last decoded frame along t-scaled vectors on the refreshes
the application produced nothing new for.

Not in scope: app-supplied motion vectors (XR_FB_space_warp — VRChat does not provide them),
client-side optical flow (Adreno budget), and any change to the video bitstream itself.

## Decisions (from recon)

1. **MV source: we compute it ourselves, server-side.** Encoder MVs are ruled out: x264's
   public API exports none, Vulkan Video has no ME output, VAAPI's FEI/Stats entrypoints are
   Intel-only (WiVRn's VAAPI users are AMD) and unreachable through FFmpeg, and NVENC's
   ME-only mode — the single working API — would gate the feature to NVIDIA *and* still
   requires an explicit N-vs-N-9 pass, because at low app fps the encoder sees duplicate
   frames and its in-encode vectors are ~zero.
2. **Transport: a new `to_headset` packet** on the stream socket, keyed by frame index. The
   protocol hash bumps — fine for this fork. A dropped packet degrades to "no smoothing this
   interval". Riding inside `view_info` would shrink shard 0's video payload — rejected.
3. **Warp insertion: extend the defoveation pass.** The client submits its layer with the
   server's render pose and lets the runtime timewarp; the warp happens in image space
   underneath that. The decoded image is an external-format YCbCr texture (sample-only), so
   the warp is a texture coordinate displacement in the existing raster pass, not a compute
   pass (the client has no compute plumbing).
4. **The old "Application SpaceWarp" toggle was not spacewarp** — it only sets
   `fps_divider=2`. Renamed to "Half framerate mode" and re-described honestly. The config
   field keeps its name (`fps_divider`) for compatibility.

## Server

### Detecting a new application frame

Monado's multi-client compositor (`comp_multi_system.c:multi_main_loop`) drives
`layer_commit` at the *native* compositor's rate — the stream rate — replaying whatever
layers the client last submitted. At 10 fps on a 90 Hz stream the compositor therefore sees
about nine replays per real frame, and nothing in the frame ids distinguishes them: the
`frame_id` in `layer_accum.data` is the native compositor's and increments every commit.

What does distinguish them is the layer stack itself, replayed byte for byte from the
client's delivered slot. `layer_fingerprint()` in `compositor.cpp` hashes the layer count,
each layer's `xrt_layer_data` (type, flags, the client's timestamp, per-view sub-image index
and array index, pose, fov) and each layer's swapchain pointers. A change means a new
application frame.

Limitation: an overlay client submitting at full rate makes every commit look new, and the
feature stays idle. That is the right answer — the composited image really does change every
frame then.

The same signal feeds a low pass filtered ratio of new frames to commits, which is the
application frame rate over the stream frame rate. The estimator only runs below 0.8 and
switches off again above 0.9.

### Block matcher

Runs at application rate, not stream rate, so robustness matters far more than either
quality or speed. Constants live in `compositor/shaders/motion_constants.glsl.inc`, a file of
nothing but `#define`s included by both compute shaders, by `motion_estimator.cpp` and by the
test — there is one copy of every number.

`motion_downsample.comp` builds a three level luma pyramid of both composited eye views in a
single dispatch: level 0 at 1/4 of render resolution, then 1/8 and 1/16. Every level is a box
average computed straight from the source with bilinear taps rather than by chaining 2x
reductions, which costs about three times the taps but needs no barrier between levels. The
source rectangle is mapped exactly the way `foveation.cpp` maps it, mirroring included, so the
pyramid is in the orientation of the image the headset displays.

`motion_estimate.comp` runs one work group of 64 threads per cell. Cells are 64 render pixels
nominally; the grid is `round(size / 64)` and covers the image exactly, so at 1728x1824 per eye
it is 27x29 = 783 cells. For each cell:

- coarsest level (1/16): exhaustive search of ±6 texels — ±96 render pixels — with an 8x8
  texel window centred on the cell. The window is deliberately wider than the cell stride;
  overlapping windows are far more stable than disjoint ones.
- levels 1 and 0: refine the propagated vector within ±2 texels.
- a parabolic fit on the level 0 costs at the winner and its four neighbours recovers a
  sub-texel position.

The cooperative search ranks candidates by a single packed key — cost in the top bits, then
vector length, then candidate index — so the shared memory reduction order cannot change the
result. That is what makes the CPU mirror in `tests/motion_estimator_test.cpp` meaningful.

Cost per cell: 169 + 25 + 25 candidates plus 4 for the fit, at 64 samples each, spread over 64
threads. About 22M texel reads for both eyes, well under a millisecond on a desktop GPU, and
it only happens on application frames. Timing shows up as "motion estimation" in the Monado
debug UI next to "foveation".

### Memory

Two pyramids (current and previous), R8, two array layers each:

    (W/4 · H/4) · 4/3 · 2 eyes · 2 pyramids ≈ 1.05 MB at 1728x1824 per eye

plus two 12.5 kB vector buffers. About 1.1 MB total, allocated only once the headset asks for
the feature *and* the application falls behind, and freed again as soon as either stops being
true.

The estimator keeps its own pyramids rather than retaining a composited image: the
application's swapchain image is recycled as soon as the frame is submitted, and the
compositor's own two YCbCr images ping-pong at stream rate, so neither holds the previous
*distinct* frame by the time it is needed.

### Reading back and sending

The work is recorded into the compositor's existing command buffer and read back after the
timeline semaphore wait that `layer_commit` already does, so there is no extra
synchronisation. The field is quantized against its own longest vector and sent from the
compositor thread with `send_stream`; a failure is logged and dropped.

Sending from the compositor thread is a deliberate trade: it avoids a queue and a second
wait, and the payload is a few kilobytes at application rate — at most a few tens of packets
per second, and typically ten. It is a datagram on the stream socket in the normal case. In
TCP-only mode it shares the control socket, where a full send buffer would stall the
compositor; if that ever shows up in practice, hand the readback to the encoder thread
instead.

## Protocol

    struct to_headset::motion_field
    {
        uint64_t frame_idx;         // video frame the field starts from
        XrTime   span_ns;           // interval it spans, headset time, > 0
        uint16_t width, height;     // cells per eye
        float    scale;             // longest vector, as a fraction of the eye image
        std::vector<int8_t> vectors; // (x, y) per cell, row major, left eye then right
    };

Cell (i, j) is centred at ((i+0.5)/width, (j+0.5)/height) in normalized coordinates of the
*defoveated* eye image. A cell value v means a displacement of (v/127)·scale: what is now at p
was at p − (v/127)·scale, span_ns ago. Index = ((view·height + j)·width + i)·2.

At 27x29 cells that is 3.1 kB of vectors plus a small header, once per application frame —
about 250 kbit/s at 10 fps, 2.8 Mbit/s in the worst case where the feature is active at 90 Hz.

## Client

`stream_defoveator` uploads the field into a two layer R8G8_SNORM texture (one layer per eye,
~1.6 kB) and binds it to the defoveation pipeline. The upload only happens when the field
changes; a frame with no field leaves the texture alone.

In `reprojection.glsl`, when the step is non-zero:

    p    = inPosition * 0.5 + 0.5          // position in the defoveated eye image
    v    = texture(motion_field, p).rg * scale
    jac  = (dFdx(uv.x)/dFdx(p.x), dFdy(uv.y)/dFdy(p.y))
    uv  -= step * v * jac

The field is measured in defoveated image coordinates but the pass samples the *foveated*
image. The vertex grid maps positions to texture coordinates affinely inside every triangle,
so the screen space derivatives of the two interpolated values give the local ratio between
them exactly — no foveation parameters need to be passed in, and a gaze change between the two
matched frames cannot skew the warp.

`step` is how far past the frame's display time this refresh lands, in units of the interval
the field spans: near zero on the refresh that first shows a frame, growing with every repeat,
capped at `constants::stream::motion_max_steps` (3, so 10 fps becomes an effective 40).
Displaced coordinates are clamped inside the image, which fills disocclusions by stretching
the edge — an accepted artefact.

The client's layer pose submission is untouched: the warp is in image space and the runtime's
timewarp continues to run on top of it.

On Pico the client already re-runs the defoveation pass on every refresh
(`hmd_traits::discard_frame == false`), so the warp is incremental work there. On
Quest-family headsets repeat refreshes are normally discarded and cost nothing; smoothing
re-enables submission for them, which is a real added cost and is precisely what the toggle
buys. With the toggle off, or with no field for the frame on screen, the frame is discarded
exactly as before.

## Failure modes

- **Wrong vector.** A local smear for the length of one application frame. The field is
  clamped to a quarter of the image, and the search itself cannot report more than ±96 render
  pixels.
- **Lost field packet.** The client ignores any field that does not name the frame it is
  displaying, so a lost, late or duplicated packet simply means no smoothing until the next
  application frame.
- **IDR, resolution change, reconnect.** Same mechanism: the frame index stops matching.
  `compositor::resume()` also drops the previous pyramid so the first field after a pause is
  never computed across the gap.
- **Application catches up.** The ratio crosses 0.9, the estimator is destroyed and its
  memory freed. Nothing on the client changes: fields simply stop arriving.
- **Toggle off.** No packets, no estimator, no client-side texture sampling. The pass is
  byte-identical to what it was before this feature.

## Testing

`tests/motion_estimator_test.cpp` mirrors the shader's search on the CPU and includes the same
constants file, so the two cannot drift on a tunable. Build and run:

    g++ -std=c++23 -O2 -o motion_estimator_test tests/motion_estimator_test.cpp && ./motion_estimator_test

It feeds synthetic non-repeating images with known displacements through the full hierarchy.
Current results at 1728x1824, 27x29 cells, over the 529 cells away from the image border:

    shift        mean     p99     worst
    (0, 0)       0.06 px  0.15    0.17
    (4, 0)       0.06     0.17    0.22
    (13, 7)      0.29     0.65    0.74
    (-24, 18)    0.61     1.59    1.84
    (40, -32)    0.06     0.17    0.21
    (-64, -48)   0.06     0.16    0.22

Border cells are excluded from the statistics and reported separately: content moves in from
outside the image there, clamp-to-edge replaces it with a smear, and there is no displacement
to recover. Their worst error reaches ~144 px at a 64 px shift, which is exactly the
disocclusion the client fills by stretching.

The test also checks that motion localised to one half of the image is recovered per region
(1.05 px worst away from the seam), that a displacement well beyond the search range is still
bounded by it rather than reported as something absurd, and that a featureless image yields
exactly zero.

Not covered offline: the downsample pass, the Vulkan plumbing, and the shared memory
reduction — the packed key is what makes the reduction order irrelevant.

For end-to-end testing: force an application to a low frame rate (a test OpenXR app with a
frame limiter), watch the Monado debug UI for the "motion estimation" timing, and compare
judder with the toggle on and off.
