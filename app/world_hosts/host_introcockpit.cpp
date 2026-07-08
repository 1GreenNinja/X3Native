// --world introcockpit host — the intro cold-open's VISIBLE LAYER: the two-seat
// fighter cockpit (assets/converted_glb/Cockpit/fighter_cockpit.glb) stood up as
// SCENE ENTITIES on the game's production render path:
//   * hull / consoles     -> drawMeshPBR route (shared 1x1 MR forces PBR)
//   * MFD / gauge screens -> Entity.emissiveTex (club1127's content-screen
//                            recipe: the screen's own texture drives WHERE it
//                            glows; black texels stay dark)
//   * canopy panes        -> Entity.transparent + CLEAR GlassMaterial (the real
//                            transparent pass: see-through glass with shine)
// Deep-space backdrop = the analytic sky's procedural starfield (haze 0, the
// host_space recipe) + the GLB's own milky-way / planet-horizon panels.
//
// The scene builder is shared by the host and the --test-introcockpit headless
// gate, and is the exact build the intro orchestrator's live-window beats will
// draw once wired (intro_orchestrator.cpp's empty beginFrame/endFrame block).
//
// Clean-room: engine interfaces (Scene / IModelLoader / IAssetSource /
// IRenderDevice) + in-tree precedents (leveldoc_world's drawable->Entity bridge,
// club1127's emissiveTex + glass recipes, host_space's deep-space sky) only.
#include "world_host_common.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../asset_root.h"
#include "../headless_device.h"
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

namespace {

// Relative path of the cockpit GLB under the mounted converted_glb root.
const char* kCockpitRel = "Cockpit/fighter_cockpit.glb";

// Everything the cockpit scene owns. The loader + Model own the GPU handles, so
// the rig must outlive every frame that draws the entities (host keeps it on the
// stack for the loop; the orchestrator will own one for the intro's lifetime).
struct IntroCockpitRig {
    std::unique_ptr<x3::asset::IAssetSource> assets;
    std::unique_ptr<x3::asset::IModelLoader> loader;
    x3::asset::Model                         model;
    x3::game::Scene                          scene;
    x3::rhi::TextureHandle                   mrShared;   // 1x1 MR (forces PBR route)
    x3::rhi::TextureHandle                   mrGlassy;   // 1x1 polished MR (canopy panes)
    // Gate diagnostics.
    uint32_t drawables = 0, entities = 0, glassPanes = 0, screens = 0;

    void shutdown(x3::rhi::IRenderDevice& device) {
        if (mrShared.valid()) device.destroyTexture(mrShared);
        if (mrGlassy.valid()) device.destroyTexture(mrGlassy);
        if (model.ok && loader) loader->unload(model);
    }
};

// Build the cockpit Scene from the GLB. Mirrors leveldoc_world.cpp's
// drawable->Entity bridge, extended with the PBR/emissiveTex/glass routing.
// Returns false (rig empty) if the GLB is missing — callers keep their graybox.
bool buildIntroCockpitRig(IntroCockpitRig& rig, x3::rhi::IRenderDevice& device) {
    rig.assets.reset(x3::asset::createAssetSource());
    if (!rig.assets->mountDir(x3::game::convertedGlbRoot(), 0)) {
        x3::logWarn("[introcockpit] mountDir failed: " + x3::game::convertedGlbRoot());
        return false;
    }
    rig.loader.reset(x3::asset::createModelLoader(&device, rig.assets.get()));
    rig.model = rig.loader->load(kCockpitRel);
    if (!rig.model.ok) {
        x3::logWarn(std::string("[introcockpit] FAILED to load ") + kCockpitRel);
        return false;
    }
    const auto drawables = x3::asset::makeDrawables(rig.model);
    rig.drawables = (uint32_t)drawables.size();

    // Shared 1x1 metallic-roughness map (satin: rough 0.55, metal 0.25). Assigning
    // it forces the PBR route on entities whose material has no MR texture — which
    // is what makes Entity.emissiveTex honored (scene.cpp routes on mrTex.valid()).
    const uint8_t mrPx[4] = { 255, 140, 64, 255 };   // R=AO(1), G=rough, B=metal
    rig.mrShared = device.createTexture(mrPx, 1, 1, /*srgb*/false);
    const uint8_t mrGl[4] = { 255, 18, 10, 255 };    // polished near-dielectric (panes)
    rig.mrGlassy = device.createTexture(mrGl, 1, 1, /*srgb*/false);

    std::vector<x3::game::Entity> panes;   // added LAST (alpha blend ordering)
    for (const auto& d : drawables) {
        if (!d.meshId) continue;
        x3::game::Entity se;
        se.mesh = x3::rhi::MeshHandle{ d.meshId };
        se.tex  = x3::rhi::TextureHandle{ d.baseColorTexId };
        for (int i = 0; i < 4; ++i) se.baseColor[i] = d.baseColorFactor[i];
        // Cockpit sits at the origin: world transform = the baked node transform.
        for (int i = 0; i < 16; ++i) se.transform[i] = d.nodeTransform[i];
        se.tag = (uint32_t)x3::game::Tag::Prop;

        if (d.baseColorFactor[3] < 0.5f) {
            // ---- CANOPY PANE -> alpha-blended PBR: simple CLEAR glass with real
            // PBR specular shine (sun + rig + IBL at grazing angles). Bypasses the
            // transparent pass's scene-copy (which samples a cleared target in
            // standalone hosts — finding filed to the engine lane). Panes are
            // collected + added LAST so they blend over the completed scene.
            se.alphaBlend = true;
            se.mrTex = rig.mrGlassy;                 // polished MR (PBR route)
            se.baseColor[0] = 0.85f; se.baseColor[1] = 0.92f;
            se.baseColor[2] = 1.0f;  se.baseColor[3] = 0.12f;   // the blend alpha
            panes.push_back(se);
            ++rig.glassPanes;
            continue;
        } else {
            // ---- OPAQUE -> PBR route (real maps where the GLB has them, the
            // shared satin MR otherwise so normal/emissive maps are honored).
            se.normalTex = x3::rhi::TextureHandle{ d.normalTexId };
            se.mrTex = d.mrTexId ? x3::rhi::TextureHandle{ d.mrTexId } : rig.mrShared;
            if (d.emissiveTexId) {
                // ---- CONTENT SCREEN (club1127 recipe): the emissive map bakes
                // WHERE the surface glows; the HDR term scales it into bloom.
                se.emissiveTex = x3::rhi::TextureHandle{ d.emissiveTexId };
                se.emissive[0] = 1.0f; se.emissive[1] = 1.0f;
                se.emissive[2] = 1.0f; se.emissive[3] = 1.6f;
                ++rig.screens;
            }
        }
        rig.scene.add(se);
        ++rig.entities;
    }
    for (const auto& pe : panes) { rig.scene.add(pe); ++rig.entities; }
    x3::logInfo("[introcockpit] built: " + std::to_string(rig.entities) + " entit(ies) from " +
                std::to_string(rig.drawables) + " drawable(s) — " +
                std::to_string(rig.screens) + " emissive screen(s), " +
                std::to_string(rig.glassPanes) + " glass pane(s)");
    return rig.entities > 0;
}

// Deep-space look: the analytic sky's procedural starfield + cool ambient
// (host_space's proven recipe) + a cockpit-scale interior light rig.
void setIntroCockpitLook(x3::rhi::IRenderDevice& device) {
    { x3::rhi::IRenderDevice::SkyParams sp{};
      sp.enabled = true;
      sp.sunDir[0] = 0.25f; sp.sunDir[1] = 0.55f; sp.sunDir[2] = -0.75f;  // sun out the windshield
      sp.sunColor[0] = 0.85f; sp.sunColor[1] = 0.88f; sp.sunColor[2] = 1.0f;
      sp.sunIntensity = 0.55f;
      sp.haze = 0.0f;                          // haze 0 == deep space, stars on the full sphere
      sp.exposure = 1.0f;
      sp.zenith[0]  = 0.003f; sp.zenith[1]  = 0.003f; sp.zenith[2]  = 0.008f;
      sp.horizon[0] = 0.004f; sp.horizon[1] = 0.005f; sp.horizon[2] = 0.011f;
      device.setSkyParams(sp);
      device.setSkyTime(10.0f);
      device.setAmbient(0.11f, 0.12f, 0.16f); }
    // SSAO/SSGI raster black against an empty space backdrop on the no-RT path
    // (memory-bank finding) — off for the showcase, same as host_space.
    { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false; device.setSsaoParams(ap); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device.setGiParams(gp); }
    // IBL probe: bake the environment (the deep-space sky above) so glass gets the
    // REAL split-sum env reflection instead of the milky no-IBL sheen fallback, and
    // metals/screens pick up the dark-space ambient specular.
    device.setIblProbe(true);
    // Interior rig (cockpit scale, 1/r^2): cool key through the windshield onto
    // the dash, warm cabin fill from behind, teal console underglow.
    { x3::rhi::PointLight pl[3];
      pl[0].pos[0] =  0.6f; pl[0].pos[1] = 2.6f; pl[0].pos[2] = -2.4f; pl[0].range = 14.0f;
      pl[0].color[0] = 6.0f; pl[0].color[1] = 7.0f; pl[0].color[2] = 9.0f;
      pl[1].pos[0] = -1.2f; pl[1].pos[1] = 1.6f; pl[1].pos[2] =  1.4f; pl[1].range = 9.0f;
      pl[1].color[0] = 4.2f; pl[1].color[1] = 3.0f; pl[1].color[2] = 2.0f;
      pl[2].pos[0] =  0.0f; pl[2].pos[1] = 0.8f; pl[2].pos[2] = -0.5f; pl[2].range = 5.0f;
      pl[2].color[0] = 1.2f; pl[2].color[1] = 3.2f; pl[2].color[2] = 4.0f;
      device.setPointLights(pl, 3); }
}

} // namespace

int hostIntroCockpit(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    if (hc.worldMode != "introcockpit") return -1;

    x3::logInfo("--world introcockpit: the intro cold-open's cockpit as Scene entities");
    IntroCockpitRig rig;
    if (!buildIntroCockpitRig(rig, *device)) {
        x3::logError("--world introcockpit: cockpit GLB failed to load");
        device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    setIntroCockpitLook(*device);

    // Pilot's-eye framing (proven in the modeltest review sweeps): eye between
    // the headrests, looking forward (+Z in engine coords) over the dash.
    float cam[5] = { 0.0f, 1.60f, -1.38f, 1.5708f, -0.055f };
    if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];

    // ===== Headless capture (--world introcockpit --screenshot <path>) =====
    if (hc.headless) {
        device->setFrustumCullEnabled(false);   // visual gate; nested GLB AABBs (host_space note)
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("G:/X3Native/captures/introcockpit.png");
        const int kFrames = 16;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) rig.scene.render(*device, frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world introcockpit: wrote " + outPath);
        else       x3::logError("--world introcockpit: capture FAILED");
        rig.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: gentle head-sway pilot's-eye; Esc to quit. =====
    x3::logInfo("--world introcockpit: pilot's-eye showcase — Esc to quit");
    double prevTime = glfwGetTime(); float t = 0.0f;
    int lastWd = (int)hc.W, lastHd = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        t += dt;
        const float yaw   = cam[3] + 0.05f * std::sin(t * 0.31f);
        const float pitch = cam[4] + 0.02f * std::sin(t * 0.23f + 1.3f);
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWd || chh != lastHd) {
            lastWd = cw; lastHd = chh;
            if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
        }
        device->setCamera(cam[0], cam[1], cam[2], yaw, pitch, 65.0f);
        auto frame = device->beginFrame();
        if (frame.valid) rig.scene.render(*device, frame);
        device->endFrame(frame);
    }
    rig.shutdown(*device);
    device->shutdown();
    if (window) glfwDestroyWindow(window); glfwTerminate();
    return 0;
}

// ===========================================================================
// --test-introcockpit: headless gate. Builds the rig on the HeadlessRenderDevice
// (no window / Vulkan) and asserts the GLB loads, entities stand up, and the
// screen/glass routing classified the drawables. Bug-2 safe (no swapchain).
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void icCheck(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[introcockpit-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[introcockpit-test] FAIL " + name); }
}
} // namespace

bool runIntroCockpitSelfTest() {
    g_pass = g_fail = 0;
    x3::game::HeadlessRenderDevice device;
    IntroCockpitRig rig;
    const bool built = buildIntroCockpitRig(rig, device);
    icCheck(built, "C0 cockpit GLB loads + entities stand up");
    icCheck(rig.drawables >= 100,
            "C1 drawable count sane (got " + std::to_string(rig.drawables) + ")");
    icCheck(rig.entities == rig.drawables,
            "C2 every drawable became an entity (" + std::to_string(rig.entities) + ")");
    icCheck(rig.glassPanes >= 3,
            "C3 canopy panes routed to the glass pass (got " + std::to_string(rig.glassPanes) + ")");
    icCheck(rig.screens >= 5,
            "C4 emissive content screens wired (got " + std::to_string(rig.screens) + ")");
    rig.shutdown(device);
    x3::logInfo(std::string("introcockpit: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

}} // namespace x3::apphost
