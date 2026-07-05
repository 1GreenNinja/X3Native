# Wave 2 Punch List — canonlevel opening 10 minutes (A1 audit, 2026-07-05)

Source: A1 (Sonnet, read-only audit; every item verified in source / parsed GLB binaries).
Executed under the AAA Studio Charter law (docs/design/AAA_STUDIO_CHARTER.md).

## P0 — player-facing, opening minutes
1. **Canon kills have NO gib/blood FX** — app_run.cpp builds the DeathFxFn sink (~1483-1522)
   and wires game + Spire managers, but `canonPlay.setDeathFxSink()` is NEVER called (only
   setCueSink at ~1214). CanonPlay::build self-wires an EMPTY sink (canon_play.cpp:475), so
   monster.cpp:533 `if (m_deathFx)` is always false on the canon path. Fix = 1 call site.
2. **BlueSynth in the first Main Hall fight is a static prop** — no blue_synth_seed*.glb on
   disk → falls back to Drone.glb (0 anims, 0 skins). Ship a rigged BlueSynth or swap the
   opening roster to Trooper/Verthani.
3. **No FP arms/hands** — weapon.cpp drawViewmodel draws the gun mesh only; no arm asset or
   draw path exists at all (confirmed: total absence, not a toggle).
4. **Enemy voices are machine-fragile** — boot_audio.h resolves taunt/attack/hit/death from
   D:/GameDevAssets + G:/Unity_Projects only; none of those WAVs are committed under
   assets/audio/. On this box they DON'T LOAD (no D:/GameDevAssets, no G: letter). Commit a
   curated mirror under assets/audio/enemies/ like weapons already do.
5. **All doors are silent** — DoorSystem has zero audio; the elevator-sounds block is gated
   to the non-canon branch. Add open/close SFX to DoorSystem + canon wiring.

## P1
6. No Attack/Death anim clips for ANY enemy (marcus/crawler/martinez GLBs = Idle/Walk/Run
   [/Jump] only; AiState::Attack has no clip; deaths are rigid topple). Author via the
   headless Blender pipeline (tools/animate_creature.py — see reference notes).
7. No ambient room tone / machinery hum / tube-buzz; only a global action-music loop (wrong
   tone for a cell wake-up). Add an ambient bed + buzz emitter tied to cell_dressing.
8. Reload silent + no dry-fire click on every weapon (no weapon def assigns reloadSfx).
9. Secret room / interactive cell terminal / trapdoor puzzle ABSENT from canonlevel —
   SecretRoom only builds on the legacy path; the cell terminal is a non-interactive prop;
   app_run's secret-room + minimap-hatch blocks read the unbuilt legacy object (inert).
10. Footsteps = gunshot WAV pitched down, player + enemies, no surface variation.
11. No player pain vocal, no jump/land SFX (Player::takeDamage has no audio hook).
12. Minimap room outlines read legacy `game.layout()` even on canonlevel (stale/zeroed rects)
    — branch on canonWorld → feed canonFloor.rooms.

## P2
13. Impact audio per-weapon only, never per-surface; no ricochet system at all.
14. Minimap is a self-admitted stub (app_run.cpp:3365) beyond the layout bug.
15. One shared vocal set for all species — extend GameCue with EnemyType → per-species table.
16. RT acoustics inert on non-RT hardware with no fallback reverb — flat roomless mix.

## Confirmed OK (no action)
- r_exposure / r_autoexposure wired end-to-end (backlog item resolved).
- Enemy taunt/harass trigger logic correct + wired on canon; only the ASSET side (item 4) fails.
