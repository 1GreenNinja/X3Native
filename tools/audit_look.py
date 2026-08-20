#!/usr/bin/env python3
"""Measure whether a CAPTURE looks art-directed or looks like slop.

WHY THIS EXISTS. docs/NO_SLOP.md has eleven rules and NINE of them are
CORRECTNESS rules -- grep first, the contact law, paired values, unset flags,
events not polling, measure don't vibe, write the receipt. Only #2 ("eyes on
against a real reference") and #3 ("no untextured stand-ins") are about LOOK,
and both are unfalsifiable prose. That is how W-UNDERRIVER v2 shipped a cavern
that obeys rule 11, cites it in its own source, measures everything and writes
an honest receipt -- and still reads as one grey tube. A rule that cannot fail
is a rule that gets admired, not followed.

The precedent that DOES work is tools/audit_normal_maps.py: it found a blue
channel decoding to z = -0.87 that every human eye had walked past, because it
MEASURED instead of looking. This is the same move for the beauty bar.

WHAT IT MEASURES, and why each one separates RiftHub from a graybox:

  KEY        p99 luminance -- is ANYTHING in the frame actually lit? A directed
             shot has a light source and a falloff; ambient-only slop tops out
             in the midtones however colourful it is.
  RANGE      p99 - p1. The single strongest discriminator measured. A frame that
             carries both real shadow and real highlight has been LIT; one that
             spans half the scale has been filled.

TWO METRICS I TRIED AND THREW AWAY, with the numbers, because shipping a check
that does not discriminate is its own kind of slop:
  DETAIL (mean |Laplacian|) FAILED. The flat cavern scores 0.0219 against
  RiftHub's 0.0192 -- a noisy tiled rock texture makes as much high-frequency
  energy as designed greebles do. It cannot tell busy from detailed.
  HUE VARIETY FAILED. Cavern 3 buckets, RiftHub 2. Colour count says nothing
  about whether the colour was directed.

CALIBRATION (perceptual sRGB luma, not linear -- the first cut measured in
linear space, where +/-0.1 is an enormous perceptual band, and duly failed
RiftHub itself):
  RiftHub hero      p99 0.798  range 0.791   <- the bar
  RiftHub gate      p99 0.787  range 0.780
  tunnel approach   p99 0.924  range 0.708
  elevator cab      p99 0.816  range 0.664   <- flat, and looks it
  cavern wide       p99 0.564  range 0.525   <- flat, and looks it
  cavern great hall p99 0.502  range 0.476

NOT A TASTE MACHINE. It cannot tell you a frame is beautiful. It tells you a
frame is FLAT, and flat is the failure this codebase actually keeps shipping.
Passing is necessary, not sufficient -- rule #2 (eyes on) still applies.

USAGE
  python tools/audit_look.py shot.png [more.png ...]
  python tools/audit_look.py --gate docs/screenshots/tunnel/*.png
  python tools/audit_look.py --json out.json shots/*.png
"""
import argparse, json, sys
import numpy as np
from PIL import Image

# Thresholds calibrated against a KNOWN-GOOD frame (rifthub_r10/R10B_ibl_hero)
# and a KNOWN-FLAT one (shots_underriver/13_cavern_wide). See --explain.
T = dict(white_min=0.70,   # p99: something must be LIT
         range_min=0.70)   # p99-p1: the frame must span shadow to highlight


def load(path):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, dtype=np.float32) / 255.0
    return a


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def measure(path):
    rgb = load(path)
    # PERCEPTUAL space on purpose: linear luminance crushes everything toward
    # zero, and a +/-0.1 window there spans most of what the eye calls dark.
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

    p1, p50, p99 = (float(np.percentile(lum, q)) for q in (1, 50, 99))

    return dict(path=str(path), p1=p1, p50=p50, p99=p99, rng=p99 - p1)


def verdict(m):
    fails = []
    if m["p99"] < T["white_min"]:
        fails.append(f"NOT LIT: p99 {m['p99']:.3f} < {T['white_min']} — nothing "
                     "in this frame is a highlight; it is filled, not lit")
    if m["rng"] < T["range_min"]:
        fails.append(f"NO RANGE: p99-p1 {m['rng']:.3f} < {T['range_min']} — the "
                     "frame does not span shadow to highlight")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots", nargs="+")
    ap.add_argument("--gate", action="store_true", help="exit 1 if any shot fails")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    print(f"{'shot':<46} {'p1':>7} {'p99':>7} {'range':>7}  verdict")
    print("-" * 92)
    out, bad = [], 0
    for s in a.shots:
        try:
            m = measure(s)
        except Exception as e:
            print(f"{s:<44} {'':>6} unreadable ({e})")
            bad += 1
            continue
        f = verdict(m)
        m["fails"] = f
        out.append(m)
        name = s.replace("\\", "/").split("/")[-1][:45]
        print(f"{name:<46} {m['p1']:7.3f} {m['p99']:7.3f} {m['rng']:7.3f}  "
              + ("PASS" if not f else f"FAIL x{len(f)}"))
        for line in f:
            print(f"    - {line}")
        if f:
            bad += 1

    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)

    print("-" * 92)
    print(f"{len(out)} shot(s), {bad} failing")
    return 1 if (a.gate and bad) else 0


if __name__ == "__main__":
    sys.exit(main())
