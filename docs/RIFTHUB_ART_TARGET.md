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
