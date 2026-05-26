# Task #31 — explosive barrels use the round Barrel.glb (not the cube)

`app/barrels.cpp` currently: `init()` does `makeCube(0.5f)` -> `m_cube`, and `render()`
draws `m_cube`. So barrels are square. The real model is
`assets/converted_glb/SciFi_Warehouse_Kit/Barrel.glb` (1 drawable prim). Load it like
`RescueVictim` / `env_art` do and draw its drawables at each barrel transform; keep the
cube as fallback.

## barrels.h — add members (near `m_cube`, `m_tex`)
```cpp
#include "engine/asset/AssetSource.h"   // IAssetSource / createAssetSource
#include "engine/asset/ModelLoader.h"   // IModelLoader / makeDrawables / ModelDrawable
// ...
std::unique_ptr<x3::asset::IAssetSource>  m_assets;
std::unique_ptr<x3::asset::IModelLoader>  m_loader;
x3::asset::ModelInstance                  m_barrelModel;     // (or whatever load() returns)
std::vector<x3::asset::ModelDrawable>     m_barrelDrawables; // empty -> cube fallback
```

## barrels.cpp — in `init()`, AFTER building the m_cube fallback, load the GLB
```cpp
// Real round barrel model (overlay; falls back to the cube if absent).
m_assets.reset(x3::asset::createAssetSource());
if (m_assets->mountDir(x3::app::convertedGlbRoot(), 0)) {           // app/asset_root.h
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    m_barrelModel = m_loader->load("SciFi_Warehouse_Kit/Barrel.glb");
    if (m_barrelModel.ok) {
        m_barrelDrawables = x3::asset::makeDrawables(m_barrelModel);
        x3::logInfo("[barrels] loaded Barrel.glb — " +
                    std::to_string(m_barrelDrawables.size()) + " prim(s)");
    } else {
        x3::logWarn("[barrels] Barrel.glb load failed; using cube fallback");
    }
}
```
(Add `#include "app/asset_root.h"` for `convertedGlbRoot()`.)

## barrels.cpp — in `render()`, draw the GLB drawables instead of the cube
Replace the per-barrel `m_device->drawMesh(frame, m_cube, m_tex, color, m);` with:
```cpp
if (!m_barrelDrawables.empty()) {
    for (const auto& d : m_barrelDrawables) {
        float fin[16];
        x3::asset::mulMat4(m, d.nodeTransform, fin);   // m = the barrel's world TRS
        // intact = the model's own PBR color; debris keeps the tinted look.
        const float* col = v.intact ? d.baseColorFactor : debris;
        m_device->drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                           x3::rhi::TextureHandle{ d.baseColorTexId }, col, fin);
    }
} else {
    m_device->drawMesh(frame, m_cube, m_tex, v.intact ? intact : debris, m);  // fallback
}
```
NOTE: verify the load() return type + makeDrawables signature against `app/rescue.cpp`
(it uses the exact same pattern: `m_assets->mountDir`, `m_loader->load`, `makeDrawables`,
then draws `d.baseColorFactor` at `model * d.nodeTransform`). The barrel's collision +
fracture/explosion are unchanged — this is render-only.

## Cleanup
In the destructor, after `if (m_cube.valid()) m_device->destroyMesh(m_cube);` the loader
owns the GLB meshes (released when m_loader resets) — match rescue.cpp's lifetime.
