# LANE 7 — fjord sea-entry captures (FOR TIM'S APPROVAL) + desert bed

All shots: `EchoHarbor.exe --world echotropolis --screenshot <png> --shot-cam <x,y,z,yaw,pitch>`
with `ECHO_ISLAND_DIR=<repo>/assets/island_fjord_v2 ECHO_CITY_PROXY=1 ECHO_TOD=noon`.
Yaw: **0 = +x east, +π/2 = +z north**. Camera y ≈ 14 m = **boat eye**, deliberately — the brief
asks for "the approach through the gorge", which is a sea-level shot, not the 160 m aerial the
earlier `island-regen` set used.

Route is real, not eyeballed: a BFS through navigable water (keel draft −1.5 m) from the open-ocean
west edge to the waterfront — **3,868 m / 2.4 mi**, one connected body of water. The stations below
are sampled along it, each aimed down-channel.

| shot | `--shot-cam` | wall heights L/R |
|---|---|---|
| `sea_01_mouth` | `-2020,14,-1040,0.245,-0.02` | 60 / 51 m — the gap seen from open ocean |
| `sea_02_narrows` | `-1384,14,-1044,-0.031,-0.02` | −9 / 223 m — the narrows, crown tower in the gap |
| `sea_03_deepreach` | `-996,14,-1056,0.075,-0.02` | 87 / 138 m |
| `sea_04_bend` | `-624,18,-1028,0.405,0.02` | 199 / 4 m |
| `sea_05_turn` | `-344,18,-908,0.725,0.03` | 192 / 4 m |
| `sea_06_cityreveal` | `-20,18,-432,1.571,0.03` | 132 / 59 m |
| `sea_07_basin` | `-20,20,-32,2.046,0.04` | basin |
| `sea_08_waterfront` | `-156,16,232,1.308,0.05` | waterfront |
| `sea_09_wall_dusk` | `60,25,140,1.5708,0.12` | the 190 m harbor wall |
| `*_v2` | same cams | re-shot on `--surface 2` (A/B for the shore-band fix) |
| `desert_01_corridor` | `1600,105,-1700,-1.5708,-0.05` | NE desert bed, north end |
| `desert_02_grade` | `1560,72,-900,-1.5708,-0.02` | NE desert bed, mid-grade |

---

## MY OWN CRITIQUE — read this before deciding anything

I read every one of these myself. **The landform sells. The materials do not.** These are
*blockout-quality* captures of a *good* world. I am not presenting them as beauty shots.

**What genuinely works**
* The gorge reads as a gorge from the water: cliff walls frame the channel, the meander hides and
  reveals the next reach, and the crown tower peeking over the ridge in `sea_02`/`sea_03` is a real
  reveal beat. `sea_02_narrows` is the strongest composition of the set.
* Water surface itself (near patch) is good — animated ripple, believable specular.
* Scale reads: 190–220 m walls over a 350 m channel feels like Columbia Gorge, which is the canon.

**What is wrong (in severity order)**
1. **Water-plane seam** — a hard horizontal line across the full frame where the near water patch
   meets the far ring (bright cyan → dark navy → white sliver). It reads as "the sea ends and a
   different sea starts" and it wrecks every sea-level shot. *Already filed by Tim as Lane 5 water
   tier 2 ("patch-vs-ring contrast at eye<140 m"). Not fixable in the terrain bake — flagging, not refiling.*
2. **No detail texture on terrain.** The bake is one 4096² albedo over 4096 m = 1 m/texel, which is
   mush at boat range; cliffs read as flat untextured mass with visible contour terracing. Needs a
   material-side detail/tiling layer. **Biggest single art win available.**
3. **Cliffs crush to black** — no ambient/bounce fill on shadowed walls, so half of most frames is a
   featureless silhouette.
4. **Mesh-skirt artifact** — the black rounded "tube" at frame right in `sea_02*` is the `SKIRT_Y=-8`
   rim skirt viewed from inside the frame.
5. Water colour is a tropical cyan; a PNW fjord wants cold dark green-grey.
6. The intro-spectacle UFO floats in shot (`sea_01`) — fine in play, wrong for an approval frame.

**What I fixed this session (`--surface 2`, A/B visible in `sea_01/02` vs `sea_01/02_v2`)**
* The **lime highlighter waterline** is gone. The old bake laid a 2.5 m `(172,156,118)` sand ramp
  straight over `(84,102,56)` grass, which traced every shoreline in bright yellow-green. v2 narrows
  it, cools it to a wet grey-tan, and jitters its width so it is no longer a uniform ribbon.
* Cliff rock got a finer noise octave (marginal — cannot beat 1 m/texel; see defect 2).

**⚠ `sea_06`–`sea_09` are BROKEN-PLACEMENT EVIDENCE, not approval frames.** Shoot the approval
decision off `sea_01/02/03` (open gorge, no authored content in frame). The city-adjacent shots
photograph the coastline-audit blocker exactly as Tim predicted:

* `sea_06_cityreveal` — **a whole row of houses is standing IN the bay**, half-submerged, water at
  the windowsills. This is verbatim Tim's no-refiling item (b): *"HOUSES ON WATER on the fjord
  world: authored-position content ignores the changed coastline."* Now confirmed visually rather
  than predicted. (Also a dark mound floating mid-water, mid-left.)
* `sea_09_wall_dusk` — **a cluster of glass towers hangs in mid-air**, fully detached, plus a
  floating tree trunk. The crown towers are seated to the *old mesa* bake's heights; on the fjord
  bake there is no ground under them.

**Conclusion: the fjord bake must NOT be landed on `echotropolis` until authored placements are
re-validated against the active heightfield.** `main` already needed `51c11190 fix(city): reject
cliff-edge lots` for the same class of failure. This is the fjord prerequisite, and it is now a
measured blocker, not a worry. Rollback stays trivial (unset `ECHO_ISLAND_DIR`).

**Desert shots — do not oversell these.** `desert_01/02` show the terrain **bed** only: the band now
retints to sand/caliche, which is a foundation. As an *environment* it is a **featureless empty
plain** with heavy horizontal terracing and a black featureless ridge on the horizon. There is no
highway, no gas station, no mine, no rock, no cactus. **Bed, not environment.** The build order for
the actual corridor is in `docs/plans/LANE7_WORLD_FRAME.md` §3.

---

## Regeneration / rollback

```
python tools/echo_terrain_gen.py --out assets/island_fjord_v2 --surface 2     # what these shots use
python tools/echo_terrain_gen.py --out assets/island_mesa    --surface 1      # bit-exact to main
```
`--surface 1` is pinned and must keep hashing to main's LFS oids:
`PNG cb28a30b…7742c94`, `GLB 7f64d366…cba91e9`. Heights are identical under both surfaces —
only the baked albedo differs, so every `--verify` anchor holds either way.
Rollback to the pre-fjord world is unchanged: unset `ECHO_ISLAND_DIR`.
