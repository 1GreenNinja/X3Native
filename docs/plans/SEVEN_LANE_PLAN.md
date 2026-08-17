# THE SEVEN-LANE PLAN — destinations, debt, and a world worth driving
*Written 2026-08-16 ~23:15 by the session lead, at the owner's direction: "write out a
plan for a team of fable agents to knock out... maybe 7 of them." Base tip at writing:
`be8b55fc` on `integration/complete`. Two lanes (W-TRAFFIC, W-WEAPONS) are ALREADY in
flight tonight and are not part of the seven — this plan is the NEXT wave.*

## Standing law for every lane
1. Read `CLAUDE.md`, `docs/NO_SLOP.md`, `docs/design/X3_WORLD_RULES.md`,
   `docs/ENGINE_GOTCHAS.md` before coding. The sketch (`docs/design/
   ROAD_NETWORK_SKETCH_V2.png`) is route-spec LAW.
2. `git fetch origin && git reset --hard origin/integration/complete` first thing.
   Commit locally with receipts; the session lead merges and pushes. NEVER push.
3. GPU etiquette: check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before every
   launch; the owner plays at night, sibling lanes capture. Retry, don't abort the lane.
   NEVER `--smoketest`. SD 3.5 generation is DEAD on this box — pack/armory textures
   only (armory: `http://localhost:8787/galleries.json`; its GLBs are often
   draco-compressed — decode with `npx @gltf-transform/cli copy`).
4. Characters animate ONLY through `app/character_anim.*` (AnimatedCharacter).
   THE CONTACT LAW (NO_SLOP rule 11) applies to every new mover.
5. A lane is done when: build green · boot zero `[ERROR]` · suites green
   (roadnetwork / terraincorridor / tunnelmouth / riverbridge minimum) · fps ≥90%
   of baseline measured on a quiet GPU · eyes-on full-res captures READ BY THE AGENT.

## The seven lanes

### LANE 1 — W-TUNNEL: tunnel interiors v2 (task #30)
Owner spec, verbatim: "Tunnels got the widening treatment too.. they are 4 lanes with
low divider.. sidewalk off a concrete shoulder on each side.. doors to stairways to
halls... ramp exit to garage.. exit from garage to mountain top parking lot.. with exit
down to the other tunnel."
- Widen the bore cross-section (kTcCorridorHalfW ripple: shell, carve, backfill — every
  tunnelmouth/terraincorridor gate re-derived). Low center divider, raised sidewalks on
  concrete shoulders, wall doors → stairways → halls (ties to task #21 rooms).
- The LOOP: tunnel → ramp → LNSS garage → up to the summit parking lot → back down
  into the OTHER tunnel. All grades legal; the lot per the sketch.
- Also owns: rewriting the stale `--test-tunneldrive` gates (A2/A3/B1/B5b consume a
  dead env var — W-FREEWAY's report has the receipt), and growing the demo-road
  aprons to spec once the carve widens (PAIRED comment in tunnel_corridor.cpp).
- Files: `app/tunnel_corridor.*` (owns), `app/terrain.cpp` carve interfaces (light).

### LANE 2 — W-WATER: one water truth (tasks #32, #23, sub from #19)
The river's drawn plane is flat while the carved channel descends — downstream it
climbs the banks (receipt: the submerged bench, task #32).
- Step/clip the drawn plane down the channel; `worldWaterLevelAt` returns the DRAWN
  level inside the reach — ONE truth, and the bench/tree `minBenchY` shim in
  `road_trees.*`/host gets deleted (rule 4).
- Rain runoff: at rain ≥6 the river visibly rises (bounded, never over the levees);
  puddle sheen on aprons is stretch.
- THE SUB: owner asked for "a sub or 2" — a patrol submarine in the deep reach,
  BoatDemo-pattern, visible from the bridge and underwater.
- Files: `app/river_bridge.*`, `engine water shaders`, `app/terrain.cpp` (water table),
  host water block.

### LANE 3 — W-PERF: hold the 165 (tasks #33, #26)
The owner's benchmark is "Huge open world... 165 fps."
- Distance-scope the corridor full-res refine: exact within ~500 m, per-vertex
  `min(chord, field)` clamp beyond (wedge can never stand above the road at range,
  fps cost dies). C7/M7/W1/W1b must stay green near-field. A/B fps receipts.
- Ridge-LOD blade towers (task #26) and the horizon-ring inner-hole seam
  (W-MOUNTAIN residual) — same subsystem, same lane.
- Files: `app/terrain.cpp` LOD/mesh path (owns after Lane 1's carve interface work —
  coordinate; terrain.cpp is the ONE file two lanes touch: refine/LOD is Lane 3's,
  carve geometry is Lane 1's).

### LANE 4 — W-TOWN: Small Mountain Town (from #35)
The sketch's ladder-switchback town, made real.
- Buildings from the licensed packs (City Street Environments / Village bundles /
  French Quarter — mine the pack DEMO SCENES for placement per the
  x3native-environments skill; convert via `tools/convert_unity_pack.py`).
- Main street along the ladder road: shops, parked cars (Vehicles/ GLBs), street
  props, benches (armory), night windows (emissive).
- NPC lives: 6-10 pedestrians on sidewalk loops via AnimatedCharacter + crowd_skin
  roster. CONTACT LAW.
- Files: new `app/town.*` (EnvArt overlay pattern), placement manifest in
  `docs/design/`.

### LANE 5 — W-STATIONS: fuel the freeway (from #35)
"Places for cars to go, to fuel up."
- 2-3 gas stations: freeway-side (off a turnaround/junction with a drivable apron)
  + one in town + one country crossroads. Canopy, pumps, kiosk from packs/armory —
  textured or held (rule 3).
- Fuel mechanic STUB: drive under the canopy, `E` refuels (HUD fuel gauge appears
  when the mechanic arms; console `fuel` cvars). Structure so the campaign can
  turn consumption on later.
- Files: new `app/gas_station.*`, host wiring, HUD hook.

### LANE 6 — W-FACTORY: the chocolate factory (from #35)
"A chocolate factory to win tickets to." The hero landmark.
- Distinct silhouette on the skyline (sketch-consistent placement — NE forest edge
  or riverside): big massing, smokestacks with particle smoke, emissive signage at
  dusk, gated drive, fence line. Pack-mined industrial kit (1900s Industrial /
  Abandoned Factory packs) — this is a REAL building, not a box.
- GOLDEN TICKET stub: five tickets hidden across the world (one at the factory
  gate); `E` collects, HUD count, console `tickets`. Finding all five opens the
  factory gate — inside can be a single hall room for now.
- Files: new `app/factory.*`, ticket state in host, map POI hook for Lane 7.

### LANE 7 — W-MAP: map v3 + POIs (task #22 + freeway residual)
The world grew; the map must catch up.
- M cycles map ↔ minimap; rotation Q/E + N/E/S/W compass rose; always-on route
  labels; sharper bake; minimap contrast pass (owner: "I just cant see ANYTHING").
- Freeway draws at its TRUE dual width on the map (W-FREEWAY residual: host stages
  `MapRouteOverlay` without `widthM`).
- POI markers: town, gas stations, factory, LNSS shop, summit lot, bridges —
  icons on map + minimap edge arrows. Consumes Lanes 4-6's positions (theirs are
  single registration calls; agree the tiny `MapPoi` struct FIRST — see below).
- Files: map/minimap module + host staging.

## Sequencing and the conflict matrix
- **Start immediately, in parallel:** Lanes 2, 4, 5, 6, 7 (disjoint files).
- **Terrain handshake:** Lane 1 and Lane 3 both enter `terrain.cpp`. Lane 3 starts
  FIRST (the refine scope is small and measurable); Lane 1 rebases on Lane 3's merge
  before its carve ripple. Do not run them concurrently in the same functions.
- **Tiny shared contract, agreed before anyone codes:** `MapPoi { name, x, z, icon }`
  registration (Lane 7 defines it in a header commit the session lead fast-merges;
  Lanes 4/5/6 consume).
- **Merge order target:** 3 → 1 → 2 → 4 → 5 → 6 → 7 (7 last so every POI exists).
- Every lane: worktree from `origin/integration/complete`, local commits, session
  lead merges with eyes-on and pushes, per tonight's ritual.

## Also in flight / near-term (not of the seven)
- W-TRAFFIC (task #34) and W-WEAPONS (task #29) — tonight's lanes, merge on report.
- W-CLOUDS capture batch → merge (task #27 closes).
- Backlog still open behind this wave: Level Architect 11.1 (#18), underground base
  rooms (#21), rd_asphalt_01 store publish, InspectorX's unmerged lanes.
