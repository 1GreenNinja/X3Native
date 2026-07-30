# CP2077 PUNCHLIST — Echotropolis vs. Cyberpunk 2077

Authored 2026-07-29 against branch `echotropolis`, read from code (not memory):
`.remember/remember.md`, `docs/PILLAR_BOARD.html`, `docs/HANDOFF_2026-07-24.md` §8 (volumetric
roadmap), captures `tim_deck3.png` / `wave_night_drag.png` / `rv_rimhwy.png` /
`mesa_noon_postcard.png`, and `app/npc_life.h`, `app/world_cars.h`, `app/hackables.h`,
`app/world_hosts/echo_interiors.h`, `app/skilltree.cpp`/`progression.cpp`/`rpg_ui.cpp`,
`app/world_hosts/host_echotropolis.cpp`, `app/app_run.cpp` (RT cvar defaults).

---

## 1. PLAYABILITY
**CP2077's bar**: seamless walk→drive→hack→fight loop across a dense vertical city; quickhack
anything on sight via optics scan, jack into any car, braindance investigation set-pieces, gunplay
+ melee + cyberware verbs layered on top.
**OUR state**: a walk/explore/talk sandbox with one emergent set piece. `world_cars.h` is a
complete E-enter/Jolt-wheel/hold-E-hack/chase-cam stack but is the pillar board's own "NEXT MOVE"
at 35% — no evidence it's live in a played session yet. `npc_life.h`'s `RobberyPhase`
(Casing→Strike→Alarm→Flee) is the one CP2077-shaped emergent beat. No weapon/combat verb, no
quickhack-on-sight, no braindance analog.
**THE GAP**:
1. Finish wiring `WorldCars` into `host_echotropolis.cpp`'s input loop end-to-end (E-enter is the
   pillar board's stated next move) — this is the single biggest playability unlock already built.
2. `HackableRegistry` (`hackables.h`) is complete + self-tested (`--test-hacking`) but has **zero
   placed objects** in `host_echotropolis.cpp` and `npcLife.build()` is called with `hax = nullptr`
   (line ~2158) — no scan/quickhack verb exists anywhere in this world today.
3. Wire `setHighlight`/`nearby()`/`lookTarget()` to a hold-key "optical scan" HUD toggle — the
   NetHack-vision primitive CP2077's quickhack-scan is built on.
4. Extend `RobberyPhase` into a repeatable, escalating loop instead of one fixed timer-fired scene.
5. No combat/weapon verb anywhere in `app/` for this world — lowest priority; skip until 1-3 land.

## 2. GRAPHICAL DETAIL
**CP2077's bar**: hand-authored mega-block density — layered signage, cable clutter, weathering,
reflective wet streets, silhouette variety from megabuilding to noodle stand.
**OUR state**: from every capture, towers read as flat glass/concrete volumes with grid-punched
white emissive window rectangles (`wave_night_drag.png`, `rv_rimhwy.png`) — uniform color, no
facade relief, no billboards. `mesa_noon_postcard.png` shows clean massing but a hazy, low-contrast
atmosphere with almost no ground-level detail visible even at readable altitude.
**THE GAP**:
1. GTA5 housing kit (#34, Mega Open World City Pack 109 GLBs) is only 25% wired per the pillar
   board — street-aligned lots via RoadGraph tangents is the queued fix for facade monotony.
2. Window emissive is a single uniform white — vary tint/on-off ratio per building
   (`host_echotropolis.cpp` window-emissive block) for the CP2077 "not every window matches" read.
3. `echo_interiors.h`'s `TheHotel_Model.glb`/`Shops01_Model` textured shells are cataloged
   (glb_audit 2026-07-29) but unwired — wiring them (see §5/§9) is also the graphical-detail fix,
   since it replaces flat boxes with real modeled facades at the buildings closest to the player.

## 3. LIGHTING (our most winnable fight)
**CP2077's bar**: saturated neon rim-light against desaturated wet streets, RT GI bouncing neon
color onto geometry, dense volumetric haze that makes every light shaft/beam visible, dynamic
day/night with believable street-level pools.
**OUR state**: genuinely closer to CP2077's own tech than most of this list. `r_ddgi` (DDGI
probe-grid GI) defaults **ON** in this world (`host_echotropolis.cpp`:2532) — real bounce light,
not flat ambient. Volumetrics (`ECHO_VOL`) exist and are wired (`fog.volumetric`,
`host_echotropolis.cpp`:493) but are **opt-in only and cost ~100ms/frame (10 FPS measured,
UNPROFILED)** — capture-quality, not playable. `r_rtreflections` and `r_rtshadows` both default
**OFF** in this host (`r_rtshadows` off due to "district self-shadow known" issue) despite the
engine supporting RT tier 2 (sun + point lights) elsewhere. Night capture (`wave_night_drag.png`):
windows and stars read well, but the **ground plane is pure black** — no streetlight pool reaches
pavement. Day is flat/hazy (`tim_deck3.png`, `mesa_noon_postcard.png`) with a visible sun disc and
almost no shadow contrast. Sky day cycle is 2h default, HUD clock reads it — the day/night cycle
itself is real and running.
**THE GAP**:
1. District glow proxies (HANDOFF §8 plan item 2): cluster each district's neon into ~3 big
   low-intensity 200-400m lights appended to `districtLights` — **best value/effort in the whole
   roadmap**, gives the existing volumetric something to scatter from altitude, does before #3.
2. Fix ground-level street-lamp reach at night (`streetLamps`/`lampScene` residency/intensity audit
   at the `wave_night_drag.png` vantage) — cheap placement/intensity bug, not missing tech.
3. Turn `r_rtshadows` back on for this world and fix the district self-shadow bug instead of
   defaulting it off — CP2077-grade shadow contrast is sitting behind one known bug.
4. Profile volumetrics (currently unprofiled at ~100ms/frame) and cut steps/resolution enough to
   run always-on at city scale — this is the tech CP2077's neon-fog identity is built on and we
   already have a working implementation, just too expensive to leave on.
5. SSR ray-march distance is hardcoded to 48m (`vk_passes.cpp:517`, per HANDOFF §8) — promote to
   `r_ssr_maxdist` so wet-street reflections (once wet shading exists) carry neon across boulevards.

## 4. SOUND
**CP2077's bar**: layered radio stations with DJ chatter, dense traffic/crowd ambience, distinct
weapon/cyberware/hack SFX stingers, reactive combat music.
**OUR state**: `host_echotropolis.cpp` (~2355) wires a genuine Cyberpunk-flavored audio kit — 2D
rain+ambient bed, night music, 3 positional 3D loops (mine servers, drone buzz, boat engine idle),
3 UI stingers (confirm/accept/deny). No radio, no weapon audio, no per-hack-type stinger.
**THE GAP**:
1. No footstep/surface SFX and no vehicle engine loop tied to `WorldCars`' driving state — the kit
   has engine sounds; nothing plays on E-enter/throttle yet (matches §1's "cars not fully wired").
2. `HackResult::effect` strings exist per-type in `hackables.h` but (per §1/§9) the registry has no
   placed objects, so there is no hack SFX to differentiate yet — sequence after §9 item 1.
3. Only 3 positional 3D loops island-wide — add per-`EchoRegion` ambient beds keyed to `onBuild`
   (natural fit with the existing region-hook contract) for the CP2077 "every block sounds
   different" texture.
4. No crowd-murmur bed under the LLM talk layer — citizens visibly/textually talk but the street
   carries no vocal texture, the cheapest win for perceived density.

## 5. SCENERY DETAIL (neon signage is CP2077's soul)
**CP2077's bar**: signage stacked floor-to-floor, holographic ads, cable/pipe clutter, wet
reflective ground selling the density even where geometry is sparse.
**OUR state**: we have the two ingredients CP2077 signage needs — an emissive-material system
(already driving every window square) and a real signboard kit (`SB_*` GLBs, referenced in
`echo_interiors.h`'s asset audit) — but grep confirms `SB_`/signboard placement exists **only** in
the not-yet-wired `echo_interiors.cpp`, not anywhere live in `host_echotropolis.cpp`. Captures show
essentially bare streets — `wave_night_drag.png` has a couple of colored blip markers and one dark
box at ground level, nothing reading as signage or clutter.
**THE GAP**:
1. Wire `echo_interiors.h`'s `buildVendorDressing()` into `buildCrown` (per its own integration
   note, step 4 is already done — confirm it's actually reached) and expand it with the
   `SB_GasStation`-style boards along every drag, not just vendor stalls.
2. Scatter the emissive-window system's technique onto a dedicated NEON SIGN pass: colored,
   varied-shape emissive quads/decals stacked vertically on tower facades — this is the cheapest,
   highest-identity CP2077 fix available and needs no new asset work, just placement + color LUT.
3. Street clutter (cones, wires, stands) beyond vendor carts — the 109-model Mega Open World City
   Pack has civic/shop-front clutter beyond what's pulled; expand the scatter pass along RoadGraph
   sidewalks (same mechanism as item 1).
4. Wet/reflective ground shading has no evidence in the codebase — pairs with §3 item 5's
   `r_ssr_maxdist` fix once wet shading exists; sequence after lighting.

## 6. ACTOR DETAIL
**CP2077's bar**: named/scannable NPCs with cyberware tells, dense varied crowd silhouettes,
faction-readable street population.
**OUR state**: `npc_life.h` is the strongest pillar in the project — 12 authored archetypes
(HotDogVendor, BankRobber, Electrician, Programmer, Baker, Gardener, Courier, StreetCop, Preacher,
OffShiftDrone, Fixer, Kid) with daily schedules, scan-card data (name/occupation/one telling
detail), karma-aware hack rules, and **LLM-generated conversation** — a real edge over CP2077's
canned barks, when it's live. Ambient filler crowd is small and uniform: `CrowdConfig cc.count = 40`
+ `mc.count = 9` sharing one body mesh (tint/scale variance only, per `NpcLife`'s single
`m_bodyMesh`).
**THE GAP**:
1. The scan-card system (CP2077's optics-scan analog) is fully authored in data but never surfaces
   — `npcLife.build()` is called with `hax = nullptr` in `host_echotropolis.cpp`:2158, so **no NPC
   is ever actually scannable** despite the karma/detail system being complete. This is the single
   highest-leverage actor-detail fix: pass a real `HackableRegistry*`.
2. 49 total ambient bodies (40+9) sharing one mesh is thin for CP2077-density streets — increase
   count and/or add a second/third body mesh + palette variety so the crowd doesn't read as clones.
3. Pillar board's own note: citizens don't yet enter buildings or react to nearby crime — natural
   follow-on once §9's interiors and §1's hacking land.

## 7. ANIMATION/RAGDOLL
**CP2077's bar**: full mocap locomotion + cover/vault verbs, cyberware finishers, physicalized
ragdoll on every death, vehicle entry/exit anims.
**OUR state**: engine primitives are strong — `anim.h` has GPU compute skinning (palette upload,
scales past a handful of characters), `applyLocomotion` blending, and `applyRagdollBlend`
(per-joint animated↔physics blend). But `npc_life.h`'s own header is explicit: NpcLife agents are
"pure kinematic Scene entities... no physics bodies, no skinning, no per-agent loads" — the 12
archetypes and ambient crowd are NOT riding the skinned/ragdoll pipeline at all in this world.
**THE GAP**:
1. Confirm via `crowd_skin.cpp`/equivalent whether citizens are proc-mesh stand-ins or ever upgrade
   to real skinned rigs — if not, this is the reason NPCs can't visibly react (no animated state to
   drive), a prerequisite for CP2077-grade actor presence.
2. `world_cars.h` teleports ("STASHED") the player capsule to the parked pose on E-enter/exit —
   no climb-in/out clip; cheap CP2077-flavor win once §1's car wiring is finished.
3. No combat/death moment exists in Echotropolis, so `applyRagdollBlend` never fires here even
   though it's engine-ready — lowest priority until §1's combat verb exists.

## 8. PATHFINDING
**CP2077's bar**: navmesh-driven pedestrians and vehicle AI that route around dynamic obstacles,
police that path the real street network mid-pursuit.
**OUR state**: schedule/waypoint-based, not navmesh-based. `npc_life.h` walks agents "the street
grid to REAL destinations" via a `retarget()`-picked post + `viaActive` centerline-follow; freeway
traffic rides a fixed `arc` parameter along `worldFreewaySampleArc`. No navmesh integration exists
anywhere in `app/world_hosts/host_echotropolis.cpp` — the engine's navmesh capability (used
elsewhere in the codebase, e.g. `act2_caves.cpp`) has zero footprint in this world.
**THE GAP**:
1. Confirm the street-grid waypoint graph NpcLife walks and the v7 RoadGraph (cars + planned
   housing lots) are the SAME source of truth — two divergent street representations will drift as
   both pillars grow.
2. No dynamic obstacle avoidance: NPCs and the freeway/robbery cops don't steer around each other,
   parked/hacked cars, or geometry — a cheap separation/steering pass buys most of CP2077's traffic
   feel far more cheaply than a full navmesh port.
3. Audit `StreetCop`'s robbery-converge path (`RobberyPhase`, `retarget()`) to confirm it follows
   the street grid rather than beelining through buildings during the one live set-piece.

## 9. HACKING/SKILLTREES
**CP2077's bar**: quickhacks (ping/short-circuit/reboot-optics/etc.) cast from a RAM pool onto
scanned targets, a deep perk/cyberware tree gated by street cred + XP, escalating gadget/implant
unlocks over the campaign.
**OUR state**: `hackables.h`'s `HackableRegistry` — the closest analog to quickhacks we have — is a
complete, self-tested taxonomy (Camera/JunctionBox/ATM/Vehicle/TrafficSignal/Npc) with real
heat+karma routing via `HackSinks`. It is **entirely unwired in Echotropolis**: no `#include` of
`hackables.h` in `host_echotropolis.cpp`, zero placed objects, and `npcLife.build()`'s `hax` arg is
`nullptr`. Separately, `skilltree.cpp`/`progression.cpp`/`rpg_ui.cpp` form a complete XP→level→
skill-point→node system with real stat mods (damage/speed/reload/ammo/crit/xp/hp) — grep confirms
they're only referenced from `app_run.cpp` (a different world) and never from
`host_echotropolis.cpp`. Zero XP is ever earned in Echotropolis today.
**THE GAP**:
1. Instantiate `HackableRegistry` in `host_echotropolis.cpp` and populate Camera/JunctionBox/
   ATM/TrafficSignal objects city-wide, mirroring the exact proven `WorldCarDef carDefs` pattern —
   this is the single biggest CP2077-identity gap and the machinery is done and tested.
2. Pass a real `HackableRegistry*` into `npcLife.build()` (currently `nullptr`) so NPC scan-cards
   (the RPG-flavored "who is this person" hook) actually surface — shared root cause with §6 item 1.
3. Wire `Progression` + a minimal `SkillTree` purchase screen (`rpg_ui.cpp` already renders one):
   XP sources = hacks, robbery interventions, exploration; ties directly into item 1's hack events.
4. `ctos_terminal.glb` is a placed prop already (`echo_interiors.h`) — repurpose district gates as
   ctOS-style unlocks that reveal `HackableRegistry` markers per district, reusing the existing
   `EchoRegionSet` region-hook pattern.
5. Extend the proven `HackSinks` pattern (already live for `WorldCars`' alarm→crowd `onViolence`)
   to junction boxes/cameras once placed, for parity across every hack type.

---

## TOP 5 HIGHEST-LEVERAGE ITEMS
1. Wire `HackableRegistry` into `host_echotropolis.cpp` (Camera/JunctionBox/ATM/TrafficSignal
   objects + a real `hax` pointer into `npcLife.build()`) — CP2077's core quickhack/scan identity,
   machinery complete and tested, currently at zero placed objects and `nullptr`.
2. District glow proxies for volumetric scatter (HANDOFF §8 item 2: cluster neon into
   ~3 big lights per district, 200-400m range) — best value/effort in the lighting roadmap, the
   fight we're already closest to winning (DDGI is ON; volumetrics exist, just need a source).
3. Neon signage placement pass — wire `echo_interiors.h`'s `SB_*` signboards + a colored emissive
   decal system onto tower facades; no new assets needed, this is CP2077's most recognizable
   missing signal and the cheapest fix on the list.
4. Wire `Progression` + `SkillTree` (already complete, only referenced from the wrong world) so
   hacks/robbery/exploration earn XP — pairs directly with item 1, unlocks the RPG-depth bar.
5. Fix ground-level night lighting (`streetLamps`/`lampScene` reach) + turn `r_rtshadows` back on
   for this world — two cheap, already-built fixes standing between the current black-ground night
   captures and CP2077-grade street-level contrast.
