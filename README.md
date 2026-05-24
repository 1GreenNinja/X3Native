# X3Native

Custom native game engine (C++20 / Vulkan 1.3) for the 1GreenNinja game portfolio — X3, TTT 1995, Pin-Pull-Tomb, and future titles. One engine, many games shipped as `.x3pak` data.

**X3Native is 100% original work.** It was built clean-room from scratch — written from behavioral specs + public technical references (the Vulkan spec, *Real-Time Rendering 4th ed.*, GPU Gems, vkguide.dev, GDC/SIGGRAPH talks, library docs) plus the author's own research. The only third-party code is the permissively-licensed libraries listed in `THIRDPARTY_LICENSES.md`. See `PROVENANCE.md` and `LICENSE-NOTICE.md`.

> **New here? Read in this order:**
> 1. `X3_NATIVE_ENGINE_PLAN.md` — the architecture decision, locked stack, runtime/pak model, milestones
> 2. `X3_NATIVE_SLICES.md` — the executable backlog
> 3. `PROVENANCE.md` — the originality record (how it was built, clean-room, what's done)
> 4. `BUILD.md` + `docs/13700K_SETUP.md` — clone, build, run

## Status

**Building + running** — a native windowed app with a playable graybox test level. Verified on RTX A2000 (laptop) and GTX 1080 Ti (13700K), runs on RTX 5090 (14900K).

Done + verified: **render device** (Vulkan 1.3 dynamic rendering, bindless textures, multidraw-indirect), **pak / virtual filesystem** (`.x3pak`), **console + cvars**, **glTF/GLB loader** + PBR, **Jolt physics** world + character controller, **forward+ point lighting** (16 lights + ACES tonemap), **skeletal animation** (CPU skinning), free-look camera + input, PNG screenshot capture. The `app/` layer has a player, weapons, melee, monsters, doors, triggers, objectives, FX, and a HUD on a Level 1 graybox.

The Babylon-JS X3 (`1GreenNinja/X3Engine`) remains the **design reference + content source + shippable fallback** — not deleted.

## Stack (locked 2026-05-19)

| Layer | Choice |
|---|---|
| Language | C++20 |
| Graphics | Vulkan 1.3 (dynamic rendering, descriptor indexing / bindless, multidraw-indirect) |
| Codebase origin | **Original — clean-room from scratch (no engine fork). See `PROVENANCE.md`.** |
| Physics | Jolt (MIT) |
| Scripting | Lua via sol3 |
| Audio | miniaudio |
| Assets | glTF/GLB + KTX2 |
| Runtime | `X3Engine.exe` + `.x3pak` (Source/Quake-style: engine binary + game-data paks) |
| Build | CMake + vcpkg, VS2026 |
| Profiler | Tracy |

## License

X3Native is **original work — proprietary, all rights reserved** (see `LICENSE`). Because no GPL or other copyleft code is incorporated, there is **no obligation to keep it public and no bar to closed-source commercial release**.

- **Engine** (`X3Engine.exe`) — Tim Smith's IP. May be shipped closed-source and sold commercially.
- **Game data** (`.x3pak` — meshes, textures, audio, Lua) — separate work, commercial anytime (the Quake 3 / Doom 3 distribution model).
- **Third-party libraries** — permissive only (MIT / Apache 2.0 / zlib / BSD / public-domain), tracked in `THIRDPARTY_LICENSES.md`. No GPL/LGPL/CC-BY-SA dependencies.

The repo is currently public for convenience; it can be made private at any time with no licensing consequence.

## Repo layout

```
engine/
  core/      console + cvars, job system, logging
  rhi/       IRenderDevice + Vulkan 1.3 backend (bindless, multidraw-indirect)
  asset/     IAssetSource (pak/VFS) + glTF/GLB model loader
  physics/   IPhysicsWorld + Jolt backend + character controller
  audio/     IAudioSystem + miniaudio backend
app/         the X3Engine host + Level 1 graybox game (player, weapons, monsters, HUD, FX)
shaders/     GLSL → SPIR-V (mesh, hud, shadow, tri)
specs/       behavioral specs the implementations are built from (clean-room record)
tools/       ktx2bake, pak builder, bootstrap
docs/        setup, roadmaps, rendering notes
PROVENANCE.md          originality record (engine is original; no foreign engine source)
THIRDPARTY_LICENSES.md every third-party lib + its permissive license
```

## Build

```powershell
git clone https://github.com/1GreenNinja/X3Native.git
cd X3Native
$env:VCPKG_ROOT = "C:\vcpkg"           # your vcpkg clone
cmake --preset windows-vs2026          # or open the folder in VS2026
cmake --build --preset windows-vs2026  # Release x64
.\build\bin\Release\X3Engine.exe       # fly/walk the lit graybox level
```

See `BUILD.md` and `docs/13700K_SETUP.md` for full setup (Vulkan SDK, vcpkg, toolchain).
