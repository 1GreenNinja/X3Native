# X3Native

Custom native game engine (C++20 / Vulkan 1.3) for the 1GreenNinja game portfolio — X3, TTT 1995, Pin-Pull-Tomb, and future titles. One engine, many games shipped as `.x3pak` data.

> **New here? Read in this order:**
> 1. `X3_NATIVE_ENGINE_PLAN.md` — the decision, stack, license strategy, milestones (M0-M10 + de-GPL D1-D8)
> 2. `X3_NATIVE_SLICES.md` — the 100-slice executable backlog
> 3. `X3_NATIVE_QUESTIONS.md` — open questions / kickoff checklist (answer the quick-block to unblock M0)
> 4. `docs/CLEANROOM_PROCESS.md` + `specs/README.md` — the de-GPL clean-room protocol

## Status

**Pre-M0.** This repo is seeded with planning + clean-room machinery from the I9DevPC laptop on 2026-05-20. The actual engine build happens on the **i9-14900K + RTX 5090** (where the RBDOOM / "Ardoom 2019" source lives). The Babylon-JS X3 (`1GreenNinja/X3Engine`) remains the **reference implementation + content source + shippable fallback** — not deleted.

## Stack (locked 2026-05-19)

| Layer | Choice |
|---|---|
| Language | C++20 |
| Graphics | Vulkan 1.3 (dynamic rendering, descriptor indexing) |
| Base codebase | RBDOOM-3-BFG / id Tech 4 (GPL v3) — **hybrid: fork now, clean-room rewrite before commercial ship** |
| Physics | Jolt (MIT) |
| Scripting | Lua via sol3 |
| Audio | miniaudio |
| Assets | glTF/GLB + KTX2 |
| Runtime | `X3Engine.exe` + `.x3pak` (Source/Doom3 model) |
| Build | CMake + vcpkg, VS2026 |
| Profiler | Tracy |

## ⚠️ License — HYBRID, read before shipping

This repo uses **GPL-v3 RBDOOM code as a temporary scaffold** (quarantined under `engine/_gpl_rbdoom/`). Commercial ship is gated on `GPL_DEBT.md` being empty — every GPL module replaced by a clean-room implementation. See `X3_NATIVE_ENGINE_PLAN.md` §5 and `docs/CLEANROOM_PROCESS.md`.

- **During prototype:** engine contains GPL code → keep public, do not sell builds.
- **After de-GPL:** engine is 100% owned → closed-source + commercial OK.

## Repo layout (target)

```
engine/
  _gpl_rbdoom/   ← quarantined GPL scaffold (deleted before commercial ship)
  core/  platform/  rhi/  render/  physics/  audio/  asset/  anim/  ui/  script/
games/
  x3/  ttt1995/  ppt/   ← Lua + small C++ glue, shipped as .x3pak
tools/
  cleanroom-setup.ps1   ← 13700K clean-room bootstrap
  x3pakbuild/           ← pak builder (M6)
specs/                  ← clean-room behavioral specs (spec team writes, clean-room team implements)
docs/
GPL_DEBT.md             ← the de-GPL ledger; ship gate = empty
```

## First action (14900K)

```powershell
git clone https://github.com/1GreenNinja/X3Native.git
cd X3Native
# Read X3_NATIVE_ENGINE_PLAN.md, then start M0 (Slice 1: locate + survey the RBDOOM source).
```
