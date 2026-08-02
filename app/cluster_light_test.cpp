// ===========================================================================
// cluster_light_test.cpp — --test-clusterlights.
//
// See cluster_light_test.h for the shape. The design principle throughout:
// NEVER check the implementation against itself. Part A checks the fast
// assignment path against a brute-force sweep over every froxel; Part B checks
// the shader's fragment->froxel lookup against the legacy loop it replaces, on
// the real GPU, by demanding the two renders be bit-identical.
// ===========================================================================
#include "cluster_light_test.h"

#include "mesh_prims.h"
#include "surface_library.h"          // decodePngRGBA8
#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/rhi/ClusterLights.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <array>
#include <vector>

namespace x3::game {

namespace {

using namespace x3::rhi;

int cl_pass = 0, cl_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) { ++cl_pass; x3::logInfo("  [PASS] " + what); }
    else    { ++cl_fail; x3::logError("  [FAIL] " + what); }
}

std::string fmt(double v, int dp = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
    return buf;
}

// ---------------------------------------------------------------------------
// The reference camera the CPU part reasons about: at the origin, looking down
// +Z, world up +Y, 60 deg vertical FOV, 16:9, 0.1 .. 500 m. Chosen so a reader
// can do the geometry in their head: at 10 m the frustum is 2*10*tan(30) =
// 11.55 m tall and 20.5 m wide.
// ---------------------------------------------------------------------------
ClusterView referenceView() {
    const float eye[3] = { 0.0f, 0.0f, 0.0f };
    const float fwd[3] = { 0.0f, 0.0f, 1.0f };
    const float up[3]  = { 0.0f, 1.0f, 0.0f };
    return makeClusterView(eye, fwd, up, 60.0f, 16.0f / 9.0f, 0.1f, 500.0f);
}

PointLight makeLight(float x, float y, float z, float range, float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    PointLight L{};
    L.pos[0] = x; L.pos[1] = y; L.pos[2] = z;
    L.range = range;
    L.color[0] = r; L.color[1] = g; L.color[2] = b;
    return L;
}

// ---------------------------------------------------------------------------
// INDEPENDENT REFERENCE. Brute-force: test the light sphere against the AABB of
// EVERY froxel, with no screen-space or depth narrowing at all. This is the
// definition of "which froxels does this light overlap"; buildClusterLightLists
// is an optimisation of it, and Part A's job is to prove the optimisation did
// not lose anything.
//
// `sliceShift` exists purely for the NEGATIVE CONTROL: pass a non-zero value to
// deliberately look at the WRONG depth slice, which must make the comparison
// fail. A gate that cannot go red is worthless.
// ---------------------------------------------------------------------------
std::vector<uint32_t> bruteForceFroxels(const ClusterView& v, const PointLight& L, int sliceShift = 0) {
    std::vector<uint32_t> out;
    const float d[3] = { L.pos[0] - v.camPos[0], L.pos[1] - v.camPos[1], L.pos[2] - v.camPos[2] };
    const float c[3] = {
        d[0] * v.camRight[0] + d[1] * v.camRight[1] + d[2] * v.camRight[2],
        d[0] * v.camUp[0]    + d[1] * v.camUp[1]    + d[2] * v.camUp[2],
        d[0] * v.camFwd[0]   + d[1] * v.camFwd[1]   + d[2] * v.camFwd[2],
    };
    const float R  = std::max(L.range, 0.0f) * (1.0f + kClusterRadiusPadRel) + kClusterRadiusPad;
    const float R2 = R * R;

    for (uint32_t iz = 0; iz < kClusterGridZ; ++iz) {
        for (uint32_t iy = 0; iy < kClusterGridY; ++iy) {
            for (uint32_t ix = 0; ix < kClusterGridX; ++ix) {
                const int zsrc = (int)iz + sliceShift;
                if (zsrc < 0 || zsrc >= (int)kClusterGridZ) continue;
                float mn[3], mx[3];
                clusterFroxelBoundsView(v, ix, iy, (uint32_t)zsrc, mn, mx);
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float e = c[k] < mn[k] ? (mn[k] - c[k])
                                  : c[k] > mx[k] ? (c[k] - mx[k]) : 0.0f;
                    d2 += e * e;
                }
                if (d2 <= R2) out.push_back(clusterIndex(ix, iy, iz));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Which froxels did the production path put light `li` in?
std::vector<uint32_t> producedFroxels(const std::vector<uint32_t>& counts,
                                      const std::vector<uint32_t>& indices, uint32_t li) {
    std::vector<uint32_t> out;
    for (uint32_t c = 0; c < kClusterCount; ++c)
        for (uint32_t k = 0; k < counts[c]; ++k)
            if (indices[(size_t)c * kMaxLightsPerCluster + k] == li) { out.push_back(c); break; }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Part-B image helpers -------------------------------------------------
struct Img { std::vector<uint8_t> px; int w = 0, h = 0; };

Img loadImg(const std::string& p) {
    Img i;
    i.px = decodePngRGBA8(p, i.w, i.h);
    return i;
}

// Count differing pixels between two same-size captures.
size_t pixelDiff(const Img& a, const Img& b) {
    if (a.w != b.w || a.h != b.h || a.px.empty() || b.px.empty()) return (size_t)-1;
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.px.size(); i += 4)
        if (a.px[i] != b.px[i] || a.px[i+1] != b.px[i+1] || a.px[i+2] != b.px[i+2]) ++n;
    return n;
}

double meanLuma(const Img& a) {
    if (a.px.empty()) return -1.0;
    double s = 0; size_t n = 0;
    for (size_t i = 0; i + 3 < a.px.size(); i += 4) {
        s += 0.2126 * a.px[i] + 0.7152 * a.px[i+1] + 0.0722 * a.px[i+2];
        ++n;
    }
    return n ? s / (double)n : -1.0;
}

double blockLuma(const Img& a, int cx, int cy, int half) {
    if (a.px.empty()) return -1.0;
    double s = 0; int n = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        if (y < 0 || y >= a.h) continue;
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || x >= a.w) continue;
            const size_t i = ((size_t)y * a.w + x) * 4;
            s += 0.2126 * a.px[i] + 0.7152 * a.px[i+1] + 0.0722 * a.px[i+2];
            ++n;
        }
    }
    return n ? s / n : -1.0;
}

// ===========================================================================
// PART A — the froxel grid + assignment, on the CPU.
// ===========================================================================
void runCpuPart() {
    x3::logInfo("--- PART A: froxel grid + light assignment (CPU) ---");
    const ClusterView v = referenceView();

    std::vector<uint32_t> counts(kClusterCount);
    std::vector<uint32_t> indices((size_t)kClusterCount * kMaxLightsPerCluster);

    // ---- A1/A2/A3: the exponential depth-slice law ------------------------
    {
        const float z0 = clusterSliceNearZ(v, 0);
        const float zN = clusterSliceNearZ(v, kClusterGridZ);
        check(std::fabs(z0 - v.zNear) < 1e-4f && std::fabs(zN - v.zFar) < 1e-2f,
              "A1 slice boundaries span exactly [zNear, zFar] (" + fmt(z0) + " .. " + fmt(zN, 1) + ")");

        bool mono = true, widening = true;
        float prevW = -1.0f;
        for (uint32_t k = 0; k < kClusterGridZ; ++k) {
            const float a = clusterSliceNearZ(v, k), b = clusterSliceNearZ(v, k + 1);
            if (!(b > a)) mono = false;
            const float w = b - a;
            if (prevW >= 0.0f && !(w > prevW)) widening = false;
            prevW = w;
        }
        check(mono, "A1b slice boundaries are strictly increasing");
        // EXPONENTIAL, not linear: every slice must be thicker than the one before
        // it. A linear grid would give constant width, which is exactly the failure
        // this slicing law exists to avoid (the whole near field in one froxel).
        const float wFirst = clusterSliceNearZ(v, 1) - clusterSliceNearZ(v, 0);
        const float wLast  = clusterSliceNearZ(v, kClusterGridZ) - clusterSliceNearZ(v, kClusterGridZ - 1);
        check(widening && wLast > wFirst * 100.0f,
              "A3 slicing is EXPONENTIAL: every slice thicker than the last, far/near width ratio " +
              fmt(wLast / wFirst, 1) + "x (near slice " + fmt(wFirst, 4) + " m, far slice " + fmt(wLast, 1) + " m)");

        // Round-trip: the midpoint of slice k must resolve back to slice k.
        bool rt = true;
        for (uint32_t k = 0; k < kClusterGridZ; ++k) {
            const float a = clusterSliceNearZ(v, k), b = clusterSliceNearZ(v, k + 1);
            const float mid = std::sqrt(a * b);          // geometric mid (this is a log grid)
            if (clusterSliceForViewZ(v, mid) != k) { rt = false; break; }
        }
        check(rt, "A2 viewZ -> slice round-trips for all " + std::to_string(kClusterGridZ) + " slices");

        // Out-of-range depths must still land inside the grid (a fragment must
        // ALWAYS resolve to a real froxel or the shader indexes out of bounds).
        check(clusterSliceForViewZ(v, -50.0f) == 0 &&
              clusterSliceForViewZ(v, 0.0f) == 0 &&
              clusterSliceForViewZ(v, 1e9f) == kClusterGridZ - 1,
              "A2b depths behind the camera / past the far plane clamp into the grid");
    }

    // ---- A4: one small light, checked against the brute-force sweep -------
    {
        const PointLight L = makeLight(0.0f, 0.0f, 10.0f, 2.0f);
        const auto r = buildClusterLightLists(v, &L, 1, counts.data(), indices.data());
        const auto got = producedFroxels(counts, indices, 0);
        const auto ref = bruteForceFroxels(v, L);
        check(!ref.empty() && got == ref,
              "A4 small light (2 m range at 10 m) -> EXACTLY the brute-force froxel set (" +
              std::to_string(got.size()) + " froxels)");
        check(r.assignments == (uint32_t)ref.size() && r.lightsVisible == 1 && r.overflows == 0,
              "A4b counters agree: " + std::to_string(r.assignments) + " assignments, 1 visible, 0 overflow");
    }

    // ---- A5: a light SPANNING many froxels --------------------------------
    {
        const PointLight L = makeLight(0.0f, 0.0f, 12.0f, 9.0f);
        const auto r = buildClusterLightLists(v, &L, 1, counts.data(), indices.data());
        const auto got = producedFroxels(counts, indices, 0);
        const auto ref = bruteForceFroxels(v, L);
        check(got == ref,
              "A5 large light (9 m range) spanning MANY froxels -> exact brute-force match (" +
              std::to_string(got.size()) + " froxels)");
        // It must genuinely span in all three axes, or "spanning" is not tested.
        uint32_t xs = 0, ys = 0, zs = 0;
        bool seenX[kClusterGridX] = {}, seenY[kClusterGridY] = {}, seenZ[kClusterGridZ] = {};
        for (uint32_t c : got) {
            const uint32_t ix = c % kClusterGridX;
            const uint32_t iy = (c / kClusterGridX) % kClusterGridY;
            const uint32_t iz = c / (kClusterGridX * kClusterGridY);
            if (!seenX[ix]) { seenX[ix] = true; ++xs; }
            if (!seenY[iy]) { seenY[iy] = true; ++ys; }
            if (!seenZ[iz]) { seenZ[iz] = true; ++zs; }
        }
        check(xs >= 3 && ys >= 3 && zs >= 3,
              "A5b the spanning light really spans all three axes (" + std::to_string(xs) + " x " +
              std::to_string(ys) + " y " + std::to_string(zs) + " z froxel columns)");
        check(r.maxClusterLoad == 1, "A5c a single light never stacks more than 1 deep in any froxel");
    }

    // ---- A6/A7/A8: lights entirely OUTSIDE the frustum are free -----------
    {
        struct Case { const char* name; PointLight L; };
        const Case cases[] = {
            { "BEHIND the camera",        makeLight(0.0f,   0.0f, -30.0f, 5.0f) },
            { "far off to the LEFT",      makeLight(-400.0f, 0.0f,  10.0f, 5.0f) },
            { "far ABOVE",                makeLight(0.0f,  400.0f,  10.0f, 5.0f) },
            { "beyond the FAR plane",     makeLight(0.0f,   0.0f, 900.0f, 5.0f) },
        };
        bool allFree = true;
        std::string detail;
        for (const auto& c : cases) {
            const auto r = buildClusterLightLists(v, &c.L, 1, counts.data(), indices.data());
            const auto ref = bruteForceFroxels(v, c.L);
            const bool ok = (r.assignments == 0 && r.lightsCulled == 1 && r.lightsVisible == 0 && ref.empty());
            if (!ok) { allFree = false; detail += std::string(" [") + c.name + " assigned " +
                                                  std::to_string(r.assignments) + "]"; }
        }
        check(allFree, "A6 lights entirely outside the frustum are CULLED, 0 assignments" + detail);
    }

    // A borderline case that must NOT be culled: a light whose centre is behind
    // the camera but whose SPHERE reaches into the frustum still lights it.
    {
        const PointLight L = makeLight(0.0f, 0.0f, -3.0f, 12.0f);
        const auto r = buildClusterLightLists(v, &L, 1, counts.data(), indices.data());
        const auto got = producedFroxels(counts, indices, 0);
        const auto ref = bruteForceFroxels(v, L);
        check(r.assignments > 0 && got == ref,
              "A6b a light BEHIND the camera whose sphere still reaches in is NOT culled (" +
              std::to_string(r.assignments) + " froxels, brute-force match)");
    }

    // ---- A9: structural integrity of the produced lists -------------------
    {
        std::vector<PointLight> many;
        for (int i = 0; i < 120; ++i) {
            const float a = (float)i * 0.7f;
            many.push_back(makeLight(std::cos(a) * (2.0f + (float)(i % 7)),
                                     std::sin(a) * 2.0f,
                                     4.0f + (float)(i % 40),
                                     3.0f + (float)(i % 5)));
        }
        const auto r = buildClusterLightLists(v, many.data(), (uint32_t)many.size(),
                                              counts.data(), indices.data());
        bool inRange = true, noDupes = true, ascending = true;
        for (uint32_t c = 0; c < kClusterCount && (inRange && noDupes && ascending); ++c) {
            if (counts[c] > kMaxLightsPerCluster) { inRange = false; break; }
            uint32_t prev = 0; bool first = true;
            for (uint32_t k = 0; k < counts[c]; ++k) {
                const uint32_t li = indices[(size_t)c * kMaxLightsPerCluster + k];
                if (li >= many.size()) { inRange = false; break; }
                if (!first && li == prev) { noDupes = false; break; }
                if (!first && li < prev) { ascending = false; break; }
                prev = li; first = false;
            }
        }
        check(inRange, "A9 every emitted index is a valid light index and no list exceeds the cap");
        check(noDupes, "A9b no light appears twice in one froxel's list");
        // Ascending order is the DETERMINISM contract the image gates depend on.
        check(ascending, "A9c each froxel's list is in ASCENDING light index (the determinism contract)");

        // Cross-check EVERY light against brute force, not just a sampled few.
        bool allMatch = true; uint32_t firstBad = 0;
        for (uint32_t li = 0; li < many.size(); ++li) {
            if (producedFroxels(counts, indices, li) != bruteForceFroxels(v, many[li])) {
                allMatch = false; firstBad = li; break;
            }
        }
        check(allMatch, allMatch
              ? "A9d all 120 lights match the brute-force sweep exactly"
              : "A9d light " + std::to_string(firstBad) + " does NOT match the brute-force sweep");
        check(r.lightsConsidered == 120, "A9e all 120 lights were considered");
    }

    // ---- A10: THE OVERFLOW POLICY -----------------------------------------
    {
        // kMaxLightsPerCluster + 40 lights piled on the SAME spot, so every froxel
        // they touch is driven past the cap.
        const uint32_t n = kMaxLightsPerCluster + 40;
        std::vector<PointLight> pile;
        for (uint32_t i = 0; i < n; ++i)
            pile.push_back(makeLight(0.0f, 0.0f, 10.0f + (float)i * 1e-4f, 1.5f));

        const auto r = buildClusterLightLists(v, pile.data(), n, counts.data(), indices.data());

        check(r.overflows > 0 && r.clustersOverflowed > 0,
              "A10 overflow FIRES and is COUNTED: " + std::to_string(r.overflows) +
              " assignment(s) dropped across " + std::to_string(r.clustersOverflowed) + " froxel(s)");
        check(r.maxClusterLoad == kMaxLightsPerCluster,
              "A10b no froxel list ever exceeds the " + std::to_string(kMaxLightsPerCluster) + " cap (max load " +
              std::to_string(r.maxClusterLoad) + ")");

        // THE POLICY, verified: the survivors in a saturated froxel are the LOWEST
        // light indices. That is the documented contract hosts rely on ("put the
        // lights that matter first").
        bool policyOk = true;
        uint32_t saturated = 0;
        for (uint32_t c = 0; c < kClusterCount; ++c) {
            if (counts[c] != kMaxLightsPerCluster) continue;
            ++saturated;
            for (uint32_t k = 0; k < kMaxLightsPerCluster; ++k)
                if (indices[(size_t)c * kMaxLightsPerCluster + k] != k) { policyOk = false; break; }
            if (!policyOk) break;
        }
        check(policyOk && saturated > 0,
              "A10c OVERFLOW POLICY: in all " + std::to_string(saturated) +
              " saturated froxel(s) the survivors are exactly lights 0.." +
              std::to_string(kMaxLightsPerCluster - 1) + " (lowest index wins, deterministic)");

        // And the total is conserved: kept + dropped == what brute force says the
        // pile should have produced. Nothing vanishes without being counted.
        uint32_t bruteTotal = 0;
        for (uint32_t i = 0; i < n; ++i) bruteTotal += (uint32_t)bruteForceFroxels(v, pile[i]).size();
        check(r.assignments + r.overflows == bruteTotal,
              "A10d NOTHING IS SILENTLY LOST: kept(" + std::to_string(r.assignments) + ") + dropped(" +
              std::to_string(r.overflows) + ") == brute-force total(" + std::to_string(bruteTotal) + ")");
    }

    // ---- A11: NEGATIVE CONTROL --------------------------------------------
    // Everything above leans on `producedFroxels == bruteForceFroxels`. If that
    // comparison cannot distinguish a WRONG assignment from a right one, every
    // PASS above is worthless. So: feed the comparator a deliberately wrong
    // assignment (the brute-force sweep looking one depth slice off) and demand
    // it reports a MISMATCH.
    {
        const PointLight L = makeLight(0.0f, 0.0f, 10.0f, 2.0f);
        buildClusterLightLists(v, &L, 1, counts.data(), indices.data());
        const auto good = producedFroxels(counts, indices, 0);
        const auto wrong = bruteForceFroxels(v, L, /*sliceShift*/ +1);
        check(good != wrong && !good.empty(),
              "A11 NEGATIVE CONTROL: an assignment off by ONE DEPTH SLICE is REJECTED by the "
              "same comparator every check above uses (" + std::to_string(good.size()) + " vs " +
              std::to_string(wrong.size()) + " froxels) — the gate can go red");

        // A second, independent way to go red: a light that IS in the frustum must
        // not produce an empty set (catches "assignment silently does nothing").
        check(!good.empty(), "A11b a light inside the frustum produces a NON-EMPTY froxel set");
    }
}

// ===========================================================================
// PART B — the real GPU, r_clusterlights 0 vs 1.
// ===========================================================================

// A panel standing in the XY plane facing -Z (the camera), 1 m half-extent.
rhi::MeshHandle makePanel(rhi::IRenderDevice& d, float cx, float cy, float cz) {
    x3::prims::PrimMesh m = x3::prims::makeBox(1.0f, 1.0f, 0.1f, cx, cy, cz, 0.5f);
    return d.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                        m.index.data(), (uint32_t)m.index.size());
}

struct Rig {
    std::vector<rhi::MeshHandle> panels;
    std::vector<float>           px, py;
    rhi::TextureHandle           albedo{};
};

// A row of panels across the view, each with its OWN lamp just in front of it,
// plus `filler` extra lamps parked far off to the side. The filler lamps are what
// makes the >64 case interesting: they occupy the low light indices, so the
// legacy path (first 64 only) never gets to the lamps that light the panels.
Rig buildRig(rhi::IRenderDevice& d, int panelCount) {
    Rig r;
    const uint8_t grey[4] = { 160, 160, 160, 255 };
    r.albedo = d.createTexture(grey, 1, 1, /*srgb*/ true);
    const float pitch = 2.6f;
    const float x0 = -pitch * (panelCount - 1) * 0.5f;
    for (int i = 0; i < panelCount; ++i) {
        const float x = x0 + pitch * i;
        r.panels.push_back(makePanel(d, x, 0.0f, 0.0f));
        r.px.push_back(x); r.py.push_back(0.0f);
    }
    return r;
}

// Render `frames` frames of the rig and capture the last one.
bool renderRig(rhi::IRenderDevice& d, const Rig& rig, const std::string& out,
               std::vector<float>* sx = nullptr, std::vector<float>* sy = nullptr) {
    const int kFrames = 20;
    for (int f = 0; f < kFrames; ++f) {
        d.setCamera(0.0f, 0.0f, -14.0f, 1.57079633f, 0.0f, 70.0f);
        if (f == kFrames - 1) d.armCapture(out.c_str());
        auto frame = d.beginFrame();
        if (frame.valid) {
            const float white[4] = { 1, 1, 1, 1 };
            const float noEmis[4] = { 0, 0, 0, 0 };
            const float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            for (auto m : rig.panels) d.drawMeshEmissive(frame, m, rig.albedo, white, noEmis, T);
        }
        d.endFrame(frame);
        if (f == kFrames - 1 && sx && sy) {
            sx->assign(rig.px.size(), 0.0f); sy->assign(rig.px.size(), 0.0f);
            for (size_t i = 0; i < rig.px.size(); ++i)
                d.worldToScreen(rig.px[i], rig.py[i], -0.1f, (*sx)[i], (*sy)[i]);
        }
    }
    return d.captureFrame(out.c_str());
}

void runGpuPart(rhi::IRenderDevice& device, const std::string& outDir) {
    x3::logInfo("--- PART B: r_clusterlights 0 vs 1 on the real device ---");
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(outDir, ec);

    // Kill every ambient source so the ONLY light on these panels is a point
    // light. Same discipline as --test-primlight: if a panel is dark, nothing is
    // reaching it, and there is nowhere for a photon to hide.
    device.setAmbient(0.0f, 0.0f, 0.0f);
    device.setIblIntensity(0.0f);
    rhi::IRenderDevice::SkyParams sky{};
    sky.enabled = false; sky.sunLight = 0.0f;
    device.setSkyParams(sky);
    device.setExposure(1.0f);

    // ---- Make each captured frame a PURE FUNCTION of the scene -------------
    // B1 compares two SEQUENTIAL renders for bit-identity, so anything with a
    // frame-to-frame memory would make them differ for reasons that have nothing
    // to do with lighting:
    //   * TAA carries a Halton JITTER PHASE and a history buffer. The phase keeps
    //     advancing across both renders, so run A and run B jitter differently
    //     and every geometric edge lands on different sub-pixels. (This is what
    //     the first version of this test actually measured: ~33k differing pixels,
    //     all of them edges, with the light assignment already provably correct.)
    //   * AUTO-EXPOSURE adapts over time, so run B starts from run A's adapted
    //     value while run A started from boot.
    // Turning both off makes the comparison mean what it claims to mean.
    rhi::IRenderDevice::PostFXParams px{};
    px.taa          = false;
    px.autoExposure = false;
    px.velocity     = false;
    device.setPostFX(px);

    device.beginUploadBatch();
    const int kPanels = 7;
    Rig rig = buildRig(device, kPanels);

    // ---------------------------------------------------------------------
    // B1. THE EQUIVALENCE PROOF. With a light set that FITS in the legacy 64,
    // the clustered path must produce a BIT-IDENTICAL image.
    //
    // Why that must hold, exactly: pointAtten() returns exactly 0.0 at or beyond
    // a light's range, and the froxel assignment uses a PADDED radius, so every
    // light that could contribute anything non-zero to a fragment is guaranteed
    // to be in that fragment's froxel list. The lights clustering drops are
    // therefore contributing exactly +0.0f, and the surviving ones are visited in
    // the SAME ascending order. Same addends, same order, same bits.
    //
    // So any difference at all means a light landed in the wrong froxel. This is
    // the end-to-end test of the shader's fragment->froxel lookup — including the
    // gl_FragCoord.y-is-top convention, which is exactly the kind of thing that
    // silently flips a grid upside down.
    // ---------------------------------------------------------------------
    {
        std::vector<rhi::PointLight> lights;
        for (int i = 0; i < kPanels; ++i) {
            rhi::PointLight L{};
            L.pos[0] = rig.px[i]; L.pos[1] = 1.1f; L.pos[2] = -2.0f;
            L.range = 3.2f;
            L.color[0] = 4.0f; L.color[1] = 3.4f; L.color[2] = 2.5f;
            lights.push_back(L);
        }
        // Pad out to 48 with lamps scattered through the scene volume so the test
        // exercises real multi-froxel assignment, not just seven isolated spheres.
        for (int i = 0; (int)lights.size() < 48; ++i) {
            rhi::PointLight L{};
            L.pos[0] = -9.0f + (float)((i * 3) % 19);
            L.pos[1] = -3.0f + (float)((i * 5) % 7);
            L.pos[2] = -4.0f + (float)((i * 7) % 11);
            L.range = 2.0f + (float)(i % 4);
            L.color[0] = 0.9f; L.color[1] = 1.2f; L.color[2] = 1.6f;
            lights.push_back(L);
        }
        device.setPointLights(lights.data(), (uint32_t)lights.size());

        const std::string a = outDir + "/ab_48lights_legacy.png";
        const std::string b = outDir + "/ab_48lights_clustered.png";
        device.setClusterLights(false);
        const bool okA = renderRig(device, rig, a);
        device.setClusterLights(true);
        const bool okB = renderRig(device, rig, b);

        if (!okA || !okB) { check(false, "B1 A/B captures written"); return; }
        const Img ia = loadImg(a), ib = loadImg(b);
        const size_t diff = pixelDiff(ia, ib);
        const auto st = device.stats();
        x3::logInfo("  [cluster] 48-light frame: " + std::to_string(st.clusterLights) + " lights, " +
                    std::to_string(st.clusterVisible) + " visible, " + std::to_string(st.clusterAssignments) +
                    " assignments, " + std::to_string(st.clusterOverflows) + " overflow, max froxel load " +
                    std::to_string(st.clusterMaxLoad) + ", " + fmt(st.clusterCpuMs, 3) + " ms CPU");
        check(diff == 0,
              "B1 EQUIVALENCE: 48 lights, r_clusterlights 0 vs 1 render BIT-IDENTICALLY (" +
              (diff == (size_t)-1 ? std::string("capture size mismatch") : std::to_string(diff) + " differing pixels") + ")");
        check(meanLuma(ib) > 3.0,
              "B1b the equivalence frame is actually LIT (mean luma " + fmt(meanLuma(ib), 2) +
              ") — two identical BLACK frames would prove nothing");
        check(st.clusterAssignments > 0 && st.clusterVisible > 0,
              "B1c the clustered frame really used the froxel path (" +
              std::to_string(st.clusterAssignments) + " assignments)");
    }

    // ---------------------------------------------------------------------
    // B2. PAST THE OLD CEILING. 300 lights, where the panel lamps sit at HIGH
    // indices (200+). The legacy path keeps the first 64 in submission order and
    // physically cannot see them, so those panels are BLACK. The clustered path
    // must light them.
    // ---------------------------------------------------------------------
    {
        std::vector<rhi::PointLight> lights;
        // 240 decoys parked well outside the frustum: they occupy the low indices
        // the legacy path keeps, and (being outside) they cost the clustered path
        // nothing at all — which the culled counter proves.
        for (int i = 0; i < 240; ++i) {
            rhi::PointLight L{};
            L.pos[0] = 300.0f + (float)i; L.pos[1] = 200.0f; L.pos[2] = -400.0f;
            L.range = 1.0f;
            L.color[0] = L.color[1] = L.color[2] = 1.0f;
            lights.push_back(L);
        }
        // THEN the lamps that actually light the panels, at indices 240..246.
        for (int i = 0; i < kPanels; ++i) {
            rhi::PointLight L{};
            L.pos[0] = rig.px[i]; L.pos[1] = 1.1f; L.pos[2] = -2.0f;
            L.range = 3.2f;
            L.color[0] = 4.0f; L.color[1] = 3.4f; L.color[2] = 2.5f;
            lights.push_back(L);
        }
        while (lights.size() < 300) {
            rhi::PointLight L{};
            L.pos[0] = -400.0f; L.pos[1] = -300.0f; L.pos[2] = -500.0f;
            L.range = 1.0f;
            lights.push_back(L);
        }
        device.setPointLights(lights.data(), (uint32_t)lights.size());

        const std::string a = outDir + "/ab_300lights_legacy.png";
        const std::string b = outDir + "/ab_300lights_clustered.png";
        std::vector<float> sx, sy;
        device.setClusterLights(false);
        const bool okA = renderRig(device, rig, a, &sx, &sy);
        device.setClusterLights(true);
        const bool okB = renderRig(device, rig, b, &sx, &sy);
        if (!okA || !okB) { check(false, "B2 A/B captures written"); return; }

        const Img ia = loadImg(a), ib = loadImg(b);
        const auto st = device.stats();
        x3::logInfo("  [cluster] 300-light frame: " + std::to_string(st.clusterLights) + " lights, " +
                    std::to_string(st.clusterVisible) + " visible, " + std::to_string(st.clusterCulled) +
                    " culled, " + std::to_string(st.clusterAssignments) + " assignments, " +
                    fmt(st.clusterCpuMs, 3) + " ms CPU");

        check(st.clusterLights == 300,
              "B2 the device accepted all 300 lights (old cap was 64) — got " +
              std::to_string(st.clusterLights));
        check(st.clusterCulled >= 240,
              "B2b the " + std::to_string(st.clusterCulled) +
              " off-screen decoys were CULLED before any froxel work");

        // Panel-by-panel: dark under legacy, lit under clustered.
        int litClustered = 0, litLegacy = 0;
        for (int i = 0; i < kPanels; ++i) {
            const int cx = (int)(sx[i] + 0.5f), cy = (int)(sy[i] + 0.5f);
            const double la = blockLuma(ia, cx, cy, 14);
            const double lb = blockLuma(ib, cx, cy, 14);
            if (la > 12.0) ++litLegacy;
            if (lb > 12.0) ++litClustered;
        }
        check(litLegacy == 0,
              "B2c LEGACY leaves all " + std::to_string(kPanels) +
              " panels BLACK — their lamps are at index 240+ and the 64-light array never reaches them (" +
              std::to_string(litLegacy) + " lit)");
        check(litClustered == kPanels,
              "B2d CLUSTERED lights all " + std::to_string(kPanels) + " panels (" +
              std::to_string(litClustered) + " lit) — the cap is genuinely raised, not just accepted");
        check(meanLuma(ib) > meanLuma(ia) + 1.0,
              "B2e the clustered frame is measurably brighter overall (mean luma " +
              fmt(meanLuma(ia), 2) + " -> " + fmt(meanLuma(ib), 2) + ")");
    }

    // Leave the device in the shipping default so nothing downstream inherits it.
    device.setClusterLights(false);
}

// ===========================================================================
// PART C — THE STRESS SCENE. A neon street corridor with 256 lights, captured
// A/B from the SAME camera with r_clusterlights 0 and 1, plus the froxel
// occupancy heatmap. This is the "look at it and give a verdict" artifact; the
// numeric assertions here are deliberately weak because the point of these
// three images is to be LOOKED AT.
// ===========================================================================
struct NeonScene {
    std::vector<rhi::MeshHandle> meshes;
    std::vector<std::array<float, 16>> xforms;
    rhi::TextureHandle tex{};
    std::vector<rhi::PointLight> lights;
};

std::array<float, 16> identityT() {
    return { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
}

// A 60 m street: road slab, two facade walls, cross-street pilasters, and
// `kNeon` neon tubes down both sides. Every light is a short-range (4-6 m) local
// source, which is exactly the case clustering is for: hundreds of lights, none
// of which reaches more than a few metres.
NeonScene buildNeonStreet(rhi::IRenderDevice& d, int neonCount) {
    NeonScene s;
    const uint8_t grey[4] = { 120, 122, 128, 255 };
    s.tex = d.createTexture(grey, 1, 1, /*srgb*/ true);

    auto addBox = [&](float hx, float hy, float hz, float cx, float cy, float cz) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
        s.meshes.push_back(d.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                        m.index.data(), (uint32_t)m.index.size()));
        s.xforms.push_back(identityT());
    };

    const float len = 60.0f, halfW = 5.0f;
    addBox(halfW, 0.15f, len * 0.5f, 0.0f, -0.15f, len * 0.5f);          // road
    addBox(0.3f, 6.0f, len * 0.5f, -halfW - 0.3f, 6.0f, len * 0.5f);     // left facade
    addBox(0.3f, 6.0f, len * 0.5f,  halfW + 0.3f, 6.0f, len * 0.5f);     // right facade
    for (int i = 0; i < 12; ++i) {                                        // pilasters
        const float z = 3.0f + (float)i * 5.0f;
        addBox(0.45f, 5.0f, 0.45f, -halfW + 0.5f, 5.0f, z);
        addBox(0.45f, 5.0f, 0.45f,  halfW - 0.5f, 5.0f, z);
    }

    // The neon. Alternating hot magenta / cyan / amber, staggered in height so
    // the pools overlap and the froxel lists actually stack.
    const float hue[3][3] = { { 5.0f, 0.6f, 3.6f }, { 0.5f, 3.4f, 5.0f }, { 5.0f, 2.6f, 0.5f } };
    for (int i = 0; i < neonCount; ++i) {
        rhi::PointLight L{};
        const int side = i & 1;
        const float t = (float)(i / 2) / (float)std::max(1, neonCount / 2 - 1);
        L.pos[0] = side ? (halfW - 0.9f) : (-halfW + 0.9f);
        L.pos[1] = 1.4f + 3.6f * (float)((i / 2) % 3) / 2.0f;
        L.pos[2] = 1.5f + t * (len - 3.0f);
        L.range  = 4.2f + 1.4f * (float)(i % 3);
        const float* c = hue[(i / 2) % 3];
        L.color[0] = c[0]; L.color[1] = c[1]; L.color[2] = c[2];
        s.lights.push_back(L);
    }
    return s;
}

bool renderNeon(rhi::IRenderDevice& d, const NeonScene& s, const std::string& out, int frames = 18) {
    for (int f = 0; f < frames; ++f) {
        // Standing in the street at eye height, looking straight down it.
        d.setCamera(0.0f, 2.0f, -6.0f, 1.57079633f, -0.06f, 68.0f);
        if (f == frames - 1) d.armCapture(out.c_str());
        auto frame = d.beginFrame();
        if (frame.valid) {
            const float white[4] = { 1, 1, 1, 1 };
            const float noEmis[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < s.meshes.size(); ++i)
                d.drawMeshEmissive(frame, s.meshes[i], s.tex, white, noEmis, s.xforms[i].data());
        }
        d.endFrame(frame);
    }
    return d.captureFrame(out.c_str());
}

void runStressPart(rhi::IRenderDevice& device, const std::string& outDir) {
    x3::logInfo("--- PART C: 256-light neon street, A/B + froxel heatmap ---");
    const int kNeon = 256;

    device.beginUploadBatch();
    NeonScene s = buildNeonStreet(device, kNeon);
    device.setPointLights(s.lights.data(), (uint32_t)s.lights.size());
    device.setAmbient(0.02f, 0.02f, 0.03f);
    device.setIblIntensity(0.0f);

    const std::string legacy   = outDir + "/neon256_legacy.png";
    const std::string clustered = outDir + "/neon256_clustered.png";
    const std::string heat      = outDir + "/neon256_froxel_heatmap.png";

    device.setClusterLights(false);
    const bool okA = renderNeon(device, s, legacy);
    device.setClusterLights(true);
    const bool okB = renderNeon(device, s, clustered);

    const auto st = device.stats();
    x3::logInfo("  [cluster] neon street: " + std::to_string(st.clusterLights) + " lights, " +
                std::to_string(st.clusterVisible) + " visible, " + std::to_string(st.clusterAssignments) +
                " assignments, " + std::to_string(st.clusterOverflows) + " overflow across " +
                std::to_string(st.clusterOverflowed) + " froxel(s), max load " +
                std::to_string(st.clusterMaxLoad) + "/" + std::to_string(kMaxLightsPerCluster) +
                ", " + fmt(st.clusterCpuMs, 3) + " ms CPU");

    // The froxel occupancy heatmap (r_debugview 6) — the one-frame answer to
    // "where is this scene dense, and is anything overflowing?".
    device.setDebugView(6);
    const bool okH = renderNeon(device, s, heat, 4);
    device.setDebugView(0);

    check(okA && okB && okH, "C1 stress captures written to " + outDir);
    if (okA && okB) {
        const Img ia = loadImg(legacy), ib = loadImg(clustered);
        check(meanLuma(ib) > meanLuma(ia),
              "C2 the 256-light street is BRIGHTER clustered than legacy (mean luma " +
              fmt(meanLuma(ia), 2) + " -> " + fmt(meanLuma(ib), 2) +
              ") — legacy can only ever see 64 of the 256 neon tubes");
        // WHERE the two differ is the real claim, and it is not "everywhere".
        // The neon nearest the camera occupies the LOW light indices, so the
        // near half of the street is identical in both captures. The far half is
        // lit by tubes at index 64+, which the legacy array cannot reach — so the
        // street should FALL OFF A CLIFF into black partway down under legacy and
        // stay lit under clustered. Measure exactly that, on the road surface,
        // rather than a whole-frame pixel count (which the identical near half
        // dilutes).
        const double nearLeg = blockLuma(ia, ia.w / 2, (int)(ia.h * 0.62), 40);
        const double nearClu = blockLuma(ib, ib.w / 2, (int)(ib.h * 0.62), 40);
        check(std::fabs(nearLeg - nearClu) < 1.0,
              "C2b the NEAR street is unchanged (luma " + fmt(nearLeg, 2) + " vs " + fmt(nearClu, 2) +
              ") — those tubes are inside the legacy 64, so both paths must agree there");

        // Scan DOWN the middle of the road for the row where the legacy capture
        // falls off its cliff, rather than guessing a magic scanline: find the
        // darkest road row in the legacy image, then compare that same row in the
        // clustered one. That is the "the street just stops" moment, located
        // automatically so it survives any camera tweak.
        int darkestRow = (int)(ia.h * 0.50); double darkest = 1e30;
        for (int y = (int)(ia.h * 0.46); y <= (int)(ia.h * 0.58); ++y) {
            const double l = blockLuma(ia, ia.w / 2, y, 10);
            if (l >= 0 && l < darkest) { darkest = l; darkestRow = y; }
        }
        const double farLeg = blockLuma(ia, ia.w / 2, darkestRow, 10);
        const double farClu = blockLuma(ib, ib.w / 2, darkestRow, 10);
        check(farClu > farLeg * 1.5 && farClu > 10.0,
              "C2c the FAR street is where legacy dies: at its darkest road row (y=" +
              std::to_string(darkestRow) + ") luma " + fmt(farLeg, 2) + " (legacy) -> " +
              fmt(farClu, 2) + " (clustered), " + fmt(farClu / std::max(farLeg, 0.01), 2) +
              "x — those tubes are at light index 64+ and only the clustered path can see them");
        x3::logInfo("  [look] whole-frame differing pixels: " + std::to_string(pixelDiff(ia, ib)) +
                    " of " + std::to_string((size_t)ia.w * ia.h) +
                    " (concentrated in the far half, as expected)");
    }
    device.setClusterLights(false);
}

// ===========================================================================
// PART D — PERF. Frame time at 64 vs 512 lights, legacy vs clustered, on the
// same neon street from the same camera. gpuFrameMs comes from the engine's own
// timestamp queries (read back with frames-in-flight latency), so these are
// measured GPU numbers, not wall-clock guesses.
// ===========================================================================
void runPerfPart(rhi::IRenderDevice& device, const std::string& outDir) {
    x3::logInfo("--- PART D: frame time, 64 vs 512 lights, legacy vs clustered ---");
    device.beginUploadBatch();
    NeonScene s = buildNeonStreet(device, 512);
    device.setAmbient(0.02f, 0.02f, 0.03f);
    device.setIblIntensity(0.0f);

    struct Row { int lights; bool clustered; double gpuMs; double cpuMs; };
    std::vector<Row> rows;

    // Fragment-bound camera: down at road level with a wide FOV so the street
    // FILLS the frame. Point-light cost lives in the fragment stage, so a camera
    // that leaves half the frame on the clear colour measures mostly nothing.
    auto perfFrame = [&]() {
        device.setCamera(0.0f, 0.7f, -3.0f, 1.57079633f, -0.02f, 95.0f);
        auto fr = device.beginFrame();
        if (fr.valid) {
            const float white[4] = { 1, 1, 1, 1 };
            const float noEmis[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < s.meshes.size(); ++i)
                device.drawMeshEmissive(fr, s.meshes[i], s.tex, white, noEmis, s.xforms[i].data());
        }
        device.endFrame(fr);
    };

    for (int lightCount : { 64, 256, 512 }) {
        for (int mode = 0; mode < 2; ++mode) {
            device.setPointLights(s.lights.data(), (uint32_t)lightCount);
            device.setClusterLights(mode != 0);
            for (int f = 0; f < 15; ++f) perfFrame();          // warm up
            double acc = 0, accCpu = 0; int n = 0;
            for (int f = 0; f < 60; ++f) {
                perfFrame();
                const auto st = device.stats();
                if (st.gpuFrameMs > 0.0f) { acc += st.gpuFrameMs; accCpu += st.clusterCpuMs; ++n; }
            }
            rows.push_back({ lightCount, mode != 0, n ? acc / n : -1.0, n ? accCpu / n : 0.0 });
        }
    }

    x3::logInfo("  scene lights | path      | GPU main-pass ms | cluster CPU ms | lights ACTUALLY rendered");
    for (const auto& r : rows) {
        char lc[8]; std::snprintf(lc, sizeof(lc), "%4d", r.lights);
        // THE POINT: the legacy path physically cannot render more than 64. Its
        // cost does not rise with scene light count because it silently DROPS
        // everything past the 64th — which is the bug, not a performance win.
        const int rendered = r.clustered ? r.lights : std::min(r.lights, 64);
        x3::logInfo(std::string("        ") + lc + " | " + (r.clustered ? "clustered" : "legacy   ") +
                    " |      " + fmt(r.gpuMs, 3) + "       |     " + fmt(r.cpuMs, 3) +
                    "      | " + std::to_string(rendered));
    }

    auto find = [&](int n, bool clu) {
        for (const auto& r : rows) if (r.lights == n && r.clustered == clu) return r.gpuMs;
        return -1.0;
    };
    const double leg64 = find(64, false), clu64 = find(64, true);
    const double clu512 = find(512, true);

    // D1 — THE APPLES-TO-APPLES COMPARISON. At 64 lights BOTH paths render the
    // same content, so this is the only place the per-pixel claim can be measured
    // honestly: legacy evaluates all 64 for every pixel, clustered evaluates only
    // the ones whose sphere reaches that pixel's froxel.
    check(leg64 > 0 && clu64 > 0 && clu64 <= leg64 * 1.02,
          "D1 SAME CONTENT (64 lights), clustered is not slower: legacy " + fmt(leg64, 3) +
          " ms -> clustered " + fmt(clu64, 3) + " ms (" +
          fmt(leg64 / std::max(clu64, 1e-6), 2) + "x)");

    // D2 — WHAT THE EXTRA COST BUYS. There is no legacy number to compare 512
    // against: legacy at 512 renders 64 and throws 448 away, so its "cost" is a
    // measurement of a scene it is not drawing. State the honest version instead.
    check(clu512 > 0 && clu512 < leg64 * 2.0,
          "D2 8x THE LIGHTS FOR UNDER 2x THE TIME: legacy renders 64 lights in " + fmt(leg64, 3) +
          " ms; clustered renders 512 in " + fmt(clu512, 3) + " ms (" +
          fmt(clu512 / std::max(leg64, 1e-6), 2) + "x the time for 8x the lights)");
    device.setClusterLights(false);
}

} // namespace

int runClusterLightTest(x3::rhi::IRenderDevice& device, const std::string& outDir) {
    cl_pass = cl_fail = 0;
    x3::logInfo("=== --test-clusterlights: CLUSTERED (froxel) FORWARD LIGHTING ===");
    x3::logInfo("  grid " + std::to_string(kClusterGridX) + " x " + std::to_string(kClusterGridY) +
                " x " + std::to_string(kClusterGridZ) + " = " + std::to_string(kClusterCount) +
                " froxels, " + std::to_string(kMaxLightsPerCluster) + " lights/froxel, " +
                std::to_string(kMaxSceneLights) + " scene light cap");

    runCpuPart();
    runGpuPart(device, outDir);
    runStressPart(device, outDir);
    runPerfPart(device, outDir);

    x3::logInfo("=== --test-clusterlights: " + std::to_string(cl_pass) + " passed, " +
                std::to_string(cl_fail) + " failed ===");
    return cl_fail == 0 ? 0 : 1;
}

} // namespace x3::game
