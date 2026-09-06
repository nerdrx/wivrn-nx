#!/usr/bin/env python3
"""The decision table: hybrid (HEVC base + nxvc) vs nxvc-only at equal
displayed PSNR, with the measured Pico costs attached."""
import json, re, subprocess
import numpy as np

W = H = 1088
FS = W * H * 3 // 2
BIN = "/run/media/nerdrx/Lex/claude/nx-warp/build-vk/bin"
SC = "/run/media/nerdrx/Lex/claude/nx-scratch"
QPS = [22, 24, 26, 28, 30]
ANCHOR_QPS = [2, 6, 10, 14, 18, 22, 24, 26, 28, 30]
POINTS = {"pan": [22, 14, 6], "panels": [26, 18, 14]}
SRCS = {"pan": f"{SC}/inter_pan8.yuv", "panels": "panels.yuv"}

# measured on the Pico 4 (docs/NXWARP-HYBRID.md 2.2 and 3.2)
ASIC_MS_PER_FRAME = 1.40      # 1088^2 HEVC decode occupancy, <=10 Mbit
PATCH_US_PER_TILE = 1.85      # AHB -> atlas, integer conversion
NXVC_FLOOR_B = 3872           # measured: an nxvc picture with a zero residual


def fbytes(nxv):
    out = subprocess.run([f"{BIN}/nxv-info", "--in", nxv], capture_output=True,
                         text=True).stdout
    return [int(m) for m in re.findall(r"^frame \d+ @\d+: num \d+  bytes (\d+)", out, re.M)]


def y(path, i):
    with open(path, 'rb') as f:
        f.seek(i * FS)
        return np.frombuffer(f.read(W * H), np.uint8).reshape(H, W).astype(np.float64)


def psnr_pair(src, dec, ia, ib):
    m = np.mean([((y(src, a) - y(dec, b)) ** 2).mean() for a, b in zip(ia, ib)])
    return 10 * np.log10(255.0 * 255.0 / max(m, 1e-12))


def psnr_hybrid(src, base, resid, ia, ib):
    tot = []
    for k, (a, b) in enumerate(zip(ia, ib)):
        rec = np.clip(y(base, b) + (y(resid, k) - 128.0), 0, 255)
        tot.append(((y(src, a) - rec) ** 2).mean())
    return 10 * np.log10(255.0 * 255.0 / max(np.mean(tot), 1e-12))


def main():
    isrc, ibase = list(range(12)), list(range(88, 100))
    report = {}

    for fx in ("pan", "panels"):
        src = SRCS[fx]
        anchor = []
        for qp in ANCHOR_QPS:
            fb = fbytes(f"nxvc_{fx}_qp{qp}.nxv")
            anchor.append({"qp": qp, "bytes": float(np.mean(fb[1:12])),
                           "psnr": psnr_pair(src, f"nxvc_{fx}_qp{qp}.yuv",
                                             range(1, 12), range(1, 12))})
        ap = np.array([a["psnr"] for a in anchor])
        ab = np.array([a["bytes"] for a in anchor])
        o = np.argsort(ap)

        def nxvc_at(p):
            if p < ap.min() or p > ap.max():
                return None
            return float(np.exp(np.interp(p, ap[o], np.log(ab[o]))))

        print(f"\n{'='*104}\n{fx}\n{'='*104}")
        print("nxvc-only anchor (inter on, real poses), bytes/frame over frames 1..11")
        print(f"  {'QP':>3} {'B/frame':>9} {'Mbit/s@90':>10} {'PSNR-Y dB':>10}")
        for a in anchor:
            print(f"  {a['qp']:>3} {a['bytes']:>9.0f} {a['bytes']*8*90/1e6:>10.2f} {a['psnr']:>10.2f}")

        rows = []
        print(f"\nhybrid = HEVC base (ASIC, 0 GPU) + nxvc residual")
        print(f"  {'base Mb':>8} {'baseB/f':>8} {'basePSNR':>9} {'gate@26':>8} "
              f"| {'QP':>3} {'residB/f':>9} {'resid-flr':>10} {'totalB/f':>9} "
              f"{'PSNR':>7} | {'nxvc B/f':>10} {'saving':>8}")
        for crf in POINTS[fx]:
            tag = f"{fx}_c{crf}"
            st = json.load(open(f"st_{tag}.json"))
            sizes = [int(x) for x in open(f"crfbase_{fx}_{crf}.sizes") if x.strip()]
            bb = float(np.mean(sizes[88:100]))
            bp = psnr_pair(src, f"crfbase_{fx}_{crf}.yuv", isrc, ibase)
            for qp in QPS:
                rb = float(np.mean(fbytes(f"rq_{tag}_{qp}.nxv")[1:12]))
                rb_net = max(0.0, rb - NXVC_FLOOR_B)
                hp = psnr_hybrid(src, f"crfbase_{fx}_{crf}.yuv", f"rq_{tag}_{qp}.yuv",
                                 isrc, ibase)
                # the honest hybrid total: base + residual ABOVE the picture
                # floor an atlas enhancement layer would not pay
                tot = bb + rb_net
                eq = nxvc_at(hp)
                rows.append({"crf": crf, "base_bytes": bb, "base_mbit": bb*8*90/1e6,
                             "base_psnr": bp, "qp": qp, "resid_bytes": rb,
                             "resid_net": rb_net, "total": tot, "psnr": hp,
                             "nxvc_equal": eq,
                             "gate": st["gate_pass_frac"], "tile": st["tile_psnr"],
                             "ssim": st["tile_ssim"]})
                eqs = f"{eq:>10.0f}" if eq else f"{'off curve':>10}"
                sav = f"{(1-tot/eq)*100:>7.1f}%" if eq else f"{'-':>8}"
                print(f"  {bb*8*90/1e6:>8.2f} {bb:>8.0f} {bp:>9.2f} "
                      f"{st['gate_pass_frac']['26']*100:>7.1f}% | {qp:>3} {rb:>9.0f} "
                      f"{rb_net:>10.0f} {tot:>9.0f} {hp:>7.2f} | {eqs} {sav}")
        report[fx] = {"anchor": anchor, "rows": rows}

    json.dump(report, open("table2.json", "w"), indent=1)

    # --- the headline: base-only vs nxvc-only at matched quality
    print(f"\n{'='*104}\nHEADLINE: base alone as the patch source, vs nxvc-only at the same PSNR")
    print(f"{'='*104}")
    print(f"  {'fixture':>8} {'base Mbit':>10} {'PSNR':>7} {'nxvc Mbit for same PSNR':>24} "
          f"{'bitrate saving':>15} {'GPU/eye/frame':>14}")
    for fx in report:
        ap = np.array([a["psnr"] for a in report[fx]["anchor"]])
        ab = np.array([a["bytes"] for a in report[fx]["anchor"]])
        o = np.argsort(ap)
        for crf in POINTS[fx]:
            r = [x for x in report[fx]["rows"] if x["crf"] == crf][0]
            p = r["base_psnr"]
            if ap.min() <= p <= ap.max():
                nb = float(np.exp(np.interp(p, ap[o], np.log(ab[o]))))
                s = f"{nb*8*90/1e6:>24.2f}"
                sv = f"{(1-r['base_bytes']/nb)*100:>14.1f}%"
            else:
                s = f"{'above nxvc QP2':>24}"; sv = f"{'n/a':>15}"
            gpu = 289 * PATCH_US_PER_TILE / 1000.0
            print(f"  {fx:>8} {r['base_mbit']:>10.2f} {p:>7.2f} {s} {sv} {gpu:>13.2f}ms")


if __name__ == "__main__":
    main()
