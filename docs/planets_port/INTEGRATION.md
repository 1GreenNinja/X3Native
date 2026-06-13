# X3Native — PlanetSystem Integration Plan

How to land the FORGE3D planet ports in the live engine. Grounded in the actual
`engine/rhi/IRenderDevice.h` API + `app/mesh_prims.h` + `shaders/mesh.{vert,frag}`
conventions I read while consolidating these files.

## What the engine gives us (verified)

- **`x3::rhi::IRenderDevice`** (header, Vulkan hidden in `.cpp`):
  - `MeshHandle createMesh(const MeshVertex* v, uint32_t vc, const uint32_t* idx, uint32_t ic)`
    — POD verts `{float pos[3]; float normal[3]; float uv[2]}`, 32-bit indices.
  - `TextureHandle createTexture(const void* rgba8, w, h, bool srgb)` — tightly
    packed RGBA8; `srgb` selects sRGB vs UNORM storage.
  - `drawMesh / drawMeshEmissive / drawMeshPBR(fc, mesh, baseColor, normal,
    metalRough, baseColorFactor[4], emissive[4], model[16], alphaMask, alphaBlend,
    emissiveTex)` — the GPU-driven mesh path. `emissive[4] = {r,g,b,strength}` is a
    LINEAR HDR additive term independent of lighting (drives bloom).
  - GPU-driven design: per-object data is an SSBO row (`ObjectData`, **128-byte
    stride** — see `mesh.vert`), camera+lights in a UBO at `set=1,binding=1`,
    textures in a **bindless array** indexed by per-object `texIndex`
    (descriptor-indexing; `supportsDescriptorIndexing()`).
  - HDR pipeline with **ACES tonemap + bloom chain** (`setBloom`), SSAO/SSGI in
    the main pass, an **analytic sky** backdrop (`setSkyParams`, composites against
    depth), and an **additive/alpha transparent pass** for particles/decals
    (`submitParticles` ADDITIVE, `submitDecals` ALPHA) drawn after opaque + water +
    GI, before bloom.
- **`app/mesh_prims.h`** — `x3::prims` builds `MeshVertex` primitives (`makeCube`,
  `makeBox`, `makeGroundQuad`, `makeRamp`) + procedural RGBA8 generators. **No
  sphere generator yet** — we add one.
- **PNG loading**: `stbi_load` is already linked (used by `env_art.cpp` for GLB PBR
  maps; `mesh_prims.h` header notes the `stbi_load → createTexture` path).

## Step 0 — UV sphere primitive (`app/mesh_prims.h`)

Add `makeUVSphere(stacks, slices)` returning the existing `PrimMesh`:

```cpp
inline PrimMesh makeUVSphere(uint32_t stacks = 64, uint32_t slices = 128) {
    PrimMesh m;
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v  = (float)i / stacks;            // 0..1 pole->pole
        float phi = v * 3.14159265f;             // latitude
        for (uint32_t j = 0; j <= slices; ++j) {
            float u  = (float)j / slices;        // 0..1 longitude
            float th = u * 6.2831853f;
            float x = sinf(phi) * cosf(th);
            float y = cosf(phi);
            float z = sinf(phi) * sinf(th);
            m.verts.push_back({{x,y,z}, {x,y,z}, {u, v}}); // unit sphere: pos==normal
        }
    }
    uint32_t cols = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
      for (uint32_t j = 0; j < slices; ++j) {
        uint32_t a = i*cols + j, b = a + cols;
        m.index.insert(m.index.end(), {a, b, a+1, a+1, b, b+1}); // CCW front face
      }
    // (collision geometry optional for a sky-hung planet; fill cverts/cindex if needed)
    return m;
}
```
Plus a flat **annulus** `makeRing(innerR, outerR, segments)` on the XZ plane for
the ring disc (UV.x = normalized radius is not needed — the frag derives radialT
from object-space length; bake `pos`/`normal=(0,1,0)`/`uv`).

## Step 1 — Shaders into the build

1. Copy this folder's `planet.vert` + `planet_*.frag` into `shaders/`.
2. Copy `planet_common.glsl` into `shaders/` and add a **textual-prepend** step to
   the SPIR-V compile script (the engine has no `#include`): for each
   `planet_*.frag`, emit `#version 450` + extensions, then the body of
   `planet_common.glsl` (minus its own `#version`), then the frag body. (See
   ARCHITECTURE.md "Shared planet_common.glsl".)
3. **Bindless conversion**: replace each frag's explicit `sampler2D` bindings with
   the engine's bindless array + per-material `uint` indices carried in
   `PlanetParams`, exactly like `mesh.frag` samples `texArray[nonuniformEXT(idx)]`.
   Keep the reference `sampler2D` versions as the readable spec.

## Step 2 — Pipelines

Create planet pipeline-state objects mirroring the 4 blend classes
(ARCHITECTURE.md). The cheapest path that touches the LEAST renderer code:

- **Reuse the existing opaque mesh PSO** for the surface bodies (same depth/cull/
  blend) — just bind the planet frag + the `PlanetParams`/texture sets. This lets
  the FIRST target render with almost no new Vulkan plumbing.
- Add **one additive PSO** (blend `ONE/ONE`, ZWrite off, depth-test on) for
  Atmosphere + SunCorona. The renderer ALREADY has an additive particle pipeline
  (`VK_BLEND_FACTOR_ONE`...) — clone its blend state for a mesh PSO.
- Add **one alpha PSO** (`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`, ZWrite off) for the Ring
  (the alpha particle pipeline's blend state is the template).

## Step 3 — `app/planet_system.{h,cpp}` (new)

A small system owning meshes, textures, and per-planet param UBOs:

```cpp
struct PlanetTextures {            // bindless TextureHandles for one planet
    x3::rhi::TextureHandle maps[10]{};  // per-type slots (see TEXTURE_MANIFEST.md)
};
struct PlanetInstance {
    enum Type { Terrestrial, Oceanic, Sand, Gas, Ice, Lava, Moon,
                Thunderstorm, Sun } type;
    float center[3]; float radius;      // world placement
    PlanetTextures tex;
    PlanetParamsBlob params;            // CPU mirror of the type's PlanetParams UBO
    bool hasAtmosphere, hasRing, hasCorona;
    PlanetTextures atmoTex, ringTex, coronaTex;
};

class PlanetSystem {
public:
    void init(x3::rhi::IRenderDevice& dev);     // build sphere+ring meshes, load PNGs
    PlanetInstance& add(PlanetInstance::Type, const float center[3], float radius);
    void render(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fc,
                float timeSeconds, const float sunDir[3], const float sunColor[3]);
private:
    x3::rhi::MeshHandle m_sphere, m_ring;
    // per-instance: PlanetParams UBO (set 2), PlanetFrame UBO (set 0), bindless idx
};
```

**Texture load** (init): `stbi_load` each PNG from `assets/textures/planets/...`
→ ensure RGBA8 → `createTexture(px, w, h, srgb)` with the sRGB/linear choice from
TEXTURE_MANIFEST.md → store the `TextureHandle` (its bindless slot index goes into
`PlanetParams`). Copy the shared `Atmosphere/sunset_*`, `Misc/polegradient_*`,
`Atmosphere/Atmosphere_*` once.

**Per-frame `render`**: write `PlanetFrame` (uSunDir, uTime, uCamPos from the
camera, uLightColor, a small uAmbient), then for each instance build `model` =
translate(center) · scale(radius), and:
1. draw the **body** (opaque PSO + `planet_<type>.frag`),
2. draw the **ring** (alpha PSO) if present,
3. draw the **atmosphere shell** (additive PSO, scale ~1.05·radius) if present,
4. draw the **corona** (additive PSO) for Suns.

For a Sun/Lava/Thunderstorm body whose frag outputs HDR emission, you can ALSO
route it through `drawMeshEmissive` with a high `strength` so it feeds bloom even
before the dedicated PSO exists (a quick bring-up shortcut).

## Step 4 — Place a planet in a scene

Hang one in the **showroom / space backdrop**: in the scene that calls
`setSkyParams`, instead (or additionally) `PlanetSystem::add(Moon, {0,40,-120},
20.f)` and call `render()` each frame. Drive `setBloom` up a touch so the
atmosphere/corona glow reads. The planet composites against the existing depth +
sky like any mesh.

---

## RECOMMENDED FIRST TARGET: **Moon**

**Why Moon first (most payoff for least dependency + risk):**
- **Self-contained, all-real textures, no shared/derived art**: 4 maps
  (`moon_0N`, `_normal`, `_detail`, `_spec`) all live in `Moon/Textures/` as
  straightforward RGB PNGs. No clouds, no city lights, no land mask, no flow
  animation, no equirect panners — the fewest moving parts of any type.
- **STATIC** (no `uTime`): removes time-sync as a variable for first light. If it
  looks right with a fixed sun, the triplanar + PBR + scatter spine is proven.
- **Exercises the whole shared spine** in one shot: object-space triplanar,
  `unpackScaleNormal` + TBN, `standardSpecular`, and the `scatterTerm` LUT (using a
  shared `sunset_*` ramp). Everything `planet_common.glsl` must get right is on the
  critical path here — so it de-risks all 7 other surface types.
- **Highest confidence port** (porter conf 0.86) and **opaque** — it can reuse the
  existing opaque mesh PSO with ZERO new blend/pipeline state. Fastest possible
  bring-up.
- Visually a textured, lit, properly-shaded sphere hanging in the sky is an
  immediate, obvious win in the showroom.

## Ordered rollout (after Moon)

1. **Moon** — prove triplanar + PBR + scatter (opaque, static, own textures).
2. **Ice** — same spine + Linstep/Ramp3 elevation color (opaque, static, own
   textures). Validates `ramp3`/`linstep` and multi-map triplanar.
3. **Gas** — adds the lat-long **flow-distortion animation** + `uTime` (opaque,
   own 2:1 equirect textures, no triplanar/clouds). Cleanly isolates the
   time/flow path and HDR band color.
4. **Lava** — adds glowing **HDR emissive veins** + animated flow + the
   composite split (`termA + termB`). First real bloom-driver body; reuses Ice's
   normal map. Validates the additive emissive look.
5. **Atmosphere shell** — first **additive PSO** + shell mesh inflation. Pairs
   with ANY body above for the limb glow (uses shared `sunset_*`/`Atmosphere_*`).
6. **Terrestrial** — the big one: two-layer **clouds + self-shadow**, biomes,
   water, **city lights**, scatter. Validates `cloudsTwoLayer` + the cloud
   self-shadow + night-gate.
7. **Oceanic** — Terrestrial minus biomes/city (reuses Terrestrial maps). Nearly
   free once Terrestrial lands.
8. **Sandstorm (Sand)** — Terrestrial-shaped with dust clouds + detail blends
   (own `sandstorm_*`/`dustcloud*` art). Confirm the stub-material texture
   assignment first (TEXTURE_MANIFEST flag).
9. **Thunderstorm** — Terrestrial base + **emissive lightning** (triple-sine +
   floor-quantized strikes). Most complex animation; reuses Moon/Terrestrial maps.
10. **Sun + SunCorona** — emissive star body + the additive corona halo
    (second additive shell, Cull Off). The "hero" set; do last since it stacks
    the most layers and the corona's `_EdgeFade` params need authoring.
11. **Ring** — alpha PSO + annulus mesh; trivial shader, drop onto Gas/Saturn.
