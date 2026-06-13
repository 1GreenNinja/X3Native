#pragma once
// FrustumCull.h — CPU per-object frustum culling utility (D15 reference baseline).
//
// WHY: VulkanRenderDevice's draw path counted EVERY instance unconditionally; the
// only cull was per-ROOM PVS (wrong granularity). This adds a conservative per-object
// world-space bounding-SPHERE vs frustum test on the CPU. It is gated by the
// `r_frustumcull` cvar (default ON) and is the REFERENCE BASELINE the future GPU
// Tier-0 cull (shaders/cull.comp) must match: its acceptance test is
// "GPU statDrawn == CPU objectsDrawn".
//
// GPU-EQUIVALENCE CONTRACT — this MUST stay bit-compatible with cull.comp:
//   * Planes: 6 world-space planes, each NORMALIZED (xyz = unit normal, w = d).
//   * Sphere: xyz = world-space center, w = radius (already scaled to world).
//   * Predicate (cull.comp frustumVisible): a sphere is OUTSIDE (cull) iff it is
//     fully behind ANY plane:  dot(plane.xyz, center) + plane.w < -radius.
//     i.e. VISIBLE (keep) iff  dot(n, c) + d >= -r  for ALL 6 planes.
//   * ALWAYS_VISIBLE: never cull (sky / skinned / fullscreen items).
//
// Header-only so the live device AND the --test-frustumcull self-test exercise the
// EXACT SAME math (no chance of the baseline and the test drifting apart).

#include <glm/glm.hpp>
#include <array>

namespace x3::rhi {

// A world-space bounding sphere: xyz = center, w = radius.
using CullSphere = glm::vec4;

// The 6 frustum planes (normalized). Order: L, R, B, T, N, F — order is irrelevant
// to the test (all 6 are ANDed), and matches the count cull.comp loops over.
struct FrustumPlanes {
    std::array<glm::vec4, 6> p;   // xyz = unit normal, w = d
};

// Gribb-Hartmann: extract the 6 frustum planes directly from a combined
// view-projection matrix, then NORMALIZE each (so plane.w is a true signed
// distance and the sphere-radius compare in frustumVisible is exact — this is
// what makes the CPU result match the GPU, whose CullParams.frustum is normalized).
//
// glm is COLUMN-major: vp[col][row]. Rows of the matrix are vp[*][row]. We build
// each plane from a row combination per the standard Gribb-Hartmann derivation for
// a clip volume of w-(-w) on each axis. Works for GL- and reverse-Y/Vulkan-style
// projections alike because the planes are derived from the matrix itself.
inline FrustumPlanes extractFrustumPlanes(const glm::mat4& vp) {
    // Pull the four rows out of the column-major matrix.
    const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

    FrustumPlanes f;
    f.p[0] = r3 + r0;   // left   ( w + x >= 0 )
    f.p[1] = r3 - r0;   // right  ( w - x >= 0 )
    f.p[2] = r3 + r1;   // bottom ( w + y >= 0 )
    f.p[3] = r3 - r1;   // top    ( w - y >= 0 )
    f.p[4] = r3 + r2;   // near   ( w + z >= 0 )
    f.p[5] = r3 - r2;   // far    ( w - z >= 0 )

    for (auto& pl : f.p) {
        const float len = glm::length(glm::vec3(pl));
        if (len > 0.0f) pl /= len;   // normalize plane: unit normal + true distance
    }
    return f;
}

// Conservative sphere-vs-frustum. Returns true if the sphere is at least partially
// inside the frustum (KEEP). Identical predicate to cull.comp's frustumVisible:
//   cull  <=>  dot(n, c) + d < -r  for ANY plane.
// A sphere STRADDLING a plane (|signed dist| < r) is kept (conservative). Mirrors
// the GPU exactly (same loop, same compare) for the D15 equivalence check.
inline bool sphereInFrustum(const FrustumPlanes& f, const CullSphere& s) {
    const glm::vec3 c(s);
    const float r = s.w;
    for (int i = 0; i < 6; ++i) {
        if (glm::dot(glm::vec3(f.p[i]), c) + f.p[i].w < -r) return false;
    }
    return true;
}

} // namespace x3::rhi
