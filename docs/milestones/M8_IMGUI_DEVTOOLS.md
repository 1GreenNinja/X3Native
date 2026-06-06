# Milestone M8 (dev tools) — Dear ImGui dev overlay

**Branch:** `feat/m8-imgui-devtools` · **Carved from** `X3_NATIVE_SLICES.md` on 2026-06-05
**Slices:** 83, 84

## Context

The shipping/gameplay UI is the **custom GUI** (`app/ui.cpp`) and is already done
(85–88 ✅) — this milestone does **not** touch it. This adds **Dear ImGui for DEV
TOOLS ONLY**, stripped from Release/ship builds. There is an existing editor
fly-cam stub (`app/editor*`) annotated "ImGui panels later" — this fills that in.

## Slices

### 83 📝 Dear ImGui dev overlay
ImGui integrated for dev tools: entity inspector, cvar editor, perf HUD.
**Gate:** F1 toggles a working dev panel.

### 84 📝 Entity inspector
Select an entity, view/edit its components live in ImGui. **Gate:** tweak a
value, see it change in-world.

## Integration notes

- Add Dear ImGui via vcpkg (docking branch preferred for dockable panels).
- Wire `imgui_impl_vulkan` into the `VulkanRenderDevice` dynamic-rendering pass;
  feed input through the existing input layer.
- **Gate ImGui behind a dev/build flag** (e.g. `X3_DEVTOOLS`) so it is compiled
  out of Release/ship — keep the shipping binary clean.
- Reuse the existing entity/component registry for the inspector reflection.

## Milestone gate

F1 opens a working dev panel; selecting an entity edits a live value that takes
effect in-world. ImGui absent from a Release build.
