#!/usr/bin/env python3
"""Synthesise the static-panels fixture ADR-0029 asks for as its Phase 2
material: 1088x1088, a mostly static UI with one moving element, under slow
head rotation.

The UI is world-static and the camera rotates, so every panel pixel moves by
the same sub-pixel pan each frame -- which is the case the atlas is built for
(one warp from the source pose) and the case a block codec handles worst.
Writes <out>.yuv (yuv420p) and <out>.poses.json matching the pan exactly, so
nxv-enc's warp is derived from the true motion rather than searched.
"""
import json, math, sys
import numpy as np

W = H = 1088
N = 12
PAN_PX = 1.5          # per frame, horizontal; slow head rotation
FOV_DEG = 95.0
PAD = 64              # canvas margin so the pan never runs off the source

rng = np.random.default_rng(7)


def ui_canvas():
    """A flat-UI panel layout: dark ground, panels, high-contrast text rows."""
    cw, ch = W + 2 * PAD, H + 2 * PAD
    a = np.full((ch, cw), 24, np.float64)          # near-black desktop
    # a soft vertical gradient, so 'flat' is not literally constant
    a += np.linspace(0, 10, ch)[:, None]

    panels = [(90, 70, 520, 420), (640, 70, 380, 200), (640, 300, 380, 190),
              (90, 530, 930, 420)]
    for (x, y, w, h) in panels:
        x += PAD; y += PAD
        a[y:y + h, x:x + w] = 210                   # panel body
        a[y:y + 3, x:x + w] = 150                   # top rule
        a[y + h - 3:y + h, x:x + w] = 150
        a[y:y + h, x:x + 3] = 150
        a[y:y + h, x + w - 3:x + w] = 150
        a[y + 6:y + 34, x + 8:x + w - 8] = 60       # title bar
        # text rows: thin high-contrast bars of varied length (the 4:2:0
        # chroma-fringe / hard-edge case PAPER 5.2 locks to lossless)
        yy = y + 48
        while yy < y + h - 20:
            n = rng.integers(3, 9)
            xx = x + 12
            for _ in range(n):
                wlen = int(rng.integers(18, 90))
                if xx + wlen > x + w - 12:
                    break
                a[yy:yy + 9, xx:xx + wlen] = 30
                xx += wlen + int(rng.integers(6, 16))
            yy += 20
    return a


CANVAS = ui_canvas()


def shifted(dx):
    """Bilinear sub-pixel crop of the static canvas -- the head rotation."""
    x0 = PAD + dx
    ix = int(math.floor(x0)); fx = x0 - ix
    lo = CANVAS[PAD:PAD + H, ix:ix + W]
    hi = CANVAS[PAD:PAD + H, ix + 1:ix + 1 + W]
    return lo * (1 - fx) + hi * fx


def frame(i):
    a = shifted(i * PAN_PX).copy()
    # the one moving element: a progress indicator sweeping a panel, in
    # SCREEN space (head-locked), which is the STATIC_MV case
    px = 100 + int(i * 62)
    a[600:640, px:px + 150] = 245
    a[610:630, px + 10:px + 60] = 40
    return np.clip(a, 0, 255)


def main(out):
    ys, us, vs = [], [], []
    for i in range(N):
        y = frame(i)
        # chroma: a mild constant tint per panel region, subsampled
        u = np.full((H // 2, W // 2), 128.0)
        v = np.full((H // 2, W // 2), 128.0)
        half = y[::2, ::2]
        u += (half - 128) * -0.08          # slight blue lift on bright panels
        v += (half - 128) * 0.05
        ys.append(np.round(y).astype(np.uint8))
        us.append(np.round(np.clip(u, 0, 255)).astype(np.uint8))
        vs.append(np.round(np.clip(v, 0, 255)).astype(np.uint8))
    with open(out + ".yuv", "wb") as f:
        for i in range(N):
            f.write(ys[i].tobytes()); f.write(us[i].tobytes()); f.write(vs[i].tobytes())

    # poses matching the pan: rotation about +y such that the image shifts by
    # PAN_PX per frame at this FOV. deg_per_px = FOV / W.
    dpp = FOV_DEG / W
    poses = {"version": 2, "convention": {"id": "nxv-openxr-1"},
             "fov_deg": {"h": FOV_DEG, "v": FOV_DEG}, "frames": []}
    for i in range(N):
        th = math.radians(i * PAN_PX * dpp)
        poses["frames"].append({"orientation_xyzw": [0, math.sin(th / 2), 0, math.cos(th / 2)]})
    with open(out + ".poses.json", "w") as f:
        json.dump(poses, f, indent=1)
    print(f"wrote {out}.yuv  {N} frames {W}x{H} yuv420p, pan {PAN_PX} px/frame "
          f"({PAN_PX*dpp:.4f} deg/frame)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "panels")
