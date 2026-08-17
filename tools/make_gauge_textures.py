#!/usr/bin/env python3
"""Generate the driving HUD gauge artwork: a dial face + a rotated-needle atlas.

WHY THIS EXISTS. The first cut of this gauge drew a "dial" out of 121 tiny
axis-aligned quads because the brief said "rectangles only". That was a
misreading of the constraint: the HUD has drawHudImage(), a TEXTURED rectangle
with UV sub-rects, so the right move is to put real, anti-aliased artwork IN the
rectangle instead of approximating curves with it. Owner's verdict on the quad
version was "slop in Carbon esque shape", and he was right.

Everything is drawn at SS x supersample and downsampled with LANCZOS, which is
what gives clean arcs and crisp numerals. No engine or shader change is needed.

Outputs (RGBA PNG, straight alpha):
  assets/ui/gauge_dial.png     1024^2   the static face: bezel, ticks, numerals,
                                        redline band, vignette
  assets/ui/gauge_gate.png     1024x512 the H-pattern shift gate, thin white
                                        strokes, numerals clear of the rails
  assets/ui/gauge_needle.png   2048^2   8x8 atlas = 64 frames of the needle,
                                        frame 0 at the sweep start, frame 63 at
                                        the end. Pick by rpm and hand the frame's
                                        UV sub-rect to drawHudImage.

Run:  python tools/make_gauge_textures.py
"""

import math
import os
from PIL import Image, ImageDraw, ImageFont

# ---- geometry contract (host_tunnel.cpp must agree with these) --------------
SWEEP_START_DEG = 210.0     # 0 rpm, down-left
SWEEP_TOTAL_DEG = 240.0     # clockwise to -30 deg, down-right
RPM_MAX         = 8000.0    # dial prints 0..8; engine redline is 7500
REDLINE_RPM     = 7000.0    # where the hot band starts

DIAL_PX   = 1024
NEEDLE_PX = 2048
ATLAS_N   = 8               # 8x8 = 64 frames
SS        = 4               # supersample factor

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui")

# Restrained, era-correct: cool white/cyan instrument on near-black, hot redline.
C_FACE_IN   = (16, 19, 24, 235)
C_FACE_OUT  = (7, 9, 12, 240)
C_BEZEL_HI  = (104, 114, 128, 255)
C_BEZEL_LO  = (22, 26, 33, 255)
C_TICK_MIN  = (150, 168, 186, 190)
C_TICK_MAJ  = (232, 242, 252, 255)
C_NUM       = (226, 238, 250, 255)
C_RED       = (232, 62, 40, 255)
C_RED_NUM   = (255, 132, 108, 255)
C_NEEDLE    = (255, 168, 42, 255)
C_NEEDLE_T  = (255, 96, 30, 255)     # tip warms


def ang_for(frac):
    """frac 0..1 along the sweep -> screen angle in degrees (y-down handled by caller)."""
    return SWEEP_START_DEG - SWEEP_TOTAL_DEG * frac


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + math.cos(a) * r, cy - math.sin(a) * r)


def load_font(px):
    for name in ("segoeuib.ttf", "arialbd.ttf", "seguisb.ttf", "arial.ttf"):
        p = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except Exception:
                pass
    return ImageFont.load_default()


def make_dial():
    S = DIAL_PX * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = S / 2.0
    R = S * 0.5 - 2 * SS

    # --- bezel: concentric rings, light from upper-left, so it reads as metal ---
    for i in range(int(0.045 * S)):
        t = i / (0.045 * S)
        # top-left lit, bottom-right shadowed, eased
        mix = 0.5 + 0.5 * math.cos(math.pi * t)
        col = tuple(int(C_BEZEL_LO[k] + (C_BEZEL_HI[k] - C_BEZEL_LO[k]) * mix) for k in range(3))
        d.ellipse([i, i, S - i, S - i], outline=col + (255,), width=SS)

    # --- face: radial gradient, darker at the rim (vignette) ---
    rf = R * 0.925
    steps = 190
    for i in range(steps, 0, -1):
        t = i / steps
        col = tuple(int(C_FACE_IN[k] + (C_FACE_OUT[k] - C_FACE_IN[k]) * t) for k in range(4))
        rr = rf * t
        d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=col)

    r_out = rf * 0.980
    r_maj = rf * 0.855
    r_min = rf * 0.900
    r_num = rf * 0.745

    # --- redline: ONE solid saturated band filling the TICK LANE from 7000 to
    # the end of the scale. Drawn BEFORE the ticks so they overprint it and it
    # reads as part of the scale. An earlier version used wide translucent glow
    # passes, which over a near-black face just came out washed pink and sat
    # outboard of the ticks like a sticker.
    f0 = REDLINE_RPM / RPM_MAX
    a0, a1 = ang_for(f0), ang_for(1.0)
    lane_mid = (r_maj + r_out) * 0.5
    lane_w   = (r_out - r_maj)
    d.arc([c - lane_mid, c - lane_mid, c + lane_mid, c + lane_mid],
          start=-a0, end=-a1, fill=(196, 34, 22, 255), width=int(lane_w))
    # a brighter inner lip so the band has depth rather than reading flat
    lip = lane_mid - lane_w * 0.30
    d.arc([c - lip, c - lip, c + lip, c + lip],
          start=-a0, end=-a1, fill=(246, 84, 58, 255), width=int(lane_w * 0.34))

    # --- outer sweep rail: cyan through the usable range, hot into the redline.
    # This is the signature of the reference art Tim supplied and it is what
    # makes the dial read as a modern instrument rather than a clock face.
    rail_r = r_out + (rf - r_out) * 0.52
    segs = 180
    for i in range(segs):
        t0 = i / segs
        t1 = (i + 1) / segs
        aa0, aa1 = ang_for(t0), ang_for(t1)
        rpm_here = t0 * RPM_MAX
        if rpm_here >= REDLINE_RPM:
            col = (244, 52, 30, 255)
        else:
            u = t0 / (REDLINE_RPM / RPM_MAX)
            col = (int(40 + 200 * u), int(200 - 60 * u), int(240 - 150 * u), 255)
        d.arc([c - rail_r, c - rail_r, c + rail_r, c + rail_r],
              start=-aa0 - 0.6, end=-aa1, fill=col, width=int(7 * SS))

    # --- ticks ---
    font_num = load_font(int(rf * 0.150))
    total_minor = int(RPM_MAX / 250)
    for i in range(total_minor + 1):
        rpm = i * 250.0
        frac = rpm / RPM_MAX
        a = ang_for(frac)
        hot = rpm >= REDLINE_RPM
        if i % 4 == 0:                                   # major, every 1000
            p0 = polar(c, c, r_maj, a)
            p1 = polar(c, c, r_out, a)
            d.line([p0, p1], fill=((255,238,232,255) if hot else C_TICK_MAJ), width=int(5 * SS))
            n = int(rpm / 1000)
            tx, ty = polar(c, c, r_num, a)
            bb = d.textbbox((0, 0), str(n), font=font_num)
            d.text((tx - (bb[2] - bb[0]) / 2, ty - (bb[3] - bb[1]) / 2 - bb[1]),
                   str(n), font=font_num, fill=(C_RED_NUM if hot else C_NUM))
        elif i % 2 == 0:                                 # mid, every 500
            p0 = polar(c, c, r_min, a)
            p1 = polar(c, c, r_out, a)
            d.line([p0, p1], fill=((255,226,218,220) if hot else C_TICK_MIN), width=int(2.6 * SS))
        else:                                            # fine, every 250
            p0 = polar(c, c, r_min * 1.045, a)
            p1 = polar(c, c, r_out, a)
            d.line([p0, p1], fill=((255,226,218,150) if hot else C_TICK_MIN[:3] + (120,)),
                   width=int(1.6 * SS))

    # --- inner hairline ring: gives the face a lip and hides tick roots ---
    rr = r_num * 0.760
    d.ellipse([c - rr, c - rr, c + rr, c + rr], outline=(90, 104, 120, 120), width=int(1.6 * SS))

    img = img.resize((DIAL_PX, DIAL_PX), Image.LANCZOS)
    return img


def make_gate():
    """The H-pattern shift gate. Tim, 2026-08-15: "Make the shifter look nicer..
    dont run the shift pattern INTO the numbers.. make them White, stylized..
    thinner, and under the tach."

    So: thin white rails, and the numerals sit OUTSIDE the gate — above the top
    rail and below the bottom one — never crossing a stroke. Sized wide-and-short
    to tuck under a round dial.
    """
    W, H = 1024 * SS, 512 * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cols = 4                       # 1/2, 3/4, 5/6, R
    pad_x = W * 0.11
    span  = W - 2 * pad_x
    step  = span / (cols - 1)
    y_top, y_bot = H * 0.40, H * 0.72
    y_mid = (y_top + y_bot) * 0.5
    lw    = max(1, int(2.6 * SS))  # THIN

    rail = (236, 244, 255, 210)
    # center rail spans the three gear columns only; R hangs off the end
    d.line([(pad_x, y_mid), (pad_x + step * 3, y_mid)], fill=rail, width=lw)
    for i in range(cols):
        x = pad_x + step * i
        top = y_top if i < 3 else y_mid      # R has no upper gate
        d.line([(x, top), (x, y_bot)], fill=rail, width=lw)

    fnum = load_font(int(H * 0.20))
    def label(txt, x, y, anchor_above):
        bb = d.textbbox((0, 0), txt, font=fnum)
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        yy = (y - h * 1.65) if anchor_above else (y + h * 0.42)
        d.text((x - w / 2 - bb[0], yy - bb[1]), txt, font=fnum, fill=(255, 255, 255, 240))

    for i, (up, dn) in enumerate((("1", "2"), ("3", "4"), ("5", "6"))):
        x = pad_x + step * i
        label(up, x, y_top, True)      # clear ABOVE the rail
        label(dn, x, y_bot, False)     # clear BELOW it
    label("R", pad_x + step * 3, y_bot, False)

    return img.resize((1024, 512), Image.LANCZOS)


def make_needle_atlas():
    cell_ss = (NEEDLE_PX // ATLAS_N) * SS
    atlas = Image.new("RGBA", (NEEDLE_PX, NEEDLE_PX), (0, 0, 0, 0))
    frames = ATLAS_N * ATLAS_N

    # Draw the needle ONCE pointing right (0 deg), then rotate per frame. Rotating
    # a supersampled sprite is what keeps the edges clean at every angle.
    base = Image.new("RGBA", (cell_ss, cell_ss), (0, 0, 0, 0))
    bd = ImageDraw.Draw(base)
    c = cell_ss / 2.0
    L = cell_ss * 0.435          # tip radius
    tail = cell_ss * 0.085       # counterweight behind the hub
    w_root, w_tip = cell_ss * 0.019, cell_ss * 0.006

    # tapered blade as a polygon, tip warmed by overdrawing the last third
    bd.polygon([(c - tail, c - w_root * 0.7), (c - tail, c + w_root * 0.7),
                (c + L, c + w_tip), (c + L, c - w_tip)], fill=C_NEEDLE)
    bd.polygon([(c + L * 0.62, c - w_tip * 1.8), (c + L * 0.62, c + w_tip * 1.8),
                (c + L, c + w_tip), (c + L, c - w_tip)], fill=C_NEEDLE_T)
    # hub
    hr = cell_ss * 0.052
    bd.ellipse([c - hr, c - hr, c + hr, c + hr], fill=(36, 40, 48, 255))
    hr2 = hr * 0.52
    bd.ellipse([c - hr2, c - hr2, c + hr2, c + hr2], fill=(255, 150, 40, 255))

    cell = NEEDLE_PX // ATLAS_N
    for i in range(frames):
        frac = i / (frames - 1)
        rot = base.rotate(ang_for(frac), resample=Image.BICUBIC, center=(c, c))
        rot = rot.resize((cell, cell), Image.LANCZOS)
        atlas.paste(rot, ((i % ATLAS_N) * cell, (i // ATLAS_N) * cell))
    return atlas


def main():
    """THIS SCRIPT OWNS THE GATE, AND ONLY THE GATE.

    NO_SLOP rule 4, found while rebuilding the dial art: make_dial() and
    make_needle_atlas() below ALSO write gauge_dial.png and gauge_needle.png —
    and they draw a DIFFERENT SWEEP from the live composer
    (210 deg / 240 deg span here, 216 / 252 in tools/compose_gauge_dial.py).
    Two owners of one file, exactly the wx_snow_in defect: whichever script ran
    last won, and running this one to regenerate the gate would silently swap
    the shipped tach for an older face whose needle atlas no longer lines up
    with its own scale. Nobody would see it until they looked at the gauge.

    The pipeline is, and is only:
        1. tools/render_gauge_bezel.py    -> gauge_bezel.png   (Blender, the metal)
        2. tools/compose_gauge_dial.py    -> gauge_dial.png, gauge_boost.png,
                                             gauge_needle.png, gauge_nos.png
        3. THIS SCRIPT                    -> gauge_gate.png    (the shift gate)

    make_dial() / make_needle_atlas() are KEPT (they are the reference for the
    original vector treatment and are still readable prior art) but they are no
    longer wired to an output. If you ever want them back, they have to agree
    with compose_gauge_dial.py's SWEEP_START_DEG / SWEEP_TOTAL_DEG first.
    """
    os.makedirs(OUT_DIR, exist_ok=True)
    gate = make_gate()
    pg = os.path.normpath(os.path.join(OUT_DIR, "gauge_gate.png"))
    gate.save(pg)
    print("wrote", pg, gate.size)
    print("(dial + needle are compose_gauge_dial.py's — this script no longer "
          "writes them; see main()'s docstring for the receipt)")


if __name__ == "__main__":
    main()
