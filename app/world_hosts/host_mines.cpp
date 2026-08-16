// --world mines host — the MINE ENTRANCE showcase (inspx/mines lane).
//
// A flat gravel apron with the family of mine entrances built by
// app/mine_fx.cpp (Armory rock + timber portal + the glowing mouth). Exists so
// the mines can be art-directed and screenshot in isolation, with a stable
// day/night rig:
//
//   X3Engine.exe --world mines --screenshot out.png            (day, hero cam)
//   MINES_TOD=night MINES_CAM=hero  -> the money shot (glowing mouth at night)
//   MINES_CAM=close|dist|family     -> portal detail / distance / the whole row
//   --shot-cam x,y,z,yaw,pitch      -> overrides everything (radians, yaw 0=+X)
//
// Windowed: WASD + mouse fly-cam, N toggles night, Esc quits.
//
// Lighting: the mines return their own PointLights (several per mouth — the
// clustered path affords real light counts, r_clusterlights doctrine). The
// host turns clustered lighting ON for this world.
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../mine_fx.h"
#include "../mesh_prims.h"
#include "../mesh_lod.h"
#include "../surface_library.h"
#include "../asset_root.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

namespace {

const char* envOr(const char* k, const char* dflt) {
    const char* v = std::getenv(k);
    return (v && v[0]) ? v : dflt;
}

// Day / night sky+ambient rig (echotropolis TOD doctrine, distilled to the two
// endpoints this showcase needs: noon baseline and the black-sky night with a
// moonlight ambient floor so the rock stays readable while the mouths own the
// frame).
void applyTod(x3::rhi::IRenderDevice* device, bool night) {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    if (!night) {
        sp.sunDir[0] = 0.45f; sp.sunDir[1] = 0.80f; sp.sunDir[2] = 0.32f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 1.0f; sp.haze = 0.45f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        device->setAmbient(0.26f, 0.27f, 0.30f);
    } else {
        // Night: black dome (stars own the sky per Tim's canon), a dim high
        // "moon" as the directional key so the CSM pass stays sane, and a
        // blue-grey ambient floor. The mouths do the rest.
        sp.sunDir[0] = 0.25f; sp.sunDir[1] = 0.85f; sp.sunDir[2] = 0.18f;
        sp.sunColor[0] = 0.62f; sp.sunColor[1] = 0.70f; sp.sunColor[2] = 0.95f;
        sp.sunIntensity = 0.0f;          // no visible disk
        sp.haze = 0.2f; sp.exposure = 1.0f;
        sp.zenith[0] = sp.zenith[1] = sp.zenith[2] = 0.0f;
        sp.horizon[0] = sp.horizon[1] = sp.horizon[2] = 0.0f;
        sp.sunLight = 0.07f;             // moon fill on the PBR meshes
        device->setSkyParams(sp);
        device->setAmbient(0.045f, 0.050f, 0.075f);
    }
}

struct Cam { float x, y, z, yaw, pitch, fov; };

// Fixed showcase cameras (metres / radians; yaw 0 faces +X, -pi/2 faces -Z —
// the hero mine's mouth opens toward +Z, so cameras stand on +Z looking back).
Cam namedCam(const std::string& name) {
    if (name == "close")  return { 2.6f, 2.3f, 6.4f, -2.05f, -0.05f, 55.0f };
    if (name == "dist")   return { 4.0f, 5.0f, 62.0f, -1.635f, -0.035f, 60.0f };
    if (name == "family") return { -39.0f, 8.0f, 55.0f, -1.5708f, -0.09f, 75.0f };
    /* hero */            return { 7.2f, 3.1f, 13.5f, -2.075f, -0.075f, 58.0f };
}

} // namespace

int hostMines(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;

    x3::logInfo("--world mines: building the mine-entrance showcase");

    x3::game::Scene scene;
    x3::game::SurfaceLibrary surf;
    surf.mount(x3::game::assetRoot() + "/surface_library");

    // ---- gravel apron ground (real surface set, not a checkerboard) --------
    {
        std::vector<x3::rhi::MeshVertex> gv; std::vector<uint32_t> gi;
        x3::prims::makeGroundQuad(140.0f, 48.0f, gv, gi);   // ~5.8 m/tile: repeats stay sub-visible
        x3::game::Entity g;
        g.mesh = device->createMesh(gv.data(), (uint32_t)gv.size(),
                                    gi.data(), (uint32_t)gi.size());
        const auto& gs = surf.get(*device, "fw_rock_cliff");
        if (gs.ok) { g.tex = gs.albedo; g.normalTex = gs.normal; g.mrTex = gs.mr; }
        g.baseColor[0] = 0.60f; g.baseColor[1] = 0.54f; g.baseColor[2] = 0.46f;   // dirt grade g.baseColor[3] = 1.0f;
        g.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(g);
    }

    // ---- the mines: gold hero at origin, the family spread along -X --------
    // (one build path — ore type only retints the glow/lights/ore props).
    std::vector<x3::rhi::PointLight> lights;
    x3::game::GoldMineWorld hero;
    hero.setSurfaceLibrary(&surf);
    hero.build(scene, *device, 0.0f, 0.0f, 0.0f);
    {
        const auto& ml = hero.pointLights();
        lights.insert(lights.end(), ml.begin(), ml.end());
    }
    x3::game::GoldMineWorld copperMine, stoneMine, uraniumMine;
    struct Row { x3::game::GoldMineWorld* m; float x; int ore; };
    const Row rows[] = {
        { &copperMine,  -26.0f, x3::game::GoldMineWorld::kOreCopper  },
        { &stoneMine,   -52.0f, x3::game::GoldMineWorld::kOreStone   },
        { &uraniumMine, -78.0f, x3::game::GoldMineWorld::kOreUranium },
    };
    for (const Row& r : rows) {
        r.m->setOre(r.ore);
        r.m->setSurfaceLibrary(&surf);
        r.m->build(scene, *device, r.x, 0.0f, 0.0f);
        const auto& ml = r.m->pointLights();
        lights.insert(lights.end(), ml.begin(), ml.end());
    }
    x3::logInfo("--world mines: " + std::to_string(scene.size()) + " entities, " +
                std::to_string(lights.size()) + " point lights");

    // Clustered froxel lighting ON — this world runs real light counts.
    device->setClusterLights(true);
    device->setPointLights(lights.data(), (uint32_t)lights.size());

    // MINES_LOD=0 forces LOD0 everywhere (A/B); MINES_DEBUGVIEW=6 shows the
    // froxel occupancy heatmap (only meaningful with clustered lighting on).
    x3::game::lodPolicy().enabled = std::strcmp(envOr("MINES_LOD", "1"), "0") != 0;
    device->setDebugView(std::atoi(envOr("MINES_DEBUGVIEW", "0")));

    bool night = std::strcmp(envOr("MINES_TOD", "day"), "night") == 0;
    applyTod(device, night);
    device->setCameraFar(400.0f);

    const float dt = 1.0f / 60.0f;

    // ===== Headless capture ================================================
    if (headless) {
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("w_mines.png");
        Cam cam = namedCam(envOr("MINES_CAM", "hero"));
        if (hc.shotCamOverride) {
            cam.x = hc.shotCam[0]; cam.y = hc.shotCam[1]; cam.z = hc.shotCam[2];
            cam.yaw = hc.shotCam[3]; cam.pitch = hc.shotCam[4];
        }
        const int kFrames = 110;   // TAA/bloom/autoexposure settle
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            hero.update(dt, scene);
            copperMine.update(dt, scene); stoneMine.update(dt, scene); uraniumMine.update(dt, scene);
            device->setCamera(cam.x, cam.y, cam.z, cam.yaw, cam.pitch, cam.fov);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) scene.render(*device, frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            char pf[160];
            const auto& ls = scene.lodStats();
            std::snprintf(pf, sizeof(pf), " gpu=%.2fms lod[%u/%u/%u/%u] trisSel=%llu trisLod0=%llu",
                          st.gpuFrameMs, ls.perLevel[0], ls.perLevel[1], ls.perLevel[2], ls.perLevel[3],
                          (unsigned long long)ls.trisSelected, (unsigned long long)ls.trisLod0);
            x3::logInfo("--world mines: wrote " + outPath +
                        " | draws=" + std::to_string(st.drawCalls) +
                        " tris=" + std::to_string(st.triangles) +
                        " lights=" + std::to_string(lights.size()) + pf +
                        (night ? " (night)" : " (day)"));
        } else x3::logError("--world mines: capture FAILED");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed fly-cam ================================================
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    Cam c = namedCam("hero");
    float fx = c.x, fy = c.y, fz = c.z, fyaw = c.yaw, fpitch = c.pitch;
    bool nHeld = false;
    int lastW = (int)hc.W, lastH = (int)hc.H;
    x3::logInfo("--world mines: WASD + mouse, Space/Ctrl up-down, Shift sprint, N day/night, Esc quits");
    // Console (~), ESC menu and the FPS/stats overlay. See host_shell.h:
    // the engine has had all three for a long time and 28 of ~31 hosts
    // wired none of them, so the worlds you could actually launch and play
    // were the one place in the engine with no developer tools at all.
    HostShell shell;
    shell.attach(hc);

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        glfwPollEvents();
        shell.beginFrame();   // ESC opens the menu now; SHIFT+ESC quits
        double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
        float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
        lastMX = mx; lastMY = my;
        auto kd = [&](int k){ return shell.key(k); };   // false while the console/menu owns input
        fyaw += ddx * 0.0025f; fpitch -= ddy * 0.0025f;
        if (fpitch >  1.55f) fpitch =  1.55f;
        if (fpitch < -1.55f) fpitch = -1.55f;
        float dx = std::cos(fpitch)*std::cos(fyaw), dy = std::sin(fpitch), dz = std::cos(fpitch)*std::sin(fyaw);
        float rl = std::sqrt(dx*dx + dz*dz); if (rl < 1e-4f) rl = 1e-4f;
        float rx = -dz/rl, rz = dx/rl;
        float spd = 8.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
        if (kd(GLFW_KEY_W)) { fx += dx*spd; fy += dy*spd; fz += dz*spd; }
        if (kd(GLFW_KEY_S)) { fx -= dx*spd; fy -= dy*spd; fz -= dz*spd; }
        if (kd(GLFW_KEY_D)) { fx += rx*spd; fz += rz*spd; }
        if (kd(GLFW_KEY_A)) { fx -= rx*spd; fz -= rz*spd; }
        if (kd(GLFW_KEY_SPACE)) fy += spd;
        if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;
        if (kd(GLFW_KEY_N)) { if (!nHeld) { night = !night; applyTod(device, night); } nHeld = true; }
        else nHeld = false;

        hero.update(fdt, scene);
        copperMine.update(fdt, scene); stoneMine.update(fdt, scene); uraniumMine.update(fdt, scene);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW=cw; lastH=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
        device->setCamera(fx, fy, fz, fyaw, fpitch, 70.0f);
        auto frame = device->beginFrame();
        if (frame.valid) scene.render(*device, frame);
        shell.draw(frame);
        device->endFrame(frame);
    }
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
