# X3Native Asset Inventory

**Generated:** 2026-05-20  
**Purpose:** Catalog of purchased game assets available for X3Native's first vertical slice  
(walk level → press button → open door → pick up weapon → shoot monster)

---

## 1. Summary Counts by Category

| Category | Count | Location(s) |
|---|---|---|
| 3D Models (.glb, ready) | 64 rigged + 5 weapons | `G:\GameModels\rigged_glb\` |
| 3D Models (.obj, needs conversion) | ~65 characters/enemies | `G:\GameModels\rodin_glb\` (folders of .obj) |
| 3D Models (.fbx, needs conversion) | ~200+ meshes across packs | See kits below |
| Environment/Level Kit FBX | 83 meshes (SciFi Warehouse) + 80 meshes (ModularSciFi) | `EscapeFromLabZero\Assets\` |
| Character FBX | 10 + 15 | `EscapeFromLabZero\Assets\Models\`, `SuperCasualShooter\` |
| Texture/PBR sets (PNG) | ~150 textures (MSI) + ~78 (SWK) + misc | See texture section |
| Audio – Weapon SFX (WAV) | 131 single shots + 78 rapid fire + 28 loopable | `Sci-Fi_Guns_Game-Of-Weapons\Audio\` |
| Audio – Ambient/Door SFX | 8 WAV | `ModularScifiInterior\Sound\` |
| Audio – Misc SFX | ~100 WAV | `EscapeLab48\Escape Lab 48\Assets\` (root) |
| Audio – Music (loopable themes) | 27 WAV (Sci-Fi Music Pack 1) | `EscapeLab48\Escape Lab 48\Assets\Sci-Fi Music Pack 1\` |

---

## 2. 3D Models Table

### Environment / Level Kits

| Pack | Meshes | Format | Readiness | Notes |
|---|---|---|---|---|
| **SciFi Warehouse Kit** | 83 FBX | `.fbx` | Needs FBX→glTF conversion (Blender or FBX2glTF) | Corridors, walls, doors, floor tiles, ceiling, catwalk, stairs, props. Both versions exist: `EscapeFromLabZero\Assets\SciFi Warehouse Kit\` and `EscapeLab48\Escape Lab 48\Assets\SciFi Warehouse Kit\` |
| **ModularSciFi Interior** | 80 FBX | `.fbx` | Needs FBX→glTF conversion | Full modular interior kit: walls (Wall_A/B/C), floors, ceilings, stairs, doors (SM_Door_A, SM_Door_B, SM_DoorFrame_A/B), console, pipes, fence panels |
| **RPG_FPS_game_assets_industrial** | 36 FBX | `.fbx` | Needs FBX→glTF conversion | Industrial props: barrels, boxes, hangar buildings (Hangar_v1–v4), cargo containers, oil tanks, dumpsters, pipes, generator, road sets. In `EscapeLab48\Escape Lab 48\Assets\RPG_FPS_game_assets_industrial\` |

### Key SciFi Warehouse Kit Mesh List (notable)

Structures: Catwalk, Corridor (4-Way, Corner, Deadend, Wide, Tee, Incline), Floor Tiles (x4), Walls (Plain, Door, Bay Door, Fan, Window, Corner), Ceiling (Closed, Skylight), Pillars, Wall Support, Stairs  
Props: Crates, Barrels, Pallets, Ducts, Shelves, Security Camera, Garbage Bin, Fire Extinguisher, Exit Sign, Fusebox, Sprinklers, Pipes (x6)  
**Doors:** `Wall Door.fbx`, `Wall BayDoor.fbx`, `Wall Door Raised.fbx`, `Wall DoorClosed.fbx`, `Corridor Doors.fbx`

### Key ModularSciFi Interior Mesh List (notable)

**Doors:** `SM_Door_A.fbx`, `SM_Door_B.fbx`, `SM_DoorFrame_A.fbx`, `SM_DoorFrame_B.fbx`, `SM_DoorPanel_A.fbx`, `SM_DoorPanel_A2.fbx`  
Walls: SM_Wall_A/B/C, SM_CornerWall, SM_Wall_Flat_A/B, SM_Wall_Fan, SM_Wall_B_Glass  
Floors: SM_Floor_A/B, SM_Floor_3WayConnector, SM_Floor_4WayConnector  
Ceilings: SM_Ceiling_A/B, SM_Ceiling_3WayConnector, SM_Ceiling_4WayConnector  
Other: SM_Console, SM_Pipes_A, SM_Light_A, SM_Stairs_Together, SM_Stairs_Step

### Weapon Models

| Model | Format | Readiness | Notes |
|---|---|---|---|
| `WeaponEnergyPistol.glb` | `.glb` | **READY** | In `G:\GameModels\rigged_glb\` |
| `WeaponShotGun.glb` | `.glb` | **READY** | In `G:\GameModels\rigged_glb\` |
| `WeaponRailGun.glb` | `.glb` | **READY** | In `G:\GameModels\rigged_glb\` |
| `WeaponRocketLauncher.glb` | `.glb` | **READY** | In `G:\GameModels\rigged_glb\` |
| `WeaponBFG.glb` | `.glb` | **READY** | In `G:\GameModels\rigged_glb\` |
| `AK47.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\Assets\Super Casual Shooter Assets\Models\` |
| `Laser Gun.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\Assets\Super Casual Shooter Assets\Models\` |
| `Huge Expo Blaster Gun.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\Assets\` |

> **No FPS viewmodel** was found pre-made. The weapon GLBs are world/pickup models. For the slice, use them as-is and add a stub viewmodel later.

### Enemy / Monster Models

| Model | Format | Readiness | Notes |
|---|---|---|---|
| `alien_crawler.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — alien monster, best slice pick |
| `EnemyOccupationTrooper777.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — humanoid trooper enemy |
| `memory_hunter.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `the_collective.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `BossBreederQueen.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — boss monster |
| `alien_crawler` (OBJ) | `.obj` | Needs conversion | `G:\GameModels\rodin_glb\alien_crawler\` — higher-poly original with PBR textures |
| `EnemyOccupationTrooper777` (OBJ) | `.obj` | Needs conversion | `G:\GameModels\rodin_glb\EnemyOccupationTrooper777\` |
| `Enemy.fbx`, `InfectedEnemy.fbx` | `.fbx` | Needs conversion | `EscapeFromLabZero\Assets\Models\` |

### Character / NPC Models

| Model | Format | Readiness | Notes |
|---|---|---|---|
| `Sarah.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — also in `G:\textures\SarahGLB\base_basic_pbr.glb` (~11 MB PBR) |
| `Jake` GLBs | `.glb` | **READY** | `G:\textures\JakeGLB\base_basic_pbr.glb` (~16 MB PBR) + shaded variant; also in EscapeLab48 `models\jake\` |
| `AnnaTactical.glb`, `AnnaTactical2.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `chief_martinez.glb`, `marcus_webb.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `BartenderDanny.glb`, `DockWorker.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `SubmarineCrewman.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `Amy.fbx`, `Emily.fbx`, `Eve.fbx` | `.fbx` | Needs conversion | `EscapeFromLabZero\Assets\Models\` — purchased Unity characters |
| `Security_Guard.fbx`, `Security_Chief.fbx` | `.fbx` | Needs conversion | `EscapeFromLabZero\Assets\Models\` |
| `LabResearcher.fbx`, `OfficeWorker.fbx` | `.fbx` | Needs conversion | `EscapeFromLabZero\Assets\Models\` |
| `JetpackGirl` | `.fbx` | Needs conversion | `G:\textures\JetpackGirl\jetpack-girl\lod_basic_pbr.fbx` (~17 MB) with PBR textures |
| `Drone` | `.fbx` | Needs conversion | `G:\textures\DroneFBX\` — lod, lod_basic_pbr, lod_basic_shaded variants |

### Props / Pickups

| Model | Format | Readiness | Notes |
|---|---|---|---|
| `CraftingStation777.glb`, `Tier7CraftingStation42.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — sci-fi prop/interactive object |
| `AugmentationChairB.glb`, `Tier7AugmentationChair256.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `Health Box.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\Assets\Super Casual Shooter Assets\Models\` — health pickup |
| Grenades, crates, barrels | `.fbx` | Needs conversion | `SuperCasualShooter\` + `RPG_FPS_game_assets_industrial\` |
| `Grenade.fbx`, `Gas Cylinder.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\` |
| `Drone Maker.fbx`, `Quad Leg Drone.fbx` | `.fbx` | Needs conversion | `SuperCasualShooter\` |

### Misc Special Models

| Model | Format | Readiness | Notes |
|---|---|---|---|
| `SpaceShip.glb` – `SpaceShip4.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `SportsCar.glb` | `.glb` | **READY** | `G:\GameModels\rigged_glb\` |
| `GreatWhiteSharkGameReady.glb`, sea creatures | `.glb` | **READY** | `G:\GameModels\rigged_glb\` — sea animals |

> **FBX→glTF Conversion Path:** Use Blender 5.1 (already present at `G:\Resources\blender-5.1.0-windows-x64.msi`) File→Import→FBX, then File→Export→glTF 2.0 (.glb). Or use `FBX2glTF` CLI. The blender scripts at `G:\GameModels\blender_decimate.py`, `blender_cleanup.py`, `blender_merge_sides.py` etc. are already in-repo for batch processing.

---

## 3. Textures — PBR Sets

### ModularSciFi Interior (PBR — best quality, direct conversion candidate)

- **Location:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Textures\`
- **Count:** ~73 PNG files organized per-mesh (Wall_A, Wall_B, Wall_C, Floor_A, Floor_B, Ceiling_A/B, Door_A, Door_B, DoorPanel, Console, CornerWall, Stairs, Fence, etc.)
- **Naming convention:** `T_{MeshName}_Dif.png` (albedo), `T_{MeshName}_Norm.png` (normal), `T_{MeshName}_MRAG.png` (Metallic/Roughness/AO/packed)
- **Resolution:** ~14 MB per texture file → approximately 4096×4096 (estimated from file size). Note: `T_MRAGG` suffix indicates Metallic/Roughness/AO/Glow packed channel.
- **Readiness:** Source PNGs are directly usable; run through `tools/ktx2bake` to produce KTX2/BC7 for the engine.

### SciFi Warehouse Kit Textures

- **Location:** `G:\Unity_Projects\EscapeFromLabZero\Assets\SciFi Warehouse Kit\Art\Textures\`
- **Count:** ~78 PNG files covering all warehouse kit pieces
- **Naming:** `{meshname}_a.png` (albedo), `{meshname}_n.png` (normal), `{meshname}_r.png` (roughness), `{meshname}_s.png` (specular/smoothness)
- **Resolution:** ~4 MB per texture → approximately 2048×2048
- **Readiness:** Source PNGs usable; note the channel convention differs slightly from MSI (separate roughness + specular vs MRAG packed). May need channel packing before ktx2bake.

### 12825-Textures Pack

- **Location:** `G:\textures\12825-Textures\Textures\`
- **Contents:** Space/sci-fi auxiliary textures: skyboxes (6-face sets: Space_01, Skybox_01–04), planet textures (albedo+AO+normal), asteroid, cloud, lava, star fields, laser VFX textures, lightmaps, wall atlas sets (Wall_Atlas_01–20 with Ao/ID/Norm/Illum channels)
- **Note:** Also duplicates the ModularSciFiInterior `T_*.png` set — appears to be a merged texture export. Wall atlas set has 20 panels with _Ao, _ID, _Norm, _Illum variants.
- **Readiness:** Source PNGs usable; skybox faces ready for cube map assembly.

### Character Textures (Jake / Sarah / Drone / JetpackGirl)

- **Location:** `G:\textures\JakeGLB\`, `G:\textures\SarahGLB\`, `G:\textures\DroneFBX\`, `G:\textures\JetpackGirl\jetpack-girl\`
- **Contents:** Each character folder has `texture_diffuse.png`, `texture_normal.png`, `texture_metallic.png`, `texture_roughness.png`, `texture_pbr.png` — all separate channels.
- **Readiness:** Ready to pack and convert; GLB versions (Jake, Sarah) already embed PBR textures internally via cgltf.

### EscapeFromLabZero Character Textures

- **Location:** `G:\EscapeFromLabZero_Textures\Characters\` (subfolders: Jake, Sarah, DrChen, Bosses)
- **Contents:** Character-specific texture maps, mostly PNG. Only one texture visible at top level (`00025-744548201.png`); character subfolder structure present.

---

## 4. Audio

### Weapon SFX — Sci-Fi Guns, Game of Weapons Pack

- **Location:** `G:\Unity_Projects\EscapeFromLabZero\Assets\Sci-Fi_Guns_Game-Of-Weapons\Audio\SFX\Wave\`
- **Single gunshots:** 131 WAV files (`Single_Gunshot_Sci-Fi_Gun-01.wav` through `-130.wav` + base file) — massive variety of sci-fi weapon shots
- **Rapid fires (burst):** 78 WAV files (`Rapid-Fires_Sci-Fi_Gun-01.wav` through `-78.wav`)
- **Loopable rapid fires:** 28 WAV files (`Loopable_Rapid-Fires_Sci-Fi_Gun_1.wav` through `_28.wav`) — for automatic weapon hold-fire
- **Format:** WAV, uncompressed
- **Best picks for slice:** `Single_Gunshot_Sci-Fi_Gun-01.wav` through `-10.wav` for variety; rotate randomly on fire

### Door / Ambient SFX — ModularSciFi Interior

- **Location:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Sound\`
- **Files (8 WAV):**
  - `S_ScifiDoor_A.WAV`, `S_ScifiDoor_B.WAV` — door open/close sounds
  - `S_Fan_A.WAV`, `S_Fan_B.WAV` — mechanical ambient fan loops
  - `S_SpaceshipAmbience1.WAV`, `S_SpaceshipAmbience1_mono.WAV` — spaceship ambient loop
  - `S_SpaceshipAmbience2.WAV`, `S_SpaceshipAmbience2_mono.WAV` — second ambient variant
- **Readiness:** WAV, directly usable.

### General SFX Pack (EscapeLab48 root)

- **Location:** `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\` (root level, ~100+ WAV)
- **Notable files for slice:**
  - `Futuristic Air Activated Door Open.wav` — door SFX
  - `Laser Gun Salve.wav`, `Railgun - Shot 6.wav`, `Proton Cannon Fire 3.wav` — weapon SFX
  - `Alien Ship Hum Loop 1.wav`, `Deep Processor Mech Drone.wav` — ambient loops
  - `Health or Energy Game Recharge 2.wav` — pickup sound
  - `Explosion 1.wav`, `Explosion 2.wav`, `Explosion 8.wav` — impact/death SFX
  - `Machine Gun 1.wav` — automatic weapon
  - `Secret door.wav` — alternative door sound
- **Format:** WAV, uncompressed.

### Sci-Fi Music Pack 1

- **Location:** `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Sci-Fi Music Pack 1\`
- **Count:** 27 WAV files (18 loops + 9 themes/stings)
- **Notable tracks:**
  - Loops: `SMP1_LOOP_Battle in the market 1/2/3.wav`, `SMP1_LOOP_Sabotage_1/2/3.wav`, `SMP1_LOOP_Search and rescue 1/2.wav`, `SMP1_LOOP_Zero8_1/2.wav`, `SMP1_LOOP_Illusion.wav`
  - Themes: `SMP1_THEME_Cargoship.wav`, `SMP1_THEME_Gliese 1214b.wav`, `SMP1_THEME_Never again.wav`, `SMP1_THEME_Propaganada.wav`, `SMP1_THEME_Space caravan.wav`, `SMP1_THEME_Voyager.wav`
- **Readiness:** WAV, directly usable. These are proper sci-fi/action looping tracks — excellent for the vertical slice environment music.

---

## 5. Character Controller Extraction (S3 Slice Reference)

This section documents behavior and parameter values extracted from C# controllers in the purchased assets. **For clean-room reimplementation in Jolt `CharacterVirtual` only — no C# code in the engine.**

### Source Scripts Analyzed

1. `G:\Unity_Projects\EscapeFromLabZero\Assets\Scripts\Core\PlayerController.cs` — FPS controller (EscapeFromLabZero core)
2. `G:\Unity_Projects\EscapeFromLabZero\Assets\StarterAssets\ThirdPersonController\Scripts\ThirdPersonController.cs` — Unity Starter Assets TPC
3. `G:\Unity_Projects\SuperCasualShooter\Assets\Super Casual Shooter Assets\C# scripts\Player\FirstPersonController.cs` — SuperCasualShooter FPS
4. `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Scripts\Player\JakeFirstPersonController.cs` — EscapeLab48 FPS (most complete)
5. `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Scripts\Player\Advanced\AdvancedMovementController.cs` — Advanced FPS with coyote time, crawl, jetpack

### Movement Parameter Table

| Parameter | EscapeFromLabZero PlayerController | StarterAssets TPC | SuperCasualShooter FPS | Jake FPS (EscapeLab48) | Advanced FPS (EscapeLab48) | Recommended for Slice |
|---|---|---|---|---|---|---|
| Walk Speed (m/s) | 5.0 | 2.0 | 10.0 | 5.0 | 5.0 | **5.0** |
| Sprint Speed (m/s) | 8.0 | 5.335 | n/a | 8.0 | 8.0 | **8.0** |
| Crouch Speed (m/s) | 2.5 | n/a | n/a | n/a | n/a (crawl=2.0) | **2.5** |
| Jump Height / Force | jumpForce=8 | JumpHeight=1.2m | jumpHeight=0.15 | jumpHeight=0.15 | jumpHeight=1.6m | **1.2–1.6 m** |
| Gravity (m/s²) | 20.0 (downward) | -15.0 | Physics * gravityScale=0.03 | Physics * gravityScale=0.03 | -22.0 | **-20.0 to -22.0** |
| Terminal Velocity | (unused) | 53.0 m/s | (unused) | (unused) | (unused) | **53.0** |
| Speed Change Rate (accel/decel lerp) | (instant) | 10.0 | (instant) | (instant) | (implicit) | **10.0** |
| Air Control Factor | Lerp(v, target, dt*2) | (none) | (none) | (none) | 0.5 | **0.5 (50%)** |
| Coyote Time | (none) | (none) | (none) | (none) | 0.15 s | **0.15 s** |
| Jump Buffer Window | (none) | (none) | (none) | (none) | 0.1 s | **0.1 s** |
| Jump Re-trigger Timeout | (none) | 0.50 s | (none) | (none) | (none) | **0.5 s** |
| Fall State Timeout | (none) | 0.15 s | (none) | (none) | (none) | **0.15 s** |
| Ground Stick Velocity | -2.0 | -2.0 | (implicit) | -0.5 | -2.0 | **-2.0** |
| Mouse Sensitivity | 2.0 | (Cinemachine) | (not exposed) | 3.0 | 1.8 | **1.8–2.0** |
| Vertical Look Clamp (up) | 90° | 70° | n/a | 80° | 80° | **80°** |
| Vertical Look Clamp (down) | -90° | -30° | n/a | -80° | -80° | **-80°** |
| Rotation Smooth Time | (instant) | 0.12 s | (instant) | (instant) | (instant) | **FPS: instant** |
| Standing Height (CharacterController) | (original height var) | (default) | (default) | (default) | 1.8 m | **1.8 m** |
| Crouch Height | 50% of standing | n/a | n/a | n/a | 1.35 m | **1.35 m** |
| Crawl Height | n/a | n/a | n/a | n/a | 1.0 m | **1.0 m** |
| Height Lerp Speed (crouch transition) | (instant) | n/a | n/a | n/a | 12.0 | **12.0** |
| Ground Check Method | `isGrounded` | Sphere overlap (-0.14 offset, r=0.28) | `isGrounded` | `isGrounded` | `isGrounded` | **Jolt `CharacterVirtual` built-in** |
| Grounded Offset | n/a | -0.14 | n/a | n/a | n/a | **use Jolt default** |
| Footstep Audio Volume | (none) | 0.5 | (none) | (none) | (none) | **0.5** |

### Jump Physics Formula (Starter Assets — canonical)

```
jumpVelocity = sqrt(JumpHeight * -2 * Gravity)
```
For JumpHeight=1.2m, Gravity=-15.0: `jumpVelocity = sqrt(1.2 * 30) = sqrt(36) = 6.0 m/s`  
For JumpHeight=1.6m, Gravity=-22.0: `jumpVelocity = sqrt(1.6 * 44) = sqrt(70.4) ≈ 8.39 m/s`

### Camera / Look Behavior Notes

- All FPS controllers split rotation: player body rotates **yaw-only** (Y axis), camera rotates **pitch-only** (local X axis)
- Smooth look: EscapeFromLabZero uses `Mathf.Lerp` with `lookSmoothness=10.0` — `lookSmoothness * Time.deltaTime` as lerp T
- Mouse input: standard `Input.GetAxis("Mouse X/Y")` multiplied by sensitivity; no delta-time multiplication on mouse (only gamepad uses dt)
- Cursor: locked+hidden during play (`CursorLockMode.Locked, Cursor.visible=false`)

### Weapon System Behavior Notes (extracted from WeaponSystem_Complete.cs + JakeWeaponManager.cs)

| Weapon | Damage | Fire Rate | Mag Size | Reserve | Reload Time | Range | Auto? |
|---|---|---|---|---|---|---|---|
| Pistol | 15 | 3 shots/s | 12 | 60 | 1.5 s | 50 m | No |
| Shotgun | 20 (per pellet, 8 pellets) | 1 shot/s | 8 | 32 | 2.5 s | 15 m | No |
| Chaingun | (tier 4, see source) | (high) | (large) | (large) | (fast) | (medium) | Yes |
| Bazooka | 150 (+ 100 explosion, r=5m) | 0.5 shots/s | 1 | 5 | 3.0 s | 200 m | No |
| AutomaticGun (SCS) | 20 (configurable 10–1000) | (per-frame on held) | configurable | — | — | (raycast) | Yes |

**For slice:** implement Pistol or AutomaticGun stats. Hitscan raycast from camera forward; damage interface on enemy.

---

## 6. Slice Asset Recommendations

These are the concrete picks for the vertical slice: "walk level → press button → open door → pick up weapon → shoot monster."

### Level / Environment Kit

**Pick: ModularSciFi Interior**  
- **Path:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Meshes\` (all FBX)
- **Why:** Has dedicated door meshes (SM_Door_A/B + frames), walls, floors, ceilings — full modular set for a sci-fi corridor/room. Companion door SFX already included.
- **Readiness:** Needs FBX→glTF conversion via Blender.
- **Textures:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Textures\` — full PBR sets (Dif/Norm/MRAG).
- **Ambient audio:** `S_SpaceshipAmbience1.WAV` or `S_SpaceshipAmbience1_mono.WAV` — spaceship hum loop for room atmosphere.

**Secondary Pick: SciFi Warehouse Kit** (more industrial, also has good door pieces)  
- **Path:** `G:\Unity_Projects\EscapeFromLabZero\Assets\SciFi Warehouse Kit\`

### Door Model

**Pick: `SM_Door_A.fbx` + `SM_DoorFrame_A.fbx`** (ModularSciFi Interior)  
- **Path:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Meshes\Doors\SM_Door_A.fbx`
- **Readiness:** Needs FBX→glTF conversion.
- **Door SFX:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Sound\S_ScifiDoor_A.WAV`
- **Textures:** `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\Textures\Door_A\T_Door_A_Dif.png`, `T_Door_A_Norm.png`, `T_Door_A_MRAG.png`

Alternative: `Wall Door.fbx` from SciFi Warehouse Kit (`G:\Unity_Projects\EscapeFromLabZero\Assets\SciFi Warehouse Kit\Art\Meshes\Structures\Walls\`)

### Weapon (+ viewmodel note)

**Pick: `WeaponEnergyPistol.glb`**  
- **Path:** `G:\GameModels\rigged_glb\WeaponEnergyPistol.glb`
- **Readiness:** **READY** — .glb, loadable by cgltf today.
- **No viewmodel exists pre-made.** Use the world model scaled and parented to camera for the slice placeholder.
- **Gunshot SFX:** `G:\Unity_Projects\EscapeFromLabZero\Assets\Sci-Fi_Guns_Game-Of-Weapons\Audio\SFX\Wave\Single_Gunshots\Single_Gunshot_Sci-Fi_Gun-01.wav` (through -10 for variety)
- **Mechanic:** Hitscan raycast, 15 dmg/shot, 3 shots/s. See Pistol row in weapon table above.

Secondary weapon pick: `WeaponShotGun.glb` (same path) — same readiness, more visual impact.

### Monster / Enemy

**Pick: `alien_crawler.glb`**  
- **Path:** `G:\GameModels\rigged_glb\alien_crawler.glb`
- **Readiness:** **READY** — .glb, loadable by cgltf today. Already confirmed loadable per engine notes.
- **Why:** Non-humanoid alien monster, visually distinct from player characters, clearly an enemy.
- **Death SFX:** `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Explosion 1.wav` or `Monster Bite.wav` from same folder.

Secondary pick: `EnemyOccupationTrooper777.glb` — humanoid trooper, also ready.

### Pickup / Collectible

**Pick: `CraftingStation777.glb`** (as an interactive button/terminal prop)  
- **Path:** `G:\GameModels\rigged_glb\CraftingStation777.glb`  
- **Readiness:** **READY**  
- **Or** use `Health Box.fbx` (needs conversion) from SuperCasualShooter as a weapon pickup crate.
- **Pickup SFX:** `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Health or Energy Game Recharge 2.wav`

### Music

**Pick: `SMP1_LOOP_Zero8_1.wav`** (or `SMP1_LOOP_Search and rescue 1.wav`)  
- **Path:** `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Sci-Fi Music Pack 1\SMP1_LOOP_Zero8_1.wav`
- **Readiness:** WAV, directly usable.
- **Why:** Sci-fi tension loop, appropriate for a lab/warehouse setting.

### Slice Asset Summary

| Slot | Asset | Path | Readiness |
|---|---|---|---|
| Level kit | ModularSciFi Interior | `EscapeFromLabZero\Assets\ModularScifiInterior\` | Needs FBX→glTF |
| Door | SM_Door_A.fbx + SM_DoorFrame_A.fbx | `ModularScifiInterior\Meshes\Doors\` | Needs FBX→glTF |
| Door SFX | S_ScifiDoor_A.WAV | `ModularScifiInterior\Sound\` | READY |
| Weapon (world model) | WeaponEnergyPistol.glb | `G:\GameModels\rigged_glb\` | **READY** |
| Gunshot SFX | Single_Gunshot_Sci-Fi_Gun-01.wav | `Sci-Fi_Guns_Game-Of-Weapons\Audio\SFX\Wave\Single_Gunshots\` | READY |
| Monster | alien_crawler.glb | `G:\GameModels\rigged_glb\` | **READY** |
| Pickup prop | CraftingStation777.glb | `G:\GameModels\rigged_glb\` | **READY** |
| Pickup SFX | Health or Energy Game Recharge 2.wav | `EscapeLab48\Escape Lab 48\Assets\` | READY |
| Ambient | S_SpaceshipAmbience1.WAV | `ModularScifiInterior\Sound\` | READY |
| Music loop | SMP1_LOOP_Zero8_1.wav | `EscapeLab48\Escape Lab 48\Assets\Sci-Fi Music Pack 1\` | READY |

---

## 7. Provenance Note

All assets cataloged here were purchased by the project owner, primarily from the Unity Asset Store and through Rodin AI generation services. Content (3D models, textures, audio) is generally usable in the owner's own games under the standard Unity Asset Store EULA (which grants a perpetual license for use in games/apps but not redistribution of raw assets). The C# code in Unity projects was mined for behavior reference only — no source code was copied into X3Native. Before any asset is shipped in a commercial release, confirm the specific EULA for that asset (particularly for Sci-Fi Music Pack 1, Sci-Fi Guns SFX pack, ModularSciFiInterior, and SciFi Warehouse Kit — these are the most commercially sensitive).

Rodin-generated models (`G:\GameModels\rodin_glb\` and `G:\GameModels\rigged_glb\`) are AI-generated and owned by the project creator. The `G:\GameModels\rodin_glb\` folder contains raw OBJ exports from Rodin with separate PBR textures; these need OBJ→glTF conversion (Blender recommended) before use in X3Native.

---

## 8. Quick Reference — Directory Paths

| Asset Type | Root Path |
|---|---|
| Rigged GLB (64 models — READY) | `G:\GameModels\rigged_glb\` |
| Rodin OBJ sources (65 folders) | `G:\GameModels\rodin_glb\` |
| ModularSciFi Interior (FBX + PBR) | `G:\Unity_Projects\EscapeFromLabZero\Assets\ModularScifiInterior\` |
| SciFi Warehouse Kit (FBX + PBR) | `G:\Unity_Projects\EscapeFromLabZero\Assets\SciFi Warehouse Kit\` |
| RPG FPS Industrial Props (FBX) | `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\RPG_FPS_game_assets_industrial\` |
| Sci-Fi Guns SFX (237 WAV) | `G:\Unity_Projects\EscapeFromLabZero\Assets\Sci-Fi_Guns_Game-Of-Weapons\Audio\SFX\Wave\` |
| Sci-Fi Music Pack 1 (27 WAV) | `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Sci-Fi Music Pack 1\` |
| Misc SFX (~100 WAV) | `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\` (root) |
| Jake GLB (PBR, ~16 MB) | `G:\textures\JakeGLB\base_basic_pbr.glb` |
| Sarah GLB (PBR, ~11 MB) | `G:\textures\SarahGLB\base_basic_pbr.glb` |
| Drone FBX | `G:\textures\DroneFBX\` |
| JetpackGirl FBX | `G:\textures\JetpackGirl\jetpack-girl\` |
| Texture PBR packs | `G:\textures\12825-Textures\` |
| EFL Character FBX models | `G:\Unity_Projects\EscapeFromLabZero\Assets\Models\` |
| SuperCasualShooter weapon FBX | `G:\Unity_Projects\SuperCasualShooter\Assets\Super Casual Shooter Assets\Models\` |
| FPS Controller C# (reference only) | `G:\Unity_Projects\EscapeFromLabZero\Assets\Scripts\Core\PlayerController.cs` |
| FPS Controller C# (advanced, reference only) | `G:\Unity_Projects\EscapeLab48\Escape Lab 48\Assets\Scripts\Player\Advanced\AdvancedMovementController.cs` |
