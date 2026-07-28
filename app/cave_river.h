#pragma once
// UNDERGROUND RIVER (feat/cave-river) — a mildly LUMINESCENT BLUE stream that threads
// the low cave tubes of the descent (club_bedrock.cpp side-shoots) and gives off SOME
// light in the dark, crystal-lit caves. It is the underground counterpart of the
// surface river — but the engine's surface water (setWaterParams/caustics/god-rays) is
// SKY+SUN dependent and there is NO sky down here. Tim's solve: make the water
// SELF-LUMINESCENT (emissive deep-blue) so it needs no sun — it IS a light source, a
// dimmer, cooler cousin of the Salvari crystals.
//
// HOW IT WORKS (NOT the sky water system):
//   * GEOMETRY: a flat water RIBBON (a chain of short quad segments) laid just above
//     the walkable floor of selected side-shoot cave tubes (Cache / Landmark cathedral
//     / Collapse), threading the low points, widening + POOLING in the cavern bellies
//     and dead-ends. Coarse, follows the authored cave layout. The node polyline is
//     emitted by buildEarthTunnels() into DescentFallLayout::river.
//   * SELF-EMISSIVE BLUE: each segment carries a MILD deep-electric-blue emissive
//     (peak channel held well under 1.0 so ACES holds the hue — a gentle glow, not a
//     blinding white strip), plus a deep-blue baseColor. No sky/IBL needed.
//   * LIGHTS THE BANKS: a HANDFUL of DIM blue point lights sit over the POOLS, pushed
//     into the SAME distance-culled crystal/bedrock light channel the Salvari crystals
//     use (host cull @50 m, 64-light cap) — so the water softly lights the surrounding
//     rock without its own bespoke lighting path.
//   * FLOWING: update() scrolls a bright CREST downstream (dt-scaled) by modulating each
//     segment's emissive strength along the river, + a gentle ripple BOB on the surface
//     Y, so it reads as living, moving water — not a static glowing strip. Pools breathe
//     slower + brighter.
//
// Self-test (--test-caveatmos folds it in): the ribbon builds, is emissive blue-dominant
// and MILD, and the bank lights are dim + short-range (a handful).

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"
#include "club_bedrock.h"          // CaveRiverNode, DescentFallLayout

#include <vector>

namespace x3::game {

class CaveRiver {
public:
    // Build the water ribbon + pool bank-lights from the published river nodes. Bank
    // lights are APPENDED to outLights (the distance-culled bedrock/crystal channel).
    // Safe no-op if nodes is empty. Returns the number of Scene entities added.
    int build(Scene& scene, x3::rhi::IRenderDevice& device,
              const std::vector<CaveRiverNode>& nodes,
              std::vector<x3::rhi::PointLight>* outLights);

    // Per-frame flow: scroll a bright crest downstream + a gentle ripple bob on each
    // segment (dt-scaled). Cheap — mutates entity emissive strength + transform Y in
    // place. No-op until built.
    void update(float dt, Scene& scene);

    bool built() const { return !m_segs.empty(); }
    int  segmentCount() const { return (int)m_segs.size(); }

    // Headless self-test: the ribbon builds, is MILD emissive blue, banks are dim/short.
    static bool runSelfTest();

private:
    struct Seg {
        uint32_t id       = 0;      // scene entity id (a ribbon quad)
        float    s01      = 0.0f;   // 0..1 position along the whole river (flow phase)
        float    baseEmis = 0.30f;  // base emissive strength (pools brighter)
        bool     pool     = false;
    };
    std::vector<Seg>       m_segs;
    float                  m_flow = 0.0f;   // accumulated flow phase (dt-scaled)
    x3::rhi::TextureHandle m_tex{};         // one shared subtle water-mottle texture
};

} // namespace x3::game
