# Escape From Lab Zero — Master Game Plan (X3Native)

> The whole-game design: **100 levels**, 3 acts, hidden Club 1127 + tunnels, mega polish, buttery-smooth. Synthesized from the canon (the novel chapter "The Game That Remembered", `G:\Books\Sovereign Rising\Definitive\CHAPTER_ESCAPE_LAB_48_CORRECTED.md`), the 940-page bible content, the shipped web Escape Lab 48 (v9.921), and `EFLZ_SPIRE_7FLOOR.spec.md` / `ELEVATOR.spec.md`. Tim, 2026-05-22.

## North star
A native (C++/Vulkan) Escape From Lab Zero: Jake — **Subject 7-Alpha**, 400%-augmented — escapes a breeding-program facility, discovers he's on an alien world, allies with the last refugees of a genocided species, and fights an empire he probably can't beat but refuses to stop fighting. **Choices matter; victory is never clean.** Mega-polished, 100 levels, secrets that reward exploration.

## Canon characters & factions (build to these)
- **Jake** (player, Subject 7-Alpha, +400% strength) · **Sarah** (rescued F7, becomes co-fighter) · **K'thara** (Salvari commander, key ally) · **Dr. Chen** (corrupted scientist) · **The Clone** (Jake's duplicate, F7 boss).
- **Dominion** (multi-armed grey — built Lab Zero, breeding program) · **Verthani** (insectoid warriors) · **The Illuminated** (energy-being elite) · **Salvari** (bioluminescent refugees, allies — 30 from 30 billion).

---

## The 100-level arc (3 acts)

### ACT 1 — THE FACILITY / "The Spire" (Levels 1–14)
The vertical tower (per `EFLZ_SPIRE_7FLOOR.spec.md`), expanded with in-between layers + tunnels into ~14 levels.
- **L1 B1 Detention** — wake at the security terminal (**+400% readout** ✓ done), crush restraints, first guard, **find the pistol in a locker**, escape the cell. Objective: "FIND SARAH."
- **L2–3 Wards** — **the 3-timer rescue: Aria / Keisha / Emily** (save only 2; the unsaved transforms into a boss — Siren / Breeder Queen / Oracle). The game's signature triage beat.
- **L4–5 Labs** — infected enemies, augmentation chairs, crafting; keycode doors (keypad ✓ done).
- **L6 Offices / Synth bay** — occupation troopers, blue-synths, the door-override puzzle.
- **L7 Executive Laboratory** — **boss: The Clone** (3 phases: separate from Sarah → destroy the neural collar minigame → mutated hybrid). Sarah wakes, arms up, fights beside Jake.
- **L8 Escape** — fight to the roof; the elevator (✓ 7-story core) + a **descent branch into the deep tunnels** (L8.5 secret) → exit onto the **cliffs**; the **Salvari ship** is glimpsed. Reveal: **not Earth.**
- **HIDDEN — Club 1127** — elevator/keypad code **1127** opens a secret club level off the tower (the lore club; neon, music, a fixer NPC, contraband upgrades). Reachable from Act 1, persists as a hub.
- **HIDDEN — Deep tunnels** — flooded service tunnels under B1 → power core, a hidden boss, lore, and a shortcut to the underground biome (ties Act 1 → Act 2).

### ACT 2 — THE ALIEN WORLD (Levels 15–48)
Open(er) levels; factions; choices cascade. Cribs the shipped EL48 content + the canon.
- **L15–20 Crystal Valleys** — first surface biome; Dominion patrols; **L18-ish: the Salvari ship crash → meet K'thara** (alliance begins).
- **L21–32** — faction warfront: Dominion vs Verthani vs the player; rescue Salvari pockets; build the alliance (companions, base, upgrades). Biomes: undersea base (the sea creatures), underground maze (5 biomes), surface cities/freeways.
- **L33–46 The Illuminated** — escalate to the elite; heavier bosses (the 18-boss roster); the breeding-program truth.
- **L47 The Alliance** — Salvari fight beside Jake en masse; the big set-piece battle.
- **L48 "The Mirror"** — no combat: the corridor of mirrors; reject the false binary (sacrifice self / sacrifice friends) by **breaking the central mirror (E)** → the hidden third path → K'thara: "come back and teach the rest of us." (Original game's ending — now Act 2's climax.)

### ACT 3 — THE RESISTANCE / BEYOND (Levels 49–100)
The expansion past the original 48 — "refusing to stop." Take the fight back.
- **L49–64** — through the third-path door: strike back at the Dominion; liberate worlds; the Salvari rebuild.
- **L65–82** — the Verthani hives + the Illuminated's seat of power; mega-bosses; the war turns.
- **L83–99** — the Overlord empire's core; the hardest content; allies fall; choices have permanent weight.
- **L100 — The Reckoning** — the finale: confront the Overlords; the "third option" writ large; multiple endings keyed to choices across all 100 levels. *"Never stop fighting."*

---

## Secrets & hubs (the "hidden club and tunnels")
- **Club 1127** (code 1127) — persistent neon hub: vendor/fixer, music, side-quests, the PlasmaGlobe mechanic (`C:\GameDev\PlasmaGlobe.md`). Discoverable in Act 1, returns throughout.
- **Deep tunnels** — flooded sublevels, hidden bosses, lore caches, biome shortcuts. The "Children of Static" caves echo here.
- **The Mirror rooms** — each act has one secret "break the binary" room rewarding lateral thinking.

## Mega polish + "smooth smooth smooth"
Targets (gameplay-side; engine perf is the farm's lane):
- **Locked 60+ FPS, zero-stutter** (idTech-8 pillar #1). Honor the farm's render budget; stream/LOD content; no per-frame allocs in gameplay.
- **Movement feel** — `ExtendedUpdate` controller, coyote time + jump buffer, air control, footstep IK (per `J-character-animation.spec`).
- **Inertialized animation transitions** (no pops), **point-to-crosshair** weapons (✓), recoil/screen-shake/hit-feedback, **active-ragdoll** hit reactions.
- **Doors/elevator** — real meshes (door-mesh swap next), smooth accel/decel, per-floor doors, audio.
- **Game feel** — readable objectives, kill feed, damage flash, satisfying SFX (M9 audio), HDR/bloom/SSAO lighting (✓ farm).
- **No jank**: fix see-through walls (side ✓; end-caps next), no z-fighting (door slimmed ✓), no fall-through, no facing-swivel (`CONVENTIONS.md`).

## Build order (14900K gameplay/content lane, off current `main`)
1. **End-cap wall fix** (the remaining see-through) — per-side facing on the cross-walls too.
2. **Door-mesh swap** — `SM_Door_A.glb` + frame (kills flat-color + ceiling-poke).
3. **Spire floors** — build L1–L8 geometry per Act 1 above; wire the elevator's 7 stops to real floors; the F2 rescue hub; F7 clone boss.
4. **Club 1127** + **deep tunnels** secrets.
5. **Act 2 biomes** (lean on the farm's terrain + water) → the faction/alliance content → L48 Mirror.
6. **Act 3** expansion to L100.
*(Content uses the media catalog: `C:\GameDev\MEDIA_CATALOG.md`. Bosses/victims/weapons assets already exist in `rigged_glb`.)*

## Sources to mine further
- `G:\Books\Sovereign Rising\Definitive\CHAPTER_*` (canon story + characters).
- `G:\GameDev\Web Escape Lab 48 versions\v9.921_ultimate\` (shipped levels/weapons/club — read FEATURE_COMPARISON + README).
- `G:\EscapeFromLabZero\Characters\Generated\INDEX.md` (character roster).
- The 940-page bible (quests/dialog/endings) — locate the master file on G:.
