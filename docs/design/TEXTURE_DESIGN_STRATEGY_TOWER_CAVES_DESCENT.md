# Textures & Design Strategy — The Office Building, The Caves Beneath, and the Descent Tunnels

**Source of structure:** LevelArchitect 10.9 (`G:\GameDev\LevelArchitectFullV10.9\js\Config.js` — floor names, the 29-room room-type vocabulary with per-type colors and lighting presets, the Floor-1 default plan with its cave annex) cross-read against the engine as built (`level1.h kWingRooms`, `wing_dressing`, `room_dressing` + `surface_library`, `spire_sublevels.h` SL1–SL3 + the Salvari cave horizon at −178 m) and the 2026-07-10 studio-bench audits (level-designer + art-director full-game passes).

---

## 1. THE OFFICE BUILDING (the glass facility, Floors 1–7)

### 1.1 The structure 10.9 gives us
Floor names are canon in the file: **1 Detention · 2 Medical Bay · 3 Genetics Lab · 4 Cybernetics Wing · 5 Drone Station · 6 Salvari Level · 7 Executive Suite.** Floor 1's default plan is a complete 29-room prison block: Jake's cell + 13 story-flavored cells (Abandoned / Failed Experiment / Skeleton / Infected / Flooded / Blood Trail / Sarah's-Empty…), two cell-block hallways + connectors, guard station, storage, medical, armory, elevator lobby, old armory (flicker-lit), creepy passage — and then the **Descending Stairs (`stairDown`, −5 m) into the cave annex** (§2). The doors table wires every cell onto its hallway spine — the layout is a comb: two parallel cell rows feeding a central circulation spine, services on the east edge, the descent in the far east corner *past* the creepy passage. That order is a pacing instruction: the player exhausts the human spaces before the earth swallows them.

### 1.2 Texture strategy per floor (zone stories, one accent hue each)
The rule that made F4/F5 land in the audit: **steel/navy base (60%), zone secondary (30%), ONE accent hue (10%) — accent carried by light + emissive, not by wall paint.** Forge sets exist for most of these (assets/surface_library, SD3.5 pipeline); the gaps are marked FORGE.

| Floor | Base surfaces | Accent (light-borne) | Notes / gaps |
|---|---|---|---|
| 1 Detention | `mw_concrete_panels_a/b` + steel trim; cell interiors rougher than halls | **Hazard AMBER** (bible) — currently cyan-miscalibrated (audit fix #3) | Cells get per-cell decals of their story (flood stain, blood trail, scorch) — FORGE 4 grunge overlay decals |
| 2 Medical | `hh_wall_01a/floor/ceiling` clinical whites | Cool green-white | Beds missing (`SM_Hospital_Bed.glb` unconverted); curtain-track + privacy glass FORGE |
| 3 Genetics | `mw_plaster_painted` (desaturate per audit) + vat glass | Vat GREEN, lit from inside the tanks | Pull vats off walls (LD note); `Clone Lab`/`Cure Lab` types in 10.9 want a cleaner white-green variant set — FORGE 1 |
| 4 Cybernetics | `sr_metal_b` racks (the audit star) + `mw_metal_grate` floor | Cold CYAN | Cool the grate's rust (audit); center-floor dressing + destination key light (LD) |
| 5 Drone Station | `mw_thermal_padding` walls + `sr_floorstripes` deck | Industrial AMBER | Enlarge floorTile so stripes read as lanes not corduroy (top remaining art fault); dress back wall w/ hangar-door silhouettes |
| 6 Salvari Level | `sr_concrete_01/_a` dark + biolume organic | BIOLUME teal — the one floor where the accent may own more than 10% (alien break-of-rules is the point) | Raise fill 1–2 stops (LD); portal spill should bounce |
| 7 Executive | `cc_porous_cement` dark steel + `cc_exec_floor` marble | BRASS/warm | The only pristine zone — zero grime law; walnut furniture (now lit post-45e1c46) |

**Why this ladder works:** ascending the tower is a color journey — amber(fear) → white(sterile) → green(life-twisted) → cyan(machine) → amber(industry) → teal(alien) → brass(power). Each elevator arrival is a one-second read of "where am I," which is exactly the failure the LD flagged in the spire captures (five identical floors). The 10.9 per-type lighting presets agree: they authored warm 1,.85,.6 halls vs cyan .2,.7,.9 cave chambers vs red-flicker creepy passages — the ladder was in the file all along.

### 1.3 Design strategies (from the LD audit, mapped to this structure)
- **One landmark per floor** at the arrival sightline (the elevator door frames it): F2 surgical theater glass, F3 the vat gallery, F4 the server canyon, F5 the hero drone pad, F6 the portal, F7 the boardroom window-wall.
- **Light as wayfinding:** one key statement per room (10.9 encodes this: `spacing:0` = single statement rooms vs `spacing:6-8` = rhythm rooms i.e. hallways/caves). Flicker is reserved in the file for exactly three types — Old Armory, Creepy Passage, Boss Arena — keep that scarcity; flicker everywhere is flicker nowhere.
- **Doors-law:** panel segmentation already cuts around openings (wing dressing) — extend the teal floor guide strips (Floor-1 wayfinding) up the tower, tinted per zone accent.
- **The comb pacing on F1:** cells alternate empty/story/monster in the file — dress intensity should follow (quiet cell = clean set, monster cell = destruction decals + broken light).

---

## 2. THE CAVES UNDERNEATH

### 2.1 Structure (two cave systems, deliberately distinct)
10.9 authors the **shallow annex** off Floor 1: Descending Stairs (−2.5) → **Cave Tunnel** (10×3.5×3, warm `1,.8,.5` lantern rhythm every 5 m) → **Crystal Cavern** (18×8×16 — the biggest room in the plan, teal `.3,.8,.7`, sparse 8 m spacing) → **Side Grotto** (8×6×8, cyan `.2,.7,.9`, single statement). The engine authors the **deep system**: SL1 Waste Disposal → SL2 Cryogenic Storage (Frozen Collective mini-boss) → SL3 Enhanced Interrogation (Dr. Chen) → the **Salvari cave floor at canon −178 m with 7 singing/lore crystals**.

### 2.2 Texture strategy
- **Transition grammar — concrete gives way to rock in three steps** (the single most important texture move down here): (a) stairwell = poured concrete w/ formwork lines (`sr_concrete_01`), (b) tunnel = shotcrete over rock — concrete with rock breaking through (FORGE: `cv_shotcrete_break`), (c) cavern = raw rock (FORGE: `cv_rock_wet` w/ triplanar to kill tiling on organic surfaces — the terrain audit's macro-variation lesson applies doubly on cave walls).
- **Crystal materials:** the singing crystals are the caves' hero. Emissive-map-gated glow (the club1127/portal recipe — dark albedo, texture-gated emissiveTex ~1.1) in teal with per-crystal hue drift; NEVER flat emissive (ACES clip law). Their light IS the cavern's lighting — 10.9's cave preset (teal, range 10, spacing 8) reads as "crystal every 8 meters," so let the crystals be the fixtures.
- **Wet floor law:** cave floors get the low-roughness treatment (G~90) so crystal light streaks — the cheapest "alive cave" move the engine's SSR/RT reflections can cash on the 3090 Ti.
- **Depth palette:** shallow annex = warm-lantern-meets-teal (human lights invading); deep system = teal → biolume as human influence dies out; SL floors keep facility surfaces but DEGRADED (FORGE: stained/frosted/scorched variants of `sr_metal`/`mw_concrete` for waste/cryo/interrogation).

### 2.3 Design strategies
- **Compression → release:** 10.9 sizes tell the story — 3 m tunnel into an 8 m-tall cavern. Preserve and exaggerate: pinch the tunnel exit to 2.5 m so the cavern reveal lands.
- **Sound + light as breadcrumbs:** singing crystals are audible before visible; each audible crystal is a beacon toward the next chamber (wayfinding without signage in a signage-free biome).
- **The grotto is the secret,** not the path: side placement in the file (x:55, off-axis) — dress it as reward space (lore crystal cluster + supply cache), single cyan statement.

---

## 3. THE DESCENT TUNNELS (the winding slide to the bottom)

### 3.1 Structure to build (new content; anchors already exist)
**Owner's canon geography (2026-07-11):** the slide starts in the BUILDING'S BASEMENT (B1), winds down a curving tunnel system, and ejects into a lower cave area — which itself still sits ABOVE Club 1127. The vertical stack, top to bottom: tower floors → basement → curving slide tunnels → lower caves → (deeper still) Club 1127. The hidden exec lift (trigger 80, SL1→SL3 to −178 m) remains the *powered* route; the slide is the physical/fast/loud one, intersecting the sub-levels as glimpse-windows on the way down (SL1 steam, SL2 frost-blue flash, SL3 red interrogation glow) so the fall previews the spaces the player will fight back UP through. That up-through-what-you-slid-past loop is the level-design thesis of the whole lower game. **And because the club is below the caves: a faint, muffled 128 BPM bass bleeds up through the lower-cave floor — the deepest "civilization" tell in the game, discovered before the club is ever seen.**
- **Form:** half-pipe corkscrew, 3 m bore (10.9's tunnel gauge), banked turns, 2–3 near-vertical drops with catch-bowls; segments of 15–25 m between direction changes so speed reads as rhythm.
- **Mechanics:** extend `desc_mechanics`/player with a slide state (surface-tangent velocity, capsule locked to spline, steer ±1.5 m across the pipe); exit into a water/gravel braking bowl in the Crystal Cavern's deep twin.

### 3.2 Texture strategy for speed
Slides are read at 15–25 m/s — texture rules change:
- **Longitudinal, not tiled:** flow-direction brushed metal (upper chute: FORGE `sl_chute_steel`, worn mirror-smooth center stripe from decades of use — the polish stripe IS the path telegraphing) transitioning to water-carved rock flume (lower: `cv_rock_flume`, same triplanar law) — the same concrete→rock grammar as the stairs, compressed into seconds.
- **Light ribbons over fixtures:** discrete lights strobe unpleasantly at speed; use continuous emissive strips (texture-gated) along the pipe shoulders — facility cyan up top, fading to crystal teal at the bottom; the ribbon color literally slides through the zone ladder in reverse.
- **Speed particles at apex turns** (dust/spark burst on bank contact) — the juice layer the CombatFx tracer system can already serve.
- **Landmark flashes:** each sub-level window is one saturated color frame (steam-white / cryo-blue / red) — at speed, color is the only readable channel; shape detail is wasted.

### 3.3 Pacing
The descent is the Act-1 midpoint exhale: tension (found the hidden lift shaft) → thrill (30–40 s ride, three glimpse-windows) → awe (ejected into crystal dark, seven songs in the black) → dread (you now climb back through everything you glimpsed). Design each chute segment to end on a glimpse-window or a drop — never both — so the ride alternates information and adrenaline.

---

## 3.4 WORLD DEPTH — owner ruling 2026-07-11: the bottom is **−700 m** (was −178 m)
The −178 m Salvari-cave horizon becomes a mid-depth stratum, not the floor. PROPOSED depth ladder (assignments below −178 are proposals for Tim's ruling, not canon):
| Depth | Stratum | Note |
|---|---|---|
| 0 → −15 | Basement B1 + slide entry | facility concrete |
| −15 → −170 | The curving slide tunnels | glimpse-windows into SL1/SL2/SL3 |
| −178 | Lower caves (crystal horizon) | 7 singing crystals; **muffled 128 BPM bass through the floor** |
| ~−250 to −350 | Club 1127 "THE DEEP" | below the caves per owner geography; the elevator's strata views already sell this journey |
| −350 → −650 | PROPOSAL: deep strata — old mines / Salvari ruins / thermal vents | the elevator strata system + world blueprint hooks |
| **−700** | PROPOSAL: the true bottom — the Salvari city / the thing the facility was built on top of | endgame reveal space |

## 4. Forge queue (SD3.5, local — new sets this report requires)
`cv_shotcrete_break`, `cv_rock_wet` (+flume variant), `sl_chute_steel` (w/ polish stripe), degraded `sr_metal_waste` / `sr_metal_cryo` / `mw_concrete_scorched`, medical `hh_privacy_glass`, genetics clean-lab variant, 4 cell-story decal overlays. ~11 sets ≈ 7 min GPU on the 3090 Ti + normals/mr derivation. Convert `SM_Hospital_Bed.glb` (asset-path fix, F2 beds currently skip).

*Filed by Snake (session lead) with the studio bench's findings folded in. Structure citations: Config.js FLOOR_NAMES/TC/FLOOR1_DEFAULT/FLOOR1_DOORS + per-type lighting presets; spire_sublevels.h SpireSubTrigger/SpireSubPlan/kCaveBaseY.*
