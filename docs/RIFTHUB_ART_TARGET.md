# RIFTHUB ART TARGET — the fable-rock gate (from Tim's reference image, 2026-07-11)

_Tim supplied a concept reference (industrial Stargate) that supersedes the "clean sci-fi
gate" look of the v1 build. This doc is the durable art brief. Current state (v1, steps 1-6):
smooth torus + blue core light + white-hot membrane + flat yellow chevrons + colored floor
plates on a checkerboard dev floor. Tim's verdict: gates read, but membranes are blown-out
white flashlights, chevrons are party triangles, and the hub is a tech demo, not a place._

## Design language extracted from the reference
1. **RING — industrial, not minimal.** Heavy riveted steel plates, weathered, TEAL-OXIDE
   patina over dark metal. Segments vary in depth/profile (break the uniform torus with an
   over-plate segment pass). Greebles: locking clamps, pistons, brackets, vents, bolted seams,
   small conduit stubs. The 9 chevrons become chunky metal CLAMP HOUSINGS seated into the
   ring, with only a small amber-lit core (not whole-shape emissive).
2. **CRADLE — the gate is INSTALLED.** A-frame support legs, floor anchor plates, base
   skirt. No floating ring.
3. **MEMBRANE — a plasma storm, not a disk.** Tim's SECOND reference locks the palette:
   **DEEP BLUE** plasma base (classic Stargate — honors the task's "blue light" spec), white
   lightning filaments crawling the surface (borrow the lightning-gun forked-bolt FX), NOT
   white-clipped: cap the emissive so the blue always reads. Fresnel-bright rim, spark/mote
   particles drifting off. SEE-THROUGH: a parallaxed vista texture inside the membrane
   (another world glimpsed through it); true render-to-texture portal view is the future
   upgrade.
4. **DRESSING.** Glowing conduit tubes (orange + teal emissive pipes) running gate→floor,
   coil rings on a piston, 1-2 holo data screens near each gate (holo-terminal tech exists).
5. **ENVIRONMENT — the hub becomes a HALL.** Industrial: concrete + steel beams, hanging
   catenary cables, ceiling strip lights, fog/haze (club fog), wet reflective floor (replace
   the checkerboard), distant machinery silhouettes. Palette LOCKED by Tim (reference 2, confirmed 'this is what we want'): BLUE membranes (key light), ORANGE conduits/accents, TEAL holo screens + patina;
   each gate's membrane is the key light of its bay.

## Pipelines to reuse (all proven in-repo)
- **SD3.5 surface forging** (18 forged PBR sets landed on feat/intro-cockpit; forge new
  rusted-steel / teal-patina / concrete sets — one SD gen at a time on the 5090).
- **Forked-bolt lightning FX** (lightning gun) → membrane tendrils.
- **Ember/spark particles** (fx system), **club fog**, **wet-floor reflections**,
  **holo-terminal screens**, **emissive tube prims**.

## Order of attack
A. Membrane v2 (fix the blown-white NOW: teal base, capped emissive, fresnel rim, tendrils,
   embers, vista parallax) — biggest visual win.
B. Ring v2 (plate/greeble pass + clamp-housing chevrons + cradle + forged patina textures).
C. Hall environment (floor/beams/cables/fog/lights).
D. Dressing (conduits, coils, holo screens) + final light balance (teal key / orange accent).
Gate each phase: build + smoketest clean, screenshot for Tim's eyeball, commit+push per phase
(crash durability — this box's KernelPower record).

## Audio status: hum/kawoosh/whoosh landed in v1 (step 5) — Tim has not yet confirmed how
they sound in-game; revisit levels after the visual pass.

## MEMBRANE ANIMATION ARC (from PortalAnimated.mp4, 2026-07-11 — docs/reference/)
Three states, mapping onto existing gameplay states (dormant / kawoosh / traverse):
1. IDLE — calm deep-blue nebula: wispy filaments, star motes, the VISTA faintly visible
   through the surface, slow drift. (membrane_state1_idle.png)
2. ACTIVATION SURGE — electric arcs form a VORTEX RING around the rim, whipping the inner
   circumference; membrane brightens; the ring's inner track segments light amber.
   (membrane_state2_surge.png)
3. OPEN / THROAT — full radial plasma streaming from center (looking down the wormhole
   throat); vista dissolves into pure energy. (membrane_state3_open.png)
Ring detail confirmed by the video: the INNER-FACING edge carries a segmented amber-lit
ratchet track that brightens with activation.

## ROUND 3 — THE DENSITY ROUND (Tim greenlit 2026-07-11: "We can do this — Fable Style")
Round 2 verdict: "Much better.. but nothing close to the grok source image." The ceiling of
box-assembly prims is reached. Round 3 closes the gap with real assets + atmosphere:
- LANE 2 (now, CPU-only): procedurally AUTHOR the gate in headless Blender (bpy script —
  stacked ring plates, pistons, pipe runs, chamfered clamp housings, bolt detail), export GLB
  with UVs, engine loads it replacing the procedural torus/plate/clamp assembly. Membrane +
  3-state machine + ratchet/audio stay untouched.
- FAKE VOLUMETRICS (now): additive translucent cones under fixtures + shafts from ceiling
  lights; a touch more mid-floor illumination.
- LANE 1 (queued for GPU-free window): SD3.5-forge gate-SPECIFIC texture sets with greebles
  baked into normal/height (riveted ring plate w/ rust streaks, dark piston steel, patina
  variants) -> G:\Assets\X3Native\surface_library. One gen at a time on the 5090; runs when
  Tim is out of the game.
Blender pipeline gotchas (proven in-repo): Microsoft-Store Blender — blender.exe is
ACL-denied, use the blender-launcher.exe alias, which DETACHES (no stdout): report via files
+ poll for a .done marker; keep helper .ps1 ASCII-only. See tools/ for existing headless
patterns (dance_bake.py etc.).

### ROUND 3 STATUS (2026-07-11, this session)
- LANE 2 LANDED: tools/build_rifthub_gate.py (bpy, 2 refinement iterations) authors the
  25.5k-tri gate -> assets/converted_glb/rifthub/gate_ring.glb (LFS); tools/gate_build.ps1
  = launch+poll driver. rifthub.cpp instances it 8x as Scene entities (3 material-group
  nodes gate_patina/steel/dark -> curated sets), procedural ring/clamps/cradle now the
  graceful fallback (self-test T9). Conduits re-authored as lower-flank hairpins hugging
  the new rim; identity trim dimmed.
- FAKE VOLUMETRICS LANDED: additive dust-billboard shaft columns (glass cones FAILED —
  the glass fallback path lifts alpha by fresnel, solid sails at grazing; documented
  in-code) under all 8 fixtures + 2 angled rakes, + 4 deck-fill lights between bays.
- LANE 1 PREPPED (not run): tools/forge_gate_textures.py + docs/FORGE_GATE_TEXTURES.md —
  owner fires `python tools/forge_gate_textures.py --all` when the 5090 is free, then
  lands the sets per that doc.
- Owner shots: docs/screenshots/rifthub/F_3_shafts_{wide,close,quarter}.png (history:
  F_1 first GLB, F_2 density iter 2).
- Honest read vs the reference stills: geometric density is finally in the same family
  (~7/10; round 2 was 6.5) — the remaining gap is BAKED surface detail (the queued forge),
  membrane richness, and photoreal grade, not silhouette.

## ROUND 4 STATUS (2026-07-11, "steal Grok's pixels")
- J1 GHOST-GLASS FIXED (commit after F_3; shots G_1_gatefix_*): the translucent
  gate was NOT alpha/blend/depth — the GLB exports opaque. It was the SSR/RT
  REFLECTION pass: the curated sets' polished MR (rough .25-.45) sat inside
  mesh.frag's mirror gate, and the half-res reflection march is wrong on the
  GLB's dense thin plates (2 m steps + 0.5 m thickness tunnel through them), so
  the gate's specular showed the bright emitters BEHIND it (X-ray). Fix =
  per-group 1x1 weathered MR overrides (rough just past the 0.6 cutoff, each
  set's metallic character kept). G_0_repro.png preserves the broken state.
- J2 MEMBRANE FLIPBOOK LANDED (G_2_flipbook_*): tools/make_membrane_flipbook.py
  bakes 48 frames of PortalAnimated.mp4's IDLE span (t 0..3.2 s; the video's
  own arc is idle ~0-3 s / surge ~3-7 s / open ~7-10 s) -> radial-masked,
  loop-blended 8x6 atlas at assets/textures/rifthub/membrane_flipbook.png
  (+ G:\Assets\X3Native\surface_library\membrane_flipbook\). Engine slices it
  into 48 tiles at build and plays the IDLE plasma layer at 18 fps with
  per-portal phase; slow rotation stays under it; SURGE arcs/env + OPEN throat
  swap unchanged on top; paler flip tint (video carries its own blue); caps
  law intact. Missing atlas -> procedural nebula (self-test T10 covers both).
- J3: deck fills +33% (owner's "tiny bit more light for that fantastic floor").
- Gates: --test-rifthub 12/12, both smoketests exit 0 / 0 VUID / alloc 0.
- KNOWN NIT: the crop keeps a faint sliver of the video's own ring track at the
  disk's right edge (rotates with the disk; reads as membrane texture in
  motion). Re-bake with --cx/--cy/--cr to tighten if it bothers the eyeball.
- OWNER EYEBALL NEEDED LIVE: the flipbook in MOTION (loop cadence + seam) —
  stills can't prove it.

## ROUND 5 — STOP HAND-CARVING IT. GENERATE IT. (Tim, 2026-07-11)
> "how hard can it be to generate something when we KNOW what it should look like."

He's right, and rounds 2-4 took the wrong road: the gate was hand-assembled from procedural
prims (torus + boxes + bpy plates) and then chased with tints and greebles. But this project
ALREADY has an image->3D pipeline: **Rodin AI** (G:\GameModels\rodin_glb\ — ~65 characters,
enemies, and the Lightning/Chain gun weapons were all Rodin-generated from images; owner-owned,
per docs/ASSET_INVENTORY.md).

THE PLAY: feed the OWNER'S OWN REFERENCE (docs/reference/ portal stills — the industrial
Stargate, and/or a clean front-on render of the SG-style ring) into **Rodin** -> download the
mesh -> OBJ/GLB convert (Blender; see the existing rodin_glb conversion note in
ASSET_INVENTORY.md) -> land as assets/converted_glb/rifthub/gate_ring.glb, REPLACING the
bpy-authored gate. The engine already instances that GLB across all 8 portals with a graceful
fallback, and maps 3 material groups -> forged PBR sets. So the swap is a drop-in: same
loader, same membrane/state-machine/audio/triggers, just a REAL gate.

Pairs with the proven texture side: SD3.5 forge (gate_ring_plate/patina/piston sets on G:) +
the Grok-video membrane flipbook. Geometry was the last hand-made link in the chain.

TODO next session: (1) generate the gate in Rodin from the reference, (2) convert + land,
(3) re-tune tiling/tints on the real mesh, (4) then judge vs docs/reference.

## ROUND 5 (2026-07-11) — THE GATE WAS NEVER LIT. Four rounds of art were invisible.
Owner reported 4 bugs (black activated portal / no swirl from behind / room too dark /
"no good looking portal surrounds") + 3 directives (kill the crayon lightning, ENLARGEN
the portal, rebuild the gate as ROUNDED PIPE with SD3.5 textures FROM his image).

### Root causes (all measured, not guessed)
1. **BLACK ACTIVATED PORTAL == NO SWIRL FROM BEHIND == ONE BUG: the VISTA disk.** An
   OPAQUE disk of the same radius parked 0.10 m outward of the OPAQUE plasma disk. From
   the hub side the plasma occluded it completely (the "parallax vista" never rendered a
   single pixel); from the far side it WAS the near surface — a near-black starscape that
   occluded the storm. The owner walks THROUGH a gate (the trigger fires 2.5 m out) and
   ends up looking at the dead vista disk. Deleted. The disk mesh was already double-wound,
   so one plasma entity now reads from BOTH sides. Locked by self-test T11.
2. **THE ROOM WAS DARK because the fills lost to the inverse-square law.** A 7.5-intensity
   light 6.8 m up delivers 7.5/6.8^2 = 0.16 to the deck. Nudging 2.4 -> 3.2 moved a number
   that already rounded to nothing. Rig rebuilt (9 overheads @ 18, 8 deck fills @ 9.2,
   per-gate key + warm under-fill). Ambient went DOWN, not up: ambient is omnidirectional,
   so it lights a room by destroying its contrast.
3. **THE GATE HAD 30-43% INSIDE-OUT TRIANGLES** (normals inverted + wound backwards ->
   backface-culled), measured off the shipped GLB. THAT is the real "ghost glass / X-ray",
   not SSR. And a **flat fake self-emissive** (no emissive map) was ~90% of the ring's pixel
   value — forcing baseColor to pure RED moved the gate by <8/255. So the ring was a flat,
   self-lit, shadowless cutout and every texture round was painted over by its own glow.
4. **THE PROCEDURAL LIGHTNING VANDALIZED THE FLIPBOOK.** The flipbook IS the owner's
   reference video; its pixels already contain real lightning. Arcs are now SURGE-ONLY.

### Landed
- `tools/build_rifthub_gate.py` REWRITTEN: rounded-pipe vocabulary (torus ring whose inner
  surface is the throat; cylinder clamp cans, torus collars, sphere joints, bevelled pipe
  curves, cable bundles, capacitor banks, trunnion cradle). Watertight primitives only +
  a signed-volume normal check => provably outward-facing.
- Membrane 1.58 -> **1.895** (+44% area). Flipbook pixels untouched (approved).
- `tools/forge_gate_textures.py`: `--img2img` / `--from-image` / `--maps-only`. Textures
  now derive FROM the owner's reference frame (1168x768 video frame > the 800x526 stills).

### NEXT LEAD (engine, unresolved)
Model-loader (GLB) meshes shade with a suspiciously low N.L versus prim meshes under the
SAME point lights: a white-albedo PBR probe on the gate reads ~0.03 where the floor reads
as predicted. That anomaly is why the gate still needs a texture-gated ambient term to
read at all. Find it and the gate can be lit honestly (and the reference's bright
weathered top plates / dark underside value range becomes reachable).

## ROUND 6 (2026-07-11) — THE ENGINE BUG, FOUND AND FIXED. It was never the normals.

### The measurement (not a theory)
A white-albedo, zero-emissive probe cube was placed at ONE world position in the hub,
lit by the SAME rig, rendered five times with only the SHADING PATH changed
(docs/screenshots/rifthub/J_0_probe_*):

| probe | mesh        | material                 | shader path | mean sRGB lum |
|-------|-------------|--------------------------|-------------|---------------|
| 1     | PRIM box    | no MR map                | dielectric  | 0.5478        |
| 2     | PRIM box    | MR metal 0, rough .5     | PBR         | 0.3771        |
| 3     | PRIM box    | MR metal .80, rough .62  | PBR         | 0.3556        |
| 4     | **GLB** cube| no MR map                | dielectric  | **0.5478**    |
| 5     | **GLB** cube| MR metal .80, rough .62  | PBR         | **0.3556**    |

Probe 1 and probe 4 are **BYTE-IDENTICAL** (same md5). So are 3 and 5. The model loader,
the glTF normals, the node transforms and the vertex format are **provably correct** —
a GLB and a prim of the same geometry shade to the same pixels. The five suspects in the
original bug report (normals not read, wrong attribute slot, missing normal matrix,
non-uniform scale, missing tangents/TBN) are all **disproven**.

### The root cause: TWO shading paths that disagree on the diffuse energy convention
`mesh.frag` picks a path per object by "does it carry an MR map" (`Scene::render` →
`drawMeshPBR` vs `drawMeshEmissive`):
* **DIELECTRIC path** (no MR) — every procedural PRIM (floors, walls, level geometry):
  `albedo * N.L * lightColor`. An UNNORMALIZED Lambert: **no 1/PI**.
* **PBR path** (any MR map) — every GLB, because `ModelLoader` synthesizes a 1x1 MR from
  the glTF factors whenever `metallic > 0`: `albedo * (1-metallic) / PI * N.L + GGX`.

Every light rig in the game was authored against the PRIM path. So under the SAME light a
GLB shaded **1/PI = 0.318x** of the prim beside it at metallic 0, and **~0.03x** once
metallic reached 0.8 — which is EXACTLY the "white probe reads 0.03 on the gate" number
measured in round 5. The gate was not badly normalled. It was correctly normalled and then
divided by PI and by (1-metallic).

### The fix (shaders/mesh.frag)
The PBR path now adopts the SAME convention as the dielectric path: the Lambert `1/PI` is
gone and `brdf()` takes the dielectric path's own diffuse weight (sun 0.75, point 1.0).
Energy still conserves — `(1-F)` hands the Fresnel share to the specular lobe. AFTER:
probe 1 = 0.5447, probe 2 = 0.5418 (**0.5% apart**, was 45% apart), probe 4 = probe 1
exactly. GLB and prim now light identically under identical lights.

### The crutches this exposes (all were compensating for the SAME bug)
Once the engine is honest, every band-aid DOUBLE-COUNTS and blows out:
1. **rifthub gate self-emissive** (`kGateAmbient`, texture-gated) — REMOVED. It blew the
   gate to a white sculpture the moment real light landed (J_2b_gate_crutch_BLOWOUT.png).
2. **rifthub gate key/fill lights** — `kGateKeyI` 46 -> 26, `kGateFillI` 20 -> 11. The 46
   was documented in-code as "NOT a typo" precisely because the gate wouldn't light.
3. **weapon viewmodel `kVmBright = 2.6`** (app/weapon.cpp) — an OVER-UNITY albedo
   multiplier that clipped the gun's own texture to white. REMOVED (now 1.0). The pistol
   went black blob -> white blob -> a correctly lit gun (J_4_level1_BEFORE/AFTER).

### Still-open (flagged, NOT fixed here)
* `app/world_hosts/host_space.cpp` — the ships are `metallicFactor=0` with no MR texture,
  so they were ALWAYS on the dielectric path and this fix does not change them. They are
  dark because the point-light rig delivers ~1/22500 of its intensity at the fleet's
  distance (inverse-square at space scale) and carries its own crutches (a `1 + 1.2*bright`
  albedo scale + a fake ambient EMISSIVE floor). The right tool there is the DIRECTIONAL
  sun, not point lights 150 m away.
* Black-silhouette metallic kit props: glTF's default `metallicFactor` is **1.0**, so any
  converted GLB whose exporter omitted it becomes a full metal — no diffuse lobe at all —
  and goes black in windowless interiors regardless of this fix. `feat/intro-cockpit`
  commit 45e1c46 band-aided this with a per-object metalness scale; that fix (or an asset
  re-convert) still belongs on the mainline.
* The engine ignores glTF's `metallicFactor`/`roughnessFactor` when an MR TEXTURE exists
  (spec says multiply). Separate defect.
* Sun/star glow composites over the ships in --world space (owner: "visibility order") —
  a transparent/additive depth-sort bug, unrelated to lighting. Its own task.
