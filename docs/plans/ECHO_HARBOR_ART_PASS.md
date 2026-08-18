# W-ECHOART — the Echo Harbor art pass
*Filed 2026-08-17 by the session lead at the owner's direction, after he flew
`--world echotropolis` and photographed it. **Two sessions are editing Echo
Harbor RIGHT NOW** — this brief is written to be handed to one of them (or run
when they are clear), NOT to be raced against them. Coordinate on the fleet
channel before touching these files.*

**References (committed):**
`docs/design/ECHO_WATER_SHARDS.png` — the P0 defect, dead centre.
`docs/design/ECHO_FOG_FLATTENS.png` — the horizon dissolving into milk.

## The honest framing
Echo Harbor has **excellent bones and no art pass.** The road and city topology
is genuinely strong — a real cloverleaf interchange, a banked elevated freeway
on hammerhead piers (`app/world_hosts/echo_roads.{h,cpp}` is better road
engineering than most shipped city games), a coastal town on a proper grid, a
bay, streetlights, crosswalks — all at 74-157 fps. Nothing below asks for new
technology. Every fix already exists somewhere in this engine and needs
*pointing at this world*.

---

## P0 — THE WATER. The owner has asked for this "about a dozen times."
`docs/design/ECHO_WATER_SHARDS.png`: the sea is a flat saturated cyan plane
**littered with dark navy angular polygon shards** — hard-edged chips scattered
across the surface that read as debris or broken geometry floating on the bay.
This is the single most damaging thing in the world and it is the FIRST thing
to fix; nothing else in this brief matters until the water stops looking broken.

- **Find what draws those chips** before theorising: they are almost certainly
  wave-crest or foam quads (hard-edged, unblended, possibly backface-visible)
  rather than a shading artifact. Grep `echo_water.{h,cpp}` (Gerstner swell
  presets) and whatever submits the crest/foam geometry.
- **This is the SHARD FAMILY.** The driving world shipped exactly this defect
  in its sky and it was convicted and killed today: hard angular silhouettes
  where something soft belongs. The cure there — read it, the lesson transfers
  — was a **sin-free hash** in `inc/sky_clouds.glsl` plus soft optical alpha
  instead of hard geometry. If these chips are noise-driven, suspect the same
  sin-based hashing; if they are quads, they need soft alpha, camera-facing
  orientation and a proper depth fade.
- While you are in the water: the cyan is over-saturated and flat — no depth
  gradient, no shore foam, and the shoreline meets the sand with a hard line.
  The driving world's river solved all three (bounded water, depth-graded
  colour, a real waterline) — `app/river_bridge.cpp` and the `WaterParams`
  work (deep/shallow colour, clarity, fresnel, horizonColor) are the reference.
  **`WaterParams::horizonColor` unset is a known seam bug** — the patch fades
  to raw analytic sky at its rim.

## P0b — THE ROADS. Owner: "the Horrific awful roads."
The road *topology* is excellent; the road *surface* is not. Diagnose which of
these it is before changing anything (capture at drive height AND from
altitude, since they look completely different):
- Untextured or flat-tinted carriageway (rule 3) vs. a real asphalt material.
  The driving world uses `rd_asphalt_01` from `assets/surface_library` with
  albedo + normal + MR; `app/road_network.cpp`'s ribbon shows the wiring, and
  it FALLS BACK to a flat tint when the surface set is missing — which looks
  exactly like "awful roads" and is a one-line asset problem, not a shader one.
- Lane paint: stamped decals vs. geometry, z-fighting, wrong width, tiling.
- Jointed bends — the driving world's law is "get rid of ALLLLL jointed bends,"
  solved by a Catmull-Rom render path at a 1.5 m station floor
  (`buildRoadRenderPath`). If Echo's roads facet visibly on curves, that is the
  fix and it is already written.
- Aprons/shoulders/kerbs: a ribbon that ends in a hard edge against terrain
  reads as a decal on grass. `road_network`'s prism + batter + apron-skirt is
  the vocabulary.

## P1 — THE FOG IS EATING THE WORLD
`ECHO_FOG_FLATTENS.png`: the entire horizon — bay, hills, sky — converges to
one cream value, flattening every silhouette. Cheapest big win in the brief:
- Pull distance-fog density down; give the fog a colour that is NOT the sky's,
  so depth reads as depth instead of dissolve.
- **Turn on DDGI** (`r_ddgi 1`, `r_ddgi_intensity`, `r_ddgi_rays` — built and
  defaulted OFF engine-wide; see `docs/design/RAYTRACING_SCOPE.md`). Ambient is
  currently a flat constant with no occlusion, which is why nothing has weight.
  Ray-query reflections are already on by default.
- The driving world's soft cumulus sky + cloud shadows (`inc/sky_clouds.glsl`,
  `cloudShadowFactor` in `mesh.frag`) never reached this host. Wiring them in
  gives the sky structure and the ground moving shade for ~4% frame time.

## P2 — BUILDINGS ARE UNTEXTURED BOXES
Blue/grey/white prisms with window-dot decals. Defensible as altitude proxies
in a city sim, indefensible up close (rule 3), and it is what the eye lands on.
- The town lane just did this exact swap in the driving world: the placement
  machinery is asset-agnostic, so it was **two tables** and 6.5 MB of real
  facades (see `docs/design/TOWN_MANIFEST.md` §8 and `TOWN_ASSET_SCOUT.md`).
- **Measure before believing a pack**: that lane found `Medium_Building_*` are
  45-76 m office blocks, `Tower_*` are 464-618 m skyscrapers, and
  `Buildings_Night/Building_1..7` are EMPTY MESHES (0×0×0). Use
  `tools/glb_contact_sheet.py` — a dependency-free rasteriser that renders a
  whole kit in seconds. It is what caught both the ruined houses and the
  clawed mutant. **The eye gate is only as good as how cheap it is to look.**
- Search all 914 owned packs: `python tools/unitypackage_index.py --search ...`

## P3 — SMEARED CLIFFS AND NUCLEAR GRASS
- The mesa and cliff faces show horizontally banded smearing: planar UVs
  dragged down a vertical face. Needs **triplanar projection above ~40°** —
  the mountain lane's slope-based cliff banding in `mesh_terrain.glsl` is the
  precedent, and `assets/surface_library` already publishes `terrain_rock`,
  `terrain_bluff_dark`, `terrain_bluff_clay`, `fw_rock_cliff`.
- The grass is over-saturated, visibly tiling, and speckled with coloured
  confetti reading as litter rather than flowers. Desaturate, break the tile,
  and either fix the detail-billboard scale/colour or drop them.

---

## GATES
Build green; boot `--world echotropolis` with zero `[ERROR]`; that world's own
suites green; fps within 10% of its current numbers (74-157 depending on the
view — record which camera). **Eyes-on FULL-RES before/after PAIRS read by the
agent itself** for each of P0, P0b, P1, P2 — same camera, same time of day.
The water pair is the one the owner will look at first.

## COORDINATION — READ THIS BEFORE STARTING
Two sessions are live in Echo Harbor right now. `echo_core/` is **FROZEN** (F0
complete, parity proven — do not "finish" or refactor it; see
`D:\GameDev\SimCityLLM2\docs\COORDINATION.md` and `docs/F1_PLAN.md`). Announce
the lane on the fleet channel and agree file ownership BEFORE editing
`echo_water.*`, `echo_roads.*` or the host. Commit at every green step; never
push — the integrator merges.
