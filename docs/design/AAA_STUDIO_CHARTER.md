# AAA STUDIO CHARTER — X3Native "Escape From Lab Zero" Quality Offensive

**Executive:** Claude Fable 5 (this session), acting studio director. Commissioned by Tim, 2026-07-05:
*"Take them all on... 3 Fable agents, and as many opus/sonnet/haiku as you need to produce the
textures, lay out the rooms so they look good, and finish all the animations, ragdoll, sounds,
features."*

**Quality bar:** Riftforged-UE5 / GTA. A frame is DONE when the director has looked at the
rendered screenshot with his own eyes and would show it to Tim without apologizing for it.

---

## Non-negotiable studio law (every agent, every task)

1. **The Fable gate.** `cmake --build build --config Release` (ALL_BUILD) + **verify
   X3Engine.exe mtime advanced** (the X3Engine/x3engine case-collision silently no-ops the app
   link — a green build with a stale exe is a LIE). Then `--smoketest` 0 VUID /
   allocationCount=0. Report only from a freshly-relinked exe.
2. **The eye gate.** Every visual change is screenshot-verified ON THE 5090
   (`--world canonlevel --screenshot <path> [--shot-cam "x,y,z,yaw,pitch"]` — leading negative
   x needs a leading SPACE; dir=(cos yaw, sin yaw); cell x −1.5..5.5, z 37..43, floor −2).
   The agent READS its own screenshots, scores /10 honestly, iterates ≤3 rounds. Overselling a
   blockout as AAA is a firing offense. The director re-reads every screenshot personally
   before integration.
3. **Surgical scope.** Own files are listed per workstream. Touching another workstream's
   files = your branch does not merge. No drive-by refactors.
4. **Level-authoring doctrine** (docs + skill): openings share wall planes, flush-or-shared
   seams (0.14 m inset vs graybox — 0.2 m slabs centered on plane), height changes only via
   legal transitions, no floating geometry, contact shadows ground props.
5. **Dressing-system facts** (learned R1–R5, do not relearn by crashing into them):
   `CellDressing::place()` is YAW-ONLY (no pitch/roll). Instance emissive is the fallback for
   EVERY drawable (whole-body glow toys). Instance `emissive[3]` SCALES material-authored
   emissive (0 = suppressed). SM_Wall_B/C carry WINDOW CUTOUTS at each panel center; panelW =
   3 m × (roomH/4.403). Monsters draw through basic `drawMesh` (no PBR) — that's WHY they look
   flat. The ceiling "wings" = the trapdoor hatch (horizontal SM_Door_A) + wall ladder.
6. **Assets are local:** D:\Assets (210 packs) + D:\GameModels (76 GB) + D:\Assets\X3AssetStore
   (the store; `tools/asset_store.py publish/fetch`). Textures on demand via the SD 3.5
   diffusers pipeline (C:\GameDev\SD_Models\sd35) — ONE generation at a time on the 5090.

## Roster — Wave 1 (the four blockers between the cell and the bar)

| Agent | Model | Workstream | Owned files |
|---|---|---|---|
| F1 | Fable (fork) | **Trapdoor hatch pass** — the ceiling escape hatch reads as engineered gameplay (recessed frame, hazard read, status light, seated ladder), elevator-overhaul style | trapdoor/door code it identifies (NOT cell_dressing.cpp, NOT monster.cpp) |
| F2 | Fable (fork) | **Enemy visual pass** — monsters onto the PBR draw path (normal/MR/emissive), drone de-uglified (gunmetal body, controlled eye glow), hit-flash preserved | app/monster.cpp/.h |
| F3 | Fable (fork) | **Cell openings + cot** — armored-glass/bars in the window cutouts, real cot replacing the crate bed (from S1's find) | app/cell_dressing.cpp/.h |
| S1 | Sonnet | **Asset scout** — find cot/bed + detention-bars candidates in D:\Assets / D:\GameModels, convert to GLB, probe bounds, stage (no store publish) | staging dir only |
| A1 | Sonnet | **Audio/anim/ragdoll/feature audit** — gap list vs the playtest backlog, ranked punch list for Wave 2 | read-only |

Wave 2 (after integration): execute A1's punch list — animations (Blender headless pipeline),
ragdoll deaths, sound coverage, feature stubs — same law, new roster.

## Integration protocol (director only)

Merge order F2 → F1 → F3 (disjoint files; order = blast radius). After each merge: full gate +
the 4-angle review set (hero / door / ceiling / bunk), read by the director's eyes. One fix
round per workstream if needed. Final: before/after set to Tim, honest score, no grade
inflation.
