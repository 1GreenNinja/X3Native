#!/usr/bin/env python3
"""Compose the car cluster's dial art: Blender-rendered metal bezel + vector scale.

Two-stage on purpose:
  * the BEZEL is rendered in Blender (tools/render_gauge_bezel.py) because
    metal is reflection, and painting fake gloss in 2D never convinces;
  * the SCALE — ticks, numerals, redline, labels — is drawn here in PIL,
    because vector text and hairline ticks are crisper drawn than rendered,
    and restyling the scale then costs seconds instead of a re-render.

THIS SCRIPT IS THE ONLY OWNER of gauge_dial / gauge_boost / gauge_needle /
gauge_nos. tools/make_gauge_textures.py owns gauge_gate and NOTHING ELSE — it
used to also write the dial and the needle atlas at a different sweep, so
regenerating the gate silently swapped the tach for a face whose needle no
longer matched its own scale. See that script's main() docstring.

Run (after the Blender step):
  python tools/compose_gauge_dial.py
Outputs:
  assets/ui/gauge_dial.png     2048^2      the finished tach face
  assets/ui/gauge_boost.png    2048^2      the boost face (same bezel, psi scale)
  assets/ui/gauge_needle.png   4096^2      8x8 atlas, 64 needle rotations
  assets/ui/gauge_nos.png      2048x1024   8x4 atlas, 32 fill states

================================ THE 2026-08 QUALITY PASS ====================
Owner, on the shipped cluster: it should look like "the quality that a game set
30 years after NFS should look like". The previous face was CORRECT — right
sweep, right numbers, paired with the model — and still read as a diagram: a
flat charcoal disc with hairline ticks, no depth, no light in it. What changed,
and the reasoning for each, because "made it nicer" is not a receipt:

  1. RESOLUTION. Bezel 1024 -> 2048, faces 1024 -> 2048, needle atlas
     2048 -> 4096 (256 px cells -> 512). The tach draws at 2R = 0.30 * screen
     height; on a 4K panel that is ~648 px being magnified out of a 256 px
     needle cell. Everything was soft because it was being enlarged.

  2. DARK GLASS, not grey card (X3_WORLD_RULES rule 7 — "near-black inset pane"
     is the standard for every display surface in this game, and an instrument
     face is one). The face is now a near-black radial with a genuine GLASS
     read on top: a broad, very faint elliptical sheen across the upper third,
     and a dark inner shadow where the pane meets the bezel lip. Rule 5's
     anti-glare law applies: the sheen is wide and weak, so it is a sheen and
     never a hot orb over the numerals.

  3. TICKS ARE TAPERED POLYGONS, not lines. PIL's line() with a wide pen gives
     square, unhinted ends that shimmer when downsampled. A four-point polygon
     that is narrower at the hub end reads as a machined mark, holds its edges
     through the LANCZOS downsample, and is what "crisp AA ticks" actually
     means at this size.

  4. THE REDLINE READS. It was a red arc of the same weight as the cyan one.
     Now: a heavier band, its own soft glow, red ticks AND red numerals through
     the sector, plus a faint red wash on the FACE behind it. A limiter you can
     see with your eyes off the numbers is the entire job of a redline.

  5. SOFT GLOWS, done cheaply and correctly. Every luminous element is drawn
     twice — once onto a glow layer that is blurred and composited UNDER the
     crisp layer, once crisp on top. The glow layer is rasterised at a quarter
     scale and scaled back up: a gaussian blur on a 6144^2 buffer is minutes,
     and at these blur radii the two are indistinguishable.

  6. HOLOGRAPHIC ACCENTS, kept tasteful. A cyan hairline ring inboard of the
     numerals and a faint cyan sector wash under the live range. They are low
     alpha on purpose: the brightness hierarchy the world rules set out is
     TEXT > schematic > decoration, and an accent that competes with the
     numerals has failed at being an accent.

Layout and positions are UNCHANGED — same sweep, same radii bands, same numeral
placement, same clear strips for the host's gear digit and MPH readout. The
host's arithmetic (app/gauge_hud.cpp) is not touched by any of this.
"""

import math
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SWEEP_START_DEG = 216.0
SWEEP_TOTAL_DEG = 252.0
RPM_MAX         = 8000.0
REDLINE_RPM     = 7500.0        # matches vd.maxEngineRPM

# SS 3 at a 2048 bezel = a 6144 px working buffer. Higher is not free: the
# working image is ~150 MB per layer at SS 3 and 270 MB at SS 4, and the visible
# difference after a LANCZOS downsample to 2048 is nil.
SS        = 3
NEEDLE_PX = 4096
ATLAS_N   = 8
GLOW_DIV  = 4                   # glow layers rasterise at 1/4 and scale back up

HERE   = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.normpath(os.path.join(HERE, "..", "assets", "ui"))
BEZEL  = os.path.join(UI_DIR, "gauge_bezel.png")

C_TICK   = (120, 232, 255, 255)   # major ticks / live-range arc
C_TICK_D = (58, 146, 174, 240)    # minor ticks
C_NUM    = (246, 251, 255, 255)
C_RED    = (238, 46, 34, 255)
C_LABEL  = (120, 206, 230, 255)
C_NEEDLE = (236, 44, 28, 255)
C_GLOW_C = (60, 190, 240)         # cyan glow (rgb; alpha applied per-draw)
C_GLOW_R = (255, 60, 34)          # red glow

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


# ---------------------------------------------------------------------------
# Shared drawing primitives for the new look.
# ---------------------------------------------------------------------------

def new_glow_layer(S):
    """A quarter-scale RGBA layer to draw glow shapes into (see note 5)."""
    g = Image.new("RGBA", (S // GLOW_DIV, S // GLOW_DIV), (0, 0, 0, 0))
    return g, ImageDraw.Draw(g)


def bake_glow(glow, S, radius_full):
    """Blur the quarter-scale glow layer and bring it back to full size."""
    g = glow.filter(ImageFilter.GaussianBlur(max(1.0, radius_full / GLOW_DIV)))
    return g.resize((S, S), Image.BICUBIC)


def taper_tick(d, c, r_in, r_out, deg, w_in, w_out, col):
    """A tick as a tapered quad — narrower at the hub end.

    Why not d.line(): a wide PIL pen draws square butt ends and a constant
    width, which downsamples into a shimmering rectangle. A real instrument
    tick is a machined wedge, and the taper is also what keeps the inner ends
    from crowding each other where the minors converge toward the hub.
    """
    a = math.radians(deg)
    ca, sa = math.cos(a), -math.sin(a)      # screen y is down
    px, py = -sa, ca                        # unit perpendicular
    ix, iy = c + ca * r_in,  c + sa * r_in
    ox, oy = c + ca * r_out, c + sa * r_out
    d.polygon([(ix + px * w_in,  iy + py * w_in),
               (ox + px * w_out, oy + py * w_out),
               (ox - px * w_out, oy - py * w_out),
               (ix - px * w_in,  iy - py * w_in)], fill=col)


def paint_dark_glass(img, S, Rf):
    """The near-black inset pane + its glass read (note 2).

    Order matters: the pane goes down first so it darkens the Blender face,
    then the rim shadow, then the sheen ON TOP of both so it looks like light
    on the cover glass rather than paint under it.
    """
    d = ImageDraw.Draw(img)
    c = S / 2.0

    # PANE — a near-black radial. Darker at the rim than the centre so the
    # numerals near the edge sit on the deepest part of the glass.
    steps = 160
    for i in range(steps, 0, -1):
        t = i / steps                      # 1 at the rim, ~0 at the hub
        rr = Rf * t
        # 3..11 in value: rule 7's "near-black", not the old flat charcoal.
        v = int(3 + 8 * (1.0 - t) ** 1.6)
        a = int(252 - 26 * (1.0 - t))
        d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=(v, v + 1, v + 4, a))

    # RIM SHADOW — the pane is INSET; the bezel lip throws a soft ring of
    # shade onto its outer few percent. Without this the face reads as a
    # sticker on top of the ring instead of glass down inside it.
    sh, shd = new_glow_layer(S)
    gs = S / GLOW_DIV
    for i in range(14):
        rr = (Rf * (1.005 - i * 0.006)) / GLOW_DIV
        shd.ellipse([gs / 2 - rr, gs / 2 - rr, gs / 2 + rr, gs / 2 + rr],
                    outline=(0, 0, 0, 26), width=max(1, int(Rf * 0.012 / GLOW_DIV)))
    img.alpha_composite(bake_glow(sh, S, Rf * 0.035))

    # SHEEN — one broad, weak ellipse across the upper third. Rule 5's
    # anti-glare law: wide and faint is a sheen; tight and bright is the hot
    # orb over the text that the law exists to prevent.
    sn, snd = new_glow_layer(S)
    ew, eh = Rf * 0.92 / GLOW_DIV, Rf * 0.52 / GLOW_DIV
    ecx, ecy = gs / 2, gs / 2 - Rf * 0.40 / GLOW_DIV
    snd.ellipse([ecx - ew, ecy - eh, ecx + ew, ecy + eh], fill=(150, 178, 205, 26))
    img.alpha_composite(bake_glow(sn, S, Rf * 0.10))


def paint_scale(img, S, Rf, spans, ticks, numerals, hairline=True):
    """Draw the arcs, ticks and numerals with glow underlays.

    spans:    [(f0, f1, rgb, width_frac, glow_alpha)]  arc bands along the sweep
    ticks:    [(frac, r_in, r_out, w_in, w_out, rgba, glow_rgb_or_None)]
    numerals: [(frac, r, text, font, rgba, glow_rgb_or_None)]
    """
    c = S / 2.0

    # ---- glow pass (quarter scale, blurred, composited UNDER the crisp art)
    glow, gd = new_glow_layer(S)
    gc = (S / GLOW_DIV) / 2.0
    gRf = Rf / GLOW_DIV
    for (f0, f1, rgb, wfrac, ga) in spans:
        if ga <= 0:
            continue
        ra = gRf * 0.995
        gd.arc([gc - ra, gc - ra, gc + ra, gc + ra],
               start=-ang_for(f0), end=-ang_for(f1),
               fill=(*rgb, ga), width=max(1, int(gRf * wfrac * 2.6)))
    for (frac, r_in, r_out, w_in, w_out, rgba, grgb) in ticks:
        if grgb is None:
            continue
        taper_tick(gd, gc, r_in / GLOW_DIV, r_out / GLOW_DIV, ang_for(frac),
                   w_in / GLOW_DIV * 2.0, w_out / GLOW_DIV * 2.0, (*grgb, 90))
    img.alpha_composite(bake_glow(glow, S, Rf * 0.030))

    # ---- crisp pass
    d = ImageDraw.Draw(img)

    # A faint cyan HOLO WASH under the live range (note 6): a wide, very low
    # alpha band just inboard of the ticks. It gives the sweep a direction at a
    # glance without adding anything you have to read.
    if hairline:
        wash, wd = new_glow_layer(S)
        gcw = (S / GLOW_DIV) / 2.0
        rw = Rf * 0.86 / GLOW_DIV
        wd.arc([gcw - rw, gcw - rw, gcw + rw, gcw + rw],
               start=-ang_for(0.0), end=-ang_for(1.0),
               fill=(*C_GLOW_C, 34), width=max(1, int(Rf * 0.20 / GLOW_DIV)))
        img.alpha_composite(bake_glow(wash, S, Rf * 0.075))
        # and a hairline ring inboard of the numerals
        rh = Rf * 0.545
        d.ellipse([c - rh, c - rh, c + rh, c + rh],
                  outline=(90, 176, 206, 70), width=max(1, int(S * 0.0009)))

    for (f0, f1, rgb, wfrac, _ga) in spans:
        ra = Rf * 0.995
        d.arc([c - ra, c - ra, c + ra, c + ra], start=-ang_for(f0), end=-ang_for(f1),
              fill=(*rgb, 255), width=max(1, int(Rf * wfrac)))
    for (frac, r_in, r_out, w_in, w_out, rgba, _g) in ticks:
        taper_tick(d, c, r_in, r_out, ang_for(frac), w_in, w_out, rgba)
    for (frac, r, text, fnt, rgba, _g) in numerals:
        tx, ty = polar(c, c, r, ang_for(frac))
        ctext(d, tx, ty, text, fnt, rgba)


def load_bezel():
    bez = Image.open(BEZEL).convert("RGBA")
    S = bez.size[0] * SS
    return bez, S, bez.resize((S, S), Image.LANCZOS)


def compose():
    bez, S, bezel_ss = load_bezel()
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.alpha_composite(bezel_ss)
    c = S / 2.0
    Rf = S * 0.5 * FACE_R_FRAC

    paint_dark_glass(img, S, Rf)

    # A faint RED WASH on the face behind the limiter sector (note 4) — drawn
    # before the scale so ticks and numerals sit on top of it.
    rw, rwd = new_glow_layer(S)
    gc = (S / GLOW_DIV) / 2.0
    rr = Rf * 0.78 / GLOW_DIV
    rwd.arc([gc - rr, gc - rr, gc + rr, gc + rr],
            start=-ang_for(REDLINE_RPM / RPM_MAX), end=-ang_for(1.0),
            fill=(*C_GLOW_R, 46), width=max(1, int(Rf * 0.40 / GLOW_DIV)))
    img.alpha_composite(bake_glow(rw, S, Rf * 0.09))

    r_out = Rf * 0.965
    r_maj = Rf * 0.815
    r_min = Rf * 0.882
    r_num = Rf * 0.690
    redf  = REDLINE_RPM / RPM_MAX

    spans = [
        (0.0,  redf, C_TICK[:3], 0.0165, 70),    # live range, cyan, soft glow
        (redf, 1.0,  C_RED[:3],  0.0330, 150),   # THE LIMITER: double weight, hot glow
    ]

    fnum = font(int(Rf * 0.180))
    ticks, nums = [], []
    n = int(RPM_MAX / 200)
    for i in range(n + 1):
        rpm = i * 200.0
        f = rpm / RPM_MAX
        hot = rpm >= REDLINE_RPM
        if i % 5 == 0:
            ticks.append((f, r_maj, r_out, Rf * 0.0085, Rf * 0.0135,
                          C_RED if hot else C_TICK, C_GLOW_R if hot else C_GLOW_C))
            nums.append((f, r_num, str(int(rpm / 1000)), fnum,
                         C_RED if hot else C_NUM, C_GLOW_R if hot else None))
        else:
            ticks.append((f, r_min, r_out, Rf * 0.0034, Rf * 0.0058,
                          C_RED if hot else C_TICK_D, C_GLOW_R if hot else None))
    paint_scale(img, S, Rf, spans, ticks, nums)

    # NOTHING else goes on the face. The two vertical strips between the hub and
    # the numerals are the only clear areas on a dial this dense, and the HOST
    # owns them: the gear digit above the hub, the MPH readout below it. Earlier
    # passes printed "RPM x1000" and a REDLINE badge exactly there, so the live
    # text landed on top of baked text. The red arc and the red 8 already say
    # where the limiter is; good instruments label very little.
    #
    # A boxed badge was tried here too and read as web UI, not as an instrument.
    return img.resize((bez.size[0], bez.size[1]), Image.LANCZOS)


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
# THE 35-PSI BUILD (Tim: "Boost should hit 35 PSI!!! This is a TURBO not a
# Supercharger"). The rule that survives every retune: this scale and
# TurboParams::maxPsi change TOGETHER — the original sin was 35 psi of model
# under 20 psi of dial, needle pinned off the end while the digits kept counting.
# THE THIRD SITE, by name: app/gauge_hud.h's kGaugeMinPsi / kGaugeMaxPsi /
# kGaugeHotPsi map psi onto the needle atlas and colour the live digits; they
# must equal the three numbers below. --screenshot-tunnel with X3_SHOT_GAUGES=1
# photographs the needle at 34.2 psi so the agreement stays an eyes-on fact.
BOOST_MIN_PSI = -10.0
BOOST_MAX_PSI =  40.0
BOOST_HOT_PSI =  30.0          # red band: approaching the 35-psi peak

C_VAC = (150, 162, 178, 255)   # vacuum side reads cool grey, not "active" cyan


def boost_frac(psi):
    return (psi - BOOST_MIN_PSI) / (BOOST_MAX_PSI - BOOST_MIN_PSI)


def compose_boost():
    bez, S, bezel_ss = load_bezel()
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.alpha_composite(bezel_ss)
    c = S / 2.0
    Rf = S * 0.5 * FACE_R_FRAC

    paint_dark_glass(img, S, Rf)

    z    = boost_frac(0.0)
    hotf = boost_frac(BOOST_HOT_PSI)

    # red wash behind the overboost sector, as on the tach
    rw, rwd = new_glow_layer(S)
    gc = (S / GLOW_DIV) / 2.0
    rr = Rf * 0.78 / GLOW_DIV
    rwd.arc([gc - rr, gc - rr, gc + rr, gc + rr],
            start=-ang_for(hotf), end=-ang_for(1.0),
            fill=(*C_GLOW_R, 46), width=max(1, int(Rf * 0.40 / GLOW_DIV)))
    img.alpha_composite(bake_glow(rw, S, Rf * 0.09))

    r_out = Rf * 0.965
    r_maj = Rf * 0.795
    r_min = Rf * 0.880
    r_num = Rf * 0.650

    spans = [
        (0.0,  z,    C_VAC[:3],  0.0120, 0),      # vacuum: no glow, it is not "live"
        (z,    hotf, C_TICK[:3], 0.0165, 70),
        (hotf, 1.0,  C_RED[:3],  0.0330, 150),
    ]

    # Bigger numerals than the tach's proportionally, because this dial is drawn
    # at ~0.70 of the tach's radius on screen and 0.150 put it under 12 px.
    fnum = font(int(Rf * 0.180))
    ticks, nums = [], []
    psi = BOOST_MIN_PSI
    while psi <= BOOST_MAX_PSI + 0.01:
        f = boost_frac(psi)
        # 50-psi span: numerals every 10, half-ticks every 5 — the old 5-psi
        # numerals were right for a 30-psi span and unreadable on this one.
        major = (abs(psi) % 10.0) < 0.01
        half  = (abs(psi) % 5.0) < 0.01
        hot   = psi >= BOOST_HOT_PSI
        if major:
            zero = abs(psi) < 0.01
            col = C_RED if hot else ((240, 246, 255, 255) if zero
                                     else (C_TICK if psi > 0 else C_VAC))
            grgb = C_GLOW_R if hot else (C_GLOW_C if psi >= 0 else None)
            ticks.append((f, r_maj, r_out,
                          Rf * (0.0110 if zero else 0.0085),
                          Rf * (0.0170 if zero else 0.0135), col, grgb))
            nums.append((f, r_num, "%d" % int(round(psi)), fnum,
                         C_RED if hot else (C_NUM if psi >= 0 else C_VAC),
                         C_GLOW_R if hot else None))
        elif half and psi > 0:   # half-ticks on the boost side only
            ticks.append((f, r_min, r_out, Rf * 0.0034, Rf * 0.0058,
                          C_RED if hot else C_TICK_D, C_GLOW_R if hot else None))
        psi += 1.0
    paint_scale(img, S, Rf, spans, ticks, nums)

    # This face DOES get a label. Unlike the tach, nothing else is drawn on it,
    # and a bare needle over unlabelled numbers is genuinely ambiguous — 0 to 20
    # of what? The host writes the live psi under the hub; this names the unit.
    d = ImageDraw.Draw(img)
    ctext(d, c, c - Rf * 0.40, "BOOST  psi", font(int(Rf * 0.135)), C_LABEL)

    return img.resize((bez.size[0], bez.size[1]), Image.LANCZOS)


# ---------------------------------------------------------------------------
# NOS ARC — solid luminescent curved bar (Tim: "Curving bar like NFS had 20
# years ago... not beads. solid luminescent bars"). A 32-state fill atlas:
# frame i shows the arc filled to i/31, lit portion a hot electric-blue core
# with a soft baked glow, unlit portion a dim husk. The host picks the frame
# by tank level — the needle-atlas pattern, applied to a fill.
#
# THE ATLAS CONTRACT IS FIXED (app/gauge_hud.cpp samples it): 32 frames in an
# 8-wide x 4-tall grid, so v advances 0.25 per row and u 1/8 per column. Cell
# SIZE is free (the host works in UV), the GRID is not.
# ---------------------------------------------------------------------------
NOS_FRAMES  = 32
NOS_ATLAS_N = 8            # 8x4 grid
NOS_CELL    = 256
NOS_A0, NOS_A1 = 210.0, 130.0    # degrees, left shoulder sweep (matches host)
NOS_R_FRAC  = 0.86         # arc radius as fraction of half-cell
NOS_W_FRAC  = 0.10         # bar thickness


def compose_nos_atlas():
    n_rows = NOS_FRAMES // NOS_ATLAS_N
    atlas = Image.new("RGBA", (NOS_ATLAS_N * NOS_CELL, n_rows * NOS_CELL), (0, 0, 0, 0))
    SSN = 4
    cell_ss = NOS_CELL * SSN
    c = cell_ss / 2.0
    rArc = c * NOS_R_FRAC
    half_w = c * NOS_W_FRAC * 0.5
    for fi in range(NOS_FRAMES):
        fill = fi / (NOS_FRAMES - 1)
        im = Image.new("RGBA", (cell_ss, cell_ss), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)

        def arc(dd, cc, rr, a_from, a_to, width, col):
            bb = [cc - rr, cc - rr, cc + rr, cc + rr]
            # PIL arcs go clockwise from 3 o'clock, y-down: convert.
            dd.arc(bb, start=-a_from, end=-a_to, fill=col, width=int(max(1, width)))

        # husk (whole sweep, dim) — with a hairline lighter edge so the empty
        # part of the bar reads as a machined channel and not as a smudge.
        arc(d, c, rArc, NOS_A0, NOS_A1, half_w * 2.3, (10, 14, 22, 215))
        arc(d, c, rArc, NOS_A0, NOS_A1, half_w * 2.0, (30, 40, 56, 220))
        if fill > 0.001:
            a_fill = NOS_A0 + (NOS_A1 - NOS_A0) * fill
            # GLOW, blurred rather than faked with stacked wide arcs: the old
            # three-pass widening left visible concentric steps at this size.
            gsz = cell_ss // GLOW_DIV
            gl = Image.new("RGBA", (gsz, gsz), (0, 0, 0, 0))
            gld = ImageDraw.Draw(gl)
            arc(gld, gsz / 2.0, rArc / GLOW_DIV, NOS_A0, a_fill,
                half_w * 2.4 / GLOW_DIV, (70, 175, 255, 170))
            gl = gl.filter(ImageFilter.GaussianBlur(half_w * 1.5 / GLOW_DIV))
            im.alpha_composite(gl.resize((cell_ss, cell_ss), Image.BICUBIC))
            # core: hot, solid, with a white-hot centre line
            arc(d, c, rArc, NOS_A0, a_fill, half_w * 2.0, (120, 210, 255, 255))
            arc(d, c, rArc, NOS_A0, a_fill, half_w * 1.0, (225, 248, 255, 255))
        im = im.resize((NOS_CELL, NOS_CELL), Image.LANCZOS)
        atlas.paste(im, ((fi % NOS_ATLAS_N) * NOS_CELL, (fi // NOS_ATLAS_N) * NOS_CELL))
    return atlas


def needle_atlas():
    """64 rotations in an 8x8 grid. Frame 0 = sweep start, 63 = sweep end.

    The needle gets a SOFT GLOW now (the owner's word). It is a real blurred
    underlay, not a lighter outline: a red instrument needle over near-black
    glass has almost no contrast in the unlit lower half of the sweep, and the
    old fix was a hard drop shadow, which reads as a decal lifted off the face.
    A warm glow under the blade separates it from the glass AND sells the
    needle as the lit element it is on a modern cluster.
    """
    cell = NEEDLE_PX // ATLAS_N
    cell_ss = cell * SS
    atlas = Image.new("RGBA", (NEEDLE_PX, NEEDLE_PX), (0, 0, 0, 0))
    c = cell_ss / 2.0
    L, tail = cell_ss * 0.400, cell_ss * 0.085
    wr, wt = cell_ss * 0.027, cell_ss * 0.0085

    def blade_pts(off, sw=1.0):
        return [(c - tail + off, c - wr * 0.62 * sw + off),
                (c - tail + off, c + wr * 0.62 * sw + off),
                (c + L + off,    c + wt * sw + off),
                (c + L + off,    c - wt * sw + off)]

    # ---- glow underlay (quarter scale, blurred, composited first)
    gsz = cell_ss // GLOW_DIV
    gl = Image.new("RGBA", (gsz, gsz), (0, 0, 0, 0))
    gld = ImageDraw.Draw(gl)
    gc, gL, gtail = gsz / 2.0, gsz * 0.400, gsz * 0.085
    gwr, gwt = gsz * 0.027, gsz * 0.0085
    gld.polygon([(gc - gtail, gc - gwr * 1.9), (gc - gtail, gc + gwr * 1.9),
                 (gc + gL,    gc + gwt * 3.4), (gc + gL,    gc - gwt * 3.4)],
                fill=(255, 66, 40, 150))
    gl = gl.filter(ImageFilter.GaussianBlur(gsz * 0.016))

    base = Image.new("RGBA", (cell_ss, cell_ss), (0, 0, 0, 0))
    base.alpha_composite(gl.resize((cell_ss, cell_ss), Image.BICUBIC))
    bd = ImageDraw.Draw(base)

    # a soft dark contact shadow keeps the blade readable where it crosses the
    # brighter sheen at the top of the glass
    bd.polygon(blade_pts(cell_ss * 0.010), fill=(0, 0, 0, 120))
    bd.polygon(blade_pts(0.0), fill=C_NEEDLE)
    # a lighter top edge so the blade reads as a formed part, not a flat decal
    bd.polygon([(c - tail, c - wr * 0.62), (c - tail, c - wr * 0.20),
                (c + L, c - wt * 0.25), (c + L, c - wt)], fill=(255, 156, 140, 225))
    # white-hot tip: the eye lands on the END of a needle, so that is where the
    # brightest pixel belongs.
    bd.polygon([(c + L * 0.80, c - wt * 0.72), (c + L, c - wt * 0.55),
                (c + L, c + wt * 0.55), (c + L * 0.80, c + wt * 0.72)],
               fill=(255, 214, 200, 240))

    # hub: dark boss, brushed ring, red cap — the chrome centre of the bezel
    hr = cell_ss * 0.062
    bd.ellipse([c - hr, c - hr, c + hr, c + hr], fill=(12, 13, 17, 255))
    for rr, col in ((0.92, (186, 192, 202, 255)),
                    (0.80, (92, 98, 108, 255)),
                    (0.62, (24, 26, 32, 255))):
        r = hr * rr
        bd.ellipse([c - r, c - r, c + r, c + r], fill=col)
    hr2 = hr * 0.36
    bd.ellipse([c - hr2, c - hr2, c + hr2, c + hr2], fill=C_NEEDLE)

    frames = ATLAS_N * ATLAS_N
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
    nos = compose_nos_atlas()
    p4 = os.path.join(UI_DIR, "gauge_nos.png")
    nos.save(p4)
    print("wrote", p4, nos.size,
          "(%dx%d grid, %d frames — the host's contract)"
          % (NOS_ATLAS_N, NOS_FRAMES // NOS_ATLAS_N, NOS_FRAMES))


if __name__ == "__main__":
    main()
