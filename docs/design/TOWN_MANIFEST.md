# THE SMALL MOUNTAIN TOWN — placement manifest (W-TOWN, Lane 4)

*What stands where, from which pack, and every number that decides it. Written
2026-08-17 alongside `app/town.{h,cpp}`. Companion law: `docs/NO_SLOP.md`,
`docs/design/X3_WORLD_RULES.md`, `.claude/skills/x3native-environments/SKILL.md`.*

---

## 1. THE SITE — why here

`docs/design/ROAD_NETWORK_SKETCH_V2.png` labels a brown **"Small Mountain
Town"** centre-east, hanging off a **yellow ladder-switchback road** between the
Large Mountain and the river.

The network already contains that ladder, and the plan already named the site:

| source | says |
|---|---|
| `app/road_network.h` `registerSummitSpur` | the ONLY switchback climb in the network |
| `app/forest.cpp:384-395` | hard-codes the spur peak `(393, 6752)` as *"the sketch's spiral-road mountain family"* |
| `docs/design/ROAD_NETWORK_PLAN.md:701` | *"Town 2 — the climb foot, where the inner tour meets the summit road."* |

So the town is **laid along the summit spur's lowest, gentlest reach** and is
handed the spur's registered `RoadSpec` + graded datum by the host — it is
never given hard-coded coordinates, so it follows the spur wherever the
hill-climb puts it.

**As built (boot log, 2026-08-17):**

```
summit spur: 1.44 miles UP - climbs 247 ft to a 349 ft peak at (393, 6752),
             junction at (-101, 4439) | max grade 14.0% | 53 nodes
town: 17 buildings, 23 props, 10 parked cars, 34 lit windows, 10 lamps
      along 690 m of main street (u 70..760);
      centre (-20.6, 15.7, 4817.5); 3 lots rejected (slope/overlap)
town: 6 pedestrians on a 52-node sidewalk loop
```

**TOWN CENTRE / MapPoi anchor: `x = -20.6, y = 15.7, z = 4817.5`.**
Main street runs from about `(-95, 4460)` to `(60, 5130)`.

---

## 2. THE PACK — and the trap in it

Everything but the vehicles and the two benches comes from **HouseForge**
(`D:/Assets/_glb/prefab_buildings/HouseForge`, catalogued in
`D:/Assets/_glb/prefab_buildings/PREFAB_MANIFEST.md`) — whole, curated,
PBR-textured *buildings*, not modular wall panels. The armory was the first
stop, per the owner's tip; only what the armory lacked would have been mined
from the Unity packs, and it lacked nothing structural.

**The trap, and the receipt.** Those armory GLBs are baked *"WebP+Draco"*.

* `KHR_draco_mesh_compression` — the engine **does** decode this
  (`engine/asset/ModelLoader.cpp:46,801,1033` link and call draco).
* `EXT_texture_webp` — **there is no WebP decoder anywhere in the tree.**
  Every one of these prefabs would have loaded with **zero textures** and
  rendered flat grey: NO_SLOP rule 3. Caught by inspecting the GLB JSON, not by
  shipping it.

`tools/town_assets.py convert` therefore decodes draco (`gltf-transform copy`)
and transcodes every image to PNG/JPEG with Pillow — `gltf-transform png` fails
on these files with a libvips `VipsInterpretation` error, which is why the
image pass is hand-rolled. `tools/town_assets.py verify` asserts the shipped
result: **0 files carry an extension or mime the loader cannot read.**

Output: `assets/converted_glb/Town/` (21 GLBs, ~120 MB, textures capped at
1024 px — a 23-material building at 4K is ~1 GB of atlas).

---

## 3. THE ASSET TABLE — measured, per X3_WORLD_RULES rule 0/3/4

Regenerate with `python tools/town_assets.py report`. These are the numbers
`kAssets[]` in `app/town.cpp` carries; **if the kit is re-baked, re-run it and
update both** (NO_SLOP rule 4 — paired values are one value).

| asset | W × H × D (m) | bbox centre (x,z) | loY | **front (engine yaw)** | untextured material |
|---|---|---|---|---|---|
| PF_StoneHouse01_JustBuilding | 18.42 × 15.06 × 16.52 | −5.97, +7.45 | −5.39 | **−84.9°** | Reinforced_Window_R_Arc |
| PF_StoneHouse01_WithSetDressing | 22.10 × 15.09 × 21.10 | −4.93, +8.22 | −5.40 | **−68.7°** | FoodStorage_02a |
| PF_StoneHouse02 | 19.28 × 13.65 × 15.27 | −5.42, −3.94 | −3.82 | **−73.7°** | Reinforced_Window_R_Arc |
| PF_WoodenHouse01_JustBuilding | 17.57 × 9.95 × 13.74 | −5.47, −6.66 | −3.25 | **−124.8°** | *fully textured* |
| PF_WoodenHouse02 | 12.69 × 10.15 × 9.93 | −3.00, −1.90 | −3.33 | **−124.4°** | *fully textured* |
| PF_WoodenHouse03 | 14.01 × 12.69 × 15.76 | −3.78, −4.74 | −3.25 | **−147.5°** | *fully textured* |
| PF_PrimitiveHouse03 | 23.41 × 14.87 × 19.82 | −1.50, +5.06 | −4.77 | **−4.5°** | Primitive_RoofOverhang |
| PF_PrimitiveHouse04 | 15.35 × 9.62 × 12.12 | −3.00, +2.68 | −0.16 | **−141.9°** | Primitive_RoofOverhang_V2 |
| PF_StorageMarket_01a / _01c / _01e / _01f | 5.4–7.3 × 2.4–4.1 × 3.1–4.5 | — | ~0 | symmetric | one food-stand mat each |
| PF_WoodCart_01a / _02a | ~5.0 × 1.6–2.0 × 3.5–4.3 | — | ~−0.9 | symmetric | one mat each |
| PF_WoodLightTorch_01a / _01b | 2.2 / 1.4 wide, 3.1 / 2.4 tall | — | ~−0.4 | symmetric | wood support |
| PF_WoodStorage_01b / _01c, PF_StockageWood_01a | 2.1–7.8 wide | — | ~−1 | symmetric | one mat each |
| `nature/SM_WoodBench_01a`, `nature/SM_Bench` | ~2.2 × 0.9 × 1.1 | 0, 0 | 0 | symmetric | already in-tree (road_trees lane) |

**Three things this table exists to prevent:**

1. **The prefabs are NOT centred on their origin.** `PF_StoneHouse01` sits 5.97 m
   to −X and 7.45 m to +Z of it. A lot that ignores that puts half the building
   in the road. `Town::build` addresses lots by **bbox centre** and backs the
   offset out at placement.
2. **`loY` is negative on every house** — the kit models a foundation/rock plinth
   *below* the ground-floor origin. That is a feature on a hillside: the plinth
   buries itself into the slope instead of leaving a floating skirt.
3. **The front is measured, never guessed** (rule 0): it is the direction from
   the bbox centre to the farthest named `SM_*_Door*` node. Rule 3 wants
   orientation documented per asset; the column above is that document.

`PF_PrimitiveHouse01/02` are **deliberately excluded**: their one untextured
material is a *wall* / roof end, and a 0.8-grey slab facing main street is
NO_SLOP rule 3 in the flesh. `03/04`'s is a roof overhang — small, high, and
patched by the `MaterialOverride` block in `town.cpp`, which gives every one of
the kit's bare-default sub-materials the kit's own weathered timber and stone
constants rather than tinting a whole asset.

---

## 4. THE LAYOUT — curated, not a lattice

`kLots[]` / `kProps[]` / `kParks[]` in `app/town.cpp` are hand-authored tables
keyed on **arc length `u` along the street**, side (±1), lateral setback, and a
per-plot yaw jitter. The tables are written against `u` 70…690 and rescaled onto
whatever reach the host gives, so the town stretches or compresses with the spur
rather than falling off the end of it.

| reach | character | plots |
|---|---|---|
| lower town | loose and rural, deep setbacks (26–33 m) | WoodenHouse02, PrimitiveHouse04, WoodenHouse03, WoodStorage_01c, StoneHouse02 |
| main street | shops crowding the pavement (18.4–20.5 m) | StoneHouse01, WoodenHouse01, market stall C, WoodenHouse02, stall A, StoneHouse02 |
| **the square** (u≈300) | the hero set back at 27 m with the market spread in front | StoneHouse01_WithSetDressing + stalls E/F + two carts + WoodStorage_01b |
| upper street | climbing, tightening | WoodenHouse03, StoneHouse01, PrimitiveHouse03, stall C, WoodenHouse01, WoodenHouse02, PrimitiveHouse04, StoneHouse02 |
| outskirts | thinning toward the switchbacks (24–32 m) | WoodenHouse03, WoodenHouse01, WoodStorage_01c, WoodenHouse02 |

Curation rules the tables encode by hand: **varied massing** (nothing repeats
twice in a row), **varied setback** (shops 19–21 m, houses 26–34 m, stalls
18.5 m), **varied gaps** (26–62 m, wide at the ends, tight through the middle),
**broken alignment** (every plot carries its own yaw jitter, so no two facades
are parallel).

**Street furniture** (`kProps[]`): 10 torch lamps alternating sides at 18 m, 4
armory benches, 2 crate stacks. **Parked cars** (`kParks[]`): 10 of the
converted fleet (`Vehicles/E30, Pickup, Jeep, Coupe, Muscle, Truck, M3_E36,
E46_New`), angle-parked nose-in at 17.9 m with ±58–63° skew off the street
tangent.

---

## 5. THE LAWS THE PLACER ENFORCES

**KEEP-OUT (the road is never built on).** `kStreetKeepOutM = 17.5 m` from the
centreline. The spur is a base-profile road: `kPavedHalfM` 14.63 m of pavement +
shoulder + apron, and `RoadSpec::halfWidth` carves a metre past that. 17.5 m
clears the carve edge and leaves the verge for the sidewalk. Nothing — building,
prop, cart, car — is ever placed inside it. The junction mouth at the spur's
foot is a second keep-out disc of `kJunctionSetbackM`.

**THE TOWNSITE PLANE.** A main street is *graded*. Each plot's ground is
`clamp(min-of-5-terrain-samples, datum − 1.6 m, datum + 1.2 m)`. Both bounds
were paid for:

* *Upper bound* — corridors only ever CUT (`road_network.h`), so beside a
  climbing spur the natural ground sits metres **above** the datum. Round one
  grounded on raw terrain and the whole town vanished behind its own bank; only
  roof spikes showed above the grass.
* *Lower bound* — round two clamped only the upper side and the square's hero
  landed in a **9 m hollow** below its own street.

**CONTACT (rule 4).** Five terrain samples per plot — four rotated footprint
corners plus the centre. The plot sits on the **lowest**, so nothing floats; a
plot whose corners disagree by more than 4.5 m is **rejected** (3 were). The
origin is then lifted by `−loY` so the modelled plinth rests on the ground, and
sunk 0.5 m so a slope shows no gap under the sill.

**COLLISION.** One yaw-rotated static box per building (`addBox` + `setBodyRotation`,
the `app/world_cars.cpp` precedent), footprint inset 0.35 m, spanning the visible
body only. Anything with a half-footprint under 1.6 m — carts, torches, benches,
crates — gets **no** collider: an invisible wall on a sidewalk is worse than
driving through a barrel. Parked cars get the same 0.85 × 0.62 × 1.95 box
`world_cars.cpp` uses.

**NIGHT (rule 5).** Two lit panes per house, recessed 6 cm into the measured
facade (proud of the wall, a pane that misses its wall floats in mid-air;
recessed, a miss is invisible). Flat emissive **0.42** over a near-black albedo —
the ACES law says anything past ~0.5 clips to a white slab — with a warm
practical behind each pane and one at every torch head. `Town::setNight(k)`
rebuilds each light from its **authored** colour, never from its current one, so
repeated calls cannot compound.

---

## 6. THE LIVES

Six pedestrians on a 52-node closed sidewalk loop (up the −side verge at
`kSidewalkLatM` 16.4 m, back down the +side). Each is an **`AnimatedCharacter`**
(`app/character_anim.h`, the one character-rig runtime) driven by its own
`Player` capsule, cycling the **crowd_skin roster**: `AnnaCasual_anim.glb`,
`marcus_webb_anim.glb`, `chief_martinez_anim.glb`.

**`townPedClipTable()` exists because labels are untrusted.** The roster rigs
carry `Idle / Walk / Run / Jump`; `jakeClipTable()`'s names are `Walking` /
`Running`. `AnimatedCharacter` resolves by EXACT name, so the wrong table yields
a sliding idle-only statue. Clip names were read out of the GLBs.

**THE CONTACT LAW** is enforced inside `AnimatedCharacter::update` — feet clamp
to `max(terrain height, downward static raycast)` every frame; no host can ship
a buried walker. The town's part of the bargain is the **`kPedActiveM` = 320 m
gate**: beyond it the terrain tiles the walkers stand on are not resident, and
ticking them is a free-fall, a raycast and a log line each, every frame, for
something nobody can see. The gate lives inside `Town::update`, and the capture
settle loop calls it too — an unticked `AnimatedCharacter` is a bind-pose statue,
which is the T-pose defect NO_SLOP rule 1 catalogues.

---

## 7. THE GATE

`X3Engine.exe --screenshot-town <dir>` — five stills, cameras **derived from the
town's own placement data** (`Town::showcaseCamera`), never typed in, because
ENGINE_GOTCHAS 4.1 is explicit that hand-picked cameras end up inside walls:

| # | shot |
|---|---|
| 01 | main street from the road, at driver eye height |
| 02 | the square's hero facade, close enough to judge the textures |
| 03 | the pedestrians, eye height on the sidewalk |
| 04 | the lit windows at dusk (the sun is dropped to the horizon for this frame only) |
| 05 | the town from across the valley |

---

## 8. OPEN — the art call, stated plainly

**The HouseForge house prefabs read as derelict, not as a town.** Eyes-on at
full res (`docs/screenshots/town/02_shop_front.png`) shows dark, spiky, broken
silhouettes; the armory's **own** thumbnail bakes render them the same way, so
this is the asset, not the pipeline. The geometry, textures, scale, orientation
and grounding are all correct — the *kit* is a ruined-settlement kit.

The placement machinery is asset-agnostic: swapping the set is an edit to
`kAssets[]` + `kLots[]` and a re-run of `tools/town_assets.py report`. The
strongest candidate found while hunting is **Medieval Lakeside Town**
(`SM_House_One…Six`, `SM_Shop_01…04` — clean, whole, well-formed houses and
shops), but its armory GLBs are **geometry-only: 0 materials with a base-colour
texture, 0 images**. Using them means the full skill pipeline —
`tools/convert_unity_pack.py` against the source Unity pack, resolving texture
GUIDs from the `.mat`/`.meta` files — which is the correct next pass and was out
of this lane's remaining budget. Shipping them untextured would be NO_SLOP
rule 3.

---

## 9. MapPoi

Lane 7 (W-MAP) had not landed a `MapPoi` header at this lane's close
(`grep -rn MapPoi` finds nothing outside `docs/plans/SEVEN_LANE_PLAN.md:122`).
When it lands, register:

```
MapPoi{ "Mountain Town", x = -20.6f, z = 4817.5f, icon = town }
```

The live value is `Town::centerX()` / `centerZ()` / `centerY()`, so the
registration should read those rather than copy the literals — the spur is
hill-climbed at boot and the centre moves with it.
