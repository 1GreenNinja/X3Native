#!/usr/bin/env python3
"""Side-by-side zoomed crop of the same region from N captures, with labels.

Used to LOOK at the reflection-denoise A/B rather than only measure it:
    refl_crop_compare.py out.png x0,y0,x1,y1 zoom img1 img2 [img3 ...]
Images are stacked left-to-right in the order given, separated by a 4 px rule.
"""
import sys
from PIL import Image, ImageDraw


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 2
    out = argv[1]
    x0, y0, x1, y1 = (int(v) for v in argv[2].split(","))
    zoom = int(argv[3])
    paths = argv[4:]

    crops = []
    for p in paths:
        im = Image.open(p).convert("RGB").crop((x0, y0, x1, y1))
        im = im.resize(((x1 - x0) * zoom, (y1 - y0) * zoom), Image.NEAREST)
        crops.append(im)

    gap, band = 4, 22
    w = sum(c.width for c in crops) + gap * (len(crops) - 1)
    h = crops[0].height + band
    sheet = Image.new("RGB", (w, h), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    x = 0
    for p, c in zip(paths, crops):
        sheet.paste(c, (x, band))
        label = p.replace("\\", "/").split("/")[-2] + " / " + p.replace("\\", "/").split("/")[-1]
        d.text((x + 4, 5), label, fill=(235, 235, 240))
        x += c.width + gap
    sheet.save(out)
    print(f"wrote {out}  ({sheet.width}x{sheet.height}, crop {x0},{y0}..{x1},{y1} @ {zoom}x)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
