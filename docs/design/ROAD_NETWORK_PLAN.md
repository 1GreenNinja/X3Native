# Plan — a road NETWORK: longer, curving, over the pass, and back

Status: DRAFT v2, pre-execution. Conditions written before any code, on purpose.
Author: InspectorX, 2026-08-15. Sharpened: Fable, 2026-08-15.
Ask: Tim —

> "We NEED a LONGER ROAD.... and then a road that curves up over the mountain
> through passes... goes somewhere else... circles back.. intersections..."
> "...a road that goes up to a parking lot on top of the mountain, with nice
> curves we can speed around"

UNITS: **feet and miles**, per Tim — feet for anything you could stand next to,
miles once it is a journey, mph for speed. Engine data stays SI; conversion at
the boundary only.

## DECIDED BY TIM, 2026-08-15 (after the sharpening pass — these WIN)

1. **BOTH rings, not one.** *"15 mile ring"* … *"Still make the big ring too"*.
   * INNER ~15 miles, radius ~2.4 miles — the tunnel ridge, the pass, the summit
     lot. Rolling country the whole way; it does NOT reach the ranges, and that
     is accepted, not an oversight.
   * OUTER ~31 miles, radius ~5 miles — tours all four ranges (N snow, E
     volcanic, S mesa, W crystal hills).
   * **The SPOKES between them are where the intersections come from free.**
     Four radial connectors give eight junctions without inventing a reason for
     roads to meet — which is exactly the structure Tim asked for.

2. **THE GOAL IS NOT LAP TIME.** *"Lap times dont matter so much now.. we just
   need roads for structure and realism and beauty."* This retires the racing
   framing. A closed loop is no longer proof of anything; the question is
   whether the world reads as a PLACE. Every acceptance condition phrased as
   "the car returns to the start" is downgraded, and the load-bearing gates
   become the ones about how it LOOKS and whether the network makes sense as
   built infrastructure.
   * Consequence: the 31-mile outer ring stops being a "19-minute lap" problem.
     Long is fine. Structure is the point.

3. **STREET LIGHTS, SMALL TOWNS, GAS STATIONS.** *"We will add street lights,
   small towns with gas stations."* Settlements are what make a road mean
   something — a road with no destination is a racetrack. Towns want: a reason
   to be where they are (junction, pass foot, water), lighting that reads at
   night, and a gas station as the recognisable anchor.
   * `street_lights.cpp` already exists in the app layer — check before writing.
   * The junctions from (1) are the natural town sites.

4. **SPEED IS NOT THE MEASURE, and the numbers we have are unreliable.** Tim
   driving: *"It never went past 120 in the non turbo version."* A headless
   probe reporting 370 mph was INVALID — it ran on a 984 ft slab, drove off the
   edge, and sampled the car falling. There is no aero drag on the wheeled
   vehicle (verified in code, not by probe), so nothing sets a terminal speed
   but gearing and terrain. Do not tune the road to a speed we have not honestly
   measured on real road.

## RIVERS, FISH, BRIDGES — added by Tim 2026-08-15

> "AHH yes.. Make rivers.. Deep ones, with fish .. and Beautiful lit concrete
> bridges"

This belongs in THIS plan, not a separate one: a bridge exists precisely where a
road meets a river, so the crossings are decided by the ring/spoke layout above.
Rivers are also what stop a 31-mile ring from being a circle on a lawn.

**What is REAL today — checked in the source, not assumed. (I oversold reuse
once already on echo_roads; this is the audited version.)**

* **The river carve EXISTS and is good.** `terrain.h:116-131`: `WorldRiverNode
  {x, z, waterY}` is ONE authored spline shared by the height-field carve
  (`terrain.cpp kRiver*`) and the water ribbon (`world_regions.cpp`), so the
  water can never sit outside its own channel. `waterY` descends monotonically
  downstream — it actually flows. An authored levee term holds the bank crests
  above the water where the natural country is low.
  Current dimensions: **223 ft wide**, bed **10.5 ft** below the surface, banks
  **7.2 ft** proud. And `TerrainCorridor` is explicitly "the polyline
  generalization of the river carve" — the roads and the river are the SAME
  primitive, which is why they will compose.
* **The fish EXIST, and they are RIVER fish.** `app/fish.cpp` (38 KB): rudd,
  bream, perch — hand-sized — plus a metre-long pike that spawns ALONE as an
  ambush predator. Real Rodin species GLBs with pose-baked swim and a PBR
  fallback; the body is a lofted three-piece hull that S-flexes at two hinges,
  laterally compressed so it shows a knife edge from above and a slab from the
  side. Already built through `worldFish.build()` in `app_run.cpp`.
* **BRIDGES DO NOT EXIST.** Every `bridge` hit in the codebase is INTERIOR
  facility geometry (corridor spans in `level_loader.cpp`, tube gaps in
  `cave_atmosphere.cpp`). There is no road bridge, no pier, no abutment, no
  deck. This is the genuinely new build.
* **There is exactly ONE river.** "Rivers" plural is new authoring.

**So the honest split:** the channel and its fish are a tuning-and-multiplying
job; the bridges are a from-scratch build.

### The interaction that will bite, and it is the same one as the switchbacks

Corridors merge **deepest-wins**. A river carve and a road carve that cross will
BOTH cut, and the river is deeper — so a road crossing a river gets its roadbed
erased and the ring drives into the water. **A bridge is not decoration here; it
is the only correct answer to the crossing**, and the road must be excluded from
carving across the span (deck carried on geometry, not on ground).

That is the same deepest-wins hazard as gate G5 (switchback undercut), which
means one mechanism explains both and one fix can serve both.

### River conditions
- [ ] R1. DEEPER: bed at least **20 ft** below the water surface in the main
      channel (today 10.5 ft), with banks that still read as banks — measured by
      probe across the channel, not by eye.
- [ ] R2. The water surface stays INSIDE its channel for the whole run: no
      sample where the terrain crest is below `waterY`. This is the river's
      equivalent of "no earth on the roadway" and it must be a gate.
- [ ] R3. Fish are visibly in the river (not just constructed): a capture from
      the bank AND one from a bridge deck looking down.
- [ ] R4. `waterY` still descends monotonically after any re-authoring — a river
      that flows uphill is the one defect nobody forgives.

### Bridge conditions
- [ ] B3. A road crosses the river on a DECK, and the terrain under the span is
      untouched river — no carve, no fill, no z-fight.
- [ ] B4. The deck is drivable end to end: a car crosses without leaving the
      surface and without a step over **0.2 ft** at either abutment.
- [ ] B5. LIT: the bridge reads at night. Lights on the deck, and the concrete
      catches them — this is the "beautiful" half of the ask and it is judged by
      capture, at night, not asserted.
- [ ] B6. Piers stand ON the riverbed at their true height (they are carved
      terrain's problem, not a floating mesh) and do not dam the water ribbon.

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

| | today | Tim's ask |
|---|---|---|
| drivable route | **0.4 miles** (2,100 ft), straight | miles, curving, looping |
| bends available | ~496 in the carve layer, **0 in the route layer** | switchbacks + intersections |
| network ceiling at current caps | **6.6 miles** (16 corridors × 32 nodes × 68 ft) | ~15–33 miles |

## What "around the world" can mean — the world has NO EDGE

Tim drove **16.7 miles** at 100 mph and would not have hit an edge at any
distance: `app/terrain.h` is explicit — an unbounded world, a camera-centred
residency ring over infinite procedural terrain. "Around the world" therefore
means A LOOP OF CHOSEN SIZE, and choosing it is design, not tech.

The finite INTERESTING region is already authored. `terrain.cpp`'s five ranges:

| range | position | peak height above the plain |
|---|---|---|
| N snow | ~5.2 miles north | ~1,250 ft, jagged |
| E volcanic | ~5.7 miles east | ~1,500 ft, tallest |
| S mesa | ~5.6 miles south | ~640 ft, flat-capped |
| W crystal hills | ~5.3 miles west | ~1,050 ft, rolling |
| **tunnel ridge** | **near centre, over the existing bore** | **~935 ft** |

RING SIZE IS TIM'S CALL — but between two HONEST options:

* **Option A — the Grand Tour, ~29–33 miles.** Radius ~4.7–5.3 miles, actually
  reaches all four ranges. ~19-minute lap at 100 mph — long between landmarks.
* **Option B — the Home Circuit, ~13–15 miles.** Hill country + OVER the tunnel
  ridge + past the city pads + the ocean shore. It does NOT touch the outer
  ranges (they are 4.3+ miles out — the draft's claim that a tight ring
  "touches every biome" was wrong); spur roads to each range come later and
  give four out-and-back drives instead of one huge lap.

Default proposal: **B now, spurs later** — the loop stays dense with things to
see, and every spur is independently shippable. Ask before building.

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

## What exists, honestly

* **Carve layer (`terrain.h/.cpp`) — READY.** Polyline corridors, crease-free,
  seam-exact, deterministic, deepest-wins union, per-corridor bbox early-out,
  `terrainCorridorContains()` for mesher decisions. C1–C5 tested.
* **Route layer (`tunnel_corridor.h/.cpp`) — STRAIGHT ONLY.** Stations,
  grading (4.5% cap), the 27×83-per-node carve derivation that took
  `--test-tunnelmouth` to 7/7, portal machinery. The frame is one constant
  heading; `worldAt(s, lat)` cannot turn. THIS is the blocker, and only this.
* **Road art layer (`echo_roads.cpp`) — REUSABLE PARTS, WRONG WORLD.** The
  welded banked ribbon mesher, curvature law + rebank, junction patches,
  lane paint, collision export all exist and are battle-tested — against the
  Echo Harbor island heightfield. The graph BUILDER is a city generator
  (rim probes, shore finding, city blocks) and does not transplant. Plan on
  porting FUNCTIONS (ribbon/junction/law over a `RoadGraph`), not the builder.

## Phases

Reordered so the riskiest unknowns fail first and something is drivable early.

### P0 — MEASURE (half a day, before anything)
The riskiest unknown is carve cost at scale, and the draft had it unmeasured at
the end. Time, on the 14900k: (a) today's 71k-query derivation; (b) per-tile
generation with a long-bbox corridor registered vs without; (c) extrapolate to
2,400 nodes / ~96 corridors. Exit: a table in this doc and a chosen sampling
strategy (see COST). If the numbers are fine, adaptive sampling is DEFERRED,
not built on faith.

### P1 — POLYLINE ROUTE LAYER (the real unblock, smaller than drafted)
The carve already curves; generalize the ROUTE: per-station frame from the
polyline tangent (`posAt`/`worldAt` walk arc length), grading over the
polyline, and carve registration that CHAINS ≤32-node corridors with shared
endpoint nodes instead of raising `kMaxNodes`. Raise `kMaxTerrainCorridors`
16 → 96 (memory ~400 bytes/corridor — trivial; the cost that matters is
per-tile evaluation, gated in B2). Banked reaches deepen their carve by
halfWidth × sin(bank) (~3.5 ft at 10°) so the low edge never meets dirt.
Exit gate (visible, can fail): the demo tunnel route re-expressed as a
polyline WITH a deliberate 30° bend passes the full G-suite, and one banked
curve stands on open ground.

### P2 — THE SWEEPER SPIKE (first drivable feel)
ONE 1,500 ft-radius banked curve on open terrain, full treatment: carve, verge
+ seam on a curve (the straight case needed a skirt and a verge; the curved
case gets per-station frames from P1), ribbon collision, superelevation
runoff. Drive it at 90 mph. This is 1/6 of the climb built early precisely so
the feel, the seam, and the banked-deck physics can fail CHEAP. If the seam
strategy is wrong, we find out here, not on a 2.7-mile mountain.

### P3 — THE CLIMB + THE SUMMIT LOT (the payoff, moved up)
The tunnel-ridge hillclimb per the geometry table, plus the summit pad. Tim
drives it (E3 applies from here on, per phase, not only at the end). E1
capture. This ships alone: a mountain road with a summit lot is a feature even
if nothing after it lands.

### P4 — SOMEWHERE ELSE + THE FIRST INTERSECTION
A destination with a reason: proposal — the ocean shore at the basin (~1.2
miles from the ridge; water already exists there), reached via the network's
first true 3-way junction (junction carve union is free — deepest-wins — but
the surface patch/stop-bar/paint-trim machinery is a port from echo_roads,
not a rewrite). N1 gates here.

### P5 — THE LOOP
Close the ring (Option A or B — Tim's call from the ring section), N2 lap
test, and the at-scale gates (B1/B2/B3, D1) run on the full network.

## Acceptance conditions

Iterate until ALL hold. Each is a test, a log line, or a named capture.

### Geometry
- [ ] G1. At every station of every route: |carved floor − road datum| ≤
      **1.6 ft**, AND natural-minus-carve never rises above the datum anywhere
      across the paved width + a 3 ft verge, sampled at ≤ 2 ft longitudinally
      (the M1 discipline, applied per route).
- [ ] G2. The mouth invariant still holds on every curved corridor — the
      "no earth on the roadway" check runs per segment, INCLUDING the low edge
      of banked reaches. The 7/7 gate must not regress.
- [ ] G3. Grade limits, concrete defaults (Tim picks a lane, not a number):
      **Interstate profile** — 6% max everywhere (AASHTO mountainous
      interstate); or **Alpine profile** — 6% mainline, 8% sustained on the
      climb, 10% only for runs under 200 ft (real alpine-pass practice).
      DEFAULT: Alpine for the climb, Interstate elsewhere. Additionally: ≤ 4%
      inside any curve tighter than 800 ft radius. Worst grade per route
      logged at boot.
- [ ] G4. Banking is real, not vestigial: every curve of radius ≤ 2,000 ft
      reaches ≥ 60% of its design superelevation; runoff develops over
      ≥ 250 ft (≤ 1° per 25 ft); bank returns to 0 on straights; max 10°.
      Boot log per route: tightest radius / max bank / max grade.
      (The draft's G4 passed trivially if the authored bank was tiny.)
- [ ] G5. **SWITCHBACK SEPARATION** (new): no two reaches whose road datums
      differ by > 6 ft pass within **150 ft** laterally (2× the 75 ft carve
      influence half-width). Asserted at boot; a violation is an authoring
      error — the lower carve undercuts the upper bench and the upper road
      floats over a hole.

### Network
- [ ] N1. At least one true 3-way intersection: the junction carve is ONE
      surface (deepest-wins gives this), the SURFACE patch has no seam, no
      z-fight, no lip over **0.2 ft** — and a scripted 60 mph drive through it
      shows no vertical acceleration spike over **0.5 g**.
- [ ] N2. The loop closes, MEASURABLY (the tunneldrive pattern, not eyeball):
      a scripted car at 60 mph target completes the full lap; wheel contacts
      are on road collision ≥ 99% of samples; never airborne > 0.5 s; no
      fall-through; start position re-reached within 50 ft.
- [ ] N3. `--test-echoroads`' checksum still passes, or is deliberately
      re-baselined in its own commit with the diff explained.

### Budget
- [ ] B1. Boot-time carve derivation for the FULL network measured and logged
      in ms on the 14900k. Budget **500 ms**. If over: job-system parallel
      first, adaptive sampling second, disk cache keyed by (seed, route hash)
      third — in that order, each measured before reaching for the next.
- [ ] B2. Per-tile generation cost with the full network registered stays
      within **+25%** of the no-road baseline (logged both ways). This is the
      REAL cost of raising `kMaxTerrainCorridors` — memory is noise. If it
      fails: per-corridor bboxes are already tight from chaining; next lever
      is a coarse spatial grid over corridor bboxes.
- [ ] B3. (new) LOD: tiles whose footprint intersects a corridor cap at Half
      LOD (`terrainCorridorContains()` already answers this per point, cheap).
      Rationale: the known ridge-LOD defect — 4 m-stride point sampling erases
      narrow crests — applies equally to a 58 ft-wide cut's edges, and a
      mountain road is mostly seen from far away.

### Determinism
- [ ] D1. (new) Boot twice: identical network checksum (routes are authored
      polylines + pure derivation, so this is free unless someone breaks it).
      The climb route also passes an M6-style perturbation proof: re-derive
      against three shifted route centres, G1/G2 still hold.

### Evidence
- [ ] E1. Capture from the summit lot looking back down the sweepers.
- [ ] E2. Capture of the intersection from the driver's seat.
- [ ] E3. Tim drives it — PER PHASE from P2 onward, not once at the end. The
      gates above exist to stop wasting his time before it is worth driving.
- [ ] E4. (new) Capture of the climb from **2+ miles out**: the road must read
      as a continuous line on the mountain — no shimmer, no vanishing
      segments. This is the ridge-LOD bug's ambush point.

## Cost — corrected numbers and the recommendation

The 27×83-per-node sampling is load-bearing (it is exactly what took the mouth
gate from 4/7 to 7/7: the carve must sample at least as finely as the
invariant is checked). Do not thin it blindly.

Corrected arithmetic: a 31-mile ring at 68 ft node spacing is ~**2,400 nodes**
≈ **5.4M** height queries (the draft undercounted at 1,300/2.9M). Estimated
1.5–3 s single-threaded. Recommendation, in order, each step measured (P0/B1):

1. **Stay boot-time.** Carve-near-player is rejected outright: the corridor
   registry is boot-only/read-only BY CONTRACT (`terrain.h`) — that contract
   is what makes worker-thread tile generation race-free and tile seams
   bit-exact. Lazy carving trades a solved determinism story for a saved
   second.
2. **Parallelise over IJobSystem** — the derivation is pure per-node work;
   ÷8–16 on the 14900k.
3. **Adaptive LONGITUDINAL sampling only** — dense (0.5 ft-class) where
   lateral relief or curvature is high, sparse on smooth ground; keep the
   lateral 27 as-is. Expected ÷3–5. G1's 2 ft check grid is the safety net
   that keeps "adaptive" honest.
4. **Disk cache** keyed by (terrain seed, route polyline hash) — the
   derivation is deterministic, so this is legal; it is also the last resort,
   not the first.

Plausible landing: 30–120 ms. If P0 says today's cost extrapolates under
500 ms with (2) alone, ship that and stop.

## Explicitly NOT in this plan

* The mountain steppes/benching defect (`kBluffStart` 180 ft, band 85 ft,
  strength 0.55 — textured, not stepped, in the saddle capture). Separate
  defect; it gets LOUD at the summit lot, which is one more reason P3 ships
  early enough to see it.
* Retiring `--test-tunneldrive`'s obsolete assertions (A2/A3/B1 assert the
  earth ramp and portal holes cut-and-cover deleted). Separate debt.
* Traffic, AI drivers, or anything that moves on the network.
* Spur roads to the four outer ranges (follow-on to Option B).
