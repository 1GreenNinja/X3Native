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

### L5. glTF default `metallicFactor` is **1.0** — CLAMPED `fix/bugs-round2`
Any GLB whose exporter omitted it becomes a **full metal** — and full metal has **no diffuse
lobe** — so it renders BLACK in windowless interiors. This is NOT the 1/π bug and is not fixed
by it. The kit props need a metalness clamp (0.35) or an asset re-convert. **A crate is not metal.**

**FIXED** — `ModelLoader.cpp:687-716`. The clamp is **scoped to the synthesized-scalar path**
(the branch that bakes a 1×1 MR map when a material has **no `metallicRoughnessTexture`**), which
is exactly where the damage lands. A model that ships a **real MR map** is authored data — the
factor merely multiplies it — and is left **byte-identical**; both of our own converters take that
route (`tools/rebind_weapon_textures.py`, `tools/convert_modular_scifi.py` bind an MR map with
factor 1.0) and `tools/test_kit_materials.py` K4 already fails any kit material sitting at 1.0
with no map. So the clamp can only ever bite a third-party/hand-authored GLB — the L5 victims.

**Who it actually hits in level1** (logged at runtime, device path only — note `mint()` returns
**untagged** handles when there is no device, so a headless trace makes *every* material look like
it lacks an MR map; measure on the device path or you will chase ghosts):

| model | metallic | roughness | baseColorFactor | has baseColorTex |
|---|---|---|---|---|
| `SciFi_Warehouse_Kit/{Garbage Bin, Wall Light, Hanging Light, Exit Sign}.glb` | 1.0 | 1.0 | 1,1,1 | yes |
| `ModularSciFi_Interior/SM_Wall_C.glb` | 1.0 | 1.0 | 1,1,1 | yes |
| `Jake_22_actions.glb`, `marcus_webb_anim.glb` | 1.0 | 1.0 | 1,1,1 | yes |
| `Detention/SM_Hospital_Bed.glb` (`M_Bedframe`) | 0.70 | 0.42 | 0.60,0.62,0.66 | no |

**metallic 1.0 + roughness 1.0 + baseColorFactor 1,1,1 is the exact glTF DEFAULT TRIPLE** — that is
not art, that is an exporter that wrote no PBR scalars at all.

**AND IT DOES NOT ONLY MAKE THINGS BLACK — IT ALSO MAKES THEM WHITE.** This entry only ever
described half the failure. For a **metal, `F0 = albedo`** — so a full metal with a *dark* albedo is
a dark mirror (**black**, the filed symptom), but a full metal with the **default white** factor is a
**100% mirror** and renders as a **blown-out featureless white sheet**. Level 1's **floor** was the
second kind: `hall_OFF_a.png` shows it as a flat white slab with no detail; with the clamp
(`hall_ON_a.png`) it is deck plating with hazard strips, cable runs and panel seams. Measured over
the frame: **void (luma ≤ 6) 51.1% → 35.2%, clipping unchanged at 0.01%**. A second corridor camera
(`hall_{OFF,ON}_b.png`) is **byte-identical** — the clamp is targeted, not a global wash.

⚠️ **SECOND ORDER (R3's rule, again): the clamp EXPOSES bad albedo; it does not cause it.** Handing
the diffuse lobe back to a prop whose albedo is a **near-white kit texture** (the B5 family —
`SM_Door_A` measured **0.768 linear**; an institutional surface is ~0.30) makes it read **hot** at
close range. That is the over-unity-albedo crutch, and it is a **B5 asset problem**, not a reason to
restore the metal crutch. **Retune the albedo. Never put the metal back.**

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
| B1 | ~~Velocity image sampled in `UNDEFINED`~~ | **FIXED** `fix/bug-sweep` (verified in tree, doc was stale). `vk_graph.cpp:318` introduces `velSampled` (import whenever TAA *samples*, not just when the vel pass writes); `:684` a `velocity-neutral-clear` pass gives the graph a write to hang the transition on; `:1621` taa-resolve now DECLARES the read. All three legs of the original complaint are gone. |
| B2 | ~~SSAO blur image sampled in `UNDEFINED`, full of garbage~~ | **FIXED** `fix/bug-sweep` (verified in tree, doc was stale). `vk_graph.cpp:723` an `ssao-neutral-clear` pass clears the blur image to WHITE (unoccluded) on frames SSAO does not run; `:915` the color pass declares the `rgSsaoBlur` read, so a real `TRANSFER_DST -> SHADER_READ_ONLY` transition is emitted; `vk_gi_rt.cpp:463` adds the `TRANSFER_DST` usage to back it. |
| B3 | ~~`mesh_probe.vert` missing the location-12/13 outputs~~ | **FIXED** `fix/prim-point-light` — location **13** (`vGlassTint`) was still missing, so the reflection-probe PSO's SPIR-V interface was incomplete and `setIblProbe(true)` could not be trusted. A room that explicitly asked to reflect ITSELF was still reflecting the sky. |
| B4 | ~~Ambient 0.42 still active outside the canon room graph~~ | **FIXED** — verified at RUNTIME on main, `app_run.cpp:2534-2587` (a block titled "B4/R2 — KILL THE LAST OF THE 0.42 AMBIENT WASH"). It covers exactly the un-owned interiors (level1 + spire + club1127 + perfshop + showroom) — which is why grepping `setAmbient` PER WORLD FILE finds zero and wrongly reads as "open": the fix is CENTRAL. Proof: `--world level1 --smoketest` logs `[light] interior atmosphere (B4/R4): scene IBL probe ON, ibl 0.5, ambient 0.030 NEUTRAL`. **And it found something deeper than this entry ever described: THE ENGINE HAS TWO AMBIENTS.** `iblAmbient()`'s baked-env path takes diffuse from `irradianceCube` and NEVER READS its `ambient` argument, and an env cube is baked BY DEFAULT, FOR EVERY SCENE, FROM THE ANALYTIC BLUE SKY — so `setAmbient()` alone was aimed at a DEAD DIAL (with `setAmbient(0)` the probe still read 55/255 of sky). It takes all three dials together: `setIblProbe(true)` (the cube becomes THE ROOM, not a sky) + `setIblIntensity()` + a NEUTRAL near-black `setAmbient()`. Same root cause as the rifthub gate ("a mirror aimed at a black room") — if a surface will not respond to light, ask what its ENVIRONMENT is before you touch a lamp. |
| B5 | ~~`SM_Door_A` ships a near-white albedo~~ | **FIXED — but the DIAGNOSIS WAS WRONG. Read below.** |
| B14 | ~~337 point lights registered in level1, device cap is **64**~~ | **FIXED** — and it is a nice illustration of the one-line rule. `EnvArtSystem` registers one per `Light_A` across all 8 plates; `setPointLights` kept the **first 64** and dropped the rest **silently**, so the upper floors' own fixtures were thrown away. `fix/prim-point-light` **found** it and filed it; `light/audit-facility` had already **built** the fix (`nearestFixtures()`, budget 44 of a 64 cap, fed nearest-to-eye on the live loop, the plain capture path, the alert path and the elevator path). The two branches landed together and the cull is what ships. **Do not restore a raw `= game.lightFixtures()` feed anywhere.** *(Filed as "B5" on prim-point-light; renumbered — B5 is the pink door.)* |
| B6 | ~~Elevator OLED text renders **mirrored**~~ | **FIXED** `fix/bugs-round2`. It was never a mirror — it is a pure **V (vertical) flip**, and the difference matters because "mirrored" sends you hunting UVs and facing on six faces. **PROVED with the shipped `X3_OLED_DUMP` hook**: the baked PPM is upright and correct, while the in-cab panel shows the lines in **reverse order with every glyph flipped top-to-bottom** and the glyph order **left-to-right intact** — so U is right on every face. ROOT CAUSE: an image uploads **TOP-DOWN** (row 0 = the top; V=0 samples row 0) but `makeBox()`'s V axis runs **BOTTOM-UP** (`mesh_prims.h:85-88` — v=0 at −hy on all six faces). Nothing flips it. It survived because on a **TILING** texture a V flip is invisible; it only bites a **baked, non-tiling image — i.e. text**. FIX: `elevator.cpp:824-846` `flipMeshV()` + an opt-in `flipV` arg on `addKit`, set on the two OLEDs (`:1053,1056`) and the floor indicator (`:1140`, same defect, same construction). **Deliberately NOT fixed in `makeBox()`**: that UV convention is shared by every textured prim in the game *and their normal maps* (a V flip flips the derived bitangent and inverts the relief) — a global change there is an engine-wide relight, not a bug fix. |
| B7 | ~~Elevator −X observation window renders as a bright noisy slab~~ | **FIXED** `fix/bugs-round2` — **and it was not glass at all.** `elevator.cpp:981` added it with `addKit`, alpha 0.35 and **no MR texel**, so `Scene::render` (`scene.cpp:150-177`) sent it to `drawMeshEmissive()` — the **non-PBR path, which never reads `baseColor[3]`**. The alpha was decorative. It rendered as a fully **OPAQUE** slab on the unnormalized-Lambert prim path (~π× brighter than every GLB beside it — R1), blue-washed by the sky IBL (measured `R,G,B = 23.8, 36.0, 56.4`, **B/R = 2.37**), and it **hid the strata plane** the window exists to show. Same class as B11/L4. FIX: a real MR texel (rough 0.12, **metal 0** — glass is a dielectric) + an honest dark smoked albedo → the PBR/IBL path, plus `Entity::alphaBlend` for see-through. Result: `B/R = 1.11` (neutral), p95 95 → 20. **NOT `Entity::transparent`** (the glass pass): normal glass rides the OPAQUE record range and therefore **replays in the depth pre-pass** (`vk_passes.cpp:878-886`) — it would have depth-rejected the strata behind it. That is **L3**, and the strata is the one thing this window must not occlude. `alphaBlend` (`scene.h:117-125`) rides the **BLEND tail** (`vk_passes.cpp:1771-1774`) and writes no depth. **SECOND ORDER — THE PATTERN, again:** the opaque window had been hiding a blown-out slab. The instant it became real glass, the strata rock face behind it rendered as a **flat clipped white sheet** — because it is *also* a bare prim with no MR texel, so it *also* took the unnormalized-Lambert path, at π× the light, 1.4 m from the cab's practical. A previous pass **saw the symptom** and reached for the albedo (0.55 → 0.42, the "brightest thing in the cab" note in `layoutVisuals`); **no albedo below 1.0 can pay off a factor of π.** Fixed at the renderer: a matte dielectric MR texel (`elevator.cpp:998-1013`). |
| B8 | Cinematic cuts to a blank blue screen | Intro/cold-open, mid-sequence |
| B9 | Cell kit ceiling reads as a black hole | |
| B10 | Two VFX bypass the entity path (flat-emissive glass) | **INVESTIGATED, DELIBERATELY NOT REFACTORED** (`fix/bugs-round2`). The stated benefits are **both zero here**, and it is worth writing down so nobody spends a day on it. **(1) PVS culling buys nothing.** `Scene::setVisibleRooms` is called only from the canon-level / `level_loader` paths (`app_run.cpp:4536,5326,8874`, `level_loader.cpp:1967,1988`) — **never in the space or descent worlds**, so the room cull is inactive there (`scene.cpp:120-122`: an empty set = everything draws). And the descent dome + streaks are **camera-anchored** (`descent.cpp:234-235,:277-281` — translation *is* the camera position), so no frustum or portal cull could ever remove them anyway. **(2) `--test-basis` coverage buys nothing.** Both descent matrices are literal identity-rotation TRS — det = +1 (dome) and scale² > 0 (streaks); there is no basis to get wrong. `decloak_vfx.cpp:310` draws with the **host's ship transform**, and the ship IS a Scene entity, so `--test-basis` already covers it. Against zero benefit, the cost is real: a mutable entity pool + per-frame material mutation (decloak opacity tracks intensity; descent draws a **variable** number of streaks and `continue`s below op 0.01), plus moving explicitly-ordered immediate-mode transparent draws into Scene's insertion-order iteration — **on a shipped cinematic**. `space/decloak_vfx.cpp:310`, `space/descent.cpp:243,294`. |
| B11 | ~~`addMetal()` in `holo_panel.cpp` made **no metal**~~ | **FIXED** (doc already said so; re-verified). `holo_panel.cpp:671` `addMetal` now takes an MR handle and every call site (`:742,745,749,767,777`) passes `m_mrGun`/`m_mrPolish`. The follow-up action ("audit other addMetal-style helpers") is also DONE: `elevator.cpp:1011,1026` both carry real MR texels. |
| B12 | The gate tube's forged SD3.5 sets **do not exist on any machine** | `gate_tube_hull` / `gate_ring_plate` / `gate_patina_plate` / `gate_piston_steel` were never harvested (LFS budget died — see the note at `m_surf.mount`). `surface_library/` is **gitignored**, so the 24 curated sets are the only ones that exist. Every gate group silently falls back — and the tints authored to tame *bright forged* textures instead crushed the *already-correct curated* ones (0.789 × 0.22 = **0.17 albedo**). Anything keyed to a forged set is dead code today. |
| B13 | ~~**`ModularSciFi_Interior` ships its EMISSION MASKS baked into the ALBEDO**~~ | **FIXED** — `fix/emissive-convert`. The whole purple/magenta prop family, in one defect. See **L7** (the observation) and **L8** (the root cause: the glow mask lives in the **alpha of the MRAG map**, which our converter discarded wholesale). *Renumbered from B11 when the lighting and emissive lines landed together — B11/B12 above were already taken.* |
| B15 | ~~**JAKE'S CELL WAS A VOID — the room the player WAKES UP IN**~~ | **FIXED** `fix/cell-relight`. Was mean luma **6.7–10.1** with **65–80% of every frame at or below luma 6**, flashlight OFF; now **21.7–24.2** mean, **24–34% void**, **0.00% clipped**. See **L9** — the cause is not what anyone assumed, *including both agents who looked at it*. Not the albedo (floor 0.185 / walls 0.172 — already honest). Not "the key dies in mid-air" (`r_debugview 2`: the lamp arrives, mean 57.7). It was **FLUX + HANG HEIGHT**: main tripled the cell's volume (4×3.5×4 → 7×4×6) while **42 m² of floor still had exactly ONE fixture**, hung 0.40 m under the ceiling — so with `w²/(d²+1)` the **ceiling caught 0.86 of it and the floor 3.6 m below caught 0.056. A 15:1 waste.** The lamp was lighting the slab above itself. *(Renumbered from a duplicate B14 — the light-cap cull owns that number.)* |

---

### L9. "HAND-CALIBRATED — DO NOT TOUCH" BECOMES A TRAP THE MOMENT THE ROOM CHANGES SIZE
Jake's cell was the one room in the facility that was *correct*, so the facility lighting audit
**deliberately excluded it** ("hand-calibrated, do not touch"). Then the level JSON grew the cell
from **4×3.5×4 → 7×4×6 — three times the volume** — and the exclusion meant the one room nobody
was allowed to re-tune became the only room still lit for a closet. **An exclusion is scoped to a
room's DIMENSIONS, not its name. When geometry changes, every "do not touch" on it expires.**

It got worse: an agent then *verified* the rig against the new cell and left a comment saying it
was fine and to leave it **byte-identical**. That comment was wrong, and it was wrong in the most
expensive way — it **pre-refuted the correct diagnosis**, so the next agent had to disprove a
teammate before fixing anything. **Do not write "verified, do not change" into the source unless
you measured it. A confident wrong comment costs more than no comment.**

**The diagnosis everyone reached for was also wrong, twice:**
- *"The key dies in mid-air / the lamp cannot reach."* **No.** `r_debugview 2` (point-light term
  alone) measured mean **57.7**, 2.6% void. **The lamp arrives.**
- *"The surfaces are asphalt"* (the 0.077-reflector class `fix/prim-point-light` had just fixed).
  **No.** `hh_floor_01a` = 0.462 linear × tint 0.40 = **0.185**; `hh_wall_01a` = 0.505 × 0.34 =
  **0.172**. Both already sit in the honest 0.18–0.20 band. **`r_debugview 5` (albedo forced flat
  0.5) still read mean 15.6** — *a room built entirely of 50% reflectors was still dark.* **When a
  WHITE room is dark, the surfaces are not the fault. Stop looking at albedo.**

**The real cause was FLUX and HANG HEIGHT — the two things the rescale actually changed:**
1. **42 m² of floor, still exactly ONE fixture.** The rest of this building lights a corridor with
   a practical every few metres (level1 registers **337** of them, each 3.2–3.3). The cell was
   asked to do 3× the volume with one lamp still tuned for the closet.
2. **The lamp hung 0.40 m under the ceiling.** With `pointAtten = w²/(d²+1)`, the **ceiling 0.40 m
   away caught 0.86** of it while the **floor 3.6 m below caught 0.056** — a **15:1 waste ratio.**
   The fixture was lighting the slab above itself. *That* is why the tube reads p95 233 while the
   deck beneath it reads p95 11.9.

**Neither dial fixes this alone** — and this is the part worth internalising: **raising RANGE could
never have worked.** The window term was already **0.79** at the floor, so 6.2 → ∞ buys **1.26×**.
And raising INTENSITY alone scorches the ceiling before the floor lights. The fix is to **hang it
lower** (0.40 → 1.10 m: floor atten 0.056 → 0.104, ceiling's share 0.86 → 0.45), **lengthen the
reach** (6.2 → 9.0), and **then** feed it (×2.73). Cool-white ratio preserved exactly.
**RESULT (4 data-derived eye cameras, flashlight OFF):** mean **6.7–10.1 → 21.7–24.2**, void
**65–80% → 24–34%**, p95 **19–27 → 65–75**, **0.00% clipped**. In line with the facility (level1
24.5% void). No ambient raised, no albedo over unity.

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
