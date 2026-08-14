#!/usr/bin/env python3
"""FACADE PLUMB CHECK — is the tower actually sheared, or is it perspective?

The owner reported the building "appears to lean" at his framing. A sheared
placement matrix and ordinary wide-angle perspective look identical to the eye,
so this measures it. For each scanline in a Y range it finds the horizontal
position of a strong vertical EDGE (the silhouette against the sky, or the
tower's near corner) by taking the largest horizontal luma gradient inside a
search window that tracks the edge down the image. It then fits a line.

READING THE RESULT. A vertical edge of a real building images as a straight
line under any pinhole projection; it is only VERTICAL on screen when the
camera has zero roll AND the edge lies in the plane through the optical axis.
So:
  * a straight fit (low residual) + nonzero slope  = PERSPECTIVE. Correct,
    leave it. Convergence is what a low wide-angle camera does.
  * a CURVED fit (high residual)                   = lens/projection problem.
  * two edges of the same face converging the WRONG way, or a slope that
    persists when the camera is put dead-on and centred, = a real shear.

Usage:
    python tools/facade_plumb.py shot.png XSTART Y0 Y1 [WIN]
"""
import sys
import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    path = sys.argv[1]
    x = int(sys.argv[2])
    y0, y1 = int(sys.argv[3]), int(sys.argv[4])
    win = int(sys.argv[5]) if len(sys.argv) > 5 else 14

    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    lum = 0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]

    xs, ys = [], []
    cur = x
    for y in range(y0, y1):
        lo, hi = max(1, cur - win), min(lum.shape[1] - 1, cur + win)
        seg = lum[y, lo:hi]
        if len(seg) < 3:
            break
        g = np.abs(np.diff(seg))
        k = int(np.argmax(g))
        if g[k] < 3.0:          # no real edge on this row -- skip, keep tracking
            continue
        cur = lo + k
        xs.append(cur)
        ys.append(y)

    if len(xs) < 20:
        print(f"{path}: only {len(xs)} edge samples -- widen WIN or pick a better X")
        return 1
    xs = np.array(xs, dtype=float)
    ys = np.array(ys, dtype=float)
    m, b = np.polyfit(ys, xs, 1)                  # x = m*y + b
    resid = xs - (m * ys + b)
    span = ys.max() - ys.min()
    print(f"{path}  edge tracked over y {int(ys.min())}..{int(ys.max())} "
          f"({len(xs)} samples)")
    print(f"  slope           = {m:+.4f} px of X per px of Y")
    print(f"  total lean      = {m * span:+.1f} px over {int(span)} px of height")
    print(f"  straightness    = residual rms {resid.std():.2f} px, "
          f"max |resid| {np.abs(resid).max():.1f} px")
    print("  verdict: " + ("CURVED -- not a simple projection"
                           if resid.std() > 1.5 else
                           "STRAIGHT -- a straight line, i.e. a correct "
                           "pinhole projection of a straight edge"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
