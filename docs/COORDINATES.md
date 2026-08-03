# X3Native / Echo Harbor — THE COORDINATE LAW

Every asset conversion and layout import MUST state its source frame and cite this doc.
Two shipped bugs already came from implicit frames (Blender roads exported as vertical
walls; Unity layouts would mirror without the Z-flip). Never assume — declare.

## ENGINE WORLD FRAME (X3Native, glTF-aligned)
- **Right-handed, Y-UP.**
- **+X = east, +Y = up, +Z = north** (Echotropolis convention).
- Yaw convention (cameras, agents, shot-cam): `forward = (cos yaw, 0, sin yaw)` on the
  ground plane → **yaw 0 = +X (east), yaw π/2 ≈ 1.5708 = +Z (north)**. Pitch >0 looks up.
- Matrices are **column-major float[16]**: cols 0..2 = rotated/scaled basis vectors,
  col 3 = translation. `T[12],T[13],T[14] = x,y,z`.
- Echotropolis ground: island heightfield `hf.heightAt(x,z)`; crown ground ≈ **Y 190**
  (NOT 0 — cameras at "street level" need y ≈ 200-240).

## SOURCE FRAMES + CONVERSIONS
| Source | Frame | Conversion to engine |
|---|---|---|
| glTF / GLB | RH, Y-up, -Z forward | **None** — native. |
| Unity scene/prefab (.unity YAML) | **LEFT-handed**, Y-up, +Z forward | **Mirror Z**: `pos' = (x, y, -z)`, `quat' = (-qx, -qy, qz, qw)`, scale unchanged. (tools/unity_scene_to_layout.py does this.) |
| Blender (authoring) | RH, **Z-UP** | Author ground planes in Blender **X-Y at Z=height**; export with `export_yup=True` → becomes engine X-Z ground, +Y normal. Verts typed as (x, y, z) with y=height make VERTICAL WALLS — the road-plane bug. |
| Unity FBX meshes | (baked by converter) | Already handled by the FBX→GLB armory conversion — per-mesh GLBs in `D:/Assets/_glb` are engine-native. Only SCENE transforms need the Unity mirror above. |

## CAMERA / SCREENSHOT ORIENTATION (bit us on 2026-07-17)
- The engine camera basis gives: **facing north (+Z, yaw≈1.5708), screen-RIGHT = WEST (−X)**
  (right = cross(forward, up) = (−1,0,0)). Top-down captures are therefore EAST-on-the-LEFT —
  a naive map reading is mirrored. When reading any top-down: screen-up = +Z north,
  screen-LEFT = +X east. (The Recife pad went into the sea from exactly this misread.)
- Island land truth (corrected): downtown crown (-20,760) sits mid-island; the big OPEN
  FLATS are EAST (x ≈ +300..+1800); island ends in sea cliffs around x ≈ −800 west.

## DISTRICT PADS (.layout files)
- `.layout` line = `<glb> px py pz qx qy qz qw sx sy sz` — **already engine-frame**
  (the offline tool applied the Unity mirror). Host applies only the PAD transform:
  `world = Translate(padX, groundY, padZ) · RotY(padYaw) · Scale(padScale) · line-TRS`.
- Layout Y=0 is the pack scene's ground; pad seats it at `hf.heightAt(padX,padZ)`.
