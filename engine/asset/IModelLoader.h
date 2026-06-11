#pragma once
// glTF / GLB Model Loader interface — M2.
// Spec: specs/M2-gltf-loader.spec.md
//
// Clean header: no cgltf / Vulkan / stb types leak across this boundary
// (Pimpl in ModelLoader.cpp). `Model` is plain data + opaque GPU handles so
// game / Lua code never sees the graphics or parsing libraries.
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

// Forward decls so the factory signature can name these opaque types without
// pulling in their headers (keeps Vulkan / parsing libs out of this boundary).
namespace x3::rhi { class IRenderDevice; }

namespace x3::asset {

class IAssetSource; // declared in IAssetSource.h

// One drawable chunk: an interleaved vertex buffer + index buffer, plus the
// material it references. The buffer fields are opaque GPU handles minted by
// IRenderDevice (0 == not uploaded).
struct MeshPrimitive {
    uint64_t vertexBuffer = 0;   // opaque GPU handle from IRenderDevice
    uint64_t indexBuffer  = 0;
    uint32_t indexCount   = 0;
    uint32_t materialIndex = 0;  // index into Model::materials
    uint32_t meshIndex    = 0;   // index of the glTF mesh this primitive came from
                                 // (lets makeDrawables() map nodes -> primitives so
                                 //  per-node TRS can be baked into each drawable)

    // ---- CPU-skinning data (J1). Only populated for primitives that carry
    // JOINTS_0 + WEIGHTS_0 (i.e. a skinned mesh) AND were uploaded to a real
    // device, so the anim runtime can recompute their vertices each frame and
    // re-upload via IRenderDevice::updateMesh(). For unskinned/static primitives
    // these vectors are empty and the primitive is drawn untouched. The bind-pose
    // positions/normals are kept (skinning is computed relative to them every
    // frame, never accumulated). vertexCount == basePos.size()/3. ----
    bool                 skinned = false;
    std::vector<float>   basePos;    // bind-pose positions (vertexCount * 3)
    std::vector<float>   baseNrm;    // bind-pose normals    (vertexCount * 3)
    std::vector<float>   baseUv;     // uv0                  (vertexCount * 2)
    std::vector<uint16_t>jointIdx;   // 4 joint indices per vertex (vertexCount * 4)
    std::vector<float>   jointWt;    // 4 weights per vertex       (vertexCount * 4)
};

struct Material {
    float baseColor[4]   = {1, 1, 1, 1};
    float metallic       = 1.0f;
    float roughness      = 1.0f;
    float emissive[3]    = {0, 0, 0};
    uint64_t baseColorTex = 0;   // opaque GPU texture handle (0 = none/default)
    uint64_t normalTex    = 0;
    uint64_t mrTex        = 0;   // metallic-roughness
    uint64_t emissiveTex  = 0;
    uint64_t occlusionTex = 0;
    uint64_t detailTex    = 0;   // HDRP DetailMap (R=desat albedo, G=nrmY, B=smooth, A=nrmX); 0 = none
    float    detailUvScale = 1.0f;   // detail UV tiling (from the converter's x3Detail extras)
    float    detailNrmScale = 1.0f;
    // CLEARCOAT lobe (car paint): a second fixed-F0 (0.04) low-roughness specular
    // layer over the base, from the converter's material.extras["x3Clearcoat"]
    // {intensity, roughness} — the x3Detail extras pattern. 0 = none (default).
    float    clearcoat      = 0.0f;
    float    clearcoatRough = 0.05f;
    bool     doubleSided  = false;
    bool     alphaBlend   = false;
    bool     alphaMask    = false;   // glTF alphaMode==MASK (alpha-cutout)
    float    alphaCutoff  = 0.5f;
};

// Node hierarchy. glTF convention is kept (right-handed, +Y up, -Z forward,
// meters); localTransform is the node's local TRS composed into a 4x4 matrix,
// column-major (matches glTF + glm).
struct Node {
    int         parent = -1;
    float       localTransform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int         meshIndex = -1;
    int         skinIndex = -1;
    // glTF node name (empty string if the source node was unnamed). Populated by
    // the loader from cgltf_node::name so callers can resolve humanoid bones by
    // name (e.g. foot-IK locating UpperLeg/LowerLeg/Foot/Hips). Additive: existing
    // index-based code is unaffected.
    std::string name;
};

struct Skin {
    std::vector<int>   joints;       // indices into Model::nodes
    std::vector<float> inverseBind;  // 16 floats per joint
};

// Sampled animation channel keyframes (T/R/S per target node). Playback /
// blending is the IAnimSystem (D8) job; here we only store the data.
enum class AnimPath : uint8_t { Translation, Rotation, Scale, Weights };

struct AnimationChannel {
    int      targetNode = -1;        // index into Model::nodes
    AnimPath path = AnimPath::Translation;
    std::vector<float> times;        // keyframe input (seconds), N samples
    std::vector<float> values;       // keyframe output, N * components
    uint8_t  components = 0;         // 3 (T/S), 4 (R quat), or weight count
};

struct AnimationClip {
    std::string name;
    float duration = 0;              // seconds (max channel time)
    std::vector<AnimationChannel> channels;
};

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
    // Load from a virtual path via IAssetSource, upload GPU resources via
    // IRenderDevice. On any fatal error returns Model{ok=false}.
    virtual Model load(std::string_view virtualPath) = 0;
    // Frees the GPU handles owned by the model and clears them to 0.
    virtual void  unload(Model& m) = 0;
};

// One drawable record per (node, primitive) pair, resolved to the device's
// handle types so the scene/app can feed it straight to IRenderDevice::drawMesh().
// meshId == 0 means the primitive was not uploaded to a real device (headless path).
//
// nodeTransform is the WORLD matrix of the glTF node that referenced this
// primitive's mesh (the product of its own + all ancestor local TRS, column-major).
// The converted GLBs lean on node transforms for Y-up correction and multi-part
// placement, so the caller MUST draw each drawable at `objectTransform * nodeTransform`
// (object/world placement times this baked node transform). For a single-node /
// identity model (e.g. the synthetic test cube) nodeTransform is the identity, so
// the multiply is a no-op and old behavior is preserved.
struct ModelDrawable {
    uint32_t meshId        = 0;          // -> rhi::MeshHandle{ meshId }
    uint32_t baseColorTexId = 0;         // -> rhi::TextureHandle{ } (0 == default white)
    uint32_t normalTexId   = 0;          // -> rhi::TextureHandle{ } (0 == none; PBR normal map)
    uint32_t mrTexId       = 0;          // -> rhi::TextureHandle{ } (0 == none; metallic-roughness)
    bool     alphaMask     = false;      // glTF alphaMode==MASK: fragment is alpha-cutout (foliage/people)
    bool     alphaBlend    = false;      // glTF alphaMode==BLEND: translucent (glass) -> blend pass
    float    emissiveFactor[3] = {0, 0, 0};  // HDR-scaled emissive color (glowing edge strips etc.)
    uint32_t emissiveTexId = 0;          // -> rhi::TextureHandle{ } (0 == none; emissive map)
    uint32_t detailTexId   = 0;          // -> rhi::TextureHandle{ } (0 == none; HDRP micro-detail map)
    float    detailUvScale = 1.0f;       // detail UV tiling (mesh.frag samples detail at vUV*this)
    float    clearcoat     = 0.0f;       // clearcoat intensity (car paint); 0 = no clearcoat lobe
    float    clearcoatRough = 0.05f;     // clearcoat roughness (mirror-ish lacquer)
    float    baseColorFactor[4] = {1, 1, 1, 1};
    float    nodeTransform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // node world (column-major)
};

// Build per-(node,primitive) draw records from a Model previously loaded with a
// REAL IRenderDevice (handles carry the device's mesh/texture ids). Walks the
// glTF node hierarchy, computes each node's world matrix, and bakes it into the
// drawable's nodeTransform so the caller can apply objectTransform * nodeTransform.
// Static meshes only — skinning is applied separately. A node-less / single-node
// model still works (identity nodeTransform). Returns empty for headless models.
std::vector<ModelDrawable> makeDrawables(const Model& m);

// Column-major 4x4 multiply helper: out = a * b (glTF/glm convention). Exposed so
// callers can compose objectTransform (a) with a drawable's nodeTransform (b)
// before handing the result to IRenderDevice::drawMesh(). out may NOT alias a/b.
void mulMat4(const float a[16], const float b[16], float out[16]);

// Decode a primitive's opaque vertexBuffer handle to the device MeshHandle id it
// was uploaded to (0 if it carries no real device mesh — the headless seam). The
// CPU-skinning runtime uses this to target IRenderDevice::updateMesh(). Mirrors the
// id makeDrawables() puts in ModelDrawable::meshId.
uint32_t meshIdOf(const MeshPrimitive& p);

// Factory. `dev` may be null (headless path): in that case opaque GPU handles
// are minted as monotonic non-zero fake IDs and no real upload is performed,
// so the loader can be exercised without a Vulkan device.
IModelLoader* createModelLoader(rhi::IRenderDevice* dev, IAssetSource* assets);

// Runs the M2 acceptance tests in-process (synthesizes a cube GLB in memory,
// checks PBR/material/missing-texture/unload paths, and — when a GLB corpus is
// found on disk — runs the batch import). Returns true if the core tests pass.
bool runModelLoaderSelfTest();

} // namespace x3::asset
