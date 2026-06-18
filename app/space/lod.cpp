// S2 — Distance-LOD selection implementation. See lod.h for the design.
#include "lod.h"

#include <algorithm>

namespace x3::space {

uint32_t LodSystem::select(const LodSet& set, float distance) const {
    if (set.levels == 0) return 0;                 // defensive: empty chain
    const uint32_t maxLevel = std::min<uint32_t>(set.levels, 4u) - 1u;

    // Choose the band index from the ascending thresholds. A distance below the
    // first threshold is LOD0; >= threshold[k] steps to level k+1. Only the
    // first (levels-1) thresholds are meaningful for a chain shorter than 4.
    uint32_t level = 0;
    for (uint32_t k = 0; k < 3 && k < maxLevel; ++k) {
        if (distance >= set.switchDist[k]) level = k + 1;
        else break;                                // ascending: first miss ends it
    }

    // Clamp to the highest populated level so a 2-LOD set never indexes an
    // empty slot when viewed from far away.
    if (level > maxLevel) level = maxLevel;
    return set.mesh[level];
}

LodSet LodSystem::makeFromChain(uint32_t lod0, uint32_t lod1, uint32_t lod2, uint32_t lod3,
                                float d01, float d12, float d23, uint32_t levels) const {
    LodSet s;
    s.levels = std::clamp<uint32_t>(levels, 1u, 4u);
    const uint32_t src[4] = {lod0, lod1, lod2, lod3};
    for (uint32_t i = 0; i < 4; ++i)
        s.mesh[i] = (i < s.levels) ? src[i] : 0u;  // zero unused slots
    s.switchDist[0] = d01;
    s.switchDist[1] = d12;
    s.switchDist[2] = d23;
    return s;
}

} // namespace x3::space
