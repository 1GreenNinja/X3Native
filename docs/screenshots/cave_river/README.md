# Underground River — in-engine screenshots

A mildly-luminescent, SELF-EMISSIVE blue stream threading the low side-shoot cave
tubes of the descent (feat/cave-river). The engine's surface water
(setWaterParams/caustics/god-rays) needs a sky + sun; there is none this deep, so the
water is made SELF-LUMINESCENT — it IS a light source, a dimmer/cooler cousin of the
Salvari crystals — and needs no sun. All captured in-engine via
`--world club --screenshot <out> --shot-cam x,y,z,yaw,pitch` on the RTX 5090.

| shot | what | shot-cam |
|------|------|----------|
| `river_collapse.png` | HERO — cold blue river meandering down the warm MAGMA-biome tube (Collapse), crystal cache at the end; the warm rock makes the self-lit blue water unmistakable | `45,-670.8,3.8,0.05,-0.24` |
| `river_cache.png` | the stream threading the dark BASALT tube (Cache) to a glowing crystal cache; clean seam-free ribbon with the flow-crest brightness banding | `45,-281.9,3.8,0.05,-0.22` |
| `river_pool.png` | the river POOLING — a broad brighter glow where it widens, the crystal cache rising out of the water | `48,-281.6,3.8,0.05,-0.28` |
| `river_landmark.png` | the CATHEDRAL cavern (Landmark, crystal-veins biome) — the river pools around the house-sized crystal (bluer biome, so the water blends more) | `46,-561.7,3.8,0.10,-0.22` |

How it's built (NOT the sky-water system):
* GEOMETRY — a flat water RIBBON (a chain of short quad segments) laid just above the
  walkable floor of three side-shoot cave tubes (Cache / Landmark cathedral / Collapse),
  meandering down the low path, WIDENING + POOLING in the cavern bellies + dead-ends.
  Coarse, follows the authored cave layout. Node polyline emitted by
  `buildEarthTunnels()` into `DescentFallLayout::river`; ribbon + lights built by
  `CaveRiver` (`app/cave_river.h`, implemented in `app/cave_atmosphere.cpp`).
* SELF-EMISSIVE BLUE — MILD deep-electric-blue emissive (peak channel held well under
  1.0 so ACES holds the hue: a gentle glow, not a white strip) + a deep-blue albedo.
* LIGHTS THE BANKS — a handful of DIM blue point lights over the pools (2/tube, 6 total),
  pushed into the SAME distance-culled crystal/bedrock light channel (host cull @50 m,
  64-light cap) the Salvari crystals use.
* FLOWING — `CaveRiver::update()` scrolls a bright CREST downstream (dt-scaled) by
  modulating each segment's emissive strength along the river, + a fast micro-shimmer +
  a sub-cm ripple bob, so it reads as living, moving water. Pools breathe slower/brighter.
  (Motion is runtime; a still frame shows only the brightness banding.)

Gate: `--test-caveatmos` folds in the river self-test (build + MILD emissive-blue + a
handful of dim bank lights + flow animates without blowing out). `--smoketest --world
club` is VMA-leak-clean (allocationCount=0).
