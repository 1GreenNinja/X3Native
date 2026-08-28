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

---

# The EVENT HORIZON pass — `horizon_*` / `_sheet_horizon_*`

Everything above is the P1.5 refraction pass. This section is the lane that
answered its one honest gap: *"at exactly 90 degrees the wormhole is essentially
invisible (light spill only)."*

## What changed, and why the fix is geometric rather than a tuning curve

The refraction lane's `facingFade` is why 90 degrees was empty, and it was the
right call for what it had: a stack of parallel discs seen edge-on is a slinky,
and fading it out beat showing it. But a real wormhole **has no edge-on view**.
It is a spherical event horizon, and a sphere's silhouette is a circle of the
same radius from every direction.

The geometry is a **billboarded impostor, which is not an approximation**: a
sphere of radius R projects to a disc of radius R for every camera, so a
camera-facing disc *is* the silhouette, exactly, for one draw. Two elements ride
that basis:

* **the horizon interior** — a radial disc (an annulus with inner radius 0, so
  its `v` really is the radius the membrane shader needs), frosted hard so the
  blurred starfield behind it reads near-black. Its alpha ramps on
  `1 - facingFade`, exactly complementary to the throat, so the two can never
  both fall away.
* **the Einstein ring / lensing halo** — one annulus, `0.86..1.40` of the mouth
  radius, at maximum lens weight on a new **1/theta deflection profile**
  (`GlassMaterial::horizon`). Ring and halo are one object because they are one
  physical thing. **It takes no facing fade at all** — that is the whole point.

The throat, the staged 6-phase opening and the transit are untouched.

## The A/B switch for THIS lane

`X3_WORMHOLE_NOHORIZON=1` disables **only** the impostor and the ring, leaving
the membrane throat exactly as `feat/wormhole-refraction` shipped it.
`X3_WORMHOLE_NOREFRACT=1` still falls all the way back to the opaque path, which
makes it the wrong instrument here — a pair taken with it shows two lanes at once
and cannot separate them. Every `horizon_*` before/after uses `NOHORIZON`.

## The angle sweep — `_sheet_horizon_sweep.png`

Top row ON, bottom row OFF, five angles measured **off the throat axis**. The
first four cameras reproduce the `refract_a*` cameras above to within 0.2
degrees, so the two sweeps are directly comparable.

| file | angle | distance | `--shot-cam` |
|---|---|---|---|
| `horizon_a000_*` | 0 (head-on) | 110 m | `264.44,31.59,-111.01,2.6063,-0.0599` |
| `horizon_a045_*` | 45 (hero three-quarter) | 110 m | `276.26,29.66,-27.58,-2.8891,-0.0424` |
| `horizon_a070_*` | 70 (the hard side-on) | 110 m | `254.76,27.25,14.91,-2.4519,-0.0205` |
| `horizon_a090_*` | 90 (exactly edge-on) | 110 m | `225.83,25.00,39.78,-2.1031,0.0000` |
| `horizon_a180_*` | 180 (from behind) | **95 m** | `88.43,19.31,-6.63,-0.5353,0.0599` |

**The 180-degree camera is at 95 m, not 110.** At 110 m it lands at x = 75,
inside the decor fleet's `x in [30,80]` — the trap the section above warns about.
95 m puts it at x = 88 and clear. The frame still has a fleet hull crossing its
right third; that hull is real geometry between the camera and the wormhole, not
an artifact.

**The read.** Bottom row: 70 degrees is a faint violet ellipse outline and 90 is
*empty* — the reported defect, reproduced exactly. Top row: 70 and 90 are the
same shot as each other, a dark churning hole ringed by a clean Einstein ring,
and 0 / 45 / 180 still carry the full throat with the ring around it.

## Two tuning rounds it took, both worth recording

1. **The ring shipped as a blown-white torus.** Outer sigma 0.20 on a band
   already 0.76 mouth-radii wide, at `kHaloGlowScale` 0.90, saturated end to end:
   the 90-degree frame was a fat glowing donut with a blue pill in it. This is
   the flat-plate failure of `_BEFORE_emissive_wash.png` arriving through the
   *base-colour* term instead of the emissive one — the emissive floors were tiny
   the whole time and did not save it. Fixed by narrowing the bake (0.055 outer
   sigma) and dropping the drive to 0.26.
2. **The lens was a smear, not a lens.** The horizon profile peaked at 3.2x on
   top of a master refraction already 2.6x the throat's, which put ~10% of the
   *screen* of displacement into the halo — the whole band sampled one distant
   patch of sky and read as grey haze. And `kHaloShimmer` 0.30 was averaging the
   starfield into a uniform glow rather than shimmering it: turbulence needs
   something continuous to distort, and a starfield is points. Profile normalised
   to peak 1, magnitude moved into `kHaloRefract` (0.075), shimmer down to 0.08.

Two knock-on fixes the captures forced:

* **`facingFade`'s `kLo` moved 0.08 -> 0.28.** The horizon interior gives the
  residual throat a dark backdrop, and rings that used to vanish into the
  starfield at 70 degrees came back as countable hoops. The throat no longer has
  to survive to 70, so it hands over earlier — 45 degrees is untouched (still
  exactly 1.00), 60 is 0.51, 70 is 0.06.
* **The mouth rim's grazing floor went 0.45 -> 0.** Its only justification was
  that deleting it deleted a still-lit wormhole; the Einstein ring answers that
  as a *circle*, where the rim edge-on was a hairline violet ellipse drawn across
  the sphere.

## The staged opening still runs — `_sheet_horizon_opening.png`

Same `X3_WORMHOLE_T` sweep and the same `a000` camera as
`_sheet_refract_opening.png`. Spark -> Bloom flare -> Unfurl -> Held all survive,
and the Einstein ring **scales with the aperture through the unfurl** rather than
popping in at Held.

## Stability at 90 degrees — `_sheet_horizon_stability90.png`

The angle where the throat that used to carry this signal is no longer drawn.
Stable holds steady blue across T = 4.0 / 4.5 / 5.0; the unstable hole swings the
*whole object*, ring included, to violet-magenta at T = 4.5 and back. Cameras:
stable `225.83,25.00,39.78,-2.1031,0.0000`, unstable (THE DERELICT APERTURE,
90 m) `159.53,25.08,196.04,-0.9862,-0.9104`.

## Light spill — same mask, same command

Identical procedure to the section above (Rec.709 luma of the OFF control,
`0.10 < L < 0.62`, rows 60..600). 613,148 hull pixels on this build.

| frame | hull mean | vs OFF |
|---|---|---|
| `spill_OFF` (control) | 0.19502 | — |
| opaque (`X3_WORMHOLE_NOREFRACT=1`) | 0.20607 | +5.66% |
| refraction lane (`X3_WORMHOLE_NOHORIZON=1`) | 0.20710 | +6.19% |
| **horizon (this lane)** | **0.20948** | **+7.41%** |

The first two rows reproduce the +5.68% / +6.20% above to two decimal places on a
mask recomputed from scratch, which is the point of having recorded the mask. The
horizon adds 1.2 points on top: the lensed halo puts light over hull pixels near
the wormhole that were previously bare space. **No regression.**

## Cost

| | |
|---|---|
| added draws | **+2 per live wormhole** (16 -> 18): the interior disc and the halo. Both take the membrane branch, which returns before the shadow taps, the clustered light loop and the IBL reflection — the cheapest fragment in the glass pass. |
| added GPU allocations | **+3** (2 meshes, 1 texture), all destroyed in `WormholeField::shutdown`. `allocationCount=0` holds in every smoketest. |
| added shaded pixels | measured by differencing ON vs `NOHORIZON` per angle: **13.0% - 16.4%** of a 1280x720 frame with a 30 m wormhole at 110 m (13.0% at 45/180, 15.2% head-on, 16.4% at 70, 16.3% at 90). |
| `ObjectData` stride | **unchanged.** `GlassMaterial::horizon` rides `terrain-pack1` **byte 3**, the one byte still free in the lane the refraction pass opened. |
| wall clock | 995 ms (`NOHORIZON`) vs 1011 ms (ON), 4 headless boot+capture runs each. **Boot-dominated - this is a stall check, not a frame time.** This tree has no headless GPU-timer harness for `--world space` (`r_passtimers` is console-only and `--bench` needs a window and its own cube field), so no per-frame millisecond figure is quoted here rather than an invented one. |

## Sync validation — the number did not move, and 0 is still masking

Same command and same grep (`SYNC-HAZARD-`, trailing dash) as the section above.

| configuration | WRITE_AFTER_READ |
|---|---|
| `X3_WORMHOLE_OFF=1` (**no wormholes at all**) | **15** |
| wormholes, opaque (`X3_WORMHOLE_NOREFRACT=1`) | 0 |
| wormholes, refraction only (`X3_WORMHOLE_NOHORIZON=1`) | 0 |
| wormholes, **horizon (this lane)** | 0 |

**Unchanged from the refraction lane in every configuration.** The 15 is the real
number, it is a `--world space` defect, and it reproduces with the field absent
entirely. This lane neither causes it nor fixes it; **the 0 is masking and must
not be quoted as an improvement.** The fix still belongs to the render-graph
import entry scopes in `vk_graph.cpp`, and no barrier was added here.

The Debug build reports the same 15 with layers verifiably on
(`[rhi] VALIDATION: layers=ON sync-validation=ON`), which is also the proof the
layer stack is live rather than silently absent — a dead layer reports nothing.
The `VUID-` count is **0** in every Debug smoketest (default / canonlevel / space
/ wormhole / wormhole-transit), with and without `--vksync`.

## Reproducing the sweep

```sh
EXE=./build/bin/Release/X3Engine.exe
CAM="225.83,25.00,39.78,-2.1031,0.0000"          # the 90-degree case
# AFTER (event horizon):
X3_MUTE=1 X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0 \
  $EXE --world space --screenshot out/a090_ON.png --shot-cam "$CAM"
# BEFORE (refraction lane only), same binary:
X3_WORMHOLE_NOHORIZON=1 X3_MUTE=1 X3_WORMHOLE_OPEN=1 X3_WORMHOLE_T=4.0 \
  $EXE --world space --screenshot out/a090_OFF.png --shot-cam "$CAM"
```

## The assertion side — `--test-wormholes` W19..W23

The sweep is judged by eye, but it is also **measured**, because a still cannot
protect this from regressing. W19a projects the impostor's rim into the plane
perpendicular to the line of sight and asserts the minor/major extent ratio is
1.0 at all five angles; W19b runs *the same projector* over the axis-aligned
mouth and asserts it collapses to |cos| — the defect and its absence in the same
units. W19c/W20c put an absolute size-times-coverage floor under 90 and 180
degrees, W21 asserts stable and unstable are still tellable apart edge-on, and
W22 asserts 60 Hz and 165 Hz agree. 95 checks, 0 failed (was 81).
