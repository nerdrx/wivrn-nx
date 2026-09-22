# Adaptive round colour foveation (experimental)

Enable `NX_DIRECT_NATIVE_CENTER=1` on the server. Requires stereo direct mode, LZ4 and safety, at least 256×256 encoded pixels per eye, and the updated client. Default remains off.

The existing foveation pass captures RGB888 before chroma averaging into a 256×256 container per eye. The host blends this with the exact decoded baseline using a circular quintic falloff. At 700 Mbit/s total/90 Hz the native core is 128 pixels across, with a 63-pixel fade on each side. This is a colour enhancement within existing renderer foveation, not a new projection map. Native spatial sharpness requires a 1:1 source map; the source-footprint log measures it.

The encoder first allocates safety bandwidth, then chooses radius from remaining bits per refresh. Full radius is 127 at 680 Mbit/s detail/90 Hz, dropping continuously to zero at 80 Mbit/s detail/90 Hz. A tiny emerging core fades in rather than popping on. Tiles wholly outside the circular support retain baseline coding and consume no native payload. Reserve only selected native tiles before choosing the baseline plan and frame admission. High-budget expansion costs more bandwidth than the old small patch; low-budget mode removes that cost rather than just hiding detail.

No extra Pico rendering pass. Native pixels are now packed RGB565: 515 words per 32×32 tile including alignment, versus 1025 words for RGB888. This preserves spatial resolution with at most four 8-bit code values of channel error from rounding. Stream versions 11/12 negotiate packed native pixels, with versions 7–10 still accepted at their original bounds. Frame v2 and all legacy frame validation remain unchanged. User bitrate settings are preserved.

Continuous native blending removes the added square seam; it does not eliminate baseline palette blocks or guarantee cadence. Centre size changes with bitrate. A later refinement can unify the underlying palette sampling layout as well; this change unifies budget allocation only.

Evidence: https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/adaptive-round

The packed format nearly halves native tile storage; it does not halve whole-stream traffic. Actual compression ratios depend on scene content. The source capture remains RGB888, and baseline palette samples are unchanged.

## Lossless compression option

`NX_DIRECT_ZSTD=1` enables Zstd level 3 for the detail frame, only when its complete envelope is at least 10% smaller than the existing LZ4/HC candidate. Otherwise the encoder retains LZ4/raw fallback. Safety stays independent and uses the existing LZ4 path. Decoder output is byte-identical before the usual frame validation.

`NX_DIRECT_NATIVE_RGB888=1` preserves original RGB888 precision as well as resolution. This is the strict visual-quality reference mode used for the overnight integration test. Omitting it uses the separately tested RGB565 representation, whose additional savings trade a small amount of colour precision.

Stream 13/14 negotiates RGB888+Zstd; 15/16 negotiates RGB565+Zstd. The NXDZ envelope includes explicit decoded and compressed sizes; content length, frame boundaries and decoded capacity are checked. A pinned Zstd 1.5.7 is built on both host and Android with its license included. No temporal frame dependencies are introduced.
