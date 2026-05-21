# 13700K — Setup & Continue

The engine is **building + running** (verified on the A2000 laptop). The 13700K (GTX 1080 Ti) should build identically — the device line will name the 1080 Ti. Follow this top-to-bottom.

## 1. Install (one-time)

| Tool | How | Notes |
|---|---|---|
| **Git** | likely already present | `git --version` to check |
| **Visual Studio 2026** (or 2022) | with the **Desktop development with C++** workload | provides MSVC + bundled CMake + Ninja |
| **Vulkan SDK 1.3+** | `winget install KhronosGroup.VulkanSDK` | **required** — the GPU driver gives the *runtime*, this gives headers/glslc/validation. New shell after, confirm `echo $env:VULKAN_SDK` is non-empty |
| **vcpkg** | `git clone https://github.com/microsoft/vcpkg C:\vcpkg` then `C:\vcpkg\bootstrap-vcpkg.bat` | then set `VCPKG_ROOT=C:\vcpkg` (System env var or per-shell) |
| **NVIDIA driver** (1080 Ti) | recent Game Ready / Studio driver | must expose Vulkan **1.3** + features: dynamicRendering, synchronization2, descriptorIndexing, timelineSemaphore, bufferDeviceAddress. Recent drivers do. If `init()` logs a feature-select failure, relax `set_required_features_*` in `engine/rhi/VulkanRenderDevice.cpp` and note it. |

vcpkg auto-fetches the C++ deps on first configure (vk-bootstrap, VMA, glfw3, miniz, glm) — no manual install. First configure takes a few minutes building them.

## 2. Clone + build

```powershell
git clone https://github.com/1GreenNinja/X3Native.git
cd X3Native
$env:VCPKG_ROOT = "C:\vcpkg"          # if not a system var
cmake --preset windows-vs2026         # or open the folder in VS2026 (CMake auto-configures)
cmake --build --preset windows-vs2026 # Release x64
```
(If the generator name differs, use `-G "Visual Studio 17 2022"`. CMakePresets targets VS2026 = "Visual Studio 18 2026".)

`vcpkg.json` baseline is pinned (`f7f9411`); no edit needed.

## 3. Run + verify

```powershell
.\build\bin\Release\X3Engine.exe              # windowed: fly around the lit cube
.\build\bin\Release\X3Engine.exe --smoketest  # headless: 30 frames + recreate, exit 0
.\build\bin\Release\X3Engine.exe --test-asset # D5 pak/VFS: 7/7 tests, exit 0
.\build\bin\Release\X3Engine.exe --test-console # D6 console: 8/8 tests, exit 0
```
Controls (windowed): mouse look, **WASD** move, **Space/Ctrl** up/down, **Shift** sprint, **Esc** quit. Expected log: `[rhi] device ready: NVIDIA GeForce GTX 1080 Ti (Vulkan 1.3 ...)`.

For a **validation-clean** check, build + run the **Debug** config's `--smoketest` (validation layers on; must be error-free).

## 4. What's done (so you don't redo it)

DONE-CLEAN + verified: **D1** render device, **3D cube** (depth + MVP + lighting), **free-look camera + input**, **D5** pak/VFS, **D6** console. See `GPL_DEBT.md`.

## 5. What to work on next (clean-room — never read RBDOOM source)

Pick from `docs/13700K_PARALLEL_WORK.md`. Highest-value next, all spec-ready:

1. **Jolt physics** (`specs/M3-physics-world.spec.md`) → physics world + capsule character controller. This makes the scene *walkable* (gravity + collision) instead of fly-only. **Recommended next.**
2. **glTF loader** (`specs/M2-gltf-loader.spec.md`) → import the 140 GLBs (disk-heavy, suits this machine).
3. **KTX2 texture bake** (no code) → compress the 2.8GB PBR masters.
4. **Lua/sol3** (`specs/M4-script-vm.spec.md`), **audio** (`specs/M9-audio-backend.spec.md`).

The bigger renderer features (bindless, multidraw-indirect, GPU-driven culling — see `RENDERING_SPEED.md`) layer onto the existing D1 device next.

> Update `GPL_DEBT.md` as you complete each subsystem; commit small, build + run the self-tests before pushing.
