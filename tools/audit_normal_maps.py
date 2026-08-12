#!/usr/bin/env python3
"""Audit tangent-space normal maps in the surface library for channel corruption.

WHY THIS EXISTS
    cc_cement_white/normal.png shipped with a DESTROYED blue channel (mean 14.8
    where the library norm is ~252). B=14.8 decodes to z=-0.88 — the normal
    pointed INTO the surface — and it went unnoticed until it was bound to the
    tunnel bore and produced "cloud-grey blotching" that got misdiagnosed as a
    filtering bug. cc_porous_cement (B=8.1) was carrying the same defect at the
    same time. Neither was caught by any gate, because nothing looked.

    This script looks. It is cheap (pure pixel statistics, no engine, no GPU)
    and it is meant to run in CI and before publishing to the asset store.

WHAT IT CHECKS
    A tangent-space normal map stores a unit vector n = (x,y,z) with z >= 0,
    encoded as RGB = (n*0.5+0.5)*255. Three things follow, and each is a check:

    B_LOW      mean(B) far below ~252. z >= 0 means B >= 128 everywhere, and for
               any plausible surface most texels are near-flat, so B clusters
               just under 255. A low mean means the channel is destroyed or the
               map is not tangent-space at all. THIS IS THE CORRUPTION THAT SHIPPED.
    NOT_UNIT   the real discriminator. A tangent-space normal is a UNIT vector, so
               z must equal sqrt(1-x^2-y^2) at every texel. When it does, the map
               is internally consistent and a low mean(B) just means the map is
               LOUD (steep tilts pull mean z down) — not damaged. When it does
               not, the channels disagree and something is wrong. This is what
               separates terrain_grass (B=196 but |n|=1.004, valid) from
               cc_porous_cement (B=8.1, z anti-correlated, destroyed). B_LOW is
               therefore only reported when the map ALSO fails this test.
    XY_BIAS    mean(R) or mean(G) far from 127.5. A tangent-space map should
               average to a flat normal; a large offset is a baked-in directional
               lighting bias (every texel tilted the same way). Not always a bug
               — it can be an art choice — so it is reported as a WARNING and a
               human decides.
    OVER_UNIT  fraction of texels where x^2+y^2 > 1, i.e. no real z exists. A
               few are normal (8-bit rounding); many mean the XY data itself is
               damaged and the map cannot be repaired by recomputing B — it has
               to be re-exported or retired.
    XY_STD     loudness of the XY signal vs the library median. Not a
               correctness check; it flags a map that will read 2-3x harsher
               than everything around it. WARNING only.

USAGE
    python tools/audit_normal_maps.py                     # audit the library, table + verdict
    python tools/audit_normal_maps.py --json out.json     # machine-readable
    python tools/audit_normal_maps.py --gate              # exit 1 if any ERROR (for CI/hooks)
    python tools/audit_normal_maps.py --dir path/to/dir   # audit somewhere else
    python tools/audit_normal_maps.py --paths a/normal.png b/normal.png

EXIT CODES
    0  no ERROR-level findings (warnings may still be present)
    1  at least one ERROR-level finding, with --gate
    2  bad invocation / nothing to audit
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DIR = REPO_ROOT / "assets" / "surface_library"

# Thresholds. Derived from the 34-set audit of assets/surface_library:
# every healthy set sits at B >= 196, and the two corrupt ones were 8.1 and 14.8.
# 128 is also the hard floor implied by z >= 0, so anything below it is
# provably not a tangent-space normal map.
B_MEAN_ERROR = 128.0   # below this, z<0 on average — provably broken
B_MEAN_WARN = 235.0    # below this AND not unit-consistent: flag for a human
# |mean - 127.5| above this = directional bias. The library's honest noise floor
# is ~3.5 grey levels; terrain_grass sits at 8.3 (R) / 11.7 (G). 6.0 separates
# them cleanly without nagging about every set.
XY_MEAN_WARN = 6.0
OVERUNIT_WARN = 0.02   # >2% of texels with x^2+y^2 > 1
OVERUNIT_ERROR = 0.20  # >20% means the XY data itself is damaged
XY_STD_RATIO_WARN = 2.0  # this map is >2x louder than the library median
# mean |z - sqrt(1-x^2-y^2)| in [-1,1] units. Healthy sets measure 0.002-0.011;
# a broken one is off by ~1.0. 0.05 is far above the noise, far below a defect.
Z_ERR_WARN = 0.05


def analyse(path: Path) -> dict:
    """Pixel statistics for one normal map. Returns a plain dict (JSON-safe)."""
    with Image.open(path) as im:
        rgb = im.convert("RGB")
        w, h = rgb.size
        a = np.asarray(rgb, dtype=np.float32)

    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    # Decode to [-1,1]. 127.5 is the true zero of the (v*0.5+0.5)*255 encoding.
    x = r / 127.5 - 1.0
    y = g / 127.5 - 1.0
    z = b / 127.5 - 1.0
    xy2 = x * x + y * y
    # Is z what x,y imply? This is the real corruption test — see NOT_UNIT above.
    z_pred = np.sqrt(np.clip(1.0 - xy2, 0.0, 1.0))
    n_len = np.sqrt(xy2 + z * z)

    return {
        "n_len_mean": float(n_len.mean()),
        "n_len_std": float(n_len.std()),
        "z_err_mean": float(np.abs(z - z_pred).mean()),
        "path": str(path).replace("\\", "/"),
        "set": path.parent.name,
        "size": [w, h],
        "r_mean": float(r.mean()),
        "g_mean": float(g.mean()),
        "b_mean": float(b.mean()),
        "b_min": float(b.min()),
        "b_max": float(b.max()),
        "xy_std": float(np.sqrt(((r - 127.5) ** 2 + (g - 127.5) ** 2).mean() / 2.0)),
        "over_unit_frac": float((xy2 > 1.0).mean()),
        "xy2_max": float(xy2.max()),
    }


def classify(rec: dict, xy_std_median: float) -> list[tuple[str, str]]:
    """Return [(level, message)] for one record. level in {ERROR, WARN}."""
    out: list[tuple[str, str]] = []
    b = rec["b_mean"]

    zerr = rec["z_err_mean"]
    consistent = zerr <= Z_ERR_WARN

    if b < B_MEAN_ERROR:
        out.append(("ERROR",
                    f"B_LOW: blue mean {b:.1f} < {B_MEAN_ERROR:.0f}. z decodes negative — "
                    f"the normal points INTO the surface. Channel is destroyed."))
    elif b < B_MEAN_WARN and not consistent:
        out.append(("WARN",
                    f"B_LOW: blue mean {b:.1f} is below the healthy band (~252) AND z does not "
                    f"match x,y — inspect."))

    if not consistent:
        out.append(("WARN",
                    f"NOT_UNIT: mean |z - sqrt(1-x^2-y^2)| = {zerr:.4f} (|n| = "
                    f"{rec['n_len_mean']:.4f}). The channels disagree — this is not a "
                    f"consistent tangent-space normal."))

    ou = rec["over_unit_frac"]
    if ou > OVERUNIT_ERROR:
        out.append(("ERROR",
                    f"OVER_UNIT: {ou * 100:.1f}% of texels have x^2+y^2 > 1 (max {rec['xy2_max']:.3f}). "
                    f"XY is damaged too — this set cannot be repaired by recomputing B; "
                    f"re-export or retire it."))
    elif ou > OVERUNIT_WARN:
        out.append(("WARN",
                    f"OVER_UNIT: {ou * 100:.1f}% of texels have x^2+y^2 > 1 (max {rec['xy2_max']:.3f})."))

    for ch, mean in (("R", rec["r_mean"]), ("G", rec["g_mean"])):
        if abs(mean - 127.5) > XY_MEAN_WARN:
            out.append(("WARN",
                        f"XY_BIAS: {ch} mean {mean:.1f} is {mean - 127.5:+.1f} off neutral (127.5) — "
                        f"a baked-in directional bias. Verify this is intentional."))

    if xy_std_median > 0 and rec["xy_std"] > XY_STD_RATIO_WARN * xy_std_median:
        out.append(("WARN",
                    f"XY_STD: {rec['xy_std']:.1f} is {rec['xy_std'] / xy_std_median:.1f}x the library "
                    f"median ({xy_std_median:.1f}) — this map will read much harsher than its neighbours."))

    return out


def collect(args) -> list[Path]:
    if args.paths:
        return [Path(p) for p in args.paths]
    root = Path(args.dir) if args.dir else DEFAULT_DIR
    return sorted(root.glob("*/normal.png"))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="audit_normal_maps.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", help=f"surface library root (default {DEFAULT_DIR})")
    ap.add_argument("--paths", nargs="*", help="explicit normal-map files to audit")
    ap.add_argument("--json", help="write the full records to this JSON file")
    ap.add_argument("--gate", action="store_true", help="exit 1 if any ERROR-level finding")
    ap.add_argument("--quiet", action="store_true", help="only print findings, not the table")
    args = ap.parse_args(argv)

    files = collect(args)
    files = [f for f in files if f.is_file()]
    if not files:
        print("no normal maps found — nothing to audit", file=sys.stderr)
        return 2

    recs = []
    for f in files:
        try:
            recs.append(analyse(f))
        except Exception as e:  # a map we cannot even open is itself a finding
            recs.append({"path": str(f).replace("\\", "/"), "set": f.parent.name,
                         "error": f"{type(e).__name__}: {e}"})

    good = [r for r in recs if "error" not in r]
    xy_std_median = float(np.median([r["xy_std"] for r in good])) if good else 0.0

    if not args.quiet:
        print(f"{'set':<26} {'size':>11} {'R':>7} {'G':>7} {'B':>7} {'Bmin':>5} "
              f"{'XYstd':>6} {'ovr%':>6} {'|n|':>6} {'zerr':>7}")
        print("-" * 97)
        for r in sorted(recs, key=lambda r: r.get("b_mean", -1)):
            if "error" in r:
                print(f"{r['set']:<26} {'UNREADABLE':>11}  {r['error']}")
                continue
            print(f"{r['set']:<26} {r['size'][0]}x{r['size'][1]:<6} "
                  f"{r['r_mean']:>7.1f} {r['g_mean']:>7.1f} {r['b_mean']:>7.1f} "
                  f"{r['b_min']:>5.0f} {r['xy_std']:>6.1f} {r['over_unit_frac'] * 100:>6.2f} "
                  f"{r['n_len_mean']:>6.3f} {r['z_err_mean']:>7.4f}")
        print()
        print(f"library median XY std = {xy_std_median:.1f}   sets audited = {len(recs)}")
        print()

    n_err = n_warn = 0
    for r in sorted(recs, key=lambda r: r.get("b_mean", -1)):
        findings = ([("ERROR", r["error"])] if "error" in r
                    else classify(r, xy_std_median))
        r["findings"] = [{"level": lv, "message": ms} for lv, ms in findings]
        for level, msg in findings:
            n_err += level == "ERROR"
            n_warn += level == "WARN"
            print(f"[{level}] {r['set']}: {msg}")

    print()
    print(f"RESULT: {len(recs)} audited, {n_err} error(s), {n_warn} warning(s)")

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(
            {"xy_std_median": xy_std_median, "errors": n_err,
             "warnings": n_warn, "records": recs}, indent=1), encoding="utf-8")
        print(f"wrote {out}")

    if args.gate and n_err:
        print("GATE FAILED: corrupt normal map(s) present — see ERROR lines above.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
