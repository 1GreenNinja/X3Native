// ============================================================================
// host_gallery — THE CHARACTER GALLERY (--world gallery).
//
// Tim's port of the Predator II "Character Arena" concept (the walkable art
// gallery of the cast from the Empires of Shadow web game): a clean, well-lit
// museum hall with every rigged character on a pedestal (the CyberWolf gets
// floor space), each looping its Idle under a floating name label. NO combat.
//
//   * Walk up to a character (aim at it / stand near) and press E to CYCLE its
//     clips one by one (Idle -> ... -> Wave -> IdleAlt -> back). The current
//     clip name lives in the character's floating label — this is a living
//     clip-inspection gallery, incl. the restored Meshy test clips (Wave /
//     IdleAlt) that gameplay's fuzzy-find never touches.
//   * Windowed:   --world gallery            (WASD + mouse, E cycles, Esc quits)
//   * Screenshot: --world gallery --screenshot [--screenshot-path p] [--shot-cam ...]
//   * Self-test:  --test-gallery             (headless: cast builds, clips lists,
//                                             the cycle advances, restored clips present)
// ============================================================================
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../player.h"
#include "../monster.h"
#include "../mesh_prims.h"
#include "../surface_library.h"
#include "../asset_root.h"

#include <cctype>
#include <filesystem>

namespace x3 { namespace apphost {

namespace {

using x3::game::Scene;
using x3::game::Entity;
using x3::game::MonsterSystem;
using x3::game::MonsterType;

// ---- The cast --------------------------------------------------------------
// One row per character: GLB stem (prefers <stem>_anim.glb like defRigged),
// display name, modelScale (from the proven call sites: canon_aliens.cpp,
// club1127.cpp, monster.cpp roster), pedestal or floor.
struct CastRow {
    const char* stem;      // rigged_glb GLB stem (no extension)
    const char* label;     // floating name label
    float       scale;     // MonsterSystem::Tuning::modelScale
    bool        pedestal;  // true = museum pedestal; false = floor space
};

const CastRow kCast[] = {
    { "canon_grey",       "Grey",             0.75f, true  },
    { "canon_mantis",     "Mantis Arbiter",   1.05f, true  },
    { "canon_nordic",     "Nordic Steward",   1.05f, true  },
    { "canon_saurian",    "Saurian Soldier",  1.10f, true  },
    { "RexBouncer",       "Rex the Bouncer",  1.00f, true  },
    { "BossBreederQueen", "Breeder Queen",    1.55f, true  },
    { "Oracle",           "The Oracle",       1.00f, true  },
    { "AnnaBodySuit",     "Anna (Bodysuit)",  1.00f, true  },
    { "SalvariPrincess",  "Salvari Princess", 1.00f, true  },
    { "chief_martinez",   "Chief Martinez",   1.15f, true  },
    { "marcus_webb",      "Marcus Webb",      1.00f, true  },
    { "AnnaCasual",       "Anna (Casual)",    1.00f, true  },
    { "alien_crawler",    "CyberWolf",        1.00f, false },   // floor space, no pedestal
    // Jake_22_actions is EXCLUDED: his Mixamo rig bakes an armature-object Y
    // offset the monster draw path doesn't compensate (the 3P path carries an
    // empirical +1.03 floor-plant for it) — through this path he stands ~1 m
    // sunk. His 22 clips already live retargeted on chief/marcus (guard-death
    // wave), so the gallery loses nothing it can't show elsewhere.
};
constexpr int kCastCount = (int)(sizeof(kCast) / sizeof(kCast[0]));

// ---- Hall dimensions (museum-simple, FAST to load) -------------------------
constexpr float kHallHalfW = 6.0f;    // X half-width
constexpr float kHallHalfL = 21.0f;   // Z half-length
constexpr float kHallH     = 5.0f;    // ceiling height
constexpr float kRowX      = -3.6f;   // character row X (left side, facing the aisle)
constexpr float kRowZ0     = -16.9f;  // first character Z
constexpr float kRowStep   = 2.6f;    // spacing along Z
constexpr float kPedTop    = 0.18f;   // pedestal top Y

// Prefer <stem>_anim.glb over <stem>.glb (the defRigged idiom).
std::string pickGlb(const char* stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string anim = std::string(stem) + "_anim.glb";
    if (fs::exists(fs::path(x3::game::riggedGlbRoot()) / anim, ec)) return anim;
    return std::string(stem) + ".glb";
}

x3::phys::Vec3 castSpot(int i) {
    const bool ped = kCast[i].pedestal;
    return { kRowX, ped ? kPedTop : 0.0f, kRowZ0 + kRowStep * (float)i };
}

// Spawn one cast member as an INERT prop (the Club 1127 recipe: chaseSpeed 0 /
// damage 0 — the AI never moves it; the gallery owns pose + clip).
std::unique_ptr<MonsterSystem> spawnCastMember(Scene& scene, x3::rhi::IRenderDevice& device,
                                               x3::phys::IPhysicsWorld& physics, int i) {
    auto sys = std::make_unique<MonsterSystem>();
    MonsterSystem::Tuning t;
    t.type             = MonsterType::Guard;
    t.hp               = 100;
    t.chaseSpeed       = 0.0f;   // inert exhibit: never moves
    t.damage           = 0;      // never attacks
    t.ranged           = false;
    t.modelFile        = pickGlb(kCast[i].stem);
    t.modelDirOverride = x3::game::riggedGlbRoot();
    t.standUpZtoY      = false;  // rigged sources are authored Y-up
    t.modelScale       = kCast[i].scale;
    sys->buildMonsterTuned(scene, device, physics, x3::game::riggedGlbRoot(), castSpot(i), t);
    return sys;
}

// Exact-name clip lookup (findClip is fuzzy substring: it can't tell "Idle"
// from "IdleAlt"). Case-insensitive full-string compare.
int exactClip(const x3::anim::Skinner& sk, const char* name) {
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string want = lower(name);
    for (uint32_t c = 0; c < sk.clipCount(); ++c)
        if (lower(std::string(sk.clipName(c))) == want) return (int)c;
    return -1;
}

// The clip-cycle step: next index, wrapping. Pure (self-testable).
int nextClip(int cur, int clipCount) {
    if (clipCount <= 0) return -1;
    return (cur + 1) % clipCount;
}

// Start every exhibit on its exact Idle (else clip 0).
void startIdle(MonsterSystem& m) {
    if (!m.skinnable()) return;
    int idle = exactClip(m.skinner(), "Idle");
    if (idle < 0 && m.skinner().clipCount() > 0) idle = 0;
    if (idle >= 0) m.setCalmLoopClip(idle);
}

// Two-pass (shadow + ink) centered HUD text.
void hudTextCentered(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const char* s, float cx, float y, float px, const float rgba[4]) {
    const float w = device.textAdvance(x3::rhi::FontRole::Menu, s, px);
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f * rgba[3] };
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, s, cx - w * 0.5f + 1.5f, y + 1.5f, px, shadow);
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, s, cx - w * 0.5f,        y,        px, rgba);
}

// The character's current clip name for the label ("Idle", "Wave", ...).
std::string currentClipName(const MonsterSystem& m) {
    if (!m.skinnable()) return "static";
    const int c = m.calmLoopClip();
    if (c < 0 || (uint32_t)c >= m.skinner().clipCount()) return "Idle";
    return std::string(m.skinner().clipName((uint32_t)c));
}

// Draw every exhibit's floating label + return the aimed/near character index
// (or -1). Selection: within 5 m of the eye AND within ~35 deg of the aim.
int drawLabelsAndPick(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const std::vector<std::unique_ptr<MonsterSystem>>& cast,
                      float ex, float ey, float ez, float yaw, float pitch) {
    const float fx = std::cos(pitch) * std::cos(yaw);
    const float fy = std::sin(pitch);
    const float fz = std::cos(pitch) * std::sin(yaw);
    int   best = -1;
    float bestDot = 0.82f;   // ~35 deg cone
    for (int i = 0; i < (int)cast.size(); ++i) {
        if (!cast[i]) continue;
        const x3::phys::Vec3 p = castSpot(i);
        const float hx = p.x - ex, hy = (p.y + 1.0f) - ey, hz = p.z - ez;
        const float d = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (d < 5.0f && d > 0.01f) {
            const float dot = (hx * fx + hy * fy + hz * fz) / d;
            if (dot > bestDot) { bestDot = dot; best = i; }
        }
    }
    for (int i = 0; i < (int)cast.size(); ++i) {
        if (!cast[i]) continue;
        const x3::phys::Vec3 p = castSpot(i);
        const float dx = p.x - ex, dz = p.z - ez;
        if (dx * dx + dz * dz > 20.0f * 20.0f) continue;      // label range gate
        const float headY = p.y + 2.1f * kCast[i].scale + 0.25f;
        float sx, sy;
        if (!device.worldToScreen(p.x, headY, p.z, sx, sy)) continue;
        const bool sel = (i == best);
        const float name[4] = { sel ? 0.95f : 0.85f, sel ? 0.95f : 0.88f, sel ? 0.55f : 0.95f, 1.0f };
        const float clip[4] = { 0.55f, 0.85f, 1.0f, sel ? 1.0f : 0.85f };
        hudTextCentered(device, frame, kCast[i].label, sx, sy - 22.0f, 17.0f, name);
        hudTextCentered(device, frame, currentClipName(*cast[i]).c_str(), sx, sy, 14.0f, clip);
    }
    return best;
}

} // namespace

int hostGallery(HostContext& hc) {
    if (hc.worldMode != "gallery") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world gallery: THE CHARACTER GALLERY — the cast on pedestals, E cycles clips");

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world gallery: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    Scene scene;

    // ---- The hall: floor + walls + ceiling + pedestals (surface-library PBR). --
    x3::game::SurfaceLibrary surf;
    surf.mount(x3::game::assetRoot() + "/surface_library");
    const x3::game::SurfaceSet& sFloor = surf.get(*device, "hh_floor_01a");
    const x3::game::SurfaceSet& sWall  = surf.get(*device, "mw_concrete_panels_a");
    const x3::game::SurfaceSet& sCeil  = surf.get(*device, "hh_ceiling_01a");
    const x3::game::SurfaceSet& sPed   = surf.get(*device, "mw_metal_trim_b");

    auto addBox = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                      const float col[4], const x3::game::SurfaceSet* sf, bool collide) {
        x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
        Entity e;
        e.mesh = device->createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                    geo.index.data(), (uint32_t)geo.index.size());
        if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
        for (int k = 0; k < 4; ++k) e.baseColor[k] = col[k];
        if (sf && sf->ok) {
            const float vt = sf->valueTint();
            for (int k = 0; k < 3; ++k) e.baseColor[k] *= vt;
        }
        if (collide)
            e.body = phys->addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                         geo.cindex.data(), (uint32_t)geo.cindex.size());
        scene.add(e);
    };
    const float cNeutral[4] = { 0.92f, 0.92f, 0.94f, 1.0f };
    const float cWall[4]    = { 0.88f, 0.88f, 0.90f, 1.0f };
    const float cPed[4]     = { 0.80f, 0.80f, 0.84f, 1.0f };
    // Floor (top at y = 0) + ceiling + four walls.
    addBox(0.0f, -0.15f, 0.0f, kHallHalfW, 0.15f, kHallHalfL, cNeutral, &sFloor, true);
    addBox(0.0f, kHallH + 0.15f, 0.0f, kHallHalfW, 0.15f, kHallHalfL, cNeutral, &sCeil, false);
    addBox(-kHallHalfW - 0.15f, kHallH * 0.5f, 0.0f, 0.15f, kHallH * 0.5f, kHallHalfL, cWall, &sWall, true);
    addBox( kHallHalfW + 0.15f, kHallH * 0.5f, 0.0f, 0.15f, kHallH * 0.5f, kHallHalfL, cWall, &sWall, true);
    addBox(0.0f, kHallH * 0.5f, -kHallHalfL - 0.15f, kHallHalfW, kHallH * 0.5f, 0.15f, cWall, &sWall, true);
    addBox(0.0f, kHallH * 0.5f,  kHallHalfL + 0.15f, kHallHalfW, kHallH * 0.5f, 0.15f, cWall, &sWall, true);
    // Pedestals.
    for (int i = 0; i < kCastCount; ++i) {
        if (!kCast[i].pedestal) continue;
        const x3::phys::Vec3 p = castSpot(i);
        addBox(p.x, kPedTop * 0.5f, p.z, 0.85f, kPedTop * 0.5f, 0.85f, cPed, &sPed, true);
    }

    // ---- Lighting: even museum wash (no sky — interior; SSAO/GI off, the
    // standalone-host law) + a warm key row over the exhibits. -----------------
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    {
        std::vector<x3::rhi::PointLight> pl;
        for (float z = -18.0f; z <= 18.0f; z += 6.0f) {       // ceiling wash row (aisle)
            x3::rhi::PointLight l{};
            l.pos[0] = 1.0f; l.pos[1] = kHallH - 0.5f; l.pos[2] = z;
            l.range = 13.0f;
            l.color[0] = 2.4f; l.color[1] = 2.35f; l.color[2] = 2.25f;
            pl.push_back(l);
        }
        for (float z = -15.0f; z <= 18.0f; z += 6.0f) {       // exhibit key row (over the cast)
            x3::rhi::PointLight l{};
            l.pos[0] = kRowX + 1.2f; l.pos[1] = 3.4f; l.pos[2] = z;
            l.range = 9.0f;
            l.color[0] = 1.9f; l.color[1] = 1.85f; l.color[2] = 1.7f;
            pl.push_back(l);
        }
        device->setPointLights(pl.data(), (uint32_t)pl.size());
    }

    // ---- The cast. -----------------------------------------------------------
    std::vector<std::unique_ptr<MonsterSystem>> cast;
    cast.reserve(kCastCount);
    for (int i = 0; i < kCastCount; ++i) {
        cast.push_back(spawnCastMember(scene, *device, *phys, i));
        startIdle(*cast.back());
        if (!cast.back()->usingRealModel())
            x3::logError(std::string("--world gallery: fallback box for ") + kCast[i].stem +
                         " (GLB missing? run: python tools/asset_store.py fetch --all)");
    }

    const float kFaceYaw = 0.0f;   // exhibits face +X (the aisle)
    auto tickCast = [&](float dt, const x3::phys::Vec3& viewer) {
        for (int i = 0; i < kCastCount; ++i) {
            cast[i]->setPropPose(castSpot(i), kFaceYaw);
            cast[i]->setPropMotion(0.0f, 0.0f);
            cast[i]->update(dt, scene, *phys, viewer);
        }
    };
    auto drawCast = [&](const x3::rhi::FrameContext& frame) {
        for (const auto& c : cast) c->drawMonster(*device, frame, scene);
    };

    // ---- Headless screenshot path. ------------------------------------------
    if (hc.headless) {
        // Showcase the RESTORED test clips in the still: Nordic mid-Wave (the
        // raised-arm membrane web) and the Oracle on her theatrical IdleAlt —
        // exactly what the E-cycle reaches in the walkable build.
        if (cast[2]->skinnable()) {
            const int w = exactClip(cast[2]->skinner(), "Wave");
            if (w >= 0) cast[2]->setCalmLoopClip(w);
        }
        if (cast[6]->skinnable()) {
            const int a = exactClip(cast[6]->skinner(), "IdleAlt");
            if (a >= 0) cast[6]->setCalmLoopClip(a);
        }
        // Default showcase vantage: from the entrance, down the exhibit row.
        float cam[5] = { 4.5f, 2.0f, -14.0f, 2.04f, -0.06f };
        if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("captures/gallery.png");
        const float dt = 1.0f / 60.0f;
        const int kSettle = 90;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            tickCast(dt, { cam[0], cam[1], cam[2] });
            phys->step(dt);
            scene.update(*phys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                drawCast(frame);
                drawLabelsAndPick(*device, frame, cast, cam[0], cam[1], cam[2], cam[3], cam[4]);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world gallery: wrote screenshot " + outPath);
        else       x3::logError("--world gallery: capture FAILED");
        phys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Walkable windowed path. --------------------------------------------
    x3::game::Player player;
    player.spawn(*phys, 3.5f, 0.2f, -19.0f);
    player.setLook(1.9f, -0.02f);   // face down the exhibit row

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpace = false, prevF = false, prevE = false;
    bool noclip = false;
    float flyX = 3.5f, flyY = 1.8f, flyZ = -19.0f, flyYaw = 1.9f, flyPitch = -0.02f;
    x3::logInfo("--world gallery: WASD, mouse look, E cycle clips at an exhibit, "
                "Space jump, LeftShift sprint, F noclip, Esc to quit");
    int lastW = (int)hc.W, lastH = (int)hc.H;
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
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
        float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
        lastMX = mx; lastMY = my;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        bool spaceNow = kd(GLFW_KEY_SPACE);
        bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevF) {
            noclip = !noclip;
            if (noclip) { float yy, pp; player.camera(flyX, flyY, flyZ, yy, pp); flyYaw = yy; flyPitch = pp; }
        }
        prevF = fNow;

        float camX, camY, camZ, camYaw, camPitch;
        if (!noclip) {
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpace;
            in.lookDX = ddx; in.lookDY = ddy;
            player.update(in, dt, *phys);
            player.camera(camX, camY, camZ, camYaw, camPitch);
        } else {
            const float sens = 0.0025f;
            flyYaw += ddx * sens; flyPitch -= ddy * sens;
            if (flyPitch >  1.55f) flyPitch =  1.55f;
            if (flyPitch < -1.55f) flyPitch = -1.55f;
            float fx = std::cos(flyPitch) * std::cos(flyYaw);
            float fy = std::sin(flyPitch);
            float fz = std::cos(flyPitch) * std::sin(flyYaw);
            float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz / rl, rz = fx / rl;
            float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyX += fx * spd; flyY += fy * spd; flyZ += fz * spd; }
            if (kd(GLFW_KEY_S)) { flyX -= fx * spd; flyY -= fy * spd; flyZ -= fz * spd; }
            if (kd(GLFW_KEY_D)) { flyX += rx * spd; flyZ += rz * spd; }
            if (kd(GLFW_KEY_A)) { flyX -= rx * spd; flyZ -= rz * spd; }
            if (spaceNow) flyY += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyY -= spd;
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        prevSpace = spaceNow;

        tickCast(dt, { camX, camY, camZ });
        phys->step(dt);
        scene.update(*phys);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }

        device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            drawCast(frame);
            const int aimed = drawLabelsAndPick(*device, frame, cast,
                                                camX, camY, camZ, camYaw, camPitch);
            // The clip-cycle interact: E on the aimed/near exhibit.
            const bool eNow = kd(GLFW_KEY_E);
            if (aimed >= 0) {
                if (eNow && !prevE && cast[aimed]->skinnable()) {
                    const int n = nextClip(cast[aimed]->calmLoopClip(),
                                           (int)cast[aimed]->skinner().clipCount());
                    if (n >= 0) cast[aimed]->setCalmLoopClip(n);
                }
                uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                const std::string hint = std::string("[E] next clip — ") + kCast[aimed].label +
                                         ": " + currentClipName(*cast[aimed]);
                const float col[4] = { 0.62f, 0.92f, 1.0f, 1.0f };
                hudTextCentered(*device, frame, hint.c_str(),
                                (hw > 0) ? hw * 0.5f : 640.0f,
                                (hh > 0) ? hh * 0.78f : 560.0f, 20.0f, col);
            }
            prevE = eNow;
        }
        shell.draw(frame);
        device->endFrame(frame);
    }

    phys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost

// ============================================================================
// --test-gallery: headless self-test (no window, no Vulkan) — the canon_aliens
// scaffolding. G1 cast builds, G2 real GLBs, G3 skinnable + clips, G4 the
// clip-cycle advances + wraps, G5 the restored test clips are present.
// ============================================================================
#include "../headless_device.h"

namespace x3 { namespace apphost {

namespace {
int g_pass = 0, g_fail = 0;
void gcheck(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[gallery-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[gallery-test] FAIL ") + name); }
}
bool hasClip(const MonsterSystem& m, const char* name) {
    return m.skinnable() && exactClip(m.skinner(), name) >= 0;
}
} // namespace

bool runGallerySelfTest() {
    g_pass = g_fail = 0;
    x3::game::HeadlessRenderDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    Scene scene;

    std::vector<std::unique_ptr<MonsterSystem>> cast;
    for (int i = 0; i < kCastCount; ++i)
        cast.push_back(spawnCastMember(scene, device, *phys, i));

    // G1: every cast member BUILDS (alive, real entity).
    {
        int ok = 0;
        for (const auto& c : cast) if (c->alive() && c->entity() != x3::game::kNoLink) ++ok;
        gcheck(ok == kCastCount, "G1 all gallery cast members build");
    }
    // G2: every cast member loads its REAL GLB (no fallback boxes in the museum).
    {
        int real = 0;
        for (int i = 0; i < kCastCount; ++i) {
            if (cast[i]->usingRealModel()) ++real;
            else x3::logError(std::string("[gallery-test] fallback box: ") + kCast[i].stem);
        }
        gcheck(real == kCastCount, "G2 all cast members load their real GLB");
    }
    // G3: every cast member is skinnable with at least one clip.
    {
        int ok = 0;
        for (int i = 0; i < kCastCount; ++i) {
            if (cast[i]->skinnable() && cast[i]->skinner().clipCount() >= 1) ++ok;
            else x3::logError(std::string("[gallery-test] not skinnable / no clips: ") + kCast[i].stem);
        }
        gcheck(ok == kCastCount, "G3 all cast members are skinnable with clips");
    }
    // G4: the clip cycle ADVANCES and WRAPS (nordic: full loop returns to start).
    {
        MonsterSystem& m = *cast[2];   // canon_nordic
        bool ok = m.skinnable() && m.skinner().clipCount() >= 2;
        if (ok) {
            startIdle(m);
            const int start = m.calmLoopClip();
            const int n = (int)m.skinner().clipCount();
            int cur = start;
            bool advanced = false;
            for (int s = 0; s < n; ++s) {
                cur = nextClip(cur, n);
                m.setCalmLoopClip(cur);
                if (m.calmLoopClip() != cur) ok = false;
                if (cur != start) advanced = true;
            }
            ok = ok && advanced && (cur == start);
        }
        gcheck(ok, "G4 clip cycle advances every clip and wraps to the start");
    }
    // G5: the RESTORED test clips are present (the whole point of the gallery).
    gcheck(hasClip(*cast[2], "Wave"),     "G5a canon_nordic has the restored Wave clip");
    gcheck(hasClip(*cast[8], "Wave"),     "G5b SalvariPrincess has the restored Wave clip");
    gcheck(hasClip(*cast[6], "IdleAlt"),  "G5c Oracle has the restored IdleAlt clip");
    gcheck(hasClip(*cast[7], "IdleAlt"),  "G5d AnnaBodySuit has the restored IdleAlt clip");

    phys->shutdown();
    x3::logInfo("[gallery-test] " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

}} // namespace x3::apphost
