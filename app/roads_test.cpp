// ============================================================================
// --test-echoroads — headless self-test for the LIFTED EchoRoads ROAD GRAPH.
//
// LIFT A. app/world_hosts/echo_roads.{h,cpp} and app/world_hosts/
// echo_heightfield.h were ported from the echotropolis lineage (branch
// inspx/city-blocks, tip bc27ce6d) onto the main lineage. Nothing else came
// with them and nothing on main was changed to suit them.
//
// THE ACCEPTANCE GATE IS EQUIVALENCE, NOT PLAUSIBILITY. EchoRoads is fully
// deterministic (hash discipline, no rand, no time, no globals), so a lift is
// correct iff it produces the IDENTICAL RoadGraph. This test therefore builds
// the graph from a fixed, closed-form, in-memory heightfield (no assets, no
// GPU, no environment) and locks a STABLE CHECKSUM over everything the graph
// exposes. The same file compiled against the SOURCE branch must print the
// same checksum; if it does not, the lift changed something.
//
// WHAT IT ASSERTS
//   R0  build() succeeds on a loaded heightfield and produces a non-empty graph
//   R1  STRUCTURE CHECKSUM — node count, edge count, and per edge the
//       RoadClass / paved width / lane counts / endpoints / sample count /
//       length, hashed in order
//   R2  CENTRELINE CHECKSUM — every centreline sample's position (x,y,z),
//       tangent (tx,tz) and bank, hashed in order (quantised to 1 mm / 1e-5
//       so the gate survives a compiler's FP scheduling; the bit-exact hash
//       is reported alongside it)
//   R3  DETERMINISM — three independent builds from the same heightfield, and
//       one build from a freshly regenerated heightfield, all agree on every
//       checksum and every count
//   R4  THE ZIGZAG LAW — every surviving edge is within its class curvature
//       limit (+ the law's own 15% grace), with exactly one documented
//       exemption: the lawExempt mine-lot cul-de-sac. If the law is removed
//       or weakened, edges survive that violate this and the test goes red
//   R5  JUNCTIONS CLUSTER — junctionCount() > 0, matches its golden, and is
//       strictly LESS than the raw candidate count recomputed here from the
//       graph (i.e. the 14 m cluster merge really merged)
//   R6  sampleFrontage() — every emitted point sits at exactly the documented
//       offset, sample +/- perp * (width/2 + curb + walk + setback), with
//       yaw = atan2(tangent) +/- 90 deg, on both sides, and checksums stably
//   R7  laneCenterOffset() — the documented 3.4 m lane width, lane centres at
//       +/-1.7 / +/-5.1 / +/-8.5, forward positive and backward mirrored
//   R8  the collision mesh, lamp slice and diagnostics are populated/consistent
//
// PROOF THAT THIS TEST CAN FAIL (all three were run on inspx/lift-roads and
// then reverted; the file is byte-identical to the source branch again):
//   NC1  kRingFixed[2].x  480 -> 484   (ONE ring control point, moved 4 m)
//        -> R1 structure, R2 centreline and R6 frontage checksums all red.
//   NC2  kLaneWidth       3.4 -> 3.5   (ONE lane-width constant)
//        -> R7 red (checksum AND the explicit lane-centre assertion). R1/R2
//           stay green, correctly: lane width is not part of the graph.
//   NC3  PHASE 1.9 skipped entirely    (the zigzag law removed)
//        -> R4 red with 4 body violators (the freeway ring at 10.2 deg/m, two
//           ramps, an avenue at 13.1 deg/m), plus every checksum.
// ============================================================================
#include "roads_test.h"

#include "world_hosts/echo_roads.h"
#include "world_hosts/echo_heightfield.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x3::game {
namespace {

int g_pass = 0, g_fail = 0;

void ok(bool cond, const std::string& what) {
    if (cond) { ++g_pass; x3::logInfo("[roads-test]   PASS  " + what); }
    else      { ++g_fail; x3::logError("[roads-test]   FAIL  " + what); }
}

std::string f2s(double v, int dp = 4) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", dp, v);
    return b;
}
std::string hex64(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%016llx", (unsigned long long)v);
    return b;
}

// ---------------------------------------------------------------------------
// FNV-1a 64. Two flavours: QUANTISED (the gate — 1 mm on positions, 1e-5 on
// unit vectors, so the checksum survives a compiler reordering an add) and
// EXACT (raw IEEE bits — reported for information; it is the stronger claim
// when it matches, but it is not what the gate rides on).
// ---------------------------------------------------------------------------
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void byte(uint8_t b) { h ^= b; h *= 1099511628211ull; }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) byte((uint8_t)(v >> (i * 8))); }
    void i64(int64_t v)  { u64((uint64_t)v); }
    void u32(uint32_t v) { u64(v); }
    // Quantised float: round-half-away-from-zero at 1/scale, as an integer.
    void q(float v, double scale) {
        const double s = (double)v * scale;
        i64((int64_t)(s >= 0.0 ? std::floor(s + 0.5) : std::ceil(s - 0.5)));
    }
    void bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
};

constexpr double kQPos = 1000.0;    // 1 mm
constexpr double kQDir = 100000.0;  // 1e-5 (unit vectors / radians)

// ---------------------------------------------------------------------------
// THE FIXED CONFIG. A closed-form island: a southern coastline, a coastal
// plain, and a mesa crowned at EchoRoads' own probe origin (-20, 760) so the
// V4.1 radial rim probe, the harbour boulevard waterline probe, the block fan
// and the mine spur all have real terrain to find. No file, no asset, no rand:
// this function IS the seed. Changing it changes the goldens, on purpose.
// ---------------------------------------------------------------------------
constexpr int   kHfN     = 1024;      // texels per side over Heightfield::kMeters
constexpr float kCrownX  = -20.0f;    // must match echo_roads.cpp's probe origin
constexpr float kCrownZ  = 760.0f;

float terrainMeters(float x, float z) {
    // Coastline: a straight shore running WNW->ESE across the south.
    const float shoreZ = 460.0f - 0.22f * x;
    const float d      = z - shoreZ;                       // + = inland
    const float land   = (d <= 0.0f) ? d * 0.55f                       // sea bed
                                     : (d * 0.35f < 40.0f ? d * 0.35f : 40.0f);
    // The mesa: flat crown out to 280 m, cliff-ish falloff to 560 m.
    const float dx = x - kCrownX, dz = z - kCrownZ;
    const float r  = std::sqrt(dx * dx + dz * dz);
    float mesa = 0.0f;
    if (r <= 280.0f)      mesa = 210.0f;
    else if (r < 560.0f)  mesa = 210.0f * (560.0f - r) / 280.0f;
    // The mesa only exists on land; it is faded out over the last 60 m of shore
    // so the rim probe finds a real, position-dependent lip rather than a
    // perfect circle.
    float shoreFade = d / 60.0f;
    shoreFade = shoreFade < 0.0f ? 0.0f : (shoreFade > 1.0f ? 1.0f : shoreFade);
    // Low-frequency relief so the smoothing / curvature passes have real work.
    const float relief = 6.0f * std::sin(x * 0.0037f) * std::cos(z * 0.0041f)
                       + 3.0f * std::sin((x + z) * 0.0091f);
    float reliefFade = d / 40.0f;
    reliefFade = reliefFade < 0.0f ? 0.0f : (reliefFade > 1.0f ? 1.0f : reliefFade);
    float h = land + mesa * shoreFade + relief * reliefFade;
    if (h < -60.0f)  h = -60.0f;
    if (h >  250.0f) h =  250.0f;
    return h;
}

// Encode terrainMeters() into a real Heightfield (the same 16-bit normalised
// grid the shipped island uses), so the test exercises the production sampler
// including its bilinear filter and sea-level bias.
void makeHeightfield(Heightfield& hf) {
    hf.w = hf.h = kHfN;
    hf.px.assign((size_t)kHfN * kHfN, (uint16_t)0);
    for (int j = 0; j < kHfN; ++j) {
        const float z = ((float)j / (float)(kHfN - 1) - 0.5f) * Heightfield::kMeters;
        for (int i = 0; i < kHfN; ++i) {
            const float x = ((float)i / (float)(kHfN - 1) - 0.5f) * Heightfield::kMeters;
            float hn = Heightfield::kSeaNorm + terrainMeters(x, z) / Heightfield::kScale;
            hn = hn < 0.0f ? 0.0f : (hn > 1.0f ? 1.0f : hn);
            hf.px[(size_t)j * kHfN + i] = (uint16_t)(hn * 65535.0f + 0.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// Checksums over the emitted RoadGraph.
// ---------------------------------------------------------------------------
struct GraphSums {
    uint64_t structure  = 0;   // R1: topology + per-edge class/width/lanes/length
    uint64_t centre     = 0;   // R2: quantised sample positions/tangents/bank
    uint64_t centreBits = 0;   // R2: bit-exact same, informational
    uint64_t lanes      = 0;   // R7: laneCenterOffset over every edge
    size_t   nodes = 0, edges = 0, samples = 0;
    double   pavedM = 0.0;
};

GraphSums summarise(const RoadGraph& g) {
    GraphSums s;
    Hash st, ce, cb, ln;
    st.u64(g.nodes.size());
    st.u64(g.edges.size());
    for (const RoadNode& n : g.nodes) { st.q(n.x, kQPos); st.q(n.z, kQPos); }
    for (const RoadEdge& e : g.edges) {
        st.u32((uint32_t)e.cls);
        st.q(e.width, kQPos);
        st.i64(e.lanesF); st.i64(e.lanesB);
        st.u32(e.a); st.u32(e.b);
        st.u64(e.center.size());
        st.q(e.length, kQPos);
        for (const RoadSample& p : e.center) {
            ce.q(p.x, kQPos); ce.q(p.y, kQPos); ce.q(p.z, kQPos);
            ce.q(p.tx, kQDir); ce.q(p.tz, kQDir); ce.q(p.bank, kQDir);
            cb.bits(p.x);  cb.bits(p.y);  cb.bits(p.z);
            cb.bits(p.tx); cb.bits(p.tz); cb.bits(p.bank);
        }
        for (int lane = 0; lane < 3; ++lane) {
            ln.q(RoadGraph::laneCenterOffset(e, lane, true),  kQPos);
            ln.q(RoadGraph::laneCenterOffset(e, lane, false), kQPos);
        }
        s.samples += e.center.size();
        s.pavedM  += e.length;
    }
    s.structure = st.h; s.centre = ce.h; s.centreBits = cb.h; s.lanes = ln.h;
    s.nodes = g.nodes.size(); s.edges = g.edges.size();
    return s;
}

// ---------------------------------------------------------------------------
// GOLDENS. Originally recorded on inspx/lift-roads and re-verified against the
// SOURCE branch inspx/city-blocks by compiling THIS FILE there unchanged.
//
// REBASELINED 2026-08-15 for commit 4852d14c ("fix(echo): pave the ground
// streets — crown lanes, corridor veto"). That fix promoted the five CROWN
// LANES from flat buildCrown GLB slabs into first-class EchoRoads Avenues
// (echo_roads.cpp §"1d-bis THE CROWN GRID"), draped/curbed/lamped and swept by
// the junction pass. That intentionally ADDS 5 edges / +10 nodes (2 endpoints
// each) / ~671 centreline samples / more junctions + frontage to the graph.
// The generator landed on main after the 2026-08-03 goldens (b810aa97) but the
// matching golden bump did not travel with the fold, so this gate was stale.
// The new graph was verified correct before rebaselining: every INVARIANT test
// stayed green (R3 determinism ×2 + across 3 process runs; R4 zigzag law 0
// violators; R5 clustering 108→68; R6 frontage offset/yaw sub-mm; R7/R8), the
// count deltas match the crown-grid arithmetic exactly, and the crown renders
// as a coherent orthogonal avenue grid with real junction patches (not garbage).
//   old → new:  nodes 87→97  edges 44→49  samples 2519→3190
//               junctions 57→68  frontagePts 594→860
// ---------------------------------------------------------------------------
constexpr uint64_t kGoldStructure   = 0xce3183d4a30f76b5ull;
constexpr uint64_t kGoldCentre      = 0x1f23544a1ef90081ull;
constexpr uint64_t kGoldLanes       = 0x6a7a4f1ead51f36cull;
constexpr uint64_t kGoldFrontage    = 0x7cfca13d834f61a0ull;
constexpr size_t   kGoldNodes       = 97;
constexpr size_t   kGoldEdges       = 49;
constexpr size_t   kGoldSamples     = 3190;
constexpr uint32_t kGoldJunctions   = 68;
constexpr size_t   kGoldFrontagePts = 860;
// The bit-exact centreline hash is NOT a gate (a compiler may legally reorder
// an FMA); it is logged. On this toolchain (MSVC 14.50, Release) it is
// 0xc382a7f5f115ac79 after the 2026-08-15 rebaseline; a differing value on
// another toolchain is informational only — the gate rides the quantised hash.
constexpr uint64_t kNoteCentreBits  = 0xc382a7f5f115ac79ull;

// Bring-up only: print the goldens instead of asserting them.
constexpr bool kRecord = false;

void expectHash(uint64_t got, uint64_t want, const std::string& what) {
    if (kRecord) { x3::logInfo("[roads-test]   RECORD " + what + " = " + hex64(got)); return; }
    ok(got == want, what + " = " + hex64(got) +
                    (got == want ? "" : "  (expected " + hex64(want) + ")"));
}
void expectCount(size_t got, size_t want, const std::string& what) {
    if (kRecord) { x3::logInfo("[roads-test]   RECORD " + what + " = " + std::to_string(got)); return; }
    ok(got == want, what + " = " + std::to_string(got) +
                    (got == want ? "" : "  (expected " + std::to_string(want) + ")"));
}

// ---------------------------------------------------------------------------
// R4 helpers — the zigzag law, measured from the OUTSIDE (from the graph the
// consumers actually see), using echo_roads.cpp's own documented per-class
// limits and its own 15% execution grace.
// ---------------------------------------------------------------------------
float classLimitDegPerM(RoadClass c) {
    switch (c) {
        case RoadClass::Freeway: return 0.8f;
        case RoadClass::Ramp:    return 1.2f;
        default:                 return 2.5f;   // Avenue / HarborStreet
    }
}

// Worst heading change per metre along an edge, in degrees — the SAME metric
// PHASE 1.9 uses: acos(t_i . t_i+1) / the NOMINAL class sample step (4 m, or
// 2 m for ramps), not the measured segment length. Reproducing the law's own
// metric is the point; inventing a different one would test something else.
//
// `skipEnds` trims that many metres of arc off each end. THE LAW RUNS BEFORE
// PHASE 2 (junctions), and PHASE 2 then EXTENDS stop-short endpoints straight
// into their target corridor and recomputes tangents — a deliberate one-sample
// hand-off kink at the mouth of a junction patch, which the filled patch
// polygon covers. Those metres are junction geometry, not road body, so the
// law's guarantee is asserted over the body. Both numbers are reported.
float worstTurnDegPerM(const RoadEdge& e, float skipEnds) {
    if (e.center.size() < 3) return 0.0f;
    const float step = (e.cls == RoadClass::Ramp) ? 2.0f : 4.0f;   // kRampStep / kSampleStep
    // Arc position of every sample, so `skipEnds` is metres and not indices.
    std::vector<float> arc(e.center.size(), 0.0f);
    for (size_t i = 1; i < e.center.size(); ++i) {
        const float dx = e.center[i].x - e.center[i - 1].x;
        const float dz = e.center[i].z - e.center[i - 1].z;
        arc[i] = arc[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    const float total = arc.back();
    float worst = 0.0f;
    for (size_t i = 0; i + 1 < e.center.size(); ++i) {
        if (arc[i] < skipEnds || arc[i + 1] > total - skipEnds) continue;
        const RoadSample& a = e.center[i];
        const RoadSample& b = e.center[i + 1];
        float dot = a.tx * b.tx + a.tz * b.tz;
        dot = dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot);
        const float deg = std::acos(dot) * 57.2957795f / step;
        if (deg > worst) worst = deg;
    }
    return worst;
}

// PHASE 2 can only extend an endpoint by kJuncCapture * (wA + wB) / 2, i.e. at
// most 0.7 * 14 m for the widest pairing here — so 16 m of arc comfortably
// contains every hand-off the law never saw.
constexpr float kHandoffArc = 16.0f;

// ---------------------------------------------------------------------------
// R5 helper — recompute the RAW junction candidate count from the graph with
// echo_roads.cpp's own thresholds but WITHOUT the 14 m cluster merge. The
// build's junctionCount() must be strictly smaller: that gap IS the clustering.
// ---------------------------------------------------------------------------
uint32_t rawJunctionCandidates(const RoadGraph& g) {
    auto ground = [](const RoadEdge& e) {
        return e.cls == RoadClass::Avenue || e.cls == RoadClass::HarborStreet;
    };
    uint32_t n = 0;
    // Pass A analogue: non-freeway endpoints captured by a ground edge.
    for (size_t e = 0; e < g.edges.size(); ++e) {
        if (g.edges[e].cls == RoadClass::Freeway) continue;
        if (g.edges[e].center.size() < 2) continue;
        for (int endSel = 0; endSel < 2; ++endSel) {
            const RoadSample end = endSel ? g.edges[e].center.back()
                                          : g.edges[e].center.front();
            float best = 1e30f; size_t bo = (size_t)-1;
            for (size_t o = 0; o < g.edges.size(); ++o) {
                if (o == e || !ground(g.edges[o])) continue;
                for (const RoadSample& s : g.edges[o].center) {
                    const float dx = s.x - end.x, dz = s.z - end.z;
                    const float d = dx * dx + dz * dz;
                    if (d < best) { best = d; bo = o; }
                }
            }
            if (bo == (size_t)-1) continue;
            const float cap = 0.70f * 0.5f * (g.edges[e].width + g.edges[bo].width);
            if (std::sqrt(best) <= cap) ++n;
        }
    }
    // Pass B analogue: ground/ground closest approach inside the cross threshold.
    for (size_t a = 0; a < g.edges.size(); ++a) {
        if (!ground(g.edges[a])) continue;
        for (size_t b = a + 1; b < g.edges.size(); ++b) {
            if (!ground(g.edges[b])) continue;
            float bd = 1e30f;
            for (size_t i = 0; i < g.edges[a].center.size(); i += 2)
                for (size_t j = 0; j < g.edges[b].center.size(); j += 2) {
                    const float dx = g.edges[a].center[i].x - g.edges[b].center[j].x;
                    const float dz = g.edges[a].center[i].z - g.edges[b].center[j].z;
                    const float d = dx * dx + dz * dz;
                    if (d < bd) bd = d;
                }
            const float th = 0.55f * 0.5f * (g.edges[a].width + g.edges[b].width);
            if (bd < th * th) ++n;
        }
    }
    return n;
}

float wrapPi(float a) {
    const float twoPi = 6.28318530718f;
    while (a >  3.14159265359f) a -= twoPi;
    while (a < -3.14159265359f) a += twoPi;
    return a;
}

} // namespace

// ===========================================================================
bool runEchoRoadsSelfTest() {
    g_pass = g_fail = 0;

    x3::logInfo("[roads-test] LIFT A acceptance gate — EchoRoads road graph, "
                "lifted from inspx/city-blocks (echotropolis lineage) onto main.");
    x3::logInfo("[roads-test] fixed config: closed-form island heightfield, " +
                std::to_string(kHfN) + "x" + std::to_string(kHfN) + " over " +
                f2s(Heightfield::kMeters, 0) + " m, crown at (" + f2s(kCrownX, 0) +
                "," + f2s(kCrownZ, 0) + "). No assets, no GPU, no rand.");

    Heightfield hf;
    makeHeightfield(hf);
    ok(hf.ok(), "R0 synthetic heightfield loads");
    ok(hf.heightAt(kCrownX, kCrownZ) > 180.0f,
       "R0 crown datum is a real mesa (" + f2s(hf.heightAt(kCrownX, kCrownZ), 1) + " m)");
    ok(hf.heightAt(0.0f, 200.0f) < 0.0f,
       "R0 the south is sea (" + f2s(hf.heightAt(0.0f, 200.0f), 1) + " m)");

    // ---- R0/R1/R2: build once, checksum -----------------------------------
    HeadlessRenderDevice dev;
    EchoRoads roads;
    const bool built = roads.build(dev, hf);
    ok(built, "R0 EchoRoads::build() succeeded");
    if (!built) {
        x3::logError("[roads-test] build failed — nothing further can be asserted");
        return false;
    }

    const RoadGraph& g = roads.graph();
    const GraphSums s = summarise(g);

    x3::logInfo("[roads-test] graph: " + std::to_string(s.nodes) + " nodes, " +
                std::to_string(s.edges) + " edges, " + std::to_string(s.samples) +
                " centreline samples, " + f2s(s.pavedM, 1) + " m of centreline, " +
                std::to_string(roads.junctionCount()) + " junction patches.");
    x3::logInfo("[roads-test] CHECKSUM structure=" + hex64(s.structure) +
                " centre=" + hex64(s.centre) + " lanes=" + hex64(s.lanes));
    x3::logInfo("[roads-test] CHECKSUM centre(bit-exact)=" + hex64(s.centreBits) +
                (s.centreBits == kNoteCentreBits
                     ? std::string("  (== the value recorded on the SOURCE branch: the lift is "
                                   "bit-identical, not merely equivalent)")
                     : std::string("  (differs from the recorded ")
                           + hex64(kNoteCentreBits) +
                           " — informational only; the gate rides the quantised hash above)"));

    ok(s.edges > 0 && s.samples > 0, "R0 graph is non-empty");
    expectCount(s.nodes,   kGoldNodes,   "R1 node count");
    expectCount(s.edges,   kGoldEdges,   "R1 edge count");
    expectCount(s.samples, kGoldSamples, "R2 centreline sample count");
    expectHash(s.structure, kGoldStructure,
               "R1 structure checksum (class/width/lanes/endpoints/length)");
    expectHash(s.centre, kGoldCentre,
               "R2 centreline checksum (positions + tangents + bank)");
    expectHash(s.lanes, kGoldLanes, "R7 laneCenterOffset checksum over every edge");

    // ---- R3: determinism ---------------------------------------------------
    {
        bool allSame = true;
        for (int rep = 0; rep < 2; ++rep) {
            HeadlessRenderDevice d2;
            EchoRoads r2;
            if (!r2.build(d2, hf)) { allSame = false; break; }
            const GraphSums s2 = summarise(r2.graph());
            allSame = allSame && s2.structure == s.structure && s2.centre == s.centre
                              && s2.centreBits == s.centreBits && s2.lanes == s.lanes
                              && s2.nodes == s.nodes && s2.edges == s.edges
                              && s2.samples == s.samples
                              && r2.junctionCount() == roads.junctionCount();
        }
        ok(allSame, "R3 three independent builds from the same heightfield are identical "
                    "(including the bit-exact centreline hash)");

        Heightfield hf2;
        makeHeightfield(hf2);
        HeadlessRenderDevice d3;
        EchoRoads r3;
        const bool b3 = r3.build(d3, hf2);
        const GraphSums s3 = summarise(r3.graph());
        ok(b3 && s3.structure == s.structure && s3.centre == s.centre
              && s3.centreBits == s.centreBits,
           "R3 a freshly regenerated heightfield reproduces the same graph");
    }

    // ---- R4: the zigzag law -----------------------------------------------
    {
        int violators = 0, exemptLike = 0;
        float worstAll = 0.0f, worstRaw = 0.0f;
        std::string worstDesc;
        for (size_t i = 0; i < g.edges.size(); ++i) {
            const RoadEdge& e = g.edges[i];
            const float w   = worstTurnDegPerM(e, kHandoffArc);
            const float raw = worstTurnDegPerM(e, 0.0f);
            const float lim = classLimitDegPerM(e.cls) * 1.15f;   // the law's own grace
            if (raw > worstRaw) worstRaw = raw;
            if (w > worstAll) {
                worstAll = w;
                worstDesc = "edge " + std::to_string(i) + " (class " +
                            std::to_string((int)e.cls) + ")";
            }
            if (w > lim) {
                // The ONLY legal survivor above the limit is the lawExempt
                // mine-lot cul-de-sac: a HarborStreet turnaround authored at
                // r = 14 m, i.e. ~57.3/14 = 4.1 deg/m, closing on itself.
                const bool culDeSac = e.cls == RoadClass::HarborStreet &&
                                      w < 6.0f && e.length < 90.0f;
                if (culDeSac) ++exemptLike;
                else {
                    ++violators;
                    x3::logError("[roads-test]   zigzag VIOLATOR edge " + std::to_string(i) +
                                 " class " + std::to_string((int)e.cls) + ": " +
                                 f2s(w, 3) + " deg/m > " + f2s(lim, 3) +
                                 " (len " + f2s(e.length, 1) + " m)");
                }
            }
        }
        x3::logInfo("[roads-test] curvature: worst over edge BODIES " + f2s(worstAll, 3) +
                    " deg/m on " + worstDesc + "; " + std::to_string(exemptLike) +
                    " lawExempt-shaped survivor(s). Including the PHASE 2 junction "
                    "hand-off seams the law never saw: " + f2s(worstRaw, 3) + " deg/m.");
        ok(violators == 0,
           "R4 the zigzag law holds: no non-exempt edge BODY exceeds its class "
           "curvature limit + 15% grace (" + std::to_string(violators) + " violators)");
        ok(exemptLike >= 1,
           "R4 the documented lawExempt cul-de-sac survived the law (a blanket clamp "
           "would have flattened or dropped it)");
    }

    // ---- R5: junction clustering ------------------------------------------
    {
        const uint32_t junc = roads.junctionCount();
        const uint32_t raw  = rawJunctionCandidates(g);
        x3::logInfo("[roads-test] junctions: " + std::to_string(junc) +
                    " clustered patches from " + std::to_string(raw) +
                    " raw candidate sites (14 m merge radius).");
        ok(junc > 0, "R5 junction detection produced patches");
        expectCount(junc, kGoldJunctions, "R5 junction patch count");
        ok(raw > junc,
           "R5 clustering merged: raw candidates (" + std::to_string(raw) +
           ") > clustered patches (" + std::to_string(junc) + ")");
    }

    // ---- R6: sampleFrontage ------------------------------------------------
    {
        constexpr float kPitch   = 20.0f;
        constexpr float kSetback = 3.5f;
        // The documented sidewalk build-up, from echo_roads.cpp's kCurbW/kWalkW.
        constexpr float kCurbW = 0.35f, kWalkW = 1.80f;

        std::vector<Frontage> fr;
        roads.sampleFrontage(kRcGround, kPitch, kSetback, fr);
        x3::logInfo("[roads-test] sampleFrontage(kRcGround, pitch " + f2s(kPitch, 1) +
                    " m, setback " + f2s(kSetback, 2) + " m): " +
                    std::to_string(fr.size()) + " points.");
        ok(!fr.empty(), "R6 sampleFrontage emitted points");

        int badOffset = 0, badPerp = 0, badYaw = 0, badMeta = 0, badClass = 0;
        int left = 0, right = 0;
        float worstOff = 0.0f, worstYaw = 0.0f;
        for (const Frontage& f : fr) {
            if (f.edge >= g.edges.size()) { ++badMeta; continue; }
            const RoadEdge& e = g.edges[f.edge];
            if (f.sample >= e.center.size()) { ++badMeta; continue; }
            const RoadSample& p = e.center[f.sample];

            // Ground classes only.
            if (!(e.cls == RoadClass::Avenue || e.cls == RoadClass::HarborStreet)) ++badClass;
            if (f.cls != e.cls || f.width != e.width || f.roadY != p.y) ++badMeta;

            // 1) the offset magnitude IS the documented expression.
            const float want = e.width * 0.5f + kCurbW + kWalkW + kSetback;
            const float dx = f.x - p.x, dz = f.z - p.z;
            const float got = std::sqrt(dx * dx + dz * dz);
            const float dOff = std::fabs(got - want);
            if (dOff > worstOff) worstOff = dOff;
            if (dOff > 1e-2f) ++badOffset;

            // 2) the direction is +/- the right-perp of the tangent, and the
            //    normal is orthogonal to the road.
            const float px = p.tz, pz = -p.tx;             // rperp()
            const float sgn = (float)f.side;
            if (std::fabs(f.nx - px * sgn) > 1e-4f ||
                std::fabs(f.nz - pz * sgn) > 1e-4f) ++badPerp;
            if (std::fabs(f.nx * p.tx + f.nz * p.tz) > 1e-4f) ++badPerp;
            if (std::fabs(f.tx - p.tx) > 1e-6f || std::fabs(f.tz - p.tz) > 1e-6f) ++badMeta;

            // 3) yaw = atan2(tangent) +/- 90 deg, in the engine convention
            //    (local +Z maps to (sin yaw, cos yaw)), so +Z faces the road.
            const float yawRoad = std::atan2(p.tx, p.tz);
            const float dYaw    = std::fabs(std::fabs(wrapPi(f.yaw - yawRoad)) - 1.57079633f);
            if (dYaw > worstYaw) worstYaw = dYaw;
            if (dYaw > 1e-3f) ++badYaw;
            // ...and the forward vector really points back at the road.
            if (std::fabs(std::sin(f.yaw) + f.nx) > 1e-3f ||
                std::fabs(std::cos(f.yaw) + f.nz) > 1e-3f) ++badYaw;

            if (f.side > 0) ++right; else ++left;
        }
        ok(badOffset == 0,
           "R6 every frontage point sits at width/2 + curb + walk + setback "
           "(worst error " + f2s(worstOff * 1000.0, 3) + " mm)");
        ok(badPerp == 0, "R6 every frontage normal is +/- perp(tangent) and road-orthogonal");
        ok(badYaw == 0,
           "R6 every frontage yaw = atan2(tangent) +/- 90 deg and faces the road "
           "(worst error " + f2s(worstYaw * 57.2957795, 6) + " deg)");
        ok(badMeta == 0, "R6 frontage metadata (edge/sample/width/roadY/tangent/class) is consistent");
        ok(badClass == 0, "R6 kRcGround selected only Avenue/HarborStreet edges");
        ok(left == right && left > 0,
           "R6 both sides are emitted (" + std::to_string(right) + " right / " +
           std::to_string(left) + " left)");

        Hash fh;
        fh.u64(fr.size());
        for (const Frontage& f : fr) {
            fh.q(f.x, kQPos); fh.q(f.z, kQPos); fh.q(f.yaw, kQDir);
            fh.q(f.nx, kQDir); fh.q(f.nz, kQDir);
            fh.q(f.roadY, kQPos); fh.q(f.width, kQPos); fh.q(f.arc, kQPos);
            fh.u32(f.edge); fh.u32(f.sample); fh.i64(f.side); fh.u32((uint32_t)f.cls);
        }
        expectCount(fr.size(), kGoldFrontagePts, "R6 frontage point count");
        expectHash(fh.h, kGoldFrontage, "R6 frontage checksum");
    }

    // ---- R7: lane centres --------------------------------------------------
    {
        RoadEdge probe;
        probe.cls = RoadClass::Avenue; probe.width = 9.0f;
        probe.lanesF = 2; probe.lanesB = 2;
        const float w = 3.4f;   // the documented kLaneWidth
        bool good = true;
        for (int lane = 0; lane < 4; ++lane) {
            const float want = ((float)lane + 0.5f) * w;
            good = good && std::fabs(RoadGraph::laneCenterOffset(probe, lane, true)  - want) < 1e-4f;
            good = good && std::fabs(RoadGraph::laneCenterOffset(probe, lane, false) + want) < 1e-4f;
        }
        ok(good, "R7 lane centres are (lane + 0.5) * 3.4 m, forward positive / backward mirrored "
                 "(lane 0 = " + f2s(RoadGraph::laneCenterOffset(probe, 0, true), 4) + " m)");
    }

    // ---- R8: collision mesh + diagnostics ----------------------------------
    {
        const RoadCollisionMesh& cm = roads.collisionMesh();
        ok(!cm.verts.empty() && !cm.indices.empty(),
           "R8 drivable-surface collision mesh is populated (" +
           std::to_string(cm.verts.size() / 3) + " verts, " +
           std::to_string(cm.indices.size() / 3) + " tris)");
        ok(cm.verts.size() % 3 == 0 && cm.indices.size() % 3 == 0,
           "R8 collision mesh arrays are triangle-shaped");
        uint32_t maxIdx = 0;
        for (uint32_t i : cm.indices) maxIdx = i > maxIdx ? i : maxIdx;
        ok((size_t)maxIdx * 3 < cm.verts.size(), "R8 every collision index is in range");
        ok(roads.pavedMeters() > 1000.0f && roads.vertexCount() > 0,
           "R8 diagnostics populated: " + f2s(roads.pavedMeters(), 1) + " paved m, " +
           std::to_string(roads.vertexCount()) + " verts");
        ok(!roads.lights().empty(),
           "R8 the static lamp slice is populated (" +
           std::to_string(roads.lights().size()) + " point lights)");
    }

    x3::logInfo("[roads-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    if (g_fail == 0)
        x3::logInfo("[roads-test] LIFT A gate: the lifted EchoRoads reproduces the "
                    "locked road graph exactly.");
    return g_fail == 0;
}

} // namespace x3::game
