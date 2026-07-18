# Audio Asset Manifest — W2-B (audio-assets pass)

Provenance for every WAV committed under `assets/audio/` by the W2-B sound-department
pass (2026-07-05). All source packs are purchased Unity/Unreal asset-store packs Tim
owns locally under `D:\Assets`; none are redistributed here beyond the curated,
converted WAV committed to this repo (mirrors the existing convention already used by
`assets/audio/weapons/**`, sourced the same way in the per-weapon-SFX pass).

Conversion: source files that were not already 44.1kHz/16-bit PCM mono WAV were
converted with `ffmpeg -i <src> -ar 44100 -ac 1 -sample_fmt s16 <dst>.wav` to match the
format already used by the committed weapon WAVs (see `assets/audio/weapons/single/*`).

| Repo path | Source pack | Original filename | Notes |
|---|---|---|---|
| `enemies/taunt.wav` | Sci-fi Evolution Gift Pack | `Alien Egg Sac Open 1.wav` | Same file already referenced (external-mount only) by the pre-existing `boot_audio.h` for `enemyTaunt` — now made portable. 1.06s. |
| `enemies/attack.wav` | Free Sound Effects Pack ("Free Pack") | `Monster Bite.wav` | Same file already referenced (external-mount only) for `enemyAttack` — now made portable. 1.34s. |
| `enemies/hit.wav` | Sci-fi Evolution Gift Pack | `Alien Game Tech Hit.wav` | Same file already referenced for `enemyHit` — now made portable. 1.77s. |
| `enemies/death.wav` | Sci-fi Evolution Gift Pack | `Fictional Game Goo Kill Smash 3.wav` | Same file already referenced for `enemyDeath` — now made portable. 0.28s (short squelchy crunch; on the low end of the 0.5-2s guidance but this is the pre-existing design pick and reads fine as a sudden kill-smash). |
| `doors/door_open.wav` | Scifi Modular Interior Space Station (`ModularScifiInterior`) | `Sound/S_ScifiDoor_A.WAV` | Same file already used as the shared `bootAudio.door` handle. Servo/pneumatic door open, 2.19s. |
| `doors/door_close.wav` | Scifi Modular Interior Space Station (`ModularScifiInterior`) | `Sound/S_ScifiDoor_B.WAV` | Matched B-side of the door-open pair (same pack), 1.38s. |
| `doors/door_locked.wav` | The Complete UI Sound Effects Library (CelerisLab) | `negative_feedback_and_warnings/access_denied_01.wav` | Clean digital access-denied buzz, 0.51s. |
| `footsteps/step_concrete_1.wav` | Starter Assets - ThirdPerson URP | `Character/Sfx/Concrete_Footsteps/Concrete_01.wav` | Purpose-built single-step concrete take, 0.32s. |
| `footsteps/step_concrete_2.wav` | Starter Assets - ThirdPerson URP | `Character/Sfx/Concrete_Footsteps/Concrete_02.wav` | 0.37s. |
| `footsteps/step_concrete_3.wav` | Starter Assets - ThirdPerson URP | `Character/Sfx/Concrete_Footsteps/Concrete_03.wav` | 0.25s. |
| `footsteps/step_concrete_4.wav` | Starter Assets - ThirdPerson URP | `Character/Sfx/Concrete_Footsteps/Concrete_04.wav` | 0.23s. |
| `player/pain_1.wav` | Free Sound Effects Pack ("Free Pack") | `Bloody punch.wav` | **Gap noted**: no male pain-grunt VO exists anywhere in the local D:\Assets library (checked every character/creature/voice pack). This is a body-hit impact substitute, not a vocalization — closest available fit. 0.56s. |
| `player/pain_2.wav` | Free Sound Effects Pack ("Free Pack") | `Indiana Jones Punch.wav` | Same gap as pain_1 — second impact take so back-to-back hits don't repeat identically. 0.60s. |
| `player/land.wav` | Starter Assets - ThirdPerson URP | `Character/Sfx/Player_Land.wav` | Purpose-built human jump/fall landing thud, 0.26s. |
| `weapons/reload_generic.wav` | Basic RPG Sounds (Multiple Solutions) | `Audio/Shotgun Reload 1.wav` | Literal mechanical reload cycle (pump/rack), 0.97s. For weapon.cpp to assign to `WeaponDef::reloadSfx` (out of scope for this pass — weapon.cpp is owned elsewhere). |
| `weapons/dryfire_click.wav` | The Complete UI Sound Effects Library (CelerisLab) | `basic_interactions_and_navigation/buttons/heavy_mechanical_button/heavy_mechanical_button_click_in_01.wav` | Crisp heavy mechanical click, 0.20s, doubles as an empty-chamber dry-fire click. |
| `ambient/room_tone_cell.wav` | Sci-Industrial Ambience | `WAV/Elements/Low Hum 4.wav` | Steady-state low sci-fi drone, 4.77s. Shorter than the 10-30s guidance but a genuinely steady-state hum with no transient — loops tolerably (no ffmpeg zero-crossing trim was needed/verified beyond that; see caveat below). |
| `ambient/fluorescent_buzz.wav` | Terminal User Interface Sound Effects Pack LITE Edition | `Mechanical/MechAmbienceLoops/CMPTMisc_ComputerAmbienceLoop_HA_TerminalUI_02.wav` | Electrical/computer ambience loop, explicitly authored as a loop by the pack, 5.11s steady-state hum. |

## Loop-cleanliness caveat

Both ambient files were chosen because they are *steady-state* drones/hums with no
audible attack/decay transient at either end (per the mission's own fallback: "if you
can't verify loop cleanliness, pick steady-state hums which loop tolerably") — no
audio playback was available in this environment to verify a perfectly seamless
zero-crossing loop, so `ma_sound_set_looping` restarting from sample 0 may produce a
very faint click on a sensitive system. If that's audible in practice, re-trim with
`ffmpeg -i <src> -af "afade=t=in:d=0.05,afade=t=out:st=<dur-0.05>:d=0.05" ...` for a
50ms in/out crossfade, or re-source a purpose-cut loop.

## Known gaps

- **Player pain vocals**: no male hurt/pain grunt VO exists anywhere in the local
  D:\Assets library (~210 packs checked, including every character/creature/voice
  pack). Substituted with two punch-impact one-shots (see table). A real VO take
  (recorded or a licensed voice pack) would be a straight drop-in replacement at
  `assets/audio/player/pain_1.wav` / `pain_2.wav` — no code changes needed.
- **Enemy vocalizations**: no dedicated creature/monster bark library exists either;
  all four enemy cues are assembled from synthetic/mechanical/organic-squelch one-shots
  already chosen by the original (pre-portability) `boot_audio.h` design, not new
  guesses by this pass.

## Per-species enemy vocals (guard-life pass, W4-3)
Buckets: humanoid = the shared `enemies/*.wav` set (no dedicated files); the two
new sets below are PCM s16le 44.1kHz mono like the rest of the tree.

| Repo path | Source | Processing |
|---|---|---|
| enemies/creature/taunt.wav  | Sci-fi Evolution Gift Pack / Alien Egg Sac Open 1.wav | none |
| enemies/creature/attack.wav | Free Pack / Monster Bite.wav | none |
| enemies/creature/hit.wav    | Free Pack / Monster Bite on Armor.wav | trimmed 1.2 s |
| enemies/creature/death.wav  | Free Pack / Monster Bite.wav | pitch x0.75 (variant) |
| enemies/synth/taunt.wav     | Sci-fi Evolution Gift Pack / Deep Processor Mech Drone.wav | 1.6 s + fade |
| enemies/synth/attack.wav    | Sci-fi Evolution Gift Pack / Tonal Mech Gear 3.wav | none |
| enemies/synth/hit.wav       | Sci-fi Evolution Gift Pack / Wrong Answer Mech Alert.wav | pitch x1.15 (variant) |
| enemies/synth/death.wav     | Sci-fi Evolution Gift Pack / Deep Processor Mech Drone.wav | pitch x0.7 + fade (variant) |

## Interact cues (trapdoor + elevator glow-up pass)
The four keypad/servo cues the elevator has resolved from EXTERNAL pack roots
since 288ce2a (silent on any box without the Unity packs) — now committed, and
shared by the cell trapdoor's new SecretRoomSounds kit (keypad clicks, wrong-code
buzz, access-granted chime, panel-slide servo loop; the seat thunk reuses
doors/door_close.wav).

| Repo path | Source | Processing |
|---|---|---|
| interact/keypad_click.wav | Sci-fi Evolution Gift Pack / Ceramic Menu Button.wav | none |
| interact/chime.wav        | Sci-fi Evolution Gift Pack / Energy Bling.wav | none |
| interact/buzz.wav         | Sci-fi Evolution Gift Pack / Negative Analog Computer Tone 2.wav | none |
| interact/servo_loop.wav   | Sci-fi Evolution Gift Pack / Deep Processor Mech Drone.wav | none (looped by startLoop) |

## Music (club + elevator max-out)
| Repo path | Source | Processing |
|---|---|---|
| music/club_ascension.wav | Free - Sci-Fi and Cyberpunk Music Pack / 01 Ascension.wav (~133 BPM) | -3 dB + 30 ms loop-seam crossfade |
| interact/muzak_loop.wav  | Free - Sci-Fi and Cyberpunk Music Pack / 03 Descent.wav (~120 BPM) | mono mix, -10 dB (cabin background), loop crossfade — REPLACES the stdlib-synth loop |

## Space dogfight combat SFX (feat/dogfight-feel, 2026-07-17)

Curated from Tim's new `D:\Assets` library (4,563-file sweep) for the intro
dogfight beats (`app/intro_orchestrator.cpp` interactive windows). Same
conversion as above: `ffmpeg -ar 44100 -ac 1 -sample_fmt s16` (PCM s16le mono).
Total added: ~0.9 MB.

| Repo path | Source pack | Original filename | Notes |
|---|---|---|---|
| `space/dogfight/player_laser.wav` | Cyberpunk Game (Epic Stock Media) | `Weapons/GUNTech_Weapons Pistol Laser Shot Crisp Energy Quick 01_ESM_CPG.wav` | Crisp quick energy shot, 0.27s — per-shot player laser (2D). |
| `space/dogfight/enemy_laser.wav` | Cyberpunk Game (Epic Stock Media) | `Weapons/GUNTech_Weapon Gunshot Laser Zap Source Layer Shimmer 01_ESM_CPG.wav` | Shimmery laser-zap, trimmed 1.96s -> 1.0s w/ 0.1s fade — enemy fire (3D at muzzle), distinct timbre from the player's. |
| `space/dogfight/impact_shield.wav` | Universe Sounds Free Pack | `Blows/Blow shield.wav` | Pack's purpose-made energy-shield hit, 1.4s — hit while shields up. |
| `space/dogfight/impact_hull.wav` | Cyberpunk Game (Epic Stock Media) | `Damage/GOREMisc_Damage Impact Blunt Metal Armor Bash 01_ESM_CPG.wav` | Metallic armor bash, 0.64s — hit with shields down (DISTINCT from shield). |
| `space/dogfight/explosion_fighter.wav` | Cyberpunk Game (Epic Stock Media) | `Explosions/EXPLDsgn_Explosion Ballistic Massive Cannon Debris 01_ESM_CPG.wav` | Big debris explosion, 2.3s — fighter kill (3D at wreck). |
| `space/dogfight/boost_antimatter.wav` | Universe Sounds Free Pack | `Various Sounds/Teleportation Jump.wav` | Sci-fi energy-jump whoosh, 2.2s — Shift ANTIMATTER BOOST engage. |
| `space/dogfight/forcefield_zap.wav` | Zap VFX - HDRP (Vefects) | `Audio/WAV/SFX_Vefects_Zap_Small_01.wav` | Electric zap, trimmed 2.75s -> 1.2s w/ fade — shield-bubble bounce (`pushOut()`). |
| `space/dogfight/lock_chirp.wav` | Cyberpunk Game (Epic Stock Media) | `UI/UIAlert_UI Chatter Confirm Select Interact Short Techy 01_ESM_CPG.wav` | Short techy confirm chatter — target-lock acquired chirp. |

Engine loops for the dogfight beats reuse the existing committed
`space/engine_hum.wav` + `space/engine_thrust.wav` (synthesized loops from the
--world space pass; volume-crossfaded at FIXED pitch per the owner's "no gear
shifts" ruling — hum tracks speed, thrust tracks throttle/boost).

## Echo Harbor / echotropolis (audio-armory pass, 2026-07-17)

The living-city soundscape for `--world echotropolis`, all under
`assets/audio/echotropolis/**`. The first nine rows are the SAME files the
cyberpunk-audio commit (5ecdeca) loaded from the hardcoded 14900K-only
`D:/Assets/Cyberpunk Game/...` path — now committed repo-local and resolved via
`resolveAudio()` so the city sounds on a fresh clone. The rest fill the silent
slots: heli rotors, mine work, crowd murmur (reuses the committed `crowd/*.wav`),
day-birds bed, gold-economy cues, build ticks, walk-mode footsteps.
Conversion: `ffmpeg -ar 44100 -ac 1 -sample_fmt s16` (house convention).

| Repo path (echotropolis/) | Source pack | Original filename | Use |
|---|---|---|---|
| ambient/city_bed.wav | Cyberpunk Game (ESM) | AMBSubn_Ambience Rainy Cyber City Noises Vehicles Urban Movements 01 | 2D city bed loop (80.7s) |
| ambient/rain_urban.wav | Cyberpunk Game (ESM) | RAIN_Ambience Loop Weather Rain Urban Heavy 01 | 2D rain layer (26.7s) |
| music/night_alleyways.wav | Cyberpunk Game (ESM) | MUSCLoop_Music Loop Explore Dark City Alleyways 01 | 2D music bed (120s) |
| ambient/mine_hum.wav | Cyberpunk Game (ESM) | AMBDsgn_Ambience Loop Computer Servers High Hum 01 | 3D loop at the gold mine |
| ambient/drone_buzz.wav | Cyberpunk Game (ESM) | DSGNDron_Ambience Loop Buzz Drone Hum Steady 01 | 3D loop over the crown |
| ambient/harbor_ship_idle.wav | Cyberpunk Game (ESM) | BOATInt_Ambience Loop Ship Cargo Engine Idle Dark 01 | 3D loop in the harbor |
| ui/confirm.wav | Cyberpunk Game (ESM) | UIAlert_UI Chatter Confirm Select Interact Short Techy 01 | panel/build/ride confirm |
| ui/accept.wav | Cyberpunk Game (ESM) | UIAlert_UI Confirm Purchase Upgrade Positive Interact Interface 01 | play-as / build place |
| ui/deny.wav | Cyberpunk Game (ESM) | UIAlert_UI Deny Error Wrong Negative Computer Interact Malfunction 01 | undo / unaffordable |
| ambient/city_day_birds.wav | Cyberpunk Game (ESM) | AMBSubn_Ambience Loop Weather Wind City Birds Downpour 01 | NEW day bed; vol = f(sun elevation), silent at night |
| vehicles/heli_rotor_a.wav | Cyberpunk Game (ESM) | SCIShip_Transportation Loop Hovercraft Idle 01 | patrol-heli rotor loop (pitch 0.88) — closest rotor-analog in the library; no true helicopter loop exists in D:\Assets |
| vehicles/heli_rotor_b.wav | Cyberpunk Game (ESM) | SCIShip_Transportation Loop Hovercraft Idle 03 | patrol-heli rotor loop (pitch 0.94) |
| vehicles/heli_rotor_c.wav | Cyberpunk Game (ESM) | SCIShip_Transportation Loop Hovercraft Idle 06 | patrol heli 3 (0.84) + OH1 hero (0.78) |
| vehicles/drone_flyby_1..3.wav | Cyberpunk Game (ESM) | SCIShip_Transportation Drone Overhead Flyby 01/02/03 | occasional 3D flyby at a live drone's position |
| mine/shovel_1..2.wav | Basic RPG Sounds (Multiple Solutions) | Shovel Digging 1/2 | mine-crew work one-shots at the seam |
| mine/ore_clank_1..2.wav | Cyberpunk Game (ESM) | GOREMisc_Damage Impact Blunt Metal Armor Bash 01/03 | pick-on-ore clank variants (pitch-jittered at play) |
| ui/gold_coin.wav | UI SFX Free Pack | Purchase/coins_1.wav | 3D coin clink AT the mine every 5 oz banked |
| ui/gold_chaching.wav | UI SFX Free Pack | Purchase/coins_270.wav | 2D treasury cha-ching on day rollover |
| ui/build_tick.wav | Terminal User Interface SFX Pack LITE | Navigation/Clicks/UIClick_Digital_HA_TerminalUI_09 | build-palette cycle/rotate tick |
| ui/phase_chime.wav | Notification and Alerts | Chirp_Bell_Tone.wav | soft chime on time-of-day phase change |
| footsteps/step_street_1..4.wav | Footsteps Sounds - Volume 02 (The Sound Guild) | Footstep_Shoe_On_Street_01..04 | walk-mode footsteps (distance-driven, round-robin + pitch jitter) |

Crowd murmur reuses the already-committed `crowd/murmur_a.wav` / `crowd/murmur_b.wav`
(see the crowd-chatter pass) as fixed 3D loops at the crown play-spots — no new files.
