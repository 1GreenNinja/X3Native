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

    ctext(d, c, c - Rf * 0.42, "RPM x1000", font(int(Rf * 0.095)), C_LABEL)

    # redline badge, lower face
    bw, bh = Rf * 0.92, Rf * 0.245
    bx, by = c - bw / 2, c + Rf * 0.50
    d.rounded_rectangle([bx, by, bx + bw, by + bh], radius=int(9 * SS),
                        outline=(40, 110, 130, 220), width=int(2 * SS))
    ctext(d, c, by + bh * 0.30, "%d RPM" % int(REDLINE_RPM), font(int(Rf * 0.088)), C_LABEL)
    ctext(d, c, by + bh * 0.72, "REDLINE", font(int(Rf * 0.095)), C_RED)

    return img.resize((bez.size[0] // SS, bez.size[1] // SS), Image.LANCZOS)


def needle_atlas():
    cell_ss = (NEEDLE_PX // ATLAS_N) * SS
    atlas = Image.new("RGBA", (NEEDLE_PX, NEEDLE_PX), (0, 0, 0, 0))
    frames = ATLAS_N * ATLAS_N
    base = Image.new("RGBA", (cell_ss, cell_ss), (0, 0, 0, 0))
    bd = ImageDraw.Draw(base)
    c = cell_ss / 2.0
    L, tail = cell_ss * 0.400, cell_ss * 0.070
    wr, wt = cell_ss * 0.016, cell_ss * 0.007
    bd.polygon([(c - tail, c - wr * 0.65), (c - tail, c + wr * 0.65),
                (c + L, c + wt), (c + L, c - wt)], fill=C_NEEDLE)
    hr = cell_ss * 0.050
    bd.ellipse([c - hr, c - hr, c + hr, c + hr], fill=(20, 22, 28, 255))
    hr2 = hr * 0.52
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
    na = needle_atlas()
    p2 = os.path.join(UI_DIR, "gauge_needle.png")
    na.save(p2)
    print("wrote", p2, na.size)


if __name__ == "__main__":
    main()
