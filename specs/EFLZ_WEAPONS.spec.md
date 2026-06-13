# EFLZ — Full Weapon Roster (from Escape Lab 48 v9.921 "PhotoRealistic Weapons enhanced")

> Source of truth: `G:\GameDev\Web Escape Lab 48 versions\v9.921_ultimate\js\systems\WeaponSystem.js`
> (+ `EnhancedWeaponEffects.js` for the photoreal muzzle/impact FX). Bring ALL of these into the
> native X3Native `Arsenal` (app/weapon.h `WeaponDef` + `makeDefaultRoster()`), replacing the current
> 4-weapon placeholder set (pistol/SMG/shotgun/plasma). Tim, 2026-05-22.

## The 12 weapons (web stats → native WeaponDef targets)

| # | Name | Dmg | Ammo | Type / special |
|---|------|-----|------|----------------|
| 1 | **Blaster** | 25 | ∞ | energy projectile (starter; infinite ammo) |
| 2 | **Bazooka** | 95 | 50 | projectile, speed 18 (fast rocket), splash |
| 3 | **Laser Gun** | 15 | ∞ | hitscan beam, **overheat** system (no ammo, heat gauge) |
| 4 | **Plasma Cannon** | 70 | 75 | heavy plasma projectile |
| 5 | **Chain Gun** | 20 | 300 | automatic, spread 5° (minigun) |
| 6 | **Shotgun** | 35 | 60 | 5 pellets, 30° spread |
| 7 | **Lightning Gun** | 12 | 150 | chaining hitscan (rapid) |
| 8 | **Flamethrower** | 8 | 250 | short-range continuous, DoT |
| 9 | **Freeze Ray** | 30 | 100 | slows/freezes target |
| 10 | **BFG-11K** | 500 | 10 | massive AoE blast (the big one) |
| 11 | **RailGun** | 150 | 30 | piercing hitscan slug |
| 12 | **Napalm Launcher** | 40 | 40 | projectile + lingering fire pool |

(Damage values are the web tuning — re-map to the native `combat::` balance bands for playtest, like
the bestiary did. Keep the relative ordering + the special behaviors.)

## Native mapping notes
- `WeaponDef`: name, damage, fireRate, magSize/reserve, automatic, hitscan-vs-projectile, pellets, spread.
- Specials to add to the native model: **overheat** (Laser), **DoT/fire-pool** (Flamethrower/Napalm),
  **freeze/slow** (Freeze Ray), **chain** (Lightning), **AoE/splash** (Bazooka/BFG/Plasma), **pierce** (RailGun).
- FX: port the photoreal muzzle flash / tracer / impact from `EnhancedWeaponEffects.js` onto the engine's
  particle + decal pipelines (already present: `IRenderDevice` instanced billboards + decals).
- Viewmodels: need GLBs per weapon (rigged_glb has WeaponEnergyPistol/RocketLauncher/…; source/convert the
  rest, or reuse the closest). `WeaponShotGun.glb` is currently MISSING (engine logs a load fail) — supply it.

## Related (photoreal characters / FP arms)
- `C:\GameDev\OneDrive\Desktop\2025\Escape from Lab Zero\1025\CharacterRenderingSystem_UltimatePhotorealistic.cs`
  — the original "Ultimate Photorealistic" character rendering system. Reference for: photoreal character
  shading AND the requested **first-person arms + legs** (punch/kick viewmodel) — Jake is currently a
  bodiless camera. Separate task from the weapon roster.
