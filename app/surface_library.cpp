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
#include <cmath>
#include <string>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace x3::game {

namespace {

// ---- Background decode cache (prewarmSurfaceSetsAsync). Keyed by full PNG
// path; each entry's future produces the raw RGBA pixels so get() only pays
// the GPU createTexture. Guarded by one mutex (a handful of entries, boot-only).
struct DecodedPng {
    std::unique_ptr<stbi_uc, void(*)(void*)> px{ nullptr, stbi_image_free };
    int w = 0, h = 0;
};
std::mutex g_decodeMx;
std::unordered_map<std::string, std::shared_future<std::shared_ptr<DecodedPng>>> g_decodes;

std::shared_ptr<DecodedPng> decodePng(const std::string& path) {
    auto d = std::make_shared<DecodedPng>();
    int comp = 0;
    d->px.reset(stbi_load(path.c_str(), &d->w, &d->h, &comp, 4));
    return d->px ? d : nullptr;
}

// Mean LINEAR luminance of an sRGB RGBA8 buffer. Subsampled on an 8x8 grid — a 4096^2
// albedo is 16.7M texels and we only need a stable mean, not an exact one (262k samples).
float meanLinearLuma(const unsigned char* px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return 0.0f;
    auto s2l = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    double acc = 0.0; long n = 0;
    for (int y = 0; y < h; y += 8) {
        for (int x = 0; x < w; x += 8) {
            const unsigned char* t = px + ((size_t)y * w + x) * 4;
            const float r = s2l(t[0] / 255.0f), g = s2l(t[1] / 255.0f), bl = s2l(t[2] / 255.0f);
            acc += 0.2126 * r + 0.7152 * g + 0.0722 * bl;
            ++n;
        }
    }
    return n ? (float)(acc / n) : 0.0f;
}

x3::rhi::TextureHandle loadPng(x3::rhi::IRenderDevice& device,
                               const std::string& path, bool srgb,
                               float* outMeanLin = nullptr) {
    // Prewarmed? Consume the background decode (wait if still in flight — it
    // started seconds ago, so this is a cache hit in practice) and drop the entry.
    std::shared_future<std::shared_ptr<DecodedPng>> fut;
    {
        std::lock_guard<std::mutex> lk(g_decodeMx);
        auto it = g_decodes.find(path);
        if (it != g_decodes.end()) { fut = it->second; g_decodes.erase(it); }
    }
    if (fut.valid()) {
        if (std::shared_ptr<DecodedPng> d = fut.get()) {
            if (outMeanLin) *outMeanLin = meanLinearLuma(d->px.get(), d->w, d->h);
            return device.createTexture(d->px.get(), (uint32_t)d->w, (uint32_t)d->h, srgb);
        }
        // decode failed — fall through to the inline path (which will also fail,
        // but logs via the caller's incomplete-set warning as before)
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) return {};
    if (outMeanLin) *outMeanLin = meanLinearLuma(px, w, h);
    x3::rhi::TextureHandle t = device.createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
    stbi_image_free(px);
    return t;
}

} // namespace

void prewarmSurfaceSetsAsync(const std::string& rootDir,
                             const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lk(g_decodeMx);
    for (const std::string& n : names) {
        for (const char* f : { "albedo.png", "normal.png", "mr.png" }) {
            std::string path = rootDir + "/" + n + "/" + f;
            if (g_decodes.count(path)) continue;
            g_decodes.emplace(std::move(path),
                std::async(std::launch::async, [p = rootDir + "/" + n + "/" + f]() {
                    return decodePng(p);
                }).share());
        }
    }
}

std::vector<uint8_t> decodePngRGBA8(const std::string& path, int& outW, int& outH) {
    outW = outH = 0;
    int comp = 0, w = 0, h = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) return {};
    std::vector<uint8_t> out(px, px + (size_t)w * (size_t)h * 4u);
    stbi_image_free(px);
    outW = w; outH = h;
    return out;
}

const SurfaceSet& SurfaceLibrary::get(x3::rhi::IRenderDevice& device,
                                      const std::string& name) {
    auto it = m_cache.find(name);
    if (it != m_cache.end()) return it->second;
    SurfaceSet s;
    const std::string d = m_root + "/" + name + "/";
    s.albedo = loadPng(device, d + "albedo.png", true, &s.albedoLin);
    s.normal = loadPng(device, d + "normal.png", false);
    s.mr     = loadPng(device, d + "mr.png",     false);
    s.ok = s.albedo.valid() && s.normal.valid() && s.mr.valid();
    if (!s.ok)
        x3::logWarn("[surface-lib] set '" + name + "' incomplete under " + m_root);
    else {
        const float t = s.valueTint();
        if (t < 0.999f || t > 1.001f)
            x3::logInfo("[surface-lib] '" + name + "' albedo " +
                        std::to_string(s.albedoLin) + " linear is OUT OF BAND -> value tint x" +
                        std::to_string(t) + " (VALUE, NOT LUMENS)");
    }
    return m_cache.emplace(name, s).first->second;
}

bool SurfaceLibrary::ownsTexture(uint32_t id) const {
    for (const auto& kv : m_cache) {
        const SurfaceSet& s = kv.second;
        if ((s.albedo.valid() && s.albedo.id == id) ||
            (s.normal.valid() && s.normal.id == id) ||
            (s.mr.valid()     && s.mr.id     == id)) return true;
    }
    return false;
}

void SurfaceLibrary::destroyAll(x3::rhi::IRenderDevice& device) {
    for (auto& kv : m_cache) {
        SurfaceSet& s = kv.second;
        if (s.albedo.valid()) device.destroyTexture(s.albedo);
        if (s.normal.valid()) device.destroyTexture(s.normal);
        if (s.mr.valid())     device.destroyTexture(s.mr);
        s = SurfaceSet{};
    }
    m_cache.clear();
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
    // Hue-preserving VALUE normalization (see SurfaceSet::valueTint). An in-band surface
    // returns 1.0 here and draws exactly as it always did.
    const float vt      = set.valueTint();
    const float bc[4]   = { vt, vt, vt, 1.0f };
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
