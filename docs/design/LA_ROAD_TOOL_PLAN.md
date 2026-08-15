# Plan — draw roads like SimCity, top-down, in Level Architect

Status: DRAFT v2, pre-execution. Written before code, per the discipline.
Author: InspectorX, 2026-08-15. Sharpened: Fable, 2026-08-15 (pass 3 —
verified against `editor_host.cpp`, `editor.cpp`, `terrain.cpp`,
`leveldoc_world.cpp`, `app_run.cpp`, `world_map.cpp`, `echo_roads.cpp`,
`city.cpp`, `tunnel_corridor.cpp` — not against this plan's own summary of
them).

> "Give me a tool that I can draw roads like simcity, in Level Architect, from
> top down view!" — Tim
> "Show terrain and mountains in LA" — Tim
> "Drag cities from the prefabs possibly?" — Tim
> "Load levels prior to corridor register." — Tim (architecture ruling,
> 2026-08-15; see T5)

UNITS: feet and miles in anything Tim reads. Engine data stays SI; where a
CODE CONSTANT is quoted it is quoted in metres with feet alongside.

## Sharpening pass (2026-08-15) — what changed and why

Every claim below was checked in source. Twelve findings, several of which
are corrections of things v1 stated as fact:

1. **The pipeline (A5) is real — the boot order ALREADY works, per Tim's
   ruling.** v1 feared corridors-register-at-boot vs doc-loads-after-boot.
   Verified false: in `--world fromdoc` the doc loads at `app_run.cpp:2942`,
   the corridor registration site is `:3225` (its own comment states the
   register-before-first-height-query contract), and the first real height
   query is the spawn probe at `:3243`. The doc is in memory 283 lines before
   corridors register. A5 stands as a hard gate. What's genuinely missing is
   only: the `roads[]` schema (T3), a `registerDocRoads()` walk at ~`:3225`
   (T5), and terrain itself in the fromdoc world (fromdoc currently builds NO
   terrain — the block at `:3204` is gated to `--world terrain|ocean`).
   Corollaries stated out loud: the registry is READ-ONLY after boot, so doc
   HOT-RELOAD re-places props but can never re-carve — road/corridor changes
   need a restart, and the in-editor carve preview (finding 3) is the
   iteration loop in between. T5 also decides the parse-early shape this
   implies for the future.
2. **v1 proposed building a viewport the game partly ships.** `app/world_map.*`
   is a complete 2D map compositor: `MapCamera` with cursor-anchored wheel
   zoom, drag-pan, and `pxToWorld`/`worldToPx` (`world_map.cpp:25-100`), a
   tile bake→`createTexture`→`drawHudImage`→destroy lifecycle (`:446-511`),
   and invalidation for hot reload. What it lacks is only a TERRAIN tile
   baker (its tiles come from canon floor docs and scene entities). T1+T2
   collapse to: one `bakeTerrainTilePixels()` + reuse of that machinery.
3. **v1's "top-down orthographic camera" was the wrong mechanism — replaced.**
   The RHI has NO ortho path: one projection site (`vk_passes.cpp:1845-1847`,
   `glm::perspective` only), and changing it entangles TAA reprojection,
   cluster-light froxels, GPU cull/HZB. Worse, there is NO cursor→world ray
   anywhere (`worldToScreen` exists; its inverse does not; editor picking is
   crosshair-only, `editor_host.cpp:1650-1657`). The plan view is therefore a
   **2D map mode** (world_map pattern): `pxToWorld` IS the picking primitive,
   exact and free. The 3D perspective viewport stays for walking/driving the
   result; it needs no changes.
4. **"Four mountain ranges" was stale — there are FIVE.** `terrain.cpp:523-536`,
   `kRangeCount = 5`: N snow, E volcanic, S mesa, W crystal hills, plus the
   small TUNNEL RIDGE (spine (-753,-740)→(-431,36), ~935 ft amp) added for
   the freeway bore. The feature field is ~14 miles across (range influence
   to ±11 km), horizon disc ~16 mi — not v1's "11 x 11". A2 rewritten.
5. **Raster cost is a solved question, with numbers.** `terrainHeightAtWorld`
   costs ~50-90 ns in open country, ~150-500 ns inside jagged ranges /
   river / corridor boxes → ~5M samples/s/core mixed. A 1024² tile ≈ 1M
   samples ≈ 0.2-0.3 s on one core, ~20-40 ms on the job system. Derive
   hillshade normals from raster neighbours (1 eval/texel), never via the
   5-eval vertex path. Reusing the game's terrain MESHER at coarse LOD is
   REJECTED: it yields 3D geometry, not a readable plan, at 5x the sampling
   cost, and needs a terrain world loaded — the pure sampler needs nothing.
6. **The editor already has undo — for brushes only, and the entity gap is a
   warning.** `EditorState` carries a full snapshot command stack with
   grouping (`editor.h:455-561`) and the host has one re-sync funnel
   (`applyEffect`, `editor_host.cpp:842-877`). But entities (models, portals)
   have NO undo at all (portal delete is un-undoable). Roads get real
   history from day one (T4), mirroring `BrushCmd`, not the entity precedent.
7. **The junction model changed: shared node pool, not link ids.** v1
   proposed per-road nodes carrying string link ids (the portal-pairing
   trick). Checked against `echo_roads.h`: `RoadGraph` does split
   `nodes[]`/`edges[]`, but its nodes are degenerate `{x,z}` never welded —
   connection is POSITIONAL, by proximity snap (`echo_roads.h:112-114`) — a
   warning, not a model. Duplicated coordinates paired by string drift the
   moment one is moved. T3 uses one top-level `roadNodes[]` pool with
   integer ids; a junction is simply a node referenced by ≥ 2 roads. The
   portal link-id contract is kept for what it is good at: tunnel MOUTHS.
8. **"The consumer side is already done" was half true.** `TunnelRoute`'s
   frame does follow the station polyline (`posAt/worldAt/tangentAt/
   segmentAt`, gated by `--test-routeframe`) — but the SEED is still
   straight-line-only: `TunnelSpec` is (centre, heading, halfLen) and PASS 1
   lays `origin + dir*s` (`tunnel_corridor.cpp:517-523`). Its own comment
   invites the fix ("a future curved seed writes x/z here and everything
   downstream follows"). A drawn bore needs that seed change + the
   32-station cap respected (`kRouteNodes = TerrainCorridor::kMaxNodes`).
9. **Budget 1 corridor per drawn tunnel, not 3.** terrain.h's "a dressed
   tunnel now registers up to THREE corridors" is STALE:
   `registerTunnelCorridorFor` calls `registerTerrainCorridor` exactly once
   (`tunnel_corridor.cpp:865`); `registerTerrainPortalHole` has ZERO
   producers in the whole repo; the boot log itself prints "CUT-AND-COVER —
   1 corridor". Two stale artifacts will mislead an implementer (the
   `city.cpp:559-567` plug comment; the `holeCount == 2` check in the tunnel
   self-test). The REAL ceiling is `kMaxTerrainCorridors = 16` (→ 192 in
   road-plan P0) at 32 nodes each — ~6.6 mi of carve today.
10. **A real bug found while verifying, upstream of this tool:**
    `TunnelRoute::cx/cz/halfLen` are declared (`tunnel_corridor.h:197-198`)
    but never assigned — `deriveRoute` and `registerTunnelCorridorFor` both
    skip them, so every drive-test survey walks from the world origin and
    the city C3b "routes are distinct" check compares 0 to 0. One-line fix,
    but it changes test outcomes — land it deliberately, BEFORE doc-driven
    multi-routes rely on those fields.
11. **T5 prefab cities re-scoped — the city generator cannot be dragged.**
    `City::build()` takes no placement arguments at all; every coordinate is
    a literal; the districts assume terrain.cpp's authored flat `kPads`; and
    `registerCityFreewayTunnels()` is a hard singleton (static `done` latch,
    fixed 4-entry table). "A named generator call with a transform + seed"
    is the right ABSTRACTION but the wrong first TARGET: v1 aims it at the
    road plan's TOWN generator (being born position-independent), rides the
    existing `entities[]` format (type "settlement"), and reuses the
    palette's documented drag-drop machinery. Relocating the big city is a
    parameterization refactor + data-driven pads — out of scope here.
12. **Grade is derived, not drawn — and the numbers come from the road plan,
    not "~8%".** The route layer grades the datum itself (grade-clamp
    sweeps; tunnel 4.5% today, echo freeway deliberately 22%; echo has NO
    vertical-grade validation at all). The tool's job is the XZ route, datum
    LOCKS at authored points, and live VALIDATION against the road plan's
    G3 profiles: Interstate ≤ 6%, Alpine 6% mainline / 8% sustained climb,
    ≤ 4% inside curves tighter than 800 ft. The profile derivation must be
    ONE pure function called by both the editor preview and the boot build —
    otherwise the preview lies (A8).

## Verified ground truth

* `LevelDoc` = entities[] + brushes[]. No polyline type. The parser
  (`editor.cpp:293-436`) is a focused recursive-descent subset with a FIXED
  `skipValue` — unknown keys of any shape are tolerated, so a new `roads[]`
  block is forward-compatible with older binaries, and adding it is
  mechanically the brushes[] loop again (~200-300 lines + tests). `%.9g` +
  strtod round-trips floats bit-exactly (A4 stands). `loadJson` (parse-only)
  and `LevelDocWorld::buildFromDoc` (build-from-parsed) already exist as
  separate APIs — T5 leans on that split.
* The editor draws no terrain and links none — but `terrain.cpp` and
  `editor_host.cpp` are the same `x3app` target; `#include "../terrain.h"`
  just works. The pure samplers (`terrainHeightAtWorld`, `worldWaterLevelAt`,
  `terrainCorridorDepthAt`) need no world, no GPU, no registry — the plan
  view works in ANY loaded world, including none.
* `terrainCorridorDepthAt(corridor, x, z)` is pure and registry-free — the
  editor can preview the exact carve the game will make, on bare stack
  corridors, without touching the boot registry. The keystone of the preview
  story.
* Textures: `createTexture`/`destroyTexture` only (no update); no
  ImGui::Image/ImTextureID bridge exists. `drawHudImage` + the world_map
  tile lifecycle is the shipped path. HUD image draws must issue from the
  render hook (`renderModels`-style), not from the ImGui-only `draw()`.
* Drag/undo pattern to copy exactly: gizmo grab→drag→release with
  `beginBrushEdit`/`commitBrushEdit` (one drag = one undo step,
  `editor_host.cpp:1453-1661`) and `beginGroup`/`endGroup` for bursts.
* `X3LevelArchitect.exe` is a pure launcher — no editor-specific host path
  exists or may exist (D3 of LEVEL_ARCHITECT_EXE_PLAN). Everything lands in
  `EditorHost` behind the data-driven menu table + `dispatchCmd`, reachable
  from both exes and the in-game pause-menu editor.
* Reusable downstream, verified: `echo_roads.cpp` PHASE 1.9-3 is an
  island-agnostic "polylines in, roads out" compiler — arc-length resample,
  curvature→banking, the zigzag law (per-class heading-change/metre limits
  with escalating cures), junction detection + patches, ribbon/paint/
  barrier/pier emitters, collision export — already proven on SYNTHETIC
  graphs by `--test-cityblocks`. The island lives entirely in PHASE 1
  (:582-1350); the extraction seam is real but mechanical (`Pending` is
  function-local).

## The pieces, in dependency order

### T1 — THE PLAN VIEW (2D map mode in the editor)

A new editor mode (`Cmd::ViewRoadPlan`, View menu + hotkey 4) that swaps the
central viewport for a 2D map composited exactly like the world map:
`MapCamera` for pan/zoom/px↔world, tiles drawn via `drawHudImage`, road
overlay drawn as ImGui foreground polylines (redrawn per frame — no texture
rebake on drag). Scale readout in FEET and MILES; an on-screen ruler. Zoom
span: the whole 16-mile disc down to ~200 ft across.

Navigating 62 miles: cursor-anchored zoom out/in IS the primary move (the
GTA-map gesture — out to overview, back in anywhere, two wheel flicks);
plus F frames the selection (the existing `Cmd::Focus`), and up to 10
named BOOKMARKS (Ctrl+1..0 set / 1..0 jump, serialized in the doc so the
network's authoring viewpoints travel with it). No minimap — the mode IS
the map.

Mode wiring points (from the host survey): 4th entry in the camera-mode
arrays, a branch in the Status panel readout, menu-table row + `dispatchCmd`
case (the table rows must have behavior behind them — the known failure
mode). Wheel arbitration follows the existing Orbit-mode pattern.

### T2 — TERRAIN TILES (the ground T1 draws)

`bakeTerrainTilePixels(rect, res)` sampling PURE `terrainHeightAtWorld` +
`worldWaterLevelAt` per texel: hypsometric tint, 100 ft contour bands
(500 ft majors), hillshade from raster neighbours, water in blue, the 4 city
pads and 5 ranges labelled from their authored tables. Tile cache keyed
(zoomLevel, tx, tz), LRU ~24 tiles at 1024² (4 MB each, ~100 MB ceiling),
missing tiles baked on the job system while the coarse overview tile shows
underneath (slippy-map style). Budget: 1024² ≈ 0.2-0.3 s/core, ~20-40 ms
threaded — pan is hitch-free by construction.

CARVE PREVIEW: a toggle. When on, tiles intersecting any drawn road's
corridor bbox add `terrainCorridorDepthAt` from doc-DERIVED corridors (bare
structs, no registry) into the bake — the same arithmetic the game runs at
boot. Rebake debounced ~250 ms after drag release (no per-frame texture
churn; the RHI has no updateTexture).

### T3 — THE SCHEMA (roads in the LevelDoc)

Two new top-level arrays, emitted/parsed exactly like brushes[]:

```
"roadNodes": [
  { "id": 7, "x": -592.0, "z": -352.0,        // metres, %.9g
    "r": 457.2,                                // optional fillet radius (m)
    "yLock": 12.5 }                            // optional datum lock (m)
],
"roads": [
  { "name": "inner_tour", "class": "interstate",   // interstate|alpine|ramp|avenue
    "nodes": [3, 7, 12, 9],                        // ordered ids into roadNodes
    "reaches": [ { "a": 1, "b": 2, "kind": "tunnel" } ],  // node-index spans
    "gen": "", "genEdited": 0 }
]
```

* A JUNCTION is a node id referenced by ≥ 2 roads. No link strings, no
  duplicated coordinates, nothing to drift.
* Authored nodes are SPARSE (bends, junctions, locks — hundreds for 62 mi).
  The 68 ft station chain, grading, banking and corridor chunks are DERIVED,
  deterministic, and never serialized (road plan D1 checksums the
  derivation).
* `class` selects width, default fillet radius, grade profile and zigzag
  limit from ONE table shared by tool and builder.
* Tunnel reaches additionally EMIT two portal entities (the existing
  LEVELDOC PORTAL CONTRACT — pos/orientation/size + `script` link id
  `"road:<name>:reach<k>:a|b"`) so the tunnel dresser consumes mouths the
  way it already consumes portals.
* Round-trip test mirrors L9: field-for-field, floats bit-exact, negative
  control included.

### T4 — THE DRAWING TOOL

Interaction model (all in the plan view; `pxToWorld` is the pick):

* **Draw Road** (per class): click places a control node (grid snap 50 ft
  default, toggleable; Details panel gives numeric entry in ft). Click near
  an existing node/edge snaps and WELDS — that is how junctions are made.
  Live readout at the cursor: segment ft, run total mi, estimated grade of
  the pending leg, carve budget consumed. Double-click/Esc/Enter ends the
  run. One draw run = one undo group.
* **Curves are fillets, not freehand.** Every interior node carries a
  radius (default per class: interstate 1,500 ft, alpine 600 ft, ramp
  250 ft, avenue 150 ft); the corner renders as tangent-arc-tangent. To
  draw the summit's 1,500 ft sweeper: place the two straight legs, grab the
  corner handle, drag until the readout says 1,500 ft (snap 50 ft). Radius
  below the class minimum paints the arc ORANGE. Rationale: piecewise
  line+arc resamples cleanly to stations, gives constant curvature per arc
  (clean banking via the existing kappa law), and cannot produce freeform
  spline slop.
* **Grade is not a brush — it is a consequence, shown live.** A PROFILE
  STRIP (ImGui plot, elevation ft vs station mi) for the selected road:
  natural ground + derived datum, from the SAME pure derivation the boot
  build runs (the P1 route layer exposes it as a callable — the
  one-derivation-two-callers seam, A8). Reaches where the clamp saturates
  against the class profile (Interstate 6%; Alpine 6/8%; ≤ 4% under 800 ft
  radius) paint RED in both strip and plan. The author's levers: move the
  route, mark a tunnel/bridge reach, or place a `yLock` (portal mouth,
  bridge deck, summit lot). No free grade knob to fight the grader with.
* **Reach marking**: select a node span → mark tunnel or bridge. Tunnel:
  the tool proposes portal stations where ground rises > 26 ft (8 m) above
  datum; the author adjusts; warns when a bore's station count would exceed
  32 (chain or shorten). Bridge: records the span gap (carve suppression —
  the road plan's SPAN GAP mechanism).
* **Validation, live while drawing** (warn, never block): grade profile
  exceeded · radius under class minimum · crosses water
  (`worldWaterLevelAt` sampled along the leg) without a bridge reach ·
  enters a range core band without a tunnel reach · road-to-river carve
  separation < 150 ft (G6) · corridor/node budget over cap (16 today, 192
  after road-plan P0) — the tally always visible.
* **Undo/redo**: `RoadCmd` snapshot commands in the EXISTING history stack
  (add/delete/move node, radius edit, reach edit), drags bracketed
  begin/commit, runs grouped. The entity-undo gap is not repeated.
* **Editing**: move node (drag), insert on edge (double-click edge), delete
  node (heals the polyline), delete road. Multi-select deferred to v2; the
  id scheme leaves the door open.

### T5 — PIPELINE CLOSURE (the drawn road reaches the game)

Tim's ruling: **"Load levels prior to corridor register"** — and verified,
the fromdoc path already obeys it (doc load `:2942` → corridor site `:3225`
→ first height query `:3243`). The concrete shape, DECIDED here:

**Parse early, build after.** The fromdoc branch switches from
`docLevel.loadFromFile(path, …)` to:

1. `LevelDoc doc; doc.loadJson(path)` — parse ONLY (existing API).
2. NEW `registerDocRoads(doc)` at the `:3225` site — run the shared
   derivation: stations at 68 ft, graded datum, banked; chain ≤ 32-node
   corridors on shared endpoint nodes; `registerTerrainCorridor` each;
   tunnel reaches build their `TunnelSpec`-with-station-list (the PASS 1
   seed change, finding 8) and register through
   `registerTunnelCorridorFor` — 1 corridor per bore.
3. Terrain: let `docWorld` into the terrain block (`:3204`) —
   TerrainStreamer + job system + horizon ring (copy `:3742-3748`).
4. `docLevel.buildFromDoc(doc, scene, device, physics)` — the full world
   build, from the SAME parsed doc (no double parse; existing API).
5. Road MESH: feed the derived stations to the extracted echo_roads
   compiler (PHASE 1.9-3) for ribbon/paint/collision. Sequenced last — the
   carve is drivable before the ribbon is pretty.

WHY parse-early rather than register-after-full-build (which would also
order correctly today): the doc build currently samples no terrain, but T6
settlements WILL seat on it via `placeOnTerrain` — a full-build-first shape
would then silently sample a carve-blind field. Parse-early makes the
registry contract hold BY CONSTRUCTION, not by the current absence of a
height query.

**Hot reload, stated plainly:** the reload path re-parses and rebuilds
scene/props — that keeps working. The corridor registry is read-only after
boot, so if a reload's `roads[]`/`roadNodes[]` differ from what was
registered, the loader logs a loud `[leveldoc] ROADS CHANGED — carve is
stale, RESTART to re-carve` and the HUD shows it. Nothing re-carves at
runtime, ever (the registry contract is what keeps worker-thread tile
generation race-free). The in-editor carve preview (T2) is the fast
iteration loop; the relaunch is the proof loop.

Dependencies owned elsewhere, stated plainly: `kMaxTerrainCorridors` 16→192
and the shared route-layer derivation are road-plan P0/P1; the
`TunnelRoute::cx/cz/halfLen` fix (finding 10) lands first; dressing drawn
bores (`TunnelCorridorWorld` is instantiated only by host_tunnel — the
city's four bores carve but are never dressed) is the tunnel plan's
N-group.

### T6 — SETTLEMENTS FROM THE PALETTE (the honest "prefab cities")

* v1 target: entity type `"settlement"` (existing entities[] format — zero
  parser change), placed by drag from the palette (palette rows are
  documented drag-drop sources already) onto the plan view; fields: preset
  (hamlet / gas stop / bridge town), seed, yaw. Consumed at boot by the
  road plan's town generator, which is being born position-independent.
* Junction / bridge / climb-foot markers are shown as SUGGESTED sites, but
  placement is the author's (the road plan's own finding: junctions alone
  are only a third of the truth about where towns belong).
* The two big cities are NOT draggable in v1 (finding 11). If ever wanted:
  parameterize `City::build`, un-latch `registerCityFreewayTunnels`, make
  terrain pads data-driven — a charter of its own.

## Acceptance conditions

- [ ] A1. Terrain tiles are honest: for 20 random texels across 3 zoom
      levels, the height each texel was baked from equals
      `terrainHeightAtWorld` at that texel's world centre (exact), and
      px→world→px round-trips within 1 px.
- [ ] A2. All FIVE ranges (incl. the tunnel ridge) are identifiable by eye
      at the zoom that frames the ~14-mile feature field; the ocean, river
      and the 4 city pads read at overview zoom.
- [ ] A3. Scale is honest: the demo bore's 2,100 ft measures 2,100 ± 20 ft
      with the on-screen ruler.
- [ ] A4. roads[]/roadNodes[] round-trip bit-exactly (%.9g), L9-style
      comparator with negative control; an OLD binary loads a roads-bearing
      doc without error (the skipValue path).
- [ ] A5. PIPELINE, gated on a SHORT road first: a ~1-mile road with one
      1,500 ft fillet and one short tunnel reach, drawn in LA, saved, then
      `--world fromdoc` boots terrain, registers its corridor chain before
      the first height query (T5 order), and the drive-through works;
      |carved floor − datum| ≤ 1.6 ft at every station (the G1 discipline).
      The 62-mile network is NOT this tool's gate — it waits on road-plan
      P0 (corridor cap 16→192).
- [ ] A6. The profile strip is correct: on the tunnel-ridge road, strip
      grade equals an independent probe of the height field along the
      stations, and the red/green verdicts match the G3 class profiles.
- [ ] A7. `--test-editor` and the LevelDoc round-trip suite stay green with
      roads present; new road undo test: every road op undoes/redoes to
      bit-identical doc state, one drag = one step, one draw run = one
      step.
- [ ] A8. PREVIEW HONESTY: for a test corridor, the plan-view carve
      preview equals the boot-carved field at the same sample points
      exactly (same pure functions, no second implementation) — asserted
      headless.
- [ ] A9. JUNCTION WELD: two roads sharing a node id produce exactly one
      junction downstream (cityblocks-style synthetic check), and moving
      the shared node moves both roads — no drift by construction.
- [ ] A10. RELOAD SEMANTICS: hot-reloading a doc whose roads changed keeps
      the world up, logs the stale-carve warning, and does NOT touch the
      registry; a restart then carves the new roads (A5 re-run).

## Explicitly NOT in this plan

* Auto-routing / pathfinding. Tim draws; nothing generates routes.
* Junction surface geometry (patches, stop bars, paint) — the road
  network's compiler owns it; the tool records shared nodes.
* An orthographic 3D viewport. The 2D map mode replaces the need; the
  perspective viewport stays as-is for inspection.
* Editing terrain from LA (read-only ground), and drawing RIVERS —
  `riverChain` is a singleton today; rivers-plural is road-plan P8.
* Relocating the existing city (see T6).
* Multi-select / marquee (v2; the id scheme leaves the door open).

## Seams with the other plans (so nobody is surprised)

* ROAD_NETWORK_PLAN P0: corridor cap 16→192 + cost measurement — the
  62-mile ceiling. P1: the shared polyline derivation this tool calls for
  its profile strip and T5 registration.
* TUNNEL_INTERIOR_PLAN N-group: bore-as-a-reach placement contract +
  dressing at scale; this tool emits the reaches and portal-mouth entities.
* Fix `TunnelRoute::cx/cz/halfLen` (never assigned — finding 10) before any
  doc-driven multi-route work; expect test outcome changes when it lands.
