#pragma once
// GOLD MINE — native Vulkan/C++ port (CORE geometry + golden mouth glow) of
// Empires of Shadow's (epochs-rts) procedural metal-mine render system.
//
// Source ported FROM (Babylon.js, read-only reference, Tim's other project):
//   D:\GameDev\epochs-rts\src\render\mine-fx.ts     (MineFxLayer, the beauty pass)
//   D:\GameDev\epochs-rts\src\render\meshes.ts      (buildMineEntranceMesh)
//   D:\GameDev\epochs-rts\src\render\metal-look.ts  (the new "gold" glow entry,
//     [1.0, 0.80, 0.16] — added on epochs-rts branch feature/gold-mine-render)
//   D:\GameDev\epochs-rts\src\render\shaders.ts     (mineMouthFragment — the
//     "light licking tunnel walls around a black centre" arch-glow math this
//     file bakes into a procedural texture; see bakeMouthGlowRGBA below)
//
// PORT STATUS (tonight, first session — see mine_fx.cpp top comment for the
// full punch list of what is NOT yet ported):
//   DONE   — timber-framed shaft entrance + rocky berm, static procedural
//            geometry (boxes only — no elliptical berm mounds yet)
//   DONE   — the mouth glow: a texture-gated emissiveTex quad baking the SAME
//            arch-SDF math as mineMouthFragment (dark throat, glowing rim,
//            brighter low on the jambs), gold-tinted, per X3_WORLD_RULES.md
//            rule 5 ("texture-gated emissiveTex ~1.1 over near-black albedo" —
//            this engine's idiomatic glow convention; NOT a live custom GLSL
//            fragment shader like the Babylon source — see follow-up list)
//   NOT DONE — rubble ring (boulders + ore rocks), wheelbarrow, tailings heap,
//            shift-lantern, rails/sleepers, threshold spill quad, drifting ore
//            motes, animated flicker/breathing, night-distance beacon scaling,
//            per-node placement/clustering (computeMineMouthIds equivalent),
//            collision (no physics body authored yet — visual-only prop)
//
// Determinism discipline (carried over from the source's own law): this is
// PURE render/prop-authoring code. It reads nothing from sim/ECS state and
// writes nothing back to it — build() just stands up Scene entities from
// caller-supplied world coordinates. No RNG; everything here is either a
// literal authored constant or a pure function of its inputs.

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

// GOLD MINE — a single timber-framed shaft entrance cut into a rocky berm,
// with a golden glow pouring from the mouth. Render-only, deterministic,
// static geometry authored directly in WORLD space (Entity::transform stays
// identity, matching the addBox/Club1127World convention for one-off static
// props — see app/club1127.cpp).
class GoldMineWorld {
public:
    // metal-look.ts "gold": saturated yellow-gold, distinct from copper's
    // rose-orange and iron's amber. Single source of truth for this port —
    // change here if the EoS-side table (metal-look.ts) is ever retuned.
    static constexpr float kGoldGlow[3] = { 1.0f, 0.80f, 0.16f };

    // Fixture census — the headless self-test's assertions.
    struct Stats {
        int  entities      = 0;    // total Scene entities authored
        bool hasBerm        = false;
        bool hasTimberFrame = false;
        bool hasThroat       = false;   // the dark shaft recess
        bool hasMouthGlow    = false;   // the emissiveTex golden glow quad
        bool hasRails        = false;
        float mouthX = 0, mouthY = 0, mouthZ = 0;   // world position of the glow quad center
    };

    // Build one entrance into `scene`, uploading meshes/textures through
    // `device`, centered at world (ox,oy,oz) with the mouth opening toward
    // +Z (engine default facing is -Z — see CLAUDE.md AXES — so a camera
    // standing on the +Z side looking toward -Z faces the mouth head-on).
    // Call once; a second call on an already-built instance is a no-op.
    const Stats& build(Scene& scene, x3::rhi::IRenderDevice& device,
                       float ox = 0.0f, float oy = 0.0f, float oz = 0.0f);

    // A fixed showcase camera pose (x,y,z,yaw,pitch) that frames the mouth
    // head-on, for the headless --screenshot proof.
    void showcaseCamera(float out[5]) const;

    const Stats& stats() const { return m_stats; }
    bool built() const { return m_built; }

private:
    // A solid static box (render mesh + Scene entity), flat-tinted. Center +
    // half-extents in world meters. No collision yet (see follow-up list).
    uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                    float cx, float cy, float cz, float hx, float hy, float hz,
                    const float color[3]);

    // The golden mouth-glow quad: a vertical +Z-facing plane at (cx,cy,cz),
    // half-extents (halfW,halfH), carrying a baked emissiveTex (see
    // bakeMouthGlowRGBA in the .cpp) through the texture-gated PBR glow path.
    uint32_t addMouthGlow(Scene& scene, x3::rhi::IRenderDevice& device,
                          float cx, float cy, float cz, float halfW, float halfH);

    bool  m_built = false;
    Stats m_stats{};
    float m_originX = 0.0f, m_originY = 0.0f, m_originZ = 0.0f;
};

// --test-minefx: headless self-test (HeadlessRenderDevice — no window/Vulkan,
// runs anywhere). Builds one gold mine entrance, asserts the fixture census
// (berm + timber frame + throat + mouth glow + rails all present, a nonzero
// entity count, and — the "the panel exists" vs. "the panel SHOWS SOMETHING"
// lesson from the club1127 OLED regression — that the mouth-glow texel data
// is NOT flat (the baked arch gradient actually varies: a bright rim and a
// dark throat, not a uniform wash)). Logs "minefx: X/Y passed" and returns
// true iff all pass.
bool runMineFxSelfTest();

} // namespace x3::game
