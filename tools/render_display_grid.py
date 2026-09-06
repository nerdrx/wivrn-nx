#!/usr/bin/env python3
# Draw the display pass's cell mask, for docs/GALLERY.md.
#
# The mask is NOT recomputed here: it is read from `stream_grid_test --dump`, which calls
# the same client/scenes/stream_grid.h the reprojection pass calls. A renderer that
# re-derived the geometry in Python would be a drawing of the mask rather than a picture
# of it.
#
#   usage: render_display_grid.py <path to stream_grid_test>
#   writes docs/assets/display-grid-100deg.png and docs/assets/display-grid-overscan.png
import subprocess, sys, os
from PIL import Image, ImageDraw

TEST = sys.argv[1]
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "assets")

DRAWN = (28, 32, 38)        # cell the pass draws
SKIPPED = (150, 40, 48)     # cell the pass leaves out
KEPT = (196, 142, 44)       # would be skipped, kept because of the overscan ring
GRID = (70, 76, 86)
ELLIPSE = (110, 200, 255)


def dump(fh, fv, nx, ny, overscan):
    out = subprocess.run(
        [TEST, "--dump", str(fh), str(fv), str(nx), str(ny), str(overscan)],
        capture_output=True, text=True, check=True).stdout.splitlines()
    h = out[0].split()
    cols, rows, masked, total = (int(x) for x in h[:4])
    rx, ry, x0, x1, y0, y1 = (float(x) for x in h[4:10])
    return dict(cols=cols, rows=rows, masked=masked, total=total,
                rx=rx, ry=ry, rect=(x0, x1, y0, y1),
                grid=[l for l in out[1:] if l])


def render(m, path, px=720):
    img = Image.new("RGB", (px, px), (16, 18, 22))
    d = ImageDraw.Draw(img)
    cw, ch = px / m["cols"], px / m["rows"]

    for y, row in enumerate(m["grid"]):
        for x, c in enumerate(row):
            col = {"0": DRAWN, "1": KEPT, "2": SKIPPED}[c]
            d.rectangle([x * cw, y * ch, (x + 1) * cw - 1, (y + 1) * ch - 1],
                        fill=col, outline=GRID)

    # The visible region, in the same tangent space the mask was tested in, mapped
    # through the FOV rectangle onto the image.
    x0, x1, y0, y1 = m["rect"]
    def to_px(tx, ty):
        return ((tx - x0) / (x1 - x0) * px, (ty - y0) / (y1 - y0) * px)
    a = to_px(-m["rx"], -m["ry"])
    b = to_px(m["rx"], m["ry"])
    d.ellipse([min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])],
              outline=ELLIPSE, width=3)

    img.save(path)
    return path


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    a = dump(100, 100, 15, 15, 0)
    render(a, os.path.join(OUT, "display-grid-100deg.png"))
    print("display-grid-100deg.png  %d of %d cells skipped (%.1f%%)"
          % (a["masked"], a["total"], 100 * a["masked"] / a["total"]))

    b = dump(100, 100, 15, 15, 0.05)
    render(b, os.path.join(OUT, "display-grid-overscan.png"))
    print("display-grid-overscan.png %d of %d cells skipped (%.1f%%), %d kept for the ring"
          % (b["masked"], b["total"], 100 * b["masked"] / b["total"], a["masked"] - b["masked"]))
