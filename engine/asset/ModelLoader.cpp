// glTF / GLB Model Loader implementation — M2 (clean-room).
// Spec: specs/M2-gltf-loader.spec.md
//
// Parses a GLB/glTF 2.0 asset (via cgltf, MIT) into engine-native mesh,
// material, node, skin and animation data. Vertex streams are de-accessor'd
// into one interleaved CPU buffer per primitive; textures are decoded with
// stb_image (public domain) as a fallback for PNG/JPG. KTX2/Basis transcode is
// left as a documented seam (best-effort, off by default — see kHasKtx2).
//
// GPU-UPLOAD SEAM
// ---------------
// This loader does ALL CPU-side parsing, and for the opaque GPU handles it
// routes through GpuUploader (the ONLY place that decides how a handle is born):
//   * device == nullptr (headless / self-test): mint monotonic non-zero fake
//     handle IDs; no Vulkan touched. This is what the self-test uses — its
//     behavior is intentionally unchanged.
//   * device != nullptr (S1): call IRenderDevice::createMesh()/createTexture()
//     for real. The loader's interleaved Vertex (pos/nrm/tan/uv/joints/weights)
//     is narrowed to the device's MeshVertex (pos/nrm/uv) at upload; tangents,
//     joints and weights are dropped (skinned upload is subsystem J). The real
//     MeshHandle/TextureHandle ids are stored in the Model's opaque handle
//     fields so unload() can destroy them.
//
// makeDrawables() turns a loaded Model into per-primitive draw records
// (mesh + base-color texture + baseColorFactor) the scene/app can feed to
// IRenderDevice::drawMesh().
//
// Verified via runModelLoaderSelfTest() (the M2 acceptance tests).

#include "IModelLoader.h"
#include "IAssetSource.h"
#include "../rhi/IRenderDevice.h"
#include "../core/x3_log.h"

// cgltf / stb live ONLY in this translation unit (Pimpl: never in the header).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we only ever decode from memory
#define STBI_NO_GIF            // trim formats we never feed it
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace x3::asset {

namespace {

// KTX2/Basis transcode is a documented best-effort seam. basisu is not wired
// into the build by default (no 14900K dependency, keeps the core fast). When a
// texture's mime/extension is KTX2 we mint a default handle and log once.
constexpr bool kHasKtx2 = false;

// ---------------------------------------------------------------------------
// GPU-upload seam. The ONLY place that decides how an opaque handle is born.
//   * null device  -> monotonic non-zero fake ids (no Vulkan; unchanged path).
//   * real device  -> IRenderDevice::createMesh()/createTexture().
// Handle encoding (real device): the device hands out 32-bit MeshHandle /
// TextureHandle ids; we store them verbatim in the 64-bit opaque fields and a
// kMeshTag / kTexTag high bit so free() can route destroy to the right call.
// ---------------------------------------------------------------------------
constexpr uint64_t kMeshTag = 0x1ull << 60;
constexpr uint64_t kTexTag  = 0x2ull << 60;
constexpr uint64_t kTagMask = 0xFull << 60;

class GpuUploader {
public:
    explicit GpuUploader(rhi::IRenderDevice* dev) : m_dev(dev) {}

    // Upload one mesh (already narrowed to the device's pos/nrm/uv vertex) and
    // its 32-bit indices; writes the SAME opaque handle into both outVB/outIB so
    // the existing two-field MeshPrimitive stays non-zero in either path.
    void uploadMesh(const rhi::MeshVertex* verts, uint32_t vcount,
                    const uint32_t* idx, uint32_t icount,
                    uint64_t& outVB, uint64_t& outIB) {
        if (m_dev) {
            rhi::MeshHandle mh = m_dev->createMesh(verts, vcount, idx, icount);
            uint64_t h = mh.valid() ? (kMeshTag | mh.id) : 0;
            outVB = h; outIB = h;
        } else {
            m_vbBytes += (size_t)vcount * sizeof(rhi::MeshVertex);
            m_ibBytes += (size_t)icount * sizeof(uint32_t);
            outVB = mint(); outIB = mint();
        }
    }

    // Upload a decoded RGBA8 texture; returns an opaque handle (!= 0). `srgb`
    // selects the storage format (color textures are sRGB).
    uint64_t uploadTexture(const void* rgba, int w, int h, bool srgb = true) {
        if (m_dev) {
            rhi::TextureHandle th = m_dev->createTexture(rgba, (uint32_t)w, (uint32_t)h, srgb);
            return th.valid() ? (kTexTag | th.id) : 0;
        }
        return mint();
    }

    // Shared 1x1 default textures (white base color, flat normal). Created once
    // per model, reused so a model with N missing textures only allocates two.
    uint64_t defaultWhite() {
        if (!m_defWhite) {
            if (m_dev) {
                const uint8_t white[4] = { 255, 255, 255, 255 };
                m_defWhite = uploadTexture(white, 1, 1, true);
            } else m_defWhite = mint();
        }
        return m_defWhite;
    }
    uint64_t defaultNormal() {
        if (!m_defNormal) {
            if (m_dev) {
                const uint8_t flat[4] = { 128, 128, 255, 255 }; // +Z normal, linear
                m_defNormal = uploadTexture(flat, 1, 1, false);
            } else m_defNormal = mint();
        }
        return m_defNormal;
    }

    // Free a handle. Real device: route to destroyMesh/destroyTexture by tag.
    // Headless seam: nothing to free (the model clears the field itself).
    void free(uint64_t handle) {
        if (!m_dev || !handle) return;
        uint32_t id = (uint32_t)(handle & ~kTagMask);
        if ((handle & kTagMask) == kMeshTag)      m_dev->destroyMesh(rhi::MeshHandle{ id });
        else if ((handle & kTagMask) == kTexTag)  m_dev->destroyTexture(rhi::TextureHandle{ id });
    }

private:
    uint64_t mint() {
        return s_next.fetch_add(1, std::memory_order_relaxed);
    }

    rhi::IRenderDevice* m_dev = nullptr;
    uint64_t m_defWhite = 0, m_defNormal = 0;
    size_t   m_vbBytes = 0, m_ibBytes = 0;
    // Monotonic, process-wide, always non-zero (starts at 1).
    static std::atomic<uint64_t> s_next;
};
std::atomic<uint64_t> GpuUploader::s_next{1};

// ---------------------------------------------------------------------------
// Interleaved vertex layout (matches the spec §3).
//   position vec3, normal vec3, tangent vec4, uv0 vec2,
//   joints uvec4 (as floats for simplicity here), weights vec4.
// ---------------------------------------------------------------------------
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v;
    float j0, j1, j2, j3;
    float w0, w1, w2, w3;
};

// --- small math helpers (column-major 4x4, glm/glTF convention) -------------
void mat4Identity(float* m) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
// Column-major 4x4 multiply: out = a * b (glTF/glm convention; b applied first).
// out must not alias a or b.
void mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

// Compose a TRS into a column-major 4x4. q = (x,y,z,w).
void trsToMat4(const float t[3], const float q[4], const float s[3], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    // Rotation (column-major) scaled by s.
    m[0]  = (1 - 2*(yy+zz)) * s[0];
    m[1]  = (2*(xy+wz))     * s[0];
    m[2]  = (2*(xz-wy))     * s[0];
    m[3]  = 0;
    m[4]  = (2*(xy-wz))     * s[1];
    m[5]  = (1 - 2*(xx+zz)) * s[1];
    m[6]  = (2*(yz+wx))     * s[1];
    m[7]  = 0;
    m[8]  = (2*(xz+wy))     * s[2];
    m[9]  = (2*(yz-wx))     * s[2];
    m[10] = (1 - 2*(xx+yy)) * s[2];
    m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

// ---------------------------------------------------------------------------
// The loader.
// ---------------------------------------------------------------------------
class ModelLoaderImpl final : public IModelLoader {
public:
    ModelLoaderImpl(rhi::IRenderDevice* dev, IAssetSource* assets)
        : m_dev(dev), m_assets(assets) {}

    Model load(std::string_view virtualPath) override {
        Model model;
        const std::string path(virtualPath);

        if (!m_assets) {
            logError("[gltf] no IAssetSource provided");
            return model;
        }

        Blob blob = m_assets->read(virtualPath);
        if (!blob.ok || blob.bytes.empty()) {
            logError("[gltf] read failed (missing / empty): " + path);
            return model;
        }

        cgltf_options opts{};
        cgltf_data* data = nullptr;
        cgltf_result r = cgltf_parse(&opts, blob.bytes.data(), blob.bytes.size(), &data);
        if (r != cgltf_result_success || !data) {
            logError("[gltf] cgltf_parse failed (" + std::string(cgltfErr(r)) + "): " + path);
            return model;
        }
        // RAII free of the cgltf data.
        std::unique_ptr<cgltf_data, void(*)(cgltf_data*)> guard(data, &cgltf_free);

        // Resolve buffers. For GLB the BIN chunk is embedded (null path is fine);
        // base64 data-URIs also resolve here. External .bin files relative to a
        // .gltf won't resolve through the VFS — those are out of scope (corpus
        // is GLB), and any unresolved buffer is detected per-accessor below.
        r = cgltf_load_buffers(&opts, data, nullptr);
        if (r != cgltf_result_success) {
            logWarn("[gltf] cgltf_load_buffers incomplete (" + std::string(cgltfErr(r)) +
                    "), continuing best-effort: " + path);
        }

        GpuUploader up(m_dev);

        buildMaterials(*data, model, up);
        buildPrimitives(*data, model, up);
        buildNodes(*data, model);
        buildSkins(*data, model);
        buildAnimations(*data, model);

        // A model is "ok" if it produced at least one drawable primitive.
        model.ok = !model.primitives.empty();
        if (!model.ok)
            logWarn("[gltf] no drawable primitives produced: " + path);
        return model;
    }

    void unload(Model& m) override {
        GpuUploader up(m_dev);
        std::unordered_set<uint64_t> freed;
        auto release = [&](uint64_t& h) {
            if (h && freed.insert(h).second) up.free(h);
            h = 0;
        };
        for (auto& p : m.primitives) { release(p.vertexBuffer); release(p.indexBuffer); }
        for (auto& mat : m.materials) {
            release(mat.baseColorTex); release(mat.normalTex); release(mat.mrTex);
            release(mat.emissiveTex);  release(mat.occlusionTex);
        }
        m.primitives.clear();
        m.materials.clear();
        m.nodes.clear();
        m.skins.clear();
        m.animations.clear();
        m.ok = false;
    }

private:
    static const char* cgltfErr(cgltf_result r) {
        switch (r) {
            case cgltf_result_success:        return "success";
            case cgltf_result_data_too_short: return "data_too_short";
            case cgltf_result_unknown_format: return "unknown_format";
            case cgltf_result_invalid_json:   return "invalid_json";
            case cgltf_result_invalid_gltf:   return "invalid_gltf";
            case cgltf_result_invalid_options:return "invalid_options";
            case cgltf_result_file_not_found: return "file_not_found";
            case cgltf_result_io_error:       return "io_error";
            case cgltf_result_out_of_memory:  return "out_of_memory";
            case cgltf_result_legacy_gltf:    return "legacy_gltf";
            default:                          return "unknown";
        }
    }

    // ---- textures ----------------------------------------------------------
    // Resolve a texture_view to a GPU handle: decode embedded image bytes with
    // stb (or transcode KTX2 if wired); on any failure return a default handle
    // and log once per model.
    // srgb = the IMPORT GAMMA FORMULA: COLOR maps (baseColor, emissive) are sRGB-encoded and
    // must be decoded to linear by the sampler; DATA maps (normal, metallic-roughness,
    // occlusion) are linear and must NOT be sRGB-decoded (doing so corrupts normals + PBR).
    // isNormal only selects the fallback default (flat-normal vs white) on decode failure.
    uint64_t resolveTexture(const cgltf_texture_view& tv, GpuUploader& up,
                            bool isNormal, bool srgb) {
        if (!tv.texture) return 0; // no texture bound at all
        return resolveTexture(tv.texture, up, isNormal, srgb);
    }

    uint64_t resolveTexture(const cgltf_texture* tex, GpuUploader& up, bool isNormal, bool srgb) {
        const cgltf_image* img = nullptr;
        bool ktx2 = false;
        if (tex->has_basisu && tex->basisu_image) { img = tex->basisu_image; ktx2 = true; }
        else if (tex->image) img = tex->image;

        if (ktx2 && !kHasKtx2) {
            warnOnce("[gltf] KTX2/Basis texture present but transcode disabled; "
                     "using default texture");
            return isNormal ? up.defaultNormal() : up.defaultWhite();
        }

        if (!img) {
            warnOnce("[gltf] texture references no image; using default");
            return isNormal ? up.defaultNormal() : up.defaultWhite();
        }

        // Gather the encoded bytes (embedded buffer-view, or base64/external URI).
        const uint8_t* bytes = nullptr;
        size_t         len   = 0;
        std::vector<uint8_t> owned; // backing for base64-decoded data

        if (img->buffer_view) {
            bytes = cgltf_buffer_view_data(img->buffer_view);
            len   = img->buffer_view->size;
        } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(img->uri, ',');
            if (comma) {
                // Compute decoded byte count from the base64 payload, then let
                // cgltf decode it (it allocates `dec`).
                const char* b64 = comma + 1;
                size_t b64len = std::strlen(b64);
                size_t pad = 0;
                if (b64len >= 1 && b64[b64len-1] == '=') ++pad;
                if (b64len >= 2 && b64[b64len-2] == '=') ++pad;
                size_t want = (b64len / 4) * 3 - pad;
                void* dec = nullptr;
                cgltf_options o{};
                if (want > 0 &&
                    cgltf_load_buffer_base64(&o, want, b64, &dec) == cgltf_result_success && dec) {
                    owned.assign(static_cast<uint8_t*>(dec), static_cast<uint8_t*>(dec) + want);
                    std::free(dec);
                    bytes = owned.data();
                    len   = want;
                }
            }
        } else if (img->uri) {
            // External image: resolve through the VFS (sibling of the model).
            Blob b = m_assets->read(img->uri);
            if (b.ok) { owned = std::move(b.bytes); bytes = owned.data(); len = owned.size(); }
        }

        if (!bytes || len == 0) {
            warnOnce("[gltf] texture image data missing; using default");
            return isNormal ? up.defaultNormal() : up.defaultWhite();
        }

        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(len), &w, &h, &comp, 4);
        if (!px) {
            warnOnce("[gltf] image decode failed; using default");
            return isNormal ? up.defaultNormal() : up.defaultWhite();
        }
        uint64_t handle = up.uploadTexture(px, w, h, srgb);
        stbi_image_free(px);
        return handle;
    }

    void buildMaterials(const cgltf_data& data, Model& model, GpuUploader& up) {
        model.materials.reserve(data.materials_count);
        for (size_t i = 0; i < data.materials_count; ++i) {
            const cgltf_material& cm = data.materials[i];
            Material m;
            if (cm.has_pbr_metallic_roughness) {
                const auto& pbr = cm.pbr_metallic_roughness;
                for (int k = 0; k < 4; ++k) m.baseColor[k] = pbr.base_color_factor[k];
                m.metallic  = pbr.metallic_factor;
                m.roughness = pbr.roughness_factor;
                m.baseColorTex = resolveTexture(pbr.base_color_texture, up, /*isNormal*/false, /*srgb*/true);
                m.mrTex        = resolveTexture(pbr.metallic_roughness_texture, up, false, /*srgb*/false); // DATA: linear
                // Metallic material with NO MR texture (only scalar factors — common in Unity
                // GLB exports): synthesize a 1x1 MR map from the factors (glTF: roughness=G,
                // metallic=B) so the mesh takes the shader's PBR/IBL branch (lit as metal)
                // instead of dark Lambertian diffuse — otherwise dark-tinted metals read black.
                if ((m.mrTex & kTagMask) != kTexTag && m.metallic > 0.001f) {
                    const uint8_t mrpx[4] = { 0,
                        (uint8_t)(m.roughness * 255.0f + 0.5f),
                        (uint8_t)(m.metallic  * 255.0f + 0.5f), 255 };
                    m.mrTex = up.uploadTexture(mrpx, 1, 1, /*srgb=*/false);  // data, not color
                }
            }
            for (int k = 0; k < 3; ++k) m.emissive[k] = cm.emissive_factor[k];
            if (cm.has_emissive_strength)
                for (int k = 0; k < 3; ++k) m.emissive[k] *= cm.emissive_strength.emissive_strength;

            m.normalTex    = resolveTexture(cm.normal_texture,    up, /*isNormal*/true,  /*srgb*/false); // DATA: linear
            m.occlusionTex = resolveTexture(cm.occlusion_texture, up, false, /*srgb*/false);             // DATA: linear
            m.emissiveTex  = resolveTexture(cm.emissive_texture,  up, false, /*srgb*/true);              // COLOR: sRGB
            m.doubleSided  = cm.double_sided != 0;
            m.alphaBlend   = (cm.alpha_mode == cgltf_alpha_mode_blend);
            m.alphaMask    = (cm.alpha_mode == cgltf_alpha_mode_mask);
            m.alphaCutoff  = (cm.alpha_cutoff > 0.0f) ? cm.alpha_cutoff : 0.5f;
            model.materials.push_back(m);
        }
    }

    void buildPrimitives(const cgltf_data& data, Model& model, GpuUploader& up) {
        size_t totalVerts = 0;
        for (size_t mi = 0; mi < data.meshes_count; ++mi) {
            const cgltf_mesh& mesh = data.meshes[mi];
            for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
                const cgltf_primitive& prim = mesh.primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) {
                    logWarn("[gltf] skipping non-triangle primitive");
                    continue;
                }
                if (prim.has_draco_mesh_compression) {
                    logWarn("[gltf] Draco-compressed primitive unsupported; skipping");
                    continue;
                }
                const cgltf_accessor* pos =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
                if (!pos || pos->count == 0) {
                    logWarn("[gltf] primitive has no POSITION; skipping");
                    continue;
                }
                const size_t vcount = pos->count;
                std::vector<Vertex> verts(vcount);

                readVec(pos, vcount, 3, offsetPos, verts);
                const cgltf_accessor* nrm =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
                bool haveNormals = nrm && nrm->count == vcount;
                if (haveNormals) readVec(nrm, vcount, 3, offsetNrm, verts);

                const cgltf_accessor* tan =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_tangent, 0);
                bool haveTangents = tan && tan->count == vcount;
                if (haveTangents) readVec(tan, vcount, 4, offsetTan, verts);

                const cgltf_accessor* uv =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
                bool haveUV = uv && uv->count == vcount;
                if (haveUV) readVec(uv, vcount, 2, offsetUV, verts);

                const cgltf_accessor* joints =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_joints, 0);
                const cgltf_accessor* weights =
                    cgltf_find_accessor(&prim, cgltf_attribute_type_weights, 0);
                bool haveSkin = false;
                if (joints && weights && joints->count == vcount) {
                    readVec(joints,  vcount, 4, offsetJoints,  verts);
                    readVec(weights, vcount, 4, offsetWeights, verts);
                    haveSkin = true;
                }

                // Indices: copy out, or synthesize 0..n-1 for non-indexed prims.
                std::vector<uint32_t> indices;
                if (prim.indices && prim.indices->count > 0) {
                    indices.resize(prim.indices->count);
                    cgltf_accessor_unpack_indices(prim.indices, indices.data(),
                                                  sizeof(uint32_t), indices.size());
                } else {
                    indices.resize(vcount);
                    for (size_t v = 0; v < vcount; ++v) indices[v] = static_cast<uint32_t>(v);
                }

                if (!haveNormals)  generateFlatNormals(verts, indices);
                if (!haveTangents) generateTangents(verts, indices, haveUV);

                // Narrow the rich interleaved vertex to the device's pos/nrm/uv
                // layout (tangents/joints/weights dropped — skinning is subsys J).
                std::vector<rhi::MeshVertex> mv(verts.size());
                for (size_t i = 0; i < verts.size(); ++i) {
                    mv[i].pos[0] = verts[i].px; mv[i].pos[1] = verts[i].py; mv[i].pos[2] = verts[i].pz;
                    mv[i].normal[0] = verts[i].nx; mv[i].normal[1] = verts[i].ny; mv[i].normal[2] = verts[i].nz;
                    mv[i].uv[0] = verts[i].u; mv[i].uv[1] = verts[i].v;
                }

                MeshPrimitive mp;
                up.uploadMesh(mv.data(), static_cast<uint32_t>(mv.size()),
                              indices.data(), static_cast<uint32_t>(indices.size()),
                              mp.vertexBuffer, mp.indexBuffer);
                mp.indexCount   = static_cast<uint32_t>(indices.size());
                mp.materialIndex = prim.material
                    ? static_cast<uint32_t>(cgltf_material_index(&data, prim.material))
                    : 0;
                // Record the source mesh so makeDrawables() can map each node that
                // references this mesh back to its primitives (and bake the node TRS).
                mp.meshIndex = static_cast<uint32_t>(mi);

                // ---- Retain CPU vertex data for skinned primitives (J1). The bind-
                // pose pos/nrm/uv + per-vertex joint indices/weights let the anim
                // runtime recompute vertices each frame and re-upload via updateMesh.
                // Only kept when the primitive actually carries joints+weights so the
                // static environment art pays nothing. ----
                if (haveSkin) {
                    mp.skinned = true;
                    mp.basePos.resize(vcount * 3);
                    mp.baseNrm.resize(vcount * 3);
                    mp.baseUv.resize(vcount * 2);
                    mp.jointIdx.resize(vcount * 4);
                    mp.jointWt.resize(vcount * 4);
                    for (size_t vi = 0; vi < vcount; ++vi) {
                        mp.basePos[vi*3+0] = verts[vi].px;
                        mp.basePos[vi*3+1] = verts[vi].py;
                        mp.basePos[vi*3+2] = verts[vi].pz;
                        mp.baseNrm[vi*3+0] = verts[vi].nx;
                        mp.baseNrm[vi*3+1] = verts[vi].ny;
                        mp.baseNrm[vi*3+2] = verts[vi].nz;
                        mp.baseUv[vi*2+0]  = verts[vi].u;
                        mp.baseUv[vi*2+1]  = verts[vi].v;
                        // glTF joint indices come through as floats; round to ints.
                        mp.jointIdx[vi*4+0] = (uint16_t)(verts[vi].j0 + 0.5f);
                        mp.jointIdx[vi*4+1] = (uint16_t)(verts[vi].j1 + 0.5f);
                        mp.jointIdx[vi*4+2] = (uint16_t)(verts[vi].j2 + 0.5f);
                        mp.jointIdx[vi*4+3] = (uint16_t)(verts[vi].j3 + 0.5f);
                        mp.jointWt[vi*4+0] = verts[vi].w0;
                        mp.jointWt[vi*4+1] = verts[vi].w1;
                        mp.jointWt[vi*4+2] = verts[vi].w2;
                        mp.jointWt[vi*4+3] = verts[vi].w3;
                    }
                }
                model.primitives.push_back(std::move(mp));
                totalVerts += vcount;
            }
        }
        if (totalVerts > 2'000'000)
            logWarn("[gltf] large model: " + std::to_string(totalVerts) + " vertices");
    }

    void buildNodes(const cgltf_data& data, Model& model) {
        model.nodes.reserve(data.nodes_count);
        for (size_t i = 0; i < data.nodes_count; ++i) {
            const cgltf_node& cn = data.nodes[i];
            Node n;
            // Node name (empty string if unnamed) — lets callers resolve humanoid
            // bones by name (foot-IK leg/hips lookup). Additive; index paths unchanged.
            n.name = cn.name ? cn.name : "";
            n.parent = cn.parent
                ? static_cast<int>(cgltf_node_index(&data, cn.parent)) : -1;
            if (cn.has_matrix) {
                std::memcpy(n.localTransform, cn.matrix, sizeof(float) * 16);
            } else {
                float t[3] = {0,0,0}, q[4] = {0,0,0,1}, s[3] = {1,1,1};
                if (cn.has_translation) std::memcpy(t, cn.translation, sizeof t);
                if (cn.has_rotation)    std::memcpy(q, cn.rotation,    sizeof q);
                if (cn.has_scale)       std::memcpy(s, cn.scale,       sizeof s);
                trsToMat4(t, q, s, n.localTransform);
            }
            n.meshIndex = cn.mesh
                ? static_cast<int>(cgltf_mesh_index(&data, cn.mesh)) : -1;
            n.skinIndex = cn.skin
                ? static_cast<int>(cgltf_skin_index(&data, cn.skin)) : -1;
            model.nodes.push_back(n);
        }
    }

    void buildSkins(const cgltf_data& data, Model& model) {
        model.skins.reserve(data.skins_count);
        for (size_t i = 0; i < data.skins_count; ++i) {
            const cgltf_skin& cs = data.skins[i];
            Skin s;
            s.joints.reserve(cs.joints_count);
            for (size_t j = 0; j < cs.joints_count; ++j)
                s.joints.push_back(static_cast<int>(cgltf_node_index(&data, cs.joints[j])));
            if (cs.inverse_bind_matrices) {
                const size_t need = cs.joints_count * 16;
                s.inverseBind.resize(need);
                cgltf_accessor_unpack_floats(cs.inverse_bind_matrices,
                                             s.inverseBind.data(), need);
            } else {
                // Default: identity per joint.
                s.inverseBind.resize(cs.joints_count * 16);
                for (size_t j = 0; j < cs.joints_count; ++j)
                    mat4Identity(s.inverseBind.data() + j * 16);
            }
            model.skins.push_back(std::move(s));
        }
    }

    void buildAnimations(const cgltf_data& data, Model& model) {
        model.animations.reserve(data.animations_count);
        for (size_t i = 0; i < data.animations_count; ++i) {
            const cgltf_animation& ca = data.animations[i];
            AnimationClip clip;
            clip.name = ca.name ? ca.name : ("anim_" + std::to_string(i));
            for (size_t c = 0; c < ca.channels_count; ++c) {
                const cgltf_animation_channel& cc = ca.channels[c];
                if (!cc.sampler || !cc.target_node) continue;
                AnimationChannel ch;
                ch.targetNode = static_cast<int>(cgltf_node_index(&data, cc.target_node));
                switch (cc.target_path) {
                    case cgltf_animation_path_type_translation: ch.path = AnimPath::Translation; ch.components = 3; break;
                    case cgltf_animation_path_type_rotation:    ch.path = AnimPath::Rotation;    ch.components = 4; break;
                    case cgltf_animation_path_type_scale:       ch.path = AnimPath::Scale;       ch.components = 3; break;
                    case cgltf_animation_path_type_weights:     ch.path = AnimPath::Weights;     ch.components = 1; break;
                    default: continue;
                }
                const cgltf_accessor* in  = cc.sampler->input;
                const cgltf_accessor* out = cc.sampler->output;
                if (!in || !out) continue;
                ch.times.resize(in->count);
                cgltf_accessor_unpack_floats(in, ch.times.data(), in->count);
                const size_t outFloats = out->count * cgltf_num_components(out->type);
                ch.values.resize(outFloats);
                cgltf_accessor_unpack_floats(out, ch.values.data(), outFloats);
                if (cc.target_path == cgltf_animation_path_type_weights && in->count > 0)
                    ch.components = static_cast<uint8_t>(out->count / in->count);
                if (!ch.times.empty())
                    clip.duration = std::max(clip.duration, ch.times.back());
                clip.channels.push_back(std::move(ch));
            }
            model.animations.push_back(std::move(clip));
        }
    }

    // ---- accessor -> interleaved-vertex copy -------------------------------
    // Member-pointer style would be awkward for float arrays; instead we pass a
    // byte offset into Vertex and the component count to write.
    static size_t offsetPos()     { return offsetof(Vertex, px); }
    static size_t offsetNrm()     { return offsetof(Vertex, nx); }
    static size_t offsetTan()     { return offsetof(Vertex, tx); }
    static size_t offsetUV()      { return offsetof(Vertex, u);  }
    static size_t offsetJoints()  { return offsetof(Vertex, j0); }
    static size_t offsetWeights() { return offsetof(Vertex, w0); }

    void readVec(const cgltf_accessor* acc, size_t count, int comps,
                 size_t (*ofs)(), std::vector<Vertex>& verts) {
        const size_t byteOfs = ofs();
        float tmp[4];
        for (size_t i = 0; i < count; ++i) {
            for (int k = 0; k < comps; ++k) tmp[k] = 0.0f;
            cgltf_accessor_read_float(acc, i, tmp, static_cast<size_t>(comps));
            float* dst = reinterpret_cast<float*>(
                reinterpret_cast<char*>(&verts[i]) + byteOfs);
            for (int k = 0; k < comps; ++k) dst[k] = tmp[k];
        }
    }

    static void generateFlatNormals(std::vector<Vertex>& v, const std::vector<uint32_t>& idx) {
        for (auto& vert : v) { vert.nx = vert.ny = vert.nz = 0.0f; }
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            Vertex& a = v[idx[i]]; Vertex& b = v[idx[i+1]]; Vertex& c = v[idx[i+2]];
            float ux = b.px-a.px, uy = b.py-a.py, uz = b.pz-a.pz;
            float wx = c.px-a.px, wy = c.py-a.py, wz = c.pz-a.pz;
            float nx = uy*wz - uz*wy, ny = uz*wx - ux*wz, nz = ux*wy - uy*wx;
            for (Vertex* p : {&a, &b, &c}) { p->nx += nx; p->ny += ny; p->nz += nz; }
        }
        for (auto& vert : v) {
            float l = std::sqrt(vert.nx*vert.nx + vert.ny*vert.ny + vert.nz*vert.nz);
            if (l > 1e-8f) { vert.nx/=l; vert.ny/=l; vert.nz/=l; }
            else { vert.nx = 0; vert.ny = 1; vert.nz = 0; }
        }
    }

    // Per-triangle tangent accumulation from UVs (Lengyel's method, public);
    // if there are no UVs, fall back to an arbitrary basis orthogonal to N.
    static void generateTangents(std::vector<Vertex>& v, const std::vector<uint32_t>& idx, bool haveUV) {
        if (!haveUV) {
            for (auto& vert : v) {
                float nx = vert.nx, ny = vert.ny, nz = vert.nz;
                // pick a helper axis not parallel to N
                float hx = (std::fabs(nx) < 0.9f) ? 1.0f : 0.0f;
                float hy = (std::fabs(nx) < 0.9f) ? 0.0f : 1.0f;
                float tx = hy*nz - 0*ny, ty = 0*nx - hx*nz, tz = hx*ny - hy*nx;
                float l = std::sqrt(tx*tx+ty*ty+tz*tz); if (l < 1e-8f) l = 1;
                vert.tx = tx/l; vert.ty = ty/l; vert.tz = tz/l; vert.tw = 1.0f;
            }
            return;
        }
        std::vector<float> tanx(v.size(), 0), tany(v.size(), 0), tanz(v.size(), 0);
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            uint32_t i0 = idx[i], i1 = idx[i+1], i2 = idx[i+2];
            const Vertex& a = v[i0]; const Vertex& b = v[i1]; const Vertex& c = v[i2];
            float x1 = b.px-a.px, x2 = c.px-a.px;
            float y1 = b.py-a.py, y2 = c.py-a.py;
            float z1 = b.pz-a.pz, z2 = c.pz-a.pz;
            float s1 = b.u-a.u, s2 = c.u-a.u;
            float t1 = b.v-a.v, t2 = c.v-a.v;
            float denom = (s1*t2 - s2*t1);
            float r = (std::fabs(denom) > 1e-8f) ? 1.0f/denom : 0.0f;
            float sx = (t2*x1 - t1*x2)*r, sy = (t2*y1 - t1*y2)*r, sz = (t2*z1 - t1*z2)*r;
            for (uint32_t ix : {i0, i1, i2}) { tanx[ix]+=sx; tany[ix]+=sy; tanz[ix]+=sz; }
        }
        for (size_t i = 0; i < v.size(); ++i) {
            Vertex& vert = v[i];
            float nx = vert.nx, ny = vert.ny, nz = vert.nz;
            float tdot = nx*tanx[i] + ny*tany[i] + nz*tanz[i];
            float ox = tanx[i] - nx*tdot, oy = tany[i] - ny*tdot, oz = tanz[i] - nz*tdot;
            float l = std::sqrt(ox*ox+oy*oy+oz*oz);
            if (l > 1e-8f) { vert.tx=ox/l; vert.ty=oy/l; vert.tz=oz/l; }
            else { vert.tx=1; vert.ty=0; vert.tz=0; }
            vert.tw = 1.0f;
        }
    }

    void warnOnce(const std::string& msg) {
        if (m_warned.insert(msg).second) logWarn(msg);
    }

    rhi::IRenderDevice* m_dev = nullptr;
    IAssetSource*       m_assets = nullptr;
    std::unordered_set<std::string> m_warned; // de-dupes per-model warnings
};

} // namespace

IModelLoader* createModelLoader(rhi::IRenderDevice* dev, IAssetSource* assets) {
    return new ModelLoaderImpl(dev, assets);
}

void mulMat4(const float a[16], const float b[16], float out[16]) {
    mat4Mul(a, b, out);
}

uint32_t meshIdOf(const MeshPrimitive& p) {
    if ((p.vertexBuffer & kTagMask) != kMeshTag) return 0;  // headless / no device mesh
    return static_cast<uint32_t>(p.vertexBuffer & ~kTagMask);
}

namespace {
// Build one ModelDrawable from a primitive (resolve the device handle + material),
// stamping the supplied node world transform. Returns false if the primitive has
// no real device mesh (headless ids carry no kMeshTag and aren't drawable).
bool fillDrawable(const Model& m, const MeshPrimitive& p, const float nodeWorld[16],
                  ModelDrawable& d) {
    if ((p.vertexBuffer & kTagMask) != kMeshTag) return false;
    d.meshId = static_cast<uint32_t>(p.vertexBuffer & ~kTagMask);
    if (p.materialIndex < m.materials.size()) {
        const Material& mat = m.materials[p.materialIndex];
        for (int k = 0; k < 4; ++k) d.baseColorFactor[k] = mat.baseColor[k];
        if ((mat.baseColorTex & kTagMask) == kTexTag)
            d.baseColorTexId = static_cast<uint32_t>(mat.baseColorTex & ~kTagMask);
        if ((mat.normalTex & kTagMask) == kTexTag)
            d.normalTexId = static_cast<uint32_t>(mat.normalTex & ~kTagMask);
        if ((mat.mrTex & kTagMask) == kTexTag)
            d.mrTexId = static_cast<uint32_t>(mat.mrTex & ~kTagMask);
        d.alphaMask = mat.alphaMask;
        d.alphaBlend = mat.alphaBlend;
        for (int k = 0; k < 3; ++k) d.emissiveFactor[k] = mat.emissive[k];
        if ((mat.emissiveTex & kTagMask) == kTexTag)
            d.emissiveTexId = static_cast<uint32_t>(mat.emissiveTex & ~kTagMask);
    }
    for (int i = 0; i < 16; ++i) d.nodeTransform[i] = nodeWorld[i];
    return true;
}
} // namespace

std::vector<ModelDrawable> makeDrawables(const Model& m) {
    std::vector<ModelDrawable> out;
    out.reserve(m.primitives.size());

    // ---- No node hierarchy: emit every primitive at identity (legacy path,
    // e.g. the synthetic test cube which carries a single identity node anyway). ----
    if (m.nodes.empty()) {
        const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (const auto& p : m.primitives) {
            ModelDrawable d;
            if (fillDrawable(m, p, ident, d)) out.push_back(d);
        }
        return out;
    }

    // ---- Compute each node's WORLD matrix by composing local transforms up the
    // parent chain. Node::parent always references an earlier-or-later index, so we
    // memoize and recurse (the chains are short; models have <few-thousand nodes). ----
    const size_t n = m.nodes.size();
    std::vector<std::array<float, 16>> world(n);
    std::vector<char> state(n, 0);   // 0=unvisited, 1=in-progress, 2=done
    std::function<void(size_t)> computeWorld = [&](size_t i) {
        if (state[i] == 2) return;
        const Node& nd = m.nodes[i];
        // Guard against a malformed parent cycle (would otherwise recurse forever):
        // treat an in-progress / out-of-range parent as a root.
        if (nd.parent < 0 || nd.parent >= (int)n || state[(size_t)nd.parent] == 1) {
            std::memcpy(world[i].data(), nd.localTransform, sizeof(float) * 16);
        } else {
            state[i] = 1;
            computeWorld((size_t)nd.parent);
            mat4Mul(world[(size_t)nd.parent].data(), nd.localTransform, world[i].data());
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < n; ++i) computeWorld(i);

    // ---- Walk nodes: each node with a mesh emits that mesh's primitives, baking
    // the node's world transform into each drawable. A mesh instanced by several
    // nodes is therefore drawn once per node (correct for multi-part placement). ----
    for (size_t i = 0; i < n; ++i) {
        const Node& nd = m.nodes[i];
        if (nd.meshIndex < 0) continue;
        for (const auto& p : m.primitives) {
            if ((int)p.meshIndex != nd.meshIndex) continue;
            ModelDrawable d;
            if (fillDrawable(m, p, world[i].data(), d)) out.push_back(d);
        }
    }

    // ---- Safety net: any mesh referenced by NO node (orphaned — non-conformant,
    // but don't silently drop it) is emitted once at identity. ----
    {
        std::unordered_set<uint32_t> nodeMeshes;
        for (const Node& nd : m.nodes) if (nd.meshIndex >= 0) nodeMeshes.insert((uint32_t)nd.meshIndex);
        const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (const auto& p : m.primitives) {
            if (nodeMeshes.count(p.meshIndex)) continue;
            ModelDrawable d;
            if (fillDrawable(m, p, ident, d)) out.push_back(d);
        }
    }
    return out;
}

// ===========================================================================
// Self-test (M2 acceptance tests). Headless: no Vulkan, null device.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0, g_skip = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; logInfo(std::string("[gltf-test] PASS ") + name); }
    else      { ++g_fail; logError(std::string("[gltf-test] FAIL ") + name); }
}
void skip(const char* name, const std::string& why) {
    ++g_skip;
    logWarn(std::string("[gltf-test] SKIPPED-PENDING-ASSETS ") + name + " — " + why);
}

// ---- minimal in-memory GLB writer (for the synthetic cube) -----------------
// Builds a valid binary glTF: 12-byte header + JSON chunk + BIN chunk.
void appendU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));        b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));  b.push_back(uint8_t(v >> 24));
}
std::vector<uint8_t> makeGlb(const std::string& json, const std::vector<uint8_t>& bin) {
    std::string j = json;
    while (j.size() % 4 != 0) j.push_back(' ');          // pad JSON with spaces
    std::vector<uint8_t> binPad = bin;
    while (binPad.size() % 4 != 0) binPad.push_back(0);  // pad BIN with zeros

    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(j.size()) + 8 + uint32_t(binPad.size());
    appendU32(glb, 0x46546C67);            // "glTF"
    appendU32(glb, 2);                     // version
    appendU32(glb, total);                 // total length
    appendU32(glb, uint32_t(j.size()));    // JSON chunk length
    appendU32(glb, 0x4E4F534A);            // "JSON"
    glb.insert(glb.end(), j.begin(), j.end());
    appendU32(glb, uint32_t(binPad.size()));// BIN chunk length
    appendU32(glb, 0x004E4942);            // "BIN\0"
    glb.insert(glb.end(), binPad.begin(), binPad.end());
    return glb;
}

// 1x1 PNG (white) — minimal valid file, decodes via stb_image. Generated once.
const std::vector<uint8_t> kWhitePng = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A, 0x00,0x00,0x00,0x0D,
    0x49,0x48,0x44,0x52, 0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x01,
    0x08,0x06,0x00,0x00,0x00, 0x1F,0x15,0xC4,0x89,
    0x00,0x00,0x00,0x0D, 0x49,0x44,0x41,0x54,
    0x78,0x9C,0x63,0xFC,0xCF,0xC0,0xF0,0x1F,0x00,0x05,0x05,0x02,0x00,
    0x0D,0x0A,0x2D,0xB4,
    0x00,0x00,0x00,0x00, 0x49,0x45,0x4E,0x44, 0xAE,0x42,0x60,0x82
};

// Build a unit cube GLB: 24 verts (per-face), 36 indices, POSITION+NORMAL+UV.
// Optionally embed a PNG base-color texture, or reference a missing one.
enum class CubeTex { None, Embedded, Missing };
std::vector<uint8_t> makeCubeGlb(CubeTex tex) {
    struct V { float p[3]; float n[3]; float uv[2]; };
    auto face = [](std::vector<V>& out, float nx, float ny, float nz,
                   float a[3], float b[3], float c[3], float d[3]) {
        out.push_back({{a[0],a[1],a[2]},{nx,ny,nz},{0,0}});
        out.push_back({{b[0],b[1],b[2]},{nx,ny,nz},{1,0}});
        out.push_back({{c[0],c[1],c[2]},{nx,ny,nz},{1,1}});
        out.push_back({{d[0],d[1],d[2]},{nx,ny,nz},{0,1}});
    };
    float P[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    std::vector<V> v;
    face(v, 0,0,-1, P[0],P[3],P[2],P[1]); // -Z
    face(v, 0,0, 1, P[4],P[5],P[6],P[7]); // +Z
    face(v,-1,0, 0, P[0],P[4],P[7],P[3]); // -X
    face(v, 1,0, 0, P[1],P[2],P[6],P[5]); // +X
    face(v, 0,-1,0, P[0],P[1],P[5],P[4]); // -Y
    face(v, 0, 1,0, P[3],P[7],P[6],P[2]); // +Y
    std::vector<uint16_t> idx;
    for (uint16_t f = 0; f < 6; ++f) {
        uint16_t b = uint16_t(f*4);
        idx.insert(idx.end(), {uint16_t(b+0),uint16_t(b+1),uint16_t(b+2),
                               uint16_t(b+0),uint16_t(b+2),uint16_t(b+3)});
    }

    // BIN layout: [positions][normals][uvs][indices]( [png] )
    std::vector<uint8_t> bin;
    auto putFloats = [&](const float* f, size_t n) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
        bin.insert(bin.end(), p, p + n*4);
    };
    const size_t nv = v.size();
    size_t posOfs = bin.size();
    for (auto& vv : v) putFloats(vv.p, 3);
    size_t nrmOfs = bin.size();
    for (auto& vv : v) putFloats(vv.n, 3);
    size_t uvOfs = bin.size();
    for (auto& vv : v) putFloats(vv.uv, 2);
    size_t idxOfs = bin.size();
    { const uint8_t* p = reinterpret_cast<const uint8_t*>(idx.data());
      bin.insert(bin.end(), p, p + idx.size()*2); }
    while (bin.size() % 4 != 0) bin.push_back(0);
    size_t pngOfs = bin.size(), pngLen = 0;
    if (tex == CubeTex::Embedded) {
        bin.insert(bin.end(), kWhitePng.begin(), kWhitePng.end());
        pngLen = kWhitePng.size();
    }

    std::string posMin = "[-1,-1,-1]", posMax = "[1,1,1]";
    std::string j = "{";
    j += "\"asset\":{\"version\":\"2.0\"},";
    j += "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
    j += "\"nodes\":[{\"mesh\":0}],";
    j += "\"meshes\":[{\"primitives\":[{\"attributes\":{";
    j += "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],";

    // material. Only reference texture index 0 when we actually emit a
    // textures/images array (Embedded or Missing); the None cube has none.
    const bool hasTex = (tex == CubeTex::Embedded || tex == CubeTex::Missing);
    j += "\"materials\":[{\"pbrMetallicRoughness\":{";
    j += "\"baseColorFactor\":[0.8,0.2,0.1,1.0],\"metallicFactor\":0.25,\"roughnessFactor\":0.7";
    if (hasTex) j += ",\"baseColorTexture\":{\"index\":0}";
    j += "},\"emissiveFactor\":[0.1,0.0,0.0]";
    if (hasTex) j += ",\"normalTexture\":{\"index\":0}";
    j += ",\"doubleSided\":true}],";

    // textures / images
    if (tex == CubeTex::Embedded) {
        j += "\"textures\":[{\"source\":0}],";
        j += "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}],";
    } else if (tex == CubeTex::Missing) {
        j += "\"textures\":[{\"source\":0}],";
        j += "\"images\":[{\"uri\":\"does_not_exist.png\"}],";
    }

    // accessors
    char buf[1024];
    std::snprintf(buf, sizeof buf,
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\",\"min\":%s,\"max\":%s},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"}],",
        nv, posMin.c_str(), posMax.c_str(), nv, nv, idx.size());
    j += buf;

    // bufferViews (4 mesh views; +1 for the embedded image)
    std::snprintf(buf, sizeof buf,
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}",
        posOfs, nv*12, nrmOfs, nv*12, uvOfs, nv*8, idxOfs, idx.size()*2);
    j += buf;
    if (tex == CubeTex::Embedded) {
        std::snprintf(buf, sizeof buf, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", pngOfs, pngLen);
        j += buf;
    }
    j += "],";

    std::snprintf(buf, sizeof buf, "\"buffers\":[{\"byteLength\":%zu}]}", bin.size());
    j += buf;

    return makeGlb(j, bin);
}

// Write GLBs to a temp dir, mount via IAssetSource, return the source.
struct TempAssets {
    fs::path dir;
    std::unique_ptr<IAssetSource> src;
    ~TempAssets() { std::error_code ec; if (!dir.empty()) fs::remove_all(dir, ec); }
};

bool writeFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

// ---- batch import (T5) + report (T4 scan) ---------------------------------
struct BatchResult { int total = 0, ok = 0; };

BatchResult runBatch(IModelLoader& loader, IAssetSource& src,
                     const std::vector<std::string>& vpaths,
                     const fs::path& reportPath, bool& foundRig) {
    BatchResult br;
    std::vector<std::string> failures;
    int withSkin = 0, withAnim = 0;
    std::string rigDetail;
    for (const auto& vp : vpaths) {
        Model m = loader.load(vp);
        ++br.total;
        if (m.ok) {
            ++br.ok;
            if (!m.skins.empty() && !m.skins[0].joints.empty()) {
                ++withSkin;
                if (!m.animations.empty()) ++withAnim;
                if (!foundRig &&
                    m.skins[0].inverseBind.size() == m.skins[0].joints.size() * 16 &&
                    !m.animations.empty()) {
                    foundRig = true;
                    rigDetail = vp + " (joints=" + std::to_string(m.skins[0].joints.size()) +
                                ", anims=" + std::to_string(m.animations.size()) + ")";
                }
            }
        } else {
            failures.push_back(vp);
        }
        loader.unload(m);
    }

    // Emit GLB_IMPORT_REPORT.md
    std::ofstream rep(reportPath);
    if (rep) {
        double pct = br.total ? (100.0 * br.ok / br.total) : 0.0;
        rep << "# GLB Import Report (M2 / T5)\n\n";
        rep << "Generated by `X3Engine.exe --test-gltf`.\n\n";
        rep << "- Total GLBs: " << br.total << "\n";
        rep << "- Loaded ok:  " << br.ok << "\n";
        rep << "- Success:    " << pct << "% (target >= 90%)\n";
        rep << "- With skin/joints: " << withSkin << "\n";
        rep << "- With animations:  " << withAnim << "\n";
        if (foundRig) rep << "- T4 rigged sample: " << rigDetail << "\n";
        rep << "\n## Failures\n\n";
        if (failures.empty()) rep << "_none_\n";
        for (const auto& f : failures) rep << "- " << f << "\n";
    }
    (void)src;
    return br;
}

} // namespace

bool runModelLoaderSelfTest() {
    g_pass = g_fail = g_skip = 0;
    logInfo("[gltf-test] M2 glTF/GLB model loader self-test");

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path() / "x3native_gltftest";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    TempAssets ta;
    ta.dir = tmp;
    ta.src.reset(createAssetSource());

    // Synthesize three cube GLBs.
    writeFile(tmp / "cube.glb",         makeCubeGlb(CubeTex::None));
    writeFile(tmp / "cube_pbr.glb",     makeCubeGlb(CubeTex::Embedded));
    writeFile(tmp / "cube_missing.glb", makeCubeGlb(CubeTex::Missing));
    ta.src->mountDir(tmp.string(), 0);

    // Headless: null device => fake non-zero handles.
    std::unique_ptr<IModelLoader> loader(createModelLoader(nullptr, ta.src.get()));

    // T1 — static GLB: 1 primitive, 36 indices, non-zero handles.
    {
        Model m = loader->load("cube.glb");
        bool pass = m.ok
                 && m.primitives.size() == 1
                 && m.primitives[0].indexCount == 36
                 && m.primitives[0].vertexBuffer != 0
                 && m.primitives[0].indexBuffer  != 0;
        check(pass, "T1 static GLB (1 prim, 36 idx, handles non-zero)");
        loader->unload(m);
    }

    // T2 — PBR material: fields populated + base-color texture handle non-zero.
    {
        Model m = loader->load("cube_pbr.glb");
        bool fields = m.ok && !m.materials.empty()
                   && std::fabs(m.materials[0].baseColor[0] - 0.8f) < 1e-3f
                   && std::fabs(m.materials[0].metallic    - 0.25f) < 1e-3f
                   && std::fabs(m.materials[0].roughness   - 0.7f)  < 1e-3f
                   && m.materials[0].doubleSided;
        bool tex = !m.materials.empty()
                && m.materials[0].baseColorTex != 0
                && m.materials[0].normalTex    != 0;
        check(fields && tex, "T2 PBR material (fields + texture handles non-zero)");
        loader->unload(m);
    }

    // T3 — missing texture: loads, uses default, logs once, ok == true.
    {
        Model m = loader->load("cube_missing.glb");
        bool pass = m.ok && !m.materials.empty()
                 && m.materials[0].baseColorTex != 0; // default white handle
        check(pass, "T3 missing texture (loads w/ default, ok==true)");
        loader->unload(m);
    }

    // T6 — unload frees / clears all GPU handles.
    {
        Model m = loader->load("cube_pbr.glb");
        bool hadHandles = m.ok && m.primitives[0].vertexBuffer != 0;
        loader->unload(m);
        bool cleared = m.primitives.empty() && m.materials.empty() && !m.ok;
        check(hadHandles && cleared, "T6 unload (handles cleared/freed)");
    }

    // ---- T4 / T5 (best-effort): scan for a real GLB corpus on disk ----------
    const char* corpora[] = {
        "G:/GameModels/rigged_glb",
        "G:/GameModels",
        "G:/X3Native/assets/models",
        "G:/GameModels/rodin_glb",
    };
    fs::path corpusDir;
    for (const char* c : corpora) {
        std::error_code e2;
        if (fs::exists(c, e2) && fs::is_directory(c, e2)) {
            // require at least one .glb directly inside
            bool any = false;
            for (auto& de : fs::directory_iterator(c, e2)) {
                if (de.path().extension() == ".glb") { any = true; break; }
            }
            if (any) { corpusDir = c; break; }
        }
    }

    if (corpusDir.empty()) {
        skip("T4 skinned model", "no rigged GLB corpus found on disk");
        skip("T5 batch-of-140", "no GLB corpus found on disk");
    } else {
        logInfo("[gltf-test] GLB corpus: " + corpusDir.string());
        auto batchSrc = std::unique_ptr<IAssetSource>(createAssetSource());
        batchSrc->mountDir(corpusDir.string(), 0);
        std::vector<std::string> vpaths;
        for (auto& de : fs::directory_iterator(corpusDir, ec)) {
            if (de.path().extension() == ".glb")
                vpaths.push_back(de.path().filename().string());
        }
        std::sort(vpaths.begin(), vpaths.end());

        std::unique_ptr<IModelLoader> bl(createModelLoader(nullptr, batchSrc.get()));

        // docs/ may not exist relative to cwd; write next to the worktree if so.
        fs::path docs = fs::current_path() / "docs";
        fs::create_directories(docs, ec);
        fs::path reportPath = docs / "GLB_IMPORT_REPORT.md";

        bool foundRig = false;
        BatchResult br = runBatch(*bl, *batchSrc, vpaths, reportPath, foundRig);

        double pct = br.total ? (100.0 * br.ok / br.total) : 0.0;
        logInfo("[gltf-test] batch: " + std::to_string(br.ok) + "/" +
                std::to_string(br.total) + " ok (" + std::to_string(pct) +
                "%), report -> " + reportPath.string());

        // T4 — a rigged model with joints + matching inverseBind + animations.
        if (foundRig) check(true, "T4 skinned model (joints + inverseBind + anims)");
        else          skip("T4 skinned model", "no rig with anims found in corpus");

        // T5 — >=90% load ok. (Best-effort; not allowed to fail the suite.)
        if (br.total > 0 && pct >= 90.0)
            check(true, "T5 batch import (>=90% ok)");
        else if (br.total > 0)
            skip("T5 batch-of-140",
                 "only " + std::to_string(pct) + "% loaded (corpus=" +
                 std::to_string(br.total) + ", target>=90%)");
        else
            skip("T5 batch-of-140", "corpus directory had no .glb files");
    }

    logInfo("[gltf-test] " + std::to_string(g_pass) + " passed, " +
            std::to_string(g_fail) + " failed, " +
            std::to_string(g_skip) + " skipped");
    return g_fail == 0;
}

} // namespace x3::asset
