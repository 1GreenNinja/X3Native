# W-WATER lane handoff (Lane 2, Seven-Lane Plan)

Worktree `agent-aa28d458b82af9a3b`, branch `worktree-agent-aa28d458b82af9a3b`.
Merged with `origin/integration/complete` @ **ce48e2b3** (W-PERF terrain
LOD/ridge, weapons, handling) and re-gated on top of it. NEVER push — the
session lead merges.

## Status: LANE COMPLETE. #32, #23 and the SUB are done and gated.

Commits on top of the integration tip:

| commit | what |
|---|---|
| 5c38387d | ONE water truth — river mode in the water pass, shims deleted, rain runoff |
| bfa46f17 | the last split-truth shims die — hulls and fish ride the real river |
| 8e41b66e | the station sweep RB8/RB9/RB10 (and the real bug it found) |
| 1a2c1423 | wip: the patrol sub — build, depth-hold prePhysics tick, composed skin |
| cb9ddc80 | the sub is wired — postPhysics, render, shutdown, and the CONTACT LAW |
| 1787dc36 | the sub patrols THE CROSSING — and the proof shots that moved it there |
| 64bb383f | merge origin/integration/complete (ce48e2b3) |
| 635ef991 | the bank/rain proof set — RB8's sweep with eyes on it |
| 9557daa7 | sub proof shots regenerated on the merged tip |

### #32 — ONE WATER TRUTH (done)
`WaterParams` carries the river's node polyline + half-width + the ocean basin
disc; `water.vert` runs the CPU's polyClosest in GLSL per vertex, so the drawn
surface steps down the channel with the carve and hands the estuary to the sea.
`riverNodeCount == 0` keeps every ocean world byte-identical.

**The temporary bridge is GONE and stays gone.** `RoadTrees::build`'s
`minBenchY` parameter and host_tunnel's `benchDryY` existed only because the two
water truths disagreed; both are deleted. `grep minBenchY benchDryY` now finds
only RECEIPT COMMENTS at app/road_trees.{h,cpp}:62/286 and
host_tunnel.cpp:790 naming the dead shim so it cannot grow back (NO_SLOP
rule 10). Benches use the honest level. RB10 proves the drawn surface and
`worldWaterLevelAt` agree to **0.0000 m**, worst case, over the whole run.

### #23 — RAIN RUNOFF (done)
Weather tick -> `setWorldRiverRainRise`; precipitation >= 0.6 swells the reach
0.3 -> 0.9 m over ~15 s and ebbs the same. The cap is MEASURED per node off the
built ground (tightest bank over the node's neighbourhood, 2.5 m stations, the
LOWER of the two banks), not the authored levee term, which lied by 10x in the
beach reach.

### THE SUB (done — owner: "a sub or 2")
One midget patrol submarine, `river_life.cpp`. **It patrols THE CROSSING**: a
76 m lane straddling the bridge, passing between the piers on the channel spine.

Two measurements decided that lane, after the first cut (210..430 m downstream)
was shot and showed *nothing* from the bridge:
* the carved channel is a **uniform 5.5 m mid-channel over the whole reach** —
  all three `[river-life] depth station` lines log 5.500 — so depth never
  required the far lane. Only the speedboats did, and their lanes start 45 m out
  on both sides, leaving the +-45 m window at the crossing.
* the lane had been anchored on the crossing's **nearest spline node**, and the
  nodes are ~145 m apart — that node sat **61 m from the bridge**. The crossing's
  arc offset is now projected onto the reach and folded into every lane distance;
  the build log prints the residual (`lane midpoint is 0.000000 m from the
  crossing`).

Clear of the speedboats twice over: 38 m vs their 45 m in plan, and 1.05 m of
vertical separation between a submerged deck and a planing keel.

`BoatDemo(isSub=true)` hull; depth held by a PD on `dive` about a measured
equilibrium bias (fully submerged the hull is 1/0.475 = 2.1x buoyant, so
`diveThrust = mass*12` balances at dive = -0.90). Patrol speed 0.75 throttle =
**1.2 m/s** (terminal = 1.6*throttle from BoatDemo's thrust/drag ratio).
`setSeaLevel` is re-fed the LOCAL surface every step — the depth hold only works
at all because of #32; on the old flat plane a sub holding 1.9 m down would have
driven into the bed downstream.

**NO_SLOP rule 11 (the contact law) is enforced on it**, extended to the one
vehicle type that lives below a surface: keel clamped above the carved bed with
the downward velocity killed, plus a mirror clamp keeping the hull top under the
surface so a broach can never make it read as a boat. `river_life.cpp`
postPhysics.

ART: composed, not GLB. The armory's only submarine is a rigged Victorian
steampunk *character* with walk/shoot clips, degenerate rest bounds and a black
preview — tonally and technically wrong. The speedboats in the same file set the
precedent, so the sub is a round pressure hull + sail + dive planes + stern
planes + screw + one amber lamp, tinted primitives on the live physics
transform. If the art ever fails to build, `render()` draws NOTHING rather than
BoatDemo's default yellow cube (rule 3).

## Gate results — ALL GREEN on the merged tip
* Build: clean (X3Engine + X3LevelArchitect relinked).
* Suites: riverbridge **12/12** (incl. RB8 sweep, RB9 storm, RB10 one-truth),
  roadnetwork **58**, player **10**, waterzap **10/10**, vehicle **10**,
  terraincorridor **16**.
* Boot: `--world tunnel`, dry and `X3_WEATHER=storm`, **zero [ERROR]** both.
* Station sweep: 79 stations over 15 carved nodes. Worst bank freeboard
  **+0.068 ft dry / +0.027 ft in the storm**, both at (352, -5). Positive
  everywhere — no water above bank crest anywhere on the reach.

## Proof captures (all read at full res by the agent that made them)
`docs/screenshots/tunnel/`
* `14_sub_from_bridge.png` — from the deck parapet, 21 m range: the submerged
  silhouette reads as a SUB and is plainly a different thing from the speedboat
  and wake on the surface above it.
* `15_sub_underwater.png` — from in the water beside it, bridge substructure in
  frame, hull shadow on the caustic-lit bed.
* `16_banks_{dry,storm}_{0..3}.png` — nodes 2/4/6/7, same cameras both runs.
  Every freeboard positive: 21.912 / 0.184 / 0.200 / 0.200 m dry,
  21.162 / 0.172 / 0.104 / 0.080 m in the storm.
* `17_waterline_{dry,storm}.png` — the rain A/B. Eye pinned 1.4 m over the DRY
  surface (same world Y in both runs by construction); the water climbs from
  1.40 m to 0.65 m under the lens, a measured **0.75 m rise**, still inside the
  gorge.

## Proof rigs (env-gated, headless `--world tunnel`)
* `X3_SHOT_SUB=1` — the two sub frames. Pre-rolls physics to a frameable moment
  and leads the hull through settleAndGrab's 200-frame settle. Logs `[sub-shot]`
  hull pos, submergence, local surface and bed at every shot.
* `X3_SHOT_BANKS=1` — the bank/rain set. Measures freeboard the way RB8 does
  (levee band, estuary skipped) and **logs [ERROR] and fails the capture if any
  station is ever negative** — a gate, not a photo op. Pre-rolls the weather
  model 30 s so the runoff ramp has finished before framing.

## NOT THIS LANE — for the session lead
* `--test-vehicle` drive-test fails 6 (wheels kept ground contact; car_ride x2;
  skidpad x3). VERIFIED PRE-EXISTING at base a1400d67 in this worktree. The
  `--test-vehicle` SUITE (the one in the gate list) is 10/10 green.
* Two dark spires appear on the horizon in `16_banks_storm_3.png` and
  `17_waterline_*.png` — one smooth cone, one jagged spike. They arrived with
  the merge (W-PERF terrain LOD/ridge). Not investigated; not water.
* The speedboat wake foam blows out to large white blobs when seen from
  directly above (`14_sub_from_bridge.png`, lower right). Pre-existing wake
  particle sizing, not touched by this lane.

## Trap paid for, do not repeat
`git add -A` in this worktree sweeps in the store-served GLBs that
`asset_store.py fetch --all` materialises, plus `*.pre-fetch.bak` — both are
merge blockers (ENGINE_GOTCHAS 2.5). ALWAYS `git add <explicit paths>` here.
Ten untracked GLBs under `assets/converted_glb/Vehicles/` are sitting in the
worktree right now; leave them untracked.
