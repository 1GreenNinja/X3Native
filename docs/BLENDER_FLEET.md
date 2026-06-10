# BLENDER_FLEET — growing 3D capability across the fleet

The no-slop principle (`feedback_visual_quality_bar` / Tim 2026-05-26) says: **Blender for everything that ships, AI for the reference that feeds Blender.** This doc is how the fleet operationalizes that — which machine does what, what flows through what, and how a new asset goes from concept to engine GLB without ever picking up an AI tell in the shipping path.

---

## The no-slop pipeline (one line)

`AI ref (Flux / SD3.5) → Blender model+materials → Cycles bake → GLB → KTX2 → assets/ in repo → engine renders it`

Every transition is auditable; the AI only sees the *reference* stage. The geometry is real, the materials are PBR, the engine renders honest pixels. **No AI artifact can survive the Blender step**, because Blender doesn't generate — it constructs.

---

## Fleet roles for 3D work

| Machine | GPU | Blender role | Why |
|---|---|---|---|
| **14900K** | **RTX 5090** (32GB) | **Primary modeler + Cycles render farm + Blender MCP host** | The 5090 is the only card that makes Cycles + OptiX denoise feel instant. This is the rig where hero assets are *made*. |
| **13700K** | 2× GTX 1080 Ti | **Integrator primary; secondary render node when idle** | The CPU is wasted on idle integrator wait-states. Cycles CPU+GPU hybrid mode turns those minutes into bake passes. |
| **Snake13700k** | GTX 1080 Ti | **Open-world / vehicle / ship lane modeler** | Already shipping Rodin Minerva. Lane is mountains, ocean, city, ships, submarine. |
| **DJBOOTH** | GTX 1080 Ti | **Mid-biome lane modeler (L12–L15 caves, swamplands, Memory Hunter)** | A 1080 Ti renders the cave-scale density (`act2-caves` scope) comfortably. |
| **i5000** | 1080 Ti class | **Desert lane modeler (L10–L11 Crystalline Desert, Salvari Camp)** + concept-pitch driver | Already pitched the Overlord Synth + Boston Dynamics ATLAS ref — leans into the concept-curator role naturally. |
| **Predator-I4400** | dual 1080 Ti SLI | **Render farm node** (Cycles tile-distributed) | Two 1080 Tis in SLI = a respectable batch renderer for LOD bakes, lightmap bakes, decimation previews. |
| **OG Dell_I9** *(this)* | RTX A2000 | **Verification renders + GLB smoke-load + gate** | The A2000 isn't a content card, but it's perfect for "does this GLB load + render at 30 FPS in the engine?" gate before integration. |

The fleet has **~6 active Blender seats** with the right division of labor. That's a real production capacity, not a one-rig bottleneck.

---

## Per-lane working pattern

1. **Lane modeler** opens Blender on their rig with the lane's asset list (per `i5000.md` / `DJBOOTH.md` / `Snake13700k.md`).
2. **Concept refs** generated on the 14900K (Flux on the 5090) and dropped to `staging/refs/<asset>/`. AI refs **never go in `assets/`** — that's reserved for shippable GLBs.
3. **Lane modeler builds in Blender**, periodically posts WIP screenshots to `#X3Native` for fleet feedback.
4. **Bake on the 14900K** (5090 + OptiX) for hero assets; lighter LODs can bake locally on the 1080 Ti rigs.
5. **Export to GLB** via the lane's local Blender; texture maps go through `tools/ktx2bake` for KTX2 compression.
6. **Drop into `assets/rigged_glb/`** or `assets/converted_glb/<kit>/` and push as a feature branch.
7. **OG Dell_I9 (A2000) verifies** the GLB loads + renders + doesn't tank perf via `--smoketest --world <whatever>`.
8. **13700K (IntegratorCaptainCommanderInspector) merges to main.**

---

## Why "grow the Blender capability across the fleet" specifically

Right now, **only Snake is actively shipping Blender content** (Rodin Minerva). If only one machine knows how to do 3D, the lane is a single point of failure and a bottleneck. The fix is *not* "buy more cards" — it's **cross-training the fleet's Claude sessions on Blender's MCP + Blender's Python API + the lane's asset list**, so any machine can pick up an open ticket from `FLEET.md`'s queue and ship a GLB.

The leverage moves, in order:

1. **Standardize the Blender MCP install** on every fleet PC with a GPU >= 1080 Ti class. README + setup script in `tools/blender-setup/` (TODO).
2. **Build a shared `assets/blend_source/` directory** — the `.blend` master files for shipped GLBs, so any rig can open/edit/re-export without re-modeling from scratch.
3. **A shared concept-ref library at `staging/refs/`** — Flux outputs the 14900K renders, dropped here, indexed by lane.
4. **Cross-machine Cycles tile rendering** — for hero bakes, the 14900K coordinates a job, distributes tiles to 13700K + Predator + DJBOOTH, gathers results. (Long-term — needs network glue. Not Phase 1.)
5. **A `BLENDER_CONVENTIONS.md`** spec — Y-up, −Z forward, 1m = 1m, naming, LOD ladder, UV islands. Locks the contract so every fleet Claude exports compatibly.

---

## What lives where

| Path | What |
|---|---|
| `tools/blender-setup/` | (TODO) Cross-machine install script for Blender + MCP + the GLB exporter |
| `tools/ktx2bake/` | (exists) PBR texture → KTX2 compression |
| `staging/refs/<asset>/` | (TODO) AI concept references — gitignored binary blobs OR LFS, TBD |
| `assets/blend_source/` | (TODO) `.blend` master files for shipped GLBs |
| `assets/rigged_glb/` | (exists) Shipped character GLBs |
| `assets/converted_glb/<kit>/` | (exists) Shipped environment GLBs |
| `docs/BLENDER_CONVENTIONS.md` | (TODO) The contract every Blender export must follow |

---

## The 5090's specific superpower

The 14900K is the only machine that should run:

- **Cycles + OptiX denoise** on hero renders (10–100× faster than the 1080 Ti)
- **Blender MCP** for live in-Blender Claude assistance (Snake is using this — the workflow scales)
- **Flux / HiDream image gen** for concept refs (Flux.1-dev needs ~24GB; the 5090 swallows it)
- **High-poly sculpting passes** before retopo/decimation (multi-million-poly meshes)
- **OpenUSD / Hydra delegate** experiments (Pixar render path, future-facing)

Everything else — game-weight modeling, LOD bakes, light scene work, MCP-driven asset edits — runs fine on the 1080 Ti class. **Don't queue 1080 Ti work onto the 5090.** Reserve it for what only it can do.

---

## How a new machine joins the lane

1. Pick the lane from `FLEET.md`'s roster table.
2. Install Blender 4.x + Blender MCP + the GLB exporter (script TBD).
3. Read `BLENDER_CONVENTIONS.md` (TBD).
4. Pull `staging/refs/<your-lane>/` for concept inputs.
5. Open the corresponding `.blend` in `assets/blend_source/` (or start fresh if first asset).
6. Build, bake, export GLB.
7. Push as feature branch, ping `#X3Native` in Matrix, wait for OG verification → 13700K integration.

---

**This doc is the policy. The TODO items (Blender setup script, conventions spec, blend_source dir, refs library) are the next concrete pieces. Whoever picks them up first should branch from main and push.**
