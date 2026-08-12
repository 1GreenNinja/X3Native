#!/usr/bin/env python3
"""Reconstruct the blue channel of a tangent-space normal map from its R/G.

WHEN THIS IS VALID
    A tangent-space normal is a UNIT vector with z >= 0, so z is fully implied
    by x and y:  z = sqrt(1 - x^2 - y^2).  If R/G survived intact, B carries no
    independent information and can be recomputed exactly. That is the whole
    repair.

    It is only valid when R/G really are intact. This script REFUSES to run
    unless it can show that, and prints the evidence either way:
      * mean(R), mean(G) close to 127.5  (no directional bias baked in)
      * x^2 + y^2 <= 1 for every texel   (a real z exists everywhere)
    If x^2+y^2 exceeds 1 anywhere meaningful, the XY data is damaged too and the
    set must be RE-EXPORTED or RETIRED, not repaired. Use --force only if you
    have read the numbers and accept clamping.

CROSS-CHECK
    Both maps repaired so far were not randomly destroyed — they were INVERTED
    (B_stored = 255 - B_true, corr < -0.999). When that is the case there are
    two INDEPENDENT reconstructions of B: the geometric one, sqrt(1-x^2-y^2),
    and the arithmetic one, 255 - B_stored. The script reports how closely they
    agree. Agreement to ~1 grey level is strong proof the repair is right and
    not merely plausible.

USAGE
    python tools/repair_normal_map.py assets/surface_library/<set>/normal.png
    python tools/repair_normal_map.py <path> --xy-scale 0.45   # also tame loudness
    python tools/repair_normal_map.py <path> --dry-run         # analyse only

The original is preserved as <name>.corrupt.bak next to it (never deleted).
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

XY_MEAN_TOL = 3.0      # |mean - 127.5| allowed before we call R/G biased
OVERUNIT_TOL = 0.001   # fraction of texels allowed to have x^2+y^2 > 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="repair_normal_map.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="normal map PNG to repair in place")
    ap.add_argument("--xy-scale", type=float, default=1.0,
                    help="scale the XY deviation from neutral (e.g. 0.45 to tame an "
                         "over-loud map). Default 1.0 = leave the art alone.")
    ap.add_argument("--dry-run", action="store_true", help="report only, write nothing")
    ap.add_argument("--force", action="store_true",
                    help="repair even if the R/G integrity checks fail (clamps)")
    args = ap.parse_args(argv)

    p = Path(args.path)
    if not p.is_file():
        print(f"no such file: {p}", file=sys.stderr)
        return 2

    with Image.open(p) as im:
        mode, size = im.mode, im.size
        a = np.asarray(im.convert("RGB"), dtype=np.float64)

    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    x = r / 127.5 - 1.0
    y = g / 127.5 - 1.0
    xy2 = x * x + y * y

    over = float((xy2 > 1.0).mean())
    dr, dg = abs(r.mean() - 127.5), abs(g.mean() - 127.5)

    print(f"file      {p}  ({mode}, {size[0]}x{size[1]})")
    print(f"R mean    {r.mean():8.3f}   (off neutral {r.mean() - 127.5:+.3f})")
    print(f"G mean    {g.mean():8.3f}   (off neutral {g.mean() - 127.5:+.3f})")
    print(f"B mean    {b.mean():8.3f}   min {b.min():.0f} max {b.max():.0f}")
    print(f"x^2+y^2   max {xy2.max():.6f}   frac>1 {over:.6f}")
    print(f"XY std    {np.sqrt(((r - 127.5)**2 + (g - 127.5)**2).mean() / 2.0):8.3f}")

    ok = True
    if dr > XY_MEAN_TOL or dg > XY_MEAN_TOL:
        print(f"REFUSE: R/G are not neutral (|off| {dr:.2f}/{dg:.2f} > {XY_MEAN_TOL}). "
              f"The XY data carries a bias — recomputing B would bake it in.")
        ok = False
    if over > OVERUNIT_TOL:
        print(f"REFUSE: {over * 100:.3f}% of texels have x^2+y^2 > 1 — no real z exists "
              f"there. XY is damaged; RE-EXPORT or RETIRE this set, do not repair it.")
        ok = False
    if not ok and not args.force:
        return 1
    if not ok:
        print("--force given: proceeding with clamping. The result is an approximation.")

    if args.xy_scale != 1.0:
        x *= args.xy_scale
        y *= args.xy_scale
        xy2 = x * x + y * y
        print(f"XY scaled by {args.xy_scale} -> new XY std "
              f"{np.sqrt(((x * 127.5)**2 + (y * 127.5)**2).mean() / 2.0):.3f}")

    z = np.sqrt(np.clip(1.0 - xy2, 0.0, 1.0))
    b_new = np.clip(np.rint((z * 0.5 + 0.5) * 255.0), 0, 255)

    # Independent cross-check: if the channel was merely INVERTED, then
    # 255 - b_stored is a second, arithmetic reconstruction of the same truth.
    if args.xy_scale == 1.0:
        b_inv = 255.0 - b
        d = np.abs(b_new - b_inv)
        corr = float(np.corrcoef(b.ravel(), b_new.ravel())[0, 1])
        print(f"cross-check vs (255 - B_stored): mean |diff| {d.mean():.4f}, "
              f"max {d.max():.0f}, {float((d <= 1).mean()) * 100:.3f}% within 1 level")
        print(f"corr(B_stored, B_repaired) = {corr:+.4f}  "
              f"({'INVERSION confirmed' if corr < -0.99 else 'not a clean inversion'})")

    print(f"B repaired: mean {b_new.mean():.3f}  min {b_new.min():.0f}  max {b_new.max():.0f}")

    if args.dry_run:
        print("--dry-run: nothing written.")
        return 0

    out = np.empty_like(a, dtype=np.uint8)
    out[..., 0] = np.clip(np.rint((x * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
    out[..., 1] = np.clip(np.rint((y * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
    out[..., 2] = b_new.astype(np.uint8)

    bak = p.with_name(p.name + ".corrupt.bak")
    if not bak.exists():
        shutil.copyfile(p, bak)
        print(f"original preserved -> {bak.name}")
    Image.fromarray(out, mode="RGB").save(p, optimize=True)
    print(f"WROTE {p} ({p.stat().st_size / 1e6:.2f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
