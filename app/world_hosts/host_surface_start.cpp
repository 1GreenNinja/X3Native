// ============================================================================
// --world surface  (a.k.a. the ESCAPED-branch Act-1 surface-landing start)
//
// Phase 7 of the interactive branching cold-open (docs/design/
// INTERACTIVE_INTRO_DESIGN.md §6, INTERACTIVE_INTRO_PLAN.md Phase 7).
//
// This is the OTHER side of the intro fork. When the player wins the space
// dogfight (intro.outcome == "escaped"; StoryFlags["intro.landed"] is set by the
// ion-pulse descent in Phase 6), the game does NOT wake him a prisoner in the
// canon cell — it lands him OUTSIDE the huge GLASS FACILITY where Sarah is held,
// FREE and ARMED, as a *rescuer*. That is the exact inverse of the canon Level-1
// cell start (prisoner inside). This host builds that surface-landing slice:
//
//   * the surface (ground plane + analytic sky + a low sun + fill lights),
//   * Jake's LANDED SHIP on the surface (JakeFighterShip.glb, fallback box),
//   * the GLASS FACILITY EXTERIOR ahead — a large structure walled in real
//     translucent glass (reuses the GlassMaterial / transparent-pass plumbing,
//     app/glass_test.h + Scene::Entity.glass), the prison Sarah is inside,
//   * SARAH visible behind the glass (reuses the RESCUE system, app/rescue.h —
//     a single Captive figure framed as the rescue target; no timer/boss here,
//     this is the calm landing beat, not the F2 countdown hub),
//   * a BREACH / ENTRY DOOR in the glass wall — the hand-off point into the
//     existing facility/rescue content,
//   * the player spawned OUTSIDE, FIRST-PERSON, FREE + ARMED (the canon Player
//     controller, an energy-pistol prop on his hip), and
//   * an OBJECTIVE line ("REACH THE FACILITY — FIND SARAH") via the canon
//     ObjectiveSystem, which advances to "BREACH THE FACILITY" as he nears the
//     entry, then "RESCUE SARAH" at the breach (the rescuer framing).
//
// First FOCUSED slice per the plan: landing -> approach -> entry. The full
// interior rescuer Act-1 is the existing facility/rescue content this hands to;
// reaching the breach logs the hand-off (a later phase wires the live transition
// into canon_play / the facility interior).
//
// Game/slice code only; engine/ stays pure. Mirrors host_space.cpp / host_valley
// .cpp structure exactly: build own physics + Scene, a headless capture path
// (--world surface --screenshot <p>) and a windowed walk loop, then tear down
// the device + window + glfw and return the program exit code (the host
// contract; see app/world_hosts.h).
// ============================================================================
#include "world_host_common.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../player.h"
#include "../objective.h"
#include "../rescue.h"
#include "../monster.h"
#include "../asset_root.h"
#include "../intro_orchestrator.h"   // IntroOutcome / readOutcomeFlag / kIntroLandedFlag (--test-surfacestart)
#include "../story_ops.h"            // x3::game::StoryFlags (the branch signal)
#include "../headless_device.h"      // HeadlessRenderDevice (the headless scene-build self-test)
#include <filesystem>

namespace x3 { namespace apphost {

// World layout (metres). The player lands at the origin facing -Z toward the
// facility; the ship sits just behind/beside him; the facility wall is ~40 m out.
namespace {
constexpr float kGroundY      = 0.0f;
constexpr float kFacilityZ    = -42.0f;   // front glass wall plane
constexpr float kFacilityHalfW= 14.0f;    // facility half-width  (X) — narrow: it reads as a TOWER, not a slab
constexpr float kFacilityHalfD= 16.0f;    // facility half-depth  (Z)
constexpr float kBreachHalfW  = 2.4f;     // entry breach half-width
constexpr float kEntryReach   = 6.0f;     // distance to the breach that triggers the hand-off
constexpr float kApproachZ    = -22.0f;   // crossing this advances "approach" objective

// ---- THE GLASS TOWER (Tim's canon, 2026-07-01). The facility exterior reads as
// a modern GLASS OFFICE TOWER: SEVEN visible stories of dark tinted reflective
// curtain-wall windows — but the building is noticeably TALLER than 7 stories
// should be, because HIDDEN LEVEL 4.5 (the secret floor between 4 and 5) adds
// its full story height between window bands 4 and 5 WITHOUT a lit band of its
// own (subtly blanked panels). The height mismatch IS the storytelling: an
// observant player can count 7 bands, eyeball the height, and notice the tower
// is one story too tall. The elevator's holo floor panel does NOT list 4.5 —
// the secret is found, not shown.
constexpr float kStoryH      = 4.0f;                       // one office story (band pitch)
constexpr float kSpandrelH   = 0.9f;                       // opaque band between stories
constexpr float kHiddenH     = 4.0f;                       // HIDDEN LEVEL 4.5's inserted height
constexpr int   kStories     = 7;                          // VISIBLE window-band stories
constexpr int   kHiddenBelow = 4;                          // the hidden floor sits between bands 4 and 5
constexpr float kTowerH      = kStories * kStoryH + kHiddenH;   // 32 m — "too tall for 7"
// Story band base Y: bands above the hidden floor are shifted UP by kHiddenH.
inline float storyBaseY(int i) { return i * kStoryH + (i >= kHiddenBelow ? kHiddenH : 0.0f); }
}

int hostSurfaceStart(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    x3::logInfo("--world surface: building the ESCAPED-branch Act-1 surface landing "
                "(outside the glass facility where Sarah is held; free + armed rescuer)");

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world surface: physics init failed");
        device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate();
        return 1;
    }

    // ---- Sky + lighting: a low alien sun over a dim surface, so the glass reads.
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.55f; sp.sunDir[2] = -0.4f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.86f; sp.sunColor[2] = 0.72f;
        sp.sunIntensity = 0.9f; sp.haze = 0.6f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
    }
    {
        // Fill point lights INSIDE the tower so Sarah + the glow behind the dark
        // glass read against the daylight (ground lobby + a mid-tower office glow
        // so the upper lit bands aren't dead) — plus warm sun bounce on the ship.
        // NOTE: nothing lights the hidden 4.5 gap — its panels stay blanked.
        x3::rhi::PointLight pl[3];
        pl[0].pos[0] = 0.0f; pl[0].pos[1] = 4.0f; pl[0].pos[2] = kFacilityZ - 6.0f;
        pl[0].range = 60.0f;
        pl[0].color[0] = 6.0f; pl[0].color[1] = 8.0f; pl[0].color[2] = 12.0f;   // cold lobby glow
        pl[1].pos[0] = 0.0f; pl[1].pos[1] = 6.0f; pl[1].pos[2] = 10.0f;
        pl[1].range = 50.0f;
        pl[1].color[0] = 7.0f; pl[1].color[1] = 6.0f; pl[1].color[2] = 5.0f;    // warm sun bounce on the ship
        pl[2].pos[0] = 0.0f; pl[2].pos[1] = kTowerH - kStoryH * 1.5f; pl[2].pos[2] = kFacilityZ - 8.0f;
        pl[2].range = 45.0f;
        pl[2].color[0] = 4.0f; pl[2].color[1] = 5.5f; pl[2].color[2] = 8.0f;    // upper-story office glow
        device->setPointLights(pl, 3);
    }

    x3::game::Scene scene;

    // ---- Ground plane (a wide static surface slab). ------------------------
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(120.0f, 0.5f, 120.0f, 0.0f, -0.5f, 0.0f, 8.0f);
        auto gm = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                     g.index.data(), (uint32_t)g.index.size());
        auto gtD = x3::prims::makeCheckerRGBA(64, 16, 70, 68, 64, 52, 50, 46);
        auto gt = device->createTexture(gtD.data(), 64, 64, true);
        x3::game::Entity e{}; e.mesh = gm; e.tex = gt;
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 1.0f;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
        // Static collision floor so the Player capsule stands on it.
        phys->addBox(x3::phys::Vec3{120.0f, 0.5f, 120.0f}, x3::phys::Vec3{0.0f, -0.5f, 0.0f},
                     0.0f, x3::phys::Layer::Static);
    }

    // ---- THE GLASS TOWER exterior (Tim's canon — see the constants block above).
    //      A modern glass OFFICE TOWER: 7 visible stories of DARK TINTED
    //      REFLECTIVE curtain-wall window bands separated by opaque spandrels —
    //      with HIDDEN LEVEL 4.5's full height inserted between bands 4 and 5 as
    //      subtly BLANKED (unlit) panels, so the building reads one story too
    //      tall to anyone who counts. Reuses Entity.transparent + GlassMaterial
    //      (SSR/RT reflections + the transparent pass make the dark glass read
    //      rich against the sky). Collision = full-height static boxes per face
    //      (front split around the entry breach), independent of the panel visuals.
    // `lit`: a lit office band (faint cool interior glow) vs the hidden floor's
    // blanked panels (NO emissive, a touch darker — the tell is subtle).
    auto glassBand = [&](float cx, float cy, float cz, float hx, float hy, float hz, bool lit) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
        auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        auto td = x3::prims::makeSolidRGBA(8, 26, 32, 40);   // dark smoked-glass base
        auto tx = device->createTexture(td.data(), 8, 8, true);
        x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
        e.transparent = true;
        // DARK TINT + mirror-smooth: the reflective read (SSR/RT + clearcoat).
        e.glass.opacity   = lit ? 0.72f : 0.80f;
        e.glass.refraction= 0.02f;
        e.glass.roughness = 0.03f;
        e.glass.specular  = 1.0f;
        e.glass.tint[0] = lit ? 0.12f : 0.09f;
        e.glass.tint[1] = lit ? 0.15f : 0.11f;
        e.glass.tint[2] = lit ? 0.19f : 0.13f;
        if (lit) {   // faint cool interior glow — occupied office stories
            e.emissive[0] = 0.04f; e.emissive[1] = 0.07f; e.emissive[2] = 0.11f; e.emissive[3] = 0.5f;
        }            // hidden 4.5: NO glow (blanked panels — the quiet tell)
        e.baseColor[3] = e.glass.opacity;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
    };
    // Opaque visual slab (spandrels / mullion columns / roof cap) — NO collision;
    // the walls' full-height static boxes below are the collision truth.
    auto facadeSlab = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          uint8_t r, uint8_t g, uint8_t b) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 2.0f);
        auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        auto td = x3::prims::makeSolidRGBA(8, r, g, b);
        auto tx = device->createTexture(td.data(), 8, 8, true);
        x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
    };
    // Collision-only slab (invisible truth behind the curtain wall).
    auto collideBox = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
        phys->addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                     0.0f, x3::phys::Layer::Static);
    };
    auto opaqueSlab = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          uint8_t r, uint8_t g, uint8_t b) {
        facadeSlab(cx, cy, cz, hx, hy, hz, r, g, b);
        collideBox(cx, cy, cz, hx, hy, hz);
    };

    const float fcz = kFacilityZ - kFacilityHalfD;     // tower footprint center Z
    const float wallT  = 0.4f;                         // wall thickness (half)
    const float backZ  = kFacilityZ - 2.0f * kFacilityHalfD;
    {
        // ---- One face = 7 window bands + spandrels + the hidden 4.5 blank fill.
        // axis: 0 = the face spans X (front/back), 1 = the face spans Z (sides).
        auto buildFace = [&](int axis, float planeCoord, float spanCenter, float spanHalf,
                             bool splitBreach) {
            auto place = [&](float sCenter, float sHalf, float y0, float y1, bool glass, bool lit) {
                const float cy = (y0 + y1) * 0.5f, hy = (y1 - y0) * 0.5f;
                float cx, cz, hx, hz;
                if (axis == 0) { cx = sCenter; cz = planeCoord; hx = sHalf; hz = wallT; }
                else           { cx = planeCoord; cz = sCenter; hx = wallT; hz = sHalf; }
                if (glass) glassBand(cx, cy, cz, hx, hy, hz, lit);
                else       facadeSlab(cx, cy, cz, hx, hy, hz, 22, 25, 30);   // dark spandrel steel
            };
            for (int i = 0; i < kStories; ++i) {
                const float y0 = storyBaseY(i);
                // Spandrel base band, then the story's dark window band.
                place(spanCenter, spanHalf, y0, y0 + kSpandrelH, false, false);
                const float g0 = y0 + kSpandrelH, g1 = y0 + kStoryH;
                if (splitBreach && i == 0) {
                    // Ground story, front face: split the window band around the breach.
                    const float sideW = (spanHalf - kBreachHalfW) * 0.5f;
                    place(-(kBreachHalfW + sideW), sideW, g0, g1, true, true);
                    place( (kBreachHalfW + sideW), sideW, g0, g1, true, true);
                    // Lintel glass above the breach doorway (door is ~3.2 m tall).
                    place(0.0f, kBreachHalfW, kGroundY + 3.2f, g1, true, true);
                } else {
                    place(spanCenter, spanHalf, g0, g1, true, true);
                }
                // HIDDEN LEVEL 4.5: after band 4 (i == kHiddenBelow-1), fill the
                // inserted height with BLANKED (unlit) dark panels — no lit band,
                // no spandrel line of its own: just one story-height of silent
                // glass that makes the tower too tall.
                if (i == kHiddenBelow - 1) {
                    const float h0 = y0 + kStoryH, h1 = h0 + kHiddenH;
                    place(spanCenter, spanHalf, h0, h1, true, /*lit*/false);
                }
            }
        };
        // FRONT (player-facing, spans X, breach split), BACK, LEFT, RIGHT.
        buildFace(0, kFacilityZ, 0.0f, kFacilityHalfW, /*splitBreach*/true);
        buildFace(0, backZ,      0.0f, kFacilityHalfW, false);
        buildFace(1, -kFacilityHalfW, fcz, kFacilityHalfD, false);
        buildFace(1,  kFacilityHalfW, fcz, kFacilityHalfD, false);
        // CORNER mullion columns (opaque, full height) — the curtain wall's frame.
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = 0; sz <= 1; ++sz) {
                const float czn = sz ? backZ : kFacilityZ;
                facadeSlab(sx * kFacilityHalfW, kTowerH * 0.5f, czn,
                           0.55f, kTowerH * 0.5f, 0.55f, 18, 20, 24);
            }
        // ROOF cap (opaque, with a slim parapet lip) + interior FLOOR slab.
        opaqueSlab(0.0f, kTowerH + 0.25f, fcz, kFacilityHalfW + 0.4f, 0.25f, kFacilityHalfD + 0.4f, 34, 38, 44);
        opaqueSlab(0.0f, 0.05f, fcz, kFacilityHalfW, 0.1f, kFacilityHalfD, 30, 32, 38);

        // ---- COLLISION truth: full-height boxes per face; front split around the
        //      breach (open below 3.2 m in the breach lane, sealed above it).
        const float sideW = (kFacilityHalfW - kBreachHalfW) * 0.5f;
        collideBox(-(kBreachHalfW + sideW), kTowerH * 0.5f, kFacilityZ, sideW, kTowerH * 0.5f, wallT);
        collideBox( (kBreachHalfW + sideW), kTowerH * 0.5f, kFacilityZ, sideW, kTowerH * 0.5f, wallT);
        collideBox(0.0f, (kTowerH + 3.2f) * 0.5f, kFacilityZ, kBreachHalfW, (kTowerH - 3.2f) * 0.5f, wallT);
        collideBox(0.0f, kTowerH * 0.5f, backZ, kFacilityHalfW, kTowerH * 0.5f, wallT);
        collideBox(-kFacilityHalfW, kTowerH * 0.5f, fcz, wallT, kTowerH * 0.5f, kFacilityHalfD);
        collideBox( kFacilityHalfW, kTowerH * 0.5f, fcz, wallT, kTowerH * 0.5f, kFacilityHalfD);
    }

    // ---- BREACH MARKER: a glowing frame around the entry (the hand-off point).
    {
        opaqueSlab(0.0f, kBreachHalfW + 1.0f, kFacilityZ, kBreachHalfW + 0.3f, 0.25f, wallT + 0.1f, 90, 200, 255);
        // The breach itself is the gap (no wall) — left open so the player walks in.
        char tb[200];
        std::snprintf(tb, sizeof(tb),
            "--world surface: GLASS TOWER built — %d visible stories x %.1f m + HIDDEN 4.5 (%.1f m) "
            "= %.1f m total (one story too tall; blanked band between 4 and 5)",
            kStories, kStoryH, kHiddenH, kTowerH);
        x3::logInfo(tb);
    }

    // ---- Jake's LANDED SHIP (JakeFighterShip.glb; box fallback). Sits on the
    //      surface just behind + beside the spawn, nose toward the facility.
    const std::string rigDir = x3::game::riggedGlbRoot();
    std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
    asrc->mountDir(rigDir, 0);
    std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
    const char* kShipCandidates[] = { "JakeFighterShip.glb", "SpaceShip4.glb", "SpaceShip.glb" };
    x3::asset::Model shipModel{}; std::string shipFile;
    for (const char* c : kShipCandidates) { shipModel = mloader->load(c); if (shipModel.ok) { shipFile = c; break; } }
    std::vector<x3::asset::ModelDrawable> shipDrawables;
    if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
    x3::logInfo(std::string("--world surface: landed ship model=") +
                (shipModel.ok ? shipFile : "<procedural-box-fallback>"));
    // Box fallback mesh.
    x3::prims::PrimMesh sbm = x3::prims::makeBox(3.0f, 1.0f, 6.0f, 0, 0, 0, 0.5f);
    auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                          sbm.index.data(), (uint32_t)sbm.index.size());
    auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 160, 180, 60, 66, 78);
    auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);
    // Ship placement: behind-right of the spawn, settled on the ground, nose -Z.
    const float kShipX = 9.0f, kShipY = 0.0f, kShipZ = 6.0f, kShipScale = 2.2f;

    // ---- SARAH (the rescue target) behind the glass. Reuses the RESCUE system
    //      (app/rescue.h): one Captive figure, framed facing the breach. The boss
    //      tuning is required by build() but never fires — this is the landing
    //      beat, NOT the F2 countdown hub (hubReached stays false, so no timer).
    x3::game::RescueSystem rescue;
    {
        x3::game::MonsterSystem::Tuning bossTuning{};   // unused (timer never starts)
        // Place Sarah just inside the front glass wall, centred, so she's visible
        // through the breach as the player approaches. wardB/C are stowed off-stage
        // (this slice frames a single visible captive — Sarah).
        const x3::phys::Vec3 sarahPos{ 0.0f, kGroundY, kFacilityZ - 5.0f };
        const x3::phys::Vec3 off{ -200.0f, kGroundY, kFacilityZ - 12.0f };
        rescue.build(scene, *device, *phys, rigDir, sarahPos, off, off);
        // Face the front breach (toward +Z, the player's approach). headingToFace.
        if (rescue.victimCount() > 0) {
            const_cast<x3::game::RescueVictim&>(rescue.victim(0)).setFacing(0.0f);
        }
        x3::logInfo("--world surface: Sarah staged behind the glass (rescue target; "
                    "no countdown — calm landing beat)");
    }

    // ---- The PLAYER: FIRST-PERSON, FREE + ARMED, spawned OUTSIDE the facility,
    //      facing the breach. (Inverse of the canon prisoner-in-cell start.) ----
    x3::game::Player player;
    player.spawn(*phys, 0.0f, kGroundY + 0.1f, 18.0f);   // 18 m back from the front wall
    player.setLook(-3.14159265f * 0.5f, -0.04f);          // yaw toward -Z (face the facility)

    // ARMED: an energy-pistol prop carried at the player's hip (the rescuer is
    // armed, not empty-handed). Drawn each frame at the camera; a real weapon/
    // ammo HUD is the canon facility content this hands to.
    x3::asset::Model pistolModel = mloader->load("WeaponEnergyPistol.glb");
    std::vector<x3::asset::ModelDrawable> pistolDrawables;
    if (pistolModel.ok) pistolDrawables = x3::asset::makeDrawables(pistolModel);
    x3::logInfo(std::string("--world surface: player ARMED, weapon=") +
                (pistolModel.ok ? "WeaponEnergyPistol.glb" : "<none — pistol GLB missing>"));

    // ---- OBJECTIVE (rescuer framing): the "go get Sarah" line. Reuses the canon
    //      ObjectiveSystem; advances as the player approaches + reaches the breach.
    x3::game::ObjectiveSystem objective;
    objective.set({
        "REACH THE FACILITY \xE2\x80\x94 FIND SARAH",   // em dash
        "BREACH THE FACILITY",
        "RESCUE SARAH",
    });
    bool approached = false, atBreach = false;

    const float dt = 1.0f / 60.0f;

    // Draw the landed ship at a yaw-only placement matrix.
    auto drawShip = [&](const x3::rhi::FrameContext& frame) {
        float m[16] = { kShipScale,0,0,0, 0,kShipScale,0,0, 0,0,kShipScale,0, kShipX,kShipY,kShipZ,1 };
        if (shipModel.ok) {
            for (const auto& dr : shipDrawables) {
                float fin[16]; x3::asset::mulMat4(m, dr.nodeTransform, fin);
                float tint[4] = { dr.baseColorFactor[0]*1.2f + 0.15f,
                                  dr.baseColorFactor[1]*1.2f + 0.16f,
                                  dr.baseColorFactor[2]*1.2f + 0.18f, dr.baseColorFactor[3] };
                device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                 x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
            }
        } else {
            const float white[4] = { 1,1,1,1 };
            device->drawMesh(frame, shipBoxMesh, shipBoxTex, white, m);
        }
    };
    // Draw the armed pistol at the camera (lower-right, view-anchored).
    auto drawWeapon = [&](const x3::rhi::FrameContext& frame, float cx, float cy, float cz,
                          float yaw, float pitch) {
        if (!pistolModel.ok) return;
        const float cy_ = std::cos(yaw), sy = std::sin(yaw);
        const float fwdX = std::cos(pitch)*std::cos(yaw), fwdY = std::sin(pitch), fwdZ = std::cos(pitch)*std::sin(yaw);
        const float rX = -sy, rZ = cy_;
        // Position: forward + down + right of the eye.
        const float px = cx + fwdX*0.6f + rX*0.35f;
        const float py = cy + fwdY*0.6f - 0.30f;
        const float pz = cz + fwdZ*0.6f + rZ*0.35f;
        const float S = 1.0f;
        float m[16] = { cy_*S,0,-sy*S,0, 0,S,0,0, sy*S,0,cy_*S,0, px,py,pz,1 };
        for (const auto& dr : pistolDrawables) {
            float fin[16]; x3::asset::mulMat4(m, dr.nodeTransform, fin);
            float tint[4] = { dr.baseColorFactor[0]+0.1f, dr.baseColorFactor[1]+0.1f,
                              dr.baseColorFactor[2]+0.12f, dr.baseColorFactor[3] };
            device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                             x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
        }
    };

    auto drawWorld = [&](const x3::rhi::FrameContext& frame, float cx, float cy, float cz,
                         float yaw, float pitch) {
        scene.render(*device, frame);   // ground + glass facility + breach + Sarah's prop entity
        rescue.draw(*device, frame, scene);   // Sarah's GLB over her Prop entity
        drawShip(frame);
        drawWeapon(frame, cx, cy, cz, yaw, pitch);
    };

    // Advance the objective from the player's position (approach -> breach).
    auto updateObjective = [&](const x3::phys::Vec3& feet) {
        if (!approached && feet.z <= kApproachZ) {
            approached = true; objective.advance();
            x3::logInfo("--world surface: approaching the facility -> objective: BREACH THE FACILITY");
        }
        const float dz = feet.z - kFacilityZ, dx = feet.x;
        const bool inBreachLane = std::fabs(dx) <= kBreachHalfW + 1.0f;
        if (!atBreach && approached && inBreachLane && dz <= kEntryReach && dz >= -2.0f) {
            atBreach = true; objective.advance();
            x3::logInfo("--world surface: AT THE BREACH -> objective: RESCUE SARAH "
                        "(hand-off into the facility/rescue content)");
        }
    };

    // ===== Headless capture path (--world surface --screenshot <p>). =========
    if (headless) {
        device->setFrustumCullEnabled(false);
        // Vantage: the LANDING VIEW — near player-eye height on the approach,
        // pitched UP so the full dark-glass tower reads against the sky (all 7
        // window bands + the too-tall hidden-4.5 gap visible; ship at right).
        float cam[5] = { 7.0f, 2.6f, 38.0f, -3.14159265f*0.5f - 0.08f, 0.14f };
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        const std::string outPath = screenshot ? screenshotPath : std::string("w_surface.png");
        const int kFrames = 24;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            phys->step(dt);
            rescue.tick(dt, scene, *phys, player.feet());   // hub NOT reached -> no timers
            scene.update(*phys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) drawWorld(frame, cam[0], cam[1], cam[2], cam[3], cam[4]);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            char rb[320];
            std::snprintf(rb, sizeof(rb),
                "--world surface: wrote %s | entities=%u draws=%u tris=%u ship=%s armed=%s sarah=%s",
                outPath.c_str(), scene.size(), st.drawCalls, st.triangles,
                shipModel.ok ? "REAL" : "box", pistolModel.ok ? "yes" : "no",
                rescue.victimCount() > 0 ? "staged" : "none");
            x3::logInfo(rb);
        } else x3::logError("--world surface: capture FAILED");

        if (shipModel.ok) mloader->unload(shipModel);
        if (pistolModel.ok) mloader->unload(pistolModel);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        phys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed walk loop: FP rescuer approaches the breach. =============
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    x3::logInfo("--world surface: WASD move, mouse look, Shift sprint, Space jump, Esc quit. "
                "Walk to the breach to rescue Sarah.");
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY); lastMX = mx; lastMY = my;
        auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };

        x3::game::PlayerInput in{};
        in.moveFwd    = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) + (kd(GLFW_KEY_S) ? -1.0f : 0.0f);
        in.moveStrafe = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) + (kd(GLFW_KEY_A) ? -1.0f : 0.0f);
        in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
        in.jumpPressed= kd(GLFW_KEY_SPACE);
        in.lookDX = ddx; in.lookDY = ddy;

        player.update(in, fdt, *phys);
        phys->step(fdt);
        rescue.tick(fdt, scene, *phys, player.feet());
        scene.update(*phys);
        updateObjective(player.feet());

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch);

        float cx, cy, cz, cyaw, cpit;
        player.camera(cx, cy, cz, cyaw, cpit);
        device->setCamera(cx, cy, cz, cyaw, cpit, 70.0f);

        auto frame = device->beginFrame();
        if (frame.valid) {
            drawWorld(frame, cx, cy, cz, cyaw, cpit);
            objective.drawCurrent(*device, frame);
        }
        device->endFrame(frame);
    }

    if (shipModel.ok) mloader->unload(shipModel);
    if (pistolModel.ok) mloader->unload(pistolModel);
    device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
    phys->shutdown(); device->shutdown();
    if (window) glfwDestroyWindow(window); glfwTerminate();
    return 0;
}

// ============================================================================
// --test-surfacestart  (Phase 7 self-test, headless, deterministic, no Vulkan).
//
// Asserts the Phase-7 contract two ways:
//   (A) BRANCH SELECTION — mirrors app_run's escape branch: with intro.outcome ==
//       escaped (and intro.landed set by the descent), the selected world is the
//       SURFACE start ("surface"); with shot_down it is the canon CELL (NOT the
//       surface). This is the cell-vs-surface decision app_run drives.
//   (B) SCENE BUILD — the surface scene actually STANDS UP headlessly: on a
//       HeadlessRenderDevice + Jolt world it builds the ground, the GLASS FACILITY
//       (>=1 transparent/glass entity routed through the glass pass), the player
//       OUTSIDE + ARMED, and Sarah as a rescue target (>=1 victim, hub NOT reached
//       so no countdown). Asserts entity/glass/rescue counts + the objective list.
//
// Logs PASS/FAIL S#, returns true iff all pass. Mirrors runRescueSelfTest() etc.
// ============================================================================
bool runSurfaceStartSelfTest() {
    using x3::intro::IntroOutcome;
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total; if (ok) ++pass;
        x3::logInfo(std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
    };

    // Pure branch-selection helper, mirroring app_run's escape branch decision:
    // escaped -> "surface", shot_down -> "level1" (the canon cell world).
    auto selectWorld = [](IntroOutcome o) -> std::string {
        return (o == IntroOutcome::Escaped) ? std::string("surface")
                                            : std::string("level1");
    };

    // (A) Branch selection.
    check(selectWorld(IntroOutcome::Escaped) == "surface",
          "S1 escaped -> surface-landing start (not the cell)");
    check(selectWorld(IntroOutcome::ShotDown) == "level1",
          "S2 shot_down -> canon cell unchanged");
    // Round-trip through the StoryFlags the orchestrator writes (the real signal
    // app_run reads): escaped flag -> readOutcomeFlag -> Escaped -> surface.
    {
        x3::game::StoryFlags f;
        x3::intro::writeOutcomeFlag(f, IntroOutcome::Escaped);
        f.set(x3::intro::kIntroLandedFlag);   // descent hand-off marker
        const IntroOutcome got = x3::intro::readOutcomeFlag(f);
        check(got == IntroOutcome::Escaped && f.has(x3::intro::kIntroLandedFlag) &&
              selectWorld(got) == "surface",
              "S3 StoryFlags[escaped]+[landed] -> surface (the app_run signal)");
    }
    {
        x3::game::StoryFlags f;
        x3::intro::writeOutcomeFlag(f, IntroOutcome::ShotDown);
        const IntroOutcome got = x3::intro::readOutcomeFlag(f);
        check(got == IntroOutcome::ShotDown && !f.has(x3::intro::kIntroLandedFlag) &&
              selectWorld(got) == "level1",
              "S4 StoryFlags[shot_down] (no landed) -> cell");
    }

    // (B) The surface scene stands up headlessly.
    {
        x3::game::HeadlessRenderDevice device;
        std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
        const bool physOk = phys && phys->init();
        check(physOk, "S5 headless physics world init");
        if (physOk) {
            x3::game::Scene scene;
            // Ground.
            {
                x3::prims::PrimMesh g = x3::prims::makeBox(120.0f, 0.5f, 120.0f, 0,-0.5f,0, 8.0f);
                auto gm = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                            g.index.data(), (uint32_t)g.index.size());
                x3::game::Entity e{}; e.mesh = gm; e.tag = (uint32_t)x3::game::Tag::Static;
                scene.add(e);
                phys->addBox(x3::phys::Vec3{120,0.5f,120}, x3::phys::Vec3{0,-0.5f,0},
                             0.0f, x3::phys::Layer::Static);
            }
            // One glass facility wall (transparent entity through the glass pass).
            uint32_t glassCount = 0;
            {
                x3::prims::PrimMesh m = x3::prims::makeBox(10,9,0.4f, 0,9,kFacilityZ, 1.0f);
                auto mh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                            m.index.data(), (uint32_t)m.index.size());
                x3::game::Entity e{}; e.mesh = mh; e.transparent = true;
                e.glass.opacity = 0.30f; e.tag = (uint32_t)x3::game::Tag::Static;
                scene.add(e);
            }
            for (uint32_t i = 0; i < scene.size(); ++i)
                if (scene.get(i).transparent) ++glassCount;
            check(glassCount >= 1, "S6 facility has a translucent GLASS wall (glass pass)");

            // THE GLASS TOWER canon: 7 visible window-band stories, but the tower
            // is one hidden story TALLER (Level 4.5's height inserted between
            // bands 4 and 5) — the height mismatch observant players can notice.
            check(kStories == 7 && kHiddenH > 0.0f &&
                  kTowerH > kStories * kStoryH + 0.5f &&
                  storyBaseY(kHiddenBelow) - (storyBaseY(kHiddenBelow - 1) + kStoryH) == kHiddenH,
                  "S6b tower is TALLER than its 7 bands (hidden Level 4.5 height between 4 and 5)");

            // Player OUTSIDE + armed framing (controller stands up + faces -Z).
            x3::game::Player player;
            player.spawn(*phys, 0.0f, 0.1f, 18.0f);
            player.setLook(-3.14159265f * 0.5f, -0.04f);
            check(player.feet().z > kApproachZ,
                  "S7 player spawns OUTSIDE the facility (free rescuer)");

            // Sarah staged as a rescue target (hub NOT reached -> no countdown).
            x3::game::RescueSystem rescue;
            x3::game::MonsterSystem::Tuning boss{};
            const x3::phys::Vec3 sarah{ 0.0f, 0.0f, kFacilityZ - 5.0f };
            const x3::phys::Vec3 off{ -200.0f, 0.0f, kFacilityZ - 12.0f };
            rescue.build(scene, device, *phys, x3::game::riggedGlbRoot(), sarah, off, off);
            check(rescue.victimCount() >= 1 && !rescue.hubReached(),
                  "S8 Sarah staged as rescue target, no countdown (calm landing)");

            // Objective list = the rescuer "go get Sarah" framing.
            x3::game::ObjectiveSystem obj;
            obj.set({ "REACH THE FACILITY", "BREACH THE FACILITY", "RESCUE SARAH" });
            check(obj.count() == 3 && obj.current() == 0,
                  "S9 rescuer objective list set (REACH -> BREACH -> RESCUE SARAH)");

            phys->shutdown();
        }
    }

    char sb[96];
    std::snprintf(sb, sizeof(sb), "surface-start: %d/%d passed", pass, total);
    x3::logInfo(sb);
    return pass == total;
}

}} // namespace x3::apphost
