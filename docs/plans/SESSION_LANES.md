# SESSION LANES — one brief per fresh session (Tim's order, 2026-07-29)

**Every session, first moves:** read `.remember/remember.md`, this file, your lane's
punchlist (`docs/plans/punchlist_*.md` — WD2/CP2077/GTAV benchmark gaps), and the
pillar board. **Universal rules:** capture-review before showing Tim; check tasklist
for Tim's running exe before builds/launches; ONE session owns `host_echotropolis.cpp`
+ CMake at a time (other lanes ship new files + an INTEGRATION note); commit per
milestone with honest gates; `ECHO_STREAM=0` and `ECHO_ISLAND_DIR` are the rollbacks.

## LANE 1 — TRAFFIC & DRIVING (the GTA V lane)
Mission: real traffic on the RoadGraph + driving feel polish.
First moves: (1) traffic AI — spawn/despawn cars driving lane centerlines
(`RoadGraph::laneCenterOffset`, built for this) with signals at junctions;
(2) V7.1 — retune the one law-dropped CA on-ramp (11.6°/m hook, boot log names it);
(3) wanted-lite: hack alarm -> cop citizens converge (npcLife converge exists).
(4) NFS HOT PURSUIT 2010 layer (Tim 2026-07-29): DRIFT feel — handbrake-drift
tuning on the Jolt wheel params (rear grip drop under handbrake, counter-steer
assist, drift camera lag) + VEHICLE UPGRADE SHOPS: the engine already ships
`vehparts.cpp` + `perfshop.cpp` (EFLZ systems, unwired here) — place shop
fronts in the city (Showroom_Model/WorkShop_Model from Mega City kit), wire
perfshop to treasury (engine/brakes/tires tiers change DriveDemo params).
Gate: 10-car flow through a junction capture + law PASS + a bought upgrade
that measurably changes a lap of the ring.

## LANE 2 — LIGHTING & NEON (the CP2077 lane)
Mission: night-city density + the last light bugs.
First moves: (1) neon signage pass — signboard kit + emissive shopfronts along drag/
boulevard (day-gated!); (2) far-water fog wash at noon (fog-over-water knob);
(3) volumetrics perf pass so ECHO_VOL can default ON at night (half-res/steps).
Gate: dusk boulevard capture that beats wave_night_drag.png.

## LANE 3 — HACKING & PROGRESSION (the WD2 lane)
Mission: wire the dormant RPG stack into play.
First moves: (1) `skilltree.cpp`/`progression.cpp`/`rpg_ui.cpp` wired to echotropolis
(XP from hacks/drives/DODOGs); (2) nethack-vision lite — highlight Hackables through
walls on a key; (3) scan-card actions: skim (exists) + distract + profile-to-LLM-talk.
Gate: earn a skill point in-game, spend it, see an effect.

## LANE 4 — INTERIORS & CITIZENS (the lifesim lane)
Mission: doors that open, rooms that matter, citizens inside.
First moves: (1) door portals on the 3 interior cells (kiosks mark them);
(2) citizens ENTER buildings on schedule (noodle bar patrons via npcLife);
(3) M-C ticks so cells truly gate; more cells (tower lobby, mine office).
Gate: follow a citizen into the noodle bar, buy, leave.

## LANE 5 — MATERIALS & ART (the beauty lane)
Mission: burn the remaining slop list (#25) + housing (#34).
First moves: (1) tree roster swap (textured conifers, paths in task #25);
(2) HouseForge/House-On-A-Hill street-aligned lots via RoadGraph tangents;
(3) water phase 2 wiring — echoShipPose into buildHarborBay (needs hull dims),
splashes; then mine rebuild, black slab, aircraft materials.
Gate: the 08:23-style shot with zero floaters and real houses.

## LANE 6 — STREAMING & PERF (the engine lane)
Mission: M-C evictions, M-D fast boot (~19s -> seconds), find the 35ms.
First moves: (1) M-C: streamer evictions live (deactivate path), silence the
'unknown builder' cosmetic; (2) r_speeds GPU timers + r_skinnedrt/r_ddgi A/Bs +
crowd bisect (districts/woodlands/crown already EXONERATED by experiment);
(3) M-D spawn-region boot. Gate: 60 FPS street-level OR named cost breakdown.

## LANE 7 — TERRAIN & FRONTIER (the world lane)
Mission: the fjord entry + the desert highway (Tim's BIG BET).
First moves: (1) fjord swap test (assets/island_fjord staged; ECHO_ISLAND_DIR)
+ sea-entry captures for Tim's approval; (2) bigger world frame decision (8-16km)
— required by both fjord miles and desert; (3) desert corridor v1: highway spur +
one gas station (Mega City Fuel_Station) + the hidden-mine set piece.
Gate: Tim's approval captures of the sea gorge; a drivable desert mile.
