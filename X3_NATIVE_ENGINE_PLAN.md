# X3 Native Engine — Architecture Decision & Build Plan

> ## ⚠️ CORRECTION (2026-05-21) — the RBDOOM/hybrid path below was NOT taken
>
> This document originally proposed a **HYBRID** strategy: fork RBDOOM-3-BFG (GPL)
> for speed, then clean-room rewrite the GPL parts before commercial ship.
> **That path was never executed.** The engine was instead built **clean-room from
> scratch** on the 13700K — from the `specs/`, public technical references, and the
> author's own research — with **no id Tech / RBDOOM / Doom 3 (or any other engine)
> source ever forked, copied, or consulted.** There is no GPL code, no
> `engine/_gpl_rbdoom/` directory, and no GPL debt.
>
> The result: X3Native is **100% original work**, proprietary, and free to ship
> closed-source / commercially. See `PROVENANCE.md`, `LICENSE`, and
> `LICENSE-NOTICE.md`. The rest of this doc is kept for its still-valid technical
> content (stack, runtime/pak model, milestones); read §5 for the corrected license
> position and treat all "fork RBDOOM" / "de-GPL" / `_gpl_rbdoom` references below as
> historical (superseded). The Babylon X3 remains the design reference + fallback.

**Decision date:** 2026-05-19 · **Corrected:** 2026-05-21
**Status:** BUILT — clean-room from scratch (no engine fork). Engine building + running with a playable Level 1 graybox.
**Supersedes the "BabylonNative port" recommendation in** `X3_NATIVE_FEASIBILITY_2026-04-27.md` **for these reasons:** Tim wants (b) a long-term engine that powers many games and (c) the ownership/portfolio value — not just the fastest path to ship X3.

### Strategy as built: clean-room original engine

Tim's goal was a long-term, **wholly-owned** engine powering many games. Rather than fork RBDOOM and carry GPL debt, the build went straight to a **clean-room original implementation**: define the interfaces, write behavioral specs, and implement each subsystem from those specs + public references + the author's own research. This yields clean commercial IP from day one — no de-GPL phase needed.

> **For the Claude picking this up:** read this top-to-bottom for the stack and milestones, but note the framing below (§1, §2, §5.1–5.4, §6 Phase 2, §7) was written for the abandoned RBDOOM-fork plan. The actual engine is original; see `PROVENANCE.md` for what was built.

---

## 1. The decision in one paragraph

Build a **custom native game engine in C++20** that Tim owns — written **clean-room from scratch** (the original plan's "use RBDOOM-3-BFG as the starting codebase" was *not* taken; see the correction banner at the top). Render with **Vulkan 1.3**. Script game logic in **Lua via sol3**. Ship as a **Source-engine-style runtime**: one `X3Engine.exe` + per-game `.x3pak` data files. Physics via **Jolt** (MIT) rather than Havok (commercial license). The engine is one product; X3, TTT 1995, Pin-Pull-Tomb, and future games are separate game-data packages that run on it.

## 2. Why RBDOOM-3-BFG was *considered* as a base (historical — not used)

> The table below was the rationale for the abandoned fork plan. It's retained to
> show what the from-scratch build had to reproduce; **none of this code was used.**


It already provides ~50-70% of an X3-class engine, battle-tested:

| Subsystem | RBDOOM state | Saves |
|---|---|---|
| Vulkan renderer | ✅ (RB's 2019-2021 work, ~Vulkan 1.1) | ~6 weeks |
| PBR materials (metallic-roughness) | ✅ | ~2-3 weeks |
| Cascaded shadow maps | ✅ | ~3 weeks |
| Skinned animation | ✅ md5; glTF via patches | ~3 weeks |
| `.pk4` pak format (zipped packs) | ✅ — **this is literally Tim's chosen runtime+pak model** | ~2 weeks |
| Console + cvar system | ✅ | ~1 week |
| Game logic reference (weapons/monsters/AI) | ✅ C++ | reference for X3 combat |

Net: ~15 weeks solo / ~5-6 weeks with parallel agent orchestration vs ~22 weeks / ~7-8 weeks from scratch.

## 3. Locked-in stack

| Layer | Choice | Notes |
|---|---|---|
| Language | **C++20** | RAII, std::span, modules. Not pure C. |
| Graphics API | **Vulkan 1.3** | Upgrade RBDOOM's ~1.1 backend → 1.3 (descriptor indexing, dynamic rendering, optionally mesh shaders). |
| Physics | **Jolt Physics (MIT)** | NOT Havok — Havok needs a commercial license for native use. Jolt is free, modern, multi-threaded (built by Guerrilla for Horizon Forbidden West). |
| Scripting | **Lua via sol3** | Engine in C++, game logic in Lua, hot-reload. Replace or run alongside id's `idScript`. |
| Audio | **miniaudio** | Single-header MIT, 3D HRTF. Replaces id's sound system. |
| Asset source-of-truth | **glTF 2.0 / GLB** | Tim has 140+ GLBs already converted. Add a glTF loader alongside md5. Parse via `cgltf`; KTX2 via `basisu`. |
| ECS (for new entities) | **flecs** or **EnTT** | Optional — id Tech 4 is OOP-entity. Introduce ECS only for new high-count systems (particles, crowds). Don't fight id's entity model wholesale. |
| Editor/dev UI | **Dear ImGui** | Replace id's Swf/CEGUI for tools. |
| In-game GUI | Custom lightweight GUI (NOT ImGui) | ImGui is dev-tool aesthetic; gameplay UI needs its own. |
| Profiler | **Tracy** | Hook in M0. Single biggest engine-dev productivity tool. |
| Build | **CMake + vcpkg** | RBDOOM already uses CMake; extend it. |

## 4. Runtime + pak model (Tim's choice: Source-engine style)

```
X3Engine.exe                 ← the engine binary (open-source on GitHub)
  base.x3pak                 ← shared engine assets (shaders, default materials, fonts)
  x3.x3pak                   ← X3 game: meshes, textures, audio, Lua scripts, levels
  ttt1995.x3pak              ← a different game on the same engine
  ppt.x3pak                  ← another
```
- `.x3pak` = zipped pack (id's `.pk4` renamed). Engine mounts paks at startup, reads assets by virtual path.
- Lua game scripts ship inside the pak.
- Launch a specific game: `X3Engine.exe +game x3` (cvar-driven, like `+set fs_game`).
- This is the Quake 3 / Doom 3 / Counter-Strike model: engine open, game data sold on Steam.

## 5. License strategy — CLEAN-ROOM ORIGINAL (no GPL)

**As built, X3Native contains no GPL or other copyleft code.** The engine was
written clean-room from scratch — no id Tech / RBDOOM / Doom 3 (or any other
engine) source was forked, copied, or consulted. Legal foundation (not legal
advice, but well-established): **copyright protects expression, not concepts.**
Studying public architecture material and writing your own implementation is
independent creation, not a derivative work — and that is what happened here.

**Current position:**
- Engine = **original work, proprietary, all rights reserved** (`LICENSE`). Free to
  ship closed-source and sell commercially with no copyleft obligation.
- Third-party code = **permissive libraries only** (MIT/Apache/zlib/BSD/PD), tracked
  in `THIRDPARTY_LICENSES.md`. No GPL/LGPL/CC-BY-SA dependencies.
- Game data (`.x3pak`) = a separate work, commercial anytime.
- Originality record + per-subsystem provenance: `PROVENANCE.md`.

There is **no de-GPL phase, no `GPL_DEBT.md`, and no `engine/_gpl_rbdoom/`** — the
"clean target" the hybrid plan aimed for is simply the starting (and current) state.

> **§5.1–§5.4 below are HISTORICAL** — they describe the safeguards/process for the
> abandoned RBDOOM-fork plan (quarantine dir, GPL debt ledger, two-machine
> information barrier). None were needed because no GPL code was ever introduced.
> The spec-driven discipline they describe *was* used — and is what makes the
> independent-creation record (`PROVENANCE.md`, the in-file "no foreign source
> consulted" notes, `specs/`) strong. Kept for reference only.

### 5.1 The three structural safeguards (historical — for the abandoned fork plan)

These make "rewrite later" actually happen instead of silently shipping GPL:

1. **Clean interfaces from day 1.** Define abstract interfaces — `IRenderDevice` (RHI), `IPhysicsWorld`, `IAudioBackend`, `IAssetSource`, `IInputSource`. Game logic (Lua) and your engine API call ONLY these interfaces. The GPL-derived RBDOOM code is wired in as the *v0 implementation behind the interface*. Clean-room rewrite later = reimplement behind the same interface, swap the binding, delete the GPL impl. Zero game-code churn.

2. **GPL debt ledger.** Maintain `GPL_DEBT.md` at engine root: every file/module derived from RBDOOM, what interface it implements, replacement status (TODO / IN-PROGRESS / DONE-CLEAN). The commercial-ship gate is "ledger is empty." Always-visible scope; never forgotten.

3. **Quarantine directory.** All GPL-derived code lives under `engine/_gpl_rbdoom/` — clearly marked, never mixed with clean code. The final purge is mechanical: when every interface has a clean impl, delete the directory and flip the build flag `USE_GPL_SCAFFOLD=OFF`. If it still builds + runs, you're GPL-free.

### 5.2 Clean-room rewrite hygiene (when replacing each GPL module)

- Reimplement against YOUR interface + public references (Vulkan spec, "Real-Time Rendering", GPU Gems, glTF spec, Jolt/miniaudio docs) — **not** by re-reading the RBDOOM source.
- Prefer permissive libraries over hand-writing (they do the hard low-level work AND are clean IP): vk-bootstrap + VMA (Vulkan), cgltf (glTF), basis_universal (KTX2), Jolt (physics), miniaudio (audio), EnTT/flecs (ECS), Dear ImGui (UI), glm (math), Tracy (profiler). All MIT/Apache/zlib/BSD — shippable closed-source.
- Note provenance: if a non-obvious idea came from a public source, cite that, not RBDOOM.

### 5.3 Commercial model regardless of timing

- Pre-purge (prototype phase): engine is GPL — keep it on a public branch, do NOT sell builds.
- Post-purge (clean phase): engine is 100% yours — closed-source, commercial, no strings. Ship `X3Engine.exe` + `.x3pak` on Steam freely.

**Open confirm for Tim:** GPL is fine *during the prototype phase* (it's just on GitHub, not sold). The commercial ship waits for the ledger to hit empty. Acknowledge you're OK with that gating.

### 5.4 Automated clean-room rewrite — two-machine information barrier

Tim's plan: dedicate the **13700K** (128GB DDR5, 2x 1080 Ti) as a clean-room rewrite farm driven by scripts + parallel agents. To keep this LEGALLY clean-room (not "dirty-room look-and-reimplement"), enforce a strict information barrier between the two machines:

```
  14900K — "SPEC TEAM"                 13700K — "CLEAN-ROOM TEAM"
  ┌─────────────────────────┐         ┌─────────────────────────────┐
  │ Reads RBDOOM GPL source  │         │ NEVER sees RBDOOM source     │
  │ Writes BEHAVIORAL SPECS: │  spec   │ Reads: specs + public refs   │
  │  - interface contract    │ ──────► │  (Vulkan spec, RTR book,     │
  │  - what it does (not how)│  .md    │   glTF spec, lib docs)       │
  │  - inputs/outputs/edge   │  only   │ Writes: clean impl behind    │
  │  - perf characteristics  │         │  the interface               │
  │ Writes NO clean code     │         │ Tests against the spec       │
  └─────────────────────────┘         └─────────────────────────────┘
```

**Protocol:**
1. Spec team (14900K) picks a GPL module from `GPL_DEBT.md`, reads it, writes `specs/<module>.spec.md` — describing the interface contract + observable behavior + test cases. **No source code in the spec.** Pseudocode only where unavoidable, expressed as algorithm description not transcription.
2. Spec is committed/pushed. The clean-room repo on the 13700K has **no checkout of the GPL source** (separate repo or sparse-checkout that excludes `engine/_gpl_rbdoom/`).
3. Clean-room team (13700K agents) implements `engine/<subsystem>/<module>.cpp` from the spec + public references only. Writes its own tests from the spec's test cases.
4. Swap the interface binding from the GPL v0 impl → the clean impl. Run the spec's acceptance tests. If green, mark the ledger entry DONE-CLEAN.
5. The clean impl's git history shows it was authored on a machine that never had the GPL source — supporting evidence of independent creation.

**Why this matters:** if challenged, "the implementation was written by a team/process with no access to the GPL source, working from a behavioral spec" is the gold-standard defense. Tim's two-machine setup makes this natural instead of contrived.

**Tooling to build (small):**
- `tools/spec-extract/` (runs on 14900K): helper that, given a GPL module path, scaffolds a `*.spec.md` template + opens the file for the spec-writing agent.
- `tools/cleanroom-verify/` (runs on 13700K): runs the spec's acceptance tests against the clean impl, updates `GPL_DEBT.md` status automatically.
- Information barrier is enforced by *repo topology*, not honor system: the 13700K clones a `*-cleanroom` branch/repo that physically omits `engine/_gpl_rbdoom/`.

## 6. Milestones (RBDOOM base, with parallel-agent compression)

| M# | Goal | Solo | Parallel | Gate |
|---|---|---|---|---|
| **M0** | Clone RDOOM on 14900K, build it, run vanilla Doom 3 BFG | 2-3 days | 1 day | Game launches, renders, takes input |
| **M1** | Upgrade Vulkan 1.1 → 1.3 (descriptor indexing, dynamic rendering) | 2 weeks | 4 days | Renders at parity, validation-clean |
| **M2** | glTF 2.0 loader alongside md5 (use Tim's 140 GLBs) | 1 week | 2 days | A GLB renders in-engine with PBR |
| **M3** | Jolt Physics integration + character controller | 1.5 weeks | 3 days | Capsule walks on heightfield, collides |
| **M4** | sol3 + Lua VM, expose engine API to Lua, scripted scene | 1 week | 2 days | A Lua script spawns + moves an entity |
| **M5** | Port one X3 area: arena + player + 1 bot + 1 weapon (Lua) | 2 weeks | 1 week | Playable X3 vertical slice, native |
| **M6** | `.x3pak` pipeline: bake assets, mount, hot-reload | 1 week | 3 days | X3 slice loads entirely from a pak |
| **M7** | Modern post-FX: GPU particles (compute), DoF, motion blur, OIT (bloom already in RBDOOM) | 2 weeks | 4 days | Visual parity with Babylon X3 |
| **M8** | UI: Dear ImGui (dev) + custom in-game GUI (HUD, menus) | 2 weeks | 1 week | HUD + pause menu functional |
| **M9** | Audio (miniaudio 3D), Steamworks SDK | 2 weeks | 1 week | Spatial SFX + music, Steam overlay |
| **M10** | Cut a real X3 build, Steam page | 1 week | 3 days | Shippable .exe + .x3pak on a clean PC |

**Total: ~15 weeks solo / ~5-6 weeks parallel.**

### Phase 2 — De-GPL clean-room track (HISTORICAL — not needed; engine was clean from the start)

> This track existed to convert forked GPL modules to clean impls. **It was never
> needed:** every subsystem (D1-D8 below) was written clean from the start, so there
> was nothing to "de-GPL." The table is kept only as the subsystem checklist; for
> actual status see `PROVENANCE.md`.

| D# | GPL module to replace | Interface | Spec team (14900K) | Clean-room (13700K) |
|---|---|---|---|---|
| **D1** | RHI / Vulkan device + swapchain | `IRenderDevice` | spec the device/swapchain/cmd-buffer contract | reimplement on vk-bootstrap + VMA |
| **D2** | Material / shader pipeline | `IMaterialSystem` | spec PBR inputs, shader permutation keys | reimplement; shaders authored fresh (GLSL→SPIR-V) |
| **D3** | Shadow maps (CSM) | `IShadowRenderer` | spec cascade split + bias behavior | reimplement from RTR/GPU Gems |
| **D4** | Mesh / scene submission | `ISceneRenderer` | spec draw submission + culling contract | reimplement (optionally GPU-driven) |
| **D5** | Pak/vfs | `IAssetSource` | spec mount order + virtual path rules | reimplement (zip via miniz, MIT) |
| **D6** | Console + cvars | `IConsole` | spec cvar registration + command dispatch | reimplement fresh (trivial) |
| **D7** | Math / containers | n/a | (often already replaceable by glm/STL) | swap to glm + std |
| **D8** | Animation (md5/skinning) | `IAnimSystem` | spec skinning + blend behavior | reimplement (already targeting glTF skins) |

When D1-D8 are all DONE-CLEAN and `engine/_gpl_rbdoom/` can be deleted with the build still green (`USE_GPL_SCAFFOLD=OFF`), the engine is commercial-ready.

**Note:** several "replacements" are really "adopt a permissive lib + write glue" rather than line-for-line rewrites — that's faster AND cleaner. The clean-room rigor matters most for the bespoke renderer logic (D2-D4), less for swappable infrastructure (D5-D7).

## 7. M0 — First actions on the 14900K

When a Claude session starts on the 14900K:

1. **Locate the source.** Find the RDOOM/RBDOOM-3-BFG / "Ardoom 2019" directory. Likely `C:\GameDev\...` — survey it, identify the delta from upstream RBDOOM-3-BFG, note the Vulkan backend version.
2. **Build it.** RBDOOM uses CMake. Generate a VS2022 solution, build Release x64. Resolve any dependency gaps (vcpkg). Get `RBDoom3BFG.exe` running vanilla.
3. **Verify the three risk items** from the April audit:
   - Does the Vulkan renderer run clean on the RTX 5090 (validation layers on)?
   - Does the physics layer work (spawn a falling object)?
   - Does the pak system mount + read assets?
4. **Gate decision:** if it builds + runs + the three checks pass → green-light M1. If a hard blocker → fall back to BabylonNative port (April plan) or a permissive-license base.
5. **Report back** the source delta, build status, Vulkan version, and a go/no-go.

## 8. Cross-references

- `X3_NATIVE_FEASIBILITY_2026-04-27.md` — module-by-module porting tier breakdown (Tier A/B/C/D), browser-API inventory. Still useful: the Tier-A "pure logic" modules (NPC systems, bot AI, world builders) are the ones whose *design* ports most directly even though we're going C++ not JS.
- `CLAUDE.md` — X3 critical patterns (still relevant for the design even in the native rewrite).
- Current Babylon X3 stays the **design sandbox + reference implementation**. Don't delete it. New native engine cribs gameplay/feel from it.

## 9. What stays on the Babylon side (don't throw away)

The current Babylon X3 (`C:\GameDev\Q3Engine`) remains valuable as:
- The **playable reference** for gameplay feel, tuning values, level layouts.
- The **content source**: meshes, textures, audio, level data all transfer to the native engine via the `.x3pak` pipeline.
- The **quality-preset + benchmark system** (just built) — its tuning data informs native perf targets.
- A **shippable fallback** (Electron-wrap) if the native engine slips.

---

*Authored 2026-05-19 on I9DevPC (Dell laptop, A2000). The native engine work happens on the 14900K (RTX 5090, 128GB) where the RDOOM source lives. This doc is the cross-machine handoff.*
