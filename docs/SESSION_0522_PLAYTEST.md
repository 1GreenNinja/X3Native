# X3Native — Playtest & Polish Session (2026-05-22, 14900K/5090)

Branch: **`feat/wave3-content`**. Interactive B1-detention playtest with Tim driving live feedback.
Companion: the EFLZ specs (`EFLZ_WEAPONS.spec.md`, `EFLZ_B1_OPENING.spec.md`) and the master plan.

## ⚠️ BUILD GOTCHA — read first
Kill the running game BEFORE building or the exe won't relink:
```
taskkill /IM X3Engine.exe /F   # THEN cmake --build --preset windows-vs2026   # THEN launch
```
"BUILD OK" only means *compile* succeeded; a running exe blocks the LINK and you keep testing a stale binary.

## Shipped (commits on feat/wave3-content, newest first)
- `weapon.cpp` — full **12-weapon EL48 roster** (bazooka/laser/lightning/flamethrower/freeze/bfg/railgun/napalm + the 4 archetypes). `--test-weapons` 14/0.
- `monster.cpp` — **enemy facing** fixed (rigged GLBs face +Z; flipped the visual yaw 180°, AI/aim untouched).
- `main.cpp` — **shot originates from the gun** (cvar-driven muzzle: `muzzle_fwd/right/down`; hitscan muzzle flash).
- `main.cpp`+`ui.cpp` — **maximized window** (default 1600x900) + live **RESOLUTION** readout in the menu.
- engine `IRenderDevice::setExposure` + `main.cpp` `r_exposure` cvar — **global brightness** (composite/ACES exposure; default 1.4).
- `main.cpp` — **crouch (C)**, **kick (X)**, **unarmed LMB = punch** (held-fire), `weapon.*` **IDKFA unlimited ammo**.
- `weapon.h` — gun viewmodel pose baked (yaw193/pitch7/fwd1.0/right0.25/down0.35).
- `main.cpp` — **DOOM cheats** `iddqd`/`god`/`idkfa`/`idfa`; `player.*` god mode.
- `main.cpp` (perf) — **dt clamp 0.1→0.034** broke the sim-accumulator death-spiral (11→29 fps).
- `monster.cpp`/`env_art.*` — distance/view culls (harmless guards; not the perf cause).
- HUD glyph-size + objective-overlap fixes; B1 opening + weapons specs added.
- (earlier) Wave 2 PR #5 (water/F2/cliffs); Wave 3 merge (Crystal Valleys `--world valley` + real crystals/ship + game-feel).

## Why it was 11 fps (diagnosis, for the farm)
Menu = 165 (sim FROZEN), playing = 11 (sim runs). GPU idle at ~4% → CPU/main-thread bound, **not** rendering.
The fixed-step accumulator (`SimAccumulator`, netcode P0) ran up to 6 catch-up substeps/frame after the ~20s
load hitch (dt pinned at the 0.1 clamp), each substep = physics->step + scene.update (~14ms) → ~84ms, locked.
Stopgap = tighter dt clamp (0.034 = ≤2 substeps). **Real fixes (farm/engine lane):** GPU skinning (in
progress on the 13700K) + physics body reduction (task #5) so one substep is cheap, then the clamp can relax.

## Live console knobs (backtick)
`r_exposure` · `muzzle_fwd/right/down` · `vm_yaw/pitch/roll/fwd/right/down` · `iddqd/idkfa/idfa/god` · `hud_fps/r_stats`.

## NEXT (prioritized, with pointers)
1. **Resolution readout live on resize** + "make default" button + res/brightness **sliders** (needs a
   settings-save file: persist res + r_exposure, read at startup). Use live `glfwGetFramebufferSize`
   (`main.cpp` ~4088) for the readout, not `device->hudSize()`.
2. **Per-weapon damage to monsters** — thread `WeaponDef.damage` through `Level1Game::onFire` →
   `MonsterManager::fire` → `MonsterSystem::fire` (currently fixed `kDamagePerShot`). + enemy **hit feedback** (HP bar).
3. **Mouse-wheel weapon cycling** to reach weapons 10-12 (number keys only do 1-9, `main.cpp` ~3618).
4. **Weapon specials** + per-weapon viewmodel GLBs (only WeaponEnergyPistol.glb exists; WeaponShotGun.glb missing). See `specs/EFLZ_WEAPONS.spec.md`.
5. **Drone**: hover height + orientation (up-axis) + enforce ranged standoff (`monster.cpp`).
6. **FP arms + legs** (punch/kick visuals). Ref `…\1025\CharacterRenderingSystem_UltimatePhotorealistic.cs`.
7. **B1 opening** sequence (`specs/EFLZ_B1_OPENING.spec.md`).
8. Push `feat/wave3-content` + PR to main (strip the temp `[perf]` probe in `main.cpp` first).
