# SESSION HANDOFF — X3Native fleet (13700K clean-room) — 2026-05-27

> Full state dump for clean continuation after context compaction. Everything below is on disk / pushed unless marked.

## Branches (ALL pushed; ~16+ behind `origin/main` → integrator rebases; this machine does NOT push main)
- **`feat/openworld`** — `--world openworld` (world_regions + city + ocean_base), gated. READY.
- **`feat/weapons-artpass` @ 844e5d0** — **10 weapons** + jagged lightning-bolt FX. Gated (Release+Debug, 0 VUID, alloc=0). Roster in `app/weapon.cpp makeDefaultRoster()`: pistol(WeaponEnergyPistol), smg(WeaponX), shotgun(WeaponShotGun), plasma(*reuses pistol — no distinct model*), chaingun(WeaponChainGun), plasma_rifle(WeaponPlasmaRifle), lightning(WeaponLightningGun), bfg(WeaponBFG), rpg(WeaponRPG), railgun(WeaponRailgun). Lightning bolt FX lives in `app/fx.{h,cpp}` (`addBolt`/`drawBolt`) + routed in `main.cpp` (`arsenal.current().beam`).
- **`feat/enemy-artpass` @ 3cac38b** — drone model swap (converted_glb/Characters/Drone.glb) + real Synth wired as `assets/rigged_glb/blue_synth_seed1.glb` (8.6MB, downscaled). ⚠️ **BlueSynth tuning is a FLYER but the synth is a humanoid** → set `flyer=false` in monster.cpp's BlueSynth def OR add a new ground-melee "Synth" enemy entry.
- **`feat/companion-ai` @ a4a32ad** — companion design spec + **reflex-AI core (Slice A) DONE, 8/8 tests**. See below.

## Companion AI — design at `docs/superpowers/specs/2026-05-26-companion-ai-design.md`
**Two-brain:** deterministic server-authoritative **reflex** (utility+BT → player commands, netcode-safe) + async off-sim **LLM cognitive** (**Grok = girls, Claude = guys**) feeding *intents* + *speech* via the `CompanionSuggestion` seam. Control = autonomous + light suggestions. Identity = both rescued story chars AND generic co-op-slot fill. Squad ≤ 7. Downed + revive. Reuse the `Player` controller.
**5 subsystems:** (1) reflex AI [**A=core DONE**, **B=integration NEXT**], (2) cognitive intent layer, (3) conversation, (4) dual-provider LLM (Grok/Claude, prompt caching), (5) voice (TTS/STT).

### Slice A — DONE (`app/companion.{h,cpp}`, commits 4111bee→a4a32ad)
`CompanionBrain::tick(CompanionContext)->CompanionCommand` — pure/deterministic utility scorer; behaviors Follow/Engage/TakeCover/Revive/Reload/Retreat/Hold + suggestion bias; `--test-companion` = 8/8. Opus-reviewed + fixed (null-guard, Reload on empty mag, Revive range-gate, named `constexpr` tuning block). **Plan:** `docs/superpowers/plans/2026-05-27-companion-reflex-ai.md`.
**Slice-B handoff flags from review:** `reviveAction` = *intent* (Slice B gates the real revive on proximity); guarantee `threats != nullptr` when `threatCount>0`; no hysteresis (threshold chatter — add stickiness later); aim uses CONVENTIONS.md yaw basis (matches Player).

### Slice B — NOT BUILT (the next build; use agents). Design locked:
Self-contained **`--world companion` showcase** (mirror `--world valley/openworld`): spawn player + N companion **`Player` instances** + a few enemy `MonsterSystem`s. Per tick, per companion: build `CompanionContext` from the live scene (selfPos, playerPos, threats = live monsters' `pos()`/dist/LOS, downed flags; `nearCover=false` — **cover deferred, no cover system exists**) → `brain.tick()` → map `CompanionCommand`→`PlayerInput` → `companion.update()` + fire weapon at the aim target. **Downed/revive state machine** (HP→0=downed; companion in `kReviveRange` revives over a timer). **Direct steering — NO navmesh/pathfinding exists.** Squad supports ≤7; spawn 3 in v1. Threats: `MonsterSystem` exposes `pos()`, `alive()`, `hp()`; spawn via `buildMonsterTuned`. Player API: `spawn/update(PlayerInput,dt,physics)/camera/hp()/takeDamage/heal/isAlive` (IDamageSink). Add `--test-companion-squad` self-test + headless screenshot.

## Asset pipeline — `G:\X3Native\tools\rodin_to_glb\` (in MAIN clone working dir, UNTRACKED — should be committed somewhere)
- `rodin_to_glb.py` (FBX/USDZ→GLB, RENAME map for slot names), `preview_glb.py` (Cycles render), `extract_frames.py` (video→PNG frames), `downscale_glb.py` (decimate to tri budget + cap textures).
- **Flow: convert → preview → downscale ONLY the heavy ones.** Check tri count first: railgun was 1.94M (decimate hard), synth 110k (light), car/pistol ~35k (LEAVE FULL — don't downscale). Use `AUTO` (PNG) textures, NOT lossy JPEG.
- ⚠️ **Rodin "Sim-Ready On" is useless for X3Native** (engine sets up all physics in code; ignores embedded mass/friction/collision/semantic) + bloats files → turn it **OFF** for chars/weapons.

## Asset backlog (converted to `_incoming/_glb_out`, NOT yet wired)
- `Pistol.glb` (7.9MB, "correct textures") → **awaiting Tim's go** to swap into `WeaponEnergyPistol.glb`.
- `Sportscar.glb` (9.5MB, **FULL quality — Tim said DON'T downscale**) → wire to `--world drive`.
- `DrLabResearcher.glb` (10.1MB) → NPC.
- `ArmoredFuturisticSoldier` — `.zip` just landed in `_incoming` (also a `.png` concept) → enemy trooper; convert→downscale→wire.
- Earlier: Lab Girls, PixieGirl, MantaRay, BFG_Maybe, RPG, WeaponX in `_glb_out`.

## Character library — `G:\GameModels\rigged_glb` (~46 game-ready) + `rodin_refs` (deep concept tiers)
Companions: Anna*/Sarah/female set (Grok girls); chief_martinez/marcus_webb/dr_chen/male set (Claude guys). NPCs: BartenderDanny, Mechanic, DockWorker, DrJohnson, DrLabResearcher. Enemies: blue_synth*, synth_halfhead*, Boss*, EnemyOccupationTrooper777, the_collective, memory_hunter, alien_crawler.

## Netcode (human co-op) — `engine/net/*` (on main); spec `specs/NETCODE-architecture.spec.md`
Phase 0/1 foundations BUILT + self-tested (`--test-net/netsync/netinterp/netpredict`) but **NOT wired into the game loop**. `createUdpTransport()` returns nullptr (**Phase 2 = real UDP + listen-server + co-op players NOT done**). Phase 3 (PvP), 4 (sharding) not done. NPC companions need NO networking (server-side command source).

## Build notes
- **cmake = VS2026 (18/Insiders):** `C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` (NOT VS2022 — can't create the "Visual Studio 18 2026" generator). See memory `x3native-build-cmake`.
- Per fresh worktree: `<cmake> --preset windows-vs2026` (slow first configure), then `<cmake> --build build --config Release|Debug`. Binary: `build\bin\<Config>\X3Engine.exe`.
- Gate: Release + Debug `--smoketest` → "30 frames + recreate OK", 0 VUID, `allocationCount=0`. **Transient note:** a smoketest run immediately after a Blender process sometimes returns exit `-1` — re-run; it's a fluke (the asset loads print before any render).
- Blender: `C:\Program Files\Blender Foundation\Blender 5.0\blender.exe` (5.0.1). 2× GTX 1080 Ti on this box + full SD stack on G: (can gen SD refs locally, 2 GPUs).

## Model Testing station (Tim's idea — PROPOSED, not built)
`--world modeltest` "The Bar": Club-1127-style interior, Bartender Danny behind the bar, weapons on a lit display stand (cycle all 10), sportscar parked, characters posed; FP walk + turntable pedestal + labels (name·tris·MB) + `[`/`]` cycle; headless capture. High value: see EVERY model in-engine (the real fix for "are models ruined") + consume the asset backlog.

## Open items / NEXT
1. **Slice B** (companion integration) — build via agents (design above).
2. Subsystems 2–5 (cognitive / conversation / dual-provider Grok+Claude / voice).
3. Netcode Phase 0 wiring → Phase 2 (UDP co-op).
4. Asset wiring: pistol swap, car→`--world drive`, NPCs (DrLabResearcher/Danny→rescue/dialog), ArmoredFuturisticSoldier (Rodin→wire), the backlog. Best done via the **Model Testing station** + direct wiring.
5. Synth flyer→ground fix (monster.cpp BlueSynth `flyer=false` or new entry).
6. Slack: plugin installed + reloaded, MCP needs Tim's OAuth to coordinate the fleet.
