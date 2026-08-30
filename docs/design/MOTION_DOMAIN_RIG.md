# The Motion Domain — verification rig, and the motion blur resolve pass

**Written 2026-08-29** on `feat/motion-blur`, against `origin/integration/complete` @ `632bd67b`.
Implements deltas **#1** and **#2** of `docs/design/RENDER_FRAMEWORK_GUIDE_2026-08.md`.
Companion post-mortem: `docs/design/WHY_THE_SURVEYS_MISSED_IT.md`.

> ### Provenance
> Unreal Engine 5.8 source is on this machine and its **directory structure** was read to
> confirm the five-stage shape the framework guide describes (velocity-flatten, tile
> scatter/gather, tile classify, apply). **No Epic code, shader text, comments or identifiers
> were read into or copied into this repository**, and no Unreal file is quoted anywhere in
> this lane. The algorithm below is written from the public literature — McGuire, Hennessy,
> Bukowski and Osman, *A Reconstruction Filter for Plausible Motion Blur* (I3D 2012), and
> *Real-Time Rendering* 4th ed. section 12.5 — which is where the tile-max/neighbour-max
> construction and the cone/cylinder tap weighting come from in the first place. Same
> clean-room discipline `taa_resolve.frag` already states in its own header. X3Native is
> public; its provenance stays clean.

---

## 0. Why the rig is delta #2 and not an afterthought

Motion blur was diagnosed correctly **three separate times** in this repository
(`RENDER_GAP_ROADMAP.md:35`, `VEHICLE_UPGRADES.md:130`, `RACING_WORLD.md:642`) and never
reached a lane. The post-mortem's deepest finding is that this is not forgetfulness:

> Every verification gate in the tree is a **still-frame A/B from a static camera**. Motion
> blur on a static camera is **mathematically the identity function**. It scores 0 dB. The
> instrument could not fail on the absence.

So the effect and its instrument were built together, and the instrument was built **first**.

---

## PART A — THE MOTION-DOMAIN VERIFICATION RIG

`app/motion_rig.{h,cpp}`. **This is an instrument, not a motion-blur fixture.** It is the gate
for *every* temporal effect this engine grows — temporal upscaling (delta #3), volumetric
temporal reprojection, SSGI reprojection, anything whose existence is a function of time.
Point it at a new effect by supplying a different scene callback and a different motion axis.

### A.1 The capture

A deterministic **camera on rails**: fixed timestep, an analytic camera path (no spline file,
no keyframes, nothing to drift), N frames written as a numbered PNG series, and a
caller-supplied draw callback that receives the **path time** as its only time source.

Two device-state resets, both through **existing documented mechanisms**, so two runs of the
same config are bit-identical rather than merely close:

| What persists | How the rig neutralises it |
|---|---|
| TAA history | `setPostFX` invalidates history on an off-to-on transition; the rig toggles through off |
| TAA jitter phase (free-running counter, 8-frame Halton cycle) | `settle + frames` is required to be a multiple of 8, so every series starts on the same phase |
| Auto-exposure adaptation | already snaps on every headless frame by design |
| Motion-blur tap dither | phase forced to 0 in headless, for exactly this reason |

`settle` frames walk the path *backwards* from `t0`, so the camera is already in steady motion
— and therefore producing real velocity — by the time frame 0 is captured.

### A.2 The metric, and why it is shaped this way

A metric for this domain must distinguish **"the scene moved"** from **"the image is blurred
by that movement."** Those are two questions, so they get two numbers.

**`temporalRms`** — RMS luminance difference between consecutive frames. Answers *did anything
move*. It is a **precondition, never evidence of blur**: it also falls when the camera simply
moves less, which is the confound that makes a naive frame-difference metric useless here.

**`anisotropy`** — RMS luminance-gradient **energy** measured *along* the known motion axis,
divided by the same *across* it. This is the discriminator, and the reason is a property of the
two operations being told apart:

- **Translation is an isometry.** Sliding an image sideways leaves its gradient statistics
  exactly unchanged. Scene motion on its own **cannot** move this number.
- **Motion blur is a directional convolution.** It attenuates detail along the motion axis and
  leaves detail across it almost untouched. It **must** move this number, downward.

A metric that fired on motion alone would be a guard that does not guard. This one cannot, by
construction.

> **Energy, not `|gradient|` — this choice is load-bearing.** Mean absolute gradient is *total
> variation*, and total variation is **conserved** when a step edge is blurred: an edge of
> height H spread over N pixels gives N pixels of gradient H/N, which still sums to H. A
> mean-|gradient| metric would be nearly blind to the very thing it measures. Gradient energy
> falls as H^2/N and collapses once the blur exceeds the feature size.

**`DBI`** (Directional Blur Index) = `1 - anisotropy(test) / anisotropy(ref)`, between two
series captured on the *same* deterministic path. 0 = equally sharp along the motion axis;
positive = the test series lost detail specifically along the axis of motion.

### A.3 Proving the rig can see the thing — and that it does not hallucinate it

`--test-motionblur`, on the real Vulkan device, over a purpose-built probe scene.
**17/17 in Release and Debug.** Four cases guard the *instrument* before anything is measured
with it:

| Case | Result |
|---|---|
| **R1** two identical rails runs | **bit-identical** |
| **R2** metric self-check, positive — a synthetic 17 px horizontal box blur applied to a real captured frame **in memory**, no renderer in the loop | **DBI 0.522** — the metric *can* see directional blur |
| **R3** metric self-check, negative — a series against itself | **DBI exactly 0** — it does not manufacture a reading |
| **R4** probe sanity — the reference series carries real detail | RMS gradient 20.0 — nothing below can pass vacuously |

R2/R3 matter because without them a broken shader and a broken probe can cancel out and read
green. The metric is validated on pixels alone.

**The negative control** — the case the tree's existing gates structurally cannot express:

| Case | Result |
|---|---|
| **N1** static camera, blur ON vs OFF | **DBI 0.0000**, and **bit-identical** |
| **N2** nothing moved | static frame-to-frame energy is **9.1 %** of the moving series' |

N1 is bit-identical by construction, not by luck: `mb_blur.frag` early-outs when the
neighbourhood velocity is under half a pixel, and takes its centre sample with `texelFetch`
rather than a filtered `texture()` so the identity path is exact to the bit.

> **N2 is a ratio, not a zero, and that is a real finding about this renderer: under TAA a
> static camera is not temporally static.** The Halton jitter moves the projection sub-pixel
> every frame and the resolve blends 10 % of that jittered frame in, so consecutive frames of a
> dead-still camera differ by a small residual on high-contrast edges (measured RMS about 7.6
> on the probe checkerboard, against 83.9 for real motion). Asserting `== 0` would have been a
> lie; asserting "an order of magnitude below real motion" is the true claim.

**Calibration note, kept because it is instructive.** The probe's first checkerboard was
about 13 px per check against a about 17 px blur. The blur then exceeded one full check
*period* and washed the pattern to flat grey, destroying the horizontal edges as thoroughly as
the vertical ones: both gradients collapsed 3.7x together, anisotropy barely moved
(1.032 to 0.947), and a large, real, correct blur scored **DBI 0.08**. The metric was right;
the probe was wrong. The checks are now sized well *above* the blur length (about 57 px vs
about 17 px). If you reuse the rig for another effect, size your probe features against the
effect's scale.

---

## PART B — THE MOTION BLUR RESOLVE PASS

### B.1 Where it sits, and why nothing moved

Three fullscreen passes between `taa-history-copy` (`vk_graph.cpp:1915`) and `auto-exposure`
(`:1968`). **No existing pass was reordered.**

```
taa-resolve -> taa-history-copy -> mb-tilemax -> mb-neighbormax -> mb-blur
            -> auto-exposure -> bloom -> composite
```

*After* the history copy so a blurred frame never enters the TAA history — a blurred history
feeds back and smears permanently. *Before* auto-exposure and bloom so the exposure meter reads
what the player will see and the bloom chain blooms the blurred image, which is what a real
lens does.

### B.2 The chain

| Pass | Shader | Output |
|---|---|---|
| 1 | `shaders/mb_tilemax.frag` | per-tile max velocity, RG16F, 20 px tiles (64x36 at 720p) |
| 2 | `shaders/mb_neighbormax.frag` | max over a +/-2-tile neighbourhood |
| 3 | `shaders/mb_blur.frag` | depth-ordered reconstruction, full-res HDR |

**Why 1-2 exist at all.** A naive blur gathers each pixel backwards along *its own* velocity.
That is wrong at every silhouette, and silhouettes are where the eye looks. A wall pixel just
ahead of a fast car has zero velocity, so a naive filter leaves it perfectly sharp — yet the
car swept *across* it during the exposure and must deposit colour there. Tile-max plus
neighbour-max give every pixel a cheap conservative answer to *"what is the fastest thing near
me that could have swept over me?"* Without them you get a hard-edged silhouette around a
blurred interior, which reads worse than no blur at all.

**The invariant that keeps the gather exact:** `maxBlurPixels <= kReach * kTile` (= 40 px). A
pixel is only ever told about tiles within `kReach`, so a longer blur could come from a tile it
never inspected. `motionBlurMaxRadius()` clamps `r_mb_maxblur` to exactly that bound.

**The reconstruction filter** walks along the neighbourhood vector and decides per tap whether
that tap is a plausible contributor, using depth and the tap's own speed:

- a tap **in front of** the centre pixel, moving fast enough to have covered the distance
  during the exposure, swept across it and contributes: `cone(dist, |vY|)`
- a tap **behind** it contributes only in so far as the centre pixel is *itself* moving and
  therefore gathered light from behind: `cone(dist, |vX|)`
- two taps at comparable speed and depth blur into each other: `cylinder * cylinder`

That asymmetry **is** the depth ordering, and it has two consequences worth naming because they
are the artefacts cheap motion blur is known for:

- a **static foreground** object in front of a fast background receives nothing from it
  (`fg = 0`, and `cone(d, |vX| ~ 0) = 0`), so it stays sharp instead of being painted with
  background colour;
- a **sharp foreground** does not smear onto a moving background either
  (`cone(d, |vY| ~ 0) = 0`), so silhouettes do not double.

### B.3 The dt trap

`velocity.frag` writes `prevUV - curUV`: a displacement accumulated over **one frame**. It is
not a speed. At 165 Hz it is about 1/5 of what it is at 33 Hz for identical physical motion.
Scaling blur by it directly makes the effect **vanish at high framerate and overwhelm at low**
— i.e. strongest exactly when the machine is struggling, which is backwards.

`engine/rhi/MotionBlur.h` holds the **one** definition:

```
blurUV = velocityUV * shutter * (1 / referenceFps) / dt
```

so the blur represents a fixed physical exposure at every framerate. `shutter` is quoted *at*
`r_mb_reffps`; 0.5 at 60 Hz is the film 180-degree shutter, an 8.33 ms exposure forever.

It is a shared header **because both the renderer's UBO fill and the test battery call it**. A
formula duplicated between a parameter fill and its test is a guard that does not guard: the
test would stay green while the shipped path drifted.

Deliberately **not** a behavioural clamp on the ratio — capping it would cap it at *high*
framerate, where dt is small and the ratio is large, which is exactly the case the rule exists
to serve. The blur *length* is bounded downstream by `motionBlurMaxRadius()`, which is the
right place. A long-frame hitch reduces the ratio, correctly: a fixed exposure is a smaller
slice of a longer frame.

**Proof, three ways:**

| Case | Result |
|---|---|
| **D1** arithmetic, on the shipped function | identical exposure displacement at 60 and 165 Hz, **0.0000 % apart** |
| **D2** negative control | the **un**-normalised per-frame velocity differs by **63.6 %** — the rule is doing real work, and D1/D3 would go red without it |
| **D3** rendered — the same physical path stepped at 1/60 and 1/165, sampled at the same wall-clock instant (4/60 s = 11/165 s exactly) | **DBI 0.370 vs 0.377, 1.8 % apart**; and at 165 Hz the blur is still emphatically present (0.377 > 0.20), which is the failure this rule prevents |

D3 compares **blur strength** (DBI against each run's own blur-off reference) rather than raw
pixels, because TAA converges per *frame* and is therefore legitimately framerate-dependent.
That isolates the motion blur from TAA's own behaviour.

### B.4 Default: OFF

`r_motionblur 0`. **The effect ships off, not subtle-on.** It changes every frame of every
world, so it is opted into rather than discovered, and every existing determinism basin stays
byte-identical. When enabled the *look* is subtle by default: a 0.5 shutter at a 60 Hz
reference, 9 taps, blur clamped to 40 px.

| cvar | default | meaning |
|---|---|---|
| `r_motionblur` | `0` | master gate. Needs `r_velocity 1` (hence `r_taa 1` + a depth pre-pass) |
| `r_mb_shutter` | `0.5` | exposure fraction **at** `r_mb_reffps`; 0.5 = film 180-degree |
| `r_mb_reffps` | `60` | the reference the shutter is quoted against; blur is dt-normalised to it |
| `r_mb_samples` | `9` | taps along the blur vector, 3 to 31 |
| `r_mb_maxblur` | `0` | blur cap in pixels; 0 = the dilation's exact bound (40) |
| `r_mb_softz` | `0.05` | depth-ordering band, as a fraction of the centre pixel's view distance |
| `r_mb_dt` | `0` | fixed frame delta; 0 = measured. Headless uses `1/r_mb_reffps` for reproducible captures |

**Graceful degradation is exact, not approximate.** A missing `mb_*.frag.spv` leaves the
pipelines null; `r_velocity 0` means the passes are never built at all and `rgPostSrc` keeps
pointing at the TAA output. Case **V1** measures this rather than asserting it: with
`r_velocity 0`, `r_motionblur 1` is **bit-identical** to `r_motionblur 0`.

### B.5 Measured

| Case | Result |
|---|---|
| **P1** the scene moved in both series | frame-to-frame energy 83.9 (off) / 71.3 (on) |
| **P2** blur ON vs OFF separates, moving camera | **DBI 0.377** |
| **Z2** depth ordering, positive half — the moving background slab | **DBI 0.345** |
| **Z1** depth ordering — the static foreground pillar in front of it | **DBI 0.0006** |

Z1/Z2 are one pair of frames, one static camera, one fast background: the far surface blurs by
0.345 while the near static surface moves by 0.0006. That is the depth ordering, measured.

**GPU cost**, 1280x720, RTX 5090, 15 taps, probe scene (trivial geometry, so this is close to
pure post cost):

| | mean GPU |
|---|---|
| static camera, blur off | 0.082 ms |
| static camera, blur on (early-out fires everywhere) | 0.128 ms — **+0.046 ms floor** |
| full-frame object motion, blur off | 0.100 ms |
| full-frame object motion, blur on | 0.160 ms — **+0.060 ms** |

The floor is the two dilation passes plus the blur pass's early-out; it is paid whenever the
feature is on. On the real default-world smoketest the delta is about 0.045 ms and is **flat
across tap count** (9/15/31 all within noise), because tap count only costs where something is
actually moving.

---

## PART C — VELOCITY COVERAGE (delta #4 — reported, NOT fixed in this lane)

The velocity pre-pass re-rasterizes **opaque draws only**: `recordVelocityPassBody` iterates
`[0, m_frameCmdOpaque)` (`vk_gi_rt.cpp:452`), and the blend tail is split off at
`vk_passes.cpp:2846-2848`.

### Writes velocity

| Category | Note |
|---|---|
| Opaque static meshes | — |
| Skinned characters | skin-compute runs first (`vk_graph.cpp:455`); the pass binds a **second vertex stream** = last frame's skinned verts (`vk_gi_rt.cpp:454`) |
| Dynamic / moving props | via the per-object prev-model SSBO ring (`vk_gi_rt.cpp:244-422`, fill `vk_passes.cpp:2706-2713`) |
| Terrain | opaque |
| **First-person viewmodel** | submitted through `device.drawMesh` (`app/weapon.cpp`, `drawWeaponAt`), i.e. **opaque**, so it *does* write velocity — and because it is view-pinned its world transform tracks the camera exactly, giving it about 0 screen velocity. **It correctly does not blur when you turn.** This is a case where per-object velocity is strictly better than a camera-only reconstruction would have been; no masking is needed. |

### Does NOT write velocity

| Category | Where it composites | Consequence under blur |
|---|---|---|
| Sky / no-geometry | attachment CLEARs to 0 | **Handled in this lane.** Both `mb_tilemax` and `mb_blur` substitute a camera-only reconstruction where depth is at the far plane, so the sky blurs correctly when the camera rotates |
| Glass / alpha-blended meshes | `vk_graph.cpp:1251`, blend tail | inherits the velocity of the opaque surface behind it |
| Water | `:1155` | same |
| Particles + decals | `:1689` | same |
| GPU debris shards | `:1638` | same |

For the four in the lower group the blur reads whatever opaque surface is *behind* them. It
degrades toward "blurred with the background" rather than toward garbage, but it is wrong in
both directions: **a fast particle over a static wall will not blur, and a static particle in
front of a fast car will.**

> **Consumer note — the wormhole-transit lane.** That lane currently fakes per-object blur with
> a 3-segment comet taper on its fastest specks and wants this pass to replace it. **If those
> specks are particles or debris, this pass will not blur them today** — delta #4 is a hard
> prerequisite for that specific substitution. If they are opaque mesh draws, they blur now.
> Worth checking which before removing the taper.

---

## PART D — TWO PRE-EXISTING DEFECTS FOUND BY ADDING A SECOND CONSUMER

Both were invisible while `m_velImg` had one consumer. This is the resource ledger's
single-consumer flag paying for itself.

### D.1 `velocity.frag` subtracts a jitter that was never added

Its header states its jitter lanes are *"0 by construction"* because the matrices it is fed are
unjittered. **`vk_passes.cpp` fills them with the real per-frame jitter.** The "defensive"
subtraction is therefore active, and the motion vector it writes is

```
trueVel_uv - (jitPrevNdc - jitCurNdc) * 0.5
```

— up to **about 1 pixel of spurious motion on a dead-static camera**. TAA absorbs it
(sub-pixel, inside the Catmull-Rom resample and the neighbourhood clamp), which is why it has
never been seen. Motion blur cannot, because a static camera must be *exactly* the identity
function.

**Not fixed at the source in this lane:** correcting it moves every `r_velocity 1` capture and
needs its own A/B. Motion blur cancels the term **for itself** via `MbUBO::params3`, computed
from the same two jitter values. Filed for a follow-up lane.

### D.2 The static-camera baseline is not reproducible with the RT stack at defaults

Measured while building the rig: **two identical static-camera runs were not bit-identical,
with motion blur entirely disabled.** The TLAS is built into a 3-slot ring with occasional
device waits (`[tlas-db]` in the log), so which acceleration structure a frame samples depends
on ring timing. On a *moving* camera TAA's neighbourhood clamp discards the difference within a
frame or two; on a *static* camera the history accumulates it and it never washes out.

Pre-existing, and a property of the RT path rather than of this lane — but a rig whose baseline
drifts cannot make a bit-identity claim about anything, so the probe scene switches the
ray-traced stack off (SSAO stays on, because the depth pre-pass and therefore the velocity pass
are gated on one of SSAO/SSGI/RT-AO/reflections being enabled — remove that line and the whole
test silently measures nothing).

---

## PART E — GATE RESULTS

Release **and** Debug, both from a **wiped** build directory. All three exes build and boot.

| | Release | Debug |
|---|---|---|
| `X3Engine` / `X3LevelArchitect` / `X3Space` build + `--smoketest` | exit 0 | exit 0 |
| `allocationCount` | 0 | 0 |
| VUID | (meaningless — banner says layers=OFF) | **0**, `layers=ON sync-validation=ON` |
| SYNC-HAZARD | — | **0** |

Held, all at their exact expected numbers: `--test-space` 31/0 (**T19-T24b green**),
`--test-wormholes` 95/0, `--test-targeting` 32/32, `--test-ship-damage` 104/104,
`--test-comms` 45/0, `--test-ui` 42/0, `--test-rifthub` 33/33, `--test-level1` 21/0,
`--test-csm` 27/0, `--test-introorch` 27/27. Added: `--test-motionblur` **17/17** in both
configs.

Pre-existing failures unchanged **by name**: `--test-ai` 12/1 *Tf nav-wired enemy routes AROUND
a wall*, `--test-phase2b` 5/1 *Td Door E closed until boss dead*.

### Sync counts, every configuration

Counted as `SYNC-HAZARD-` (with the hyphen — `SYNC-HAZARD checking active` in the banner is not
a hazard, and matching it inflates the count).

| Config | layers | VUID | SYNC-HAZARD | mb chain active |
|---|---|---|---|---|
| Debug, default, blur **off** | ON / sync ON | 0 | 0 | no |
| Debug, default, blur **on** | ON / sync ON | 0 | 0 | **yes** |
| Debug, `--world canonlevel`, blur **on** | ON / sync ON | 0 | 0 | **yes** |
| Debug, `--world space`, blur **on** | ON / sync ON | 0 | 0 | **yes** |
| Debug, `X3Space.exe`, blur **on** | ON / sync ON | 0 | 0 | **yes** |

**The documented `--world space` 15-WAR reading did not reproduce on this tip — it reads 0.**
That is the value the framework guide says you get *with wormholes present*, and wormholes are
present here (`--test-wormholes` 95/0 passes on this branch); there is no flag to remove them,
so the 15 could not be re-exposed for comparison. **0 is therefore reported as "unchanged /
could not be re-exposed", not as a win.**

Because a 0 is worthless unless the instrument can go red, that was verified directly: deleting
the `mb-blur` pass' `rgMbNeigh` READ declaration and rebuilding produced **60 x
`VUID-vkCmdDraw-imageLayout-00344`** under `--vksync`, with the chain-ACTIVE line present. The
declaration was restored and the count returned to 0. A one-shot `[rhi] motion-blur chain
ACTIVE this frame` line now prints whenever the chain is built, because `r_motionblur 1` is a
silent no-op when `r_velocity` is 0 — without it a clean validation run is indistinguishable
from one where the passes never existed. **Every sync/VUID claim about this feature must quote
that line.**

---

## PART F — LEFT UNDONE

1. **Velocity coverage (delta #4)** — reported in Part C, not fixed. It is the prerequisite for
   blurring particles, debris, water and glass, and it is the gating item for the
   wormhole-transit lane's comet-taper replacement.
2. **`velocity.frag`'s jitter over-subtraction (D.1)** — cancelled locally for motion blur, not
   fixed at the source. Fixing it properly moves every `r_velocity 1` capture.
3. **The RT-path static-camera nondeterminism (D.2)** — characterised, not fixed. Worked around
   inside the probe scene only.
4. **The tile reduction is a fragment pass, not compute.** Chosen to reuse
   `createFullscreenPipeline` unchanged rather than add storage-image plumbing. Total work is
   about one fullscreen pass of taps; a compute version with shared-memory reduction would be
   cheaper at 4K. Not measured as a problem at 720p/1080p.
5. **No tile classification.** The reference architecture sorts tiles by how much blur they need
   and dispatches indirectly per category. Here every pixel pays the neighbourhood lookup and
   the early-out; the cost measurement above says that floor is 0.046 ms at 720p, so this has
   not earned its complexity yet. Revisit at 4K.
6. **Not yet exercised on a real world in motion under human eyes.** The pixel evidence is the
   probe scene plus the A/B crops in `captures/mb/review/`. Tim has not opted the effect into
   any world, which is the intent — `r_motionblur` defaults to 0.
