#!/usr/bin/env python3
"""Pixel diff between two PNGs: max/mean absolute channel difference + count.

Used to tell a BIT-EXACTNESS failure apart from an ULP-level shader-recompilation
difference: a structural regression shows large, clustered deltas, while FP
contraction shifting under a recompile shows scattered +-1/255.
"""
import sys
import numpy as np
from PIL import Image


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    a = np.asarray(Image.open(argv[1]).convert("RGB")).astype(np.int32)
    b = np.asarray(Image.open(argv[2]).convert("RGB")).astype(np.int32)
    if a.shape != b.shape:
        print(f"SIZE MISMATCH {a.shape} vs {b.shape}")
        return 1
    d = np.abs(a - b)
    nz = int((d > 0).sum())
    tot = int(d.size)
    print(f"max={d.max():3d}  mean={d.mean():.6f}  "
          f"nonzero={nz}/{tot} ({100.0*nz/tot:.3f}%)  "
          f">1LSB={int((d > 1).sum())}  >4LSB={int((d > 4).sum())}")
    if d.max() > 0:
        ys, xs = np.where(d.max(axis=2) == d.max())
        print(f"  worst pixel at x={xs[0]} y={ys[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
