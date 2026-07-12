# X3Native — agent operating rules (auto-loaded)

## AXES — THE LAW. Memorize. Never re-derive. (docs/CONVENTIONS.md §3 is authoritative.)
Engine space is **right-handed, Y-up:**

| axis | direction |
|------|-----------|
| **+X** | RIGHT (−X = left) |
| **+Y** | UP (−Y = down) |
| **−Z** | FORWARD — into the screen, the default facing |

- Handedness: right-handed, `+X × +Y = +Z`. Same as glTF / OpenGL / glm.
- **1 unit = 1 metre.** Human ≈1.7 m, door ≈2.1 m, bed ≈2.3 m.
- **Blender → glTF:** author Z-up, export `export_yup=True`. Axis map: `(x,y,z)_blender → (x, z, −y)_gltf`. A character authored facing **+Y in Blender** faces **−Z (forward) in engine**.

## THE MODEL/ASSET CONSTITUTION — read before any model or placement work
**`docs/design/X3_WORLD_RULES.md`** is LAW. It eliminates the recurring class of bug (wrong-facing / buried / wrong-size / floating / black / off-surface models). The seven rules: (0) verify with an ortho top+side render, never assume; (1) metres, no cm; (2) the axes above; (3) orientation is authored + DOCUMENTED per asset in engine coords; (4) origin at the contact surface (feet on floor, base on shelf, mount face into wall); (5) material laws — no untextured full-metal (renders black), emissiveTex needs a valid mrTex, flat emissive >0.5 clips under ACES (use texture-gated emissiveTex ~1.1 over near-black), glass alpha (0,0.07) near-clear, additive VFX keep a glow floor; (6) room dims from LevelArchitect canon (single source), contents relative to bounds, openings ⊆ floor; (7) dark-glass rounded screens are the display standard, never flat bright quads.

**When something "renders wrong": run the verify render FIRST, identify which rule broke, THEN change code.** Do not guess-loop on the model.

## OPERATIONAL
- Bounded engine runs only (`timeout` + kill zombies). NEVER `--smoketest` (Bug 2 Vulkan contention).
- The owner may be PLAYING — check `Get-Process X3Engine` before any engine launch; if one runs that isn't yours, code+build only.
- Clean-room: never read RBDOOM / idTech / Doom / Quake source.
- Push only verified-green; full-res eyes-on every capture; commit local, session lead reviews + pushes.
