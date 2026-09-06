# The base layer as a patch source: the quality gate

Measured 2026-09-06, branch `nx-warp-hybrid`. Companion to
[`NXWARP-HYBRID.md`](NXWARP-HYBRID.md), whose §7.1 named this as the one
unmeasured risk. Code and data: [`hybrid-proto/gate/`](../hybrid-proto/gate/).

Nothing under `nx-warp/ref/` was modified; `nxv-enc`, `nxv-dec` and `nxv-info`
from `build-vk/bin` were used as shipped.

---

## 0. Verdict

**PASS, decisively — and the base layer should ship in atlas v1.**

| | measured |
|---|---|
| Fraction of tiles passing nxvc's own skip gate as base-sourced patches | **100.0 %** at every operating point, at QP 22 through 30 |
| Residual bytes nxvc needs on top | **0–293 B/frame** (≤ 0.2 Mbit/s), and 0 at 10 of 12 operating points |
| Bitrate at equal displayed PSNR, vs nxvc-only | **45 % to 71 % less** |
| GPU cost of a full-frame refresh, both eyes | **1.07 ms/pair** against nxvc-only's ~4.00 ms/pair |
| Worst single tile in the whole study | 38.71 dB — still clears the QP 26 gate (32.83 dB) |

The result is stronger than expected and needs its caveats read (§6), the
sharpest being that **this is partly a restatement of ADR-0022's finding that
mature HEVC beats the young nxvc codec on rate-distortion.** What is new is
§5: under the atlas that no longer implies "just send HEVC", because the atlas
architecture — not the rate-distortion — is what nxvc is for.

---

## 1. What the skip gate actually is

Read from the encoder rather than assumed. `ref/src/codec_impl.inc:1699-1716`:

```
skip_sse <= qnoise * (skip_thresh / 256) * npix ,   qnoise = qstep^2 / 12
```

and identically, integer-only, at `vk/encoder/inter/E1c_decide.comp:425-436`:
`sse * 786432 <= qstep^2 * skip_thresh * npix`. With `qstep[qp] = 2^(qp/6)`
(Annex A.2), `npix = 4096` luma samples and the default `skip_thresh = 256`
(1.0× the noise floor), the gate reduces to a per-tile luma MSE ceiling:

| QP | qstep | gate MSE | equivalently, tile passes if PSNR ≥ |
|---|---|---|---|
| 22 | 12.699 | 13.439 | **36.85 dB** |
| 24 | 16.000 | 21.333 | 34.84 dB |
| 26 | 20.159 | 33.865 | 32.83 dB |
| 28 | 25.398 | 53.757 | 30.83 dB |
| 30 | 32.000 | 85.333 | 28.82 dB |

The gate says: *the predictor's error is already under what a coded tile would
have left behind anyway, so no residual could improve on it.* Applying it to a
base-decoded tile is the exact question "is this base tile a free atlas patch".

---

## 2. Method

**Fixtures**, both 1088×1088, 12 frames, yuv420p:

* `pan` — `nx-scratch/inter_pan8.yuv` with its `.poses.json`; head-rotation
  class, ~10.5 px/frame pan.
* `panels` — synthesised as ADR-0029's Phase 2 material asks
  ([`gate/mkfixture.py`](../hybrid-proto/gate/mkfixture.py)): a mostly static
  flat UI (panels, hard-edged text rows) that is world-static under a slow
  1.5 px/frame head rotation, plus one head-locked moving element. Its
  `.poses.json` is generated to match the pan exactly, so nxvc's warp is
  derived from true motion rather than searched.

**Base layer.** libx265, low-latency IPPP shape: `bframes=0 ref=1 scenecut=0
rc-lookahead=0 keyint=1000 repeat-headers=1 slices=1 **ctu=64**`. CTB 64 makes
HEVC's quantisation and deblocking boundaries coincide with the nxvc 64×64 tile
grid. Decoded with FFmpeg, which
[`NXWARP-HYBRID.md`](NXWARP-HYBRID.md) §3.3 measured byte-identical to the
Pico's ASIC on 180/180 frames — so these are the headset's pixels.

Each 12-frame fixture is **ping-pong looped to 132 frames** (period 22, no cut,
so no artificial scene change) with a single IDR at frame 0. All measurements
are taken on frames 88–99, a forward pass of the original 12 in steady-state P.

**Rate control: CRF, not ABR, and this mattered.** x265's ABR model never
converges on a 132-frame clip. Measured: an ABR encode targeting 15 Mbit landed
at 7.08 Mbit *and* 1.8 dB below what CRF 14 reached at 3.34 Mbit. Every base
here is CRF and is reported at its **achieved** bitrate.

**Operating points** are chosen on the measured CRF curve:

| CRF | `pan` Mbit/s | `pan` PSNR | `panels` Mbit/s | `panels` PSNR |
|---|---|---|---|---|
| 6 | 4.68 | 60.52 | 25.92 | 66.49 |
| 10 | 3.92 | 55.60 | 19.88 | 62.47 |
| 14 | 3.34 | 52.21 | **14.68** | 58.53 |
| 18 | 2.80 | 48.83 | **10.46** | 54.82 |
| 22 | **2.36** | 45.52 | 6.82 | 51.15 |
| 26 | 1.91 | 42.23 | **4.26** | 47.60 |
| 30 | 1.48 | 38.92 | 2.33 | 43.89 |

`panels` spans the requested 5 / 10 / 15 Mbit/s almost exactly (4.26 / 10.46 /
14.68). **`pan` cannot: it saturates.** At CRF 6 it is 60.5 dB — visually
lossless — for 4.68 Mbit/s, and there is nothing left to spend 10 or 15 Mbit on.
That is itself a result: at 1088² per eye, a head-rotation base layer needs
*single-digit* Mbit/s, not the 50–100 Mbit WiVRn streams today.

**Residual coding.** `nxv-enc` accepts no external prediction — there is no
flag to hand it a predictor — so a true enhancement layer cannot be encoded
directly. The closest measurable proxy was used: the difference
`source − base`, offset to mid-grey, coded as an ordinary nxvc picture, then
decoded and added back to the base to form the displayed picture, whose PSNR is
then measured for real. Residual clipping at ±127 was **0.0000 %** of samples at
every operating point, so the offset costs nothing.

That proxy carries one artifact which is corrected explicitly: an nxvc picture
with an **exactly zero residual still costs 3872 B/frame** (measured, at QP 22,
26 and 30 alike — a constant-128 input). That is the intra picture-header and
per-tile-header floor, i.e. exactly the cost ADR-0029 Cheat 9 exists to remove
(it puts the atlas floor at 5 B/frame with the `row_present` bitmap). A real
enhancement layer under the atlas would not pay it, so the tables below report
**residual bytes net of that 3872 B floor**, and show the gross figure beside it.

---

## 3. Per-tile quality of the base as a patch source

289 tiles × 12 frames = 3468 tiles per operating point.

| fixture | base Mbit/s | tile PSNR p05 | p50 | **min** | tile SSIM p05 | min |
|---|---|---|---|---|---|---|
| `pan` | 2.36 | 44.35 | 45.52 | 43.56 | 0.99956 | 0.99945 |
| `pan` | 3.34 | 51.17 | 52.25 | 50.30 | 0.99991 | 0.99989 |
| `pan` | 4.68 | 59.03 | 60.62 | 57.69 | 0.99999 | 0.99998 |
| `panels` | 4.26 | 41.54 | 54.40 | **38.71** | 0.99964 | 0.99746 |
| `panels` | 10.46 | 48.79 | 62.76 | 46.08 | 0.99994 | 0.99981 |
| `panels` | 14.68 | 52.39 | 66.19 | 49.38 | 0.99997 | 0.99993 |

`panels`' median is far above its p05 because most of its tiles are static UI
that HEVC reproduces exactly; the mean is meaningless there and is not quoted.
**The p05 and the min are the numbers that matter, and the worst tile anywhere
in the study is 38.71 dB.**

### Gate pass fraction — the headline

| fixture | base Mbit/s | QP 22 | QP 24 | QP 26 | QP 28 | QP 30 |
|---|---|---|---|---|---|---|
| `pan` | 2.36 | **100.0 %** | 100.0 % | 100.0 % | 100.0 % | 100.0 % |
| `pan` | 3.34 | 100.0 % | 100.0 % | 100.0 % | 100.0 % | 100.0 % |
| `pan` | 4.68 | 100.0 % | 100.0 % | 100.0 % | 100.0 % | 100.0 % |
| `panels` | 4.26 | **100.0 %** | 100.0 % | 100.0 % | 100.0 % | 100.0 % |
| `panels` | 10.46 | 100.0 % | 100.0 % | 100.0 % | 100.0 % | 100.0 % |
| `panels` | 14.68 | 100.0 % | 100.0 % | 100.0 % | 100.0 % | 100.0 % |

**Every tile at every operating point is a free atlas patch**, by the encoder's
own rule, across the whole QP 22–30 range. The margin is not thin: the tightest
case (`panels` at 4.26 Mbit, worst tile 38.71 dB, against QP 22's 36.85 dB gate)
still clears by 1.9 dB, and at QP 26 by 5.9 dB.

An earlier run at a *lower* base rate found the boundary, which is worth
recording because it shows the gate is not trivially satisfied: an ABR base at
**2.59 Mbit/s and 34.83 dB** passed only **1.0 %** of tiles at QP 22, 50.8 % at
QP 24 and 99.9 % at QP 26. The gate bites somewhere around 35 dB, and every
sensible operating point is well above it.

---

## 4. The decision table

Bytes per frame per eye, over frames 1–11 (excluding the intra frame 0, to match
the base's steady-state-P window). "nxvc B/f" is the nxvc-only anchor
interpolated to the **same displayed PSNR**, on a log-rate curve measured at
QP 2, 6, 10, 14, 18, 22, 24, 26, 28, 30.

### `pan`

| base Mbit/s | base B/f | base PSNR | gate | QP | resid B/f gross | net | **total B/f** | displayed PSNR | nxvc-only B/f at same PSNR | **saving** |
|---|---|---|---|---|---|---|---|---|---|---|
| 2.36 | 3274 | 45.52 | 100 % | 22–30 | 3872 | **0** | **3274** | 45.52 | 9981 | **67.2 %** |
| 3.34 | 4633 | 52.21 | 100 % | 22–30 | 3872 | **0** | **4633** | 52.21 | 16216 | **71.4 %** |
| 4.68 | 6504 | 60.52 | 100 % | 22–30 | 3872 | **0** | **6504** | 60.52 | off curve (> nxvc QP 2) | — |

### `panels`

| base Mbit/s | base B/f | base PSNR | gate | QP | resid B/f gross | net | **total B/f** | displayed PSNR | nxvc-only B/f at same PSNR | **saving** |
|---|---|---|---|---|---|---|---|---|---|---|
| 4.26 | 5915 | 47.60 | 100 % | 22 | 4165 | 293 | 6207 | 48.18 | 20675 | **70.0 %** |
| 4.26 | 5915 | 47.60 | 100 % | 26 | 3921 | 49 | 5964 | 47.81 | 20241 | **70.5 %** |
| 4.26 | 5915 | 47.60 | 100 % | 30 | 3872 | 0 | 5915 | 47.60 | 19974 | **70.4 %** |
| 10.46 | 14525 | 54.82 | 100 % | 22 | 3875 | 3 | 14527 | 54.83 | 31134 | **53.3 %** |
| 10.46 | 14525 | 54.82 | 100 % | 26–30 | 3872 | 0 | 14525 | 54.82 | 31108 | **53.3 %** |
| 14.68 | 20392 | 58.53 | 100 % | 22 | 3872 | 0 | 20392 | 58.54 | 37313 | **45.3 %** |
| 14.68 | 20392 | 58.53 | 100 % | 26–30 | 3872 | 0 | 20392 | 58.53 | 37297 | **45.3 %** |

Spending residual bits is *never* worthwhile here: at `panels` 4.26 Mbit the
QP 22 residual buys +0.58 dB for 293 B/frame, while moving the base from CRF 26
to CRF 18 buys +7.2 dB for 8610 B/frame — **12× better value per byte in the
base.** This reproduces ADR-0022's "quality rises monotonically with the base
share" on entirely different material and under the atlas model.

### Cost, with the Pico numbers attached

| | per eye per frame | per frame pair | share of the 4.2 ms budget |
|---|---|---|---|
| nxvc-only atlas (ADR-0029: Pass A ~1 ms + Pass B ~1 ms) | 2.00 ms GPU | 4.00 ms | 95 % |
| **hybrid, full-frame base refresh** | **0.53 ms GPU** | **1.07 ms** | **25 %** |
| base HEVC decode | 1.40 ms **ASIC**, 0 GPU | — | — |

The ASIC cost is not GPU time. Two eyes at 238 pairs/s (the 240 fps-equivalent
target) is 476 decodes/s against the **721 fps aggregate measured for two
concurrent instances** — 66 % of one decoder, with the second eye adding no
contention.

---

## 5. What this does and does not prove

**It does not prove "hybrid beats nxvc-only as a codec".** A 45–71 % bitrate
gap at equal PSNR is largely the gap between a mature HEVC encoder and a
codec whose own ADR-0022 records it sitting ~4 dB behind x265. Read as pure
rate-distortion, this table says what ADR-0022 already said, and its conclusion
there was "then just send HEVC".

**What changes under the atlas is that rate-distortion is not what nxvc is
for.** Plain HEVC cannot give you: a late-latched display pose (Cheat 2), a
display rate decoupled from the decode rate, per-tile refresh and skip with no
whole-frame requirement (Cheat 1), per-tile foveation the encoder controls
(Cheat 3), or loss that costs a tile instead of a frame. The atlas is what
provides those, and the atlas does not care whether a tile's pixels arrived
from an nxvc coded tile or from a rect of a base picture.

So the proposition this table supports is narrow and specific:

> **Hybrid gives the atlas architecture at HEVC's rate-distortion efficiency
> and a quarter of nxvc's GPU cost.** The base fills the atlas for 1.07 ms of
> GPU per frame pair and single-digit Mbit/s; nxvc keeps the properties that
> made the atlas worth building.

And it reassigns the enhancement layer's job. At these operating points nxvc is
**not** needed for quality correction — the gate says so at 100 %. What it is
still needed for is:

1. **Loss.** A lost base picture leaves the atlas stale; nxvc refreshes the
   affected tiles per-tile, with no IDR (`NXWARP-HYBRID.md` §4.6).
2. **Latency.** The base is 2.76 ms behind (measured); nxvc tiles arrive
   earlier and can carry the newest content into the atlas before the base
   catches up.
3. **Fovea.** The base spends its bits uniformly. nxvc coded tiles are the
   mechanism for spending extra where the eye is, which is ADR-0027 §2's
   "largest unexploited lever".
4. **Content the base handles badly** — none was found in these two fixtures,
   which is a limitation of the fixtures (§6.3), not evidence of absence.

---

## 6. Caveats, in order of how much they could move the answer

1. **The fixtures are synthetic and easy.** `pan` reaches 60.5 dB for
   4.68 Mbit/s; real VRChat or Alyx capture will not. The corpus'
   `wivrn-capture` entries (`wivrn-vrchat-1440`, `wivrn-beatsaber-1440`,
   `wivrn-alyx-1440`) are declared in `MANIFEST.json` with **zero frames** —
   they are not materialised. Re-running this on real capture is the single
   most valuable follow-up, and it is the one that could move the gate fraction
   off 100 %.
2. **The residual is a proxy.** `nxv-enc` takes no external prediction, so the
   residual was coded as a picture and its 3872 B/frame picture floor
   subtracted. This slightly *understates* what a purpose-built enhancement
   layer could do on tiles that need one — but since ~0 tiles need one, it
   barely matters here. If a harder fixture pushes the gate below 100 %, this
   proxy stops being adequate and the measurement needs an encoder that accepts
   a predictor.
3. **12 frames, 11 of them steady state.** Short. Rate control, drift and the
   rolling intra refresh all have periods longer than this clip.
4. **No loss, no thermals, no live session.** As in `NXWARP-HYBRID.md` §7.
5. **PSNR and SSIM, not a perceptual metric.** ADR-0027 used FovVideoVDP in the
   Pico display model; this gate did not. The eccentricity-weighted question —
   *where* in the visual field the base is weak — is unanswered.
6. **One base encoder.** x265 at `preset medium`. A hardware HEVC encoder on
   the server (VAAPI/NVENC) is worse at equal bitrate, which would shift every
   row toward the base needing more bits.

---

## 7. Recommendation

**Ship the base layer in atlas v1**, with the enhancement layer's role written
down as loss recovery, latency cover and foveal detail — *not* quality
correction, which the gate shows is unnecessary at every operating point tested.

Concretely, and in this order:

1. **Re-run this gate on real WiVRn capture** before the encoder policy is
   fixed. `corpus/MANIFEST.json`'s three `wivrn-capture` entries need
   materialising first. If the gate stays at or near 100 % there, the base's
   refresh policy can be "full refresh every base frame" and the encoder's
   nxvc budget goes entirely to fovea and loss.
2. **Set the base at single-digit Mbit/s.** Measured saturation is 3–5 Mbit/s
   for head-rotation content and ~10–15 Mbit/s for dense static UI. The base
   share in `split_bitrate()` should be a policy number in that range, not the
   `width*height` weighting it would get by default.
3. **Prioritise ADR-0029 Cheat 9's `row_present` bitmap.** The measured
   3872 B/frame nxvc picture floor is 2.79 Mbit/s of pure signalling — larger
   than the entire `pan` base layer at 2.36 Mbit/s. On a hybrid stream where
   nxvc codes almost nothing, the floor *is* the enhancement layer's cost.
4. Then build, per `NXWARP-HYBRID.md` §6.

---

## Appendix: reproducing

```sh
cd hybrid-proto/gate
python3 mkfixture.py panels     # the static-panels fixture + its poses
./mkbase.sh                     # ping-pong loops (ABR bases, superseded)
./mkbase2.sh                    # the CRF rate-quality curve -> crfcurve.json
./final.sh                      # per-tile stats, residuals, nxvc residual coding
python3 table2.py               # the tables above -> table2.json
```

All CPU work runs under `chrt -i 0 taskset -c 0-7 nice -n 19`.

| file | what |
|---|---|
| `mkfixture.py` | synthesises the static-panels fixture and matching poses |
| `mkbase.sh`, `mkbase2.sh` | HEVC base layers; ABR (superseded) and CRF |
| `analyse.py` | per-tile PSNR/SSIM, the skip gate, residual planes |
| `sweep.sh`, `final.sh` | nxvc anchor, all-skip probe, residual coding |
| `table.py`, `table2.py` | the decision tables |
| `crfcurve.json`, `st_*.json`, `table2.json` | the numbers above |
