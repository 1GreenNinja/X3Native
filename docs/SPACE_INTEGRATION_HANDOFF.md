# Space Engine — Integration Handoff (Snake → FarmBoss)

**2026-05-30.** The full 13-subsystem space engine (spec: `docs/superpowers/specs/2026-05-28-space-engine-design.md`) is built — 13 green `feat/` branches + 2 supporting. All gated: `--test-<name>` Release+Debug + `--world <name>` screenshot pixel-variance (std>15, uniq>100). **NO `--smoketest` in any lane gate** (Bug 2). Handoff to FarmBoss for the consolidation into `cull-combined`.

## Branches to integrate (suggested order = dependency order)

**Wave 1 — foundation (integrate FIRST; everything depends on S0):**
| Branch | Sub | Commit | Gate |
|---|---|---|---|
| `feat/space-layer` | S0 spine | `b5541e8` | --test-spacelayer 23/23 R+D |
| `feat/space-lod` | S2 LOD | `05228fe` | --test-lod 18/18 R+D |
| `feat/space-env` | S1 env | `52f4b2c` | --test-spaceenv 12/12 R+D |
| `feat/ship-art` | S11 ship art | `ef58f5f` | --test-shipanim 19/19 R+D |

**Wave 2 — experience + combat + EVA:**
| Branch | Sub | Commit | Gate |
|---|---|---|---|
| `feat/wormhole-transit` | S3 | `1c4c89c` | --test-wormhole-transit 17/17 R+D |
| `feat/atmo-descent` | S4 | `1b7becb` | --test-atmo-descent 14/14 R+D |
| `feat/ship-interior` | S5 | `b03fbf0` | --test-ship-interior 6/6 R+D |
| `feat/ship-ai` | S8 | `cb5a09d` | --test-ship-ai 9/9 R+D |
| `feat/ship-targeting` | S9 | `32bc80e` | --test-targeting 19/19 R+D |
| `feat/ship-damage` | S10 | `f6a7a4e` | --test-ship-damage 20/20 R+D |
| `feat/eva-spacewalk` | S12 | `f22f11c` | --test-eva 8/8 R+D |

**Wave 3 — polish:**
| Branch | Sub | Commit | Gate |
|---|---|---|---|
| `feat/ship-windows` | S6 | `ab705e1` | --test-ship-windows 5/5 R+D |
| `feat/ship-repair` | S7 | `1a7f439` | --test-ship-repair 8/8 R+D |

**Supporting:**
- `feat/wormhole-vfx` @ `7838f14` — the WormholeVfx class (--test-wormhole 17/17). S3 builds on it.
- `feat/coop-companion-merge` @ `fac23f0` — the coop-npcs faction merge (separate, already verified; see its own NOTE_TO_FARMBOSS).

## ⚠️ Known integration snags (these WILL bite — forewarned)

1. **`app/main.cpp` boilerplate tangle.** Every `--test-X` block shares identical `int pass=0, total=0; auto check=...` boilerplate, so git ALIASES the blocks and splices two different test bodies into one when you union-merge. **Un-splice: each `--test-X` must end up a complete standalone `if (testX) { ... }` block.** This is the Task #20 smell (main.cpp monolith). Integrate sequentially and check each merge's main.cpp compiles.
2. **`wormhole_vfx.{h,cpp}` + `shaders/wormhole.*` are duplicated** on BOTH `feat/wormhole-vfx` AND `feat/wormhole-transit` (byte-identical — S3 copied them since the VFX wasn't merged when it built). Add/add conflict → take either side (identical).
3. **LFS pointers.** `SpaceShip*.glb` on `feat/ship-art` are LFS objects. Lanes branched from `feat/space-layer` (S8 ship-ai, S12 eva) don't have them fetched, so they used placeholder hulls. After ship-art integrates, the GLBs resolve — re-run those `--world` showcases to confirm real ships draw.
4. **CMakeLists source-list unions** — each lane adds its `app/space/*.cpp` to the list; trivial union conflicts (keep all).
5. **`/STACK:16MB`** must stay on the X3Engine target (mech-pilot set it; several `--world` blocks need it).

## Post-integration wiring (Task #39 — not done yet)

The converted ships/interior are staged in `G:\GameModels\converted_glb\Space\` (NOT in repo): Hurricane/Rikka/Sparrow_orange/G6_green ships + the 60-piece ScifiModularInterior kit. To use in-engine: copy into `assets/rigged_glb/` (LFS; SPARROW 97MB needs ktx2 bake first) + wire into S8's enemy-ship candidate list + S5's ShipInterior. Both lanes have "real models later" hooks.

## What Snake is doing next (parallel)

Building the **intro-cinematic VFX** as standalone lanes (like wormhole-vfx): `feat/decloak-vfx` (capital-ship decloak shimmer) + `feat/tractor-beam` (tractor beam). These feed the **intro capstone** (Task #42 — Jake's fighter → Jupiter → decloaking G6 → tractor beam → "6 Months Later" → cell), which builds AFTER this integration pass lands.

-- SNAKE / rightscreen / 13700K + 1080Ti
