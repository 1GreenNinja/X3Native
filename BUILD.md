# Building X3Native

## Prerequisites (13700K / any dev machine)

1. **Visual Studio 2026** (Desktop C++ workload) — confirmed installed.
2. **Vulkan SDK** (LunarG): `winget install KhronosGroup.VulkanSDK` — then a new shell so `%VULKAN_SDK%` is set.
3. **vcpkg**: `git clone https://github.com/microsoft/vcpkg C:\vcpkg && C:\vcpkg\bootstrap-vcpkg.bat` then set `VCPKG_ROOT=C:\vcpkg`.
4. **CMake ≥ 3.25** (bundled with VS2026, or `winget install Kitware.CMake`).

## One-time: pin the vcpkg baseline

`vcpkg.json` has `"builtin-baseline": "REPLACE_WITH_VCPKG_COMMIT_SHA"`. Set it once:
```powershell
cd C:\vcpkg; git rev-parse HEAD     # copy the SHA
# paste it into vcpkg.json builtin-baseline
```
(Or run `vcpkg x-update-baseline --add-initial-baseline` inside the repo.)

## Configure + build

```powershell
# from the repo root, with VCPKG_ROOT set:
cmake --preset windows-vs2026          # configures; vcpkg fetches deps (first run is slow)
cmake --build --preset windows-vs2026  # builds Release x64
# run it:
.\build\bin\Release\X3Engine.exe
```

## Expected first-build result (skeleton)

A 1280×720 window titled **X3Engine** opens. Console logs:
```
[INFO]  X3Engine starting...
[INFO]  [rhi] device ready: NVIDIA GeForce GTX 1080 Ti (Vulkan 1.3, dynamic-rendering + sync2 + descriptor-indexing)
[INFO]  [asset] mountPak stub (D5 not implemented): base.x3pak   (this is expected)
[INFO]  entering main loop (close the window to exit)
```
The window is **black** (no rendering yet) — that's correct. The build proves the whole toolchain: SDK found, vk-bootstrap + VMA + glfw + miniz + glm link, validation layers load, a real GPU is selected at Vulkan 1.3.

> If the GTX 1080 Ti driver doesn't expose all required 1.3 features (`dynamicRendering`/`sync2`/`descriptorIndexing`/`timelineSemaphore`/`bufferDeviceAddress`), `init()` will log which selection failed. 1080 Ti on a recent driver supports all of these, but if not, relax `set_required_features_*` in `VulkanRenderDevice.cpp` and note it.

## Next work (13700K, from specs)

- **D1** — implement swapchain + per-frame acquire/clear/present (dynamic rendering) + VMA in `engine/rhi/VulkanRenderDevice.cpp` until `specs/D1-render-device.spec.md`'s 5 acceptance tests pass.
- **D5** — implement `engine/asset/PakAssetSource.cpp` from `specs/D5-asset-source.spec.md` (miniz).
- Update `GPL_DEBT.md` rows to DONE-CLEAN as each passes.
