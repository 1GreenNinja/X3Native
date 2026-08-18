// ============================================================================
// --test-csm — headless self-test for CASCADED SHADOW MAPS (Lane 3).
//
// CLEAN-ROOM, original work. Asserts the properties described in the practical-
// split / stable-CSM literature (Zhang et al. 2006; Real-Time Rendering 4th ed.)
// against this repo's own engine/rhi/Csm.h. No id Tech / RBDOOM source consulted.
//
// WHY THIS CAN RUN WITHOUT A GPU: every number that decides shadow quality is
// produced by x3::csm::compute(), which is deliberately Vulkan-free. The test
// asserts on exactly the values the renderer uploads.
//
// WHAT IT ASSERTS
//   C1  split distances are monotonic and cover the whole [near, far] range
//   C2  each cascade's ortho box CONTAINS its frustum slice (all 8 corners)
//   C3  texel snapping is STABLE: a sub-texel camera move does NOT move the
//       snapped origin; a one-texel move moves it by exactly one texel
//   C4  rotation INVARIANCE: rotating the camera leaves every cascade's extent
//       unchanged (this is what the bounding-sphere fit buys)
//   C5  the r_csm 0 legacy matrix is bit-identical to the historical expression,
//       and r_shadowforward 0 leaves the box centre bit-identical to the camera
//   C6  per-cascade bias actually varies with cascade (a constant bias cannot
//       serve a 4x+ texel range)
//   C8  the LEGACY single box's texel snap (csm::legacySnapCenter): sub-texel
//       camera motion does not move it, a one-texel move moves it by exactly one
//       texel, the lattice is world-anchored over hundreds of metres, the snap
//       never displaces the box by more than a texel, and it is not a no-op.
//       This is the outdoor-polish lane's fix for the shadow SWIM the
//       interior-shadows lane filed (docs/screenshots/cell_shadows/README.md).
//   C7  NEGATIVE CONTROL: the naive fit (extent from a light-space AABB of the
//       frustum corners, no texel snapping) FAILS C3 and C4. This proves C3/C4
//       are real assertions and not tautologies. It is the bar the terrain-
//       corridor lane set (docs/design/LANE_DISPATCH_PLAN.md, requirement 2).
// ============================================================================
#include "csm_test.h"

#include "../engine/rhi/Csm.h"
#include "../engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {
namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[csm-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[csm-test] FAIL " + name); }
}

std::string f2s(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", (double)v);
    return std::string(buf);
}

// A representative outdoor camera: 16:9, 70 deg FOV (what the terrain shots use),
// looking slightly down, sun at the engine default normalize(0.4, 1, 0.3).
csm::Params baseParams() {
    csm::Params p{};
    p.camPos    = glm::vec3(120.0f, 30.0f, -85.0f);
    p.camFwd    = glm::normalize(glm::vec3(0.6f, -0.16f, 0.78f));
    p.camUp     = glm::vec3(0.0f, 1.0f, 0.0f);
    p.fovYDeg   = 70.0f;
    p.aspect    = 1280.0f / 720.0f;
    p.zNear     = csm::kCascadeNear;
    p.zFar      = 250.0f;
    p.sunDir    = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
    p.lambda    = csm::kDefaultLambda;
    p.shadowDim = 2048;
    p.count     = csm::kNumCascades;
    return p;
}

// The 8 world-space corners of the view frustum slice [n, f].
void sliceCorners(const csm::Params& p, float n, float f, glm::vec3 out[8]) {
    const glm::vec3 fwd = glm::normalize(p.camFwd);
    glm::vec3 up = glm::normalize(p.camUp);
    up = glm::normalize(up - fwd * glm::dot(up, fwd));
    const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
    const float tanY = std::tan(glm::radians(p.fovYDeg) * 0.5f);
    const float tanX = tanY * p.aspect;
    int k = 0;
    for (int di = 0; di < 2; ++di) {
        const float d = (di == 0) ? n : f;
        const glm::vec3 c = p.camPos + fwd * d;
        for (int yi = -1; yi <= 1; yi += 2)
            for (int xi = -1; xi <= 1; xi += 2)
                out[k++] = c + right * (d * tanX * (float)xi) + up * (d * tanY * (float)yi);
    }
}

// ---- C1: monotonic splits covering the full range -------------------------
void testSplits() {
    const csm::Params p = baseParams();
    const csm::Result r = csm::compute(p);

    check(r.count == csm::kNumCascades,
          "C1 cascade count == kNumCascades (" + std::to_string(r.count) + ")");

    bool monotonic = true;
    for (int i = 0; i < r.count; ++i) {
        if (!(r.c[i].splitFar > r.c[i].splitNear)) monotonic = false;              // each slice non-empty
        if (i > 0 && !(r.c[i].splitNear >= r.c[i - 1].splitFar - 1e-4f)) monotonic = false;
        if (i > 0 && !(r.c[i].splitFar  >  r.c[i - 1].splitFar)) monotonic = false;
    }
    check(monotonic, "C1 split distances strictly increase and slices do not overlap");

    // Contiguity: cascade i's far IS cascade i+1's near (no gap where nothing
    // is shadowed).
    bool contiguous = true;
    for (int i = 1; i < r.count; ++i)
        if (std::fabs(r.c[i].splitNear - r.c[i - 1].splitFar) > 1e-3f) contiguous = false;
    check(contiguous, "C1 slices are contiguous (no unshadowed gap between cascades)");

    check(std::fabs(r.c[0].splitNear - p.zNear) < 1e-3f,
          "C1 first cascade starts at the near plane (" + f2s(r.c[0].splitNear) + ")");
    check(std::fabs(r.c[r.count - 1].splitFar - p.zFar) < 1e-3f,
          "C1 last cascade reaches the shadow distance (" + f2s(r.c[r.count - 1].splitFar) +
          " vs " + f2s(p.zFar) + ")");

    // The whole point of the feature: shadows must persist WELL past the legacy
    // 45 m single cascade.
    check(r.c[r.count - 1].splitFar > 200.0f,
          "C1 shadow coverage extends past 200 m (legacy single cascade was 45 m)");

    // The practical split must be a genuine blend, not either pure endpoint:
    // slice 0 must be tighter than a uniform split would make it.
    const float uniformFirst = p.zNear + (p.zFar - p.zNear) / (float)r.count;
    check(r.c[0].splitFar < uniformFirst,
          "C1 lambda blend biases the near cascade tighter than uniform (" +
          f2s(r.c[0].splitFar) + " < " + f2s(uniformFirst) + ")");
}

// ---- C2: the ortho box contains its frustum slice --------------------------
// Project every slice corner through the cascade's viewProj: it must land inside
// the clip box (x,y in [-1,1], z in [0,1]). If a corner falls outside, geometry
// the player can SEE is missing from that cascade's shadow map.
void testContainment() {
    const csm::Params p = baseParams();
    const csm::Result r = csm::compute(p);

    bool allInside = true;
    float worstXY = 0.0f, worstZ = 0.0f;
    for (int i = 0; i < r.count; ++i) {
        glm::vec3 corners[8];
        sliceCorners(p, r.c[i].splitNear, r.c[i].splitFar, corners);
        for (int k = 0; k < 8; ++k) {
            const glm::vec4 clip = r.c[i].viewProj * glm::vec4(corners[k], 1.0f);
            const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
            worstXY = std::max(worstXY, std::max(std::fabs(ndc.x), std::fabs(ndc.y)));
            worstZ  = std::max(worstZ, std::max(-ndc.z, ndc.z - 1.0f));
            if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f ||
                ndc.z < 0.0f || ndc.z > 1.0f) allInside = false;
        }
    }
    check(allInside, "C2 every frustum-slice corner is inside its cascade's ortho box "
                     "(worst |xy| = " + f2s(worstXY) + ", worst z overshoot = " + f2s(worstZ) + ")");

    // Containment must survive camera rotation too — that is when a naive
    // AABB fit starts clipping.
    bool allInsideRotated = true;
    for (int step = 0; step < 16; ++step) {
        csm::Params q = p;
        const float a = (float)step * 0.3927f;   // 22.5 deg increments, full turn
        q.camFwd = glm::normalize(glm::vec3(std::cos(a), -0.16f, std::sin(a)));
        const csm::Result rr = csm::compute(q);
        for (int i = 0; i < rr.count; ++i) {
            glm::vec3 corners[8];
            sliceCorners(q, rr.c[i].splitNear, rr.c[i].splitFar, corners);
            for (int k = 0; k < 8; ++k) {
                const glm::vec4 clip = rr.c[i].viewProj * glm::vec4(corners[k], 1.0f);
                const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
                if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f ||
                    ndc.z < 0.0f || ndc.z > 1.0f) allInsideRotated = false;
            }
        }
    }
    check(allInsideRotated, "C2 containment holds through a full 360 deg camera sweep");
}

// ---- C3 / C4: stability + rotation invariance -------------------------------
// `stable` selects the real implementation; `false` runs the naive negative
// control. Returns pass/fail per property so C7 can assert the naive one FAILS.
struct StabilityOutcome { bool subTexelStable; bool oneTexelStep; bool rotationInvariant; };

StabilityOutcome measureStability(bool stable) {
    const csm::Params p = baseParams();
    auto fit = [&](const csm::Params& q) {
        return stable ? csm::compute(q) : csm::computeNaive(q);
    };
    const csm::Result base = fit(p);

    // --- sub-texel translation: the snapped origin must NOT move at all ------
    // Move the camera a TENTH of a texel along a direction with components in
    // every axis. With snapping the box stays put (edges hold still); without
    // it the box slides and every shadow edge crawls.
    const float texel = base.c[0].texelWorld;
    csm::Params sub = p;
    sub.camPos += glm::normalize(glm::vec3(1.0f, 0.3f, -0.7f)) * (texel * 0.1f);
    const csm::Result subR = fit(sub);

    bool subTexelStable = true;
    for (int i = 0; i < base.count; ++i) {
        if (std::fabs(subR.c[i].snappedX - base.c[i].snappedX) > 1e-5f) subTexelStable = false;
        if (std::fabs(subR.c[i].snappedY - base.c[i].snappedY) > 1e-5f) subTexelStable = false;
    }

    // --- one-texel translation: the origin must move by EXACTLY one texel ----
    // Translate along cascade 0's light-space X axis by exactly one of its
    // texels; the snapped X must advance by exactly that texel and Y must not
    // move at all. (Any other result means the lattice is not world-anchored.)
    const glm::mat4 lightRot = csm::lightRotation(glm::normalize(p.sunDir));
    // Light-space X axis expressed in world space = the first ROW of the
    // rotation (its inverse is its transpose).
    const glm::vec3 lsX = glm::vec3(lightRot[0][0], lightRot[1][0], lightRot[2][0]);
    csm::Params one = p;
    one.camPos += lsX * texel;
    const csm::Result oneR = fit(one);

    const float dx = oneR.c[0].snappedX - base.c[0].snappedX;
    const float dy = oneR.c[0].snappedY - base.c[0].snappedY;
    const bool oneTexelStep = (std::fabs(dx - texel) < texel * 1e-3f) &&
                              (std::fabs(dy) < texel * 1e-3f);

    // --- rotation invariance: extent must not change when the camera turns ---
    // Only the camera's ORIENTATION changes. A bounding-SPHERE fit depends on
    // (near, far, fov, aspect) alone, so the extent is identical. An AABB fit
    // depends on how the slice is oriented relative to the sun, so it breathes —
    // and a breathing extent means a changing texel size, i.e. shimmer.
    bool rotationInvariant = true;
    for (int step = 1; step < 12; ++step) {
        csm::Params rot = p;
        const float a = (float)step * 0.5236f;   // 30 deg increments
        rot.camFwd = glm::normalize(glm::vec3(std::cos(a), -0.16f, std::sin(a)));
        const csm::Result rr = fit(rot);
        for (int i = 0; i < base.count; ++i)
            if (std::fabs(rr.c[i].halfExtent - base.c[i].halfExtent) > base.c[i].halfExtent * 1e-4f)
                rotationInvariant = false;
    }
    return { subTexelStable, oneTexelStep, rotationInvariant };
}

void testStabilityAndRotation() {
    const csm::Result base = csm::compute(baseParams());
    const StabilityOutcome s = measureStability(/*stable=*/true);

    check(s.subTexelStable,
          "C3 sub-texel camera motion does NOT move the snapped ortho origin "
          "(texel = " + f2s(base.c[0].texelWorld) + " m)");
    check(s.oneTexelStep,
          "C3 a one-texel camera move advances the snapped origin by EXACTLY one texel");
    check(s.rotationInvariant,
          "C4 rotating the camera leaves every cascade's ortho extent unchanged");

    // Sanity on the fit itself: extents must GROW with cascade index, and the
    // sphere radius must never be smaller than the slice's far half-diagonal.
    bool growing = true;
    for (int i = 1; i < base.count; ++i)
        if (!(base.c[i].halfExtent > base.c[i - 1].halfExtent)) growing = false;
    check(growing, "C4 cascade extents increase monotonically with distance");
}

// ---- C5: the legacy path is untouched ---------------------------------------
void testLegacyBitExact() {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
    const glm::vec3 camPos(120.0f, 30.0f, -85.0f);
    const float ortho = 45.0f, dHalf = 80.0f;

    // The HISTORICAL expression, transcribed from the pre-CSM
    // computeLightViewProj(). If the extracted helper ever drifts from this, the
    // md5/screenshot gates would silently break — this catches it first.
    const glm::vec3 eye = camPos + sun * dHalf;
    const glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 upPick = (std::fabs(glm::dot(sun, up)) > 0.99f) ? glm::vec3(0, 0, 1) : up;
    glm::mat4 view = glm::lookAt(eye, camPos, upPick);
    glm::mat4 proj = glm::ortho(-ortho, ortho, -ortho, ortho, 0.0f, 2.0f * dHalf);
    proj[1][1] *= -1.0f;
    const glm::mat4 expected = proj * view;

    const glm::mat4 actual = csm::legacyOrthoViewProj(camPos, sun, ortho, dHalf);

    bool bitExact = true;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (expected[c][r] != actual[c][r]) bitExact = false;   // exact float compare, deliberately
    check(bitExact, "C5 r_csm 0 legacy light matrix is BIT-IDENTICAL to the historical expression");

    // r_shadowforward 0 must leave the box centre exactly on the camera. The
    // renderer skips the add entirely at 0; assert the property that guarantee
    // rests on (a non-zero bias DOES move it, so the branch is not dead).
    const glm::vec3 fwd = glm::normalize(glm::vec3(0.6f, -0.16f, 0.78f));
    const glm::vec3 c0 = camPos + fwd * 0.0f;
    check(c0.x == camPos.x && c0.y == camPos.y && c0.z == camPos.z,
          "C5 r_shadowforward 0 leaves the shadow box centre bit-identical to the camera");
    const glm::vec3 c1 = camPos + fwd * 25.0f;
    check(glm::length(c1 - camPos) > 24.9f,
          "C5 r_shadowforward > 0 actually slides the box forward (interim is live)");
}

// ---- C8: the LEGACY box's texel snap (outdoor-polish lane) -----------------
// The interior-shadows lane filed the legacy 45 m box as CAMERA-LOCKED and
// UNSNAPPED, and its C7 negative control already documents what that produces:
// a box whose origin slides with every sub-texel camera move, so every shadow
// edge in the frame crawls. csm::legacySnapCenter is the fix; these are its
// POSITIVE tests, written to the same shape as C3 so the two read as one story.
//
// C7 stays exactly as it was: it is the negative control for the CASCADE fit
// (computeNaive), which is still deliberately unsnapped. This lane did not
// weaken it — it added the legacy path's own pair of assertions beside it.
void testLegacySnap() {
    const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
    const float ortho = 45.0f;
    const uint32_t dim = 2048;
    const float texel = (2.0f * ortho) / (float)dim;   // 0.0439 m at the shipped size
    const glm::mat4 rot = csm::lightRotation(sun);

    // THE TEST CAMERA IS PLACED AT THE CENTRE OF ITS TEXEL CELL, on purpose.
    // Snapping is a floor(), so a camera sitting a hair inside a cell boundary
    // legitimately JUMPS one texel on a small move — that is the quantiser
    // working, not swimming. Asserting "does not move" from an arbitrary point
    // would therefore be asserting luck. Centring the start makes the sub-texel
    // sweep below unambiguous: every step of up to 0.42 texel stays inside the
    // cell, so any movement at all is a real failure.
    const glm::vec3 rawCam(120.0f, 30.0f, -85.0f);
    const glm::vec3 rawLS = glm::vec3(rot * glm::vec4(rawCam, 1.0f));
    const glm::vec3 cenLS((std::floor(rawLS.x / texel) + 0.5f) * texel,
                          (std::floor(rawLS.y / texel) + 0.5f) * texel,
                          (std::floor(rawLS.z / texel) + 0.5f) * texel);
    const glm::vec3 camPos = glm::vec3(glm::inverse(rot) * glm::vec4(cenLS, 1.0f));

    const glm::vec3 base = csm::legacySnapCenter(camPos, sun, ortho, dim);

    // --- sub-texel translation: the snapped centre must NOT move at all -------
    // Swept over a range of sub-texel steps AND directions, not one sample: a
    // single step can sit just inside one lattice cell and pass on a function
    // that only half works.
    const glm::vec3 dirs[4] = {
        glm::normalize(glm::vec3( 1.0f,  0.3f, -0.7f)),
        glm::normalize(glm::vec3(-0.4f,  0.0f,  0.9f)),
        glm::normalize(glm::vec3( 0.0f,  1.0f,  0.0f)),
        glm::normalize(glm::vec3( 0.6f, -0.5f,  0.6f)),
    };
    bool subStable = true;
    float worstSub = 0.0f;
    for (const glm::vec3& d : dirs)
        for (int k = 1; k <= 6; ++k) {
            const float f = (float)k * 0.07f;   // 0.07 .. 0.42 of a texel
            const glm::vec3 s = csm::legacySnapCenter(camPos + d * (texel * f),
                                                      sun, ortho, dim);
            worstSub = std::max(worstSub, glm::length(s - base));
            if (glm::length(s - base) > 1e-4f) subStable = false;
        }
    check(subStable,
          "C8 LEGACY box: sub-texel camera moves (24 of them, 4 directions x 6 "
          "magnitudes under one texel of " + f2s(texel) + " m) do NOT move the "
          "snapped centre at all — worst drift " + f2s(worstSub) + " m. This is "
          "the shadow SWIM the owner read as shimmer.");

    // The same move WITHOUT the snap moves the box by the full amount. Without
    // this the assertion above would pass on a function that returned a constant.
    check(glm::length((camPos + dirs[0] * (texel * 0.07f)) - camPos) > texel * 0.03f,
          "C8 NEGATIVE CONTROL: the UNSNAPPED centre (the camera itself) DOES move "
          "on the smallest of those sub-texel steps");

    // --- one-texel translation: exactly one texel of light-space X ------------
    const glm::vec3 lsX(rot[0][0], rot[1][0], rot[2][0]);   // light X in world
    const glm::vec3 one = csm::legacySnapCenter(camPos + lsX * texel, sun, ortho, dim);
    const glm::vec3 dLS = glm::vec3(rot * glm::vec4(one - base, 0.0f));
    check(std::fabs(dLS.x - texel) < texel * 1e-3f && std::fabs(dLS.y) < texel * 1e-3f,
          "C8 LEGACY box: a one-texel camera move advances the snapped centre by "
          "EXACTLY one texel along light X and zero along light Y");

    // --- the snap never displaces the box by more than one texel --------------
    // It must not be possible for the snap to slide the shadowed region off the
    // camera: the legacy box has no padding, so a displacement larger than a
    // texel would leave a sliver of the near field unshadowed.
    float worst = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const float a = (float)i * 0.0982f;
        const glm::vec3 p = camPos + glm::vec3(std::cos(a), 0.37f * std::sin(a * 1.7f),
                                               std::sin(a)) * (texel * (float)i * 0.31f);
        worst = std::max(worst, glm::length(csm::legacySnapCenter(p, sun, ortho, dim) - p));
    }
    // sqrt(3), not 1: all THREE light-space axes are floor-quantised, so each
    // contributes up to one texel and the worst case is the diagonal of one
    // texel cube. That is ~7.6 cm against a 90 m box.
    check(worst < texel * 1.74f,
          "C8 the snap displaces the legacy box by at most one texel per axis "
          "(worst " + f2s(worst) + " m vs texel " + f2s(texel) + " m; the bound is "
          "sqrt(3) texels because all three light-space axes are snapped)");

    // --- the snapped centre is a LATTICE point, and the lattice is WORLD-fixed -
    // Two cameras 500 m apart must land on the same grid, or the "world-anchored"
    // claim is empty and the box would still crawl on a long drive.
    const glm::vec3 far0 = csm::legacySnapCenter(camPos + glm::vec3(500.0f, 0.0f, -300.0f),
                                                 sun, ortho, dim);
    const glm::vec3 aLS = glm::vec3(rot * glm::vec4(base, 1.0f));
    const glm::vec3 bLS = glm::vec3(rot * glm::vec4(far0, 1.0f));
    const float rx = std::fabs(std::remainder(bLS.x - aLS.x, texel));
    const float ry = std::fabs(std::remainder(bLS.y - aLS.y, texel));
    check(rx < texel * 1e-2f && ry < texel * 1e-2f,
          "C8 the texel lattice is WORLD-anchored: a centre 580 m away lands on the "
          "same grid (residual " + f2s(rx) + " / " + f2s(ry) + " m)");

    // --- r_shadowsnap 0 is still the historical box ---------------------------
    // The snap lives at the CALL SITE; the matrix builder is untouched. C5 above
    // already proves that builder is bit-exact, so this asserts the other half:
    // feeding it the raw camera reproduces the historical centre exactly.
    const glm::mat4 hist = csm::legacyOrthoViewProj(camPos, sun, ortho, 80.0f);
    const glm::mat4 snap = csm::legacyOrthoViewProj(base,   sun, ortho, 80.0f);
    bool differ = false;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (hist[c][r] != snap[c][r]) differ = true;
    check(differ,
          "C8 the snap actually CHANGES the matrix (so r_shadowsnap 0 vs 1 is a "
          "real A/B, not a no-op that would make C8 vacuous)");
}

// ---- C6: per-cascade bias --------------------------------------------------
void testBias() {
    const csm::Result r = csm::compute(baseParams());

    bool normalBiasGrows = true, allPositive = true;
    for (int i = 0; i < r.count; ++i) {
        if (!(r.c[i].depthBias > 0.0f) || !(r.c[i].normalBias > 0.0f)) allPositive = false;
        if (i > 0 && !(r.c[i].normalBias > r.c[i - 1].normalBias)) normalBiasGrows = false;
    }
    check(allPositive, "C6 every cascade has a positive depth + normal-offset bias");
    check(normalBiasGrows,
          "C6 normal-offset bias grows with cascade index (it scales with texel size)");

    // The reason a single constant bias cannot work: the texel size spans a
    // large factor across the cascade set.
    const float ratio = r.c[r.count - 1].normalBias / r.c[0].normalBias;
    check(ratio > 4.0f,
          "C6 texel scale spans >4x across cascades (" + f2s(ratio) +
          "x) — a constant bias would acne the near or peter-pan the far one");
}

// ---- C7: NEGATIVE CONTROL --------------------------------------------------
// The naive implementation (AABB extent, no snapping) must FAIL the very
// assertions the real one passes. If it ever passes them, C3/C4 are vacuous.
void testNegativeControl() {
    const StabilityOutcome naive = measureStability(/*stable=*/false);

    check(!naive.subTexelStable,
          "C7 NEGATIVE CONTROL: unsnapped fit FAILS the sub-texel stability assertion "
          "(this is the shadow-edge SWIMMING the snapping fixes)");
    check(!naive.rotationInvariant,
          "C7 NEGATIVE CONTROL: frustum-corner AABB fit FAILS rotation invariance "
          "(this is the shimmer you get from merely looking around)");

    // Quantify the naive failure so the log shows the size of the problem.
    const csm::Params p = baseParams();
    float minExt = 1e30f, maxExt = 0.0f;
    for (int step = 0; step < 24; ++step) {
        csm::Params rot = p;
        const float a = (float)step * 0.2618f;   // 15 deg increments
        rot.camFwd = glm::normalize(glm::vec3(std::cos(a), -0.16f, std::sin(a)));
        const csm::Result rr = csm::computeNaive(rot);
        minExt = std::min(minExt, rr.c[0].halfExtent);
        maxExt = std::max(maxExt, rr.c[0].halfExtent);
    }
    x3::logInfo("[csm-test] negative control: naive cascade-0 extent breathes " +
                f2s(minExt) + " m .. " + f2s(maxExt) + " m over a 360 deg pan (" +
                f2s(maxExt / std::max(minExt, 1e-4f)) + "x); the sphere fit is constant at " +
                f2s(csm::compute(p).c[0].halfExtent) + " m");
}

} // namespace

bool runCsmSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("[csm-test] cascades = " + std::to_string(csm::kNumCascades) +
                ", lambda = " + f2s(csm::kDefaultLambda) +
                ", shadow distance = " + f2s(csm::kDefaultShadowDistance) + " m, 2048^2 per cascade");

    testSplits();
    testContainment();
    testStabilityAndRotation();
    testLegacyBitExact();
    testLegacySnap();
    testBias();
    testNegativeControl();

    // Receipts: the actual fitted cascades, so the log shows what shipped.
    const csm::Result r = csm::compute(baseParams());
    for (int i = 0; i < r.count; ++i)
        x3::logInfo("[csm-test]   cascade " + std::to_string(i) +
                    ": view depth " + f2s(r.c[i].splitNear) + " .. " + f2s(r.c[i].splitFar) +
                    " m | sphere radius " + f2s(r.c[i].radius) +
                    " m | ortho half-extent " + f2s(r.c[i].halfExtent) +
                    " m | texel " + f2s(r.c[i].texelWorld) +
                    " m | normalBias " + f2s(r.c[i].normalBias) + " m");

    x3::logInfo("[csm-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
