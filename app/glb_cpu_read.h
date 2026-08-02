#pragma once
// ============================================================================
// Minimal CPU-side GLB primitive reader — LOD chain building only (Lane 5).
//
// CLEAN-ROOM, original work, written from the public glTF 2.0 / GLB container
// specification (Khronos). No GPL / id Tech / RBDOOM / Unreal / cgltf source
// consulted. See docs/CLEANROOM_PROCESS.md.
//
// WHY THIS EXISTS AND WHY IT IS NOT THE ENGINE'S LOADER:
// engine/asset/IModelLoader uploads straight to the device and hands back opaque
// GPU handles — by design, so game code never sees vertex data. Building an LOD
// chain needs the OPPOSITE: the CPU triangles, so the decimator can collapse
// them. Rather than widen the engine loader's contract (a shared file other
// lanes touch), this reads the same .glb file a second time, on the CPU, for the
// LOD demo/capture rig. It is NOT on any runtime path.
//
// SCOPE: exactly what the decimator needs and nothing else —
//   * the GLB container (12-byte header + JSON chunk + BIN chunk)
//   * accessors of type VEC3/VEC2/SCALAR with float, uint16 or uint32 components
//   * POSITION / NORMAL / TEXCOORD_0, and the (required) index accessor
//   * node-hierarchy world transforms, so a multi-part model lands assembled
// Draco / meshopt / sparse accessors / external .bin URIs are NOT supported and
// are reported as a load failure rather than guessed at.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// One triangle-list chunk, already transformed into the model's world space.
struct GlbPrimitive {
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>            idx;
    int         baseColorImage = -1;   // index into GlbModel::images, or -1
    std::string name;
};

// A decoded image blob, still in its source encoding (PNG/JPEG) — decode with
// stb_image at the call site so this header stays dependency-free.
struct GlbImage {
    std::vector<uint8_t> bytes;
    std::string          mime;
};

struct GlbModel {
    std::vector<GlbPrimitive> prims;
    std::vector<GlbImage>     images;
    bool ok = false;
    std::string error;
};

// Read `path`. Primitives smaller than `minTriangles` are dropped (collision
// hulls, screws, and other geometry that is pointless to LOD). On any
// unsupported construct the result has ok == false and `error` says why.
GlbModel readGlbForLod(const std::string& path, uint32_t minTriangles = 200);

} // namespace x3::game
