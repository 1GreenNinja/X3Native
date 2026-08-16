#!/usr/bin/env python3
"""Compose the final tach face: Blender-rendered glossy bezel + vector scale.

Two-stage on purpose:
  * the BEZEL is rendered in Blender (tools/render_gauge_bezel.py) because
    metal is reflection, and painting fake gloss in 2D never convinces;
  * the SCALE — ticks, numerals, redline, labels — is drawn here in PIL,
    because vector text and hairline ticks are crisper drawn than rendered,
    and restyling the scale then costs seconds instead of a re-render.

Style follows the owner's reference: cyan ticks over the usable range, white
numerals, a hard red band over the limiter.

Run (after the Blender step):
  python tools/compose_gauge_dial.py
Outputs:
  assets/ui/gauge_dial.png     the finished face
  assets/ui/gauge_needle.png   8x8 atlas, 64 needle rotations
"""

import math
import os
from PIL import Image, ImageDraw, ImageFont

SWEEP_START_DEG = 216.0
SWEEP_TOTAL_DEG = 252.0
RPM_MAX         = 8000.0
REDLINE_RPM     = 7500.0        # matches vd.maxEngineRPM

SS        = 4
NEEDLE_PX = 2048
ATLAS_N   = 8

HERE   = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.normpath(os.path.join(HERE, "..", "assets", "ui"))
BEZEL  = os.path.join(UI_DIR, "gauge_bezel.png")

C_TICK   = (86, 220, 248, 255)
C_TICK_D = (44, 128, 152, 235)
C_NUM    = (244, 249, 255, 255)
C_RED    = (228, 34, 26, 255)
C_LABEL  = (110, 200, 224, 255)
C_NEEDLE = (232, 40, 26, 255)

# The bezel render frames the ring to the image edge; the face starts here.
FACE_R_FRAC = 0.815


def ang_for(f):
    return SWEEP_START_DEG - SWEEP_TOTAL_DEG * f


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + math.cos(a) * r, cy - math.sin(a) * r)


def font(px, bold=True):
    names = ("segoeuib.ttf", "arialbd.ttf") if bold else ("segoeui.ttf", "arial.ttf")
    for n in names:
        p = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", n)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except Exception:
                pass
    return ImageFont.load_default()


def ctext(d, cx, cy, s, f, col):
    bb = d.textbbox((0, 0), s, font=f)
    d.text((cx - (bb[2] - bb[0]) / 2 - bb[0], cy - (bb[3] - bb[1]) / 2 - bb[1]),
           s, font=f, fill=col)


def compose():
    bez = Image.open(BEZEL).convert("RGBA")
    S = bez.size[0] * SS
    bez = bez.resize((S, S), Image.LANCZOS)

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.alpha_composite(bez)
    d = ImageDraw.Draw(img)
    c = S / 2.0
    Rf = S * 0.5 * FACE_R_FRAC

    # Darken the rendered face so the scale reads. Radial, so the bezel's own
    # inner shadow is preserved rather than flattened by a hard disc.
    steps = 130
    for i in range(steps, 0, -1):
        t = i / steps
        rr = Rf * t
        a = int(236 * (1.0 - 0.30 * t))
        d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=(6, 7, 10, a))

    r_out = Rf * 0.965
    r_maj = Rf * 0.815
    r_min = Rf * 0.882
    r_num = Rf * 0.690

    # arcs: cyan over the usable range, red over the limiter
    ra = Rf * 0.995
    d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(0.0),
          end=-ang_for(REDLINE_RPM / RPM_MAX), fill=C_TICK, width=int(4 * SS))
    d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(REDLINE_RPM / RPM_MAX),
          end=-ang_for(1.0), fill=C_RED, width=int(9 * SS))

    fnum = font(int(Rf * 0.165))
    n = int(RPM_MAX / 200)
    for i in range(n + 1):
        rpm = i * 200.0
        f = rpm / RPM_MAX
        a = ang_for(f)
        hot = rpm >= REDLINE_RPM
        if i % 5 == 0:
            d.line([polar(c, c, r_maj, a), polar(c, c, r_out, a)],
                   fill=(C_RED if hot else C_TICK), width=int(5.5 * SS))
            tx, ty = polar(c, c, r_num, a)
            ctext(d, tx, ty, str(int(rpm / 1000)), fnum, C_RED if hot else C_NUM)
        else:
            d.line([polar(c, c, r_min, a), polar(c, c, r_out, a)],
                   fill=(C_RED if hot else C_TICK_D), width=int(2.4 * SS))

    # NOTHING else goes on the face. The two vertical strips between the hub and
    # the numerals are the only clear areas on a dial this dense, and the HOST
    # owns them: the gear digit above the hub, the MPH readout below it. Earlier
    # passes printed "RPM x1000" and a REDLINE badge exactly there, so the live
    # text landed on top of baked text. The red arc and the red 8 already say
    # where the limiter is; good instruments label very little.
    #
    # A boxed badge was tried here too and read as web UI, not as an instrument.
    return img.resize((bez.size[0] // SS, bez.size[1] // SS), Image.LANCZOS)


# ---------------------------------------------------------------------------
# BOOST GAUGE — the same bezel, a pressure scale.
#
# It reads in PSI and it goes NEGATIVE, because a boost gauge spends most of its
# life in vacuum: the engine pumping against a closed throttle plate. A gauge
# that sat at zero off-throttle would be the tell that there is no real manifold
# model behind it. Zero gets a brighter tick, since crossing into positive
# pressure is the event the driver is actually watching for.
#
# Smaller than the tach on screen, so the scale is coarser on purpose: fewer
# numerals, heavier ticks, no minor ticks below zero.
# ---------------------------------------------------------------------------
BOOST_MIN_PSI = -10.0
BOOST_MAX_PSI =  20.0
BOOST_HOT_PSI =  16.0          # matches TurboParams::maxPsi — over this is overboost

C_VAC = (150, 162, 178, 255)   # vacuum side reads cool grey, not "active" cyan


def boost_frac(psi):
    return (psi - BOOST_MIN_PSI) / (BOOST_MAX_PSI - BOOST_MIN_PSI)


def compose_boost():
    bez = Image.open(BEZEL).convert("RGBA")
    S = bez.size[0] * SS
    bez = bez.resize((S, S), Image.LANCZOS)

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.alpha_composite(bez)
    d = ImageDraw.Draw(img)
    c = S / 2.0
    Rf = S * 0.5 * FACE_R_FRAC

    steps = 130
    for i in range(steps, 0, -1):
        t = i / steps
        rr = Rf * t
        d.ellipse([c - rr, c - rr, c + rr, c + rr],
                  fill=(6, 7, 10, int(236 * (1.0 - 0.30 * t))))

    r_out = Rf * 0.965
    r_maj = Rf * 0.795
    r_min = Rf * 0.880
    r_num = Rf * 0.650

    ra = Rf * 0.995
    z = boost_frac(0.0)
    hotf = boost_frac(BOOST_HOT_PSI)
    d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(0.0), end=-ang_for(z),
          fill=C_VAC, width=int(3 * SS))                        # vacuum
    d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(z), end=-ang_for(hotf),
          fill=C_TICK, width=int(4 * SS))                       # useful boost
    d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(hotf), end=-ang_for(1.0),
          fill=C_RED, width=int(9 * SS))                        # overboost

    # Bigger numerals than the tach's proportionally, because this dial is drawn
    # at ~0.70 of the tach's radius on screen and 0.150 put it under 12 px.
    fnum = font(int(Rf * 0.165))
    psi = BOOST_MIN_PSI
    while psi <= BOOST_MAX_PSI + 0.01:
        f = boost_frac(psi)
        a = ang_for(f)
        major = (abs(psi) % 5.0) < 0.01
        hot = psi >= BOOST_HOT_PSI
        if major:
            zero = abs(psi) < 0.01
            col = C_RED if hot else ((240, 246, 255, 255) if zero
                                     else (C_TICK if psi > 0 else C_VAC))
            d.line([polar(c, c, r_maj, a), polar(c, c, r_out, a)],
                   fill=col, width=int((7.0 if zero else 5.5) * SS))
            tx, ty = polar(c, c, r_num, a)
            ctext(d, tx, ty, "%d" % int(round(psi)), fnum,
                  C_RED if hot else (C_NUM if psi >= 0 else C_VAC))
        elif psi > 0:      # no minor ticks on the vacuum side — it is coarser
            d.line([polar(c, c, r_min, a), polar(c, c, r_out, a)],
                   fill=(C_RED if hot else C_TICK_D), width=int(2.4 * SS))
        psi += 1.0

    # This face DOES get a label. Unlike the tach, nothing else is drawn on it,
    # and a bare needle over unlabelled numbers is genuinely ambiguous — 0 to 20
    # of what? The host writes the live psi under the hub; this names the unit.
    ctext(d, c, c - Rf * 0.40, "BOOST  psi", font(int(Rf * 0.128)), C_LABEL)

    return img.resize((bez.size[0] // SS, bez.size[1] // SS), Image.LANCZOS)


def needle_atlas():
    cell_ss = (NEEDLE_PX // ATLAS_N) * SS
    atlas = Image.new("RGBA", (NEEDLE_PX, NEEDLE_PX), (0, 0, 0, 0))
    frames = ATLAS_N * ATLAS_N
    base = Image.new("RGBA", (cell_ss, cell_ss), (0, 0, 0, 0))
    bd = ImageDraw.Draw(base)
    c = cell_ss / 2.0
    L, tail = cell_ss * 0.400, cell_ss * 0.085
    wr, wt = cell_ss * 0.027, cell_ss * 0.0085

    def blade(off, col):
        bd.polygon([(c - tail + off, c - wr * 0.62 + off),
                    (c - tail + off, c + wr * 0.62 + off),
                    (c + L + off,    c + wt + off),
                    (c + L + off,    c - wt + off)], fill=col)

    # drop shadow first: the needle must separate from the dark face even where
    # it crosses the unlit lower half.
    blade(cell_ss * 0.012, (0, 0, 0, 150))
    blade(0.0, C_NEEDLE)
    # a lighter top edge so the blade reads as a formed part, not a flat decal
    bd.polygon([(c - tail, c - wr * 0.62), (c - tail, c - wr * 0.20),
                (c + L, c - wt * 0.25), (c + L, c - wt)], fill=(255, 142, 128, 220))

    # hub: dark boss, brushed ring, red cap — the chrome centre of the bezel
    hr = cell_ss * 0.062
    bd.ellipse([c - hr, c - hr, c + hr, c + hr], fill=(14, 15, 19, 255))
    for i, (rr, col) in enumerate(((0.92, (172, 178, 188, 255)),
                                   (0.80, (86, 92, 102, 255)),
                                   (0.62, (26, 28, 34, 255)))):
        r = hr * rr
        bd.ellipse([c - r, c - r, c + r, c + r], fill=col)
    hr2 = hr * 0.36
    bd.ellipse([c - hr2, c - hr2, c + hr2, c + hr2], fill=C_NEEDLE)

    cell = NEEDLE_PX // ATLAS_N
    for i in range(frames):
        rot = base.rotate(ang_for(i / (frames - 1)), resample=Image.BICUBIC, center=(c, c))
        atlas.paste(rot.resize((cell, cell), Image.LANCZOS),
                    ((i % ATLAS_N) * cell, (i // ATLAS_N) * cell))
    return atlas


def main():
    if not os.path.exists(BEZEL):
        raise SystemExit("missing %s — run tools/render_gauge_bezel.py first" % BEZEL)
    dial = compose()
    p1 = os.path.join(UI_DIR, "gauge_dial.png")
    dial.save(p1)
    print("wrote", p1, dial.size)
    # The boost gauge shares the needle atlas: same bezel, same sweep start and
    # span, so frame i points at the same angle on both faces. Only the meaning
    # of the angle differs, and that lives in the scale, not the needle.
    boost = compose_boost()
    p3 = os.path.join(UI_DIR, "gauge_boost.png")
    boost.save(p3)
    print("wrote", p3, boost.size)
    na = needle_atlas()
    p2 = os.path.join(UI_DIR, "gauge_needle.png")
    na.save(p2)
    print("wrote", p2, na.size)


if __name__ == "__main__":
    main()
