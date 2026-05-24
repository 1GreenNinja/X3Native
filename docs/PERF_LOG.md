# X3Native — Performance Log
*Track FPS with every change. Document what each change does to performance — up or down. Authored 2026-05-24.*

## How we measure
- **Live per-system breakdown** (added 2026-05-24): the windowed game logs a `[perf]` line every 120 frames:
  ```
  [perf] <FPS> FPS  frame=<ms> | game.tick=<ms>  healthbars=<ms>  (rest=render+physics+hud)
  ```
  `game.tick` = all monster AI + locomotion + beats; `healthbars` = the per-living-enemy world->screen + LOS raycast loop (`main.cpp`); rest = render submission + physics step + HUD.
- **Headless stat line** (`--smoketest`): `stats draws=N tris=N objs=N gpu=Xms` — geometry/draw load + GPU time, deterministic per build. Good for per-commit tracking (no window needed).
- **Frame cap**: `r_maxfps` (default 240). Uncapped (`r_maxfps 0`) to see true headroom.
- **Live knobs** (backtick console): `r_exposure` (brightness), `hud_fps`, `r_stats`.

## Rule of thumb for entries
Every perf-affecting change records: the change, the FPS before→after (scene + conditions), and WHY. Render-cost-neutral changes (shader constants, gated/optional features) say so explicitly.

## The open investigation (2026-05-24)
- **Symptom:** ~10 FPS (100 ms/frame) in the B1 detention cell.
- **Smoking gun (Tim):** ~85 FPS with the last monster alive → **1300 FPS the instant it dies** (NPC girl still walking). So the per-living-monster per-frame cost dominates (~11 ms/monster).
- **Ruled OUT — CPU skinning:** smoketest confirms **36/36 monsters are GPU-SKINNED** (compute pre-pass), not the CPU `updateMesh` fallback. So it is NOT the documented CPU-skin bottleneck.
- **Remaining suspects (now being measured by the `[perf]` timers):**
  1. `game.tick` — monster AI (navmesh A*, guard LOS rays, decision logic) scaling with living-monster count.
  2. `healthbars` — one physics `rayCast` per *living* enemy per frame (stops exactly when the last enemy dies → matches the 85→1300 cliff). **Prime suspect.**
  3. GPU skin-compute barriers serializing per skinned mesh.
  4. Physics bodies in the sim-accumulator substeps.
- **Next:** read the `[perf]` line in a live run with monsters present; whichever of `game.tick` / `healthbars` is large is the culprit. If `healthbars` → cache the LOS result / throttle to N Hz / skip when off-screen. If `game.tick` → profile the monster AI inner loop.

## Change log (effect on performance)
| Date | Change | Effect | Notes |
|---|---|---|---|
| 2026-05-24 | **Per-system `[perf]` timers** (`main.cpp`) | **~neutral** | A few `glfwGetTime()`/frame; logs every 120 frames. The measurement tool itself. |
| 2026-05-24 | **Ambient floor 0.10→0.26** (`VulkanRenderDevice.cpp`, fixes "incredibly dark") | **neutral** | A shader constant (`ambientCount.rgb`); no extra draws/work. Brightness only. |
| 2026-05-24 | **RT Phase 0** — enable VK_KHR_ray_query/accel-structure (gated) | **neutral** | Only enables device features at init; zero per-frame work (no rays cast, no AS yet). Verified: device still inits + renders. |
| 2026-05-24 | **ECS → GPU render feed** (`ecs_render.cpp`) | **win (at scale)** | Feeds the existing multidraw-indirect path; 10k entities sharing meshes collapse to a few GPU draws. Not yet wired into the live game. |
| 2026-05-24 | **Monster topple-corpse death** (corpses linger, drawn) | **watch** | Corpses keep DRAWING (extra draws), but do NOT re-skin (update() returns for dead). Net minor draw cost; revisit if corpses pile up. |
| (earlier) | `r_maxfps` 240 cap | **caps headroom** | Stops vsync-off maxing the GPU (1300→240). No-op with vsync on. |
| (earlier) | dt clamp 0.1→0.034 (sim accumulator) | **win** | Broke the 6-substep death-spiral after the load hitch (11→29 FPS). Real fix = GPU skinning (done) + physics-body reduction (open). |

> Append a row for EVERY perf-affecting change. If you don't know the effect yet, write "measure" and fill it from the next `[perf]` run.
