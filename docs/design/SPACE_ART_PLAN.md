# X3Native — Space Environment Art Plan

> Act-3 (Beyond the Stars, ~L36-75 per MasterPlan) needs a full visual environment beyond just ship hulls. This plan organizes asset categories, generation tools, and a phased rollout. Companion to `docs/design/X3_WORLD_BLUEPRINT.md` and `eflz-design-corpus` memory.

## Asset categories

| Category | What | Generation approach |
|---|---|---|
| Skybox / nebula backdrop | 360° HDR panorama (the deep-space "wallpaper" you fly through) | Blockadelabs Skybox AI (skybox.blockadelabs.com) — purpose-built for game skyboxes, generates ready equirectangular HDR cubemaps from text prompts |
| Procedural starfield | Infinite small-stars layer on top of nebula | Engine fragment shader (`app/sky_stars.{h,cpp}`, this branch) — hashes view-direction → stable parallax-correct twinkling stars. NO gen tools needed |
| Planets (sphere geometry) | Mid-distance planets to fly around | **Tim has Planet Packs** (path TBD as of 2026-05-27) — use those. Fallback: NASA public imagery (Earth/Mars/Jupiter/Saturn moons, CC0 free) + SDXL on 14900K alien-planet prompts |
| Sun | Distant point-light star with bloom + lens flare | Engine-side: directional light + additive billboard sprite + bloom from existing post pipeline |
| Asteroids | 8-12 rock variants, GPU-instanced in clusters | Rodin Image-to-3D batch — one good asteroid concept render → Rodin Multi-view → 8-12 rock GLBs |
| Stations / structures | Stations, dropships, mothership for encounters | Rodin Image-to-3D, Phase 3. 4 SpaceShip*.glb already in rigged_glb |

## Phased rollout

### Phase 1 — `--world space` showcase (immediate)
- ✅ Procedural starfield shader (this branch — `feat/space-stars`)
- 1 skybox cubemap from Blockadelabs (prompt: "deep space with purple-blue nebula, distant galaxy, scattered bright stars, HDR, cinematic")
- 2-3 planets from Tim's Planet Packs (Earth-like + Jupiter-like + 1 alien)
- Sun = engine directional light + additive sprite
- Integrates into space-pilot's `--world space` block at integration time

### Phase 2 — Act-3 sector variety (~5-10 sectors of Beyond-the-Stars)
- 5-10 skyboxes per sector (different nebula moods): Salvari blue-purple home space, Verthani green-toxic, Illuminated golden, Dominion grey-industrial, Overlord crimson-corrupted
- 8-12 planet textures (Tim's packs + Phase-2 supplemental gen if needed)
- Rodin asteroid batch → 8-12 rock GLBs (instanced)

### Phase 3 — Encounter set pieces
- Stations: Salvari refugee, Dominion garrison, Overlord hive-ship
- Mothership (L100 finale)
- Damaged debris fields (recycle asteroids + ship-wreckage Rodin gen)
- Dropships / boarding craft (Rodin from concept art)

## Tool / pipeline summary

| Tool | Use | Where | Cost |
|---|---|---|---|
| Blockadelabs Skybox AI | All skybox panoramas | This rig (browser) | ~$10/mo unlimited |
| Tim's Planet Packs | Planet sphere textures (Phase 1+2) | Local (path TBD) | already owned |
| NASA imagery | Real planet photos (fallback) | Browser download | Free / CC0 |
| SDXL on 14900K (Deliberate Cyber v6) | Alien planet textures + asteroid concept art | 14900K | Free |
| Rodin Image-to-3D | Asteroids, stations (Phase 2+3) | This rig browser | ~0.5 credits per gen |
| Engine code (shader / particle / light) | Starfield, sun, lens flare | Agent dispatch | Free |

## File / asset organization

- `assets/skyboxes/space_<sector>.ktx2` — cubemap-converted Blockadelabs outputs
- `assets/planets/<name>_albedo.png` + `_normal.png` + `_specular.png`
- `assets/rigged_glb/asteroid_<N>.glb` — Rodin asteroid batch
- `app/sky_stars.{h,cpp}` + `shaders/starfield.{vert,frag}` — procedural starfield (this branch)
