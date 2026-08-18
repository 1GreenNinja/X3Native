# Outdoor polish — evidence (`fix/outdoor-polish`, 2026-08-17)

Tim, live at the city street / parking lot at dusk:

> "mountain is shimmering" · "Lights look fake in the parking lot."

Base: `fix/interior-shadows` @ `93ab9348` (carries the RT shadow-sampling work and
the `--test-grounding` restore).

---

## 1 — THE SHIMMER

Two suspects were filed. **Both are real, and they are different sizes.**

### (a) The legacy 45 m sun box was camera-locked and unsnapped — CONFIRMED, FIXED

`computeLightViewProj()` centred a 2x45 m ortho box on the camera position and
handed it straight to the matrix builder. The box therefore slid continuously
with the camera and the whole shadow map re-rasterized against a lattice that
moved a fraction of a texel every frame — the classic shadow swim. Cascades
(`r_csm 1`) had snapped since Lane 3; the one box the **legacy** path draws, which
is what every world without an explicit `applyOutdoorCsm()` renders, never did.

**Fix** — `csm::legacySnapCenter()` (`engine/rhi/Csm.cpp`), called from
`computeLightViewProj()` behind `r_shadowsnap` (default 1). It quantises the box
centre in the *world-anchored* light frame (`csm::lightRotation`, a pure rotation
about the origin) to whole shadow texels. `legacyOrthoViewProj()` itself is
untouched, so `--test-csm` C5's bit-exactness assertion still holds and
`r_shadowsnap 0` reproduces every historical capture.

All **three** light-space axes are snapped, not two. That was forced by this
lane's own C8 test: leaving Z continuous left the box sliding along the sun on
every sub-texel move, so "the box holds still" would have been true of the
lattice and false of the box.

**Measured** — `--screenshot-csm`, new `swim_*` bursts: 8 frames, the camera
stepped **0.0119 m = 27% of one shadow texel** per frame (texel = 90 m / 2048 =
0.0439 m). Consecutive frames diffed, luma > 6. TAA/SSAO/SSGI are off in that rig
already, which is what makes the reading mean anything.

Crop = the flat apron carrying a cast shadow, **no pillar geometry in frame**, so
a changed pixel can only be a moved shadow edge (123 500 px):

| burst | mean flip px / frame |
|---|---|
| `swim_csm0_snap0` — legacy box, snap OFF (the defect) | **95.6** |
| `swim_csm0_snap1` — legacy box, snap ON (the fix) | **26.3** — 3.6x fewer |
| `swim_csm1_snap1` — cascades (already snapped; control) | 53.0 |

Second crop (the mid-distance shadows, 45 000 px): 21.4 → 14.9.

Whole-frame numbers are *not* the measurement and are quoted only to be honest
about what they contain: 1141 → 1065. At 1.19 cm the near pillars' own silhouettes
move about a pixel, and with no TAA those hard edges dominate the count. That is
geometric edge aliasing, not shadow swim, and no shadow fix can move it.

### (b) Terrain aliasing at range — CONFIRMED, PARTLY FIXED

Anisotropy and mips were **not** the problem, and that is worth stating because it
was the first hypothesis: `createSampledTexture()` already builds a full mip chain
for every texture (normal maps included) and already takes the *device maximum*
anisotropy, not a hardcoded 8. Terrain samples through `textureGrad` with explicit
gradients, so mip and aniso selection are correct.

The aliasing is one level up. The mip chain filters what is **inside** each tap; it
does nothing for the **weights between** taps, and terrain computes those
analytically, per pixel:

* the stochastic hex-tiling lattice (`kStochLattice`) has a period of
  **~15.9 world metres**;
* the ragged-snowline fine octave has a period of **~28 m**;
* the tangent-space normal relief is a **sub-metre** bake.

At the range Tim is looking — the massif sits 7–8 km from the districts — one 720p
pixel spans **~7 m of mountain**. Every one of those terms is at or below the
footprint, so adjacent pixels land in different lattice triangles with different
random offsets, and a sub-pixel camera move re-rolls which. That is a per-frame
re-roll of a full-contrast 3-way blend: it sparkles, and no sampler state can
touch it.

**Fix** — `shaders/inc/mesh_terrain.glsl`, behind `r_terrainaa` (default 1):

* `stochBlend()` fades the 3-tap stochastic blend back to the plain, un-offset,
  lattice-independent tap as the footprint approaches the lattice cell. The
  premise of the technique is that the offset hides a visible 5.56 m repeat;
  once a pixel is wider than the repeat there is no repeat left to hide. Near
  field takes literally the old three taps (byte-identical); far field takes
  **one** (cheaper).
* `terrainDetailFade()` fades the sub-footprint splat-mask noise octaves.
* `terrainReliefAmount()` fades the normal relief to the geometry normal, and
  `terrainSurface()` converts the lost normal variance into roughness (specular
  anti-aliasing) so the energy is re-broadened rather than dropped.

**Measured** — `--screenshot-terrain`, new `_shimmer_aa{0,1}_*` bursts: 6 frames,
camera stepped **0.35 m ≈ 5% of one pixel** at the far vantage, **TAA/SSAO/SSGI
off** (all three are temporal or noise-seeded and each re-rolls per frame; with
them on the A/B read flat at 821.8 vs 856.2 flips — the instrument was measuring
three louder things).

| crop | r_terrainaa 0 | r_terrainaa 1 |
|---|---|---|
| ground just under the horizon, x100-1180 y350-390 (luma>3) | 38.6 | **27.0** — 30% fewer, max delta 16.8 → 12.3 |
| mid-field ground, x100-1180 y380-560 (luma>6) | 6468 | **5907** — 8.7% fewer |
| mid-field ground (luma>3) | 33 189 | **29 723** — 10.4% fewer |
| mountain band, x300-980 y280-350 (luma>6) | 332.0 | 325.0 — 2% |

**Honest residual.** On the mountain *band* the fix is worth ~2%, because that
crop is dominated by the massif's **silhouette against the sky** — a hard
geometric edge with a max delta of 184 (sky↔rock), which is edge aliasing that
TAA handles and terrain shading cannot. The material fix does real work (63 127 px
change by more than 6 luma between `aa0` and `aa1` at a fixed camera), and it does
most of it on the **ground plane**, where the win is 10–30%. It is a genuine
improvement, not a complete answer to "the mountain shimmers"; the remaining
share at the silhouette belongs to geometric AA, and the hard vertical LOD/region
seam visible at the right of `terrain_range.png` is InspectorX's documented bug,
deliberately not touched here.

The near `_range` vantage (~1.5 km, massif filling the frame) measures **16 246 vs
16 204** — no change, which is the correct answer: at that distance every detail
term is comfortably resolved and the fade is designed to do nothing.

---

## 2 — TIM'S CALIBRATION: sparkle or matte

The grazing sand sparkle may be **intended** ("hard baked sand shimmering in the
evening glow"). That is a *material* look, not aliasing — which is exactly why the
aliasing had to be fixed first: otherwise there is no way to tell a deliberate
glint from a broken one. `r_terrainaa 1` is ON in **both** frames below; only
`r_terrain_sparkle` differs.

Shot at the shore band under a **~9 degree evening sun**, looking into it (a
grazing glint only exists near the specular direction):

| | |
|---|---|
| `terrain_sparkle_on.png` | glossy hardpan — a full sun-glitter path across the sand, cooler and wetter-looking. **Currently the shipped default.** |
| `terrain_sparkle_matte.png` | dry loose sand — keeps its own tan, broad soft sheen, no glitter path. |

**Tim picks.** `r_terrain_sparkle 0..1` is a live console cvar; the two ends are
`kRoughSandM/kSpecSandM` and `kRoughSandS/kSpecSandS` in `mesh_terrain.glsl`.

### Per-band material is now authorable

Terrain used **one** flat dielectric roughness — 0.5 — for grass, cliff rock, snow
and sand alike (`mesh.frag`'s untextured path has a single constant and terrain
carries no MR map). `terrainSurface()` now blends an authored roughness *and*
specular per band with **exactly** the splat weights the albedo used, so a rock
face's colour can never arrive with the grass's gloss:

| band | roughness | specular (xF0) |
|---|---|---|
| grass | 0.88 | 0.55 |
| rock | 0.74 | 1.00 |
| snow | 0.52 | 1.25 |
| sand — matte | 0.82 | 0.45 |
| sand — sparkle | 0.30 | 1.55 |

`r_terrainmat 0` restores the flat 0.5 exactly. `r_terrain_rough` scales the
whole table.

---

## 3 — THE LIGHTS

`before_street.png` is Tim's complaint, and it is unmistakable: eight or ten
**solid white cones** with hard silhouettes standing on the grass, each with a
small bright coin of a pool at its base. Milk-carton traffic cones, not light.

Four causes, all fixed in `app/street_lights.cpp` (exterior fixtures only — the
interior/exterior dressing split from `fix/exterior-atmosphere` is respected):

1. **No source.** The luminaire was a dark housing *box* with an emissive term —
   a flat-shaded glowing rectangle. A small view-independent glow sphere now hangs
   at the aperture at 6.5 (work light 9.0), far above the composite's 1.10 bloom
   threshold, so the fixture **blooms** the way every light in a dusk photograph
   does. It is additive through the glass pass and flickers with the tube.
2. **Pool radius was physically wrong.** `poolR` was 2.0–2.2 m on a 4.9 m pole. A
   luminaire that high throws a pool 2–3x its mounting height across — that is why
   lamps are spaced ~30 m and still overlap. Now ~1.15x the mounting height
   (5.6–6.2 m; dock rig 9.5 m). The geometry said *spotlight* while the pooled
   point light (range 16 m) said *street lamp*; they now agree.
3. **The pool fell off linearly.** `pow(1-r, 2.2)` is a cone of light: slow near
   the centre, zero at a definite radius — a flat disc with a rim. Replaced with
   the real thing, `E/E0 = (1 + (d/h)^2)^(-3/2)` (point source at height h,
   cos^3 law), normalised to the rim and tapered over the outer 15% so the
   24-gon's own edge can never show.
4. **The cone had a hard silhouette and a hot throat.** The visible shaft is now
   decoupled from the pool (`kConeToPool = 0.55`) — the pool is lit by the whole
   reflector, the shaft you can *see* is the dense near-axis part, and tying them
   together forced a choice between a pencil beam and a 100-degree funnel. Rim
   power 1.8 → 2.15 so the outline dissolves; the axial gradient now ramps in over
   the first 12% so there is no hot ring at the housing, and still dissolves before
   the base — soft at both ends, no edge in between.

**And the visual now matches the real light.** `l.intensity` already carried a
deterministic ±15% per-lamp spread; the cone, head and pool took the flat zone
constants, so a lamp 15% down still glowed at full strength. `Lamp::emisMul`
applies the same scalar to all four, through the flicker machine and the grid-cut
re-strike.

**Read at Tim's vantage** (`--screenshot-city --set r_citylights 1 --set
r_clusterlights 1`, 263 clustered lights, 0 overflow):

* `before_street.png` → `after_street.png` — the solid cones are gone; the road
  surface is visibly lit, the fixture heads bloom, the mid-distance lamps read as
  glowing points in soft haze.
* `before.png` → `after.png` (aerial establishing) — hard white cone-dots become
  soft overlapping pools tracing the street grid.
* `before_scrapyard.png` / `after_scrapyard.png`.

Cost: GPU 0.446 → 0.446 ms at the street vantage (the extra glow sphere is ~80
triangles shared by every lamp; tris 125 304 → 156 792, draws 306 → 307).

**Honest score: 3/10 → 6.5/10.** Residual, and it is structural rather than a
tuning miss: the ground pool is still an *additive emissive disc* floating above
the asphalt, so from a 2.1 m eye height the near pool reads slightly hazy — light
*in front of* the road rather than *on* it. `discStr` was cut 0.16 → 0.10 to keep
that in check (pool area scales with the square of the radius, so carrying the old
strength onto a 2.8x wider disc multiplied its contribution ~8x and laid a milky
sheet over the near kerb — caught by reading the first pass). The real fix is a
surface-shaded decal or letting the point light carry the pool outright; filed as
follow-up, not attempted here.

---

## Files

* Shadows: `engine/rhi/Csm.{h,cpp}`, `engine/rhi/vk/vk_passes.cpp`,
  `engine/rhi/IRenderDevice.h`, `engine/rhi/VulkanRenderDevice.cpp`
* Terrain: `shaders/inc/mesh_terrain.glsl`, `shaders/mesh.frag`
* Lights: `app/street_lights.{h,cpp}`
* Cvars: `app/engine_console.cpp`, `app/app_run.cpp`,
  `app/world_hosts/world_host_common.h`
* Harness: `app/csm_test.cpp` (C8), `app/screenshot_hosts.cpp` (both bursts +
  the sparkle pair)
