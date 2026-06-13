# DDGI probe-grid GI — before/after proof (r_ddgi)

Classic DDGI (Majercik, McGuire et al., JCGT 2019 — built from the public paper)
on the shared ray-query TLAS: per-frame probe rays, octahedral irradiance +
Chebyshev visibility atlases, hysteresis convergence, mesh.frag replaces the
ambient DIFFUSE term (flat ambient / IBL irradiance cube) by grid confidence.
Specular stays IBL/SSR-RT reflections. Tier gate: ray query +
`VK_KHR_ray_tracing_position_fetch` (hit normals); Pascal-class devices keep the
existing ambient path byte-for-byte.

All captures from the same `feat/ddgi` Release build. "off" = default
(`r_ddgi 0`), "on" = `--ddgi` (probes settle 120+ frames before capture).

## THE GATE SHOT (`--screenshot-ddgi`)

A sealed two-room rig: room A holds the ONLY lights (a warm point light + an
emissive ceiling panel); room B connects to A through a single doorway; room C
sits beside A behind a FULL wall (the leak canary). Sun is parked below the
horizon — every photon in these shots comes from the room-A sources.

| file | verdict |
|---|---|
| `ddgi_corridor_off.png` | Room B under the engine's FLAT ambient: a uniform blue wash everywhere — fake light with no source. |
| `ddgi_corridor_on.png` | **The money shot.** Room B goes genuinely dark; warm light spills through the doorway only — doorframe, floor pool with real falloff, warm gradient up the near wall. The red accent wall in A reads through the opening. Honest traced bounce, no screen-space anything. |
| `ddgi_leak_off.png` / `ddgi_leak_on.png` | Sealed room C, wall-adjacent to bright room A. With DDGI ON it is **pitch black** — the Chebyshev visibility weighting statistically rejects every probe direction that crosses the wall. mean abs pixel delta off→on = 64.9 (the fake ambient removed), max-side bleed = none. |
| `ddgi_probes_debug.png` | `r_ddgi_debug 1`: the raw interpolated irradiance field (warm gradient flooding B from the doorway, dark corners). `2` shows grid confidence. |
| `ddgi_emissive_only.png` | **Dynamic proof:** the point light is removed mid-run; over ~1–2 s of hysteresis the probes re-converge to the EMISSIVE PANEL as the only GI source — the doorway spill survives, dimmer and panel-toned. Emissive/sun changes propagate by construction (hits shade per-object emissive from the live ObjectData SSBO). |

## Scene stills

| file | verdict |
|---|---|
| `level1_ddgi_off/on.png` | Level-1 detention cell. OFF: the uniform blue ambient lift. ON: real darkness with the cell fixture's bounce carrying the door panel — moodier and honest (auto-fit grid 24x8x24, ~2 m spacing). |
| `showroom_floor2_day_ddgi_off/on.png` | Showroom 2nd floor, DAY. mean abs delta 0.08 — **non-destructive**: in a wide-open, sky-dominated interior the traced field agrees with the IBL irradiance (the showroom auto-fit spans the whole 240 m world, ~10 m probe spacing). The win is enclosed spaces; the no-regression here is the point. |
| `showroom_day_ddgi_off/on.png` | Spire exterior day sanity: mean abs delta 0.39, no artifacts, trees/terrain stable. |

## What shipped (numbers)

- Probe grid: configurable + auto-fit (AABB over static draw-record origins,
  padded, clamped 240 m/axis). Level-1 fit: 24x8x24 probes over 46x45.5x46 m →
  2.0 x 6.5 x 2.0 m spacing. Gate rig: explicit 20x6x20 over 19x6x17 m.
- Rays: `r_ddgi_rays` 96/probe/frame default (gate rig used 128), spherical
  Fibonacci + per-frame uniform random rotation (Shoemake quaternion).
- Atlases: irradiance 8x8 texels/probe (6x6 interior + oct-wrap border),
  RGBA16F; visibility mean/mean² 16x16/probe (14x14 + border), RG16F,
  weight = cos^50, distances clamped to 1.5x spacing diagonal; backface hits
  store negative 0.2x distance (probe-in-wall kill).
- Hit shading: TLAS `instanceCustomIndex` → per-frame ObjectData SSBO row
  (160 B stride untouched) → `albedo * (sun N·L x shadow ray + point lights) +
  emissive + albedo * prevProbeField` (infinite bounce, gain 0.95); hit normal
  from `rayQueryGetIntersectionTriangleVertexPositionsEXT` (BLAS built with
  ALLOW_DATA_ACCESS). Miss = IBL env cube (mip 2).
- Hysteresis: 0.97 irradiance / 0.98 visibility, with a cumulative-moving-
  average warm-up ramp (h = n/(n+1) capped) + an intensity fade-in over the
  first 16 updates — cold probes never read as "ambient removed".
- GPU cost (RTX 5090, shared with a concurrent engine instance — treat as
  indicative): gate rig (2400 probes x 128 rays, small TLAS) ≈ 0.2–0.5 ms for
  rays+update; Level-1 (4608 probes x 96 rays, 8.5k-instance TLAS) ≈ 2.4 ms
  (whole-frame timestamp delta 9.66 → 12.10 ms in adjacent runs).

## Honest notes

- v1 traces the STATIC TLAS (same as RT AO/reflections): skinned characters
  don't occlude or bounce GI yet (documented next tier).
- Glass instances are opaque to probe rays (BLAS is opaque-only).
- Probes converge over ~1–2 s by design; lights teleporting leaves a brief
  trailing field (TAA + hysteresis hide it).
- The showroom auto-fit is world-sized (coarse). Tight interior volumes can be
  passed explicitly via `DdgiParams` origin/size (the gate rig does).
- Suite/validation: full `--test-*` suite green; Release + Debug `--smoketest`
  and `--test-ddgi` 0 VUID, `allocationCount=0` (see the branch report).
