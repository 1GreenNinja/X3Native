// ============================================================================
// --test-refldenoise — headless self-test for the REFLECTION DENOISE filter.
//
// CLEAN-ROOM, original work. Asserts the properties an edge-aware a-trous
// denoiser must have (Dammertz et al. HPG 2010; Schied et al. HPG 2017) against
// this repo's own engine/rhi/ReflDenoise.h. No GPL / id Tech / RBDOOM / Unreal
// source consulted.
//
// WHY THIS CAN RUN WITHOUT A GPU: engine/rhi/ReflDenoise.cpp is deliberately
// Vulkan-free and IS the definition of the filter;
// shaders/refl_denoise.comp is a transcription of it. The test asserts on the
// filter; the screenshots are the gate on the shader.
//
// WHAT IT ASSERTS
//   D1  OFF IS IDENTITY. iterations = 0 reproduces the input bit-for-bit. This
//       is the r_refldenoise 0 contract that keeps md5 gates holding.
//   D2  EDGE PRESERVATION ACROSS A DEPTH DISCONTINUITY. Two surfaces at very
//       different view distances carrying very different reflected radiance:
//       neither side may pick up the other. This is the SECOND reported defect
//       — reflection bleeding past the car's lower silhouette onto the floor.
//   D3  EDGE PRESERVATION ACROSS A NORMAL DISCONTINUITY at CONSTANT depth (a
//       crease). The depth stop is blind to this; the normal stop is not.
//   D4  ENERGY CONSERVATION ON A FLAT REGION. On one continuous surface with
//       uniform confidence the filter must preserve the MEAN radiance (it is a
//       normalised weighted average, not a gain) while collapsing the variance.
//   D5  CONFIDENCE-WEIGHTED (PREMULTIPLIED) RECONSTRUCTION. Half the pixels have
//       confidence 0 and therefore BLACK stored colour — refl.comp's binary
//       hit/miss, which is the measured blotch. Filtering must recover the
//       confident colour, NOT half of it. This is the property that separates
//       this filter from mesh.frag's consumer disc, which averages rgb and a
//       independently and therefore darkens.
//   D6  SKY / INVALID PASS-THROUGH. Texels with no valid surface are untouched
//       and never contribute to a neighbour.
//   D7  NEGATIVE CONTROL, PERMANENT. The identical loop with the depth and
//       normal edge stops REMOVED (a plain 5x5 a-trous box) must FAIL D2 and D3.
//       If it ever passes them, D2/D3 are tautologies and this test fails —
//       which is the bar docs/design/LANE_DISPATCH_PLAN.md requirement 2 sets.
// ============================================================================
#include "refl_denoise_test.h"

#include "../engine/rhi/ReflDenoise.h"
#include "../engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {
namespace {

using x3::refldn::Aux;
using x3::refldn::Params;
using x3::refldn::Pixel;

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[refldn-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[refldn-test] FAIL " + name); }
}

std::string f2s(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return std::string(buf);
}

constexpr int kW = 64, kH = 64;
constexpr int kN = kW * kH;

// A deterministic hash-noise source so the "noisy" cases are reproducible and
// nobody has to reason about a PRNG's distribution.
float hashNoise(int x, int y, uint32_t salt) {
    uint32_t s = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ salt * 83492791u;
    s ^= s >> 13; s *= 0x5bd1e995u; s ^= s >> 15;
    return (float)(s & 0xFFFFu) / 65535.0f;   // [0,1]
}

struct Scene {
    std::vector<Pixel> src;
    std::vector<Aux>   aux;
    Scene() : src(kN), aux(kN) {}
};

// Mean of a channel over an inclusive x-range, excluding a margin so the
// clamp-to-edge boundary never colours the result.
double meanR(const std::vector<Pixel>& img, int x0, int x1, int y0, int y1) {
    double s = 0.0; int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) { s += img[(size_t)y * kW + x].rgb[0]; ++n; }
    return n ? s / n : 0.0;
}

double stddevR(const std::vector<Pixel>& img, int x0, int x1, int y0, int y1) {
    const double m = meanR(img, x0, x1, y0, y1);
    double s = 0.0; int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double d = img[(size_t)y * kW + x].rgb[0] - m; s += d * d; ++n;
        }
    return n ? std::sqrt(s / n) : 0.0;
}

// ---------------------------------------------------------------------------
// D2's scene: a vertical DEPTH discontinuity down the middle. Left half is a
// near surface (5 m) reflecting a dim environment; right half is a far surface
// (40 m) reflecting a very bright one. Normals are IDENTICAL on both sides, so
// only the depth stop can save the edge — that is deliberate.
// ---------------------------------------------------------------------------
Scene depthEdgeScene() {
    Scene s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = (size_t)y * kW + x;
            const bool near = x < kW / 2;
            s.aux[i].n[0] = 0.0f; s.aux[i].n[1] = 0.0f; s.aux[i].n[2] = 1.0f;
            s.aux[i].dist = near ? 5.0f : 40.0f;
            const float v = near ? 1.0f : 20.0f;
            s.src[i].rgb[0] = s.src[i].rgb[1] = s.src[i].rgb[2] = v;
            s.src[i].conf = 1.0f;
        }
    return s;
}

// ---------------------------------------------------------------------------
// D3's scene: a CREASE. Constant view distance everywhere (the depth stop sees
// nothing at all), but the surface normal turns 90 degrees at the midline and
// the two faces reflect very different radiance.
// ---------------------------------------------------------------------------
Scene normalEdgeScene() {
    Scene s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = (size_t)y * kW + x;
            const bool left = x < kW / 2;
            s.aux[i].n[0] = left ? 0.0f : 1.0f;
            s.aux[i].n[1] = 0.0f;
            s.aux[i].n[2] = left ? 1.0f : 0.0f;
            s.aux[i].dist = 12.0f;                 // IDENTICAL depth on both sides
            const float v = left ? 1.0f : 20.0f;
            s.src[i].rgb[0] = s.src[i].rgb[1] = s.src[i].rgb[2] = v;
            s.src[i].conf = 1.0f;
        }
    return s;
}

// ---------------------------------------------------------------------------
// D4's scene: ONE continuous flat surface carrying noisy radiance with a known
// mean. This is the "flat door skin" the shipped metric is measured on.
// ---------------------------------------------------------------------------
Scene flatNoisyScene(float mean, float amp) {
    Scene s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = (size_t)y * kW + x;
            s.aux[i].n[0] = 0.0f; s.aux[i].n[1] = 0.0f; s.aux[i].n[2] = 1.0f;
            s.aux[i].dist = 8.0f;
            const float v = mean + amp * (hashNoise(x, y, 7u) - 0.5f) * 2.0f;
            s.src[i].rgb[0] = s.src[i].rgb[1] = s.src[i].rgb[2] = v;
            s.src[i].conf = 1.0f;
        }
    return s;
}

// ---------------------------------------------------------------------------
// D5's scene: refl.comp's BINARY hit/miss on one continuous flat surface. Half
// the texels (a checker, so the geometry is uniform and only confidence varies)
// carry conf 1 and colour C; the rest carry conf 0 and BLACK, exactly as
// refl.comp writes a miss. The correct reconstruction of the colour is C.
// ---------------------------------------------------------------------------
Scene binaryConfidenceScene(float C) {
    Scene s;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = (size_t)y * kW + x;
            s.aux[i].n[0] = 0.0f; s.aux[i].n[1] = 0.0f; s.aux[i].n[2] = 1.0f;
            s.aux[i].dist = 8.0f;
            const bool hit = ((x + y) & 1) == 0;
            s.src[i].conf = hit ? 1.0f : 0.0f;
            const float v = hit ? C : 0.0f;
            s.src[i].rgb[0] = s.src[i].rgb[1] = s.src[i].rgb[2] = v;
        }
    return s;
}

// Run the chain and return the result.
std::vector<Pixel> run(const Scene& sc, const Params& p, bool edgeStops) {
    std::vector<Pixel> dst(kN), scratch(kN);
    x3::refldn::filter(sc.src.data(), sc.aux.data(), kW, kH, p, dst.data(), scratch.data(), edgeStops);
    return dst;
}

Params shipped() {
    Params p{};      // the shipped defaults: 3 iterations, sigma 0.06, pow 16
    return p;
}

// D2/D3 measured as a single number so the negative control can be held to the
// SAME yardstick: how much of the bright side leaked into the dim side, as a
// fraction of the contrast across the edge. 0 = perfect preservation.
//
// MEASURED IN THE 8 COLUMNS IMMEDIATELY LEFT OF THE DISCONTINUITY, not in the
// dim side's far interior. That distinction is the whole test: at 3 iterations
// the a-trous footprint reaches +-14 texels, so averaging over the far interior
// dilutes the leak below any threshold and the NEGATIVE CONTROL passes — which
// it did on the first run of this test, and which would have made D2/D3
// tautologies. Right at the edge is also where the defect actually presents:
// the reflection halo bleeding past the car's lower silhouette onto the floor.
double leakFraction(const std::vector<Pixel>& out, double dim, double bright) {
    const double m = meanR(out, kW / 2 - 8, kW / 2 - 1, 2, kH - 3);
    return (m - dim) / (bright - dim);
}

} // namespace

bool runReflDenoiseSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("[refldn-test] edge-aware a-trous reflection denoise "
                "(engine/rhi/ReflDenoise.cpp; shaders/refl_denoise.comp transcribes it)");

    // ---- D1: OFF IS IDENTITY (the r_refldenoise 0 bit-exactness contract) ----
    {
        Scene sc = flatNoisyScene(3.0f, 1.5f);
        Params p = shipped(); p.iterations = 0;
        std::vector<Pixel> out = run(sc, p, true);
        bool identical = true;
        for (int i = 0; i < kN && identical; ++i) {
            identical = out[i].conf == sc.src[i].conf
                     && out[i].rgb[0] == sc.src[i].rgb[0]
                     && out[i].rgb[1] == sc.src[i].rgb[1]
                     && out[i].rgb[2] == sc.src[i].rgb[2];
        }
        check(identical, "D1 iterations=0 reproduces the input BIT-FOR-BIT (r_refldenoise 0)");
    }

    // ---- D2: edge preservation across a DEPTH discontinuity ----------------
    // (defect two: reflection bleeding past the car's lower silhouette)
    double leakDepthReal = 0.0, leakDepthCtrl = 0.0;
    {
        Scene sc = depthEdgeScene();
        leakDepthReal = leakFraction(run(sc, shipped(), true), 1.0, 20.0);
        leakDepthCtrl = leakFraction(run(sc, shipped(), false), 1.0, 20.0);
        check(leakDepthReal < 0.01,
              "D2 depth edge: bright side leaks <1% into the dim side (leak=" +
              f2s(leakDepthReal * 100.0) + "%)");
    }

    // ---- D3: edge preservation across a NORMAL discontinuity ---------------
    double leakNrmReal = 0.0, leakNrmCtrl = 0.0;
    {
        Scene sc = normalEdgeScene();
        leakNrmReal = leakFraction(run(sc, shipped(), true), 1.0, 20.0);
        leakNrmCtrl = leakFraction(run(sc, shipped(), false), 1.0, 20.0);
        check(leakNrmReal < 0.01,
              "D3 normal crease at CONSTANT depth: leaks <1% (leak=" +
              f2s(leakNrmReal * 100.0) + "%)");
    }

    // ---- D4: energy conservation + variance collapse on a flat region -------
    {
        const float mean = 3.0f;
        Scene sc = flatNoisyScene(mean, 1.5f);
        std::vector<Pixel> out = run(sc, shipped(), true);
        const double mIn  = meanR(sc.src, 4, kW - 5, 4, kH - 5);
        const double mOut = meanR(out,    4, kW - 5, 4, kH - 5);
        const double sIn  = stddevR(sc.src, 4, kW - 5, 4, kH - 5);
        const double sOut = stddevR(out,    4, kW - 5, 4, kH - 5);
        check(std::fabs(mOut - mIn) / mIn < 0.01,
              "D4a flat region: MEAN radiance preserved within 1% (in=" + f2s(mIn) +
              " out=" + f2s(mOut) + ")");
        check(sOut < sIn * 0.30,
              "D4b flat region: high-frequency variation collapses >70% (sd " +
              f2s(sIn) + " -> " + f2s(sOut) + ")");
    }

    // ---- D5: premultiplied-confidence reconstruction ------------------------
    // The measured blotch IS this: refl.comp's hit/miss is binary, so a
    // confidence-blind average of rgb pulls the black misses in and darkens.
    {
        const float C = 4.0f;
        Scene sc = binaryConfidenceScene(C);
        std::vector<Pixel> out = run(sc, shipped(), true);
        const double mOut = meanR(out, 4, kW - 5, 4, kH - 5);
        check(std::fabs(mOut - C) / C < 0.01,
              "D5a binary confidence: colour reconstructs to the CONFIDENT value, "
              "not a darkened average (want " + f2s(C) + ", got " + f2s(mOut) + ")");
        // Confidence itself must average toward the local hit RATE (0.5 here) —
        // that is the smooth blend weight mesh.frag needs instead of a per-pixel
        // 0/1 flicker.
        double cs = 0.0; int cn = 0;
        for (int y = 4; y < kH - 4; ++y)
            for (int x = 4; x < kW - 4; ++x) { cs += out[(size_t)y * kW + x].conf; ++cn; }
        const double cMean = cs / cn;
        check(std::fabs(cMean - 0.5) < 0.02,
              "D5b binary confidence: confidence field converges to the hit RATE (want 0.5, got " +
              f2s(cMean) + ")");
        // And it must actually be SMOOTH now, not the 0/1 checker it started as.
        double maxJump = 0.0;
        for (int y = 6; y < kH - 6; ++y)
            for (int x = 6; x < kW - 6; ++x)
                maxJump = std::max(maxJump,
                    (double)std::fabs(out[(size_t)y * kW + x].conf - out[(size_t)y * kW + x + 1].conf));
        check(maxJump < 0.10,
              "D5c binary confidence: neighbouring confidences no longer jump 0<->1 (max jump " +
              f2s(maxJump) + ")");
    }

    // ---- D6: sky / invalid texels pass through untouched --------------------
    {
        Scene sc = flatNoisyScene(3.0f, 1.0f);
        // Punch a "sky" hole and give it a poison colour: it must survive
        // verbatim, and it must not bleed into its neighbours.
        const int hx = 32, hy = 32;
        const size_t hi = (size_t)hy * kW + hx;
        sc.aux[hi].dist = 0.0f;
        sc.src[hi].rgb[0] = sc.src[hi].rgb[1] = sc.src[hi].rgb[2] = 999.0f;
        sc.src[hi].conf = 1.0f;
        std::vector<Pixel> out = run(sc, shipped(), true);
        check(out[hi].rgb[0] == 999.0f && out[hi].conf == 1.0f,
              "D6a sky/invalid texel passes through verbatim");
        double worst = 0.0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                if (!dx && !dy) continue;
                worst = std::max(worst, (double)out[(size_t)(hy + dy) * kW + (hx + dx)].rgb[0]);
            }
        check(worst < 10.0,
              "D6b a poisoned invalid texel does NOT contaminate its neighbours (worst " +
              f2s(worst) + ")");
    }

    // ---- D7: NEGATIVE CONTROL (permanent) -----------------------------------
    // The SAME loop with the edge stops removed — a plain 5x5 a-trous box. It
    // must FAIL D2 and D3. This is what proves those two assertions have teeth:
    // without it, "the filter preserved the edge" could just mean "the filter
    // barely did anything".
    {
        check(leakDepthCtrl > 0.05,
              "D7a NEGATIVE CONTROL (no edge stops) FAILS the depth-edge assertion as required "
              "(leak=" + f2s(leakDepthCtrl * 100.0) + "% vs the real filter's " +
              f2s(leakDepthReal * 100.0) + "%)");
        check(leakNrmCtrl > 0.05,
              "D7b NEGATIVE CONTROL (no edge stops) FAILS the normal-crease assertion as required "
              "(leak=" + f2s(leakNrmCtrl * 100.0) + "% vs the real filter's " +
              f2s(leakNrmReal * 100.0) + "%)");
        // ...and it must still be a *working blur*, so the failure above is
        // specifically the missing edge stops and not a broken control.
        Scene sc = flatNoisyScene(3.0f, 1.5f);
        const double sIn  = stddevR(sc.src, 4, kW - 5, 4, kH - 5);
        const double sOut = stddevR(run(sc, shipped(), false), 4, kW - 5, 4, kH - 5);
        check(sOut < sIn * 0.30,
              "D7c the control is a WORKING blur on a flat region (sd " + f2s(sIn) +
              " -> " + f2s(sOut) + ") — its D7a/D7b failure is the edge stops, nothing else");
    }

    // ---- Reach check: the a-trous dilation must actually widen the support --
    // 3 iterations reach +-14 texels for 75 taps; a non-dilating filter would
    // reach +-2. Verify by how far a single confident texel's influence spreads
    // on an otherwise zero-confidence flat surface.
    {
        Scene sc;
        for (int i = 0; i < kN; ++i) {
            sc.aux[i].n[0] = 0.0f; sc.aux[i].n[1] = 0.0f; sc.aux[i].n[2] = 1.0f;
            sc.aux[i].dist = 8.0f;
            sc.src[i].conf = 0.0f;
        }
        const int cx = 32, cy = 32;
        sc.src[(size_t)cy * kW + cx].conf = 1.0f;
        std::vector<Pixel> out = run(sc, shipped(), true);
        int reach = 0;
        for (int d = 1; d < 30; ++d)
            if (out[(size_t)cy * kW + (cx + d)].conf > 0.0f) reach = d;
        check(reach >= 14,
              "D8 a-trous DILATION: 3 iterations reach >= 14 texels for 75 taps (reach=" +
              std::to_string(reach) + "; a non-dilating 5x5 chain would reach 6)");
    }

    x3::logInfo("[refldn-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
