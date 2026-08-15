# Plan — a road NETWORK: longer, curving, over the pass, and back

Status: DRAFT v3, pre-execution (P1 in flight). Conditions written before any
code, on purpose.
Author: InspectorX, 2026-08-15. Sharpened: Fable, 2026-08-15 (pass 1), Fable,
2026-08-15 (pass 2 — the rivers/bridges/towns material and the decided rings).
Ask: Tim —

> "We NEED a LONGER ROAD.... and then a road that curves up over the mountain
> through passes... goes somewhere else... circles back.. intersections..."
> "...a road that goes up to a parking lot on top of the mountain, with nice
> curves we can speed around"
> "AHH yes.. Make rivers.. Deep ones, with fish .. and Beautiful lit concrete
> bridges"

UNITS: **feet and miles**, per Tim — feet for anything you could stand next to,
miles once it is a journey, mph for speed. Engine data stays SI; conversion at
the boundary only. Where a CODE CONSTANT is quoted it is quoted in its own
units (metres) with the feet alongside, so nobody mis-edits a source file from
this doc.

## Sharpening pass 2 (2026-08-15) — what changed and why

The new material (both rings, the retirement of the racing frame, towns, and
the whole rivers/fish/bridges section) had never been sharpened. Everything
below was re-verified against `terrain.h/.cpp`, `fish.h/.cpp`, `app_run.cpp`,
`world_regions.cpp`, `street_lights.h/.cpp`, `echo_roads.h`,
`tunnel_corridor.h`, and `engine/rhi/ClusterLights.h` — not against the plan's
own summary of them.

1. **The crossing mechanism was misdescribed — corrected.** The river carve is
   NOT a `TerrainCorridor`: it is a hand-authored block in
   `authoredLandforms()` (terrain.cpp ~809), applied BEFORE `applyCorridors()`.
   "Deepest-wins erases the roadbed" is the wrong story. The real reasons a
   road cannot cross at grade: (a) corridors only LOWER — nothing in the
   terrain layer can FILL a channel; (b) the 4.5% grade clamp leaves the road
   datum hanging ~17 ft above the bed across the channel, so the carve floor
   steps off a cliff; (c) the road cut breaches the LEVEE below the waterline
   — a dry trench beside standing water, the one image nobody forgives. The
   conclusion (a bridge is the only correct answer) SURVIVES; the mechanism
   section is rewritten, and the carve-exclusion mechanism turns out to be
   nearly free (see THE SPAN GAP) because pass 1 already made corridors chain.
2. **As literally decided, NOTHING crosses the one river — stated plainly.**
   The river's entire extent is within 0.9 mi of the origin; ring-to-ring
   spokes live 2.4–5 mi out. Crossings exist only because the inner ring is an
   authored TOUR that visits the ocean shore, and because rivers-PLURAL will
   be authored across the ring country. Bridge sites are authored decisions,
   not emergent ones. The bridge chapter now names its site.
3. **The outer ring cannot be a circle — flagged with numbers.** A 31-mile
   ring is radius ~5 mi ≈ 8.05 km; the N range's CORE band is 7.8–8.8 km out
   and the W core starts at 8.1 km. A circle drives straight over two range
   spines (1,250 ft and 1,050 ft of climb each). Both rings are authored
   waypoint tours; "radius" is nominal.
4. **The bridge got real engineering** — type, spans, pier count, structure
   depths, deck elevation (with the low-set/high-set decision Tim actually has
   to make), abutments, collision, and how "lit" is built inside the light
   budget. New section, new gates B7/B8.
5. **The light-budget premise was stale — corrected.** 64 is the LEGACY path's
   cap. `ClusterLights` exists (`kMaxSceneLights = 1024`, built exactly so "a
   fully dressed tunnel" and a night city fit) behind `r_clusterlights`,
   default 0 because every md5/screenshot gate pins the legacy path. Design
   rule kept anyway: emissive-first, ≤ 6 pooled lights per structure — then
   the work is correct on BOTH paths and flipping the cvar is pure upside.
6. **Street lights already line arbitrary roads — the plan's open question is
   closed.** `StreetLights::buildDistrictLamps()` takes host-supplied rows
   `{x0,z0,x1,z1,y,spacing}` and `setGroundQuery()` seats the ground pools on
   real terrain. What's missing is only a polyline wrapper (chain rows along
   route stations) and possibly a Highway zone look. Also decided: rural rings
   stay DARK between settlements — real highways are unlit; lighting is pooled
   at towns/junctions/bridges/the summit, and the darkness between is what
   makes them read.
7. **Fish: deepening is nearly free, banding is the real work.** Depth is
   query-driven (`FishWaterFn`/`FishBedFn` — a deeper bed propagates
   automatically), and the schools ARE seeded on the river spline
   (app_run.cpp ~3810). What breaks at 20 ft: `slotD` spreads fish only
   0–4.6 ft below the surface band, so the bottom 14 ft of a deep channel is
   EMPTY water. New R5: per-species depth bands (pike deep and alone, rudd
   shallow in schools). Also corrected: the "three-piece S-flexing hull" is
   the procedural FALLBACK; the shipped fish are pose-baked Rodin GLBs (one
   entity, mesh swap per beat frame).
8. **River deepening is a two-site edit and should become per-node.**
   `kWorldRiverBedDrop = 3.2` (terrain.h) and the hard literal `w - 3.2f`
   (terrain.cpp ~816) must move together — or better, the bed drop becomes a
   per-node array like `kRiverC[]`, so the river DEEPENS DOWNSTREAM (real
   rivers do) and the beach reach by the facility stays wadeable.
9. **Network arithmetic re-done at the DECIDED scope.** Both rings + spokes +
   climb + connectors ≈ 62 mi ≈ 4,800 nodes ≈ 10.8M boot queries — double
   pass 1's 31-mile figure. `kMaxTerrainCorridors` wants ~192, not 96, and B1's
   500 ms budget likely needs adaptive sampling, not just the job system.
10. **Phases resequenced end-to-end** (P0–P9): rivers deepen early (cheap,
    Tim-visible, independent of P1), the bridge lands as its own phase right
    after the first junction forces the abutment interface, towns follow the
    bridge, and the 31-mile outer ring goes LAST — biggest cost, least
    novelty. P1 itself is untouched and is already in flight: the working
    tree's `tunnel_corridor.h` carries `tangentAt()`/`segmentAt()`, a
    local-tangent `worldAt()`, and multi-route `TunnelSpec` — exactly P1's
    shape.

## BUILT 2026-08-15 — the outer tour + Bridge No. 1 (what the terrain said back)

Delivered on `inspx/outer-ring`: `registerOuterRing()` (road_network) and the
valley road + Bridge No. 1 (`app/river_bridge.{h,cpp}`), gated by
`--test-roadnetwork` (17/17) and `--test-riverbridge` (9/9). Driveable in
`--world tunnel` under `X3_OUTER_RING=1` / `X3_RIVER_ROAD=1`. Everything below
was MEASURED against the field; four places where this plan and the terrain
disagreed are recorded because they change how the next phases read this doc:

1. **The outer tour is 30.8 miles with FIVE bores, 3.9 miles underground** —
   not "four bores, one per range". Measured: the nominal circle needs an
   862 ft trench through the N massif (the self-test's negative control keeps
   proving it); the N range takes two tunnels (flank 1.8 km + massif 3.2 km),
   the W range three (the circle runs near-parallel to its spine — the worst
   geometry — and a 2nd survey found the only sane lane at r 6800 with three
   short-to-long bores), the S mesa needs NONE (open bench ride at r 7600),
   and the E volcanic core is out of reach entirely: its spine is 9.8 km from
   the ring centre, beyond any 31-mile geometry. The east arc rides foothills.
2. **Tunnel grade physics vetoes high portals.** A bore's internal profile is
   capped at 4.5%, so a long tunnel drags its uphill portal far below any
   bench the road might meet it on (measured: −60 to −100 m on the first W
   authoring — the "summit plateau crossing" died there). Portals must be
   authored at chord ends whose GROUND matches what 4.5% can reach; descents
   steeper than 7% happen INSIDE the rock (Crystal Descent Tunnel, 2.8 km at
   4.49%).
3. **Bridge No. 1 is a GORGE bridge, not a levee bridge.** This plan modelled
   the N5–N6 site as low levees (crests 4.6–7.2 ft, deck ≈ waterY + 9 ft).
   Measured: the country around that reach stands ~100 ft above the water and
   the channel cuts through it as a slot — the "low-set" crest+2 ft deck is a
   280 ft span 106 ft over the river on ~100 ft piers. All B-gates hold on the
   real geometry (soffit clear, piers outside the floor, span bit-untouched);
   the LOOK is more than was promised, not less.
4. **`worldRiverNodes()` returns the 16-node Chaikin chain, not the 9 authored
   nodes.** Indexing it as authored put the first bridge plan on the facility
   beach reach; the soffit gate caught it. The N5–N6 mid-reach is chain
   [10]→[11].
5. Also: `--test-echoroads` already fails 9 gates (R6 frontage checksum …) on
   the base branch BEFORE this work — N3's "still passes" premise is stale.

New machinery, reusable by P4–P9: `RoadSpec::Gap` (span gaps: bores and decks
suppress the carve, datum pinned across), the PORTAL RAMP grading ceiling
(bounded approach embankments — corridors still never fill), a close-the-loop
depth pass in `registerRoad` (the tunnel module's step-4 discipline, ported),
and `X3_RING_SURVEY` (dump graded-cut profiles along candidate circles — the
instrument every waypoint above was authored with).

## DECIDED BY TIM, 2026-08-15 (after the sharpening pass — these WIN)

1. **BOTH rings, not one.** *"15 mile ring"* … *"Still make the big ring too"*.
   * INNER ~15 miles — the tunnel ridge, the pass, the summit lot, the city
     pads, the ocean shore. Rolling country the whole way; it does NOT reach
     the ranges, and that is accepted, not an oversight.
   * OUTER ~31 miles — tours all four ranges (N snow, E volcanic, S mesa,
     W crystal hills).
   * **The SPOKES between them are where the intersections come from free.**
     Four radial connectors give eight junctions without inventing a reason for
     roads to meet — which is exactly the structure Tim asked for.
   * (pass 2) BOTH rings are authored WAYPOINT TOURS, not circles — see THE
     RINGS ARE TOURS below. The inner ring must dive nearly to the centre to
     cross the tunnel ridge (0.3–0.7 mi out) and swing to the shore; a 31-mile
     circle at nominal radius drives through the N range's core. "Radius" in
     this plan is shorthand for scale, never geometry.
2. **THE GOAL IS NOT LAP TIME.** *"Lap times dont matter so much now.. we just
   need roads for structure and realism and beauty."* This retires the racing
   framing. A closed loop is no longer proof of anything; the question is
   whether the world reads as a PLACE. Every acceptance condition phrased as
   "the car returns to the start" is downgraded to an integrity check, and the
   load-bearing gates become the ones about how it LOOKS and whether the
   network makes sense as built infrastructure.
3. **STREET LIGHTS, SMALL TOWNS, GAS STATIONS.** *"We will add street lights,
   small towns with gas stations."* Settlements are what make a road mean
   something — a road with no destination is a racetrack.
   * (pass 2) `street_lights.cpp` was checked: it ALREADY lines arbitrary
     roads (`buildDistrictLamps` rows + `setGroundQuery` terrain seating,
     `lampRow` alternates sides and faces heads at the road). The town chapter
     below owns what remains.
   * (pass 2) "The junctions from (1) are the natural town sites" is only a
     third true. Ring/spoke junctions sit 2.4–5 mi out in empty rolling
     country; the sites with a REASON are the river crossing (bridge town) and
     the foot of the summit climb (last-gas-before-the-pass town). Junction
     hamlets come when the outer ring lands. See TOWNS.
4. **SPEED IS NOT THE MEASURE, and the numbers we have are unreliable.** Tim
   driving: *"It never went past 120 in the non turbo version."* A headless
   probe reporting 370 mph was INVALID — it ran on a 984 ft slab, drove off the
   edge, and sampled the car falling. There is no aero drag on the wheeled
   vehicle (verified in code, not by probe), so nothing sets a terminal speed
   but gearing and terrain. Do not tune the road to a speed we have not honestly
   measured on real road.

## CORRECTION 2026-08-15 — "WE CAN DRIVE THROUGH A MOUNTAIN. WE HAVE TUNNELS."

Sharpening pass 2 concluded the outer ring **cannot be a circle**, because at
4.93 miles radius it lands inside the north range's core band (7.8-8.8 km) and
would drive through a mountain.

Tim: *"We CAN drive through a mountain!!!! we have TUNNELS!!!!"*

He is right, and it is the better answer. Driving through a mountain is not the
failure mode here — it is a SHIPPED FEATURE with a 7/7 gate on it. The premise
"a road cannot go there" quietly imported an assumption from games that have no
tunnels. This one has `TunnelCorridorWorld`, cut-and-cover carve, portals, a
backfill lid, and `registerCityFreewayTunnels()` already boring four city
freeway bores.

**What this changes:**

* The outer tour may run CLOSE TO A CIRCLE and BORE THROUGH the ranges where it
  meets them, instead of detouring around them. A ring road that punches through
  four mountain ranges is a far better road than one that avoids them, and it
  reuses the most finished system in the lane.
* Tunnels stop being a set piece and become **infrastructure** — a repeatable
  element the network uses wherever it meets a range. That is a different
  engineering problem from one showcase bore (cost at multiples, per-bore
  identity so eight do not read as eight copies, bores on a CURVE now that the
  frame follows a polyline, long-bore behaviour). The tunnel spec is being
  re-sharpened in that light; see TUNNEL_INTERIOR_PLAN.md.
* The "authored waypoint tour" conclusion still holds for the INNER tour (it
  goes to the valley, the climb and the summit by choice) but is no longer
  FORCED on the outer one by terrain.

**Still true, and not overturned:** a tunnel is only the answer where there is a
mountain. The outer tour still cannot be a naive circle drawn without looking —
it must be authored so its range crossings land where a bore makes sense
(through the ridge, not clipping a shoulder), and so the four bores are spaced
as landmarks rather than arriving in a clump.

## DECIDED 2026-08-15 — HOW THE ROADS REACH THE WATER: "Both, valley route first"

Pass 2 established that as literally specified, **nothing crosses the river**:
its whole run is 0.20-0.89 miles from origin (kRiverX/kRiverZ, 9 nodes), while
the inner tour sits at 2.39 miles and the outer at 4.93. Nearest approach is
~1.5 miles. Beautiful lit bridges with nothing to span.

Tim's call, asked as an either/or and answered **both**:

1. **VALLEY ROUTE FIRST.** Bring the roads to the water: the inner tour's shore
   leg runs the river valley and crosses at the named site (N5-N6, Bridge No. 1).
   This gets ONE real bridge standing early, where "beautiful lit concrete" can
   be judged before committing to several. It also drags the inner tour inward
   off its circle — which it had to leave anyway (see THE RINGS ARE TOURS).
2. **THEN RIVERS PLURAL.** Bring the water to the roads: additional rivers
   radiating outward so both tours cross naturally and there are several
   bridges. Sequenced second because `riverChain()` is a hard-coded SINGLETON —
   plural is a registry refactor, not a copy-paste, and it should not block the
   first bridge.

Sequencing consequence: P4/P5 (shore leg + Bridge No. 1) keep their place, and
P8 (rivers plural) stays after them. Nothing moves; the decision confirms the
order pass 2 already chose, and records WHY so it is not re-litigated.

## RIVERS, FISH, BRIDGES — added by Tim 2026-08-15

> "AHH yes.. Make rivers.. Deep ones, with fish .. and Beautiful lit concrete
> bridges"

This belongs in THIS plan, not a separate one: a bridge exists precisely where
a road meets a river — but (pass 2) the meetings are AUTHORED, not emergent.
The decided geometry produces ZERO crossings by itself: the one river's whole
run (x 320–900, z 180…−1120 in engine metres) is inside 0.9 mi of the origin,
and the ring-to-ring spokes live 2.4–5 mi out. Crossings happen because (a)
the inner tour visits the ocean shore and must cross the river's lower reach
to get there, and (b) rivers-PLURAL will be authored down from the ranges
across the outer country. Rivers are also what stop a 31-mile ring from being
a circle on a lawn.

**What is REAL today — checked in the source, not assumed.**

* **The river carve EXISTS and is good.** `terrain.h:116–131`: `WorldRiverNode
  {x, z, waterY}` is ONE authored spline shared by the height-field carve
  (`authoredLandforms()` in terrain.cpp — note: NOT a `TerrainCorridor`; the
  corridor primitive is the river carve's polyline GENERALIZATION, per its own
  header) and the water ribbon (`world_regions.cpp`), so the water can never
  sit outside its own channel. `waterY` descends monotonically downstream — it
  actually flows. An authored levee term holds the bank crests above the water
  where the natural country is low, with a `deepSkip` guard so no berms grow
  underwater. Current dimensions: water ribbon **223 ft** wide
  (`kWorldRiverHalfWidth` 34 m), full-depth bed floor **79 ft** wide (12 m
  half-width), banks shelving out over a further **85 ft**, bed **10.5 ft**
  below the surface (`kWorldRiverBedDrop` 3.2 m — AND a hard literal
  `w - 3.2f` in terrain.cpp ~816; two sites, one number), levee crests
  **7.2 ft** proud (per-node `kRiverC[]`, dropped to a 0.7 ft beach on the
  facility reach).
* **The fish EXIST, they are RIVER fish, and they are ALREADY IN THE RIVER.**
  `fish.cpp` species table: rudd (10 in), bream (12 in), perch (9 in) — plus a
  **3 ft pike** that spawns ALONE as an ambush predator (`solitary = true`;
  "a predator does not shoal — that is the whole point of the pike"). The
  shipped art is pose-baked Rodin GLBs (one entity per fish, mesh swapped per
  beat frame — real skinned-quality deformation for zero per-frame vertex
  work); the lofted three-piece S-flexing hull is the never-break-the-world
  FALLBACK. Schools are seeded ON the river spline nodes with downstream
  headings, plus two estuary schools (app_run.cpp ~3810). Depth handling is
  QUERY-DRIVEN: `FishWaterFn`/`FishBedFn` feed surface and bed live, so a
  deeper carve propagates to the fish automatically.
* **BRIDGES DO NOT EXIST.** Every `bridge` hit in the codebase is INTERIOR
  facility geometry (corridor spans in `level_loader.cpp`, tube gaps in
  `cave_atmosphere.cpp`). There is no road bridge, no pier, no abutment, no
  deck. This is the genuinely new build — but its PARTS are proven: welded
  ribbon meshing + drivable-surface collision export (echo_roads), headwall/
  wingwall/retaining-wall craft + real concrete surface sets (tunnel_corridor
  + surface_library), lamp/cone/pool-disc kit (street_lights).
* **There is exactly ONE river, and the plumbing is single-river by
  construction.** `riverChain()` is a singleton; `worldRiverNodes()` is one
  table; `worldWaterLevelAt()` answers for that one chain plus the sea.
  "Rivers plural" is not just authoring — it is a RIVER REGISTRY refactor
  (array of chains, same shape as `kRanges`), touching the carve, the ribbon
  builder, the water query, and the fish-school seeding loop. Mechanical, not
  hard — but it is a refactor, and the plan should stop calling it
  "multiplying".

**So the honest split:** deepening the channel and banding its fish is a
small, early, Tim-visible job; the bridge is a from-scratch ASSEMBLY of proven
parts; rivers-plural is a real refactor that unlocks the outer country's
crossings.

### The crossing problem, stated correctly (pass 2 rewrite)

Three separate things go wrong when a road corridor is driven across the
river, and none of them is "deepest-wins":

1. **Corridors only LOWER.** `terrainCorridorDelta()` is ≤ 0 by contract.
   Nothing in the terrain layer can raise a channel bed to carry a roadbed.
   The 223 ft of water is uncrossable at grade, full stop.
2. **The grading clamp strands the road in the air.** The route layer grades
   `roadY` at ≤ 4.5%; the bank drops ~18 ft in ~125 ft (14%). Across the
   channel the graded datum hangs far above the bed, the corridor's depth
   profile (natural − datum) goes negative and clamps, and the carve floor
   simply steps off the bank.
3. **The cut breaches the levee.** The road's approach cut through the bank
   would carve below `waterY` beside the ribbon — the terrain crest dips under
   the water surface (an R2 violation) and the eye reads a dry trench next to
   standing water.

**A bridge is the only correct answer, and the exclusion mechanism is nearly
free — THE SPAN GAP.** Pass 1 already made carve registration CHAIN ≤ 32-node
corridors. So: the route's carve corridors simply END at the near abutment
face and RESUME at the far abutment. No new terrain mechanism, no portal-hole
analog (there is no ground above the deck to drop). What IS new, in the ROUTE
layer: a per-reach `bridge` flag that (a) pins `roadY` across the gap (level
or one long vertical curve — no grading against ground that isn't there),
(b) suppresses carve registration for stations inside the gap, and (c) makes
the G1/G2 gates check DECK-vs-datum across the span instead of carved floor
(they would otherwise fail on untouched river, which is exactly condition B3
working). The deck, piers and abutments are meshes with collision — the same
`addStaticMesh` lane the tunnel road ribbon and echo_roads' collision export
already use.

The corridor-vs-corridor deepest-wins hazard is REAL but it lives elsewhere:
it is gate G5 (switchback undercut), and it will also apply where a road
corridor parallels a future CREEK carve too closely. Same 150 ft separation
rule; note added to the rivers-plural phase.

### THE BRIDGE — real engineering for a 223 ft river (new, pass 2)

Site for Bridge No. 1: **the N5–N6 reach** (engine (480,−560)→(620,−830)) —
the river's middle run, levee crests 4.6–7.2 ft, clear of the facility guard,
outside the basin release, and exactly where the inner tour's shore leg wants
to cross. Cross on a TANGENT reach, square to the channel (skew ≤ 15°) — real
practice, and it keeps the abutments simple and the deck unbanked (also real
practice: superelevation is avoided on bridges when possible).

**Type: continuous prestressed-concrete HAUNCHED BOX GIRDER, three spans.**
* Why not cable-stayed: agreed wrong — cable economics start ~500 ft spans.
* Why not an arch: an arch wants rock abutments or a gorge; these banks are
  low earth levees, and an arch springing off a floodplain reads as fake.
* Why haunched concrete: it IS the "beautiful lit concrete bridge" — the
  haunch curve over each pier is the signature line the lighting will pick
  out at night. It is also the type whose span range (100–250 ft) fits this
  river exactly.

| element | number | why |
|---|---|---|
| crossing, crest to crest | ~250 ft | ribbon 223 ft + levee crests |
| deck length | **280 ft**, spans **80 / 120 / 80** | side:main ratio 0.67 — standard for continuous girders; main span clears the 79 ft full-depth floor with margin |
| piers | **TWO**, at ±60 ft from channel centreline | just outside the full-depth floor (±39 ft), standing in ~17 ft of water on the carved slope |
| pier form | rounded-nose wall pier, 6 ft thick × 20 ft wide, semicircular cutwaters both ends | the classic river pier; reads right from every angle |
| pier footing collar | 8 ft collar slab at the waterline | hides the pier/ribbon mesh intersection — the ribbon is a render mesh, the pier passes through it; the collar is what a real pier shows at the waterline anyway (B6) |
| structure depth | **6 ft at piers, haunching to 3.5 ft at midspan** | span/20 → span/34, textbook for a haunched box; the varying soffit line is the beauty |
| deck out-to-out | **43 ft** | two 12 ft lanes + two 6 ft shoulders + two 1.5 ft parapets — carries the 39 ft paved width (`kTcRoadHalfWidth` 6 m) with parapet margin |
| parapet | 2.9 ft concrete + rail, WITH collision | a car must not leave the deck sideways |
| abutments | seat-type ON the levee crest, wingwalls turned parallel to the river, 25 ft approach slab | the tunnel module's headwall/wingwall craft, reused; the carve's span gap starts at the abutment face |
| deck elevation | **DECISION FOR TIM — default LOW-SET**: deck top = crest + 2 ft (≈ waterY + 9 ft), midspan soffit ≈ waterY + 5.5 ft | low-set needs NO approach works: the abutment sits on the crest and the road carries on at country grade. HIGH-SET (waterY + 16–20 ft, the majestic option) is REJECTED for Bridge No. 1: corridors cannot FILL, so raised approaches need embankment MESHES or viaduct spans — real work, deferred until a bridge earns it |
| materials | surface_library concrete sets + grime | the exact sets the portal headwalls wear — the world's concrete stays one family |
| collision | deck top + approach slabs + parapets via `addStaticMesh` | same lane as the tunnel ribbon and echo_roads' `RoadCollisionMesh` |

**How "LIT" is actually built (B5), inside the budget.** The engine's DEFAULT
light path caps 64 pooled lights per frame (`kMaxPointLights`, legacy); the
clustered path (`r_clusterlights 1`, `kMaxSceneLights` 1024) exists and was
built for exactly this class of scene, but every md5/screenshot gate pins the
legacy path, so the bridge must read correctly on BOTH. Doctrine (same as the
bore's `kTcMaxBoreLights = 6`): emissive geometry is free, pooled lights are
precious.
* **8 parapet lamps** (4 per side, ~70 ft stagger): street_lights-style post +
  emissive head + fake-volumetric cone + ground-pool disc on the deck. All
  emissive — zero pooled cost.
* **≤ 6 REAL pooled lights total**: four at the third-points of the deck (the
  drive-across read), two as pier uplights (the from-the-bank read). Merged
  through the same nearest-K `selectLights()` lane the street lamps use,
  gated by `cityLightsOn`.
* **Pier uplight geometry**: an emissive gradient quad hugging each pier face
  under the haunch — the concrete "catches" light even when the pooled light
  loses the nearest-K cut.
* Do NOT light the water from below; the water surface has its own shader and
  uncalibrated glow under it reads as slop. The reflection of the lit parapet
  line is the shot.

### River conditions
- [ ] R1. DEEPER, DOWNSTREAM, IN THE THALWEG — not a uniform trench: bed drop
      becomes PER-NODE (a `kRiverD[]` beside `kRiverC[]`), shallow at the
      source (~5 ft), **≥ 20 ft from N4 to the mouth**, measured by probe as:
      bed ≤ waterY − 20 ft across ≥ 60 ft of mid-channel width on the deep
      reaches. Margins still shelve (fish and wading need shallows; the
      facility beach reach stays wadeable). The header constant and the
      terrain.cpp literal are unified into ONE source before any number moves.
      NOTE the cheap truth: everything ABOVE the waterline is untouched by
      deepening — banks, levees and sightlines cannot regress; only the
      underwater profile and its consumers can.
- [ ] R2. The water surface stays INSIDE its channel for the whole run: no
      sample where the terrain crest is below `waterY`. Already spot-asserted
      by `--test-worldregions` W10 (banks dry along the ribbon edge); this
      gate densifies it and re-runs it after every carve change, including the
      bridge's span gap and approach cuts.
- [ ] R3. Fish are visibly in the river: a capture from the bank AND one from
      the bridge deck looking down. RISK, named now: a pike at 20 ft may be
      INVISIBLE through the water surface shader — nobody has ever looked into
      6 m of this water. If the deck capture can't resolve it, the answer is
      the pike's band (it holds a mid-depth station, below the shoals, above
      the void — see R5), not a clarity hack on the water.
- [ ] R4. `waterY` still descends monotonically after any re-authoring — a
      river that flows uphill is the one defect nobody forgives.
- [ ] R5. SPECIES DEPTH BANDS (new): today every fish hangs in the top 4.6 ft
      (`slotD` = 0–1.4 m below the surface band) — a 20 ft channel would be
      empty water below 6 ft, which reads as a swimming-pool liner, not a deep
      river. Add per-species bands to `FishSpeciesDesc`: rudd/perch in the top
      6 ft in schools, bream 6–12 ft, **pike within 6 ft of the bed, alone,
      barely moving**. Asserted by probe over simulated time, not by eye.
- [ ] R6. RIVERS-PLURAL REGISTRY (later phase, gate stated now): the single
      `riverChain()` becomes an array of authored chains (the `kRanges`
      pattern); `worldWaterLevelAt`, the ribbon builder, the carve and the
      fish seeding iterate it. Each new river passes R2/R4 independently.
      New rivers are mountain CREEKS (30–60 ft wide, 2–4 ft deep, steeper
      `waterY` gradient, no navigation pretensions) running off the ranges
      toward the basin — they are what the outer ring and spokes actually
      cross, and each crossing is a SINGLE-SPAN 60–80 ft bridge (the kit's
      easy case). A creek carve near a road corridor obeys the G5 150 ft
      separation rule like any other pair of cuts.

### Bridge conditions
- [ ] B3. A road crosses the river on a DECK, and the terrain under the span is
      untouched river — no carve, no fill, no z-fight. (The span gap working.)
- [ ] B4. The deck is drivable end to end: a car crosses without leaving the
      surface and without a step over **0.2 ft** at either abutment. The deck's
      end vertex rows are WELDED to the road ribbon's last station rows (the
      echo_roads shared-vert discipline) — a butt joint will not pass this.
- [ ] B5. LIT: the bridge reads at night — parapet lamp line, lit haunches,
      pier uplights, the reflection in the water — judged by capture, at
      night, from the bank three-quarter view AND from the driver's seat.
      Pooled-light spend ≤ 6; must read on the LEGACY light path.
- [ ] B6. Piers stand ON the carved riverbed at their true sampled height
      (terrain's problem, not a floating mesh), wear their waterline collars,
      and do not visually dam the ribbon.
- [ ] B7. THE SPAN GAP (new): no carve influence anywhere between the abutment
      faces — probed along the span at the corridor's own sample pitch. The
      river's R2 must hold THROUGH the bridge reach (the approach cuts stop at
      the crest and never notch the levee below `waterY`).
- [ ] B8. STRUCTURE SANITY (new): pier spacing clears the full-depth floor;
      soffit clears `waterY` by ≥ 4 ft at midspan; parapet collision stops a
      60 mph glancing hit. Asserted in the builder, logged at boot.

## Sharpening pass (Fable, 2026-08-15) — what changed and why

1. **"The carve cannot follow a curve" was WRONG — deleted.** `terrain.h`'s
   `TerrainCorridor` is a polyline BY CONSTRUCTION: capsule-union per segment,
   crease-free at joints, tile-seam exact, and `--test-terraincorridor` C4
   already proves joint continuity. The straight-only limitation lives ONLY in
   `TunnelRoute`'s frame/grading layer (`tunnel_corridor.h`: one `dirX/dirZ`,
   "the frame is constant"). P1 shrinks accordingly: generalize the ROUTE
   layer, leave the carve primitive alone.
2. **"The graph already exists so this is mostly wiring" was OVERSOLD —
   reframed.** `EchoRoads::build()` is hard-wired to the Echo Harbor island
   `Heightfield` (`hf.heightAt` rim probes from `kCrownX/kCrownZ`, shore
   probing, mesa-rim topology, city blocks). It cannot be pointed at the
   unbounded terrain. What IS reusable — and it is a lot — is the data shapes
   (`RoadGraph/RoadEdge/RoadSample` with `bank`), the welded ribbon mesher,
   the zigzag law + curvature-derived rebank, the junction patcher, and the
   collision export. The route AUTHORING for mountain terrain is new work.
3. **"16 corridors buys 16 bends" was a misdiagnosis — corrected.** Each
   corridor is a 32-node polyline, so the registry buys ~496 bends today. The
   real limits are the TOTAL NODE BUDGET (16×32 nodes at 68 ft spacing ≈ 6.6
   miles of network) and per-tile evaluation cost. Consequence: **do not raise
   `kMaxNodes`; CHAIN 32-node corridors** (shared endpoint node; deepest-wins
   union makes the joint seamless and keeps every bounding box tight). Only
   `kMaxTerrainCorridors` rises.
4. **The 12–15 mile "still touches every biome" ring was FALSE — replaced.**
   The four ranges sit 4.3–5.7 miles out. A 12–15 mile ring is radius ~2
   miles: it never leaves the rolling country. Two honest options are given.
5. **New hazard: SWITCHBACK UNDERCUT.** Overlapping carves merge deepest-wins,
   so a hairpin's lower leg erases the upper leg's bench when legs come within
   ~150 ft laterally (2× the 75 ft carve influence width). New gate G5.
6. **Phases reordered** so the riskiest unknown (carve cost at scale) is
   measured FIRST, something drivable exists by P2, and the summit lot — the
   deliverable Tim judges by driving — lands in P3, not last.
7. **Cost arithmetic corrected**: a 31-mile ring at today's density is ~2,400
   nodes ≈ **5.4M** boot queries, not 1,300/2.9M. Recommendation: boot-time +
   job-system parallel + curvature-adaptive longitudinal sampling; carve-near-
   player is REJECTED (violates the registry's boot-only/read-only contract →
   seams + lost determinism).
8. **Added what was missing**: banked-deck carve interplay (the low edge digs
   ~3.5 ft deeper), superelevation runoff, LOD-at-distance (the ridge-LOD bug
   applies to cut edges; `terrainCorridorContains()` already exists to cap
   corridor tiles at Half LOD), streaming/boot contract, determinism gates,
   junction drive-through gate, and concrete summit-climb geometry grounded in
   AASHTO numbers so G3 is a choice between options, not an invention.

## Scale check

| | today | Tim's decided ask |
|---|---|---|
| drivable route | **0.4 miles** (2,100 ft), straight | ~62 miles of network |
| bends available | ~496 in the carve layer, route layer being unblocked NOW (P1 in flight) | switchbacks + 8+ junctions |
| network ceiling at current caps | **6.6 miles** (16 corridors × 32 nodes × 68 ft) | needs `kMaxTerrainCorridors` ≈ **192** |
| rivers | ONE (223 ft wide, 10.5 ft deep) | deep main channel + mountain creeks |
| bridges | **zero** | one 3-span showpiece + single-span creek crossings |

## THE RINGS ARE TOURS — the world has NO EDGE and the circles don't work

Tim drove **16.7 miles** at 100 mph and would not have hit an edge at any
distance: `app/terrain.h` is explicit — an unbounded world, a camera-centred
residency ring over infinite procedural terrain. A "ring" is therefore A LOOP
OF CHOSEN SHAPE, and (pass 2) the shape cannot be a circle for either loop:

The finite INTERESTING region, from `terrain.cpp`'s authored map:

| feature | position | note |
|---|---|---|
| N snow range | spine ~5.2 mi out, core band 4.8–5.5 mi | ~1,250 ft, jagged |
| E volcanic | spine ~5.7 mi, core 5.4–6.1 mi | ~1,500 ft, tallest |
| S mesa | spine ~5.6 mi, core 5.3–5.9 mi | ~640 ft, flat-capped |
| W crystal hills | spine ~5.3 mi, core 5.0–5.7 mi | ~1,050 ft, rolling |
| **tunnel ridge** | **0.3–0.7 mi from origin** | **~935 ft, over the existing bore** |
| the river | inside 0.9 mi of origin, runs SE to the sea | the only water a road can cross today |
| ocean basin | centre 1.1 mi SE, bowl radius 0.6 mi | the shore the inner tour visits |

* **INNER TOUR, ~15 mi**: hill country → OVER the tunnel ridge (the climb +
  summit lot ARE part of this loop) → past the city pads → across the river
  at N5–N6 (**Bridge No. 1**) → the ocean shore → back through the rolling
  country. Radius varies ~0.5–4 mi. This is pass 1's Option B with the bridge
  now explicit.
* **OUTER TOUR, ~31 mi**: an authored waypoint loop riding each range's FOOT
  bench (the outW aprons begin ~3.8 mi out; the loop works the 4–5.3 mi band
  where the foothills give constant views), cutting the diagonal gaps between
  ranges. A raw 5-mi-radius circle is 8.05 km out — INSIDE the N range's core
  band (7.8–8.8 km) and touching the W core (8.1 km): two accidental
  1,000 ft+ mountain crossings. If Tim WANTS a deliberate range crossing, it
  is ONE authored pass with G5-clean switch legs, not a side effect of a
  compass and string.
* **FOUR SPOKES** between the tours (≈ 2.6 mi each), eight junctions. Spokes
  cross the future CREEKS (R6) — that is where the single-span bridges live.

## The summit: a parking lot you drive UP to

The mountain is chosen for us: **the tunnel ridge** — climb over the top of the
very hill the bore goes under. Drive through it, then drive over it. The summit
is ~935 ft above the plain, near the centre of the map, next to everything.

"Nice curves we can speed around" means SWEEPERS, NOT HAIRPINS. A hairpin is a
15–25 mph corner; nothing about it is "speed around". Design geometry, grounded
in AASHTO curve mechanics (R ≈ V²/(15(e+f)), then checked against game grip
μ≈1.0 where flat-out speed ≈ √(gR)):

| element | number | why |
|---|---|---|
| signature sweepers | radius **1,200–1,600 ft**, 4–6 of them | holds 90–110 mph at game grip with bank; AASHTO 80 mph design ≈ 2,370 ft, game tires are grippier |
| technical pair below the summit | radius **500–700 ft** | 60–75 mph rhythm change so the climb isn't one note |
| hairpins | **none** by default | wrong feel for the ask; if Tim wants one rally moment, ONE, legs ≥ 200 ft apart (G5) |
| superelevation | up to **10°** on the sweepers, 6° on the technical pair | real e_max is ~5.7°; games read 8–12° as "banked" without reading as a wall |
| bank runoff | full bank develops over **≥ 250 ft** (≤ 1° per 25 ft) | snap-on bank is the #1 "procedural road" tell |
| grade | **6.5% average, 8% max pitch**, ≤ 4% inside curves tighter than 800 ft | see G3; flattening grade in tight curves is real practice and reads right |
| climb | **~920 ft vertical over ~2.7 miles** | falls straight out of 935 ft at 6.5% |
| arrival | final 180° sweep, radius ~300 ft, opening onto the lot | arrival should be a moment, not a stop |
| the lot | **350 × 250 ft** flattened pad, overlook edge on the bore axis | fits a 150 ft skidpad circle for messing around; you park above the portal you drove through |

The pad flattens the summit the way `kPads` flattens the city districts (same
blend construction, authored addition). Note the benching-does-not-read defect
(see NOT in this plan) is loudest exactly here — a parked car is the closest
eyes ever get to that rock.

## TOWNS + GAS STATIONS + LIGHTING POOLS (new chapter, pass 2)

**What makes a TOWN rather than scattered props — the minimum kit, in order of
load-bearing:**

1. **The road itself changes.** Lanes visually narrow (paint, curbs, a
   sidewalk ribbon), the paved edge gets kerbs instead of verge, and the
   change happens at an EDGE — a town has a beginning (sign, fence line,
   first lamp), not a density fade. Geometry does the speed-limit's job.
2. **Buildings FRONT the street.** Consistent setback, zero yaw jitter —
   every building faces the road it addresses. echo_roads' V8 machinery
   (`sampleFrontage`/`CityPlan`, position-derived seeds, footprint seating
   with the plinth/grade rejection rules) is the exact discipline; it ports
   over the new `RoadGraph` with the rest of the echo_roads functions. A town
   whose houses are hash-scattered and then deleted off the road is the slop
   this codebase already litigated once — do not relive it.
3. **The GAS STATION is the anchor.** One tall lit canopy + pumps + shop +
   forecourt apron, road-fronting. It is the most recognisable roadside
   silhouette that exists, day or night, and it earns its place at exactly two
   sites first: the bridge town and the climb foot ("last gas before the
   pass" — flavor for free). Asset source: check the converted packs FIRST
   (armory), else prims + surface_library concrete/metal + emissive canopy
   underside; do not order bespoke art before the layout proves itself.
4. **8–14 buildings minimum**, 2–3 models repeated with tint/rotation variance
   and varied setbacks. Below 8 it reads as a rest stop (which is also a
   valid, smaller thing: gas station + 2 buildings = a HAMLET at a spoke
   junction later).
5. **Ground truth**: each town sits on an authored PAD-CLASS flatten (the
   `kPads` blend construction at small radius) chosen on ≤ 4% natural grade,
   so buildings seat without plinth towers and the road crosses the pad blend
   without a seam.
6. **Night**: a lamp zone per town (`buildDistrictLamps` rows along the main
   street, `setGroundQuery` wired), warm-vs-cool per town character, plus
   window-glow pooled lights (`adoptCityGlows` — the mechanism exists) behind
   emissive window bands. **The country BETWEEN settlements stays DARK.**
   Rural highways are unlit in the real world; 31 miles of continuous lamps
   would be ~2,000 lamps of pure slop. Lighting pools at towns, junctions,
   the bridge and the summit lot — darkness between is what makes each pool
   an arrival.

**WHERE (decided by geography, not by the junction dogma):**
* **Town 1 — the bridge town**, NE bank at the N5–N6 crossing, outside the
  floodplain shelf. Rivers crossings are where real towns are; it also puts
  windows and lamps in the bridge's night capture background (B5 gets better
  for free).
* **Town 2 — the climb foot**, where the inner tour meets the summit road.
  Gas station anchor, "last gas" flavor.
* **Hamlets at spoke junctions** — LATER, with the outer ring (P9); a gas
  station + 2 buildings each, so the eight junctions read as places without
  eight full towns of authoring.

### Town conditions (new)
- [ ] T1. The anchor reads: the gas station is identifiable at 300 ft in
      daylight AND at night (canopy underglow), by capture.
- [ ] T2. Frontage discipline: every town building faces its street with the
      authored setback; zero yaw jitter; no building intersects the paved
      width + verge (asserted from the graph, not eyeballed).
- [ ] T3. Lighting pools: town lamps + window glow at night, and a capture
      from 1 mile out showing the town as a LIGHT POOL in dark country.
- [ ] T4. The pad: town site natural grade ≤ 4%; road crosses the pad blend
      with G1 still holding (no seam, no lip).
- [ ] T5. Drive-through feel: entering the town at speed, the geometry alone
      (kerbs, narrowing, lamps, buildings) says "slow down" — judged from the
      driver's seat capture, not asserted.

## What exists, honestly

* **Carve layer (`terrain.h/.cpp`) — READY.** Polyline corridors, crease-free,
  seam-exact, deterministic, deepest-wins union, per-corridor bbox early-out,
  `terrainCorridorContains()` for mesher decisions. C1–C5 tested. The river/
  canyon/ravine landforms are a SEPARATE authored layer applied before the
  corridors — they compose with roads by construction, except where a road
  must CROSS water (the span gap, above).
* **Route layer (`tunnel_corridor.h/.cpp`) — P1 IN FLIGHT.** Stations, grading
  (4.5% cap), the 27×83-per-node carve derivation that took
  `--test-tunnelmouth` to 7/7, portal machinery, multi-route `TunnelSpec`.
  The working tree already carries the polyline frame (`tangentAt`,
  `segmentAt`, local-tangent `worldAt`) — P1's exit gate below still stands
  and must be PASSED, not assumed.
* **Road art layer (`echo_roads.cpp`) — REUSABLE PARTS, WRONG WORLD.** The
  welded banked ribbon mesher, curvature law + rebank, junction patches,
  lane paint, collision export, frontage/city-plan machinery all exist and are
  battle-tested — against the Echo Harbor island heightfield. The graph
  BUILDER is a city generator (rim probes, shore finding, city blocks) and
  does not transplant. Plan on porting FUNCTIONS (ribbon/junction/law/
  frontage over a `RoadGraph`), not the builder.
* **Lighting (`street_lights.*`, `ClusterLights.h`) — READY.** Arbitrary
  terrain-seated lamp rows exist today (`buildDistrictLamps` +
  `setGroundQuery`); the lamp kit (post/head/cone/pool disc, deterministic
  dead/flicker variance) is the town-and-bridge kit as-is. Legacy light path
  caps 64 pooled; clustered path (1024) is one cvar away when the night
  world wants it. Doctrine: emissive-first, ≤ 6 pooled per structure.
* **Water + fish (`terrain.cpp`, `world_regions.cpp`, `fish.*`) — READY, ONE
  RIVER.** Carve + ribbon + water query + swim + god rays + zap all share the
  single authored spline. Fish are live-query depth-bound and already river-
  seeded. Single-river plumbing throughout (see R6).

## Phases

Resequenced (pass 2) so the riskiest unknowns still fail first, the bridge
lands right after the junction that forces its interface, and the biggest
low-novelty cost (the outer tour) goes last. P1 is fixed and in flight.

### P0 — MEASURE (half a day, before anything else lands)
Time, on the 14900k: (a) today's 71k-query derivation; (b) per-tile generation
with a long-bbox corridor registered vs without; (c) extrapolate to the
DECIDED scope: ~62 mi ≈ **4,800 nodes ≈ 10.8M queries, ~156 corridors**.
Exit: a table in this doc and a chosen sampling strategy (see COST). If the
numbers are fine, adaptive sampling is DEFERRED, not built on faith.

### P1 — POLYLINE ROUTE LAYER (in flight — the gate stands)
The carve already curves; generalize the ROUTE: per-station frame from the
polyline tangent, grading over the polyline, and carve registration that
CHAINS ≤32-node corridors with shared endpoint nodes instead of raising
`kMaxNodes`. Raise `kMaxTerrainCorridors` 16 → **192** (memory ~400
bytes/corridor — trivial; the cost that matters is per-tile evaluation, gated
in B2). Banked reaches deepen their carve by halfWidth × sin(bank) (~3.5 ft at
10°) so the low edge never meets dirt. Include the `bridge` reach flag's
PLUMBING (flag + span-gap suppression + roadY pinning) even though nothing
uses it yet — it is three small hooks now or a refactor later.
Exit gate (visible, can fail): the demo tunnel route re-expressed as a
polyline WITH a deliberate 30° bend passes the full G-suite, and one banked
curve stands on open ground.

### P2 — THE SWEEPER SPIKE (first drivable feel)
ONE 1,500 ft-radius banked curve on open terrain, full treatment: carve, verge
+ seam on a curve, ribbon collision, superelevation runoff. Drive it at
90 mph. This is 1/6 of the climb built early precisely so the feel, the seam,
and the banked-deck physics can fail CHEAP.

### P2R — DEEP RIVER + BANDED FISH (parallel lane — touches none of P1/P2's files)
R1 (per-node bed drop, constants unified), R2 re-densified, R4, R5 (species
bands), and the bank + underwater captures for R3's first half. Small,
independent, and the first thing from the new ask Tim can SEE. Do it while
P2 cooks.

### P3 — THE CLIMB + THE SUMMIT LOT (the payoff)
The tunnel-ridge hillclimb per the geometry table, plus the summit pad. Tim
drives it (E3 applies from here on, per phase). E1 capture. This ships alone:
a mountain road with a summit lot is a feature even if nothing after it lands.

### P4 — THE SHORE LEG + THE FIRST INTERSECTION
The inner tour's leg from the climb foot toward the ocean shore, stopping AT
the river's west bank — the near abutment IS this phase's endpoint, so the
bridge's interface is forced honestly. On the way: the network's first true
3-way junction (the junction carve union is free — deepest-wins — but the
surface patch/stop-bar/paint-trim machinery is a port from echo_roads). N1
gates here.

### P5 — BRIDGE No. 1 (the from-scratch assembly)
The 280 ft haunched-box crossing per the table: span gap (B7), deck + piers +
abutments + collision, welded abutment joints (B4), lighting kit (B5), B3/B6/
B8, and R3's deck capture. Exit: drive the shore leg across the river in both
directions at 60 mph; night capture from the NE bank.

### P6 — CLOSE THE INNER TOUR (~15 mi)
Close the loop through the shore and back. The scripted-drive integrity check
(N2 — reframed: it proves surface continuity, not lap time), G-suite at
scale, first at-scale budget look (B1/B2 trend line).

### P7 — TOWNS 1 + 2 AND THE LIGHTING POOLS
Bridge town, climb-foot town, gas stations, lamp zones, window glow, T1–T5.
The inner tour now has DESTINATIONS, which is what Tim actually asked the
roads to mean.

### P8 — RIVERS PLURAL (the registry) + CREEKS
R6: the river registry refactor, then 2–3 authored mountain creeks off the N
and W ranges toward the basin, each R2/R4-clean, each crossed by the FUTURE
outer tour/spokes at authored sites with single-span bridges (the kit's easy
case, now proven by P5). Creek-vs-road proximity obeys G5's 150 ft rule.

### P9 — THE OUTER TOUR + SPOKES (~31 mi + 4 × 2.6 mi)
The authored range-foot waypoint loop (NOT a circle — see THE RINGS ARE
TOURS), four spokes, eight junctions, junction hamlets, creek bridges. The
full at-scale gates run here: B1/B2/B3, D1, E4. This is the biggest single
cost and the least novel work in the plan, which is exactly why it is last.

## Acceptance conditions

Iterate until ALL hold. Each is a test, a log line, or a named capture.
(River R1–R6, bridge B3–B8 and town T1–T5 gates live in their own sections
above.)

### Geometry
- [ ] G1. At every station of every route: |carved floor − road datum| ≤
      **1.6 ft**, AND natural-minus-carve never rises above the datum anywhere
      across the paved width + a 3 ft verge, sampled at ≤ 2 ft longitudinally
      (the M1 discipline, applied per route). Bridge reaches check DECK-vs-
      datum ≤ 0.2 ft instead (the ground below is river, by design — B3).
- [ ] G2. The mouth invariant still holds on every curved corridor — the
      "no earth on the roadway" check runs per segment, INCLUDING the low edge
      of banked reaches. The 7/7 gate must not regress. Span gaps are skipped
      by construction, not by exception lists in the test.
- [ ] G3. Grade limits, concrete defaults (Tim picks a lane, not a number):
      **Interstate profile** — 6% max everywhere (AASHTO mountainous
      interstate); or **Alpine profile** — 6% mainline, 8% sustained on the
      climb, 10% only for runs under 200 ft (real alpine-pass practice).
      DEFAULT: Alpine for the climb, Interstate elsewhere. Additionally: ≤ 4%
      inside any curve tighter than 800 ft radius. Worst grade per route
      logged at boot.
- [ ] G4. Banking is real, not vestigial: every curve of radius ≤ 2,000 ft
      reaches ≥ 60% of its design superelevation; runoff develops over
      ≥ 250 ft (≤ 1° per 25 ft); bank returns to 0 on straights and across
      bridge decks; max 10°. Boot log per route: tightest radius / max bank /
      max grade.
- [ ] G5. **SWITCHBACK SEPARATION**: no two reaches whose road datums differ
      by > 6 ft pass within **150 ft** laterally (2× the 75 ft carve influence
      half-width). Applies equally to road-vs-CREEK carve pairs (P8).
      Asserted at boot; a violation is an authoring error.

### Network
- [ ] N1. At least one true 3-way intersection: the junction carve is ONE
      surface (deepest-wins gives this), the SURFACE patch has no seam, no
      z-fight, no lip over **0.2 ft** — and a scripted 60 mph drive through it
      shows no vertical acceleration spike over **0.5 g**.
- [ ] N2. Loop INTEGRITY (reframed per Tim — this proves the surface, not a
      lap time): a scripted car at 60 mph target completes each closed tour;
      wheel contacts on road/deck collision ≥ 99% of samples; never airborne
      > 0.5 s; no fall-through; start re-reached within 50 ft.
- [ ] N3. `--test-echoroads`' checksum still passes, or is deliberately
      re-baselined in its own commit with the diff explained.

### Budget
- [ ] B1. Boot-time carve derivation for the FULL network measured and logged
      in ms on the 14900k. Budget **500 ms** through P6; the P9 full-scope
      number may motivate lever 3 (see COST) — measure before reaching.
- [ ] B2. Per-tile generation cost with the full network registered stays
      within **+25%** of the no-road baseline (logged both ways). At ~156
      corridors the bbox early-out is ~624 float compares per height sample
      worst case: if B2 fails, the next lever is a coarse spatial grid over
      corridor bboxes (bin by 512 ft tiles at boot), not tighter boxes —
      chaining already made them tight.
- [ ] B3. LOD: tiles whose footprint intersects a corridor cap at Half
      LOD (`terrainCorridorContains()` already answers this per point, cheap).
      Rationale: the known ridge-LOD defect — 4 m-stride point sampling erases
      narrow crests — applies equally to a 58 ft-wide cut's edges, and a
      mountain road is mostly seen from far away.

### Determinism
- [ ] D1. Boot twice: identical network checksum (routes are authored
      polylines + pure derivation, so this is free unless someone breaks it).
      The climb route also passes an M6-style perturbation proof: re-derive
      against three shifted route centres, G1/G2 still hold. The bridge's
      span gap and the river registry are inside the checksum.

### Evidence
- [ ] E1. Capture from the summit lot looking back down the sweepers.
- [ ] E2. Capture of the intersection from the driver's seat.
- [ ] E3. Tim drives it — PER PHASE from P2 onward, not once at the end. The
      gates above exist to stop wasting his time before it is worth driving.
- [ ] E4. Capture of the climb from **2+ miles out**: the road must read
      as a continuous line on the mountain — no shimmer, no vanishing
      segments. This is the ridge-LOD bug's ambush point.
- [ ] E5. (new) The bridge night capture (B5's bank three-quarter) and the
      town light-pool capture (T3) join E1–E4 as the standing evidence set —
      these five images ARE "structure, realism, beauty" made checkable.

## Cost — numbers at the decided scope

The 27×83-per-node sampling is load-bearing (it is exactly what took the mouth
gate from 4/7 to 7/7: the carve must sample at least as finely as the
invariant is checked). Do not thin it blindly.

Scope arithmetic (pass 2): inner tour 15 mi + outer tour 31 mi + spokes
~10.4 mi + climb 2.7 mi + shore/connector legs ~3 mi ≈ **62 mi** at 68 ft node
spacing ≈ **4,800 nodes** ≈ **10.8M boot queries** ≈ ~156 chained corridors
(cap → 192). Estimated 3–6 s single-threaded. Recommendation, in order, each
step measured (P0/B1):

1. **Stay boot-time.** Carve-near-player is rejected outright: the corridor
   registry is boot-only/read-only BY CONTRACT (`terrain.h`) — that contract
   is what makes worker-thread tile generation race-free and tile seams
   bit-exact. Lazy carving trades a solved determinism story for a saved
   second.
2. **Parallelise over IJobSystem** — the derivation is pure per-node work;
   ÷8–16 on the 14900k. At full scope this alone lands ~400–750 ms —
   probably NOT under 500 ms, which is why lever 3 exists.
3. **Adaptive LONGITUDINAL sampling only** — dense (0.5 ft-class) where
   lateral relief or curvature is high, sparse on smooth ground (the two
   tours are mostly smooth ground — this is where the ÷3–5 actually lives);
   keep the lateral 27 as-is. G1's 2 ft check grid is the safety net that
   keeps "adaptive" honest.
4. **Disk cache** keyed by (terrain seed, route polyline hash) — the
   derivation is deterministic, so this is legal; it is also the last resort,
   not the first.

Plausible landing: 100–250 ms with (2)+(3). Through P6 (inner tour only,
~2,000 nodes), (2) alone should land under 500 ms — ship that and defer (3)
until P9's numbers demand it.

## Explicitly NOT in this plan

* The mountain steppes/benching defect (`kBluffStart` 180 ft, band 85 ft,
  strength 0.55 — textured, not stepped, in the saddle capture). Separate
  defect; it gets LOUD at the summit lot, which is one more reason P3 ships
  early enough to see it.
* Retiring `--test-tunneldrive`'s obsolete assertions (A2/A3/B1 assert the
  earth ramp and portal holes cut-and-cover deleted). Separate debt.
* Traffic, AI drivers, or anything that moves on the network.
* WATERFALLS (open ask, separate lane — feat/waterfalls exists on another
  box; when creeks land in P8, its cascade shader has real placement sites,
  but wiring it is that lane's call, not this plan's).
* The HIGH-SET bridge option's approach works (embankment meshes / viaduct
  spans) — deferred until a crossing earns the majesty.
* Boats, river navigation, water flow simulation.
