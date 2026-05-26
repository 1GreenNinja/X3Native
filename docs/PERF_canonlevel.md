# Performance: data-driven level loader + per-room occlusion cull (2026-05-26)

**Branch:** `feat/doors-death-anim` (commits `5c55cc4` loader+cull, `11ab5a7` doors+halls,
plus the lighting/feature pass). Machine: 14900K + RTX 5090.

## The win (measured, RTX 5090)
| | Full tower (default level, no cull) | `--world canonlevel` (per-room PVS cull) |
|---|---|---|
| Objects drawn | **8,700 / 8,700** (everything) | **~155–160** (current room + neighbors) |
| Triangles/frame | **49,653,991** | **~8,000** |
| GPU time/frame | **~9.9 ms** | **~0.1 ms** |
| GPU utilization | **100%** (pegged) | **~25%** at uncapped, ~23% capped |
| FPS (uncapped) | vsync-bound | **~900** (1.1 ms frame); benchmark scene hits 2,290 |

From a GPU pegged at 100% to single-digit-millisecond frames — the whole 7-floor tower
was being submitted every frame with no occlusion culling. The cull + loader fix that.

## How it works
1. **Data-driven loader** (`app/level_loader.{h,cpp}`): parses the canonical LevelArchitect
   export (`EscapeLab48_AllFloors_v2.project.json`) — rooms `{n,t,x,y,z,w,h,d}` + door
   index-pairs — and builds the level geometry. A **doorway resolver** handles the
   Babylon→native gap: adjacent rooms get a doorway cut in the shared wall; ~1 m gaps cut
   through the wall; ~2 m gaps get a bridge corridor with doorway mouths punched in both
   facing walls; isolated rooms (caves) get vertical descent tubes. Real `SM_Door_A` +
   `SM_DoorFrame_A` doors, scaled to each opening, slide their full height to clear it.
2. **Per-room PVS occlusion cull** (`app/scene.{h,cpp}`): every built entity is tagged with
   a `roomId`; each frame the host computes the visible-room set from the player's current
   room (+ doored neighbors) and `Scene::render` skips entities whose room isn't visible.
   That's the ~8,700→~160 object drop. `r_roomcull 0/1` cvar toggles it (e.g. noclip
   overview). **Next:** portal flood-fill PVS (BFS through open doorways, frustum-aware) +
   a tunable depth cvar so long halls don't over-cull (task #36).

## Propagate to all forks (task #34)
The cull (`Scene` roomId + visible-room gate) and the loader are engine-side and
level-agnostic. Merge `feat/doors-death-anim` → `main` so every X3Native fork/branch
inherits the tech. The cull benefits the hand-coded levels too once their entities are
room-tagged. Verified: `--test-canonlevel` 13/13, `--test-level1` 21/21, all `--test-*`
green, `--smoketest` Release+Debug 0 VUID + allocationCount=0.
