# Terrain AAA pass — vista gallery

Rendered via `X3Engine.exe --screenshot-terrain <path>` (streamed canonical world +
freeway + world landmarks). `base_*` = the phase-1 flat-grass baseline; `aaa_*` =
this pass.

## Ground materials (the headline — flat grass → AAA)
- `aaa_vista.png` — EAST / volcanic range: dark basalt, glowing lava ember veins,
  craggy detail-normal relief catching the sun. The transformation shot.
- `aaa_tower_pad.png`, `aaa_freeway.png` — near ground: clod/scree micro-relief +
  dry/lush macro breakup + the paved freeway threading the graded corridor.

## The four biome ranges (compass identity on the horizon)
- `aaa_biome_north.png` — SNOW: cool frost cast + pale dusting on the high ground.
- `aaa_vista.png`       — EAST VOLCANIC: basalt + ember emissive.
- `aaa_biome_south.png` — SANDSTONE MESA: warm dry tan, sand apron on the flanks.
- `aaa_biome_west.png`  — CRYSTAL HIGHLAND: mossy blue-green + pink→cyan crystal glints.

## Landmarks (canon coords, rough massing)
- `aaa_lng_tank.png` — the LNG TANK set piece @ (-500,525): building-scale steel
  sphere on a canted-leg skirt + hazard band + base pipes + red beacon.
- `aaa_city.png`     — Scrapyard CITY massing @ (-600,500): lit blocks, cyan/amber
  neon crowns (silhouette/layout stand-in; neon-city detail is a separate campaign).
- `aaa_hero.png` / `aaa_tower_pad.png` — the facility TOWER stand-in on the origin
  pad (white concrete + dark-glass band rhythm + cyan crown) with the freeway in +
  the mountain horizon behind.

## Notes for Tim's eyes
- The TOWER is a layout STAND-IN (procedural white-concrete + dark-glass massing).
  The real asset lives on `feat/tower-white-concrete` (`host_surface_start`) — merge
  it onto the origin pad to finish the money shot.
- Single-frame full-world HERO (tower + city + tank + ranges together) is limited by
  the engine's GPU HZB occlusion cull: from the low city-side angle the intervening
  hills occlude the tower, so the hero is shot from the tower pad's clear-sightline
  side and the city/tank get their own verified frames. An elevated fly-cam or a
  bumped landmark cull bound would let one frame hold everything.
- North/West ranges fall on a low "pass" for the current terrain seed, so their
  snow-caps / crystal peaks are modest; the biome now also tints the GROUND so each
  compass still reads. Taller directional peaks would need a heightfield tweak.
- Faint horizontal LOD-ring banding in distant grass is a pre-existing streamer LOD
  artifact (present in `base_*` too), not from this material pass.
