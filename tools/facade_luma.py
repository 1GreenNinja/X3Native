#!/usr/bin/env python3
"""FACADE LUMA PROBE — the gate for "black glass on white concrete".

Tim's spec: BLACK GLASS BANDS on WHITE CONCRETE with thick spandrels.
The failure this measures: the facade reading as FLAT BEIGE STRIPES, i.e.
the glass bands no darker (or barely darker) than the concrete spandrels.

METHOD. Take a rect covering facade only. Reduce to a per-scanline mean luma
(the bands are horizontal, so a row IS either concrete or glass). A raw
percentile split is useless here because the facade also carries a strong
SMOOTH vertical gradient (sky gradient + ambient falloff) that dwarfs the
band structure -- that gradient is what made a "flat" facade still measure a
+23 spread. So:

  1. local trend  = moving median over `win` rows (~1.5 storeys), which
     follows the smooth gradient and ignores the banding;
  2. residual     = row - trend  -> the BAND STRUCTURE alone;
  3. classify     = residual >= 0 is concrete, < 0 is glass;
  4. report the mean absolute luma of each class, and the CONTRAST between
     them. That contrast is the number the eye reads as "black glass on
     white concrete", and it is invariant to where the rect is placed.

Usage:
    python tools/facade_luma.py shot.png X0 Y0 X1 Y1 [--win N] [--profile]
"""
import sys
import numpy as np
from PIL import Image


def luma(rgb):
    # Rec.709 luma on the displayed (sRGB-encoded) pixels -- what the eye reads.
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def moving_median(a, win):
    half = win // 2
    pad = np.pad(a, (half, half), mode="edge")
    return np.array([np.median(pad[i:i + win]) for i in range(len(a))])


def probe(path, x0, y0, x1, y1, win=55):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    rows = luma(im[y0:y1, x0:x1]).mean(axis=1)
    trend = moving_median(rows, min(win, len(rows) | 1))
    resid = rows - trend
    conc = rows[resid >= 0]
    glass = rows[resid < 0]
    return {
        "path": path, "rect": (x0, y0, x1, y1), "rows": rows, "resid": resid,
        "concrete": float(conc.mean()) if len(conc) else float("nan"),
        "glass": float(glass.mean()) if len(glass) else float("nan"),
        "overall": float(rows.mean()),
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 5:
        print(__doc__)
        return 2
    win = 55
    if "--win" in sys.argv:
        win = int(sys.argv[sys.argv.index("--win") + 1])
    r = probe(args[0], *(int(v) for v in args[1:5]), win=win)
    spread = r["concrete"] - r["glass"]
    print(f"{r['path']}  rect={r['rect']}  rows={len(r['rows'])}  win={win}")
    print(f"  concrete spandrel luma = {r['concrete']:.1f}")
    print(f"  glass band      luma = {r['glass']:.1f}")
    print(f"  BAND CONTRAST        = {spread:+.1f}   "
          f"(glass/concrete = {r['glass'] / max(r['concrete'], 1e-6):.2f})")
    print(f"  overall facade luma  = {r['overall']:.1f}")
    if "--profile" in sys.argv:
        for i, (v, d) in enumerate(zip(r["rows"], r["resid"])):
            print(f"    {int(args[2]) + i:5d}  {v:6.1f}  resid {d:+6.1f}  "
                  f"{'#' * max(0, int(v / 4))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
