#!/usr/bin/env python3
"""Reflection BLOTCH metric — the exact measurement that found the defect.

mean |px - 9x9 local mean| over a rectangular region, on the luma of an sRGB PNG.
This is the number quoted in commit 02146c10 for the hero car's flat door skin:

    reflections off   5.53
    shipped (blur 14) 7.69      +39% high-frequency blotch

and the number the reflection DENOISE stage (r_refldenoise) has to move back
down. Kept as a tool so the measurement is reproducible rather than a claim.

Usage:
    refl_blotch_metric.py <region> IMAGE [IMAGE ...]
    refl_blotch_metric.py --rect x0,y0,x1,y1 IMAGE [IMAGE ...]

<region> is a named preset (see REGIONS) resolved against the image size in
FRACTIONS of width/height, so the same preset works at any capture resolution.
"""
import sys
import numpy as np
from PIL import Image

# Fractional (x0, y0, x1, y1) boxes. Fractions, not pixels, so a preset survives
# a resolution change.
REGIONS = {
    # FLAT DOOR SKIN on car_day_profile — the window the shipped 5.53 / 7.69
    # numbers were measured in. The original commit (02146c10) quoted the values
    # but not the rectangle, so this is a reconstruction: it is the door + rear
    # quarter, inboard of the shutlines, wheel arches and glass, tuned until it
    # reproduced the published pair on the committed baseline stills. It lands at
    # 5.306 (ssr_off) / 7.453 (shipped) vs the published 5.53 / 7.69 — +40.5% vs
    # the published +39%, and it reproduces the SAME flat blur sweep. Pixels
    # 612,326..802,398 at 1280x720.
    "door":       (612 / 1280, 326 / 720, 802 / 1280, 398 / 720),
    # LOWER SILHOUETTE band: the sill/rocker line and the floor immediately under
    # it, which is where reflection contribution was bleeding past the car with
    # hard blocky stair-steps (the second reported defect).
    "silhouette": (450 / 1280, 420 / 720, 900 / 1280, 500 / 720),
    # Whole-frame sanity number — catches "the fix just flattened everything".
    "frame":      (0.10, 0.10, 0.90, 0.90),
}


def luma(path):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im).astype(np.float64)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2], im.size


def box_mean(a, k):
    """Separable k x k box mean with edge replication."""
    pad = k // 2
    p = np.pad(a, pad, mode="edge")
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)), mode="constant")
    h, w = a.shape
    return (c[k:k + h, k:k + w] - c[0:h, k:k + w]
            - c[k:k + h, 0:w] + c[0:h, 0:w]) / float(k * k)


def blotch(path, rect_frac, k=9):
    a, (w, h) = luma(path)
    x0 = int(rect_frac[0] * w); y0 = int(rect_frac[1] * h)
    x1 = int(rect_frac[2] * w); y1 = int(rect_frac[3] * h)
    # The local mean is computed on the FULL image and then cropped, so pixels at
    # the region border still see their true neighbourhood.
    d = np.abs(a - box_mean(a, k))
    return float(d[y0:y1, x0:x1].mean()), (x0, y0, x1, y1, w, h)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    if argv[1] == "--rect":
        # Absolute pixels; converted to fractions against the first image.
        x0, y0, x1, y1 = (int(v) for v in argv[2].split(","))
        imgs = argv[3:]
        _, (w, h) = luma(imgs[0])
        rect = (x0 / w, y0 / h, x1 / w, y1 / h)
        label = f"rect {x0},{y0},{x1},{y1}"
    else:
        rect = REGIONS[argv[1]]
        imgs = argv[2:]
        label = argv[1]
    base = None
    for p in imgs:
        v, (x0, y0, x1, y1, w, h) = blotch(p, rect)
        rel = "" if base is None else f"   ({(v / base - 1.0) * 100:+.1f}% vs first)"
        if base is None:
            base = v
        print(f"{v:8.3f}  [{label} = {x0},{y0}..{x1},{y1} of {w}x{h}]  {p}{rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
