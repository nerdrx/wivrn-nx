# Direct compact reconstruction and stream capture

The matching NX decoder now evaluates only the retained samples of eligible
flat PLANAR tiles and writes compact NV12 directly. It bypasses full-tile
shared reconstruction for these tiles. Ordinary INTRA centre reconstruction
and normal full-size output remain unchanged. UINT and UNORM compact readback
match all CPU-reference samples; native output also matches the CPU reference.

The client adds a diagnostic request, not a continuous recorder:

```sh
adb shell setprop debug.wivrn.nx.capture inspection1
# Reconnect, then run a streaming application.
adb pull /sdcard/Android/data/org.meumeu.wivrn.nx.warp/files/nx_capture_inspection1_0.png
adb pull /sdcard/Android/data/org.meumeu.wivrn.nx.warp/files/nx_capture_inspection1_1.png
adb shell setprop debug.wivrn.nx.capture 0
# Reconnect before timing tests to remove transfer-source swapchain usage.
```

A unique request captures both acquired stream eye layers before release to
OpenXR. Cached/released images are never captured. The copy restores the
color-attachment layout, waits for completion, invalidates mapped readback
memory, and reports PNG errors. Capture blocks the render thread and must be
excluded from performance measurements. Default has no GPU readback and no
extra transfer-source swapchain usage. Desktop builds use WIVRN_NX_CAPTURE.
RGBA/BGRA SRGB layers are supported; other formats report a capture error.

Actual Pico images now verify both stereo application outputs despite the
system tracking dialog. They do not verify final compositor output or physical
motion. The shader preserves the sharp centre; outer PLANAR cells remain coarse.

[Raw timings, pixel checks and actual eye images](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-09/compact-direct).

One native capture session failed after saving its images with an Adreno
sync-fd/fence error. It did not reproduce on the first retry. The final diagnostic
uses queue-idle synchronization rather than a temporary capture fence; it passed
a 45-second native run with two requests. This does not establish the driver's
root cause or guarantee a fix. Capture remains experimental and disabled during
all performance runs. See the evidence note for retained failure logs.
