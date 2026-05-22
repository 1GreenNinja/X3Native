# Spec: EFLZ Level — "The Spire" (7-floor glass office tower)

> Written by the SPEC TEAM (5090/14900K). Implemented by the CLEAN-ROOM/build team (13700K farm). Game/slice code only — `engine/` stays pure.
> Supersedes the 6-room graybox `EFLZ_LEVEL_01.spec.md` as the Level-1 target. Tim's vision, 2026-05-21.

## 1. Concept
Escape From Lab Zero is a secret lab disguised as a **corporate office tower** — all glass curtain walls and clean sci-fi interiors. Jake is the test subject. He **wakes in the basement security wing**, discovers a **400% strength augmentation** at a security terminal, crushes his restraints/equipment, and fights **upward** through 7 floors to escape from the roof. Vertical level, ascending tension, glass-and-neon aesthetic.

## 2. Vertical structure (deep tunnels → tower → cliffs)
| Floor | Wing | Role | Key beats |
|---|---|---|---|
| **B1 — Basement Security** | holding/security | **START** | Jake wakes restrained; **strength terminal (400%)**; crush restraints; first guard; **Boss: Chief Martinez** (security chief) gates the elevator |
| **F1 — Lobby/Atrium** | glass atrium | breather + reveal | huge glass curtain-wall lobby (the **Showroom kit** fits here ⭐); see the snowy exterior; first keycode door |
| **F2 — Medical Wards** | wards A/B/C | **rescue hub** | 3 rescue victims on 5-min timers: **Aria (Ward A), Keisha (Ward B), Emily (Ward C)** |
| **F3 — Labs** | research | hazards + crafting | infected enemies, augmentation chairs, crafting station; keycode from a terminal |
| **F4 — Offices** | cubicles/glass offices | combat sprawl | occupation troopers; cover; door-override puzzle |
| **F5 — R&D / Synth bay** | synth production | synth waves | `blue_synth`/`the_collective` enemies; a transformed-victim mini-boss if a timer expired |
| **F6 — Executive** | exec suites | escalation | heavy guards; **Sarah** (4th rescue victim) held in an exec office |
| **F7 — Rooftop** | helipad/exfil | **finale** | final boss + escape; glass roof, sky |

Connected by a **central elevator** (keycode/clearance-gated per floor) + emergency **stairwells** — AND a full vertical world beyond the tower:

### 2b. Deep underground tunnels (below B1)
Below the security wing, service tunnels descend into the lab's hidden infrastructure — power core, waste/**flooded** sections, and a connection to the wider X3 underground (the bible's 5-biome maze). A branching descent: a secret/escape route + optional content (loot, lore, a hidden boss). Flooded stretches tie to the undersea biome (sea-creature ambushes — `GreatWhiteSharkGameReady`, `sea_*`). Reached via a breached floor or a maintenance shaft in B1.

### 2c. In-between layers (mezzanines / service voids)
Between each floor: maintenance catwalks, ventilation shafts, atrium balconies, service voids. This is the **strength-and-traversal layer** — Jake's 400% strength lets him pry vent grates, climb shafts, and **punch through weak floors** to flank or skip a floor's front-door fight. Stealth/alternate routes + verticality between the main floors.

### 2d. Above-ground exterior — cliffs + the Salvari landing (finale)
The tower stands in a **snowy mountain landscape** (matches the Showroom kit's exterior). Past the F7 rooftop the level opens onto **above-ground cliffs**. The **Salvari ship lands** on a cliff-side pad — the climax: an exterior arena, the ship as a set-piece, Salvari forces (`SalvariPrincess.glb` + troopers) and the final confrontation / exfil. Sky, snow, cliffs, the alien ship — the payoff after the ascent.

**This realizes the X3 "vertical layers" concept** (bible: layers linked by elevators/teleporters) in one continuous level: **deep tunnels → basement → 7 tower floors (+ in-between layers) → rooftop → cliffs → Salvari ship.**

## 3. Opening beat — basement strength terminal (the "Awakening")
- Jake starts at `spawn` in B1, restrained near a **security terminal**. Interact (E) → terminal screen reads:
  ```
  SUBJECT: JAKE  ·  STATUS: AUGMENTED
  MUSCULOSKELETAL OUTPUT: +400%
  RESTRAINT INTEGRITY: FAILING
  ```
- Triggers the existing **strength-discovery** beat ("equipment crushed") → Jake gains super-strength melee (V) immediately. (Already partly in: Level1 fires "STRENGTH DISCOVERED — equipment crushed".)
- HUD objective: "Escape the basement security wing."

## 4. Door system (two needs)
**4a. Real door MESH (replace the procedural box).** Use `converted_glb/ModularSciFi_Interior/SM_Door_A.glb` (or `SM_Door_B`) for the door slab + `SM_DoorFrame_A` for the frame — fixes the flat-red box AND the see-through gap (frame covers the wall opening). Door still slides up (portcullis) or swap to a slide-side animation matching the mesh. Glass doors for the office floors.

**4b. Keycode doors (the door-code keypad).** Locked doors show a keypad; player presses **E** near a locked coded door → enters **code-entry mode** (digits 0-9, Enter submits, Backspace deletes, Esc cancels) → correct code unlocks + opens. Codes found on terminals/notes/dead guards. **Lore code: 1127** (Club 1127 reference). Per-door codes stored on `Door`/`DoorSpec` (`int code`). HUD prompt: "DOOR LOCKED — ENTER CODE: ▮▮▮▮". (Bounded feature — see "Build order" #1.)

## 5. Rescue system (lab rescue content)
- 4 victims: **Aria (F2-WardA), Keisha (F2-WardB), Emily (F2-WardC), Sarah (F6/F7)**. Each has a **5-minute timer** shown on the HUD (up to 3 active at once on F2).
- **Rescue (interact while alive)** → victim becomes a **companion** (follows, light combat assist) — uses the existing `MonsterManager`/companion pattern, friendly faction.
- **Timer expires** → victim **transforms into a boss**: Aria→**The Siren**, Keisha→**Breeder Queen**, Emily→**Oracle** (assets already exist: `BossTheSiren.glb`, `BossBreederQueen.glb`, `Oracle.glb`).
- New system `app/rescue.*` (mirrors `monster.*`): victim entities + timers + HUD + transform-on-expire; serialize via the existing `G.serializeRescueState` hook pattern (already referenced in the save system).

## 6. Boss content
- **Martinez** (B1/elevator gate) — already implemented (`m_martinez`, multi-phase `BossPhase`, `spawnMartinez`). Reposition to the basement security boss role.
- **Transformed victims** (Siren/Breeder Queen/Oracle) — mid-bosses spawned by rescue-timer expiry (§5), reuse the boss/monster framework + `bosses.json` phase data.
- **Final boss (F7 rooftop)** — `BossTheOverlord.glb` / `OverLordEnforcer99.glb`. Multi-phase per `bosses.json`.

## 7. Art / kits to use (from `MEDIA_CATALOG.md`)
- Interiors: `ModularSciFi_Interior` (✓ converted) for security/labs/offices; **glass curtain walls** + atrium from the **3D Showroom Vol 30** kit (F1) and `SciFi Warehouse Kit` (high bays).
- Doors: `SM_Door_A/B` + `SM_DoorFrame_A/B`.
- Characters: `chief_martinez`, `marcus_webb`, occupation troopers, synths, the boss roster, the rescue victims (Aria/Sarah/etc. — map to `Anna*`/`Sarah.glb`/`Oracle`).
- Props: `AugmentationChair`, `CraftingStation`, terminals (need a terminal prop — gen via SD or find a kit).

## 8. Engine work required (build order — bounded → big)
1. **Door-code keypad** (bounded; level-independent). `int code` on `Door`/`DoorSpec`; `Level1Game::setDoorCode/tryDoorCode/nearLockedCodedDoor`; code-entry state machine + HUD prompt in `main.cpp`. **Can ship today on a branch.**
2. **Door-mesh swap** (bounded). Load `SM_Door_A.glb` in `buildLevelDoor` instead of the procedural box (keep the static collision box; draw the GLB at the slab transform). Fixes red box + see-through.
3. **Strength terminal** (bounded). A terminal interactable in B1 that shows the 400% readout and fires the strength beat. (Beat already exists; add the terminal prop + UI.)
4. **Rescue system** `app/rescue.*` (medium). §5.
5. **7-floor geometry** (big). Extend `level1.*` from 6 rooms → a B1+7-floor vertical layout with elevator/stairwell transitions + floor streaming. The largest piece; farm-led.
6. **Boss expansion** (medium). Wire transformed-victim bosses + the F7 final boss into the existing boss framework.

## 9. Acceptance
- Jake spawns in B1, reads the 400% terminal, crushes restraints, melee enabled.
- A keycode door blocks progress; entering the right code opens it.
- Doors render as real meshes (no red box, no see-through).
- On F2, 3 rescue timers run; rescuing one yields a companion; letting one expire spawns its boss.
- The elevator ascends floors; each floor reads as a distinct wing; F7 rooftop finale.
- Runs at target FPS on the 5090; capture via `--screenshot`.

## 10. Notes
- This is the EFLZ Level-1 target; the current 6-room graybox is the seed. Build incrementally — features #1-#4 land on the current level and carry into the 7-floor build.
- Keep `engine/` pure; all of the above is game-layer (`app/`).
