# Render Framework Guide — Motion Blur, Volumetrics, Temporal Reprojection
## plus a pipeline-stage survey of the whole frame

**Written 2026-08-28** against `origin/integration/complete` @ `632bd67b`.
Companion: `WHY_THE_SURVEYS_MISSED_IT.md` (the post-mortem that dictates this document's shape).

**This is a study, not an implementation.** No engine code changes accompany it.

> ### Provenance
> Unreal Engine 5.8 source is on this machine and was read **to understand technique only**.
> **No Epic code, shader text, or comments were copied** into this document or into X3Native.
> Unreal file paths appear purely as reference pointers so a reader can locate the same
> material; every algorithm description, pass ordering, and rationale below is written from
> scratch in our own words. X3Native is a public repository and its provenance must stay clean.
> This is the same clean-room discipline the tree already applies to id-lineage source, applied
> here for licence reasons rather than lineage reasons.

---

## 0. How to read this, and why it is shaped this way

The previous three surveys (`RENDER_QUALITY_UPLIFT.md`, `RENDER_FIDELITY_GAPS_PLAN.md`,
`RENDER_GAP_ROADMAP.md`) were organised as **lists of named features**, and verified with
**still frames**. Both choices are why a fully-built velocity buffer sat for months with one
consumer instead of three. The post-mortem sets four constraints; this document obeys them:

- **C1** — indexed by **pipeline stage**, never by feature name (§3).
- **C2** — a separate **resource ledger** (§2) naming every intermediate buffer and *its
  consumers*, so a producer with one consumer is visible as a standing question.
- **C3** — effort priced against the tree **as of today**, with rows whose inputs already
  exist marked as a distinct high-leverage class.
- **C4** — motion-domain items carry a **motion-domain verification requirement**; a still
  capture cannot gate them.

**Do not read the "X3Native today" column as aspirational.** Every claim in it was verified
in this tree at the cited file:line while writing. The previous surveys' worst failure mode
was reporting existing features as gaps (`RENDER_QUALITY_UPLIFT.md:184-188` claims "no
environment map / cubemap infrastructure anywhere" and "no G-buffer"; both are now false).
Stale claims cost the same attention as missed ones.

---

## 1. The frame as it actually is

Verified in `engine/rhi/vk/vk_graph.cpp::buildAndExecuteGraph` (`:204`). Gates resolved in the
prologue at `:266-331`.

```
 1  skin-compute            :455   m_skinStepPending
 2  debris-compute          :473
 3  hzb-build               :500   r_hzb / r_vis 3      (LAST frame's depth, half-res)
 4  gpu-cull dispatch       :520   r_cullpath
 5  shadow-depth  (CSM)     :547   4-cascade 2D array, r_csm
 6  depth-prepass           :579   prePassOn = ssao||gi||rtao||refl
 7  velocity-prepass        :612   r_velocity  (REQUIRES prePassOn)      -> m_velImg RG16F full-res
 8  ssao / ssao-blur        :666
12  rtao-compute            :823   r_rtao
13  refl-compute            :851   r_ssr / r_rtreflections  (HARD-GATED on r_taa, :284)
14  refl-denoise-aux        :887
15  refl-denoise (a-trous)  :923   r_refldenoise
16  ddgi-rays / ddgi-update :964   r_ddgi
18  MAIN-COLOR (sky+opaque) :1031  -> HDR
19  water                   :1155
20  glass-scenecopy         :1200  + glass-frost mip chain :1225
21  glass                   :1251
22  gi-gather/temporal/     :1339  r_ssgi
    denoise/apply/prevdepth :1538
27  rtao-apply              :1577
28  debris-draw             :1638
29  particles + DECALS      :1689
30  depth-fog / VOLUMETRIC  :1749  r_fog*   <- TRUE last HDR writer (LOAD_OP_LOAD onto m_hdrView)
31  TAA-RESOLVE             :1860  r_taa
32  taa-history-copy        :1915
33  auto-exposure (compute) :1968  r_autoexposure
34  bloom-down / bloom-up   :1991  r_bloom
36  COMPOSITE (ACES+filmic) :2005  + HUD :2102
37  editor-ui               :2134
38  capture-copy / present  :2181
```

Three structural facts that matter for everything below:

- **Volumetric scattering runs before the TAA resolve.** Deliberate and correct: the march's
  interleaved-gradient dither is integrated away by TAA's 8-frame Halton cycle. It also means
  the volumetric result *is* in TAA history, so it inherits TAA's ghosting behaviour.
- **`r_ssr` is silently forced off when `r_taa 0`** (`vk_graph.cpp:284`). Reflections have a
  hard dependency on the TAA history image.
- **`r_velocity 1` is a no-op unless SSAO, SSGI, RTAO or reflections are also on**, because
  the velocity pass requires the depth pre-pass (`vk_graph.cpp:289`, `:329`). Worth knowing
  before anyone benchmarks a new velocity consumer.

---

## 2. Resource ledger — the check the old surveys did not have

Every significant per-frame intermediate, and who reads it. **A producer with one consumer is
a standing question, not a finished state.**

| Resource | Produced at | Format / res | Consumers | Count |
|---|---|---|---|---|
| `m_depthImg` | depth-prepass `:579` | D32F, full | main pass (EQUAL), ssao, ssgi, rtao, refl, fog/volumetric, taa_resolve, hzb (next frame) | many |
| **`m_velImg`** | **velocity-prepass `:612`** | **RG16F, full** (`VulkanRenderDevice_internal.h:2823`, created `vk_targets.cpp:328`) | **`taa_resolve.frag:41` only** (written once, `vk_targets.cpp:1019`) | **1 ⚠** |
| `m_hzbImg` | hzb-build `:500` | R32F, **half**, single-polarity, **stale by one frame** (`vk_pipelines.cpp:240-253`) | `cull.comp` only (`vk_pipelines.cpp:322`) | 1 ⚠ |
| CSM array | shadow-depth `:547` | depth array ×4 | mesh.frag, volumetric.frag `:36-45` | 2 |
| scene-copy + frost mips | `:1200`/`:1225` | HDR + mip chain | `glass.frag` | 1 |
| `m_taaOutImg` / hist | taa-resolve `:1860` | HDR | autoexposure, bloom, composite, refl (history) | 4 |
| DDGI probe volume | ddgi-update `:982` | irradiance/depth atlas | mesh.frag | 1 |
| refl buffer | refl-compute `:851` + denoise | HDR, half | `inc/mesh_reflections.glsl` | 1 |
| SSAO / SSGI targets | `:666` / `:1339` | R8 / HDR half | mesh.frag, rtao-apply | 2 |

**Two flagged rows.**

`m_velImg` is the headline: a full-resolution RG16F target, rasterized by a dedicated pipeline
with its own descriptor pool, UBO ring, and **per-object previous-model-matrix SSBO ring**
(`engine/rhi/vk/vk_gi_rt.cpp:244-422`, fill at `vk_passes.cpp:2706-2713`), consumed by exactly
one shader. In a modern renderer this buffer normally feeds **three or four** consumers:
temporal AA/upscaling, motion blur, temporal denoiser reprojection, and temporal volumetric
reprojection. We built the expensive part and wired one wire.

`m_hzbImg` is flagged for the opposite reason — to prevent a future survey from mistaking it
for a usable HiZ. It is half-res, one-frame-stale, and **single-polarity** (`REDUCE_MAX` is a
spec constant set to min-or-max by reversed-Z at `GpuCull.cpp:139`, never both). It is fine
for occlusion culling and **not usable as-is** for screen-space ray marching, which needs a
current-frame conservative pyramid.

---

# PART 2 — THE THREE EFFECTS

---

## 2.1 MOTION BLUR — *inputs present, consumer absent*

### What it simulates, and why it reads as cinematic

A real camera integrates light over a finite exposure — the shutter is open for some fraction
of the frame interval (the "shutter angle"; 180° = half the frame). Anything that moves during
that window deposits energy along the path it travelled, not at a point. A film frame is
therefore a **line integral of the scene over time**, not a sample of it.

A renderer that samples one instant produces frames that are each individually *too correct*.
Played back, the eye receives a sequence of sharp, temporally-uncorrelated samples and reads
it as strobing — the "video look" or judder. This is the single most reliable subconscious
tell separating game footage from film footage, and it is why reference gameplay capture (the
GTA VI clip our punchlist derives from) looks softer than our renders under still comparison
while looking *more* real in motion.

Two distinct phenomena, both required:

- **Camera blur** — the whole frame smears when the view rotates or translates. Cheap: derivable
  from the camera matrices alone.
- **Per-object blur** — a car passes a static camera and smears while the background stays
  sharp. Requires per-pixel velocity. **This is the one we already have the data for.**

### How the reference engine structures it

Unreal builds motion blur as a **five-stage tile pipeline**, not a single blur. Reference
pointers: `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessMotionBlur.cpp`,
shaders under `Engine/Shaders/Private/MotionBlur/`.

1. **Velocity flatten** — the raw velocity buffer is converted into the form the blur needs
   (a length/direction encoding, with camera motion folded in and the shutter scale applied),
   and simultaneously reduced to per-tile summaries. Tile size is a fixed constant
   (`PostProcessMotionBlur.cpp:123`).
2. **Tile max / neighbour max** — for each tile, the *largest* velocity in it and in its
   neighbourhood. This exists to solve the fundamental problem below. Two implementations are
   offered: a **scatter** version that rasterizes each tile as a quad stretched along its
   motion so a fast tile physically reaches into the tiles it will bleed into, and a **gather**
   version where each tile searches its neighbourhood. Scatter is more accurate for long
   vectors; gather is more GPU-friendly.
3. **Tile classification** — tiles are sorted into categories by how much blur they need
   (static, one dominant velocity, two conflicting velocities, half-res-sufficient…), and an
   indirect dispatch is built per category (`PostProcessMotionBlur.cpp:941-988`). A static tile
   costs almost nothing; only genuinely fast tiles pay for the expensive filter.
4. **Reconstruction filter** — per pixel, sample along the tile's dominant velocity direction,
   weighting each tap by whether that tap's own velocity and depth make it a plausible
   contributor to this pixel.
5. Output, which also emits the **half- and quarter-resolution scene-colour slices that the
   bloom/DOF chain then consumes** (`PostProcess/PostProcessing.cpp:~1271`) — so the downsample
   is not paid twice and bloom correctly blooms the blurred image.

**The one architectural idea to take from this:** stages 1–2 are shared with the temporal
upscaler. Unreal lets TSR emit the motion-blur velocity-flatten/tile textures as a by-product
of its own velocity dilation, gated by an "allow external" switch
(`TemporalSuperResolution.cpp:2546-2559`; `PostProcessMotionBlur.h:57-70`;
`PostProcessMotionBlur.cpp:61`). **Motion blur and temporal AA are one subsystem sharing
intermediates, not two features.** Our surveys split them across ranks 2 and 8 of the same
table. That split is the bug.

### The problem that forces the tile architecture

A naive blur samples backwards along each pixel's own velocity. That is wrong at every
silhouette, and wrongness at silhouettes is exactly where the eye looks.

Consider a fast car against a static wall. A pixel **on the wall, just ahead of the car**, has
zero velocity — so a naive filter leaves it perfectly sharp. But physically, during the
exposure the car *moved across that pixel*, so it must receive some of the car's colour. The
blur has to be **gathered from where the moving thing came from and is going to**, which means
a static pixel must know about its fast neighbours. That is the entire reason for tile-max and
neighbour-max: they give every pixel a cheap conservative answer to "what is the fastest thing
near me that could have swept over me?"

Get this wrong and you get the classic artefact: a hard-edged sharp silhouette with a blurred
interior, which reads worse than no blur at all.

### Failure modes to defend against

| Failure | Cause | Defence |
|---|---|---|
| Sharp silhouette on a blurred object | naive per-pixel gather, no neighbourhood | tile-max / neighbour-max (above) |
| Background bleeding onto a foreground object | taps weighted without a depth test | weight taps by depth compatibility — a tap far behind the centre pixel may contribute *to* it but not *from* it |
| Blur amount changes with framerate | velocity is a per-**frame** delta, not per-**second** | **normalise by dt against a reference shutter.** See the gotcha below — this is our house rule |
| Skinned / vertex-animated geometry doesn't blur | velocity written only for rigid transforms | the velocity pass must be fed the same skinning as the main pass |
| Transparency, particles, water don't blur, or blur wrongly | translucency typically writes no velocity, and no depth | a separate translucent velocity pass, or deliberately composite affected translucency *after* blur |
| Blur pollutes the temporal history | blurred colour written into the TAA history image | run blur on the TAA **output**, after the history copy |
| Blurred UI / viewmodel | first-person weapon has huge velocity relative to world | exclude viewmodel and UI from the velocity pass or mask them |

### What X3Native already has

| Motion blur needs | X3Native has | Where |
|---|---|---|
| Per-pixel screen velocity | **Yes** — RG16F, **full-res**, UV-space `prevUV - curUV` | `shaders/velocity.frag:24,36`; target `VulkanRenderDevice_internal.h:2823`, created `vk_targets.cpp:328` |
| Per-object (not just camera) motion | **Yes** — prev-model matrix SSBO ring | `vk_gi_rt.cpp:244-422`; fill `vk_passes.cpp:2706-2713` |
| Jitter correctly removed | **Yes** — subtracted from both endpoints against unjittered matrices | `velocity.frag:27-34`; UBO fill `vk_passes.cpp:2013-2025` |
| Skinned geometry covered | **Yes** — skin-compute runs first (`vk_graph.cpp:455`) and the velocity pass re-rasterizes the frame's draws | `vk_gi_rt.cpp:438-449` |
| Depth for tap rejection | **Yes** — D32F full-res | depth-prepass `vk_graph.cpp:579` |
| A correct slot in the frame | **Yes** — between `taa-history-copy` (`:1915`) and `auto-exposure` (`:1968`) | see below |
| Graceful-degradation idiom to copy | **Yes** — the velocity pass's own "missing .spv ⇒ feature off, byte-identical" pattern | `vk_gi_rt.cpp:249-258` |
| **A blur pass** | **No.** Zero hits for motion-blur anywhere in `shaders/` or `engine/` | — |

**The slot is already correct and free.** Motion blur belongs after `taa-history-copy`
(`vk_graph.cpp:1915`) and before `auto-exposure` (`:1968`). After the history copy so the
blurred frame never enters TAA history (a blurred history feeds back and smears permanently);
before bloom so the bloom chain blooms the blurred image, which is what a real lens does.
This mirrors the reference engine's ordering exactly, and it needs no reordering of any
existing pass.

### The gotcha that will bite: shutter time, not frame time

`velocity.frag` writes a **per-frame** displacement. At 165 Hz on this hardware that
displacement is roughly one-fifth of what it is at 33 Hz for the same physical motion. Scaling
blur directly by it makes the effect **vanish at high framerate and overwhelm at low
framerate** — and worse, makes it *stronger when the machine is struggling*, which is exactly
backwards.

The scale must be
`blurPixels = velocityUV * extent * (shutterAngle/360) * (referenceFrameTime / dt)`,
so the blur represents a fixed **physical exposure duration** regardless of framerate. The
reference engine does the equivalent by folding a time-scale term into the velocity scale
before the flatten stage (`PostProcessMotionBlur.cpp:623-630`).

This is the standing house rule — *delta time, never frame time* — in a new place. It is worth
writing into the pass's header comment on day one, because it is invisible on the machine you
develop on and obvious on every other machine.

### Honest remaining delta

**Effort: S–M** (S ≤2 days, M ≈1 week), split:

- **S — a correct single-tier blur.** One fullscreen fragment pass reading `m_velImg`,
  `m_taaOutImg` and depth; N taps along the velocity vector with depth-weighted rejection;
  `r_motionblur` + `r_mb_shutter` cvars; dt normalisation as above. Slots between
  `taa-history-copy` (`vk_graph.cpp:1915`) and `auto-exposure` (`:1968`) with no reordering. This alone delivers most of the perceived effect for camera motion and
  for large fast objects, which is the entire "speed sensation" ask in
  `VEHICLE_UPGRADES.md:130`.
- **+S — the tile pass.** A tile-max/neighbour-max compute pre-pass to fix silhouettes. Needed
  before it survives a close look at a car passing a static camera. Do not skip it permanently;
  the sharp-silhouette artefact is the thing that makes cheap motion blur look cheap.
- **+S — coverage.** Decide and document what does *not* write velocity today (particles at
  `vk_graph.cpp:1689`, debris `:1638`, water `:1155`, glass `:1251`) and either give them
  velocity or mask them. Untouched, they will read as suspiciously sharp holes punched through
  a blurred frame.

**Verification is the real constraint, not the code.** Per constraint C4, no still capture can
gate this. It needs the deterministic multi-frame rig described in §4.

---

## 2.2 VOLUMETRIC SCATTERING — *exists; the delta is architectural, not existential*

### What it simulates

Air is not empty. It contains aerosols, dust, moisture and smoke that both **absorb**
(extinction, Beer–Lambert) and **scatter** light out of and into the view ray. The visible
consequences are: distant objects lose contrast and shift toward the sky's colour (aerial
perspective); a light source seen through haze wears a halo; and light passing through a gap
in an occluder carves a visible shaft, because the lit air along that path scatters toward you
while the shadowed air beside it does not.

The term that makes it read as *photographic* rather than as *grey fog* is the **phase
function** — scattering is not isotropic. Real aerosols scatter strongly forward, so looking
toward a light through haze blooms hard while looking away from it stays subtle. Without
anisotropy, volumetrics look like someone lowered a grey sheet.

### How the reference engine structures it

Unreal's volumetric fog is **froxel-based**, not screen-space. Reference pointers:
`Engine/Source/Runtime/Renderer/Private/VolumetricFog.cpp`, `Engine/Shaders/Private/VolumetricFog.usf`.

A 3D grid is laid over the view frustum — grid cells sized ~16 screen pixels laterally
(`VolumetricFog.cpp:118`) with 64 slices in depth (`:126`), distributed **exponentially**
(`:110`) so near air gets fine slices and distant air gets coarse ones. Then:

1. **Voxelize** participating media into the grid (density, albedo, emissive) — fog volumes and
   materials write into froxels.
2. **Light scattering** — for each froxel, evaluate the lights that reach it, shadowed, times
   the phase function. Local lights are injected separately depending on whether they cast
   shadows (`:102`). Emissive media contribute (`:175`).
3. **Temporal reprojection of the volume itself** (`:134`, history weight 0.9 at `:150`, jitter
   at `:142`, and a supersample burst when history misses at `:158`). This is the key
   performance idea: because the result is temporally accumulated, each frame can afford very
   few samples per froxel.
4. **Final integration** — march front-to-back once along Z, producing a volume texture where
   each froxel holds *integrated in-scattered radiance and transmittance from the camera to
   that depth*.

The payoff of producing a **volume texture** rather than a screen overlay is that **any pass
can sample it at any depth**. Opaque surfaces, translucent surfaces, particles, and hair all
look up their own froxel and get correctly-attenuated, correctly-in-scattered air in front of
them. Screen-space fog cannot do this, because a translucent surface's depth is not in the
depth buffer.

### Failure modes

| Failure | Cause | Defence |
|---|---|---|
| Banding / "onion rings" | too few march steps, all rays starting at the same phase | per-pixel low-discrepancy dither of the ray start + per-frame rotation, integrated by TAA |
| Light leaking through walls | froxel/step samples a shadow map at a position whose occluder is thinner than a step | shadow-tap per step; conservative depth handling |
| Volume ghosting when the camera turns fast | temporal reprojection of the volume with a fixed weight | reduce history weight on reprojection failure and supersample that frame |
| Aliasing/crawl on shaft edges | high-frequency shadow signal at low sample count | jitter + temporal accumulation; soft-fade the light contribution near range limits |
| Fog on translucency wrong or absent | screen-space application at depth-buffer depth only | froxel volume sampled per-surface (the architectural fix) |
| Cost explodes with light count | every light evaluated per sample | per-ray or per-froxel light culling |

### What X3Native already has

**`shaders/volumetric.frag` (232 lines) is a real, well-built raymarched scattering pass**, not
a stub. Enabled by `FogParams::volumetric` (`engine/rhi/IRenderDevice.h:399`), it *replaces*
the flat `fog.frag` at the same graph slot (`vk_graph.cpp:1749`), and reduces to byte-identical
flat fog when `scatterStrength == 0` (`IRenderDevice.h:396-400`). Present and verified:

- **Sun shafts** — one hardware-compare tap into the same CSM the meshes receive, per march step
  (`volumetric.frag:36-45`).
- **Point/spot light haze** — the frame's forward light array scattered into the air, using the
  *same* windowed falloff and spot-cone law as `inc/mesh_lighting.glsl`, so haze dies exactly
  where surface lighting dies. The flashlight lane routes its spot cone through this.
- **Henyey–Greenstein phase** with `anisotropy` default 0.70 (`IRenderDevice.h:401`) — the term
  that sells the effect.
- **Per-light pre-cull** — 64 cheap ray-vs-range tests up front collapse the inner loop to
  `kMaxRayLights`.
- **Non-uniform step distribution** — `t(s) = tEnd·(s/steps)^kMarchPow` packs samples near the
  camera, with segment lengths carried correctly so the Beer–Lambert integral stays right.
- **Dither** — interleaved-gradient noise with a golden-ratio per-frame rotation, explicitly
  designed to be integrated away by TAA's 8-frame cycle. Correct, and correctly ordered
  (volumetric at `:1749` runs *before* TAA at `:1860`).
- **Aerial perspective** on the flat path — height falloff and sky-colour blend
  (`IRenderDevice.h:382-389`), shipped in GTA6 campaign Phase 0.3.
- Steps clamped 4–64, live cvars `r_fogdensity / r_fogstart / r_fogheight / r_fogskyblend / r_fogmax`.

This is a solid mid-tier volumetric. Reporting it as a gap would repeat the old surveys' error.

### Honest remaining delta

Three real differences, in leverage order:

**(a) It is screen-space, so it cannot fog transparency correctly. Effort M.**
The pass runs fullscreen against the depth buffer after the last HDR writer. Glass
(`vk_graph.cpp:1251`), water (`:1155`), particles and debris (`:1689`, `:1638`) are composited
*before* it and are fogged at whatever depth the depth buffer holds — which for alpha-blended
surfaces is the opaque geometry behind them, not the surface itself. Symptom: a neon sign seen
through glass, or smoke in front of a shaft, receives the wrong amount of air. The fix is the
froxel volume: produce an integrated scattering/transmittance 3D texture and have every shading
pass look up its own depth. That is the architectural change, and it is what unlocks
volumetrics that behave correctly everywhere rather than only on opaque pixels.

**(b) No temporal reprojection of the volume. Effort S–M, inputs present.**
Today the march re-integrates from scratch every frame at full screen resolution and relies
solely on TAA to hide the noise. A froxel volume with its own history — reprojected using the
velocity/camera data we already produce — would let step count drop sharply while *improving*
stability. Note this is a **second consumer for `m_velImg`**: the same underused resource from
§2. Cost today scales as `screenPixels × steps`; froxel cost scales as `gridX × gridY × gridZ`
with the lateral grid at ~1/16 screen resolution.

**(c) No participating-media authoring. Effort M–L.**
Density is globally uniform (modulated only by the flat path's height falloff). There is no way
to place a fog volume in a room, author a smoke plume as a medium, or make a light-shaft
corridor denser than the street outside. This is a content-authoring capability, not an
image-quality one, and it should follow (a).

**Verdict: deepen, don't rebuild.** (b) is cheap and buys headroom that funds (a). (a) is the
one that changes what is possible. (c) is a want, not a gap.

---

## 2.3 TEMPORAL REPROJECTION AND UPSCALING — *exists at the 2015 tier; the delta is upscaling*

### What it does

Rendering one sample per pixel per frame under-samples a signal that is far higher-frequency
than the pixel grid, which produces aliasing, and — worse — aliasing that *changes every
frame*, which the eye reads as crawling and sparkle. Temporal techniques fix this by treating
consecutive frames as samples of the same signal: jitter the projection sub-pixel each frame
so successive frames sample *different* positions within each pixel, then reproject last
frame's accumulated result into this frame's coordinates and blend. Over N frames you converge
on an N×-supersampled image at 1× cost.

The entire difficulty is **reprojection is a lie**. A pixel's history may not exist (it was
occluded last frame), may belong to a different surface (disocclusion), or may be the same
surface under different lighting (a shadow moved across it). Blending in wrong history produces
ghosting and smearing. Every generation of this technique is defined by how intelligently it
decides *when to distrust history*.

Once that machinery exists, **upscaling is nearly free**: if you are already accumulating
multiple jittered samples per output pixel, you can render fewer input pixels than output pixels
and let accumulation make up the difference. This is where the performance is.

### How the reference engine structures it

Unreal's TSR is a **ten-pass chain**, reference pointers under
`Engine/Shaders/Private/TemporalSuperResolution/` and
`Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp`. The pass
order, from the RDG event names in that file:

1. **MeasureFlickeringLuma** (`:1770`) — records per-pixel luminance *before* the resolve, so
   next frame can distinguish "this pixel legitimately flickers" (fireflies, specular sparkle)
   from "history is wrong". Decoupling flicker measurement from rejection is the single most
   important idea in the chain.
2. **MeasureThinGeometryCoverage** (`:1810`) / **DetectThinGeometry** (`:2835`) — wires, railings
   and foliage are narrower than a pixel and are the hardest case for any temporal method.
3. **DilateVelocity** (`:2586`) — dilate motion vectors toward the *closest occluder*, and build
   a **reprojection field** rather than one vector per pixel. Also the stage that optionally
   emits the motion-blur tile textures.
4. **DecimateHistory** (`:2728`) — reproject and resample history into the current grid.
5. **WeightRelaxation** (`:2892`).
6. **RejectShading** (`:3135`) — the core. Compare current shading against reprojected history
   over a neighbourhood and produce a *per-pixel* rejection strength.
7. **SpatialAntiAliasing** (`:3178`) — spatially antialias the pixels where history was rejected,
   so a rejected pixel still gets a clean result instead of raw aliasing. Without this, history
   rejection trades ghosting for shimmer.
8. **UpdateHistory** (`:3371`) → **ResolveHistory** (`:3460`) — accumulate, then output at
   display resolution.

Two further ideas worth naming: **history resurrection** (`:246-272`) keeps an older persistent
frame around so a surface revealed after long occlusion can recover detail rather than
restarting from scratch; and **weight clamping by pixel speed** (`:309-324`) shortens the
effective history for fast-moving pixels, where old samples are least trustworthy.

### Failure modes

| Failure | Cause | Defence |
|---|---|---|
| Ghosting / trails behind moving objects | history reprojected with camera-only vectors | per-object velocity (we have this) |
| Smearing at disocclusion | revealed pixels have no valid history | detect the miss; fall back to spatial AA, not to raw current |
| Blur in motion | history over-weighted when reprojection is unreliable | clamp effective history length by pixel speed |
| Thin geometry disappears or crawls | sub-pixel features never accumulate consistently | explicit thin-geometry detection; dilate velocity to the closest occluder |
| Fireflies burned into history | a single bright sample accumulated forever | measure flicker separately from rejection; clamp in a perceptual space |
| Loss of legitimate detail | a blunt neighbourhood clamp pulls valid history to the current frame's range | replace the clamp with a measured per-pixel rejection |
| Translucency/particles ghost | they write no velocity | translucent velocity pass, or composite after resolve |
| History poisoned by post effects | blurred or graded colour written into history | resolve on pre-tonemap HDR; keep blur/grade downstream |

### What X3Native already has

**`shaders/taa_resolve.frag` is a complete, competent TAA.** Present and verified:

- Halton(2,3) sub-pixel jitter folded into the projection, 8-frame cycle, deterministic phase
  (`vk_passes.cpp:1926-1955`).
- History ping-pong with an explicit `m_taaHistoryValid` invalidated on resize
  (`vk_passes.cpp:1955`), history copy at `vk_graph.cpp:1915`.
- **Per-object velocity reprojection** when available (`taa_resolve.frag:41`, gated by
  `params1.z`), with **camera-only reprojection as the fallback** — and a documented guarantee
  that `r_velocity 0` is byte-identical to the pre-velocity path.
- **YCoCg 3×3 neighbourhood clamp** — tighter on chroma than an RGB AABB, which suppresses
  colour-fringe ghosting (`taa_resolve.frag:55-60`).
- **Depth dilation** — reprojection uses the closest depth in the 3×3 neighbourhood, so thin
  silhouettes reproject with the foreground surface they belong to.
- **9-tap Catmull-Rom history resampling**, so repeated resampling does not progressively blur.
- Correct placement: on the **pre-tonemap HDR** target, after the last HDR writer, before
  auto-exposure/bloom/composite.
- `r_taa`, `r_taasharpen`; and a clean DLSS seam already documented with all four required
  inputs produced (`docs/VELOCITY_DLSS_REPORT.md`).

Squarely the Karis-2014-lineage TAA, done properly and honestly commented. The header even
states its references and that no game-engine source was consulted — keep that discipline.

### Honest remaining delta

**(a) No upscaling. This is the big one. Effort M, inputs present.**
The TAA is fixed-resolution: it renders at display resolution and antialiases. It does not
render at, say, 67% and resolve to 100%. Every input a temporal upscaler needs is already
produced — jitter, per-object velocity, depth, history, ping-pong — and the whole subsystem is
already gated and degradation-safe. Converting to TAAU means decoupling render extent from
display extent (a plumbing change through the graph and every fullscreen pass's texel
constants) and weighting accumulation by the sample-count ratio. **This buys 30–50% GPU
headroom**, which is what funds motion blur, froxel volumetrics, and higher RT quality. It is
the highest-leverage item in this document that is not free.

Note the sequencing: doing this *before* the DLSS seam in `VELOCITY_DLSS_REPORT.md` is
strictly better, because a working native TAAU is the fallback path DLSS needs anyway (for
non-RTX hardware and for the case where the licence-gated SDK never arrives).

**(b) The neighbourhood clamp is a blunt instrument. Effort M.**
A YCoCg AABB clamp is a good 2014 answer and a poor 2026 one. It cannot distinguish "history is
wrong" from "history contains detail the current frame under-sampled", so it discards
legitimate accumulated detail at exactly the high-frequency pixels where accumulation is most
valuable. The upgrade path, in increasing order of cost: measure a per-pixel rejection strength
instead of a hard clamp; add a spatial AA fallback for rejected pixels (otherwise rejection
trades ghosting for shimmer — this pairing is not optional); clamp effective history length by
pixel speed.

**(c) Velocity coverage is opaque-only. Effort S.**
Particles, debris, water and glass do not write velocity, so they reproject camera-only and
lean on the neighbourhood clamp. Cheap to characterise, cheap to fix for the cases that matter,
and it is a shared prerequisite with motion blur (§2.1) — do it once, two consumers benefit.

**(d) No thin-geometry handling, no history resurrection.** Effort L each. Real, and correctly
last: they matter most for foliage and wires at distance. Not worth touching before (a).

**Verdict: verify and deepen. The reprojection is sound; the missing capability is upscaling.**

---

# PART 3 — THE WIDER SURVEY, BY PIPELINE STAGE

Organised per constraint C1. **"Have" entries were verified in this tree.** Effort: **S** ≤2
days · **M** ≈1 week · **L** 2–4 weeks · **XL** >1 month. The **◆** marker means *inputs
already exist in the tree* — the high-leverage class per constraint C3.

### Stage 1 — Geometry, visibility, culling

| Modern AAA | X3Native today | Delta |
|---|---|---|
| GPU-driven bindless draw | **Have** — ObjectData SSBO + MDI, bindless | — |
| Two-phase HiZ occlusion cull | **Have** — `hzb_build.comp`, `cull.comp`, `r_hzb`/`r_cullpath` (`vk_graph.cpp:500-520`) | pyramid is one-frame-stale, half-res, single-polarity (`vk_pipelines.cpp:240-253`) — fine for cull, unusable for post tracing |
| PVS / portal culling | **Have** — `r_roomcull`, PVS→TLAS | — |
| Meshlet / mesh-shader pipeline | **Partial** — `meshlet.mesh`/`meshlet.task` present, Tier-2 not wired | **M** |
| Continuous mesh LOD | **Missing** for props/buildings/vehicles; terrain has LOD | **M–L** — the real draw-cost lever at long view distances |
| Impostors / billboard LOD for distant density | **Missing** | **M** |
| Compute skinning | **Have** — `skin.comp`, `r_skinnedrt` | — |

### Stage 2 — Shadowing

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Cascaded sun shadows, texel-snapped | **Have** — 4-cascade 2D array + blend band, `r_csm`, `r_csm_lambda/dist/blend`, `r_shadowsnap` (`mesh.frag:81-95`) | — (note `RACING_WORLD.md:642`'s "not implemented" is **stale**) |
| Ray-traced sun/area shadows | **Have** — `r_rtshadows`, `r_rtsun_size` | — |
| Local/point light shadows | **RT only** — `r_rtshadows 2`, `r_rtpoint_max`, `r_rtpoint_size`. **No cube/atlas shadow-map path** | **M** ◆ — on non-RT hardware every local light is shadowless. A cube/atlas fallback is the min-spec story |
| Virtual shadow maps | **Missing** | **XL** — do not attempt; CSM+RT covers the need |
| Screen-space contact shadows | **Missing** (planned in `RENDER_QUALITY_UPLIFT.md` P0.4, never built) | **S** ◆ — depth + sun dir already present; grounds thin geometry cheaply |
| Alpha-cutout shadow correctness | **Broken** — RT soft shadows (tier 2, default on ray-query HW) treat cutout as the full quad; `setShadowCutout` is powerless while tier > 0 because the shader takes `min(CSM, RT)` (`docs/KNOWN_BUGS.md` B16) | **S–M** — trees/crowds cast black rectangles. Real fix = any-hit / opacity-micromap alpha test in the ray query. Correctness bug, not a feature |

### Stage 3 — Direct lighting

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Clustered/froxel light assignment | **Have** — `r_clusterlights`, `engine/rhi/ClusterLights.cpp`, `shaders/inc/mesh_lighting.glsl:5-14`, froxel heatmap debug | — (`RACING_WORLD.md`'s "not implemented, 64-light cap" is **stale**) |
| Physical light units (lux/lumens) | **Missing** — intensities are authored numbers | **S–M** — the fix for per-scene brightness drift |
| Area lights (rect/tube/disc) | **Missing** — point/spot only | **M** — broad soft highlights instead of hard GGX dots; matters for interiors and neon |
| Clearcoat / sheen lobes | **Have** — `inc/mesh_reflections.glsl`, clearcoat in glass + mesh paths | — |
| Surface wetness / porosity | **Have** — `inc/mesh_wetness.glsl`, `r_wetness*` | — |
| Skin / subsurface scattering | **Missing** for characters (only a planet-ice LUT) | **M** — characters read waxy; matters for the rescue/companion content |

### Stage 4 — Indirect / GI

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Dynamic diffuse GI probes | **Have** — DDGI, `r_ddgi*`, probe volume + rays + update (`vk_graph.cpp:964-982`) | — |
| Screen-space GI | **Have** — 4-pass gather/temporal/denoise/apply, `r_ssgi` | — |
| Split-sum IBL + reflection probe | **Have** — `ibl_prefilter/irradiance/brdf_lut` | — (`RENDER_QUALITY_UPLIFT.md:184-188`'s "no cubemap infrastructure anywhere" is **stale**) |
| AO | **Have** — SSAO + blur, plus RT-AO (`r_rtao*`) | — |
| GTAO / bent normals / specular occlusion | **Missing** — SSAO is the older estimator, no bent normal | **M** ◆ — depth + normals present; halos and over-darkening are the symptom |
| SSGI reprojected with per-object velocity | **No** — `gi-temporal` uses its own prev-depth copy (`vk_graph.cpp:1538`), camera-only | **S** ◆ — **third unused consumer for `m_velImg`**; would fix SSGI's documented ghosting on moving objects |

### Stage 5 — Reflections

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Ray-traced reflections + denoise | **Have** — `refl.comp`, `refl_aux.comp`, `refl_denoise.comp` a-trous, `r_rtreflections`, `r_reflquality`, `r_refldenoise*` | — |
| Roughness-aware glossy reflection | **Have** — golden-angle disc widening with roughness, per-pixel rotation, radius tuned by A/B (`inc/mesh_reflections.glsl:1-40`) | — |
| Screen-space reflection fallback | **Missing** — `r_ssr` exists but selects the RT path; no SS march | **M** — matters only for non-RT hardware; the min-spec reflection story |
| Reflections without TAA | **Not possible** — hard-gated off when `r_taa 0` (`vk_graph.cpp:284`) | document it; it is a real constraint on any `--notaa` path |
| Known defect | intermittent black-frame on the drive path with RT reflection fallback, sidestepped with `--legacypost`/`--notaa`/`--norefl` (reported `docs/design/RACING_WORLD.md:640`; the line reference it gives into `host_drive.cpp` has since drifted — re-locate before fixing) | **S** — fix before shipping a racing mode on the default post stack |

### Stage 6 — Transparency and refraction

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Screen-space refraction via scene copy | **Have** — `glass-scenecopy` + blurred frost mip chain (`vk_graph.cpp:1200-1251`) | — |
| Water with its own pass | **Have** — `water.vert/frag`, tessellated snapped grid | — |
| Order-independent transparency | **Missing** — sorted blending only | **L** — accept the limitation; OIT is rarely worth it outside heavy foliage/smoke |
| Transparency writes velocity | **No** | **S** ◆ — shared prerequisite for §2.1 and §2.3(c) |
| Transparency samples volumetrics at its own depth | **No** — see §2.2(a) | **M** |

### Stage 7 — Volumetrics and atmosphere

| Modern AAA | X3Native today | Delta |
|---|---|---|
| Raymarched scattering, shadowed, HG phase | **Have** — `shaders/volumetric.frag`, sun shafts + light haze, `FogParams::volumetric` | — |
| Aerial perspective / height fog | **Have** — height falloff + sky blend (`IRenderDevice.h:382-389`) | — |
| Froxel volume sampled by all shading passes | **Missing** — screen-space only | **M** — §2.2(a) |
| Temporal reprojection of the volume | **Missing** | **S–M** ◆ — §2.2(b) |
| Authored fog volumes / media | **Missing** | **M–L** — §2.2(c) |
| Volumetric clouds | **Partial** — W-CLOUDS fBm deck + ground shadows; no in-scattering transport | **M** — already punchlist G3 |

### Stage 8 — Temporal

| Modern AAA | X3Native today | Delta |
|---|---|---|
| TAA with per-object velocity | **Have** — `taa_resolve.frag`, jitter, YCoCg clamp, depth dilation, Catmull-Rom | — |
| Temporal **upscaling** (render < display res) | **Missing** | **M** ◆ — §2.3(a). *The largest single performance lever available* |
| Measured per-pixel history rejection + spatial AA fallback | **Missing** — blunt neighbourhood clamp | **M** — §2.3(b) |
| Vendor upscaler (DLSS/FSR/XeSS) | **Seam ready, SDK absent** (`docs/VELOCITY_DLSS_REPORT.md`) | **S once SDK obtained** — but native TAAU first |
| Thin-geometry handling, history resurrection | **Missing** | **L** each |

### Stage 9 — Post and tonemap

| Modern AAA | X3Native today | Delta |
|---|---|---|
| HDR chain + ACES + filmic grade | **Have** — `composite.frag`, `r_tonemap`, `r_filmic`, split-tone, vignette, grain | — |
| Auto-exposure / eye adaptation | **Have** — `autoexposure.comp`, `r_autoexposure/aespeed/aemin/aemax/aekey` | — |
| Bloom | **Have** — down/up chain, `r_bloom*` | — |
| **Motion blur** | **MISSING — inputs fully present** | **S–M** ◆ — §2.1. **Top of the list** |
| Depth of field (lens, in-world) | **Missing** entirely; punchlist G9 is a *UI* gaussian only | **M** ◆ — depth present; the second lens effect after motion blur |
| Chromatic aberration | **Missing** | **S** — small; pairs with the filmic grade |
| Contrast-adaptive sharpen | **Missing** as a post pass (`r_taasharpen` is inside the resolve) | **S** — becomes near-mandatory once TAAU lands |
| 3D-LUT grade | **Missing** — parametric grade only | **S–M** |
| Local exposure / local tonemap | **Missing** | **M** |
| Dither before 8-bit output | **Missing** — verified: no dither/Bayer anywhere in `composite.frag` | **S** — banding on smooth gradients is the tell; worth a small default-on |

### Stage 10 — UI composite

| Modern AAA | X3Native today | Delta |
|---|---|---|
| UI composited after tonemap | **Have** — HUD inside composite (`vk_graph.cpp:2102`) | — |
| Blur/desaturate behind modal UI | **Missing** — punchlist G9 | **S** — becomes trivial once a real DoF/blur pass exists |
| UI excluded from velocity/TAA/blur | **Verify** — check the viewmodel too; a first-person weapon has enormous relative velocity | **S** — a correctness prerequisite for §2.1 |

---

## 4. Ranked deltas, by leverage per unit effort

**◆ = inputs already exist in the tree.** These outrank equal-impact items that need new
infrastructure, per constraint C3.

| # | Delta | Effort | ◆ | Why it ranks here |
|---|---|---|---|---|
| **1** | **Motion blur resolve pass** | **S–M** | **◆◆** | Full-res per-object velocity built, verified, and read by one shader. The slot in the graph is free and correctly placed. Directly satisfies the "speed sensation" ask (`VEHICLE_UPGRADES.md:130`) and the strongest film-vs-game tell |
| **2** | **Motion-domain verification rig** (deterministic N-frame camera-on-rails capture) | **S** | ◆ | Not a render feature — the *gate*. Without it no temporal claim in this tree can be honestly verified, and the blind spot recurs. Headless capture already exists; it needs a fixed-spline, fixed-timestep, N-frame mode |
| **3** | **Temporal upscaling (native TAAU)** | **M** | **◆** | All four inputs produced. Buys 30–50% GPU headroom, which funds every other item here. Strictly precedes the DLSS seam, and is the non-RTX fallback that seam will need anyway |
| **4** | **Velocity coverage: transparency, particles, water, viewmodel/UI masking** | **S** | **◆** | One change, three consumers (motion blur correctness, TAA ghosting, future volumetric reprojection). Prevents motion blur from shipping with sharp holes punched through it |
| **5** | **Volumetric temporal reprojection + froxel volume** | **S–M** then **M** | ◆ | Reprojection is cheap and buys the step budget; the froxel volume then makes fog correct on transparency, which is the one thing screen-space fog structurally cannot do |
| 6 | Screen-space contact shadows | S | ◆ | Grounds thin geometry indoors; depth + sun dir present |
| 7 | SSGI reprojection using `m_velImg` | S | ◆ | Fixes documented SSGI ghosting; fourth consumer of the same buffer |
| 8 | Alpha-cutout shadow fix | S | — | Correctness bug: foliage and crowds cast black rectangles |
| 9 | Depth of field (in-world, lens) | M | ◆ | Second camera-lens effect; subsumes punchlist G9 |
| 10 | GTAO + bent normals + specular occlusion | M | ◆ | Removes SSAO halos and over-darkening |
| 11 | Point/local shadow-map fallback for non-RT hardware | M | — | The min-spec lighting story |
| 12 | Mesh LOD for props/buildings/vehicles | M–L | — | The real draw-cost lever at long view distances |
| 13 | Area lights | M | — | Soft broad highlights; interiors and neon |
| 14 | Character subsurface scattering | M | — | Characters currently read waxy |
| 15 | Physical light units | S–M | — | Ends per-scene brightness drift at the source |

**Items 1–5 are the answer to "what should we do next."** Four of the five are ◆, which is the
point: the previous surveys' ranking would have placed none of them in the top four, because
its effort column was priced against a June engine and never re-derived.

---

## 5. Cross-reference against Tim's GTA VI plan

`docs/plans/PUNCHLIST_GTA6_MATCH.md` and `docs/plans/PLAN_GTA6_CAMPAIGN.md`.

### Where this survey agrees

- **The campaign's core thesis is right.** "We are not missing systems, we are missing
  integrity and density" (`PLAN_GTA6_CAMPAIGN.md:5-6`) is confirmed by §3: the stage-by-stage
  audit found very few *missing systems*. The engine has clustered lighting, CSM, DDGI, SSGI,
  RT reflections and shadows, volumetrics, IBL, auto-exposure, ACES+filmic, wetness. The
  campaign is diagnosing the right disease.
- **Phase 0 was the right first move.** The normal-map audit finding — *85 of 152 GLBs author
  zero normal maps* (`PLAN_GTA6_CAMPAIGN.md`, 0.4) — is a content-integrity result no renderer
  feature can compensate for, and it correctly outranks every item in §4.
- **G1/0.3 aerial perspective, G3 clouds, G5 foliage translucency, G6 decals, G7 grime, G8
  roofscape** are all genuine and are all in the density/integrity axis this survey does not
  duplicate. §3 does not re-file them.
- **The capture-review law is good discipline** for the axes it was designed for. The dB
  receipts on Phase 0 are real evidence.

### Where this survey finds something the punchlist missed

**Flagged explicitly, per the brief.**

1. **The GTA VI plan has no motion-domain axis at all.** Neither document mentions motion blur,
   shutter, per-object blur, temporal AA, or upscaling — not once. Its stated gap axes
   (`:10-11`) are "material/lighting integrity, content density per square meter, and reaction
   polish", all three of which are observable in a still frame. The reference material is a
   **2:21 video clip**, decomposed into numbered stills (f015, f045, f070) and gated frame by
   frame. A frame pulled from real gameplay footage already has the reference's motion blur
   baked in; under still-vs-still comparison that reads as softness or compression to be
   discounted, not as an effect to be matched. **The plan's single reference asset contains the
   evidence for the gap, and the plan's method discards it.**

2. **G9 "UI depth-of-field" is the shadow of a missing system.** It is scoped as a full-screen
   blur behind a menu, justified by "(their radio-wheel shot)" — i.e. wanted because it was
   visible in a still. There is no in-world depth of field, no focus distance, no aperture
   anywhere in the tree. The correct move is to build a real DoF pass (§3 stage 9) and let the
   UI case fall out of it, rather than building a one-off menu gaussian that will need
   replacing.

3. **The campaign's "what we have" inventory omits the entire post and temporal stack.**
   `PLAN_GTA6_CAMPAIGN.md:4-12` lists traffic, water, weather, day/night, weapons, vehicles,
   interiors, map, radio, missions, NPCs, destruction, RT — and no post-process or temporal
   effect. The inventory was taken at gameplay-system granularity, so post gaps were never
   enumerable. §1–§3 of this document are intended to be the missing half of that inventory.

4. **Two campaign-adjacent correctness bugs are unfiled**: the intermittent black frame with RT
   reflection fallback on the drive path (`host_drive.cpp:164-167`) and full-quad alpha-cutout
   shadow casting (`docs/KNOWN_BUGS.md:431`). Both are visible in stills and both will show in
   exactly the kind of capture the punchlist gates on.

5. **Stale "not implemented" claims exist in neighbouring docs** and will mislead anyone
   planning from them: `RACING_WORLD.md:642` says CSM and clustered lighting are not
   implemented (both are), and `RENDER_QUALITY_UPLIFT.md:184-188` says there is no cubemap/IBL
   infrastructure and no G-buffer (both false now). Worth a correction pass.

### Suggested placement

Items 1, 2 and 4 from §4 (motion blur, the motion rig, velocity coverage) belong as a
**Phase 1.8 "motion domain"** in `PLAN_GTA6_CAMPAIGN.md` — small, self-contained, and the one
axis the campaign has no coverage of. Item 3 (TAAU) is better treated as its own performance
lane, since its value is headroom rather than appearance.

---

## 6. What would have to be true for this survey to fail the same way

Stated so the next reader can check:

- If someone adds a feature without adding its resources to the §2 ledger, the single-consumer
  check stops working. **The ledger is the load-bearing part of this document, not §3.**
- If the effort column in §4 is quoted six months from now without re-deriving it, this
  document becomes exactly as misleading as the ones it replaces. **The effort column expires.**
  Re-price it whenever a ◆ prerequisite lands, because that is the event that invalidates it.
- If item 2 (the motion rig) is never built, every temporal claim in this tree remains
  unverifiable, and the blind spot is intact regardless of what gets implemented.
- If render findings continue to be filed in domain documents (`RACING_WORLD.md`,
  `VEHICLE_UPGRADES.md`) rather than routed here, correct findings will keep being orphaned.
  A periodic tree-wide grep for render vocabulary outside `docs/design/RENDER_*` is the cheap fix.
