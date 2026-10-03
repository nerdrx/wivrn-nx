# NXVC paired-Haar experiment

The experimental `pyrowave-probe` branch can replace PyroWave's CDF 9/7
transform with a paired Haar transform. This is a different bitstream format,
not a faster decoder for an unchanged CDF packet. Native image dimensions and
colour sampling remain unchanged by this option; it does not apply foveation.

The purpose is to remove reconstruction work while accepting a modest change
in texture detail. Clean edges and stable-looking images matter more than
matching a reference pixel exactly. GPU commands, buffer sizes, and host/client
format pairing still have to be valid.

## What changes

The encoder represents each pair by its average and half-difference. The
matching decoder reconstructs a 2x2 footprint using sums and differences of
four coefficients, without CDF's lifting filter. The existing packet layout,
rate control and quantized-coefficient unpacking remain in use. The build
option is off by default:

```sh
cmake -S . -B build -DPYROWAVE_HAAR_FORMAT=ON
cmake --build build
```

Build **both the host and headset** with the same setting. Haar start-of-frame
headers carry extended code 1; standard CDF uses code 0. A decoder rejects the
other code. Standalone fixture helpers also distinguish `PYROHAAR` from
`PYROWAVE`. Current stream negotiation still calls this experimental transport
PyroWave; this option is not a separately negotiated consumer codec.

The Haar library forces the matching compute inverse even when a caller
requests the old fragment path. The client uses the decoder's actual output
stage for image barriers. Haar's inverse does not request full subgroups,
because it does not use subgroup operations. Output cropping is selected at
pipeline creation for non-32-aligned dimensions; aligned frames compile out
the edge checks.

## Measured reconstruction cut

At a roughly 694 kB packet budget, the native 4352x2176 4:4:4 fixture showed
slightly more plane error than CDF, with coherent reconstructed detail. Six
independently encoded translated frames and a separate 4:2:0 fixture were
also decoded on the host. These are spatial checks, not proof of comfortable
live VR, motion stability, 90 Hz presentation or photon latency.

The integrated inverse reconstructs all five levels algebraically in one
compute pass per component: three dispatches instead of fifteen. It avoids
intermediate LL image writes and barriers. This prototype supports precision
1 (two R16F high mips and three R32F low mips); other settings fail explicitly.

On the Pico, the matched native 4:2:0 packet measured **12.42 ms GPU p50**,
versus **16.85 ms** for standard CDF: a **26% GPU reduction**. The synchronous
call measured **15.32 ms**, versus **21.11 ms**. Both used the same source and
approximately 694 kB packet budget, with 12 warmups and 30 measured repeats.
Pico and desktop Haar output matched byte-for-byte. These isolated static
decode timings still exceed the 11.11 ms 90 Hz frame budget and exclude
transport, presentation, encoder cost, and photon latency.

Clearing coefficient images and skipping zero writes made reconstruction
slower; it is not integrated. Alternate dispatch layouts and typed final
stores showed no convincing benefit. A correct 4x4 output variant was roughly
6% slower than 2x2 on Pico and is rejected. Dithering is deferred.

Results and full measurement scopes are maintained in
[the NX Warp experiment report](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-03/pyrowave-full-image).

The incorporated PyroWave sources retain Hans-Kristian Arntzen's MIT
copyright and SPDX notices. This experiment builds on that work.
