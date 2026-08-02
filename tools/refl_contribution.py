#!/usr/bin/env python3
"""Visualise the RECTION CONTRIBUTION itself: |with - without| x gain.

Differencing a capture against the matching `--norefl` capture isolates exactly
what the reflection chain added, which is the only honest way to SEE the second
reported defect: reflection contribution bleeding past the car's lower
silhouette onto the floor in blocky half-res stair-steps. In the raw image that
bleed hides inside the floor's own mirror reflection; in the difference it is the
only thing on the floor.

    refl_contribution.py out.png gain x0,y0,x1,y1 zoom BASE_norefl IMG [IMG ...]
"""
import sys
import numpy as np
from PIL import Image, ImageDraw


def main(argv):
    if len(argv) < 7:
        print(__doc__)
        return 2
    out = argv[1]
    gain = float(argv[2])
    x0, y0, x1, y1 = (int(v) for v in argv[3].split(","))
    zoom = int(argv[4])
    base_path = argv[5]
    paths = argv[6:]

    base = np.asarray(Image.open(base_path).convert("RGB")).astype(np.float64)
    tiles, labels = [], []
    for p in paths:
        a = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
        d = np.clip(np.abs(a - base) * gain, 0, 255).astype(np.uint8)
        im = Image.fromarray(d).crop((x0, y0, x1, y1))
        tiles.append(im.resize(((x1 - x0) * zoom, (y1 - y0) * zoom), Image.NEAREST))
        parts = p.replace("\\", "/").split("/")
        labels.append(f"{parts[-2]}/{parts[-1]}  |delta| x{gain:g}")

    gap, band = 4, 22
    w = sum(t.width for t in tiles) + gap * (len(tiles) - 1)
    sheet = Image.new("RGB", (w, tiles[0].height + band), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    x = 0
    for lab, t in zip(labels, tiles):
        sheet.paste(t, (x, band))
        d.text((x + 4, 5), lab, fill=(235, 235, 240))
        x += t.width + gap
    sheet.save(out)
    print(f"wrote {out} ({sheet.width}x{sheet.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
