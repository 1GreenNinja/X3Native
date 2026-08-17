# W-WATER lane handoff (Lane 2, Seven-Lane Plan)

Worktree `agent-aa28d458b82af9a3b`, branch `worktree-agent-aa28d458b82af9a3b`,
base a1400d67. Build dir EXISTS and is configured with the vcpkg toolchain
(gotcha 1.4 already paid). NEVER push — the session lead merges.

## Status: tasks #32 and #23 DONE and gated. The SUB is the remaining item.

Commits (all local):
- 5c38387d one water truth — river mode in the water pass, shims deleted, rain runoff
- bfa46f17 the last split-truth shims die — hulls and fish ride the real river
- 8e41b66e the station sweep RB8/RB9/RB10 (and the real bug it found)

### What was wrong and what fixed it
The drawn Gerstner patch was FLAT at the bridge's waterY while the carved
channel descends ~1.2 m per chain node, so downstream the painted water climbed
the banks (receipt: a bench shipped submerged at (-340,11,-468) while PASSING
worldWaterLevelAt+0.5). `WaterParams` now carries the river's own node polyline
+ half-width + the ocean basin disc; `water.vert` runs the CPU's polyClosest in
GLSL per vertex, so the surface steps down the channel, fades out across the
waterline (vMask, fragment discards past it), and hands the estuary to the sea.
`riverNodeCount == 0` keeps every ocean world byte-identical.

Shims deleted (rule 4): `RoadTrees::build` minBenchY + host `benchDryY`; the
player feed's 260 m clamp (only the +0.35 m presentation bias survives);
river_life's fish clamp, the boat lanes' level bound, and the flat wake level.
`IVehicleController::setSeaLevel` (BuoyancyController honours it, BoatDemo
passes it through) lets each hull be re-fed its LOCAL level every step.

Rain runoff: weather tick -> `setWorldRiverRainRise`; precipitation >= 0.6
swells the reach 0.3 -> 0.9 m over ~15 s, ebbing the same. Cap is MEASURED per
node off the built ground (tightest bank over the node's neighbourhood, 2.5 m
stations, the LOWER of the two banks) — not the authored levee term, which lied
by 10x in the beach reach.

### Gate results (all on a fresh relink, this worktree)
riverbridge 12/12 (incl. RB8 sweep, RB9 storm, RB10 one-truth), roadnetwork 58,
player 10, waterzap 10/10, terraincorridor 11, tunnelmouth 8/8, sealevel 27,
oceanbase 7/7, worldregions 11/11.

STILL OWED: boot zero-[ERROR] run of the driving world, and the eyes-on
full-res captures (downstream water inside its banks, rain-swollen river, the
sub from the bridge and from underwater).

### NOT THIS LANE — for the session lead
`--test-vehicle` drive-test fails 6 (wheels kept ground contact; car_ride x2;
skidpad x3). VERIFIED PRE-EXISTING: built base a1400d67 in this worktree and
got the identical 6 failures. Some other lane's regression, already on
integration/complete.

### Trap paid for, do not repeat
`git add -A` in this worktree sweeps in the store-served GLBs that
`asset_store.py fetch --all` materialises, plus `*.pre-fetch.bak` — both are
merge blockers (ENGINE_GOTCHAS 2.5). The first two commits had to be rewritten.
ALWAYS `git add <explicit paths>` here.

## THE SUB — remaining work (owner: "a sub or 2")
Plan: one patrol submarine in the deep 18-ft reach (kWorldRiverMidDrop 5.5 m
mid-channel), submerged silhouette visible from the bridge and while swimming,
slow patrol along the channel spine, clear of the speedboat lanes.

Groundwork already done:
- `BoatDemo::build(..., isSub=true)` EXISTS and gives the hull dive thrust
  (`bd.diveThrust = mass * 12`). `VehicleInput::dive` -1 = down.
- `setSeaLevel` (new, above) is what lets a sub hold depth on a river whose
  surface descends — feed `worldWaterLevelAt` at the hull each step.
- river_life.cpp is the home: it already owns boats, drivers, wake, audio, and
  the `pointAlongReach` lane builder (now unbounded).
- ART: the armory's only submarine is
  `D:/Assets/_glb/tech/Steampunk Submarine Diver/.../Submarine.glb` — real
  textures (Submarine_C + Submarine_N), one material, torpedo/prop nodes, but
  it is a RIGGED "Industrial / Victorian" steampunk character rig with
  walk/run/shoot clips, its rest-pose bounds are degenerate
  (min Y -30) and `tools/preview_glb.py` renders it black. Tonally wrong for a
  modern river too. The speedboats in this same file set the precedent: no boat
  GLB existed, so the hull is COMPOSED from tinted boxes on the live physics
  transform (`drawBoatSkin`). Do the same for the sub (cylinder hull + conning
  tower + fins), or hold it (NO_SLOP rule 3) — do NOT ship the yellow solid
  cube BoatDemo draws by default.
