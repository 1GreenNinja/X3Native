# FORGE3D Planets HD — Port Architecture (X3Native / Vulkan / GLSL 450)

Unified design for porting the FORGE3D "Planets HD" Unity/ASE shaders into the
X3Native native engine. All reference GLSL lives in this folder
(`docs/planets_port/`); integration into the live renderer is described in
`INTEGRATION.md`.

## Files

| File | Role |
|---|---|
| `planet_common.glsl` | Shared helper/noise library — ONE deduped copy. Triplanar, lat-long UV, fresnel, flow-map distortion, scatter LUT, two-layer clouds, GGX `standardSpecular`, polar coords, rotateUV, Ramp3/Linstep, and analytic value/gradient/simplex/voronoi/fbm fallback noise. |
| `planet.vert` | One shared UV-sphere vertex shader. Emits `vWorldPos, vWorldNormal, vObjPos, vUV (lat-long), vViewDir, vTBN`. |
| `planet_terrestrial.frag` | Earth-like: biomes + water + clouds + city lights + scatter. |
| `planet_oceanic.frag` | Water world: depth-tinted water + clouds + scatter. |
| `planet_sand.frag` | Sandstorm/desert: detail color blends + dust clouds + scatter. |
| `planet_gas.frag` | Gas giant: animated flow-distorted bands + scatter (lat-long panners). |
| `planet_ice.frag` | Ice: triplanar Linstep/Ramp3 ice color + subsurface scatter (static). |
| `planet_lava.frag` | Lava: rock ramp + glowing magma veins + animated flow (HDR emissive). |
| `planet_moon.frag` | Moon: triplanar albedo/detail/normal/spec + scatter (static). |
| `planet_thunderstorm.frag` | Storm world: flow clouds + emissive lightning flashes. |
| `planet_sun.frag` | Emissive star body (no scene light) + flow + heat gradient. ADDITIVE-capable HDR. |
| `planet_atmosphere.frag` | Fresnel glow SHELL. **ADDITIVE** (One/One), ZWrite Off, Cull Back. |
| `planet_suncorona.frag` | Corona halo SHELL/billboard. **ADDITIVE** (One/One), ZWrite Off, Cull Off. |
| `planet_ring.frag` | Ring disc. **ALPHA BLEND**, ZWrite Off, Cull Back. |

## Sphere mesh generation

A single UV-sphere primitive (radius 1, origin-centered) serves every body.
Stacks/slices ~ 64x128 for hero planets (smooth silhouette + dense triplanar
sampling). Per vertex (matches `x3::rhi::MeshVertex {pos[3]; normal[3]; uv[2]}`):

- `pos`    = unit sphere position (object space) — also the object-space normal dir.
- `normal` = `normalize(pos)`.
- `uv`     = lat-long: `u = atan2(z,x)/2π + 0.5`, `v = acos(y)/π`.

Build it in `app/mesh_prims.h` as `makeUVSphere(stacks, slices)` returning the
same `PrimMesh` struct. Cull a duplicate seam column or accept the lat-long seam
(triplanar is seam-free; only the cloud/scatter lat-long lookups touch `vUV`, and
those textures should wrap). The shell meshes (Atmosphere/Corona) reuse the SAME
sphere; the shell is enlarged either by a uniform model-matrix scale (~1.02–1.10)
or, faithfully, by inflating along the normal in `planet.vert` by
`uVertexOffset*0.1`.

The **Ring** is a separate flat annulus disc on the XZ object plane (inner radius
`uRingOffset+1`, outer `uRingSize`), origin-centered on the planet.

## Uniform / UBO layout

Three descriptor scopes, deliberately mirroring the engine's existing
set bindings so a planet can ride the GPU-driven mesh path or a dedicated pipeline:

- **set 0, binding 0 — `PlanetFrame` (UBO, per-frame, shared by all types):**
  ```glsl
  vec3 uSunDir;    float uTime;     // toward light; seconds
  vec3 uCamPos;    float _pf0;
  vec3 uLightColor;float _pf1;      // sun radiance (Unity _LightColor0)
  vec3 uAmbient;   float _pf2;      // flat GI fill (X3Native SSGI handles real GI in main pass)
  ```
- **set 1 — engine mesh path** (`Objects` SSBO @ b0, `Camera` UBO @ b1). Used only
  if the planet draws through the standard GPU-driven path; `planet.vert` reads
  `model` + `viewProj` + `camPos` from here. A dedicated pipeline can collapse
  this into a push-constant `model` + the `PlanetFrame` camera.
- **set 2, binding 0 — `PlanetParams` (UBO, per-planet instance):** the per-type
  knob block (colors, tilings, factors, speeds). Each `planet_<type>.frag`
  declares its own `PlanetParams`; sizes differ per type. One UBO per planet
  material instance (the FORGE pack ships 4–9 authored variants per type — each
  maps to one filled `PlanetParams`).
- **set 3 — textures** (the per-type maps). In X3Native these become **bindless
  texture indices** carried in `PlanetParams` (replace each `sampler2D` with a
  `uint` index into the global bindless array + `texture(uTex[nonuniformEXT(idx)],
  ...)`), exactly like `mesh.frag`'s `vTexIndex`. The reference frags use explicit
  `sampler2D` bindings for clarity; swap during integration.

All `PlanetParams` blocks are `std140`-friendly (vec4-padded). Author the C++
mirror structs with matching padding (see `INTEGRATION.md`).

## Shared `planet_common.glsl`

Every per-type frag begins with `// #include "planet_common.glsl"`. The X3Native
shader build has no real `#include`, so the asset/compile step does ONE of:

1. **Textual prepend (recommended):** a tiny step in the SPIR-V compile script
   concatenates `planet_common.glsl` (minus its `#version`) after each frag's
   `#version`/extension lines. Cleanest; no glslang include path needed.
2. **glslang include:** add `#extension GL_GOOGLE_include_directive : require`
   and an `-I docs/planets_port` include path, then `#include`.

The library is the dedup spine: the **scatter LUT**, **two-layer clouds**, the
**GGX `standardSpecular`** stand-in, **object-space triplanar**, **flow-map
distortion**, **polar coords / rotateUV**, and **Ramp3/Linstep** appear once and
are called by 3–9 shaders each. (Analytic Simplex/Voronoi/fbm are a *fallback* —
the faithful FORGE ports are entirely texture-driven; no port calls them by
default.)

## Uber-shader vs separate pipelines

**Recommendation: separate pipelines per type, ONE shared `planet.vert`.**

Rationale:
- The 12 types diverge hard: blend state alone splits into 3 classes (opaque
  surface, additive shell, alpha disc), plus emissive-only (Sun) and lightning
  (Thunderstorm). A type-uniform uber-shader would carry every texture binding +
  every branch on every pixel — wasteful and a descriptor-layout headache.
- They naturally cluster, so it's not 12 unrelated pipelines:
  - **Surface family** (Terrestrial / Oceanic / Sand share clouds+scatter+water;
    Gas / Ice / Lava / Moon / Thunderstorm share triplanar+scatter). All opaque,
    same blend/depth state, same `planet.vert`, same `PlanetFrame`. These *could*
    fold into one "surface uber-frag" gated by a `uPlanetType` push constant if
    you want fewer PSOs — but the texture-set differences make per-type frags
    cleaner. Start per-type; merge later if PSO count bites.
  - **Additive shells** (Atmosphere, SunCorona): one pipeline state object
    (One/One, ZWrite off), two frags.
  - **Sun body**: opaque/emissive, its own PSO.
  - **Ring**: alpha-blend PSO.

So: **~4 pipeline-state classes** (opaque surface, additive shell, alpha, emissive
body), each potentially with a couple of frag variants, all sharing `planet.vert`
and the `PlanetFrame` UBO.

## Layer composite over a planet

A full "hero planet" is a STACK of draws, back-to-front for the transparent
layers, all sharing the same world center + `PlanetFrame`:

1. **Body** (opaque): `planet_<type>.frag` on the unit sphere. Writes depth.
   Lava/Thunderstorm/Sun output HDR-emissive that feeds bloom.
2. **Ring back half** (alpha, optional): draw the ring disc; with Cull Back +
   ZWrite Off the far half composites behind the planet first if you split it, or
   draw the whole disc once depth-tested (the body occludes the part behind it).
3. **Atmosphere shell** (additive): a sphere ~2–10% larger (`uVertexOffset`),
   Cull Back, ZWrite Off, blend One/One — a fresnel limb glow over the body.
4. **Ring front half** (alpha): the part in front of the planet.
5. **SunCorona** (additive, Sun only): a large back-shell/billboard around the
   star, Cull Off, ZWrite Off, One/One — the streaming HDR storm halo.

In X3Native these transparent layers slot into the **same HDR transparent pass as
particles/decals** (after opaque + water + GI, before bloom — see
`IRenderDevice::submitParticles`/`submitDecals` ordering), so the additive
atmosphere/corona naturally drive the bloom chain and the ACES tonemap. Depth is
TESTED (planet body occludes the far shell hemisphere) but NOT written by the
shells.

## HDR / tonemap

The engine renders to a linear HDR target with ACES tonemap + a bloom chain
(`mesh.frag`/composite). The FORGE intensities are intentionally huge
(`_MagmaGlow≈1111`, `_SolarStormPower≈311`, atmosphere `≈85–140`) — they are
DESIGNED to clip into bloom. Keep planets in the HDR float target and let the
existing bloom/tonemap consume them; do not pre-clamp the emissive terms.
