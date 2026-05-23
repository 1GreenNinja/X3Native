# Escape From Lab Zero — Reconciled Master Game Plan (X3Native)

> **Status: DRAFT for Tim's approval (2026-05-22).** This reconciles the two visions that existed in the design corpus, per Tim's call:
> - the **narrative bible** (50 levels / 4 acts; true Act-1 floor identities, Floor 4.5 Chorus, the F5 drone level, F7 sub-levels, the 12 endings) — see `docs/design/EFLZ_WORLD_STRUCTURE.md`, `EFLZ_BESTIARY.md`, `EFLZ_NARRATIVE.md`;
> - the engine's **`docs/MASTER_GAME_PLAN.md`** (100 levels / 3 acts "Spire" plan; Club 1127, the Mirror, mega-polish).
>
> **Tim's decisions:** "The Spire-plan things ADD. The 100 levels give time for **space exploration + life on other planets before rallying the army to save Earth.** Keep the Club — that's an **easter egg**." → So: the **bible is the narrative spine**, the **100-level scope is kept** (most of the added room goes to a real space act), and the **bible's true Act-1 floors + bosses replace the generic ones.**
>
> On approval, this supersedes `docs/MASTER_GAME_PLAN.md` as the canonical build target.

## North star
Jake Hunter — **Subject 7-Alpha**, augmented to ~400% strength but kept his free will — wakes in **Lab Zero**, a 7-floor breeding/infection facility revealed to sit on the alien world **Keth'zar**, run by the **Overlord** hive that conquers worlds and turns their populations into hybrid armies. Jake escapes the lab, allies with the last refugees of a genocided species (the **Salvari**), takes the war to space and other worlds, and returns to liberate an already-invaded Earth. **Choices matter; victory is never clean.** 100 levels, secrets that reward exploration, locked-60-FPS polish.

---

## The 100-level arc — 4 acts

### ACT 1 — LAB ZERO (the Facility / "The Spire") · ~L1–12 + hidden
The vertical 7-floor tower, with the **true floor identities + bosses** from the bible (the engine currently has the right *spine* — Martinez, the F2 rescue, the F7 Clone+Sarah — but generic mid-floors; this is the first rebuild target). Reached floor-to-floor by the central elevator; some content is **off-elevator** (Floor 4.5) or **hidden** (F7 sub-levels), found later.

| # | Floor / area | Identity | Boss | Signature beat |
|---|---|---|---|---|
| L1 | Floor 1 | **Detention / Awakening** | **Chief Martinez** | wake at the terminal (+400% readout ✓), crush restraints, find the pistol, "FIND SARAH" |
| L2 | Floor 2 | **Medical Bay** | **Dr. Chen (Corrupted)** | the **3-timer rescue** — Aria / Keisha / Emily (save only 2; the unsaved becomes a later boss) + Sarah's distant timer |
| L3 | Floor 3 | **Genetics Lab** | **Failed Experiment #7** (Marcus Webb) | tragic predecessor; "Memory Flash" gimmick; drops the F4 key |
| L4 | Floor 4 | **Cybernetics Workshop** | **The Collective** | Augmentation chairs + the **Humanity meter**; fleet/Overlord reveal; unlocks ChainGun |
| **L4.5** | **Nexus Chamber** (off-elevator) | **The Chorus** | the 5-merged-scientist plural mind — the **in-between floor the elevator doesn't stop at**, bridging F4→F5; save up to 4 of the voices |
| L5 | Floor 5 | **Drone Manufacturing** (the **drone level**) | **Swarm Controller AI** | **Sarah's 90-second master hack** strips the boss + flips the drone army to your side; unlocks Plasma Rifle |
| L6 | Floor 6 | **Alien Technology Lab** | **Alien Overseer** | **Salvari first contact**, **K'thara** joins, the cure; unlocks Lightning Gun |
| L7 | Floor 7 | **Executive Laboratory** | **Jake's Clone** | timed **Sarah rescue**; the **timeline (Alpha/Beta/Omega) LOCKS here** |
| **L7.5** | **Sub-levels** (hidden descent) | **Frozen Collective** (Cryo) | revealed only AFTER the Clone dies **and** Sarah is saved: hidden lift behind the exec desk → Waste Disposal / Cryo Storage / Interrogation → **Dr. Chen rescue / Return Mission** (fight F7→F1) |
| L8 | Escape | Roof + descent | — | fight to the roof; elevator/tunnel branch → the cliffs; **reveal: not Earth**; the Salvari ship glimpsed |

- **HIDDEN — Club 1127 (easter egg):** elevator/keypad code **1127** opens a secret neon club off the tower (fixer NPC, contraband upgrades, the PlasmaGlobe mechanic). Discoverable in Act 1, persists as a hub. *Easter egg per Tim — optional, not a main-path level.*
- **HIDDEN — Deep tunnels:** flooded service tunnels under B1 → power core, a hidden boss, lore, a shortcut into the Act-2 underground biome.

### ACT 2 — KETH'ZAR, THE ALIEN WORLD (open world) · ~L9–35
Surface emergence on the alien world; the Salvari alliance; faction warfront; choices cascade from the Act-1 timeline. Leans on the engine's terrain + streaming + water + sky.
- **Crystal valleys** — first surface biome; Overlord patrols; the Salvari ship crash → cement the **K'thara** alliance.
- **Faction warfront** — Overlord forces vs the refugees vs you; rescue Salvari pockets; build the alliance (companions, a base, upgrades). Biomes: **undersea base** (sea creatures), **underground maze** (multi-biome), surface cities/freeways.
- **Optional — pilotable drone content** (the `Grok/CrazyDrone` "sanity→rage" prototype) as a sandbox/minigame here, not a numbered main level.
- **The Salvari Archives / Crystal Heart** — the breeding-program truth; the Beta-path bosses appear if the F2 women weren't saved (Siren/Aria, Breeder Queen/Keisha, Oracle/Emily).
- **"The Mirror"** (homage set-piece) — the corridor of mirrors; a no-combat "break the binary" room (the original web game's ending, kept as a lateral-thinking set-piece). *See open decision #2 on its placement/numbering.*
- **Act-2 climax** — steal the ship (the **Storm Runner**) and leave the planet.

### ACT 3 — BEYOND THE STARS (space + other worlds) · ~L36–75 — THE EXPANSION
**This is where the 100-level scope buys what Tim wants: real space exploration + life on other planets before Earth.** The space journey home becomes a campaign of its own — multiple star systems, planets, and stations; recruit a multi-species fleet; meet alien civilizations (allies and enemies) before turning toward home.
- Star-system to star-system travel; **distinct alien worlds/biomes + civilizations** (the "life on other planets" content); diplomacy + combat.
- Recruit a **multi-species fleet/alliance**; **Salvari Prime**; **Void Pirates** (mini-boss Admiral/Captain Vex); espionage/casino set-piece; the **romance/marriage** beat with K'thara.
- The **Memory Hunter** psychological-warfare boss (bible L30) lives here.
- Discover **Earth is already invaded** → rally the army and turn for home (the act's turn).

### ACT 4 — EARTH LIBERATION · ~L76–100
Arrive at the invaded Earth; rally and liberate; the finale branches into the 12 endings.
- **Orbital assault → regional → city liberation**; enemies: Overlord soldiers, **Human Collaborators**, **Converted Military**.
- The **5 Proto-Overlords** (General / Scientist / Priest / Enforcer / Voice) across the late levels.
- The **Overlord Mothership**.
- **L100 — The Reckoning:** the 4-phase Overlord finale (incl. a 5-neural-node phase + a non-combat ending-choice phase; scales with allies/captives saved across all 4 acts) → **the 12 endings**, keyed to the Act-1 timeline lock + relationship/Humanity/Mercy/Redemption axes (Omega/Alpha/Beta/Gamma branches). See `EFLZ_NARRATIVE.md` for the ending table.

---

## The 12 endings (summary)
Keyed to: the Act-1 **timeline** (who you saved on F2 + Sarah on F7), and the running **Humanity / Trust / Mercy / Love / Redemption / Augmentation** axes. Golden, Good, Bittersweet, Tragic, Fractured, Dark, Nightmare, Solo Victory, K'thara Romance, Polyamorous Family, Chen's Redemption, New Beginning. (Full triggers: `docs/design/EFLZ_NARRATIVE.md`.)

## Secrets & hubs
- **Club 1127** (code 1127) — easter-egg neon hub (vendor/fixer, side-quests, PlasmaGlobe). Optional.
- **Deep tunnels** — flooded sublevels, hidden bosses, lore, biome shortcuts.
- **Mirror rooms** — recurring "break the binary" lateral-thinking set-pieces.

## Polish targets (gameplay-side; engine perf is its own lane)
Locked 60+ FPS / zero-stutter; movement feel (coyote time, jump buffer, air control, **foot-IK ✓**); inertialized transitions ✓; point-to-crosshair weapons ✓; recoil/shake/hit-feedback; active-ragdoll reactions; readable objectives/kill-feed/damage-flash; HDR/bloom/SSAO/SSGI ✓; **GPU skinning ✓ (crowds now affordable)**; no jank (see-through walls, z-fight, fall-through, facing-swivel — `CONVENTIONS.md`).

---

## OPEN RECONCILIATION DECISIONS (need Tim's confirmation before the Act-1 rebuild)
1. **Faction/enemy canon names.** The engine roster (`monster.h`) is 4 generic stand-ins — **DominionTrooper / Verthani / Illuminated / BlueSynth** — exercising 4 AI shapes. The bible canon is the **Overlord** empire + lab security/scientists + the **Infected** line + **drones** + **Salvari** (+ Void Pirates, Collaborators, fauna). Proposal: adopt the bible canon and **map the 4 stand-ins** onto real species (e.g. keep them as the in-engine AI archetypes but rename to canon: lab Security Guard / Infected Stage-1 / Elite Guard / Combat Drone), then add the missing species + the 5 mid-bosses as new data rows. **Confirm the canon faction list + whether to rename or keep "Dominion/Verthani/Illuminated" as the Overlord's subject races.**
2. **"The Mirror" placement.** Old plan made it the Act-2 climax (web-game ending homage); the bible's L48 is the Overlord Mothership. Proposal: keep the Mirror as an Act-2 **set-piece room** (homage), put the Mothership at **L100**. Confirm, or keep a literal "Level 48 Mirror."
3. **Per-act level counts (to total 100).** Proposed split — Act 1 ~12 (+hidden), Act 2 ~27 (L9–35), Act 3 ~40 (L36–75, the space expansion), Act 4 ~25 (L76–100). Confirm or reweight (esp. how big the space act should be).
4. **Martinez floor.** Engine + merged narrative = **Floor 1** (used here); one source floats him to F2. Confirm F1.

## Build order (engine/13700K lane, off current `main`)
**Phase A — bring Act 1 to canon (highest value; partially built):**
1. Fix floor **identities** F1–F7 (Detention / Medical / Genetics / Cybernetics / DroneStation / AlienTech / Executive) — rename + re-theme the existing plates.
2. Add the **5 missing mid-bosses** as data rows (Dr. Chen, Failed Experiment #7, The Collective, Swarm Controller AI, Alien Overseer) on the existing boss machine; extend it for multi-body/phase bosses (Chorus 5-pods, Swarm pre-fight hack).
3. **Floor 4.5 — the Chorus / Nexus Chamber** (off-elevator in-between).
4. **Floor 5 — the drone level**: Sarah's 90s master hack + the drone-army flip.
5. **Floor 7 sub-levels** (hidden descent) + the **Dr. Chen Return Mission**.
6. F2 rescue depth (3 timed victims → companions/bosses), F4 **Humanity meter**, F7 **timeline lock**.
7. Weapon ladder (ChainGun / Plasma Rifle / Lightning Gun).

**Phase B — Act 2** (surface biomes on the terrain/water/streaming systems → alliance/faction content → Mirror → leave the planet).
**Phase C — Act 3** (the space expansion: systems/worlds/fleet).
**Phase D — Act 4** (Earth liberation → L100 → 12 endings).

*Asset sources: `rigged_glb` (bosses/victims/weapons), the media catalog, the web EL48 level JSONs (`G:\GameDev\Web Escape Lab 48 versions\…\Level_*.json`). The full per-floor/level detail lives in `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` (digested in `docs/design/`).*
