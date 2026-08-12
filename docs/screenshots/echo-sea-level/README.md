# Echo Harbor — ONE SEA LEVEL (`fix/echo-sea-level`)

Base: `origin/fix/echo-sea-lanes` @ `ba7dd0fc`.

Every frame below is 1280x720, `--world echotropolis`, `ECHO_TOD=noon`,
`ECHO_SHOT_STREAMED=1` (without it the capture path draws **no** streamed
content — no boats, no fleet — because `regionSet.drawAll()` is gated on a
WorldStreamer that only ticks in the live loop).

BEFORE/AFTER are the **same binary at identical framing**, switched by two
default-off levers, so nothing but the thing under test differs:

* `ECHO_SEA_LEGACY_Y=0.10` — lift the drawn Gerstner patch back off the datum
* `ECHO_SHIP_FLATBOB=1` — restore the old flat `sin()` hull bob

---

## What was wrong

Four live answers to "where is the sea", all shipping at once:

| value | who says it | where it is read |
|---|---|---|
| **+0.10** | Gerstner wave patch (`applyOcean`, all 3 swell presets) | what the player SEES at play altitude |
| **0.00** | `Heightfield::heightAt` zero-crossing | every seat, road, lane clip, swim + car water query (~40 sites) |
| **-0.40** | baked 28 km ocean quad (`echo_terrain_gen.py OCEAN_Y`) | the ONLY sea drawn at night and above 140 m eye height |
| **-0.30** | swim-entry threshold | `host_echotropolis.cpp` |

Plus five more thresholds (`kWaterMinLand` 1.5, `kGateLandSafe` 2.0,
`kLandSafe` 2.5, `kKeelDraft` -4.0, `kBoatY` 0.6) authored as **absolute
heights** rather than offsets, so none of them tracked the sea.

**The datum is `heightAt`'s zero-crossing (0).** Not a preference: `heightAt`
is welded to the baked island GLB (`SEANORM 0.20`). Moving the sea to +0.10
means moving `kSeaNorm`, which puts the sampler 0.10 m below the mesh that is
actually drawn — re-opening the exact desync `fix/echo-road-surface` just
closed — and costs a re-bake of a 27 MB LFS asset plus a re-audit of every
placement. Moving the wave patch down costs one float and a tuning table.
Full argument in `app/world_hosts/echo_sea.h`.

---

## `BEFORE_hull.png` / `AFTER_hull.png` — the real win

`--shot-cam -577,2.2,115,1.5708,-0.029`, `ECHO_SHOT_T=1.8167` (the capture
lands at t = 2.2 s).

That moment is chosen, not lucky: at t = 2.2 s the old flat bob
(`kBoatY + sin(t*0.7)*0.35`) puts the hero hull at **y = 0.950** while the
Gerstner surface under it is at **y = 0.344** — the ship floats **0.606 m
proud of its own water**. The bob shared no phase, frequency, direction or
amplitude with the sea; it was an unrelated sine.

BEFORE: the hull bottom is clear of the water, ship dead level.
AFTER: the hull is immersed to its proper waterline and visibly pitched —
`echoShipPose()` (which shipped in `echo_water.h` and was never called)
sampling bow/stern/port/starboard on the same Gerstner sum the shader draws.

Hull half-length/half-beam are **measured** from the loaded mesh AABB, not
guessed per asset — all three authored lanes are axis-aligned, so the AABB's
X/Z extents are exactly the hull's length and beam.

## `BEFORE_fleet.png` / `AFTER_fleet.png`

`--shot-cam -545,4,200,-2.601,-0.058`, `ECHO_SHOT_T=0`. Same fix at t = 0,
where the divergence happens to be small (0.20 m). Included deliberately: it
is the honest weak case. The pitch/roll is visible, the heave is not.

## `BEFORE_waterline.png` / `AFTER_waterline.png` — **the honest negative**

`--shot-cam -440,8.0,-152,1.5708,-0.6` — a steep look-down on an undeveloped
beach, framed so ~0.037 m of ground maps to one pixel.

**The waterline barely moves: mean 0.58 px, max 2.0 px.** That is not a
failure of the fix, it is the measured truth about this island. Dropping the
sea 0.10 m moves the shoreline by `0.10 / slope`, and this bake has no gentle
beaches — median shore gradient 0.195 m/m, and 0.40 m/m at this site, so
0.10 m of datum is 0.25 m of coastline. Do not read these two frames as
"sea level fixed the shoreline". It did not, visibly. What it fixed is that
the water the game DRAWS and the water the game COMPUTES WITH are now the
same plane.

## `RESIDUAL_waterline_ringonly.png` — what is **not** fixed

Same framing, `ECHO_WATER_OFF=1` — i.e. what the game renders at night and
above 140 m eye height, where the Gerstner patch is disabled and the baked
-0.40 quad is the only sea.

98.8% of pixels differ from AFTER (flat slab vs. live surface) but the
waterline moves only ~0.09 px on average: the 0.40 m residual is a **shading**
difference far more than a **position** difference.

This residual is knowingly accepted. The ring cannot be both "the sea when the
patch is off" (wants ring == datum) and "the floor the troughs must clear"
(wants ring below the trough). This lane declares it the FLOOR only, and caps
**amplitude** instead of lifting the sea. `--test-sealevel` prints the
residual every run so it stays a known limitation.

## `NOT-FIXED_waterfront_houses.png` — attribution, not a fix

`--shot-cam -500,11,-178,0.862,-0.20`. The waterfront cottage row.

Measured on this framing:

* mesh-matched sampler vs `ECHO_RAW_HF=1` (the roads lane's fix): **0.02%** of pixels differ
* unified sea vs legacy sea (this lane): **1.56%** of pixels differ, and **81% of that is water**, the rest the wet band at its edge

**No building moved.** Houses clipped at the water's edge are a placement /
seating defect, not a sea-level defect — the building gates (`landOk >= 2.5`,
`placeSeated`) are numerically unchanged by this lane. Attributed here rather
than absorbed.

Likewise the `infra` deck legs (`host_echotropolis.cpp:1591-1626`): on this
base `placeDeckP`/`placePillar` are `(void)`-cast — **dead code**, retired in
favour of the EchoRoads module. Whatever plants a column from the seabed is
the road module's pier pass (`echo_roads.cpp:532 pillar()`), seated off
`hf.heightAt`, which is `fix/echo-road-surface`'s sampler, not this lane's
datum.

---

## Downstream claims, judged

| claim | verdict |
|---|---|
| hulls sitting proud of the surface | **FIXED** — 0.606 m worst-case error removed, frame-proven |
| the shoreline where land meets sea | **consistent, not visibly different** — 0.58 px mean. Land tests are now correct by construction (no cell is "dry" by the datum yet under the drawn water); the coastline itself does not visibly move on this bake |
| houses clipped at the water's edge | **NOT FIXED, not ours** — placement/seating; no building moved |
| piers ending in air or underwater | **NOT OURS** — road-module piers off `heightAt`; belongs to `fix/echo-road-surface` |

## Perf

Live loop at the sea-level viewpoint (`--shot-cam -577,3.0,140,1.5708,-0.03`),
3 interleaved A/B runs of 40 s each, first 10 s dropped as boot/stream warm-up
(the untrimmed p10 is dominated by pipeline compiles and says nothing):

| | n | min | p10 | p50 | p90 |
|---|---|---|---|---|---|
| legacy | 87 | 46.5 | 47.5 | 48.1 | 48.5 |
| unified | 87 | 46.5 | **47.2** | 48.0 | 48.6 |

No regression (the 45 extra `sin`/frame for 9 hulls is not measurable).

## Cost, stated plainly

Dropping the sea 0.10 m cost every swell preset 0.10 m of headroom over the
-0.40 ring. CALM and HARBOR are unchanged. **STORM's amplitude had to fall
0.22 -> 0.175** (a real 20% loss of storm wave height) because 0.22 now troughs
at -0.4224, i.e. *through* the ring. To get the big storm back the lever is the
RING, not the sea: re-bake with `OCEAN_Y = -1.0` (invisible — the ring is only
ever seen above 140 m or at night) and `echoMaxAmplitude()` rises to 0.49 by
itself.
