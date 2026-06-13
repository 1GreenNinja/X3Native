# Level Architect — Concrete Phased Build Plan

The native level editor for **X3Native** (C++20 / Vulkan 1.3 GPU-driven renderer, Jolt
physics, GLB assets, GLFW). This is the *implementation* plan that turns the
[`LEVEL_ARCHITECT_ROADMAP.md`](LEVEL_ARCHITECT_ROADMAP.md) P0/P1/P2 feature roadmap into an
ordered, buildable-and-verifiable sequence of phases. It does **not** redo the roadmap — it
sequences and grounds it in the actual code seams that exist today (verified by reading the
tree on 2026-06-02).

## Decisions already locked

- **Dear ImGui is EDITOR-ONLY.** The game keeps its custom `FontRole` HUD
  (`drawHudQuad`/`drawHudText`/`drawHudTextF`, `IRenderDevice.h:617-648`). The ImGui render
  pass is cleanly separated from the game HUD pass.
- **The editor is gated to `--editor` mode** (and an in-game F8 toggle). With no `--editor`
  flag, ImGui never initializes — **zero cost, zero allocation** in the shipping game path.
- **One process, one engine.** The editor *is* the running engine (same `Scene` + physics +
  device + canon loader), so Play-In-Editor is a mode flag, not a relaunch.

## ⚠️ Hard scheduling gate (Phase 0)

> **Phase 0 must wait for the in-flight IBL graphics change to clear
> `engine/rhi/VulkanRenderDevice.cpp`.** That change already touches the same regions Phase 0
> edits — `buildAndExecuteGraph()` and the post/composite block (`kIblCubeFormat`/
> `kIblBrdfFormat` near 7727-7728, `ibl_*.frag` in `app/CMakeLists.txt:97-100`). Phase 0
> inserts a new render pass into `buildAndExecuteGraph()` and adds device members; landing it
> before IBL clears would force a painful rebase on both sides. **Rebase Phase 0 onto the
> post-IBL `VulkanRenderDevice.cpp`.** Phases 1–5 do not touch the renderer's graph and are
> not gated by IBL (Phase 2's grid texture uses the existing `createTexture` API).

---

## Phase list at a glance

| Phase | Deliverable | Roadmap | Effort | Gated on |
|---|---|---|---|---|
| **0** | Dear ImGui (docking) integrated into `VulkanRenderDevice`, `--editor` flag parsed, ImGui pass after composite | P0.0 | M | **IBL change clears `VulkanRenderDevice.cpp`** |
| **1** | `--editor` live mode + viewport nav (orbit/fly/FPS-walk) + ImGui dockspace + menubar/toolbar shell | P0.1, P0.2 | M | Phase 0 |
| **2** | **BLOCKOUT MODE**: grid material + box/ramp/cylinder/stairs brushes, grid-snap place/move/resize, `brushes[]` JSON, Jolt static collision (walkable), promote-to-art | P1.1 (blockout MVP) | L | Phases 1, 3-selection |
| **3** | Selection + TRS gizmos (ImGuizmo) + multi-grid snap + undo/redo command stack + Outliner + reflection-driven Inspector | P0.3–P0.6 | L | Phase 1 |
| **4** | Content/model browser (offscreen thumbnails + drag-to-place) + Play-In-Editor toggle | P0.8, P0.9 | L | Phases 1, 3 |
| **5** | Canonical JSON save/load round-trip + loader `entities[]`/`triggers[]`/`brushes[]` instantiation | P0.7 | L | Phases 2, 3 |

Phases 1→5 are individually buildable and verifiable. Phase 3's **multi-select + tagged
selection + command stack** are a soft prerequisite for Phase 2's brush move/resize and Phase
4's drag-place (both route edits through commands), so **build Phase 3's selection/command
core early** — it can land alongside or just before Phase 2. The headless data-model parts of
Phase 2 (`BlockoutBrush` struct + `brushes[]` JSON + self-test E7) and Phase 3 (multi-select,
three grids, command stack) have **no ImGui/live-mode dependency** and can proceed in parallel
while Phase 0 waits on IBL.

---

## Phase 0 — Dear ImGui (docking) integration into `VulkanRenderDevice`  [effort M]

**Goal:** draw editor panels into the swapchain image *after* the composite/HUD pass, with
zero cost when `--editor` is absent. ImGui stays editor-only; the game `FontRole` HUD is
untouched.

### Files to touch
- `vcpkg.json` — add `{"name":"imgui","features":["docking-experimental","glfw-binding","vulkan-binding"]}`.
- `CMakeLists.txt` — `find_package(imgui CONFIG REQUIRED)` after the other `find_package`s (~line 37).
- `engine/CMakeLists.txt` — link `imgui::imgui` **PRIVATE** to `x3engine` (keeps ImGui types
  out of `IRenderDevice.h`; the vcpkg port compiles `imgui_impl_vulkan.cpp` /
  `imgui_impl_glfw.cpp` *into* the package, so no `third_party/` vendoring and no new engine `.cpp`).
- `engine/rhi/IRenderDevice.h` — add **5 non-pure no-op virtuals** (header stays
  Vulkan/ImGui-type-free):
  `initEditorUI(void* glfwWindow)`, `beginEditorUI()`, `endEditorUI()`, `shutdownEditorUI()`,
  `editorUIActive() const`.
- `engine/rhi/VulkanRenderDevice.cpp` — new members + the 5 method impls + 1 graph pass +
  resize one-liner + shutdown hook.
- `app/main.cpp` — parse `--editor` (arg block ~1459, next to `--test-editor`); call sites in
  the interactive loop.

### Approach
1. **Device-side API split** (so CPU draw-data build happens outside the command buffer):
   - `beginEditorUI()` = `ImGui_ImplVulkan_NewFrame` + `ImGui_ImplGlfw_NewFrame` + `ImGui::NewFrame`.
   - editor host issues `Begin`/`End`/widget calls between begin and end.
   - `endEditorUI()` = `ImGui::Render()` + stash `m_editorDrawData = ImGui::GetDrawData()`
     (must run **before** `device->endFrame()` so the draw data exists when the graph records).
   - Actual GPU recording (`ImGui_ImplVulkan_RenderDrawData`) happens **inside** `endFrame`'s
     graph (step 3) — mirrors the existing `prepareFrameData`→`recordMeshDraws` split.
2. **New device members**, all defaulting to null/false (a non-editor run allocates nothing):
   `VkDescriptorPool m_imguiPool`, `bool m_imguiInit=false`, `ImDrawData* m_editorDrawData=nullptr`,
   plus `m_editorUiAttach`/`m_editorUiRenderInfo` member storage so the `VkRenderingInfo`/
   attachment structs outlive `execute()` (same pattern as composite at ~3130/3155).
3. **`initEditorUI(win)`** (guarded `!m_headless && !m_imguiInit`):
   - Create a **dedicated** descriptor pool (1000 `COMBINED_IMAGE_SAMPLER`,
     `FREE_DESCRIPTOR_SET` flag) — never reuse the bindless/HUD pools.
   - `IMGUI_CHECKVERSION`; `ImGui::CreateContext`; `io.ConfigFlags |= DockingEnable`
     (**not** `ViewportsEnable` yet — defer multi-viewport/2nd-monitor to a later piece).
   - `ImGui_ImplGlfw_InitForVulkan((GLFWwindow*)win, true)` — `true` **chains** the game's
     existing GLFW callbacks (key/char/scroll) so gameplay input still fires.
   - `ImGui_ImplVulkan_InitInfo` filled from the device's own handles:
     `Instance=m_inst.instance`, `PhysicalDevice=m_dev.physical_device`, `Device=m_dev.device`,
     `QueueFamily=m_gfxFamily`, `Queue=m_gfxQueue`, `DescriptorPool=m_imguiPool`,
     `MinImageCount/ImageCount=m_swapImages.size()`, `MSAASamples=1`,
     `UseDynamicRendering=true`, and a `PipelineRenderingCreateInfo{ colorAttachmentCount=1,
     pColorAttachmentFormats=&m_format, depthAttachmentFormat=UNDEFINED }` — **identical** to
     the HUD pipeline (`createHud`, ~3646-3648), which is why ImGui composites correctly onto
     the LDR composited image.
   - Fonts: imgui 1.90+ auto-uploads on first `RenderDrawData`; an older resolved port needs
     one `ImGui_ImplVulkan_CreateFontsTexture()`. **Branch on the resolved version.**
4. **New `editor-ui` graph pass** in `buildAndExecuteGraph()`, inserted **AFTER composite/HUD
   (~3189) and BEFORE the present-finalize pass (~3247-3254)**, gated
   `(m_imguiInit && m_editorDrawData && m_editorDrawData->CmdListsCount>0)`:
   - One `ResourceUse` on `rgColor` `{COLOR_ATTACHMENT_OPTIMAL, COLOR_ATTACHMENT_OUTPUT,
     COLOR_ATTACHMENT_WRITE, isWrite=true}` so the graph derives the dependency from the
     composite write.
   - `usesDynamicRendering=true`, attachment = `colorTargetView`, **`loadOp=LOAD`** (preserve
     composited scene+HUD — do NOT clear), `storeOp=STORE`, `renderArea={{0,0},m_extent}`.
   - record lambda: set viewport/scissor + `ImGui_ImplVulkan_RenderDrawData(m_editorDrawData, c)`.
   - The pass leaves `rgColor` in `COLOR_ATTACHMENT_OPTIMAL` so the present-finalize pass
     (→`PRESENT_SRC_KHR`) is unchanged. The capture-copy path (~3198-3241) and `--screenshot`
     are unaffected (editor UI never inits headless).
5. **Resize:** in `recreateSwapchain()` (~5232), one guarded line:
   `if (m_imguiInit) ImGui_ImplVulkan_SetMinImageCount(newCount)`. With dynamic rendering ImGui
   holds no per-image framebuffers; `m_format` is stable so no font/pipeline rebuild.
6. **Shutdown:** in device `shutdown()` (~282) when `m_imguiInit`: `vkDeviceWaitIdle` →
   `ImGui_ImplVulkan_Shutdown` → `ImGui_ImplGlfw_Shutdown` → `ImGui::DestroyContext` → destroy
   `m_imguiPool`.
7. **main.cpp wiring (minimal in Phase 0):** parse `--editor` → `bool editorMode`; after
   `device->init()` succeeds (~2270), `if (editorMode && window) device->initEditorUI(window)`.
   The begin/end call sites land with the Phase 1 host. A Phase-0 smoke check can drop a single
   `ImGui::ShowDemoWindow()` between begin/end.

### Verification
Build with `--editor`; run `X3Engine --editor` and confirm the ImGui demo window draws **on
top of** the live scene + game HUD, is movable, and that the game still renders/responds. Run
**without** `--editor` and confirm byte-identical output to today (and no ImGui allocations —
`m_imguiInit` stays false). Run `--screenshot-showroom` to confirm the offscreen capture path
is untouched.

---

## Phase 1 — `--editor` live mode + viewport navigation + ImGui dockspace  [effort M]

**Goal:** boot the engine into edit mode hosting `EditorState` over the live rendered canon
world, with three camera modes and the docked-panel shell. First moment the owner can "walk
around the level."

### Files to touch
- `app/main.cpp` — route `--editor` through the **interactive canon loop**, NOT the
  `--test-editor` early-return (`--test-editor` at ~1752-1755 runs `runEditorSelfTest()` and
  returns before any window). The canon world already builds at ~7067 and runs the full
  interactive loop at **8541** (with `glfwPollEvents` at **8565**, `beginFrame` at **9532**,
  `endFrame` at **10132**) — reuse this path; it already has scene, physics, device, camera,
  rayCast, flood-cull, and the HUD primitives.
- NEW `app/editor/editor_host.{h,cpp}` (`x3::editor`) — the live-mode orchestrator owning
  `EditorState` + `EditorCamera` + panel visibility flags + refs to device/scene/physics. One
  `tick(dt, window, frame)` entry point called from the loop when `editorMode`.
- NEW `app/editor/editor_camera.{h,cpp}` — the three nav modes.
- `app/editor/editor.h`/`.cpp` — no change required for Phase 1 (consumed as-is).

### Approach
1. **Mode launch:** `editorMode` bool gates edit-input vs game-input inside the existing loop.
   **F9 conflict:** F9 is quick-LOAD (and F5 quick-save) in the live loop — bind the editor
   toggle to **F8**, and only honor F9-as-load when `!editorMode`.
2. **`EditorHost::tick`:** (a) runs the active camera and calls `device->setCamera(...)`;
   (b) renders scene + env_art as today; (c) `device->beginEditorUI()` →
   build dockspace (`ImGui::DockSpaceOverViewport`) + menubar (from `editorMenuBar()`,
   `editor.cpp:47-82`) + toolbar (tools from `Tool`/`Cmd` enums) + empty Outliner/Inspector
   docks → `device->endEditorUI()` immediately before `device->endFrame()`.
3. **`EditorCamera{ pos[3], yaw, pitch, mode, pivot[3], dist }`:**
   - **Fly** = lift the existing fly-cam pattern (RMB-held mouse-delta look clamp ±1.55 +
     WASD on cos/sin forward/right, shift=sprint, scroll=speed) from the interactive loop
     (~4447-4465).
   - **Orbit** = Alt+LMB rotates yaw/pitch about `pivot` at fixed `dist`; scroll dollies;
     MMB pans pivot; `F` focuses pivot to selection center + `dist` from its bounds.
   - **FPS-walk** = constrained fly: lock `pos.y` to floor+eyeHeight via a downward
     `physics.rayCast` each tick (the owner's literal "walk the level").
   - Keys 1/2/3 switch mode (matches `Cmd::CamOrbit/Fly/FpsWalk`). Expose a `buildViewProj`
     helper mirroring `setCamera` math so Phase 3 picking + ImGuizmo consume the same matrices.
4. **Input arbitration:** gate all viewport mouse/key handling on
   `!ImGui::GetIO().WantCaptureMouse` / `WantCaptureKeyboard` so panels don't leak input to
   gameplay/camera.

### Verification
`X3Engine --editor` boots into the canon world with a docked ImGui shell (menubar + toolbar +
empty panels). 1/2/3 switch orbit/fly/FPS-walk; F8 toggles edit/game; mouse over a panel does
not move the camera. The level renders + you can fly/walk through it.

---

## Phase 2 — BLOCKOUT MODE (UE5 greybox)  [effort L]

**Goal:** a clean grid prototyping material + parametric box/ramp/cylinder/stairs brushes with
grid snap, a serialized `brushes[]` block, Jolt static-box collision so the greybox is walkable
in PIE, and a promote-to-art swap.

### Files to touch
- `app/mesh_prims.h` — `makeBlockoutGridRGBA()` grid texture; `makeCylinder()`, `makeStairs()`
  origin-centered builders (reuse `makeBox` with `cx=cy=cz=0` for Box, `makeRamp` for Ramp);
  a `buildBrushMesh(BrushType, size[3], extras) -> PrimMesh` dispatch.
- `app/editor/editor.h` / `editor.cpp` — `BlockoutBrush` struct; `LevelDoc.brushes`;
  `toJson`/`fromJson` `brushes[]` block (mirror the `entities[]` writer/parser at
  `editor.cpp:110-124` / `178-196` using the same `num()`/`vec3()`/`esc()` helpers);
  `EditorState` brush ops; self-test **E7**.
- NEW `app/editor/blockout_runtime.{h,cpp}` (`x3::editor`) — the live spawn/edit/collision/
  promote bridge between the headless brush list and the live Scene/device/physics.
- `app/editor/editor_host.cpp` — a "Blockout" ImGui tool group (Box/Ramp/Cylinder/Stairs +
  grid-size dropdown).

### Approach
- **(a) Grid material:** `makeBlockoutGridRGBA(n=1024)` — light-grey base (#c8c8c8), faint 1 m
  minor lines, a stronger 5 m major line (cool tint), low-contrast so it reads as a scale
  reference. Authored so 1 texel-cell == 1 world meter when `makeBox` UVs use `uvScale=1.0`
  (the 1 m line lands on each integer-meter boundary). The major-line pitch must **divide n**
  (use the existing `makeSciFiPanel` `clampPitch=n/8` trick) so it tiles seamlessly on big
  floors. Created **once per editor session** via `device.createTexture(px,n,n,/*srgb*/true)`
  and cached in the blockout subsystem (one texture shared by every brush) — **not** at
  headless self-test time (no GPU there).
- **(b) Brush primitives** (all **origin-centered/LOCAL space**, unlike `makeBox`'s world-baked
  center — see risk): `Box` (=`makeBox` with `c=0`), `Ramp` (=`makeRamp`), `makeCylinder(r,
  halfH, sides=16)` (capped prism, render + collision), `makeStairs(halfW, run, rise, steps=8)`
  (N stacked boxes merged into one `PrimMesh`). A `BrushType{Box,Ramp,Cylinder,Stairs}` enum
  selects the generator via `buildBrushMesh`.
- **(c) Data model + JSON:** `struct BlockoutBrush { name; uint32_t type; float pos[3]; float
  size[3]; float yaw; float tint[3]; bool collide; std::string promotedAsset; uint32_t
  sceneEntity, body /*not serialized*/ }`. Add `std::vector<BlockoutBrush> brushes` to
  `LevelDoc`; emit/parse a top-level `"brushes":[...]` array. Self-test **E7** asserts a
  `brushes[]` round-trip preserves type/pos/size/yaw/collide/asset (E0/E5 pattern).
- **(d) Editor tools** (headless-testable): `addBrush(type, pos, size)` (snaps pos + size to
  grid, selects); `resizeSelectedBrush(Axis, delta)` (grows a face with grid snap). Selection
  must span both `entities[]` and `brushes[]` — see **tagged selection** in Phase 3
  (`m_selKind{None,Entity,Brush}` + index). Snap reuses the editor's `m_snap`/`m_grid`
  (presets 1 m / 0.5 m / 0.25 m).
- **(e) Live spawn + Jolt collision** (`blockout_runtime`): per brush — `buildBrushMesh` →
  `device.createMesh` → `Entity{ mesh, tex=gridTex, baseColor=tint, transform from pos+yaw
  (column-major), tag=Tag::Static }`; collision via `physics.addBox({size*0.5}, pos, 0.0f,
  Layer::Static)` for Box/Cylinder bbox, OR `physics.addStaticMesh(geo.cverts, geo.cindex)`
  for **Ramp/Stairs** (an AABB box would make a ramp un-walkable — you'd only reach its bbox
  top). Store `body`+`sceneEntity` back on the brush. On move: `physics.setBodyPosition`. On
  resize: `removeBody` + `destroyMesh`+`createMesh` (safer than `updateMesh`, whose
  changing-vertex-count realloc semantics are unverified) + re-add collision. The greybox is
  **immediately walkable in PIE** with zero rebuild.
- **(f) Promote to art:** `promoteBrush(idx, glbPath)` sets `promotedAsset`, tears down the
  greybox mesh (`destroyMesh`), keeps/refits the Jolt collision, and spawns the GLB via the
  existing `EnvArtSystem::buildFromGlb` path. JSON persists `asset != ""` so reload spawns the
  real mesh; clearing `asset` reverts to greybox. Blockout→art is a per-brush **data flip**.

### Verification
`--test-editor` self-test passes including **E7** (`brushes[]` round-trip). In `--editor`:
pick Box/Ramp/Cylinder/Stairs from the Blockout palette, click-place on the grid, move/resize
with snap, see the grid material. Toggle PIE (Phase 4) or FPS-walk and **walk up a ramp/
staircase and stand on a box** (collision proof). Save → reload → brushes reappear identical.
Promote one brush to a GLB and confirm the real mesh replaces the greybox.

---

## Phase 3 — Selection + TRS gizmos + undo/redo + Outliner + Inspector  [effort L]

**Goal:** the core manipulation loop. After this it *feels* like a real editor. Build the
**command stack first** so every op routes through it (retrofitting undo later is painful).

### Files to touch
- `app/editor/editor.h`/`.cpp` — replace `int m_selected` with `std::vector<int> m_selection`
  + anchor + **tagged kind** (`m_selKind{None,Entity,Brush}`); add `isSelected/selectAdd/
  selectToggle/clearSelection`; three grids (`m_gridMove` 0.25/0.5/1/4 m, `m_gridRotate`
  5/15/45°, `m_gridScale` 0.1/0.25); rotate/scale manipulation (`applyTransformDelta`).
- NEW `app/editor/editor_commands.{h,cpp}` — `ICommand{do/undo/label}` + `CommandStack`
  (push runs doIt + clears redo; cap ~256) + concrete commands (Transform/AddEntity/
  DeleteEntity/AddBrush/Property), each holding `EditorHost&` so it mutates the doc AND syncs
  the live Scene transform + Jolt body.
- NEW `app/editor/editor_reflect.{h,cpp}` — `PropDesc{name,type,offset,min,max,widget}` +
  `propsFor(type)` lightweight descriptor table (NOT RTTI).
- NEW `app/editor/editor_panels.cpp` — ImGui Outliner + Inspector + menubar/toolbar dispatch.
- `app/editor/editor_host.cpp` — picking, gizmo manipulate, panel hosting, command routing.

### Approach
1. **Undo/redo (first):** command pattern; **every** P0/P2/P4 op constructs a command instead
   of mutating directly. Ctrl+Z/Ctrl+Y dispatch `Cmd::Undo`/`Cmd::Redo`.
2. **Picking:** on LMB (gated on `!io.WantCaptureMouse`), prefer `physics.rayCast(eye, dir,
   dist, Layer::Static)` → `scene.entityForBody(hit.body)` (`scene.h:143`) → map the Scene id
   back to the doc index via `EditorEntity::sceneEntity` / `BlockoutBrush::sceneEntity`; fall
   back to `EditorState::pickRay` for body-less doc entities. Shift=add, Ctrl=toggle, empty
   click=clear. Marquee = screen-rect via `device.worldToScreen` per entity center.
3. **Gizmos (ImGuizmo, rides on Phase 0 ImGui):** compute the gizmo matrix from the selection
   pivot (single = its transform; multi = average pos); `ImGuizmo::Manipulate(view, proj, op,
   mode, matrix)` with `op` from current `Tool` (Q/W/E/R), `mode` world/local from Tab, snap
   from the active grid. Decompose the delta into a `TransformCommand` (begin on
   `ImGuizmo::IsUsing` rising edge, commit on falling edge — one drag = one undo unit). Each
   edit pushes to `scene.get(id).transform` AND `physics.setBodyPosition/setBodyRotation`.
   (Rotate initially accumulates `yaw`; a full quaternion field on `EditorEntity` is a
   follow-on.)
4. **Outliner:** ImGui child listing `doc.entities` + `doc.brushes` (color swatch + name +
   type); click selects (shares the **one** `m_selection` source of truth); search filter;
   per-row eye→`Entity.visible`, lock→host-side locked bitset; optional editor-only `folder`
   string per entity (serialized, ignored by gameplay) grouped under `TreeNode`.
5. **Inspector:** iterate `propsFor(type)` over `EditorEntity`/`BlockoutBrush` fields, render
   `DragFloat`/`ColorEdit3`/`InputText` by type, write through `(char*)&entity + offset`, and
   on `IsItemDeactivatedAfterEdit` route a `PropertyCommand`. Multi-edit shows a dash for mixed
   values. **This is the same metadata layer P1.8 FORGE3D sliders will reuse.**

### Verification
Click-pick selects (precise via Jolt rayCast); Shift/Ctrl multi-select; marquee selects a box
of entities. W/E/R gizmos translate/rotate/scale with per-grid snap; the live mesh AND its
physics body track. Ctrl+Z/Ctrl+Y undo/redo every op (move, add, delete, property). Outliner
and Inspector share selection with the viewport and reflect edits live.

---

## Phase 4 — Content/model browser + Play-In-Editor toggle  [effort L]

**Goal:** "drag a model in, it floor-snaps" + the PIE toggle that ends the documented
stale-exe / kill-before-build / rebuild-relaunch pain.

### Files to touch
- NEW `app/editor/content_browser.{h,cpp}` — asset index + offscreen thumbnails + drag-source.
- `app/editor/editor_host.cpp` — PIE `HostMode{Edit,Play}` state machine + `PieSnapshot`.

### Approach
1. **Asset index:** recursively index the converted-GLB dir (the same root
   `EnvArtSystem::buildFromGlb` mounts, e.g. `G:/GameModels/converted_glb`) into
   `AssetEntry{relPath, name, thumb, thumbReady}`.
2. **Thumbnails:** a lazy offscreen queue — for the first N visible-but-unthumbed entries per
   frame, load the GLB via an `EnvArtSystem`-style loader, frame the camera on `worldBounds`,
   `armCapture(temp)` → render one offscreen frame → `captureFrame` → decode the PNG and
   `createTexture` into `thumb`. **Cache to disk** under `thumbs/` keyed by path+mtime so
   re-open is instant. ⚠️ Reuses `armCapture`/`captureFrame` (today one-shot `--screenshot`
   paths) — use a small dedicated offscreen target + a reentrancy guard so it never stalls the
   main swapchain frame; verify against the **post-IBL** `VulkanRenderDevice`.
3. **Drag-to-place:** `ImGui::BeginDragDropSource` carries the asset index; on drop over the
   viewport, `rayCast` from the cursor to the floor hit, `addEntity` of type `prop` with an
   `assetPath`, spawn its Scene drawable via the shared `EnvArtSystem`, floor-snap the
   transform, wrap in `AddEntityCommand` (Phase 3).
4. **Play-In-Editor:** `HostMode{Edit,Play}`. On `P`: snapshot mutable state (player spawn from
   `doc.playerStart`, dynamic transforms + body positions, selection, camera) into a
   `PieSnapshot`, freeze editor input (gizmos/picking off, panels hidden, a small PLAYING
   overlay), and hand the loop to the existing player+physics+game update path starting at
   `doc.playerStart`. On eject (Esc / P): restore the snapshot, return to Edit. Because the
   editor IS the running engine with the level already loaded, PIE is a mode flag + save/restore
   — no rebuild/relaunch.

### Verification
Browser shows a thumbnailed, searchable grid of GLBs; thumbnails render once and cache.
Drag a GLB into the viewport → it floor-snaps + is undoable. Press P → play the level from the
player spawn with full physics/gameplay; Esc → snap back to the editor with the scene
unchanged (no relaunch).

---

## Phase 5 — JSON save/load round-trip + loader entity instantiation  [effort L]

**Goal:** close the loop — the editor reads/writes the canonical
`EscapeLab48_AllFloors_v2.project.json`, and the loader instantiates `entities[]`/`triggers[]`/
`brushes[]` so authored content survives a reload. **#1 structural prerequisite for all content
authoring.**

### Files to touch
- `app/level_loader.h`/`.cpp` — new `CanonEntity`/`CanonTrigger`/`CanonBrush` structs on
  `CanonFloor`; an `entities[]`/`triggers[]`/`brushes[]` parse loop in `loadCanonFloor()`
  (after the doors loop, ~653, using the loader's own `JValue`/`JParser`); a
  `spawnCanonEntities()` called from `buildCanonFloor()` (~830); and a **write path**
  (the loader has none today — only the editor's hand-rolled writer exists).
- `app/editor/editor_host.cpp` — Save/Load route through the loader's richer canonical
  read/write rather than the flat `LevelDoc` writer.
- `app/level_loader.cpp` — extend `runCanonLevelSelfTest()` (`--test-canonlevel`) with a
  round-trip identity assertion.

### Approach
1. **Schema decision (locked here):** the editor's flat `LevelDoc` cannot represent the
   canonical rooms/doors schema, and the loader cannot read `entities[]`/`triggers[]` today.
   **EXTEND the loader's in-memory `CanonFloor` model** (add `CanonEntity{type, asset/glbPath,
   pos, yaw, scale, tint, params, roomId}`, `CanonTrigger{bounds, condition, actions}`,
   `CanonBrush` mirroring `BlockoutBrush`) rather than forking a parallel format — the mature
   loader's PVS / doorway-resolver / lights all key off `CanonFloor`. The editor reads/writes
   canonical floors via the loader's parser.
2. **Loader instantiation contract (the real design task, not just plumbing):** a `type` enum →
   factory. Each `CanonEntity` becomes a Scene `Entity`: graybox prim (the `addBox` path the
   shells use) OR a GLB drawable reusing `EnvArtSystem::buildFromGlb` + `worldBounds` floor-snap,
   plus an optional Jolt body, tagged with `roomId` like the shells. `CanonBrush` reuses the
   Phase 2 `blockout_runtime` spawn. `CanonTrigger` is parsed + stored now (interpreted at
   runtime is roadmap P2.6).
3. **Write path:** serialize `CanonFloor` (rooms/doors untouched + the now-populated
   `entities[]`/`triggers[]`/`brushes[]`) back to the canonical JSON shape, preserving round-trip
   identity.

### Verification
Extend `--test-canonlevel`: load Floor 1, add entities/brushes, save, reload, assert
**round-trip identity** (rooms/doors/entities/brushes all preserved) and that the loader
**spawns** the authored content (drawn-count + body-count proof, headless, no GPU). In
`--editor`: place brushes/props, Save, restart `X3Engine --editor` (or `--world canonlevel`),
and confirm everything reappears in the live world.

---

## Top risks (carry into every phase)

1. **Phase 0 is hard-gated on the in-flight IBL change clearing `VulkanRenderDevice.cpp`.**
   Both edit `buildAndExecuteGraph()` + the post/composite region; land Phase 0 only after IBL
   and rebase onto it. The editor-UI pass MUST be inserted **after** composite/HUD and
   **before** the present-finalize (`→PRESENT_SRC_KHR`) pass, or it's a validation error / black
   UI.
2. **vcpkg imgui version + font upload.** The pinned baseline `f7f9411` must resolve `imgui`
   with `docking-experimental` + `glfw-binding` + `vulkan-binding`. 1.90+ auto-uploads fonts and
   `PipelineRenderingCreateInfo` is a struct field; an older port needs
   `ImGui_ImplVulkan_CreateFontsTexture()` + a one-time command. Verify the resolved version and
   branch the font code.
3. **ImGui descriptor pool + input arbitration.** ImGui needs its **own** `FREE_DESCRIPTOR_SET`
   pool (1000 samplers); reusing the bindless/HUD pools corrupts them. `ImGui_ImplGlfw_Init...
   (win, true)` chains the game's GLFW callbacks — confirm gameplay key/char/scroll still fire,
   and gate all viewport input on `io.WantCaptureMouse/Keyboard`. Keep `ViewportsEnable`
   **off** in Phase 0 (multi-viewport secondary swapchains the device doesn't manage → defer to
   the dedicated 2nd-monitor piece).
4. **Two divergent JSON formats + two parsers** (editor flat `LevelDoc` vs canonical
   floor/room/door `CanonFloor`; `editor.cpp:87-217` vs `level_loader.cpp:37-185`). Unifying is
   invasive — **extend `CanonFloor`, don't rewrite**. `brushes[]` lives as a new top-level array,
   and the **canonical** loader must read it (and `entities[]`/`triggers[]`) or content vanishes
   on reload through the canonical path.
5. **Blockout collision shape choice.** `addBox` is an AABB — fine for Box/Cylinder bbox but
   makes a Ramp/Stairs un-walkable (you'd only reach the bbox top). Use `addStaticMesh(cverts,
   cindex)` for Ramp/Stairs. Brushes MUST be **origin-centered** (carry pos/yaw in
   `Entity::transform`), unlike `makeBox`'s world-baked center (`mesh_prims.h:85`), or
   interactive move/resize forces a full mesh regen every frame.
6. **`updateMesh` realloc semantics on resize are unverified** (`IRenderDevice.h:157` requires
   matching vertex count). Brush resize changes vert count (stairs steps / cylinder sides) — use
   `destroyMesh`+`createMesh`, the safe path.
7. **Offscreen thumbnail reentrancy.** Phase 4 reuses `armCapture`/`captureFrame` (one-shot
   `--screenshot` paths). Rendering many small frames per session needs a dedicated small
   offscreen target + a reentrancy check so it never stalls the main swapchain frame; verify
   against the post-IBL device.
8. **Undo/redo retrofit cost.** Build the Phase 3 command stack early; every op written without
   it must be rewritten to route through commands.
9. **Loader entity instantiation is genuinely unbuilt** (loader places room shells only). The
   `type` → Scene-Entity/Jolt-body/GLB-drawable factory is a design task, not plumbing — without
   it, authored props never appear on reload.
