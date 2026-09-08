# NX Warp atlas display

The live atlas display path is opt-in. `atlas:auto` is meaningful only when the
negotiated NX Warp tool mask contains bit 31 (ATLAS) and bit 34 (ATLAS_REBASE),
and the stream is the paired stereo form. The renderer accepts the decoder's
8-bit R8 `CT_NONE` atlas view, with dimensions read from decoded images.
The original 2176×1088 combined luma fixture is historical; current native
captures use 4352×2176 combined luma and 2160×2160 output per eye.

The renderer consumes per-pool snapshots of the decoder-owned atlas planes and
the 64-byte-per-tile GPU table. The snapshot is retained with the frame handle
until its display submission retires; this adds storage and synchronization work
to the atlas path. A follow-up removes repeated copies of the unused ordinary
picture after each pool item is initialized; its measured copy stage fell from
0.56 to 0.29 ms, without an overall cadence improvement. The ordinary decoded-picture path remains the comparison
baseline. A useful A/B keeps server, stream, client settings, and capture window
fixed, changing only atlas negotiation and display selection.

Desktop and Android builds pass. The first resting-headset comparison regressed:
median new-source cadence fell from 65.0 to 52.5/s, while decoder GPU window means
rose from 10.5 to 12.8 ms. Display GPU fell from 1.8 to 1.5 ms. Keep atlas off by
default; these results do not establish a 240 FPS capability or moving-scene
quality. The new-source counter is separate: it counts source frame IDs at projection submission,
while repeated refreshes of one decoded frame remain presentation work.

The client requires the atlas GPU handoff API from nx-warp codec `6030bab` (or a
newer compatible API). Client configuration rejects an older API with a clear error. Ordinary streams
still use the existing display path with a compatible codec.

Example server encoder options: `"inter":"true", "atlas":"auto"`.
The default `"atlas":"off"` retains ordinary coding. Optional
`"atlas-picture-threshold":"8"` sets the full-picture trigger in luma samples.

The server ACK merge is covered by `g++ -std=c++23 -Wall -Wextra -Werror -Icommon tests/nxwarp_held_ack_test.cpp`: an older report at wire 98 with bit 0 must shift left two places when merged into base 100. Empty ACK-only payloads update reconstruction state but carry no transport receipt and are not passed to the sender.


## Full-resolution atlas output

NX Warp's automatic source scale is now 1.0, and the real atlas renderer uses
full defoveated output resolution. Explicit source/output scale settings still
take precedence. R8 stereo source dimensions and partial 64-pixel edge tiles are
read from decoded images instead of assuming a 1088-pixel source.

The display mesh is normalized against the full foveation extent. Viewport size
only controls raster resolution; it must not change field of view. Earlier
scaled-output experiments used the reduced viewport as the geometry basis and
cropped the image. Their timing remains historical evidence, but not a comparison
of equivalent full-field output. The grid regression rejects that old behavior.

PICO 4 retains exact caller-owned R8 output to remove the snapshot image copy.
Its `nxwarp_atlas_speed` trait now controls only that optimization; it no longer
reduces output resolution. For comparisons, disable it before restarting:

```sh
adb shell setprop debug.wivrn.nxwarp_atlas_speed false
```

Restore automatic model selection:

```sh
adb shell 'setprop debug.wivrn.nxwarp_atlas_speed ""'
```

The previous [ABBA results](https://github.com/nerdrx/nx-warp/tree/main/bench/results/240fps-2026-09-07/live-atlas/abba-180s)
are explicitly caveated; new full-resolution measurements must identify both
encoded source and output dimensions. Exact sRGB conversion remains in use.


## Persistent target catchup (experimental)

The optional borrowed-target cache refreshes only tiles changed since a pool
image was last decoded into. It keeps a bounded history across intervening
frames, uses a separate GPU dirty list, and fully refreshes new generations or
images whose history has expired. Recreated images receive a fresh generation.
The caller preserves pixels and supplies the previous image layout; the decoder
returns the target in `GENERAL` for the existing display barrier.

This client requires nx-warp codec `4000fbf` or a compatible newer API. Enable
before reconnecting with `adb shell setprop debug.wivrn.atlas_dirty_catchup 1`;
set it to `0` to restore ordinary full target refresh. The option is off by default.
Eight changing host GPU frames match full-reference pixels across three target
images, with synchronization validation enabled; the ordinary three-frame
borrowed-target regression also passes. Live image quality still depends on
lossy atlas prediction and optimistic admission, independently of this cache.

For latency experiments, `WIVRN_DUMP_TIMINGS` now records `nx_frame_map` rows
joining outer encoder frame IDs to the NX 16-bit wire IDs after a successful
send. Paced or dropped source frames make those ID sequences diverge. Historical
CSVs without this mapping cannot support a direct NX encode-to-feedback join.
Use the codec repository's `tools/summarize_pipeline_latency.py` on a complete
single-session capture. `blit` measures render selection, not scanout or photons.
The [codec evidence record](https://github.com/nerdrx/nx-warp/tree/main/bench/results/240fps-2026-09-07/live-atlas/borrowed-cache-corrected)
contains screenshots, reproducible measurements, and experiment limits.
