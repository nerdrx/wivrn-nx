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
