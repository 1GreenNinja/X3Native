// --world undersea host — EFLZ Act-4 Abyssal Station showcase.
//
// PORT (payload re-land): the Act-4 OceanBase graybox (disc on the seafloor)
// draped with the real textured Abyssal Station GLB (UnderseaArtSystem), lit
// deep-sea, with drifting marine-snow particulate. Headless --screenshot path +
// a slow windowed orbit. Lifted from the feat/undersea-art inline main() block
// and reshaped onto the #28 HostContext world-host pattern (mirrors host_cliffs).
//
// Clean-room: built from IRenderDevice/IPhysicsWorld/UnderseaArtSystem + prims
// only. No third-party engine source consulted.
#include "world_host_common.h"
#include "../undersea_art.h"   // UnderseaArtSystem (+ ocean_base.h -> OceanBasePlan)
#include "../mesh_prims.h"     // x3::prims: makeBox / makeCheckerRGBA / makeCube
#include "../asset_root.h"     // x3::game::convertedGlbRoot()

namespace x3 { namespace apphost {

int hostUndersea(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;

    // ---- Undersea base showcase (--world undersea) -------------------------
    // The Act-4 OceanBase graybox (3-level disc on the seafloor) draped with the
    // real textured Abyssal Station GLB (UnderseaArtSystem), lit deep-sea, with a
    // headless --screenshot path + a slow windowed orbit. Mirrors --world swim.
    x3::logInfo("--world undersea: building the Act-4 undersea base + Abyssal Station overlay");
    std::unique_ptr<x3::phys::IPhysicsWorld> uphys(x3::phys::createPhysicsWorld());
    if (!uphys->init()) {
        x3::logError("--world undersea: physics init failed");
        device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    // Showcase at NEAR-ORIGIN so the point lights actually reach the station.
    // The real OceanBase sits at offshore (1100,-1350) where the Forward+ light
    // grid does not currently cover it (see undersea_art.cpp's note — flagged to
    // Integrator), so the station reads dark there. Here we place an equivalent
    // OceanBasePlan at the origin + a dark sediment seafloor, and the same GLB
    // overlay reads as a LIT hero. The --test-undersea-art gate still verifies
    // the real OceanBase placement; this is purely the visual showcase.
    x3::game::OceanBasePlan p{};
    p.cx = 0.0f; p.cz = 0.0f; p.surfaceY = 70.0f;   // surface far above -> we are UNDERWATER
    p.baseDeckY = 0.0f; p.seafloorY = -3.0f; p.radius = 45.0f; p.levels = 1;
    p.hasSubDock = p.hasAirlock = p.hasReactor = true;

    // Dark sediment seafloor at origin (the station sits on it).
    x3::prims::PrimMesh sf = x3::prims::makeBox(95.0f, 1.0f, 95.0f, 0.0f, p.seafloorY - 1.0f, 0.0f, 0.5f);
    auto sfMesh = device->createMesh(sf.verts.data(), (uint32_t)sf.verts.size(),
                                     sf.index.data(), (uint32_t)sf.index.size());
    auto sedPx  = x3::prims::makeCheckerRGBA(128, 24, 44, 50, 56, 30, 35, 41);
    auto sedTex = device->createTexture(sedPx.data(), 128, 128, true);
    // Marine snow: a tiny emissive cube instanced as drifting particulate for
    // deep-sea atmosphere (faint cool specks catching the light in the murk).
    std::vector<x3::rhi::MeshVertex> snv; std::vector<uint32_t> sni;
    x3::prims::makeCube(0.07f, snv, sni);
    auto snowMesh = device->createMesh(snv.data(), (uint32_t)snv.size(),
                                       sni.data(), (uint32_t)sni.size());
    const int kSnow = 150;

    x3::game::UnderseaArtSystem undersea;
    undersea.build(*device, x3::game::convertedGlbRoot(), p);

    // ---- Deep-sea sky + water (dark, high haze) ----
    { x3::rhi::IRenderDevice::SkyParams sk{};
      sk.enabled = true;
      sk.sunDir[0] = 0.05f; sk.sunDir[1] = 1.0f; sk.sunDir[2] = 0.15f;
      sk.sunIntensity = 1.8f; sk.haze = 0.9f; sk.exposure = 0.8f;   // god-ray top light
      device->setSkyParams(sk); }
    { x3::rhi::IRenderDevice::WaterParams wp{};
      wp.enabled = true; wp.seaLevel = p.surfaceY;
      wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
      wp.deepColor[0]    = 0.008f; wp.deepColor[1]    = 0.035f; wp.deepColor[2]    = 0.075f;
      wp.shallowColor[0] = 0.05f;  wp.shallowColor[1] = 0.22f;  wp.shallowColor[2] = 0.32f;
      wp.specular = 8.0f; wp.fresnel = 0.02f;
      device->setWaterParams(wp); }

    // Station's own cool point-light fixtures + a bright high key so the PBR
    // hull reads + feeds bloom (these DO reach it at the origin).
    std::vector<x3::rhi::PointLight> plights = undersea.lightFixtures();
    { x3::rhi::PointLight key; key.pos[0]=p.cx; key.pos[1]=p.baseDeckY+42.0f; key.pos[2]=p.cz;
      key.range=130.0f; key.color[0]=2.2f; key.color[1]=2.6f; key.color[2]=3.2f;
      plights.push_back(key); }
    if (!plights.empty()) device->setPointLights(plights.data(), (uint32_t)plights.size());

    float snowT = 0.0f;   // advanced each frame so the snow drifts down
    auto drawScene = [&](const x3::rhi::FrameContext& frame) {
        const float white[4] = { 1, 1, 1, 1 };
        const float idM[16]  = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        device->drawMesh(frame, sfMesh, sedTex, white, idM);  // seafloor
        undersea.draw(*device, frame);                        // the real textured station, LIT
        // ---- drifting marine snow (deterministic scatter + slow descent) ----
        auto fracf = [](float x){ return x - std::floor(x); };
        const float snowCol[4] = { 0.65f, 0.74f, 0.88f, 1.0f };
        const float snowEm[4]  = { 0.55f, 0.68f, 0.85f, 1.6f };  // faint cool HDR speck
        for (int s2 = 0; s2 < kSnow; ++s2) {
            const float hx = fracf(std::sin((float)s2 * 12.9898f) * 43758.55f);
            const float hy = fracf(std::sin((float)s2 * 78.233f)  * 43758.55f);
            const float hz = fracf(std::sin((float)s2 * 37.719f)  * 43758.55f);
            const float px = p.cx + (hx * 2.0f - 1.0f) * 48.0f;
            const float pz = p.cz + (hz * 2.0f - 1.0f) * 48.0f;
            const float span = 56.0f;
            const float drift = std::fmod(hy * span + snowT * 1.6f, span);
            const float py = p.baseDeckY + span - drift;       // descends, wraps
            const float mm[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, px,py,pz,1 };
            device->drawMeshEmissive(frame, snowMesh, x3::rhi::TextureHandle{0},
                                     snowCol, snowEm, mm);
        }
    };

    // Camera framing: orbit the disc centre, eye a touch above the top deck,
    // looking down slightly to catch station + disc + a sliver of seafloor.
    const float bx = p.cx, bz = p.cz;
    const float lookY = p.baseDeckY + 19.0f;   // station mid (it spans deck..+~42 m)
    auto computeCam = [&](float ang, float cam[5]) {
        const float R = 92.0f;                  // hero framing
        const float camX = bx + R * std::cos(ang);
        const float camZ = bz + R * std::sin(ang);
        const float camY = p.baseDeckY + 34.0f;
        const float dx = bx - camX, dy = lookY - camY, dz = bz - camZ;
        const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        cam[0]=camX; cam[1]=camY; cam[2]=camZ;
        cam[3]=std::atan2(dz, dx);
        cam[4]=std::asin(dy / (len > 1e-3f ? len : 1e-3f));
    };
    const float dt = 1.0f / 60.0f;

    // ===== Headless capture (--world undersea --screenshot <path>) =====
    if (headless) {
        float cam[5]; computeCam(-0.7f, cam);
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("G:/X3Native/captures/undersea.png");
        const int kFrames = 40;
        float waterT = 0.0f;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            uphys->step(dt);
            waterT += dt; snowT += dt;
            { x3::rhi::IRenderDevice::WaterParams wp{};
              wp.enabled = true; wp.seaLevel = p.surfaceY; wp.time = waterT;
              wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
              wp.deepColor[0]=0.008f; wp.deepColor[1]=0.035f; wp.deepColor[2]=0.075f;
              wp.shallowColor[0]=0.05f; wp.shallowColor[1]=0.22f; wp.shallowColor[2]=0.32f;
              wp.specular = 8.0f; wp.fresnel = 0.02f;
              device->setWaterParams(wp); }
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world undersea: wrote " + outPath);
        else       x3::logError("--world undersea: capture FAILED");
        device->destroyMesh(sfMesh); device->destroyTexture(sedTex); device->destroyMesh(snowMesh);
        uphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: slow auto-orbit, ESC to quit. =====
    x3::logInfo("--world undersea: slow orbit showcase — Esc to quit");
    double prevTime = glfwGetTime();
    float waterT = 0.0f, ang = -0.7f;
    int lastWd = (int)W, lastHd = (int)H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        uphys->step(fdt);
        waterT += fdt; ang += fdt * 0.12f; snowT += fdt;
        { x3::rhi::IRenderDevice::WaterParams wp{};
          wp.enabled = true; wp.seaLevel = p.surfaceY; wp.time = waterT;
          wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
          wp.deepColor[0]=0.008f; wp.deepColor[1]=0.035f; wp.deepColor[2]=0.075f;
          wp.shallowColor[0]=0.05f; wp.shallowColor[1]=0.22f; wp.shallowColor[2]=0.32f;
          wp.specular = 8.0f; wp.fresnel = 0.02f;
          device->setWaterParams(wp); }
        float cam[5]; computeCam(ang, cam);
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWd || chh != lastHd) {
            lastWd = cw; lastHd = chh;
            if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
        }
        device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);
        auto frame = device->beginFrame();
        if (frame.valid) drawScene(frame);
        device->endFrame(frame);
    }
    device->destroyMesh(sfMesh); device->destroyTexture(sedTex); device->destroyMesh(snowMesh);
    uphys->shutdown(); device->shutdown();
    if (window) glfwDestroyWindow(window); glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
