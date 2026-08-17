# Small Mountain Town — asset scouting report
*W-TOWNSCOUT, 2026-08-17. Every verdict below came from LOOKING at the asset
(thumbnail or decompressed geometry), not from its name — which is the whole
point, since three of the most promising-sounding packs turned out to be
unusable. Query the full library with
`python tools/unitypackage_index.py --search <terms>`.*

## The verdicts

### USE — storefronts and shops
**`Complete Racing Game URP All in One`** (extracted, `\\p13700\G\Assets\...\Racing_Game\Models\Level_Design\Models\Buildings` + `Buildings _Night`)
`Medium_Building_2/3/4/7/10/11/16/18`, `Building_1..7`, `Parking_2`, `Tower_1/2/5`.
Real photographic facade/signage atlases ship alongside in `map/` — `Shop.jpg`
opens as a lit glass storefront with mannequins and signage. Authored for a
DRIVING game (8 `Race_Track_*.unity` scenes + `Garage.unity`), so the register
matches a freeway world. Needs the standard `tools/convert_unity_pack.py`
FBX→GLB+texture pass. Caveat: some Asian-language signage — swap signs for a
US-freeway read. **This is the best register match found.**

### USE WITH CARE — secondary residential massing
**Armory `Modular Houses`** (`SM_HouseBuilding_001_a/002_a/003_a/003_b`, `SM_House_Prefab_05`)
Clean multi-storey houses, pitched roofs, real window cutouts; bbox ≈14×15×8 m.
**Draco: yes. Textured: NO (`images: 0`).**
**Armory `House On A Hill`** (`SM_House.glb`) — clean, chimneys + dormers,
≈15.6×17.4×9.3 m. Draco: yes. Textured: NO.
⚠ NO_SLOP rule 3 (no untextured stand-ins ship): these need a material/paint
pass or a texture harvest before they can appear in a shipped frame. Do not
drop them in flat-tinted.

### DRESSING ONLY
**Armory `SundownShopStore`** (682 pieces) — a convenience-store INTERIOR
fitout (shelving, product boxes, signage decals). No exterior shell. Pair with
a storefront from the racing pack for window dressing and signage.

### REJECTED, with reasons
- **HouseForge `PF_MetalHouse01/02`, `PF_WoodenHouse01`, `PF_StoneHouse01`,
  `PF_PrimitiveHouse01`** — the geometry is authored as COLLAPSED RUINS:
  jagged holes in walls, scattered debris. Not a style mismatch, broken by
  design. **This pack is already wired into the project** (`prefab_buildings_x3native`)
  — anywhere it stands in for a house, the house is a wreck.
- **`Mega Open World City Pack (Mobile-Optimized for Driving Simulation Games)`**
  — despite the perfect name, `Buildings01/02`, `Shops01/02`, `WorkShop`,
  `School`, `ShoppingMall` render as flat grey blockouts / thin ground-plane
  silhouettes. Placeholder lot markers, not buildings.
- **`Urban Night City - Open World`** — genuinely clean, well-textured glass
  office towers, but ≈61×43×79 m: high-rises. Distant skyline only, never Main
  Street.
- **`Stronghold Village`** — medieval/castle register (`ChurchButressDeco`,
  `ChurchPinaccle`); manifest mistags it "Timeless/Generic". Thumbnails render
  near-blank (the armory's own thumbnailer chokes on these), so no confident
  visual verdict. Last resort only, after a proper decompressed render.
- **`Modular Wooden Buildings`, `Maplewood Village`** — true modular kits
  (separate wall/roof/door pieces). A nice rustic path, but the placement lane
  must hand-assemble; not a drop-in.

## The engineering finding that applies to EVERY armory asset
Every GLB checked — across four unrelated packs (Modular Houses, House On A
Hill, Stronghold Village, Urban Night City) — carries
`extensionsRequired: ["KHR_draco_mesh_compression"]`. This is not a bad asset,
it is a blanket property of the armory's conversion pipeline. **The engine's
loader silently drops draco geometry: the model renders INVISIBLE while
logging as loaded** (receipt: commit 7158cc5e, the invisible bench). Decompress
every armory GLB before use:

```
npx @gltf-transform/cli copy in.glb out.glb     # verified on 5 samples, seconds each
```

## Recommendation
Build the town from a combination: **racing-pack buildings** for shops and
storefronts (the only source with real modern signage and window textures),
**armory `Modular Houses` + `House On A Hill`** for varied residential massing
once they are textured, and **SundownShopStore** for signage and shop-window
dressing. Skip HouseForge entirely, skip Mega Open World City's building
meshes, keep Urban Night City for the far skyline. **Open gap:** no usable
civic/church building was confirmed — `Stronghold Church00` deserves an honest
decompressed render before anyone decides.

## Note on agent hardening
The scout treated two mid-task coordinator messages as suspected prompt
injection (it could not verify the claimed cache path from inside its sandbox)
and refused the scope change, while independently verifying the one concrete
on-share path it was given and folding it in on its own merits. That is the
correct instinct and worth preserving: **findings are adopted on evidence, not
on a claim of authority.**
