# X3Native — Playtest Guide (2026-05-24, main @ b39cb28)

## Launch
Build (if needed): `cmake --build --preset windows-vs2026` → `.\build\bin\Release\X3Engine.exe`.
- **Play Act 1 (the Spire):** `.\build\bin\Release\X3Engine.exe`  *(no args = the Level-1 interactive game; add `--width 2560 --height 1440` for hi-res)*
- **Demos:** `.\build\bin\Release\X3Engine.exe --world <mode>` — modes: `club` `elevator` `ragdoll` `physjoint` `terrain` `ocean` `valley` `cliffs` `destruct` `drive` `boat` `fly`

## Controls (Level 1)
- **Move** WASD · **Look** mouse · **Sprint** Left-Shift
- **Fire** Left-Mouse · **Weapon switch** number keys `1`..`N` · **Reload** `R`
- **Interact** `E` (buttons, doors, weapon pickups, rescue captives)
- **Keypad** walk up to a locked coded door → it enters code mode → type the digits, Enter. (`Esc` cancels.) Codes: lab/floor doors per floor; **`1127`** = the Club 1127 easter egg.
- **Console** `` ` `` (backtick) — cvars/commands · **Save/Load** pause menu (+ `F5` quick-save / `F9` quick-load) · **Esc** pause / cancel
- Demo cams (`--world …`): WASD + Shift, `Space`/`Ctrl` up-down on fly cams, `E`/`F` interact, `Esc` exits. Vehicles (`drive/boat/fly`): W/S throttle, A/D steer, arrows pitch, Q/E roll.

## Act 1 flow to walk (the canon Spire, B1→F7 + secrets)
1. **B1/F1 Detention** — wake, crush restraints, grab the pistol, fight out → **boss Chief Martinez**.
2. Elevator up. **F2 Medical Bay** — the 3-victim timed rescue (Aria/Keisha/Emily) + **boss Dr. Chen**.
3. **F3 Genetics** — **boss Failed Experiment #7**. 4. **F4 Cybernetics** — augmentation/Humanity choice.
4. **F4.5 Nexus** (off-elevator, found on the F4→F5 path) — **The Chorus** (5 pods, save up to 4).
5. **F5 Drone Manufacturing** — Sarah's 90s master-hack flips the drone army → **boss Swarm Controller AI**.
6. **F6 Alien Tech** — Salvari/K'thara + the cure → **boss Alien Overseer**.
7. **F7 Executive** — **boss Jake's Clone** + timed Sarah rescue → **timeline locks**.
8. **F7 hidden sub-levels** (open only after Clone dead + Sarah saved) → Cryo **Frozen Collective** → **Dr. Chen Return**.
9. **Secrets:** elevator keypad **`1127`** → disco descent → **Club 1127** (Y=−200); Salvari caves below.

## What to check, per area (✓/✗ + a note)
For each floor: loads? · rooms feel right-sized (75–97 m wide)? · elevator stops on walkable floor? · enemies + boss behave (chase/attack/retreat, no T-pose/through-walls)? · keypad doors open? · rescue/timer works + gates correctly? · weapons feel right (fire/reload/switch/damage)? · HUD (HP/ammo/objective/health-bars) readable? · any fall-through / z-fight / see-through walls / stutter?
Demos: **club** (DJ booth/ORB/bars read?) · **elevator** (FSM ride + `1127` disco→club?) · **ragdoll** (collapses naturally?) · **physjoint** (cubes swing when bumped?) · **terrain/ocean/valley/cliffs** (world reads?).

## Honest expectations (most-built → roughest)
- **Most solid:** Act-1 Spire combat loop, floors/elevator/keypads/rescue, weapons, GPU-skinned characters, HUD. Headless-verified by 55 self-tests + 0-VUID smoketests, but *in-game feel* is what you're judging now.
- **Newer / likely rough:** the souped-up elevator FSM + strata + `1127` disco→Club hookup; Club 1127 interior; the real 283 m vertical scale (long elevator rides); Salvari caves.
- **Wired but not yet surfaced in the Level-1 walk:** ToD/Weather, the Timeline/Karma/12-endings backbone, AI-dialog+TTS (these have systems + tests but aren't fully hooked into the default playthrough yet — they're foundations).
- **In progress (fleet):** open-world (regions/city/ocean/subs), Act-2 biomes (L10–20), the in-engine Level Editor.

> Note what's broken/ugly/missing per the checklist and hand it back — that becomes the next polish wave.
