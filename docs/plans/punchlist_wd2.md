# WD2 PUNCHLIST — Echotropolis vs. Watch Dogs 2

Authored 2026-07-29 against branch `echotropolis`, read from code (not memory):
`.remember/remember.md`, `docs/PILLAR_BOARD.html`, `docs/plans/TIER2_STREAMING_PLAN.md`,
captures `tim_deck3.png` / `wave_night_drag.png` / `water_v2_shore.png` / `v6_curve.png`,
and `app/npc_life.h`, `app/world_cars.h`, `app/hackables.h`, `app/world_hosts/echo_interiors.h`,
`app/skilltree.cpp`, `app/progression.cpp`, `app/anim.h`.

---

## 1. PLAYABILITY
**WD2's bar**: a moment-to-moment loop of phone-hack anything in sight, drive/hijack any car,
parkour across rooftops, deploy a drone/RC car for recon, and chain DedSec ops that escalate
heat and end in a chase or escape.
**OUR state**: a walk/explore/talk sandbox. `world_cars.h` is genuinely wired this window —
E-enter, Jolt wheels, hold-E hack, chase cam, 6 parked cars on the street. `npc_life.h` has one
emergent set piece (`RobberyPhase`: Casing→Strike→Alarm→Flee). No phone/nethack verb, no
parkour, no drone/RC play.
**THE GAP**:
1. `hackables.h`'s `HackableRegistry` is complete and self-tested (`--test-hacking`) but has
   **zero placed objects** in `host_echotropolis.cpp` beyond the NPC scan-card path routed
   through `npc_life.cpp` — no Camera/JunctionBox/ATM/TrafficSignal instances exist in this
   world. Populate a `carDefs`-style list and call `HackableRegistry::build`.
2. No "nethack vision" reveal toggle is wired to player input — `setHighlight`/`nearby()`/
   `lookTarget()` exist but nothing in `host_echotropolis.cpp` calls them for a hold-key HUD.
3. Extend `RobberyPhase` into a repeatable/chained loop (multiple robberies over a play
   session, heat escalation via `AlertSystem`) instead of one fixed set piece.
4. `hackDrone` is a patrol prop only (region table, `crown`) — not player-controllable; wiring
   it as a hackable RC toy would land WD2's drone verb cheaply.
5. Parkour/climb verb: no evidence anywhere in `app/`; lowest priority — skip until the above land.

## 2. GRAPHICAL DETAIL
**WD2's bar**: dense San Francisco block detail — varied facades, awnings, AC units, visible
weathering, prop-level LOD close up.
**OUR state**: from captures, towers read as flat blocky volumes with grid-punched emissive
window squares (`wave_night_drag.png`) — no facade relief. `v6_curve.png` shows a visible
horizontal-banding tiling seam across the grass terrain. `water_v2_shore.png` and
`tim_deck3.png` both show the water surface blown out near-white in bright light.
**THE GAP**:
1. GTA5 housing kit (#34, Mega Open World City Pack 109 GLBs + House On A Hill) is found but
   only 25% wired — street-aligned lots via RoadGraph tangents would fix facade monotony;
   this is already the planned move, just needs the pass.
2. Terrain tiling seam in `v6_curve.png` (dark horizontal lines across the hillside grass) —
   audit the island terrain shader's texture/UV scale or a detail-normal mip issue.
3. Water specular blowout in daylight (`tim_deck3.png`, `water_v2_shore.png`) — check
   `echo_water` tonemap/specular clamp; #36 (WATER V2 Living Bay) is the queued fix, not yet
   baked per `.remember/remember.md`.

## 3. LIGHTING
**WD2's bar**: volumetric rain/fog atmosphere, saturated neon rim light against a desaturated
wet environment, moody bounce GI at street level.
**OUR state**: night capture (`wave_night_drag.png`) shows nicely lit windows and a starfield,
but the **ground plane is pure black** — no streetlight pools reach the pavement at this
vantage, despite `crown` owning `streetLamps` + `lampScene` per the region table in
`TIER2_STREAMING_PLAN.md`. Day captures (`tim_deck3.png`, `v6_curve.png`) are flat/hazy with a
visible sun disc but little shadow contrast; the sunset warmth in `v6_curve.png` is the one
genuinely atmospheric shot.
**THE GAP**:
1. Verify `streetLamps`/`lampScene` are actually resident + intensity/radius reach the drag at
   the `wave_night_drag.png` camera position — this is a placement/intensity bug, not a
   missing system.
2. Add a volumetric/fog density pass for the WD2 "rain haze" look — the Cyberpunk audio kit
   already streams a rain ambient loop (`host_echotropolis.cpp` ~2355) with no matching visual.
3. Fix the water specular blowout (shared root cause, §2 item 3).

## 4. SOUND
**WD2's bar**: dense layered ambience, traffic doppler, overheard radio/crowd chatter,
distinct hack stingers, surface-varied footsteps.
**OUR state**: genuinely strong start — `host_echotropolis.cpp` (~2355) wires a 1067-sound
Cyberpunk kit: 2D rain+ambient bed, night music, 3 positional 3D loops (mine servers, drone
buzz, boat engine idle), 3 UI stingers (confirm/accept/deny).
**THE GAP**:
1. No footstep/surface-variety SFX, no vehicle engine loop tied to `worldCars` driving state
   (the kit has engine sounds; nothing plays them on E-enter/throttle).
2. No hack-type-specific stinger — `HackResult::effect` strings exist per-type in
   `hackables.h` but every hack currently plays the same generic confirm/accept/deny.
3. Only 3 positional 3D loops for the whole island — add per-`EchoRegion` loops keyed to
   `onBuild` (fits the WP-1 region-hook contract, `TIER2_STREAMING_PLAN.md` §3).
4. No crowd-chatter audio bed backing the LLM text layer — street carries no murmur texture.

## 5. SCENERY DETAIL
**WD2's bar**: dense street clutter — cones, dumpsters, newspaper stands, parked bikes,
graffiti, overhead wires, signage everywhere.
**OUR state**: `streetProps` = vendor carts (Meshy hot-dog cart etc.). Captures show largely
bare streets at ground level — `wave_night_drag.png` has a couple of colored blip markers and
one dark box, nothing read as clutter.
**THE GAP**:
1. The licensed 109-model Mega Open World City Pack includes shop-front/civic clutter beyond
   the vendor carts already pulled — expand `buildVendorDressing()` (`echo_interiors.h`) or add
   a sibling clutter-scatter pass along RoadGraph sidewalks.
2. `SB_*` signboards are catalogued as available (`echo_interiors.h`) but not confirmed placed
   island-wide — audit `echo_region_builders` for placement and add where missing.
3. Graffiti/overhead wires: no evidence found anywhere; lowest priority, skip.

## 6. ACTOR DETAIL
**WD2's bar**: citizen archetype variety, each pedestrian carries a phone-hackable "profile"
(name/job/salary humor) as a scan gag, moderate crowd density with distinct silhouettes.
**OUR state**: STRONGEST pillar per the pillar board. `npc_life.h` has 12 authored archetypes
(HotDogVendor, BankRobber, Electrician, Programmer, Baker, Gardener, Courier, StreetCop,
Preacher, OffShiftDrone, Fixer, Kid) with schedules, scan-cards (name/occupation/one telling
detail), karma-aware hacking (hacking the Kid/OffShiftDrone/Baker costs more), and **LLM-driven
conversation** — WD2 only ever used canned barks, so this genuinely exceeds the bar.
**THE GAP**:
1. Verify ambient (non-schedule) crowd filler — separate from the 12 archetypes — has real
   palette/silhouette variety rather than reading as clones; check `crowd_skin.cpp`/
   `CrowdConfig` for the variety knob count.
2. Confirm the scan-card HoloPanel triggers reliably at range for ambient crowd too, not just
   the 12 named archetypes (WD2's profile reveal is instant on anyone in frame).
3. Pillar board's own next note: NPC "interiors participation, crime reactions" — citizens
   don't yet enter buildings or react to nearby crime; natural follow-on once #27 interiors land.

## 7. ANIMATION/RAGDOLL
**WD2's bar**: full parkour animation tree, vehicle entry/exit anims, ragdoll on takedown/death,
vaulting over cover.
**OUR state**: engine-side primitives are strong — GPU skinning palette upload, locomotion
blending (`applyLocomotion`), and death ragdoll via `applyRagdollBlend` (partial, per-joint
blend between animated and physics pose) all exist in `app/anim.h`.
**THE GAP**:
1. `npc_life.h`'s own header states its agents are "pure kinematic Scene entities... no
   skinning" — meaning the 12 archetypes and ambient crowd may not be riding the GPU-skinned/
   ragdoll pipeline at all. Audit `crowd_skin.cpp`/`npc_skin.cpp` to confirm whether citizens
   are skinned rigs or proc-mesh/billboard stand-ins.
2. No vehicle entry/exit animation — `world_cars.h` "STASHES" (teleports) the player capsule to
   the parked pose on E-enter/exit rather than playing a climb-in/out clip.
3. No combat/takedown moment exists in Echotropolis, so death ragdoll never triggers here even
   though the engine primitive is ready — lowest priority until police-escalation lands.

## 8. PATHFINDING
**WD2's bar**: navmesh-driven citizen and vehicle pathing around dynamic obstacles; traffic AI
merges lanes; cops path the real road network during a pursuit.
**OUR state**: ours is schedule/waypoint-based, not navmesh-based — `npc_life.h` walks agents
"the street grid to REAL destinations" via a `retarget()`-picked destination, and vehicle
traffic follows a fixed authored freeway loop (`kRoute`, per `TIER2_STREAMING_PLAN.md`'s
persistent-lane table). The engine DOES have navmesh capability, but grep shows it used **only**
in `app/act2_caves.cpp` — zero hits in Echotropolis.
**THE GAP**:
1. Confirm the street-grid waypoint graph and the v7 RoadGraph (used by cars + planned housing
   lots) are the SAME source of truth, not two divergent street representations that will drift.
2. No dynamic obstacle avoidance — NPCs/cars don't steer around each other, parked/hacked cars,
   or the Bank Robber's converging cops; a cheap separation/steering pass sells WD2 traffic feel
   far more cheaply than a full navmesh port of the open world.
3. Audit that `StreetCop`'s robbery-converge behavior (`RobberyPhase`, `npc_life.cpp`
   `retarget()`) actually paths the street grid rather than beelining through geometry.

## 9. HACKING/SKILLTREES
**WD2's bar**: ctOS tower unlocks reveal map regions/collectibles; a deep skill tree
(combat/hacking/driving/stealth) gated by XP; escalating gadgets over the campaign.
**OUR state**: `hackables.h`'s taxonomy (Camera/JunctionBox/ATM/Vehicle/TrafficSignal/Npc) with
heat+karma routing is complete and tested. `ctos_terminal.glb` exists as a placed prop
(`echo_interiors.h`). Engine-side `skilltree.cpp`/`progression.cpp` is a full XP→level→
skill-point→node system with real stat mods (damage/speed/reload/ammo/crit/xp/hp). Grep of
`host_echotropolis.cpp` confirms: **neither `SkillTree`/`Progression` nor the general
`HackableRegistry` (beyond the NPC scan path) are wired anywhere in this world.**
**THE GAP**:
1. Instantiate `HackableRegistry` and populate Camera/JunctionBox/ATM/TrafficSignal objects
   city-wide, mirroring the exact pattern already proven for `WorldCarDef carDefs` in
   `host_echotropolis.cpp` — this is the single biggest WD2-identity gap and the machinery is
   done and tested.
2. Wire `Progression` (XP sources: hacks, robbery interventions, exploration) and a minimal
   `SkillTree` purchase UI — right now zero XP is ever earned in Echotropolis.
3. Repurpose district gate/tower entities (the 37 "Urban Night City" `towers`) as ctOS-style
   unlocks that reveal `HackableRegistry` markers for their district — ties directly into the
   existing `EchoRegionSet`/`appendNearLights` region pattern.
4. Extend the proven `HackSinks` pattern (already live for car alarms → crowd `onViolence`) to
   junction boxes and cameras for parity with the car-hack fantasy.

---

## TOP 5 HIGHEST-LEVERAGE ITEMS
1. Wire `HackableRegistry` city-wide (Camera/JunctionBox/ATM/TrafficSignal) + a nethack-vision
   toggle — WD2's signature verb, machinery complete and tested, only placement+host wiring left.
2. Wire `Progression` + `SkillTree` so hacks/robbery/exploration actually earn XP and points —
   a complete, unwired system that pairs directly with item 1.
3. Fix ground-level night lighting (`streetLamps`/`lampScene` reach at the drag) — cheap
   placement/intensity fix, outsized atmosphere payoff versus the black-ground capture.
4. Street clutter + signage scatter pass (expand `buildVendorDressing`, place `SB_*` boards) —
   the licensed asset pack is already in hand; streets currently read as empty.
5. Push GTA5 housing (#34: RoadGraph-tangent lots from the Mega City/House-On-A-Hill kits) —
   already the planned next move at 25%, and it's the highest-leverage graphical-detail fix.
