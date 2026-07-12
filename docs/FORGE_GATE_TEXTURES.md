# FORGE_GATE_TEXTURES — queued SD3.5 gate-PBR forge (ROUND 3 lane 1)

**Status: PREPPED, NOT RUN.** Owner fires this when the 5090 is free (one
generation at a time; never while the game is running).

## Fire it

```
python tools/forge_gate_textures.py --all
# or one at a time:
python tools/forge_gate_textures.py --set gate_ring_plate
python tools/forge_gate_textures.py --set gate_patina_plate
python tools/forge_gate_textures.py --set gate_piston_steel
```

Model: local diffusers SD3.5 at `C:\GameDev\SD_Models\sd35` (the proven
surface-library forge path — NOT the broken :7860 WebUI). Deps: `torch,
diffusers, transformers, accelerate, pillow, numpy`.

Output convention: `G:\Assets\X3Native\surface_library\<set>\` with
`albedo.png` (SD3.5, offset-blend tileable), `normal.png` (Sobel from
luminance-derived height — greebles/rivets baked into relief), `mr.png`
(glTF packing G=roughness B=metallic, cavity-varied roughness), plus
`height.png` (extra, future parallax).

## The three sets

| set | role on the gate | look |
|---|---|---|
| `gate_ring_plate` | the big segmented ring plates (`gate_patina` group) | riveted weathered grey-white armor, teal-oxide streaks, baked panel seams + bolts |
| `gate_patina_plate` | over-plates / base shoulders (`gate_steel` group) | rust streaks bleeding from bolts, chipped teal oxide |
| `gate_piston_steel` | clamps, rods, pipes, bolts (`gate_dark` group) | near-black brushed gunmetal, tooling marks |

## Landing the sets (after the forge)

1. Copy each set dir into the repo: `assets/surface_library/<set>/`
   (`albedo/normal/mr.png` — the loader ignores `height.png`). They ride the
   existing `assets/**/*.png` LFS route.
2. Wire the gate mapping in `app/rifthub.cpp` — the gate-GLB material block
   (search `ROUND 3 gate-GLB material tints`): swap
   `m_surf.get(device, "mw_metal_panels_a")` -> `"gate_ring_plate"` etc. for
   the three `sPlate/sTrim/sDark` gets used by the gate path (or add three
   dedicated gets so the hall keeps its current sets), and lift the
   gate tints toward neutral (`kGatePlateTint` -> ~0.85 grey) since the forged
   albedo carries its own value/patina.
3. Gates: `--test-rifthub` 10/10+, `--smoketest --world rifthub` 0 VUID /
   allocationCount=0, F-series screenshot for the owner's eyeball.

The bpy gate mesh smart-projects UVs at ~2.0-2.6x island scale
(`tools/build_rifthub_gate.py`), so 1024px sets land at a sane texel density
on the 5 m gate. If the plates read too busy, drop the UV scale in the bpy
script and re-export rather than shrinking the texture.
