// GOLD MINE — see mine_fx.h for the port status / punch list. This file:
//   1. bakeMouthGlowRGBA — ports mine-fx.ts's mouthMouthFragment arch-SDF math
//      (D:\GameDev\epochs-rts\src\render\shaders.ts, mineMouthFragment) from a
//      LIVE Babylon ShaderMaterial fragment shader into a small procedural
//      RGBA8 texture baked once on the CPU. X3Native's idiomatic glow
//      convention (docs/design/X3_WORLD_RULES.md rule 5) is a texture-gated
//      emissiveTex over a near-black albedo, not a bespoke live fragment
//      shader per-effect, so this bakes the SAME gradient a live shader would
//      compute per-pixel, once, at build time. A live animated GLSL version
//      (flicker/breathing/pulse) is the natural next step — see the punch
//      list in mine_fx.h.
//   2. GoldMineWorld::build — the timber-framed shaft entrance + rocky berm,
//      ported from buildMineEntranceMesh (D:\GameDev\epochs-rts\src\render\
//      meshes.ts) as a set of world-space-authored boxes (this engine's
//      static-prop convention — see app/club1127.cpp's addBox), streamlined
//      to the CORE silhouette for tonight (berm mass + timber frame + dark
//      throat + rails); NOT yet ported: rubble ring, wheelbarrow, tailings
//      heap, shift-lantern, threshold spill, motes, animation.
//   3. runMineFxSelfTest — headless (no window/Vulkan) proof that the build
//      produces real, varying (not flat) glow texel data and the expected
//      fixture census — the club1127 OLED-regression lesson ("the panel
//      exists" != "the panel SHOWS SOMETHING").
#include "mine_fx.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Same clamp-and-Hermite smoothstep the Babylon shader uses (GLSL smoothstep).
inline float smoothstepf(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Ports mineMouthFragment (shaders.ts) 1:1: p in [-0.5,0.5]^2 pane-local
// space (Babylon's vLocal = the unit plane's own position.xy — NOT a UV
// attribute, so this is genuinely the same coordinate space, not an
// approximation). Returns the "g" glow term, ~0..1.1 — 0 in the dark throat
// and off-pane, rising to a bright rim licking the jambs/crown, brighter low
// (near the sill) than at the crown, exactly the "light climbs from below"
// read the source's comment calls out.
float mouthGlowSDF(float px, float py) {
    // arch silhouette: straight jambs, rounded crown, floor cut at the sill
    float prof = (py > 0.10f) ? std::sqrt(px * px + std::pow((py - 0.10f) * 1.05f, 2.0f))
                               : std::fabs(px);
    float arch = (1.0f - smoothstepf(0.30f, 0.42f, prof)) * smoothstepf(-0.52f, -0.44f, py);
    // the THROAT — an inner dark arch running down to the sill (the passage
    // descending out of sight): light reads as a lit RIM around a dark way
    // down, never a donut hole.
    float profIn = (py > -0.02f) ? std::sqrt(px * px + std::pow((py + 0.02f) * 1.15f, 2.0f))
                                  : std::fabs(px);
    float throat = 1.0f - smoothstepf(0.13f, 0.27f, profIn);
    float wall   = smoothstepf(0.10f, 0.31f, profIn);   // glow licking the jambs + crown
    // the light climbs from BELOW: strongest low on the jambs, dimmer at crown
    float low = 0.62f + 0.55f * smoothstepf(0.32f, -0.50f, py);
    float g = arch * std::clamp(wall * low - throat * 1.2f, 0.0f, 1.1f);
    return g;
}

// Bakes the arch-glow gradient into a WxH grayscale-in-RGBA8 texture (R=G=B=
// g*255, A=255). u,v run 0..1 across the quad; texel (col,row) -> v = row/(H-1)
// (row 0 = pane bottom, +Y = up — matches the quad builder below, which puts
// UV v=0 on the -halfH/bottom vertices). Returns the raw byte buffer.
std::vector<uint8_t> bakeMouthGlowRGBA(uint32_t w, uint32_t h, float* outMaxG = nullptr,
                                       float* outMinG = nullptr) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    float maxG = -1e9f, minG = 1e9f;
    for (uint32_t row = 0; row < h; ++row) {
        float v = (h > 1) ? (float)row / (float)(h - 1) : 0.5f;
        float py = v - 0.5f;
        for (uint32_t col = 0; col < w; ++col) {
            float u = (w > 1) ? (float)col / (float)(w - 1) : 0.5f;
            float pxl = u - 0.5f;
            float g = mouthGlowSDF(pxl, py);
            maxG = std::max(maxG, g);
            minG = std::min(minG, g);
            uint8_t b = (uint8_t)std::clamp(g * 255.0f, 0.0f, 255.0f);
            size_t idx = (static_cast<size_t>(row) * w + col) * 4;
            px[idx + 0] = b; px[idx + 1] = b; px[idx + 2] = b; px[idx + 3] = 255;
        }
    }
    if (outMaxG) *outMaxG = maxG;
    if (outMinG) *outMinG = minG;
    return px;
}

// 1x1 metallic-roughness texel (glTF packing: G=roughness, B=metallic) — a
// plain matte dielectric, the same recipe club1127.cpp's makeMr1x1 uses to
// force an entity onto the PBR/emissiveTex route (X3_WORLD_RULES.md rule 5:
// "assign a shared 1x1 MR to force the PBR path").
std::vector<uint8_t> makeMr1x1(uint8_t rough, uint8_t metal) {
    return { 255, rough, metal, 255 };
}

} // namespace

uint32_t GoldMineWorld::addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                               float cx, float cy, float cz,
                               float hx, float hy, float hz,
                               const float color[3]) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.baseColor[0] = color[0]; e.baseColor[1] = color[1]; e.baseColor[2] = color[2]; e.baseColor[3] = 1.0f;
    e.tag = (uint32_t)Tag::Static;
    return scene.add(e);
}

uint32_t GoldMineWorld::addMouthGlow(Scene& scene, x3::rhi::IRenderDevice& device,
                                     float cx, float cy, float cz,
                                     float halfW, float halfH) {
    // A vertical +Z-facing quad authored directly in world space, same index/
    // winding convention as makeBox's own +Z face (CCW, matches
    // VK_FRONT_FACE_COUNTER_CLOCKWISE): bottom-left, bottom-right, top-right,
    // top-left; UV v=0 at the bottom (matches bakeMouthGlowRGBA's row->v map).
    x3::prims::PrimMesh geo;
    geo.verts = {
        {{cx - halfW, cy - halfH, cz}, {0, 0, 1}, {0, 0}},
        {{cx + halfW, cy - halfH, cz}, {0, 0, 1}, {1, 0}},
        {{cx + halfW, cy + halfH, cz}, {0, 0, 1}, {1, 1}},
        {{cx - halfW, cy + halfH, cz}, {0, 0, 1}, {0, 1}},
    };
    geo.index = { 0, 1, 2, 0, 2, 3 };

    const uint32_t texN = 64;
    std::vector<uint8_t> glowRGBA = bakeMouthGlowRGBA(texN, texN);
    x3::rhi::TextureHandle glowTex = device.createTexture(glowRGBA.data(), texN, texN, /*srgb*/ false);

    auto mrPx = makeMr1x1(/*rough*/ 200, /*metal*/ 0);
    x3::rhi::TextureHandle mr1x1 = device.createTexture(mrPx.data(), 1, 1, /*srgb*/ false);

    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.emissiveTex = glowTex;
    e.mrTex       = mr1x1;   // routes through drawMeshPBR so emissiveTex is honored (rule 5)
    // Near-black albedo (rule 5: "texture-gated emissiveTex ~1.1 over near-
    // black albedo") — all the visual interest comes from the emissive mask.
    e.baseColor[0] = 0.02f; e.baseColor[1] = 0.02f; e.baseColor[2] = 0.02f; e.baseColor[3] = 1.0f;
    // GOLD glow (metal-look.ts "gold" = [1.0, 0.80, 0.16]), strength pushed
    // past 1 so the bright rim drives the bloom chain (the additive-VFX "keep
    // a glow floor, only ever add light" law — this term is strictly additive).
    e.emissive[0] = kGoldGlow[0]; e.emissive[1] = kGoldGlow[1]; e.emissive[2] = kGoldGlow[2];
    e.emissive[3] = 2.4f;
    e.tag = (uint32_t)Tag::Static;
    return scene.add(e);
}

const GoldMineWorld::Stats& GoldMineWorld::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                                  float ox, float oy, float oz) {
    if (m_built) return m_stats;
    m_built = true;
    m_originX = ox; m_originY = oy; m_originZ = oz;

    const uint32_t entsBefore = scene.size();

    // ---- palette (ported 1:1 from meshes.ts buildMineEntranceMesh) --------
    const float ROCK_DK[3]    = { 0.14f, 0.15f, 0.18f };
    const float ROCK[3]       = { 0.24f, 0.24f, 0.27f };
    const float ROCK_LT[3]    = { 0.33f, 0.33f, 0.36f };
    const float TIMBER[3]     = { 0.36f, 0.26f, 0.15f };
    const float TIMBER_DK[3]  = { 0.25f, 0.17f, 0.10f };
    const float DARKINT[3]    = { 0.03f, 0.03f, 0.045f };
    const float RAIL[3]       = { 0.30f, 0.28f, 0.26f };

    // ---- rocky berm: a cut hillside humped around the mouth, open to +Z ---
    // (streamlined to boxes for tonight's core pass — the source's elliptical
    // sphere mounds are a follow-up, see mine_fx.h punch list)
    addBox(scene, device, ox, oy + 0.55f, oz - 0.70f, 1.10f, 0.90f, 0.60f, ROCK_DK); // back mound
    addBox(scene, device, ox - 0.85f, oy + 0.50f, oz - 0.25f, 0.50f, 0.70f, 0.55f, ROCK);   // left shoulder
    addBox(scene, device, ox + 0.85f, oy + 0.50f, oz - 0.25f, 0.50f, 0.70f, 0.55f, ROCK);   // right shoulder
    addBox(scene, device, ox, oy + 1.55f, oz - 0.55f, 0.85f, 0.35f, 0.50f, ROCK_LT);        // crag over the lintel
    m_stats.hasBerm = true;

    // ---- the shaft throat (near-black recess the glow lights up) ----------
    addBox(scene, device, ox, oy + 0.64f, oz - 0.35f, 0.56f, 0.64f, 0.30f, DARKINT);
    m_stats.hasThroat = true;

    // ---- timber portal frame around the mouth ------------------------------
    addBox(scene, device, ox - 0.63f, oy + 0.75f, oz + 0.02f, 0.09f, 0.75f, 0.10f, TIMBER);      // left post
    addBox(scene, device, ox + 0.63f, oy + 0.75f, oz + 0.02f, 0.09f, 0.75f, 0.10f, TIMBER);      // right post
    addBox(scene, device, ox, oy + 1.46f, oz + 0.02f, 0.78f, 0.11f, 0.12f, TIMBER_DK);           // heavy lintel
    m_stats.hasTimberFrame = true;

    // ---- mine-cart rails running out the mouth (+Z), on the ground ---------
    addBox(scene, device, ox - 0.23f, oy + 0.03f, oz + 0.80f, 0.025f, 0.025f, 0.80f, RAIL);
    addBox(scene, device, ox + 0.23f, oy + 0.03f, oz + 0.80f, 0.025f, 0.025f, 0.80f, RAIL);
    m_stats.hasRails = true;

    // ---- (a) the lit shaft — light licking the tunnel walls around a black
    // core; the mouth sits a touch forward of the throat, lifted to the arch
    // middle (mirrors mine-fx.ts's `my = y + 0.66`).
    const float mx = ox, my = oy + 0.70f, mz = oz + 0.06f;
    addMouthGlow(scene, device, mx, my, mz, 0.60f, 0.78f);
    m_stats.hasMouthGlow = true;
    m_stats.mouthX = mx; m_stats.mouthY = my; m_stats.mouthZ = mz;

    m_stats.entities = (int)(scene.size() - entsBefore);
    x3::logInfo("[mine_fx] built GOLD MINE entrance at (" + std::to_string(ox) + ", " +
               std::to_string(oy) + ", " + std::to_string(oz) + "): " +
               std::to_string(m_stats.entities) + " entities");
    return m_stats;
}

void GoldMineWorld::showcaseCamera(float out[5]) const {
    // Standing on the +Z side, a few meters back and up, looking toward -Z
    // (engine default facing) so the mouth glow fills the frame head-on.
    // NOTE: the engine's yaw is measured from +X, not -Z (see
    // VulkanRenderDevice_internal.h's own default m_camYaw = -1.5708f "look
    // toward -Z", and the forward-vector formula in vk_passes.cpp:1287-1289:
    // fwd = (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw)) — yaw=0
    // faces +X, yaw=-pi/2 faces -Z). Use -pi/2 here, NOT 0.
    out[0] = m_originX;             // x
    out[1] = m_originY + 1.35f;     // y
    out[2] = m_originZ + 4.2f;      // z
    out[3] = -1.5707963f;           // yaw: -pi/2 faces -Z (toward the mouth)
    out[4] = -0.12f;                // pitch (slight look-down)
}

// ---------------------------------------------------------------------------
bool runMineFxSelfTest() {
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [") + (ok ? "PASS" : "FAIL") + "] " + name);
    };

    // ---- the glow gradient itself: prove it VARIES (not a flat wash) — the
    // club1127 OLED-regression lesson ("the panel exists" != "the panel SHOWS
    // SOMETHING"). A dark corner (outside the arch, e.g. near a pane corner)
    // must read near-zero; the bright rim (low on a jamb) must read strongly
    // lit; and the deep throat center must stay dark (it's a HOLE, not a lit
    // disc — the defining visual claim of this whole effect).
    {
        float maxG = 0, minG = 0;
        auto rgba = bakeMouthGlowRGBA(64, 64, &maxG, &minG);
        check("mouth glow texture is nonempty", rgba.size() == 64u * 64u * 4u);
        check("mouth glow has a genuine bright rim (max > 0.5)", maxG > 0.5f);
        check("mouth glow has real dark texels (min < 0.05)", minG < 0.05f);
        // A pane corner (u=0.05,v=0.95 -> p=(-0.45,0.45), well outside the
        // arch silhouette) must be dark.
        float cornerG = mouthGlowSDF(-0.45f, 0.45f);
        check("pane corner (outside the arch) reads dark", cornerG < 0.05f);
        // On the jamb rim itself (profIn just past the throat's inner radius,
        // mid-height) should be bright — the "light climbs from below" claim
        // from the source comment. (Verified by hand against mouthGlowSDF's
        // ported formula: at (0.28,-0.10) arch=1, throat=0, wall~0.94,
        // low~0.90 => g~0.85.)
        float jambG = mouthGlowSDF(0.28f, -0.10f);
        check("jamb rim reads bright (the climbs-from-below rim)", jambG > 0.5f);
        // Dead-center, deep in the throat, must stay dark (a hole, not a lit
        // disc — mirrors mine-fx.ts's design intent verbatim).
        float throatG = mouthGlowSDF(0.0f, 0.0f);
        check("throat centre stays dark (a hole, not a lit disc)", throatG < 0.15f);
    }

    // ---- the geometry + fixture census, on the headless (no window/Vulkan)
    // device — mirrors the runClubSelfTest pattern.
    {
        HeadlessRenderDevice device;
        Scene scene;
        GoldMineWorld mine;
        const auto& stats = mine.build(scene, device, 0.0f, 0.0f, 0.0f);

        check("build() is idempotent (second call is a no-op)",
              &mine.build(scene, device, 0.0f, 0.0f, 0.0f) == &stats);
        check("berm authored", stats.hasBerm);
        check("timber frame authored", stats.hasTimberFrame);
        check("throat authored", stats.hasThroat);
        check("rails authored", stats.hasRails);
        check("mouth glow authored", stats.hasMouthGlow);
        check("nonzero entity census", stats.entities > 0);
        check("scene actually grew by the reported count", scene.size() == (uint32_t)stats.entities);

        // The mouth-glow entity itself: find it (last-added entity) and prove
        // it carries BOTH the emissiveTex AND the mrTex the PBR/glow route
        // needs — the exact "emissiveTex with no mrTex silently drops the
        // glow" trap Scene.h's KNOWN_BUGS L4 comment documents. We assign our
        // own mrTex explicitly (not relying on Scene's fallback), so assert
        // that directly.
        bool mouthOk = false;
        if (scene.size() > 0) {
            const Entity& last = scene.get(scene.size() - 1);
            mouthOk = last.mesh.valid() && last.emissiveTex.valid() && last.mrTex.valid() &&
                     last.emissive[3] > 1.0f;   // strength > 1 => a real HDR bloom source
        }
        check("mouth-glow entity carries mesh + emissiveTex + mrTex + HDR emissive strength", mouthOk);

        // Camera pose sanity: faces the mouth from the +Z side, matches
        // build()'s origin.
        float cam[5]; mine.showcaseCamera(cam);
        check("showcase camera stands off the +Z side of the mouth", cam[2] > stats.mouthZ);

        // Clean teardown: no crash destroying what we created.
        for (uint32_t i = 0; i < scene.size(); ++i) {
            Entity& e = scene.get(i);
            if (e.mesh.valid()) device.destroyMesh(e.mesh);
        }
        check("teardown did not crash (reached this line)", true);
    }

    x3::logInfo("minefx: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

} // namespace x3::game
