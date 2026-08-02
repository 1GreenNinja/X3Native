// ============================================================================
// --test-geolod — headless self-test for DISCRETE MESH LOD (Lane 5).
//
// CLEAN-ROOM, original work. See app/mesh_lod.h and app/mesh_decimate.h for the
// technique provenance. No GPL / id Tech / RBDOOM / Unreal source consulted.
//
// WHY THIS RUNS WITHOUT A GPU: every number that decides which LOD is drawn is
// produced by app/mesh_lod.cpp, and every number that decides what a LOD level
// looks like is produced by app/mesh_decimate.cpp. Both are deliberately
// RHI-free, so the test asserts on exactly the values the renderer consumes.
//
// WHAT IT ASSERTS
//   L1  the decimator actually decimates, keeps SUBSET placement (every emitted
//       index addresses an input vertex), and produces a monotonically
//       non-decreasing geometric error across the chain
//   L2  selection is MONOTONIC in camera distance: pulling the camera back never
//       selects a FINER level
//   L3  a mesh with NO chain always selects LOD0, at any distance — the
//       "behaves exactly as today" contract
//   L4  SCREEN-SPACE ERROR picks DIFFERENT levels for a large and a small object
//       at the SAME camera distance. This is the whole point of the metric.
//   L5  HYSTERESIS: oscillating the camera across a switch threshold changes the
//       selected level AT MOST ONCE, where the un-hystereticised selector flips
//       on every step
//   L6  r_meshlod 0 forces LOD0 for every object at every distance (the fallback
//       contract that keeps today's behaviour reachable)
//   V1  VERTEX COMPRESSION: the packed strides are the advertised 32 / 24 / 20 B
//   V2  format 0 round-trips BIT-EXACTLY (this is the fallback contract)
//   V3  the 10-10-10 normal round-trips within 0.15 deg over a sphere sweep, and
//       stays unit length
//   V4  half2 UV round-trips within 1e-3 over 0..1, and the test MEASURES the
//       precision loss at tiled UVs instead of pretending it is not there
//   L7  NEGATIVE CONTROL: the same rig with DISTANCE-ONLY selection FAILS L4 —
//       the large and the small object swap at the same distance. That is what
//       proves L4 is a real assertion and not a tautology, and it is the bar
//       docs/design/LANE_DISPATCH_PLAN.md requirement 2 sets.
// ============================================================================
#include "geolod_test.h"

#include "mesh_decimate.h"
#include "mesh_lod.h"
#include "engine/rhi/VertexPack.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {
namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[geolod-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[geolod-test] FAIL " + name); }
}

std::string f2s(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return std::string(buf);
}

// ---- A deterministic test mesh: a UV sphere of radius r --------------------
// Chosen because it is smooth (so the quadric has something to work with), it
// has no boundary edges (so the boundary term never dominates), and every length
// on it scales linearly with r — which is exactly the "large object vs small
// object" axis L4 needs.
struct TestMesh {
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>            idx;
};

TestMesh makeSphere(float r, uint32_t rings, uint32_t segs) {
    TestMesh m;
    const float kPi = 3.14159265358979f;
    for (uint32_t y = 0; y <= rings; ++y) {
        const float v  = (float)y / (float)rings;
        const float ph = v * kPi;
        for (uint32_t x = 0; x <= segs; ++x) {
            const float u  = (float)x / (float)segs;
            const float th = u * 2.0f * kPi;
            const float nx = std::sin(ph) * std::cos(th);
            const float ny = std::cos(ph);
            const float nz = std::sin(ph) * std::sin(th);
            x3::rhi::MeshVertex mv{};
            mv.pos[0] = nx * r; mv.pos[1] = ny * r; mv.pos[2] = nz * r;
            mv.normal[0] = nx;  mv.normal[1] = ny;  mv.normal[2] = nz;
            mv.uv[0] = u;       mv.uv[1] = v;
            m.verts.push_back(mv);
        }
    }
    const uint32_t stride = segs + 1;
    for (uint32_t y = 0; y < rings; ++y)
        for (uint32_t x = 0; x < segs; ++x) {
            const uint32_t a = y * stride + x, b = a + 1;
            const uint32_t c = (y + 1) * stride + x, d = c + 1;
            m.idx.insert(m.idx.end(), { a, c, b, b, c, d });
        }
    return m;
}

// Build a chain PURELY on the CPU (no device): the same successive decimation
// app/lod_chain.cpp performs, minus the upload. Handles are dummies; the
// selector never dereferences them, it only reads error[] / triangles[] / levels.
MeshLodChain cpuChain(const TestMesh& m, uint32_t levels = 4) {
    MeshLodChain c{};
    meshBoundingSphere(m.verts.data(), (uint32_t)m.verts.size(), c.center, c.radius);
    static const float kRatios[3] = { 0.5f, 0.5f, 0.4f };
    std::vector<uint32_t> cur = m.idx;
    float err = 0.0f;
    c.error[0]     = 0.0f;
    c.triangles[0] = (uint32_t)(cur.size() / 3);
    c.mesh[0]      = x3::rhi::MeshHandle{ 1 };
    c.levels       = 1;
    for (uint32_t l = 1; l < levels; ++l) {
        const DecimateResult r = decimateMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                              cur.data(), (uint32_t)cur.size(),
                                              kRatios[l - 1], err);
        if (r.indices.size() >= cur.size() || r.indices.size() < 3) break;
        cur = r.indices;
        err = r.maxError;
        c.error[l]     = err;
        c.triangles[l] = (uint32_t)(cur.size() / 3);
        c.mesh[l]      = x3::rhi::MeshHandle{ 1 + l };
        c.levels       = l + 1;
    }
    return c;
}

// A column-major model matrix: uniform scale `s`, translated to (0,0,z).
void modelAt(float out[16], float s, float z) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = out[5] = out[10] = s;
    out[15] = 1.0f;
    out[14] = z;
}

LodView baseView() {
    LodView v{};
    v.eye[0] = v.eye[1] = v.eye[2] = 0.0f;
    v.fovYDeg   = 70.0f;   // what the terrain / CSM captures use
    v.viewportH = 720;     // the headless render height
    return v;
}

// ---- L1: the decimator ----------------------------------------------------
void testDecimator() {
    const TestMesh m = makeSphere(1.0f, 32, 48);
    const uint32_t inTris = (uint32_t)(m.idx.size() / 3);

    const DecimateResult r = decimateMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                          m.idx.data(), (uint32_t)m.idx.size(), 0.5f);
    check(r.triangles > 0 && r.triangles <= inTris * 60 / 100,
          "L1 decimator reaches the ratio (" + std::to_string(inTris) + " -> " +
          std::to_string(r.triangles) + " triangles, target 50%)");

    bool subset = true;
    for (uint32_t i : r.indices) if (i >= m.verts.size()) { subset = false; break; }
    check(subset, "L1 SUBSET placement: every emitted index addresses an ORIGINAL vertex "
                  "(this is what lets a whole chain share one vertex buffer)");

    check(r.maxError > 0.0f && r.maxError < 1.0f,
          "L1 geometric error is a plausible model-space displacement (" +
          f2s(r.maxError) + " m on a 1 m-radius sphere)");

    const MeshLodChain c = cpuChain(m);
    check(c.levels >= 3, "L1 chain reaches at least 3 levels (got " + std::to_string(c.levels) + ")");
    bool ascendErr = true, descendTris = true;
    for (uint32_t l = 1; l < c.levels; ++l) {
        if (!(c.error[l] >= c.error[l - 1]))        ascendErr = false;
        if (!(c.triangles[l] < c.triangles[l - 1])) descendTris = false;
    }
    check(ascendErr, "L1 error is monotonically NON-DECREASING across the chain "
                     "(lodSelect stops at the first level over budget, so it must be)");
    check(descendTris, "L1 triangle count strictly DECREASES across the chain");

    std::string tri = "[geolod-test] chain: ";
    for (uint32_t l = 0; l < c.levels; ++l)
        tri += "LOD" + std::to_string(l) + " " + std::to_string(c.triangles[l]) +
               " tris / err " + f2s(c.error[l]) + " m" + (l + 1 < c.levels ? " | " : "");
    x3::logInfo(tri);

    // A mesh too small to usefully decimate must not produce a bogus chain.
    const TestMesh tiny = makeSphere(1.0f, 2, 3);
    const MeshLodChain tc = cpuChain(tiny);
    check(tc.levels >= 1, "L1 a near-minimal mesh still yields a valid chain (levels = " +
                          std::to_string(tc.levels) + ")");
}

// ---- L2: monotonic in distance --------------------------------------------
void testMonotonic() {
    const TestMesh m = makeSphere(2.0f, 32, 48);
    const MeshLodChain c = cpuChain(m);
    const LodView v = baseView();
    LodPolicy p{};

    uint32_t prev = 0;
    bool monotone = true;
    float firstSwitch[kMaxLodLevels] = { 0, 0, 0, 0 };
    for (int step = 1; step <= 4000; ++step) {
        const float z = (float)step * 0.5f;      // 0.5 m .. 2000 m
        float model[16]; modelAt(model, 1.0f, z);
        const uint32_t lvl = lodSelect(v, p, c, model);
        if (lvl < prev) monotone = false;
        if (lvl > prev && lvl < kMaxLodLevels) firstSwitch[lvl] = z;
        prev = lvl;
    }
    check(monotone, "L2 selection is MONOTONIC in distance: pulling the camera back never "
                    "selects a finer level");
    check(prev == c.levels - 1, "L2 the far end of the sweep reaches the COARSEST level");

    std::string s = "[geolod-test] switch distances (2 m sphere, 1.5 px budget, 720p / 70 deg):";
    for (uint32_t l = 1; l < c.levels; ++l)
        s += " LOD" + std::to_string(l - 1) + "->" + std::to_string(l) + " at " +
             f2s(firstSwitch[l]) + " m;";
    x3::logInfo(s);
}

// ---- L3: no chain -> always LOD0 ------------------------------------------
void testNoChain() {
    MeshLodChain none{};                 // levels == 0 — nothing was ever built
    none.radius = 1.0f;
    MeshLodChain single{};               // a real mesh, but only LOD0
    single.levels = 1; single.radius = 1.0f; single.mesh[0] = x3::rhi::MeshHandle{ 7 };
    single.triangles[0] = 500;

    const LodView v = baseView();
    LodPolicy p{};
    bool allZero = true;
    for (int step = 0; step < 500; ++step) {
        float model[16]; modelAt(model, 1.0f, 0.1f + (float)step * 4.0f);
        if (lodSelect(v, p, none, model) != 0)   allZero = false;
        if (lodSelect(v, p, single, model) != 0) allZero = false;
        if (lodSelectHysteretic(v, p, single, model, 3) != 0) allZero = false;
    }
    check(allZero, "L3 a mesh with NO authored LOD chain selects LOD0 at every distance "
                   "(and a stale hysteresis state cannot lift it off LOD0)");
}

// ---- L4 + L7: the large-vs-small property, and the negative control --------
struct SizeOutcome {
    uint32_t big = 0, small = 0;
    float    bigErrPx = 0.0f, smallErrPx = 0.0f;
};

// Same distance, same shape, only the SIZE differs. `distanceOnly` selects the
// negative-control path.
SizeOutcome measureSizeSplit(bool distanceOnly, float dist) {
    // A 40 m-radius tower-scale object and a 0.4 m-radius prop-scale object.
    // Both are the same sphere, scaled — so their LOD chains are identical up to
    // a factor of 100 in every length, which is precisely the case a distance-
    // only selector cannot tell apart.
    static const TestMesh mesh = makeSphere(1.0f, 32, 48);
    static const MeshLodChain c = cpuChain(mesh);

    const LodView v = baseView();
    LodPolicy p{};
    p.distanceOnly = distanceOnly;

    float bigModel[16];   modelAt(bigModel,   40.0f, dist);
    float smallModel[16]; modelAt(smallModel,  0.4f, dist);

    SizeOutcome o{};
    o.big        = lodSelect(v, p, c, bigModel);
    o.small      = lodSelect(v, p, c, smallModel);
    o.bigErrPx   = lodPixelError(v, p, c, bigModel,   1);
    o.smallErrPx = lodPixelError(v, p, c, smallModel, 1);
    return o;
}

void testSizeSplit() {
    // 120 m out: far enough that the small prop is only a few pixels across,
    // close enough that the tower still fills a large part of the frame.
    const SizeOutcome sse = measureSizeSplit(/*distanceOnly=*/false, 120.0f);

    check(sse.big != sse.small,
          "L4 SCREEN-SPACE ERROR selects DIFFERENT levels for a large and a small object at "
          "the SAME 120 m distance (tower -> LOD" + std::to_string(sse.big) +
          ", prop -> LOD" + std::to_string(sse.small) + ")");
    check(sse.big < sse.small,
          "L4 and it is the LARGE object that holds the finer level (its LOD1 error projects "
          "to " + f2s(sse.bigErrPx) + " px vs the prop's " + f2s(sse.smallErrPx) + " px)");
}

void testNegativeControl() {
    const SizeOutcome naive = measureSizeSplit(/*distanceOnly=*/true, 120.0f);

    check(naive.big == naive.small,
          "L7 NEGATIVE CONTROL: DISTANCE-ONLY selection puts the tower and the prop on the "
          "SAME level (LOD" + std::to_string(naive.big) + ") at the same distance — it FAILS "
          "the L4 assertion, which is what proves L4 is real");

    // Quantify how wrong it is across the whole useful range.
    int disagree = 0, total = 0;
    for (int step = 1; step <= 60; ++step) {
        const float d = (float)step * 5.0f;              // 5 m .. 300 m
        const SizeOutcome sse    = measureSizeSplit(false, d);
        const SizeOutcome naive2 = measureSizeSplit(true,  d);
        ++total;
        if (sse.big != naive2.big || sse.small != naive2.small) ++disagree;
    }
    x3::logInfo("[geolod-test] negative control: distance-only disagrees with screen-space "
                "error on " + std::to_string(disagree) + " of " + std::to_string(total) +
                " sampled distances");
    check(disagree > total / 2,
          "L7 the two policies genuinely differ over the working range (not a relabelling)");
}

// ---- L5: hysteresis --------------------------------------------------------
struct FlipOutcome { uint32_t changes = 0; uint32_t first = 0, last = 0; };

// Oscillate the camera across a switch threshold and count how many times the
// selected level CHANGES. `hyst` = 0 is the un-hystereticised selector.
FlipOutcome measureFlip(float hyst) {
    const TestMesh mesh = makeSphere(2.0f, 32, 48);
    const MeshLodChain c = cpuChain(mesh);
    const LodView v = baseView();
    LodPolicy p{};
    p.hysteresis = hyst;

    // Find the LOD0->LOD1 threshold distance by bisection on the STATELESS
    // selector, then oscillate a small amount around it.
    float lo = 0.5f, hi = 4000.0f;
    for (int i = 0; i < 64; ++i) {
        const float mid = 0.5f * (lo + hi);
        float model[16]; modelAt(model, 1.0f, mid);
        if (lodSelect(v, p, c, model) >= 1) hi = mid; else lo = mid;
    }
    const float thr = 0.5f * (lo + hi);

    // +-2% of the threshold: well inside the 1.35x dead band the default
    // hysteresis buys, and far outside float noise.
    const float amp = thr * 0.02f;
    FlipOutcome o{};
    uint32_t lvl = 0;
    {
        float model[16]; modelAt(model, 1.0f, thr - amp);
        lvl = lodSelectHysteretic(v, p, c, model, 0);
        o.first = lvl;
    }
    for (int step = 0; step < 200; ++step) {
        const float z = thr + amp * std::sin((float)step * 0.7f);
        float model[16]; modelAt(model, 1.0f, z);
        const uint32_t next = lodSelectHysteretic(v, p, c, model, lvl);
        if (next != lvl) ++o.changes;
        lvl = next;
    }
    o.last = lvl;
    return o;
}

void testHysteresis() {
    const FlipOutcome withH = measureFlip(0.15f);
    check(withH.changes <= 1,
          "L5 HYSTERESIS: 200 camera steps oscillating +-2% across the LOD0->LOD1 threshold "
          "change the selected level " + std::to_string(withH.changes) + " time(s) (must be <= 1)");

    const FlipOutcome noH = measureFlip(0.0f);
    check(noH.changes > 10,
          "L5 NEGATIVE CONTROL: with the dead band removed (hysteresis 0) the SAME oscillation "
          "flips the level " + std::to_string(noH.changes) + " times — the flicker the band "
          "exists to stop");
}

// ---- L6: the r_meshlod 0 fallback -----------------------------------------
void testFallback() {
    const TestMesh m = makeSphere(2.0f, 32, 48);
    const MeshLodChain c = cpuChain(m);
    const LodView v = baseView();
    LodPolicy off{};
    off.enabled = false;

    bool allZero = true;
    for (int step = 1; step <= 2000; ++step) {
        float model[16]; modelAt(model, 1.0f, (float)step);
        if (lodSelect(v, off, c, model) != 0) allZero = false;
        if (lodSelectHysteretic(v, off, c, model, 3) != 0) allZero = false;
    }
    check(allZero, "L6 r_meshlod 0 forces LOD0 for every object at every distance "
                   "(today's behaviour stays reachable and bit-exact)");
}

// ---- V1..V4: vertex compression -------------------------------------------
void testVertexPack() {
    using namespace x3::rhi;

    check(vertexStrideFor(kVtxFmtLegacy) == 32 &&
          vertexStrideFor(kVtxFmtNormal10) == 24 &&
          vertexStrideFor(kVtxFmtNormal10Uv16) == 20,
          "V1 packed strides are 32 / 24 / 20 bytes (-0% / -25% / -37.5%)");

    // ---- V2: the legacy format is a bit-exact identity ----
    {
        bool exact = true;
        uint8_t buf[32];
        for (int i = 0; i < 4096; ++i) {
            MeshVertex v{};
            v.pos[0] = (float)i * 0.37f - 700.0f;
            v.pos[1] = (float)i * -1.9f + 3.5f;
            v.pos[2] = std::sin((float)i) * 512.0f;
            v.normal[0] = std::cos((float)i * 0.11f);
            v.normal[1] = std::sin((float)i * 0.07f);
            v.normal[2] = std::cos((float)i * 0.29f);
            v.uv[0] = (float)i * 0.013f;
            v.uv[1] = (float)i * -0.0071f;
            packVertex(v, kVtxFmtLegacy, buf);
            MeshVertex r{};
            unpackVertex(buf, kVtxFmtLegacy, r);
            for (int k = 0; k < 3; ++k) {
                if (r.pos[k] != v.pos[k] || r.normal[k] != v.normal[k]) exact = false;
            }
            if (r.uv[0] != v.uv[0] || r.uv[1] != v.uv[1]) exact = false;
        }
        check(exact, "V2 format 0 round-trips BIT-EXACTLY over 4096 vertices "
                     "(exact float compare — the --vtxfmt 0 fallback contract)");
    }

    // ---- V3: the 10-10-10 normal ----
    {
        double worstDeg = 0.0, worstLen = 0.0;
        uint8_t buf[24];
        for (int a = 0; a < 180; ++a)
            for (int b = 0; b < 360; b += 3) {
                const float th = (float)a * 3.14159265f / 180.0f;
                const float ph = (float)b * 3.14159265f / 180.0f;
                MeshVertex v{};
                v.normal[0] = std::sin(th) * std::cos(ph);
                v.normal[1] = std::cos(th);
                v.normal[2] = std::sin(th) * std::sin(ph);
                packVertex(v, kVtxFmtNormal10, buf);
                MeshVertex r{};
                unpackVertex(buf, kVtxFmtNormal10, r);
                const double len = std::sqrt((double)r.normal[0] * r.normal[0] +
                                             (double)r.normal[1] * r.normal[1] +
                                             (double)r.normal[2] * r.normal[2]);
                double d = (double)r.normal[0] * v.normal[0] +
                           (double)r.normal[1] * v.normal[1] +
                           (double)r.normal[2] * v.normal[2];
                if (len > 1e-9) d /= len;
                d = std::max(-1.0, std::min(1.0, d));
                worstDeg = std::max(worstDeg, std::acos(d) * 180.0 / 3.14159265358979);
                worstLen = std::max(worstLen, std::abs(len - 1.0));
            }
        check(worstDeg < 0.15,
              "V3 A2B10G10R10_SNORM normal round-trips within " + f2s(worstDeg) +
              " deg over a full sphere sweep (budget 0.15 deg)");
        check(worstLen < 0.005,
              "V3 and stays unit length to " + f2s(worstLen) + " (budget 0.005)");
    }

    // ---- V4: the half2 UV, INCLUDING where it stops being good enough ----
    {
        double worst01 = 0.0;
        uint8_t buf[20];
        for (int i = 0; i <= 4000; ++i) {
            MeshVertex v{};
            v.uv[0] = (float)i / 4000.0f;
            v.uv[1] = 1.0f - (float)i / 4000.0f;
            packVertex(v, kVtxFmtNormal10Uv16, buf);
            MeshVertex r{};
            unpackVertex(buf, kVtxFmtNormal10Uv16, r);
            worst01 = std::max({ worst01,
                                 (double)std::abs(r.uv[0] - v.uv[0]),
                                 (double)std::abs(r.uv[1] - v.uv[1]) });
        }
        check(worst01 < 1.0e-3,
              "V4 half2 UV round-trips within " + f2s(worst01) + " over the 0..1 range "
              "(budget 1e-3 == a quarter texel of a 4096 map)");

        // The known limitation, MEASURED rather than hand-waved: half floats lose
        // absolute precision as the exponent grows, so a tiled UV is where format 2
        // stops being free. This is why format 1 (full-precision UV) exists and why
        // format 2 is not the default.
        double worstTiled = 0.0;
        for (int i = 0; i <= 4000; ++i) {
            MeshVertex v{};
            v.uv[0] = 64.0f * (float)i / 4000.0f;
            v.uv[1] = v.uv[0];
            packVertex(v, kVtxFmtNormal10Uv16, buf);
            MeshVertex r{};
            unpackVertex(buf, kVtxFmtNormal10Uv16, r);
            worstTiled = std::max(worstTiled, (double)std::abs(r.uv[0] - v.uv[0]));
        }
        check(worstTiled > 1.0e-3,
              "V4 and DEGRADES at tiled UVs as expected: worst error over 0..64 is " +
              f2s(worstTiled) + " uv units (" + f2s(worstTiled * 4096.0) +
              " texels of a 4096 map) — the documented reason --vtxfmt 2 is opt-in");
    }
}

} // namespace

bool runGeoLodSelfTest() {
    g_pass = g_fail = 0;
    LodPolicy def{};
    x3::logInfo("[geolod-test] defaults: r_meshlod_err = " + f2s(def.pixelError) +
                " px, r_meshlod_hyst = " + f2s(def.hysteresis) +
                " (dead band " + f2s((1.0 + def.hysteresis) / (1.0 - def.hysteresis)) + "x)");

    testDecimator();
    testMonotonic();
    testNoChain();
    testSizeSplit();
    testHysteresis();
    testFallback();
    testVertexPack();
    testNegativeControl();

    x3::logInfo("[geolod-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
