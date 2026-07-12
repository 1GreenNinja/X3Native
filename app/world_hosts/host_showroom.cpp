// --world showroom host (+ all --screenshot-showroom-* proofs) — lifted VERBATIM
// from main() (#28 deep split). The ONLY non-verbatim edit beyond the alias
// prelude: the 6 `device.get()` calls (main()'s device was a unique_ptr) are
// `device` here (a raw IRenderDevice*); semantically identical.
#include "world_host_common.h"
#include "../showroom_tod.h"
#include "../cinematic.h"        // NightSkyPlanet / loadNightSkyPlanets / drawNightSkyPlanets
#include "../scene.h"
#include "../mesh_prims.h"
#include "../env_art.h"
#include "../player.h"
#include "../monster.h"
#include "../npc_dialog.h"
#include "../rescue.h"
#include "../holo_terminal.h"
#include "../elevator.h"
#include "../level1_game.h"      // x3::game::KeypadEntry (the hatch keypad)
#include "../asset_root.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3 { namespace apphost {

// Showroom hidden-hatch override code (moved from main.cpp with the host; #28).
// Themed "ARIA" on a phone keypad (A=2,R=7,I=4,A=2). Exercised headless by
// --test-hatchcode (which keeps its own private copy in test_registry.cpp).
constexpr int kShowroomHatchCode = 2742;

int hostShowroom(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool ddgiForce = hc.ddgiForce;
    const bool showroomFpShot = hc.showroomFpShot;
    const std::string& showroomFpShotPath = hc.showroomFpShotPath;
    const bool showroomRagdollShot = hc.showroomRagdollShot;
    const std::string& showroomRagdollShotPath = hc.showroomRagdollShotPath;
    const bool showroomDeckShot = hc.showroomDeckShot;
    const std::string& showroomDeckShotPath = hc.showroomDeckShotPath;
    const bool showroomElevShot = hc.showroomElevShot;
    const std::string& showroomElevShotPath = hc.showroomElevShotPath;
    const bool showroomStairShot = hc.showroomStairShot;
    const std::string& showroomStairShotPath = hc.showroomStairShotPath;
    const bool showroomFloor2Shot = hc.showroomFloor2Shot;
    const std::string& showroomFloor2ShotPath = hc.showroomFloor2ShotPath;
    const bool showroomDoorShot = hc.showroomDoorShot;
    const std::string& showroomDoorShotPath = hc.showroomDoorShotPath;
    const bool showroomStrutsShot = hc.showroomStrutsShot;
    const std::string& showroomStrutsShotPath = hc.showroomStrutsShotPath;
    const bool showroomGalleryShot = hc.showroomGalleryShot;
    const std::string& showroomGalleryShotPath = hc.showroomGalleryShotPath;
    const bool showroomCivShot = hc.showroomCivShot;
    const std::string& showroomCivShotPath = hc.showroomCivShotPath;

    // ==== VERBATIM host body (device.get() -> device) ====
    if (worldMode == "showroom") {
        x3::logInfo("--world showroom: building the interactive night showroom walkthrough");

        // Physics world for the showroom (separate from the Level-1 path below).
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world showroom: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene sscene;

        // Load the baked Unity scene export (same GLB / path as --screenshot-showroom).
        x3::game::EnvArtSystem showroom;
        const bool envOk = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                                 "ShowRoom_Vol30/Example_01.glb");
        if (!envOk) x3::logError("--world showroom: scene GLB failed to load (floor + Aria still build)");

        // ---- DAY<->NIGHT toggle state -------------------------------------------------
        // gShowroomDay flips the whole sky/sun/ambient/bloom/interior-point-light recipe
        // via applyShowroomTimeOfDay(). Default = NIGHT (unchanged); X3_SHOWROOM_DAY=1
        // seeds DAY (for the headless proofs); the live loop flips it with the 'T' key.
        bool gShowroomDay = showroomDayDefault();
        // The CIVILIAN proof is a DAY shot by spec (the public-floors look is the
        // bright snow-bounce day grade) — force DAY regardless of the env default.
        if (showroomCivShot) gShowroomDay = true;

        // ---- Night-sky + lighting recipe (mirrors the --screenshot-showroom block) ----
        // The night `sp` is kept in scope as the BASE the ragdoll PROOF brightens (a
        // night-only debug shot). The live look is (re)applied by applyShowroomTimeOfDay
        // below for the chosen state — once for the headless proofs, and again on every
        // 'T' toggle in the interactive loop.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.42f; sp.sunDir[2] = -0.2f;   // low raking MOON
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // cool moonlight (still casts shadows)
        sp.haze = 0.15f; sp.exposure = 0.62f;
        sp.zenith[0]  = 0.012f; sp.zenith[1]  = 0.012f; sp.zenith[2]  = 0.028f;   // near-black zenith
        sp.horizon[0] = 0.10f;  sp.horizon[1] = 0.13f;  sp.horizon[2] = 0.20f;    // faint cool horizon
        // (sky/ambient/bloom are applied via applyShowroomTimeOfDay AFTER the interior
        // point lights are built — see the INTERIOR LIGHTING block.)
        // Disable the SSAO/GI depth PRE-PASS (the showroom uses alpha-cutout foliage that
        // the pre-pass can't discard — would punch sky holes; see the screenshot block).
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // Night-sky planets (shared helper; same files/order as --screenshot-nightsky).
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device, planetMesh, nPlanetTexFail, "--world showroom", &ringMesh);
        if (nPlanetTexFail > 0)
            x3::logWarn("--world showroom: " + std::to_string(nPlanetTexFail) + " planet texture(s) missing");

        // ---- Building footprint (engine-space) — same kBuild subset as the screenshot block.
        const std::vector<std::string> kBuild = {
            "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
            "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
        float bmn[3], bmx[3];
        const uint32_t nb = showroom.namedBounds(kBuild, bmn, bmx);
        if (nb == 0) { showroom.worldBounds(bmn, bmx); x3::logWarn("--world showroom: 0 named building nodes; using full scene bounds"); }
        x3::logInfo("--world showroom: building bounds (" + std::to_string(nb) + " nodes) min(" +
            std::to_string(bmn[0]) + "," + std::to_string(bmn[1]) + "," + std::to_string(bmn[2]) +
            ") max(" + std::to_string(bmx[0]) + "," + std::to_string(bmx[1]) + "," + std::to_string(bmx[2]) + ")");

        const float cx = (bmn[0] + bmx[0]) * 0.5f, cz = (bmn[2] + bmx[2]) * 0.5f;
        const float halfX = std::max(8.0f, (bmx[0] - bmn[0]) * 0.5f + 4.0f);   // footprint XZ half-extents (+ margin)
        const float halfZ = std::max(8.0f, (bmx[2] - bmn[2]) * 0.5f + 4.0f);

        // ---- SYNTHESIZE the GROUND floor. EnvArtSystem makes NO collision bodies + its
        // bounds are ORIGIN-only (not the true floor plane), so floorY is a TUNE POINT:
        // start at the building-bounds min Y. One large static slab sized to the footprint
        // XZ, its TOP surface at floorY (so player feet at floorY+eps stand on it). 1 m
        // thick (half-extent 0.5), centered 0.5 below floorY.  *** TUNE floorY HERE ***
        // SOLID now (no holes): the OWNER'S entrance is a HIDDEN WALL DOOR on the 2nd
        // floor — not a ground floor-hatch — and the glass elevator now boards in the
        // upper ATRIUM, so the ground level no longer needs a hatch drop or a shaft pit.
        float floorY = bmn[1];   // <-- empirical start; adjust if the player floats/sinks.
        {
            sphys->addBox({ halfX, 0.5f, halfZ }, { cx, floorY - 0.5f, cz }, 0.0f, x3::phys::Layer::Static);
            x3::logInfo("--world showroom: GROUND floor slab (solid) top floorY=" + std::to_string(floorY) +
                        " center(" + std::to_string(cx) + "," + std::to_string(cz) + ") (TUNE POINT)");
        }
        // Perimeter walls (4 static slabs, 4 m tall) so the player can't walk off the slab.
        {
            const float wallH = 2.0f;   // half-height (4 m wall)
            const float wallT = 0.3f;   // half-thickness
            const x3::phys::Vec3 wc{ cx, floorY + wallH, cz };
            sphys->addBox({ wallT, wallH, halfZ }, { cx - halfX, wc.y, cz }, 0.0f, x3::phys::Layer::Static); // -X
            sphys->addBox({ wallT, wallH, halfZ }, { cx + halfX, wc.y, cz }, 0.0f, x3::phys::Layer::Static); // +X
            sphys->addBox({ halfX, wallH, wallT }, { cx, wc.y, cz - halfZ }, 0.0f, x3::phys::Layer::Static); // -Z
            sphys->addBox({ halfX, wallH, wallT }, { cx, wc.y, cz + halfZ }, 0.0f, x3::phys::Layer::Static); // +Z
        }

        // ---- Player spawn at the building center, feet just above the floor slab.
        const float sx = cx, sy = floorY + 0.05f, sz = cz;
        x3::game::Player splayer;
        splayer.spawn(*sphys, sx, sy, sz);
        x3::logInfo("--world showroom: player spawn feet(" + std::to_string(sx) + "," +
                    std::to_string(sy) + "," + std::to_string(sz) + ")");

        // ---- Companion ARIA: a single RescueVictim a few metres in front (+Z) of spawn,
        // standing on the floor. NEVER activated (hubReached stays false -> no countdown,
        // no boss). AnnaCasual_anim.glb carries Idle/Walk/Run/Talk (retargeted from Jake),
        // so the loco blend engages — she walks/runs while following, not idle-slides.
        const float gx = cx + 3.0f, gz = cz + 4.0f;
        x3::game::RescueVictim girl;
        girl.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                   x3::phys::Vec3{ gx, floorY, gz }, x3::game::VictimId::Aria, "Aria",
                   "AnnaCasual_anim.glb", 1e9f /*huge timer — never expires*/,
                   x3::game::MonsterSystem::Tuning{});
        x3::logInfo("--world showroom: Aria at (" + std::to_string(gx) + "," +
                    std::to_string(floorY) + "," + std::to_string(gz) + ")");

        // ===================================================================
        // ADDITIVE: 2ND-FLOOR hidden wall door -> passage -> stairs -> elevator
        // atrium -> glass elevator -> glass spire-top deck. (OWNER'S VISION.)
        // All geometry below is ADDITIVE to the showroom (does NOT touch the
        // building GLB / Aria / night-sky code). It FOLLOWS THE BUILDING'S OWN
        // ARCHITECTURE — clad in the building's WHITE PANEL material, aligned to
        // its axes / walls / floor levels (derived from the GLB node bounds):
        //   GROUND floor   y = floorY (-9)   : where the player spawns.
        //   2nd FLOOR      y = floor2Y (3)   : top of the GLB Room_01 slab; the
        //                  player CLIMBS here on a synthesized stair approximating
        //                  the GLB "Stair" nodes (left run x~[44,54]).
        //   ATRIUM floor   y = atriumFloorY (9): one flight ABOVE the 2nd floor,
        //                  where the glass elevator now BOARDS.
        //   DECK           y = deckTopY (90) : the glass spire-top deck (unchanged).
        // Feature chain (all WALKABLE, all white-clad, all axis-aligned):
        //   STAGE 1  CLIMB collision: a stair (approximating the GLB Stair run) +
        //            a 2nd-floor slab so the player walks up from ground to y=3.
        //   STAGE 2  a FLUSH HIDDEN WALL DOOR set into a real 2nd-floor wall
        //            (Pilar_02, z~-101), keypad-gated (code 2742 unchanged); the
        //            panel SLIDES ASIDE when unlocked.
        //   STAGE 3  behind the door: an ENTRY PASSAGE (-Z) -> a 90 deg TURN ->
        //            a FLIGHT OF STAIRS UP -> the ELEVATOR ATRIUM (a white room
        //            around the lift). The elevator's LOWER stop is the atrium.
        //   (PHASE 1/2 below keep the GLASS DECK + the glass elevator that rides
        //    atrium<->deck.) Glass is the engine BLEND path (drawMeshPBR
        //    alphaBlend=true), drawn explicitly each frame; the white-panel
        //    opaque geometry goes through the Scene as textured entities.
        // ===================================================================
        const float spireX = cx;          // spire is over the building center X
        const float spireZ = -100.0f;     // spire Z (per the night-showroom blueprint)
        const float deckTopY = 90.0f;     // walkable deck surface height (TUNE vs spire top y~88.5)
        const float shaftX   = spireX + 9.0f;  // elevator shaft just +X of the spire (clear of geo)
        const float shaftZ   = spireZ;
        // ---- Building floor levels (from the GLB node bounds; see tools/glb_node_bounds.py).
        // Room_01 (the 2nd-floor slab) has its TOP at world y=3; the GLB "Stair" nodes climb
        // from the ground (y=-9). The atrium sits one short flight above the 2nd floor.
        const float floor2Y     = 3.0f;   // 2nd-floor walkable surface (Room_01 top)
        // ELEVATOR LEVEL (Y10, in the owner's Y10-14 range): the hidden stair climbs UP
        // INSIDE the back strut to THIS height, where it meets the glass-elevator boarding
        // atrium. The lift's LOWER stop is computed from atriumFloorY below, so setting it
        // here moves the boarding level to the strut-stair landing (OWNER'S vision). Y10 is
        // chosen so the strut's diagonal stepped stair climbs at a walkable ~43 deg (the
        // character controller's max walkable slope is 50 deg; steps are <=0.4 m each).
        const float atriumFloorY = 10.0f; // elevator-atrium floor == strut-stair landing

        // ---- Shared procedural meshes (authored at WORLD center; identity xform).
        // Glass tints reused for deck slab, rails, car, and shaft glints. The
        // alphaBlend draw multiplies baseColorFactor (incl. alpha) onto the texel.
        auto makeWorldMesh = [&](const x3::prims::PrimMesh& g) {
            return device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                      g.index.data(), (uint32_t)g.index.size());
        };
        // A helper to draw one glass box (translucent) at an identity-placed world
        // mesh, OR offset by a model translation for the moving car.
        const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto drawGlass = [&](const x3::rhi::FrameContext& fr, x3::rhi::MeshHandle m,
                             const float model[16], float r, float g, float b, float a,
                             float emisStrength) {
            const float bcf[4]  = { r, g, b, a };
            const float emis[4] = { r * 0.6f, g * 0.7f, b * 0.9f, emisStrength };
            device->drawMeshPBR(fr, m, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                x3::rhi::TextureHandle{}, bcf, emis, model,
                                /*alphaMask*/false, /*alphaBlend*/true, x3::rhi::TextureHandle{});
        };

        // ===================================================================
        // WHITE-PANEL CLADDING — match the building's wall material.
        // The GLB walls (Room_01/Pilar_01/Pilar_02) all use a "Wall_Atlas..._White"
        // textured material (baseColorFactor white). So every additive surface of
        // the entrance run (climb stair, 2nd-floor slab, hidden door panel, passage,
        // turn, upper stair, atrium) is clad in a procedural WHITE sci-fi PANEL tile
        // tinted near-white, so the whole run reads as part of the structure (NOT
        // grey boxes). One shared tiling texture + a tiny "add a white-clad static
        // box (render entity + collision)" helper keep it uniform + axis-aligned.
        // -------------------------------------------------------------------
        x3::rhi::TextureHandle whitePanelTex{};
        {
            // CLEAN, SMOOTH near-white powder-coat panel — matches the sleek Unity
            // ShowRoom interior (smooth light-grey/white walls + floors). The earlier
            // pass used makeSciFiPanelRGBA tinted ~x2.7, which lifted the panel FACES to
            // white but left the seam grooves / bolts / bevels as a HIGH-CONTRAST grid
            // (heavy dark grout) that clashed with the smooth GLB. makeCleanPanelRGBA is
            // a flat light face with only a WHISPER-FINE 1-px low-contrast seam hairline
            // (no grout, no bolts, no bevels), so the additive cladding reads flush with
            // the imported white walls. 4 panel divisions across the 512 tile.
            std::vector<uint8_t> px = x3::prims::makeCleanPanelRGBA(
                512, /*panels*/4, x3::prims::detail::kNoTint, /*seams*/true);
            whitePanelTex = device->createTexture(px.data(), 512, 512, /*srgb*/true);
        }
        // The white-panel base tint (multiplies the already-white texel). Near-1 so the
        // clean white panels read under the dim moonlight, like the GLB walls.
        const float kWhite[4] = { 0.98f, 0.98f, 1.0f, 1.0f };
        // Add ONE axis-aligned white-clad box: a render Scene entity (white panel
        // texture) PLUS matching Static collision. Returns the Scene entity id.
        // (cx,cy,cz) = center; (hx,hy,hz) = half-extents. collide=false => visual only.
        auto addWhiteBox = [&](float bx, float by, float bz, float hx, float hy, float hz,
                               bool collide = true) -> uint32_t {
            x3::rhi::MeshHandle m = makeWorldMesh(x3::prims::makeBox(hx, hy, hz, bx, by, bz, 0.25f));
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            const uint32_t id = sscene.add(e);
            if (collide) sphys->addBox({ hx, hy, hz }, { bx, by, bz }, 0.0f, x3::phys::Layer::Static);
            return id;
        };
        // POLISHED-FLOOR material dial (SSR/RT reflections money shot): a 1x1
        // metallic-roughness map (glTF packing: G=roughness, B=metallic, linear)
        // applied to the showroom's WALKED floor slabs so they read as the sleek
        // polished showroom surface they were always meant to be — roughness 0.08
        // (mirror-sharp, inside the SSR full-strength band) + metallic 0.5 (semi-
        // metal powder coat: strong F0 without fully killing the white diffuse).
        // Only entities explicitly polish()-ed change; every other white box keeps
        // the satin dielectric default.
        x3::rhi::TextureHandle polishedMrTex{};
        {
            const uint8_t mr[4] = { 0, 20, 128, 255 };   // R unused, G=rough 0.08, B=metal 0.50
            polishedMrTex = device->createTexture(mr, 1, 1, /*srgb*/false);
        }
        auto polish = [&](uint32_t id) { sscene.get(id).mrTex = polishedMrTex; };

        // ===================================================================
        // STAGE 1 — let the player CLIMB to the 2ND FLOOR (y = floor2Y = 3).
        // The GLB has no collision, so we SYNTHESIZE it, aligned to the building:
        //   * the 2nd-floor SLAB = the GLB Room_01 footprint (x~[29,115], z~[-123,
        //     -99]) with its TOP at floor2Y. Built as two halves around the central
        //     tower-core gap (x~[71,73]) so it matches the real split floor.
        //   * a CLIMB STAIR approximating the GLB "Stair" node (left run, x~[44,54],
        //     running along +Z), EXTENDED so it rises the full ground->2nd-floor
        //     drop (floorY -> floor2Y = 12 m) at a walkable pitch. makeRamp builds
        //     the walkable wedge; we cap it with a white tread plate so it reads as
        //     a clad stair, and a small landing where it meets the 2nd floor.
        // -------------------------------------------------------------------
        // 2nd-floor slab — Room_01 footprint, split around the central tower gap.
        const float r2z0 = -122.8f, r2z1 = -99.2f;            // Room_01 Z span
        const float r2cz = (r2z0 + r2z1) * 0.5f, r2hz = (r2z1 - r2z0) * 0.5f;
        const float slabHY = 0.4f;                            // 0.8 m thick slab; TOP at floor2Y
        const float slabCY = floor2Y - slabHY;
        // Left half x~[29.3,71.2], right half x~[73,114.9]; leave the x~[71.2,73] gap.
        // Both halves POLISHED (SSR money shot: the 2nd-floor walk reflects the
        // building lights + window wall in the floor).
        polish(addWhiteBox((29.3f + 71.2f) * 0.5f, slabCY, r2cz, (71.2f - 29.3f) * 0.5f, slabHY, r2hz));
        polish(addWhiteBox((73.0f + 114.9f) * 0.5f, slabCY, r2cz, (114.9f - 73.0f) * 0.5f, slabHY, r2hz));
        // CLIMB stair: approximate the GLB left "Stair" (x~[43.8,53.7]) but lengthen
        // the run so the 12 m rise is a walkable ~40 deg. Runs along +Z from a low
        // edge on the ground (z=stairLowZ @ floorY) up to a high edge that meets the
        // 2nd-floor slab (z=stairHighZ @ floor2Y).
        const float climbCX = 48.75f;          // GLB left-stair X center
        const float climbHalfW = 4.8f;         // matches the GLB stair width (~9.9 m)
        const float climbRise = floor2Y - floorY;   // 12 m
        const float climbRun  = 15.0f;         // walkable pitch (~39 deg)
        const float stairLowZ  = -119.0f;      // low edge on the ground (inside Room_01 -Z half)
        {
            x3::prims::PrimMesh ramp = x3::prims::makeRamp(climbCX, floorY, stairLowZ,
                                                          climbHalfW, climbRun, climbRise,
                                                          /*axis*/1 /*+Z*/, /*dir*/+1.0f, 0.25f);
            x3::rhi::MeshHandle m = makeWorldMesh(ramp);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            sscene.add(e);
            sphys->addStaticMesh(ramp.cverts.data(), (uint32_t)ramp.cverts.size() / 3,
                                 ramp.cindex.data(), (uint32_t)ramp.cindex.size());
            x3::logInfo("--world showroom: STAGE1 climb stair x=" + std::to_string(climbCX) +
                        " run +Z z=" + std::to_string(stairLowZ) + ".." +
                        std::to_string(stairLowZ + climbRun) + " rise " + std::to_string(climbRise) +
                        " m floorY->floor2Y; 2nd-floor slab top y=" + std::to_string(floor2Y));
        }

        // ===================================================================
        // STRUT SET — the building's SYMMETRIC RADIAL "/" blade-fin legs, REBUILT
        // thicker (the GLB struts are fixed thin geometry, so we clad NEW white-panel
        // strut-SHELLS over them). FOUR canted struts at the four footprint corners
        // (back-left/back-right/front-left/front-right), each LEANING INWARD+UP toward
        // the central spire core — a matched radial set (same thickness/width/height,
        // mirrored about the center). ONE strut (the BACK-LEFT, Z~-134, least visible
        // from the central pad per the interior reference) is HOLLOW and carries the
        // HIDDEN STAIR: a keypad door (code 2742) set into its canted outward "/" face
        // at the civilian floor (floorY) climbs UP INSIDE it to the ELEVATOR LEVEL
        // (atriumFloorY=14), where a short bridge meets the glass-elevator atrium.
        //   - Strut layout sampled from the GLB Plateform_05/06 corner fins
        //     (tools/glb_node_bounds.py): the 4 canted disc-edge supports.
        //   - Built via prims::makeCantedStrut (a sheared prism) so they read as the
        //     real leaning legs, clad in the SAME white-panel material as the GLB walls.
        // -------------------------------------------------------------------
        // Add a white-clad procedural mesh (render entity + static collision). Used for
        // the canted struts / stair steps that aren't axis-aligned boxes.
        auto addWhiteMesh = [&](const x3::prims::PrimMesh& g, bool collide = true) -> uint32_t {
            x3::rhi::MeshHandle m = makeWorldMesh(g);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            const uint32_t id = sscene.add(e);
            if (collide && !g.cverts.empty())
                sphys->addStaticMesh(g.cverts.data(), (uint32_t)g.cverts.size()/3,
                                     g.cindex.data(), (uint32_t)g.cindex.size());
            return id;
        };

        // --- Matched strut dimensions (ALL four identical -> symmetric). ---
        const float strutBaseY  = floorY;        // strut foot on the civilian floor (-12)
        const float strutTopY   = atriumFloorY;  // strut head at the elevator level (10)
        const float strutHalfW  = 4.0f;          // 8 m wide (tangential) — beefy leg
        const float strutHalfT  = 2.6f;          // 5.2 m thick (radial) — THICKENED
        const float strutHeadR  = 10.0f;         // head pulled IN to ~10 m from the core
                                                 // (distinct legs, just inside the disc spring line;
                                                 //  gives the hollow strut a ~44 deg walkable stair)
        // Base/top XZ centers per corner, derived radially from the center so the four
        // are perfectly mirrored. Base sits OUT at the corner; head pulled IN near the
        // spire core (the "/" inward lean -> the four legs gather toward the spire). The
        // head-near-core lean also gives a long diagonal run (~23 m) so the internal
        // stair climbs the floorY->elevator-level drop at a walkable ~43 deg.
        struct StrutDef { float bx,bz,tx,tz,rox,roz; };
        auto strutFor = [&](float cornerX, float cornerZ) -> StrutDef {
            float dx = cornerX - cx, dz = cornerZ - cz;
            float L = std::sqrt(dx*dx + dz*dz); if (L < 1e-3f) L = 1.0f;
            float ux = dx/L, uz = dz/L;             // radial-OUT unit
            StrutDef s;
            s.bx = cornerX + ux*2.0f;     s.bz = cornerZ + uz*2.0f;     // foot a touch further out
            s.tx = cx + ux*strutHeadR;    s.tz = cz + uz*strutHeadR;    // head near the core
            s.rox = ux; s.roz = uz;
            return s;
        };
        // GLB Plateform_05/06 corner-fin centers (the canted disc-edge supports).
        const StrutDef sBL = strutFor((47.2f+70.0f)*0.5f, (-139.6f-129.7f)*0.5f); // back-left  (Z~-134) HOLLOW
        const StrutDef sBR = strutFor((74.3f+97.0f)*0.5f, (-139.7f-129.8f)*0.5f); // back-right
        const StrutDef sFL = strutFor((47.2f+70.0f)*0.5f, (-92.2f-82.3f)*0.5f);   // front-left
        const StrutDef sFR = strutFor((74.3f+97.1f)*0.5f, (-92.3f-82.4f)*0.5f);   // front-right
        // Build the THREE SOLID struts (BR/FL/FR). The hollow BL one is built below
        // (its walls + door + interior stair).
        auto buildSolidStrut = [&](const StrutDef& s) {
            addWhiteMesh(x3::prims::makeCantedStrut(s.bx, strutBaseY, s.bz, s.tx, strutTopY, s.tz,
                                                    strutHalfW, strutHalfT, s.rox, s.roz,
                                                    /*uvScale*/0.4f, /*hollow*/false));
        };
        buildSolidStrut(sBR); buildSolidStrut(sFL); buildSolidStrut(sFR);
        x3::logInfo("--world showroom: STRUTS x4 canted blades (halfW=" + std::to_string(strutHalfW) +
                    " halfT=" + std::to_string(strutHalfT) + " baseY=" + std::to_string(strutBaseY) +
                    " topY=" + std::to_string(strutTopY) + ") symmetric about (" +
                    std::to_string(cx) + "," + std::to_string(cz) + "); BACK-LEFT is HOLLOW (stair)");

        // ---- The HOLLOW BACK-LEFT strut: four canted WALLS (outward face has the door
        // gap), the internal STAIR climbing the cant, the keypad door, and the top
        // landing + bridge to the elevator atrium. The blade is clad white like the rest.
        // Geometry follows the SAME canted axis (base sBL.b* -> top sBL.t*).
        const float hsRox = sBL.rox, hsRoz = sBL.roz;       // radial-out unit (toward corner)
        const float hsTx  = -hsRoz,  hsTz = hsRox;          // tangential unit
        // Door sits in the OUTWARD canted face at the civilian floor. Door-face center at
        // the base, pushed out to the outward wall plane.
        const float doorFaceX = sBL.bx + hsRox*strutHalfT;
        const float doorFaceZ = sBL.bz + hsRoz*strutHalfT;
        // Keep the proven keypad-host variable NAMES (door*/hatch*) so the interaction +
        // smoke code is untouched; they now address the STRUT-FACE door at civilian level.
        const float doorHalfW   = 1.2f;          // 2.4 m wide opening
        const float doorHalfH   = 1.3f;          // 2.6 m tall
        const float doorPanelHZ = 0.20f;         // panel thickness
        const float doorFloorY  = floorY;        // civilian floor the player stands on
        const float doorX = doorFaceX;           // door center X (proximity + panel)
        const float doorZ = doorFaceZ;           // door center Z
        const float doorCY = doorFloorY + doorHalfH;   // panel center (sill on the floor)
        const float hatchX = doorX, hatchZ = doorZ, hatchHalf = doorHalfW;   // proximity window
        constexpr int HATCH_CODE = kShowroomHatchCode;   // 2742 (UNCHANGED)
        // The hollow strut SHELL (outward wall omitted so the doorway/interior is open).
        // Render + collision: the 3 enclosed faces (inward + both tangential) + caps give
        // the blade its solid read and keep the stair enclosed; the outward face is clad
        // separately below (around the door gap) so a real door-sized hole exists.
        addWhiteMesh(x3::prims::makeCantedStrut(sBL.bx, strutBaseY, sBL.bz, sBL.tx, strutTopY, sBL.tz,
                                                strutHalfW, strutHalfT, hsRox, hsRoz,
                                                /*uvScale*/0.4f, /*hollow*/true));
        // OUTWARD-FACE CLADDING: thin CANTED slabs (matching the blade's lean) covering
        // the full outward face EXCEPT a door-sized gap at the base — so from outside the
        // strut reads as a clean clad "/" blade with a flush door set into it. Built as
        // canted slabs (thin in the radial axis) at the outward face plane:
        //   * the UPPER cladding (above the door lintel, up the whole face),
        //   * a LEFT and RIGHT jamb cladding flanking the door at the base.
        {
            const float cladHT = 0.18f;   // cladding slab thickness (radial)
            // The outward face plane sits at +halfT along radial from the strut axis. The
            // canted slab's own axis runs base->top; we offset both base/top OUT by halfT.
            const float ofbx = sBL.bx + hsRox*strutHalfT, ofbz = sBL.bz + hsRoz*strutHalfT;
            const float oftx = sBL.tx + hsRox*strutHalfT, oftz = sBL.tz + hsRoz*strutHalfT;
            const float doorTopY = doorFloorY + doorHalfH*2.0f;   // top of the 2.6 m door
            // UPPER cladding: from the door top up to the head, full width. Its base sits
            // at the height where the door ends (interpolate the face axis to that height).
            {
                const float tDoorTop = (doorTopY - strutBaseY) / (strutTopY - strutBaseY);
                const float ubx = ofbx + (oftx - ofbx)*tDoorTop, ubz = ofbz + (oftz - ofbz)*tDoorTop;
                addWhiteMesh(x3::prims::makeCantedStrut(ubx, doorTopY, ubz, oftx, strutTopY, oftz,
                                                        strutHalfW, cladHT, hsRox, hsRoz, 0.4f, false));
            }
            // LEFT + RIGHT jamb cladding at the base (door height), flanking the 2.4 m gap.
            const float jambHW = (strutHalfW - doorHalfW) * 0.5f;     // half-width of each jamb panel
            const float jambOff = doorHalfW + jambHW;                 // tangential offset to jamb center
            for (float sgn : { +1.0f, -1.0f }) {
                const float jbx = ofbx + hsTx*jambOff*sgn, jbz = ofbz + hsTz*jambOff*sgn;
                // jamb top tracks the cant up to the door top height.
                const float tDoorTop = (doorTopY - strutBaseY) / (strutTopY - strutBaseY);
                const float jtx = jbx + (oftx - ofbx)*tDoorTop, jtz = jbz + (oftz - ofbz)*tDoorTop;
                addWhiteMesh(x3::prims::makeCantedStrut(jbx, strutBaseY, jbz, jtx, doorTopY, jtz,
                                                        jambHW, cladHT, hsRox, hsRoz, 0.4f, false));
            }
        }
        // Concealed door PANEL: a thin CANTED white slab flush in the outward face,
        // matching the blade's lean + the same white texture so it's invisible until
        // opened. Authored at its WORLD position (door tangential center, sill on the
        // floor) so its entity transform starts at identity; the slide animation then
        // translates it tangentially. Collision is a matching axis-aligned box (removed
        // on open). A faint cyan glow pulses when the player is near + it's still closed.
        const float doorTopY2 = doorFloorY + doorHalfH*2.0f;
        x3::rhi::MeshHandle hatchMesh = makeWorldMesh(x3::prims::makeCantedStrut(
            doorFaceX, doorFloorY, doorFaceZ,
            doorFaceX + (sBL.tx - sBL.bx) * ((doorTopY2-strutBaseY)/(strutTopY-strutBaseY)),
            doorTopY2,
            doorFaceZ + (sBL.tz - sBL.bz) * ((doorTopY2-strutBaseY)/(strutTopY-strutBaseY)),
            doorHalfW, doorPanelHZ, hsRox, hsRoz, 0.4f, false));
        x3::game::Entity hatchEnt; hatchEnt.mesh = hatchMesh; hatchEnt.tex = whitePanelTex;
        hatchEnt.baseColor[0]=kWhite[0]; hatchEnt.baseColor[1]=kWhite[1]; hatchEnt.baseColor[2]=kWhite[2]; hatchEnt.baseColor[3]=1.0f;
        hatchEnt.emissive[0]=0.10f; hatchEnt.emissive[1]=0.45f; hatchEnt.emissive[2]=0.55f; hatchEnt.emissive[3]=0.0f;
        const uint32_t hatchIdx = sscene.add(hatchEnt);   // authored in world -> identity transform
        x3::phys::BodyId hatchLidBody =
            sphys->addBox({ doorHalfW, doorHalfH, doorPanelHZ }, { doorX, doorCY, doorZ }, 0.0f, x3::phys::Layer::Static);
        bool  hatchOpen = false;
        float hatchSlide = 0.0f;
        // The slide-aside direction (tangential, +) for the cosmetic open animation.
        const float hatchSlideX = hsTx, hatchSlideZ = hsTz;

        // ---- INTERNAL STAIR up the hollow strut: many small STEPPED white treads
        // following the canted axis from the door foot (floorY) up to the head landing
        // (atriumFloorY). Each step rises <=0.4 m (the character controller steps up to
        // 0.4 m) so the player WALKS UP every step; collision is on every tread. Each
        // tread is an ORIENTED block (built via makeCantedStrut so its cross-section is
        // aligned to the strut's tangential + radial axes — NOT axis-aligned — so the
        // steps fill the canted blade interior and march along the diagonal cant.
        {
            const float stepRise = 0.4f;                                  // <=0.4 m -> walkable
            const int   nSteps   = (int)std::ceil((strutTopY - strutBaseY) / stepRise);  // ~48
            const float treadHW  = strutHalfW - 0.9f;                     // tread half-width (tangential, inside walls)
            const float treadHT  = strutHalfT - 0.5f;                     // tread half-depth (radial, inside walls)
            for (int i = 0; i < nSteps; ++i) {
                float t0  = (float)(i + 1) / (float)nSteps;               // top of step i
                float cxs = sBL.bx + (sBL.tx - sBL.bx) * t0;
                float czs = sBL.bz + (sBL.tz - sBL.bz) * t0;
                float topY = strutBaseY + (strutTopY - strutBaseY) * t0;  // this tread's TOP
                // ORIENTED riser block: a short vertical prism (cross-section tangential x
                // radial, aligned to the strut) from just below the tread up to its top.
                // (Same XZ for base+top -> a vertical block; height = 0.55 m riser.)
                addWhiteMesh(x3::prims::makeCantedStrut(cxs, topY - 0.55f, czs, cxs, topY, czs,
                                                        treadHW, treadHT, hsRox, hsRoz, 0.4f, false));
            }
            // Top LANDING: a white floor pad at the strut head (atriumFloorY) where the
            // stair tops out and the bridge to the elevator atrium begins.
            addWhiteBox(sBL.tx, strutTopY - 0.25f, sBL.tz, strutHalfW, 0.25f, strutHalfT);
            x3::logInfo("--world showroom: HOLLOW strut stair " + std::to_string(nSteps) +
                        " steps (rise " + std::to_string(stepRise) + " m each) base(" +
                        std::to_string(sBL.bx) + "," + std::to_string(sBL.bz) + ") -> head(" +
                        std::to_string(sBL.tx) + "," + std::to_string(sBL.tz) +
                        ") rise floorY->atriumFloorY=" + std::to_string(strutTopY) +
                        "; door face(" + std::to_string(doorFaceX) + "," + std::to_string(doorFaceZ) +
                        ") at civilian floor y=" + std::to_string(doorFloorY));
        }

        // -------------------------------------------------------------------
        // PHASE 1 — GLASS DECK at the spire top.
        // 14x14 m glass slab (thin), TOP surface at deckTopY, centered over the
        // spire. Four low glass rail boxes around the edge (1.1 m tall). Static
        // collision: the slab floor + the four rails (so you stand + can't fall).
        // -------------------------------------------------------------------
        const float deckHalf = 7.0f;       // 14 m square
        const float deckSlabHalfY = 0.15f; // thin glass slab
        const float deckSlabCY = deckTopY - deckSlabHalfY;   // center so TOP == deckTopY
        x3::rhi::MeshHandle deckMesh = makeWorldMesh(
            x3::prims::makeBox(deckHalf, deckSlabHalfY, deckHalf, spireX, deckSlabCY, spireZ, 0.5f));
        // Rails: 4 thin tall glass boxes hugging each edge (top ~1.1 m above deck).
        const float railH = 0.55f, railT = 0.08f;
        const float railCY = deckTopY + railH;
        x3::rhi::MeshHandle railNZ = makeWorldMesh(x3::prims::makeBox(deckHalf, railH, railT, spireX, railCY, spireZ - deckHalf, 1.0f));
        x3::rhi::MeshHandle railPZ = makeWorldMesh(x3::prims::makeBox(deckHalf, railH, railT, spireX, railCY, spireZ + deckHalf, 1.0f));
        x3::rhi::MeshHandle railNX = makeWorldMesh(x3::prims::makeBox(railT, railH, deckHalf, spireX - deckHalf, railCY, spireZ, 1.0f));
        x3::rhi::MeshHandle railPX = makeWorldMesh(x3::prims::makeBox(railT, railH, deckHalf, spireX + deckHalf, railCY, spireZ, 1.0f));
        // Static collision: deck floor slab + 4 rail slabs.
        sphys->addBox({ deckHalf, deckSlabHalfY, deckHalf }, { spireX, deckSlabCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ deckHalf, railH, railT }, { spireX, railCY, spireZ - deckHalf }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ deckHalf, railH, railT }, { spireX, railCY, spireZ + deckHalf }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ railT, railH, deckHalf }, { spireX - deckHalf, railCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ railT, railH, deckHalf }, { spireX + deckHalf, railCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        x3::logInfo("--world showroom: PHASE1 glass deck 14x14 top y=" + std::to_string(deckTopY) +
                    " center(" + std::to_string(spireX) + "," + std::to_string(spireZ) + ") + 4 rails");

        // -------------------------------------------------------------------
        // PHASE 2 — GLASS ELEVATOR atrium<->deck (reuses app/elevator.cpp).
        // ElevatorSystem provides the moving Static-layer body that CARRIES the
        // player: update() returns the per-frame vertical delta (the host adds it
        // to a rider's Y) and playerRiding(feet) detects a rider standing on the
        // cab TOP. So the cab is a thin FLOOR PLATFORM the player rides INSIDE a
        // glass box that rises above it. The cab-top is the standable surface:
        //   LOWER stop -> cab top at the ATRIUM floor (atriumFloorY) so you step
        //                 from the atrium into the cab (OWNER'S vision — the lift no
        //                 longer reaches the ground; you climb to it via the door);
        //   UPPER stop -> cab top at the DECK (deckTopY) so you step out onto it.
        // The glass walls (a 2.5 x 3 x 2.5 m box) are a translucent VISUAL drawn
        // around/above the platform each frame (not collision — open so you walk in).
        // -------------------------------------------------------------------
        const float carHX = 1.25f, platHY = 0.12f, carHZ = 1.25f;   // thin platform
        const float carBoxHY = 1.5f;   // glass box half-height (3 m tall walls)
        // Stop centers so the PLATFORM TOP (center + platHY) lands at atriumFloorY / deckTopY.
        const float elevBaseCenterY = atriumFloorY - platHY;   // cab top == atrium floor
        const float elevTopCenterY  = deckTopY      - platHY;   // cab top == deckTopY
        x3::game::ElevatorSystem elev;
        const uint32_t elevEntIdx = sscene.size();   // the cab platform entity lands here
        elev.build(sscene, *device, *sphys, shaftX, shaftZ, carHX, platHY, carHZ,
                   { elevBaseCenterY, elevTopCenterY }, /*startStop*/0);
        elev.setSpeed(14.0f);   // m/s — a brisk readable climb (~7 s over the 99 m shaft)
        // GLASS-BOTTOM cab (OWNER'S vision): hide the opaque plate Entity and draw a
        // translucent glass floor slab at the cab top each frame (in drawAdditiveGlass).
        // Collision is the elevator's moving Static body, so the rider still stands + rides
        // — they just see DOWN through the floor at the gallery falling away as it climbs.
        if (elevEntIdx < sscene.size()) {
            sscene.get(elevEntIdx).visible = false;
        }
        // Glass BOX walls authored centered at ORIGIN; drawn at (cabTop + carBoxHY)
        // each frame so the walls rise from the platform. Hollow-look: a single
        // translucent box reads as a glass cab around the rider.
        x3::rhi::MeshHandle carMesh = makeWorldMesh(
            x3::prims::makeBox(carHX, carBoxHY, carHZ, 0, 0, 0, 0.6f));
        // Glass FLOOR slab (cab footprint, thin) — the see-through bottom the rider stands
        // on; drawn translucent at the cab plate each frame (replaces the opaque plate).
        x3::rhi::MeshHandle carFloorMesh = makeWorldMesh(
            x3::prims::makeBox(carHX, platHY, carHZ, 0, 0, 0, 0.4f));
        // Four slim vertical guide POSTS flanking the shaft (opaque structure), so the
        // shaft reads as built while staying mostly open for the see-through ride. They
        // span from the base floor up to the deck rail height.
        const float postT = 0.10f;
        const float postTopY = deckTopY + 1.0f;                 // up past the deck
        const float postH  = (postTopY - atriumFloorY) * 0.5f;  // half-height (atrium -> deck)
        const float postCY = atriumFloorY + postH;
        x3::rhi::MeshHandle postMesh = makeWorldMesh(
            x3::prims::makeBox(postT, postH, postT, 0, 0, 0, 1.0f));
        auto addPost = [&](float px, float pz) {
            x3::game::Entity e; e.mesh = postMesh;
            e.baseColor[0]=0.20f; e.baseColor[1]=0.22f; e.baseColor[2]=0.28f; e.baseColor[3]=1.0f;
            e.transform[12]=px; e.transform[13]=postCY; e.transform[14]=pz;
            sscene.add(e);
        };
        addPost(shaftX - carHX - 0.25f, shaftZ - carHZ - 0.25f);
        addPost(shaftX + carHX + 0.25f, shaftZ - carHZ - 0.25f);
        addPost(shaftX - carHX - 0.25f, shaftZ + carHZ + 0.25f);
        addPost(shaftX + carHX + 0.25f, shaftZ + carHZ + 0.25f);
        x3::logInfo("--world showroom: PHASE2 glass elevator shaft(" + std::to_string(shaftX) + "," +
                    std::to_string(shaftZ) + ") stops cab-top {atriumFloorY=" + std::to_string(atriumFloorY) +
                    ", deck=" + std::to_string(deckTopY) + "} carry-via ElevatorSystem");

        // -------------------------------------------------------------------
        // ELEVATOR ATRIUM (at the strut-stair landing level, atriumFloorY=14) + a
        // short BRIDGE from the hollow strut's head to the glass-elevator boarding.
        // (This SUPERSEDES the old 2nd-floor partition-door + passage + up-stair: the
        //  new route is keypad door in the strut face -> stair UP inside the strut ->
        //  this atrium -> glass elevator -> deck.) All white-clad, all walkable.
        // -------------------------------------------------------------------
        {
            // Atrium floor pad around the lift shaft (shaftX=cx+9, shaftZ=-100) at Y14.
            const float atX0 = shaftX - 9.0f, atX1 = shaftX + 5.0f;
            const float atZ0 = shaftZ - 9.0f, atZ1 = shaftZ + 5.0f;
            polish(addWhiteBox((atX0+atX1)*0.5f, atriumFloorY - 0.25f, (atZ0+atZ1)*0.5f,
                        (atX1-atX0)*0.5f, 0.25f, (atZ1-atZ0)*0.5f));   // POLISHED (refl money shot)
            // BRIDGE: a white walkway from the strut head landing (sBL.t*) to the atrium
            // edge, both at atriumFloorY, so the player crosses from the strut to the lift.
            {
                const float ax0 = std::min(sBL.tx, atX0) - 1.6f, ax1 = std::max(sBL.tx, atX0) + 1.6f;
                const float az0 = std::min(sBL.tz, atZ0) - 1.6f, az1 = std::max(sBL.tz, atZ0) + 1.6f;
                addWhiteBox((ax0+ax1)*0.5f, atriumFloorY - 0.25f, (az0+az1)*0.5f,
                            (ax1-ax0)*0.5f, 0.25f, (az1-az0)*0.5f);
            }
            x3::logInfo("--world showroom: ELEVATOR ATRIUM floor y=" + std::to_string(atriumFloorY) +
                        " around shaft(" + std::to_string(shaftX) + "," + std::to_string(shaftZ) +
                        ") + bridge from strut head(" + std::to_string(sBL.tx) + "," + std::to_string(sBL.tz) + ")");
        }

        // ===================================================================
        // HIDDEN ANALYST GALLERY (OWNER'S VISION). A secret surveillance level
        // ringing the CENTRAL VOID above the civilian floor, AT THE ELEVATOR
        // LEVEL (galleryY == atriumFloorY) so the strut stair lands ON it and the
        // glass elevator boards FROM it. A walkable white-panel RING (annulus)
        // around an OPEN VOID over the building center (cx,cz); through the void
        // (rimmed with DARK ONE-WAY GLASS) the analysts look DOWN onto the
        // civilians on the ground floor (Y=floorY) + the 2nd floor (Y=floor2Y).
        // HOLOGRAPHIC TERMINALS (reuse holo_terminal.cpp) ring the void facing in;
        // a subset carry idle ANALYST FIGURES (RescueVictim skinned, never rescued).
        //
        // DARK-GLASS BALUSTRADE (real-time, fixed-alpha BLEND path — no per-pixel
        // fresnel): the void edge is treated with an ELEGANT thin parapet (low white
        // kerb + slim cap rail) topped by a band of FLAT DARK-TINTED GLASS held by
        // slim metal mullions — like the Unity interior's glass railings. The dark
        // tint gives the analysts a shaded look-down onto the civilians while the void
        // stays OPEN below the glass band (so the gallery still overlooks the floor).
        // LIMITATION: the tint is a fixed-alpha approximation, not a true angle-
        // dependent one-way material — but it reads sleek + minimal (NOT a lumpy
        // louver/gear). Glass tint/alpha (galGlass* below) are the TUNE POINTS.
        // ===================================================================
        // Gallery ring sits at the elevator level so it connects to the existing
        // strut-stair landing + bridge + elevator boarding (all at atriumFloorY).
        const float galleryY   = atriumFloorY;          // walkable ring surface (== Y10)
        const float voidR      = 9.0f;                  // central VOID radius (open down-look)
        const float galOuterR  = 17.0f;                 // ring outer radius (~8 m wide balcony)
        // Ring built as a fan of trapezoidal SEGMENTS around (cx,cz). Each segment is a
        // white-clad box laid along its mid-radius arc; collision on each so it's walkable.
        const int   galSeg     = 16;                    // ring segments (+ terminal slots)
        const float galMidR    = (voidR + galOuterR) * 0.5f;
        // Persistent gallery glass state (drawn each frame via the BLEND path, like the
        // deck/elevator glass). The slats + rim pane are authored at WORLD positions so
        // their model matrix is a per-slat rotation about the slat center.
        struct GalGlass { x3::rhi::MeshHandle mesh; float model[16]; float r,g,b,a,emis; };
        std::vector<GalGlass> galGlass;
        // Holographic terminals around the ring + the analyst figures at a subset.
        std::vector<x3::game::HoloTerminal> galTerms;
        std::vector<x3::game::RescueVictim> galAnalysts;
        galTerms.reserve(galSeg);
        galAnalysts.reserve(6);
        {
            // ---- (1) WALKABLE RING FLOOR — galSeg trapezoid segments forming an annulus
            // around the void, white-clad + collision. A small thick slab per segment
            // (top at galleryY) tangent to its arc. Gaps at the strut-landing + elevator
            // sides are bridged by the existing atrium/bridge floor (both at atriumFloorY).
            const float ringHalfRad = (galOuterR - voidR) * 0.5f;   // radial half-extent of a segment
            const float ringHY = 0.22f;                              // 0.44 m thick floor slab
            const float ringCY = galleryY - ringHY;                  // center so TOP == galleryY
            for (int s = 0; s < galSeg; ++s) {
                const float ang = (6.2831853f * (s + 0.5f)) / (float)galSeg;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float segX = cx + ca * galMidR, segZ = cz + sa * galMidR;
                // Tangential half-width sized so adjacent segments overlap into a closed ring.
                const float tanHW = (3.14159265f * galMidR) / (float)galSeg + 0.35f;
                // Author the segment as an axis-aligned box then rotate it to lie along the
                // arc tangent via a yaw model matrix (addWhiteMesh uses world meshes; here we
                // build a rotated box by composing the rotation into vertices is overkill —
                // instead use a radial-aligned box: radial = ringHalfRad, tangential = tanHW,
                // approximated axis-aligned per-segment which is fine at 16 segments).
                // Build the segment in LOCAL (radial=x, tangential=z) then place rotated:
                // a white-clad render box + a matching rotated static collision body.
                x3::prims::PrimMesh seg = x3::prims::makeBox(ringHalfRad, ringHY, tanHW, 0,0,0, 0.3f);
                // Rotate verts by `ang` about Y so radial axis points outward.
                for (auto& v : seg.verts) {
                    const float lx = v.pos[0], lz = v.pos[2];
                    v.pos[0] = lx * ca - lz * sa + segX;
                    v.pos[1] += ringCY;
                    v.pos[2] = lx * sa + lz * ca + segZ;
                    const float nx = v.normal[0], nz = v.normal[2];
                    v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                }
                x3::rhi::MeshHandle m = makeWorldMesh(seg);
                x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
                sscene.add(e);
                // Collision: a small axis-aligned box at the segment center (a touch larger so
                // the ring is seamlessly walkable; the player capsule never notices the facets).
                sphys->addBox({ tanHW*std::fabs(sa) + ringHalfRad*std::fabs(ca) + 0.1f, ringHY,
                                tanHW*std::fabs(ca) + ringHalfRad*std::fabs(sa) + 0.1f },
                              { segX, ringCY, segZ }, 0.0f, x3::phys::Layer::Static);
            }
            // ---- (2) VOID-EDGE PARAPET + DARK-GLASS BALUSTRADE — an ELEGANT, THIN,
            // sleek treatment of the void rim (replacing the old chunky tilted-slat
            // "louver" ring that read as a crude gear/cog). It mirrors the Unity
            // interior's glass railings: a SMOOTH LOW PARAPET (a thin white kerb + a
            // slim white cap rail) topped by a continuous band of FLAT DARK-TINTED
            // GLASS held by SLIM METAL MULLIONS. The analysts still get a dark
            // look-down onto the civilians (the glass is dark-tinted, see-through
            // looking down through the open void below the glass band), and the
            // railing/safety read is preserved. Minimal geometry: a clean kerb ring,
            // a thin cap ring, slim mullion posts, and ONE merged flat glass band.
            //   * kerb     — a thin white solid ~0.35 m tall at the void edge (the
            //                low parapet base; gives a clean lip + collision).
            //   * cap rail — a slim white bar capping the glass band (the handrail).
            //   * mullions — slim dark metal posts at each segment (hold the glass).
            //   * glass    — ONE merged ring of FLAT vertical dark-tinted panes via the
            //                existing BLEND path (galGlass), set just inboard of the kerb.
            const float kerbH       = 0.35f;                 // low parapet kerb height (m)
            const float kerbHY      = kerbH * 0.5f;
            const float kerbHRad    = 0.07f;                 // thin radial half-thickness
            const float glassH      = 0.62f;                 // dark-glass band height (m)
            const float glassTopY   = galleryY + kerbH + glassH;  // top of the glass = handrail height (~1.0 m)
            const float capHY       = 0.04f;                 // slim cap-rail half-height
            const float capHRad     = 0.10f;                 // slim cap-rail radial half-depth
            const float mullHRad    = 0.045f, mullHTan = 0.045f;  // slim mullion post half-dims
            // Dark-tinted FLAT glass (smoky, low transmission) — same dark tint family
            // the deck/elevator glass uses; alpha = BLEND opacity (dark, see-through down).
            const float galGlassR = 0.030f, galGlassG = 0.040f, galGlassB = 0.065f;
            const float galGlassA = 0.62f;
            // The kerb + cap rails are rotated boxes at each segment (like the ring floor).
            for (int s = 0; s < galSeg; ++s) {
                const float ang = (6.2831853f * (s + 0.5f)) / (float)galSeg;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float tanHW = (3.14159265f * voidR) / (float)galSeg + 0.25f;
                // Place a white box (radial half=hRad, given height) at radius `rad`,
                // centered at world Y `cy`, rotated to lie along the arc tangent. Adds a
                // render entity (clean white panel) + a matching static collision body.
                auto placeRing = [&](float rad, float cy, float hRad, float hY, float colTanPad) {
                    const float rx = cx + ca * rad, rz = cz + sa * rad;
                    x3::prims::PrimMesh b = x3::prims::makeBox(hRad, hY, tanHW, 0,0,0, 1.0f);
                    for (auto& v : b.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + rx;
                        v.pos[1] += cy;
                        v.pos[2] = lx * sa + lz * ca + rz;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                    }
                    x3::rhi::MeshHandle m = makeWorldMesh(b);
                    x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                    e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
                    sscene.add(e);
                    if (colTanPad >= 0.0f)
                        sphys->addBox({ tanHW*std::fabs(sa) + hRad*std::fabs(ca) + colTanPad, hY,
                                        tanHW*std::fabs(ca) + hRad*std::fabs(sa) + colTanPad },
                                      { rx, cy, rz }, 0.0f, x3::phys::Layer::Static);
                };
                // Low parapet KERB at the void edge (collision = the safety barrier).
                placeRing(voidR + 0.10f, galleryY + kerbHY, kerbHRad, kerbHY, 0.05f);
                // Slim CAP RAIL atop the glass band (the handrail; thin collision lid).
                placeRing(voidR + 0.10f, glassTopY + capHY, capHRad, capHY, 0.0f);
            }
            // SLIM MULLIONS — a dark metal post at each segment boundary, spanning kerb
            // top -> cap, holding the glass. Built white-clad-mesh path but tinted dark.
            {
                const float mullBaseY = galleryY + kerbH;
                const float mullHY    = glassH * 0.5f;
                for (int s = 0; s < galSeg; ++s) {
                    const float ang = (6.2831853f * (float)s) / (float)galSeg;   // on segment edges
                    const float ca = std::cos(ang), sa = std::sin(ang);
                    const float rx = cx + ca * (voidR + 0.10f), rz = cz + sa * (voidR + 0.10f);
                    x3::prims::PrimMesh post = x3::prims::makeBox(mullHRad, mullHY, mullHTan, 0,0,0, 1.0f);
                    for (auto& v : post.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + rx;
                        v.pos[1] += mullBaseY + mullHY;
                        v.pos[2] = lx * sa + lz * ca + rz;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                    }
                    x3::rhi::MeshHandle m = makeWorldMesh(post);
                    x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                    // Slim dark metal mullion tint (cool gunmetal, distinct from the white).
                    e.baseColor[0]=0.16f; e.baseColor[1]=0.18f; e.baseColor[2]=0.22f; e.baseColor[3]=1.0f;
                    sscene.add(e);   // visual only (no collision; the kerb is the barrier)
                }
            }
            // ---- (3) FLAT DARK-GLASS BAND — ONE merged ring of FLAT VERTICAL dark-tinted
            // panes spanning the kerb top -> cap rail, just inboard of the void edge. The
            // dark tint gives the analysts a shaded look-down onto the civilians (and the
            // void is OPEN below the glass band, so the gallery still overlooks the floor);
            // the flat vertical glass reads as a sleek railing pane (NOT a lumpy louver).
            // Drawn via the existing BLEND path (galGlass) as a SINGLE merged draw.
            {
                const int   galGlassSeg = 32;                 // panes around the ring (smooth band)
                const float glassMidY   = galleryY + kerbH + glassH * 0.5f;
                const float glassHY     = glassH * 0.5f;
                const float glassHTan   = (3.14159265f * voidR) / (float)galGlassSeg + 0.10f;
                const float glassHThk   = 0.02f;              // thin flat pane
                x3::prims::PrimMesh glassMerged;
                for (int s = 0; s < galGlassSeg; ++s) {
                    const float ang = (6.2831853f * (s + 0.5f)) / (float)galGlassSeg;
                    const float ca = std::cos(ang), sa = std::sin(ang);
                    const float panX = cx + ca * (voidR + 0.05f), panZ = cz + sa * (voidR + 0.05f);
                    // Flat vertical pane: radial=thin, height=glassHY, tangential=glassHTan;
                    // rotate about Y to lie along the arc tangent (NO tilt -> flat band).
                    x3::prims::PrimMesh pane = x3::prims::makeBox(glassHThk, glassHY, glassHTan, 0,0,0, 1.0f);
                    const uint32_t vb = (uint32_t)glassMerged.verts.size();
                    for (auto v : pane.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + panX;
                        v.pos[1] += glassMidY;
                        v.pos[2] = lx * sa + lz * ca + panZ;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                        glassMerged.verts.push_back(v);
                    }
                    for (uint32_t idx : pane.index) glassMerged.index.push_back(vb + idx);
                }
                GalGlass gg{}; gg.mesh = makeWorldMesh(glassMerged);
                gg.model[0]=1;gg.model[5]=1;gg.model[10]=1;gg.model[15]=1;   // identity (verts in world)
                gg.r=galGlassR; gg.g=galGlassG; gg.b=galGlassB; gg.a=galGlassA; gg.emis=0.03f;
                galGlass.push_back(gg);
            }

            // ---- (4) HOLOGRAPHIC TERMINALS — reuse holo_terminal.cpp. One per segment
            // slot, on the OUTER side of the ring facing INWARD toward the void (so an
            // analyst standing between the terminal and the rail watches the floor below
            // over the readout). ~10-12 around the ring. Each seeds a surveillance-feed
            // readout. Terminal anchor sits at chest height on the ring floor.
            const int kNumTerms = 11;                 // ~10-12 terminals
            const float termR = galOuterR - 2.2f;     // terminal stands near the outer wall
            const float termY = galleryY + 1.25f;     // screen center at chest/eye height
            for (int t = 0; t < kNumTerms; ++t) {
                const float ang = (6.2831853f * t) / (float)kNumTerms + 0.18f;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float tx = cx + ca * termR, tz = cz + sa * termR;
                // Face the terminal INWARD (toward the void center). HoloTerminal::build
                // yaws the screen's local +Z front by `yaw`; aim it at (cx,cz) so the
                // readout faces an analyst standing between the terminal and the rail.
                const float inwardYaw = std::atan2(cx - tx, cz - tz);
                galTerms.emplace_back();
                x3::game::HoloTerminal& term = galTerms.back();
                term.build(sscene, *device, x3::phys::Vec3{ tx, termY, tz }, inwardYaw,
                           1.2f, 0.78f, /*ceilingY*/ galleryY + 2.6f);
                // Surveillance-feed readout (line 0 = header title; 1+ = data rows).
                term.setLines({
                    std::string("SURVEILLANCE FEED ") + (char)('A' + (t % 8)) +
                        "-" + std::to_string(10 + t),
                    "ZONE: CIVILIAN ATRIUM",
                    "TRACKING: ACTIVE",
                    std::string("CONTACTS: ") + std::to_string(3 + (t * 5) % 9),
                    "BIOMETRICS: NOMINAL",
                    "ONE-WAY GLASS: ENGAGED",
                });
            }
            x3::logInfo("--world showroom: GALLERY terminals = " + std::to_string(galTerms.size()) +
                        " around void (r=" + std::to_string(termR) + ") facing in");

            // ---- (5) ANALYST FIGURES — a modest subset (4) of skinned idle figures at
            // four spread terminals, facing their terminal/the void. Reuse RescueVictim
            // (the AnnaCasual_anim.glb idle path Aria uses); built as Captive + never
            // rescued + hubReached=false so the timer never runs and there's no follow AI
            // — they just idle (breathe) in place. setFacing aims each at the void center.
            const int kNumAnalysts = 4;
            const int kTermsPerAnalyst = (galTerms.empty() ? 1 : (int)galTerms.size()) / kNumAnalysts;
            for (int aN = 0; aN < kNumAnalysts; ++aN) {
                const int slot = aN * std::max(1, kTermsPerAnalyst);
                const float ang = (6.2831853f * slot) / (float)kNumTerms + 0.18f;
                const float ca = std::cos(ang), sa = std::sin(ang);
                // Stand ~1.4 m IN from the terminal (between it and the rail) on the ring.
                const float aR = galOuterR - 3.6f;
                const float axp = cx + ca * aR, azp = cz + sa * aR;
                galAnalysts.emplace_back();
                x3::game::RescueVictim& an = galAnalysts.back();
                an.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                         x3::phys::Vec3{ axp, galleryY, azp },
                         x3::game::VictimId::Aria, std::string("Analyst") + std::to_string(aN + 1),
                         "AnnaCasual_anim.glb", 1e9f /*never expires*/,
                         x3::game::MonsterSystem::Tuning{});
                // Face the void center (and thus the terminal, which is just outward of it).
                // headingToFace law (CONVENTIONS): yaw = atan2(-dirX,-dirZ) points local -Z
                // along (dirX,dirZ); dir = center - self.
                an.setFacing(std::atan2(-(cx - axp), -(cz - azp)));
                // A cool analyst tint (distinct from Aria's friendly cyan).
                an.setTint(0.78f, 0.82f, 0.92f, 1.0f);
            }
            x3::logInfo("--world showroom: GALLERY analysts = " + std::to_string(galAnalysts.size()) +
                        " (skinned idle, facing the void) + dark-glass parapet = " +
                        std::to_string(galGlass.size()) + " merged BLEND mesh (flat dark-glass balustrade band)");
        }
        x3::logInfo("--world showroom: hidden-trigger = KEYPAD code-entry (press E at the STRUT-FACE door, type code " +
                    std::to_string(HATCH_CODE) + ", Enter to submit) — the white panel slides aside, stair climbs inside the strut");

        // ===================================================================
        // CIVILIAN FIGURES — the public milling on the GROUND floor + 2nd-floor
        // mezzanine (the museum-lobby crowd). EXACT same reuse pattern as the
        // companion Aria + the gallery ANALYSTS: each is a RescueVictim built
        // Captive, AnnaCasual_anim.glb idle, timer 1e9 (never expires),
        // hubReached=false (no countdown, no follow AI) -> they just idle/breathe
        // in place. setFacing() holds each at a natural static heading (small
        // groups facing each other / facing out the glass / toward the blue pad).
        // WARMER, VARIED tints (vs the analysts' cool blue-grey 0.78/0.82/0.92)
        // so they read as the PUBLIC, not staff. Positions are hand-jittered (NOT
        // a grid) on the walkable floors: GROUND at y=floorY (-9) around the
        // central blue pad + lounge, 2ND FLOOR at y=floor2Y (3) on the Room_01
        // mezzanine deck. setFacing law (CONVENTIONS / setFacing doc): yaw =
        // atan2(-dirX,-dirZ) aims the model's local -Z along (dirX,dirZ); to face
        // a target T from self S pass dir = T - S.
        //
        // PERF: each skinned tick() does a GPU readback (vkDeviceWaitIdle) ->
        // costly under 4x headless SSAA. The headless proofs POSE these on the
        // first ~2 frames then render static (see the proof loops). Total skinned
        // figures kept modest: Aria(1) + analysts(4) + civilians(8) = 13.
        std::vector<x3::game::RescueVictim> civilians;
        civilians.reserve(8);
        {
            // A civilian = {x,z, facing-target x,z, tint r,g,b, y-level}. Facing a
            // target point reads more natural than a raw yaw (groups face each
            // other / the pad / out the glass). Warm/varied civilian palette.
            struct Civ { float x, z, tx, tz, r, g, b, y; const char* name; };
            const float padX = cx, padZ = cz;            // central blue pad center (social heart)
            const std::vector<Civ> civDefs = {
                // ---- GROUND floor (y=floorY): ~5 around the blue pad + lounge ----
                // A) a chatting PAIR just off the pad's +X edge, facing each other.
                { padX + 4.6f, padZ + 1.2f,  padX + 6.2f, padZ + 2.0f,  0.86f, 0.52f, 0.46f, floorY, "Civ_PairA" }, // warm terracotta
                { padX + 6.4f, padZ + 2.4f,  padX + 4.6f, padZ + 1.2f,  0.52f, 0.66f, 0.40f, floorY, "Civ_PairB" }, // warm olive
                // B) a lone visitor at the pad's -X lounge edge, gazing OUT the glass (-X).
                { padX - 5.8f, padZ - 1.5f,  padX - 40.0f, padZ - 1.5f, 0.80f, 0.74f, 0.42f, floorY, "Civ_Gazer" }, // warm gold
                // C) a small group of two on the -Z lounge arc, facing IN toward the pad.
                { padX - 1.4f, padZ - 6.2f,  padX,         padZ,        0.74f, 0.50f, 0.70f, floorY, "Civ_TrioA" }, // warm mauve
                { padX + 2.0f, padZ - 7.0f,  padX,         padZ,        0.58f, 0.62f, 0.84f, floorY, "Civ_TrioB" }, // soft periwinkle
                // ---- 2ND-FLOOR mezzanine (y=floor2Y): ~3 on the Room_01 deck ----
                // Deck footprint x~[29..115], z~[-123..-99]; place clear of the central
                // tower gap (x~71..73) + the void, looking along the deck / down at the pad.
                // D) a pair at the +X end of the deck, facing each other near the rail.
                { 96.0f, -114.0f,  93.0f, -112.0f,  0.84f, 0.58f, 0.40f, floor2Y, "Civ_MezzA" }, // warm amber
                { 92.0f, -112.5f,  96.0f, -114.0f,  0.66f, 0.78f, 0.62f, floor2Y, "Civ_MezzB" }, // sage
                // E) a lone figure at the -X end of the deck, looking DOWN toward the pad below.
                { 40.0f, -107.0f,  cx,    cz,        0.82f, 0.66f, 0.50f, floor2Y, "Civ_MezzC" }, // warm tan
            };
            for (const auto& c : civDefs) {
                civilians.emplace_back();
                x3::game::RescueVictim& cv = civilians.back();
                cv.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                         x3::phys::Vec3{ c.x, c.y, c.z },
                         x3::game::VictimId::Aria, c.name,
                         "AnnaCasual_anim.glb", 1e9f /*never expires*/,
                         x3::game::MonsterSystem::Tuning{});
                cv.setFacing(std::atan2(-(c.tx - c.x), -(c.tz - c.z)));
                cv.setTint(c.r, c.g, c.b, 1.0f);
            }
            x3::logInfo("--world showroom: CIVILIANS = " + std::to_string(civilians.size()) +
                        " skinned idle (5 ground @ y=" + std::to_string(floorY) +
                        " around blue pad + lounge, 3 mezzanine @ y=" + std::to_string(floor2Y) +
                        "), warm/varied tint, facing pad/each-other/glass");
        }
#if 0
        // -------------------------------------------------------------------
        // [SUPERSEDED] STAGE 2/3 — HIDDEN 2ND-FLOOR WALL DOOR -> entry passage -> 90 deg
        // turn -> flight of stairs UP -> ELEVATOR ATRIUM. REPLACED by the strut-face
        // keypad door + internal strut stair above. Kept under #if 0 for reference only.
        // Concealed entrance on the 2ND FLOOR: a flush WHITE wall panel set into the
        // GLB Pilar_01/02 left BACK wall (z~-121) — chosen because the player has
        // ample 2nd-floor room IN FRONT of it (interior side, z>-120) to walk up and
        // face it, while CONCEALED space sits behind it (z<-122). On the keypad code
        // (2742) the panel SLIDES ASIDE; you walk -Z into the passage, TURN +X, then
        // a grand FLIGHT OF STAIRS climbs +X-and-up to the ELEVATOR ATRIUM where the
        // glass lift boards. The keypad mechanic + code are UNCHANGED (the hatch*
        // variables below drive the WALL DOOR; the same KeypadEntry + value()==
        // kShowroomHatchCode gate that --test-hatchcode shares).
        // -------------------------------------------------------------------
        // ---- The hidden DOOR (a flush white wall panel) set into a WHITE PARTITION
        // WALL I build across the open 2nd-floor mid-room (clear of the thick GLB
        // structural walls at z~-101 / z~-121, which would otherwise occlude it). The
        // partition is axis-aligned + clad in the same white panels, so it reads as a
        // built interior wall; its sill rests on the 2nd floor (floor2Y). The player,
        // having climbed to the 2nd floor (landing at z~-104), faces -Z toward it.
        const float doorX = 52.0f;             // door center X (left half, near the climb landing)
        const float doorZ = -106.0f;           // partition plane (open mid-room, clear of GLB walls)
        const float doorHalfW = 1.2f;          // 2.4 m wide opening
        const float doorHalfH = 1.25f;         // 2.5 m tall (fits under the GLB Tube vault at y~6)
        const float doorPanelHZ = 0.18f;       // panel thickness (set into the partition)
        const float doorCY = floor2Y + doorHalfH;   // panel center (sill on the 2nd floor)
        // Keep the proven keypad-host variable NAMES (hatch*) so the interaction +
        // smoke code is untouched; they now address the WALL DOOR on the 2nd floor.
        const float hatchX = doorX;            // door center X (proximity test)
        const float hatchZ = doorZ;            // door plane Z (proximity test)
        const float hatchHalf = doorHalfW;     // proximity half-window
        const float doorFloorY = floor2Y;      // the floor the player stands on to use it
        constexpr int HATCH_CODE = kShowroomHatchCode;   // 2742 (UNCHANGED) — themed "ARIA"

        // WHITE PARTITION WALL the door sits in: flanking jambs + a lintel above,
        // floor-to-vault, clad in white panels + solid collision. The door opening is
        // the only gap (the player approaches from +Z, the climb-landing side).
        const float partHalfH = 1.4f;          // partition half-height (~2.8 m, to the vault)
        const float partCY = floor2Y + partHalfH;
        {
            const float jambW = 5.0f;          // jamb half-extent each side
            // -X jamb + +X jamb (the door opening is the gap between them).
            addWhiteBox(doorX - doorHalfW - jambW, partCY, doorZ, jambW, partHalfH, doorPanelHZ);
            addWhiteBox(doorX + doorHalfW + jambW, partCY, doorZ, jambW, partHalfH, doorPanelHZ);
            // Lintel above the opening up to the partition top.
            addWhiteBox(doorX, (floor2Y + doorHalfH*2.0f + (floor2Y + partHalfH*2.0f))*0.5f, doorZ,
                        doorHalfW, ((floor2Y + partHalfH*2.0f) - (floor2Y + doorHalfH*2.0f))*0.5f, doorPanelHZ);
        }

        // Concealed door PANEL: a thin white box flush in the partition, clad in the
        // SAME white panel texture so it is invisible until opened. It slides +X aside
        // on unlock; its Static collision body is REMOVED on open so the player walks
        // through. A faint cyan glow pulses when the player is near + it's still closed.
        x3::rhi::MeshHandle hatchMesh = makeWorldMesh(
            x3::prims::makeBox(doorHalfW, doorHalfH, doorPanelHZ, 0, 0, 0, 0.25f));
        x3::game::Entity hatchEnt; hatchEnt.mesh = hatchMesh; hatchEnt.tex = whitePanelTex;
        hatchEnt.baseColor[0]=kWhite[0]; hatchEnt.baseColor[1]=kWhite[1]; hatchEnt.baseColor[2]=kWhite[2]; hatchEnt.baseColor[3]=1.0f;
        hatchEnt.emissive[0]=0.10f; hatchEnt.emissive[1]=0.45f; hatchEnt.emissive[2]=0.55f; hatchEnt.emissive[3]=0.0f; // glows only when armed
        hatchEnt.transform[12]=doorX; hatchEnt.transform[13]=doorCY; hatchEnt.transform[14]=doorZ;
        const uint32_t hatchIdx = sscene.add(hatchEnt);
        // The panel's solid collision while CLOSED (it seals the wall). Removed on open.
        x3::phys::BodyId hatchLidBody =
            sphys->addBox({ doorHalfW, doorHalfH, doorPanelHZ }, { doorX, doorCY, doorZ }, 0.0f, x3::phys::Layer::Static);
        bool  hatchOpen = false;        // latched once triggered
        float hatchSlide = 0.0f;        // 0=closed .. 1=fully slid aside

        // ---- Aligned interior run, all WHITE-clad + walkable, behind the door:
        //   ENTRY PASSAGE : -Z from the door into the open mid-room (y=floor2Y).
        //   90 deg TURN   : dogleg from -Z to +X.
        //   STAIRS UP     : a grand +X flight climbing floor2Y -> atriumFloorY.
        //   ELEVATOR ATRIUM: a white room at y=atriumFloorY enclosing the lift shaft.
        const float passHalfW   = 1.6f;        // passage/turn corridor half-width
        const float passTopGap  = 2.4f;        // interior head-height (fits under the GLB vault)
        const float passDoorZ   = doorZ - doorPanelHZ;   // -Z (concealed) face of the door
        const float passEndZ    = -113.0f;     // back of the entry passage (the turn corner)
        const float turnCX      = doorX;        // the dogleg corner X (== door X)
        const float upStairLowX = doorX + passHalfW + 1.0f;  // stair low edge (on the 2nd floor)
        const float upStairRun  = 18.0f;       // +X run (gentle grand flight)
        const float upStairRise = atriumFloorY - floor2Y;   // 6 m
        const float stairZ      = passEndZ;     // the +X stair runs along the turn-corner Z line
        const float atriumX0    = 70.0f, atriumX1 = 91.0f;  // atrium X span (encloses shaftX=81)
        const float atriumZ0    = -115.0f, atriumZ1 = -98.5f; // atrium Z span (encloses shaftZ=-100)
        // Helper: a white floor slab (top at topY) of the given XZ rect.
        auto whiteFloor = [&](float x0, float x1, float z0, float z1, float topY) {
            addWhiteBox((x0+x1)*0.5f, topY - 0.25f, (z0+z1)*0.5f, (x1-x0)*0.5f, 0.25f, (z1-z0)*0.5f);
        };
        // Helper: a white ceiling slab (bottom at botY).
        auto whiteCeil = [&](float x0, float x1, float z0, float z1, float botY) {
            addWhiteBox((x0+x1)*0.5f, botY + 0.15f, (z0+z1)*0.5f, (x1-x0)*0.5f, 0.15f, (z1-z0)*0.5f);
        };
        // (A) ENTRY PASSAGE: floor + ceiling + the two side walls, from the door
        // (z=passDoorZ) -Z back to the turn corner (z=passEndZ), centered on doorX.
        {
            const float wallHy = passTopGap * 0.5f, wallCy = floor2Y + wallHy;
            whiteFloor(turnCX - passHalfW - 0.3f, turnCX + passHalfW + 0.3f, passEndZ + passHalfW - 0.3f, passDoorZ, floor2Y);
            whiteCeil (turnCX - passHalfW - 0.3f, turnCX + passHalfW + 0.3f, passEndZ + passHalfW - 0.3f, passDoorZ, floor2Y + passTopGap);
            // -X side wall of the passage (full length). +X side wall ONLY on the door
            // half (the -Z end opens via the turn into the +X stair run).
            addWhiteBox(turnCX - passHalfW - 0.15f, wallCy, (passEndZ + passDoorZ)*0.5f, 0.15f, wallHy, (passDoorZ - passEndZ)*0.5f);
            addWhiteBox(turnCX + passHalfW + 0.15f, wallCy, (passDoorZ + (passEndZ + 2*passHalfW))*0.5f, 0.15f, wallHy, (passDoorZ - (passEndZ + 2*passHalfW))*0.5f);
        }
        // (B) 90 deg TURN corner: a white floor+ceiling patch at (turnCX,passEndZ)
        // bridging the -Z passage into the +X stair run; a -Z back wall seals the corner.
        {
            const float wallHy = passTopGap * 0.5f, wallCy = floor2Y + wallHy;
            whiteFloor(turnCX - passHalfW - 0.3f, upStairLowX + 0.5f, passEndZ - passHalfW - 0.3f, passEndZ + passHalfW + 0.3f, floor2Y);
            whiteCeil (turnCX - passHalfW - 0.3f, upStairLowX + 0.5f, passEndZ - passHalfW - 0.3f, passEndZ + passHalfW + 0.3f, floor2Y + passTopGap);
            addWhiteBox((turnCX + upStairLowX)*0.5f, wallCy, passEndZ - passHalfW - 0.15f, (upStairLowX + passHalfW - turnCX)*0.5f + 0.3f, wallHy, 0.15f); // -Z back wall
        }
        // (C) STAIRS UP: a grand white ramp climbing +X from the 2nd floor (floor2Y) to
        // the atrium (atriumFloorY), centered on stairZ. Matches the building's stairs.
        {
            x3::prims::PrimMesh ramp = x3::prims::makeRamp(upStairLowX, floor2Y, stairZ,
                                                          passHalfW, upStairRun, upStairRise,
                                                          /*axis*/0 /*+X*/, /*dir*/+1.0f, 0.25f);
            x3::rhi::MeshHandle m = makeWorldMesh(ramp);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            sscene.add(e);
            sphys->addStaticMesh(ramp.cverts.data(), (uint32_t)ramp.cverts.size() / 3,
                                 ramp.cindex.data(), (uint32_t)ramp.cindex.size());
        }
        // (D) ELEVATOR ATRIUM: a white room at atriumFloorY around the lift shaft. The
        // stair tops out at its -Z/-X corner; the player walks +Z across it to board the
        // cab. Floor slab + a high ceiling + three bounding walls (+Z left open for the
        // shaft view + the deck above). The shaft passes up through the ceiling gap.
        {
            whiteFloor(atriumX0, atriumX1, atriumZ0, atriumZ1, atriumFloorY);
            const float atriumWallHy = 2.2f, atriumWallCy = atriumFloorY + atriumWallHy;  // 4.4 m walls
            // Ceiling with a gap around the shaft so the cab rises through it.
            whiteCeil(atriumX0, shaftX - carHX - 0.6f, atriumZ0, atriumZ1, atriumFloorY + 4.6f); // -X of shaft
            whiteCeil(shaftX + carHX + 0.6f, atriumX1, atriumZ0, atriumZ1, atriumFloorY + 4.6f); // +X of shaft
            // Bounding walls: -X, +X, -Z. (+Z left open toward the front / shaft.)
            addWhiteBox(atriumX0 - 0.15f, atriumWallCy, (atriumZ0+atriumZ1)*0.5f, 0.15f, atriumWallHy, (atriumZ1-atriumZ0)*0.5f); // -X wall
            addWhiteBox(atriumX1 + 0.15f, atriumWallCy, (atriumZ0+atriumZ1)*0.5f, 0.15f, atriumWallHy, (atriumZ1-atriumZ0)*0.5f); // +X wall
            addWhiteBox((atriumX0+atriumX1)*0.5f, atriumWallCy, atriumZ0 - 0.15f, (atriumX1-atriumX0)*0.5f, atriumWallHy, 0.15f); // -Z wall
            x3::logInfo("--world showroom: STAGE2/3 hidden wall door(" + std::to_string(doorX) + "," +
                        std::to_string(doorZ) + ") on the 2nd floor y=" + std::to_string(floor2Y) +
                        " -> passage(-Z to z=" + std::to_string(passEndZ) + ") -> turn(+X) -> stair rise " +
                        std::to_string(upStairRise) + " m -> atrium floor y=" + std::to_string(atriumFloorY) +
                        " (shaft " + std::to_string(shaftX) + "," + std::to_string(shaftZ) + ")");
        }
        x3::logInfo("--world showroom: hidden-trigger = KEYPAD code-entry (press E at the 2nd-floor wall door, type code " +
                    std::to_string(HATCH_CODE) + ", Enter to submit) — the white panel slides aside");
#endif // [SUPERSEDED] old 2nd-floor wall-door entrance

        // ===================================================================
        // INTERIOR LIGHTING (forward POINT LIGHTS) — make the walkable INTERIOR
        // read like a clean, bright, evenly-lit white Unity interior, WITHOUT
        // touching the NIGHT sky/sun/ambient (those stay dark so the sky + planets
        // are unchanged outside). mesh.frag accumulates these on TOP of the dim
        // moonlight sun + cool ambient, so the white-panel slab/passage/stair/
        // atrium catch them and read clean white; the building's GLB emissive
        // fixtures (halogen/tube/showcase/tv_screen) still glow via their material
        // emissive (a small bloom nudge below makes them read as ceiling/strips).
        //
        // ALL lights are COOL-WHITE (color ~(1,1,1.05) pre-multiplied by an
        // intensity) placed near ceiling height in each space, range ~10-16 m,
        // spaced so the floors/walls light evenly (no dark pools, no hot blobs).
        // Shared block => applies to BOTH the interactive --world showroom AND the
        // headless proof flags. Budget: kMaxPointLights = 64.
        //   *** TUNING KNOBS: kPL_I (intensity), kPL_R (range), grid steps below.
        // plights is declared at BLOCK scope (full NIGHT intensity) so the live 'T'
        // toggle can re-push it scaled (DAY x0.3) / full (NIGHT) via the helper.
        // -------------------------------------------------------------------
        std::vector<x3::rhi::PointLight> plights;
        {
            plights.reserve(64);
            // Cool-white tint, slightly blue-biased. The 3 color channels are
            // PRE-MULTIPLIED by the intensity so the shader sees color*intensity.
            const float kPL_I = 3.4f;          // *** master interior intensity (brighter -> clean Unity-white interior, dominates the blue night ambient)
            const float kPL_R = 13.0f;         // *** master range (m); attenuation -> 0 here
            auto addLight = [&](float x, float y, float z, float range, float intensity) {
                if (plights.size() >= 64) return;
                x3::rhi::PointLight pl{};
                pl.pos[0] = x; pl.pos[1] = y; pl.pos[2] = z;
                pl.range  = range;
                pl.color[0] = 1.04f * intensity;   // clean, faintly WARM white (interior fill) —
                pl.color[1] = 1.00f * intensity;   // counters the cold blue night ambient so the
                pl.color[2] = 0.96f * intensity;   // white panels read crisp like the Unity interior
                plights.push_back(pl);
            };

            // (1) GROUND entrance / spawn area — a grid a few metres above the
            // ground floor over the building footprint center, so the spawn room
            // + the foot of the climb stair read lit. floorY ~ -9; ceiling is open,
            // so hang the lights ~6 m up. 3x3 grid centered on (cx,cz), ~14 m step.
            {
                const float gY = floorY + 6.0f;
                const float gStep = 14.0f;
                for (int ix = -1; ix <= 1; ++ix)
                    for (int iz = -1; iz <= 1; ++iz)
                        addLight(cx + ix * gStep, gY, cz + iz * gStep, 16.0f, kPL_I);
                // Extra light at the foot of the climb stair so the ascent reads.
                addLight(climbCX, floorY + 5.0f, stairLowZ + 3.0f, 14.0f, kPL_I);

                // WAVE-2B (LD review #2): the player's FIRST walkable frame at the ground
                // spawn read ~85% black (captures/ldreview2_showroom.png). The cool grid
                // above hangs high + neutral, so the entry floor stayed dark. Add ONE WARM
                // KEY low over the spawn, pushed a few metres toward Aria / the climb stair
                // (the gallery objective, +Z) so it both LIFTS the entry passage and PULLS
                // the eye forward into the space — a warm-vs-cool contrast that reads as a
                // welcoming threshold light, not another ceiling fill. Bright + close so the
                // near floor + Aria catch it; range covers spawn-to-stair.
                {
                    // Dead ahead of the FP spawn eye (which looks +Z, level), low + strong so
                    // the near entry FLOOR + Aria catch a warm pool that reads against the cold
                    // night interior and pulls the eye forward toward the gallery.
                    x3::rhi::PointLight key{};
                    key.pos[0] = cx + 0.5f; key.pos[1] = floorY + 2.6f; key.pos[2] = cz + 3.0f;
                    key.range  = 34.0f;
                    const float kWarm = 10.0f;                // dominates the cold grid at the entry
                    key.color[0] = 1.10f * kWarm;             // warm amber-white threshold key
                    key.color[1] = 0.82f * kWarm;
                    key.color[2] = 0.55f * kWarm;
                    plights.push_back(key);
                    // A second, softer warm bounce a few metres deeper (+Z) so the pool doesn't
                    // fall off a cliff — carries the warmth down the entry toward the stair.
                    x3::rhi::PointLight key2 = key;
                    key2.pos[0] = cx + 1.0f; key2.pos[1] = floorY + 3.4f; key2.pos[2] = cz + 12.0f;
                    key2.range  = 26.0f;
                    const float kWarm2 = 6.0f;
                    key2.color[0] = 1.10f * kWarm2; key2.color[1] = 0.82f * kWarm2; key2.color[2] = 0.55f * kWarm2;
                    plights.push_back(key2);
                }
            }

            // (2) 2ND-FLOOR Room_01 — x[29,115], z[-122.8,-99.2], floor y=3, vaulted
            // ceiling y~6..13.6. Hang lights ~y=9 (under the vault, above head). A
            // grid across the long X span x 2 rows in Z so the whole slab lights.
            {
                const float fY = floor2Y + 6.0f;       // ~y=9, under the vault
                const float xs[] = { 36.0f, 52.0f, 68.0f, 84.0f, 100.0f, 112.0f };
                const float zs[] = { r2z0 + 6.0f, (r2z0 + r2z1) * 0.5f, r2z1 - 6.0f };
                for (float zx : zs)
                    for (float xx : xs)
                        addLight(xx, fY, zx, kPL_R, kPL_I);
            }

            // (3) HOLLOW STRUT INTERIOR — the hidden stair climbing UP inside the
            // back-left strut from the door foot (floorY) to the head (atriumFloorY).
            // Hang a few lights stepping up the canted axis (base sBL.b* -> head sBL.t*)
            // a touch above the treads, so the white strut interior + the stair read
            // bright + even from the keypad door up to the landing.
            {
                const int kSteps = 4;
                for (int s = 0; s <= kSteps; ++s) {
                    const float t = (float)s / (float)kSteps;
                    const float lx = sBL.bx + (sBL.tx - sBL.bx) * t;
                    const float lz = sBL.bz + (sBL.tz - sBL.bz) * t;
                    const float ly = strutBaseY + (strutTopY - strutBaseY) * t + 2.6f;
                    addLight(lx, ly, lz, 10.0f, kPL_I * 0.9f);
                }
            }

            // (4) ELEVATOR ATRIUM / BRIDGE — the boarding level at atriumFloorY(14)
            // around the shaft, plus the bridge from the strut head. Hang lights ~3 m
            // above the floor so the white room + the glass cab read clean white.
            {
                const float aY = atriumFloorY + 3.0f;
                addLight(shaftX,         aY, shaftZ,         12.0f, kPL_I);
                addLight(shaftX - 6.0f,  aY, shaftZ - 6.0f,  12.0f, kPL_I);
                addLight(shaftX - 6.0f,  aY, shaftZ + 3.0f,  12.0f, kPL_I);
                // Over the strut head landing + the bridge mid-span.
                addLight(sBL.tx, atriumFloorY + 2.6f, sBL.tz, 11.0f, kPL_I * 0.95f);
                addLight((sBL.tx + shaftX) * 0.5f, atriumFloorY + 2.6f,
                         (sBL.tz + shaftZ) * 0.5f, 11.0f, kPL_I * 0.95f);
                // One brighter light over the shaft mouth so the boarding cab pops.
                addLight(shaftX, atriumFloorY + 2.0f, shaftZ, 10.0f, kPL_I * 1.1f);
            }

            x3::logInfo("--world showroom: INTERIOR point lights = " + std::to_string(plights.size()) +
                        "/64 (cool-white, intensity " + std::to_string(kPL_I) + ", range ~" +
                        std::to_string((int)kPL_R) + " m) covering ground/2nd-floor/strut-stair/atrium");
        }
        // APPLY the chosen DAY/NIGHT state: sky/sun/ambient + bloom + the interior point
        // lights (full at night, x0.3 by day). DAY = bright cool Unity-match (snow-bounce
        // ambient dominates, point lights dimmed, low bloom); NIGHT = dark planet sky +
        // dim moon + full fixtures + the HERO bloom (0.22) on the GLB emissive fixtures.
        applyShowroomTimeOfDay(device, gShowroomDay, &plights);
        x3::logInfo(std::string("--world showroom: time-of-day = ") + (gShowroomDay ? "DAY" : "NIGHT"));

        // E-to-talk dialog state (the headless-tested NpcDialog).
        x3::game::NpcDialog npcDialog;
        float       npcBarkTimer = 0.0f;
        std::string npcBarkText;

        // Frame the sun's shadow box on the building so it casts shadows.
        device->setShadowBounds(cx, (bmn[1] + bmx[1]) * 0.5f, cz, 150.0f);

        // (Night-sky planet placement is CELESTIAL — fixed world-space sky directions
        // (az/el table in loadNightSkyPlanets) anchored on the camera eye inside
        // drawNightSkyPlanets each draw — so the old camera-basis "placePlanets" fan
        // is GONE. The bodies neither parallax as the player walks nor glue to the
        // look direction; they wheel past correctly as the view turns, like a sky.)

        // Draw the ADDITIVE translucent glass (deck slab + 4 rails + the riding car)
        // each frame, AFTER the opaque scene/env (the BLEND pass is depth-tested over
        // them). The car follows elev.cabCenter(); deck/rails are world-fixed meshes.
        auto drawAdditiveGlass = [&](const x3::rhi::FrameContext& fr) {
            // Deck slab — cool cyan tint, faint self-glow so it reads at night.
            drawGlass(fr, deckMesh, kIdentity, 0.55f, 0.78f, 0.95f, 0.34f, 0.25f);
            drawGlass(fr, railNZ,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railPZ,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railNX,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railPX,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            // Glass elevator BOX — walls rise from the cab platform top. cabTop =
            // cabCenter().y + platHY; the box center sits carBoxHY above that.
            const x3::phys::Vec3 cc = elev.cabCenter();
            float carModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            carModel[12] = cc.x; carModel[13] = cc.y + platHY + carBoxHY; carModel[14] = cc.z;
            drawGlass(fr, carMesh, carModel, 0.50f, 0.80f, 1.00f, 0.26f, 0.30f);
            // Glass-bottom FLOOR — the see-through plate the rider stands on (centered at
            // the cab plate; collision is the elevator Static body). Ride up + look DOWN.
            float carFloorModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            carFloorModel[12] = cc.x; carFloorModel[13] = cc.y; carFloorModel[14] = cc.z;
            drawGlass(fr, carFloorMesh, carFloorModel, 0.45f, 0.72f, 0.95f, 0.30f, 0.10f);
            // ANALYST GALLERY dark-glass balustrade: the flat dark-tinted glass band
            // (merged ring, world-space verts + identity model) + dark tint/opacity.
            for (const GalGlass& g : galGlass)
                drawGlass(fr, g.mesh, g.model, g.r, g.g, g.b, g.a, g.emis);
        };

        // ===== HEADLESS first-person proof (--screenshot-showroom-fp): one frame from
        // the spawn eye, looking toward Aria (+Z), settle so she skins + bloom registers.
        // --screenshot-showroom-ragdoll reuses the SAME setup but COLLAPSES Aria into a
        // physics ragdoll and steps the world long enough for her to fall into a heap,
        // then captures one frame — proof the ragdoll drives the skin (she's down, not
        // standing). The camera backs up + looks down a touch so the heap fills the frame.
        if (headless) {
            const bool ragShot  = showroomRagdollShot;
            const bool deckShot = showroomDeckShot;
            const bool elevShot = showroomElevShot;
            const bool stairShot= showroomStairShot;
            const bool floor2Shot = showroomFloor2Shot;
            const bool doorShot = showroomDoorShot;
            const bool strutsShot = showroomStrutsShot;
            const bool galleryShot = showroomGalleryShot;
            // FP shot frames Aria standing from the spawn eye (look +Z, level). Ragdoll
            // shot moves the eye CLOSE to her (2.5 m back on the eye->Aria diagonal, lower
            // eye) and AIMS the camera straight at her floor spot so the collapsed heap
            // fills the frame and reads clearly — she's down, not standing.
            // Camera: the ragdoll shot uses the eye placed a short distance from Aria,
            // AIMED directly at her spot so the heap is centered. The FP shot keeps the
            // proven spawn-eye/level look. Aiming at her floor spot means a STANDING Aria
            // fills the frame center; once collapsed the SAME shot shows a low heap there —
            // a direct A/B proof at one framing.
            // Shared helper: brighten the dim moonlit interior for a PROOF capture so the
            // white-panel run reads clearly (headless only — does not touch the live look).
            // NOTE: the interior is now genuinely lit by the INTERIOR POINT LIGHTS
            // set up in the shared block above, so the proofs no longer raise the
            // night sky/sun/ambient to fake brightness — that would also brighten
            // the sky/planets in the shot. brightenProof is kept as a NO-OP so the
            // interior proofs render with the TRUE night values: the interior reads
            // bright from the point lights while the sky stays dark (an honest test
            // of the interior lighting). (Args ignored; kept for call-site shape.)
            auto brightenProof = [&](float /*amb*/, float /*sun*/, float /*expo*/) {
                // intentionally empty — point lights carry the interior now.
            };

            // ===== HIDDEN ANALYST GALLERY proof (--screenshot-showroom-gallery). Captures
            // MULTIPLE frames in one run from the gallery level:
            //   (a) <path>             — the gallery: terminals glowing + analyst figures,
            //                            standing on the ring looking ALONG it.
            //   (b) <path>_down.png    — from the gallery, looking DOWN through the dark
            //                            one-way glass onto the civilian floor/pad.
            //   X3_SHOWROOM_GALLERY_UP=1 swaps (b) for an UP view from the civilian floor at
            //   the dark-glass ceiling band (should read dark — analysts hidden).
            // Settles a few frames so the skinned analysts pose + the terminal holo bakes,
            // then captures each vantage. Self-contained: builds, captures, exits.
            if (galleryShot) {
                static const bool kUpView = (std::getenv("X3_SHOWROOM_GALLERY_UP") != nullptr);
                const float dt = 1.0f / 60.0f;
                float gelapsed = 10.0f;
                // Helper: drive the gallery sub-systems one settle frame + render one frame
                // from the given eye/look, optionally arming a capture to `outPath`.
                // `tickHeavy`: re-pose the skinned analysts + re-bake the holo terminals.
                // Each skinned tick triggers a GPU readback (vkDeviceWaitIdle) so it is
                // EXPENSIVE under 4x SSAA — for a STILL capture we only need to pose them
                // a couple of frames to seat the idle pose + bake the holo textures ONCE,
                // then SKIP the heavy systems and just re-render the (now static) scene.
                auto galTickAndRender = [&](float ex, float ey, float ez, float gyaw, float gpitch,
                                            bool tickHeavy, const char* outPath) {
                    glfwPollEvents();
                    splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                    sphys->step(dt);
                    sscene.update(*sphys);
                    if (tickHeavy) {
                        girl.tick(dt, false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                        for (auto& tm : galTerms) tm.update(dt);
                        for (auto& an : galAnalysts)
                            an.tick(dt, /*hubReached*/false, sscene, *sphys, an.pos());
                        for (auto& cv : civilians)
                            cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                    }
                    gelapsed += dt;
                    device->setCamera(ex, ey, ez, gyaw, gpitch, 80.0f);
                    if (!gShowroomDay) device->setSkyTime(gelapsed);
                    if (outPath) device->armCapture(outPath);
                    auto frame = device->beginFrame();
                    if (frame.valid) {
                        sscene.render(*device, frame);
                        showroom.draw(*device, frame);
                        girl.draw(*device, frame, sscene);
                        for (auto& an : galAnalysts) an.draw(*device, frame, sscene);
                        for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                        drawAdditiveGlass(frame);   // incl. the dark one-way gallery glass
                        if (!gShowroomDay)
                            drawNightSkyPlanets(device, frame, planetMesh, planets, gelapsed,
                                                ex, ey, ez, ringMesh);
                    }
                    device->endFrame(frame);
                };
                // The "_down" / "_up" output paths derived from the base path.
                std::string base = showroomGalleryShotPath;
                std::string stem = base; std::string ext = ".png";
                { const size_t dot = base.find_last_of('.'); if (dot != std::string::npos) { stem = base.substr(0,dot); ext = base.substr(dot); } }
                const std::string downPath = stem + (kUpView ? "_up" : "_down") + ext;
                // ---- Vantage A: ON the gallery ring, looking ALONG/ACROSS it so a run of
                // terminals + the nearest analyst read, with the void + rail in frame.
                // Stand on the ring at one azimuth, look tangentially toward the next slots.
                const float galY  = atriumFloorY;
                // HERO: a raised 3/4 from the ring's outer edge, angled DOWN across the void
                // so the WHOLE gallery reads — the white ring, the OPEN VOID with its dark
                // one-way glass louver rim, the terminals + analyst figures around it, the
                // dome above. Eye out by the wall; aim past the void center onto the rim/glass.
                const float aAng  = 2.3f;
                const float aex = cx + std::cos(aAng) * 15.5f;
                const float aez = cz + std::sin(aAng) * 15.5f;
                const float aey = galY + 4.2f;                    // raised for the down-across angle
                const float atx = cx - std::cos(aAng) * 5.0f;     // aim past the void center
                const float atz = cz - std::sin(aAng) * 5.0f;
                const float aty = galY - 0.8f;                    // tilt down onto the void rim/glass
                {
                    const float vx = atx - aex, vy = aty - aey, vz = atz - aez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float gyaw = std::atan2(vz, vx), gpitch = std::atan2(vy, vlxz);
                    const int kGalSettle = 4;
                    for (int i = 0; i < kGalSettle; ++i)
                        galTickAndRender(aex, aey, aez, gyaw, gpitch, /*tickHeavy*/ i < 2,
                                         (i == kGalSettle - 1) ? base.c_str() : nullptr);
                    const bool w1 = device->captureFrame(base.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (w1 ? "wrote " : "FAILED ") + base);
                }
                // ---- Vantage A2 (<path>_term.png): a PLAYER-height close-up of one analyst
                // at a glowing surveillance terminal, facing the void — proving the terminals
                // render their holo readout + the skinned analyst figures read. Stand just
                // inward of an analyst slot, look outward/along at the analyst + terminal.
                if (!kUpView) {
                    const std::string termPath = stem + "_term" + ext;
                    const float slotAng = (6.2831853f * 2) / 11.0f + 0.18f;   // analyst slot 2 / a terminal
                    const float ca = std::cos(slotAng), sa = std::sin(slotAng);
                    const float t2ex = cx + ca * (17.0f - 6.5f), t2ez = cz + sa * (17.0f - 6.5f);
                    const float t2ey = galY + 1.65f;
                    const float t2tx = cx + ca * (17.0f - 1.5f), t2tz = cz + sa * (17.0f - 1.5f);
                    const float t2ty = galY + 1.2f;
                    const float vx = t2tx - t2ex, vy = t2ty - t2ey, vz = t2tz - t2ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float gyaw = std::atan2(vz, vx), gpitch = std::atan2(vy, vlxz);
                    for (int i = 0; i < 4; ++i)
                        galTickAndRender(t2ex, t2ey, t2ez, gyaw, gpitch, /*tickHeavy*/ i < 2,
                                         (i == 3) ? termPath.c_str() : nullptr);
                    const bool wT = device->captureFrame(termPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (wT ? "wrote " : "FAILED ") + termPath);
                }
                // ---- Vantage B: DOWN through the dark glass OR UP at the ceiling band.
                float bex, bey, bez, byaw, bpitch;
                if (kUpView) {
                    // Stand on the CIVILIAN floor UNDER the gallery void + look near-straight
                    // UP at the dark-glass band ringing the void mouth ~19 m overhead (it
                    // should read DARK — the analysts behind it hidden). Aim at the void rim
                    // directly above (a hair off-vertical to avoid gimbal).
                    bex = cx; bez = cz; bey = floorY + 1.6f;
                    const float tx2 = cx + 2.5f, tz2 = cz + 1.0f, ty2 = galleryY - 0.3f;
                    const float vx = tx2 - bex, vy = ty2 - bey, vz = tz2 - bez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    byaw = std::atan2(vz, vx); bpitch = std::atan2(vy, vlxz);   // steep UP
                } else {
                    // Lean out OVER the void at the rail + look almost straight DOWN through
                    // the dark one-way glass onto the civilian floor/pad ~19 m below. The eye
                    // is nudged just INSIDE the void rim (over the opening) so the downward
                    // sightline clears the gallery floor + the GLB dome shells (which sit over
                    // the spire at Z~-100, away from this void center at Z~cz) and reaches the
                    // ground. Aim at a point on the floor a touch toward center so the pad
                    // reads (not dead-vertical, which would show only floor directly under).
                    // A raised 3/4 over the void rail (away from the GLB dome at Z~-100),
                    // tilted DOWN so the OPEN VOID + its dark one-way glass rim fill the
                    // frame and, through them, the civilian floor + the companion ARIA below
                    // read — proving the analysts watch the civilians through the glass. Eye
                    // lifted + pulled back on the -Z bearing; aim into the void at Aria.
                    bex = cx; bez = cz - 14.0f;          // back on the -Z gallery arc
                    bey = galY + 5.0f;                   // lifted for the down-into-void angle
                    const float tx2 = gx, tz2 = gz, ty2 = floorY + 1.0f;  // aim at Aria below, through the void
                    const float vx = tx2 - bex, vy = ty2 - bey, vz = tz2 - bez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    byaw = std::atan2(vz, vx); bpitch = std::atan2(vy, vlxz);   // down into the void
                }
                {
                    // The analysts/terminals are already posed+baked from vantage A; just
                    // settle a couple frames at the new camera (light ticks) + capture.
                    for (int i = 0; i < 4; ++i)
                        galTickAndRender(bex, bey, bez, byaw, bpitch, /*tickHeavy*/ false,
                                         (i == 3) ? downPath.c_str() : nullptr);
                    const bool w2 = device->captureFrame(downPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (w2 ? "wrote " : "FAILED ") + downPath);
                }
                sphys->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }

            // ===== CIVILIAN-FLOOR proof (--screenshot-showroom-civilians). DAY shot.
            // Captures TWO frames in one run:
            //   (a) <path>          — wide GROUND floor: the blue pad + lounge civilians.
            //   (b) <path>_mezz.png — the 2nd-floor mezzanine deck civilians.
            // PERF: each skinned tick() does a GPU readback (vkDeviceWaitIdle) — costly
            // under 4x SSAA. So we POSE all skinned figures (Aria + analysts + the 8
            // civilians) on only the FIRST 2 settle frames, then SKIP the heavy tick and
            // re-render the now-static scene for the remaining settle + capture frame.
            if (showroomCivShot) {
                const float dt = 1.0f / 60.0f;
                float celapsed = 10.0f;
                auto civTickAndRender = [&](float ex, float ey, float ez, float cyaw, float cpitch,
                                            bool poseFrame, const char* outPath) {
                    glfwPollEvents();
                    splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                    sphys->step(dt);
                    sscene.update(*sphys);
                    if (poseFrame) {
                        // Seat the idle pose ONCE (first frames of each vantage). Each
                        // tick is a GPU readback — kept to the first 2 frames only.
                        girl.tick(dt, false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                        for (auto& cv : civilians)
                            cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                    }
                    celapsed += dt;
                    device->setCamera(ex, ey, ez, cyaw, cpitch, 78.0f);
                    if (outPath) device->armCapture(outPath);
                    auto frame = device->beginFrame();
                    if (frame.valid) {
                        sscene.render(*device, frame);
                        showroom.draw(*device, frame);
                        girl.draw(*device, frame, sscene);
                        for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                        drawAdditiveGlass(frame);
                    }
                    device->endFrame(frame);
                };
                // Derive the "_mezz" output path from the base path.
                std::string base = showroomCivShotPath;
                std::string ext = ".png";
                std::string stem = base;
                if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".png") {
                    ext = stem.substr(stem.size() - 4); stem = stem.substr(0, stem.size() - 4);
                }
                const std::string mezzPath = stem + "_mezz" + ext;

                // ---- (a) GROUND floor: eye near the +X/+Z lounge, elevated a touch,
                // looking back across the blue pad (toward -X/-Z) so the pad civilians
                // (the chatting pair, the gazer, the trio) all read on the floor.
                {
                    const float ex = cx + 11.0f, ez = cz + 9.5f, ey = floorY + 1.7f;
                    const float tx = cx + 1.0f, ty = floorY + 0.9f, tz = cz - 1.0f;
                    const float vx = tx - ex, vy = ty - ey, vz = tz - ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float yw = std::atan2(vz, vx), pt = std::atan2(vy, vlxz);
                    for (int i = 0; i < 5; ++i)
                        civTickAndRender(ex, ey, ez, yw, pt, /*poseFrame*/ i < 2,
                                         (i == 4) ? showroomCivShotPath.c_str() : nullptr);
                    const bool w1 = device->captureFrame(showroomCivShotPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-civilians: ") +
                                (w1 ? "wrote " : "FAILED ") + showroomCivShotPath);
                }
                // ---- (b) 2nd-floor mezzanine: eye on the Room_01 deck (y=floor2Y),
                // standing close to the +X civilian pair so they read clearly, looking
                // along the deck at the pair + the down-gazing figure beyond.
                {
                    const float ex = 80.0f, ez = -109.0f, ey = floor2Y + 1.7f;
                    const float tx = 94.0f, ty = floor2Y + 0.9f, tz = -113.5f;
                    const float vx = tx - ex, vy = ty - ey, vz = tz - ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float yw = std::atan2(vz, vx), pt = std::atan2(vy, vlxz);
                    // Figures already posed from (a) — light settle frames, no heavy tick.
                    for (int i = 0; i < 4; ++i)
                        civTickAndRender(ex, ey, ez, yw, pt, /*poseFrame*/ false,
                                         (i == 3) ? mezzPath.c_str() : nullptr);
                    const bool w2 = device->captureFrame(mezzPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-civilians: ") +
                                (w2 ? "wrote " : "FAILED ") + mezzPath);
                }
                sphys->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }

            float eyeX, eyeZ, eyeY, yaw, pitch;
            if (strutsShot) {
                // EXTERIOR proof: stand well OUTSIDE the back-left corner, elevated, and
                // look back at the building center so the SYMMETRIC radial set of
                // thickened "/" strut legs all read (the four matched canted blades
                // leaning in toward the spire core).
                eyeX = cx - 60.0f; eyeZ = cz - 60.0f; eyeY = floorY + 40.0f;
                const float tx = cx, ty = floorY + 4.0f, tz = cz;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
            } else if (deckShot) {
                // PHASE 1 proof: stand ON the glass deck, near the -X rail, look out
                // ACROSS the deck toward +X/+Z and up a touch at the wheeling sky.
                eyeX = spireX - deckHalf + 1.5f; eyeZ = spireZ; eyeY = deckTopY + 1.6f;
                yaw = 0.35f /*toward +X, slightly +Z*/; pitch = 0.12f /*look up at the sky*/;
            } else if (floor2Shot) {
                // STAGE 1 proof: stand ON the 2nd floor (y=floor2Y) just -X of the climb
                // stair top, looking across the open 2nd-floor slab + DOWN the climb ramp
                // (toward +X/-Z) so the rising ramp, the solid 2nd-floor slab, and the
                // building + night sky all read — proving the player climbed up onto solid
                // 2nd-floor collision (NOT boxed in, NOT underground).
                eyeX = climbCX - 9.0f; eyeZ = stairLowZ + climbRun + 1.0f; eyeY = floor2Y + 1.7f;
                const float tx = climbCX + 3.0f, ty = floorY + 2.0f, tz = stairLowZ + 4.0f;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.55f, 1.15f, 1.0f);
            } else if (doorShot) {
                // STAGE 2 proof: stand ON the 2nd floor (interior side, +Z of the door) a
                // few metres back, looking -Z straight at the hidden wall door in the back
                // wall. By default the door is OPEN (slid aside, revealing the dark passage
                // behind); X3_SHOWROOM_DOORCLOSED=1 keeps it CLOSED so the panel reads
                // flush/concealed in the strut face.
                // Stand OUTSIDE the back-left strut at civilian level (floorY), a few
                // metres radially-OUT from the door, looking back IN at the hidden door
                // set into the strut's canted "/" face. OPEN by default reveals the dark
                // stair interior; X3_SHOWROOM_DOORCLOSED=1 keeps it CLOSED so the panel
                // reads flush/concealed in the white strut face.
                eyeX = doorFaceX + hsRox * 4.0f; eyeZ = doorFaceZ + hsRoz * 4.0f;
                eyeY = doorFloorY + doorHalfH;   // level with the door center, head-on
                const float tx = doorFaceX, ty = doorFloorY + doorHalfH, tz = doorFaceZ;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.58f, 1.2f, 1.0f);
            } else if (elevShot) {
                // ELEVATOR-LEVEL proof: stand on the atrium floor (y=atriumFloorY=14)
                // beside the parked glass cab, looking at the cab + up the shaft toward the
                // deck — proving the strut stair lands at the boarding level where the lift
                // now BOARDS.
                eyeY = atriumFloorY + 1.7f;
                eyeX = shaftX - 5.5f; eyeZ = shaftZ - 6.0f;
                const float tx = shaftX, ty = atriumFloorY + 2.5f, tz = shaftZ;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.52f, 1.1f, 0.98f);
            } else if (stairShot) {
                // STRUT-STAIR proof: stand just inside the strut door at the foot of the
                // hidden stair (at the door face, civilian floor) looking UP the canted
                // axis toward the strut head landing (atriumFloorY) — proving the stair
                // climbs UP INSIDE the strut to the elevator level.
                // Look through the OPEN door (slid aside for this shot) from just outside,
                // head-on + slightly up, framed on the doorway so the ASCENDING STEPS
                // inside the canted blade read climbing up behind the opening.
                eyeX = doorFaceX + hsRox * 3.5f; eyeZ = doorFaceZ + hsRoz * 3.5f;
                eyeY = doorFloorY + 1.3f;
                const float tx = sBL.bx + (sBL.tx - sBL.bx) * 0.22f;
                const float tz = sBL.bz + (sBL.tz - sBL.bz) * 0.22f;
                const float ty = strutBaseY + (strutTopY - strutBaseY) * 0.22f + 0.2f;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.55f, 1.15f, 1.0f);
            } else if (ragShot) {
                const float tx = gx, ty = floorY + 0.3f, tz = gz;   // aim at her floor spot
                // A HIGH 3/4 vantage 2.5 m back + 4.0 m up looks STEEPLY DOWN onto the
                // heap, clearing the floor slab's raised near edge (which occludes a flat
                // body from any near-level eye) so the sprawled ragdoll reads clearly
                // against the dark floor (bright proof tint makes it pop).
                float ax = sx - gx, az = sz - gz;                   // Aria -> center (toward the eye)
                const float al = std::sqrt(ax*ax + az*az);
                const float ux = (al > 1e-4f) ? ax/al : 0.0f, uz = (al > 1e-4f) ? az/al : -1.0f;
                const float back = 2.5f;
                eyeX = tx + ux*back; eyeZ = tz + uz*back; eyeY = floorY + 4.0f;
                // Brighten the dim moonlit showroom for the PROOF so the collapsed heap
                // reads clearly against the dark floor (headless capture only — does not
                // touch the interactive --world showroom look). NIGHT-only: in DAY the
                // snow-bounce ambient already reads bright, so leave the DAY state intact.
                if (!gShowroomDay) {
                    device->setAmbient(0.42f, 0.45f, 0.55f);
                    x3::rhi::IRenderDevice::SkyParams sb = sp; sb.sunIntensity = 0.9f; sb.exposure = 0.85f; device->setSkyParams(sb);
                }
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw   = std::atan2(vz, vx);            // look at Aria (engine yaw: atan2(dz,dx))
                pitch = std::atan2(vy, vlxz);          // gentle down-tilt onto her
            } else {
                eyeX = sx; eyeZ = sz; eyeY = sy + 1.6f; yaw = 1.5708f /*+Z*/; pitch = -0.05f;
            }
            // For the ragdoll proof, tint Aria a bright warm hue so the collapsed body
            // reads clearly against the dark moonlit floor (headless capture only).
            if (ragShot) girl.setTint(1.6f, 0.9f, 0.55f, 1.0f);
            // STRUT-door proof: OPEN the hidden strut-face door (slide the panel aside
            // along the strut's tangential axis + remove its collision) so the interior
            // stair reads. The door/stair shots default OPEN; X3_SHOWROOM_DOORCLOSED=1
            // keeps it concealed flush in the strut face for the door-closed proof.
            static const bool kDoorClosed = (std::getenv("X3_SHOWROOM_DOORCLOSED") != nullptr);
            const bool openDoorForProof = (stairShot || elevShot || (doorShot && !kDoorClosed));
            if (openDoorForProof) {
                hatchOpen = true; hatchSlide = 1.0f;
                sphys->removeBody(hatchLidBody);
                if (hatchIdx < sscene.size()) {
                    x3::game::Entity& he = sscene.get(hatchIdx);
                    // Mesh is authored in WORLD space -> transform translation is a pure
                    // tangential DELTA (0 = closed). Slide one panel-width aside.
                    he.transform[12] = hatchSlideX * hatchHalf * 2.0f;   // slid fully aside (tangential)
                    he.transform[14] = hatchSlideZ * hatchHalf * 2.0f;
                }
            }
            // Ragdoll shot needs ~45 physics steps to fall + settle; FP shot just settles.
            const int kSettle = ragShot ? 50 : (ddgiForce ? 120 : 24);   // --ddgi: probe-field convergence
            const float dt = 1.0f / 60.0f;
            float elapsed = 10.0f;   // non-zero so the starfield/clouds read animated
            const std::string outPath =
                strutsShot ? showroomStrutsShotPath :
                deckShot   ? showroomDeckShotPath   :
                floor2Shot ? showroomFloor2ShotPath :
                doorShot   ? showroomDoorShotPath   :
                elevShot   ? showroomElevShotPath   :
                stairShot  ? showroomStairShotPath  :
                ragShot    ? showroomRagdollShotPath : showroomFpShotPath;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                // STAGE 3 atrium proof: keep the cab parked at its LOWER stop (the atrium
                // boarding level) so the cab + atrium both read; just sync its transform.
                if (elevShot) {
                    elev.update(dt, sscene, *sphys);
                }
                // Collapse Aria after a few settle frames (so her CURRENT idle pose seeds
                // the ragdoll), THEN keep stepping so the bodies fall and her skin flops.
                // Control image (same framing, STANDING): set X3_RAGDOLL_NOCOLLAPSE=1 to
                // skip the collapse so the A/B comparison is at one identical camera.
                static const bool kNoCollapse = (std::getenv("X3_RAGDOLL_NOCOLLAPSE") != nullptr);
                if (ragShot && !kNoCollapse && i == 4) {
                    girl.ragdoll(sscene, *sphys);
                    x3::logInfo("--screenshot-showroom-ragdoll: Aria collapsed (ragdolled=" +
                                std::string(girl.ragdolled() ? "1" : "0") + ")");
                }
                sphys->step(dt);
                sscene.update(*sphys);
                girl.tick(dt, /*hubReached*/false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                // Civilians: pose-then-static (each tick is a GPU readback — costly under
                // 4x SSAA). Seat their idle pose on the first 2 frames only, then render
                // them static for the rest of the settle + capture.
                if (i < 2)
                    for (auto& cv : civilians)
                        cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                elapsed += dt;
                device->setCamera(eyeX, eyeY, eyeZ, yaw, pitch, ragShot ? 58.0f : 72.0f);
                if (!gShowroomDay) device->setSkyTime(elapsed);   // wheeling sky = NIGHT only
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    sscene.render(*device, frame);
                    showroom.draw(*device, frame);
                    girl.draw(*device, frame, sscene);
                    for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                    drawAdditiveGlass(frame);   // deck slab + rails + glass car (BLEND)
                    // Planets are a NIGHT feature — never drawn in DAY (the starfield in
                    // sky.frag auto-hides on the bright DAY sky).
                    if (!gShowroomDay)
                        drawNightSkyPlanets(device, frame, planetMesh, planets, elapsed,
                                            eyeX, eyeY, eyeZ, ringMesh);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            const char* tag =
                strutsShot ? "--screenshot-showroom-struts"   :
                deckShot   ? "--screenshot-showroom-deck"     :
                floor2Shot ? "--screenshot-showroom-floor2"   :
                doorShot   ? "--screenshot-showroom-door"     :
                elevShot   ? "--screenshot-showroom-elevator" :
                stairShot  ? "--screenshot-showroom-stair"    :
                ragShot    ? "--screenshot-showroom-ragdoll"  : "--screenshot-showroom-fp";
            if (wrote) x3::logInfo(std::string(tag) + ": wrote " + outPath);
            else       x3::logError(std::string(tag) + ": capture FAILED");
            sphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + E-to-talk. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceS = false, prevES = false, prevKS = false, prevFS = false, prevTS = false;
        float elapsed = 0.0f;
        // ---- HIDDEN-HATCH keypad host state. Mirrors the Level-1 §6.4 keypad gate
        // (main.cpp ~line 6921): a local KeypadEntry buffer + per-key rising-edge
        // trackers. When the player is near the still-closed hatch and presses E,
        // hatchCodeMode opens: digit keys 0-9 append, Backspace deletes, Enter submits
        // (== HATCH_CODE -> run the existing open logic; else flash DENIED + clear),
        // Esc cancels. Same KeypadEntry state machine exercised by --test-doorcode. ----
        bool                  hatchCodeMode = false;
        x3::game::KeypadEntry hatchKeypad;
        bool hkDigitPrev[10] = {};
        bool hkEnterPrev = false, hkBackPrev = false, hkEscPrev = false;
        float hatchDeniedTimer = 0.0f;   // >0 while the "DENIED" flash is shown
        x3::logInfo("--world showroom: walk the showroom — WASD, mouse look, Space jump, "
                    "LeftShift sprint, E talk to Aria / open the strut-door keypad, F ride elevator, "
                    "T toggle DAY/NIGHT, K ragdoll-collapse Aria, Esc to quit");
        x3::logInfo(std::string("--world showroom: starting in ") + (gShowroomDay ? "DAY" : "NIGHT") +
                    " (press T to toggle; instant switch)");
        x3::logInfo("--world showroom: walk to the BACK-LEFT STRUT LEG, find the HIDDEN DOOR in its canted "
                    "face at floor level — press E to open the KEYPAD, type the code + Enter to slide the panel "
                    "aside, then climb the STAIR UP INSIDE the strut to the ELEVATOR LEVEL, press F to ride the "
                    "glass elevator to the deck.");

        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            // Esc (edge-detected): while the hatch keypad is up, the FIRST Esc cancels
            // code-entry (mirrors the §6.4 gate, where Esc backs out of codeMode);
            // otherwise Esc quits the walkthrough as before.
            bool escNow = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escNow && !hkEscPrev) {
                if (hatchCodeMode) { hatchCodeMode = false; hatchKeypad.clear(); }
                else { hkEscPrev = escNow; break; }
            }
            hkEscPrev = escNow;

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;
            elapsed += dt;

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;

            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
            bool spaceNow = kd(GLFW_KEY_SPACE);

            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceS;
            in.lookDX = ddx; in.lookDY = ddy;
            prevSpaceS = spaceNow;

            splayer.update(in, dt, *sphys);
            sphys->step(dt);
            sscene.update(*sphys);

            // ANALYST GALLERY: idle the terminals (holo shimmer) + the analyst figures
            // (breathe in place; Captive + hubReached=false => no timer, no follow AI).
            for (auto& tm : galTerms) tm.update(dt);
            for (auto& an : galAnalysts)
                an.tick(dt, /*hubReached*/false, sscene, *sphys, an.pos());
            // CIVILIANS: idle in place (Captive + hubReached=false => no timer, no follow AI).
            for (auto& cv : civilians)
                cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());

            // ---- GLASS ELEVATOR: advance the cab + CARRY the rider (reuse elevator.cpp).
            // update() returns the cab's per-frame vertical delta; if the player is on
            // the cab top (playerRiding), add it to the player's feet so they ride up/down.
            {
                const float elevDy = elev.update(dt, sscene, *sphys);
                if (elevDy != 0.0f && elev.playerRiding(splayer.feet())) {
                    x3::phys::Vec3 f = splayer.feet();
                    f.y += elevDy;
                    splayer.setFeetPosition(*sphys, f);
                }
            }

            // T rising-edge: flip DAY<->NIGHT and re-apply the whole lighting STATE
            // (sky/sun/ambient/bloom + interior point lights scaled). Instant switch
            // (v1 — no cross-fade). The planet draw + setSkyTime below are gated to NIGHT.
            bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevTS) {
                gShowroomDay = !gShowroomDay;
                applyShowroomTimeOfDay(device, gShowroomDay, &plights);
                x3::logInfo(std::string("--world showroom: T pressed — time-of-day = ") +
                            (gShowroomDay ? "DAY (bright cool snow-bounce)" : "NIGHT (planets + dim moon + fixtures)"));
            }
            prevTS = tNow;

            // K rising-edge: collapse Aria into a physics ragdoll (debug/test hook).
            // The shared physics world is stepped above, so once ragdolled her tick()
            // drives the skin from the falling bodies. Idempotent — repeat K is a no-op.
            bool kNow = kd(GLFW_KEY_K);
            if (kNow && !prevKS) {
                girl.ragdoll(sscene, *sphys);
                x3::logInfo("--world showroom: K pressed — Aria ragdoll-collapse");
            }
            prevKS = kNow;

            // ---- HIDDEN 2ND-FLOOR WALL DOOR (KEYPAD-gated) + ELEVATOR call.
            //   * Near the concealed wall panel (still closed) -> E opens the KEYPAD;
            //     digits + Enter submit. The CORRECT code (2742) runs openHatch()
            //     (slides the panel aside + removes its collision so you walk through).
            //   * On/at the elevator cab -> F calls it to the next stop (atrium<->deck).
            // The keypad reuses the SAME KeypadEntry state machine + edge-handling shape
            // as the Level-1 §6.4 door-code gate (main.cpp ~line 6921).
            const x3::phys::Vec3 pf = splayer.feet();
            // Horizontal radial proximity to the strut-face door point (canted door, not
            // axis-aligned) + standing at the door's floor level, while it's still closed.
            const float ndx = pf.x - hatchX, ndz = pf.z - hatchZ;
            const bool nearHatch = (ndx*ndx + ndz*ndz <= (hatchHalf + 2.5f)*(hatchHalf + 2.5f)) &&
                                   (std::fabs(pf.y - doorFloorY) <= 2.6f) && !hatchOpen;
            const bool atElevator = elev.playerRiding(pf);
            // The existing open logic, factored so the keypad-submit path (and the
            // headless smoke) can run it. Removes the panel collision ONCE; idempotent
            // if already open. (Passage/turn/stair/atrium/elevator wiring untouched.)
            auto openHatch = [&](const char* via) {
                if (hatchOpen) return;
                hatchOpen = true;
                sphys->removeBody(hatchLidBody);      // open the doorway
                x3::logInfo(std::string("--world showroom: hidden wall door OPENED (") + via +
                            ") — walk in, turn, take the stair up to the elevator atrium");
            };

            // F: elevator call only (the door does not open on F).
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevFS) {
                if (atElevator) {
                    elev.callNext();                       // atrium <-> deck
                    x3::logInfo("--world showroom: elevator called (F) -> stop " +
                                std::to_string(elev.targetStop()));
                }
            }
            prevFS = fNow;

            // E near the still-closed hatch (and not already entering): open the keypad.
            // Mirrors §6.4 "near a locked coded door + E -> codeMode = true; keypad.clear()".
            bool eHatchNow = kd(GLFW_KEY_E);
            if (eHatchNow && !prevES && nearHatch && !hatchCodeMode) {
                hatchCodeMode = true; hatchKeypad.clear(); hatchDeniedTimer = 0.0f;
                x3::logInfo("--world showroom: hatch keypad — type the code, Enter to submit, Esc to cancel");
            }

            // Hatch keypad edge-handling while active: digits 0-9 append, Backspace
            // deletes, Enter submits. (Esc-cancel is handled in the Esc block above.)
            // Same per-key rising-edge shape as the §6.4 codeMode block.
            if (hatchCodeMode) {
                for (int dgt = 0; dgt < 10; ++dgt) {
                    bool dn = kd(GLFW_KEY_0 + dgt) || kd(GLFW_KEY_KP_0 + dgt);
                    if (dn && !hkDigitPrev[dgt]) hatchKeypad.pushDigit(dgt);
                    hkDigitPrev[dgt] = dn;
                }
                bool backNow = kd(GLFW_KEY_BACKSPACE);
                if (backNow && !hkBackPrev) hatchKeypad.backspace();
                hkBackPrev = backNow;
                bool enterNow = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_KP_ENTER);
                if (enterNow && !hkEnterPrev) {
                    if (hatchKeypad.value() == HATCH_CODE) {
                        x3::logInfo("--world showroom: hatch keypad ACCEPTED — opening");
                        hatchCodeMode = false; hatchKeypad.clear();
                        openHatch("keypad");
                    } else {
                        x3::logInfo("--world showroom: hatch keypad DENIED");
                        hatchDeniedTimer = 1.5f;          // flash "DENIED"
                        hatchKeypad.clear();              // clear the buffer, stay in entry
                    }
                }
                hkEnterPrev = enterNow;
            }
            if (hatchDeniedTimer > 0.0f) hatchDeniedTimer -= dt;
            // Animate the strut-face panel sliding aside (along the strut tangential
            // axis) once opened (cosmetic; collision is already removed). Slides ~1
            // panel-width over ~0.5 s.
            if (hatchOpen && hatchSlide < 1.0f) {
                hatchSlide = std::min(1.0f, hatchSlide + dt * 2.0f);
                if (hatchIdx < sscene.size()) {
                    x3::game::Entity& he = sscene.get(hatchIdx);
                    // Pure tangential DELTA (mesh authored in world; 0 = closed).
                    he.transform[12] = hatchSlide * hatchSlideX * hatchHalf * 2.0f;
                    he.transform[14] = hatchSlide * hatchSlideZ * hatchHalf * 2.0f;
                }
            }
            // Pulse the door panel's glow when the player is near + it's still closed
            // (a subtle "interactable" tell, per Tim's concealed-entrance vision).
            if (hatchIdx < sscene.size()) {
                x3::game::Entity& he = sscene.get(hatchIdx);
                he.emissive[3] = (nearHatch ? 0.8f : 0.0f);
            }

            float camX, camY, camZ, camYaw, camPitch;
            splayer.camera(camX, camY, camZ, camYaw, camPitch);
            const x3::phys::Vec3 eye{ camX, camY, camZ };
            girl.tick(dt, /*hubReached*/false, sscene, *sphys, eye);

            // E rising-edge: start/advance the Aria exchange; completing it rescues her.
            bool eNow = kd(GLFW_KEY_E);
            std::string talkWho; x3::phys::Vec3 talkPos{};
            const bool talkInRange = girl.captive() && [&]{
                const float dx = eye.x - girl.pos().x, dz = eye.z - girl.pos().z;
                return dx*dx + dz*dz <= x3::game::kTalkReach * x3::game::kTalkReach;
            }();
            if (talkInRange) { talkWho = girl.name(); talkPos = girl.pos(); }
            if (eNow && !prevES && (npcDialog.active() || talkInRange)) {
                const std::string barkName = talkWho.empty() ? npcDialog.partner() : talkWho;
                const bool rescued = npcDialog.interact(
                    talkInRange, talkWho, talkPos, [&]{ return girl.tryRescue(eye); });
                if (rescued) {
                    npcBarkText  = x3::game::companionBark(barkName);
                    npcBarkTimer = 4.0f;
                    x3::logInfo("--world showroom: Aria rescued — now a companion");
                }
            }
            prevES = eNow;
            // Keep the box anchored / cancel if she drifts out of range; age the bark.
            if (npcDialog.active()) {
                if (talkInRange) npcDialog.setAnchor(girl.pos());
                else             npcDialog.cancel();
            }
            if (npcBarkTimer > 0.0f) npcBarkTimer -= dt;

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 72.0f);
            if (!gShowroomDay) device->setSkyTime(elapsed);   // wheeling sky = NIGHT only
            auto frame = device->beginFrame();
            if (frame.valid) {
                sscene.render(*device, frame);
                showroom.draw(*device, frame);
                girl.draw(*device, frame, sscene);
                for (auto& an : galAnalysts) an.draw(*device, frame, sscene);   // gallery analysts
                for (auto& cv : civilians) cv.draw(*device, frame, sscene);     // civilian crowd
                drawAdditiveGlass(frame);   // glass deck + rails + riding car + gallery dark glass (BLEND)
                // Planets are a NIGHT feature — never drawn in DAY (the bright sky carries it).
                if (!gShowroomDay)
                    drawNightSkyPlanets(device, frame, planetMesh, planets, elapsed,
                                        camX, camY, camZ, ringMesh);

                // ---- HUD: "[E] Talk" prompt over Aria, or the dialog box while talking.
                uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);
                // Center proximity prompt: "[E] Keypad" at the still-closed hatch (now
                // code-gated), or "[F] Ride elevator" at the cab. Suppressed while the
                // hatch keypad is up (the entry prompt below owns the screen then).
                {
                    const char* fp = (nearHatch && !hatchCodeMode) ? "[E] Keypad"
                                   : atElevator                    ? "[F] Ride elevator" : nullptr;
                    if (fp) {
                        const float fw = device->textAdvance(x3::rhi::FontRole::Menu, fp, 22.0f);
                        const float fx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - fw * 0.5f;
                        const float fy = (hudH > 0) ? hudH * 0.72f : 480.0f;
                        const float fsh[4] = { 0.0f, 0.0f, 0.0f, 0.75f };
                        const float fcl[4] = { 0.62f, 0.92f, 1.0f, 1.0f };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, fp, fx + 1.5f, fy + 1.5f, 22.0f, fsh);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, fp, fx, fy, 22.0f, fcl);
                    }
                }
                // Hatch KEYPAD entry prompt (centered) while code-entry is active —
                // mirrors the §6.4 door-code HUD (KeypadEntry::prompt drives the digits).
                // A "DENIED" flash overrides the buffer line briefly on a wrong code.
                if (hatchCodeMode) {
                    const std::string kp = (hatchDeniedTimer > 0.0f)
                        ? std::string("DOOR LOCKED   DENIED")
                        : (std::string("DOOR LOCKED   ENTER CODE: ") + hatchKeypad.buf + "_");
                    const float kw = device->textAdvance(x3::rhi::FontRole::Menu, kp.c_str(), 26.0f);
                    const float kx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - kw * 0.5f;
                    const float ky = (hudH > 0) ? hudH * 0.5f - 30.0f : 330.0f;
                    const float ksh[4] = { 0.0f, 0.0f, 0.0f, 0.80f };
                    const bool denied = (hatchDeniedTimer > 0.0f);
                    const float kcl[4] = { 1.0f,
                                           denied ? 0.30f : 0.82f,
                                           denied ? 0.26f : 0.18f,
                                           1.0f };                        // red DENIED / amber entry
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, kp.c_str(), kx + 1.5f, ky + 1.5f, 26.0f, ksh);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, kp.c_str(), kx, ky, 26.0f, kcl);
                }
                if (npcDialog.active()) {
                    const auto& ln = npcDialog.currentLine();
                    const std::string speaker = ln.speaker.empty() ? npcDialog.partner() : ln.speaker;
                    const float ccx = (hudW > 0) ? hudW * 0.5f : 640.0f;
                    const float boxW = (hudW > 0) ? hudW * 0.66f : 840.0f;
                    const float boxH = 118.0f;
                    const float boxX = ccx - boxW * 0.5f;
                    const float boxY = (hudH > 0) ? hudH - 190.0f : 540.0f;
                    const float panel[4]  = { 0.05f, 0.07f, 0.12f, 0.82f };
                    const float border[4] = { 0.40f, 0.78f, 1.0f, 0.85f };
                    device->drawHudQuad(frame, boxX - 3.0f, boxY - 3.0f, boxW + 6.0f, boxH + 6.0f, border);
                    device->drawHudQuad(frame, boxX, boxY, boxW, boxH, panel);
                    const bool isYou = (speaker == "YOU");
                    const float herCol[4] = { 1.0f, 0.62f, 0.78f, 1.0f };
                    const float youCol[4] = { 0.66f, 0.92f, 1.0f, 1.0f };
                    const float nshadow[4] = { 0.0f, 0.0f, 0.0f, 0.75f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                         boxX + 25.5f, boxY + 19.5f, 26.0f, nshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                         boxX + 24.0f, boxY + 18.0f, 26.0f, isYou ? youCol : herCol);
                    const float lineCol[4] = { 0.96f, 0.97f, 1.0f, 1.0f };
                    const float lshadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, ln.text.c_str(),
                                         boxX + 25.5f, boxY + 59.5f, 30.0f, lshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, ln.text.c_str(),
                                         boxX + 24.0f, boxY + 58.0f, 30.0f, lineCol);
                    const char* hint = (npcDialog.lineIndex() + 1 >= npcDialog.lineCount())
                                       ? "[E] Free her" : "[E] Continue";
                    const float hw = device->textAdvance(x3::rhi::FontRole::Menu, hint, 18.0f);
                    const float hintCol[4] = { 0.75f, 0.85f, 0.95f, 0.85f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, hint,
                                         boxX + boxW - hw - 22.0f, boxY + boxH - 28.0f, 18.0f, hintCol);
                } else if (talkInRange) {
                    const x3::phys::Vec3 cp = girl.pos();
                    float ssx = 0.0f, ssy = 0.0f;
                    if (device->worldToScreen(cp.x, cp.y + 1.85f, cp.z, ssx, ssy)) {
                        const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                        const float col[4]    = { 1.0f, 0.72f, 0.84f, 1.0f };
                        device->drawHudText(frame, "[E] Talk", ssx - 40.0f + 1.5f, ssy + 1.5f, 18.0f, shadow);
                        device->drawHudText(frame, "[E] Talk", ssx - 40.0f, ssy, 18.0f, col);
                    }
                }
                if (npcBarkTimer > 0.0f && !npcBarkText.empty()) {
                    float a = npcBarkTimer; if (a > 1.0f) a = 1.0f;
                    const float bw = device->textAdvance(x3::rhi::FontRole::Menu, npcBarkText.c_str(), 22.0f);
                    const float bx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - bw * 0.5f;
                    const float by = (hudH > 0) ? hudH * 0.62f : 420.0f;
                    const float bshadow[4] = { 0.0f, 0.0f, 0.0f, 0.7f * a };
                    const float bcol[4]    = { 1.0f, 0.72f, 0.84f, a };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(), bx + 1.5f, by + 1.5f, 22.0f, bshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(), bx, by, 22.0f, bcol);
                }
            }
            device->endFrame(frame);
        }

        sphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
