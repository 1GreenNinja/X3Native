# Level Architect — Feature Roadmap

The native level editor for **X3Native** (C++20 / Vulkan 1.3 GPU-driven renderer, bindless
textures, Jolt physics, GLB assets, Dear ImGui UI). This roadmap synthesizes the **best of
Unreal Engine 5 (5.5–5.7)** and **Unity 6 (6000.x)** editors into a plan that is feasible in
*our own* engine — no Unity/UE runtime, everything ported as native C++/Vulkan/ImGui code on
top of the substrate we already have.

> Status date: 2026-05-31. Companion docs: `LEVEL_EDITOR_VISION.md` (owner vision),
> `WORLD_AND_EDITOR_PLAN.md` (E1/E2/E3 phases), `X3_WORLD_BLUEPRINT.md`,
> `SPIRE_LEVELARCHITECT_DIMS.md`.

---

## 1. Vision Recap — tied to what already exists

The owner's stated vision for Level Architect:

1. **First-person walkaround of the live level** — fly/walk through the actual rendered world
   while editing, not a top-down abstraction.
2. **Click a wall to retexture it**, with **AI style suggestions** offering area-aware material
   choices.
3. **A model browser on a 2nd monitor** — drag a GLB in, it floor-snaps into the world.
4. **Save → level JSON.** A data-driven level loader is the spine: edit in-engine → save →
   reload identical.

### What already exists (the substrate this roadmap builds on)

| Vision piece | Already in the tree | Gap |
|---|---|---|
| FP walkaround | Vulkan viewport + fly camera + PBR/shadows/bloom/SSAO/GI render the world today | No **editor mode** hosting edit input over the live viewport (`main.cpp:1299-1301` runs `--test-editor` then returns; no `--editor`/F9 live mode — F9 is quick-LOAD at `:7331-7338`) |
| Click-a-wall | `IPhysicsWorld::rayCast` returns `RayHit{point,normal,body,...}`; `Scene::entityForBody()` maps hit → entity; `Entity` carries `tex`/`baseColor`/`emissive` so retexture = swap `tex` + `createTexture` | No hit→surface→material→persist path; rooms have no per-surface material data (`f` flags field is empty in all data) |
| Model browser / drag-to-place | `env_art.cpp` `EnvArtSystem` proves GLB load → `ModelDrawable` → placed `EnvInstance` with transform+emissive; `worldBounds`/`namedBounds` for framing | No asset enumeration, no thumbnail render-to-texture, no drag-to-place, no 2nd-monitor window |
| Save → JSON | Editor brain `LevelDoc` has `saveJson/loadJson` (hand-rolled writer + parser); loader `level_loader.cpp` parses the **canonical** `EscapeLab48_AllFloors_v2.project.json` (7 floors, 124 rooms, 160 doors) | **Two divergent formats**: editor's flat `entities[]` vs canonical floor/room/door schema. Editor can't read/write the real file, and canonical `entities[]`/`triggers[]` are EMPTY — nothing authors them |
| UI toolkit | Engine HUD primitives: `drawHudQuad`, `drawHudText`, `drawHudTextF`, `worldToScreen` | **Dear ImGui is NOT integrated** (no source, no `imgui_impl_vulkan/glfw`, no CMake ref). Naming it in docs ≠ having it |

**Net:** ~60% of the *plumbing* exists. The three foundational blockers — (a) no live editor
mode, (b) no UI toolkit integrated, (c) two incompatible JSON formats + a loader that can't
place objects — must be closed before any UE5/Unity6 feature-parity work has a home.

---

## 2. Prioritized Feature Roadmap

Each feature: **name** · **inspired-by** · **what it does** · **feasibility/approach in our
engine** (and whether it extends existing code). Difficulty noted as easy / medium / hard.

### TIER P0 — Foundation (you cannot ship an editor without these)

#### P0.0 — Integrate Dear ImGui (Vulkan + GLFW backend) — *blocking prerequisite*
- **Inspired by:** both (the docked-panel shell is universal).
- **What it does:** Vendors `imgui` + `imgui_impl_vulkan` + `imgui_impl_glfw`, with docking +
  multi-viewport branch, wired into the existing frame loop (one render pass after the scene).
- **Approach:** *medium.* New `third_party/imgui/`, CMake target, a backend init that shares
  our Vulkan device/queue/swapchain and the existing GLFW window. Docking branch gives us the
  dockspace + the second-monitor window for free (multi-viewport). This is the accelerator the
  plan assumes; do it first. Fallback (not recommended): hand-build panels on `drawHudQuad`/
  `drawHudText`.

#### P0.1 — Live Editor Mode (`--editor` / hotkey toggle)
- **Inspired by:** both (Scene View / Level Viewport).
- **What it does:** Boots the engine into edit mode hosting `EditorState` over the live
  rendered world; toggles edit-input vs game-input.
- **Approach:** *easy-medium.* `editor.cpp`/`.h` already implement the headless brain
  (`EditorState`, `pickRay`, `moveSelected`, `snapSelected`, `add/deleteEntity`). Instantiate it
  in the render loop, add a mode flag, resolve the **F9 conflict** (F9 = quick-load today;
  rebind editor toggle to F8 or move quick-load). Extends existing code directly.

#### P0.2 — Viewport navigation: Orbit / Fly / FPS-walk camera modes
- **Inspired by:** both. **What it does:** Three documented-but-stubbed camera modes; FPS-walk
  is the owner's literal "walk around the level" ask.
- **Approach:** *easy.* Fly cam exists. Add orbit (pivot on selection/focus point) and
  FPS-walk (Jolt character or constrained fly + gravity). Focus-frame (`F`) uses
  `worldBounds`/`namedBounds`.

#### P0.3 — Selection + TRS Gizmos + multi-grid snapping
- **Inspired by:** both (Q/W/E/R from UE; Gizmos/Handles from Unity).
- **What it does:** Click-pick select, marquee multi-select, translate/rotate/scale gizmos,
  three independent snap grids (move/rotate/scale), vertex + surface (drop-to-floor) snap,
  world/local space toggle.
- **Approach:** *medium.* `pickRay` + `moveSelected(axis,delta)` + grid snap already exist;
  `EditorTheme` even defines gizmo handle colors (W=green/E=blue/R=pink, X/Y/Z axes). Missing:
  rotate/scale **manipulation logic** (only `yaw`/`scale` fields exist), multi-select,
  gizmo rendering. **ImGuizmo** is a ready-made TRS handle drop-in over our `worldToScreen`.

#### P0.4 — Outliner / Hierarchy (scene tree)
- **Inspired by:** both. **What it does:** Searchable, filterable tree of all entities with
  editor-only folders, drag-to-reparent, per-row visibility/lock, type/name filter; shares one
  selection source-of-truth with viewport + inspector.
- **Approach:** *easy.* ImGui `TreeNode` over the `LevelDoc` entity list / ECS world + a folder
  string per entity. No engine changes.

#### P0.5 — Details / Inspector (reflection-driven property panel)
- **Inspired by:** both (UE Details + Unity Inspector). **What it does:** Context panel
  auto-rendering every editable property of the selection (typed exact transform, material
  slots, gameplay fields like door codes / enemy HP), multi-edit, search-in-properties.
- **Approach:** *medium.* ImGui widgets are trivial; the real work is a small **reflection /
  property-descriptor layer** (name, type, min/max, widget hint) over entity/component +
  material structs so editors auto-generate and serialize to JSON. Hand-rolled per-type table
  is the easy fallback. **This is the same metadata layer P1 material tweaking needs — build it
  once here.**

#### P0.6 — Undo/Redo command stack
- **Inspired by:** both. **What it does:** Universal undo/redo for every edit (move, add,
  delete, retexture, property change).
- **Approach:** *medium.* Declared in `editorMenuBar()` (Ctrl+Z/Y, Duplicate, Delete) but **no
  command stack exists.** Implement a command pattern (do/undo pairs) that all P0 ops route
  through. Foundational — retrofitting later is painful, so do it in P0.

#### P0.7 — JSON Save/Load to the CANONICAL format (close the loop)
- **Inspired by:** both. **What it does:** Editor reads and writes
  `EscapeLab48_AllFloors_v2.project.json` (floors/rooms/doors/entities/triggers) and **populates
  the empty `entities[]`/`triggers[]`** — round-trips identically.
- **Approach:** *medium-hard, #1 structural prerequisite.* Replace/extend `LevelDoc` so its
  in-memory model = the canonical schema (or add a converter). The loader's `CanonRoom`/
  `CanonDoorway` parse already exists; make the editor write the same shape. **Loader must also
  learn to instantiate `entities[]`/`triggers[]`** (today it builds room shells only) — otherwise
  authored props never appear on reload.

#### P0.8 — Content / Model Browser (+ 2nd-monitor window)
- **Inspired by:** both (UE Content Browser / Unity Project window). **What it does:** Thumbnailed,
  searchable, tag-filterable grid of GLBs/textures/materials; drag-into-viewport with floor-snap;
  mirrored to a 2nd monitor.
- **Approach:** *medium.* ImGui grid is easy. Needs (1) an asset index over the GLB/texture
  folders, (2) **offscreen thumbnail render** — one render of each asset to a small framebuffer
  with the existing renderer (reuse `env_art`'s `buildFromGlb` + `worldBounds` framing), (3)
  drag-to-viewport reusing `rayCast` floor-drop, (4) 2nd-monitor = an ImGui multi-viewport
  window (free once P0.0 docking branch is in). Reuses the proven `env_art`/GLB substrate.

#### P0.9 — Play-In-Editor toggle
- **Inspired by:** UE (PIE). **What it does:** One key flips the loaded session from edit →
  running gameplay at current state, with eject-back-to-flycam.
- **Approach:** *easy-medium.* The editor **is** the running engine (same process, same loaded
  level), so PIE is a mode flag: freeze edit input, hand control to the existing player+physics
  loop, hotkey to eject. State save/restore for clean re-entry is the only fiddly part. **Directly
  kills the documented stale-exe / kill-before-build / rebuild-relaunch pain.**

---

### TIER P1 — The "Architecting" Layer (geometry, lighting, materials, reuse)

#### P1.1 — Modeling / Brush Mode (native blockout)
- **Inspired by:** both (UE Modeling Mode + Unity ProBuilder). **What it does:** Create/edit
  geometry in-viewport — parametric primitives (box/stairs/arch/cylinder), vertex/edge/face edit,
  extrude/bevel, the Cut tool to carve doorways, per-face material slots, collision gen. The
  direct supercharged successor to the owner's Doom/Quake brush days + the clean-BSP-brush
  directive.
- **Approach:** *hard (flagship).* Needs a **half-edge / dynamic-mesh kernel**, sub-element
  selection, and live vertex/index upload to Vulkan. MVP = primitives + face-extrude +
  per-face material; defer booleans/UV/sculpt. Reuses ray-pick + gizmos + Jolt collision-gen as
  scaffolding. Per-face material assignment *is* the "click-a-wall retexture" surface.

#### P1.2 — Surface / Material assignment + texture swap
- **Inspired by:** both (Unity per-face material; UE Material Instances). **What it does:**
  Click a wall/face → assign or swap its material live.
- **Approach:** *easy-medium.* `rayCast` already returns the surface + normal; `Entity::tex` /
  `baseColor` / `emissive` are per-entity and bindless texture swap is cheap. Needs: map hit →
  surface/face, swap texture index, **persist into per-surface material data in the schema** (the
  unused `f` flags field, or a new `materials` block per room/face). Extends existing draw paths.

#### P1.3 — AI Texture / Asset Suggestion box  ⭐ signature vision feature
- **Inspired by:** novel (the owner's signature ask; UE has none of this). **What it does:** On
  clicking a surface, an area-aware recommender proposes curated materials ("this is a Medical
  Bay wall → suggest clean-white panel / biohazard trim / blood-decal variant").
- **Approach:** *medium.* Context = room `type` (already in schema: Cell/Lab/Medical/Armory/…)
  + nearby entities + the available material library. v1 = rule/heuristic map (room-type →
  ranked material set) surfaced as ImGui thumbnails next to P1.2's swap UI. v2 = embeddings /
  local LLM ranking, or our existing SD 3.5 pipeline (`reference_sd35_texgen`) to *generate* a
  bespoke variant on request. Builds on P1.2.

#### P1.4 — Lighting authoring (place/tune lights, no-bake live preview)
- **Inspired by:** both. **What it does:** Drag-place directional/point/spot/rect/sky lights with
  gizmos, tune intensity/color/temperature/attenuation in the Details panel, see it instantly.
- **Approach:** *easy-medium.* Renderer already does CSM/HDR/bloom/SSAO/GI shading; loader
  already auto-mints per-room ceiling lights (`buildCanonLights`) + visible-subset selection
  (cap 16/frame, device cap 64). Missing piece is purely the **interactive authoring loop**:
  light entities + gizmos + Details fields + persist to JSON. The "edit and see it now, no bake"
  loop is the biggest lighting QoL win.

#### P1.5 — Light Probes / Indirect-light authoring (APV-style)
- **Inspired by:** Unity (Adaptive Probe Volumes) + UE (Lumen feel). **What it does:** Place
  probe volumes; bake irradiance (SH) feeding the existing GI pass for higher-quality indirect.
- **Approach:** *hard.* Volume-placement UI is easy; the GI **bake** is the hard part. Pragmatic
  path: place volumes in-editor, bake offline via the existing GI/path-trace, store SH
  coefficients, sample at runtime. (Full real-time Lumen needs hardware-RT — flagged as the
  biggest 5090 upgrade — out of near-term scope. DDGI/voxel-cone-trace is the fallback for the
  live-update feel.)

#### P1.6 — Post-FX / Lighting-Scenario Volumes
- **Inspired by:** Unity (Volume framework + Lighting Scenarios) + UE (Data Layers /
  post-process volumes). **What it does:** Local volumes hold priority-blended post-FX overrides
  (bloom/tonemap/fog/grading/exposure) per region; switchable lighting states (day/night,
  power-on/off, pre/post-rescue) without duplicating geometry.
- **Approach:** *easy.* Post chain already exists. Add a `volume` entity (bounds + profile of
  param overrides) and lerp active post uniforms by camera position/priority. Maps directly onto
  the storyline's terminal-code / power-state / rescue mood shifts.

#### P1.7 — Prefab / Instance / Override / Variant system
- **Inspired by:** Unity (Prefabs, nested, variants, overrides) — highest-leverage idea.
- **What it does:** Each placed object = `{prefab_id, transform, override_map}`; loader resolves
  base + overrides (+ variant inheritance). Edit base → all instances update.
- **Approach:** *hard.* Requires a real override/diff + propagation model in the JSON schema and
  loader. Build incrementally: start with **instance + flat override**, add nesting + variants
  later. This is the biggest content-reuse / authoring-speed multiplier (ship N rooms/props from
  one base).

#### P1.8 — Procedural-asset live params (FORGE3D-style planet tweaking)  ⭐
- **Inspired by:** Unity (Planets pack: shader exposes ~60 typed ranged params; Inspector
  auto-renders sliders; live edits; "Save as variant" → `.mat`). **What it does:** A material/
  shader declares a manifest of typed params (name,type,min,max,default,widget,texture-slot); the
  editor auto-generates ImGui controls; dragging a slider writes straight into the bindless
  material UBO and re-renders instantly; "Save as variant" serializes the param set to level/
  material JSON.
- **Approach:** *medium — no node graph required.* We already render these PBR scenes in-engine.
  New work = the **param-manifest → auto-ImGui → live-UBO → save-variant** loop, reusing the P0.5
  reflection layer. This reproduces the entire FORGE3D live-tweak workflow and is an explicit
  owner ask. (Full node-graph material editor → SPIR-V is *hard* and deferred; the param manifest
  is the high-value 90%.)

---

### TIER P2 — Scale & Authoring Power (deferred, high-effort)

#### P2.1 — Foliage / Scatter paint (instanced)
- **Inspired by:** both (UE Foliage Mode + Unity Paint Details/Trees). **What it does:**
  Brush-paint instanced meshes (rubble, pipes, seafloor clutter, vegetation) with density/scale/
  rotation jitter + erase.
- **Approach:** *medium.* Brush = raycast against surfaces to seed instance transforms with
  jitter; feeds straight into the GPU-driven instance pipeline. Brush UI + jitter sampler; no new
  render tech.

#### P2.2 — PCG / rule-based scatter
- **Inspired by:** UE (PCG Framework). **What it does:** Author a rule ("consoles along walls,
  barrels in corners, avoid doorways") and regenerate placements.
- **Approach:** *medium for rule/script-driven* (emit placements into level JSON from data-driven
  rules + spline/area inputs); *hard for a real-time node graph.* Start rule-based; visual graph
  later. Aligns with the realistic-environments curated-not-grid goal.

#### P2.3 — Terrain sculpt / paint
- **Inspired by:** both. **What it does:** Heightfield brush sculpt (raise/lower/smooth/flatten/
  erosion), multi-layer splat texture paint, instanced scatter — for mountains, surface ring,
  seafloor.
- **Approach:** *medium, self-contained.* Heightmap texture + tessellated/clipmap mesh +
  compute-shader brush stamping into height/splat maps; **Jolt heightfield collision shape is
  ready to consume it.** Triplanar shading already exists in the Planets shaders.

#### P2.4 — Sequencer / Timeline (cinematics)
- **Inspired by:** both (UE Sequencer + Unity Timeline/Cinemachine). **What it does:** Track/
  keyframe timeline animating cameras/transforms/materials + an event track; virtual cameras
  (follow/aim/noise, priority blends). Serves the cold-open (45s ship flight, shot down, wake in
  cell) + L2 interrupt-rescue beats + 3P follow-cam intent.
- **Approach:** *medium.* Build the **runtime track/clip evaluation + vcam blend solver** first
  (data model, drivable by the existing GLB animation/skinning system + entity transforms), add
  the ImGui timeline UI second. Author vcams as entities to start — no UI strictly required.

#### P2.5 — World Partition / streaming + Data Layers
- **Inspired by:** UE. **What it does:** Auto grid-cell streaming of one persistent world
  (cells load/unload around the camera), a 2D minimap to box-select the region you edit, plus
  logical Data Layers (lighting scenarios, pre/post-rescue states, debug-only) toggled
  orthogonally. The only way the huge vertical world (Spire 283m / seafloor / maze / surface)
  stays editable + shippable.
- **Approach:** *medium-hard.* GPU-driven multidraw + ECS instance pipeline + the loader's
  **portal-PVS flood-fill cull** already exist, so cells are mostly spatial bucketing (quadtree/
  grid over level JSON) + async GLB load/unload keyed to camera distance — no RHI changes. Hard
  parts: async asset residency + eviction (Unity-Addressables-style key→GUID→refcounted async
  load + catalog manifest) and the minimap UI. **Data Layers are *easy*** — a layer bitmask field
  per entity + a visibility panel; rendering/streaming already iterate entities.

#### P2.6 — Visual scripting / trigger-event graph
- **Inspired by:** UE (Blueprint) — but the realistic subset, not a full VM. **What it does:**
  Author gameplay logic in-editor: trigger volumes + condition + action lists (door triggers,
  spawners, terminal-code unlocks, secret-room reveals, rescue beats) serialized to JSON, so the
  logic Tim wants lives in data, not hardcoded in `level1.cpp`.
- **Approach:** *medium for the trigger/event subset* (volumes + condition + action lists in
  JSON, interpreted at runtime — the canonical schema already has an empty `triggers[]` array
  waiting); *hard for a full visual-scripting VM.* A tiny embedded scripting language is an
  alternative middle path. Start data-driven triggers.

---

## 3. First 5 Things to Build Next (shortlist)

Ordered to unblock everything else and deliver a visibly working editor fastest.

1. **Integrate Dear ImGui (Vulkan+GLFW, docking branch)** — *P0.0.* Nothing else has a home
   without it; the docking/multi-viewport branch also hands us the 2nd-monitor window for free.
   This corrects the plan's wrong assumption that ImGui is already in.
2. **Live Editor Mode + viewport nav** — *P0.1 + P0.2.* Instantiate the already-tested
   `EditorState` brain over the live viewport, resolve the F9 conflict, add orbit/FPS-walk. This
   is the first moment the owner can literally "walk around the level and edit," and it's
   mostly *wiring*, not new systems.
3. **Canonical JSON save/load + loader entity instantiation** — *P0.7.* Make the editor read/
   write `EscapeLab48_AllFloors_v2.project.json` and teach the loader to place `entities[]`/
   `triggers[]` (today it only builds room shells). Without this, nothing the owner places
   survives a reload — it's the #1 structural prerequisite and gates all content authoring.
4. **Selection + TRS gizmos + snapping + undo/redo + Outliner + Details** — *P0.3–P0.6.* The core
   manipulation loop. `pickRay`/`moveSelected`/snap exist; add rotate/scale logic (ImGuizmo), the
   command stack (do this now — retrofitting undo later is painful), and the two ImGui panels that
   share one selection state. After this it *feels* like a real editor.
5. **Content/Model Browser with thumbnails + drag-to-place, and Play-In-Editor toggle** —
   *P0.8 + P0.9.* Realizes the "drag a model in, it floor-snaps" vision (reusing the proven
   `env_art`/GLB substrate + offscreen thumbnail render) and the PIE toggle that ends the
   documented stale-exe / rebuild-relaunch pain — a huge daily-iteration win for low effort.

**Rationale:** these five turn a *tested headless library* into a *usable editor* and close all
three foundational blockers (no live mode, no UI, divergent/empty JSON). Only after the P0
foundation is real should the flagship P1 work (Brush Mode, click-a-wall + AI suggestion,
FORGE3D-style live material tweaking, prefab/variant reuse) begin.

---

## 4. Risks & Unknowns

- **ImGui integration friction.** Sharing our Vulkan device/queue/swapchain with `imgui_impl_vulkan`
  (descriptor pools, render pass ordering, the multi-viewport secondary swapchains for the
  2nd monitor) is the classic integration hazard. Budget real time; it gates everything.
- **JSON schema unification is invasive.** Collapsing the editor's flat `LevelDoc` and the
  canonical floor/room/door/entity/trigger schema risks breaking the mature `level_loader.cpp`
  (PVS cull, doorway resolver, descent tubes, per-room lights all key off this data). Prefer
  *extending* the loader's in-memory model over a parallel format. Round-trip identity must be a
  test (extend `--test-canonlevel`).
- **Loader entity instantiation is unbuilt and underspecified.** The loader places room shells
  only; there's no defined contract for how an `entities[]` record (type/asset/transform/params)
  becomes a live `Scene` entity + Jolt body + GLB drawable. This contract design is a real task,
  not just plumbing.
- **Mesh-editing kernel (P1.1) is genuine R&D.** A robust half-edge kernel with extrude/bevel/cut
  + live Vulkan buffer rebuilds + per-face materials is the single largest engineering item.
  Scope the MVP hard (primitives + face-extrude + per-face material) or it eats the schedule.
- **No hardware ray tracing today.** Full Lumen-style real-time GI and Virtual Shadow Maps are
  out of reach until the RT upgrade (flagged as the top 5090 item). P1.5 must commit to a
  baked-SH / DDGI / voxel-cone-trace fallback, not assume RT.
- **Reflection layer scope creep.** The auto-generating Details panel + FORGE3D param-manifest
  share one reflection/metadata layer. If it's over-engineered (full RTTI) it stalls; if too thin,
  every type needs hand-written editors. Aim for a lightweight descriptor table (name/type/
  min-max/widget) and grow it.
- **Async streaming residency (P2.5).** Cell load/unload eviction, GLB refcounting, and avoiding
  hitches during editor fly-through is the hard half of World Partition — the spatial bucketing
  itself is easy. Likely needs an Addressables-style catalog + budget/eviction policy.
- **Undo/redo retrofit cost.** If the command stack (P0.6) is deferred, every P0/P1 op written
  without it must be rewritten to route through commands. Build it early or pay later.
- **AI suggestion quality + latency (P1.3).** A local-LLM/embeddings recommender or on-demand
  SD 3.5 generation must stay responsive in the edit loop; a rule/heuristic v1 de-risks this and
  ships value immediately.
