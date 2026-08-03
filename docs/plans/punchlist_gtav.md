# PUNCHLIST vs GTA V — Echotropolis (X3Native)

Read: `.remember/remember.md`, `docs/PILLAR_BOARD.html`, captures
`tim_deck3.png`/`cars_drag2.png`/`v6_curve.png`/`roads_flank_v41.png`,
`world_cars.h`, `vehicle.cpp`, `echo_roads.h`, `crowd.h`, `npc_life.h`.
State: 2026-07-29, V7 road surfaces + interiors just landed; cars pillar
#26 is 35% ("KIT FOUND / NEXT MOVE" — full stack exists, unwired).

## 1. PLAYABILITY (driving feel)
**GTA V's bar:** weighty chassis lean, tire slip reads as grip not ice,
traffic reacts to you (swerve/honk/junction stop), cops path realistically.
**OUR state:** real Jolt 4-wheel vehicle in `vehicle.cpp` (RWD, tuned
`gripScale=1.7` to dodge Jolt's default burnout), `world_cars.h` wires
E-enter/exit, chase+hood cams, hold-E hack + alarm hook, water-kill exit.
`RoadGraph::laneCenterOffset()` was purpose-built for traffic lanes.
NOTHING drives it: `cars_drag2.png`'s road is empty; loop "cars" seen
elsewhere are parked/decorative, not agents.
**THE GAP:**
1. Spawn ambient traffic agents that sample `RoadGraph` edge centers and
   hold lane via `laneCenterOffset()` — zero current consumers of this API.
2. Naive junction yield using `echo_roads.h`'s logged junction patches
   (slow inside patch, yield to cross traffic).
3. Player-proximity brake/honk: nearest traffic car reacts when
   `WorldCars::carPosition()` closes inside ~8 m in-lane.
4. A second, tamer suspension/grip tuning profile so traffic doesn't all
   drive like the sports-car hero rig.
5. Extend `NpcLife`'s existing `freewayMovers` (already riding
   `worldFreewaySampleArc`) onto ground streets for cheap ambient motion
   before real traffic AI lands.

## 2. GRAPHICAL DETAIL
**GTA V's bar:** dense material variety — facades, signage, grime, no
obvious tiling at walking speed.
**OUR state:** V7 road textures just landed (asphalt/paint/concrete/
sidewalk/shoulder/grime buckets). Skyline in `cars_drag2.png`/`tim_deck3.png`
is flat-shaded boxes with punched window lights; housing pillar #34 is 25%
("KIT FOUND", unplaced).
**THE GAP:**
1. Place the already-owned #34 kit (Mega City / House-on-a-Hill / Seaside
   Town) street-aligned via `RoadGraph` tangents — biggest owned-asset win.
2. Second material bucket (banding/cornice) for the plain glass towers.
3. Extend V7's texture-bucket pattern to sidewalks under new housing.
4. Facade signage via emissive quads, reusing `drawNightGlow`'s technique.

## 3. LIGHTING
**GTA V's bar:** god-rays through skyline gaps, warm sodium night lighting,
real headlight cones, strong day/night contrast.
**OUR state:** day/night gate (`cityLightsOn`) drives road lamp glow
(`drawNightGlow`); water sun-glare reads well in `tim_deck3.png`. No
headlight/taillight emitters on the driven car in `world_cars.h`'s draw path.
**THE GAP:**
1. Add forward point/spot lights to the live car in `WorldCars::draw()`,
   fed through the same nearest-K select the road lamps use.
2. Taillight emissive quads via the `drawNightGlow` technique.
3. Verify DDGI is actually active over the city district (compare against
   `ddgi_ON`/`ddgi_OFF` captures) — daytime shots read flat/washed.
4. Contact-shadow/SSAO pass under the deck pier colonnade
   (`roads_flank_v41.png` shows no falloff onto the water).

## 4. SOUND
**GTA V's bar:** in-car radio (multi-station, DJ chatter, genre-coded) is
as iconic as driving; full positional ambience.
**OUR state:** no radio/station system exists — background music plays at
scene/cinematic level (`cinematic.h`/`app_run.cpp`), not diegetic to the
car. `world_cars.h`'s `updateAudio()` is solid: RPM-pitched engine loop,
throttle-scaled volume/pitch, clean start/stop on `m_driving`.
**THE GAP:**
1. Radio toggle while driving, layering a second `IAudioSystem` loop under
   the engine loop already wired in `updateAudio`.
2. Source 2-3 station loops via the existing `resolveAudio()` pack pattern.
3. Duck radio volume by throttle/RPM the same way engine pitch already is.
4. Positional traffic-ambience bed once section 1's traffic exists, seeded
   from `NpcLife` freeway-mover positions.
5. Audio stinger on `crowd.h`'s `onViolence` scatter — currently silent.

## 5. SCENERY DETAIL
**GTA V's bar:** street-furniture density (benches, trash, parked-car
variety, poles, clutter) that inhabits streets with zero NPCs present.
**OUR state:** road furniture is strong (shoulder/grime textures, lamp
pole+arm fixtures, crosswalks/debris per remember). Only non-road props are
`crowd.h` work crates and `npc_life.h`'s vendor cart/courier bike. Tree
roster still queued (remember's art-wave item).
**THE GAP:**
1. Land the queued tree roster along `RoadGraph` sidewalks/medians, reusing
   the sample-walk technique already built for lot alignment.
2. Vary parked cars: `WorldCarDef` already supports per-car `tint`/yaw —
   author more entries instead of repeating one hero mesh.
3. Authored bench/trash/pole clutter along `HarborStreet`/`Avenue` edges —
   no system seeds generic furniture off the graph today, only lamps.
4. More vendor-stall archetypes along the main drag beyond Tess's stall.

## 6. ACTOR DETAIL
**GTA V's bar:** dozens of ped skins per district, readable silhouettes,
density scaled to district type.
**OUR state:** strongest pillar (70%). `npc_life.h`'s 12 archetypes each
carry persona/tint/scale/voice/LLM scan-card detail — deeper on identity
than GTA V. Visually thin: one shared body mesh for all archetypes,
differentiated only by tint/scale/props.
**THE GAP:**
1. Silhouette add-ons (hat/apron/vest) for the 3-4 highest-value
   archetypes (StreetCop, BankRobber, Baker) instead of tint-only.
2. Check whether `crowd.h`'s ambient crowd and `npc_life.h`'s living NPCs
   can share one upgraded mesh+variant system rather than two flat ones.
3. Confirm `NpcLifeConfig` supports multiple per-district instances
   (downtown vs shanty vs woodlands) with different counts/mixes.
4. Confirm the Kid archetype's smaller `scale` actually renders visibly
   smaller — karma-protected but not confirmed visually distinct.

## 7. ANIMATION / RAGDOLL
**GTA V's bar:** reactive procedural ragdoll (stumble, partial recovery,
believable slumps), full ped animation variety.
**OUR state:** real GPU-skinned death ragdolls exist (`ragdoll.h/.cpp`,
`host_ragdoll.cpp`). Ambient crowd/NPC layers are explicitly unskinned —
`crowd.h` states "no physics bodies, no skinning" and exposes
`visBob`/`visCrouch`/`visLean` specifically for a skinned layer to consume.
**THE GAP:**
1. Confirm/finish `crowd_skin.h` applying those exposed gesture values to a
   rigged character — the data contract already exists, may just be unwired.
2. Confirm ragdoll triggers from a vehicle hit (player rams an NPC), not
   only scripted combat — no NPC-collision hook visible in `world_cars.h`.
3. Add a stumble/limp state between scatter and cower in `NpcAgent`'s FSM —
   reuse the existing scatter->cower transition as the template.
4. Confirm non-club civilians get a distinct idle from the club dance-sway
   (`crowd.h`'s `dance` bool suggests one shared sway function today).

## 8. PATHFINDING
**GTA V's bar:** lane-accurate vehicle routing with junction rules;
pedestrians flow along sidewalks, cross at crosswalks, queue naturally.
**OUR state:** no traffic pathfinding (see section 1). Pedestrians use
`npc_life.h`'s schedule walker: home/work/leisure posts plus one
`viaActive` street-centerline waypoint — point-to-point, not a sidewalk
graph. `crowd.h`'s ambient crowd wanders inside a raw radius/rect clamp,
never touching the road graph.
**THE GAP:**
1. Extend the single `via` waypoint to a full multi-edge walk along
   `RoadGraph` nodes so peds follow the actual street grid.
2. Reuse `echo_roads.h`'s junction-patch stop-bar positions as legal
   pedestrian crossing points.
3. Build the traffic-lane router from section 1 as the real shared
   consumer of `laneCenterOffset` — the actual pathfinding gap.
4. Clamp street-edge crowd deployments to sidewalk-adjacent `RoadGraph`
   samples instead of a raw radius, so crowds don't stand mid-street.
5. No obstacle-avoidance/steering-around-each-other in either crowd or NPC
   update loops — lowest priority but a real GTA-V-vs-ours gap.

## 9. PROGRESSION (economy loop, not stat trees)
**GTA V's bar:** money loops (missions/heists/stocks/properties) that make
progress legible; light stat growth.
**OUR state:** ahead of GTA V's ped-level depth already. `npc_life.h` ties
`skimCredits`/`hackKarma` per scan target (vulnerable marks like Kid/Baker
cost karma, Fixer/Robber are neutral) — real risk/reward per NPC. The Bank
Robber set-piece FSM (Casing->Strike->Alarm->Flee->Escaped/Caught) is
already wired to `AlertSystem` with player counterplay via `notifyHacked`.
**THE GAP:**
1. Surface running skim/karma totals in the HUD — mechanics exist
   per-NPC, no session ledger closes the loop yet.
2. Confirm robbery outcomes (`Caught`/`Escaped`) actually move treasury
   numbers, not just log a phase change.
3. Vary `skimCredits`/`hackKarma` per instance, not static per archetype —
   avoids farming an identical faucet.
4. Extend `StreetCop` `converge` beyond the scripted robbery into a general
   heat response to sustained negative karma.
5. Scale scan/skim value by district once density rises (section 1/8) so
   downtown reads richer than the shanty fringe.

## TOP 5 HIGHEST-LEVERAGE ITEMS (ranked)
1. **Wire ambient traffic onto `RoadGraph::laneCenterOffset`** — the data
   structure exists for exactly this with zero consumers; fixes
   PLAYABILITY and half of PATHFINDING at once.
2. **Place the owned #34 housing kit via `RoadGraph` tangents** — assets
   already sourced; fastest skyline/streetscape texture-variety win.
3. **Finish `crowd_skin.h`'s reactive-visual wiring** — `crowd.h` already
   computes and exposes bob/crouch/lean for exactly this consumer.
4. **In-car radio layered on the existing engine-loop audio path** — small
   addition to the pillar Tim named the soul of the game.
5. **HUD ledger + payout hook for skim/karma/robbery** — turns an economy
   sim that already beats GTA V's depth into one that *feels* like progress.
