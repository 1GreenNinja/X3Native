# Native Level Editor — vision (Tim, 2026-05-26)

> **Status:** design vision for the deferred native-editor lane. The **data-driven loader**
> (`app/level_loader.{h,cpp}`, lands a level from JSON) is now built — that's this editor's
> load/save spine. What remains is the **in-engine editing front-end** described here.

## The core idea (from Tim's Doom/Quake mapping days)
The old Doom/Quake editors let you **walk around the level in first person**, **select a
texture on a wall**, and **change it** — live, in the space. Build that, but supercharged
with the AI + asset power we now have.

## Features
1. **First-person walkaround edit mode** — toggle into the running level (`--editor` flag or
   an in-game hotkey); fly/walk the actual level, no separate abstract viewport. (FP-walk +
   noclip already exist in the engine.)
2. **Select-a-surface → apply texture** — click a wall/floor/ceiling (or an individual wall
   **section**) and change its texture. Granularity: whole floor, a wall, or one section.
3. **AI-powered suggestion box** — when a surface (or area) is selected, show a panel of
   **textures + assets whose style fits that area** (e.g. Medical Bay → clinical white/teal;
   Armory → industrial; caves → organic). The AI (Claude) proposes area-appropriate options
   so the designer picks from curated, on-theme choices instead of hunting a flat list.
4. **Model object browser (second screen)** — a browsable model/prop catalog, ideally on a
   **second monitor/window**. Drag/drop a model into the level. Placed objects **sit on the
   floor by default** and are **easily movable** (grab + drag, snap to floor).
5. **Easy edit verbs** — change walls/floors/individual sections; place/move/rotate/scale
   objects; the Unreal Q/W/E/R gizmo set (Tim's LevelArchitect had Q/W/move/scale; this adds
   the rotate it lacked).

## Save / round-trip
Edits write back to the **canonical level JSON** (the same `EscapeLab48_AllFloors` format the
loader reads) — edit in-engine → save → reload identical. That closes the data-driven loop:
the editor and the game share one level format. Per-room data (the cull's roomId, lights,
spawns) is authored here too.

## Why it's very achievable now
We already have: the fast renderer (sub-ms culled frames → editing is buttery), the
data-driven loader (load/save), FP-walk + noclip, per-room geometry, the asset/GLB pipeline,
and an AI in the loop for the suggestion box. The editor is the **front-end on top of the
foundation we just shipped**, not a from-scratch tool.

See also: `docs/design/WORLD_AND_EDITOR_PLAN.md` (earlier editor roadmap), the LevelArchitect
v10.9 / Task9D editors (Tim's Babylon UX reference), `tools/v2_floor_topdown.py`.
