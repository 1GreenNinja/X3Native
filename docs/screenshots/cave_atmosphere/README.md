# Cave / Tunnel Atmosphere — in-engine screenshots

The 800 m fall-shaft + side-shoot cave system as a crystal-lit, eerie underworld
(feat/cave-atmosphere). All captured in-engine via `--world club --screenshot ...
--shot-cam x,y,z,yaw,pitch` on the RTX 5090. Crystal-only lighting (club ambient/IBL
faded to near-black in the caves), dark rock, glowing blue Salvari crystals, strata
biomes, fog haze.

| shot | what | shot-cam |
|------|------|----------|
| `hero_singing_cave.png` | the money shot — dark basalt cavern, blue crystal cache, stalactites, rubble | `45,-282.5,3.8,0,-0.03` |
| `shaft.png` | crystal-lit fall shaft — crystal glow pool + shards in the dark bore | `38,-433,3.8,3.05,-0.05` |
| `tube_cathedral.png` | a lumpy TUBE cavern (limestone) — stalactites/stalagmites, passage back to the shaft | `58,-140.5,3.8,3.14,0.02` |
| `cache.png` | dead-end CACHE (basalt biome) — dense crystal reward in a blue alcove | `44,-283,3.8,0,-0.02` |
| `collapse.png` | COLLAPSE (magma biome) — warm self-glowing rock, tumbled blocks, cold crystal cache | `44,-672,3.8,0.05,-0.05` |
| `landmark_crystal.png` | HOUSE-SIZED crystal LANDMARK beacon (crystal-veins biome) | `45,-560.5,1.0,0.22,0.06` |
| `shrine.png` / `shrine_altar.png` | the ancient SALVARI SHRINE — dais, altar crystal, glyph stelae | `47,-421.2,3.8,0,-0.06` / `50.5,-421.3,3.8,0,-0.12` |
| `fog_godray.png` | volumetric fog haze around the landmark crystal | `43,-561,3.8,0.02,0.08` |

Biomes read at a glance: basalt/granite = dark grey, obsidian = near-black, magma =
warm orange (self-glowing), crystal-veins = blue. The crystal BEAT-PULSE + felt bass
+ singing hum are motion/audio (self-test `--test-caveatmos`; live audio only).
