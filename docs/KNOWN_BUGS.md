# KNOWN BUGS & LANDMINES — X3Native

**Append-only. Never delete an entry — mark it FIXED with the commit hash.**
Read this BEFORE debugging anything. Half the bugs in this project have been "fixed" 2-9 times
because nobody knew the fix already existed, or because a crutch was hiding the real cause.

---

## 🚨 LANDMINES — read before you touch these

### L1. `feat/tractor-beam` is the space superset — NOT `feat/space-wave1-integrated`
Deleting the ~19 space branches on the obvious assumption **destroys 18 branches of work**
(ship_interior / windows / repair / anim, both wormhole modules, tractor_beam, mech_pilot).
Never run a branch-delete sweep without `docs/INTEGRATION_AUDIT_2026-07-11.md` open.

### L2. A STALE LEVEL JSON ON `C:` SILENTLY OVERRIDES THE LEVEL
`level_loader.cpp` has a fallback to
`C:\GameDev\OneDrive\GameDev\DellGameDev\Escape48BLN\LevelArchitect\EscapeLab48_AllFloors_v2.project.json`
— which still exists on the 14900K **with the OLD 4×3.5×4 cell**. Any launch whose **cwd is not
the repo root** silently loads it. This is a plausible cause of "the 14900K doesn't have the
fixes" and of the tiny cell. **ALWAYS launch with the working directory set to the repo root.**
TODO: kill that fallback.

### L3. Glass writes depth in the depth pre-pass
Any glass pane BETWEEN the eye and a screen **depth-rejects the screen** — it does not blend
over it. The screen never rasterizes and you see the pane's flat fill. This ate the holo
terminals for months. The cell terminal only ever "worked" **by accident**, because its yaw
(π) pointed its bad panes *away* from the player. **Never put glass in front of a screen.**
Frames must be OPAQUE metal (coplanar rims cannot occlude). — fixed c44da59

### L4. `Scene::submit()` drops `emissiveTex` without an MR map
It only forwards `Entity::emissiveTex` on the `mrTex.valid()` PBR branch. An entity with an
emissive map but **no MR map silently loses it** and falls to `drawMeshEmissive()`. Give it a
matte MR texel. (This blew the perfshop neon sign to a white blob.)

### L5. glTF default `metallicFactor` is **1.0**
Any GLB whose exporter omitted it becomes a **full metal** — and full metal has **no diffuse
lobe** — so it renders BLACK in windowless interiors. This is NOT the 1/π bug and is not fixed
by it. The kit props need a metalness clamp (0.35) or an asset re-convert. **A crate is not metal.**

### L6b. THE ELEVATOR STAMPS THE ENGINE DEFAULTS OVER YOUR WHOLE WORLD — fixed `fix/prim-point-light`
`ElevatorSystem::applyCabAtmosphere`'s "outside the cab" branch used to **restore hardcoded
`setAmbient(0.42,0.44,0.50)` + `setIblIntensity(1.0)`** — the exact crutch R2 is about. `m_cabAir`
starts at **-1**, so "you are not in the cab" is an EDGE on **frame one**: every atmosphere a world
set during build was silently overwritten before the first pixel. Level 1 could not set its own air
at all, and nobody could see why. The world now TELLS the elevator what to hand back
(`setWorldAtmosphere`). **If you add a system that restores atmosphere, restore the WORLD's, never a
constant.**

### L6. One build dir per agent
Concurrent agents in one working tree cause: MSBuild holding `.obj` files (recompiles silently
skipped → you debug a **stale exe**), `git stash` sweeping another agent's work, and merges
blocked by locked files. Use isolated worktrees + separate build dirs.

### L8. A Unity pack's EMISSION can be hidden in a channel your converter throws away
*(Landed as "L7" on `fix/emissive-convert`; renumbered to L8 when it merged alongside the
lighting line, which had independently claimed L7 for the **observation** of this same defect.
L7 is what you SEE — magenta paint on the props. L8 is WHY. Read both.)*

`ModularSciFi_Interior` ships `T_<X>_MRAG.png` = **M**etallic(R) **R**oughness(G) **AO**(B)
**GLOW(A)**. The kit's shader is `emission = Dif.rgb * MRAG.a` — the glow mask is the **alpha
of the MR map**, and the *diffuse* carries the emitter's COLOUR as a key: pure yellow
`(255,255,0)` panels, magenta `(255,0,255)` lenses, blue `(0,0,255)` console screens, green
`(0,255,0)` stair strips. The original convert kept only Dif + Norm and **dropped MRAG
entirely** — so every light lens shipped as flat PAINT (and every surface as uniform
metallic-0/roughness-0.5 plastic). The 0.42 ambient hid it for months; the moment lighting
went honest the facility was slabs of magenta and yellow.
**Before converting ANY pack: list its texture files and account for every channel.** A map
you cannot name is a map you are about to throw away. — fixed by `tools/convert_modular_scifi.py`,
gated by `tools/test_kit_materials.py`.
Two traps found while fixing it, both worth knowing:
- **Pillow premultiplies alpha on RGBA `resize()`.** An MRAG map is 4 unrelated DATA channels,
  not colour+alpha; a plain resize multiplied metal/rough/AO by the glow mask (~0 over 95% of
  the atlas) and crushed roughness 0.22 → **0.02 — every wall a mirror**. Resize per channel.
- **This engine's ACES is the per-channel Narkowicz approximation** (`shaders/composite.frag`).
  A channel that is 0 stays 0 at *any* exposure, so a pure-yellow (zero-blue) emitter can NEVER
  roll its core off to white — raising emissive strength just clamps it to a flat yellow swatch.
  If an emitter must read as a *lamp*, the core desaturation has to be baked into the emissive
  MAP. (Fixing the tonemapper to a hue-preserving/AP1 ACES would remove this constraint.)

---

## 🔦 THE TWO ROOT CAUSES (fixed — but read them, they explain most of the game's history)

### R1. `mesh.frag` had TWO lighting paths — every GLB shaded at 1/π  — FIXED `5c35d65`
Prims took an **unnormalized Lambert** (`albedo × N·L`, no 1/π). Every GLB took the **PBR path**
(`albedo×(1−metallic)/π × N·L + GGX`) — because `ModelLoader` synthesizes a 1×1 MR map whenever
`metallic > 0`. **Every light rig in the game was authored against the non-physical prim path.**
So every GLB shaded **0.318×** (metallic 0) to **~0.03×** (metallic 0.8) darker than the prim
beside it. Proven with byte-identical white-probe cubes (prim vs GLB → same md5).
**This was:** black-silhouette kit props · Jake's ship as a black blob · the washed-out Overlord ·
4 rounds of gate art that were *invisible* · and the reason the whole fleet band-aided art
instead of fixing the renderer.

### R2. The default ambient is `{0.42, 0.44, 0.50}` and **nothing ever called `setAmbient`**
Every interior in the game has been floodlit by an **engine default**. That's why the cell had
**17 point lights and zero shadows**. Worse — **the wash was HIDING REAL BUGS**:
> **The elevator's only ceiling light has been INSIDE THE DISCO BALL this entire time.**
> Both at the same Y. With RT shadows on, the cab renders **pitch black**. Nobody noticed for
> months because the 0.42 ambient was lighting the car instead of the lamp.
**Kill the crutch → the real bug walks out.** This happened FOUR separate times in one session.
Partially fixed (`dfcb65d`): `RoomDressing::applyZoneAtmosphere` now owns ambient+IBL for canon
zones. **STILL TODO: every interior outside the canon room graph** (level1, club, spire,
perfshop, showroom) still runs ambient 0.42.

---

### R3. THE MIRROR — a NEGATIVE-DETERMINANT basis draws the model INSIDE-OUT and unlit — SWEPT `fix/mirror-basis-sweep`
The idiom, copy-pasted around the codebase:
```
right = (-outward.z, 0, outward.x);   columns [right, up, outward]   // determinant = -1
```
**A negative determinant is a REFLECTION, not a rotation.** It reverses triangle winding, so
`VK_FRONT_FACE_COUNTER_CLOCKWISE` + back-face culling throws away the model's **OUTER shell** and
rasterizes its **INNER shell**. You are looking at the *inside* of the object, whose normals point
away from every light in the room. **Zero diffuse at any albedo under any light.**

**Why it hides:** the silhouette is perfect (a mirrored mesh has the same outline), albedo and
normal-map relief look right (texture lookups don't care about winding), the specular is coherent.
Every symptom points at the **art**. The rift-hub gate survived **nine rounds of art passes** on
this — our own notes recorded *"5× the key light barely moved it."*

**The smoking-gun test:** a GLB **cube carrying the object's exact material** renders blown-out
white in the same room while a **120-intensity probe light 3 m away leaves the object black**.
No material bug and no lighting bug can produce that pair. **Only a mirror can.**

**Where it was:** the gate GLB (`rifthub.cpp`, fixed `a9983ed`); the rift hub's fallback torus,
cradle skirt + anchor plates and the whole membrane stack (plasma disk / contact rim / falloff
shells); **the descent slide's entire track frame** (`frameToTransform` fed `[right, up, tan]` with
`right = cross(tan, +Y)` — the LEFT-handed lateral — so all 689 props on the ride were inside-out);
the descent slide's **cavern** (`flat.right` hand-written `{0,0,1}`, det -1 — floor, ceiling and all
four rock walls); and `fx.cpp`'s tracer billboard + muzzle flash.

**The law now:** `app/basis.h` → `basisFromOutward()` is the ONLY sanctioned way to orient a model
off an outward/forward/normal/tangent vector; it returns a guaranteed right-handed orthonormal basis.
**`--test-basis` is TOTAL**: it builds the worlds headless and asserts `det(upper 3x3) > 0` on
**every entity Scene ever received** — a list of known sites would rot the moment the idiom is pasted
into a new file. It ships with **negative controls** (the legacy idiom + a planted mirrored entity)
that prove it can go red.

**SECOND-ORDER (read this before you "fix" any art):** anything tuned against a mirrored object was
tuned against a lie. The gate's key rig had been cranked **70 → 15** fighting a surface *physically
incapable of responding*; un-mirrored it had to come **DOWN to 5.0**. Its group tints
(`0.789 × 0.22 = 0.17` albedo, then metallic `0.80` ate 80% of the remainder) were authored to tame a
forged texture set that **never existed** (B12). **Un-mirroring exposes bad art; it does not cause it.
Retune honestly — never restore the mirror.**

### R4. THERE ARE **TWO AMBIENTS** AND `setAmbient` CONTROLS THE WRONG ONE — FIXED `fix/prim-point-light`
`iblAmbient()` (mesh.frag) has two paths. On the **baked-environment** path it computes its diffuse
from `texture(irradianceCube, N)` and its specular from `prefilterCube` — and **never reads the
`ambient` argument at all.** An environment is baked **by default, for every scene, from the ANALYTIC
SKY** unless a host calls `setIblProbe`. Therefore:

> **`setAmbient()` — the dial the entire "AMBIENT IS NOT LIGHT, BRING IT DOWN" doctrine turns — has
> been a NO-OP in most of the game, and the real ambient has been a full-strength BLUE SKY CUBE.**

That is how a **windowless detention basement** ended up lit blue. It is also why R2's "kill the 0.42
wash" only ever half-worked: you could take the visible ambient to zero and the room stayed blue,
because the ambient you could see was not the ambient that was lighting it.

**The dials are coherent now:** `setIblIntensity(0)` means *"this room has no environment"* and drops
to the flat-ambient path, where `setAmbient` does exactly what it says. Every existing host
(0.22 / 0.5 / 1.0) is byte-identical. **Guarded by `--test-primlight`'s negative control**, which
asserts that with both dials at zero an unlit surface is **exactly black** — it measured **55/255 of
blue sky** before this fix.

**A windowless interior has no sky and therefore no sky IBL.** Its environment is THE ROOM
(`setIblProbe(true)`), its ambient is a near-black **NEUTRAL** floor, and its light is its fixtures.

### R5. "THE GRAYBOX WALLS RECEIVE ZERO POINT LIGHT" WAS **FALSE** — the hue tell lied
Filed as `LIGHTING_AUDIT_FACILITY.md` **P2** on the strength of one measurement: under a working
fixture, ceiling luma 53 WARM, floor 43 WARM, **wall 4.9 and BLUE-DOMINANT** — "a surface lit by a
warm fixture reads warm, therefore this one gets no point light." **Reproduced exactly** (wall
`(2.2, 4.6, 7.9)`, luma 4.30, blue) — **and the conclusion is wrong.** The walls were never off the
light path:

* `--test-primlight`: a PRIM and a GLB with identical albedo under an identical lamp agree to **3%**.
* `r_debugview 2` (the point-light term ALONE): the walls catch a healthy warm term.
* `r_debugview 5` (real lighting, albedo forced to a flat 0.5): those same walls read **WARM at
  77-81% of the floor's radiance.** A surface that receives no light cannot do that.

They were **multiplying that light by nothing**: the graybox wall panel was **0.077 linear** (a 7.7%
reflector — asphalt), knocked down again by baseColor tints of 0.50-0.62 authored to sit inside the
old blue ambient wash. Final albedo ≈ **0.04**, and **blue-biased enough (B/R = 1.8) to overturn the
tungsten lamp's warm tilt (R/B = 1.6)**. So the surface read blue *while being lit by a warm lamp* —
and the hue tell, which is a good tell, told a lie.

**THE LESSON:** DECISIONS.md's order of operations exists for this. **Prove the surface can be lit
(white albedo + probe) BEFORE you conclude anything from its colour.** A blue-biased albedo and a
hidden blue ambient will both forge the fingerprint of "this thing is not on the light path."

## 🐛 OPEN BUGS

| # | Bug | Notes |
|---|-----|-------|
| B1 | Velocity image sampled in `UNDEFINED` | `taa_resolve` binds `m_velView` unconditionally; graph imports `rgVel` only when `velOn`, and taa-resolve never declares a read of it |
| B2 | SSAO blur image sampled in `UNDEFINED`, full of garbage | `mesh.frag` set3 binds it unconditionally; reads guarded by `if (ssaoOn)`. `r_rtao` *replaces* SSAO → **undefined memory multiplied into the ambient term** |
| B3 | ~~`mesh_probe.vert` missing the location-12/13 outputs~~ | **FIXED** `fix/prim-point-light` — location **13** (`vGlassTint`) was still missing, so the reflection-probe PSO's SPIR-V interface was incomplete and `setIblProbe(true)` could not be trusted. A room that explicitly asked to reflect ITSELF was still reflecting the sky. |
| B4 | ~~Ambient 0.42 still active outside the canon room graph~~ | **CLOSED** in `integration/land-lighting`. level1 by `fix/prim-point-light` (scene IBL probe + IBL 0.5 + ambient 0.030 **neutral**); club / spire / perfshop / showroom now take **the same model** in `app_run.cpp` (they previously got a tinted `setAmbient` and **no IBL call at all** — i.e. a full-strength blue sky). **Read R4 first — `setAmbient` alone would not have fixed any of them.** |
| B5 | ~~`SM_Door_A` ships a near-white albedo~~ | **FIXED — but the DIAGNOSIS WAS WRONG. Read below.** |
| B14 | ~~337 point lights registered in level1, device cap is **64**~~ | **FIXED** — and it is a nice illustration of the one-line rule. `EnvArtSystem` registers one per `Light_A` across all 8 plates; `setPointLights` kept the **first 64** and dropped the rest **silently**, so the upper floors' own fixtures were thrown away. `fix/prim-point-light` **found** it and filed it; `light/audit-facility` had already **built** the fix (`nearestFixtures()`, budget 44 of a 64 cap, fed nearest-to-eye on the live loop, the plain capture path, the alert path and the elevator path). The two branches landed together and the cull is what ships. **Do not restore a raw `= game.lightFixtures()` feed anywhere.** *(Filed as "B5" on prim-point-light; renumbered — B5 is the pink door.)* |
| B6 | Elevator OLED text renders **mirrored** | Pre-existing UV/facing bug on the twin viewscreens |
| B7 | Elevator −X observation window renders as a bright noisy slab | Needs one more pass |
| B8 | Cinematic cuts to a blank blue screen | Intro/cold-open, mid-sequence |
| B9 | Cell kit ceiling reads as a black hole | |
| B10 | Two VFX bypass the entity path (flat-emissive glass) | `space/decloak_vfx.cpp:310`, `space/descent.cpp:294` |
| B11 | `addMetal()` in `holo_panel.cpp` made **no metal** | It set `baseColor` and **no `mrTex`**, so every frame/mount entity took `drawMeshEmissive` — a flat DIELECTRIC at albedo 0.66–0.76, i.e. **white plastic**. The lambda's *name* asserted the one thing it didn't do, so ~10 re-fixes never re-checked it. FIXED `art/rifthub-canon` (real 1×1 MR + machined-steel F0). **Audit other "addMetal"-style helpers for a missing MR map.** |
| B12 | The gate tube's forged SD3.5 sets **do not exist on any machine** | `gate_tube_hull` / `gate_ring_plate` / `gate_patina_plate` / `gate_piston_steel` were never harvested (LFS budget died — see the note at `m_surf.mount`). `surface_library/` is **gitignored**, so the 24 curated sets are the only ones that exist. Every gate group silently falls back — and the tints authored to tame *bright forged* textures instead crushed the *already-correct curated* ones (0.789 × 0.22 = **0.17 albedo**). Anything keyed to a forged set is dead code today. |
| B13 | ~~**`ModularSciFi_Interior` ships its EMISSION MASKS baked into the ALBEDO**~~ | **FIXED** — `fix/emissive-convert`. The whole purple/magenta prop family, in one defect. See **L7** (the observation) and **L8** (the root cause: the glow mask lives in the **alpha of the MRAG map**, which our converter discarded wholesale). *Renumbered from B11 when the lighting and emissive lines landed together — B11/B12 above were already taken.* |

---

### R3. THE RIFT GATE WAS INSTANCED THROUGH A **MIRROR** — FIXED `art/rifthub-canon`
The rifthub gate GLB was placed with the basis `[right, up, outward]` where
`right = (-outwardZ, 0, outwardX)`. For portal 0 (`out = +X`) that is
`right=(0,0,1), up=(0,1,0), out=(1,0,0)` — **determinant −1**. A negative determinant is a
**REFLECTION, not a rotation**: every gate was instanced mirrored. That reverses triangle
winding, so back-face culling **threw away the tube's OUTER shell and drew its INNER shell** —
we were looking at the *inside* of the tube, whose normals point away from every light in the
room. **This is why the gate tube was a BLACK VOID RING for nine rounds.**

It is a vicious bug because *every* symptom points at the art:
- perfect torus silhouette (a torus seen inside-out looks the same),
- correct albedo + normal-map relief (texture lookups don't care about winding),
- coherent specular,
- and **zero diffuse at ANY albedo under ANY light** — hence "5× the key light barely moved it."

**The tell that cracks it:** a GLB cube carrying the tube's *exact* material renders blown-out
white in the same room, while a **120-intensity probe light 3 m from the tube leaves it black**.
No material or lighting bug can do that. Only a mirror can. (`tools/build_rifthub_gate.py` even
logs `TUBE inside-out N/M` — the mirror was being fought at *export* time instead of at the
transform.) **Fix = one sign** (`locX = -right` → det +1).

⚠️ **CHECK EVERY OTHER PLACEMENT BASIS IN THE GAME FOR A NEGATIVE DETERMINANT.** This basis
idiom (`right = (-outZ, 0, outX)`) is copy-pasted around the codebase. Anywhere a GLB is
instanced through it, that model is inside-out and unlit.

**COROLLARY — light rigs tuned against a mirrored object are GARBAGE.** The gate key was
70 → 15 → and had to come down to **5.0** once the tube actually took light. Both earlier
numbers were fitted to a surface that was *physically incapable of responding*. Before you
tune a light, prove the surface can be lit at all.

---

### B5 — THE PINK DOOR. Three defects, and the headline one was NOT the albedo. — FIXED
B5 said "near-white albedo -> blows out pink". The albedo *was* wrong. **It was not what made
it pink.** Chasing the stated diagnosis would have dimmed the door and left it pink.

**The falsifying measurement.** Renormalized the door's albedo 0.768 -> 0.32 linear and re-shot:
the slab's brightness dropped, and its hue **did not move** — `R-G` held at **+57 -> +58**.
A value change cannot fix a hue. Therefore the door was a *neutral surface being lit red*.

| # | defect | evidence | fix |
|---|---|---|---|
| 1 | **A misplaced red light — THE PINK.** The cell's red "threshold tell" hung **0.5 m off the door face, HEAD-ON (N·L≈1), at slab-centre height**, range 3.0. It delivered **~0.43 red** to the slab while the room's white key — a *ceiling* tube grazing a *vertical* slab — delivered only ~0.35. **The red light WON: the door was a red-lit surface.** | slab `(154, 97, 101)`, `R-G=+57` | `cell_dressing.cpp` — mount it **over the lintel** (a real "LOCKED" indicator), range 3.0 -> 1.5. Grazing incidence -> a red gradient at the head of the door + a pool on the jamb. **The tell survives; the wash does not.** Slab is now `(28, 32, 37)`, `R-G=-4.3` — neutral steel. |
| 2 | **The door bypassed the PBR path.** `DoorSystem::drawMeshes` called the 5-arg `drawMesh()` — the NON-PBR path — and **threw away the normal + MR maps that `makeDrawables()` had already resolved**. So the slab shaded dead flat AND took the **unnormalized Lambert (~π× brighter** than every GLB beside it — R1/`5c35d65`). A **third** entity-path bypass, alongside B10's two. | — | `door.cpp` -> `drawMeshPBR` with its real maps. |
| 3 | **Albedo 0.768 linear** (body = 42% of texels at sRGB 227). Snow is 0.85; an institutional door is ~0.30. Same over-unity crutch as the 1.08 cot. | — | `door.cpp`: renormalize via glTF's own `baseColorFactor` (×0.42 -> **0.32 linear / 0.60 sRGB**, the value that fixed the rifthub tube). **Not** by rewriting the `.glb` — it is Git-LFS tracked and the asset is not corrupt; our *use* of it was never normalized. |

**The lesson, and it is THE PATTERN again:** a crutch (the ambient wash) hid a light that was
**pointing at the wrong thing**. Two of these three are invisible to a screenshot and only fall
out of a *measurement*. **MEASURE THE HUE, NOT JUST THE BRIGHTNESS** — `R-G` and `B-G` on a
surface tell you instantly whether you are looking at an albedo problem or a *light* problem.
An albedo error scales all three channels together; a coloured light does not.

### L7. A KIT'S EMISSION MASK CAN SHIP AS ALBEDO PAINT — the purple/magenta props
*(The OBSERVATION. The root cause — the glow mask lives in the alpha of a MRAG map the converter
discarded — is **L8**, and the re-convert that fixes it is `tools/convert_modular_scifi.py`.)*
**MEASURED** (2026-07-12), and it explains *every* magenta prop at once, not one at a time:

| asset | pure-magenta texels **in the base-color map** | mean sRGB | its emissive slot |
|---|---|---|---|
| `SM_Door_A` / `T_Door_A_Dif` | 0.08 % | **(242, 0.7, 242)** | *empty* |
| `SM_Ceiling_A` / `T_Ceiling_A_Dif` | 0.64 % | **(160, 1.8, 161)** | *empty* (`emissiveFactor: None`) |

R ≈ B with **G ≈ 0** is not paint — it is the classic Unity **emission-mask key colour**. In the
source pack those regions are LIGHT STRIPS and indicator panels that GLOW. Our GLB conversion
dropped the emissive slot, so the mask stayed behind **in the albedo** and the props now ship
magenta *paint* where they should carry emissive *light*. That is why the door's trim and the
cell's ceiling insets read purple/magenta, and it is a whole-pack defect, not a per-prop one.

**Do NOT "fix" this by tinting individual props.** The fix is to key an emissive map off the
magenta mask at convert time (mask -> emissiveTexture; neutralize those texels in the albedo) —
then the strips glow, which is what the art intended, and the purple goes away by construction.
Beware **L4** when you do: an emissive map with no MR map is silently dropped by `Scene::submit()`.

**Corollary — this is NOT what made the door pink.** See B5.
**Root cause + fix: L8.** The mask was not "left behind" by accident — the pack keys its glow off
the **alpha of a MRAG map** our converter dropped on the floor. Read L8 before you touch a kit.

---

## ✅ THE PATTERN (memorize this)

**Every crutch in this codebase was hiding a broken thing underneath.**
- Fake ambient → hid a light sealed inside a disco ball, and cells with no working lamps.
- Fake self-emissive → hid geometry that was never being lit.
- Over-unity albedo (a cot at **1.08** — a surface reflecting 108% of the light hitting it) → hid 1/π.
- Point lights in space (delivering **1/22,500** of their intensity at fleet distance) → hid behind a
  fake ambient floor and an albedo scale, which is why one ship went black and the other went white.

**When something looks wrong: suspect the renderer and the crutches BEFORE you suspect the art.**
Four "art problems" in one session — the pistol, the props, the gate, the ship — were **all** the
renderer or a crutch around it. **Not one of them was the art.**

**And: VALUE, NOT LUMENS.** If a surface reads wrong, fix its **albedo** first. The rifthub tube was
black; 5× the key light barely moved it. Renormalizing albedo to ~0.60 sRGB fixed it instantly.
Ambient is not light — it's omnidirectional, so raising it lights a room **by destroying its contrast**.
