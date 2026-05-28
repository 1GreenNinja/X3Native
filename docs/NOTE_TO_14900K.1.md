# NOTE TO 14900K — pull `feat/cull-combined` (volume 2)

From P13700 / clean-room rig. Big consolidation just pushed. When you're at a stopping point on the weapons-artpass debug, please pull and rebase your latest work on it.

## What's in `feat/cull-combined` (HEAD `7f1aebe` — `X3 v0.3.00420`)

`--version` works; the same line shows under the main-menu subtitle.

### Perf (game-wide)
- **Per-floor PVS cull** for the hand-coded Spire (incl. ~7,937 env-art GLB instances) — B1 went 8568→1646 objects / 49.6M→18.4M tris / 42→20 ms GPU.
- **POV frustum cull** (CPU per-object AABB vs Gribb-Hartmann planes, animated meshes inflate-or-exclude) — stacks on top of the floor cull; **6.7× GPU** measured on a Main-Hall-spine camera (20.5 ms → 3.06 ms). `r_frustumcull` cvar.

### Combat
- **AI shoot-through-floor fix.** Root cause: the **melee** path used `horiz = XZ distance only`, so a B1 enemy under a player on F3 had `horiz ≈ 0` and dealt damage through the slab every cooldown (the render cull hid the model, so it looked like enemies were shooting from outside the level). Fix is two-layer: cross-floor AI gate (`|dY| > 3 m` raycasts Static; if blocked skip AI tick) **+** melee LOS raycast before damage. `--test-multifloor-ai` 2/2 pass.

### Glass
- **UE5/Filament glass shader rework.** `shaders/glass.frag` replaced: F0-from-IOR, Cook-Torrance D·V·F (Filament Smith-GGX height-correlated), roughness-aware Schlick fresnel, fresnel-weighted screen-space env reflection (samples `sceneCopy` along reflected view + sky/ambient fallback), energy-split composite (`kS = Fenv; kT = 1 − max(kS)`).
- New `GlassMaterial` params: `metallic`, `ior`, `reflectance`, `transmittanceColor` (plus the existing `opacity`/`roughness`/`refraction`/`specular`/`tint`).
- Full spec at `docs/GLASS_MATERIAL_SPEC.md`; headless demo at `--world glass --screenshot <path>`; reference capture at `captures/glass_demo.png`.
- **Holo-terminal texture-split.** `makeHologramRGBA` emits alpha-0 on empty pixels (no baked dark glass plate) and the UI rides as **texel-only emissive** (no per-entity flat-teal flood). The terminal now reads as clear glass with a glowing readout overlay — the "solid teal slab" problem is solved.

### B1 content
- **Glass lounge** in the Old Armory (+Z behind Jake's Cell): clear 8 cm blue-tint plate-glass table on 4 slim metal legs, 4 chairs each with 4 legs, all room-tagged.
- **Sit-at-chair** state machine: walk in range + face a chair → `[E] Sit` → 0.35 s cosine-eased FP lerp to a seated pose (1.1 m eye, oriented at the table), free mouse-look, locked movement. `[E] Stand` or tap WASD = stand (camera lerps back to the live standing pose). Same `keyDown` gate as the terminal — no fire-while-typing / no sit-while-menuing. `--test-sit` 8/8 pass.
- **6 placeable terminal kiosks** with real world effects:
  - `cell_lock` code `1278` → UnlockCell (existing)
  - `armory_door` code `OPEN` → OpenDoor (door B)
  - `lights_central` code `LIGHTS` → ToggleLights (cycle nearest 3 point lights)
  - `alarm_armory` code `ALARM` → TriggerAlarm (3 s screen-tint pulse)
  - `lore_intel` code `READ` → ShowLore (6-line PROJECT X3 dump)
  - `crate_dispense` code `CRATE` → SpawnCrate (emissive crate in arena)
  - Generalised `HoloTerminalSystem` (`app/holo_terminal_system.*`) so more can be added by data. `--test-terminals` 21/21 pass.

### Versioning (your feat/versioning, merged)
- Build-time `0.3.<commit-count>` via `cmake/GitVersion.cmake` → `engine/core/version.h` (template at `version.h.in`).
- `--version`, console `version`, menu subtitle, `HudModel::showVersion` ready for an in-game watermark.

### YOUR feat/doors-death-anim — all folded in
- `0ddb17d` gib-despawn + ragdoll quat teardown
- `62c5848` + `583b11b` 3p view (F1=first / F2=third, Jake_22_actions.glb, follow cam, held weapon)
- `ad5c8a5` stoppable auto-fire loop + Settings audio polish
- `e02b024` Music + SFX volume sliders (live)
- `0be04a3` LMB-fire + RMB-autorun suppressed in menus (`simFrozen` gate)
- `4b0fce5` arrow-key turn + move (RDP-friendly)
- `adb5925` canonlevel doored-doorway pass-through + real crouch capsule

## How to pull

```
git fetch origin
git checkout feat/cull-combined          # OR
git merge feat/cull-combined             # fold it into your current branch
```

Gate has been green throughout: 75/75 `--test-*`, Release+Debug `--smoketest` tower + canonlevel exit 0, 0 VUID, `allocationCount=0`.

## Still queued (landing soon, will push on top)

- **Health-bar polish** — thin, distance-fade, **LOS-occluded** (no through-walls), **on every monster**. Agent finishing now.
- **IDDQD clears the red damage flash** — one-liner (`Player::clearDamageFlash`).
- **Minimap floor indicator** — `FLOOR: B1 / F1 / F2 ...` text under the radar, debug-grade.

— P13700 / clean-room integrator
