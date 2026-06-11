# RT soft shadows — before/after proof (r_rtshadows)

Per-pixel inline ray-query shadows in the mesh fragment stage (mesh_rt.frag.spv,
the refl_rt two-variant pattern), traced against the SAME scene TLAS RT-AO /
RT-reflections / DDGI share. Tier gate: ray query; Pascal-class devices keep the
raster path byte-for-byte (the plain mesh.frag.spv was verified codegen-identical
to base via spirv-dis — only the OpSource debug line + two appended, unread UBO
members differ).

- **tier 0** (`r_rtshadows 0`): CSM-only — today's path, plain pipelines bound.
- **tier 1**: SUN — one cone-jittered ray/pixel (`r_rtsun_size`, default 0.5° angular
  radius), `min()`-combined with the CSM term so skinned characters (absent from
  the static TLAS) keep their raster shadows.
- **tier 2** (default on ray-query hardware): sun + **POINT LIGHTS** — lamps finally
  cast. The first K contributing lights per pixel (`r_rtpoint_max`, default 4) each
  get one ray toward a jittered point on the light's spherical source
  (`r_rtpoint_size`, default 0.10 m); rays stop a clearance short of the source so a
  light parked inside its own fixture mesh doesn't self-occlude.

Per-frame jitter rotation; TAA accumulates the 1-spp penumbra noise (seed pinned 0
with TAA off → static dither, no sizzle). All captures from the same
`feat/rt-shadows` Release build via `--screenshot-rtshadows`.

## THE GATE SHOT (lamp shadowing the cell)

A detention-cell rig lit by ONE ceiling lamp (sun parked below the horizon): a low
bunk near the wall + a tall pillar mid-room.

| file | verdict |
|---|---|
| `lamp_rtshadows_off.png` | Today's renderer: the lamp casts **nothing** — bunk and pillar float in pure distance attenuation. |
| `lamp_rtshadows_on.png` | **The money shot.** The pillar throws a shadow band up the wall behind it; the bunk casts onto the floor and kicks up the wall, soft-edged; the penumbra visibly widens with occluder→receiver distance (tight under the bunk, broad at the pillar shadow's far end). Honest traced occlusion from a point source with area. |
| `lamp_motion_f0/1/2.png` | Three consecutive frames while the camera slides: shadows stay put, no sizzle/ghost trails through TAA. |

## Sun: CSM vs RT (tier 0 vs tier 1)

Outdoor plate, LOW sun (~23°): a cube + a 4 m pole throwing a ~9 m shadow.

| file | verdict |
|---|---|
| `sun_csm.png` / `sun_rt.png` (+ `*_nearcrop` / `*_farcrop`) | RT: crisp contact at the cube base and the pole foot, progressively softening along the 9 m run (the 0.5° solar disk); CSM shows its constant 3×3 PCF blur everywhere **and truncates the pole shadow at its frustum edge — the RT shadow simply continues.** |

## Cost (RTX 5090, 1280×720, 60-frame GPU-timestamp averages; a CONCURRENT
engine instance held the GPU at ~100% during capture — treat as indicative)

- COST plate (576-instance TLAS, 8 overlapping lamps so EVERY pixel saturates
  the K=4 point budget + the sun ray = 5 rays/pixel full-res):
  tier 0 **0.262 ms** → tier 1 **0.273 ms** (+0.011 ms sun) → tier 2
  **0.302 ms** (+0.029 ms points). Whole feature ≈ **+0.04 ms**.
- Gate rig (8-box room, 1 lamp): tier 0 0.285 ms → tier 2 0.300 ms (+0.015 ms).
- Level-1 smoketest single-frame stat was useless under the contention
  (both tiers ranged 22–1850 ms run-to-run, overlapping); the averaged plates
  above are the credible numbers.
- **Full-res verdict:** inline fragment-stage queries are so cheap at our scene
  scale that the half-res shadow target + bilateral upsample contingency was
  NOT implemented — measured first, as planned.

## Honest notes (v1, shared with the other TLAS consumers)

- Opaque-only rays: alpha-cutout foliage/billboards occlude as their full quad.
- Skinned characters don't cast RT shadows (not in the static TLAS); the sun
  keeps their CSM shadows via the min() combine — lamps don't see them yet
  (documented next tier).
- Beyond the K-light budget (or below the contribution floor) point lights are
  unshadowed — exactly the pre-existing behavior.
- The sun term is min(CSM, RT): RT contact hardening is bounded by CSM's PCF
  blur where both shadow the same texel (a deliberate trade to keep dynamic
  casters; pure-RT sun would drop them).
