# X3 GAME WORLD RULES — the model & asset constitution
**Status: LAW. Every model, Blender export, converter, and placement obeys this. No exceptions without an owner ruling.**

This document exists because one *class* of bug kept recurring across EVERYTHING — characters facing the wrong way, buried in geometry, a quarter of the correct size, floating, rendering black, sliding off furniture. Each was fixed one-off. They are all the same failure: **assets entered the world without an enforced contract for units, axes, orientation, origin, and material.** These are that contract. Written 2026-07-11 from the full ledger of tonight's defects (Keisha head-at-foot, Jake's ¼-cell + see-through floor, the cm-scale hospital bed, the black metallic props, the floating cell crates, the off-bed captive).

---

## RULE 0 — VERIFY, NEVER ASSUME (the meta-rule that would have caught all of them)
Before any authored asset ships into a scene, it is **rendered orthographic top + side in Blender headless and looked at.** Orientation, scale, and origin are *seen*, not reasoned about. Three agents "fixed" a phantom model bug on Keisha; a 30-second top-render proved the model was perfect and the placement was wrong. **The render is cheaper than the guess-loop.** Tooling: `tools/asset_verify.py` (below) renders + asserts; run it at convert time and before placement.

---

## RULE 1 — UNITS: 1 unit = 1 metre. Always.
- The engine and physics are metric. A door is ~2.1 m, a human ~1.7 m, a bed ~2.3 m, a cell ~7×4×6 m.
- **NO centimetre exports.** The hospital bed imported at 233 units (cm) and had to be rescaled to 2.30. Blender scene unit scale = 1.0, export in metres.
- **Validator:** every asset declares an expected bbox size-class (human / prop-small / prop-large / room). Convert fails loud if the imported bbox is >3× or <0.33× the class norm (catches cm/inch/×100 exports).

## RULE 2 — AXES: one handedness, one up.
- **Engine:** Y-up, right-handed. +X right, +Y up, −Z forward. (docs/CONVENTIONS.md §3 is authoritative; this restates it for assets.)
- **Blender → glTF:** author Z-up; export with `export_yup=True`. The axis map is `(x,y,z)_blender → (x, z, −y)_gltf`. Memorize it; every placement math bug traces to getting this wrong.
- **The converter (`tools/convert_fbx_glb.py`) sets `export_yup=True` — never override it.**

## RULE 3 — ORIENTATION: authored facing is canonical and DOCUMENTED per asset.
- **Characters** are authored STANDING, facing **+Y in Blender** (→ **−Z / forward in engine**). A character dropped into the world with identity transform faces engine-forward. To face elsewhere, the placement applies a yaw — it never bakes a rotation into the mesh unless the pose demands it.
- **Posed/lying bakes** MUST record their result axis in the asset's sidecar note: where the head points, where the feet point, which way the face looks, in ENGINE coordinates. Keisha's supine bake was correct (head −Z, feet +Z, face +Y) — the defect was placement not knowing that. **A pose bake without a recorded orientation is unfinished.**
- **Props** face +Y_blender / −Z engine by default; symmetric props (crates, barrels) may waive facing but still obey origin (Rule 4).

## RULE 4 — ORIGIN / PIVOT: at the CONTACT surface.
- **Characters:** origin at **feet-centre, on the ground plane** (Y=0 at the soles). Placement Y = the floor height; the character stands. A supine character's origin is at the **back plane that rests on the mattress**, so placement Y = mattress surface. (The off-bed captive came from an origin/shift tuned for the wrong orientation.)
- **Props:** origin at the **base-centre that sits on the surface** (a crate's origin is its bottom face). Placement Y = the shelf/floor it rests on. Floating crates = origin not at the base. **Never author a prop with origin at its geometric centre if it's meant to rest on something.**
- **Wall fixtures** (screens, vents): origin at the mounting face; the +Z depth extends INTO the wall, not into the room (the "floating ceiling crates" were duct vents whose depth pushed into the room — origin/facing error).

## RULE 5 — MATERIALS: the render-correctness laws (all learned the hard way).
- **No untextured full-metal.** A drawable with `metallic = 1.0` and no environment renders **BLACK** in a windowless room (a full metal has no diffuse lobe; its only light is a reflection, and reflecting a dark room = black). Every prop either ships a metallic-roughness texture, or its metalness is clamped ≤ ~0.6, or the dressing applies the material-lift. (The F2 black-prop plague.)
- **emissiveTex is only honored on the PBR route.** A Scene entity's `emissiveTex` glows only when `mrTex` is valid — assign a shared 1×1 MR to force the PBR path (the club1127 / intro-cockpit recipe).
- **ACES emissive law.** The pipeline is linear-HDR + ACES + auto-exposure. **Flat `emissive` strength above ~0.5 clips to a white slab.** The durable glow is **texture-gated `emissiveTex` at ~1.1 over a near-black albedo** — bright texels bloom, dark stays dark. (Every screen, portal, crystal, ribbon.)
- **Glass:** authored baseColor alpha in **(0, 0.07)** is honored literally as near-clear glass (the canopy band); ≥ 0.07 gets the legacy opacity floor. `Entity.transparent` routes the real glass pass. Alpha-blended draws come **LAST** in the frame. **Anti-glare display glass:** matte MR (roughness ~0.6, specular ~0.05) so lights give a soft sheen, not hot orbs over text.
- **Additive VFX** (beams, arcs) must keep a **glow floor** — an emissive that drops below the background *subtracts* under alpha-over and darkens the scene (the tractor-beam-goes-dark bug). Only ever add light.

## RULE 6 — WORLD DIMENSIONS: one source of truth.
- Room/level dimensions come from the **LevelArchitect canon** (currently V10.9, `G:/GameDev/LevelArchitectFullV10.9/js/Config.js` FLOOR data) — NOT from a hand-edited JSON that can silently regress. Jake's cell was regressed to 4×3.5×4 in a project JSON vs canon 7×4×6; that single wrong number caused the ¼-size cell AND the see-through floor (the oversized hatch overflowed the shrunk floor → skipped floor segments → the descent chute showed through).
- **Contents position RELATIVE to bounds**, never absolute — so a bounds change re-seats the bed/terminal/hatch automatically. A hatch/opening must be fully inside its floor with margin; the floor builder must never emit zero/negative segments (validate: opening + margin ⊆ floor extent).
- Dimension changes run `--test-canonlevel` (doorway histogram + reachability) — the arbiter for neighbor-overlap.

## RULE 7 — DARK-GLASS SCREENS ARE THE STANDARD.
Any in-world display is the **dark-glass rounded terminal** (near-black inset pane + rounded chrome bezel + texture-gated glowing readout), never a flat bright emissive quad. Reuse `holo_terminal` / `holo_panel` (`bakeMedicalMonitor` / `makeHologramRGBA`). Text lives in zoned columns with the corner-radius safe inset (no line-art through text; TEXT > schematic > decoration brightness hierarchy). Matte anti-glare per Rule 5.

---

## THE VALIDATOR (`tools/asset_verify.py` — to build)
A headless Blender + GLB-accessor check run at convert time and before placement. Asserts, per asset:
1. **Units** — bbox within the declared size-class band (Rule 1).
2. **Axes** — exported Y-up (Rule 2).
3. **Origin** — for `ground`/`rest` assets, origin within ε of the min-Y contact plane (Rule 4).
4. **Orientation** — renders ortho top+side PNGs for eyes-on + records the facing note (Rule 0/3).
5. **Material** — flags drawables with metallic ≥ 0.9 and no MR texture (Rule 5); flags flat emissive > 0.5.
Output: PASS / a per-rule failure list + the two render PNGs. **An asset that hasn't passed does not enter a scene.**

## ENFORCEMENT
- New/converted models: run the validator; attach the top/side renders + facing note to the asset.
- Placement code: position relative to bounds/surface; never bake orientation guesses — read the asset's recorded facing.
- Agents building scenes are handed this file. "It renders wrong" gets a validator run FIRST, before any model or placement change — the render tells you which rule was broken.

*This is the fix for the whole class. One-offs stop here.*
