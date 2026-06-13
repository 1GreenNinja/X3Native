# FORGE3D Planets HD — Texture Manifest (per planet type)

Source pack root: `C:\Users\Tim\X3\Assets\FORGE3D\Planets\`
All `<Type>/Textures/` paths below are relative to that root.

## Key facts established by inspecting the pack + materials

- **Every PNG in the pack is 8-bit RGB (PNG color type 2), NO alpha channel.**
  So "single-channel" maps (heights, masks, scatter ramps, city detail) actually
  store their data in a specific channel of an RGB image (usually R, some use R+G
  or R+B). Normal maps are **plain RGB** (xy in `.rg`), **NOT Unity DXT5nm** — so
  `unpackScaleNormal` reads `.rg` (the default in `planet_common.glsl`; do NOT
  define `PLANET_NORMAL_AG`).
- **The "scatter LUT" is a real baked texture**, not procedural: the materials
  point `_ScatterMap` at the `Atmosphere/sunset_*.png` ramps (e.g. Terrestrial &
  Ice use `sunset_yellow_05`, Gas `sunset_yellow_01`, Lava `sunset_red_04`,
  Oceanic `sunset_green_01`). These same `sunset_*` PNGs are the atmospheric
  scattering gradients — they double as both the surface scatter LUT and the
  Atmosphere shell ramp. Sample with **CLAMP_TO_EDGE**.
- The `Shaders/*.asset` files (`Triplanar`, `ScatterColor`, `Ramp3`, `PolarCoord`,
  `Fresnel`, `Lightning`, `Water`, etc.) are **ASE ShaderFunction node graphs, NOT
  textures** — they are already reimplemented in `planet_common.glsl` / the frags.
- `Oceanic/` and `Thunderstorm/` and `Sandstorm/` ship NO unique surface
  height/normal art of their own where noted — they **reuse Terrestrial/Moon
  maps** (verified via material GUID resolution).
- The `_Gradient` (cloud pole<->belly blend ramp) is the shared
  `Misc/Textures/polegradient_01.png` (128×128 RGB) for all cloud planets.

---

## Terrestrial  (`planet_terrestrial.frag`)
| Shader sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_HeightMap` | `Terrestrial/Textures/terrestrialdetail_01..04.png` (2048²) | R=base/mtn, G=veg, B=desert/elev | triplanar terrain biome control |
| `_NormalMap` | `Terrestrial/Textures/terrestrialdetail_01..04_normal.png` | RG (z reconstr.) | triplanar surface normal |
| `_LandMask`  | `Terrestrial/Textures/landmask_01..05.png` (4096²) | R=land/water split, G=shore | equirect land/sea mask (lat-long UV) |
| `_ScatterMap`| `Atmosphere/sunset_yellow_05.png` (et al.) | RGB | atmosphere/terminator LUT (N.L, N.V) |
| `_Gradient`  | `Misc/Textures/polegradient_01.png` | R | cloud pole↔belly blend |
| `_CloudsTop` | `Terrestrial/Textures/cloudscap_01..08.png` | R | polar (rotated) cloud layer |
| `_CloudsMiddle` | `Terrestrial/Textures/clouds_01..08.png` | R | belly (scrolled) cloud layer |
| `_CityLightMap` | `Terrestrial/Textures/lights_01..03.png` | R | night city-light detail |
| `_CityLightUVMap` | `Terrestrial/Textures/lights_01..03_uv.png` | RG | UV remap for city lights |
| `_CityLightMaskMap` | `Terrestrial/Textures/lights_01..03_mask.png` | R | population/coverage mask |
| *(equirect base)* | `Terrestrial/Textures/terrestrial_01..04.png` | RGB | optional baked surface (not used by the triplanar port; available as a fallback albedo) |

## Oceanic  (`planet_oceanic.frag`) — reuses Terrestrial art
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_HeightMap` | `Terrestrial/Textures/terrestrialdetail_02.png` | R,B → water depth | triplanar depth |
| `_NormalMap` | `Terrestrial/Textures/terrestrialdetail_02_normal.png` | RG | ocean normal |
| `_ScatterMap`| `Atmosphere/sunset_green_01.png` | RGB | scatter LUT |
| `_Gradient`  | `Misc/Textures/polegradient_01.png` | R | cloud blend |
| `_CloudsTop` | `Terrestrial/Textures/cloudscap_02.png` | R | polar clouds |
| `_CloudsMiddle` | `Terrestrial/Textures/clouds_05.png` | R | belly clouds |

## Sandstorm (= "Sand")  (`planet_sand.frag`)
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_DetailMap` | `Sandstorm/Textures/sandstorm_01..04.png` (2048²) | R=base/spec, G=mtn, B=desert | triplanar terrain control |
| `_NormalMap` | `Sandstorm/Textures/sandstorm_01..04_normal.png` | RG | triplanar normal |
| `_ScatterMap`| `Atmosphere/sunset_yellow_*.png` | RGB | scatter LUT |
| `_Gradient`  | `Misc/Textures/polegradient_01.png` | R | dust-cloud blend |
| `_CloudsTop` | `Sandstorm/Textures/dustcloudcap_01..04.png` | R | polar dust cap |
| `_CloudsMiddle` | `Sandstorm/Textures/dustcloud_01..04.png` | R | belly dust bands |
> ⚠️ The shipped `Planet_Sandstorm_01.mat` is a URP-template stub with empty texture
> slots; the assignment above is inferred from the `Sandstorm/Textures/` file
> naming (which exactly matches the cloud/detail/normal roles). Confirm against a
> Standard-pipeline `.mat` or the prefab before final wiring.

## Gas  (`planet_gas.frag`)
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_HeightMap` | `Gas/Textures/planet_gas_01..07.png` (2048×1024, **2:1 equirect**) | RGB | banded color/albedo+spec (lat-long panner, NOT triplanar) |
| `_UVDistortionMap` | `Gas/Textures/planet_gas_08.png` (also `Gas_Distortion_01.png`) | R | flow distortion of the band UVs |
| `_ScatterMap`| `Atmosphere/sunset_yellow_01.png` | RGB | scatter LUT |

## Ice  (`planet_ice.frag`) — STATIC
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_ColorMap` | `Ice/Textures/ColorMapSqr.png` (256²) / `icecolormap.png` (256×128) | RGB | ice color tint (triplanar) |
| `_HeightMap`| `Ice/Textures/ice_01..05.png` (2048²) | R | elevation |
| `_NormalMap`| `Ice/Textures/ice_01..05_normal.png` | RG | normal |
| `_DetailMap`| `Ice/Textures/icedetail_01..04.png` | R,G → pow(R*G,0.5) | fine detail / spec mask |
| `_ScatterMap`| `Atmosphere/sunset_yellow_05.png` | RGB | subsurface scatter LUT |

## Lava  (`planet_lava.frag`)
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_HeightMap`| `Lava/Textures/lava_01..04.png` (2048²) | R (used at falloff 5 AND 2) | rock height + lava-mask source |
| `_DetailMap`| `Lava/Textures/lavadetail_01..04.png` | R,G | crack mask (also R = fresnel detailX) |
| `_MagmaMap` | `Lava/Textures/lavadetail_*.png` (same as detail in `_01.mat`) | R | glowing vein mask |
| `_NormalMap`| `Ice/Textures/ice_04_normal.png` (reused) | RG | normal (falloff 1) |
| `_DistortionMap` | `Lava/Textures/lavadistmap.png` (2048²) | RGB + RG warp | animated molten flow color + UV warp |
| `_ScatterMap`| `Atmosphere/sunset_red_04.png` | RGB | scatter LUT |

## Moon  (`planet_moon.frag`) — STATIC
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_Albedo`   | `Moon/Textures/moon_01..10.png` (2048²) | RGB | base color (triplanar) |
| `_Normal`   | `Moon/Textures/moon_0N_normal.png` (and `_NRM`, `moondetail_0N_normal`) | RG | normal |
| `_DetailMap`| `Moon/Textures/moon_01_detail.png` | RGB | detail overlay (boosted into albedo) |
| `_SpecularMap` | `Moon/Textures/moon_0N_spec.png` | R | specular mask (.r only) |
| `_ScatterMap`  | `Atmosphere/sunset_*.png` | RGB | wrap-light/scatter LUT |

## Thunderstorm  (`planet_thunderstorm.frag`) — reuses Terrestrial/Moon art + storm maps
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_NormalMap` | `Moon/Textures/moon_07_normal.png` (reused) | RG | animated cloud normal |
| `_DistortionMap` | `Thunderstorm/Textures/thunderstorm_01..04.png` | R → Tint gradient | detail/cloud bands |
| `_DistortionUVMap` | `Terrestrial/Textures/terrestrialdetail_04.png` (or storm) | R | flow distortion |
| `_ScatterMap`| `Atmosphere/sunset_yellow_05.png` | RGB | scatter LUT |
| `_LightingMaskMap` | `Thunderstorm/Textures/stormmask.png` (512²) | R | where lightning is allowed |
| `_LightingMap` | `Thunderstorm/Textures/storm_01..02.png` (1024²) | R | lightning strike texture |

## Sun body  (`planet_sun.frag`) — EMISSIVE
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_SurfaceMap` (`_SurfraceMap`) | `Sun/Textures/sunsurface_01.png` (1024²) | R=flakes, G=surface heat | triplanar surface heat/granule |
| `_DistortionMap` | `Thunderstorm/Textures/storm_02.png` + `stormmask.png` | RG | flow distortion of the surface UVs |

## SunCorona  (`planet_suncorona.frag`) — ADDITIVE
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_CoronaMap` | `Sun/Textures/suncorona_01.png` (1024²) | R (cNoiseC/B/E), G (cNoiseA/D) | grayscale flow-noise atlas for all corona layers |

## Atmosphere shell  (`planet_atmosphere.frag`) — ADDITIVE
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_AtmosphereSample` | `Atmosphere/Atmosphere_01..03.png` and/or `Atmosphere/sunset_<color>_*.png` | RGB | horizon optical-depth gradient ramp (1D-as-2D, sampled at `uUVOffset + saturate(N.V)`) — CLAMP_TO_EDGE |

## Ring  (`planet_ring.frag`) — ALPHA
| Sampler | File(s) | Channels | Role |
|---|---|---|---|
| `_DetailMap` | `Gas/Textures/ring_01..08.png` (1024×64, **radial strip**) | RGB = ring color, R = alpha source | sampled at `(radialT, 0.5)` inner→outer — CLAMP_TO_EDGE |

---

## Runtime-procedural / no-file flags

- **No procedural-noise textures are required** — the analytic
  Simplex/Voronoi/fbm in `planet_common.glsl` are a *fallback only*. Every
  faithful look uses the PNGs above.
- The scatter LUT, gradient ramp, and atmosphere ramp are **shared** `Atmosphere/`
  + `Misc/` PNGs reused across many planet types — copy them ONCE into the engine
  and reference by all materials. (Recommended engine layout:
  `assets/textures/planets/<type>/...` plus `assets/textures/planets/shared/`
  for `polegradient_*`, `sunset_*`, `Atmosphere_*`.)
- Convert PNG→RGBA8 on load (the engine's `createTexture` wants tightly-packed
  RGBA8). Use **sRGB** for the color/albedo/cloud maps (`_Albedo`, `_HeightMap`
  color bands for Gas, `_ColorMap`, cloud layers, `_ScatterMap`, ring detail) and
  **UNORM/linear** for data maps (`_NormalMap`, `_LandMask`, height/detail masks,
  `stormmask`, `lights_*_uv`, `polegradient`).
