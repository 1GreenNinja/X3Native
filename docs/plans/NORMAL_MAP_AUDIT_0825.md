# NORMAL-MAP AUDIT — Phase 0.4 (2026-08-25)

Scan of all 152 converted GLBs (`assets/converted_glb`). Loader-side decode
failures already fall back to a default flat normal WITH a warning, and every
load now prints a `nrm=X/Y` bind receipt — so the remaining gap is CONTENT:
**85 GLBs author zero normal maps** and shade flat under any light.

## PROGRESS
- 2026-08-27 batch 1: Town kit DONE (albedo-derived + Light_2 real normal;
  town_assets.py). 2026-08-27 batch 2: SciFiKit3 DONE — 25 GLBs wired with the
  pack's REAL normal atlases (tools/kit_normal_wire.py). VERDICTS from batch 2
  recon: the racing-pack CARS author no normals anywhere and smooth paint is
  CORRECTLY flat — derived bumps would emboss decals into grooves; only tire
  tread would benefit (small, deferred). Vol2/Vol3/Warehouse kits use per-mesh
  material names over shared atlases with no recoverable name link — needs an
  ATLAS-FINGERPRINT pass (match each GLB's embedded diffuse bytes against the
  source atlas diffuses, then wire the sibling _Norm) — batch 3.

## Priority targets (player-facing, hit by the mr-factors sweep too)
- Town/House_1..4 _Red/_White (all 8 town houses — Main Street shades flat)
- Town/Billboard_1, Billboard_2, Light_2, Wood_Fence
- Vehicles/CTR (19 mats), Muscle (18), Skyline_by_BUMSTRUM
- Vehicles/Traffic: Sedan_Car3, Sedan_Car4, Pickup2_URP, LightBar
- GasStation (1)

## Bulk (kit pieces — batch-bake candidates)
- SciFiKit3 (26), ScifiKitVol3Decoded (22), SciFi_Warehouse_Kit (11),
  ScifiKitVol2Decoded (4), rifthub (2)

## How to fix (content lane)
Source Unity packs usually SHIP normal maps the converter didn't wire — check
`tools/unitypackage_extract.py` output for `*_Normal/_N/_nrm` textures beside
each albedo before authoring new ones. Wiring an existing map into the glTF is
a converter fix; baking new ones (AI upscale box has the tooling) is last
resort. Re-run `nmscan.py`-style scan after each batch; the boot `nrm=X/Y`
lines are the in-engine receipt.
