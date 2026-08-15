# Plan — a road NETWORK: longer, curving, over the pass, and back

Status: DRAFT, pre-execution. Conditions written before any code, on purpose.
Author: InspectorX, 2026-08-15. Ask: Tim —

> "We NEED a LONGER ROAD.... and then a road that curves up over the mountain
> through passes... goes somewhere else... circles back.. intersections..."

UNITS: **feet and miles**, per Tim — feet for anything you could stand next to
(kerbs, widths, node spacing), miles once it is a journey (route length, network
scale), mph for speed. Engine data stays SI; conversion happens at the boundary
only, never in the code.

Scale check for this ask, in those units:

| | today | Tim's ask |
|---|---|---|
| drivable route | **0.4 miles** (2,100 ft), straight | miles, curving, looping |
| bends available | **0** (corridor is straight by construction) | switchbacks + intersections |
| absolute ceiling on straights | **6.4 miles** (16 corridors x 0.4) | not a way to get curves |

## The good news: the graph already exists, and it is switched off

`app/world_hosts/echo_roads.h` already defines exactly the thing being asked
for, and it is far more than a stub:

* `RoadGraph { nodes, edges }` — **nodes ARE intersections** (positional joins).
* `RoadEdge` carries a real curved centerline: `std::vector<RoadSample>`,
  arc-length-even at ~13 ft (~6.5 ft on ramps), each sample with a unit tangent
  AND a `bank` angle (superelevation — the road leans into its curves).
* `RoadClass { Freeway, Ramp, Avenue, HarborStreet }` — freeway is an elevated
  2+2 deck with barriers, banked curves and pillar rows; **Ramp already exists
  as the interchange link that grades deck-to-ground**.
* Lane maths (`laneOffset`) with right-hand traffic already worked out.

It is deliberately UNWIRED. Per `app/CMakeLists.txt`, the sole consumer today is
`--test-echoroads`, which locks the emitted graph to a checksum. Nothing draws
it, nothing carves terrain for it, no host boots it.

**So "curving roads with intersections" is mostly a WIRING job, not an invention
job.** That is the single most important fact in this document.

## The blocker: the CARVE cannot follow a curve

The road can curve. What cannot curve is the terrain corridor that makes the
ground accept a road. From `app/tunnel_corridor.h`:

```
float dirX = 1.0f, dirZ = 0.0f;   // unit XZ heading (constant — a straight run)
...
// The corridor is a straight run, so the frame is constant.
```

Three hard limits, all in the corridor layer, none in the road layer:

| limit | value | what it costs us |
|---|---|---|
| `TunnelRoute` heading | **straight only** | a road cannot bend into a pass |
| `TerrainCorridor::kMaxNodes` | **32** | 2,100 ft at 68 ft spacing |
| `kMaxTerrainCorridors` | **16** | at best ~6.4 miles of straight segments |

Chaining straight corridors is not a way out: a switchback road up a pass wants
a bend every few hundred feet, and 16 corridors buys 16 bends.

## Cost warning, from yesterday's fix

The carve now samples the natural surface at the resolution the invariant is
checked at (27 x 83 per node — that is what took the mouth gate to 7/7). That is
~71,000 height queries for today's 32 nodes. A 5-mile network at the same node
density is ~1,300 nodes and ~2.9 MILLION queries at boot.

That is not fatal (pure function, boot-time, parallelisable over the job system)
but it MUST be measured, not assumed, and it is a real reason to make the sample
window adaptive: dense where the ground is rough, sparse where it is smooth.

## Phases

Sequenced so each one is independently useful and independently verifiable. If
the work stops after any phase, what shipped still works.

### P1 — POLYLINE corridors (the unblock)
Give `TerrainCorridor`/`TunnelRoute` a per-node heading instead of one route
heading. `posAt`/`worldAt` walk the polyline by arc length; the frame becomes
per-node instead of constant. Raise `kMaxNodes` from 32 to whatever the carve
cost supports (measure first — see A5).

Nothing visible ships here. This is the load-bearing change and everything else
waits on it.

### P2 — LONGER, still straight-ish
Take the demo route from 2,100 ft to ~1 mile using the new node budget. Proves
the polyline maths on geometry we already understand before adding curvature.

### P3 — THE PASS
A curving climb over the ridge: switchbacks, real grade limits, banked curves
(the `bank` field is already in `RoadSample` and already unused). This is the
first phase Tim can drive and feel.

### P4 — SOMEWHERE ELSE
A second destination on the far side. Not scenery — a place with a reason to
drive to it.

### P5 — THE LOOP + INTERSECTIONS
Circle back, and make `RoadNode` junctions real on the ground: merged carve at
the join, no z-fighting deck-on-deck, drivable through the intersection.

## Acceptance conditions

Iterate until ALL hold. Each is checkable by a test, a log line, or a named
capture. None is "looks good".

### Geometry
- [ ] G1. A corridor follows a polyline: max deviation between the carved
      centreline and the authored centreline under **1.6 ft** anywhere.
- [ ] G2. The mouth invariant STILL holds on every curved corridor —
      `--test-tunnelmouth`'s "no earth on the roadway" check, run per segment.
      This is the gate that just went 4/7 -> 7/7; it must not regress.
- [ ] G3. No carved segment exceeds the drivable grade. Pick the number with
      Tim: a real mountain highway tops out near **7 %**, hairpins ~10 %.
- [ ] G4. Banked curves actually bank: `RoadSample::bank` reaches the authored
      superelevation on the tightest curve and returns to 0 on the straights.

### Network
- [ ] N1. At least one true intersection where three or more edges meet, and the
      carve at the junction is ONE surface — no seam, no z-fight, no lip over
      **0.2 ft**.
- [ ] N2. The loop closes: a car driven from the start reaches the far
      destination and returns to the start without leaving the road surface.
- [ ] N3. `--test-echoroads`' existing checksum still passes, or is deliberately
      re-baselined in its own commit with the diff explained.

### Budget
- [ ] B1. Boot-time carve cost measured and logged in ms, before and after.
      If it exceeds ~500 ms, make the sampling adaptive rather than shipping it.
- [ ] B2. Corridor count stays inside `kMaxTerrainCorridors`, or that constant is
      raised deliberately with its memory cost stated.

### Evidence
- [ ] E1. One capture from the pass summit looking back down the switchbacks.
- [ ] E2. One capture of an intersection from the driver's seat.
- [ ] E3. Tim drives the loop. That is the acceptance that counts; the gates
      above exist to stop me wasting his time before it is worth driving.

## Explicitly NOT in this plan

* The mountain steppes. Benching constants exist (`kBluffStart` 180 ft,
  `kBluffBandH` 85 ft, strength 0.55) but do NOT read in the saddle capture —
  the slope is textured, not stepped. That is its own defect, filed separately,
  and it matters MORE once a road climbs the thing at eye level.
* Retiring `--test-tunneldrive`'s obsolete assertions (A2/A3/B1 assert an earth
  ramp and portal holes that cut-and-cover deleted). Separate debt.
* Traffic, AI drivers, or anything that moves on the network.
