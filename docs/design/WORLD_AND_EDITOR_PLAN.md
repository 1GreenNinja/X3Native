# X3Native — Full World Adoption + Level Editor Plan
*Synthesis of the complete Q3Engine Babylon source (160 files / 6.1 MB) + the Task9D LevelArchitect, mapped to a native C++/Vulkan adoption + a UE-style editor. Authored 2026-05-23.*

> **Why this doc:** the Babylon build (`C:\GameDev\OneDrive\GameDev\Q3Engine\src`) is the *fully-built-out world vision* — it dies on FPS because Babylon pushes thousands of CPU meshes, but it defines exactly what the native engine must carry at speed. The Task9D LevelArchitect HTML is the canonical Spire geometry. This plan turns "use ALL of it" into an ordered, testable native roadmap.

---

## PART A — The World (what exists, where it sits)

### A.1 Vertical coordinate datums (the spine of everything)
One global Y-stack organizes the whole world (`src/core/x3-state.js:40-44`, `:190-198`):

| Datum | Y | Meaning |
|---|---|---|
| `ARENA_Y` | **+600** | Floating combat arena / space layer |
| `ATMOSPHERE_TOP` | -200 | Gravity ramp boundary (8→28 m/s²) |
| `PLANET_Y` | **-300** | Planet surface datum (world origin XZ = crash site 0,0) |
| `OCEAN_SURFACE_Y` | -308 | Sea level |
| `MAZE_Y_CEILING / FLOOR` | -305 / -315 | Underground maze |
| Command Center | -370 | Deep hub |
| Seafloor base | -346..-334 | 3-level disc |
| `OCEAN_FLOOR` | -360 (shallow) … -480 (trench) | |
| `VOID_FLOOR_Y` | **-500** | Death floor — **Club 1127 "The Deep" lives at ≈ -500** (`PLANET_Y-200`, `x3-console.js:411`) |

`WORLD_RADIUS=15000`, storm wall at 14500. Zones (space/surface/underground/underwater/club) switch on a `_currentZone` string keyed off these Y-bands — drives fog, post-FX, music, reverb, culling.

### A.2 The Spire (Task9D LevelArchitect v10.9 — canonical dims)
The hero building. `const FLOORS` (Task9D HTML:4706) + shaft constants:
- **Floors:** SUB -170 · F1 Detention 0 · F2 Medical 10 · F3 Genetics 20 · F4 Cybernetics 30 · **F4.5 Nexus ~35-53 (tiered)** · F5 Drone 65 · F6 Alien Tech 78 · F7 Executive 91 · ROOF 104.
- **Elevator shaft:** X=22, Z=-29.5, 4×4, spans Y -175→108 (**283 m, 8 stops**). Floor plates are 52×54 with a shaft gap (x 19-25, z -32→-26).
- **182 m descent tube** (8 angled sections) + 3 explorable offshoots + cave system at **-178 m**, hidden sub-level at **-170 m**. ~60 rooms, 7 bosses.
- Native status: `main` already re-laid **F1 to a real ~75×43 m, 29-room detention complex** (`app/level1.*` `L1DetentionRoom` table, transcribed from this LevelArchitect). F2-F7 dimensioning is an active lane (Snake13700k). **The descent tube + cave + sub-level + Club-at-the-bottom are NOT yet in native.**

### A.3 Surface ring (6k–12k out from origin)
- **Two cities:** Scrapyard City (`buildCity`, center -600,500, ~23 buildings + 50 NPCs) and New District (`buildSurfaceStructures`, center 200,500, ~28 buildings) joined by `buildCityRoads` (grid + connectors + traffic lights).
- **4 mountain ranges** (N snow / E volcanic+caldera / S mesa+ruins / W crystal) with **4 drivable road tunnels** boring through them.
- **Nature:** ~80 trees / 4 lakes / grass+flowers thin-instanced, biome-scattered, terrain-conformed.
- Terrain: 30k×30k heightmap, `ps2_heightAt(x,z)` is the height oracle every prop samples.

### A.4 Underground
- **Deep maze** (`buildMaze`): 5 arteries from a central hub "The Nexus", ~1700×1800 m, junction rooms + 15 dead-ends, biome-themed, lava/crystal features. Reached by 5 surface cave-mouth ramps (`x3-maze-entrances`) + teleporters.
- **Command Center** (-370): circular hub + armory/barracks/medbay/generator + 36 terminals.
- **Arena under-complex** (`x3-arena-underground`): Ops/Armory/Reactor + 5 live security-camera RTTs.

### A.5 Underwater (the "HUGE" one — confirmed)
- **Seafloor base** (`x3-seafloor-base`, center 1100,-1400): a **160 m-diameter, 3-level disc on a 34 m stalk**, **177 workstations** (thin-instanced, 4 rings), reactor, bridge, war room, science lab w/ Salvari artifacts, glass dome. The single largest interior.
- 5 ocean landmarks: Sunken Outpost, Coral Reef, Kelp Forest, **Deep Trench + Leviathan lair**, Sub Dock.
- Access: 38 m sub-elevator (surface→base), 815 m emergency tunnel (maze→base L3), 20×10 sub blast-door bay. Player wet/dry state swaps at an airlock.

### A.6 Club 1127 "The Deep" (the very bottom — user priority)
- A faithful model of Tim's real Miami club. **15.24 × 30.48 × 9.14 m (50×100×30 ft)**, sealed box, at world **Y ≈ -500** — the lowest authored interior, entered via the elevator **disco code `1127`** through a 3.5 m gap in its east wall.
- Contents: 2-story engine room + lounge (interactive glass doors), suspended DJ booth (keypad door) + aerial neon bar, ~450-tile dance floor, THE ORB mirror ball (4 spotlights), ground bar + 7 stools, 6 POE TVs, 36-speaker rig, 26 pulsing blacklights, VIP couches (romance zones).
- Native status: `app/club1127.cpp` exists. **Verify it matches these `D`-block dims + sits at the shaft bottom.**

---

## PART B — The Engine/Systems (what the world runs on)

Mapped from all of `src/` (systems 46 · gameplay 29 · entities 30 · features 13 · audio 7 · ui 5 · core 4 · workers 2). Native-relevant inventory:

**Gameplay DATA to mirror exactly** (mostly JSON in `Q3Engine/data/`):
- `enemies.json` — **89 enemies** (not 52), schema hp/dmg/speed/armor/drops/abilities, acts 1-4.
- `bosses.json` — **18 bosses**, phase machines (hpRange→behavior+attacks), arenas. Final: the_overlord 15000 HP / 4 phases.
- 12-weapon roster w/ exact balance (`x3-weapons-fire.js:75-158`).
- Player movement consts (`x3-state.js:182-186`): capsule 2.0/0.45, ground 18 / air 12, jump 3.5, dynamic gravity 8→28.
- powerups (6), damage types (6 + armor presets), companions (5), karma/humanity, infection (9-min), skills (36/3 trees), inventory (40-slot), quests, story (4 acts/10 endings).

**Engine subsystems with native analogs already present or needed:**
- Render/LOD/cull: material dedup + mesh-merge + freeze + distance/zone cull (`x3-optimize`, `x3-lighting`, `x3-lod-worker`). Native already merges/instances — this is the FPS fix Babylon lacked.
- Physics: Jolt backend (native) now has dynamic bodies + **point joints + ragdoll** (this session). Babylon had `x3-ragdoll.js` (limb-clone + impulse) as reference.
- Multithreading: SharedArrayBuffer bot-AI worker (Q3 van-Waveren model, 16 bots) + LOD worker — map to native job system.
- Save: versioned/checksummed/compressed schema (`x3-save-system`, SAVE_VERSION 2) + per-system serialize/deserialize.
- **Level format:** `x3-level-builder.js` streams **50 JSON campaign levels** (4 acts, max 3 simultaneous for open-world), `BIOME_PRESETS`, level def = `playerStart` + `areas[]→entities` + `objectives` + `bossEncounters`. **This JSON schema is the natural native level format + the editor's save target.**

**Critical finding for the editor:** there is **NO existing level editor** in the Babylon source. "Level Architect" is the standalone Task9D HTML tool; in-game, levels are external JSON consumed by `x3-level-builder`. The Babylon Inspector (`scene.debugLayer`) is the only runtime tweak tool. So the native editor is **new build** — but the JSON level schema already exists to target.

---

## PART C — Native Adoption Roadmap (full building + tunnels + Club + seafloor)

Ordered so each phase is independently testable (headless `--test-*` + gate) and lands on its own `feat/` branch for the 13700K integrator. **Worker-machine parallelizable** (matches the i5000/Snake/DJBOOTH dispatch model).

**P1 — Complete the Spire vertical (extends current F1 relay).**
Native already has F1's 29-room detention. Add: F2-F7 real dimensioning (Snake lane, in flight) → the 182 m **descent tube** (8 spline segments) → **cave system (-178)** + **hidden sub-level (-170)**. New `app/spire_descent.*`. Test: floor footprints + tube reachability + 8 elevator stops land on walkable geometry.

**P2 — Club 1127 at the shaft bottom.**
Verify/extend `app/club1127.cpp` to the `D`-block dims at Y≈-500; wire the elevator disco-code `1127` path to descend there. Test: club footprint + east-wall entrance gap + elevator-code reachability.

**P3 — Seafloor base (the HUGE interior).**
New `app/seafloor_base.*`: 160 m 3-level disc + stalk, 177 thin-instanced workstations, reactor/bridge/warroom/sciencelab, glass dome; airlock wet/dry + sub bay. Reuse the act2/spire authoring pattern. Test: 3 levels build, footprint, airlock toggles player state, 177 stations instanced.

**P4 — Surface ring.** Terrain heightmap + `heightAt` oracle → 2 cities + roads → mountains + 4 tunnels → nature scatter. Largest art lift; leans on native merge/instance/cull for the FPS win. Test: prop counts, height-conform, city/tunnel footprints.

**P5 — Underground maze + command center + arena under-complex.** 5 arteries + hub + dead-ends; cave-mouth ramps from surface; teleport network. Test: artery reachability, hub teleport targets.

**P6 — Connectivity + zone streaming.** The `_currentZone` Y-band system → native zone manager driving fog/post-FX/music/cull + the 50-level JSON streaming (`x3-level-builder` schema). Test: zone transitions, additive load/unload by distance.

Throughout: port the **JSON data tables** (enemies/bosses/weapons/etc.) as the canonical native data — they're already engine-agnostic.

---

## PART D — Native Level Editor (UE-style) — "Level Architect, but native + fast"

**Verdict: achievable, and the native engine is a better substrate than Babylon was.** Task9D died on FPS; native merge/instance/cull renders the whole built-out world fast, so the editor can show it all. ~60% of editor plumbing already exists: Vulkan viewport, HUD text/quads, `worldToScreen`, Jolt `rayCast` (= click-pick), console/cvars, fly cam, settings persistence, headless test harness.

**Accelerator: Dear ImGui** (MIT — clean-room-safe, the game-editor standard). Gives docked panels / details inspector / content drawer fast instead of hand-rolling HUD UI.

**Save target:** the existing `x3-level-builder` JSON schema (`playerStart` + `areas[]→entities` + `objectives` + `bossEncounters` + `biome`). Editor writes it; `x3-level-builder` (ported) loads it. Closes the authoring loop.

| Phase | Deliverable (= Level Architect parity, then UE polish) | Lift |
|---|---|---|
| **E1 MVP** | ImGui integration + viewport (Orbit/Fly/FPS-walk, Solid/Wire, POS readout, floor toggles) + **click-select via rayCast** + **move gizmo** + **details/inspector panel** (entity transform/tint/tag/flags) + **JSON level save/load** | Medium |
| **E2** | Rotate/scale gizmos · multi-select · grid snapping · **content browser** (enum GLBs + render-to-texture thumbnails) + **drag-to-place** · undo/redo command stack | Medium |
| **E3** | **Pathway/spline editor** (the descent tube) · room/prefab stamping · **play-in-editor** (engine already runs the game — add a mode switch) · the UE details niceties (categories, per-component) | Medium-Large |

**Reality check:** full UE parity is multi-month (an editor rivals an engine in size). We target **Level Architect parity + UE polish**, phased; E1 already matches most of what Task9D showed. Builds in `app/editor/` behind an `--editor` flag (or `F9`, echoing Task9D's Editor key), reusing the existing viewport + scene + physics.

---

## Quick reference — canonical constants
- Spire floors/shaft: Task9D `Task9D_AllFloors_v10_3D_Models & Editor.html:4706` (`FLOORS`), `:4713-4714` (shaft).
- World datums: `Q3Engine/src/core/x3-state.js:40-44`, `:190-198`.
- Club 1127 dims + Y: `x3-club1127.js:33-55`; spawn at `x3-console.js:411` (PLANET_Y-200).
- Seafloor base: `x3-seafloor-base.js` (DISC_RADIUS 80, 3×LEVEL_HEIGHT 6, STALK 34, 177 stations).
- Level format: `x3-level-builder.js` (50 levels, areas→entities, biome presets).
- Native today: `app/level1.* L1DetentionRoom` (F1 relay), `app/club1127.cpp`, `app/act2_world.*`, `app/spire_mid/top/nexus/sublevels.*`.
