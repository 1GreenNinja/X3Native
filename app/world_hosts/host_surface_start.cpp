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
// MASSING = lab.jpg (Tim's 2026-07-01 synthesis: "style of Lab2, as massive as
// lab.jpg"): a WIDE monolithic slab — 60 m x 34 m footprint — under the
// unchanged 71.5 m height canon. Facade LANGUAGE stays Lab2 (white concrete
// dominant, dark banded windows inset with thin mullions).
constexpr float kFacilityHalfW= 30.0f;    // 60 m wide  (X) — the lab.jpg monolith presence
constexpr float kFacilityHalfD= 17.0f;    // 34 m deep  (Z)
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

    // ---- NIGHT sky + moonlight (matches the reference Lab2.jpg: a bright,
    //      moonlit WHITE-CONCRETE tower rim-lit against a DARK STARRY sky, with
    //      dark reflective glass bands and glowing crystals pooling colour on the
    //      dark rock). The mesh directional sun is a FIXED-magnitude near-white
    //      (mesh.frag kSunColor) — so we keep the SKY's own sun DISK dark
    //      (sunIntensity 0): nothing burns a moon into frame, only the building
    //      is lit, exactly like the reference. Dark zenith/horizon + near-zero
    //      haze trip the sky shader's night gate, so the starfield blooms in.
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        // Moonlight rakes from upper-LEFT-FRONT so the +Z facade + the -X flank
        // catch a cool kicker and the inset reveals/spandrels cast contact lines.
        sp.sunDir[0] = -0.40f; sp.sunDir[1] = 0.60f; sp.sunDir[2] = 0.70f;
        sp.sunColor[0] = 0.72f; sp.sunColor[1] = 0.82f; sp.sunColor[2] = 1.0f;  // cool moon (sky disk only)
        sp.sunIntensity = 0.0f;    // no visible moon disk/glow — clean, dark sky
        sp.haze = 0.03f;           // near-zero -> deep night; the starfield shows
        sp.exposure = 1.0f;
        sp.zenith[0]  = 0.006f; sp.zenith[1]  = 0.011f; sp.zenith[2]  = 0.026f;  // near-black blue overhead
        sp.horizon[0] = 0.020f; sp.horizon[1] = 0.033f; sp.horizon[2] = 0.062f;  // faint cool horizon glow
        sp.nebula = 0.85f;   // ALIEN NIGHT (lab.jpg): teal + rose nebula PATCHES behind the stars (dark sky dominant)
        device->setSkyParams(sp);
    }
    // A cool ambient FILL so the SHADOWED faces of the white concrete read as
    // deep silver rather than black (the night-sky IBL is nearly black on its own).
    device->setAmbient(0.090f, 0.110f, 0.155f);
    // Pin exposure (auto-exposure OFF): a mostly-dark night frame would otherwise
    // let eye-adaptation crank the gain and milk out the black sky. A fixed,
    // slightly hot exposure keeps the moonlit concrete bright and the sky inky.
    {
        x3::rhi::IRenderDevice::PostFXParams fx{};
        fx.autoExposure = false;
        device->setPostFX(fx);
        device->setExposure(1.55f);
        device->setBloom(0.85f);   // gentle bloom on lit windows + crystals (not a white glare)
    }
    // NOTE: all the scene POINT LIGHTS (warm lit windows, the crystal glow pool,
    // the ship bounce) are set together AFTER the crystals are built, below.

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
        // Scrub-desert ground (visual + the collision floor) — tinted DOWN so the
        // night terrain reads as dark alien rock-plain (lab.jpg), not moonlit lawn.
        slab(0.0f, -0.5f, 0.0f, 260.0f, 0.5f, 260.0f, groundTex, mrGround, 0.085f,
             0.42f, 0.42f, 0.52f);
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
            e.glass.opacity   = 0.62f;
            e.glass.refraction= 0.02f;
            e.glass.roughness = 0.04f;
            e.glass.specular  = 1.0f;
            // DARK entry glazing (the recessed ground level reads as shadow).
            e.glass.tint[0] = 0.045f; e.glass.tint[1] = 0.06f; e.glass.tint[2] = 0.075f;
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

            // BAND 1 — the double-height LOBBY: a RECESSED, DARKER entry level
            // (lab.jpg's ground floor sits back in shadow under the mass above).
            // Deep-set translucent glazing (split around the entrance portal on
            // the front) + slim piers set at half the recess.
            {
                const float g0 = kPlinthH, g1 = kLobbyH;
                const float entryInset = kReveal * 2.1f;   // ~0.95 m — a real shadow gap
                if (isFront) {
                    const float sideW = (runHalf - kBreachHalfW) * 0.5f;
                    place(spanCenter - (kBreachHalfW + sideW), sideW, g0, g1, entryInset, 0.08f, 2);
                    place(spanCenter + (kBreachHalfW + sideW), sideW, g0, g1, entryInset, 0.08f, 2);
                    place(spanCenter, kBreachHalfW, kPortalH, g1, entryInset, 0.08f, 2);   // lintel glass
                } else {
                    place(spanCenter, runHalf, g0, g1, entryInset, 0.08f, 2);
                }
                place(sL, cornerW * 0.5f, 0.0f, g1, 0.0f, wallT, 0);   // corner margins
                place(sR, cornerW * 0.5f, 0.0f, g1, 0.0f, wallT, 0);
                // Slim lobby piers (~3.6 m module), skipping the portal lane in front.
                const int nMod = std::max(1, (int)std::floor((runHalf * 2.0f) / 3.6f));
                for (int p = 1; p < nMod; ++p) {
                    const float s = spanCenter - runHalf + (runHalf * 2.0f) * p / nMod;
                    if (isFront && std::fabs(s - spanCenter) < kBreachHalfW + 0.6f) continue;
                    place(s, 0.22f, g0, g1, entryInset * 0.5f, 0.30f, 0);
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
                // CONTINUOUS band strip (Lab2 style): the glass runs the full
                // facade width; only THIN pale mullions divide it into wide
                // panels (they sit at HALF the reveal so they read as window
                // framing INSIDE the band, not concrete punch-hole piers).
                const int nWin = std::max(1, (int)std::floor((runHalf * 2.0f) / 2.6f));
                for (int p = 1; p < nWin; ++p) {                            // thin mullions
                    const float s = spanCenter - runHalf + (runHalf * 2.0f) * p / nWin;
                    place(s, 0.055f, g0, g1, kReveal * 0.5f, 0.06f, 3);
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

    // ---- NIGHT DRESSING (reference Lab2.jpg): a few WARM LIT WINDOW cells so the
    //      tower reads INHABITED (most bands stay dark glass), a rooftop antenna
    //      mast cluster, and — the hero foreground — a DARK ROCK outcrop with a
    //      cluster of GLOWING pink/cyan CRYSTALS pooling colour on the rock + the
    //      tower base. -----------------------------------------------------------
    auto rockTex = loadSurfaceTex(device, "rock_dark.png", true, 34, 34, 40);
    std::vector<uint32_t> neonIds;   // the hidden NEON-VARIANT strip entities
    {
        // WARM LIT WINDOWS: emissive panels flush in a handful of front-face
        // window cells (a few lit floors sell habitation; the rest read as dark
        // reflective glass). Placed just inside the facade plane, in the window
        // openings between the concrete piers.
        auto litWin = [&](float cx, float cy, float hx, float hy) {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, 0.05f, cx, cy, kFacilityZ - 0.08f, 1.0f);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            x3::game::Entity e{}; e.mesh = mh;
            e.baseColor[0] = 0.32f; e.baseColor[1] = 0.22f; e.baseColor[2] = 0.12f;
            e.emissive[0] = 1.0f; e.emissive[1] = 0.66f; e.emissive[2] = 0.34f; e.emissive[3] = 2.6f;
            e.tag = (uint32_t)x3::game::Tag::Static;
            scene.add(e);
        };
        auto glassGlow = [&](int f) { return bandBaseY(f) + kSpandrelH + (kStoryH - kSpandrelH) * 0.5f; };
        litWin(  9.8f, glassGlow(2), 1.05f, 1.05f);   // band 2, right
        litWin( 17.2f, glassGlow(2), 1.05f, 1.05f);
        litWin(-13.5f, glassGlow(3), 1.05f, 1.05f);   // band 3, left
        litWin(-22.0f, glassGlow(3), 1.05f, 1.05f);
        litWin( -6.2f, glassGlow(5), 1.05f, 1.05f);   // band 5 (above the 4.5 expanse)
        litWin( 12.4f, glassGlow(5), 1.05f, 1.05f);
        litWin( 23.0f, glassGlow(6), 1.05f, 1.05f);   // band 6, far right

        // ROOFTOP ANTENNA MAST cluster (back-right of the crown, Lab2's dish/mast
        // silhouette) + roof plant boxes.
        for (int k = 0; k < 6; ++k) {
            const float mx = 9.0f + k * 2.6f;
            const float mh = 3.0f + ((k % 3) * 2.1f);
            slab(mx, kTowerH + 0.44f + mh * 0.5f, backZ + 3.0f, 0.10f, mh * 0.5f, 0.10f,
                 apronTex, mrApron, 1.0f);
        }
        slab(12.0f, kTowerH + 1.2f, backZ + 5.5f, 2.2f, 0.55f, 1.2f, apronTex, mrApron, 0.6f); // roof plant
        slab(-14.0f, kTowerH + 1.0f, backZ + 6.0f, 1.6f, 0.40f, 1.0f, apronTex, mrApron, 0.6f);

        // PARAPET RAILING (Lab2 silhouette): a thin top rail + sparse posts
        // tracing the roof edge, just inside the coping, on all four sides.
        {
            const float railY = kTowerH + 0.44f + 1.0f;   // rail 1 m above the cap
            const float rx = kFacilityHalfW + 0.30f, rz0 = kFacilityZ - 0.30f, rz1 = backZ + 0.30f;
            const float rcz = (rz0 + rz1) * 0.5f, rhz = (rz0 - rz1) * 0.5f;
            slab(0.0f, railY, rz0, rx, 0.030f, 0.030f, apronTex, mrApron, 1.0f);   // front rail
            slab(0.0f, railY, rz1, rx, 0.030f, 0.030f, apronTex, mrApron, 1.0f);   // back rail
            slab(-rx, railY, rcz, 0.030f, 0.030f, rhz, apronTex, mrApron, 1.0f);   // side rails
            slab( rx, railY, rcz, 0.030f, 0.030f, rhz, apronTex, mrApron, 1.0f);
            for (int p = -5; p <= 5; ++p) {                                        // posts (front + back)
                slab(p * (rx / 5.5f), railY - 0.5f, rz0, 0.028f, 0.5f, 0.028f, apronTex, mrApron, 1.0f);
                slab(p * (rx / 5.5f), railY - 0.5f, rz1, 0.028f, 0.5f, 0.028f, apronTex, mrApron, 1.0f);
            }
        }

        // ---- CYAN NEON PARAPET STRIP (lab.jpg's signature) — built HIDDEN.
        //      Tim named Lab2 as the facade style, so the PRIMARY hero renders
        //      without it; the headless path captures a SECOND "<name>_neon.png"
        //      variant with these visible so Tim picks. Four emissive strips
        //      trace the coping edge and MEET at the corners (a continuous ring).
        {
            auto neonStrip = [&](float cx, float cz, float hx, float hz) {
                const float y = kTowerH + 0.50f;   // riding the coping's top edge
                x3::prims::PrimMesh m = x3::prims::makeBox(hx, 0.06f, hz, cx, y, cz, 1.0f);
                auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                             m.index.data(), (uint32_t)m.index.size());
                x3::game::Entity e{}; e.mesh = mh;
                e.baseColor[0] = 0.10f; e.baseColor[1] = 0.55f; e.baseColor[2] = 0.50f;
                e.emissive[0] = 0.25f; e.emissive[1] = 1.0f; e.emissive[2] = 0.92f; e.emissive[3] = 4.2f;
                e.visible = false;   // the NEON VARIANT toggle
                e.tag = (uint32_t)x3::game::Tag::Static;
                neonIds.push_back(scene.add(e));
            };
            const float nx = kFacilityHalfW + 0.62f, nz0 = kFacilityZ + 0.62f, nz1 = backZ - 0.62f;
            const float ncz = (nz0 + nz1) * 0.5f, nhz = (nz0 - nz1) * 0.5f;
            neonStrip(0.0f, nz0, nx + 0.07f, 0.07f);          // front edge
            neonStrip(0.0f, nz1, nx + 0.07f, 0.07f);          // back edge
            neonStrip(-nx, ncz, 0.07f, nhz);                  // left edge
            neonStrip( nx, ncz, 0.07f, nhz);                  // right edge
        }

        // ---- THE DARK JAGGED ROCK RIDGE + GLOWING CRYSTAL CLUSTERS (lab.jpg's
        //      world): a broken ridge of leaning dark rock slabs crossing the
        //      foreground, with pink/cyan crystal clusters scattered ALONG it and
        //      more clusters at the building's base — the ground-level light. ----
        // A leaning rock slab: box rotated yaw (about Y) then roll (about Z).
        auto rockSlab = [&](float px, float py, float pz, float hx, float hy, float hz,
                            float yawA, float rollA, float shade) {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 0.5f);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            const float cy = std::cos(yawA), sy = std::sin(yawA);
            const float cr = std::cos(rollA), sr = std::sin(rollA);
            x3::game::Entity e{}; e.mesh = mh; e.tex = rockTex; e.mrTex = mrGround;
            // M = Ry(yaw) * Rz(roll), column-major.
            e.transform[0] = cy*cr;  e.transform[1] = sr;  e.transform[2]  = -sy*cr;
            e.transform[4] = -cy*sr; e.transform[5] = cr;  e.transform[6]  = sy*sr;
            e.transform[8] = sy;     e.transform[9] = 0;   e.transform[10] = cy;
            e.transform[12] = px;    e.transform[13] = py; e.transform[14] = pz;
            e.transform[15] = 1.0f;
            e.baseColor[0] = shade; e.baseColor[1] = shade; e.baseColor[2] = shade * 1.14f;
            e.tag = (uint32_t)x3::game::Tag::Static;
            scene.add(e);
        };
        // A faceted glowing shard at an ABSOLUTE spot.
        auto crystal = [&](float px, float py, float pz, float radius, float height,
                           float leanX, float leanZ, float r, float g, float b) {
            x3::prims::PrimMesh m = x3::prims::makeCrystal(radius, height, 6);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            const float cx = std::cos(leanX), sx = std::sin(leanX);
            const float cz = std::cos(leanZ), sz = std::sin(leanZ);
            x3::game::Entity e{};
            e.mesh = mh;
            e.transform[0] = cz;        e.transform[1] = sz;        e.transform[2]  = 0.0f;
            e.transform[4] = -sz * cx;  e.transform[5] = cz * cx;   e.transform[6]  = sx;
            e.transform[8] = sz * sx;   e.transform[9] = -cz * sx;  e.transform[10] = cx;
            e.transform[12] = px;       e.transform[13] = py;       e.transform[14] = pz;
            e.transform[15] = 1.0f;
            e.transparent = true;
            e.glass.opacity   = 0.62f;
            e.glass.roughness = 0.06f;
            e.glass.refraction= 0.04f;
            e.glass.specular  = 1.0f;
            e.glass.tint[0] = r * 0.8f; e.glass.tint[1] = g * 0.8f; e.glass.tint[2] = b * 0.8f;
            e.baseColor[0] = r; e.baseColor[1] = g; e.baseColor[2] = b; e.baseColor[3] = e.glass.opacity;
            // Emissive MODEST so cyan/pink reads as colour, not a white core.
            e.emissive[0] = r; e.emissive[1] = g; e.emissive[2] = b; e.emissive[3] = 1.25f;
            e.tag = (uint32_t)x3::game::Tag::Static;
            scene.add(e);
        };
        // A CLUSTER: 5-6 shards of mixed size/colour erupting around (px, pz) at
        // ground height topY, overall scale s. Deterministic per-call layout.
        auto clusterAt = [&](float px, float pz, float topY, float s, int seed) {
            struct Sh { float dx, dz, rad, h, lx, lz; float col[3]; };
            static const Sh kSh[6] = {
                { -0.30f,  0.20f, 0.34f, 2.6f,  0.05f, -0.10f, {0.42f, 0.78f, 1.00f} },  // cyan hero
                {  0.75f,  0.55f, 0.24f, 1.7f,  0.09f,  0.14f, {1.00f, 0.40f, 0.72f} },  // pink
                { -0.95f,  0.60f, 0.20f, 1.3f, -0.14f, -0.05f, {0.80f, 0.42f, 0.95f} },  // violet
                {  0.30f, -0.70f, 0.18f, 1.0f,  0.12f,  0.20f, {0.48f, 0.72f, 1.00f} },  // cyan
                { -1.30f, -0.35f, 0.15f, 0.8f, -0.20f,  0.08f, {0.95f, 0.45f, 0.90f} },  // violet-pink
                {  1.25f, -0.25f, 0.13f, 0.6f,  0.16f, -0.15f, {1.00f, 0.48f, 0.80f} },  // pink low
            };
            for (int i = 0; i < 6; ++i) {
                const Sh& c = kSh[(i + seed) % 6];
                const float flip = (seed % 2) ? -1.0f : 1.0f;
                crystal(px + c.dx * s * flip, topY, pz + c.dz * s,
                        c.rad * s, c.h * s, c.lx * flip, c.lz,
                        c.col[0], c.col[1], c.col[2]);
            }
        };

        // THE RIDGE: jagged leaning slabs crossing the foreground between the
        // camera line and the approach (walkable OVER via a low collision apron).
        rockSlab(-29.5f, 0.20f, -15.6f, 24.0f, 0.9f, 8.5f, -0.56f,  0.00f, 0.50f);  // ridge apron
        collideBox(-29.5f, 0.35f, -15.6f, 22.0f, 0.9f, 8.0f);
        rockSlab(-42.0f, 1.10f, -23.5f, 5.2f, 2.0f, 2.4f, -0.42f,  0.14f, 0.42f);
        rockSlab(-35.5f, 0.90f, -19.6f, 4.4f, 1.6f, 2.1f, -0.70f, -0.10f, 0.47f);
        rockSlab(-28.6f, 1.25f, -15.2f, 5.6f, 2.3f, 2.6f, -0.52f,  0.09f, 0.44f);
        rockSlab(-21.4f, 0.80f, -11.0f, 4.0f, 1.4f, 2.0f, -0.66f, -0.13f, 0.50f);
        rockSlab(-15.0f, 0.55f,  -7.2f, 3.2f, 1.0f, 1.7f, -0.44f,  0.11f, 0.46f);
        rockSlab(-47.5f, 0.60f, -27.0f, 3.6f, 1.1f, 1.9f, -0.60f, -0.08f, 0.42f);
        // Low scattered boulders trailing off the ridge ends.
        rockSlab(-10.0f, 0.30f,  -4.6f, 1.6f, 0.55f, 1.1f, -0.30f, 0.18f, 0.48f);
        rockSlab(-51.0f, 0.35f, -30.0f, 2.0f, 0.6f, 1.3f, -0.75f, -0.15f, 0.44f);

        // CRYSTAL CLUSTERS along the ridge crest (multiple sizes)...
        clusterAt(-41.5f, -23.0f, 2.55f, 1.35f, 0);
        clusterAt(-28.8f, -14.8f, 3.05f, 1.75f, 1);   // the hero cluster, mid-frame
        clusterAt(-20.8f, -10.6f, 1.85f, 1.05f, 2);
        clusterAt(-14.6f,  -6.8f, 1.25f, 0.70f, 3);
        // ...and AT THE BUILDING'S BASE (the reference scatters them against the
        // dark recessed entry level).
        clusterAt(-18.0f, -44.3f, 0.0f, 0.85f, 4);
        clusterAt(  6.5f, -44.6f, 0.0f, 0.65f, 5);
        clusterAt( 21.0f, -44.1f, 0.0f, 0.95f, 1);

        // ---- ALL POINT LIGHTS: one pooled glow per crystal cluster (alternating
        //      cyan/magenta), the warm lobby + lit-window bleeds, and a soft cool
        //      bounce on the landed ship. ---------------------------------------
        x3::rhi::PointLight pl[16];
        int n = 0;
        auto glow = [&](float px, float py, float pz, float range, bool cyan, float k) {
            pl[n].pos[0] = px; pl[n].pos[1] = py; pl[n].pos[2] = pz; pl[n].range = range;
            if (cyan) { pl[n].color[0] = 0.9f * k; pl[n].color[1] = 3.8f * k; pl[n].color[2] = 6.0f * k; }
            else      { pl[n].color[0] = 6.0f * k; pl[n].color[1] = 1.6f * k; pl[n].color[2] = 4.2f * k; }
            ++n;
        };
        glow(-41.5f, 4.6f, -23.0f, 18.0f, true,  1.0f);   // ridge clusters
        glow(-28.8f, 5.6f, -14.8f, 24.0f, true,  1.2f);   // hero
        glow(-27.6f, 3.6f, -15.6f, 14.0f, false, 0.9f);   // hero's magenta half
        glow(-20.8f, 3.4f, -10.6f, 14.0f, false, 0.8f);
        glow(-14.6f, 2.4f,  -6.8f, 11.0f, true,  0.7f);
        glow(-18.0f, 2.0f, -44.0f, 13.0f, true,  0.8f);   // building-base clusters
        glow(  6.5f, 1.6f, -44.2f, 11.0f, false, 0.7f);
        glow( 21.0f, 2.2f, -43.8f, 13.0f, true,  0.8f);
        // Warm lobby interior (bleeds through the recessed entry glazing).
        pl[n].pos[0] = 0.0f; pl[n].pos[1] = 3.0f; pl[n].pos[2] = kFacilityZ - 6.0f;
        pl[n].range = 40.0f; pl[n].color[0] = 8.0f; pl[n].color[1] = 5.8f; pl[n].color[2] = 3.2f; ++n;
        // Warm bleed just inside two lit-window bands.
        pl[n].pos[0] = 13.0f; pl[n].pos[1] = glassGlow(2); pl[n].pos[2] = kFacilityZ - 1.2f;
        pl[n].range = 12.0f; pl[n].color[0] = 5.0f; pl[n].color[1] = 3.2f; pl[n].color[2] = 1.6f; ++n;
        pl[n].pos[0] = -17.0f; pl[n].pos[1] = glassGlow(3); pl[n].pos[2] = kFacilityZ - 1.2f;
        pl[n].range = 12.0f; pl[n].color[0] = 5.0f; pl[n].color[1] = 3.2f; pl[n].color[2] = 1.6f; ++n;
        // Soft cool moon bounce near the landed ship.
        pl[n].pos[0] = 9.0f; pl[n].pos[1] = 3.5f; pl[n].pos[2] = 10.0f;
        pl[n].range = 22.0f; pl[n].color[0] = 2.2f; pl[n].color[1] = 2.6f; pl[n].color[2] = 3.4f; ++n;
        device->setPointLights(pl, (uint32_t)n);
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

    // ---- TWO CRESCENT MOONS (lab.jpg's alien sky): the FORGE3D Moon body drawn
    //      twice via drawPlanet — one large, one small, upper-left of the hero
    //      framing. CELESTIAL anchoring (the fix/planets-sky technique): each is
    //      re-anchored on the CAMERA EYE every draw (pos = eye + dir * 140 m) so
    //      they never parallax as the player walks. The scene's raking moonlight
    //      sun direction lights them into CRESCENT phase. Missing textures are
    //      graceful (moons simply not drawn).
    x3::rhi::MeshHandle moonMesh{};
    x3::rhi::TextureHandle moonMaps[5];
    bool moonOk = false;
    {
        const std::string p = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/";
        auto loadAbs = [&](const std::string& path, bool srgb) {
            int w = 0, h = 0, c = 0;
            stbi_uc* px = stbi_load(path.c_str(), &w, &h, &c, 4);
            x3::rhi::TextureHandle t{};
            if (px) { t = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgb); stbi_image_free(px); }
            return t;
        };
        moonMaps[0] = loadAbs(p + "Moon/Textures/moon_02.png",        true);   // albedo
        moonMaps[1] = loadAbs(p + "Moon/Textures/moon_02_normal.png", false);  // normal
        moonMaps[2] = loadAbs(p + "Moon/Textures/moon_01_detail.png", true);   // detail
        moonMaps[3] = loadAbs(p + "Moon/Textures/moon_02_spec.png",   false);  // spec
        moonMaps[4] = loadAbs(p + "Atmosphere/sunset_blue_01.png",    true);   // scatter LUT — BRIGHT cool blue-white (lab.jpg's silver moons); _05 was too dark
        moonOk = moonMaps[0].valid();
        if (moonOk) {
            x3::prims::PrimMesh sm = x3::prims::makeUVSphere(48, 96);
            moonMesh = device->createMesh(sm.verts.data(), (uint32_t)sm.verts.size(),
                                          sm.index.data(), (uint32_t)sm.index.size());
        }
        x3::logInfo(std::string("--world surface: crescent moons ") +
                    (moonOk ? "LOADED (FORGE3D Moon x2)" : "SKIPPED (textures missing)"));
    }
    // Sky-direction + apparent-size table for the two moons (normalized below).
    struct MoonBody { float dir[3]; float diamDeg; };
    // Placed UPPER-LEFT + forward + high (lab.jpg framing) — and, crucially, angled
    // so each moon's direction has a POSITIVE dot with the scene sunDir
    // (-0.40,0.60,0.70): that puts the terminator across the visible disc so they
    // read as a lit CRESCENT/half rather than a flat full disc. Kept below the
    // ~35 deg frame ceiling (pitch 0.30 + half-fov) so both stay in shot.
    // Bright COOL gibbous pair, upper-left + upper-center in clear sky. Directions
    // chosen so dot(sunDir,dir) is negative (~-0.2): most of each disc is lit
    // (bright, reads as a moon) with a slim terminator on one edge (the phase),
    // rather than a dim half-lit ball. High enough to clear the tower, inside the
    // frame ceiling.
    // In the OPEN upper-right sky (the tower + ridge fill the left/centre of the
    // hero framing, so the right is the clean expanse). dot(sunDir,dir) ~ -0.35
    // -> a bright ~68%-lit disc with a slim terminator on the lower-left edge (the
    // phase reads), rather than a dull half-ball.
    MoonBody moons[2] = {
        { { 0.28f, 0.50f, -0.80f }, 8.5f },   // the LARGE moon, open upper-right
        { { 0.46f, 0.60f, -0.66f }, 4.4f },   // the small companion, up + right of it
    };
    for (auto& mb : moons) {
        const float l = std::sqrt(mb.dir[0]*mb.dir[0] + mb.dir[1]*mb.dir[1] + mb.dir[2]*mb.dir[2]);
        mb.dir[0] /= l; mb.dir[1] /= l; mb.dir[2] /= l;
    }
    constexpr float kMoonDist = 140.0f;   // inside the 200 m far plane, past all geometry
    auto drawMoons = [&](const x3::rhi::FrameContext& frame, float ex, float ey, float ez) {
        if (!moonOk) return;
        for (const auto& mb : moons) {
            const float r = kMoonDist * std::tan(mb.diamDeg * 0.5f * 3.14159265f / 180.0f);
            float m[16] = { r,0,0,0, 0,r,0,0, 0,0,r,0,
                            ex + mb.dir[0]*kMoonDist, ey + mb.dir[1]*kMoonDist, ez + mb.dir[2]*kMoonDist, 1 };
            device->drawPlanet(frame, moonMesh, m, 0u /*Moon*/, moonMaps, 5u, 0.0f);
        }
    };

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
        drawMoons(frame, cx, cy, cz);   // the two crescent moons (camera-anchored)
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
        // Derive the NEON-VARIANT path: "<stem>_neon.<ext>" (default hero_final ->
        // hero_final_neon). Tim picks between primary (Lab2 clean crown) and neon
        // (lab.jpg's cyan parapet strip).
        auto neonVariantPath = [](const std::string& p) {
            const size_t dot = p.find_last_of('.');
            return (dot == std::string::npos) ? p + "_neon"
                                              : p.substr(0, dot) + "_neon" + p.substr(dot);
        };
        // Render kFrames from the fixed vantage (so temporal effects settle),
        // arming the capture on the final frame, then write it to disk. Moons are
        // drawn camera-anchored; the view-anchored weapon is omitted (at 0.6 m it
        // blacks out a corner of the architecture shot).
        auto renderAndCapture = [&](const std::string& path) -> bool {
            const int kFrames = 40;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                phys->step(dt);
                rescue.tick(dt, scene, *phys, player.feet());   // hub NOT reached -> no timers
                scene.update(*phys);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kFrames - 1) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    scene.render(*device, frame);
                    rescue.draw(*device, frame, scene);
                    drawShip(frame);
                    drawMoons(frame, cam[0], cam[1], cam[2]);   // TWO crescent moons in the sky
                }
                device->endFrame(frame);
            }
            return device->captureFrame(path.c_str());
        };
        auto setNeon = [&](bool on) { for (uint32_t id : neonIds) scene.get(id).visible = on; };

        // PRIMARY hero — Lab2's clean white crown (neon parapet OFF).
        setNeon(false);
        const bool wrote = renderAndCapture(outPath);
        // NEON VARIANT — lab.jpg's cyan parapet ring ON (second labeled shot).
        setNeon(true);
        const std::string neonPath = neonVariantPath(outPath);
        const bool wroteNeon = renderAndCapture(neonPath);
        setNeon(false);
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            char rb[380];
            std::snprintf(rb, sizeof(rb),
                "--world surface: wrote %s (+neon %s:%s) | entities=%u draws=%u tris=%u ship=%s moons=%s sarah=%s",
                outPath.c_str(), neonPath.c_str(), wroteNeon ? "ok" : "FAIL",
                scene.size(), st.drawCalls, st.triangles,
                shipModel.ok ? "REAL" : "box", moonOk ? "x2" : "none",
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
