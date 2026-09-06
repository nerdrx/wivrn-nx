#!/usr/bin/env python3
"""Per-64x64-tile analysis of the HEVC base layer as an atlas patch source.

For each fixture and base bitrate, over the 12-frame evaluation window:
  * per-tile luma PSNR and SSIM of the base-decoded tile against the source;
  * the fraction of tiles that pass nxvc's OWN skip gate, computed with the
    identical expression the encoder uses:

        skip_sse <= (qstep^2 / 12) * (skip_thresh / 256) * npix

    (ref/src/codec_impl.inc:1703-1716, vk/encoder/inter/E1c_decide.comp:425)
    with qstep = kQStep[qp]/16 = 2^(qp/6), npix = 4096, skip_thresh = 256
    (the default, 1.0x the noise floor). A tile that passes needs no coded
    residual at all: it is a free atlas patch.
  * the residual the remaining tiles need, written out for nxv-enc to code.

Emits JSON; the residual planes are written as a yuv420p sequence with the
difference offset to mid-grey.
"""
import json, sys
import numpy as np

W = H = 1088
FS = W * H * 3 // 2
TILE = 64
GRID = W // TILE            # 17
NT = GRID * GRID            # 289
NPIX = TILE * TILE          # 4096 luma samples
WIN = range(88, 100)        # the evaluation window in the ping-pong loop


def qstep(qp):
    return 2.0 ** (qp / 6.0)


def gate_mse(qp, skip_thresh=256):
    """The gate as a per-luma-sample MSE, which is what it reduces to."""
    return qstep(qp) ** 2 / 12.0 * (skip_thresh / 256.0)


def tiles(a):
    """(H,W) -> (NT, TILE, TILE) in raster tile order."""
    return (a.reshape(GRID, TILE, GRID, TILE)
             .transpose(0, 2, 1, 3)
             .reshape(NT, TILE, TILE))


def ssim_tiles(x, y):
    """Global SSIM per tile (one window == the tile), luma, 8-bit."""
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    x = x.reshape(NT, -1).astype(np.float64)
    y = y.reshape(NT, -1).astype(np.float64)
    mx, my = x.mean(1), y.mean(1)
    vx, vy = x.var(1), y.var(1)
    cxy = ((x - mx[:, None]) * (y - my[:, None])).mean(1)
    return (((2 * mx * my + C1) * (2 * cxy + C2)) /
            ((mx ** 2 + my ** 2 + C1) * (vx + vy + C2)))


def load_y(path, idx):
    with open(path, 'rb') as f:
        f.seek(idx * FS)
        return np.frombuffer(f.read(W * H), np.uint8).reshape(H, W)


def load_frame(path, idx):
    with open(path, 'rb') as f:
        f.seek(idx * FS)
        b = np.frombuffer(f.read(FS), np.uint8)
    return (b[:W * H].reshape(H, W),
            b[W * H:W * H + (W * H) // 4].reshape(H // 2, W // 2),
            b[W * H + (W * H) // 4:].reshape(H // 2, W // 2))


def main():
    fixture, mb, srcpath, basepath, out_json, out_resid = sys.argv[1:7]
    qps = [22, 24, 26, 28, 30]

    per_tile_psnr, per_tile_ssim = [], []
    gate_pass = {q: 0 for q in qps}
    gate_tot = 0
    resid_bytes_raw = []
    clip_lo = clip_hi = clip_n = 0

    rf = open(out_resid, 'wb') if out_resid != '-' else None

    for k, f in enumerate(WIN):
        sy, su, sv = load_frame(srcpath, k)        # source: the 12 originals
        by, bu, bv = load_frame(basepath, f)       # base: from the loop
        st, bt = tiles(sy).astype(np.float64), tiles(by).astype(np.float64)
        sse = ((st - bt) ** 2).reshape(NT, -1).sum(1)
        mse = sse / NPIX
        psnr = 10 * np.log10(255.0 * 255.0 / np.maximum(mse, 1e-12))
        per_tile_psnr.append(psnr)
        per_tile_ssim.append(ssim_tiles(tiles(sy), tiles(by)))
        gate_tot += NT
        for q in qps:
            gate_pass[q] += int((mse <= gate_mse(q)).sum())

        if rf is not None:
            # residual, offset to mid-grey. Range of s-b is [-255,255]; the
            # clipping rate at +-127 is measured and reported rather than
            # assumed away.
            for (s, b) in ((sy, by), (su, bu), (sv, bv)):
                d = s.astype(np.int32) - b.astype(np.int32) + 128
                clip_lo += int((d < 0).sum()); clip_hi += int((d > 255).sum())
                clip_n += d.size
                rf.write(np.clip(d, 0, 255).astype(np.uint8).tobytes())
    if rf is not None:
        rf.close()

    P = np.concatenate(per_tile_psnr)
    S = np.concatenate(per_tile_ssim)
    res = {
        "fixture": fixture, "mbit_target": mb,
        "tiles": int(gate_tot), "frames": len(WIN),
        "tile_psnr": {"mean": float(P.mean()), "p05": float(np.percentile(P, 5)),
                      "p50": float(np.median(P)), "p95": float(np.percentile(P, 95)),
                      "min": float(P.min()), "max": float(P.max())},
        "tile_ssim": {"mean": float(S.mean()), "p05": float(np.percentile(S, 5)),
                      "p50": float(np.median(S)), "min": float(S.min())},
        "gate_pass_frac": {str(q): gate_pass[q] / gate_tot for q in qps},
        "gate_mse": {str(q): gate_mse(q) for q in qps},
        "resid_clip_frac": (clip_lo + clip_hi) / max(clip_n, 1),
    }
    json.dump(res, open(out_json, 'w'), indent=1)
    print(f"{fixture:7s} {mb:>3s}m  tile PSNR mean {P.mean():5.2f} p05 {np.percentile(P,5):5.2f} "
          f"min {P.min():5.2f} | SSIM mean {S.mean():.4f} p05 {np.percentile(S,5):.4f} "
          f"| gate pass " + " ".join(f"QP{q}:{gate_pass[q]/gate_tot*100:5.1f}%" for q in qps)
          + (f" | resid clip {(clip_lo+clip_hi)/max(clip_n,1)*100:.4f}%" if rf is not None else ""))


if __name__ == "__main__":
    main()
