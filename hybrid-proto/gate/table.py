#!/usr/bin/env python3
"""Build the decision table: hybrid (HEVC base + nxvc) against nxvc-only,
at equal displayed PSNR, with the measured ASIC cost attached."""
import json, re, subprocess, sys
import numpy as np

W = H = 1088
FS = W * H * 3 // 2
NF = 12
BIN = "/run/media/nerdrx/Lex/claude/nx-warp/build-vk/bin"
SC = "/run/media/nerdrx/Lex/claude/nx-scratch"
QPS = [22, 24, 26, 28, 30]
RATES = [5, 10, 15]
# measured in NXWARP-HYBRID.md 2.2: ASIC occupancy per 1088^2 frame, and the
# GPU cost of copying all 289 tiles of one eye into the atlas
ASIC_MS = {5: 1.40, 10: 1.40, 15: 1.40}     # flat to ~10 Mbit; 50 Mbit was 3.91
PATCH_MS_EYE = 0.534                         # 289 tiles x 1.85 us, measured mean


def frame_bytes(nxv):
    out = subprocess.run([f"{BIN}/nxv-info", "--in", nxv], capture_output=True,
                         text=True).stdout
    return [int(m) for m in re.findall(r"^frame \d+ @\d+: num \d+  bytes (\d+)", out, re.M)]


def hevc_bytes(sizes_file):
    s = [int(x) for x in open(sizes_file) if x.strip()]
    return s[88:100]


def planes(path, i):
    with open(path, 'rb') as f:
        f.seek(i * FS)
        b = np.frombuffer(f.read(FS), np.uint8)
    n = W * H
    return b[:n].reshape(H, W), b[n:n + n // 4], b[n + n // 4:]


def psnr_y(a_path, b_path, idx_a, idx_b):
    """Luma PSNR averaged as mean-of-MSE over the frame set (not mean of dB)."""
    tot = 0.0
    for ia, ib in zip(idx_a, idx_b):
        x = planes(a_path, ia)[0].astype(np.float64)
        y = planes(b_path, ib)[0].astype(np.float64)
        tot += ((x - y) ** 2).mean()
    m = tot / len(idx_a)
    return 10 * np.log10(255.0 * 255.0 / max(m, 1e-12)), m


def hybrid_recon_psnr(fixture, src, base_yuv, resid_dec, idx_src, idx_base):
    """base + (decoded residual - 128), clamped: the displayed picture."""
    tot = 0.0
    for k, (isrc, ibase) in enumerate(zip(idx_src, idx_base)):
        s = planes(src, isrc)[0].astype(np.float64)
        b = planes(base_yuv, ibase)[0].astype(np.int32)
        r = planes(resid_dec, k)[0].astype(np.int32) - 128
        rec = np.clip(b + r, 0, 255).astype(np.float64)
        tot += ((s - rec) ** 2).mean()
    m = tot / len(idx_src)
    return 10 * np.log10(255.0 * 255.0 / max(m, 1e-12)), m


def main():
    idx_src = list(range(NF))
    idx_base = list(range(88, 100))
    rows = {}

    for fx in ("pan", "panels"):
        src = f"{SC}/inter_pan8.yuv" if fx == "pan" else "panels.yuv"

        # --- nxvc-only anchor: bytes/frame over 1..11, PSNR over the same
        anchor = []
        for qp in QPS:
            fb = frame_bytes(f"nxvc_{fx}_qp{qp}.nxv")
            bpf = float(np.mean(fb[1:12]))
            p, _ = psnr_y(src, f"nxvc_{fx}_qp{qp}.yuv", range(1, 12), range(1, 12))
            skip_fb = frame_bytes(f"skip_{fx}_qp{qp}.nxv")
            anchor.append({"qp": qp, "bytes_frame": bpf, "psnr": p,
                           "skip_floor_bytes": float(np.mean(skip_fb[1:12]))})

        # --- hybrid
        hyb = []
        for mb in RATES:
            st = json.load(open(f"stats_{fx}_{mb}m.json"))
            hb = float(np.mean(hevc_bytes(f"base_{fx}_{mb}m.sizes")))
            base_only_p, base_only_m = psnr_y(src, f"base_{fx}_{mb}m.yuv",
                                              idx_src, idx_base)
            ent = {"mbit": mb, "base_bytes_frame": hb,
                   "base_mbit_actual": hb * 8 * 90 / 1e6,
                   "base_psnr": base_only_p,
                   "gate": st["gate_pass_frac"],
                   "tile_psnr": st["tile_psnr"], "tile_ssim": st["tile_ssim"],
                   "resid": []}
            for qp in QPS:
                rb = frame_bytes(f"rc_{fx}_{mb}m_qp{qp}.nxv")
                rbpf = float(np.mean(rb[1:12]))
                p, _ = hybrid_recon_psnr(fx, src, f"base_{fx}_{mb}m.yuv",
                                         f"rc_{fx}_{mb}m_qp{qp}.yuv",
                                         idx_src, idx_base)
                ent["resid"].append({"qp": qp, "resid_bytes_frame": rbpf, "psnr": p})
            hyb.append(ent)
        rows[fx] = {"anchor": anchor, "hybrid": hyb}

    json.dump(rows, open("table.json", "w"), indent=1)

    for fx in rows:
        a = rows[fx]["anchor"]
        print(f"\n################ {fx} ################")
        print("nxvc-only anchor (inter on, real poses), frames 1..11")
        print(f"  {'QP':>3} {'bytes/frame':>12} {'Mbit/s@90':>10} {'PSNR-Y dB':>10} "
              f"{'all-skip floor B':>17}")
        for e in a:
            print(f"  {e['qp']:>3} {e['bytes_frame']:>12.0f} "
                  f"{e['bytes_frame']*8*90/1e6:>10.2f} {e['psnr']:>10.2f} "
                  f"{e['skip_floor_bytes']:>17.0f}")

        # nxvc rate at a given PSNR, log-linear interpolation on bytes
        ps = np.array([e["psnr"] for e in a])
        bs = np.array([e["bytes_frame"] for e in a])
        order = np.argsort(ps)

        def nxvc_bytes_at(p):
            if p > ps.max() or p < ps.min():
                return None
            return float(np.exp(np.interp(p, ps[order], np.log(bs[order]))))

        print("\nhybrid: HEVC base + nxvc residual")
        print(f"  {'base':>5} {'base B/f':>9} {'Mbit/s':>7} {'basePSNR':>9} "
              f"{'gateQP26':>9} | {'QP':>3} {'resid B/f':>10} {'total B/f':>10} "
              f"{'PSNR':>7} {'nxvc-only B/f':>14} {'ratio':>7}")
        for hentry in rows[fx]["hybrid"]:
            g = hentry["gate"]["26"] * 100
            for r in hentry["resid"]:
                tot = hentry["base_bytes_frame"] + r["resid_bytes_frame"]
                eq = nxvc_bytes_at(r["psnr"])
                eqs = f"{eq:>14.0f}" if eq else f"{'>anchor max':>14}"
                rat = f"{tot/eq:>7.2f}" if eq else f"{'-':>7}"
                print(f"  {hentry['mbit']:>4}m {hentry['base_bytes_frame']:>9.0f} "
                      f"{hentry['base_mbit_actual']:>7.2f} {hentry['base_psnr']:>9.2f} "
                      f"{g:>8.1f}% | {r['qp']:>3} {r['resid_bytes_frame']:>10.0f} "
                      f"{tot:>10.0f} {r['psnr']:>7.2f} {eqs} {rat}")


if __name__ == "__main__":
    main()
