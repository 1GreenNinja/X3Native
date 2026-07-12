#!/usr/bin/env python3
"""Luminance stats for a lighting audit shot. mean / p05 / p95 / %clipped-white / %near-black."""
import sys, glob
from PIL import Image
import numpy as np

def stats(p):
    im = Image.open(p).convert("RGB")
    a = np.asarray(im).astype(np.float32)
    # Rec.709 luma on the 0-255 display-referred buffer (what the eye sees).
    y = 0.2126*a[...,0] + 0.7152*a[...,1] + 0.0722*a[...,2]
    clip = float((y >= 250.0).mean()*100.0)
    void = float((y <= 6.0).mean()*100.0)
    p05, p50, p95 = np.percentile(y, [5, 50, 95])
    # hue tell: mean R-G (a coloured light shows here; an albedo error does not)
    rg = float(a[...,0].mean() - a[...,1].mean())
    return dict(mean=float(y.mean()), p05=float(p05), p50=float(p50), p95=float(p95),
                spread=float(p95-p05), clip=clip, void=void, rg=rg)

if __name__ == "__main__":
    files = []
    for g in sys.argv[1:]:
        files += sorted(glob.glob(g))
    print(f"{'shot':44s} {'mean':>6s} {'p05':>6s} {'p50':>6s} {'p95':>6s} {'sprd':>6s} {'%clip':>6s} {'%void':>6s} {'R-G':>6s}")
    for f in files:
        try:
            s = stats(f)
        except Exception as e:
            print(f"{f[-44:]:44s}  ERR {e}"); continue
        name = f.replace("\\","/").split("/")[-1]
        print(f"{name:44s} {s['mean']:6.1f} {s['p05']:6.1f} {s['p50']:6.1f} {s['p95']:6.1f} "
              f"{s['spread']:6.1f} {s['clip']:6.2f} {s['void']:6.1f} {s['rg']:+6.1f}")
