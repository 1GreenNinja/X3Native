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

## ROUND 6 (2026-07-11) — EVERY MEMBRANE STATE IS THE OWNER'S REAL FOOTAGE
> Owner, on the live build: **"The swirling one looks fake.. Why the dot in the middle?"**

Both complaints were the SAME disease as the round-5 "crayon lightning": procedural
fakery layered on top of — or instead of — the reference video.

### 1. The OPEN state was hand-coded math
IDLE played the baked reference-video flipbook; OPEN swapped to `makeThroatRGBA()`, a
polar ridged-noise spiral. Real footage standing next to hand-drawn math — the eye picks
the fake instantly. **The video already CONTAINS the throat.** Frames extracted and
eyeballed across the full 10 s arc:

| span | what it actually shows | atlas |
|------|------------------------|-------|
| t 0.00–3.20 | lightning webs over a calm nebula | `membrane_flipbook.png` (round 4) |
| t 6.40–8.30 | the vortex ring collapsing into the throat | `membrane_flipbook_surge.png` **(new)** |
| t 8.40–9.95 | the settled radial-streaming throat | `membrane_flipbook_open.png` **(new)** |

The surge span ENDS where the open span begins → they hand off frame-continuously. All
three share the same disc crop (629,420 r214), so the membrane never jumps scale on a
state swap. `tools/make_membrane_flipbook.py` gained `--loop-blend N`: the SURGE bakes
with 0 (a ONE-SHOT film — it must keep its true final frame), IDLE/OPEN keep the 8-frame
tail blend for a seamless modulo loop.

Engine: ONE playback path for all three states (`loadFlipbookAtlas` + `flipFrameIndex`).
IDLE/OPEN loop at 18 fps with a per-portal phase; SURGE is played once against the
kawoosh's LINEAR progress (not the exponential brightness envelope, which would stall the
film on its last frames). `makeThroatRGBA` / `makePlasmaRGBA` survive **only** as the
missing-atlas fallback (fresh clone with LFS stubs → a lit blue membrane, never a black
disk).

### 2. The fake center dot
A v1 leftover: two bright blue-white disks (`coreEnt` / `coreInnerEnt`) drawn at the exact
ring center, ON TOP of the footage — plus a `exp(-r*r*9)` hot-center burst baked into the
procedural throat. All deleted (entities, geometry constants, emissive pulse, cap). The
footage carries its own center. **The blue POINT LIGHT each gate casts into its bay stays**
— that is lighting in the room, not a sprite on the membrane.

### 3. Footage-calibrated values (found by LOOKING at the shots, not by declaring victory)
- `kPlasmaEmBaseOpen` 1.90 → **1.58**: 1.90 was tuned for the dim procedural map; on the
  bright footage it lifted the video's deep-navy channels to mid-blue and washed the throat
  toward cyan-white — destroying the contrast we went to the video for.
- `kPlasmaSpinOpenX` 2.6 → **1.15**: the fast spin existed to make the procedural SPOKES
  stream. The footage streams on its own; spinning it fast on top reads as a rotating
  texture (i.e. as fake).

### Gates + shots
`--test-rifthub` **15/15** (T1/T5/T6/T8 updated for the dead core disks; **T12** = OPEN
plays the open atlas when present and falls back gracefully when absent; **T13** = the
SURGE film runs once then hands back to the OPEN loop). `--smoketest` and `--smoketest
--world rifthub`: exit 0, 0 VUID, allocationCount=0. Shots
`docs/screenshots/rifthub/I_*.png` (idle / open front+close+quarter+behind+wide / mid-surge).
New headless hook `X3_RIFTHUB_SURGE=1` freezes a shot mid-kawoosh (`X3_RIFTHUB_OPEN=1`
still lands the settled OPEN state).

### Honest read
The open gate is now unmistakably the same material as the idle gate — same grain, same
lightning, same blue — because it IS the same footage. Nothing hand-drawn is composited on
the membrane in ANY state. Remaining membrane gap vs the video: our disk is a flat emissive
sheet, so it has no depth/parallax down the throat (a real see-through vista still needs a
render-to-texture portal view), and the surge's white-hot peak sits close to the cap.

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

## ROUND 7 — THE GATE IS *ONE LARGE METALLIC TUBE* (Tim, 2026-07-11)
> "the gate.. is supposed to be ONE LARGE metallic Tube"

THE CORRECTION that supersedes the round-5/6 gate: the gate is NOT an assembly of 233 bolted-on
parts (coil cans, capacitor banks, actuator rods, pipe rails scattered around a ring). That reads
as cluttered scaffolding. It is **ONE MONOLITHIC RING** — a single, large, heavy METALLIC TUBE
(a thick round-cross-section torus) that dominates by mass and simplicity.

RULES for the rebuild:
- ONE primary form: a big, thick, round metallic tube/torus. Generous radius, heavy wall — it
  should read as a single cast/machined object, not a kit.
- Detail is INTEGRATED INTO the tube, not bolted onto it: chevrons/clamps RECESSED into the tube's
  surface, panel seams and glyph bands wrapping the tube, bolt rings following its curve, subtle
  segment joins. Think one object with features cut into it.
- Anything that is not the tube is MINIMAL and subordinate: a base/cradle it sits in, and a few
  cables/conduits feeding in. No forest of floating cans and rods.
- The tube's ROUNDNESS is the whole visual payoff: a long specular highlight sweeping around the
  torus is what makes it read as massive machined metal (this is why round beats square).
- Material: heavy weathered metal (the SD/img2img-forged sets), lit HONESTLY (see the engine
  N.L / GLB-lighting fix) — bright top surface, dark underside, highlight along the tube.
Reference: docs/reference/ portal stills — note the ring is a single massive body; the greebles
sit ON it and never outnumber it.

### ROUND 7 addendum (Tim): "we can have all the grok imagined stuff on it"
ONE TUBE does NOT mean bare/plain. It means one PRIMARY MASS wearing ALL the Grok reference's
richness. The detail gets there two ways — neither of which is a forest of separate meshes:
1. IN THE TEXTURES (the main channel): the img2img sets forged FROM Grok's own image carry
   rivets, panel seams, plate joins, vents, rust streaks, warning stencils — baked into the
   NORMAL/HEIGHT maps. Huge apparent detail on a smooth tube, zero extra polygons. Most of the
   reference's richness is SURFACE, not silhouette. This is why the img2img forge matters.
2. CUT INTO THE TUBE (geometry): chevrons/clamps RECESSED into its face, glyph/indicator bands
   wrapping it, bolt rings following its curve, segment joins, vent slots — features OF the tube.
Subordinate objects (cradle, a few feed cables) stay minimal. The tube's sweeping specular
highlight must never be broken up by clutter.

### ROUND 7 addendum 2 (Tim): "No chevrons needed"
DROP THE CHEVRONS entirely. This is NOT a Stargate-franchise replica — it's OUR industrial
portal generator. No chevron locking clamps, no amber chevron slits (they were a hangover from
the earlier Stargate-inspired direction and have been a persistent source of the toy look).
The tube's surface detail = the img2img-forged plate/rivet/seam/vent/stencil richness + any
recessed bands/joins that read as machined. Indicator lighting, if any, stays subtle and
integrated (recessed slits), never a ring of amber triangles.

## ROUND 8 — THE TUBE + THE CONSOLE (Tim, 2026-07-11) — BUILD THIS
> "build the tube, with an lcd panel on it, some buttons, glowing led displays.. and also have
> the signs in front of each portal relocated to a glass holoterminal, black glass with the blue
> and green text, showing where each portal goes. let the user interact with each portal...
> they can do wonderful or disasterous things with the console to it"

### A. THE GATE = ONE LARGE METALLIC TUBE (see R7 + addenda)
One massive machined round tube. NO chevrons. Grok richness lives in the img2img-forged
normal/height maps (rivets/plates/seams/vents/rust/stencils), plus recessed features cut INTO
the tube. Minimal subordinate geometry (cradle + a few feed cables). One long specular highlight
sweeping the body = the payoff.

### B. ON THE TUBE: an operator panel
- An LCD/display panel set INTO the tube's face (recessed, with a bezel).
- Physical BUTTONS (a small cluster — chunky, with travel/bevel).
- Glowing LED indicator displays / readout strips (small, integrated, recessed; NOT a ring of
  amber triangles). Subtle emissive.

### C. THE PORTAL CONSOLE (replaces the floating teal sign rectangles)
The current per-portal "sign" (a floating flat teal rectangle) is DELETED. In its place, in front
of each portal, a proper HOLOTERMINAL matching the project's established holo design language:
- BLACK GLASS slab, glowing BLUE/GREEN text (per Tim's canonical holo-terminal spec: black glass
  slab, blue/green/orange status text, shiny metallic ROUND-PIPE frame around the glass, single
  support pipe up to the ceiling so it HANGS rather than floats — reuse the HoloPanel/holo-terminal
  platform already in the engine).
- Content: WHERE THIS PORTAL GOES (destination name/world, status, coordinates/readout flavor).

### D. GAMEPLAY: the player can INTERACT with each portal console
Reuse HoloTerminalSystem (app/holo_terminal_system.*) — already proven: placeable kiosks, code
entry, real world effects (cell_lock/armory_door/lights_central/alarm_armory/lore_intel/
crate_dispense). Point it at the RIFTS. The player walks up, [E] to use, and can do
"wonderful OR DISASTROUS things":
- WONDERFUL: open/stabilize the rift, re-target it to another destination, boost power (bigger,
  brighter portal), unlock a hidden destination, run a diagnostic that reveals lore.
- DISASTROUS: overload/destabilize it (violent surge, alarms, hall lights fail), mis-target it
  (sends you somewhere hostile), collapse it (that rift goes dark/unusable), trigger a
  containment breach (something comes THROUGH).
Design the command set as DATA (like the existing terminal effects) so more can be added. Make
consequences REAL and visible in the hub (lights, audio, the membrane's state/colour, alarms).

### ROUND 8 addendum (Tim): THE CONSOLE HAS *VALUES*, AND THEY HAVE CONSEQUENCES
> "changing certain values will warp the room, and others will cause a temporal rift .. others
> an implosion"

The console is not a menu of commands — it is a set of TUNABLE PARAMETERS the player dials in,
mad-scientist style. Safe ranges do useful things; wrong combinations do spectacular things.
Suggested parameter set (data-driven, extensible):
  POWER / FREQUENCY / PHASE / APERTURE / CONTAINMENT / TARGET
Outcome classes (each must be REAL and visible — geometry, lights, audio, membrane state):
- NOMINAL: portal opens/stabilises; re-target to another destination; brighter/bigger aperture.
- ROOM WARP: the hub geometry visibly WARPS — space bends around the player (FOV/lens distortion,
  the hall's walls/floor bow and ripple, props drift). Disorienting, survivable.
- TEMPORAL RIFT: time distorts — slow-motion, stuttered/echoed motion, ghost-echoes of the player
  and props, audio pitch-bend/reverse. Possibly duplicate/after-image entities.
- IMPLOSION: violent collapse — the membrane inverts and sucks inward, debris and props are
  dragged toward it, a shockwave, damage to the player, that gate goes DARK/dead afterwards.
- (Keep room for more: containment breach = something comes THROUGH.)
Rules: consequences are persistent where it makes sense (a collapsed gate stays dead); alarms +
hall lighting react; the membrane's colour/behaviour reflects instability BEFORE it blows (the
player should be able to read danger building). Data-driven so new parameter/outcome combos can
be authored without code.

## ROUND 8 STATUS (2026-07-11) — what SHIPPED, and what did not

### The tube (A)
`tools/build_rifthub_gate.py` rewritten. ONE swept mesh (bmesh, not a bpy torus):
the minor radius is a FIELD `rr(u,v)`, so the detail is CUT INTO the single surface —
12 segment seams, 2 longitudinal recessed bands, the indicator groove, 7 vent grilles
(4 louvres each), and the bevelled operator bay. **73,728 tris of tube**; 4,220 steel +
1,528 dark + 2,188 panel = **81,664 total**, 47 objects (R5 shipped 233).
**Zero chevrons. 0/73,728 inside-out.**
- Two bugs found and fixed on the way: (1) `EDGE_SPLIT` disconnects deep cut features
  into their own islands, and `recalc_face_normals` then INVERTS them — 100% of the
  groove, 4,096 tris. The sweep's winding is outward by construction, so the tube is
  now exempt from the recalc. (2) A vent slot narrower than one minor step aliases
  into spikes; NSEG went 96 -> 144 and the slots were widened.
- A centroid-based inside-out test is MEANINGLESS on a torus (the tube's inner half
  legitimately faces the object centre). The check now tests each face normal against
  the tube's own analytic axis.

### The surface (C)
`gate_tube_hull` + `gate_tube_stencil`, SD3.5 img2img from the owner's reference frame,
normal strength 21 (~2x the R5 sets). Rivets, plate joins, weld beads, vent slats, rust
runs and stencils live in the normal/height — a smooth tube reading dense. Landed local
+ `G:\Assets\X3Native\surface_library\`.
- **CROP LAW (learned the hard way):** the reference frame is a LIT SCENE with a BLUE
  PLASMA MEMBRANE in the middle of it. The first pass clipped it and baked a glowing
  blue smear into an ALBEDO — plasma painted onto metal, permanently, in a map that is
  supposed to be reflectance. Both crops now sit on the ring's left flank.
- **VALUE:** the tube's albedo had to be renormalized to ~0.60 sRGB. Below that it
  silhouettes to black against its own membrane (the brightest thing in the hall drives
  auto-exposure down and takes the metal with it). Adding light does NOT fix this —
  auto-exposure just eats it; measured, 5x the key barely moved the pixels.

### The panel (B) and the consoles (C/D)
LCD + a 4x2 cluster of chunky travel buttons + LED readout strips, recessed into the
tube's bay, shipped IN the GLB as two emissive groups — the only emissive parts of the
gate. The floating teal signs are deleted; each rift now has a HoloTerminal hanging in
its approach (black glass, round-pipe frame, support pipe to the ceiling), off the
centreline so it does not occlude the gate it operates.

### The gameplay (E)
Six parameters, a DATA outcome table (`kRiftRules`), typed TARGET (world name =
re-target; override codes SINGULARITY/CHRONOS/MOBIUS force outcomes the sliders cannot
reach). Glowing sliders/knobs/text-fields (`ui.h`: glowSlider/knob/textField), each
shifting blue-green -> amber -> red and pulsing harder with ITS OWN danger.
`instability()` is continuous and the whole machine reads it — controls, the tube's LCD
and LEDs, the membrane's hue, the gate's key light. The rift snarls under your hand.

| outcome | state |
|---|---|
| NOMINAL (open / re-target / wider aperture) | **LIVE** |
| IMPLOSION (membrane inverts, debris dragged in, shockwave, gate DEAD forever) | **LIVE** |
| ROOM WARP (hall props ripple + lens breathes; restored exactly) | **LIVE** |
| TEMPORAL RIFT (slow-mo + real stutter) | **LIVE** |
| CONTAINMENT BREACH | **STUBBED** — rule/alarm/destabilise wired; nothing walks out yet |

### KNOWN DEFECT (not fixed this round)
The hanging console's WORLD-SPACE GLASS renders as a featureless dark-blue slab: the
baked readout text does not show. `HoloTerminal::update()` IS now being called (it was
not — `setLines` only marks the glass dirty), so the texture regenerates; the suspicion
is that the engine's translucent-GLASS pass does not sample `Entity::tex`. The 2D
control surface the player actually operates is unaffected. Next session: check the
glass pass, or drop `transparent` on the readout quad.

### Honest read vs docs/reference/
Silhouette: **hit.** It is unmistakably ONE large machined tube, heavy and round, with
one unbroken specular sweep — not a kit. Surface: **close.** The forged maps put the
reference's rivet/plate/seam/rust density on it, and it reads at playing distance.
Value: **the remaining gap.** The reference's ring has BRIGHT weathered white-grey top
plates over a dark belly; ours is dark blue-black machined metal throughout. The crop
that fed the hull came from the ring's darker flank. A re-forge from the reference's
bright upper plate band is the next single highest-value move.
Dressing: the legacy engine conduits (orange sticks + coil rings) now read as bent coat
hangers hanging off a tube they were never designed for, and the cradle/cable cluster
under the gate is a black spiky blob at some angles. Both want a pass.

Shots: `docs/screenshots/rifthub/K_1_tube_front.png`, `K_2_tube_quarter.png`,
`K_3_panel_close.png`, `K_4_console_hanging.png`, `K_5_implosion.png`.
