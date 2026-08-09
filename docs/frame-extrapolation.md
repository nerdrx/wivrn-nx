# Motion smoothing (WiVRn NX)

Status: implemented. Headset selector "Motion smoothing" on the streaming settings page,
*Off* / *Headset* / *Server (experimental)*, off by default. One server-side estimator
feeds both active modes; *Headset* sends the field as a `to_headset::motion_field` packet
and the client warps in its defoveation pass, *Server* warps on the PC before encoding and
sends nothing extra.

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
2. **Transport: a new `to_headset` packet** on the stream socket, keyed by frame index, cut
   into datagram-sized chunks. The protocol hash bumps — fine for this fork. A dropped chunk
   degrades to "no smoothing this interval". Riding inside `view_info` would shrink shard 0's
   video payload — rejected.
3. **Warp insertion: extend the defoveation pass.** The client submits its layer with the
   server's render pose and lets the runtime timewarp; the warp happens in image space
   underneath that. The decoded image is an external-format YCbCr texture (sample-only), so
   the warp is a texture coordinate displacement in the existing raster pass, not a compute
   pass (the client has no compute plumbing).
4. **The old "Application SpaceWarp" toggle was not spacewarp** — it only sets
   `fps_divider=2`. Renamed to "Half framerate mode" and re-described honestly. The config
   field keeps its name (`fps_divider`) for compatibility.
5. **Where the warp runs is a user choice, not a design decision.** The two ends trade
   different things (see *Server mode* below) and which trade is better depends on the
   headset, the link and the content, so both are shipped and the user picks. The mode is
   `wivrn::motion_mode` on the wire, in an `std::optional` beside the original boolean; the
   boolean is kept in step with it so anything reading the plain switch still works.

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
wait, and the payload is a few kilobytes at application rate, split into four to six chunks —
at most a few hundred packets per second, and typically fifty. They are datagrams on the
stream socket in the normal case. In TCP-only mode it shares the control socket, where a
full send buffer would stall the compositor; if that ever shows up in practice, hand the
readback to the encoder thread instead.

## Protocol

    struct to_headset::motion_field
    {
        uint64_t frame_idx;         // video frame the field starts from
        XrTime   span_ns;           // interval it spans, headset time, > 0
        uint16_t width, height;     // cells per eye, of the whole field
        float    scale;             // longest vector, as a fraction of the eye image
        uint8_t  view;              // eye this chunk belongs to
        uint16_t row_offset;        // first grid row of that eye carried here
        uint16_t row_count;         // rows carried here
        std::vector<int8_t> vectors; // (x, y) per cell, row major, this chunk's rows only
    };

Cell (i, j) is centred at ((i+0.5)/width, (j+0.5)/height) in normalized coordinates of the
*defoveated* eye image. A cell value v means a displacement of (v/127)·scale: what is now at p
was at p − (v/127)·scale, span_ns ago. In the reassembled field the index is
((view·height + j)·width + i)·2; within a chunk it is ((j − row_offset)·width + i)·2.

A whole field at 27x29 cells is 3.1 kB of vectors, and 32x35 cells gives 4.5 kB — more than
one datagram may carry, and well past the 2048 byte slots the headset reads datagrams into
(anything larger is dropped outright). So a field goes out as several chunks of whole grid
rows of one eye, at most `max_chunk_bytes` (1024) of vectors each, which keeps a packet
comfortably under any MTU worth worrying about. Every chunk repeats the whole header, so
chunks are self-describing and may arrive in any order; the headset gathers them per frame
index, drops a partial assembly as soon as a chunk of a newer frame arrives, and only warps
along a field it holds every row of. Four packets per application frame at 27x29, six at
32x35.

Bandwidth is unchanged by the chunking beyond the repeated headers: about 250 kbit/s at
10 fps, 2.8 Mbit/s in the worst case where the feature is active at 90 Hz.

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

## Server mode

Same estimator, same field, same warp — moved to the other end of the link. On a commit the
application produced nothing new for, the server warps the last real composited frame forward
and encodes *that*, so what leaves the PC is a stream of genuinely different pictures and the
headset does nothing special with any of them.

Everything above about detecting a new application frame, the gating ratio and the block
matcher is unchanged: the estimator needs pairs of real frames either way. What changes is
what happens to the result — the field is not sent, the readback into host visible memory is
not even recorded, and the vectors are read straight out of the device local buffer the
matching pass wrote.

### The two images

`motion_warper` (`compositor/motion_warper.{h,cpp}`) owns two `R8G8B8A8_SRGB` images, each
the size of a composited eye view with one array layer per eye:

- **retained**, written on an application frame by `motion_retain.comp`, which resamples the
  composited views through the same mapping `motion_downsample.comp` uses — the shared
  `motion_axis_mapping()`, mirroring included. What lands in it is therefore the defoveated
  eye image in the orientation the headset displays, which is exactly the space the motion
  vectors live in;
- **output**, written on a duplicate commit by `motion_warp.comp` and read by the foveation
  pass instead of the live composited image.

Both are written through an UNORM view and read through an sRGB one, the arrangement the
layer squasher's render target already uses: the shader encodes, the sampler decodes, and a
reader sees the same linear values it would have sampled from the composited image itself.
Eight bits of sRGB is what the encoder is going to get anyway.

    1728 · 1824 · 4 B · 2 eyes · 2 images ≈ 50 MB

allocated, like the estimator's pyramids, only once the headset asks for *this* mode **and**
the application has fallen behind, and freed again the moment either stops being true.

### The warp

`motion_warp.comp`, one invocation per output pixel:

    p = (coord + 0.5) / size            // normalized, in the eye image
    d = bilinear(field, p) · t
    out(p) = retained(clamp(p - d, 0, 1))

`t` is `motion_warp_step()` from `common/motion_field.h`, the same four lines the headset
runs: how far past the retained frame's display time this commit's predicted display time
falls, in units of the interval the field spans, clamped to `[0, MOTION_MAX_STEPS]` — the
same 3 the client caps at, and now a constant in `motion_constants.glsl.inc` so the two
cannot drift apart.

Two things are simpler here than on the headset. The vectors are read as the floats the
matching pass wrote, with no quantisation and no packet in between, so the whole `scale` and
`int8` business does not exist. And the image being sampled is the retained eye view rather
than a foveated one, so there is no foveation Jacobian to undo: field and image share one
coordinate system. Coordinates outside the image are clamped, which fills disocclusions by
stretching the edge — the same artefact the headset-side mode accepts.

Bindings: 0 the retained image as a `sampler2DArray` (sRGB view, linear filter, clamp to
edge), 1 the estimator's vector buffer as a read-only storage buffer, 2 the output image as
a write-only `image2DArray` (UNORM view). Push constants: eye size, grid size, `t`.

### What a warped commit says about itself

The picture it carries is the *retained* application frame moved forward, so the commit
describes that frame: `view_info.pose` and `view_info.fov` are the retained ones, and the
foveation pass is handed the retained field of view with an identity source rectangle and
`flip_y` false. The head having moved since is the runtime's timewarp's problem on the
headset — which is exactly the deal the headset-side mode already makes with the frame it is
holding.

That is a real trade in the squashed path: without the warp, a duplicate commit re-squashes
the same layers against the fresh head pose, so the server reprojects and the headset's
timewarp has less to do. Warping hands the older pose over instead. In the single-projection
fast path there is nothing to lose — the composited image *is* the application's eye image
and its pose does not change between replays.

The alpha (passthrough) plane travels inside the retained RGBA image and is warped with the
colour, because it is the same composited pixel and a mask that does not follow what it masks
is worse than one that does. The promoted quad stream is untouched: it is built from the live
layer as usual, never warped, and a commit that would promote a *different* quad than the one
the retained frame was composited around is simply not warped at all — the retained image was
built with that layer taken out.

An IDR landing on a synthesized frame is fine and is deliberately not special-cased. A warped
frame is a complete picture; the decoder cannot tell, and the next real frame refreshes it a
fraction of a second later. `request_idr()` and the encoders' own key frame logic never look
at where the pixels came from.

### Command buffer and barriers

The warp joins the compositor's existing command buffer between the squash / fast path and
the foveation pass, with its own timestamp pair (it shows up as "motion warp" in the Monado
debug UI, next to "foveation" and "motion estimation"). The dependencies:

- **retained image**: `Undefined → General` before the copy (source scope
  `ComputeShader / ShaderSampledRead`, which is what keeps the write behind the previous
  commit's warp), then `General → ShaderReadOnlyOptimal` after it. That release is what makes
  the frame visible to every warp until the next retain — a barrier's second synchronisation
  scope covers everything after it in *submission* order, not only the rest of its command
  buffer.
- **vector buffer**: the estimator's post-dispatch barrier now names both consumers, transfer
  read (for the host copy, when there is one) and compute read (for the warp, in a later
  submission). A second barrier before the matching dispatch turns the write-after-read
  around, so a new estimate cannot overtake the warps still reading the old vectors.
- **warp output**: `Undefined → General` before the dispatch, `General →
  ShaderReadOnlyOptimal` after it, both inside the one command buffer the foveation pass is
  in.

The compositor's timeout flag suppresses the warp and the retained copy the same way it
suppresses the estimator: while `motion_unsafe` is set the mode reads as `off` for the
commit, nothing is recorded and nothing is destroyed.

### The bitrate trade

This is the whole reason the mode is a choice and not a replacement.

In headset mode the duplicate frames on the wire are what they have always been: the same
picture, re-encoded, which every codec turns into a handful of skip macroblocks. They cost
almost nothing, so an application at 10 fps on a 90 Hz stream spends essentially the entire
bitrate on its ten real frames. Those frames are as good as the link can make them, and the
smoothing is free.

In server mode every commit carries a different picture, so every commit costs bits. The same
budget is now divided between ten real frames and eighty synthesized ones, and the real frames
get visibly less of it. What is bought with that is a headset that does no extra work at all —
no re-enabled submission on repeat refreshes, no field packets, no per-refresh pass on the
Adreno — and a warp with the full float field rather than an `int8` one, applied in the eye
image's own space rather than through a foveation Jacobian.

The timing is slightly worse, too: the server warps to a *predicted* display time chosen a
frame ahead, while the headset warps to the refresh it is actually about to scan out. The
difference is one frame period of prediction error on the extrapolation distance, which at
these ratios is a few percent of a step.

Rule of thumb: headset mode when the link is the constraint, server mode when the headset is
(or when the headset-side warp is unavailable — an old client, or a runtime that will not
re-submit repeat refreshes).

### Mode switches mid-session

The mode is read from the live settings on every commit, so it changes with no reconnect.

- **headset → server**: fields stop being sent; the client's assembler keeps whatever it had,
  which never again names the frame on screen, so it stops warping on its own. The server
  starts retaining on the next application frame and warping once the estimator has produced
  a field across two real frames — two application frames, so a fifth of a second at 10 fps.
- **server → headset**: the warper is destroyed and its 50 MB freed on the next commit, the
  retained state is dropped, the readback and the packets resume.
- **either → off**, **the application catching up**, or **the estimator/warper going away for
  any other reason**: same path, everything is freed and the commits go back to being byte
  for byte what they were before the feature existed.
- **warper creation fails**: logged once and the mode falls back to *headset* for the rest of
  the session, rather than losing the feature. The estimator failing is still fatal to both
  modes, as before.

## Failure modes

- **Wrong vector.** A local smear for the length of one application frame. The field is
  clamped to a quarter of the image, and the search itself cannot report more than ±96 render
  pixels.
- **Lost field packet.** The client ignores any field that does not name the frame it is
  displaying, and an incomplete one just as much, so a lost, late or duplicated chunk simply
  means no smoothing until the next application frame.
- **IDR, resolution change, reconnect.** Same mechanism: the frame index stops matching.
  `compositor::resume()` also drops the previous pyramid so the first field after a pause is
  never computed across the gap.
- **Application catches up.** The ratio crosses 0.9, the estimator is destroyed and its
  memory freed. Nothing on the client changes: fields simply stop arriving.
- **Estimator creation fails.** Out of memory, a missing shader, anything: it is logged once
  and the feature is held off for the rest of the session. Nothing about it would fix itself
  on the next commit, and the filtered ratio climbs back over the threshold within a fraction
  of a second, so retrying would mean the same warning several times per second forever.
- **Compositor timeout.** The submission may still be executing the estimator's pipelines
  against its pyramids, so until a later semaphore wait succeeds nothing is recorded into the
  estimator and, above all, it is not destroyed. One frame without an estimate, no device
  wait on the hot path.
- **Toggle off.** No packets, no estimator, no warper, no client-side texture sampling. The
  pass is byte-identical to what it was before this feature.
- **Server mode, no field yet.** The first application frame after the mode is entered, after
  a pause or after a resolution change has nothing to be matched against, so the duplicates
  that follow it go out unwarped — the ordinary repeat. One application interval later there
  is a field and the warp starts.
- **Server mode, the promoted quad changes.** The retained image was composited with that
  layer taken out; a commit that would promote a different one is left unwarped rather than
  shown the wrong picture.

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

`tests/motion_field_chunk_test.cpp` covers the rest of what can be checked without a GPU: the
chunking and reassembly, `motion_warp_step` (both ends run it, so the caps and the sign
conventions are checked once), and the mode selector — `effective_motion_mode` resolving a
settings packet, and the packet and the transport status surviving a round trip with the new
fields.

Not covered offline: the downsample pass, the Vulkan plumbing, the shared memory reduction
(the packed key is what makes the reduction order irrelevant), and the whole server-side warp
path — the retain resample, the field sampling, and the barriers that carry the retained image
and the vector buffer across submissions.

For end-to-end testing: force an application to a low frame rate (a test OpenXR app with a
frame limiter), watch the Monado debug UI for the "motion estimation" timing, and compare
judder with the toggle on and off.
