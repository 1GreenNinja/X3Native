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

### L6. One build dir per agent
Concurrent agents in one working tree cause: MSBuild holding `.obj` files (recompiles silently
skipped → you debug a **stale exe**), `git stash` sweeping another agent's work, and merges
blocked by locked files. Use isolated worktrees + separate build dirs.

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

## 🐛 OPEN BUGS

| # | Bug | Notes |
|---|-----|-------|
| B1 | Velocity image sampled in `UNDEFINED` | `taa_resolve` binds `m_velView` unconditionally; graph imports `rgVel` only when `velOn`, and taa-resolve never declares a read of it |
| B2 | SSAO blur image sampled in `UNDEFINED`, full of garbage | `mesh.frag` set3 binds it unconditionally; reads guarded by `if (ssaoOn)`. `r_rtao` *replaces* SSAO → **undefined memory multiplied into the ambient term** |
| B3 | `mesh_probe.vert` missing the location-12 output | `45e1c46` added the input to `mesh.frag`, updated only `mesh.vert` → breaks the reflection-probe PSO's SPIR-V interface |
| B4 | Ambient 0.42 still active outside the canon room graph | level1, club, spire, perfshop, showroom |
| B5 | `SM_Door_A` ships a near-white albedo | Game-wide (owned by `DoorSystem`); blows out pink under honest light |
| B6 | Elevator OLED text renders **mirrored** | Pre-existing UV/facing bug on the twin viewscreens |
| B7 | Elevator −X observation window renders as a bright noisy slab | Needs one more pass |
| B8 | Cinematic cuts to a blank blue screen | Intro/cold-open, mid-sequence |
| B9 | Cell kit ceiling reads as a black hole | |
| B10 | Two VFX bypass the entity path (flat-emissive glass) | `space/decloak_vfx.cpp:310`, `space/descent.cpp:294` |
| B11 | `addMetal()` in `holo_panel.cpp` made **no metal** | It set `baseColor` and **no `mrTex`**, so every frame/mount entity took `drawMeshEmissive` — a flat DIELECTRIC at albedo 0.66–0.76, i.e. **white plastic**. The lambda's *name* asserted the one thing it didn't do, so ~10 re-fixes never re-checked it. FIXED `art/rifthub-canon` (real 1×1 MR + machined-steel F0). **Audit other "addMetal"-style helpers for a missing MR map.** |
| B12 | The gate tube's forged SD3.5 sets **do not exist on any machine** | `gate_tube_hull` / `gate_ring_plate` / `gate_patina_plate` / `gate_piston_steel` were never harvested (LFS budget died — see the note at `m_surf.mount`). `surface_library/` is **gitignored**, so the 24 curated sets are the only ones that exist. Every gate group silently falls back — and the tints authored to tame *bright forged* textures instead crushed the *already-correct curated* ones (0.789 × 0.22 = **0.17 albedo**). Anything keyed to a forged set is dead code today. |

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
