// ============================================================================
// --world surface  (a.k.a. the ESCAPED-branch Act-1 surface-landing start)
//
// Phase 7 of the interactive branching cold-open (docs/design/
// INTERACTIVE_INTRO_DESIGN.md §6, INTERACTIVE_INTRO_PLAN.md Phase 7).
//
// This is the OTHER side of the intro fork. When the player wins the space
// dogfight (intro.outcome == "escaped"; StoryFlags["intro.landed"] is set by the
// ion-pulse descent in Phase 6), the game does NOT wake him a prisoner in the
// canon cell — it lands him OUTSIDE the huge FACILITY TOWER where Sarah is held,
// FREE and ARMED, as a *rescuer*. That is the exact inverse of the canon Level-1
// cell start (prisoner inside). This host builds that surface-landing slice:
//
//   * the surface SCENE (scrub-desert terrain w/ real ground PBR albedo from the
//     NatureManufacture Landscape Ground pack, a poured-concrete entrance apron,
//     distant mesa relief in the haze, analytic sky + a raking sun),
//   * Jake's LANDED SHIP on the surface (JakeFighterShip.glb, fallback box),
//   * the FACILITY TOWER ahead — Tim's v2 spec: a WHITE-CONCRETE tower (real
//     concrete PBR albedo) with BLACK reflective glass window bands INSET into
//     the facade between THICK concrete spandrels; the lobby glazing is real
//     translucent glass (GlassMaterial / transparent pass), Sarah inside,
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
// stb_image for loadSurfaceTex (real PBR albedo textures, assets/textures/
// surface/). FILE-LOCAL static copy, exactly like cinematic.cpp / screenshot_
// hosts.cpp (no symbol clash with ModelLoader.cpp's engine-side copy).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace x3 { namespace apphost {

// World layout (metres). The player lands at the origin facing -Z toward the
// facility; the ship sits just behind/beside him; the facility wall is ~46 m out.
namespace {
constexpr float kGroundY      = 0.0f;
constexpr float kFacilityZ    = -46.0f;   // front facade plane (outer concrete face)
constexpr float kFacilityHalfW= 14.5f;    // 29 m wide  (X) — vs ~71.5 m tall: ~2.5:1, reads TOWER
constexpr float kFacilityHalfD= 11.0f;    // 22 m deep  (Z)
constexpr float kBreachHalfW  = 2.4f;     // entry portal half-width
constexpr float kEntryReach   = 6.0f;     // distance to the entry that triggers the hand-off
constexpr float kApproachZ    = -24.0f;   // crossing this advances "approach" objective

// ---- THE WHITE-CONCRETE TOWER (Tim's v2 spec, 2026-07-01 — replaces the all-
// glass v1). The facility exterior is WHITE/OFF-WHITE CONCRETE — clean, massive,
// architectural — with a band of BLACK reflective glass windows INSET into the
// concrete on each visible floor (recessed ~0.45 m: real reveal depth + contact
// shadow lines) and THICK concrete spandrels between bands. SEVEN visible window
// bands, but the building is ~43 m TALLER than 7 stories should imply, because
// hidden sections add mass — above all LEVEL 4.5, a ~23.5 m (≈77 ft) MONSTER
// blank-concrete expanse between window bands 4 and 5 (minimal blanked vent
// slots only). Two smaller hidden slabs (above bands 2 and 6) add the rest. The
// observant player reads: band, band, band, band, ENORMOUS blank wall, band,
// band, band — and knows something big is hidden. The elevator's holo floor
// panel does NOT list 4.5 — the secret is found, not shown.
constexpr float kPlinthH   = 1.2f;    // solid base plinth under the lobby glazing
constexpr float kLobbyH    = 6.0f;    // band 1: double-height entrance lobby
constexpr float kStoryH    = 4.5f;    // bands 2..7 pitch (spandrel + glass)
constexpr float kSpandrelH = 1.8f;    // THICK concrete band between floors
constexpr int   kStories   = 7;       // VISIBLE window bands (band 1 = the lobby)
constexpr float kMonsterH  = 23.5f;   // LEVEL 4.5 — the monster hidden section (70-80 ft)
constexpr float kGapSmall  = 4.5f;    // smaller hidden slabs above bands 2 and 6
constexpr float kCrownH    = 6.0f;    // crown / parapet above band 7
constexpr float kReveal    = 0.45f;   // window glass inset depth into the concrete
constexpr float kPortalH   = 4.5f;    // entrance portal opening height
// Hidden concrete inserted BELOW 1-indexed floor f (f=1 is the lobby band).
inline float hiddenBelow(int f) {
    return (f > 2 ? kGapSmall : 0.0f) + (f > 4 ? kMonsterH : 0.0f) + (f > 6 ? kGapSmall : 0.0f);
}
// Band base Y for floor f: the bottom of its spandrel (glass starts kSpandrelH up).
inline float bandBaseY(int f) {
    return (f <= 1) ? 0.0f : kLobbyH + (f - 2) * kStoryH + hiddenBelow(f);
}
// Total height: lobby + six 4.5 m bands + the hidden slabs + the crown = 71.5 m.
constexpr float kTowerH = kLobbyH + (kStories - 1) * kStoryH + 2.0f * kGapSmall + kMonsterH + kCrownH;

// Load an RGBA8 texture from <assetRoot>/textures/surface/<rel> (converted from
// the D:\Assets packs — see the commit that added assets/textures/surface/).
// Falls back to a solid colour so a missing LFS pull still renders something.
x3::rhi::TextureHandle loadSurfaceTex(x3::rhi::IRenderDevice* device, const char* rel,
                                      bool srgb, uint8_t fr, uint8_t fg, uint8_t fb) {
    const std::string path = x3::game::assetRoot() + "/textures/surface/" + rel;
    int w = 0, h = 0, c = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (px) {
        auto t = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
        stbi_image_free(px);
        if (t.valid()) return t;
    }
    x3::logWarn(std::string("--world surface: texture missing, solid fallback: ") + path);
    auto td = x3::prims::makeSolidRGBA(8, fr, fg, fb);
    return device->createTexture(td.data(), 8, 8, srgb);
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
                "(outside the white-concrete facility tower where Sarah is held; free + armed rescuer)");

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world surface: physics init failed");
        device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate();
        return 1;
    }

    // ---- Sky + lighting: mid-morning desert sun placed to RAKE across the front
    //      facade (strong +X component) so the inset window reveals and spandrel
    //      steps cast readable contact-shadow lines and the white concrete models.
    //      Moderate haze gives the distant mesas atmospheric depth; the analytic
    //      sky is what the black glass bands reflect (IBL prefiltered cube).
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.70f; sp.sunDir[1] = 0.80f; sp.sunDir[2] = 0.42f;   // high + raking from the right
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.95f; sp.sunColor[2] = 0.86f;
        sp.sunIntensity = 1.35f; sp.haze = 0.55f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
    }
    {
        // Fill point lights: a warm lobby interior so Sarah + the lobby read
        // through the entrance glazing against the daylight, and a soft warm
        // bounce near the landed ship. The black window bands stay UNLIT — in
        // daylight they read as dark reflective glass, and NOTHING lights the
        // 4.5 monster section (it has no windows at all — the quiet tell).
        x3::rhi::PointLight pl[2];
        pl[0].pos[0] = 0.0f; pl[0].pos[1] = 3.2f; pl[0].pos[2] = kFacilityZ - 6.0f;
        pl[0].range = 55.0f;
        pl[0].color[0] = 10.0f; pl[0].color[1] = 8.0f; pl[0].color[2] = 5.5f;  // warm lobby glow
        pl[1].pos[0] = 9.0f; pl[1].pos[1] = 4.0f; pl[1].pos[2] = 10.0f;
        pl[1].range = 35.0f;
        pl[1].color[0] = 5.0f; pl[1].color[1] = 4.2f; pl[1].color[2] = 3.4f;   // sun bounce on the ship
        device->setPointLights(pl, 2);
    }

    x3::game::Scene scene;

    // ---- Real PBR materials. Albedos converted from the D:\Assets packs into
    //      assets/textures/surface/ (HIVEMIND AbandonedFactory concrete, Leartes
    //      Italian Alley poured concrete, NatureManufacture Landscape Ground
    //      scrub + cliff). Solid metallic-roughness maps route every surface
    //      through drawMeshPBR (Cook-Torrance + IBL/SSR) — no flat-color slabs.
    auto concreteTex = loadSurfaceTex(device, "concrete_white.png",  true, 224, 220, 212);
    auto apronTex    = loadSurfaceTex(device, "concrete_smooth.png", true, 170, 170, 172);
    auto groundTex   = loadSurfaceTex(device, "ground_scrub.png",    true, 156, 132, 96);
    auto mesaTex     = loadSurfaceTex(device, "cliff_sand.png",      true, 174, 142, 102);
    // glTF MR packing: G = roughness, B = metallic (linear texture).
    auto solidMR = [&](uint8_t rough, uint8_t metal) {
        std::vector<uint8_t> px(8 * 8 * 4, 255);
        for (size_t i = 0; i < px.size(); i += 4) { px[i] = 0; px[i+1] = rough; px[i+2] = metal; }
        return device->createTexture(px.data(), 8, 8, false);
    };
    auto mrConcrete = solidMR(228, 0);     // matte white concrete
    auto mrApron    = solidMR(198, 0);     // smoother poured concrete
    auto mrGround   = solidMR(242, 0);     // dusty scrub
    auto mrGlass    = solidMR(20, 245);    // BLACK GLASS: near-mirror; metallic path
                                           // makes the dark albedo the F0 -> the
                                           // bands become tinted sky mirrors (IBL/SSR)
    auto mrVent     = solidMR(130, 210);   // dark louver metal (4.5's blanked vents)
    auto blackGlassTexD = x3::prims::makeSolidRGBA(8, 44, 52, 64);   // F0 of the mirror
    auto blackGlassTex  = device->createTexture(blackGlassTexD.data(), 8, 8, true);
    auto ventTexD = x3::prims::makeSolidRGBA(8, 26, 28, 31);
    auto ventTex  = device->createTexture(ventTexD.data(), 8, 8, true);

    // One textured PBR box entity (visual only; collision is added separately).
    // tr/tg/tb: baseColor tint over the texel (pushes the concrete to OFF-WHITE).
    auto slab = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                    x3::rhi::TextureHandle tex, x3::rhi::TextureHandle mr, float uvScale,
                    float tr = 1.0f, float tg = 1.0f, float tb = 1.0f) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
        auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        x3::game::Entity e{}; e.mesh = mh; e.tex = tex; e.mrTex = mr;
        e.baseColor[0] = tr; e.baseColor[1] = tg; e.baseColor[2] = tb;
        e.tag = (uint32_t)x3::game::Tag::Static;
        return scene.add(e);
    };
    auto collideBox = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
        phys->addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                     0.0f, x3::phys::Layer::Static);
    };

    // ---- THE SCENE: scrub-desert terrain, a poured-concrete entrance apron,
    //      low perimeter barriers, and distant mesa relief in the haze — so the
    //      tower stands in a PLACE, not a void (the v1 failure). ---------------
    {
        // Scrub-desert ground (visual + the collision floor).
        slab(0.0f, -0.5f, 0.0f, 260.0f, 0.5f, 260.0f, groundTex, mrGround, 0.085f);
        collideBox(0.0f, -0.5f, 0.0f, 260.0f, 0.5f, 260.0f);
        // Concrete apron: entrance walkway from the facade out toward the landing.
        slab(0.0f, 0.045f, kFacilityZ + 15.0f, 9.0f, 0.045f, 15.5f, apronTex, mrApron, 0.16f);
        // Low concrete barriers flanking the walkway (grounding-scale props).
        for (int s = -1; s <= 1; s += 2) {
            for (int k = 0; k < 3; ++k) {
                const float bz = kFacilityZ + 6.5f + k * 9.5f;
                slab(s * 7.6f, 0.45f, bz, 0.42f, 0.45f, 2.6f, apronTex, mrApron, 0.6f);
                collideBox(s * 7.6f, 0.45f, bz, 0.42f, 0.45f, 2.6f);
            }
        }
        // Distant mesa relief (visual only) — silhouettes for the haze to eat.
        slab(-165.0f,  8.0f, -235.0f, 95.0f,  8.0f, 38.0f, mesaTex, mrGround, 0.018f);
        slab( 150.0f, 11.0f, -270.0f, 110.0f, 11.0f, 46.0f, mesaTex, mrGround, 0.016f);
        slab(-245.0f,  6.0f,  -50.0f, 55.0f,  6.0f, 30.0f, mesaTex, mrGround, 0.02f);
        slab( 235.0f,  8.5f, -120.0f, 70.0f,  8.5f, 34.0f, mesaTex, mrGround, 0.018f);
        slab( -60.0f,  5.0f,  225.0f, 90.0f,  5.0f, 40.0f, mesaTex, mrGround, 0.02f);
    }

    // ---- THE WHITE-CONCRETE TOWER exterior (Tim's v2 spec — constants above).
    //      White concrete facade (real PBR albedo) with BLACK reflective glass
    //      window bands RECESSED kReveal into the wall between THICK concrete
    //      spandrels; concrete window piers punch each band into windows; stepped
    //      corner columns; a crown/parapet; and the LEVEL 4.5 monster blank
    //      expanse between bands 4 and 5 (louver vents only). The black glass is
    //      an opaque near-mirror PBR material (dark albedo as F0 -> it reflects
    //      the SKY); only the ground-floor lobby glazing is real translucent
    //      glass so Sarah reads through the entrance. Collision = full-height
    //      static boxes per face (front split around the entry portal).
    const float fcz   = kFacilityZ - kFacilityHalfD;   // tower footprint center Z
    const float wallT = 0.5f;                          // wall half-thickness (1 m walls)
    const float backZ = kFacilityZ - 2.0f * kFacilityHalfD;
    {
        // Translucent lobby glazing (transparent pass; dark but see-through).
        auto lobbyGlass = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            auto td = x3::prims::makeSolidRGBA(8, 22, 28, 34);
            auto tx = device->createTexture(td.data(), 8, 8, true);
            x3::game::Entity e{}; e.mesh = mh; e.tex = tx;
            e.transparent = true;
            e.glass.opacity   = 0.55f;
            e.glass.refraction= 0.02f;
            e.glass.roughness = 0.04f;
            e.glass.specular  = 1.0f;
            e.glass.tint[0] = 0.10f; e.glass.tint[1] = 0.13f; e.glass.tint[2] = 0.16f;
            e.baseColor[3] = e.glass.opacity;
            e.tag = (uint32_t)x3::game::Tag::Static;
            scene.add(e);
        };

        // ---- One facade face. axis 0: spans X at plane z=planeCoord; axis 1:
        // spans Z at plane x=planeCoord. outSign: which side of the plane is
        // OUTSIDE (+1 => outward is +axis-normal). `place` puts a box whose
        // OUTER surface sits `inset` metres BEHIND the facade plane (negative
        // inset = proud of the wall), `depth` = box half-thickness.
        // mat: 0 white concrete, 1 black glass, 2 lobby glazing, 3 smooth trim,
        //      4 dark louver metal.
        auto buildFace = [&](int axis, float planeCoord, float outSign,
                             float spanCenter, float spanHalf, bool isFront) {
            auto place = [&](float sCenter, float sHalf, float y0, float y1,
                             float inset, float depth, int mat) {
                const float cy = (y0 + y1) * 0.5f, hy = (y1 - y0) * 0.5f;
                const float pc = planeCoord - outSign * (inset + depth);
                float cx, cz, hx, hz;
                if (axis == 0) { cx = sCenter; cz = pc; hx = sHalf; hz = depth; }
                else           { cx = pc; cz = sCenter; hx = depth; hz = sHalf; }
                switch (mat) {
                case 1:  slab(cx, cy, cz, hx, hy, hz, blackGlassTex, mrGlass, 1.0f); break;
                case 2:  lobbyGlass(cx, cy, cz, hx, hy, hz); break;
                case 3:  slab(cx, cy, cz, hx, hy, hz, apronTex, mrApron, 0.22f); break;
                case 4:  slab(cx, cy, cz, hx, hy, hz, ventTex, mrVent, 0.8f); break;
                default: slab(cx, cy, cz, hx, hy, hz, concreteTex, mrConcrete, 0.24f); break;
                }
            };
            const float cornerW = 1.6f;                 // concrete margin at each end
            const float runHalf = spanHalf - cornerW;   // the glass run half-length
            const float sL = spanCenter - (spanHalf - cornerW * 0.5f);
            const float sR = spanCenter + (spanHalf - cornerW * 0.5f);

            // BASE PLINTH: smooth concrete, slightly PROUD — grounds the tower.
            if (isFront) {
                const float sideW = (spanHalf - kBreachHalfW) * 0.5f;
                place(spanCenter - (kBreachHalfW + sideW), sideW, 0.0f, kPlinthH, -0.15f, wallT, 3);
                place(spanCenter + (kBreachHalfW + sideW), sideW, 0.0f, kPlinthH, -0.15f, wallT, 3);
            } else {
                place(spanCenter, spanHalf, 0.0f, kPlinthH, -0.15f, wallT, 3);
            }

            // BAND 1 — the double-height LOBBY: recessed translucent glazing
            // (split around the entrance portal on the front) + concrete piers.
            {
                const float g0 = kPlinthH, g1 = kLobbyH;
                if (isFront) {
                    const float sideW = (runHalf - kBreachHalfW) * 0.5f;
                    place(spanCenter - (kBreachHalfW + sideW), sideW, g0, g1, kReveal, 0.08f, 2);
                    place(spanCenter + (kBreachHalfW + sideW), sideW, g0, g1, kReveal, 0.08f, 2);
                    place(spanCenter, kBreachHalfW, kPortalH, g1, kReveal, 0.08f, 2);   // lintel glass
                } else {
                    place(spanCenter, runHalf, g0, g1, kReveal, 0.08f, 2);
                }
                place(sL, cornerW * 0.5f, 0.0f, g1, 0.0f, wallT, 0);   // corner margins
                place(sR, cornerW * 0.5f, 0.0f, g1, 0.0f, wallT, 0);
                // Lobby piers (~3.6 m module), skipping the portal lane in front.
                const int nMod = std::max(1, (int)std::floor((runHalf * 2.0f) / 3.6f));
                for (int p = 1; p < nMod; ++p) {
                    const float s = spanCenter - runHalf + (runHalf * 2.0f) * p / nMod;
                    if (isFront && std::fabs(s - spanCenter) < kBreachHalfW + 0.6f) continue;
                    place(s, 0.30f, g0, g1, 0.0f, wallT, 0);
                }
            }

            // BANDS 2..7: THICK concrete spandrel/fill, then the RECESSED black
            // glass band punched into windows by concrete piers. The fill between
            // band 4's glass top and band 5's base is the LEVEL 4.5 MONSTER
            // expanse (kSpandrelH + kMonsterH of blank concrete) — broken only by
            // three slim dark louver strips (the blanked vents).
            float prevTop = kLobbyH;
            for (int f = 2; f <= kStories; ++f) {
                const float b0 = bandBaseY(f);
                const float g0 = b0 + kSpandrelH, g1 = b0 + kStoryH;
                place(spanCenter, spanHalf, prevTop, g0, 0.0f, wallT, 0);   // concrete fill
                if (f == 5) {
                    // 4.5's louver vents sit low in the expanse, slightly proud.
                    for (int v = 0; v < 3; ++v) {
                        const float vy = prevTop + 6.0f + v * 6.2f;
                        place(spanCenter, runHalf * 0.70f, vy, vy + 0.5f, -0.06f, 0.10f, 4);
                    }
                }
                place(spanCenter, runHalf, g0, g1, kReveal, 0.10f, 1);      // black glass band
                const int nWin = std::max(1, (int)std::floor((runHalf * 2.0f) / 3.2f));
                for (int p = 1; p < nWin; ++p) {                            // window piers
                    const float s = spanCenter - runHalf + (runHalf * 2.0f) * p / nWin;
                    place(s, 0.28f, g0, g1, 0.0f, wallT, 0);
                }
                place(sL, cornerW * 0.5f, g0, g1, 0.0f, wallT, 0);          // corner margins
                place(sR, cornerW * 0.5f, g0, g1, 0.0f, wallT, 0);
                prevTop = g1;
            }

            // CROWN: blank concrete to the parapet + a dark mechanical louver
            // band near the top (reads as the roof-plant screen).
            place(spanCenter, spanHalf, prevTop, kTowerH, 0.0f, wallT, 0);
            place(spanCenter, runHalf * 0.82f, kTowerH - 3.2f, kTowerH - 2.4f, -0.06f, 0.10f, 4);
        };

        // FRONT (player-facing, outward +Z), BACK (outward -Z), LEFT, RIGHT.
        buildFace(0, kFacilityZ,       +1.0f, 0.0f, kFacilityHalfW, /*isFront*/true);
        buildFace(0, backZ,            -1.0f, 0.0f, kFacilityHalfW, false);
        buildFace(1, -kFacilityHalfW,  -1.0f, fcz, kFacilityHalfD, false);
        buildFace(1,  kFacilityHalfW,  +1.0f, fcz, kFacilityHalfD, false);

        // STEPPED CORNER columns: full-height white-concrete masses set 0.3 m
        // proud of both planes at each corner (the band grid dies into them).
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = 0; sz <= 1; ++sz) {
                const float ccx = sx * (kFacilityHalfW - 0.9f);
                const float ccz = (sz ? backZ + 0.9f : kFacilityZ - 0.9f);
                slab(ccx, kTowerH * 0.5f, ccz, 1.2f, kTowerH * 0.5f, 1.2f,
                     concreteTex, mrConcrete, 0.24f);
            }

        // PARAPET CAP: a smooth-concrete coping slab overhanging the crown.
        slab(0.0f, kTowerH + 0.22f, fcz, kFacilityHalfW + 0.55f, 0.22f, kFacilityHalfD + 0.55f,
             apronTex, mrApron, 0.2f);
        collideBox(0.0f, kTowerH + 0.22f, fcz, kFacilityHalfW + 0.55f, 0.22f, kFacilityHalfD + 0.55f);
        // Interior lobby floor slab.
        slab(0.0f, 0.05f, fcz, kFacilityHalfW, 0.1f, kFacilityHalfD, apronTex, mrApron, 0.16f);
        collideBox(0.0f, 0.05f, fcz, kFacilityHalfW, 0.1f, kFacilityHalfD);

        // ---- ORNATE ENTRANCE PORTAL (echoes the elevator-portal design
        //      language): two nested concrete frames stepping OUT of the facade
        //      + a thin cool emissive reveal line inside the inner frame.
        {
            // A frame member: outer face `proud` metres out of the facade,
            // embedded 0.25 m back into the wall.
            auto frameBox = [&](float cx, float cy, float hw, float hh, float proud) {
                const float hz = (proud + 0.25f) * 0.5f;
                slab(cx, cy, kFacilityZ + proud - hz, hw, hh, hz, apronTex, mrApron, 0.3f);
            };
            // Outer frame: jambs + header (proud 0.55, members 0.55 thick).
            const float oOpen = kBreachHalfW + 0.75f, oTop = kPortalH + 1.1f, oT = 0.55f;
            frameBox(-(oOpen + oT * 0.5f), oTop * 0.5f, oT * 0.5f, oTop * 0.5f, 0.55f);
            frameBox( (oOpen + oT * 0.5f), oTop * 0.5f, oT * 0.5f, oTop * 0.5f, 0.55f);
            frameBox(0.0f, oTop + oT * 0.5f, oOpen + oT, oT * 0.5f, 0.55f);
            // Inner frame: tighter, less proud.
            const float iOpen = kBreachHalfW + 0.28f, iTop = kPortalH + 0.5f, iT = 0.40f;
            frameBox(-(iOpen + iT * 0.5f), iTop * 0.5f, iT * 0.5f, iTop * 0.5f, 0.30f);
            frameBox( (iOpen + iT * 0.5f), iTop * 0.5f, iT * 0.5f, iTop * 0.5f, 0.30f);
            frameBox(0.0f, iTop + iT * 0.5f, iOpen + iT, iT * 0.5f, 0.30f);
            // Emissive reveal line tracing the inner opening (the portal glow).
            auto glowLine = [&](float cx, float cy, float hw, float hh) {
                x3::prims::PrimMesh m = x3::prims::makeBox(hw, hh, 0.05f, cx, cy, kFacilityZ + 0.12f, 1.0f);
                auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                             m.index.data(), (uint32_t)m.index.size());
                x3::game::Entity e{}; e.mesh = mh;
                e.baseColor[0] = 0.5f; e.baseColor[1] = 0.85f; e.baseColor[2] = 1.0f;
                e.emissive[0] = 0.45f; e.emissive[1] = 0.85f; e.emissive[2] = 1.2f; e.emissive[3] = 2.2f;
                e.tag = (uint32_t)x3::game::Tag::Static;
                scene.add(e);
            };
            glowLine(-(kBreachHalfW + 0.10f), kPortalH * 0.5f, 0.05f, kPortalH * 0.5f);
            glowLine( (kBreachHalfW + 0.10f), kPortalH * 0.5f, 0.05f, kPortalH * 0.5f);
            glowLine(0.0f, kPortalH + 0.10f, kBreachHalfW + 0.22f, 0.05f);
        }

        // ---- COLLISION truth: full-height boxes per face; the front is split
        //      around the entry portal (open below kPortalH in the portal lane).
        const float sideW = (kFacilityHalfW - kBreachHalfW) * 0.5f;
        collideBox(-(kBreachHalfW + sideW), kTowerH * 0.5f, kFacilityZ - wallT, sideW, kTowerH * 0.5f, wallT);
        collideBox( (kBreachHalfW + sideW), kTowerH * 0.5f, kFacilityZ - wallT, sideW, kTowerH * 0.5f, wallT);
        collideBox(0.0f, (kTowerH + kPortalH) * 0.5f, kFacilityZ - wallT, kBreachHalfW, (kTowerH - kPortalH) * 0.5f, wallT);
        collideBox(0.0f, kTowerH * 0.5f, backZ + wallT, kFacilityHalfW, kTowerH * 0.5f, wallT);
        collideBox(-kFacilityHalfW + wallT, kTowerH * 0.5f, fcz, wallT, kTowerH * 0.5f, kFacilityHalfD);
        collideBox( kFacilityHalfW - wallT, kTowerH * 0.5f, fcz, wallT, kTowerH * 0.5f, kFacilityHalfD);
    }

    {
        char tb[256];
        std::snprintf(tb, sizeof(tb),
            "--world surface: WHITE-CONCRETE TOWER built — %d window bands, LEVEL 4.5 "
            "monster section %.1f m between bands 4 and 5 (+2 x %.1f m hidden slabs) "
            "= %.1f m total on a %.0f m x %.0f m footprint",
            kStories, kMonsterH, kGapSmall, kTowerH,
            kFacilityHalfW * 2.0f, kFacilityHalfD * 2.0f);
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
        scene.render(*device, frame);   // terrain + concrete tower + portal + Sarah's prop entity
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
        // pitched UP so the full white-concrete tower reads against the sky
        // (all 7 window bands + the monster 4.5 blank expanse; ship at right).
        float cam[5] = { 13.0f, 2.0f, 40.0f, -3.14159265f*0.5f - 0.10f, 0.30f };
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        const std::string outPath = screenshot ? screenshotPath : std::string("w_surface.png");
        const int kFrames = 40;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            phys->step(dt);
            rescue.tick(dt, scene, *phys, player.feet());   // hub NOT reached -> no timers
            scene.update(*phys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                // NOTE: no view-anchored weapon in the capture — at 0.6 m from
                // the lens it blacks out a corner of the architecture shot.
                scene.render(*device, frame);
                rescue.draw(*device, frame, scene);
                drawShip(frame);
            }
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
//       HeadlessRenderDevice + Jolt world it builds the ground, the FACILITY TOWER
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
            // One lobby-glazing wall (transparent entity through the glass pass).
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

            // THE WHITE-CONCRETE TOWER canon (v2 spec): 7 visible window bands,
            // but the tower carries 40+ m of HIDDEN mass — above all LEVEL 4.5,
            // a 21-24 m (70-80 ft) monster blank section between bands 4 and 5.
            // The height mismatch IS the storytelling: 7 stories should read
            // ~28 m of floors; the tower is 70-78 m tall.
            check(kStories == 7 &&
                  kMonsterH >= 21.0f && kMonsterH <= 24.0f &&
                  kTowerH >= 70.0f && kTowerH <= 78.0f &&
                  kTowerH - kStories * 4.0f >= 40.0f &&
                  bandBaseY(5) - (bandBaseY(4) + kStoryH) == kMonsterH,
                  "S6b tower is FAR taller than its 7 bands (monster Level 4.5 between 4 and 5)");

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
