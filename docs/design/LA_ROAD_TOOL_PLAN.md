# Plan — draw roads like SimCity, top-down, in Level Architect

Status: DRAFT, pre-execution. Written before code, per the discipline.
Author: InspectorX, 2026-08-15.

> "Give me a tool that I can draw roads like simcity, in Level Architect, from
> top down view!" — Tim
> "Show terrain and mountains in LA" — Tim

UNITS: feet and miles in anything Tim reads. Engine data stays SI.

## Why this is the right thing to build NOW

Tim's standing architecture direction was *"generators Generate levels that can
be Edited in Level Architect… which will make it easier to correct already good
stuff"*. This is that idea applied to the thing we are about to author 62 miles
of: **roads**.

Every alternative is worse. Hand-authoring 62 miles of waypoints as C++
constants means a rebuild per tweak and no way to see what you did. A procedural
ring is exactly the "generated, not designed" slop Tim rejects. Drawing the
route over the real terrain, seeing the ridges you are routing around, is how a
road gets a REASON to be where it is — which is the whole stated goal
("structure and realism and beauty").

And the consumer side is already done: **P1 (442626ab) made `TunnelRoute`'s
frame follow a polyline**, so a drawn polyline has something to become.

## What exists, verified — not assumed

* `LevelDoc` = `entities[]` (points: pos/yaw/kind/script) + `brushes[]`
  (BlockoutBrush: box/ramp/cylinder/stairs). **There is no polyline type.**
* **There is no top-down or orthographic view.** No `ortho`, no `topDown`
  anywhere in `app/editor/`.
* **The editor draws no terrain.** It shows entities and brushes; the ground is
  not part of the document or the view.
* LA 11.0 DOES give us the pattern to copy: `"portal"` entities carry a
  `script` LINK ID (`"tunnel_a_west"`) that consumers use to pair them. Road
  nodes can use the same mechanism for junctions.
* Level Architect is its OWN EXE now (`X3LevelArchitect.exe`, 45bfba1d), over
  the shared `x3app.dll`, so editor work cannot drift from the game.

## The four pieces, in dependency order

### T1 — TERRAIN IN THE VIEWPORT (the prerequisite)
Tim asked for this second but it comes first: a road tool over an empty grid is
useless. The editor must render the real height field — the same
`terrainHeightAtWorld()` the game carves — plus the mountain ranges, so the
author can see what they are routing around.

Cheapest honest version: a top-down HEIGHT RASTER of the region, shaded by
elevation with contour banding, rebuilt when the view moves. It does not need
the game's splat/relief pipeline; it needs to be READABLE and true.

### T2 — TOP-DOWN ORTHOGRAPHIC CAMERA
A plan view locked to +Y-down, pan and zoom, with a scale readout in FEET and
MILES. This is where SimCity's feel comes from: you are looking at a map, not
flying a camera.

### T3 — THE ROAD POLYLINE TYPE
New `roads[]` array in `LevelDoc`, round-tripped by the same `%.9g` serializer
LA 11.0 already fixed. Minimum per road: an id/name, a class (freeway / avenue
/ ramp, matching `RoadClass`), a width, and an ordered list of nodes. Nodes
carry a link id so two roads sharing one become a JUNCTION — the same trick the
portal pairing already uses.

### T4 — THE DRAWING TOOL
Click to place nodes, drag to move, insert/delete mid-run, and a curve control
so a bend is a sweeper rather than a dogleg. Live readout while drawing:
segment length in feet, total in miles, and **grade in percent** from the
terrain under the polyline — because a road that cannot be driven is the failure
mode this tool exists to prevent.

### T5 — DRAG CITIES FROM PREFABS (Tim, 2026-08-15: "Drag cities from the prefabs possibly?")

The natural extension: if you can draw the road, you should be able to drop the
places the road GOES. Towns, gas stations, and the two named cities become
things you drag onto the plan view rather than coordinates in a header.

**Verified state: there is NO prefab system.** No `prefab` anywhere in
`app/editor/`. The city side exposes generator entry points
(`registerCityFreewayTunnels`, the city builders), not a placeable asset. So a
"prefab" here means something new and it needs defining rather than assuming:

* the honest minimum is a NAMED GENERATOR CALL WITH A TRANSFORM — a LevelDoc
  entry that says "a city of this preset, centred here, rotated so, with this
  seed", which the world builder then runs. That is a prefab in the sense that
  matters (drag it, move it, the world rebuilds around it) without pretending we
  have a baked-asset library we do not have;
* the LEVEL of prefab matters and should be Tim's call: whole city, a town
  block, or single buildings/gas stations. Roads meeting towns is the structure
  he asked for, so TOWN-scale is probably the sweet spot, with the two big
  cities as their own thing;
* junctions from the road tool are the natural town anchors — a settlement wants
  a reason to be where it is.

This is sequenced AFTER T1-T4: drawing roads over visible terrain is the
load-bearing feature, and a prefab you cannot place along a road is not useful.

## Acceptance conditions

- [ ] A1. Terrain is visible in the LA viewport and matches the game: sample
      `terrainHeightAtWorld` at 20 random points in view and assert the raster
      agrees within **3 ft**. Not "it looks like a mountain".
- [ ] A2. The four mountain ranges are identifiable by eye in plan view, at a
      zoom that shows the whole 11 x 11 mile feature box.
- [ ] A3. Scale is honest: a known 2,100 ft distance (the demo bore) measures
      2,100 ft ± 20 ft with the on-screen ruler.
- [ ] A4. A drawn road round-trips through save/load **bit-exactly** —
      `%.9g`, same as LA 11.0's fix; a node at x = 123.4567 must not come back
      as 123.5.
- [ ] A5. A road drawn in LA becomes a real carved corridor in the game with no
      hand-editing: deviation between the drawn centreline and the carved one
      under **1.6 ft** (this is G1 from the road plan, and it is the whole
      point of the tool).
- [ ] A6. The grade readout is correct: draw a road up the known tunnel ridge
      and confirm the reported percent matches a probe of the height field.
- [ ] A7. `--test-editor` stays green (23/23 as of the LA 11.0 fold) and the
      existing LevelDoc round-trip test still passes with `roads[]` present.

## Explicitly NOT in this plan

* Auto-routing / pathfinding. Tim wants to DRAW, not to press "generate".
* Junction geometry (the surface patch, stop bars, paint). The tool records
  that two roads share a node; building the junction is the road network's job.
* Editing terrain from LA. Read-only ground here.
