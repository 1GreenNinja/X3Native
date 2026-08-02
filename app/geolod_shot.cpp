// ===========================================================================
// geolod_shot.cpp — --screenshot-geolod, the DISCRETE MESH LOD proof capture.
//
// See geolod_shot.h. CLEAN-ROOM, original work; no GPL / id Tech / RBDOOM /
// Unreal source consulted.
//
// The rig is deliberately a REAL-ART scene, not a box farm: LOD pop is a
// perceptual claim, and a box has nothing to pop. Geometry comes from the same
// .glb files the engine loads at runtime, re-read on the CPU (app/glb_cpu_read.h)
// so it can be decimated; textures come from the same files' embedded images.
//
// Everything the numbers rest on is measured, not asserted:
//   * triangles = x3::rhi::RenderStats::triangles (what the renderer submitted)
//   * per-level histogram + CPU triangle roll-up = Scene::lodStats()
//   * frame time = RenderStats::gpuFrameMs, averaged over a settled window
// ===========================================================================
#include "geolod_shot.h"

#include "glb_cpu_read.h"
#include "lod_chain.h"
#include "mesh_lod.h"
#include "mesh_prims.h"
#include "scene.h"
#include "asset_root.h"
#include "app_run.h"          // registerViewmodelCVarsForTest / applyRtaoCVarsForTest
#include "engine/core/IConsole.h"
#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"

#include <GLFW/glfw3.h>

// stb_image — file-local copy (the screenshot_hosts.cpp recipe). Needed to
// decode the GLB's embedded base-colour images out of memory.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457 4459)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <memory>
#include <vector>

namespace x3::game {
namespace {

using namespace x3::rhi;

std::string fmt(double v, int dp = 2) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
    return buf;
}

std::string withCommas(uint64_t n) {
    std::string s = std::to_string(n), out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        out.push_back(s[(size_t)i]);
        if (++c % 3 == 0 && i > 0) out.push_back(',');
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ---- One art asset, decimated into a chain and ready to instance -----------
struct Piece {
    MeshLodChain  chain;
    TextureHandle tex{};
    float         radius = 1.0f;    // model-space bounding radius (for placement)
    float         minY = 0.0f;      // so instances can be seated on the ground
    std::string   name;
};

TextureHandle uploadGlbImage(IRenderDevice& d, const GlbImage& img) {
    if (img.bytes.empty()) return {};
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(img.bytes.data(), (int)img.bytes.size(), &w, &h, &comp, 4);
    if (!px) return {};
    // Downsize very large atlases: the capture is 1280x720 and a 4K albedo per
    // piece costs seconds of upload for no visible difference here.
    TextureHandle t{};
    if (w > 1024 || h > 1024) {
        const int sx = std::max(1, w / 1024), sy = std::max(1, h / 1024);
        const int nw = std::max(1, w / sx), nh = std::max(1, h / sy);
        std::vector<uint8_t> small((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                    small[((size_t)y * nw + x) * 4 + c] = px[((size_t)(y * sy) * w + (x * sx)) * 4 + c];
        t = d.createTexture(small.data(), (uint32_t)nw, (uint32_t)nh, /*srgb*/ true);
    } else {
        t = d.createTexture(px, (uint32_t)w, (uint32_t)h, /*srgb*/ true);
    }
    stbi_image_free(px);
    return t;
}

// Merge every primitive of a model that shares a base-colour image into ONE
// mesh, then decimate that into a chain. Merging matters: LOD is only
// interesting on a mesh with enough triangles to lose, and these kits split a
// single visual object across dozens of tiny primitives.
std::vector<Piece> piecesFromGlb(IRenderDevice& d, const std::string& path,
                                 uint32_t minTrisPerPiece) {
    std::vector<Piece> out;
    const GlbModel m = readGlbForLod(path, /*minTriangles*/ 32);
    if (!m.ok) {
        x3::logError("[geolod-shot] " + path + ": " + m.error);
        return out;
    }

    // Group primitives by base-colour image.
    std::vector<int> imgs;
    for (const GlbPrimitive& p : m.prims)
        if (std::find(imgs.begin(), imgs.end(), p.baseColorImage) == imgs.end())
            imgs.push_back(p.baseColorImage);

    for (int img : imgs) {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        std::string name;
        for (const GlbPrimitive& p : m.prims) {
            if (p.baseColorImage != img) continue;
            const uint32_t base = (uint32_t)verts.size();
            verts.insert(verts.end(), p.verts.begin(), p.verts.end());
            for (uint32_t i : p.idx) idx.push_back(base + i);
            if (name.empty()) name = p.name;
        }
        if (idx.size() / 3 < minTrisPerPiece) continue;

        Piece pc;
        pc.name = std::filesystem::path(path).stem().string() + (name.empty() ? "" : ("/" + name));
        LodChainStats st{};
        pc.chain = buildLodChain(d, verts.data(), (uint32_t)verts.size(),
                                 idx.data(), (uint32_t)idx.size(), 4, nullptr, &st);
        if (!pc.chain.valid()) continue;
        pc.radius = pc.chain.radius;
        pc.minY = verts[0].pos[1];
        for (const MeshVertex& v : verts) pc.minY = std::min(pc.minY, v.pos[1]);
        if (img >= 0 && img < (int)m.images.size()) pc.tex = uploadGlbImage(d, m.images[(size_t)img]);

        std::string line = "[geolod-shot] chain " + pc.name + " r=" + fmt(pc.radius) + " m:";
        for (uint32_t l = 0; l < st.levels; ++l)
            line += " LOD" + std::to_string(l) + " " + withCommas(st.triangles[l]) +
                    " tris (err " + fmt(st.error[l], 3) + " m)";
        line += "  [built in " + fmt(st.buildMs, 1) + " ms]";
        x3::logInfo(line);
        out.push_back(std::move(pc));
    }
    return out;
}

// Column-major TRS: uniform scale, yaw about Y, translation.
void makeXform(float out[16], float s, float yaw, float x, float y, float z) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    out[0] =  c * s; out[1] = 0;  out[2] = -sn * s; out[3] = 0;
    out[4] =  0;     out[5] = s;  out[6] = 0;       out[7] = 0;
    out[8] =  sn * s; out[9] = 0; out[10] = c * s;  out[11] = 0;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1.0f;
}

struct Capture {
    std::string label;
    float camX, camY, camZ, yaw, pitch, fov;
};

} // namespace

int runGeoLodShot(IRenderDevice& device, const std::string& outDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    x3::logInfo("--screenshot-geolod: discrete mesh LOD proof -> " + outDir);

    // ---- lighting: a plain sunny exterior, nothing that could mask geometry ----
    IRenderDevice::SkyParams sky{};
    sky.enabled = true;
    sky.sunDir[0] = 0.55f; sky.sunDir[1] = 0.62f; sky.sunDir[2] = 0.56f;
    sky.sunColor[0] = 1.0f; sky.sunColor[1] = 0.97f; sky.sunColor[2] = 0.92f;
    sky.sunIntensity = 1.0f; sky.haze = 0.0f; sky.exposure = 1.0f;
    device.setSkyParams(sky);
    device.setCameraFar(6000.0f);
    device.setAmbient(0.34f, 0.36f, 0.42f);

    device.beginUploadBatch();

    // ---- the art ------------------------------------------------------------
    // A LARGE structure (the "tower" class) and a SMALL vehicle (the "prop"
    // class). Having both is the point: screen-space error must treat them
    // differently at the same distance, and the capture should show that.
    const std::string root = assetRoot();
    std::vector<Piece> towers = piecesFromGlb(device, root + "/converted_glb/Undersea/abyssal_station.glb", 2000);
    std::vector<Piece> props  = piecesFromGlb(device, root + "/converted_glb/Vehicles/CTR.glb", 2000);
    if (towers.empty() && props.empty()) {
        x3::logError("--screenshot-geolod: no art loaded (assets missing?) — aborting");
        device.endUploadBatch();
        return 1;
    }
    if (towers.empty()) towers = props;
    if (props.empty())  props  = towers;

    // ---- ground -------------------------------------------------------------
    x3::prims::PrimMesh ground = x3::prims::makeBox(4000.0f, 2.0f, 4000.0f, 0.0f, -1.0f, 0.0f, 1.0f);
    MeshHandle groundMesh = device.createMesh(ground.verts.data(), (uint32_t)ground.verts.size(),
                                              ground.index.data(), (uint32_t)ground.index.size());
    auto groundTexels = x3::prims::makeCheckerRGBA(64, 16, 96, 100, 104, 82, 86, 90);
    TextureHandle groundTex = device.createTexture(groundTexels.data(), 64, 64, true);

    device.endUploadBatch();

    // ---- the scene: a 6x6 skyline of towers + a field of props ---------------
    Scene scene;
    std::vector<uint32_t> towerChain, propChain;
    for (const Piece& p : towers) towerChain.push_back(scene.addLodChain(p.chain));
    for (const Piece& p : props)  propChain.push_back(scene.addLodChain(p.chain));

    // ONE scale per model, from the union of its pieces' bounds — the pieces
    // share a model space (glb_cpu_read bakes the node hierarchy in), so scaling
    // each piece by its own radius would tear the model apart.
    auto modelRadius = [](const std::vector<Piece>& v) {
        float r = 0.0f;
        for (const Piece& p : v) r = std::max(r, p.radius);
        return std::max(r, 0.001f);
    };
    auto modelMinY = [](const std::vector<Piece>& v) {
        float y = 1e30f;
        for (const Piece& p : v) y = std::min(y, p.minY);
        return (y < 1e29f) ? y : 0.0f;
    };
    const float towerRadius = modelRadius(towers), towerMinY = modelMinY(towers);
    const float propRadius  = modelRadius(props),  propMinY  = modelMinY(props);

    {
        Entity g{};
        g.mesh = groundMesh; g.tex = groundTex;
        makeXform(g.transform, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        scene.add(g);
    }

    uint32_t seed = 0x5EED1234u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 8) & 0xFFFF) / 65535.0f; };

    // 36 towers on a 6x6 grid, ~90 m across, 220 m pitch, deterministic jitter so
    // the grid does not read as a lattice. Grid centre 900 m down +Z.
    const float towerScale = 45.0f / towerRadius;
    const float pitch = 220.0f;
    int towerCount = 0;
    for (int gz = 0; gz < 6; ++gz)
        for (int gx = 0; gx < 6; ++gx) {
            const float x = ((float)gx - 2.5f) * pitch + (rnd() - 0.5f) * 50.0f;
            const float z = ((float)gz - 2.5f) * pitch + (rnd() - 0.5f) * 50.0f + 900.0f;
            const float s = towerScale * (0.75f + rnd() * 0.6f);
            const float yaw = rnd() * 6.28318f;
            for (size_t pi = 0; pi < towers.size(); ++pi) {
                Entity e{};
                e.mesh = towers[pi].chain.mesh[0];
                e.tex  = towers[pi].tex;
                e.lodChain = towerChain[pi];
                makeXform(e.transform, s, yaw, x, -towerMinY * s, z);
                scene.add(e);
            }
            ++towerCount;
        }

    // 200 small props (2.4 m radius cars) across the plaza, PLUS a hero row right
    // in front of the near camera so the closest capture has a small object whose
    // silhouette a reader can check for pop.
    const float propScale = 2.4f / propRadius;
    int propCount = 0;
    auto addProp = [&](float x, float z, float yaw) {
        for (size_t pi = 0; pi < props.size(); ++pi) {
            Entity e{};
            e.mesh = props[pi].chain.mesh[0];
            e.tex  = props[pi].tex;
            e.lodChain = propChain[pi];
            makeXform(e.transform, propScale, yaw, x, -propMinY * propScale, z);
            scene.add(e);
        }
        ++propCount;
    };
    for (int i = -3; i <= 3; ++i) addProp((float)i * 9.0f, 120.0f + (float)std::abs(i) * 6.0f, 1.2f);
    for (int i = 0; i < 200; ++i)
        addProp((rnd() - 0.5f) * 1300.0f, (rnd() - 0.5f) * 1300.0f + 900.0f, rnd() * 6.28318f);

    // ---- three camera distances --------------------------------------------
    // All three look down +Z at the same skyline; only the eye moves back.
    const float kYawPlusZ = 1.57079633f;
    const Capture shots[3] = {
        { "near",  0.0f,  20.0f,  200.0f, kYawPlusZ,  0.02f, 70.0f },
        { "mid",   0.0f,  75.0f, -260.0f, kYawPlusZ, -0.02f, 70.0f },
        { "far",   0.0f, 190.0f, -900.0f, kYawPlusZ, -0.04f, 70.0f },
    };

    LodPolicy& pol = lodPolicy();
    pol.enabled = true;
    pol.pixelError = 1.5f;
    pol.hysteresis = 0.15f;

    struct Row {
        std::string label;
        uint64_t trisOff = 0, trisOn = 0;
        uint64_t cpuTrisOff = 0, cpuTrisOn = 0;
        uint32_t hist[kMaxLodLevels] = { 0, 0, 0, 0 };
        double   msOff = 0.0, msOn = 0.0;
        uint32_t drawsOff = 0, drawsOn = 0;
        double   diffMean = 0.0, psnr = 0.0;
        int      diffMax = 0;
        double   pctOver2 = 0.0, pctOver8 = 0.0, pctOver32 = 0.0;
    };
    std::vector<Row> rows;
    bool allOk = true;

    // Render `frames` frames at a camera and optionally capture the last one.
    auto run = [&](const Capture& c, bool lodOn, const std::string& file,
                   int frames, int warmup, double& outMs, uint64_t& outTris,
                   uint32_t& outDraws, LodFrameStats& outLod) {
        pol.enabled = lodOn;
        scene.resetLodState();          // no state bleed between the A and B pass
        double sum = 0.0; int n = 0;
        for (int i = 0; i < frames; ++i) {
            glfwPollEvents();
            device.setCamera(c.camX, c.camY, c.camZ, c.yaw, c.pitch, c.fov);
            if (!file.empty() && i == frames - 1) device.armCapture(file.c_str());
            auto frame = device.beginFrame();
            if (frame.valid) scene.render(device, frame);
            device.endFrame(frame);
            const RenderStats st = device.stats();
            if (i >= warmup) { sum += st.gpuFrameMs; ++n; }
            if (i == frames - 1) { outTris = st.triangles; outDraws = st.drawCalls; outLod = scene.lodStats(); }
        }
        outMs = (n > 0) ? sum / (double)n : 0.0;
        if (!file.empty()) {
            const bool ok = device.captureFrame(file.c_str());
            if (ok) x3::logInfo("[geolod-shot] wrote " + file);
            else  { x3::logError("[geolod-shot] capture FAILED for " + file); allOk = false; }
        }
    };

    for (const Capture& c : shots) {
        Row r; r.label = c.label;
        LodFrameStats lsOff{}, lsOn{};
        run(c, /*lodOn*/ false, outDir + "/" + c.label + "_lod_off.png", 100, 40,
            r.msOff, r.trisOff, r.drawsOff, lsOff);
        run(c, /*lodOn*/ true,  outDir + "/" + c.label + "_lod_on.png",  100, 40,
            r.msOn,  r.trisOn,  r.drawsOn,  lsOn);
        r.cpuTrisOff = lsOff.trisLod0;
        r.cpuTrisOn  = lsOn.trisSelected;
        for (uint32_t l = 0; l < kMaxLodLevels; ++l) r.hist[l] = lsOn.perLevel[l];
        rows.push_back(r);
    }

    // ---- how different do the two images actually LOOK? ---------------------
    // "No visible pop" is the claim the whole error metric exists to support, so
    // measure it instead of asserting it: read the A and B PNGs back and report
    // the per-pixel difference. A correct screen-space-error budget puts every
    // difference on a 1-2 px SILHOUETTE band; a wrong one moves whole surfaces,
    // which shows up immediately as a large mean and a big >32 population.
    auto diffStats = [](const std::string& fa, const std::string& fb, Row& r) {
        int wa = 0, ha = 0, ca = 0, wb = 0, hb = 0, cb = 0;
        stbi_uc* A = stbi_load(fa.c_str(), &wa, &ha, &ca, 3);
        stbi_uc* B = stbi_load(fb.c_str(), &wb, &hb, &cb, 3);
        if (!A || !B || wa != wb || ha != hb) { if (A) stbi_image_free(A); if (B) stbi_image_free(B); return; }
        const size_t n = (size_t)wa * ha;
        double sum = 0.0, sq = 0.0;
        int mx = 0; size_t over2 = 0, over8 = 0, over32 = 0;
        for (size_t i = 0; i < n; ++i) {
            int px = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs((int)A[i * 3 + c] - (int)B[i * 3 + c]);
                sum += d; sq += (double)d * d;
                px = std::max(px, d);
            }
            mx = std::max(mx, px);
            if (px > 2) ++over2;
            if (px > 8) ++over8;
            if (px > 32) ++over32;
        }
        r.diffMean = sum / (double)(n * 3);
        r.diffMax  = mx;
        const double mse = sq / (double)(n * 3);
        r.psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        r.pctOver2  = 100.0 * (double)over2  / (double)n;
        r.pctOver8  = 100.0 * (double)over8  / (double)n;
        r.pctOver32 = 100.0 * (double)over32 / (double)n;
        stbi_image_free(A); stbi_image_free(B);
    };
    for (Row& r : rows)
        diffStats(outDir + "/" + r.label + "_lod_off.png", outDir + "/" + r.label + "_lod_on.png", r);

    // ---- the numbers --------------------------------------------------------
    x3::logInfo("--screenshot-geolod: ============ RESULTS ============");
    for (const Row& r : rows) {
        const double triCut = (r.trisOff > 0) ? 100.0 * (1.0 - (double)r.trisOn / (double)r.trisOff) : 0.0;
        const double msCut  = (r.msOff > 0.0) ? 100.0 * (1.0 - r.msOn / r.msOff) : 0.0;
        x3::logInfo("[geolod-shot] " + r.label +
                    ": triangles  LOD off = " + withCommas(r.trisOff) +
                    "  LOD on = " + withCommas(r.trisOn) +
                    "  (-" + fmt(triCut, 1) + "%)");
        x3::logInfo("[geolod-shot] " + r.label +
                    ": gpu frame  LOD off = " + fmt(r.msOff, 3) + " ms" +
                    "  LOD on = " + fmt(r.msOn, 3) + " ms" +
                    "  (" + fmt(-msCut, 1) + "% => " + fmt(r.msOn - r.msOff, 3) + " ms)");
        x3::logInfo("[geolod-shot] " + r.label +
                    ": draws " + std::to_string(r.drawsOff) + " -> " + std::to_string(r.drawsOn) +
                    " | level histogram LOD0=" + std::to_string(r.hist[0]) +
                    " LOD1=" + std::to_string(r.hist[1]) +
                    " LOD2=" + std::to_string(r.hist[2]) +
                    " LOD3=" + std::to_string(r.hist[3]) +
                    " | chained-entity tris " + withCommas(r.cpuTrisOff) + " -> " + withCommas(r.cpuTrisOn));
        x3::logInfo("[geolod-shot] " + r.label +
                    ": A/B image  mean|d| = " + fmt(r.diffMean, 3) +
                    "  max|d| = " + std::to_string(r.diffMax) +
                    "  PSNR = " + fmt(r.psnr, 1) + " dB" +
                    "  pixels differing >2/>8/>32: " + fmt(r.pctOver2, 3) + "% / " +
                    fmt(r.pctOver8, 3) + "% / " + fmt(r.pctOver32, 3) + "%");
    }

    // ---- VERTEX COMPRESSION probe (Lane 5, piece 2) --------------------------
    // The colour pass above is triangle-setup bound, not vertex-FETCH bound, so a
    // narrower vertex shows up as nothing there. The case the narrowing is FOR is
    // the one where the same geometry is re-fetched several times per frame with
    // almost no shading attached: the depth pre-pass plus the four CSM cascades.
    // Measure that explicitly instead of quoting the colour pass and hoping.
    {
        std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
        x3::apphost::registerViewmodelCVarsForTest(*console);
        console->set("r_ssao", "0");
        console->set("r_ssgi", "0");
        console->set("r_taa",  "0");

        auto sweep = [&](const char* csm, bool lodOn, int frames, int warmup) {
            console->set("r_csm", csm);
            // Set r_meshlod through the CONSOLE, not lodPolicy() directly:
            // applyRtaoCVarsForTest re-reads the cvar every frame and would
            // otherwise stamp the policy straight back to the cvar's value.
            console->set("r_meshlod", lodOn ? "1" : "0");
            scene.resetLodState();
            double sum = 0.0; int n = 0;
            uint64_t tris = 0;
            for (int i = 0; i < frames; ++i) {
                glfwPollEvents();
                x3::apphost::applyRtaoCVarsForTest(*console, device);
                device.setCamera(shots[1].camX, shots[1].camY, shots[1].camZ,
                                 shots[1].yaw, shots[1].pitch, shots[1].fov);
                auto frame = device.beginFrame();
                if (frame.valid) scene.render(device, frame);
                device.endFrame(frame);
                const RenderStats st = device.stats();
                if (i >= warmup) { sum += st.gpuFrameMs; ++n; }
                tris = st.triangles;
            }
            const double ms = (n > 0) ? sum / (double)n : 0.0;
            x3::logInfo(std::string("[geolod-shot] VTXFMT PROBE  stride=") +
                        std::to_string(device.meshVertexStride()) + " B  r_csm=" + csm +
                        "  r_meshlod=" + (lodOn ? "1" : "0") +
                        "  tris=" + withCommas(tris) +
                        "  gpu = " + fmt(ms, 3) + " ms");
        };
        x3::logInfo("[geolod-shot] VTXFMT MEMORY  stride=" +
                    std::to_string(device.meshVertexStride()) + " B  mesh vertex buffers = " +
                    withCommas(device.meshVertexBytes()) + " bytes (" +
                    fmt((double)device.meshVertexBytes() / (1024.0 * 1024.0), 2) + " MB)");
        // LOD OFF is the interesting one: 21.6 M triangles fetched once for the
        // depth pre-pass, four more times for the cascades, once for colour.
        sweep("0", false, 140, 60);
        sweep("1", false, 140, 60);
        sweep("1", true,  140, 60);
        console->set("r_csm", "0");
    }

    pol.enabled = true;
    return allOk ? 0 : 1;
}

} // namespace x3::game
