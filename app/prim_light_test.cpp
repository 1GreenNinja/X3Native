// ===========================================================================
// prim_light_test.cpp — --test-primlight: ONE LIGHTING PATH. (KNOWN_BUGS R1)
//
// THE INVARIANT: a GRAYBOX PRIM surface and a GLB surface with the SAME albedo,
// the SAME geometry and the SAME light must receive the SAME radiance.
//
// The engine has two shading BRANCHES in mesh.frag, selected purely by whether a
// draw carries a metallic-roughness map:
//   * no MR map  -> the DIELECTRIC branch  (every graybox prim: Scene::render sends
//                   them through drawMeshEmissive)
//   * an MR map  -> the COOK-TORRANCE branch (every GLB: ModelLoader synthesizes an
//                   MR map, so env_art / room dressing draw them via drawMeshPBR)
// R1 (`5c35d65`) already found these two branches disagreeing by 0.318x-0.03x and
// "unified" them. NOTHING GUARDED THAT UNIFICATION. This test does.
//
// It renders THREE identical 2x2 m panels under THREE identical point lights, on the
// REAL Vulkan device, in ONE frame (so exposure/tonemap are provably identical for
// all three), then reads the captured PNG back and measures each panel:
//   [A] PRIM   — dielectric branch  (drawMeshEmissive, no MR map)
//   [B] GLB    — Cook-Torrance      (drawMeshPBR + a metallic=0 / rough=0.5 MR map,
//                                    exactly what ModelLoader hands every GLB)
//   [C] NEGATIVE CONTROL — the same prim with its NORMALS INVERTED: the geometry a
//       broken submit path produces. N.L is 0 for every light in the room, so it can
//       receive ONLY ambient. The probe MUST report it as failing. A gate that cannot
//       go red is worthless.
//
// Ambient is set to ZERO for the measurement, so every photon on those panels came
// from a POINT LIGHT. The sun is off (SkyParams::sunLight = 0). If a panel is black,
// it is not on the point-light path — there is nowhere else for the light to hide.
// ===========================================================================
#include "prim_light_test.h"

#include "mesh_prims.h"   // makeBox + the graybox wall-panel texture generator
#include "surface_library.h"      // decodePngRGBA8
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace x3::game {

namespace {

int pl_pass = 0, pl_fail = 0;

void plCheck(bool ok, const std::string& what) {
    if (ok) { ++pl_pass; x3::logInfo("  [PASS] " + what); }
    else    { ++pl_fail; x3::logError("  [FAIL] " + what); }
}

struct Rgb { double r = 0, g = 0, b = 0; };
inline double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// Mean sRGB of a square block of the captured image centered on (px, py).
Rgb sampleBlock(const std::vector<uint8_t>& px, int w, int h, int cx, int cy, int half) {
    Rgb sum; int n = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || x >= w) continue;
            const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4;
            sum.r += px[i]; sum.g += px[i + 1]; sum.b += px[i + 2];
            ++n;
        }
    }
    if (n == 0) return {};
    return { sum.r / n, sum.g / n, sum.b / n };
}

std::string fmt(double v, int dp = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
    return buf;
}
std::string rgbStr(const Rgb& c) {
    return "(" + fmt(c.r, 1) + ", " + fmt(c.g, 1) + ", " + fmt(c.b, 1) + ")";
}

// A 2 x 2 m panel standing in the XY plane, its lit face pointing at -Z (the camera).
// `flipNormals` builds the BROKEN geometry for the negative control: identical
// positions + winding, normals reversed.
x3::rhi::MeshHandle makeProbePanel(x3::rhi::IRenderDevice& device, float cx, bool flipNormals) {
    x3::prims::PrimMesh m = x3::prims::makeBox(1.0f, 1.0f, 0.1f, cx, 0.0f, 0.0f, 0.5f);
    if (flipNormals)
        for (auto& v : m.verts) { v.normal[0] = -v.normal[0]; v.normal[1] = -v.normal[1]; v.normal[2] = -v.normal[2]; }
    return device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                             m.index.data(), (uint32_t)m.index.size());
}

} // namespace

int runPrimLightTest(x3::rhi::IRenderDevice& device, const std::string& outPath) {
    pl_pass = pl_fail = 0;
    x3::logInfo("=== --test-primlight: ONE LIGHTING PATH (prim vs GLB radiance parity) ===");

    // ---- The rig. Ambient ZERO: every photon on the panels is a point-light photon.
    // BOTH ambients, or this test measures nothing: setAmbient() alone does NOT turn the
    // ambient off — an IBL environment is baked from the ANALYTIC SKY for every scene by
    // default, and iblAmbient()'s baked-env path never reads the setAmbient value. That
    // second, invisible, BLUE ambient is what made a windowless facility's walls read
    // cold under tungsten lamps. setIblIntensity(0) now means "this room has no
    // environment" and drops to the flat-ambient path (mesh.frag). The NEGATIVE CONTROL
    // below is the regression test for exactly that: with both dials at zero an unlit
    // surface MUST be black. If someone re-introduces a hidden ambient, it goes red.
    device.setAmbient(0.0f, 0.0f, 0.0f);
    device.setIblIntensity(0.0f);
    x3::rhi::IRenderDevice::SkyParams sky{};
    sky.enabled  = false;
    sky.sunLight = 0.0f;                       // no directional key: point lights ONLY
    device.setSkyParams(sky);
    device.setExposure(1.0f);

    device.beginUploadBatch();

    // ONE albedo for all three panels: a flat mid-grey (0.5 sRGB) 1x1 texture, and the
    // SAME baseColor factor. Identical material -> any radiance difference is the
    // SHADING PATH, not the art. (KNOWN_BUGS R1: "not albedo — a dark albedo would
    // still show the light's HUE".)
    const uint8_t grey[4] = { 128, 128, 128, 255 };
    const x3::rhi::TextureHandle albedoTex = device.createTexture(grey, 1, 1, /*srgb*/true);
    // The MR map every GLB gets from ModelLoader: glTF convention G = roughness,
    // B = metallic. metallic = 0 (a wall is not metal), roughness ~0.5 — which is
    // EXACTLY the dielectric branch's hardcoded kDielectricRough. Same material,
    // both branches.
    const uint8_t mr[4] = { 0, 128, 0, 255 };
    const x3::rhi::TextureHandle mrTex = device.createTexture(mr, 1, 1, /*srgb*/false);

    // The REAL graybox wall surface, on the same rig: level1/level_loader dress every
    // interior partition with makeSciFiPanelRGBA (a gunmetal panel, base texel
    // (78,84,94) sRGB = 0.077 linear). Measured beside the 0.216-linear grey it tells
    // us what a graybox wall is actually WORTH under a lamp — the "VALUE" half of
    // DECISIONS.md's order of operations.
    auto wallPx = x3::prims::makeSciFiPanelRGBA(256, 2, x3::prims::detail::kNoTint,
                                                60, 170, 200, 0.0f,
                                                x3::prims::WallVariant::Plain);
    const x3::rhi::TextureHandle wallTex = device.createTexture(wallPx.data(), 256, 256, /*srgb*/true);

    const float kPitch = 3.0f;
    struct Probe { const char* name; float x; x3::rhi::MeshHandle mesh; bool pbr; bool wall; };
    Probe probes[4] = {
        { "PRIM (dielectric branch)",  -kPitch * 1.5f, {}, false, false },
        { "GLB  (Cook-Torrance)",      -kPitch * 0.5f, {}, true,  false },
        { "NEGCTL (normals inverted)",  kPitch * 0.5f, {}, false, false },
        { "GRAYBOX WALL PANEL tex",     kPitch * 1.5f, {}, false, true  },
    };
    probes[0].mesh = makeProbePanel(device, probes[0].x, /*flip*/false);
    probes[1].mesh = makeProbePanel(device, probes[1].x, /*flip*/false);
    probes[2].mesh = makeProbePanel(device, probes[2].x, /*flip*/true);
    probes[3].mesh = makeProbePanel(device, probes[3].x, /*flip*/false);

    // One warm light per panel, at the SAME offset from each panel's center — so the
    // distance, the incidence angle and the colour are identical for all three. Warm
    // tungsten (the facility's fixtures) so the HUE is the tell, exactly as in the
    // field: a lit surface reads WARM (R > G > B); an unlit one cannot.
    const float kI = 4.0f;
    x3::rhi::PointLight lights[4]{};
    for (int i = 0; i < 4; ++i) {
        lights[i].pos[0] = probes[i].x;
        lights[i].pos[1] = 1.2f;
        lights[i].pos[2] = -2.0f;
        // RANGE 3.0 IS LOAD-BEARING: pointAtten hard-cuts at range, so each lamp lights
        // ONLY its own panel (2.33 m away) and contributes exactly ZERO to its
        // neighbours (3.8 m). Without it the middle panels catch more spill than the
        // outer ones and the rig itself manufactures a 13% "path split" — the test would
        // be measuring its own geometry. Every panel now sees an IDENTICAL lamp.
        lights[i].range  = 3.0f;
        lights[i].color[0] = 1.00f * kI;
        lights[i].color[1] = 0.86f * kI;
        lights[i].color[2] = 0.62f * kI;
    }
    device.setPointLights(lights, 4);

    // ---- Render. All three in ONE frame: identical exposure, identical tonemap.
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);

    const int kFrames = 24;                    // settle auto-exposure / TAA
    float sx[4] = {0,0,0,0}, sy[4] = {0,0,0,0};
    for (int f = 0; f < kFrames; ++f) {
        device.setCamera(0.0f, 0.0f, -9.5f, 1.57079633f /*+Z*/, 0.0f, 65.0f);
        if (f == kFrames - 1) device.armCapture(outPath.c_str());
        auto frame = device.beginFrame();
        if (frame.valid) {
            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const float noEmis[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (const Probe& p : probes) {
                const float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };  // world-baked verts
                const x3::rhi::TextureHandle tex = p.wall ? wallTex : albedoTex;
                if (p.pbr)
                    device.drawMeshPBR(frame, p.mesh, tex, x3::rhi::TextureHandle{}, mrTex,
                                       white, noEmis, T);
                else
                    device.drawMeshEmissive(frame, p.mesh, tex, white, noEmis, T);
            }
        }
        device.endFrame(frame);
        if (f == kFrames - 1)
            for (int i = 0; i < 4; ++i)
                device.worldToScreen(probes[i].x, 0.0f, -0.1f, sx[i], sy[i]);
    }
    if (!device.captureFrame(outPath.c_str())) {
        x3::logError("[primlight] capture FAILED: " + outPath);
        return 1;
    }
    x3::logInfo("[primlight] wrote " + outPath);

    // ---- Measure the captured PNG.
    int w = 0, h = 0;
    std::vector<uint8_t> px = decodePngRGBA8(outPath, w, h);
    if (px.empty() || w <= 0 || h <= 0) {
        x3::logError("[primlight] could not read back the capture");
        return 1;
    }
    Rgb c[4];
    for (int i = 0; i < 4; ++i) {
        const int cxp = (int)(sx[i] + 0.5f), cyp = (int)(sy[i] + 0.5f);
        c[i] = sampleBlock(px, w, h, cxp, cyp, 18);
        const bool warm = c[i].r > c[i].g && c[i].g > c[i].b;
        x3::logInfo("  " + std::string(probes[i].name) + " @px(" + std::to_string(cxp) + "," +
                    std::to_string(cyp) + ")  sRGB " + rgbStr(c[i]) +
                    "  luma " + fmt(luma(c[i])) + "  hue " + (warm ? "WARM" : "NOT-WARM"));
    }

    const double lPrim = luma(c[0]), lGlb = luma(c[1]), lNeg = luma(c[2]);

    // 1. Both real panels must actually be LIT. Ambient is zero, so a dark panel is a
    //    panel that is not on the point-light path.
    plCheck(lPrim > 20.0, "PRIM panel receives point light (luma " + fmt(lPrim) + " > 20)");
    plCheck(lGlb  > 20.0, "GLB  panel receives point light (luma " + fmt(lGlb) + " > 20)");

    // 2. THE INVARIANT. Same albedo, same geometry, same light -> same radiance.
    //    10% of the brighter panel: enough headroom for the GGX spec lobe's small
    //    contribution, nowhere near enough to hide a 0.318x path split (R1) or a
    //    "this surface gets no point light at all".
    const double denom = std::max(lPrim, lGlb);
    const double rel   = denom > 0.0 ? std::fabs(lPrim - lGlb) / denom : 1.0;
    plCheck(rel <= 0.10, "PRIM and GLB agree within 10% (prim " + fmt(lPrim) + " vs glb " +
                         fmt(lGlb) + " -> " + fmt(rel * 100.0, 1) + "% apart)");

    // 3. THE HUE IS THE TELL. A surface lit by a warm fixture reads warm.
    plCheck(c[0].r > c[0].g && c[0].g > c[0].b, "PRIM panel reads WARM " + rgbStr(c[0]));
    plCheck(c[1].r > c[1].g && c[1].g > c[1].b, "GLB  panel reads WARM " + rgbStr(c[1]));

    // 4. NEGATIVE CONTROL — the probe can go red, AND ambient-off means ambient-off.
    //    An inverted-normal panel has N.L <= 0 for every light in the room. With both
    //    ambient dials at zero it MUST be black, and the very comparisons above MUST
    //    fail for it. Two ways this goes red: someone breaks the parity detector (it
    //    would stop catching the split), or someone re-introduces a hidden ambient the
    //    host cannot turn off (the panel stops being black). Both are the bugs this
    //    file exists for. A gate that cannot fail is worthless.
    const double relNeg = denom > 0.0 ? std::fabs(lNeg - lGlb) / denom : 0.0;
    const bool negCaughtDark  = !(lNeg > 20.0);
    const bool negCaughtSplit = !(relNeg <= 0.10);
    plCheck(negCaughtDark && negCaughtSplit,
            "NEGATIVE CONTROL: inverted-normal panel is measured DARK (luma " + fmt(lNeg) +
            ") and FAILS the parity check (" + fmt(relNeg * 100.0, 1) + "% apart) — the probe can fail");
    plCheck(lNeg < 4.0,
            "AMBIENT OFF MEANS AMBIENT OFF: with setAmbient(0) + setIblIntensity(0) an "
            "unlit surface is BLACK (luma " + fmt(lNeg) + " < 4). A hidden sky-IBL ambient "
            "immune to setAmbient is what lit a windowless facility blue.");

    // 5. VALUE, NOT LUMENS (DECISIONS.md §3). The graybox wall panel every interior
    //    partition in the tower wears, under the SAME warm lamp as the grey panels:
    //    it must read WARM (a wall lit by tungsten is warm — if its albedo is blue
    //    enough to overturn the lamp, the room's own hue tell starts lying), and it
    //    must be in the same VALUE BAND as its neighbours, not charcoal. It measured
    //    0.077 linear (7.7% reflector, B/R = 1.45) until 2026-07-12.
    const double lWall = luma(c[3]);
    plCheck(c[3].r > c[3].g && c[3].g > c[3].b,
            "GRAYBOX WALL PANEL reads WARM under a warm lamp " + rgbStr(c[3]));
    const double wallFrac = lGlb > 0.0 ? lWall / lGlb : 0.0;
    plCheck(wallFrac > 0.55,
            "GRAYBOX WALL PANEL is in the VALUE BAND: " + fmt(wallFrac * 100.0, 1) +
            "% of the 0.216-linear grey reference (luma " + fmt(lWall) + " vs " + fmt(lGlb) + ")");

    x3::logInfo("=== --test-primlight: " + std::to_string(pl_pass) + " passed, " +
                std::to_string(pl_fail) + " failed ===");
    return pl_fail == 0 ? 0 : 1;
}

} // namespace x3::game
