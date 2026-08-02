// Screen-space-error discrete LOD selection. See mesh_lod.h for the design.
#include "mesh_lod.h"

#include "engine/core/IConsole.h"

#include <algorithm>
#include <cmath>

namespace x3::game {
namespace {

// Never divide by a distance smaller than this. An object the camera is INSIDE
// of would otherwise produce an infinite error; clamping simply pins it to LOD0,
// which is what you want.
constexpr float kMinDist = 0.05f;

} // namespace

float LodView::projScale() const {
    const float t = std::tan(fovYDeg * 0.5f * 3.14159265358979f / 180.0f);
    if (t <= 1e-6f) return 0.0f;
    return (float)viewportH / (2.0f * t);
}

float lodMaxScale(const float m[16]) {
    // Column-major: columns 0,1,2 are the basis vectors.
    float s = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float x = m[c * 4 + 0], y = m[c * 4 + 1], z = m[c * 4 + 2];
        s = std::max(s, std::sqrt(x * x + y * y + z * z));
    }
    return s;
}

float lodDistance(const LodView& v, const MeshLodChain& c, const float m[16]) {
    // Transform the model-space sphere centre into world space.
    const float cx = c.center[0], cy = c.center[1], cz = c.center[2];
    const float wx = m[0] * cx + m[4] * cy + m[8]  * cz + m[12];
    const float wy = m[1] * cx + m[5] * cy + m[9]  * cz + m[13];
    const float wz = m[2] * cx + m[6] * cy + m[10] * cz + m[14];
    const float dx = wx - v.eye[0], dy = wy - v.eye[1], dz = wz - v.eye[2];
    const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float r  = c.radius * lodMaxScale(m);
    return std::max(d - r, kMinDist);
}

float lodPixelError(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                    const float m[16], uint32_t level) {
    (void)p;
    if (level == 0 || level >= c.levels) return (level == 0) ? 0.0f : 1.0e30f;
    const float worldErr = c.error[level] * lodMaxScale(m);
    if (worldErr <= 0.0f) return 0.0f;
    return worldErr * v.projScale() / lodDistance(v, c, m);
}

namespace {

// Distance-band selection — the NEGATIVE CONTROL. Kept here (rather than in the
// test) so the test drives the SAME entry point the shipping path uses and the
// only difference is the policy flag. It is never enabled outside --test-geolod.
uint32_t selectDistanceOnly(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                            const float m[16]) {
    const float d = lodDistance(v, c, m);
    uint32_t level = 0;
    for (uint32_t k = 0; k < 3 && k + 1 < c.levels; ++k) {
        if (d >= p.distanceBand[k]) level = k + 1;
        else break;
    }
    return std::min(level, c.levels - 1);
}

} // namespace

uint32_t lodSelect(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                   const float m[16]) {
    if (!p.enabled || !c.hasChain()) return 0;
    if (p.distanceOnly) return selectDistanceOnly(v, p, c, m);

    const float budget = std::max(p.pixelError, 1.0e-4f);
    uint32_t level = 0;
    for (uint32_t i = 1; i < c.levels; ++i) {
        if (lodPixelError(v, p, c, m, i) <= budget) level = i;
        else break;              // error[] ascends, so the first miss ends it
    }
    return level;
}

uint32_t lodSelectHysteretic(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                             const float m[16], uint32_t prev) {
    if (!p.enabled || !c.hasChain()) return 0;
    if (p.distanceOnly) return selectDistanceOnly(v, p, c, m);

    const float budget = std::max(p.pixelError, 1.0e-4f);
    const float h      = std::clamp(p.hysteresis, 0.0f, 0.9f);
    const float upThr  = budget * (1.0f + h);   // must EXCEED this to refine
    const float dnThr  = budget * (1.0f - h);   // must fall UNDER this to coarsen

    uint32_t level = std::min(prev, c.levels - 1);

    // Refine while the level we are on is too coarse by more than the band.
    while (level > 0 && lodPixelError(v, p, c, m, level) > upThr) --level;
    // Coarsen while the NEXT level is comfortably inside the band.
    while (level + 1 < c.levels && lodPixelError(v, p, c, m, level + 1) <= dnThr) ++level;

    return level;
}

LodPolicy& lodPolicy() {
    static LodPolicy p;
    return p;
}

LodView lodViewFromDevice(const x3::rhi::IRenderDevice& device) {
    LodView v{};
    device.cameraLodInfo(v.eye, v.fovYDeg, v.viewportH);
    return v;
}

void registerLodCVars(x3::con::IConsole& console) {
    // ---- DISCRETE MESH LOD (Lane 5) ---------------------------------------
    // r_meshlod 0 forces LOD0 everywhere — today's behaviour, and the behaviour
    // of every mesh that has no chain regardless of the setting. Default 1: no
    // mesh in the engine ships an LOD chain yet, so turning it on changes
    // nothing until content opts in by calling buildLodChain().
    console.registerCVar("r_meshlod",      "1",    "discrete mesh LOD (0 = always LOD0, today's behaviour)");
    console.registerCVar("r_meshlod_err",  "1.5",  "mesh LOD screen-space error budget in PIXELS (bigger = swap sooner)");
    console.registerCVar("r_meshlod_hyst", "0.15", "mesh LOD hysteresis dead band as a fraction of the error budget (0 = none, flicker at thresholds)");
}

void applyLodCVars(const x3::con::IConsole& console) {
    LodPolicy& p = lodPolicy();
    p.enabled    = console.getInt("r_meshlod") != 0;
    p.pixelError = console.getFloat("r_meshlod_err");
    p.hysteresis = console.getFloat("r_meshlod_hyst");
    // distanceOnly is deliberately NOT cvar-exposed: it exists only as the
    // negative control --test-geolod flips in-process to prove the assertion.
}

} // namespace x3::game
