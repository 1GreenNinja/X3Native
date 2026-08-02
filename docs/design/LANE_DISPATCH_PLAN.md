# Lane Dispatch Plan — parallel agent execution

INTERNAL. Written 2026-08-02. Governs how the remaining engine lanes are handed to agents
without them colliding.

## THE FIX: per-lane files (the race-free doctrine, applied to shaders)

Tim's call, 2026-08-02: rather than serialising lanes around a shared file, **give every lane
its own file** — the same doctrine the fleet already adopted for `FLEET-STATUS.md` (per-lane
sources + assemble, so no two agents ever edit the same file).

`shaders/mesh.frag` is the contention point. It is being split into a thin orchestrator plus
per-concern includes (`glslc` supports `#include` with `-I`):

```
shaders/mesh.frag                  orchestrator: layout decls, main(), calls the modules
shaders/inc/mesh_lighting.glsl     Lane 2 owns  (point-light loop, sun, ambient)
shaders/inc/mesh_shadows.glsl      Lane 3 owns  (shadow sampling, PCF, cascade select)
shaders/inc/mesh_reflections.glsl  Lane 1 owns  (sampleReflGlossy + both refl consumers)
shaders/inc/mesh_material.glsl     shared       (BRDF/GGX/Fresnel helpers)
```

**Lane 2 performs the split** (it owns `mesh.frag` at the time of writing), as a SEPARATE
commit that must be a **provable no-op** — reference capture before, identical capture after,
and both shader variants (`mesh.frag` and the `mesh_rt.frag.spv` define variant) still
compiling — before any lighting work begins. If the code proves too interdependent to
decompose without behaviour change, the correct outcome is a report explaining why, not a
forced split.

After the split, the serialisation below relaxes: Lane 3 can proceed against
`mesh_shadows.glsl` without waiting on Lane 2's lighting work.

`mesh.vert` and `vk_passes.cpp` have the same problem at smaller scale; apply the same
treatment if a second lane needs either.

## The hazard: shared files (pre-split; retained for reference)

Three lanes want the same two files. Running them in parallel produces merge conflicts in
the most delicate shader in the engine.

| File | Wanted by | Region |
|---|---|---|
| `shaders/mesh.frag` | **Lane 1** (done: `sampleReflGlossy` + both refl consumers), **Lane 2** (the 64-light loop, ~`:934/:978`), **Lane 3** (shadow sampling / cascade select) | different regions, same file |
| `shaders/mesh.vert` | **Lane 2** (light data), **Lane 5** (vertex format) | different regions, same file |
| `engine/rhi/vk/vk_passes.cpp` | **Lane 3** (shadow pass), **Lane 5** (draw/indirect path) | different regions, same file |

**Rule: one owner per file at a time.** A lane that needs a file another lane owns waits, or
branches from that lane's head once it lands.

## Wave 1 — dispatch NOW (provably disjoint)

### Lane 4 — City block/lot/frontage generator
* **Branch:** `inspx/city-blocks`, cut from `origin/echotropolis` (the city code is NOT on main).
* **Owns:** `app/world_hosts/echo_roads.{h,cpp}`, `echo_region_builders.cpp`, `echo_regions.cpp`.
* **Touches nothing** any other lane wants. Zero conflict risk.
* **Spec:** `docs/design/05-city-placement-review.md` equivalent — Tier 0 then Tier 1:
  `EchoRoads::sampleFrontage()` API; replace the polar hash rings with a frontage walk;
  planar face extraction -> `CityBlock`; block inset -> buildable polygon; recursive OBB lot
  subdivision preserving frontage; lot-driven placement (`yaw = frontYaw`, setback,
  footprint-fits-lot, footprint-corner terrain seating); then DELETE the four `corridorHits`
  veto sites. Position-derived seeds; district palettes replacing `%5`/`%8`.
* **Deliver:** before/after screenshots of the same city block, headless.

### Lane 5a — Vertex compression + mesh LOD (geometry)
* **Branch:** `inspx/geo-lod` from `main`.
* **Owns:** `engine/rhi/vk/vk_resources.cpp` (mesh creation), `app/` LOD selection.
* **DEFERS** the `vk_passes.cpp` draw-path change until Lane 3 lands (shared file).
* **Scope this wave:** discrete mesh LOD with screen-space-error selection + hysteresis;
  vertex format 32B -> 20B behind a format version so existing GLBs still load.
* **Deliver:** a headless test asserting LOD selection thresholds + a triangle-count readout
  before/after, plus screenshots at 3 distances proving no visible pop.

## Wave 2 — after Wave 1 lands (serialized on `mesh.frag`)

### Lane 2 — Clustered/tiled lighting  *(highest value; do FIRST in this wave)*
* **Branch:** `inspx/clustered-lights`, cut from **`inspx/rt-reflections`** (so it inherits
  the `mesh.frag` reflection work and there is nothing to merge later).
* **Owns:** `shaders/mesh.frag`, `glass.frag`, `mesh.vert`, the light SSBO in `engine/rhi`.
* **Scope:** froxel grid (e.g. 16x9x24, z-exponential), light assignment into a per-cluster
  index SSBO, shader iterates only its cluster. Raise the cap from 64 to 1024+.
  **Keep `r_clusterlights 0` = the legacy 64-light loop, bit-exact**, so existing md5 gates hold.
* **Justification (five independent asks in one day):** Echo Harbor neon night city; one
  tunnel's dressing alone would eat 48 of 64 slots; opulent elevator interiors; BL's building
  interiors were light-starved; car underglow in Lane 7.
* **Deliver:** a stress scene with 200+ lights, before/after screenshots, and a frame-time
  comparison at 64 vs 512 lights.

### Lane 3 — Cascaded shadow maps
* **Branch:** `inspx/csm`, cut from **Lane 2's head** (both edit `mesh.frag`).
* **Owns:** `shaders/mesh.frag` (shadow region), `engine/rhi/vk/vk_passes.cpp` (shadow pass),
  `VulkanRenderDevice_internal.h` (`kShadowDim`/`kShadowOrtho`/`kShadowDepthHalf`).
* **Scope:** 3-4 cascades, practical split scheme, one render per cascade into a 2D array or
  atlas quadrants, **stable texel snapping** (round light-space origin to the texel grid — this
  is what kills shadow-edge swimming), cascade select + blend band, per-cascade bias.
  **Keep `r_csm 0` = today's single cascade** for md5 gates. Preserve the `m_shadowOverride`
  scene-tuning contract.
* **Blocker note:** racing cannot ship without this — today's single 45 m camera-locked ortho
  box is swept past the car in 0.8 s at 200 km/h. **Interim one-liner available now:** bias the
  ortho box forward along the velocity vector.
* **Deliver:** screenshots at 3 distances showing shadows persisting past 45 m, and a
  camera-pan capture proving edges do not swim.

### Lane 5b — shared mega vertex/index buffer -> true multi-draw
* **Branch:** from Lane 3's head (needs `vk_passes.cpp`).
* **Scope:** one vertex/index arena with per-mesh offsets -> `vkCmdDrawIndexedIndirect(drawCount=N)`,
  which also makes the existing D15 GPU cull fully pay off.

## Standing requirements for EVERY lane

1. **Clean-room.** Techniques may be learned from public specs/papers and this repo's own
   passes. No GPL / id Tech / RBDOOM / UE source consulted. Per-file provenance headers per
   `docs/CLEANROOM_PROCESS.md`.
2. **A headless self-test**, registered as `--test-*` through `cli.h`/`cli.cpp`/`main.cpp`/
   `test_registry.{h,cpp}` like every existing one. Prove the test can FAIL (write the naive
   implementation as a negative control and show it fails) — this is what the terrain-corridor
   lane did and it is now the bar.
3. **SCREENSHOTS ARE PART OF VERIFICATION.** Compile + test + *image*. Use the engine's own
   headless capture (`DeviceDesc::headless`, `--screenshot`/`armCapture`). A/B pairs with the
   feature on and off, same camera. Save under `docs/screenshots/<lane>/`. **Then actually look
   at them and give an honest verdict** — "could not verify, here is why" beats a vague pass.
4. **No stray processes.** Always use a timeout; kill any lingering `X3Engine.exe`.
5. **A fallback cvar** so the previous behaviour is reachable and md5 gates stay bit-exact.
6. **Report honestly**, including what could not be verified and where the existing code fought
   the change.

## Known open items any lane may trip over

* `polyClosest` (river carve / canyon / ravines) uses the naive closest-point formulation and
  carries a medial-axis step wherever per-node values grade across a bend. Fixing it changes the
  canonical heightfield — needs a decision, out of scope for a lane.
* Native flipped Z for the mountains but kept city coordinates unflipped and byte-identical to
  BL. Breaks the first time a route is authored from the city toward a range.
* `--test-vehparts` P3: the tier-1 "street" build accelerates SLOWER than stock
  (640 vs 708 ticks to 25 m/s) while race is correctly fastest. Tires and the
  no-forced-induction path are ruled out; needs a profile of `compose()`'s curve/peakNm path.
* Nothing anywhere grants credits (`awardCredits()` does not exist), so the shop can only spend
  the seeded 12,000.
* Melee is fenced out of terrain worlds by a `!terrainWorld` guard in `app_run.cpp`, so the
  400% strength does nothing in the open world.
