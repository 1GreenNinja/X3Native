# Island regen — the fjord-harbor terrain (Tim's two-city canon)

The lost `assets/island_mesa` bake (GLB + 16-bit heightfield PNG) is REGENERATED
in-repo by `tools/echo_terrain_gen.py` — one deterministic script, one height
array, both artifacts, so mesh and heightfield can never drift and the asset can
never be "lost" again. The landform is Tim's canon (2026-08-03): a winding
cliff-walled ocean inlet (Chelan-gorge landform) into a harbor basin; ECHO
HARBOR (lower city) on the curving shore shelf under a ~190 m wall; ECHOTROPOLIS
(upper city, the "crown") on the plateau above; the freeway ring climbing
between them on a graded promontory, with a ravine viaduct on the NE descent.

Headless captures, all `--world echotropolis --screenshot <png> --shot-cam
x,y,z,yaw,pitch` with `ECHO_CITY_PROXY=1 ECHO_TOD=noon`:

| shot | `--shot-cam` | what it shows |
|---|---|---|
| `aerial_full.png` | `-300,1650,300,0.0,-1.5707` | the whole landform: gorge serpentine, basin, bluff, city, ring (hazy — engine aerial fog is tuned for low orbits) |
| `aerial_city.png` | `150,950,560,0.0,-1.5707` | waterfront blocks + boulevard + glass row + the gate trumpet + the freeway climb (screen up = +x east, right = +z north) |
| `harbor_wall_from_sea.png` | `60,45,140,1.5708,0.10` | THE canon frame: the 190 m harbor wall with crown towers on the lip and the waterfront row at its foot |
| `gorge_inlet.png` | `-1750,160,-880,0.35,-0.06` | the cliff-walled inlet reach with the hanging-valley notch |
| `freeway_climb.png` | `820,150,330,2.30,-0.08` | the SE promontory climb — deck, piers, city below |
| `ne_ravine_viaduct.png` | `980,200,890,2.9,-0.18` | the NE descent crossing the ravine on a viaduct (tunnel-worthy spurs beside it) |
| `crown_edge_vista.png` | `0,208,600,-1.5708,-0.30` | from a crown street over the lip: harbor, bluff, gorge mouth |
| `street_harbor.png` | `-40,10,530,-0.35,0.0` | lower-city street level: blocks, kerb, water railing, the wall behind |
| `street_crown.png` | `-280,196,702,0.0,0.0` | upper-city plateau with the ring deck on the horizon |

Capture caveats (same class as `../city-blocks/README.md`):

1. **Buildings are ECHO_CITY_PROXY blockout masses** — the real packs
   (`HouseForge`, `Urban Night City`, districts, boats) live under
   `D:/Assets/_glb` + `D:/GameDev` on the authoring box and are absent here.
   Without the proxy the counts are 0; with it: 9 lot + 205 frontage buildings,
   14 waterfront towers, 1/5 hero houses (the other four fail min-spacing
   against the new frontage row — authored positions predate V8).
2. **No pines**: the woodlands scatter computes (9017 instances, slice
   self-test PASS) but every `assets/veg/tree_pine*.glb` LFS blob is absent on
   this machine, so nothing renders.
3. **Mine spur dropped by the zigzag law** (edges 4/5, ~7 deg/m): with the
   south rim inside the new seam clearance, `nearestRingSample` for the
   truck-lot tee lands on the ring's closing leg whose tangent points AWAY
   from the lot — the hermite tee hairpins by construction. Roads-lane
   follow-up: pick the tee deck sample by tangent alignment, not distance
   alone.
