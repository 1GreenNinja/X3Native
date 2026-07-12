# SEAM 3 — Outdoor lanes as streamed regions of the canon master world

*Recon 2026-07-09 (session 392f6e4d). Tim's world-merge order: 2 → 3 → 4 (1 = the
vertical spine, done). This is the work order for wiring `WorldStreamer` into the
`canonWorld` path so walking off the facility apron streams in the planet.*

## The streamer API (app/world_stream.{h,cpp})

- `WorldRegionDesc` (h:63): `id, name, builder("city"|"oceanbase"|"worldregions"|""),
  levelDoc, floor, anchor[3], radius, loadRadius(300), unloadRadius(450), neighbors[]`.
  **Anchor/radius are residency math only — builders place content at their own
  absolute coords.**
- `WorldRegionGraph::load(worldRegionsJsonPath())` → `assets/world/regions.json`
  (format `x3.regions/1`; validates hysteresis + neighbor ids).
- `WorldStreamer`: `init(graph, jobs)` → `buildStartRegions(scene,dev,phys, x,y,z)`
  (synchronously realizes regions containing the point) → per-frame
  `update(scene,dev,phys, px,py,pz, vx,vy,vz, budgetMs, alreadySpentMs)`
  (**XZ-only — py/vy ignored**; velocity lookahead, hysteresis, neighbor warm,
  ≤1 realize/frame nearest-first, chunked evict) → `shutdown()`.
  Proxy floor: invisible collision quad at anchor **Y=0** when standing in a
  non-resident wanted footprint (cpp:310).
- `realize()` (cpp:217) dispatches builders inside `beginUploadBatch` +
  `Scene::beginEntityCapture` (ownership ledger; shared `SurfaceLibrary m_surflib`
  excluded from teardown).

## The lanes (absolute coords; regions.json must merely enclose them)

| Lane | Entry | Content at | Y |
|---|---|---|---|
| city | `City::build` (city.h:64; coords city.cpp:45-56) | Scrapyard (-600,500) r250, New District (200,500) r190, Industrial (-200,350) r150, freeway tunnels, road grid | terrain (~Y0) |
| oceanbase | `OceanBase::build` (ocean_base.h:60) | disc (1100,-1350) r80 | absolute deep Y |
| worldregions | `WorldRegions::build` (world_regions.h:65; table world_regions.cpp:37-44) | Crash Site **(0,0) r30** ⚠, East Outpost (800,400), West Outpost (-880,-320), 4 mountain ranges 8-9 km | terrain surface |
| spire_f1 (leveldoc) | buildCanonFloor via realize | the tower JSON floor 1 (x0..50, z-40..40) | Y0 |

Shipping `assets/world/regions.json`: spire_f1 anchor [22,0,10] r70 · city
[-200,0,425] r750 · ocean_base [1100,-40,-1350] r230 · surface_landmarks
[0,0,0] r9850 (neighbors all).

Authority for the map: `docs/design/X3_WORLD_BLUEPRINT.md` §1 (15 km Keth'zar;
native lanes already match the gazetteer in XZ; native surface = Y0 where the
blueprint says -300 — Q3Engine convention).

## Steps (target: app_run.cpp canonWorld branch)

1. Streamer members in canon scope; wiring pattern verbatim from
   `world_hosts/host_streamed.cpp:54-72`.
2. **Canon region set**: drop `spire_f1` (canonWorld already builds the tower —
   never double-build); keep city / ocean_base / surface_landmarks. New
   `assets/world/regions.canon.json` or a filter.
3. Init `TerrainStreamer` for canon (worldTerrainConfig, centered on tower,
   radius ~8) — the canon interior path may not init terrain today; verify.
4. Boot: `buildStartRegions` at tower center (~25,0) — with spire_f1 removed only
   surface_landmarks contains it.
5. Per-frame umbrella: `wstream.update` (terrain, measured) then `wsm.update(...,
   wsBudgetMs, terrainMs)` — copy host_streamed.cpp:410-417. Feed camera + velocity.
6. Draw gating vs canon PVS: streamed entities carry no room id — bypass the room
   filter only when the player is outdoors / near the apron; keep streamed physics
   out of interior queries (city places bodies; worldregions places none).
7. Horizon stitch: `HorizonRingDesc` + `setCameraFar(15000)`
   (host_streamed.cpp:276-283).
8. Teardown before canon shutdown.

## The 3 risks

1. **Crash Site at (0,0) r30 spawns inside the tower/apron.** Relocate the prop in
   world_regions.cpp:37 (clear of the tower + facade footprint) or add a keep-out.
2. **PVS/room-cull vs streamed entities** — wrongly culled indoors or wrongly
   always-drawn; gate as an "exterior PVS zone" tied to the apron/lobby exit.
3. **XZ-only residency vs the underground** (Club 1127 Y=-200): suppress wants (or
   hold-resident w/ proxy disabled) while the player is below ~Y=-20 — the proxy
   floor would otherwise drop an invisible plane at Y=0 above the club. Preserve
   XZ separation for ocean_base (no Y culling exists). Measure the light budget
   with apron+city resident (city is mostly emissive materials, not lights).

## Gates

- `--test-worldstream` (W0-W7: parse, resident-before-arrival, unload-behind,
  hysteresis, budget, leak-free tour, teleport proxy, city ≥200 entities) — extend
  or add a canon-graph variant when spire_f1 is dropped.
- `--test-streaming` (terrain tile ring only), `--test-city`, `--test-oceanbase`,
  `--test-worldregions` (re-run after any coordinate relocation).
- New canon-hosted assertion: boot canonWorld headless, walk off the apron, assert
  city/surface_landmarks go Resident and no streamed body intersects the interior.
- CLI knobs: `--ws-budget <ms>`, `--ws-lookahead <s>` → HostContext.
