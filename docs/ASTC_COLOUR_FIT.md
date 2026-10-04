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

CEM8 endpoint ordering must compare the decoded eight-bit colours, rather than
their six-bit codes. Equal code sums can become reversed after bit replication
and accidentally select ASTC blue contraction. The known vector `(32,0,0)` and
`(15,15,2)` has equal code sums32 but decoded sums130 and128; an independent
ASTC decoder confirms the unintended colour change. The encoder now sorts using
the replicated values before fitting weights. This fixes a palette interpretation
error without changing block size, dispatch count or decoder work.

## Selective independent colour plane

Colour-rich tiles can use legal one-partition CEM8, 4×4 dual-plane ASTC weights.
One component receives independent shading weights, instead of forcing all RGB
components onto one colour line. Block footprint and 128-bit size stay unchanged;
the existing Pico ASTC sampler decodes either mode. Ordinary tiles retain their
5×5 single-plane representation. No new network format, client pass or history.

A chroma-variance threshold 500 limits the extra PC fit. The actual quantized and
interpolated candidate must reduce modeled RGB squared error by at least 5% at
q2/q3, or 20% at q4–q6. The stronger sharp-quality gate reduces synthetic phase
variation. q0/q1 and flat tiles retain the ordinary path. The bitrate controller
continues to use actual compressed packet sizes.

Exact two-photo PC validation externally decoded all candidates. The production
port generated identical blocks to the validated scratch shader across both
photos and all seven quality settings. q6 gains were 0.55/0.08 dB with approximately
unchanged compressed size; q2 dark gained0.47dB with 7.9% more compressed bytes.
These gains vary by content. Eight-phase synthetic pans still had slightly greater
phase-step variation at q4/q6; this is not headset-motion stability proof.
Pico legal-mode sampling checks passed against the independent decoder, without
an observed sampling cost increase in short 1920×1080 checks. A short native 2176×2176-per-eye live check held approximately 89–90 viewer
updates/s with the app GPU pass at 1.8–2.1 ms. This stationary wake-burst check
does not prove complex moving-scene throughput or visual preference. The
existing PCA covariance is reused for the independent-component fit, deleting
its duplicate mean/covariance passes; all photo/q0–q6 output bytes remained
identical to the independently validated selected shader.

Run `python3 tests/astc_colour_tables_test.py` to check generated BISE, interpolation
and least-squares table invariants. Add `--decoder PATH` for an independently
decoded CEM8 endpoint-order fixture. Padding entries with zero coefficients must
be skipped before indexing grid or RHS arrays.
