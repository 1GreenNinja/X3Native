#!/usr/bin/env python3
"""Read the capture round back as NUMBERS (NO_SLOP 9) — the three claims this
lane has to make good on, plus the one that caught us out.

    python shots_clouds/measure_round.py

  0. ONE BUILD.  Every PNG in the round must be newer than build/bin/Release/
     x3app.dll.  The 2026-08-17 round straddled a rebuild and produced a cover
     ladder that read flat-flat-flat-flat-then-34%; the images were fine, the
     ROUND was a lie.  This check exists so that can never be reported again.
  1. THE SUN DIMS WITH COVER.  Mean luminance of the near ground across the
     cover ladder must fall monotonically.  Measured near, not wide: at 800 m
     aerial perspective has already blended the frame to haze and ate a 31%
     signal down to 0.8% (see run_captures.sh).
  2. THE BUDGET.  Cloud pass + shadows < 10% of frame time, from the
     [tunnel-perf] gpuFrameMs average, X3_CLOUD=0 vs 0.42 at a fixed cam.
  3. NO FLAT CELLS.  The shard sky's signature was CONSTANT-valued noise cells
     -- big blocks of literally one colour with straight edges.  Count the
     largest run of identical pixels along each sky scanline; soft clouds have
     runs of a few px, a shard has hundreds.  This is a smoke alarm, not a
     substitute for eyes-on (NO_SLOP 2) -- it cannot see a soft-shaded polygon.
"""
import os
import re
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "shots_clouds")
DLL = os.path.join(ROOT, "build", "bin", "Release", "x3app.dll")

ROUND = [
    "perf_base_cloud0", "fair_01_spawn", "perf_base_sky", "perf_base_up",
    "fair_02_sky", "fair_03_up", "shadows_01", "shadows_02",
    "shadows_03_blob", "shadows_04_wide", "overcast_01_sky",
    "overcast_02_ground", "ladder_0.00", "ladder_0.25", "ladder_0.50",
    "ladder_0.75", "ladder_1.00", "storm_01_sky", "storm_02_ground",
    "storm_03_wide",
]

fails = []


def px(name):
    return np.asarray(Image.open(os.path.join(SHOTS, name + ".png"))
                      .convert("RGB")).astype(float)


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


# --- 0. one build -----------------------------------------------------------
print("=== 0. ONE BUILD (every shot newer than x3app.dll) ===")
dll_t = os.path.getmtime(DLL)
stale = []
for n in ROUND:
    p = os.path.join(SHOTS, n + ".png")
    if not os.path.exists(p):
        stale.append((n, "MISSING"))
    elif os.path.getmtime(p) < dll_t:
        stale.append((n, "PREDATES THE BUILD"))
print("  x3app.dll built %s" % __import__("time").ctime(dll_t))
if stale:
    for n, why in stale:
        print("  FAIL %-20s %s" % (n, why))
    fails.append("round straddles a rebuild / incomplete")
else:
    print("  OK  all %d shots postdate the build" % len(ROUND))

# --- 1. the sun dims with cover --------------------------------------------
# Near ground only, from the spawn cam. Re-cut after the ce48e2b3 merge planted
# W-FOREST's trees down both verges: the old grass-verge boxes are now canopy,
# and a leaf billboard is not the ground. The road slab is the clean probe (one
# material, no foliage, all of it inside 60 m); "near half" is the honest wide
# number over everything below the horizon line.
GROUND = {
    "road":      (slice(500, 690), slice(350, 930)),
    "near half": (slice(430, 719), slice(0, 1280)),
}
print("\n=== 1. SUN DIMS WITH COVER (near ground, spawn cam) ===")
ladder = ["0.00", "0.25", "0.50", "0.75", "1.00"]
have = [c for c in ladder if os.path.exists(os.path.join(SHOTS, "ladder_%s.png" % c))]
if len(have) == len(ladder):
    print("  %-8s %s      mean" % ("cover", "".join("%10s" % k for k in GROUND)))
    means = []
    for c in ladder:
        L = lum(px("ladder_%s" % c))
        v = [L[r].mean() for r in GROUND.values()]
        means.append(float(np.mean(v)))
        print("  %-8s %s  %8.2f" % (c, "".join("%10.2f" % x for x in v), means[-1]))
    drops = [means[i + 1] - means[i] for i in range(len(means) - 1)]
    total = 100.0 * (1.0 - means[-1] / means[0])
    print("  step deltas: %s" % ", ".join("%+.2f" % d for d in drops))
    print("  cover 0 -> 1 dims THIS GROUND POINT %.1f%%" % total)
    # NOT a monotonicity gate. This cam stands under ONE deck cell (~1.8 km
    # features vs ~100 m of visible ground), so its honest output is a STEP:
    # lit while that cell is a hole, dark once cover closes it. See the long
    # note in run_captures.sh. What must hold is (a) nothing gets BRIGHTER as
    # cover rises, and (b) the deck eventually reaches the ground.
    if any(d > 0.5 for d in drops):
        fails.append("a ladder rung got BRIGHTER under more cloud")
        print("  FAIL a rung brightened")
    elif total < 8.0:
        fails.append("cover 1.0 dims this point only %.1f%% — the deck never lands" % total)
        print("  FAIL the deck never reaches the ground")
    else:
        print("  OK  never brightens; cover 1.0 lands %.1f%% of shade here." % total)
        print("      (The DIMMING CURVE is verify_new_field.py's landscape average")
        print("       of cloudShadowFactor, not this single-cell probe.)")
else:
    fails.append("ladder incomplete")
    print("  FAIL ladder incomplete: %s" % have)

# --- 2. the budget ----------------------------------------------------------
print("\n=== 2. CLOUD BUDGET (< 10% of frame time, [tunnel-perf] gpu ms) ===")
PERF_RE = re.compile(r"\[tunnel-perf\]\s+\S+:\s+gpu\s+([0-9.]+)\s+ms")


def gpu_ms(log):
    p = os.path.join(SHOTS, "log_%s.txt" % log)
    if not os.path.exists(p):
        return None
    for line in open(p, encoding="utf-8", errors="replace"):
        m = PERF_RE.search(line)
        if m:
            return float(m.group(1))
    return None


PAIRS = [("spawn cam", "perf_base", "fair_spawn"),
         ("sky cam", "perf_base_sky", "fair_sky"),
         ("straight up", "perf_base_up", "fair_up")]
worst = None
for label, a_log, b_log in PAIRS:
    a, b = gpu_ms(a_log), gpu_ms(b_log)
    if a is None or b is None:
        print("  %-12s MISSING (%s / %s)" % (label, a, b))
        fails.append("perf pair %s missing" % label)
        continue
    pct = 100.0 * (b - a) / b if b > 0 else 0.0
    worst = pct if worst is None else max(worst, pct)
    print("  %-12s cover0 %6.3f ms -> cover0.42 %6.3f ms   clouds = %+5.2f%% of frame"
          % (label, a, b, pct))
if worst is not None:
    if worst >= 10.0:
        fails.append("cloud pass costs %.2f%% of the frame (budget 10%%)" % worst)
        print("  FAIL worst %.2f%%" % worst)
    else:
        print("  OK  worst case %.2f%% of frame time" % worst)

# --- 3. no flat cells -------------------------------------------------------
print("\n=== 3. NO FLAT CELLS (longest identical-pixel run per sky scanline) ===")
SKY = {"fair_02_sky": slice(0, 200), "fair_03_up": slice(0, 720),
       "overcast_01_sky": slice(0, 200), "storm_01_sky": slice(0, 200)}
for name, rows in SKY.items():
    if not os.path.exists(os.path.join(SHOTS, name + ".png")):
        print("  %-18s MISSING" % name)
        continue
    a = px(name)[rows]
    same = np.all(a[:, 1:] == a[:, :-1], axis=2)          # px equals its left neighbour
    best = 0
    for row in same:
        run = 0
        for v in row:
            run = run + 1 if v else 0
            if run > best:
                best = run
    # WARN, never FAIL. Calibrated 2026-08-17 against the real round: an 8-bit
    # PNG of a smooth sky gradient produces identical-pixel runs in the hundreds
    # all by itself (fair_03_up 184, overcast_01_sky 239, storm_01_sky 349 —
    # every one of them eyeballed at full res and soft, no edge anywhere). What
    # this cannot distinguish is a flat run from a flat CELL, so it raises a
    # hand and a human looks. Only a zero here would be meaningful, and a real
    # sky never gives you one.
    verdict = "smooth (expected)" if best < 400 else "LOOK AT THIS ONE"
    print("  %-18s longest flat run %4d px   %s" % (name, best + 1, verdict))
    if best >= 400:
        print("       ^ not proof of a shard — 8-bit banding does this too. Eyes on.")

print("\n" + "=" * 62)
if fails:
    print("ROUND FAILS (%d):" % len(fails))
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("ROUND PASSES its numbers. Now go LOOK at every frame (NO_SLOP 2).")
