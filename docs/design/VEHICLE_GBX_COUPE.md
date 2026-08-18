# GBX COUPE — the hero car asset sheet

*W-HEROCAR, 2026-08-17. Built by `tools/build_gbx_hero_car.py` from the Unity
package **HDRP GBX COUPE Free Update**. Roster entry `gbx` in
`app/car_roster.h`; GLB `assets/converted_glb/Vehicles/GBX_Coupe.glb`
(store-served — `python tools/asset_store.py fetch --all`).*

---

## 1. WHAT THIS CAR ACTUALLY IS — read this before quoting it to the owner

The owner asked for **"acura NSX-S-R 2022 A spec cars, in Black"**.

**This is not an NSX and it is not mid-engine.** The 914-package library owns no
NSX and no mid-engine supercar mesh — the traffic lane established that honestly
(`python tools/unitypackage_index.py --search nsx` finds nothing) and shipped a
Skyline labelled `NSX-SUB` as the placeholder. This is the *best hero-car mesh
the library actually holds*, found only after `tools/unitypackage_extract.py`
made the ~700 never-extracted packs usable.

What it IS, from the geometry rather than the marketing:

| | |
|---|---|
| body style | 2+2 front-engine GT coupe, fastback roof, ducktail rear deck |
| overall | **1.926 W × 1.403 H × 4.723 L m** |
| wheelbase | **2.882 m** (front hub z +1.4708, rear −1.4118 in GLB space) |
| track | 1.635 m centre-to-centre, wheel radius 0.3305 m, width 0.27 m |
| engine bay | `Engine_MD.FBX` occupies z **+0.79 … +2.22** — under the bonnet, at the front |
| closest real analogue | a Mercedes-AMG GT / C63 Coupe silhouette |

For scale against the incumbent: CTR is 2.274 m wheelbase and 106 k triangles.
Real NSX Type S is 4.54 × 1.94 × 1.21 m on a 2.63 m wheelbase — this car is
longer, taller and longer-nosed. If the owner wants an actual mid-engine
supercar, the answer is still "the library does not have one", not this.

---

## 2. ORIENTATION + ORIGIN (X3_WORLD_RULES rules 2/3/4 — the recorded facing note)

* **Nose = +Z** in the GLB, matching the engine's hero-car contract
  (`app/vehicle.cpp` `kBodySkin` bakes the 180° flip to the engine's −Z
  forward). Verified by node position, not assumed: `Body_F_*` and
  `Head_Lights_*` sit at z ≈ +2.1…+2.3; `Body_Rear_*` and `Rear_Lights_*` at
  z ≈ −2.3…−1.9.
* **+X = the car's LEFT** (`Wheel_FL` is at x +0.82). Same handedness as CTR.
* **Origin ON THE CONTACT PLANE**: lowest vertex y = **+0.001 m**, the tyre
  contact patch (rule 4). Placement Y is the road surface; no fudge offset.
* **Up = +Y**, 1 unit = 1 metre (rule 1).

## 3. NODE / MATERIAL LAYOUT

8 nodes, 21 materials, 243,182 triangles, 13.25 MB.

| node | tris | what |
|---|---:|---|
| `Body_Shell` | 26,577 | main body kit + carbon floor pan |
| `Body_Panels` | 116,590 | bonnets, boot lids, bumper skins, glass, trims, plates |
| `Body_Lights` | 53,484 | head + tail lamp assemblies (emissive lenses) |
| `Body_Grille` | 9,217 | front intakes (2.30 M tris in the source — see §4) |
| `Wheel_FL/FR/RL/RR` | ~9,330 ea | hub-centred, axle on mesh-local ±X |

**The wheels spin.** `DriveDemo::skin()` matches nodes whose name contains
`Wheel_FL/FR/RL/RR`, **zeroes the node translation** and keeps rotation+scale,
then drives them from the live Jolt wheel poses (steer + spin + suspension).
That is why each wheel's geometry is centred on its own hub and the station
lives only in the node translation. Axle on ±X matches CTR, so
`kWheelAxisFix` stays identity.

**Paint.** `PaintBlack`: baseColor `0.014, 0.014, 0.016` linear, metallic 0.80,
roughness 0.33, `extras["x3Clearcoat"] = {intensity 1.0, roughness 0.04}` (the
clearcoat lobe `ModelLoader.cpp:756` parses), plus `CarFlakes_NM` for real
metallic flake. The owner's BLACK. `DriveDemo::setPaintTint` repaints exactly
the clearcoat materials, so the world-cars tint machinery works on it unchanged.

**Nothing ships flat-tinted** (NO_SLOP rule 3). 12 real pack textures are
embedded (5.5 MB): carbon-fibre BC/NM/mask on the floor pan, `Rubber_BC`+`_NM`
on the tyres, `Tire_Side_D` on the sidewalls, and brushed-aluminium /
scratched-chrome / scratched-metal / plastic-dirt HDRP **MaskMaps** converted to
glTF MR — `R=AO, G=1−smoothness, B=metal`, the `tools/tex_curate.py` "rma"
conversion (ENGINE_GOTCHAS 3.6: never inject a packed mask raw).

**Glass is opaque smoked mirror** (baseColor 0.010 linear, metal 1.0, rough
0.045), the same call `tools/convert_car_glb.py` made for CTR: this LOD0 ships
no cabin, and an alpha-blended pane would show the inside of the far bodywork.
The pack DOES ship a full interior (`Interior_MD/01/02/03`, `Seat_MD`,
`Door_Interior`, leather/suede/fabric textures) — that is the input for a
cockpit or dealership pass, not for a car seen from outside.

## 4. WHY THE BUILDER DECIMATES PER GROUP

The source exterior is **3.67 M triangles**, and `Body_F_Bumper_Grill_02_M`
alone is **2.12 M** because the artist modelled every wire of the grille mesh.
One global simplify ratio either shreds the reflective bonnet (where every
collapsed edge shows as a crease under clearcoat) or leaves half a million
triangles of grille nobody can resolve at 3 m. So each group carries its own
budget through meshoptimizer (`npx @gltf-transform/cli simplify`):

| group | source | shipped | ratio / error |
|---|---:|---:|---|
| grille | 2,304,346 | 9,217 | 0.004 / 0.02 |
| shell | 531,680 | 26,577 | 0.05 / 0.004 |
| panels | 832,943 | 116,590 | 0.14 / 0.006 |
| lights | 534,876 | 53,484 | 0.10 / 0.006 |
| wheel (each) | 77,830 | ~9,330 | 0.09 / 0.008 |

## 5. TWO TRAPS THE PACK SETS

1. **One material for 27 meshes.** The FBX assigns `HDRP_CheckBox_UV` to
   everything, and the `.mat` files are Shader Graph with hashed property names
   (`Texture2D_4D3C9E50`) and GUID-only texture refs — and the pack ships **no
   `.meta`**, so there is no GUID→file map to resolve. Materials are therefore
   assigned **by node name**, the ScansFactory / Mega-pack precedent. The node
   names are excellent (`Body_Window_Rubber_01_M`, `A_Wheels_FL_04_Tire`), so
   the rule table in the builder reads like a paint sheet.
2. **Only the LEFT wheels exist.** Unity mirrored them in the prefab. The right
   pair is generated by negating X in the **vertex data** and reversing triangle
   winding — *not* by a negative node scale, which flips winding at draw time
   and backface-culls both wheels.

## 6. EYES-ON RECEIPTS

`docs/screenshots/vehicles/gbx_coupe/` — rendered before any integration work
and again after every material change:

* `00_source_raw_4views.png` — the raw 3.67 M-tri exterior, four angles. This is
  the render that established "front-engine GT coupe, not an NSX".
* `01_built_front_quarter.png`, `02_built_rear_quarter.png`,
  `03_built_side.png` — `tools/glb_contact_sheet.py` on the shipped GLB, real
  baseColor textures sampled per pixel.
* `04_built_ortho_side_front.png` — ortho side + front: wheels seated in the
  arches, contact plane at y=0, the mirrored right pair correct.

The **satin-black rocker** (`ChromeSatinBlack`) exists because of this gate: at
the pack's stock chrome value the first contact sheet showed a white ledge
running the length of a black car.

## 7. SELECTING IT

`--car gbx` (the default) / `--car ctr` for the incumbent. Honoured by
`--world drive`, `--world tunnel` (also first in the garage fleet row) and
`--screenshot-car`. An unknown id is rejected loudly, never silently swapped.

**The simulation is NOT retuned for it.** `app/car_roster.h` carries the
geometry (stations, box, mass, skin widen/drop); the grip curves, CoM offset,
anti-roll rates and torque map in `app/vehicle.cpp` are still the CTR numbers,
measured against the skidpad/slalom receipts in that file. The GBX is 500 kg
heavier and has a 0.6 m longer wheelbase, so it will feel like a CTR that
understeers into slow corners until someone re-runs those measurements. Say so
rather than letting it be discovered.
