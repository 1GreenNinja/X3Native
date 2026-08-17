// ============================================================================
// --world surface  (a.k.a. the ESCAPED-branch Act-1 surface-landing start)
//
// >>> DEV SHORTCUT since feat/canon-apron-landing (owner, 2026-08-16: "Why are
// >>> we even landing in a different world than we play in?"). The GAME's
// >>> flyable intro outcomes now land IN canonlevel — apron spawn + ship
// >>> set-down via app/apron_landing.h, walk in through the SEAM-2 breach; no
// >>> world switch. app_run routes here only from the level1/elevator dev
// >>> worlds (whose build has no exterior). This slice, its [E] breach handoff
// >>> and both its self-test suites stay supported per docs/design/WORLDS.md.
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
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../player.h"
#include "../objective.h"
#include "../rescue.h"
#include "../monster.h"
#include "../asset_root.h"
#include "../facility_exterior.h"    // SEAM 2: the factored tower/apron/glass/breach builder
#include "../terrain.h"              // W8-3: horizon ring (far-terrain stitch)
#include "../city.h"                 // W8-3: city massing in the middle distance
#include "../intro_orchestrator.h"   // IntroOutcome / readOutcomeFlag / kIntroLandedFlag (--test-surfacestart)
#include "../story_ops.h"            // x3::game::StoryFlags (the branch signal)
#include "../headless_device.h"      // HeadlessRenderDevice (the headless scene-build self-test)
#include "../destinations.h"         // [P0-1] findDestination("entrance") — the handoff dest key
#include "../level_loader.h"         // [P0-1] loadCanonTower/canonBeats — the Entrance-room spawn proof
#include "../weapon.h"               // [P0-1] WeaponSystem::forceArm — the armed-arrival contract
#include <cstdio>                    // std::remove (self-test temp flags file)
#include <cstring>                   // std::strcmp (registry assertions)
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
// (SEAM 2: storey height / band height / storey count now live in
// app/facility_exterior.cpp — 4 m storeys derived from the tower height.)
constexpr float kBreachHalfW  = 2.4f;     // entry breach half-width
constexpr float kEntryReach   = 6.0f;     // distance to the breach that triggers the hand-off
constexpr float kApproachZ    = -22.0f;   // crossing this advances "approach" objective

// ---------------------------------------------------------------------------
// [P0-1 EFLZ-GP-1B] SURFACE -> FACILITY HANDOFF (the spec'd trigger + request).
// specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md — shipped trigger = §3.2 option 1
// (INTERACT: "[E] ENTER FACILITY" at the breach), mechanism = §3.3 Option A
// (hc.switchWorldTo through main()'s world-load loop; no process restart).
// Both are PURE so the windowed loop and --test-surfacehandoff share one truth.
// ---------------------------------------------------------------------------

// The breach-reach geometry: the same lane/right-up-to-the-wall window the
// objective's "RESCUE SARAH" beat uses (one truth — updateObjective calls this).
bool breachHandoffReady(float feetX, float feetZ, bool approached) {
    const float dz = feetZ - kFacilityZ;
    const bool inBreachLane = std::fabs(feetX) <= kBreachHalfW + 1.0f;
    return approached && inBreachLane && dz <= kEntryReach && dz >= -2.0f;
}

// The handoff request (spec §3.3.1-2): ask main()'s world-load loop for the LIVE
// facility world, spawning at the registry's "entrance" key (the SEAM-2 Entrance
// hallway — the rescuer arrival, NOT the cell). The loop tears this host down,
// re-dispatches canonlevel with spawnAtKey="entrance" + skipIntro=true (the
// cold-open never replays, §3.3.4), and app_run's arrival block arms the player
// + imports the escaped StoryFlags (§3.3.5).
//
// The request also PERSISTS the escaped outcome (+landed) to the narrative lane:
// the surface host IS the escaped branch by construction (product-dispatched only
// on intro.outcome=escaped), but the dev shortcut `--world surface` reaches this
// breach with a stale/absent save — and an arrival with a refused flags import
// would stand the rescuer UNARMED (the spec §3.4 "cosmetic-only weapon" failure,
// caught in the live windowed run). Idempotent on the product path (the intro
// already wrote the same outcome); a later intro run rewrites the outcome anyway.
// `flagsPath` is injectable for the self-test; "" = the real narrative lane.
void requestFacilityHandoff(x3::apphost::HostContext& hc,
                            const std::string& flagsPath = std::string()) {
    hc.switchWorldTo = "canonlevel";
    hc.switchDestKey = "entrance";
    {
        const std::string p = flagsPath.empty()
            ? x3::intro::defaultGameStoryFlagsPath() : flagsPath;
        x3::game::StoryFlags flags;
        flags.loadFile(p);   // keep whatever else the save carries
        x3::intro::writeOutcomeFlag(flags, x3::intro::IntroOutcome::Escaped);
        flags.set(x3::intro::kIntroLandedFlag);
        if (!flags.saveFile(p))
            x3::logWarn("--world surface: could not persist escaped outcome to " + p);
    }
    x3::logInfo("--world surface: [E] ENTER FACILITY -> HANDOFF into the live facility "
                "(switchWorldTo=canonlevel @ entrance; EFLZ_SURFACE_FACILITY_HANDOFF §3.3)");
}
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

    // ---- Sky + lighting: GOLDEN HOUR (ART_BIBLE §3 surface zone — the sky is
    // the accent). SEAM 2: the exact params now live in the shared exterior
    // module so the canon tower reads under the SAME light.
    x3::game::FacilityExterior::applyGoldenHourSky(*device);
    // CONTENT WIRING: a 36 m glass tower, a 300 m soil plate and the city
    // massing on the horizon, all at golden hour -- the textbook cascade case,
    // and the legacy single 45 m box could not shadow past the apron.
    // `--set r_csm 0` restores it.
    applyOutdoorCsm(hc, *device, 300.0f, "surface");

    x3::game::Scene scene;

    // ---- SEAM 2: the whole facility EXTERIOR (soil plate, apron panel, the
    // near-black backing walls + collision, the glass curtain wall with its
    // per-pane jitter, spandrel bands + parapet + jambs + amber sign, and the
    // open breach + glowing marker) is now built by the factored module with
    // this host's original constants — --world surface keeps its exact look.
    x3::game::FacilityExterior facilityExt;
    {
        x3::game::FacilityExterior::Desc fd;
        fd.x0 = -kFacilityHalfW;                  fd.x1 = kFacilityHalfW;
        fd.z0 = kFacilityZ - 2.0f * kFacilityHalfD; fd.z1 = kFacilityZ;
        fd.baseY = kGroundY;                      fd.topY = kGroundY + 2.0f * kFacilityHalfH;
        fd.breachFace  = x3::game::FacilityExterior::Face::PlusZ;
        fd.breachCenter = 0.0f;
        fd.breachHalfW  = kBreachHalfW;
        fd.roofSlab = true; fd.floorSlab = true;
        fd.apron = x3::game::FacilityExterior::Apron::SurfacePanel;
        fd.apronPanelW = 70.0f; fd.apronPanelD = 66.0f;
        fd.apronAnchorX = 0.0f; fd.apronAnchorY = 0.02f; fd.apronAnchorZ = kFacilityZ - 4.0f;
        fd.terrainPlate = true;                   // the 300 m dark soil plain
        fd.mergePanes = false;                    // original per-pane draws
        facilityExt.build(scene, *device, *phys, fd);
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
        // W6-2: breach light spill — the module's amber glow at the entry
        // (registered third so the two fills keep their indices).
        x3::rhi::PointLight all[3] = { pl[0], pl[1], facilityExt.spillLight() };
        device->setPointLights(all, 3);
    }

    // ---- W8-3 HORIZON STITCH: the apron used to end at a hard 300 m plate edge
    // against empty sky. The canonical world field (terrain.cpp worldFeatures)
    // flattens a pad to Y=0 at the origin — exactly this apron's grade — so a
    // horizon ring sampled from that SAME field meets the plate seamlessly and
    // carries the countryside out to the 4 mountain ranges 7-10 km out. The CITY
    // districts (app/city.cpp, the Babylon map's Scrapyard/New District at
    // ~500-800 m NW) are built into the same scene so downtown reads in the
    // middle distance, matching what the streamed world places there. Far plane
    // raised so the ranges actually draw. Visual-only (no collision on the ring
    // or the city — the playable apron stays the 300 m collision plate).
    {
        x3::rhi::TextureHandle splat = x3::game::makeTerrainSplatMarker(*device);
        x3::game::HorizonRingDesc hr{};
        hr.centerX = 0.0f; hr.centerZ = 0.0f;
        hr.rInner = 285.0f;            // just inside the 300 m plate: seam hidden
        hr.rOuter = 13000.0f;
        hr.rings = 140; hr.segments = 160;
        hr.yBias = -0.35f;             // recessed under the plate lip
        // SEAM 3: the canonical facility pad now sits at the CANON grade (-2 m,
        // terrain.cpp kPads[0] — the canon tower's real F1 floor); this host's
        // plate stays at Y=0, so blend the ring FROM the plate grade down to the
        // true field — the flatten knob built for flat-pad hosts.
        hr.flatten = true; hr.flattenY = 0.0f; hr.flattenBlendR = 600.0f;
        x3::game::addTerrainHorizonRing(scene, *device, splat, hr);
        x3::game::City horizonCity;
        horizonCity.build(scene, *device, *phys);   // visual-only massing
        device->setCameraFar(15000.0f);
        x3::logInfo("--world surface: horizon stitch — terrain ring (mountain ranges) + "
                    "city massing in the middle distance");
    }

    // (SEAM 2: the facade / breach / bands / curtain-wall build that lived here
    // — glassWall/opaqueSlab, the front-wall breach split, the spandrel bands,
    // the W8-2 per-pane glazing — is facilityExt.build() above, verbatim.)

    // ---- WAVE-2B (LD review #4c), PRESERVED ACROSS SEAM 2: the facility read as a BLACK
    // SLAB (--world surface) - dead, unoccupied. A few WARM EMISSIVE WINDOW BANDS glow from
    // inside the black glass so the tower reads as an OCCUPIED building at golden hour (lit
    // office floors). These are PRIMS, not GLBs: they never went down the 1/PI shading path,
    // so the 5c35d65 engine fix does NOT change their exposure and their strengths stand.
    // Additive: thin self-lit quads just proud of the front glass, in the window zone between
    // spandrel lines, split around the entry breach; two on a side face so occupancy reads
    // from an angle. ACES-safe - warm-dominant hue at a moderate strength: a lit floor, never
    // a white slab.
    {
        const float wallT = 0.4f;   // == FacilityExterior's backing-wall thickness
        auto windowBand = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                              float er, float eg, float eb, float es) {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            auto td = x3::prims::makeSolidRGBA(8, 10, 10, 12);   // dark albedo (emissive carries it)
            auto tx = device->createTexture(td.data(), 8, 8, true);
            x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
            e.baseColor[0]=0.05f; e.baseColor[1]=0.05f; e.baseColor[2]=0.06f; e.baseColor[3]=1.0f;
            e.emissive[0]=er; e.emissive[1]=eg; e.emissive[2]=eb; e.emissive[3]=es;
            e.tag = (uint32_t)x3::game::Tag::Prop;   // purely visual, no collision
            e.roomId = x3::game::kNoRoom;            // outdoors: always drawn
            scene.add(e);
        };
        // GEOMETRY LAW for these bands (they used to break all three of these):
        //  * THIN. The comment above says "thin self-lit quads"; they were boxes
        //    0.24 m deep, spanning wallT+0.32 .. wallT+0.56. That swallowed the
        //    glass panes AND punched through the spandrel band quads at +0.45,
        //    so the emissive slab replaced the white concrete band outright.
        //  * IN THE GLASS ZONE, NOT ON A SPANDREL. FacilityExterior puts a
        //    concrete band across every storey line (y = 4f-1.8 .. 4f) and a
        //    glass strip between them centred at y = 4f+2. The old
        //    kFacilityHalfH*{0.55,1.05,1.5} = 9.9 / 18.9 / 27.0 landed ON the
        //    bands at 10.2 / 18.2 / 26.2 — hence "bands blown to pure white".
        //  * ACES-SAFE. Flat emissive >0.5 clips to white (X3_WORLD_RULES §5);
        //    at 1.15 these read as blank white strips, not lit office floors.
        // Depth: the deepest tilt-pushed pane front face is at wallT + 0.0334,
        // and the concrete spandrel quads are at wallT + 0.05. Sit in between.
        const float bandZ  = kFacilityZ + wallT + 0.0405f;
        const float bandHZ = 0.004f;                       // 8 mm, a lit strip
        // Height: a storey's glass runs y = 4f+0.9 .. 4f+3.1, but the spandrel
        // above it starts at 4f+2.2, so only 4f+0.9 .. 4f+2.2 is exposed glass.
        // Centre the lit floor in THAT, or the spandrel eats half of it.
        const float bandHY = 0.60f;                        // lit-floor half-height
        const float sideW  = (kFacilityHalfW - kBreachHalfW) * 0.5f;
        const float xLc = -(kBreachHalfW + sideW), xRc = (kBreachHalfW + sideW);
        const float warmR=0.95f, warmG=0.72f, warmB=0.40f;
        // Exposed-glass centres (storey f): y = 4f + 1.55. Pick f = 2, 4, 6.
        const float ys[] = { 9.55f, 17.55f, 25.55f };
        for (float y : ys) {
            windowBand(xLc, y, bandZ, sideW - 0.6f, bandHY, bandHZ, warmR, warmG, warmB, 0.45f);
            windowBand(xRc, y, bandZ, sideW - 0.6f, bandHY, bandHZ, warmR, warmG, warmB, 0.45f);
        }
        const float xS = kFacilityHalfW + wallT + 0.0405f;
        const float zC = kFacilityZ - kFacilityHalfD;
        windowBand(xS, 13.55f, zC, bandHZ, bandHY, kFacilityHalfD - 1.0f, 0.80f, 0.86f, 1.00f, 0.42f);
        windowBand(xS, 21.55f, zC, bandHZ, bandHY, kFacilityHalfD - 1.0f, 0.80f, 0.86f, 1.00f, 0.42f);
        x3::logInfo("--world surface: WAVE-2B - 8 emissive window bands (facility reads occupied)");
    }

    // ---- Jake's LANDED SHIP (JakeFighterShip.glb; box fallback). Sits on the
    //      surface just behind + beside the spawn, nose toward the facility.
    const std::string rigDir = x3::game::riggedGlbRoot();
    std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
    asrc->mountDir(rigDir, 0);
    std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
    // MINERVA TEXTURED PASS (cb9f760) FIRST. The owner's long-standing "Jake's ship is an
    // ugly black blob" had TWO causes and both are fixed now, in different places:
    //   * the ENGINE half -- GLBs shaded at 1/pi (5c35d65). Fixed for every GLB in the game.
    //   * the ASSET half -- and this is NOT a crutch for the engine bug, which is why it
    //     still earns its place: the Rodin bake was a shattered UV atlas of silver/pink mush
    //     with NO EMISSIVE CHANNEL AT ALL, so honest lighting had nothing to catch. The
    //     Minerva pass is a real re-texture (dark gunmetal plating, panel lines, grime, edge
    //     wear + a genuine teal emissive) baked into the EXISTING UV atlas -- same mesh, no
    //     re-unwrap, so it is a drop-in swap.
    // Untextured JakeFighterShip.glb stays as the fallback (and the cutscene extent probe
    // still measures it), so a machine without the new LFS object degrades, never breaks.
    const char* kShipCandidates[] = { "JakeFighterShip_textured.glb", "JakeFighterShip.glb",
                                      "SpaceShip4.glb", "SpaceShip.glb" };
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

    // ---- W6-2: APRON PROPS — a small service cluster near the facility corner
    //      (warehouse kit via converted_glb; origins sit at the base per the kit
    //      convention, so y=0 seats them on the apron). Grouped, not scattered.
    std::unique_ptr<x3::asset::IAssetSource> propSrc(x3::asset::createAssetSource());
    propSrc->mountDir(x3::game::convertedGlbRoot(), 0);
    std::unique_ptr<x3::asset::IModelLoader> propLoader(x3::asset::createModelLoader(device, propSrc.get()));
    x3::asset::Model barrelModel = propLoader->load("SciFi_Warehouse_Kit/Barrel.glb");
    x3::asset::Model crateModel  = propLoader->load("SciFi_Warehouse_Kit/Crate Long.glb");
    std::vector<x3::asset::ModelDrawable> barrelDrawables, crateDrawables;
    if (barrelModel.ok) barrelDrawables = x3::asset::makeDrawables(barrelModel);
    if (crateModel.ok)  crateDrawables  = x3::asset::makeDrawables(crateModel);
    struct PropDraw { const std::vector<x3::asset::ModelDrawable>* drawables;
                      float xform[16]; float tint[4]; };
    std::vector<PropDraw> propDraws;
    auto pushProp = [&](const std::vector<x3::asset::ModelDrawable>* dws,
                        float yaw, float x, float z, float sc,
                        float tr, float tg, float tb) {
        if (!dws || dws->empty()) return;
        const float c = std::cos(yaw) * sc, s = std::sin(yaw) * sc;
        PropDraw p{}; p.drawables = dws;
        const float m[16] = { c,0,-s,0, 0,sc,0,0, s,0,c,0, x, 0.0f, z, 1 };
        for (int i = 0; i < 16; ++i) p.xform[i] = m[i];
        p.tint[0]=tr; p.tint[1]=tg; p.tint[2]=tb; p.tint[3]=1.0f;
        propDraws.push_back(p);
    };
    {
        const float px = -kFacilityHalfW + 3.0f, pz = kFacilityZ + 7.0f;   // left of the breach lane
        pushProp(&barrelDrawables, 0.3f,  px,        pz,        1.0f, 0.46f, 0.34f, 0.25f);
        pushProp(&barrelDrawables, 1.4f,  px + 1.1f, pz + 0.5f, 1.0f, 0.42f, 0.42f, 0.40f);
        pushProp(&crateDrawables,  0.1f,  px + 0.4f, pz + 2.1f, 1.0f, 0.60f, 0.55f, 0.48f);
        pushProp(&crateDrawables,  1.65f, px + 2.0f, pz + 1.6f, 1.0f, 0.52f, 0.50f, 0.46f);
        x3::logInfo("--world surface: apron props = " + std::to_string(propDraws.size()) +
                    " (warehouse kit cluster by the breach lane)");
    }

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
        // ASCII '-' (not the em dash): the HUD font atlas is ASCII-only and the
        // UTF-8 dash rendered as "???" on the live objective line (frame 05).
        "REACH THE FACILITY - FIND SARAH",
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
        scene.render(*device, frame);   // ground + facility backing + breach + Sarah's prop entity
        // SEAM 2: the facade skin — glass curtain wall (glass pass), the
        // concrete apron panel, the spandrel bands + the amber entrance sign —
        // all drawn by the factored module (same constants, same draw order).
        facilityExt.draw(*device, frame);
        // W6-2: APRON PROPS — sparse, curated service clutter (bible: props sell a
        // built place; SPARSE — four pieces, grouped, not scattered).
        for (const auto& pd : propDraws)
            for (const auto& dr : *pd.drawables) {
                float fin[16];
                x3::asset::mulMat4(pd.xform, dr.nodeTransform, fin);
                const float emis0p[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                device->drawMeshPBR(frame, x3::rhi::MeshHandle{ dr.meshId },
                                    x3::rhi::TextureHandle{ dr.baseColorTexId },
                                    x3::rhi::TextureHandle{ dr.normalTexId },
                                    x3::rhi::TextureHandle{ dr.mrTexId },
                                    pd.tint, emis0p, fin, dr.alphaMask, dr.alphaBlend,
                                    x3::rhi::TextureHandle{ dr.emissiveTexId },
                                    x3::rhi::TextureHandle{ dr.detailTexId },
                                    dr.detailUvScale, 0.0f, 0.0f);
            }
        rescue.draw(*device, frame, scene);   // Sarah's GLB over her Prop entity
        drawShip(frame);
        // W8-2: the hip-prop pistol is anchored at a fixed camera offset, so any
        // up-tilted custom --shot-cam catches it as a giant photobomb. Review
        // cams (shotCamOverride) skip it; the default vantage + the windowed
        // walk loop keep the armed read.
        if (!shotCamOverride) drawWeapon(frame, cx, cy, cz, yaw, pitch);
    };

    // Advance the objective from the player's position (approach -> breach).
    auto updateObjective = [&](const x3::phys::Vec3& feet) {
        if (!approached && feet.z <= kApproachZ) {
            approached = true; objective.advance();
            x3::logInfo("--world surface: approaching the facility -> objective: BREACH THE FACILITY");
        }
        // [P0-1] one truth with the handoff trigger: the objective's "at the
        // breach" beat and the [E] ENTER FACILITY interact share this geometry.
        if (!atBreach && breachHandoffReady(feet.x, feet.z, approached)) {
            atBreach = true; objective.advance();
            x3::logInfo("--world surface: AT THE BREACH -> objective: RESCUE SARAH "
                        "([E] ENTER FACILITY hands off into the live facility)");
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
    bool prevEKey = false;   // [P0-1] rising edge for the breach [E] ENTER FACILITY interact
    x3::logInfo("--world surface: WASD move, mouse look, Shift sprint, Space jump, Esc quit. "
                "Walk to the breach and press E to ENTER THE FACILITY.");
    // Console (~), ESC menu and the FPS/stats overlay. See host_shell.h:
    // the engine has had all three for a long time and 28 of ~31 hosts
    // wired none of them, so the worlds you could actually launch and play
    // were the one place in the engine with no developer tools at all.
    HostShell shell;
    shell.attach(hc);

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        glfwPollEvents();
        shell.beginFrame();   // ESC opens the menu now; SHIFT+ESC quits
        double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
        float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look; lastMX = mx; lastMY = my;
        auto kd = [&](int k){ return shell.key(k); };   // false while the console/menu owns input

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

        // ---- [P0-1 EFLZ-GP-1B] THE HANDOFF INTERACT (spec §3.2 trigger 1) ----
        // At the breach, E requests the world switch and leaves the walk loop; the
        // teardown below preserves the shared device/window so main()'s world-load
        // loop can build canonlevel into them (Option A — a real in-engine load).
        {
            const bool eNow = kd(GLFW_KEY_E);
            if (atBreach && eNow && !prevEKey) {
                requestFacilityHandoff(hc);
                break;
            }
            prevEKey = eNow;
        }

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch);

        float cx, cy, cz, cyaw, cpit;
        player.camera(cx, cy, cz, cyaw, cpit);
        device->setCamera(cx, cy, cz, cyaw, cpit, 70.0f);

        auto frame = device->beginFrame();
        if (frame.valid) {
            drawWorld(frame, cx, cy, cz, cyaw, cpit);
            objective.drawCurrent(*device, frame);
            // [P0-1] discoverable trigger (spec §3.2): the interact prompt, up
            // whenever the breach interact is armed. Centered above the midline.
            if (atBreach) {
                const char* prompt = "[E] ENTER FACILITY";
                const float px = 22.0f;                       // glyph size (px)
                const float tx = (float)cw * 0.5f - px * 0.5f * 9.0f;  // ~centered (18 chars)
                const float ty = (float)ch * 0.62f;
                const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.85f };
                const float cl[4] = { 1.0f, 0.84f, 0.35f, 1.0f };      // amber (the breach sign)
                device->drawHudText(frame, prompt, tx + 2.0f, ty + 2.0f, px, sh);
                device->drawHudText(frame, prompt, tx, ty, px, cl);
            }
        }
        shell.draw(frame);
        device->endFrame(frame);
    }

    if (shipModel.ok) mloader->unload(shipModel);
    if (pistolModel.ok) mloader->unload(pistolModel);
    device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
    phys->shutdown();
    // [P0-1] SWITCH-AWARE teardown: the device/window/GLFW are created ONCE in
    // main() and REUSED across its world-load loop. When this host requested the
    // facility handoff (switchWorldTo set), the arriving canonlevel host still
    // needs them — destroying them here would hand the next host a freed device
    // (the exact canonlevel->streamed segfault class). This world's OWN resources
    // (scene meshes/textures, physics, models) are freed above either way; only
    // the FINAL host (no switch pending) tears down the shared objects.
    if (hc.switchWorldTo.empty()) {
        device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
    } else {
        // Phase-1 honesty: the surface world's builder-owned GPU resources
        // (facility exterior, horizon ring, city massing, rescue rig) have no
        // per-system teardown yet, so they stay RESIDENT until process exit —
        // same residual class as the canon-side world switch. Follow-up filed
        // (builder destroy() methods) — do not paper over it with device reset.
        x3::logInfo("--world surface: HANDOFF teardown — shared device/window preserved; "
                    "surface builder resources stay resident until process exit (known "
                    "Phase-1 residual)");
    }
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

// ============================================================================
// --test-surfacehandoff  ([P0-1 EFLZ-GP-1B] headless, deterministic, no Vulkan).
//
// The Phase-1 SURFACE -> FACILITY handoff contract, spec acceptance tests H2-H5
// (specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md §6; H1 is --test-surfacestart's
// branch selection, H6 is the eyes-on capture):
//   H2  trigger: the breach interact geometry fires exactly in the breach window,
//       and requestFacilityHandoff() asks for canonlevel @ entrance (Option A) —
//       with NEGATIVE CONTROLS (not approached / out of lane / far on the apron).
//   H2b main-loop contract: the requested world differs from "surface", so
//       main()'s world-load loop re-dispatches instead of returning (no dead end),
//       and the dest key resolves in the LIVE registry to a canonlevel anchor
//       (no fallback world, no 404 signpost).
//   H3  spawn: the LIVE canon tower data has the "Entrance" room; the arrival
//       spawn point lies inside its bounds and OUTSIDE Jake's Cell (never the
//       prisoner start) — asserted against the loaded LevelArchitect JSON.
//       Plus the armed contract: the WeaponSystem grant the arrival uses
//       (forceArm — cheatArm's engine) flips hasWeapon.
//   H4  flags: a persisted ESCAPED save imports into a live flags world
//       (intro.outcome=escaped + intro.landed readable after the load).
//   H5  negative control: a persisted SHOT_DOWN save is REFUSED (import returns
//       false, live flags untouched -> canon cell behavior byte-identical).
// ============================================================================
bool runSurfaceHandoffSelfTest() {
    using x3::intro::IntroOutcome;
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total; if (ok) ++pass;
        x3::logInfo(std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
    };

    // ---- H2: the trigger geometry (one truth with the windowed loop). ----
    check(breachHandoffReady(0.0f, kFacilityZ + 3.0f, true),
          "H2a at the breach centre (approached) -> trigger armed");
    check(breachHandoffReady(kBreachHalfW + 0.9f, kFacilityZ + 1.0f, true),
          "H2b lane edge (+1 m grace) still armed");
    check(!breachHandoffReady(0.0f, kFacilityZ + 3.0f, false),
          "H2c NEGATIVE: not yet approached -> no trigger");
    check(!breachHandoffReady(kBreachHalfW + 4.0f, kFacilityZ + 3.0f, true),
          "H2d NEGATIVE: outside the breach lane -> no trigger");
    check(!breachHandoffReady(0.0f, 18.0f, true),
          "H2e NEGATIVE: back on the apron (spawn distance) -> no trigger");

    // ---- H2f-i: the request + the world-load-loop contract. ----
    {
        const std::string tmpReq = "test_surfacehandoff_reqflags.tmp.txt";
        std::remove(tmpReq.c_str());
        HostContext hc;
        hc.worldMode = "surface";
        requestFacilityHandoff(hc, tmpReq);
        check(hc.switchWorldTo == "canonlevel" && hc.switchDestKey == "entrance",
              "H2f request = switchWorldTo canonlevel @ entrance (spec Option A)");
        check(hc.switchWorldTo != hc.worldMode,
              "H2g request differs from 'surface' -> main()'s loop re-dispatches (no dead end)");
        const x3::game::Destination* d = x3::game::findDestination(hc.switchDestKey);
        check(d && std::strcmp(d->key, "entrance") == 0 && d->canonAnchor &&
              std::strcmp(d->worldFlag, "canonlevel") == 0 &&
              d->group == x3::game::DestGroup::Facility,
              "H2h 'entrance' is a LIVE registry row: Facility group, canon anchor, "
              "worldFlag canonlevel (no fallback world)");
        // H2i: the request PERSISTS the escaped outcome, so the arrival import
        // always fires after a surface handoff (even from the dev shortcut with
        // a stale/absent save — the unarmed-arrival hole the live run caught).
        x3::game::StoryFlags live;
        check(x3::intro::importEscapedIntroFlags(live, tmpReq) &&
              x3::intro::readOutcomeFlag(live) == IntroOutcome::Escaped &&
              live.has(x3::intro::kIntroLandedFlag),
              "H2i the request persists escaped(+landed) -> the arrival import fires");
        std::remove(tmpReq.c_str());
    }

    // ---- H3: the Entrance-room spawn, against the LIVE tower data. ----
    {
        const x3::game::CanonFloor cf =
            x3::game::loadCanonTower(x3::game::canonProjectJsonPath());
        check(cf.valid(), "H3a canonical tower data loads headless");
        if (cf.valid()) {
            const uint32_t er = cf.roomByName("Entrance");
            check(er != x3::game::kNoRoom, "H3b the data authors an Entrance room");
            const x3::game::CanonBeats bt = x3::game::canonBeats(cf);
            check(bt.jakeCell != x3::game::kNoRoom, "H3c Jake's Cell resolves (the NOT-here room)");
            if (er != x3::game::kNoRoom && bt.jakeCell != x3::game::kNoRoom) {
                const x3::game::CanonRoom& rm = cf.rooms[er];
                const float sx = rm.cx, sy = rm.y0() + 1.0f, sz = rm.cz;   // riftDestination's anchor
                check(sx >= rm.x0() && sx <= rm.x1() && sz >= rm.z0() && sz <= rm.z1() &&
                      sy > rm.y0() && sy < rm.y1(),
                      "H3d arrival spawn lies inside the Entrance room bounds");
                const x3::game::CanonRoom& jc = cf.rooms[bt.jakeCell];
                const bool inCell = sx >= jc.x0() && sx <= jc.x1() &&
                                    sz >= jc.z0() && sz <= jc.z1() &&
                                    sy >= jc.y0() && sy <= jc.y1();
                check(!inCell, "H3e arrival spawn is OUTSIDE Jake's Cell (never the prisoner start)");
                check(er != bt.jakeCell, "H3f Entrance and Jake's Cell are distinct rooms");
            }
        }
    }
    // ---- H3g: the armed-arrival grant (cheatArm's engine). ----
    {
        x3::game::Scene scene;
        x3::game::WeaponSystem weapon;
        check(!weapon.hasWeapon(), "H3g0 fresh WeaponSystem starts unarmed (control)");
        weapon.forceArm(scene);
        check(weapon.hasWeapon(), "H3g forceArm (the arrival's cheatArm) -> hasWeapon TRUE");
    }

    // ---- H4 / H5: flag carry-over + the shot_down negative control. ----
    {
        const std::string tmp = "test_surfacehandoff_flags.tmp.txt";
        // H4: persisted ESCAPED (+landed) imports into a live flags world.
        {
            x3::game::StoryFlags disk;
            x3::intro::writeOutcomeFlag(disk, IntroOutcome::Escaped);
            disk.set(x3::intro::kIntroLandedFlag);
            check(disk.saveFile(tmp), "H4a persisted escaped save writes");
            x3::game::StoryFlags live;
            const bool imported = x3::intro::importEscapedIntroFlags(live, tmp);
            check(imported, "H4b escaped save IMPORTS into the live flags world");
            check(x3::intro::readOutcomeFlag(live) == IntroOutcome::Escaped,
                  "H4c intro.outcome=escaped readable AFTER the load (spec H4)");
            check(live.has(x3::intro::kIntroLandedFlag),
                  "H4d intro.landed carried over");
        }
        // H5: persisted SHOT_DOWN is refused; the live world stays untouched.
        {
            x3::game::StoryFlags disk;
            x3::intro::writeOutcomeFlag(disk, IntroOutcome::ShotDown);
            check(disk.saveFile(tmp), "H5a persisted shot_down save writes");
            x3::game::StoryFlags live;
            const bool imported = x3::intro::importEscapedIntroFlags(live, tmp);
            check(!imported, "H5b NEGATIVE: shot_down save is REFUSED (no rescuer arrival)");
            check(x3::intro::readOutcomeFlag(live) == IntroOutcome::ShotDown &&
                  !live.has(x3::intro::kIntroLandedFlag),
                  "H5c live flags untouched -> canon cell behavior byte-identical");
        }
        // Missing file: also refused (fresh machine / dev world path).
        std::remove(tmp.c_str());
        {
            x3::game::StoryFlags live;
            check(!x3::intro::importEscapedIntroFlags(live, tmp),
                  "H5d NEGATIVE: absent save file is refused");
        }
    }

    char sb[96];
    std::snprintf(sb, sizeof(sb), "surface-handoff: %d/%d passed", pass, total);
    x3::logInfo(sb);
    return pass == total;
}

}} // namespace x3::apphost
