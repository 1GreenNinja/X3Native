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

## 🐛 OPEN BUGS

| # | Bug | Notes |
|---|-----|-------|
| B1 | Velocity image sampled in `UNDEFINED` | `taa_resolve` binds `m_velView` unconditionally; graph imports `rgVel` only when `velOn`, and taa-resolve never declares a read of it |
| B2 | SSAO blur image sampled in `UNDEFINED`, full of garbage | `mesh.frag` set3 binds it unconditionally; reads guarded by `if (ssaoOn)`. `r_rtao` *replaces* SSAO → **undefined memory multiplied into the ambient term** |
| B3 | `mesh_probe.vert` missing the location-12 output | `45e1c46` added the input to `mesh.frag`, updated only `mesh.vert` → breaks the reflection-probe PSO's SPIR-V interface |
| B4 | Ambient 0.42 still active outside the canon room graph | level1, club, spire, perfshop, showroom |
| B5 | ~~`SM_Door_A` ships a near-white albedo~~ | **FIXED — but the DIAGNOSIS WAS WRONG. Read below.** |
| B6 | Elevator OLED text renders **mirrored** | Pre-existing UV/facing bug on the twin viewscreens |
| B7 | Elevator −X observation window renders as a bright noisy slab | Needs one more pass |
| B8 | Cinematic cuts to a blank blue screen | Intro/cold-open, mid-sequence |
| B9 | Cell kit ceiling reads as a black hole | |
| B10 | Two VFX bypass the entity path (flat-emissive glass) | `space/decloak_vfx.cpp:310`, `space/descent.cpp:294` |
| B11 | **`ModularSciFi_Interior` ships its EMISSION MASKS baked into the ALBEDO** | See L7 — this is the whole purple/magenta prop family |

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
