# ASTC colour fit: projected endpoints

The native NXVC path encodes an 8×8 patch as a standard 128-bit ASTC block.
Its cheap one-partition representation has one RGB gradient and a 5×5 weight
grid. Multiple unrelated colours inside a patch can therefore produce visible
colour blocks even at the highest adaptive quality rung.

The encoder already estimates a principal colour axis from the patch covariance.
Previously, its endpoints were the actual pixels with the smallest and largest
projections on that axis. Those pixels can contain colour components unrelated
to the fitted gradient. The encoder now projects the extrema onto the line
through the patch mean instead:

```
endpoint = mean + axis * (extreme_projection - dot(mean, axis))
```

The existing endpoint clamp, quantization, legal RGB ordering, weight fitting,
packet compression and adaptive bitrate remain in place. This changes three
endpoint-fitting lines; it adds no scan, dispatch, temporal history, client
shader, block bytes or texture format.

On the two exact, unresized 1920×1080 screenshots supplied by the user, the
independently decoded q6 RGB MSE falls by 21.9% and 15.4%. Zstd3 payloads rise
by 0.20% and 0.03%. Lower-quality q2/q4 comparisons improve too. Eight synthetic
one-pixel translation phases retain the gain and lower motion-aligned adjacent
error. An approximate 8-bit BT.709 4:2:0 input test also retains an improvement.

These are offline image and synthetic-grid results, not a guarantee that all
colour blocks disappear or proof of sustained moving-scene headset performance.
The RGBA harness and approximate chroma conversion are not a capture of the
actual compositor input. The client still samples the same legal ASTC format;
its live GPU time must be measured separately.

Tables, crops, scripts, provenance limits and rejected alternatives are in the
[dense-colour report](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-04/dense-colour).

The 6×6 footprint and two-partition search candidates were not integrated:
their measured byte or PC cost did not justify their quality gains. Reverting
the endpoint projection restores the original encoder without changing packet
or client compatibility.
