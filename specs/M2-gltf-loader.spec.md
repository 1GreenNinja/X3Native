# Spec: glTF / GLB Model Loader  (M2)

> Clean-room — implement from THIS FILE + public refs ONLY. No RBDOOM source.
> Standard subsystem (cgltf + basisu wrappers) — zero 14900K dependency. Safe for the 13700K now.

- **Implements interface:** `IModelLoader` (`engine/asset/IModelLoader.h`)
- **Status:** SPEC (ready)
- **Depends on:** `IAssetSource` (D5) for byte access; `IRenderDevice` (D1) for GPU upload.

## 1. Purpose
Parse a GLB/glTF 2.0 file into engine-native mesh, material, and skeleton data, and upload GPU resources (vertex/index buffers, textures). The source-of-truth asset format for the whole engine (Tim has 140+ GLBs already converted).

## 2. Interface contract
```cpp
// engine/asset/IModelLoader.h — clean, no cgltf/Vulkan types leak
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

namespace x3::asset {

struct MeshPrimitive {
    uint64_t vertexBuffer = 0;   // opaque GPU handle from IRenderDevice
    uint64_t indexBuffer  = 0;
    uint32_t indexCount   = 0;
    uint32_t materialIndex = 0;  // index into Model::materials
};
struct Material {
    float baseColor[4]   = {1,1,1,1};
    float metallic       = 1.0f;
    float roughness      = 1.0f;
    float emissive[3]    = {0,0,0};
    uint64_t baseColorTex = 0;   // opaque GPU texture handle (0 = none)
    uint64_t normalTex    = 0;
    uint64_t mrTex        = 0;    // metallic-roughness
    uint64_t emissiveTex  = 0;
    uint64_t occlusionTex = 0;
    bool     doubleSided  = false;
    bool     alphaBlend   = false;
};
struct Node { int parent = -1; float localTransform[16]; int meshIndex = -1; int skinIndex = -1; };
struct Skin { std::vector<int> joints; std::vector<float> inverseBind; /* 16*joints */ };
struct AnimationClip { std::string name; float duration = 0; /* channels: see notes */ };

struct Model {
    std::vector<MeshPrimitive> primitives;
    std::vector<Material>      materials;
    std::vector<Node>          nodes;
    std::vector<Skin>          skins;
    std::vector<AnimationClip> animations;
    bool ok = false;
};

class IModelLoader {
public:
    virtual ~IModelLoader() = default;
    // Load from a virtual path via IAssetSource, upload GPU resources via IRenderDevice.
    virtual Model load(std::string_view virtualPath) = 0;
    virtual void  unload(Model& m) = 0;     // frees GPU handles
};

IModelLoader* createModelLoader(class rhi::IRenderDevice* dev, class IAssetSource* assets);

} // namespace x3::asset
```

## 3. Behavior
- Input: a virtual path (`"models/jake.glb"`) resolved through `IAssetSource`.
- Output: a `Model` with GPU-uploaded buffers + textures and CPU-side node/skin/anim data.
- Coordinate system: glTF is right-handed, +Y up, -Z forward, meters. Document any conversion to engine space (keep glTF convention to minimize surprises).
- Vertex layout: position (vec3), normal (vec3), tangent (vec4), uv0 (vec2), and for skinned: joints (uvec4) + weights (vec4). Pack into one interleaved vertex buffer.
- Textures: prefer KTX2 (transcode via basisu to BC7 on desktop); fall back to PNG/JPG decode for non-KTX2.

## 4. Edge cases & error handling
- Missing file / parse error → `Model{ok=false}`, log path + cgltf error.
- glTF with no normals → generate flat normals; no tangents → generate from UVs.
- Unsupported extension (e.g., draco, unless added) → log, skip that primitive, continue.
- Texture missing → use a 1×1 white/normal default, don't fail the whole model.
- Huge model (>e.g. 2M verts) → still load, log a warning.

## 5. Performance targets
- Load a typical 50k-tri PBR character (KTX2 textures) in ≤ 50 ms on the 13700K.
- One GPU upload batch per model where possible (staging buffer reused).
- Texture transcode (basisu) is the cost — cache transcoded results to disk keyed by content hash.

## 6. Acceptance tests
1. **T1 — Static GLB:** load a cube.glb → 1 primitive, 36 indices, renders (with D1 once rendering lands; for now: buffers created, handles non-zero).
2. **T2 — PBR material:** a GLB with baseColor+metallic+roughness+normal maps → Material fields populated, texture handles non-zero.
3. **T3 — Missing texture:** GLB referencing an absent texture → loads, uses default, logs once, `ok==true`.
4. **T4 — Skinned model:** a rigged GLB → skins[0].joints non-empty, inverseBind size == joints*16, animations non-empty.
5. **T5 — Batch 140:** run over Tim's 140 GLBs → ≥90% load with `ok==true`; emit `docs/GLB_IMPORT_REPORT.md` listing failures + reasons.
6. **T6 — Unload:** `unload` frees all GPU handles (verify with VMA/validation no-leak).

## 7. Public references
- glTF 2.0 Specification (Khronos) — accessor/bufferView/mesh/material/skin/animation.
- cgltf README + examples.
- KTX2 / Basis Universal docs; `basisu` transcoder API.
- "glTF tangent generation" (MikkTSpace algorithm — public).

## 8. Suggested permissive libraries
- **cgltf** (MIT) — glTF/GLB parse.
- **basis_universal** (Apache 2.0) — KTX2 transcode.
- **MikkTSpace** (zlib) — tangent generation.
- **stb_image** (public domain) — PNG/JPG fallback.

## 9. Notes for the clean-room implementer
- Keep cgltf + Vulkan out of the header (Pimpl). `Model` is plain data + opaque handles.
- Animation channels (T/R/S keyframes per node) — store them; the playback/blend logic is the `IAnimSystem` (D8) spec, separate.
- Disk cache for transcoded textures lives under a dev cache dir, keyed by SHA of source bytes — big speedup on the 140-GLB batch.
