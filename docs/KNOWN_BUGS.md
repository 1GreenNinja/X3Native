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

### L7. THE PLANETS ARE NOT A SKYBOX — they are geometry, 140 m from your eye
`drawNightSkyPlanets` pins every celestial body to a shell `kNightSkyDist` (140 m) from the
camera, as **real depth-tested triangles drawn AFTER the opaque meshes**
(`VulkanRenderDevice::recordPlanetDraws`). The opaque bodies **depth-WRITE**; the ring /
atmosphere / corona shells depth-test **LEQUAL** and **alpha/additively blend**. So:

> **Any mesh FURTHER from the eye than the sky shell gets punched out by the planet disc and
> smeared over by the ring and atmosphere.**

That is fine for the hosts it was written for (nightsky / showroom / car — a building a few
dozen metres out). It is **catastrophic** for any scene that plays out past 140 m. It was the
whole of **B10**. The far plane and the sky anchor are a **PAIR** — move one, move the other.
`app/sky_scale.h` states the invariant; `--test-cutscene` asserts it with negative controls.
**Before you put a sky body in a scene, ask how far away the furthest hull is.**

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
| B5 | `SM_Door_A` ships a near-white albedo | Game-wide (owned by `DoorSystem`); blows out pink under honest light |
| B6 | Elevator OLED text renders **mirrored** | Pre-existing UV/facing bug on the twin viewscreens |
| B7 | Elevator −X observation window renders as a bright noisy slab | Needs one more pass |
| B8 | Cinematic cuts to a blank blue screen | Intro/cold-open, mid-sequence |
| B9 | Cell kit ceiling reads as a black hole | |
| ~~B10~~ | ~~Two VFX bypass the entity path (flat-emissive glass)~~ | **FIXED — but the diagnosis above was WRONG on every point. See R3 below.** |

---

### R3. The "see-through Overlord" was **THE SKY IN FRONT OF THE SHIP** — FIXED `fix/decloak-handoff`
Tim: *"the capital ship renders as SEE-THROUGH GLASS with red/pink/yellow smears — you can see
the planet straight through its hull."* Everyone (this file included) assumed a **glass draw
path**. It was nothing of the kind. **Every part of the filed B10 diagnosis was false:**
- `decloak_vfx.cpp` is **DEAD CODE** — `DecloakVfx` is never instantiated anywhere in the game,
  and the `--test-decloak` flag it references doesn't exist. It also never draws the ship: its
  glass call is a **shimmer shell BOX**, not a hull. It cannot have caused anything.
- `descent.cpp:294` is a **fog dome + cloud streaks**. Volumetric atmosphere *should* be
  translucent. No entity is being bypassed. Not a bug.
- The capital ship is **`SpaceShip4.glb`**, not `OverLordEnforcer99.glb` (that one is the F6
  boss *monster*). Its material is `alphaMode: OPAQUE`. It was **never on a glass path**, and
  it was already drawn through `drawMeshPBR` on the honest, self-lit entity path.
- Its albedo is clean: **zero** magenta/emission-key texels (measured) — *not* the ModularSciFi
  keyed-albedo class of bug.

**The actual cause:** the cold open's camera ran the engine-default **200 m far plane**, while
its capital ship is a **200 m hull** the camera frames from ~90 m, and the FORGE3D sky bodies
were pinned **140 m from the eye** (see **L7**) — i.e. *inside the fleet*. The gas giant's
**opaque disc punched a hole through the hull** (it depth-writes) and its **alpha ring +
additive atmosphere smeared across it** (they depth-test LEQUAL and blend over anything
further away). Measured: the "smears" are the gas giant's **red body, RGB (99.5, 15.0, 12.2)**,
and its tan/yellow ring — sitting where hull should be.
The same 200 m far plane **clipped the capital ship clean out of frame for the first half of
its reveal** (t=20..27, at 340–550 m), so the authored "distant speck → looming hull" ramp
never rendered — the ship just **popped in** at 200 m. Both symptoms, one cause:
**THE SCENE WAS FIVE TIMES BIGGER THAN ITS FRUSTUM.**
**Fix:** far plane → 15 km and the sky shell → 10.5 km, moved **as a pair** (`app/sky_scale.h`).
Apparent sky size is unchanged (radius = dist·tan(diam/2)); only its **depth** moved.

> **The lesson is THE PATTERN again, one level up:** four separate people read "translucent
> hull" and went hunting for a transparency bug *in the ship*. The ship was fine. **Something
> in front of it was wrong.** When a surface looks wrong, also ask **what is between it and the
> camera** — not only what it is made of.

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
