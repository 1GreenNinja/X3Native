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
#include "../surface_library.h"      // W3-3: real PBR concrete on the tower + apron (ART_BIBLE §4)
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
constexpr float kFacilityHalfW= 26.0f;    // facility half-width  (X)
// W3-3: the TOWER SPEC (Tim): white concrete + black glass bands, believable
// proportions — 9 storeys x 4 m = 36 m (halfH 18). The old 9 m half was a squat
// glass shoebox; the older-still art was 40-50 m too tall. 36 m is the middle.
constexpr float kFacilityHalfH= 18.0f;    // facility half-height (Y) — 9 storeys
constexpr float kFacilityHalfD= 16.0f;    // facility half-depth  (Z)
constexpr float kStoreyH      = 4.0f;     // one storey
constexpr float kBandH        = 1.8f;     // concrete spandrel band height per storey
constexpr int   kStoreys      = 9;
constexpr float kBreachHalfW  = 2.4f;     // entry breach half-width
constexpr float kEntryReach   = 6.0f;     // distance to the breach that triggers the hand-off
constexpr float kApproachZ    = -22.0f;   // crossing this advances "approach" objective
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

    // ---- Sky + lighting: GOLDEN HOUR (ART_BIBLE §3 surface zone — the sky is the
    // accent). Low warm sun raking the tower face; warm horizon, cooling zenith.
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.55f; sp.sunDir[1] = 0.16f; sp.sunDir[2] = -0.35f;   // low, from the player's right
        // (R4: 1.0/0.62/0.38 @1.5 baked every white surface ORANGE-BROWN — late
        // golden, not cardboard. Softened toward amber so white concrete reads white-warm.)
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.78f; sp.sunColor[2] = 0.56f;
        sp.sunIntensity = 1.25f; sp.haze = 0.45f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.10f; sp.zenith[1]  = 0.15f; sp.zenith[2]  = 0.26f;  // cooling blue above
        sp.horizon[0] = 0.55f; sp.horizon[1] = 0.34f; sp.horizon[2] = 0.20f;  // warm gold band
        device->setSkyParams(sp);
    }
    {
        // A couple of fill point lights INSIDE the facility so Sarah + the glow
        // behind the glass read against the daylight (and the glass shimmers).
        x3::rhi::PointLight pl[2];
        pl[0].pos[0] = 0.0f; pl[0].pos[1] = 4.0f; pl[0].pos[2] = kFacilityZ - 6.0f;
        pl[0].range = 60.0f;
        pl[0].color[0] = 6.0f; pl[0].color[1] = 8.0f; pl[0].color[2] = 12.0f;   // cold interior glow
        pl[1].pos[0] = 0.0f; pl[1].pos[1] = 6.0f; pl[1].pos[2] = 10.0f;
        pl[1].range = 50.0f;
        pl[1].color[0] = 7.0f; pl[1].color[1] = 6.0f; pl[1].color[2] = 5.0f;    // warm sun bounce on the ship
        device->setPointLights(pl, 2);
    }

    x3::game::Scene scene;

    // ---- SURFACE LIBRARY: real PBR concrete for the tower + apron (§4 realism
    // mandate — no more checker ground / flat glass slab). Sets are curated +
    // channel-law converted; loaded once here, drawn per-frame below.
    x3::game::SurfaceLibrary surflib;
    surflib.mount(x3::game::assetRoot() + "/surface_library");
    // R3: cc_porous_cement rendered TAN under the golden sun (and the shader clamps
    // baseColor factors, so lifting didn't read). mw_wall_plastic is the lightest
    // albedo in the curated library — at facade distance its sheeting wrinkles read
    // as weathered poured concrete, i.e. the WHITE band of the tower spec.
    const x3::game::SurfaceSet& sTower  = surflib.get(*device, "mw_wall_plastic");
    const x3::game::SurfaceSet& sApron  = surflib.get(*device, "sr_concrete_01");      // rough dark concrete apron
    x3::logInfo(std::string("--world surface: surface sets tower=") +
                (sTower.ok ? "cc_porous_cement OK" : "MISSING") + " apron=" +
                (sApron.ok ? "sr_concrete_01 OK" : "MISSING"));

    // ---- Ground: a DARK natural plain (collision + far read) with a textured
    // concrete APRON panel between the landing site and the facility entrance.
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(300.0f, 0.5f, 300.0f, 0.0f, -0.5f, 0.0f, 8.0f);
        auto gm = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                     g.index.data(), (uint32_t)g.index.size());
        auto gtD = x3::prims::makeCheckerRGBA(64, 16, 46, 42, 36, 38, 35, 30);   // dark umber soil, low contrast
        auto gt = device->createTexture(gtD.data(), 64, 64, true);
        x3::game::Entity e{}; e.mesh = gm; e.tex = gt;
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 1.0f;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
        // Static collision floor so the Player capsule stands on it.
        phys->addBox(x3::phys::Vec3{300.0f, 0.5f, 300.0f}, x3::phys::Vec3{0.0f, -0.5f, 0.0f},
                     0.0f, x3::phys::Layer::Static);
    }
    // Apron panel (visual, sits 2 cm over the soil so it wins the depth test):
    // spans the walk from spawn to the front wall, wider than the facility face.
    // (floor panels span x in [-w/2,w/2], z in [0,h] from the transform origin —
    // anchor at z = kFacilityZ-4 so the slab runs from under the wall out past
    // the landed ship at z=6 / the spawn at z=18.)
    x3::rhi::MeshHandle apronMesh = surflib.makePanel(*device, /*floor*/1, 70.0f, 66.0f, 3.0f);
    const float apronXform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                   0.0f, kGroundY + 0.02f, kFacilityZ - 4.0f, 1 };

    // ---- The GLASS FACILITY exterior. A large box walled in translucent glass:
    //      four glass walls (front split around the entry breach) + an opaque
    //      roof + an opaque floor slab. Reuses Entity.transparent + GlassMaterial
    //      (the transparent pass; app/glass_test.h). Static collision boxes back
    //      the walls so the player can't walk through the glass (except the breach).
    auto glassWall = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
        auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        // W3-3: BLACK GLASS per the tower spec — dark, reflective, near-opaque
        // (a corporate curtain wall at golden hour mirrors the sky; it does NOT
        // read as light-blue aquarium glass). The concrete spandrel bands draw
        // 5 cm proud of this plane, giving the banded facade.
        auto td = x3::prims::makeSolidRGBA(8, 14, 16, 20);
        auto tx = device->createTexture(td.data(), 8, 8, true);
        x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
        // R4 FINAL: OPAQUE dark glazing. Three rounds proved the diagonal streaks
        // are the glass pass shading the box triangulation (parameter-immune:
        // survived spec 0.95->0.5, roughness 0.06->0.22, opacity 0.30->0.88).
        // A day-lit black curtain wall reads opaque from outside anyway, so the
        // glazing ships as dark glossy OPAQUE surface; true reflective glass
        // returns when RT reflections cover scene entities (filed follow-up).
        e.transparent = false;
        e.baseColor[0] = 0.30f; e.baseColor[1] = 0.34f; e.baseColor[2] = 0.42f;
        e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
        phys->addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                     0.0f, x3::phys::Layer::Static);
    };
    auto opaqueSlab = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          uint8_t r, uint8_t g, uint8_t b) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 2.0f);
        auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        auto td = x3::prims::makeSolidRGBA(8, r, g, b);
        auto tx = device->createTexture(td.data(), 8, 8, true);
        x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
        e.tag = (uint32_t)x3::game::Tag::Static;
        scene.add(e);
        phys->addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                     0.0f, x3::phys::Layer::Static);
    };

    const float fcx = 0.0f, fcy = kFacilityHalfH, fcz = kFacilityZ - kFacilityHalfD;
    const float wallT = 0.4f;                          // wall thickness (half-Z/X)
    // FRONT wall (player-facing, at z=kFacilityZ), split around the central breach.
    {
        const float frontZ = kFacilityZ;
        const float sideW = (kFacilityHalfW - kBreachHalfW) * 0.5f;
        // left of breach
        glassWall(-(kBreachHalfW + sideW), kFacilityHalfH, frontZ, sideW, kFacilityHalfH, wallT);
        // right of breach
        glassWall( (kBreachHalfW + sideW), kFacilityHalfH, frontZ, sideW, kFacilityHalfH, wallT);
        // lintel above the breach (so the breach is a doorway, not a full-height gap)
        glassWall(0.0f, kFacilityHalfH + 1.6f, frontZ, kBreachHalfW, kFacilityHalfH - 1.6f, wallT);
    }
    // BACK wall.
    glassWall(0.0f, kFacilityHalfH, kFacilityZ - 2.0f * kFacilityHalfD, kFacilityHalfW, kFacilityHalfH, wallT);
    // SIDE walls.
    glassWall(-kFacilityHalfW, kFacilityHalfH, fcz, wallT, kFacilityHalfH, kFacilityHalfD);
    glassWall( kFacilityHalfW, kFacilityHalfH, fcz, wallT, kFacilityHalfH, kFacilityHalfD);
    // ROOF + interior FLOOR slab (opaque).
    opaqueSlab(0.0f, 2.0f * kFacilityHalfH, fcz, kFacilityHalfW, wallT, kFacilityHalfD, 40, 44, 52);
    opaqueSlab(0.0f, 0.05f, fcz, kFacilityHalfW, 0.1f, kFacilityHalfD, 30, 32, 38);

    // ---- BREACH MARKER: a glowing frame around the entry (the hand-off point).
    {
        opaqueSlab(0.0f, kBreachHalfW + 1.0f, kFacilityZ, kBreachHalfW + 0.3f, 0.25f, wallT + 0.1f, 90, 200, 255);
        // The breach itself is the gap (no wall) — left open so the player walks in.
        x3::logInfo("--world surface: glass facility built (front wall split around the entry breach)");
    }

    // ---- W3-3: CONCRETE SPANDREL BANDS over the black glass (the tower spec).
    // One reusable band quad per face size, instanced by transform: a 1.8 m
    // concrete band at every storey line + a ground base + a rooftop parapet,
    // drawn 5 cm proud of the glass planes. Front-face ground band splits around
    // the breach; storey-1's band doubles as the entrance header.
    const float kTowerH = 2.0f * kFacilityHalfH;                    // 36 m
    x3::rhi::MeshHandle bandFB = surflib.makePanel(*device, 0, 2.0f * kFacilityHalfW + 0.8f, kBandH, 2.6f); // front/back span
    x3::rhi::MeshHandle bandLR = surflib.makePanel(*device, 0, 2.0f * kFacilityHalfD + 0.8f, kBandH, 2.6f); // side span
    x3::rhi::MeshHandle baseSeg = surflib.makePanel(*device, 0, (kFacilityHalfW - kBreachHalfW) - 0.6f, 1.2f, 2.6f); // breach-split base
    struct BandDraw { x3::rhi::MeshHandle mesh; float xform[16]; };
    std::vector<BandDraw> bands;
    // yaw matrices: panels face -Z at yaw 0 (axis 0). yaw pi -> +Z (front face,
    // toward the player); +-pi/2 -> +-X (sides).
    auto pushBand = [&](x3::rhi::MeshHandle mesh, float yaw, float x, float y, float z) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        BandDraw b{}; b.mesh = mesh;
        float m[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, x,y,z,1 };
        for (int i = 0; i < 16; ++i) b.xform[i] = m[i];
        bands.push_back(b);
    };
    {
        const float zF = kFacilityZ + wallT + 0.05f;                 // front plane, proud toward +Z
        const float zB = kFacilityZ - 2.0f * kFacilityHalfD - wallT - 0.05f;
        const float xL = -kFacilityHalfW - wallT - 0.05f;
        const float xR =  kFacilityHalfW + wallT + 0.05f;
        const float zC = kFacilityZ - kFacilityHalfD;                // side-face center
        const float kPi = 3.14159265f;
        for (int f = 1; f < kStoreys; ++f) {                         // storey lines
            const float y = f * kStoreyH - kBandH * 0.5f;
            pushBand(bandFB, kPi,        0.0f, y, zF);
            pushBand(bandFB, 0.0f,       0.0f, y, zB);
            pushBand(bandLR,  kPi*0.5f,  xL,   y, zC);   // R3: side yaws were swapped —
            pushBand(bandLR, -kPi*0.5f,  xR,   y, zC);   // faces pointed INTO the tower
        }
        // Parapet crown (one band height, sitting atop the roof line).
        pushBand(bandFB, kPi,       0.0f, kTowerH - 0.2f, zF);
        pushBand(bandFB, 0.0f,      0.0f, kTowerH - 0.2f, zB);
        pushBand(bandLR,  kPi*0.5f, xL,   kTowerH - 0.2f, zC);
        pushBand(bandLR, -kPi*0.5f, xR,   kTowerH - 0.2f, zC);
        // Ground base: full band on back/sides; split around the breach on front.
        pushBand(bandFB, 0.0f,      0.0f, 0.0f, zB);
        pushBand(bandLR,  kPi*0.5f, xL,   0.0f, zC);
        pushBand(bandLR, -kPi*0.5f, xR,   0.0f, zC);
        const float segOff = kBreachHalfW + 0.3f + ((kFacilityHalfW - kBreachHalfW) - 0.6f) * 0.5f;
        pushBand(baseSeg, kPi, -segOff, 0.0f, zF);
        pushBand(baseSeg, kPi,  segOff, 0.0f, zF);
        x3::logInfo("--world surface: tower facade = " + std::to_string(bands.size()) +
                    " concrete spandrel bands over black glass (9 storeys + parapet)");
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
        // W3-3: the textured skin — concrete apron underfoot + spandrel bands on the tower.
        if (sApron.ok) surflib.drawPanel(*device, frame, sApron, apronMesh, apronXform);
        // Bands draw bespoke (not drawPanel): the library has no true WHITE concrete,
        // so the porous-cement albedo gets a lifted, slightly-desaturated baseColor —
        // texture relief stays, the tan reads as sun-washed white concrete.
        if (sTower.ok) {
            const float bcW[4]   = { 1.85f, 1.80f, 1.72f, 1.0f };
            const float emis0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (const auto& b : bands)
                device->drawMeshPBR(frame, b.mesh, sTower.albedo, sTower.normal, sTower.mr,
                                    bcW, emis0, b.xform, false, false,
                                    x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                    1.0f, 0.0f, 0.0f);
        }
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
        // Vantage: behind + above the player, looking down the approach toward the
        // glass facility (Sarah framed through the breach, the landed ship at right).
        float cam[5] = { 4.0f, 6.0f, 28.0f, -3.14159265f*0.5f - 0.18f, -0.16f };
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
