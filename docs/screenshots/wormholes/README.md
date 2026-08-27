# Wormhole captures — and how to reproduce them

The `refract_*` / `_sheet_refract_*` frames are the P1.5 refraction pass evidence.
Everything else predates it.

**This file exists because the previous round did not have one.** Its `+7.3% held /
+23.8% at bloom` hull-spill figures, and the camera each frame was shot from, are
recorded in no file in this tree — so they cannot be reproduced or compared
against, only quoted. Every number and every frame below names its exact command.

Space is dark: **gamma-lift before judging.** A raw PNG of a wormhole against a
starfield reads as a black rectangle with a smudge. All the sheets here are lifted
(gamma 1.4–1.5); the individual frames are raw.

---

## The A/B switch

`X3_WORMHOLE_NOREFRACT=1` falls the throat and rim back to the opaque
`drawMeshEmissive` path they shipped with. **Same binary, same camera, same frame** —
which is the only honest way to show what the refraction pass changed. Without it a
"before" is a different build and the comparison proves nothing.

Distinct from `X3_WORMHOLE_OFF=1` (seeds no wormholes at all — the control for
measuring the light spill).

## Camera geometry

The stable hole `THE GAMMA CORRIDOR` sits at `(170, 25, -55)` on axis
`(0.86, 0.06, -0.51)`, radius 30. `--shot-cam` takes `x,y,z,yaw,pitch` where
`forward = (cos(pitch)cos(yaw), sin(pitch), cos(pitch)sin(yaw))`.

The four angles are measured **off the throat axis** — 0 = dead head-on,
90 = exactly edge-on — with the camera 110 m out and aimed at the mouth:

| file | angle off-axis | `--shot-cam` |
|---|---|---|
| `refract_a00_*` | 0 (head-on) | `264.44,31.59,-111.01,2.6063,-0.0599` |
| `refract_a45_*` | 45 (three-quarter, the hero angle) | `276.46,29.66,-27.70,-2.8906,-0.0424` |
| `refract_a70_*` | 70 (**the reported "hard side-on" defect**) | `255.03,27.25,14.75,-2.4546,-0.0205` |
| `refract_a90_*` | 90 (exactly edge-on) | `226.11,25.00,39.61,-2.1061,-0.0000` |

Do **not** frame a head-on shot by walking back along the axis from the mouth: the
decor fleet occupies `x in [30,80]` and the camera lands inside a capital's hull.
(It renders as a flat grey wall. It cost a capture round to notice.)

## Reproducing the angle sweep

```sh
EXE=./build/bin/Release/X3Engine.exe
CAM="255.03,27.25,14.75,-2.4546,-0.0205"          # the 70-degree case
# AFTER (refraction):
X3_MUTE=1 X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0 \
  $EXE --world space --screenshot out/a70_ON.png --shot-cam "$CAM"
# BEFORE (opaque), same binary:
X3_WORMHOLE_NOREFRACT=1 X3_MUTE=1 X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0 \
  $EXE --world space --screenshot out/a70_OFF.png --shot-cam "$CAM"
```

`X3_WORMHOLE_OPEN=1` runs the staged opening live from Dormant; `X3_WORMHOLE_T=<sec>`
pre-rolls it at a fixed 1/165 s step, so a single-frame capture can land anywhere on
the sequence. `T=4.0` is HELD, `T=0.48` is the middle of the BLOOM flare.

## Reproducing the opening sequence

`refract_open_t*` — the same camera as `a00`, sweeping `X3_WORMHOLE_T` over
`0.05 0.25 0.48 0.62 0.85 1.15 1.60 2.05 2.60 4.00`. **A staged effect cannot be
judged from one still**; the sheet is `_sheet_refract_opening.png`.

## Reproducing the light-spill measurement

Default `--world space` headless camera (no `--shot-cam`), three captures:

```sh
X3_WORMHOLE_OFF=1                              $EXE --world space --screenshot out/spill_OFF.png
X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0           $EXE --world space --screenshot out/spill_ON.png
X3_WORMHOLE_NOREFRACT=1 X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0 \
                                               $EXE --world space --screenshot out/spill_ON_opaque.png
```

**Hull mask** (this is the part that was missing before — a spill percentage means
nothing without the mask that produced it): take Rec.709 luma of the `OFF` control,
keep `0.10 < L < 0.62` (excludes empty space and the star points, keeps the decor
fleet's lit hulls), restricted to rows `60..600` (drops the HUD furniture, which the
wormhole does not light). Then compare mean luma over that mask.

Measured on this build, 611,922 hull pixels:

| frame | hull mean | vs OFF |
|---|---|---|
| `spill_OFF` (control) | 0.19506 | — |
| `spill_ON_opaque` (30-layer-era shading) | 0.20613 | **+5.68%** |
| `spill_ON` (refraction) | 0.20716 | **+6.20%** |
| bloom flare, either path | 0.22113 | **+13.36%** (identical to 5 dp) |

**Read this as the ON-vs-opaque delta, not against the old +7.3/+23.8 figures** —
those were taken with an unrecorded mask and framing, so they are not commensurable
with these. What the table shows is what matters: dropping 30 annuli to 14 and moving
the throat to the blended path did **not** regress the hull spill (it is marginally
up), and the bloom flare is untouched to five decimal places — as expected, since it
is the point light plus the additive halo, and `collectLights()` never referenced the
layer count.

## Sync validation (`--vksync`)

Counted by grepping the run log for `SYNC-HAZARD-` **with the trailing dash** — a bare
`SYNC-HAZARD` also matches the engine's own status banner.

```sh
$EXE --world space --screenshot out/x.png --validate --vksync 2> run.log
grep -c "SYNC-HAZARD-" run.log
```

| configuration | WRITE_AFTER_READ |
|---|---|
| `X3_WORMHOLE_OFF=1` (**no wormholes at all**) | **15** |
| wormholes, opaque path (`X3_WORMHOLE_NOREFRACT=1`) | 0 |
| wormholes, refraction path | 0 |

**The 15 is the real number and it is a `--world space` defect that has nothing to do
with wormholes.** It reproduces with the field absent entirely. It is a cross-frame
WAR: `vkCmdPipelineBarrier2` performs a layout transition that writes an image the
*other* frame-in-flight's `vkCmdBeginRendering` read, with the transition's srcStageMask
not covering those stages — the validator's own note is *"an execution dependency is
sufficient to prevent this hazard."* It is the same class already fixed for three
imported resources in `vk_graph.cpp` (`frame.color`, `scene.copy`, `gi.prevDepth`),
where an import's entry `ResourceState` must name what the PREVIOUS frame did to it.

The refraction pass neither causes it nor fixes it — it reads 0 exactly as the opaque
path does. **A lower number here is masking, not an improvement**, and it should not be
quoted as one. The fix belongs to the render-graph import scopes (`fix/vk-sync-hazards`).
