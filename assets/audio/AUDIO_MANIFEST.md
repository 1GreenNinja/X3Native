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
