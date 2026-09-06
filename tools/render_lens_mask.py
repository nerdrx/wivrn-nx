#!/usr/bin/env python3
# Draw the lens mask over a picture, for docs/GALLERY.md.
#
# The mask is NOT recomputed here: it is read from `lens_mask_test --dump`, which calls the
# same client/utils/view_geometry.h the server does. A renderer that re-derived the geometry
# in Python would be a drawing of the mask rather than a picture of it.
#
#   usage: render_lens_mask.py <path to lens_mask_test> <background png>
#   writes docs/assets/lensmask-pico4.png and docs/assets/lensmask-synthetic-100deg.png
import subprocess, sys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

TEST = sys.argv[1]

def dump(fh, fv, w, h, sw, sh, margin=1):
    out = subprocess.run([TEST, "--dump", str(fh), str(fv), str(w), str(h), str(sw), str(sh), str(margin)],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    hdr = out[0].split()
    cols, rows, masked, total = (int(x) for x in hdr[:4])
    rx, ry, x0, x1, y0, y1 = (float(x) for x in hdr[4:10])
    grid = [line for line in out[1:] if line]
    return dict(cols=cols, rows=rows, masked=masked, total=total, rx=rx, ry=ry,
                rect=(x0, x1, y0, y1), grid=grid)

def render(bg, m, w, h, title, sub, path):
    img = bg.convert("RGB").resize((w, h))
    ov = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    tile = 64
    for ty, row in enumerate(m["grid"]):
        for tx, c in enumerate(row):
            box = [tx*tile, ty*tile, min((tx+1)*tile, w)-1, min((ty+1)*tile, h)-1]
            if c == '2':      # masked: never coded
                d.rectangle(box, fill=(0x77, 0x00, 0xFF, 150), outline=(0xBB, 0x88, 0xFF, 255))
            elif c == '1':    # outside the region, kept by the margin ring
                d.rectangle(box, fill=(0x77, 0x00, 0xFF, 40), outline=(0x99, 0x55, 0xDD, 120))
    # the visible region, mapped from tangent space onto the image rectangle
    x0, x1, y0, y1 = m["rect"]
    def px(tx_):  return (tx_ - x0) / (x1 - x0) * w
    def py(ty_):  return (ty_ - y0) / (y1 - y0) * h
    d.ellipse([px(-m["rx"]), py(-m["ry"]), px(m["rx"]), py(m["ry"])],
              outline=(255, 255, 255, 230), width=4)
    img = Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")

    bar = 84
    out = Image.new("RGB", (w, h + bar), (12, 10, 20))
    out.paste(img, (0, 0))
    d2 = ImageDraw.Draw(out)
    try:
        f1 = ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", 26)
        f2 = ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans.ttf", 19)
    except OSError:
        f1 = f2 = ImageFont.load_default()
    d2.text((16, h + 12), title, font=f1, fill=(0xEE, 0xE6, 0xFF))
    d2.text((16, h + 48), sub, font=f2, fill=(0xA9, 0x96, 0xC8))
    out.save(path)
    print(path, m["masked"], "of", m["total"])

# 1. the Pico 4 geometry, over a real decoded frame from the e2e harness
bg = Image.open(sys.argv[2])
m = dump(105, 105, 1088, 1088, 1088, 1088, 1)
render(bg, m, 1088, 1088,
       "Lens mask - Pico 4 geometry, 1088x1088 per eye",
       f"{m['masked']} of {m['total']} tiles masked (filled) - pale ring: outside the region, kept by the 1-tile margin - white: visible region",
       "docs/assets/lensmask-pico4.png")

# 2. the synthetic symmetric 100 deg case, on a plain grid
grid = Image.new("RGB", (1088, 1088), (30, 26, 44))
g = ImageDraw.Draw(grid)
for i in range(0, 1089, 64):
    g.line([(i, 0), (i, 1088)], fill=(58, 50, 84))
    g.line([(0, i), (1088, i)], fill=(58, 50, 84))
m2 = dump(100, 100, 1088, 1088, 1088, 1088, 1)
render(grid, m2, 1088, 1088,
       "Lens mask - synthetic symmetric 100 deg FOV, no foveation",
       f"{m2['masked']} of {m2['total']} tiles masked - the four corners, never the centre (tests/lens_mask_test.cpp)",
       "docs/assets/lensmask-synthetic-100deg.png")
