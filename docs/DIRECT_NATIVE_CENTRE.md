# Adaptive round colour foveation (experimental)

Enable `NX_DIRECT_NATIVE_CENTER=1` on the server. Requires stereo direct mode, LZ4 and safety, at least 256×256 encoded pixels per eye, and the updated client. Default remains off.

The existing foveation pass captures RGB888 before chroma averaging into a 256×256 container per eye. The host blends this with the exact decoded baseline using a circular quintic falloff. At 700 Mbit/s total/90 Hz the native core is 128 pixels across, with a 63-pixel fade on each side. This is a colour enhancement within existing renderer foveation, not a new projection map. Native spatial sharpness requires a 1:1 source map; the source-footprint log measures it.

The encoder first allocates safety bandwidth, then chooses radius from remaining bits per refresh. Full radius is 127 at 680 Mbit/s detail/90 Hz, dropping continuously to zero at 80 Mbit/s detail/90 Hz. A tiny emerging core fades in rather than popping on. Tiles wholly outside the circular support retain baseline coding and consume no native payload. Reserve only selected native tiles before choosing the baseline plan and frame admission. High-budget expansion costs more bandwidth than the old small patch; low-budget mode removes that cost rather than just hiding detail.

No extra Pico rendering pass. Optional `NX_DIRECT_NATIVE_RGB565=1` packs native pixels as RGB565: 515 words per 32×32 tile including alignment, versus 1025 words for RGB888. This preserves spatial resolution with at most four 8-bit code values of channel error from rounding. Stream versions 11/12 negotiate packed native pixels, with versions 7–10 still accepted at their original bounds. Frame v2 and all legacy frame validation remain unchanged. User bitrate settings are preserved.

Continuous native blending removes the added square seam; it does not eliminate baseline palette blocks or guarantee cadence. Centre size changes with bitrate. A later refinement can unify the underlying palette sampling layout as well; this change unifies budget allocation only.

Evidence: https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/adaptive-round

The packed format nearly halves native tile storage; it does not halve whole-stream traffic. Actual compression ratios depend on scene content. The source capture remains RGB888, and baseline palette samples are unchanged.

## Lossless compression option

`NX_DIRECT_ZSTD=1` enables Zstd level 3 for the detail frame, only when its complete envelope is at least 10% smaller than the existing LZ4/HC candidate. Otherwise the encoder retains LZ4/raw fallback. Safety stays independent and uses the existing LZ4 path. Decoder output is byte-identical before the usual frame validation.

`NX_DIRECT_NATIVE_RGB888=1` preserves original RGB888 precision as well as resolution. This is the strict visual-quality reference mode used for the overnight integration test. RGB888 is also the default. `NX_DIRECT_NATIVE_RGB565=1` explicitly enables the separately tested RGB565 representation, whose additional savings trade a small amount of colour precision. The RGB888 override wins if both are set.

Stream 13/14 negotiates RGB888+Zstd; 15/16 negotiates RGB565+Zstd. The NXDZ envelope includes explicit decoded and compressed sizes; content length, frame boundaries and decoded capacity are checked. A pinned Zstd 1.5.7 is built on both host and Android with its license included. No temporal frame dependencies are introduced.

## Network budgeting

Compressed direct streams now settle frame admission using the complete encoded payload size, including safety and the wrapper, with the existing 25% transport/FEC allowance. Previously admission always charged the larger raw representation, so a low bitrate could skip frames even when compressed data fitted the budget. `NX_DIRECT_WIRE_ADMISSION=0` restores legacy charging for comparison. No image-quality setting changes. A large frame is charged in full and delays subsequent admission; at most one interval of scheduling credit accumulates.

`NX_DIRECT_COMPRESSION_CREDIT=1` separately enables an experimental quality planner, only for the native+Zstd path. It can spend proven compression savings on more detail, with eight-frame conservative sampling, a 1.5 cap and immediate retreat after an oversized frame. It is off by default and is not a hard per-frame size guarantee. See the [controlled results](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-23/compression-credit).

## Optional UDP tail experiment

`NX_DIRECT_TAIL_PACKETS=64` sends 64 small, discarded packets after a complete direct frame on the primary UDP path. The default is zero; the maximum is 64. It requires the updated server/client protocol pair. There are no sleeps, frame dependencies, image changes, or extra GPU passes. Secondary paths and TCP do not receive this padding.

On this Pico/access-point pair, appended packets reduced the measured first-to-last main-frame arrival span. The mechanism is not established and other networks may gain nothing. Keep this opt-in: it adds up to 5,760 packets/s at 90 Hz. Each packet is 16 bytes before WiVRn encryption framing, 24 bytes with the encrypted counter, or 52 bytes including IPv4/UDP headers: approximately 2.40 Mbit/s before link-layer overhead. Padding is excluded from frame telemetry and the codec byte budget; account for this additional traffic when comparing total bandwidth. The existing admission transport allowance is unchanged.

Short paired photo workloads showed unchanged app GPU-pass time, but one busy-scene run selected fewer fresh sources despite lower delivery delay. This is experimental transport evidence, not proof of photon latency or universal smoothness. [Measurements and caveats](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-23/photo-tail).

## Optional selection comparison

The default direct-frame selection still chooses the newest completed stereo image. The diagnostic Android property `debug.wivrn.nx.direct_nearest=1` (desktop `WIVRN_NX_DIRECT_NEAREST=1`) instead uses the existing nearest-display-target selection. It is read once per client process, so reconnect after restarting the app to change modes. Safety-image selection remains unchanged.

A paired crowded-photo run increased fresh-source selection from 87.0 to 89.15 FPS, but increased the approximate client first-arrival-to-predicted-display interval from 41.2 to 47.72 ms. This is a smoothness/age tradeoff, not a latency improvement; newest remains the default. [Numeric evidence](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-23/photo-selection).

### Exact repeat reuse and optional byte prediction

The encoder now reuses a compressed result when the complete raw frame and compression policy match exactly. The cache owns both byte arrays and compares every byte; there is no hash-only acceptance or cross-frame decoder dependency. It is enabled by default. Set `NX_DIRECT_COMPRESSION_CACHE=0` for paired measurements. Changing frames still use the normal compressor.

`NX_DIRECT_PREDICTOR=1`, together with `NX_DIRECT_ZSTD=1`, enables an experimental lossless byte predictor before Zstd. Each byte records its difference from the byte four positions earlier; the decoder restores the original bytes inside the same frame. The encoder compares ordinary and predicted Zstd and requires another 5% reduction before choosing prediction. Existing LZ4/raw fallback remains available. This spends additional PC work and adds a small decoder pass; it is off by default.

Predictor-capable stream versions are 17/18 for RGB888 and 19/20 for RGB565. NXDZ envelope version 2 denotes predicted bytes; version 1 remains ordinary Zstd. Both endpoints must support the new stream version. Old clients reject it rather than interpreting incompatible data. Frame bounds and decompressed-size validation remain enabled.

## Bounded live timing profile (23 September)

A paired Pico experiment used the existing Android settings below, with a 90 Hz
refresh rate and fixed 500 Mbit/s requested quality budget:

```sh
adb shell setprop debug.wivrn.nx.jit_max_sleep_us 5000
adb shell setprop debug.wivrn.nx.ready_wait_us 4000
```

Restart the headset app before reconnecting. The first caps JIT sleep; the second
allows a bounded wait for a completed stereo image only when presentation would
otherwise repeat, retaining the existing display-deadline reserve. Neither adds
a GPU pass. These are experimental settings, not changed global defaults.

In four 120-second duplicated-photo runs, the combined settings changed mean
fresh-source selection from 88.52 to 89.54 per second and derived software delay
from 46.63 to 42.62 ms. Those figures do not measure physical motion-to-photon
latency, unique scene updates, or arbitrary game motion. The 5 ms JIT setting on
its own was inconsistent; do not attribute the combined result to it alone.

Clear both properties and restart the app to restore the default timing:

```sh
adb shell setprop debug.wivrn.nx.jit_max_sleep_us '""'
adb shell setprop debug.wivrn.nx.ready_wait_us '""'
```

[Paired measurements and limitations](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-23/overnight-gains/ready-abba-report).
