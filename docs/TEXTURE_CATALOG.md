# TEXTURE CATALOG — The Grand Survey (2026-07-01)

The shopping list for every level texture pass. Swept **D:\Assets completely (212 entries, ~198 GB)**
plus `\\p13700\G\Assets` (verified: identical pack set — no G:-only extras; D: is authoritative).
Probed every non-audio pack with a PBR map/resolution scanner (map-type counts + PNG/TGA/JPG header
dimensions). `X3AssetStore` is our content-addressed object store, not a pack — excluded.

**Conventions.** Nearly every photoreal pack is HDRP-authored (Leartes / Hivemind / ScansFactory /
NatureManufacture) using the **HDRP MaskMap** (R=metal, G=AO, B=detail, A=smoothness) — exactly what
`tools/convert_unity_pack.py` already repacks into glTF ORM. "B/N/ORM" = complete PBR set.
**Tileable surface sets are the gold** — they texture walls/floors/ceilings/rock; prop atlases only
dress objects.

---

## Tier 1 — the A-list per category (use these first)

### Sci-fi / facility walls · floors · ceilings · trim (canonical facility interior)
| Pack | Peak res | Tex | PBR | Why |
|---|---|---|---|---|
| **Office Corridor Environment** | 4K (113 of 116 maps are 4K!) | 116 | B/N/ORM | Leartes; clean interior corridor walls/floors/ceilings — THE facility-interior pick |
| **Subway Station Environment** | 4K(34)/2K | 139 | B/N/ORM | explicit tileable `ConcreteTiling`/`ConcreteWall` + grime decals — underground facility concrete |
| **Modular Shooting Range (Military Facility)** | 4K/2K | 149 | B/N/Mask/Emis | military facility walls/floors — security/armory character |
| **Command Center** | 4K(38)/2K(117) | 344 | B/N/ORM (EXR ORM) | tileable Porous_Cement_Wall, Concrete, Tilemetal, concrete trim — sci-fi ops rooms |
| **Cyberpunk City …Sci-Fi City (Hivemind)** | 2K | 171 | B/N/Mask/Emis | dedicated `Textures/Tileable/` Concrete, Concrete_01, Fabric, Metals folders |
| **3D Scifi Kit Starter Kit** | 2K | 120 | Alb/N/AO/Metal (separate) | tileable scifi Floor_01/02 + Wall_01/02 panels; clean but slightly game-y |
| **Space Station Free** | 4K (all 47) | 47 | B/N/Metal/Emis | small but all-4K hull/panel metal |
| Modular Warehouse | 2K | 174 | B/N/Mask/Emis/AO | warehouse walls/floors + props |
| Sci-Fi Construction Kit Modular | 2K(66)/4K(16) | 85 | Alb/N/Spec/Rough (legacy specular!) | some tileable ceiling/wall; needs spec→metal conversion |
| Scifi Modular Interior Space Station | 4K(38)/1K | 157 | B/N sparse | modular panels, incomplete PBR — B-list |

### Concrete · industrial · metal (facility + service zones)
| Pack | Peak res | Tex | PBR | Why |
|---|---|---|---|---|
| **Warehouse - Abandoned Factory District** | 4K (bulk 1K/2K) | 1664 | B/N/M/R tiling sets | ScansFactory photogrammetry; tileable brick/cables + huge prop set — top-tier realism |
| **Urban Abandoned District** | 4K (bulk 1K/2K) | 1065 | B/N/H/M tiling | ScansFactory scans; tileable brick/cobble/concrete/ground |
| **Abandoned Factory (…Industrial Warehouse)** | 4K, mostly 2K | 115 | B/N/ORM | full tileable set: Bricks, Concrete, CorrugatedMetalSheet, FloorTiles Dirt/Off/Rubble |
| **Industrial Building Materials Pack** | 4K(14)/2K(41) | 58 | B/N/AO/Metal/Mask | purpose-built tileable building-material surfaces |
| Industrial Fabric Pack | 4K | 25 | B/N/AO/Metal/Mask | 5 tileable 4K fabrics (tarps, drapes) |
| Modular Sewers Tunnels | 1K (+4K) | 136 | B/N/Mask/Metal | sewer/tunnel/corridor surfaces + decals (1K only) |
| Post Apocalyptic Town (Hivemind) | 4K(20)/1K | 146 | B/N/ORM + TrimSheet | trimsheets + tileable metal/concrete + landscape material |
| Modular Destroyed Buildings | 2K | 164 | B/N/Mask/Metal | tileable ruined concrete/brick/doors/windows |
| Industrial Equipment Pack | 4K(107) | 119 | B/N/AO/Metal/Mask/Emis | 4K machinery panels (semi-tileable) |
| South American Slums - Favela | 2K(220) | 690 | B/N/ORM | tileable slum walls/corrugated metal + **223-decal weathering/graffiti library — top decal source** |

### Rock / geology — THE STRATA
| Pack | Peak res | Tex | PBR | Why |
|---|---|---|---|---|
| **Rocky Hills Environment - Whitebark Pine** | 4K(51)/2K | 308 | Alb/N/Height/MAG | tileable Cliff/Grass/Dirt WITH height — real rock strata bands |
| **Shatter Stone Stones Rocks** | 2K(26) | 44 | B/N/ORM + tileable Pattern | Granite/Layers/Soft tileable strata patterns + 6 stone sets — layered-strata gold |
| **Modular Cave (Ancient/Mayan Cavern)** | 1K | 232 | B/N/Metal/Mask/Height | cavern/rock strata + ruins (all packed 1K — resolution gap) |
| Shatter Stone Minerals Crystals | 2K(32) | 50 | B/N/Metal + emissive | mineral/crystal ore surfaces, glow variants — strata accent veins |
| Shatter Stone Metal Ores | 2K(19) | 37 | B/N + Metal | metal-ore veined rock |
| Shatter Stone Fossils | 2K(19) | 43 | B/N/Metal | fossil-embedded stone — one-off strata bands |
| Crystals and Crafting | 1K(336) | 345 | B/N/ORM | many CrystalRock/ArmourStones strata variants (1K) |
| Time Ghost Environment | 4K(121!) | 242 | Alb/Mask/OSN | Unity film demo; scanned Kullaberg rocks are reference-grade BUT use per-mesh object-space normals (not tileable) |
| Ancient Desert Town | 2K(+36×4K) | 557 | B/N/AO/H/Metal | desert stone/stucco strata + Essential Terrain Pack |
| Stronghold Village `_DLNK Texture Library` | 4K(110)/2K | 1398 | B/N/AO/H/Metal | standalone tileable Brick/Wood/Floor/Stone library + terrain rock detail maps |

### Club / luxury interior (Club 1127)
| Pack | Peak res | Tex | PBR | Why |
|---|---|---|---|---|
| **Gothic Interior Megapack** | 4K(102) | 204 | B/N/Mask/Rough | trim sheets, rich stone/wood, stained glass — luxury texture DNA |
| **Miami Vice City (Leartes)** | 4K(104) | 181 | B/N/Mask/Metal/Emis | tileable Asphalt/WallTile + neon-emissive facades — club-district vibe |
| **Rinos Diner Environment** | 2K(107) | 139 | B/N + heavy Metal | chrome/enamel/tile — polished metal + bar surfaces |
| Student Apartment Interior | 2K(151) | 176 | B/N/ORM | modern interior walls/floors/furniture |
| Modern Office Environment | 2K | 164 | B/N/Mask/Emis | clean modern interior surfaces |
| SundownShopStore | 4K(58)/2K(116) | 232 | B/N/ORM | tileable concrete + ceiling/floor tiles, retail interior |
| Wills Room Environment | 4K, bulk 1K/2K | 356 | B/N/ORM | dorm interior; mostly prop atlases, some tileable sheets |
| English Cottage | 2K | 182 | B/N/ORM | tileable hardwood_floor, WallTexture, Wood_Floor |
| POLYGON Nightclubs (Synty) | 4K atlases | 176 | flat albedo + emissive | **STYLIZED — layout/geometry reference ONLY, never photoreal texturing** |
| SciFi Neon Buildings | 1K | 167 | emissive atlases | neon/emissive reference; low tileability |

### Exterior terrain / ground (landing scene)
| Pack | Peak res | Tex | PBR | Why |
|---|---|---|---|---|
| **Landscape Ground Pack 3 (NatureManufacture)** | 4K | 198 | B/N/AO/Metal/H/Mask | tileable ground/sand/coast + terrain stamps + decals — THE terrain pick |
| **Environment Vegetation - Bundle** | 4K(103), bulk 1K/2K | 1058 | B/N/R (+ORM) | huge; tileable DryGround, GrassGreen, GroundRock, ForestFloor |
| Rocky Hills Environment | 4K(51) | 308 | Alb/N/H/MAG | cliff + ground tiles w/ height (double-dips with strata) |
| Nature Package - Forest Environment | 4K(22)/2K(76) | 121 | B/N/mask | 58 tileable ground textures (grass/mud/dirt/leaves) |
| Lake Coniferous - Environment | 4K | 535 | B/N/R/Metal/AO | ground/grass tileables + vegetation atlases |
| 1900s Industrial Environment | 2K(+21×4K) | 133 | B/N/ORM | tileable Road_01-03, Grass, Stones — good for pads/roads |
| The Medieval Castle | 2K(183) | 211 | N/AO heavy | Terrain_Textures tileables |
| Alien Planet Creator V2 | 4K | 7 | partial | 6 alien planet/terrain maps — niche exotic ground |

### Urban / street (Spire surroundings, city views)
Leartes street set, all photoreal B/N/ORM: **French Quarter Street** (4K, 383 tex, fullest PBR in
the library), **Italian Alley** (4K/2K, 206), **Roman Street** (2K, 296 — clean stone/plaster/paving),
**1970s NYC Alley** (2K, 313), **Chinese Alley** (2K, 296), Cyberpunk City Recife (2K/1K, 429 —
concrete/sidewalk + damage decals), Miami Vice City (above).

### Props (dressing — already partially converted in assets/converted_glb)
Uniform-vendor 4K B/N/AO/Metal/Mask per-object sets: Industrial Silos/Ladders/Fireset/Workbench/
Scaffolds/Shipping-Container/Racks/Fuse-Box/Lockers packs, 12 Industrial Props, Abandoned Props (56×4K),
Construction Props MegaPack, 10 Hand Tools (2K), 108 Cylinders, Road Props (4K), Toilet Props Set (5×4K),
Trailer with Chassis (4K), Rigged Construction Truck (4K, 31). Hero vehicles/ships: USS Virginia,
G6 Battleship, SPARROW, Star Sparrow, Hurricane 07f, Rikka IFO.

---

## Tier 2 — themed / situational
- **Medieval/fantasy surface libraries** (photoreal, reusable stone/wood/plaster): Fantasy Castle (2K),
  Modular Gothic Cathedral (4K×104), Modular Dungeon (2K trims), Medieval Kingdom (1K), Modular Rural
  Town (1K), Modular Medieval Fantasy Village (4K/1K), Modular Viking Village (2K TT_CliffRock/
  CobbleWall/DirtTracks), House Forge (2K TT_CliffRock/Grass + metal/stone libs), The Aftermath (4K/2K),
  Witch Village (2K trim sheets + decals), Sorcerers Hut (4K×61), Pirate Tavern (2K wood), Bandit
  Valley Village (1K EXR mud/stone walls), Asian Fishing Village (4K/2K concrete/fabric/mossy rock),
  Spanish Cottage (1K stucco/terracotta), Fire Watch Tower (1K complete sets), Stronghold Village (above).
- **Interior misc**: Modular Abandoned Hospital (1K, B/N only — thin), Bowling Alley (ceiling tiles only),
  Modular Haunted House (2K wood/metal), Modular Wooden Buildings (2K).
- **Characters (not surfaces)**: Time Ghost Character (film-grade 4K), Gamer Girl (4K×147), Succubus
  Sisters (4K×196), Beach Bundle (4K×411), Cyberpunk Ninja Girl Vex, SCI FI Shooter Vol 3, ELEMENTAL
  DRAGONS, Fantasy Monsters, CombatGirls, stylized character packs.
- **VFX/water/tools**: Flipbook VFX Bundle (fire/dust/electric sheets), Oceanis (caustics/water normals),
  UNI VFX 6D, Ultimate Loot, Zap VFX, Spells Pack, Snow effects, Particle Dissolve, FLOW, Path Painter II.
- **Stylized (NOT for photoreal passes)**: POLYGON Nightclubs, Toon City, ToonScapes, Low Poly Dungeons/
  Nature, FANTASTIC Interior/Seaside, Stylized Casual/Fantasy characters, Road System (low-poly),
  Stylized Nature/OniValley (semi-real terrain, usable in a pinch).

## Not texture sources (verified)
- **Audio despite promising names**: Industrial Sci-fi Vol I & II (.wav ambience!), Sci-fi Evolution
  Gift Pack (.wav), SCI-FI GUNS GAME OF WEAPONS (weapon SFX), Fantasy SFX "Texture Effect Library",
  Deep In Space, Pursuit of the Death (.mp3) + ~20 obvious SFX/music packs.
- **Code/tools**: First Person Controller Pro, FS Swimming, Motion Matching ×2, Realistic Car Controller,
  Racing Game Creator 2, Synaptic AI, Custom Inspector, Heat UI, Survival Engine (sprites), MegaBook 2.
- **Empty `_Layouts` stubs (content NOT extracted — assets live in other packs or not downloaded)**:
  City Street Environments Bundle 8 Packs, Environment Ultimate Bundle Summer Edition, Modern Interior
  Environments Bundle, Modular Village Nature Environments Bundle 10 Packs, Industrial Props Equipment
  Mega Bundle, Sci-Fi Effects, Post Apocalyptic Ultimate Bundle, Shatter Stone Bundle (meta; sub-packs
  present individually).

---

## GAPS — for Tim's undownloaded-packs shopping list
What the library CANNOT deliver today at movie-set quality; the not-yet-downloaded Unity-store packs
should be checked against this list (download to **D:\Assets**):

1. **Detention-cell / prison materials** — no dedicated prison pack (padded cell, cell-block steel,
   institutional paint-over-block). Closest: Shooting Range + Subway concrete + Hospital (1K-thin).
2. **True nightclub-luxury surfaces** — POLYGON Nightclubs is stylized; we have NO photoreal polished
   black marble / lacquer / velvet / brushed-brass / backlit-onyx bar-top sets. Club 1127 will be
   assembled from Gothic Interior + Rinos Diner chrome + Miami neon; a dedicated luxury-interior
   (marble/velvet/brass) pack is the single biggest gap.
3. **4K layered sedimentary strata** — Shatter Stone patterns are 2K, Cave pack is 1K, Time Ghost rocks
   aren't tileable. A 4K tileable cliff/strata/sediment set (or Quixel-style scans) would lift the ride.
4. **Clean high-tech sci-fi trim sheets** (Star-Citizen-style greeble/trim + emissive strips) — current
   sci-fi kits are semi-real or 2K; no dedicated 4K sci-fi trim-sheet pack.
5. **Glass** — no dedicated architectural-glass/dirty-glass/frosted set anywhere (engine-side shader
   work + a glass decal/smudge pack needed).
6. **The empty `_Layouts` bundle stubs above** — those 7 bundles were bought but their content was never
   extracted/downloaded; re-download would add 8 street environments + summer nature + interior bundles.
7. **Terrain macro-variation** — no 8K/4K macro splat/flow maps for large-scale exterior ground blending.

## Pipeline notes
- Converter: `tools/convert_unity_pack.py` (FBX2glTF at `D:\GameDev\tools\FBX2glTF.exe` on this box —
  script default points at C:\GameDev\tools; pass/patch accordingly). HDRP MaskMap→ORM handled,
  `_BaseColor` tint → baseColorFactor handled, atlas cap MAX_TEX=1024 for per-mesh GLB (dedup path
  keeps 2K+).
- Big binaries: `tools/asset_store.py publish` → `D:\Assets\X3AssetStore` (primary) — commit manifest only.
- Sibling packs on `\\p13700\G\Assets` are byte-identical set; always read from D:.
