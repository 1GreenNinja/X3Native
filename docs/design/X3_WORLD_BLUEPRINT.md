# X3 / Escape From Lab Zero — MASTER WORLD BLUEPRINT

> The complete game **as already built** in the Q3Engine web codebase, **reconciled to the 100-level / 4-act / 12-ending design canon**, as the authoritative target for the native **X3Native** (C++20 / Vulkan) engine.
>
> *Generated 2026-05-23 by the 13700K integrator from three deep exploration passes (SRC world gazetteer · SRC systems catalog · design-doc canon) over Tim's own IP. No third-party (id Tech / RBDOOM / Quake) engine source consulted — clean-room intact.*

---

## 0. SOURCES & AUTHORITY CHAIN

Three bodies of truth feed this blueprint. When they disagree, this is the precedence:

1. **`Q3Engine\src` (the SRC trove)** — `C:\Users\Tim Smith\OneDrive\GameDev\Q3Engine\src\`, ~100+ `x3-*.js` Babylon modules. **The NEWEST + most complete implementation** of the real game ("THE LONGEST YARD"). *Came AFTER LevelArchitect and Task9D.* **Primary source for the world, the elevator, Club 1127, the undersea base, and all gameplay systems.** ("Q3Engine" is a misleading name — it is Tim's own X3 game, not id Tech.)
2. **`Task9D_AllFloors_v10_3D_Models & Editor.html`** (`…\OneDrive\GameDev\DellGameDev\Escape48BLN\`) — the all-floors 3D Spire model. **Authoritative for the Spire vertical dims** (its `FLOORS[]` == the elevator `FLOOR_DEFS`). Pre-LevelArchitect.
3. **LevelArchitect v10.9** (`…\GameDev\LevelArchitectFullV10.9\js\Config.js`) — Floor-1 detention room geometry only (29 rooms, ~75×43 m). F2–7 were never authored there.
4. **Design canon** — in-repo `docs/design/EFLZ_{MASTER_PLAN,WORLD_STRUCTURE,NARRATIVE,BESTIARY}.md` + the master corpus `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` (TASK_1–14 + narrative bible). **Authoritative for the 100-level structure, the 12 endings, character arcs, factions.**

**Native state today** (`origin/main`): Act-1 built to canon (the 7 floors + F4.5 Nexus + F7 sub-levels, all bosses, Sarah's hack); an Act-2 surface host (L8/L9); F2–7 given real-scale footprints. But the native Spire is **~8× vertically compressed** vs the real model and the open world / souped-up elevator / Club 1127 / undersea base are **not yet ported**.

---

## 1. THE WORLD — GAZETTEER & COORDINATE MAP

**World scale:** 15,000-unit-radius planet (Keth'zar). Soft boundary 13,000, hard wall 14,500. 1 unit ≈ 1 meter. RH, +X right / +Y up / −Z forward.

**Key Y planes:** `ARENA_Y=600` (floating arena sits at Y≈1200) · `PLANET_Y=−300` (surface) · `OCEAN_SURFACE≈−300..−346` · `SEAFLOOR≈−380` · `MAZE −305..−315`.

### Region map (world XZ + Y)
| Region | World pos (x, y, z) | Extent | Status in SRC |
|---|---|---|---|
| **The Spire / Facility** | (3500, 0, −2000) | 48×60 footprint, Y −175→108 | shell + elevator ✅; **per-floor interiors = STUB** |
| **Souped-up Elevator** | shaft at (22, –, −29.5), Y −175→108 | car 3.8×5.2×3.8 | ✅ complete (1,807 lines) |
| **Club 1127** | beneath facility, **Y=−200** | 50×100×30 ft | ✅ complete |
| **Descent Tube** | facility → caves | 182 m, 8 sections | **STUB** (reference only) |
| **Caves** | `CAVE_Y=−178` | H 8 | light props ✅; **Salvari crystals = MISSING** |
| **Hidden Sub-Levels** | `SUB` Y=−170 | collision shell | **STUB** (no interior) |
| **Undersea Base** | (1100, −346, −1350) | 80-radius 3-level disc | ✅ complete (177 workstations, reactor, sub-dock, airlock) |
| **Crash Site** (start) | (0, −300, 0) | — | ✅ |
| **East Outpost** | (800, −300, 400) | military camp, antenna farm | ✅ |
| **West Outpost** | (−880, −300, −320) | drill rig, processing plant | ✅ |
| **North Cave entrance** | (−200, −300, 1000) | cyan biolume → maze | ✅ |
| **South Cave entrance** | (320, −300, −920) | geothermal → maze | ✅ |
| **Northern Mtn Range** | R≈6000 N | snow peaks 150–250 m | ✅ |
| **Eastern Mtn Range** | R≈8000 E | volcanic basalt + lava veins | ✅ |
| **Southern Mtn Range** | R≈7000 S | mesa/sandstone + ruins | ✅ |
| **Western Highlands** | R≈9000 W | mossy + crystal formations | ✅ |
| **Scrapyard City / New District** | (−600, −300, 500) / (200, −300, 500) | road grid + freeway | ✅ |
| **Freeway Tunnels** | through mountains | 4 named bores | ✅ |
| **Maze / Underground Nexus** | hub (0, −315, 0) | octagonal hub + 5 biome arteries + 15 hazard rooms | ✅ complete |
| **Floating Arena** | Y≈1200 | cross/star platforms, jump pads, teleporters | ✅ complete |
| **Space / Skybox** | dome R=2200 | 1500 stars, 2 moons, sun + corona | ✅ complete |
| **Warehouse** | near facility/outpost | crates/racks/tanks | present (light) |

### Vertical layer stack
```
Y≈+1200   FLOATING ARENA  (+ space dome above)
Y=+104    ROOF / Helipad
Y=0       SURFACE (PLANET_Y=−300 ground) — facility tower base, city, roads
Y=−170    SUB hidden level   |   Y=−178 CAVES   |   Y=−200 CLUB 1127
Y=−346    UNDERSEA BASE (seafloor disc, sub-dock)
Y=−315    MAZE NETWORK (hub + arteries)
```

---

## 2. THE SPIRE (Act 1 vertical) — REAL DIMS, ELEVATOR, CLUB, FLOORS

### 2.1 Authoritative floor table (Task9D `FLOORS[]` == elevator `FLOOR_DEFS`)
Shaft **−175 → 108 (≈283 m, 8 stops)** at **X=22, Z=−29.5**. **NON-uniform** pitch:

| Stop | Y (m) | Canon identity | Boss | Native y0 today |
|---|---|---|---|---|
| SUB Hidden | **−170** | hidden sub-levels (Return Mission) | Frozen Collective | −5/−10/−15 |
| F1 Detention | **0** | Awakening / detention (32 cells) | **Chief Martinez** | 0 |
| F2 Medical Bay | **10** | 3 timed rescues (Aria/Keisha/Emily) | **Dr. Chen** | 10 |
| F3 Genetics Lab | **20** | gene-vats, nursery | **Failed Experiment #7 (Marcus Webb)** | 15 |
| F4 Cybernetics | **30** | augmentation, Humanity meter | (→ F4.5) | 20 |
| **F4.5 Nexus** | **~35** (30 m∅ × 25 m) | the in-between chamber | **The Collective / Chorus** (5 minds) | (off-elevator) |
| F5 Drone Station | **65** | the drone level | **Swarm Controller AI** + Sarah's hack | 25 |
| F6 Alien Tech | **78** | Salvari first contact, the cure | **Alien Overseer** | 30 |
| F7 Executive | **91** | finale, Sarah, timeline LOCK | **Jake's Clone** | 35 |
| ROOF Helipad | **104** | escape → L8 surface | — | (none) |

> **Native gap:** native uses a **uniform 5 m pitch (35 m total)**. To match, the vertical stack must be re-scaled to these Y values (≈283 m), which touches the elevator stop pitch + everything that reads `floorBaseY[]` (spire_mid/top/nexus/sublevels). **Big change — phased below.**

### 2.2 The souped-up elevator (`features/x3-elevator.js`, 1,807 lines — UNPORTED)
Premium **glass car** (3.8×5.2×3.8, MAX_SPEED 14, accel 6 / decel 8) on **4 steel cables (300 m)**. **Must port in full:**
- **10-state FSM:** IDLE · ACCELERATING · CRUISING · DECELERATING · ARRIVING · DOORS_OPENING · DOORS_OPEN · DOORS_CLOSING · EMERGENCY_STOP · **FREEFALL**.
- **Earth-strata scroll display** (behind glass): 9 layers — Sky&Concrete (+200) → Foundation → Limestone → Granite → Basalt → Obsidian → **Crystal Veins** (glow) → **Magma** (glow) → **Alien Substrate (−400)** (glow).
- **Twin OLED viewscreens** (geo survey + floor directory), back-wall **mirror**, **blue access terminal** + keypad, ceiling fluorescent.
- **Procedural audio:** motor hum 40→120 Hz, wind bandpass, 60 Hz mains drone, random creaks (3 s), floor-passing dings (880 Hz), pentatonic muzak (72 BPM).
- **Horror events** (8% on floors 0 & 3): shake + emergency strobe.
- **DISCO MODE — keypad code `1127`** → 128 BPM Cm7 disco kit, mirror ball + 4 spots + strobe (4 Hz), glass cycles color, ¼-speed; **descends to Club 1127 at Y=−200**.

### 2.3 Club 1127 (`world/x3-club1127.js` — UNPORTED) — *Tim's real Miami nightclub*
Y=−200, **50×100×30 ft** main room. Suspended **DJ booth** (turntables, mixer, 2 OLED, keypad door), **the ORB** (2 m mirror ball), **aerial bar** + **ground bar** (7 stools), 2-story engine-room/lounge (12-step stair), VIP couches. **Real PA rig:** 4× SVS PB16-Ultra subs, 8 pairs JBL JRX200 + 2500 W amps, 4× JBL 18" subs, 16× surrounds; **28 blacklights**; 6-screen TV multiplex (POE). 80 m cull, materials frozen, meshes merged.

### 2.4 Per-floor content (canon, from design docs)
Each floor = **open eastern arrival/combat hall** (the existing encounters) + **west-wing identity rooms** (native floors2-7 pass) — but the **real designed rooms** live in Task9D/LevelArchitect (F1 = 29 detention rooms) and the design docs (F2 wards + breeding chambers; F3 specimen tanks + nursery + spawning chamber; F4 augmentation bay + brain-upload + contract room; F5 manufacturing/repair/recharge depots + hive-mind chamber + 12 turrets; F6 Salvari cells + ancient-tech archive + cure lab + K'thara quarters; F7 executive suite + clone pods).

### 2.5 ⚠️ GAPS TO ADD (Tim, 2026-05-23 — NOT in SRC, MUST be in the native build)
- **(a) Salvari crystals in the caves** — the −178 m cave story beat (singing/lore crystals, alien markings). *Not implemented anywhere; author fresh.*
- **(b) The later-discovered hidden sub-levels** — F7 hidden descent **SL1 Waste Disposal / SL2 Cryo Storage [Frozen Collective] / SL3 Enhanced Interrogation → Dr. Chen Return**, gated on Clone-dead + Sarah-saved; + the Act-4 **L45 "Return to Lab Zero"** 3 NEW sub-levels (Breeding Core / Chen's True Lab / the 65-Myr Original Artifact). *Already built in native `app/spire_sublevels.*` — reconcile into the real-scale world.*

### 2.6 Vertical canon (Tim, 2026-05-23 — RESOLVED)
- **Club 1127 is the BOTTOM** of the facility (Y=−200) — the lowest social hub.
- **The "descent tube" is canonically the ALIEN TUNNELS** — a network of **MASSIVE alien slide-tubes** connecting **F1 Detention (Y=0) down to Club 1127 (Y=−200)**; a human "easily slides down them" (~200 m, ~12 s slide). The −178 m **caves** + the hidden **SUB level (−170)** branch off these tunnels (the **Salvari crystals** live in these caves).
- **The elevator strata are LITERAL / reachable** (not merely atmospheric): the deep layers — Crystal Veins → Magma → **Alien Substrate (down to −400)** — are **real explorable zones** reached via the alien-tunnel network (the alien / Overlord-origin depths beneath the facility). The native world must extend down into them — not stop at the elevator's −175 SUB stop.

---

## 3. SYSTEMS CATALOG — native: HAS / EXTEND / NEW

**Already in X3Native (HAS):** Vulkan renderer, Jolt physics, GLTF, skinned animation, monster/combat AI, weapons, terrain streaming, navigation, netcode, save/load, UI, audio, GPU destruction.

**EXTEND existing native systems:** LOD/cull (worker→thread pool) · light pooling + distance cull · post-FX per-zone (bloom/ACES/vignette) · animation sinusoids · NPC pathing on the existing navmesh · input bindings · RPG/skill/craft/progression math · story branching + karma/humanity tracking.

**NEW — must build (the differentiators that make X3 *X3*):**
| System | Source file | Key spec |
|---|---|---|
| **Strata elevator** | `x3-elevator.js` | 10-state FSM + 9 strata + disco/Club hook (§2.2) |
| **SPH fluid** (meltwater) | `x3-sph-fluid.js` | 600-particle spatial-hash, flow/evaporate, GPU fluid render |
| **NPC mood + schedule sim** | `x3-npc-system.js`, `x3-npc-schedules.js` | 4-layer (awareness 1Hz / fuzzy mood 2Hz / FSM 5Hz / anim), 200 pool, patrol/work/flee |
| **Procedural NPC gen** | `x3-npc-{face,hair,body,outfits}.js` | faces/hair/bodies + 8 outfit palettes |
| **AI-powered dialog** | `x3-ai-dialog.js` | **3 selectable in-game modes (RESOLVED): scripted trees / live Claude+Grok / hybrid.** Claude/Grok calls for Sarah/K'thara/Ashley; built mode-agnostic |
| **Submarine combat** | `x3-submarine-combat.js` | enemy subs, torpedoes (40 dmg), depth charges, hull regen, Leviathan |
| **Infection timer → branching** | `x3-infection.js`, `x3-story.js`, `x3-karma.js` | 9-min 4-stage, cure rates [0,90,60,30,0]%, timeline locks |
| **Damage-type system** | `x3-damage-types.js` | ballistic/explosive/energy/fire/electric/cryo + resist/DOT/slow/chain |
| **Destructibles** | `x3-destructibles.js` | HP objects, debris pool, respawn queue |
| **Grapple hook** | `x3-grapple.js` | raycast attach, pendulum swing (range 80, accel 15) |
| **Weather** | `x3-weather.js` | 7 states, 30 s transitions, biome-gated, puddles |
| **Time-of-day** | `x3-tod.js` | 4-phase 6-min cycle, dynamic sky/sun/city-lights, aurora |
| **Vehicles** | `x3-vehicles.js` | heli/airship/sub/rover, hackable, combat AI |
| **Q3 fuzzy-logic bot AI** | `bot-ai-worker.js` | 20+ traits, 6 combat styles, 8-state FSM, 10 Hz |
| **Photo mode / cutscenes** | `x3-photo-mode.js`, `x3-cutscenes.js` | free-cam gallery; Hermite-spline cinematics |
| **Skill trees / crafting / karma** | `x3-skills.js` etc. | 3 trees/36 skills; 20 recipes/5 stations; karma −100..+100 + Humanity 0..100 |

---

## 4. THE 100-LEVEL / 4-ACT CAMPAIGN

**Spine:** the original 50-level design (Acts split 7/13/15/15) is the canon; the **100-level expansion** roughly doubles Acts 3–4 to add "real space exploration + life on other planets before rallying to save Earth." Acts 1–2 stay close to the spine.

| Act | Levels | Setting | Premise |
|---|---|---|---|
| **1 — Lab Zero (the Spire)** | L1–8 + F4.5 + F7 sub-levels | underground facility on Keth'zar | wake as Subject 7-Alpha, fight up 7 floors, rescue Sarah; **timeline locks at F7** |
| **2 — Keth'zar surface** | L8–35 | alien open world | emerge, survive biomes, recruit the **Salvari**, steal the **Storm Runner**, leave the planet |
| **3 — Beyond the Stars** | L36–75 *(the expansion)* | space + other worlds | galactic alliance-building, Salvari Prime, Memory Hunter, fleet assembly, discover Earth is invaded |
| **4 — Earth Liberation** | L76–100 | invaded Earth | break the blockade, regional liberation, Return to Lab Zero, Mothership raid, **4-phase Overlord finale → 12 endings** |

### 4.1 Act 1 level-by-level (full detail)
- **L1 Detention/Awakening** — boss **Chief Martinez** (3-phase, sympathetic; kill/spare). Pistol + Bazooka.
- **L2 Medical Bay** — **THE TRIAGE:** Room A **Aria** (5:00→Siren), Room B **Keisha** (7:00→Breeder Queen), Room C **Emily** (4:00→Oracle) + Sarah's background timer. Boss **Dr. Chen** (kill=50% cure / cure=100% + ally).
- **L3 Genetics Lab** — boss **Failed Experiment #7 (Marcus Webb)**, Memory-Flash on pod destruction. Marcus's badge = F4 key.
- **L4 Cybernetics** — **Humanity meter** + augmentation; fleet reveal. → **L4.5 Nexus Chamber: The Collective/Chorus** (5 merged minds; consciousness-rescue minigame, save up to 4; drops ChainGun).
- **L5 Drone Station** — boss **Swarm Controller AI**; **Sarah's 90-s 3-phase Master Hack** flips the drone army; drone shop. Plasma Rifle. (5-drone roster: Surveillance/Combat[BlueSynth]/Medical/Breeder/Hacker.)
- **L6 Alien Tech** — boss **Alien Overseer**; **Salvari first contact + K'thara**; the **cure**; trust choice (ally/attack). Lightning Gun.
- **L7 Executive** — boss **Jake's Clone** (3-phase mirror; kill/incap/neural-hack); **timed Sarah rescue → TIMELINE LOCK (Omega/Alpha/Beta/Gamma).**
- **L7.5 Hidden Sub-Levels (Return Mission)** — SL1 Waste Disposal / SL2 Cryo Storage (**Frozen Collective**) / SL3 Enhanced Interrogation (**Dr. Chen rescue** → cure quality). Gauntlet back up F7→F1.
- **L8 Surface Emergence** — pursuit drones; reveal **NOT EARTH** (Keth'zar, twin suns, purple sky); Salvari scout R'thek.
- **Secrets:** **Club 1127** (code 1127, hub) · **Deep Tunnels** under B1.

### 4.2 Act 2 (L8–35, Keth'zar) — selected beats
L9 Crystalline Desert Edge · L10 Desert Depths (first Salvari camp) · L11 **Salvari Camp / Refugee Haven** (K'thara recruit, hub) · **L12 Advanced Caves** (Crystal Heart, boss **Memory Hunter**) · L13–14 Toxic Swamplands (Beta: **Siren/Aria**) · L15 Tree Cities · L16 Ruined Metropolis (Beta: **Breeder Queen/Keisha**) · L17 Downtown · L18 Underground Resistance · L19 Spaceport Approach · **L20 The Spaceport** (capture **Storm Runner**, boss Garrison Commander). L21–35 extend the alliance arc.

### 4.3 Act 3 (L36–75, space) — selected beats
Orbital escape + asteroid field · **Salvari Prime trilogy** (Memory Hunter, K'thara's dead homeworld) · Mining Colony liberation · **Space Casino "Fortune's End"** (Beta: **Oracle/Emily**) · Asteroid Rebel Base (Crystal Heart install) · Fleet Assembly · Warp Gate Assault · **Sol Arrival** (Earth already invaded). Storm Runner = mobile hub; Omega **wedding** beat.

### 4.4 Act 4 (L76–100, Earth) — selected beats
Orbital Assault (Dreadnought "Extinction") · Atmospheric Entry (pick landing zone) · Base Establishment (The Broadcaster) · **Regional Liberation** (5 continents, 3 mission types, scaling) · NY Reclamation (**PRIME-7**) · London / Tokyo · **L45 Return to Lab Zero** (3 new sub-levels; 65-Myr Overlord-origin reveal) · United Earth counter-offensive · **Mothership raid** · Proto-Overlord gauntlet (5 corrupted leaders) · **L100 Overlord Core** (4-phase: Avatar → Consciousness/5 nodes → Truth → P4 non-combat choice).

### 4.5 The 12 endings
Determined by **timeline lock (F7)** × **morality axes** (Humanity, Trust, Mercy, Love, Redemption, Augmentation) × **ally count (Alliance Points)** × **finale P4 choice** (Destroy/Negotiate/Sacrifice/Alliance):
1 Golden (perfect Omega) · 2 Good · 3 Bittersweet · 4 Tragic (Sarah's sacrifice) · 5 Fractured · 6 Dark (Jake corrupted) · 7 Nightmare (Gamma, Sarah=The Bride) · 8 Solo Victory · 9 K'thara Romance (Beta) · 10 Polyamorous Family (Alpha, all 3 saved) · 11 Chen's Redemption · 12 New Beginning.
**Timelines:** Omega (all saved, perfect) · Alpha (~65%, women saved/Sarah lost) · Beta (~25%, Sarah saved/women→bosses) · Gamma (fail).

### 4.6 Factions & key cast
**Factions:** Dominion (grey architects of Lab Zero) · Verthani (insectoid soldiers) · Illuminated (energy-elite/psychic) · Salvari (refugee allies) · the **Overlord** (hive consciousness, 2,847 worlds, final boss) · Void Pirates (neutral) · Human Resistance / Collaborators (Earth).
**Cast arcs:** **Jake** (Subject 7-Alpha, the throughline) · **Sarah** (hacker; marry/leave/sacrifice/lost) · **Aria/Keisha/Emily** (rescues or Siren/Breeder Queen/Oracle bosses) · **K'thara** (Salvari mentor, optional Beta romance) · **Dr. Chen** (corrupted→redeemable, gates cure quality) · **Jake's Clone** (mirror) · the **Overlord**.

---

## 5. NATIVE X3NATIVE — CURRENT STATE vs TARGET

| Area | Native today (`origin/main`) | Target (this blueprint) |
|---|---|---|
| Spire floors | B1 detention (29 rooms) + F2–7 footprints/west-wings; 5 m uniform pitch | real 283 m non-uniform stack + real per-floor rooms |
| Elevator | plain platform lift | the souped-up strata/disco elevator |
| Club 1127 | none | full club at Y=−200 |
| Caves / sub-levels | `spire_sublevels.*` (SL1–3, gated) at −5/−10/−15 | real −178 m caves + Salvari crystals + deep sub-levels |
| Act 2 | `act2_world.*` host + L8/L9 graybox | L8–35 Keth'zar open world |
| Open world | terrain streaming + water + L1 interior | undersea base, mountains, city, freeways, maze, arena, space |
| Bosses | Act-1 bosses to canon (Martinez/Chen/FE#7/Chorus/Swarm/Overseer/Clone) | + Memory Hunter, Proto-Overlords, Overlord finale |
| Systems | renderer/physics/anim/AI/weapons/terrain/net/save/UI/audio/destruction | + the NEW list in §3 |
| Endings/arcs | — | the 12-ending timeline system + character arcs |

---

## 6. BUILD ROADMAP (phased; fleet lanes)

The native engine already nailed Act-1-to-canon. The blueprint adds **scale + world + systems**. Suggested phases (each = fleet-dispatchable `feat/` lanes, gated Release+Debug 0-VUID/leak-clean, integrator merges):

- **Phase 0 — Spire to real scale.** Re-scale `level1.cpp` floor Y to the §2.1 table (283 m, non-uniform); update the elevator stops + everything reading `floorBaseY[]`; transcribe the real F1 (29 rooms) + F2–7 designed rooms. Add the **descent tube** + **−178 m caves + Salvari crystals** + reconcile the deep sub-levels. *(Supersedes `feat/floors2-7-dims`.)*
- **Phase 1 — The souped-up elevator** (`x3-elevator.js` port): 10-state FSM, strata display, OLED, audio, disco → **Club 1127** (`x3-club1127.js` port at Y=−200).
- **Phase 2 — Act 2 open world** (Keth'zar L8–35): extend `act2_world.*` to the real surface — mountains, outposts, caves→maze, city/roads/freeways, the Salvari camp; faction warfront.
- **Phase 3 — Core NEW systems** (§3): infection timer + timeline/karma/Humanity branching, damage-types, destructibles, grapple, weather + ToD, NPC mood/schedule sim + procedural NPCs, vehicles, SPH fluid.
- **Phase 4 — Undersea base + submarine combat + the maze/arena/space layers.**
- **Phase 5 — Acts 3–4** (space L36–75, Earth L76–100): Storm Runner hub, Memory Hunter, Proto-Overlords, the Overlord finale, the **12-ending** resolution + character arcs.
- **Cross-cutting:** AI-dialog (Claude/Grok), photo mode, cutscenes, music engine.

---

## 7. DESIGN DECISIONS — RESOLVED (Tim, 2026-05-23)
- **Level count — NO CAP.** Blueprint split is the baseline (Act1 ~8 / Act2 L8–35 / Act3 space L36–75 / Act4 Earth L76–100), but the game **may exceed 100 levels** — design expandable; the space act (Act 3) can grow. ✅
- **AI dialog — ALL THREE, selectable in-game.** Ship (1) scripted trees, (2) **live Claude + Grok conversations** (Tim: "hilarious/crazy"), and (3) hybrid as a player-selectable option. Scripted-only is the "boring" floor; live AI is the headline. Build the dialog system mode-agnostic. ✅ (see §3)
- **The Mirror — a literal numbered level** (not just a set-piece). ✅
- **Vertical / strata — LITERAL reachable depth.** Club 1127 is the bottom (−200); **massive alien tunnels** (slide-down, ~200 m) connect Detention↔Club; the deep **alien substrate (−400) is a real explorable zone** reached via the tunnels. ✅ (see §2.6)

---

*This blueprint is the single reconciled target. Update it as phases land. Authority chain in §0; gaps to add in §2.5; native gap in §5; build order in §6.*
