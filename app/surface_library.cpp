// See surface_library.h. PNG decode via stb_image (same precedent as
// cinematic.cpp's planet loader); textures through the plain createTexture
// path (albedo sRGB, normal/mr linear) so the batch-upload window applies.
#include "surface_library.h"

#include "asset_root.h"
#include "engine/core/x3_log.h"

// File-local STB copy, exactly like cinematic.cpp: the engine's implementation
// is file-local to ModelLoader.cpp, so each app TU that decodes PNGs hosts its
// own STATIC instance (no symbol clash).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace x3::game {

namespace {

x3::rhi::TextureHandle loadPng(x3::rhi::IRenderDevice& device,
                               const std::string& path, bool srgb) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) return {};
    x3::rhi::TextureHandle t = device.createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
    stbi_image_free(px);
    return t;
}

} // namespace

const SurfaceSet& SurfaceLibrary::get(x3::rhi::IRenderDevice& device,
                                      const std::string& name) {
    auto it = m_cache.find(name);
    if (it != m_cache.end()) return it->second;
    SurfaceSet s;
    const std::string d = m_root + "/" + name + "/";
    s.albedo = loadPng(device, d + "albedo.png", true);
    s.normal = loadPng(device, d + "normal.png", false);
    s.mr     = loadPng(device, d + "mr.png",     false);
    s.ok = s.albedo.valid() && s.normal.valid() && s.mr.valid();
    if (!s.ok)
        x3::logWarn("[surface-lib] set '" + name + "' incomplete under " + m_root);
    return m_cache.emplace(name, s).first->second;
}

x3::rhi::MeshHandle SurfaceLibrary::makePanel(x3::rhi::IRenderDevice& device, int axis,
                                              float w, float h, float tile) const {
    const float uMax = w / std::max(tile, 0.01f);
    const float vMax = h / std::max(tile, 0.01f);
    x3::rhi::MeshVertex v[4]{};
    auto set = [&](int i, float a, float b, float u, float vv) {
        x3::rhi::MeshVertex& m = v[i];
        if (axis == 0) {        // wall in the XY plane, facing -Z
            m.pos[0] = a; m.pos[1] = b; m.pos[2] = 0.0f; m.normal[2] = -1.0f;
        } else if (axis == 1) { // floor in the XZ plane, facing +Y
            m.pos[0] = a; m.pos[1] = 0.0f; m.pos[2] = b; m.normal[1] = 1.0f;
        } else {                // wall in the ZY plane, facing +X
            m.pos[0] = 0.0f; m.pos[1] = b; m.pos[2] = a; m.normal[0] = 1.0f;
        }
        m.uv[0] = u; m.uv[1] = vv;
    };
    set(0, -w * 0.5f, 0.0f, 0.0f, vMax);
    set(1,  w * 0.5f, 0.0f, uMax, vMax);
    set(2,  w * 0.5f, h,    uMax, 0.0f);
    set(3, -w * 0.5f, h,    0.0f, 0.0f);
    // Wind both ways so panels read regardless of the viewer side (cheap; these
    // are a handful of quads per room, not a perf surface).
    const uint32_t idx[12] = { 0,1,2, 0,2,3, 0,2,1, 0,3,2 };
    return device.createMesh(v, 4, idx, 12);
}

void SurfaceLibrary::drawPanel(x3::rhi::IRenderDevice& device,
                               const x3::rhi::FrameContext& frame,
                               const SurfaceSet& set, x3::rhi::MeshHandle mesh,
                               const float transform[16]) const {
    const float bc[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float emis[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    device.drawMeshPBR(frame, mesh, set.albedo, set.normal, set.mr, bc, emis,
                       transform, /*alphaMask=*/false, /*alphaBlend=*/false,
                       x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                       /*detailUvScale=*/1.0f, /*clearcoat=*/0.0f,
                       /*clearcoatRough=*/0.0f);
}

// ---------------------------------------------------------------------------
// --screenshot-matlib host
// ---------------------------------------------------------------------------
int runMatlibShot(x3::rhi::IRenderDevice& device, const std::string& outDir) {
    namespace fs = std::filesystem;
    SurfaceLibrary lib;
    const std::string root = assetRoot() + "/surface_library";
    lib.mount(root);

    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory()) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        x3::logError("[matlib] no sets under " + root);
        return 1;
    }
    fs::create_directories(outDir, ec);
    x3::logInfo("[matlib] " + std::to_string(names.size()) + " sets -> " + outDir);

    // One bay per set along +X. REVIEW GEOMETRY: exactly ONE texture repeat per
    // panel (2.5 m panel / 2.5 m tile) — several curated sets are FEATURE walls
    // (wainscot rows, trim bands) that are not authored to wrap; a 1:1 mapping
    // judges the material without wrap smearing. In-game tiling density is the
    // room recipe's decision, not the review rig's.
    device.beginUploadBatch();
    const x3::rhi::MeshHandle wall  = lib.makePanel(device, 0, 2.5f, 2.5f, 2.5f);
    const x3::rhi::MeshHandle floor = lib.makePanel(device, 1, 2.5f, 2.0f, 2.0f);
    struct Bay { const SurfaceSet* set; float x; };
    std::vector<Bay> bays;
    const float kPitch = 3.4f;
    for (size_t i = 0; i < names.size(); ++i)
        bays.push_back({ &lib.get(device, names[i]), (float)i * kPitch });

    // Neutral rig, re-positioned per shot: warm key + cool fill.
    auto rig = [&](float cx) {
        // Brighter than a game rig on purpose — this reviews ALBEDO/relief, and
        // round 1 at game levels buried every mid-dark set in shadow.
        x3::rhi::PointLight L[3]{};
        L[0].pos[0] = cx + 1.3f; L[0].pos[1] = 2.3f; L[0].pos[2] = -1.9f;
        L[0].range = 10.0f; L[0].color[0] = 4.4f; L[0].color[1] = 4.1f; L[0].color[2] = 3.6f;
        L[1].pos[0] = cx - 1.6f; L[1].pos[1] = 1.1f; L[1].pos[2] = -1.6f;
        L[1].range = 8.0f; L[1].color[0] = 1.3f; L[1].color[1] = 1.45f; L[1].color[2] = 1.8f;
        L[2].pos[0] = cx;        L[2].pos[1] = 1.4f; L[2].pos[2] = -2.8f;
        L[2].range = 7.0f; L[2].color[0] = 1.1f; L[2].color[1] = 1.1f; L[2].color[2] = 1.1f;
        device.setPointLights(L, 3);
    };

    auto renderShot = [&](float camX, float camY, float camZ, float fov,
                          const std::string& path) {
        const int kFrames = 36;
        rig(camX);
        for (int i = 0; i < kFrames; ++i) {
            // Face +Z (dir = (cos yaw, sin yaw) in XZ -> yaw = pi/2).
            device.setCamera(camX, camY, camZ, 1.57079633f, -0.06f, fov);
            // Contract (IRenderDevice.h): arm BEFORE the beginFrame of the frame
            // to grab, then captureFrame AFTER its endFrame writes the PNG.
            if (i == kFrames - 1) device.armCapture(path.c_str());
            auto frame = device.beginFrame();
            if (frame.valid) {
                for (const Bay& b : bays) {
                    float wallT[16]  = {1,0,0,0, 0,1,0,0, 0,0,1,0, b.x, 0, 0, 1};
                    float floorT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, b.x, 0,-2.0f, 1};
                    lib.drawPanel(device, frame, *b.set, wall,  wallT);
                    lib.drawPanel(device, frame, *b.set, floor, floorT);
                }
            }
            device.endFrame(frame);
        }
        if (!device.captureFrame(path.c_str()))
            x3::logWarn("[matlib] capture FAILED: " + path);
    };

    // Per-set closeups (the review medium) + two overview rows.
    for (size_t i = 0; i < names.size(); ++i)
        renderShot(bays[i].x, 1.5f, -3.1f, 55.0f, outDir + "/" + names[i] + ".png");
    const float mid1 = bays[std::min<size_t>(4, bays.size() - 1)].x + kPitch * 0.5f;
    renderShot(mid1, 1.7f, -13.0f, 70.0f, outDir + "/_row1_overview.png");
    if (bays.size() > 10) {
        const float mid2 = bays[std::min<size_t>(14, bays.size() - 1)].x + kPitch * 0.5f;
        renderShot(mid2, 1.7f, -13.0f, 70.0f, outDir + "/_row2_overview.png");
    }
    x3::logInfo("[matlib] wrote " + std::to_string(names.size() + 2) + " shots");
    return 0;
}

} // namespace x3::game
