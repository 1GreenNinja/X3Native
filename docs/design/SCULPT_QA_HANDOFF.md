# Vulva Sculpt — Vision-QA Handoff (for a vision-capable Claude)

Written 2026-08-16 by the DeepSeek session. **You (Claude) are here because you have
vision and I don't.** Goal: produce a realistic female **vulva + vaginal introitus
(opening)** as a shape key on the MPFB2 nude female body, for the X3Native explicit-
penetration asset pipeline (see `WARD_SCENE_REGISTER.md`).

## Current state

- MPFB2 (Blender 4.5 add-on) installed; `create_human` generates a nude female body
  (~19k verts) with shape keys.
- A procedural vulva is applied as a **`FemaleGenitals` shape key** to the crotch
  region, exported to `assets\makehuman\female_vulva.glb`.
- **Blocked:** 3 blind parameter iterations, and Tim reports it still **reads as a
  phallus** in both renders. I cannot see the images, so I'm stuck steering blind.

## Your job

1. **Read these two renders** and say precisely what's wrong:
   - `D:\GameDev\X3Native\docs\screenshots\sculpt\vulva_front.png`
   - `D:\GameDev\X3Native\docs\screenshots\sculpt\vulva_angle.png`
2. **Tune the parameters** in `D:\GameDev\X3Native\tools\sculpt_vulva.py`, re-run, and
   re-render until it reads as a vulva.

## The geometry (what the code currently does)

Coordinate convention (confirmed): the body stands Z-up, feet at Z=0, head Z≈1.61.
**Front of the body = −Y** (verified via the `nippleTip` vertex group at y≈−0.16).
So: **recess INTO the body = +Y** (positive dy); **proud OUT toward the front = −Y**
(negative dy).

The shape key displaces crotch vertices (Z 0.70–0.94, |X|≤0.075, Y≤−0.10) with this
field (in `sculpt_vulva.py`):

- **Central cleft** (pudendal groove): gaussian recess at X=0, depth `CLEFT_A`, widths
  `CLEFT_SX`/`CLEFT_SZ`, pushing **+Y (inward)**.
- **Introitus** (vaginal opening): a recessed opening at the lower end of the cleft,
  `OPEN_X` wide, `OPEN_A` deep, **+Y (inward)**.
- **Labia majora**: two subtle proud ridges at X=±`LABIA_W`, `LABIA_A` deep, **−Y
  (outward)**.
- **Mons pubis**: a subtle proud mound at the top, **−Y (outward)**.

Current (v3) values:
```
CZ=0.84  CLEFT_SX=0.011 CLEFT_SZ=0.036 CLEFT_A=0.013
OPEN_X=0.009 OPEN_A=0.008 OPEN_Z0=-0.026 OPEN_Z1=-0.006
LABIA_W=0.018 LABIA_SX=0.005 LABIA_A=0.004  MONS_Z=0.018 MONS_A=0.003
```

**History:** v1 made the labia *bulge* 7mm outward → clearly phallic. v2 inverted to a
recessed cleft + shallowed labia. v3 (above) deepened/widened the cleft and raised the
labia on the advice of a pixel-relief reconstruction — which Tim says STILL reads as a
phallus. So the relief numbers alone are misleading you: trust your eyes, not the math.

## How to re-run (headless Blender, this box)

Blender 4.5 is the Microsoft Store package. `blender.exe` is ACL-denied; use the
launcher alias, which **detaches** (no stdout) — the Python scripts report to a
sidecar log you poll:

```
LAUNCHER="/c/Users/Tim/AppData/Local/Microsoft/WindowsApps/blender-launcher.exe"
"$LAUNCHER" --background --python "D:/GameDev/X3Native/tools/sculpt_vulva.py" -- /tmp/sculpt_vulva.log   # re-sculpt + export female_vulva.glb
# poll for /tmp/sculpt_vulva.log, then:
"$LAUNCHER" --background --python "D:/GameDev/X3Native/tools/sculpt_render.py" -- /tmp/sculpt_render.log # re-render PNGs
```

- Render engine in `sculpt_render.py` is **`BLENDER_WORKBENCH`** (`BLENDER_EEVEE` is
  invalid in 4.5 — it's `BLENDER_EEVEE_NEXT` now).
- After the sculpt script exports `female_vulva.glb`, the render script imports it and
  sets the `FemaleGenitals` shape key to 1.0 before rendering (the GLB stores it as a
  morph target at 0).

## Success criteria

The front render should read unmistakably as a vulva: a **vertical recessed groove**
running down the center of the crotch, framed by two **soft fleshy labia** that sit
nearly flush (NOT proud cylinders), with the **introitus** a modest recessed opening
at the lower end. No single protruding feature that reads phallic from the 3/4 angle.
