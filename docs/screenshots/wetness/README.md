# Surface wetness — the A/B, including the two frames that were WRONG

All captured headless, same camera, `ECHO_CITY_PROXY=1 ECHO_TOD=noon`,
`--world echotropolis --set r_wetness <0|1> --shot-cam <cam>`.

| frame | cam | what it shows |
|---|---|---|
| `vista_dry.png` / `vista_wet.png` | `0,208,600,-1.5708,-0.30` | crown edge over the harbour. Deck mean luminance 113.55 -> 106.51. |
| `road_dry.png` / `road_wet.png` | `820,150,330,2.30,-0.08` | the freeway climb. Wet asphalt against pervious grass. |

## The two defects, kept on purpose

`DEFECT_1_cliff_mirror.png` — the FIRST wet capture. Every wet fragment was
collapsed to the film roughness (0.06), so the cliff faces became mirrors and
the noon sun lit them with blown specular streaks. Two wrong assumptions: that
a water film levels every surface it lands on (it does not — a thin coat
follows the substrate; only POOLED water levels), and that ambient occlusion
identifies a puddle (it does not — AO is low on any occluded geometry, and a
vertical cliff shaded by its own terrain reads as a deep cavity). Pooling now
requires the cavity AND an upward-facing surface. Gravity is not optional.

`DEFECT_2_grass_gloss.png` — the second. Cliffs fixed, but the green hillsides
were streaked with chrome. Water SOAKS INTO soil, grass and scree: they darken
and never gloss. Only impervious surfaces hold a film on top. Gating on
`FLAG_TERRAIN` changed nothing at all in the A/B — this world's terrain reaches
the shader WITHOUT that flag — so the perviousness is applied on the untextured
dielectric branch, which is that path.

Both frames are committed because the numbers alone would not have caught
either one. Blown-pixel count over the cliff band actually went UP between the
first fix and the second (30 -> 235) while the image got better, because a
broader specular lobe spreads the same energy over more pixels. The image is
the verification; the metric only ever supports it.
