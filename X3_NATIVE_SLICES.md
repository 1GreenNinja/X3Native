# X3 Native Engine — 100-Slice Build Plan

**Written**: 2026-05-20 by Opus 4.7, expanding the M0–M10 milestones in `X3_NATIVE_ENGINE_PLAN.md` into fine-grained, individually-shippable slices.
**Engine repo**: `1GreenNinja/X3Native` — **original work, proprietary** (see `LICENSE` / `PROVENANCE.md`).

> ## ⚠️ CORRECTION (2026-05-21) — built clean-room, not from RBDOOM
>
> This backlog was written assuming an RBDOOM fork. **The engine was instead built
> clean-room from scratch** (no id Tech / RBDOOM source). Two consequences:
> - **License is settled:** the engine is **original + proprietary** (not GPL-open, not hybrid). The "license-path drift" question below is resolved.
> - **M0 (Slices 1–8 — "clone/build/run RBDOOM") is OBSOLETE.** The foundation now in place is the *clean* one: render device, pak/VFS, console, glTF, physics, lighting, animation (see `PROVENANCE.md`). The de-GPL "D1–D8" track is also moot — those subsystems were written clean from the start.
>
> The feature slices from M1 onward (gameplay, content, FX, audio, UI, ship) remain a
> useful backlog — read them as targets, ignoring the RBDOOM-bringup framing.

**Read the plan first** (`X3_NATIVE_ENGINE_PLAN.md`) for rationale and stack. This doc is the executable backlog.

**Legend**: ✅ shipped · 🚧 in progress · ⛔ blocked-on-decision · 📝 planned

> ## 📊 Status snapshot (2026-06-05, branch `feat/cull-combined`)
>
> Headings below are stamped from a source-verified audit (every ✅ has
> concrete file:line evidence; anything thin was kept 🚧/📝 rather than
> marked done). M0 (Slices 1–8) is OBSOLETE and left unstamped.
>
> **✅ 37 shipped · 🚧 21 in progress · 📝 34 planned · ⛔ 8 obsolete (M0)**
>
> - **Strong / mostly shipped:** M1 Vulkan 1.3 (9–15, 17), M2 glTF/PBR (21–26),
>   M3 Jolt physics + CharacterVirtual (31–42, near-complete), M5 game slice
>   (53–64 in C++), core post-FX (SSAO/SSGI/bloom/ACES/compute-particles),
>   custom GUI + HUD + menus (85–88), `.x3pak` mount/priority/override (67, 69).
> - **Biggest gaps (📝, no trace in tree):**
>   - **M4 Lua/sol3 scripting (43–52) — entirely absent.** Gameplay the plan
>     assumed would be Lua (weapons 55, AI 56) is implemented in C++ instead.
>   - **No Dear ImGui** (83–84) — only a custom UI helper.
>   - **No Steamworks** (95–96) and no ship/packaging pipeline (97–100).
>   - **Advanced post-FX:** DoF (74), motion blur (75), OIT (76), TAA/DLSS (81),
>     quality presets (82).
> - **Notable 🚧 nuances:** timeline semaphores are *enabled* but frame pacing
>   still uses fences/binary semaphores (14); `+game` pak-launch is a stub —
>   the engine ships one exe with a `--world` selector (68); navmesh is a custom
>   grid-A* rather than Recast/Detour (57).

---

## Gates before starting — RESOLVED

- **G1 — License decision**: ✅ resolved — **original engine, proprietary** (all rights reserved). No fork, no GPL.
- **G2 — Foundation**: ✅ resolved — the clean foundation is built and running (render device, pak/VFS, console, glTF, physics, lighting, animation). No RBDOOM bring-up was needed.

---

# M0 — Foundation (Slices 1–8) — OBSOLETE (was RBDOOM bring-up)

> These slices described locating/building/running RBDOOM. **Not applicable** — the
> engine was built clean-room from scratch. Kept only for historical context; the
> equivalent clean foundation is recorded in `PROVENANCE.md`.

## Slice 1 — Locate + survey the RBDOOM source
**Goal**: Find the RDOOM/RBDOOM-3-BFG/"Ardoom 2019" tree on the 14900K. Document its delta from upstream RBDOOM-3-BFG + the Vulkan backend version.
**Output**: `docs/SOURCE_SURVEY.md` — path, git remote (if any), upstream delta, Vulkan version, build system state.
**Gate**: we know exactly what we're starting from.

## Slice 2 — CMake + vcpkg bootstrap
**Goal**: Generate a VS2022 x64 solution from RBDOOM's CMake. Resolve dependency gaps via vcpkg manifest.
**Files**: `CMakeLists.txt` patches, `vcpkg.json`.
**Gate**: solution generates without missing-dependency errors.

## Slice 3 — Vanilla build (Release x64)
**Goal**: `RBDoom3BFG.exe` (or renamed) compiles clean in Release x64.
**Gate**: build succeeds, exe produced.

## Slice 4 — Vanilla run + input
**Goal**: Launch the exe, it renders the menu, takes keyboard/mouse input. (Needs Doom 3 BFG base assets present — note where they are.)
**Gate**: game launches, renders, responds to input.

## Slice 5 — Vulkan validation-layer pass on RTX 5090
**Goal**: Run with Vulkan validation layers ON. Capture + triage any errors. Risk check #1 from the plan.
**Output**: `docs/VULKAN_VALIDATION_BASELINE.md`.
**Gate**: renders clean OR known-benign warnings documented.

## Slice 6 — Physics smoke test
**Goal**: Spawn a falling object via the existing id physics; verify it collides. Risk check #2.
**Gate**: object falls + collides. (This validates the layer we'll later replace with Jolt.)

## Slice 7 — Pak mount + asset read
**Goal**: Confirm `.pk4` mounting works; read a known asset by virtual path. Risk check #3.
**Gate**: asset loads from pak.

## Slice 8 — Tracy profiler integration
**Goal**: Hook Tracy into the frame loop. Single biggest engine-dev productivity tool — do it now.
**Files**: Tracy submodule, frame-mark macros in the main loop.
**Gate**: Tracy connects, shows per-frame CPU timeline. **→ M0 GO/NO-GO decision (G2).**

---

# M1 — Vulkan 1.1 → 1.3 upgrade (Slices 9–20)

## Slice 9 ✅ — Bump Vulkan headers + loader to 1.3
**Goal**: Update the Vulkan SDK target + VkInstance/VkDevice creation to apiVersion 1.3.
**Gate**: device creates at 1.3, validation clean.

## Slice 10 ✅ — Enable required 1.3 features
**Goal**: Turn on dynamic rendering, synchronization2, descriptor indexing, timeline semaphores via VkPhysicalDeviceVulkan13Features + 12Features.
**Gate**: features query + enable succeeds on 5090.

## Slice 11 ✅ — Migrate to dynamic rendering
**Goal**: Replace VkRenderPass/VkFramebuffer objects with vkCmdBeginRendering. Removes a whole class of boilerplate.
**Files**: renderer backend.
**Gate**: scene renders identically, no render-pass objects remain in the main path.

## Slice 12 ✅ — Descriptor indexing / bindless textures
**Goal**: Move material textures into a bindless descriptor array. Foundation for high draw-count perf.
**Gate**: materials sample from a bindless table; validation clean.

## Slice 13 ✅ — synchronization2 barriers
**Goal**: Replace legacy pipeline barriers with VkDependencyInfo / vkCmdPipelineBarrier2.
**Gate**: no sync hazards under validation's sync layer.

## Slice 14 🚧 — Timeline-semaphore frame pacing
**Goal**: Replace binary-semaphore + fence frame management with timeline semaphores.
**Gate**: clean multi-frame-in-flight, no stalls in Tracy.

## Slice 15 ✅ — VMA (Vulkan Memory Allocator) integration
**Goal**: Route all device allocations through VMA if not already.
**Gate**: no manual vkAllocateMemory in hot paths; budget visible.

## Slice 16 📝 — Render-parity screenshot diff
**Goal**: Capture before/after screenshots of the same scene; verify visual parity post-upgrade.
**Output**: `docs/RENDER_PARITY.md`.
**Gate**: pixel-diff within tolerance.

## Slice 17 ✅ — GPU timestamp queries
**Goal**: Per-pass GPU timing surfaced in Tracy. Know where frame time goes.
**Gate**: GPU pass timings visible.

## Slice 18 📝 — Pipeline cache to disk
**Goal**: Serialize VkPipelineCache to disk; cuts shader-stutter on subsequent launches.
**Gate**: second launch noticeably faster pipeline creation.

## Slice 19 📝 — Shader hot-reload (dev)
**Goal**: File-watch shader sources, recompile to SPIR-V, rebuild pipeline live.
**Gate**: edit a shader, see the change without restart.

## Slice 20 📝 — Validation-clean CI gate
**Goal**: A scripted headless run with validation that fails on any error — wire into the build.
**Files**: `scripts/validate_run.ps1`.
**Gate**: green = no validation errors. **→ M1 done: renders at parity, validation-clean.**

---

# M2 — glTF 2.0 / GLB loader (Slices 21–30)

## Slice 21 ✅ — cgltf integration
**Goal**: Parse GLB/glTF via cgltf alongside existing md5.
**Files**: `engine/asset/ModelLoader.cpp` / `engine/asset/IModelLoader.h`.
**Gate**: a GLB's node graph + meshes parse into engine structures.

## Slice 22 ✅ — Static mesh import + render
**Goal**: One of Tim's 140 GLBs renders in-engine (geometry only).
**Gate**: GLB appears in viewport.

## Slice 23 ✅ — PBR metallic-roughness materials from glTF
**Goal**: Map glTF PBR (baseColor/metallic/roughness/normal/emissive/occlusion) to the engine's own clean-room PBR (`shaders/mesh.frag`; importer in `engine/asset/ModelLoader.cpp`). *(Note: much of this is already implemented — see audit.)*
**Gate**: GLB renders with correct PBR shading.

## Slice 24 🚧 — KTX2 / Basis Universal textures
**Goal**: Load KTX2 textures via basisu; transcode to BC7/ASTC at load.
**Files**: `basisu` integration.
**Gate**: a KTX2-textured GLB renders; VRAM lower than raw.

## Slice 25 ✅ — glTF skinned animation
**Goal**: Skeletal animation from glTF (joints + inverse-bind + animation channels).
**Gate**: an animated character GLB plays its clip.

## Slice 26 ✅ — Animation blending
**Goal**: Crossfade between clips (idle→walk→run).
**Gate**: smooth transitions, no pops.

## Slice 27 🚧 — Material instance system
**Goal**: Share base materials, override per-instance params (tint, emissive scale) without duplicating.
**Gate**: 100 instances of one material, 1 pipeline.

## Slice 28 🚧 — Mesh LOD support
**Goal**: glTF LOD extension or distance-based LOD switching.
**Gate**: distant meshes drop to lower LOD; Tracy shows tri reduction.

## Slice 29 📝 — Asset hot-reload (GLB)
**Goal**: Re-import a GLB on file change in dev mode.
**Gate**: edit a GLB, see it update live.

## Slice 30 🚧 — Batch-convert Tim's 140 GLBs
**Goal**: Pipeline pass importing all existing converted GLBs; catalog which load clean vs need fixing.
**Output**: `docs/GLB_IMPORT_REPORT.md`.
**Gate**: ≥90% of the 140 load + render. **→ M2 done.**

---

# M3 — Jolt Physics + character controller (Slices 31–42)

## Slice 31 ✅ — Jolt integration + CMake
**Goal**: Vendored Jolt, building in the solution.
**Gate**: Jolt compiles + links.

## Slice 32 ✅ — Physics world + fixed-step update
**Goal**: A JPH::PhysicsSystem ticking at a fixed timestep, decoupled from render rate.
**Gate**: world steps deterministically.

## Slice 33 ✅ — Static collision from level geometry
**Goal**: Convert level brushes/meshes into Jolt static bodies.
**Gate**: a dropped box rests on the floor.

## Slice 34 ✅ — Rigid-body dynamics
**Goal**: Dynamic bodies with mass/restitution/friction; sleeping.
**Gate**: a stack of boxes settles + sleeps.

## Slice 35 ✅ — Character controller (capsule)
**Goal**: JPH::CharacterVirtual capsule walks on heightfield, collides with walls, steps stairs.
**Gate**: capsule walks + collides + climbs steps. **(Plan's M3 gate.)**

## Slice 36 🚧 — Player movement feel tuning
**Goal**: Accel/decel/air-control/jump matching the Babylon X3 reference feel.
**Reference**: current Babylon X3 movement tuning values.
**Gate**: side-by-side feel parity.

## Slice 37 ✅ — Raycast / shapecast queries
**Goal**: Expose ray + shape casts (for weapons, AI line-of-sight, interaction).
**Gate**: a ray from camera hits geometry + reports normal/distance.

## Slice 38 ✅ — Collision layers + filtering
**Goal**: Player/enemy/projectile/world layers with a collision matrix.
**Gate**: projectiles pass through their owner, hit enemies.

## Slice 39 ✅ — Trigger volumes
**Goal**: Overlap-only sensor bodies firing enter/exit callbacks.
**Gate**: walk into a trigger → callback fires.

## Slice 40 📝 — Physics debug draw
**Goal**: Jolt debug renderer wired to the engine's line renderer (collision shapes, contacts).
**Gate**: toggle shows capsule + static shapes.

## Slice 41 ✅ — Multi-threaded job system bridge
**Goal**: Point Jolt's JobSystem at the engine's thread pool (or its own).
**Gate**: physics scales across cores in Tracy.

## Slice 42 ✅ — Determinism + replay hooks
**Goal**: Seeded determinism for the same input sequence (foundation for netcode + replays).
**Gate**: same inputs → same final state across 2 runs. **→ M3 done.**

---

# M4 — Lua / sol3 scripting (Slices 43–52)

## Slice 43 📝 — sol3 + LuaJIT/Lua 5.4 VM
**Goal**: Embed sol3 + a Lua runtime; a `.lua` runs at startup.
**Gate**: "hello from Lua" logs.

## Slice 44 📝 — Engine→Lua API: logging + cvars
**Goal**: Bind `print`, `cvar.get/set`, `log` into Lua.
**Gate**: Lua reads/writes a cvar.

## Slice 45 📝 — Entity spawn API
**Goal**: Lua can spawn an entity by type at a position.
**Gate**: a Lua call spawns a visible mesh entity. **(Plan's M4 gate.)**

## Slice 46 📝 — Transform + movement API
**Goal**: Lua reads/sets entity transform; a script moves an entity over time.
**Gate**: scripted entity slides across the floor.

## Slice 47 📝 — Event/callback bridge
**Goal**: Engine events (OnTick, OnCollision, OnTrigger, OnDamage) callable into Lua handlers.
**Gate**: a Lua OnCollision prints on contact.

## Slice 48 📝 — Lua hot-reload
**Goal**: File-watch Lua scripts, reload + re-bind without engine restart.
**Gate**: edit a behavior script, see it live.

## Slice 49 📝 — Coroutine-based sequencing
**Goal**: Lua coroutines for timed sequences (cutscenes, ability combos, spawner waves).
**Gate**: a coroutine schedules 3 timed events.

## Slice 50 📝 — Lua error sandboxing
**Goal**: Script errors don't crash the engine; logged with stack + script/line. pcall wrapping.
**Gate**: deliberate Lua error → logged, engine survives.

## Slice 51 📝 — Lua physics + raycast API
**Goal**: Expose Jolt raycasts + apply-impulse to Lua.
**Gate**: Lua weapon script raycasts + applies knockback.

## Slice 52 📝 — Lua API doc generator
**Goal**: Auto-emit the bound API surface to `docs/LUA_API.md` from the sol3 binding tables.
**Gate**: doc lists every bound function. **→ M4 done.**

---

# M5 — X3 vertical slice (Slices 53–64)

## Slice 53 ✅ — Import one X3 arena (geometry)
**Goal**: Bring a single X3 level's geometry into the native engine (from Babylon level data → GLB/level format).
**Gate**: arena renders + is walkable.

## Slice 54 ✅ — Player pawn + camera
**Goal**: First/third-person camera + character controller wired for the arena.
**Gate**: walk the arena, look around.

## Slice 55 🚧 — Player weapon (1) in Lua
**Goal**: One weapon — fire, raycast/projectile, muzzle FX, damage application — scripted in Lua.
**Gate**: shoot, hit a target, damage registers.

## Slice 56 🚧 — Bot AI (1) in Lua
**Goal**: One enemy bot — patrol, aggro on sight, chase, attack — Lua state machine using engine raycasts + navmesh.
**Gate**: bot finds + engages the player.

## Slice 57 🚧 — Navmesh generation
**Goal**: Bake or runtime-build a navmesh over the arena (Recast/Detour).
**Gate**: bot paths around obstacles, not straight-line.

## Slice 58 ✅ — Health / damage / death
**Goal**: HP component, damage events, death + respawn for player + bot.
**Gate**: kill the bot, die to the bot, both respawn.

## Slice 59 ✅ — HUD (minimal)
**Goal**: HP + ammo + crosshair via the custom in-game GUI (placeholder OK pre-M8).
**Gate**: HUD reflects state.

## Slice 60 🚧 — Weapon feel pass
**Goal**: Recoil, screen-shake, hit feedback, fire-rate tuned to the Babylon X3 reference.
**Gate**: feels good side-by-side with the reference.

## Slice 61 ✅ — Combat audio (placeholder)
**Goal**: Fire/impact/death sounds via temp audio path (full audio is M9).
**Gate**: actions have audible feedback.

## Slice 62 ✅ — Particle FX (muzzle, impact, blood/spark)
**Goal**: Basic CPU or compute particles for combat moments.
**Gate**: muzzle flash + impact puff visible.

## Slice 63 🚧 — Win/lose loop
**Goal**: Frag count → round end → restart. The minimal game loop.
**Gate**: a complete match start-to-finish.

## Slice 64 🚧 — Vertical-slice playtest + capture
**Goal**: Record a clean playthrough of the slice; document FPS on the 5090.
**Output**: `docs/VERTICAL_SLICE_REPORT.md` + capture.
**Gate**: playable X3 vertical slice, native. **→ M5 done (keystone milestone).**

---

# M6 — .x3pak pipeline (Slices 65–72)

## Slice 65 🚧 — Pak format spec
**Goal**: Define `.x3pak` (zipped, virtual paths, manifest). Document it.
**Output**: `docs/X3PAK_FORMAT.md`.
**Gate**: spec reviewed.

## Slice 66 📝 — Pak builder tool
**Goal**: CLI that bakes a folder of assets + Lua + levels into a `.x3pak`.
**Files**: `tools/x3pakbuild/`.
**Gate**: produces a valid pak from the X3 slice content.

## Slice 67 ✅ — Pak mount + virtual filesystem
**Goal**: Engine mounts multiple paks (base.x3pak + game pak), resolves assets by virtual path with override order.
**Gate**: asset loads from the right pak by priority.

## Slice 68 🚧 — `+game` launch flag
**Goal**: `X3Engine.exe +game x3` mounts x3.x3pak; `+game ttt1995` mounts another.
**Gate**: two different games launch from one exe.

## Slice 69 ✅ — Pak hot-reload (dev)
**Goal**: Loose-files-override-pak in dev so editing is fast; pak is the shipping path.
**Gate**: edit loose file, see change; ship reads pak.

## Slice 70 📝 — Asset compression + dedup
**Goal**: Compress pak contents; dedup identical assets across paks.
**Gate**: pak smaller than raw folder, no dupes.

## Slice 71 📝 — Pak integrity / version check
**Goal**: Manifest checksum; engine refuses mismatched-version paks gracefully.
**Gate**: tampered pak → clear error, no crash.

## Slice 72 📝 — X3 slice loads entirely from pak
**Goal**: The M5 vertical slice runs with zero loose files — 100% from x3.x3pak.
**Gate**: clean-folder run from pak only. **→ M6 done.**

---

# M7 — Modern post-FX (Slices 73–82)

## Slice 73 ✅ — GPU compute particle system
**Goal**: Compute-shader particles (emit/simulate/sort) for high counts.
**Gate**: 100k particles at frame budget.

## Slice 74 📝 — Depth of field
**Goal**: Cinematic DoF (bokeh) post-pass.
**Gate**: focus pull looks clean (matches Tim's cinematic standard).

## Slice 75 📝 — Motion blur (per-object + camera)
**Goal**: Velocity-buffer-driven motion blur.
**Gate**: fast motion blurs without smearing static geo.

## Slice 76 📝 — Order-independent transparency (OIT)
**Goal**: Weighted-blended or per-pixel-linked-list OIT for glass/particles.
**Gate**: overlapping transparents sort correctly.

## Slice 77 ✅ — SSAO / GTAO
**Goal**: Ground-truth ambient occlusion pass.
**Gate**: contact shadows in corners.

## Slice 78 🚧 — Screen-space reflections
**Goal**: SSR for wet/metal surfaces.
**Gate**: floor reflects nearby geometry.

## Slice 79 ✅ — Tonemap + color grading
**Goal**: ACES tonemap + LUT-based grading (matches Babylon X3 look).
**Gate**: visual parity with Babylon reference.

## Slice 80 ✅ — Bloom tuning + lens dirt
**Goal**: Tune the engine's own bloom (`shaders/bloom_down.frag` / `shaders/bloom_up.frag`); optional lens-dirt overlay.
**Gate**: highlights glow tastefully, not blown out.

## Slice 81 📝 — TAA / DLSS hook
**Goal**: Temporal AA; optionally wire DLSS/FSR (RTX 5090 → DLSS).
**Gate**: clean edges, no ghosting; DLSS toggle works.

## Slice 82 📝 — Post-FX quality presets
**Goal**: Low/Med/High/Ultra presets gating the above (port the Babylon X3 quality-preset philosophy).
**Gate**: each preset measurable FPS delta. **→ M7 done: visual parity with Babylon X3.**

---

# M8 — UI: Dear ImGui (dev) + custom in-game GUI (Slices 83–90)

## Slice 83 📝 — Dear ImGui dev overlay
**Goal**: ImGui integrated for dev tools (entity inspector, cvar editor, perf HUD).
**Gate**: F1 toggles a working dev panel.

## Slice 84 📝 — Entity inspector
**Goal**: Select an entity, view/edit its components live in ImGui.
**Gate**: tweak a value, see it in-world.

## Slice 85 ✅ — Custom in-game GUI framework
**Goal**: Lightweight retained/immediate GUI for gameplay UI (NOT ImGui aesthetic). Widgets: panel, text, button, bar, image.
**Gate**: a styled panel renders + takes clicks.

## Slice 86 ✅ — HUD (full)
**Goal**: Production HUD — HP/ammo/score/minimap/crosshair — in the custom GUI.
**Gate**: HUD reflects all gameplay state.

## Slice 87 🚧 — Pause + settings menu
**Goal**: Pause menu, graphics/audio/controls settings, key rebinding.
**Gate**: pause, change a setting, resume.

## Slice 88 ✅ — Main menu + level select
**Goal**: Title screen, play, settings, quit.
**Gate**: navigable from launch to in-game.

## Slice 89 📝 — GUI skinning system
**Goal**: Data-driven GUI theme (colors/fonts/9-slice) — mirror of Riftforged's skin idea.
**Gate**: swap a theme asset, UI restyles.

## Slice 90 📝 — Localization scaffold
**Goal**: String table + locale switch (en first).
**Gate**: UI strings come from a table. **→ M8 done.**

---

# M9 — Audio + Steamworks (Slices 91–96)

## Slice 91 🚧 — miniaudio integration
**Goal**: Add the miniaudio audio backend (clean-room; there is no prior "id sound" to replace); play a 2D sound. *(Note: `engine/audio/MiniaudioSystem.cpp` already exists — IN PROGRESS per PROVENANCE.)*
**Gate**: a sound plays.

## Slice 92 🚧 — 3D spatial audio (HRTF)
**Goal**: Positional sources + listener from camera; HRTF.
**Gate**: sound pans + attenuates with position.

## Slice 93 🚧 — Music + ambience system
**Goal**: Streaming music, crossfade, ambient beds, ducking.
**Gate**: music crossfades on area change; ducks under combat.

## Slice 94 🚧 — Audio bus mixer
**Goal**: Master/SFX/Music/Voice buses with volume control (mirror Marble TTT's bus model).
**Gate**: per-bus volume in settings.

## Slice 95 📝 — Steamworks SDK integration
**Goal**: Steam init, overlay, achievements, cloud-save hooks.
**Gate**: Steam overlay opens in-game; a test achievement fires.

## Slice 96 📝 — Audio + Steam quality pass
**Goal**: Mix levels, achievement set, rich presence.
**Gate**: shippable audio + Steam presence. **→ M9 done.**

---

# M10 — Ship (Slices 97–100)

## Slice 97 🚧 — Release build + packaging
**Goal**: Optimized Release build; bundle `X3Engine.exe` + `base.x3pak` + `x3.x3pak` into a distributable.
**Gate**: a packaged folder runs.

## Slice 98 📝 — Clean-PC test
**Goal**: Run the package on a PC without VS / SDKs / dev tools (the 13700K or a VM). Catch missing redists.
**Gate**: launches + plays on a clean machine.

## Slice 99 📝 — Steam page + depot upload
**Goal**: Steam store page assets; SteamPipe depot build; Steam-key smoke test.
**Gate**: installable via Steam (beta branch).

## Slice 100 📝 — Public X3 build live
**Goal**: First shippable X3 on the native engine, published. Day-1 patch branch ready.
**Gate**: a stranger can buy/download + play X3 native. **→ engine v1 + first game shipped.**

---

# Cross-cutting concerns

- **Engine/data boundary**: the engine (`X3Engine.exe`) is original, proprietary IP; game data (`.x3pak`) + Lua (obfuscated) are the per-game sellable layer. Every game ships as a pak, not as an engine fork.
- **Babylon X3 is the reference, not deleted**: cribs gameplay feel, tuning values, level layouts, content. It's also the shippable fallback if native slips.
- **Profiler-first**: Tracy from Slice 8. Every perf claim is measured, not guessed.
- **Validation-clean always**: Vulkan validation is a CI gate (Slice 20). Never merge with new validation errors.
- **Parallel-agent compression**: the plan estimates ~15 weeks solo → ~5-6 weeks with parallel agents. Slices within a milestone parallelize; milestones are mostly sequential (M1 needs M0, etc.).
- **Many games, one engine**: every slice should ask "does this belong in the engine or in a game pak?" Engine = generic; game-specific logic = Lua in the pak.

---

# Recommended first session (on the 14900K)

Do M0 in order — it's a gate, not a buffet:
1. **Slice 1** — survey the source (you can't plan without knowing the delta)
2. **Slice 2-3** — CMake/vcpkg + vanilla build
3. **Slice 4** — vanilla run
4. **Slice 5-7** — the 3 risk checks (Vulkan validation, physics, pak)
5. **Slice 8** — Tracy, then **make the G2 go/no-go call**

If G2 is green, M1 (Vulkan 1.3) is the next focused session. If red, fall back to the BabylonNative port per the April feasibility doc and shelve this plan.

**Heavy milestones to dedicate full multi-session blocks to**: M1 (Vulkan upgrade), M5 (vertical slice — the keystone), M7 (post-FX parity). **Lighter, parallelizable**: M2, M4, M6, M9.
