# AUDIO LIBRARY MAP — D:\Assets sound library → X3Native game needs

**Catalog date:** 2026-07-17 · **Branch:** `docs/audio-library-map` (doc-only, zero code changes)
**Library:** `D:\Assets` (I9DEVPC local 2TB NVMe) — **4,563 audio files** (.wav/.ogg/.mp3), **~7.4 GB**, across **62 packs**.

This is the game-wide sound map every audio lane builds from. It answers three questions:
1. What sounds do we OWN? (per-pack inventory, §2)
2. Where does the GAME need sound? (per-lane candidate tables, §4)
3. What do we NOT own and must hunt? (gaps, §5)

**Honesty note — how this was graded.** Nobody listened to these files. The catalog is built
from filenames, directory structure, file sizes, and per-file durations measured with ffprobe
(all 4,563 files probed). Duration is the loop/one-shot heuristic: `<2s` ≈ one-shot,
`2–10s` ≈ tail-heavy one-shot or short loop, `≥10s` ≈ loop/bed. Every candidate carries a
confidence grade:
- **HIGH** — professionally named pack, name states exactly what it is (e.g. `Footstep_Metal_01.wav`).
- **MED** — name strongly implies the content but style/quality is a guess (e.g. "Playful Game Explosion" for a sci-fi game).
- **LOW** — plausible from the name only; audition before wiring.

---

## 1. How the game consumes audio (read this before wiring anything)

- Resolver: `app/audio_root.h` → `x3::game::resolveAudio("<rel-path>")`. Search order:
  1. **Repo-local committed mirror** `assets/audio/<rel-path>` — tried FIRST, ships with the build.
  2. External roots: `D:/GameDevAssets`, `G:/Unity_Projects/EscapeFromLabZero/Assets`, `G:/Unity_Projects/EscapeLab48/Escape Lab 48/Assets`.
- **GOTCHA (I9DEVPC):** `D:/GameDevAssets` **does not exist** on this machine, and **`D:\Assets` is
  NOT in the resolver's root list.** A raw pack-relative reference only resolves here if the same
  pack happens to exist on `G:\Unity_Projects\...`. **The established pipeline is therefore: curate
  the file OUT of `D:\Assets`, convert, and COMMIT it under `assets/audio/<lane>/`** — exactly what
  `assets/audio/AUDIO_MANIFEST.md` documents (W2-B pass convention).
- Conversion convention (from AUDIO_MANIFEST.md): `ffmpeg -i <src> -ar 44100 -ac 1 -sample_fmt s16 <dst>.wav`
  (44.1 kHz / 16-bit PCM / mono). Add a provenance row to `assets/audio/AUDIO_MANIFEST.md` for every commit.
- A missing file loads invalid and plays silent — `load()` is graceful, never crashes.
- **Music defaults MUTED** per owner preference (settings screen has Music/SFX volume rows,
  `app/ui.cpp` `setMusicEnabled`/`sfxVol`). Music picks are beds for people who opt in.
- Licensing frame: these are the owner's purchased/licensed Unity-pack assets, curated per-file into
  the repo (never the whole pack). Same precedent as the committed weapon WAVs and curated GLBs —
  see `assets/audio/AUDIO_MANIFEST.md` provenance table and `docs/CLEANROOM_PROCESS.md`.

### What is ALREADY committed and wired (don't re-source these)
`assets/audio/` today: `weapons/{single,rapid,loops,impact}` (per-gun fire + impacts),
`weapons/reload_generic.wav` + `dryfire_click.wav`, `doors/` (open/close/locked),
`footsteps/step_concrete_1-4`, `player/` (pain_1/2 substitutes, land), `enemies/` (+`creature/`,
`synth/` per-species sets), `interact/` (keypad, chime, buzz, servo, muzak, heartbeat, door hiss,
cable creak, club_track), `crowd/` (murmur_a/b, grumble_low — synth-generated walla),
`ambient/` (room_tone_cell, fluorescent_buzz), `water/` (splash_enter/exit),
`space/` (engine_hum, engine_thrust), `rifthub/` (hum, kawoosh, whoosh),
`vehicles/engine_loop.wav`, `music/` (club_descent, club_ascension).

---

## 2. Pack inventory — summary table

Sorted by usefulness to X3Native. "Dur profile" = one-shots `<2s` / mid `2–10s` / loops `≥10s`.

| Pack (dir under `D:\Assets\`) | Files | Fmt | Size | Dur profile | Content sketch (from filenames) | Best for lanes |
|---|---|---|---|---|---|---|
| **Cyberpunk Game** | 1,067 | wav | 1.1 GB | 610/406/51 | THE flagship (ESM "Cyberpunk Game 16bit"): UI 201, Weapons 136 (laser/plasma rifle-pistol-shotgun-sniper-railgun-grenade families ×4-10 takes), Weapon Handling 106 (reloads/equip/drop), Weapon Dry 60 (dry-fire per gun), Footsteps 78 (FEETHmn incl. mud splash), Vocalization 77 (robot/cyborg vox, female combat grunts, alien-language female assistant, low-health breath loops), Dialogue 48 (vocoder droid: access denied/confirmed/warnings), Data+UI-alert hacking beeps, Ambience 38 (rain/wind/vent/server loops), Damage 30 + gore, Transportation 27 (hovercraft idle loop, flying-vehicle takeoff, cyber motorcycle, drone flyby), Explosions 19, Atmosphere 21 (dark tonal beds), Music 6 loops | WEAPONS, UI, FACILITY, SPACE, CLUB, AMBIENCE |
| **The Complete UI Sound Effects Library** | 505 | wav | 87 MB | 493/12/0 | CelerisLab: button_click ×15, rejection ×23, dropdown ×17, notification ×16, upgrade/buff applied, quest_completed, level_up, reward_crate, glass_crystal buttons ×18, inventory open/close, access_denied (already used for door_locked) | UI/HUD |
| **SCI-FI GUNS GAME OF WEAPONS** | 237 | wav | 33 MB | 159/78/0 | 131 single gunshots, 78 rapid-fire bursts, 28 loopable rapid-fire loops — already the per-gun fire source | WEAPONS, SPACE lasers |
| **Health Damage System Sound Effects Pack** | 233 | wav | 101 MB | 176/57/0 | Hit/damage glitches (light/med), health-falling + regen risers, death-cue stingers ×16, heartbeat ×11, healing synths | WEAPONS feedback, HUD |
| **Sci-Industrial Ambience** | 108 | ogg+wav | 269 MB | 4/24/80 | Drones ×20, machinery, low hums (source of room_tone_cell), tech ambience, ventilation, creaky hull, alien beds — most ≥10s loops. NOTE: every file duplicated .ogg + .wav (54 pairs) | FACILITY, AMBIENCE |
| **Footsteps Sounds - Volume 02** | 103 | wav | 5 MB | 103/0/0 | The Sound Guild free pack: per-surface step sets — street/metal/grass/sand/mud/boots/sneakers/sandals/heels/bare-feet/soft, 5-9 takes each, all 0.2s one-shots | FACILITY footsteps, CLUB |
| **Basic RPG Sounds** | 200 | wav | 532 MB | 117/58/25 | Multiple Solutions: **Underwater 1-10 (16s loops)**, submarine, phone booth/dial ×30, safe cracking, lockpick, dungeon door locks, shotgun, gatling laser, electricity, containment unlock, D-pad UI | UNDERWATER, FACILITY, WEAPONS |
| **Space Crafts** | 130 | wav | 171 MB | 2/102/26 | Ship engine lands ×11, futuristic vehicle passbys, speeder/cruiser flybys, plasma ship risers, alien craft engine hovers, RTS vehicle moves — mostly 2-10s | SPACE/FLIGHT |
| **Sci-Fi Alarm SFX** | 30 | wav | 225 MB | 0/7/23 | 30 distinct sci-fi alarm loops, most 16s | FACILITY alarms, SPACE warnings |
| **Terminal User Interface SFX Pack LITE** | 81 | wav | 25 MB | 78/3/0 | Digital clicks, mechanical/laptop keys, negative+positive beeps, VCR buttons, terminal print loops, computer ambience (source of fluorescent_buzz) | FACILITY keypads, UI |
| **Notification and Alerts** | 101 | wav | 27 MB | 74/27/0 | Sci-fi ring tones, alien notifications, sirens ×2, factory alarm hits, hangar alarm tones, achievement bursts, buzz chirps | UI, FACILITY |
| **Shadowy Sci-Fi Cyberpunk Ambiences Lite** | 20 | wav | 494 MB | 0/0/20 | Room-size-graded 90s ambience beds: large_room/medium_space/small_space/long_space ×5 each — maps directly onto RT-acoustics room sizes | FACILITY, AMBIENCE |
| **Ultimate Sci Fi and Dark Ambience Pack** | 80 | wav | 1.8 GB | 0/0/80 | 80 two-minute dark sci-fi beds (40 base + loop variants) — biggest single pool of ambience | AMBIENCE |
| **Ancient Monster Voice** | 49 | wav | 12 MB | 37/12/0 | Creature breaths/growls ×8, orc grunts ×7, groans, orc deaths, monster hits/growls/snarls/roar, 2 character screams, 1 male grunt | Monsters, Salvari stand-in |
| **Car and transportation sounds collection** | 28 | wav | 43 MB | 6/14/8 | **Real elevator set: button/door open+close/start/going(10s)/stop** ×7, tram accel/decel/doors, Citroën 2CV6 engine start/off/warming | FACILITY elevator, vehicles |
| **Fantasy Loot Chest Crate and Lootbox Sounds** | 50 | wav | 21 MB | 18/32/0 | 30 special + 20 basic lootbox/chest opens | Pickups, secret rooms |
| **Free Sound Effects Pack** | 51 | wav | 308 MB | 15/23/13 | "Free Pack": handgun ×4, explosions ×3 (already used), bullet impacts, metal impacts, monster bite (already used), magic spells, medieval city walla-ish bed, missile shot | WEAPONS, misc |
| **Epic Whoosh Flybys** | 38 | wav | 33 MB | 0/38/0 | Cinematic flybys 3-7s: AlienShip, SpaceBattle, StutterShip, Plane, Tank, Transformer, OutofOrbit | SPACE passbys, cinematics |
| **Universe Sounds Free Pack** | 18 | wav+mp3 | 41 MB | 3/13/2 | Spaceship engines ×4, **"Target acquired"/"Missile launch detected" VOICE lines**, cannon blow, shield blow, teleport jump, turret shot | SPACE/FLIGHT |
| **Flipbook VFX Bundle** | 251 | wav | 241 MB | 168/33/50 | Vefects SFX for VFX flipbooks: hits ×15, projectile bursts+loops, **shield loops, bubbles loop 28s**, liquid hits, explosions (magic), gun shots, heal loops, dust loops | WEAPONS impacts, UNDERWATER, shields |
| **Zap VFX - HDRP** | 5 | wav | 3 MB | 0/5/0 | Vefects zap small/medium/big — medium is the wired lightning-gun crackle | WEAPONS (lightning gun) |
| **Sci fi Locks Unlocks** | 37 | wav | 7 MB | 37/0/0 | Device/gadget gears, crank/lever, gear locks, latch clunks, futuristic device opens — all <2s | FACILITY doors/interact |
| **Sci fi Gas** | 42 | wav | 10 MB | 36/6/0 | Gas releases, pressure hisses, plasma tube bursts, sizzles — pneumatic vocabulary | FACILITY vents/doors |
| **Sci-fi Evolution Gift Pack** | 34 | wav | 14 MB | 23/8/3 | Grab-bag already mined 6× (alarm, keypad click, chime, buzz, servo, enemy vox) | (mostly mined) |
| **Damage and Health** | 39 | wav | 13 MB | 20/19/0 | Retro player deaths, damage chirps, **low-health loops ×5**, electrocute | HUD feedback |
| **Epic Game Explosions SFX** | 34 | wav | 6 MB | 34/0/0 | Cartoony/8-bit explosions ("Air Explode Bonus", "Poof", "Playful") — style mismatch risk for a realistic game | LOW-priority explosions |
| **Full Basic Grenade System Explosion SFX** | 6 | wav | 1 MB | 3/3/0 | Grenade throw/impact/explosion ×3/pickup | WEAPONS (grenade) |
| **Epic Whoosh Pack SFX** | 18 | wav | 2 MB | 18/0/0 | Sword/arrow/blade whooshes <1s | Melee, UI transitions |
| **Epic Cinematic Reverse** | 28 | wav | 25 MB | 6/19/3 | Reversed risers/metallic/bells — trailer/transition vocabulary | Cinematics, rift FX |
| **Epic Positive Game SFX** | 57 | wav | 9 MB | 53/4/0 | Collect/bonus/magnet/star-burst/invincible — casual-bright | Pickups (style check) |
| **FREE Casual Game SFX Pack** | 50 | wav | 10 MB | 43/7/0 | Casual UI/game one-shots (DM-CGS_*) | UI (style check) |
| **UI SFX Free Pack** | 38 | wav | 4 MB | 36/2/0 | button_press ×9, ok ×4, cancel ×3, warning ×4, coins, slide+loop | UI |
| **Buttons Slides Dings and Drops** | 36 | wav | 5 MB | 32/4/0 | Chords ×10, button noises, presses, scrapes, alerts, fairy bells | UI |
| **Heat - Complete Modern UI** | 3 | wav | <1 MB | 3/0/0 | click/hover/notification | UI |
| **General RPG SFX Pack** | 32 | wav | 16 MB | 8/24/0 | Footsteps ×5, spell casting, heal, buy/sell, save, menu voice | RPG-ish misc |
| **Magic Game Buffs** | 27 | wav | 10 MB | 13/14/0 | Heal/armor/power-up buffs | Pickups/powerups |
| **Fantasy SFX (Particle Distort)** | 42 | wav | 26 MB | 22/19/1 | Magic explosions, **shield bell/glass/wind**, auras, fire loop | Shields, rift FX |
| **FS Swimming System** | 16 | wav | 3 MB | 12/3/1 | Water splashes ×7, big/small splash, **coming-out-of-water, Underwater Sound Effect (10s)** | UNDERWATER |
| **UniStorm (weather)** | 35 | wav+mp3+ogg | 77 MB | 2/17/16 | Thunder ×6, rain (light/heavy), gusts, hail, birds, crickets | Exterior weather |
| **Urban Abandoned District** | 9 | wav | 51 MB | 2/3/4 | 2 urban ambient beds (231s max), car alarm, car engine, dog barks ×3, fluorescent lamp, transformer hum | City exterior, FACILITY |
| **Scifi Modular Interior Space Station** | 8 | wav | 19 MB | 1/2/5 | S_ScifiDoor_A/B (the wired doors), Fan_A/B (15s), spaceship ambience beds ×4 | FACILITY (mined) |
| **Sci-Fi Construction Kit Modular** | 10 | wav | 16 MB | 1/5/4 | pressure releases, background rumble, engine start, fan, siren, footstep | FACILITY |
| **Industrial Sci-fi Vol I** | 10 | wav | 254 MB | 0/0/10 | 45-180s cyber-city music/ambience LOOPs (Citadel Downtown, Neon Mirage, Synthex) | CLUB/city beds |
| **Industrial Sci-fi Vol II** | 10 | wav | 176 MB | 0/0/10 | 50-160s beds: Cryo Chamber Hum, **Submerged Circuitry**, Neural Static Field, Rain-Slicked Chrome | AMBIENCE, UNDERWATER-adjacent |
| **Sci Fi Ambiances** | 10 | wav | 234 MB | 0/0/10 | 5 atmos beds ×2 (base + loop cut), ~2 min each | AMBIENCE |
| **Sci Fi Ambiant Power** | 5 | wav | 160 MB | 0/0/5 | Dark matter / Old spaceship / Space beds, 1-4 min | AMBIENCE |
| **Sci-Fi Music Pack 1** | 26 | wav | 379 MB | 0/0/26 | SMP1 LOOP/THEME sets (Zero8 already the wired menu music), battle/chase/sad themes 13-169s | MUSIC |
| **Free - Sci-Fi and Cyberpunk Music Pack** | 5 | wav | 129 MB | 0/0/5 | Ascension + Descent already committed as club music; Curiosity/Fallen/Taurus unused | MUSIC |
| **FREE Sci-Fi Horror Music** | 9 | wav | 107 MB | 0/0/9 | Atmosphere ×3, safehouse, stress, chase beds 10-140s | MUSIC (facility horror) |
| **FREE Horror Pack (VOID OST)** | 11 | mp3 | 12 MB | 0/0/11 | "Hide and Seek" theme + 10 loop cuts | MUSIC |
| **Retro synth - 8090s** | 3 | wav | 78 MB | 0/0/3 | 3 synthwave tracks ~2:50 | MUSIC (drive world?) |
| **Deep In Space** | 1 | wav | 23 MB | 0/0/1 | Single 134s deep-space drone bed | SPACE ambience |
| **Pursuit of the Death** | 1 | mp3 | 3 MB | 0/0/1 | Single 184s chase track | MUSIC |
| **Just tik-tok** | 1 | wav | 14 MB | 0/0/1 | 84s "Epic Intense Horror" cinematic track | MUSIC |
| **Organic Rips Movement** | 448 | wav | 64 MB | 422/23/3 | Cloth rips/tears ×200+, duct-tape peels ×90 — foley for cloth/gore, not obviously needed | Gore foley (niche) |
| **Starter Assets - ThirdPerson URP** | 19 | wav | 1 MB | 19/0/0 | Concrete footsteps + player land (already mined) | (mined) |
| **Survival Engine** | 9 | wav | 1 MB | 8/1/0 | build/craft/hit one-shots | misc |
| **First Person Controller Pro** | 3 | wav | <1 MB | 0/1/2 | grass/metal/stone surface loops | footstep ref |
| **3D Scifi Kit Starter Kit** | 4 | wav | 3 MB | 2/1/1 | ambient loop, computer loop, machine loop, pneumatic door | FACILITY |
| **3D Games HUD Pack Vol 1** | 2 | wav+mp3 | 1 MB | 0/1/1 | explosion + music loop | misc |

---

## 3. Where the game already plays sound (code hook survey)

Every `resolveAudio()` call site as of `0b8ff12` (integration/playable-build):

| System | File | Hooks (rel path under assets/audio unless noted) |
|---|---|---|
| Boot kit | `app/boot_audio.h` | gun, door, pickup, death, enemies/{taunt,attack,hit,death} ×3 species, footsteps/concrete ×4, player pain ×2 + land |
| Per-weapon | `app/weapon.cpp` | fireSfx/impactSfx per gun (SCI-FI GUNS singles/loops, Laser_Impact, Vefects_Zap for lightning), shared reload_generic + dryfire_click |
| Space flight | `app/world_hosts/host_space.cpp` | space/engine_hum + engine_thrust loops (throttle-tracked), interact/chime as mode-blip + CRITICAL warn loop |
| Club 1127 | `app/world_hosts/host_club.cpp`, `app/club1127.cpp` | music/club_descent.wav (128 BPM house, beat-synced) |
| Crowd | `app/app_run.cpp:1286` | crowd/murmur_a, murmur_b, grumble_low (synth-generated walla, `tools/gen_crowd_chatter.py`) |
| Elevator | `app/app_run.cpp:1566`, `:7877` | interact/{chime,servo_loop,keypad_click,buzz,muzak_loop,cable_creak,door_hiss_open/close,door_thunk,club_track}, doors open/close |
| Doors | `app/app_run.cpp:1814` | doors/{door_open,door_close,door_locked} |
| Secret room / trapdoor | `app/app_run.cpp:2478` | interact/{buzz,chime,servo_loop,heartbeat}, doors/door_close |
| Water | `app/app_run.cpp:6507` | water/splash_enter, splash_exit — **that's ALL water audio** |
| Cell ambience | `app/app_run.cpp:7127` | ambient/{room_tone_cell,fluorescent_buzz} |
| Terminals | `app/app_run.cpp:8116` | interact/keypad_click |
| Vehicles (drive world + cars) | `app/world_cars.cpp:395`, `app/world_hosts/host_drive.cpp` | vehicles/engine_loop.wav (ONE loop for every car), gun-30 as car weapon |
| Rift hub | `app/world_hosts/host_rifthub.cpp` | rifthub/{hum,kawoosh,whoosh} |
| Intro cinematic | `app/cinematic.h:526` | Sci-fi Evolution Alarm, Free Pack Explosions 1+2, Vefects zap, gun-66 bolt, SMP1 Zero8 music (external-root refs) |
| Menu music | `app/app_run.cpp:1019` | SMP1_LOOP_Zero8 (external-root ref) |

**Confirmed SILENT systems (audio hooks absent):**
- **UI/HUD** — `app/ui.cpp` has volume sliders but no click/hover/confirm sounds anywhere.
- **Descent slide** — `app/world_hosts/host_descentslide.cpp:69` literal `TODO(audio): muffled 128 BPM bass-bleed at the cavern floor (Club 1127 below) — needs loopable playSound3D`.
- **Rescue system** (`app/rescue.cpp`) — zero audio; captive-girl VO/interrupt stingers unserved.
- **Dialog/cutscene** (`app/dialog.cpp`) — text only, no VO or blip-per-character.
- **Underwater** — splashes exist; NO submerged ambience, bubbles, or sonar.
- **Monsters beyond the 3-species kit** — no per-monster variety pool.

---

## 4. THE MAP — candidates per lane

Paths are full `D:\Assets\...` source paths. Pipeline per §1: convert → commit under
`assets/audio/<lane>/` → manifest row. The long Cyberpunk prefix is written out every time;
it is always `D:\Assets\Cyberpunk Game\Assets\Cyberpunk_Game_16bit\Cyberpunk_Game_16bit\<Category>\...`.

### 4.1 SPACE / FLIGHT — ⚠ being wired NOW by `feat/dogfight-feel`; coordinate before committing

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Laser shot | `D:\Assets\SCI-FI GUNS GAME OF WEAPONS\Sci-Fi_Guns_Game-Of-Weapons\Audio\SFX\Wave\Single_Gunshot\Single_Gunshot_Sci-Fi_Gun-*.wav` (131 takes; -01/-30/-57/-66 already wired on-foot — pick UNUSED numbers for ship guns) | HIGH | Purpose-built sci-fi gunshots, 0.3-2s one-shots |
| | `D:\Assets\Cyberpunk Game\...\Weapons\GUNTech_Weapons Energy Lazer Rifle Sharp Quick 01-10_ESM_CPG.wav` | HIGH | 10-take laser family, 2s, "sharp quick" reads as ship cannon |
| | `D:\Assets\Cyberpunk Game\...\Weapons\GUNTech_Weapons Plasma Rifle Energy Bursts 01-09_ESM_CPG.wav` | HIGH | Burst-fire energy variant for a second ship weapon |
| Impact / hull hit | `D:\Assets\Flipbook VFX Bundle\Vefects\Pixel Craft VFX\Audio\WAV\Fer\SFX_Vefects_Hit_*.wav` (15 takes) | MED | Generic energy hits; audition for metallic weight |
| | `D:\Assets\Cyberpunk Game\...\Damage\` (30 files, GOREMisc/UIGlitch families) | MED | Damage-designed one-shots |
| Explosion (ship kill) | `D:\Assets\Cyberpunk Game\...\Explosions\EXPLDsgn_Explosion Ballistic Massive Cannon Debris 01-04_ESM_CPG.wav` | HIGH | "Massive + debris" = kill explosion with tail |
| | `D:\Assets\Cyberpunk Game\...\Explosions\EXPLDsgn_Explosion Energy Plasma Cannon Long 01-03_ESM_CPG.wav` | HIGH | Energy-flavored long boom for energy kills |
| Engine idle loop | already committed `assets/audio/space/engine_hum.wav` (synth) — upgrade candidates: `D:\Assets\Cyberpunk Game\...\Transportation\SCIShip_Transportation Loop Hovercraft Idle 01-08_ESM_CPG.wav` (9.5s loops) | HIGH | Authored idle LOOP, 8 takes |
| Boost / thrust | `D:\Assets\Space Crafts\Ship Land\Covenant_Ship_Engine_Land*.wav` (7.3s swells) + `D:\Assets\Space Crafts\...\Ship_Engine_Rise_*.wav` | MED | Engine swells; reverse/trim for boost onset |
| | `D:\Assets\Universe Sounds Free Pack\Universe Sounds Free Pack\Spaceship Engine 2.wav` / `Spaceship Engine 3.wav` | MED | Free-pack engines, audition quality |
| Enemy flyby / passby | `D:\Assets\Epic Whoosh Flybys\WOOSH FLYBY AlienShip.wav`, `WOOSH FLYBY SpaceBattle.wav`, `WOOSH FLYBY StutterShip.wav` (+2-take variants, 3-7s) | HIGH | Literal ship flybys — doppler passes for dogfight |
| | `D:\Assets\Space Crafts\Pass By\Futuristic_Vehicle_Passby_*.wav` (6 takes) | HIGH | Second passby family |
| Shield zap / hit | `D:\Assets\Flipbook VFX Bundle\Vefects\Pixel Craft VFX\Audio\WAV\Fer\SFX_Vefects_Shield_Loop_01.wav` (+`...\Sergi\..._02.wav`, 4.4s loops) | MED | Shield hum loop; pair with Zap for hit |
| | `D:\Assets\Universe Sounds Free Pack\Universe Sounds Free Pack\Blows\Blow shield.wav` (1.4s) | MED | Shield-hit one-shot, named exactly |
| Lock-on chirp | `D:\Assets\Universe Sounds Free Pack\Universe Sounds Free Pack\Various Sounds\Target acquired (Voice).wav` (2.8s VO) + `Missile launch detected (Voice).wav` | HIGH | Literal targeting VO — instant cockpit flavor |
| | `D:\Assets\Terminal User Interface Sound Effects Pack LITE Edition\...\UIBeep_PositiveArpHigh*_HA_TerminalUI_*.wav` | HIGH | Clean chirp beeps for the lock tone itself |
| Warning klaxon (hull/shield crit) | `D:\Assets\Sci-Fi Alarm SFX\Sci-Fi Alarm SFX\Sci_Fi_Alarm_Loop_01-30.wav` (16s loops, 30 flavors) | HIGH | Replaces the current chime-as-warn-loop hack |
| Space ambience bed | `D:\Assets\Deep In Space\Deep_In_Space\Deep_In_Space.wav` (134s) | HIGH | The pack IS the bed |

### 4.2 WEAPONS on-foot

Per-gun fire is wired (see §3). What's missing: per-gun reloads, per-gun dry-fire, impacts by material.

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Per-gun reloads | `D:\Assets\Cyberpunk Game\...\Weapon Handling\GUNTech_Weapons Plasma Pistol Reload Handling 01-05_ESM_CPG.wav` | HIGH | Pistol-class reload, 5 takes |
| | `D:\Assets\Cyberpunk Game\...\Weapon Handling\GUNTech_Weapons Plasma Shotgun Reload Crisp 01-05_ESM_CPG.wav` (1.2s) | HIGH | Shotgun-class reload |
| | `D:\Assets\Cyberpunk Game\...\Weapon Handling\GUNTech_Weapon Handling Augmentation Cyberware Reload 01-08_ESM_CPG.wav` (0.8s) | HIGH | Techy reload for energy guns — replaces shared reload_generic |
| Equip/holster | `D:\Assets\Cyberpunk Game\...\Weapon Handling\SCIWeap_Weapon Handling Tech Plasma Rifle Equip 01-04_ESM_CPG.wav` (+ Drop 01-04) | HIGH | Weapon-swap foley (currently silent) |
| Per-gun dry-fire | `D:\Assets\Cyberpunk Game\...\Weapon Dry\` — 60 files: `GUNAuto_Weapon Dry Submachine Gun Full Auto P90 Interior *.wav` etc. per gun family | HIGH | A whole dry-fire directory; replaces shared dryfire_click |
| Impacts: metal | `D:\Assets\Free Sound Effects Pack\...\Metal impact.wav` / `Metal impact 2.wav`; `D:\Assets\Footsteps Sounds - Volume 02\The_Sound_Guild_FREE_PACK_Footsteps_Volume_02\WAV - 44100 Hz - 16 Bit\Footstep_Metal_01-07.wav` (as ricochet ticks) | MED | Thin coverage — see gaps |
| Impacts: flesh/gore | `D:\Assets\Cyberpunk Game\...\Damage\GORESplt_Damage Impact Bitreduction Gore Thump 01+_ESM_CPG.wav`, `GORESqsh_*`, `D:\Assets\Survival Engine - Crafting Building Farming\...\Flesh Hit.wav` | HIGH | Named gore impacts |
| Impacts: energy | already wired `Laser_Impact_Light_6.wav`; more: `D:\Assets\Flipbook VFX Bundle\...\SFX_Vefects_Projectile_Burst_Impact_01-03.wav` | MED | Energy splat variety |
| Lightning gun crackle | wired: `assets/audio/weapons/loops/Vefects_Zap_Medium_01.wav`; layer options: `D:\Assets\Zap VFX - HDRP\Vefects\Zap VFX HDRP\Audio\WAV\SFX_Vefects_Zap_Big_01.wav` + `_02`, `SFX_Vefects_Zap_Small_01.wav` | HIGH | Same family = coherent small/med/big charge tiers |
| Grenade | `D:\Assets\Full Basic Grenade System Explosion Sound effects\...\Explosion 1-3.wav`, `Throw.wav`, `Impact.wav` | HIGH | Complete tiny kit, literal names |
| Railgun / heavy | `D:\Assets\Cyberpunk Game\...\Weapons\GUNArtl_Weapons Railgun Three Shot Heavy Burst 01-06_ESM_CPG.wav`; `GUNTech_Weapons Energy Charging Railgun Riser 01-04` (charge-up!) | HIGH | Charge-riser + heavy shot pair for a future heavy gun |
| Hit-feedback (player) | `D:\Assets\Health Damage System Sound Effects Pack\HealthAndDamageSystemSFX\UIGlitch_LightDmg_HA_HealthDmg_*.wav` (22) / `_MedDmg_*` (15); low-health: `HMNHart_HeartBeat_HA_HealthDmg_01-11.wav`, `D:\Assets\Cyberpunk Game\...\Vocalization\HMNBrth_Vocalization Loop Breath Low Health 01-03_ESM_CPG.wav` (5.1s loops) | HIGH | Purpose-built damage-direction + low-health layer |

### 4.3 CLUB 1127 — ⚠ OG14900k owns this lane; these are sourcing suggestions only

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Crowd walla | **GAP** — no real human crowd/walla recording exists in `D:\Assets` (committed `crowd/murmur_*.wav` are synth takes). Closest: `D:\Assets\Free Sound Effects Pack\...\Medieval City.wav` (period-wrong, LOW) | — | See gaps §5 |
| Glass clinks | `D:\Assets\Cyberpunk Game\...\UI\UIClick_UI Item Pickup Glass Clink Knock Impact 01-05_ESM_CPG.wav` (0.5s) | MED | Literal glass clink takes (UI-context recording) |
| | `D:\Assets\The Complete UI Sound Effects Library\...\glass_crystal_button_01-18.wav` | LOW | Glassy UI taps as sweetener layer |
| Glass break (bar brawl) | `D:\Assets\Basic RPG Sounds\Multiple Solutions\Basic RPG Sounds\Audio\Glass Breaking 1-5.wav` | HIGH | Literal glass breaks |
| Bass thump / club track | committed `music/club_descent.wav` + `music/club_ascension.wav`; more flavor: `D:\Assets\Cyberpunk Game\...\Music\MUSCLoop_Music Loop Future Club Deals Cool Cyber 01_ESM_CPG.wav` (105s loop) | HIGH | Literally "Future Club" loop — second room / VIP track |
| | `D:\Assets\Industrial Sci-fi VolI\...\Citadel's_Downtown_LOOP.wav`, `Neon_Mirage_LOOP.wav`, `Synthex_LOOP.wav` (45-180s) | MED | Cyber-city synth beds for club exterior/queue |
| Bass-bleed through floor (descent slide TODO, `host_descentslide.cpp:69`) | low-pass filter the committed `assets/audio/interact/club_track.wav` / `music/club_descent.wav` (ffmpeg `-af lowpass=f=220`) rather than sourcing new | HIGH | Same track = correct diegetic continuity, 128 BPM already beat-synced in code |
| Club ambience bed | `D:\Assets\Shadowy Sci-Fi Cyberpunk Ambiences Lite\Shadowy Sci-Fi Cyberpunk Ambiences Lite\LargeRooms\ambience_large_room_02.wav` (90s) | MED | Large-room air under the music |

### 4.4 FACILITY (cells, halls, elevator, alarms)

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Doors (variety beyond A/B pair) | `D:\Assets\3D Scifi Kit Starter Kit\...\Pneumatic-door.wav`; `D:\Assets\Sci fi Gas\Airy_Gas_Release_1-3.wav` (hiss layer); `D:\Assets\Sci fi Locks Unlocks\Mech_Gear_Clunk_Lock_1.wav` (0.6s bolt clunk) | HIGH | Hiss + clunk layers for heavy/locked door tiers |
| Keypads / terminals | `D:\Assets\Terminal User Interface Sound Effects Pack LITE Edition\...\CMPTKey_MechanicalKey_HA_TerminalUI_*.wav` (5), `UIClick_Digital_HA_TerminalUI_*.wav` (7), `CMPTMisc_TerminalPrintLoop_HA_TerminalUI_*.wav` (5) | HIGH | Purpose-built terminal kit; print-loop for VIGIL terminals |
| Vents | `D:\Assets\Cyberpunk Game\...\Ambience\WINDInt_Ambience Loop Air Vent Full Spectrum Flutter 01_ESM_CPG.wav` (9.4s loop) | HIGH | Literal air-vent loop |
| | `D:\Assets\Sci-Industrial Ambience\Sci-Industrial Ambience\WAV\Elements\Ventilation.wav` (17.2s) + `Ventilation 2.wav`; `D:\Assets\Scifi Modular Interior Space Station\ModularScifiInterior\Sound\S_Fan_A.WAV` (14.8s) + `S_Fan_B.WAV` | HIGH | Vent/fan loop family, same pack as wired doors |
| Elevator (real recordings) | `D:\Assets\Car and transportation sounds collection\Car and transportation sounds collection\ElevatorStartToGo.wav`, `ElevatorGoing.wav` (10s loop), `ElevatorStop.wav`, `ElevatorDoorOpen.wav` (5.6s), `ElevatorDoorClose.wav`, `ElevatorButton.wav`, `ElevatorDoor.wav` | HIGH | Complete real elevator cycle — upgrades the current servo/chime kit |
| Alarms / lockdown | `D:\Assets\Sci-Fi Alarm SFX\Sci-Fi Alarm SFX\Sci_Fi_Alarm_Loop_01-30.wav` (30 loops, 16s) | HIGH | One pack covers every alarm tier (breach, lockdown, drone alert) |
| | `D:\Assets\Cyberpunk Game\...\Environment\ALRMSirn_Environment Loop Emergency Siren Distressed 01_ESM_CPG.wav` (9.5s) + `...System Down 01...` | HIGH | Environmental siren color |
| | `D:\Assets\Notification and Alerts\Alarm_Siren_1.wav` / `_2.wav` (2.2s stingers) | HIGH | Short alarm HITS for one-shot alerts |
| Footsteps by surface | `D:\Assets\Footsteps Sounds - Volume 02\The_Sound_Guild_FREE_PACK_Footsteps_Volume_02\WAV - 44100 Hz - 16 Bit\Footstep_Metal_01-07.wav` (catwalks/vents), `Footstep_Shoe_On_Street_01-09.wav` (city), `Footstep_Grass_*`, `Footstep_Sand_*`, `Footstep_Mud_*`, `Footstep_Boots_*` | HIGH | Per-surface sets, 5-9 takes each, exactly the round-robin format boot_audio already uses for concrete |
| | `D:\Assets\Cyberpunk Game\...\Footsteps\FEETHmn_*` (40) + `FEETMisc_*` (38), incl. `FEETHmn_Footsteps Locomotion Human Mud Splash 01+` | HIGH | Second full surface family (16-bit flavor) |
| Room tones by size | `D:\Assets\Shadowy Sci-Fi Cyberpunk Ambiences Lite\Shadowy Sci-Fi Cyberpunk Ambiences Lite\{LargeRooms,MediumSpaces,SmallSpaces,LongSpaces}\ambience_*_0X.wav` (90s each, 5 per size) | HIGH | Room-size-graded beds — maps 1:1 onto hall/cell/atrium sizes (RT-acoustics friendly) |
| Machinery / strata | `D:\Assets\Sci-Industrial Ambience\Sci-Industrial Ambience\WAV\{Ambiences,Elements}\Machinery*.wav`, `Drone*.wav` (20), `Creaky Hull*.wav` | HIGH | Source pack of room_tone_cell — 100 more where that came from |
| PA / announcer voice | `D:\Assets\Cyberpunk Game\...\Dialogue\ROBTVox_Dialogue Vocoder Robot Access Denied 01_ESM_CPG.wav`, `ROBTVox_Dialogue Spectral Driod {Access Denied, Error Detected, Identity Denied/Confirmed, Confirmation Required, Destination Confirmed}...` (48 files) | HIGH | Robot PA/VIGIL barks — facility security voice for free |
| Drone enemies | `D:\Assets\Cyberpunk Game\...\Vocalization\ROBTVox_Vocalization Robot Synthesized Droid Short 01-10` + `Gibberish 01-10`; `...\Transportation\SCIShip_Transportation Drone Overhead Flyby 01-05` | HIGH | Drone chatter + flyby movement (white-Y drones) |

### 4.5 UNDERWATER / OCEAN

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Submerged ambience loop | `D:\Assets\Basic RPG Sounds\Multiple Solutions\Basic RPG Sounds\Audio\Underwater 1-10.wav` (16s loops, 10 takes) | HIGH | Named exactly; the missing `water/underwater_loop.wav` |
| | `D:\Assets\FS Swimming System\Fantacode Studios\Swimming System\Audio\Underwater Sound Effect.wav` (10s) | HIGH | Second take from a swimming-dedicated pack |
| | `D:\Assets\Basic RPG Sounds\...\Audio\Submarine 3(Underwater).wav` (12.9s) + `Submarine 4(Underwater).wav` | MED | Sub-interior flavor for deep/vehicle sections |
| Bubbles | `D:\Assets\Flipbook VFX Bundle\Vefects\Pixel Craft VFX\Audio\WAV\Fer\SFX_Vefects_Bubbles_Loop_01.wav` (28s loop) + `...\Sergi\..._02.wav`, one-shots `SFX_Vefects_Bubble_01/02.wav` | HIGH | Loop + one-shot pair |
| Splashes / surface transitions | `D:\Assets\FS Swimming System\...\Audio\Big Splash.wav` (2.2s), `Small Splash.wav`, `Water Splash 1-7.wav`, `Coming-out-of-water.wav` | HIGH | Complete enter/swim/exit kit (upgrades synth splash_enter/exit) |
| Swim strokes | `D:\Assets\FS Swimming System\...\Audio\Water Splash 1-7.wav` round-robin | MED | Stroke-rate one-shots |
| Sonar ping | **GAP** — nothing named sonar. Stand-in: `D:\Assets\Terminal User Interface Sound Effects Pack LITE Edition\...\UIBeep_PositiveHighO_HA_TerminalUI_*.wav` pitched + long reverb tail via ffmpeg | LOW | Synthesizable but not owned |
| Whale / shark / sea creature | **GAP** — zero sea-life audio in the library. Nearest texture: `D:\Assets\Ancient Monster Voice\Ancient_Game_Creature_Breath_and_Growl_01-08.wav` pitched down | LOW | See gaps §5 |
| Deep pressure bed | `D:\Assets\Industrial Sci-fi Vol II\...\Submerged Circuitry.wav` (long bed) | MED | Name suggests underwater-industrial tone for the seafloor facility |

### 4.6 UI / HUD (currently 100% silent — biggest cheap win)

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Click / press | `D:\Assets\The Complete UI Sound Effects Library\basic_interactions_and_navigation\buttons\...\button_click_01-15.wav` (505-file pack, category dirs) | HIGH | The dedicated UI library; already sourced door_locked + dryfire from it |
| Hover / move | `D:\Assets\Heat - Complete Modern UI\...\Hover.wav`; `D:\Assets\The Complete UI Sound Effects Library\...\dropdown_menu_01-17.wav` (0.5s) | HIGH | Subtle hover + move ticks |
| Confirm / OK | `D:\Assets\UI SFX Free Pack\...\ok_*.wav` (4); `D:\Assets\The Complete UI Sound Effects Library\...\crafting_success_01+.wav`, `quest_completed_01-09.wav` | HIGH | Positive confirms in two weights |
| Error / denied | `D:\Assets\The Complete UI Sound Effects Library\...\button_click_rejection_01-23.wav`; `D:\Assets\Terminal User Interface Sound Effects Pack LITE Edition\...\UIBeep_NegativeFB_HA_TerminalUI_*.wav` (0.8s) | HIGH | 23 rejection takes; negative beeps match facility terminals |
| Menu open/close | `D:\Assets\Cyberpunk Game\...\UI\UIAlert_Body System Tech Armor HUD System Crisp Open 01-04_ESM_CPG.wav` + `Crisp Close 01-04` | HIGH | Literal open/close pairs, HUD-flavored |
| | `D:\Assets\The Complete UI Sound Effects Library\...\inventory_close_01+.wav` / open | HIGH | Softer inventory variant |
| Notification / toast | `D:\Assets\The Complete UI Sound Effects Library\...\notification_01-16.wav`; `D:\Assets\Notification and Alerts\Positive_Tech_Alert_2/3.wav` | HIGH | Two flavors |
| Level-up / reward | `D:\Assets\The Complete UI Sound Effects Library\...\level_up_01-08.wav`, `reward_crate_open_01-09.wav`; lootbox: `D:\Assets\Fantasy Loot Chest Crate and Lootbox Sounds\...\Special Lootbox *.wav` (30) | HIGH | Progression + chest/secret-room rewards |
| Pickup (upgrade current) | wired `Health or Energy Game Recharge 2` (external ref); commit-path alternates: `D:\Assets\Epic Positive Game SFX\...\Collect Game Material 1-12.wav` | MED | 12-take round-robin pickup pool |

### 4.7 AMBIENCE / MUSIC beds (music defaults MUTED — beds are opt-in)

| Need | Candidate (full path) | Conf | Why |
|---|---|---|---|
| Facility explore music | `D:\Assets\Cyberpunk Game\...\Music\MUSCLoop_Music Loop Explore Laboratory Mystery 01_ESM_CPG.wav` | HIGH | The game IS a laboratory mystery |
| Chase / combat music | `D:\Assets\Cyberpunk Game\...\Music\MUSCLoop_Music Loop Sequence Chase Suspense 01_ESM_CPG.wav`, `...Sequence Escape Danger Caught 01...`; `D:\Assets\Sci-Fi Music Pack 1\Loops\SMP1_LOOP_Battle in the market*.wav` (3 cuts) | HIGH | Named escape/chase loops — interrupt-rescue + alert states |
| Dark facility beds | `D:\Assets\Ultimate Sci Fi and Dark Ambience Pack\Ultimate Sci Fi Ambiences Pack\Ultimate Sci Fi 22-40 Loop.wav` (2 min each, 80 total files) | HIGH | Deepest bed pool; strata variety for the tower |
| Dark tonal stings | `D:\Assets\Cyberpunk Game\...\Atmosphere\DSGNTonl_Atmosphere Tonal Dark Subterranean Complex 01-05_ESM_CPG.wav`, `DSGNDron_Atmosphere Loop Drone Modulating Energy Buzz 01-03` (8.9s) | HIGH | "Dark Subterranean Complex" = level 4.5 monster-section tell |
| Horror layer (hidden level 4.5) | `D:\Assets\FREE Sci-Fi Horror Music\...\Atmosphere 1-3.wav`, `Stress 1-2.wav`; `D:\Assets\FREE Horror Pack Hide and Seek From The VOID OST\9. Hide and Seek (Loop 1-10).mp3` | MED | Horror beds + 10 pre-cut loop variants |
| City / exterior weather | `D:\Assets\Cyberpunk Game\...\Ambience\RAIN_Ambience Loop Weather Rain Urban Alleyway 01-06_ESM_CPG.wav` (30s), `THUN_*` thunder ×7; `D:\Assets\UniStorm\...\Thunder 1-6.wav` (11.6s), `Heavy Rain*.wav` | HIGH | Planet-surface / glass-exterior breach weather |
| Drive world / synthwave | `D:\Assets\Retro synth - 8090s\...\{Run the brave, Space intruder, Can't espace with us}.wav` (~2:50 each) | MED | Synthwave fits LATE NIGHT SPEED vibe |
| Menu / misc themes | `D:\Assets\Sci-Fi Music Pack 1\Themes\SMP1_THEME_*.wav` (Cargoship, Gliese 1214b, Propaganda…) + unused `D:\Assets\Free - Sci-Fi and Cyberpunk Music Pack\...\{02 Curiosity, 04 Fallen, 05 Taurus}.wav` | HIGH | Same packs already shipping music — consistent palette |

---

## 5. GAPS — nothing good in the library; owner should hunt these

1. **Male player pain/effort VO** — confirmed twice now (also in AUDIO_MANIFEST.md W2-B note): zero
   male hurt-grunt takes in all 62 packs. Current pain_1/2 are punch impacts. Drop-in fix once sourced.
2. **Real human crowd walla / cheers / applause** — Club 1127's crowd is synth-generated murmur.
   No recorded walla, bar chatter, cheering, or applause anywhere. (OG14900k lane blocker.)
3. **Sea life** — no whale, shark, dolphin, or any underwater-creature vocal. Ocean world reads dead
   beyond ambience + bubbles.
4. **Sonar ping** — none; synthesizable from UI beeps + reverb, but a real sonar set would be better.
5. **Salvari / alien humanoid vocals** — closest owned: `Ancient Monster Voice` (fantasy orc flavor,
   MED-LOW fit) and 3 files of `VOXFem_Vocalization Synthetic Female Assistant Alien Language 01-03`
   (3.3s, Cyberpunk pack — good for ONE alien NPC voice, not a species). A proper alien-vocal pack is needed.
6. **Female dialogue VO (rescue storyline)** — `app/rescue.cpp` is silent. VOXFem has combat
   grunts/jumps (0.2s barks) only; no spoken lines, sobs, or captive-distress takes. Per-girl dialog
   will need recorded or TTS VO.
7. **Car engine variety (25-car roster)** — one shared `vehicles/engine_loop.wav` for every car. The
   library offers only a Citroën 2CV6 (comedy), a tram, and Cyberpunk hovercraft/motorcycle loops.
   No V8/exotic/EV engine set for the LNSS shop roster.
8. **Bullet impacts by material (rich set)** — metal/concrete/glass impact coverage is thin
   (a handful of Free Pack one-shots). A dedicated ballistic-impacts pack would serve WEAPONS properly.
9. **Human male NPC barks** (guards, club patrons) — nothing; ROBTVox covers robots only.
10. **Rain-on-glass interior loop** — for the glass-exterior breach sequence; nearest is
    `RAIN_Ambience Loop Weather Rain Wind Indoor Simulation` (1 file, MED).

---

## 6. `.done_audio` markers, licensing, and dupes

- **`.done_audio` markers:** 45 pack dirs under `D:\Assets` carry a zero-byte-style `.done_audio`
  marker (another pipeline's ingest bookkeeping — the asset-cataloger/audio sweep). **Ignore them**;
  they do NOT mean the sounds are wired into X3Native. Notably **unmarked** (never swept by that
  pipeline): `Cyberpunk Game`, `Space Crafts`, `Ancient Monster Voice`, `Notification and Alerts`,
  `Sci fi Gas`, `Sci fi Locks Unlocks`, `Epic Whoosh Flybys`, `Epic Game Explosions SFX`,
  `Damage and Health`, `Magic Game Buffs`, `Epic Cinematic Reverse`, `Epic Positive Game SFX`,
  `Epic Whoosh Pack SFX`, `Full Basic Grenade System Explosion Sound effects`, `Organic Rips Movement`
  — i.e. the single most valuable pack (Cyberpunk Game) was never ingested by whatever produced the markers.
- **Licensing:** all packs are the owner's purchased/imported Unity asset-store packs (project
  precedent: per-file curation into `assets/audio/` with provenance rows — see
  `assets/audio/AUDIO_MANIFEST.md`, `docs/CLEANROOM_PROCESS.md`). Packs whose names flag them as
  free/demo tiers — **verify redistribution terms before shipping**: `Footsteps Sounds - Volume 02`
  ("FREE PACK" in its own dir name), `UI SFX Free Pack`, `FREE Casual Game SFX Pack`,
  `Free Sound Effects Pack`, `Free - Sci-Fi and Cyberpunk Music Pack`, `FREE Sci-Fi Horror Music`,
  `FREE Horror Pack Hide and Seek From The VOID OST`, `Universe Sounds Free Pack`,
  `Terminal User Interface Sound Effects Pack LITE Edition` (LITE tier),
  `Shadowy Sci-Fi Cyberpunk Ambiences Lite` (Lite tier), `Sci-fi Evolution Gift Pack` (gift tier).
  Asset-store free packs are normally licensed like paid ones (fine to ship in a build), but the two
  loose single-track dirs `Just tik-tok` and `Pursuit of the Death` look like stray downloads, not
  store packs — **provenance unknown, do not ship without checking**.
- **Dupes:** `Sci-Industrial Ambience` ships every file as both .ogg and .wav (54 pairs — count 108
  overstates unique content by 2×). `Ultimate Sci Fi and Dark Ambience Pack` ships 40 tracks + ~40
  "Loop" recuts of the same material.

---

*Produced by the audio-catalog docs pass, 2026-07-17. Sources: full recursive sweep of `D:\Assets`
(4,563 files ffprobe'd), `app/` code survey at `0b8ff12`. No files were auditioned; grades are
filename/duration heuristics — audition before shipping anything graded MED or LOW.*
