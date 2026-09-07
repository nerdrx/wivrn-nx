# NX Warp atlas display

The live atlas display path is opt-in. `atlas:auto` is meaningful only when the
negotiated NX Warp tool mask contains bit 31 (ATLAS) and bit 34 (ATLAS_REBASE),
and the stream is the paired stereo form. The current renderer accepts the
decoder's R8 `CT_NONE` view at 2176×1088 luma resolution, with 1088×544
interleaved CbCr. Other atlas dimensions or formats fail clearly at the display
boundary until a matching shader variant exists.

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


## Pico 4 speed preset

The `PICO 4` model now defaults to caller-owned R8 atlas targets and a 0.40
AUTO output scale (864×864 per eye in the measured stream). This preserves the
encoded atlas resolution while reducing display fragment work and image copies.
An explicit `defoveate_scale` still takes precedence. Other headset models keep
their existing defaults.

Four 180-second control/combined/combined/control captures measured renderer GPU
medians of 2.5/1.6/1.6/2.3 ms and copy medians of 0.29/0.01/0.01/0.29 ms.
Decoder wall medians were 2.3/1.9/2.0/2.3 ms. These summarize report-window means
under uncontrolled shared host load; session gaps prevent a physical-FPS claim.
The smaller output is a quality tradeoff. See the
[raw evidence](https://github.com/nerdrx/nx-warp/tree/main/bench/results/240fps-2026-09-07/live-atlas/abba-180s).

For comparison, disable the model preset before restarting the client:

```sh
adb shell setprop debug.wivrn.nxwarp_atlas_speed false
```

Clear that override to restore model selection:

```sh
adb shell 'setprop debug.wivrn.nxwarp_atlas_speed ""'
```

`NXWARP_ATLAS_DIRECT=0` also disables direct targets when the client process is
launched with that environment variable. Exact sRGB conversion remains in use;
the polynomial approximation was measured and rejected.
