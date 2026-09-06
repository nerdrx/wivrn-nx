# The reprojection pass

The client's display pass is the second-largest consumer of the Pico 4's GPU, after the
decode. From the 90 s capture on `a779904b`:

```
render: this app's own GPU pass 6.37 ms per iteration     loop 43.64/s
render: defoveate 1088x1088 per eye x2 = 2.37 Mpx/frame at scale 0.50 atlas-mode 0
render: shader path sharpness 0.00 fsr false alpha false motion false blend false
        glow 0.28 vignette 0.00 deband 1.00 lowpoly 0.00 levels 0 full-kernel false
```

**6.37 ms x 43.64/s = 278 ms/s**, against the decode's 643 ms/s. Together they are 92% of
one Adreno ring, which is why the decode's fence wait contains 5 ms of queueing
(`docs/CLIENT-DECODE-WALL.md`).

## What it is, and what it costs per pixel

It is a **fragment** shader (`client/shaders/reprojection.glsl`, `VERT_SHADER` +
`FRAG_SHADER`), not a compute dispatch. The vertex stage draws the foveation grid — a
triangle strip whose vertex count comes from `required_vertices(foveation[view])` — and
the fragment stage does the work, once per output pixel, `command_buffer.draw(...)` per
view (`stream_defoveator.cpp:1031-1032`).

The output is **2.37 Mpx per frame** (1088x1088 per eye, both eyes), which at 6.37 ms is
**372 Mpx/s**. That is the number worth staring at: an Adreno 650 blitting a texture
should be an order of magnitude above it. So the cost is per-pixel work, not fill rate.

What is actually switched on in the measured session is short: **glow 0.28 and deband
1.00, and nothing else.** Sharpening, FSR, motion smoothing, frame blending, the vignette
and the low-poly filter are all off, and `atlas_mode` is 0, so the atlas homography path
is not entered either.

That matters because it rules most of the shader out. The remaining per-pixel cost is:

1. **One YCbCr sample through a `VkSamplerYcbcrConversion`** with
   `chromaFilter = eLinear` (`nxwarp_decoder.cpp:242`), on a
   `G8_B8R8_2PLANE_420_UNORM` image. Every pixel pays this.
2. **The ambient glow**, three extra taps — but only near the edge. `ambient_glow()`
   early-returns when the pixel is outside the margin (`reprojection.glsl:432-434`), so
   the interior does not pay it.
3. **The deband dither**, arithmetic only, no taps.

## Ranked variants

Ranked by expected ms/s recovered, and every one of them is a display change: **byte
exactness is not a constraint here**, this is the picture and the compositor resamples it
again anyway.

### 1. Chroma filter — every pixel, implemented

`chromaFilter = eLinear` is the one cost every pixel pays. On Adreno a linear chroma
filter cannot use the plain bilinear path: the driver reconstructs chroma explicitly
around each sample. `eNearest` is one enum, and the quality argument against it is weak —
the chroma planes are already at half resolution and the whole layer is resampled by the
compositor during timewarp.

Behind `debug.wivrn.chroma_nearest`, read when the conversion is created, so it takes
effect on the next connection. The client logs which it used:
`nxwarp: ycbcr chroma filter nearest`.

### 2. The re-present cache — about one iteration in eight, implemented

`reduce_gpu_load` re-presents the image already in the swapchain when nothing the pass
draws has changed. It is **off by default** and was off for the whole measured session
(`0 re-presented from the cache`).

The loop turns 43.64 times a second and 38.5 decoded frames arrive, so ~12% of iterations
redraw an image identical to the last one, at 6.37 ms each: **~33 ms/s for no new code.**

Its dirty check is already conservative — extents, alpha, scale, bias, sharpness, CAS
kernel, FSR, vignette, glow, deband, motion step and frame, blend weight, GUI state and
every stream's frame index — and it refuses when a promoted quad is on screen.

I have **not** flipped the default. It is documented as experimental, the saving is the
smallest of the three, and I cannot see the picture from here. What is implemented is the
override, `debug.wivrn.reducegpu`, polled once a second so both halves can be measured
inside one session.

### 3. Render resolution — already exists, already on

The obvious "render the pass smaller and let the compositor upscale" is `defoveate_scale`,
and it is already doing exactly that: **auto resolved to 0.50** in the measured session,
which is what makes the pass 1088x1088 per eye instead of the panel's 2176x2176. The
comment in `resolve_defoveate_scale()` records the measurement: 2160x2160 per eye at
8.4 ms against 1080x1080 at 2.7 ms.

There is no second knob to add here. What is left is the floor: the scale is clamped to
0.4, and whether below that is usable is a picture question for the device.

### 4. Skip pixels the lens never shows — NOT implemented, and why

This is the structurally interesting one and I have deliberately not shipped it.

The right shape is not a `discard` — on a tiler, discard defeats early-Z and can cost more
than it saves — but dropping grid cells that are entirely outside the visible region
before they are ever rasterised. The geometry is already generated per frame from the
foveation parameters (`stream_defoveator.cpp:817-874`), so the seam exists.

What does not exist is the mask. `client/utils/view_geometry.h` is not in the tree yet.
Guessing it is not acceptable: the naive guess is the inscribed circle, which would drop
21.5% of the pixels, and if the lens actually shows part of those corners the result is a
**visibly cropped image** — which I cannot check, because this was written without the
headset. A wrong mask here does not degrade gracefully; it cuts the picture.

So this waits for `view_geometry.h`, and then it is worth roughly what the mask says it is
worth: 20% of 278 ms/s is ~56 ms/s, the largest item on this list.

## What to measure when the device is free

All three switches are in one build, which is the point:

```
adb shell setprop debug.wivrn.chroma_nearest 1   # takes effect on reconnect
adb shell setprop debug.wivrn.reducegpu 1        # takes effect within a second
```

The numbers to compare are the ones the log already prints: `this app's own GPU pass` and
the loop rate for the pass itself, `gpu duty` for the decode beside it, and the pose age
and `queue` term to see whether relieving the ring shortens the decode's fence wait — the
prediction of the 92% finding is that it should.
