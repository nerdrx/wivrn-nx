# Adaptive round colour foveation (experimental)

Enable `NX_DIRECT_NATIVE_CENTER=1` on the server. Requires stereo direct mode, LZ4 and safety, at least 256×256 encoded pixels per eye, and the updated client. Default remains off.

The existing foveation pass captures RGB888 before chroma averaging into a 256×256 container per eye. The host blends this with the exact decoded baseline using a circular quintic falloff. At 700 Mbit/s total/90 Hz the native core is 128 pixels across, with a 63-pixel fade on each side. This is a colour enhancement within existing renderer foveation, not a new projection map. Native spatial sharpness requires a 1:1 source map; the source-footprint log measures it.

The encoder first allocates safety bandwidth, then chooses radius from remaining bits per refresh. Full radius is 127 at 680 Mbit/s detail/90 Hz, dropping continuously to zero at 80 Mbit/s detail/90 Hz. A tiny emerging core fades in rather than popping on. Tiles wholly outside the circular support retain baseline coding and consume no native payload. Reserve only selected native tiles before choosing the baseline plan and frame admission. High-budget expansion costs more bandwidth than the old small patch; low-budget mode removes that cost rather than just hiding detail.

No extra Pico rendering pass. Raw native tiles remain 1025 words including alignment. Stream versions 9/10 negotiate the larger capacity, with versions 7/8 still accepted at their original bounds. Frame v2 and all legacy frame validation remain unchanged. User bitrate settings are preserved.

Continuous native blending removes the added square seam; it does not eliminate baseline palette blocks or guarantee cadence. Centre size changes with bitrate. A later refinement can unify the underlying palette sampling layout as well; this change unifies budget allocation only.

Evidence: https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/adaptive-round
