# CP2077 PUNCHLIST — Echotropolis vs. Cyberpunk 2077

Authored 2026-07-29 against branch `echotropolis`, read from code (not memory):
`.remember/remember.md`, `docs/PILLAR_BOARD.html`, `docs/HANDOFF_2026-07-24.md` §8, captures
`tim_deck3.png`/`wave_night_drag.png`/`rv_rimhwy.png`/`mesa_noon_postcard.png`, and
`app/npc_life.h`, `app/world_cars.h`, `app/hackables.h`, `app/world_hosts/echo_interiors.h`,
`app/skilltree.cpp`/`progression.cpp`/`rpg_ui.cpp`, `app/world_hosts/host_echotropolis.cpp`.

---

## 1. PLAYABILITY
**CP2077's bar**: seamless walk→drive→hack→fight loop across a dense vertical city; quickhack
anything on sight via optics scan, jack any car, braindance set-pieces, gunplay/cyberware verbs.
**OUR state**: walk/explore/talk sandbox, one emergent beat. `world_cars.h` (E-enter, Jolt wheels,
hold-E hack, chase cam) is complete but is the pillar board's own "NEXT MOVE" at 35% — not yet
proven live. `npc_life.h`'s `RobberyPhase` (Casing→Strike→Alarm→Flee) is the one CP2077-shaped
set-piece. No weapon/combat verb, no quickhack-on-sight, no braindance analog.
**THE GAP**:
1. Finish wiring `WorldCars` into `host_echotropolis.cpp`'s input loop end-to-end — the biggest
   playability unlock already built, just unproven live.
2. `HackableRegistry` (`hackables.h`) is complete + self-tested but has **zero placed objects**;
   `npcLife.build()` passes `hax = nullptr` (~line 2158) — no scan/quickhack verb exists anywhere.
3. Wire `setHighlight`/`nearby()`/`lookTarget()` to a hold-key "optical scan" HUD toggle — the
   NetHack-vision primitive CP2077's quickhack-scan is built on.
4. Extend `RobberyPhase` into a repeatable, escalating loop instead of one fixed timer scene (no
   combat/weapon verb exists anywhere in `app/` for this world — lowest priority, skip until above).

## 2. GRAPHICAL DETAIL
**CP2077's bar**: hand-authored mega-block density — layered signage, cable clutter, weathering,
reflective wet streets, silhouette variety from megabuilding to noodle stand.
**OUR state**: every capture shows flat glass/concrete towers with grid-punched white emissive
window rectangles (`wave_night_drag.png`, `rv_rimhwy.png`) — uniform color, no facade relief, no
billboards. `mesa_noon_postcard.png` shows clean massing but hazy, low-contrast air with almost no
ground-level detail even from a readable altitude.
**THE GAP**:
1. GTA5 housing kit (#34, Mega Open World City Pack 109 GLBs) is only 25% wired per the pillar
   board — street-aligned lots via RoadGraph tangents is the queued fix for facade monotony.
2. Window emissive is uniform white — vary tint/on-off ratio per building for CP2077's "not every
   window matches" read; `echo_interiors.h`'s cataloged textured shells (`TheHotel_Model.glb`/
   `Shops01_Model`) are unwired — wiring them (§5/§9) also fixes flat-box facades near the player.

## 3. LIGHTING (our most winnable fight)
**CP2077's bar**: saturated neon rim-light against desaturated wet streets, RT GI bouncing neon
onto geometry, volumetric haze making every beam visible, believable street-level pools.
**OUR state**: closer to CP2077's own tech than any other category. `r_ddgi` defaults **ON** here
(`host_echotropolis.cpp`:2532) — real bounce light, not flat ambient. Volumetrics (`ECHO_VOL`)
exist and are wired but **opt-in only, ~100ms/frame (10 FPS measured, unprofiled)** — capture-only,
not playable. `r_rtreflections`/`r_rtshadows` both default **OFF** here (shadows off for a known
"district self-shadow" bug) despite the engine supporting RT tier 2 elsewhere. Night capture:
windows/stars read well but the **ground plane is pure black** — no streetlight pool reaches
pavement. Day is flat/hazy with almost no shadow contrast. Day cycle is a real, running 2h default.
**THE GAP**:
1. District glow proxies (HANDOFF §8 #2): cluster each district's neon into ~3 big low-intensity
   200-400m lights appended to `districtLights` — **best value/effort in the roadmap**, gives the
   volumetric something to scatter from altitude; do before #3.
2. Fix ground-level street-lamp reach at night (`streetLamps`/`lampScene` residency/intensity audit
   at the `wave_night_drag.png` vantage) — cheap placement bug, not missing tech.
3. Turn `r_rtshadows` back on and fix the district self-shadow bug — shadow contrast is sitting
   behind one known issue.
4. Profile volumetrics (~100ms/frame, unprofiled) and cut cost enough to run always-on, plus
   promote the hardcoded 48m SSR ray-march (`vk_passes.cpp:517`) to `r_ssr_maxdist` for wet-street
   neon reflection — both already implemented, just too costly/limited to leave on.

## 4. SOUND
**CP2077's bar**: layered radio stations with DJ chatter, dense traffic/crowd ambience, distinct
weapon/cyberware/hack SFX, reactive combat music.
**OUR state**: `host_echotropolis.cpp` (~2355) wires a genuine Cyberpunk-flavored kit — 2D
rain+ambient bed, night music, 3 positional 3D loops, 3 UI stingers. No radio, weapon audio, or
per-hack-type stinger.
**THE GAP**:
1. No footstep/surface SFX and no vehicle engine loop tied to `WorldCars`' driving state — the kit
   has the sounds; nothing plays them yet (mirrors §1's unfinished car wiring). `HackResult::effect`
   strings exist per-type too, but no hack SFX plays until §1/§9's registry has placed objects.
2. Only 3 positional 3D loops island-wide — add per-`EchoRegion` beds keyed to `onBuild` for
   CP2077's "every block sounds different" texture, plus a crowd-murmur bed under the LLM talk
   layer (cheapest win for perceived density).

## 5. SCENERY DETAIL (neon signage is CP2077's soul)
**CP2077's bar**: signage stacked floor-to-floor, holographic ads, cable/pipe clutter, wet
reflective ground selling density even where geometry is sparse.
**OUR state**: we have the two ingredients CP2077 signage needs — an emissive-material system
(already driving every window square) and a real signboard kit (`SB_*` GLBs, per `echo_interiors.h`'s
asset audit) — but placement exists **only** in the not-yet-wired `echo_interiors.cpp`. Captures
show bare streets — `wave_night_drag.png` has a couple of blip markers and one dark box, nothing
reading as signage or clutter.
**THE GAP**:
1. Wire `buildVendorDressing()` into `buildCrown` (confirm it's actually reached) and expand it
   with `SB_GasStation`-style boards along every drag, not just vendor stalls.
2. Add a dedicated NEON SIGN pass reusing the emissive-window technique: colored, varied-shape
   emissive quads/decals stacked on tower facades — cheapest, highest-identity fix, no new assets.
3. Street clutter beyond vendor carts — the 109-model Mega City Pack has unused civic/shop-front
   clutter; scatter it along RoadGraph sidewalks (item 1's mechanism), and add wet/reflective
   ground shading (no evidence in the codebase today) once §3's SSR fix lands.

## 6. ACTOR DETAIL
**CP2077's bar**: named/scannable NPCs with cyberware tells, dense varied crowd silhouettes,
faction-readable street population.
**OUR state**: `npc_life.h` is the strongest pillar in the project — 12 authored archetypes
(HotDogVendor, BankRobber, Electrician, Programmer, Baker, Gardener, Courier, StreetCop, Preacher,
OffShiftDrone, Fixer, Kid) with schedules, scan-card data, karma-aware hack rules, and
**LLM-generated conversation** — a real edge over CP2077's canned barks, when live. Ambient filler
crowd is thin and uniform: `CrowdConfig cc.count = 40` + `mc.count = 9` sharing one body mesh
(tint/scale variance only).
**THE GAP**:
1. Scan-cards (CP2077's optics-scan analog) are fully authored but never surface —
   `npcLife.build()` gets `hax = nullptr` (~2158), so **no NPC is ever scannable**. Single highest-
   leverage actor-detail fix: pass a real `HackableRegistry*`.
2. 49 total ambient bodies sharing one mesh is thin for CP2077 density — raise the count and add a
   second/third body mesh + palette variety so the crowd doesn't read as clones.
3. Citizens don't yet enter buildings or react to nearby crime (pillar board's own note) — follow-
   on once §9's interiors and §1's hacking land.

## 7. ANIMATION/RAGDOLL
**CP2077's bar**: full mocap locomotion + cover/vault verbs, cyberware finishers, physicalized
ragdoll on every death, vehicle entry/exit anims.
**OUR state**: engine primitives are strong — `anim.h` has GPU compute skinning, `applyLocomotion`
blending, and `applyRagdollBlend` (per-joint animated↔physics blend). But `npc_life.h`'s own header
is explicit: agents are "pure kinematic Scene entities... no physics bodies, no skinning" — the 12
archetypes and ambient crowd are NOT on the skinned/ragdoll pipeline in this world at all.
**THE GAP**:
1. Confirm whether citizens are proc-mesh stand-ins or ever upgrade to real skinned rigs — if not,
   that's why NPCs can't visibly react, a prerequisite for CP2077-grade actor presence.
2. `world_cars.h` teleports ("STASHED") the player capsule on E-enter/exit, no climb-in/out clip —
   cheap win once §1's car wiring is finished.
3. No combat/death moment exists here, so `applyRagdollBlend` never fires though engine-ready —
   lowest priority until §1's combat verb exists.

## 8. PATHFINDING
**CP2077's bar**: navmesh-driven pedestrians and vehicle AI routing around dynamic obstacles;
police path the real street network mid-pursuit.
**OUR state**: schedule/waypoint-based, not navmesh-based. `npc_life.h` walks agents "the street
grid to REAL destinations" via a `retarget()`-picked post + centerline-follow; freeway traffic
rides a fixed `arc` parameter. No navmesh integration exists in `host_echotropolis.cpp` — the
engine's navmesh capability (used elsewhere, e.g. `act2_caves.cpp`) has zero footprint here.
**THE GAP**:
1. Confirm the street-grid waypoint graph NpcLife walks and the v7 RoadGraph (cars + planned
   housing lots) are the SAME source of truth — divergent street representations will drift.
2. No dynamic obstacle avoidance: NPCs/cops don't steer around each other, parked/hacked cars, or
   geometry — a cheap separation/steering pass buys most of CP2077's traffic feel far cheaper than
   a full navmesh port.
3. Audit `StreetCop`'s robbery-converge path to confirm it follows the street grid, not geometry.

## 9. HACKING/SKILLTREES
**CP2077's bar**: quickhacks cast from a RAM pool onto scanned targets, a deep perk/cyberware tree
gated by street cred + XP, escalating gadget/implant unlocks.
**OUR state**: `hackables.h`'s `HackableRegistry` — the closest analog to quickhacks — is a
complete, self-tested taxonomy (Camera/JunctionBox/ATM/Vehicle/TrafficSignal/Npc) with real
heat+karma routing via `HackSinks`. **Entirely unwired**: no `#include` in `host_echotropolis.cpp`,
zero placed objects, `npcLife.build()`'s `hax` is `nullptr`. Separately, `skilltree.cpp`/
`progression.cpp`/`rpg_ui.cpp` form a complete XP→level→skill-point→node system with real stat
mods — referenced only from `app_run.cpp` (a different world), never here. Zero XP is ever earned
in Echotropolis today.
**THE GAP**:
1. Instantiate `HackableRegistry` in `host_echotropolis.cpp`, populate Camera/JunctionBox/ATM/
   TrafficSignal city-wide, mirroring the proven `WorldCarDef carDefs` pattern — the single biggest
   CP2077-identity gap; machinery done and tested.
2. Pass a real `HackableRegistry*` into `npcLife.build()` (currently `nullptr`) so NPC scan-cards
   surface — shared root cause with §6 item 1.
3. Wire `Progression` + a minimal `SkillTree` purchase screen (`rpg_ui.cpp` already renders one):
   XP from hacks, robbery interventions, exploration.
4. `ctos_terminal.glb` is already a placed prop — repurpose district gates as ctOS-style unlocks
   revealing `HackableRegistry` markers per district, and extend the proven `HackSinks` pattern
   (already live for `WorldCars`' alarm→crowd `onViolence`) to junction boxes/cameras once placed.

---

## TOP 5 HIGHEST-LEVERAGE ITEMS
1. Wire `HackableRegistry` into `host_echotropolis.cpp` (Camera/JunctionBox/ATM/TrafficSignal +
   a real `hax` pointer into `npcLife.build()`) — CP2077's core quickhack/scan identity, machinery
   complete and tested, currently zero placed objects and `nullptr`.
2. District glow proxies for volumetric scatter (HANDOFF §8 #2: ~3 big lights per district,
   200-400m) — best value/effort in the roadmap, the fight we're already closest to winning (DDGI
   ON; volumetrics exist, just need a source).
3. Neon signage placement pass — wire `echo_interiors.h`'s `SB_*` signboards + a colored emissive
   decal system onto tower facades; no new assets needed, cheapest high-identity fix on the list.
4. Wire `Progression` + `SkillTree` (already complete, referenced from the wrong world) so hacks/
   robbery/exploration earn XP — pairs directly with item 1, unlocks the RPG-depth bar.
5. Fix ground-level night lighting (`streetLamps`/`lampScene` reach) + turn `r_rtshadows` back on —
   two cheap, already-built fixes standing between today's black-ground captures and CP2077-grade
   street-level contrast.
