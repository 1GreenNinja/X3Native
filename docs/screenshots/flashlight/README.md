# The flashlight — evidence (fix/flashlight-feel, 2026-08-18)

Tim, live:

> "the Flashlight defaults to ON.. and it now projects a PURE BRIGHT CIRCLE with
> sharp edges.. not like a game flashlight at all.. I would like it OFF by default"

All frames: 720p headless `--world canonlevel`, torch forced on with
`--flashlight-on`. `--set r_torchlegacy 1` rebuilds the OLD torch exactly, so
every BEFORE below comes out of the SAME binary as its AFTER — one cvar apart.

| frame | what |
|---|---|
| `TORCHOFF_cell.png`   | the room with no torch at all — the baseline |
| `BEFORE_cell.png`     | `r_torchlegacy 1` — the old omni pair |
| `AFTER_cell.png`      | the spot-cone torch |
| `BEFORE_cellhall.png` | old torch, E cell hall |
| `AFTER_cellhall.png`  | new torch, same camera |

Cameras: cell `2,1.6,42.4,-1.5708,-0.06`; hall `38.5,1.7,38,-1.5708,-0.12`.

## What made the circle sharp

The engine had **no cone**. `x3::rhi::PointLight` carried a position, a range and
a colour and nothing else, so nothing in this codebase could describe a beam.
The "flashlight" was two OMNI BULBS hanging 2 m and 0.3 m in front of the
player's face, and the disc on the wall was never a beam — it was inverse-square
times N·L on whatever surface the front bulb happened to be near.

That geometry is what produced the complaint:

* **Flat.** Face a wall and the 20 m bulb sits ~0.5 m off it, where
  `3.30 / (d² + 1)` is ~2.6 — well into the tonemapper's shoulder. Everything
  across the near surface lands on the same flat part of the curve, so the
  gradient a real beam has is compressed out of existence.
* **Edged.** Off-axis brightness fell only through N·L and distance, both of
  which change slowly across a near wall and then quickly once the surface turns
  away. The image gets a large uniform bright region that stops. There is no
  penumbra anywhere in it, because there is no cone to have one.

Measured on a horizontal scanline straight through the middle of the pool
(y = 360, x = 300 → 1000 in steps of 100), which is the whole argument in one row
of numbers:

```
BEFORE   58   36   43   55   19   17   16   19     <- flat, then a step
AFTER    59   60  109  219  215   83   33   12     <- hotspot -> corona -> dark
```

BEFORE has no structure to read. AFTER is a bell.

## The fix

1. **`x3::rhi::PointLight` grew a spot cone** (`dir`, `coneInnerCos`,
   `coneOuterCos`) — `engine/rhi/IRenderDevice.h`. The GPU row went from two
   vec4s to three (`dirCone`); every shader that declares the light struct was
   updated in lockstep. `dir` all-zero means OMNI and the shader's cone factor is
   a literal `1.0`, so **every pre-existing fixture in the game is untouched.**
2. **`spotCone()`** in `shaders/inc/mesh_lighting.glsl` (mirrored in
   `volumetric.frag`, which carries its own copy of `pointAtten`, and inlined in
   `ddgi_rays.comp`): smoothstep between the inner and outer cosine, then
   **squared**, so the spill leaves with zero slope instead of meeting the dark
   at a crease. Same family as the cos³ taper `fix/outdoor-polish` gave the
   street lamps at `5c14b9b2`.
3. **The torch is a reflector at the EYE**, not a bulb downrange: a tight warm
   KEY (3.70, 13°/28°, 28 m) plus a wide dim SPILL (0.95, 18°/50°, 12 m). Their
   sum is hotspot → corona → darkness. Warmer too — 1.00/0.88/0.72 against the
   old 1.00/0.94/0.83.
4. Moving the source to the eye is also what buys the **throw**: the old bulb sat
   2 m downrange so the near half of the beam was crushed against the falloff
   curve. A wall 2.5 m ahead now takes `3.70/(2.35² + 1) = 0.57` instead of
   `3.30/(0.5² + 1) = 2.6`, and 10 m downrange reads ~0.036 instead of ~0.01. A
   bigger intensity number, a **darker** near field, ~3x the usable throw.

`docs/screenshots/flashlight/AFTER_cell.png` is the result: a soft hotspot, a
corona, and a spill edge you cannot point at.

## Default OFF

* `r_flashlight` default `1` → `0`.
* `bool flashlight = !hc.flashlightOff` → `hc.flashlightOn && !hc.flashlightOff`.
* The cvar used to be read as a per-frame clamp (`if (r_flashlight == 0)
  flashlight = false;`). Harmless while the default was 1 — with the default 0 it
  would have pinned the torch off forever and **broken the L key**. It is now
  EDGE-triggered: the torch follows any *change* to the cvar, so `r_flashlight 0`
  works exactly as the lighting audits use it and `r_flashlight 1` turns it on.
* `--flashlight-on` added; `--flashlight-off` still parses and still works.
* No test or gate depends on the torch state — `grep -rn flashlight app/*test*`
  is empty, and every measured number in `docs/LIGHTING_AUDIT_FACILITY.md`,
  `cell_dressing.cpp`, `room_dressing.cpp` and `level_loader.cpp` was already
  taken **flashlight OFF**. Defaulting off moves the game TOWARD those baselines.

## The capture path never had a torch

`--screenshot` fed the settle loop its own light list and never included the
flashlight, so `--screenshot --flashlight-off` was a no-op and the torch could
regress to a hard white disc without a single capture catching it. The settle
loop now calls the same `buildTorchLights()` the live loop does. It is opt-in
(`--flashlight-on` / `r_flashlight 1`) and the torch defaults off, so every
pre-existing capture recipe still photographs the room's own practicals, byte for
byte — which is exactly why the frames above could be made at all.
