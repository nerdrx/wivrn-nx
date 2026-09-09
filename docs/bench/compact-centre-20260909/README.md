# Experimental compact centre output

This client can consume the local NX decoder compact NV12 layout while
retaining native source coordinates for stereo projection and smoothing.
It requires the matching NX Warp `NXVC_VKD_FLAG_COMPACT_CENTRE` implementation;
it does not advertise a new network tool or change the wire format.

The only supported input is independent CT_NONE 8-bit YCbCr420 stereo,
2176 × 2176 per eye without alpha. Output storage is 1856 × 928 for both
eyes, with native 512 × 512 centres and quarter-density outer axes.
The headset still renders full-size swapchain images. This reduces stored
pixels by 81.8%; it does not reduce final presentation pixel count.

Enable `debug.wivrn.nx.planar_centre=1`, then
`debug.wivrn.nx.compact_centre=1`, and reconnect. Keep borrowed output and
peripheral smoothing enabled for the archived comparison. Set compact_centre
to 0 and reconnect to restore normal full-size storage. Default is off.
The property is captured in decoder metadata, so changing it during a session
does not reinterpret an already allocated image.

The shader uses a branchless separable map, clamps each eye independently,
and keeps the hardware NV12 sampler. Boundary arithmetic was compared with
the original piecewise map. The release APK was built and installed on Pico.
CPU-reference pixel exactness applies to compact decoder samples, not to
bilinear presentation or peripheral quality.

[Decoder tests, live measurements, scripts and figures](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-09/compact-centre).
Physical motion and visual verification remain outstanding: a Pico tracking
dialog obscured the captured headset image during the stationary live tests.

The bilinear-only candidate (`compact_centre=1`, `peripheral_smooth=0`)
measured 71.22 / 72.25 fresh updates/s in two stationary captures, versus
53.88/s for the same-APK native control. Source offset fell from 73.27 ms
to 69.98 / 69.38 ms. This trades peripheral filtering for speed; no physical
motion or image-quality win is established. Normal settings were restored
(compact 0, peripheral smoothing 1) after testing.
