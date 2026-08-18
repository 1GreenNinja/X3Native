// THE MEGA STACK — see app/stack.h.
#include "stack.h"

#include "terrain.h"
#include "scene.h"
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace x3::game {

namespace {

constexpr float kMToFt = 3.28084f;

// ---- THE GEOMETRY, in one place ------------------------------------------
// The ramps' straight legs run PARALLEL to the mainline they leave, this far
// outside its paved edge, so a pier on a straight leg stands on natural
// ground beside the freeway and never in a lane.
constexpr float kRampOffsetPadM = 18.0f;
// The pinwheel's Sa (see the derivation in stack.h). R = Sa + offset, and
// Sa*(sqrt2 - 1) must beat the offset by a real margin or opposite ramps
// touch. With a 12 m median that is offset 74 m, Sa 210 -> R 284, and the
// opposite decks pass 27 m apart centre-to-centre.
constexpr float kArcSaM         = 210.0f;
// The straight climbing leg between the terminal and the arc. THIS NUMBER IS
// DERIVED, not taste: L4 sits ~21 m over the freeway, a 6% climb needs
// 21/0.06 = 350 m of it, the K law adds 0.06/4.5e-4 = 133 m of vertical
// curve at each end, and the profile has to be AT level ~90 m before the
// first crossing. First cut ran 330 m, the climb could not finish inside it,
// and the profile arrived at the pile still rising — which is exactly what
// gate S4's "level plateau -413 m" caught.
constexpr float kRampLeadM      = 700.0f;
// The gore taper: the ramp's terminal is fully separate from the mainline;
// this is the paved wedge that gets a car from the outer running lane onto
// it. ~260 m over a ~20 m lateral shift is 1:13 — a real high-speed exit.
constexpr float kGoreTaperM     = 260.0f;
// The crossing freeway's half length: past the ramp terminals, past the
// descent off the deck, plus a country tail. v1 dead-ends in the open,
// exactly as the diamond's crossroad does and for the same reason.
constexpr float kBHalfLenM      = 1400.0f;
// Ramp node spacing. Gap reaches are LINEAR on the chord
// (buildRoadRenderPath), so the node spacing IS the render resolution over
// the whole flyover: 6 m on a 284 m radius facets at 1.2 deg.
constexpr float kRampSpacingM   = 6.0f;
// The authored vertical rate: 10% inside road_network's 5e-4 /m cap, the
// same margin the diamond's crossroad keeps.
constexpr float kVRate          = 4.5e-4f;

float segPtDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float L2 = dx * dx + dz * dz;
    float t = 0.0f;
    if (L2 > 1e-9f)
        t = std::max(0.0f, std::min(1.0f, ((px - ax) * dx + (pz - az) * dz) / L2));
    const float qx = ax + dx * t, qz = az + dz * t;
    return std::sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
}

float distToSpec(const RoadSpec& s, float px, float pz) {
    float best = 1e18f;
    for (size_t k = 0; k + 1 < s.x.size(); ++k)
        best = std::min(best, segPtDist(px, pz, s.x[k], s.z[k], s.x[k+1], s.z[k+1]));
    return best;
}

// The natural (pre-carve) hillside — the same derivation the diamond scores
// its site against. Scoring the carved field would let a site score well
// because somebody else already cut there.
float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

// A route's graded datum nearest a point, INTERPOLATED along the winning
// segment. The diamond's clearance gate was first fooled by a start-node
// datum that was 4.3 m wrong on a 61 m segment at 7% — never snap.
float datumNear(const RoadSpec& s, const std::vector<float>& y,
                float qx, float qz) {
    float bestD = 1e18f, out = y.empty() ? 0.0f : y[0];
    for (size_t i = 0; i + 1 < s.x.size() && i + 1 < y.size(); ++i) {
        const float ax = s.x[i], az = s.z[i];
        const float dx = s.x[i+1] - ax, dz = s.z[i+1] - az;
        const float L2 = dx * dx + dz * dz;
        if (L2 < 1e-9f) continue;
        const float t = std::max(0.0f, std::min(1.0f,
            ((qx - ax) * dx + (qz - az) * dz) / L2));
        const float px = ax + dx * t, pz = az + dz * t;
        const float d = (qx - px) * (qx - px) + (qz - pz) * (qz - pz);
        if (d < bestD) { bestD = d; out = y[i] + (y[i+1] - y[i]) * t; }
    }
    return out;
}

SurfaceLibrary& stackSurfaces() { static SurfaceLibrary lib; return lib; }

// Quad-buffer mesh with winding-derived normals — the river bridge / diamond
// MeshBuf pattern (a viaduct is boxes and swept sections, not sculpture).
struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              float uScale = 1.0f, float vScale = 1.0f) {
        const float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float e2[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                       e1[0]*e2[1]-e1[1]*e2[0] };
        const float nl = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nl > 1e-6f) { n[0]/=nl; n[1]/=nl; n[2]/=nl; }
        const uint32_t base = (uint32_t)v.size();
        const float* pts[4] = { a, b, c, d };
        const float us[4] = { 0, 1, 1, 0 }, ws[4] = { 0, 0, 1, 1 };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=pts[k][0]; mv.pos[1]=pts[k][1]; mv.pos[2]=pts[k][2];
            mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
            mv.uv[0]=us[k]*uScale; mv.uv[1]=ws[k]*vScale;
            v.push_back(mv);
        }
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }
    bool empty() const { return i.empty(); }
};

// An ORIENTED box — echo_roads.cpp's obox(): long axis (ax,az) with half
// extent halfA, cross extent halfB, y0..y1 tall.
void obox(MeshBuf& m, float cx, float cz, float y0, float y1,
          float ax, float az, float halfA, float halfB) {
    if (y1 <= y0) return;
    const float bx = -az, bz = ax;
    const float c[4][2] = {
        { cx + ax*halfA + bx*halfB, cz + az*halfA + bz*halfB },
        { cx + ax*halfA - bx*halfB, cz + az*halfA - bz*halfB },
        { cx - ax*halfA - bx*halfB, cz - az*halfA - bz*halfB },
        { cx - ax*halfA + bx*halfB, cz - az*halfA + bz*halfB },
    };
    for (int f = 0; f < 4; ++f) {
        const int g = (f + 1) % 4;
        const float A[3] = { c[f][0], y0, c[f][1] };
        const float B[3] = { c[g][0], y0, c[g][1] };
        const float C[3] = { c[g][0], y1, c[g][1] };
        const float D[3] = { c[f][0], y1, c[f][1] };
        m.quad(A, B, C, D, 1.0f, (y1 - y0) / 6.0f);
    }
    const float T0[3] = { c[0][0], y1, c[0][1] }, T1[3] = { c[1][0], y1, c[1][1] };
    const float T2[3] = { c[2][0], y1, c[2][1] }, T3[3] = { c[3][0], y1, c[3][1] };
    m.quad(T0, T1, T2, T3);
    const float B0[3] = { c[0][0], y0, c[0][1] }, B1[3] = { c[1][0], y0, c[1][1] };
    const float B2[3] = { c[2][0], y0, c[2][1] }, B3[3] = { c[3][0], y0, c[3][1] };
    m.quad(B3, B2, B1, B0);
}

// THE PIER — lifted from echo_roads.cpp's pillar() (V7.3 STRUCTURAL, the slop
// verdict "the piers are black sticks — 61:1, no cap, no taper, no bearing
// seat"): a footing pad half-buried at grade, a TAPERED shaft in 1-3
// overlapping sections (more the taller the pier), and a HAMMERHEAD cap beam
// seated under the deck, oriented ACROSS it by the caller's perp.
// PAIRED (NO_SLOP rule 4) with echo_roads.cpp:535 — the proportions are that
// function's, scaled up for a Stack's 20 m columns; a change to the
// vocabulary there is a change here.
constexpr float kPierHalf = 1.55f;   // PAIRED with echo_roads.cpp's kPillarHalf
constexpr float kPierCapH = 1.70f;
void emitPier(MeshBuf& m, const StackPier& p) {
    const float h = p.ySoffit - p.yGround;
    if (h <= 0.5f) return;
    obox(m, p.x, p.z, p.yGround - 1.4f, p.yGround + 1.0f, 1.0f, 0.0f,
         kPierHalf * 2.3f, kPierHalf * 2.3f);
    const float capBase = p.ySoffit - kPierCapH;
    const float shaftTop = std::max(capBase + 0.3f, p.yGround + 1.2f);
    const int secs = h > 60.0f ? 3 : (h > 18.0f ? 2 : 1);
    for (int s = 0; s < secs; ++s) {
        const float t0 = (float)s / (float)secs, t1 = (float)(s + 1) / (float)secs;
        const float half = kPierHalf * (1.9f - 0.75f * t0);
        const float y0 = p.yGround + 0.7f + (shaftTop - p.yGround - 0.7f) * t0;
        const float y1 = p.yGround + 0.7f + (shaftTop - p.yGround - 0.7f) * t1;
        obox(m, p.x, p.z, y0 - 0.3f, y1, 1.0f, 0.0f, half, half);
    }
    obox(m, p.x, p.z, capBase, capBase + kPierCapH + 0.25f, p.px, p.pz,
         p.capHalfLenM, kPierHalf * 1.2f);
}

inline void deckPt(const StackDeckStation& st, float lat, float y, float o[3]) {
    o[0] = st.x + (-st.tz) * lat;
    o[1] = y;
    o[2] = st.z + ( st.tx) * lat;
}

// THE BOX SECTION — echo_roads.cpp's deckFascia() vocabulary: a wearing
// course on top, side fascia walls dropping `depth`, and a soffit between
// them pulled in, so the underside reads as a poured box girder and not a
// sheet of paper.
void emitDeckBox(MeshBuf& asph, MeshBuf& conc, const StackDeckRun& r) {
    const auto& s = r.s;
    if (s.size() < 2) return;
    const float inset = std::min(1.9f, r.halfW * 0.35f);
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const float ayT = s[i].y   + kPaveProud, byT = s[i+1].y + kPaveProud;
        const float ayS = s[i].y   - r.depth,    byS = s[i+1].y - r.depth;
        const float vs  = (s[i+1].u - s[i].u) / 8.0f;
        float aL[3], aR[3], bL[3], bR[3];
        deckPt(s[i],  -r.halfW, ayT, aL); deckPt(s[i],   r.halfW, ayT, aR);
        deckPt(s[i+1],-r.halfW, byT, bL); deckPt(s[i+1], r.halfW, byT, bR);
        asph.quad(aL, aR, bR, bL, 1.0f, vs);
        float aLs[3], aRs[3], bLs[3], bRs[3];
        deckPt(s[i],  -r.halfW + inset, ayS, aLs); deckPt(s[i],   r.halfW - inset, ayS, aRs);
        deckPt(s[i+1],-r.halfW + inset, byS, bLs); deckPt(s[i+1], r.halfW - inset, byS, bRs);
        conc.quad(aL, bL, bLs, aLs, 1.0f, vs);      // -side fascia web
        conc.quad(bR, aR, aRs, bRs, 1.0f, vs);      // +side fascia web
        conc.quad(aLs, bLs, bRs, aRs, 1.0f, vs);    // soffit
    }
}

// THE PARAPET — the owner's "High concrete barriers". A solid wall on the
// deck edge: outer face, inner face, top cap, both ends closed. Emitted as
// one welded run per edge, so it is CONTINUOUS by construction: gate S8
// measures the run lengths against the deck length instead of trusting it.
void emitParapets(MeshBuf& conc, const StackDeckRun& r) {
    const auto& s = r.s;
    if (s.size() < 2) return;
    for (int side = -1; side <= 1; side += 2) {
        const float latOut = (float)side * r.halfW;
        const float latIn  = (float)side * (r.halfW - kStackParapetW);
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            const float ay0 = s[i].y   + kPaveProud, ay1 = ay0 + kStackParapetH;
            const float by0 = s[i+1].y + kPaveProud, by1 = by0 + kStackParapetH;
            float aO0[3], aO1[3], bO0[3], bO1[3], aI0[3], aI1[3], bI0[3], bI1[3];
            deckPt(s[i],   latOut, ay0, aO0); deckPt(s[i],   latOut, ay1, aO1);
            deckPt(s[i+1], latOut, by0, bO0); deckPt(s[i+1], latOut, by1, bO1);
            deckPt(s[i],   latIn,  ay0, aI0); deckPt(s[i],   latIn,  ay1, aI1);
            deckPt(s[i+1], latIn,  by0, bI0); deckPt(s[i+1], latIn,  by1, bI1);
            const float vs = (s[i+1].u - s[i].u) / 4.0f;
            // WINDING, derived not guessed: MeshBuf::quad takes n = (b-a) x
            // (d-a), so the outer face wants (b-a) along the tangent and
            // (d-a) up on the +lat side and the mirror on the -lat side. The
            // CAP is the one that does NOT mirror — up is up on both edges —
            // and writing it symmetrically with the faces put both caps
            // face-down, i.e. invisible from the deck you are driving on.
            if (side > 0) {
                conc.quad(aO0, bO0, bO1, aO1, 1.0f, vs);   // outer face
                conc.quad(bI0, aI0, aI1, bI1, 1.0f, vs);   // inner face
                conc.quad(aO1, bO1, bI1, aI1, 1.0f, vs);   // cap, up
            } else {
                conc.quad(bO0, aO0, aO1, bO1, 1.0f, vs);
                conc.quad(aI0, bI0, bI1, aI1, 1.0f, vs);
                conc.quad(aI1, bI1, bO1, aO1, 1.0f, vs);   // cap, up
            }
        }
        for (int e = 0; e < 2; ++e) {
            const StackDeckStation& st = (e == 0) ? s.front() : s.back();
            const float y0 = st.y + kPaveProud, y1 = y0 + kStackParapetH;
            float p0[3], p1[3], p2[3], p3[3];
            deckPt(st, latOut, y0, p0); deckPt(st, latIn, y0, p1);
            deckPt(st, latIn,  y1, p2); deckPt(st, latOut, y1, p3);
            // the nose at the start of the run faces BACK along the deck
            if (e == 0) conc.quad(p1, p0, p3, p2);
            else        conc.quad(p0, p1, p2, p3);
        }
    }
}

// Split a route's render path into its ELEVATED runs (the reaches its Gaps
// own). One run per contiguous gap; tangents re-derived from the run so a
// deck is never twisted at a joint.
void deckRunsOf(const RoadSpec& spec, const std::vector<float>& roadY,
                std::vector<std::vector<StackDeckStation>>& runs) {
    std::vector<RoadRenderStation> path;
    std::vector<float> mp;
    if (spec.dualCarriageway) mp = computeMedianPlan(spec, roadY);
    buildRoadRenderPath(spec, &roadY, mp.empty() ? nullptr : &mp, path);
    const size_t n = path.size();
    if (n < 2) return;
    std::vector<uint8_t> on(n, 0);
    for (size_t i = 0; i + 1 < n; ++i)
        if (path[i].gap) { on[i] = 1; on[i + 1] = 1; }
    std::vector<StackDeckStation> cur;
    for (size_t i = 0; i < n; ++i) {
        if (!on[i]) {
            if (cur.size() >= 2) runs.push_back(cur);
            cur.clear();
            continue;
        }
        StackDeckStation d;
        d.x = path[i].x; d.z = path[i].z; d.y = path[i].y;
        if (!cur.empty()) {
            const float dx = d.x - cur.back().x, dz = d.z - cur.back().z;
            d.u = cur.back().u + std::sqrt(dx * dx + dz * dz);
        }
        cur.push_back(d);
    }
    if (cur.size() >= 2) runs.push_back(cur);
    for (auto& r : runs)
        for (size_t i = 0; i < r.size(); ++i) {
            const size_t ip = (i + 1 < r.size()) ? i + 1 : i;
            const size_t im = (i > 0) ? i - 1 : i;
            float tx = r[ip].x - r[im].x, tz = r[ip].z - r[im].z;
            const float tl = std::sqrt(tx * tx + tz * tz);
            if (tl > 1e-4f) { r[i].tx = tx / tl; r[i].tz = tz / tl; }
        }
}

}  // namespace

// ---------------------------------------------------------------------------
// UnderRoute
// ---------------------------------------------------------------------------
UnderRoute UnderRoute::fromRoute(const char* name, const RoadSpec& spec,
                                 const std::vector<float>& roadY, bool dual,
                                 float halfSpanM) {
    UnderRoute u;
    u.name = name ? name : "route";
    u.dual = dual;
    u.halfSpanM = halfSpanM;
    std::vector<float> medianPlan;
    if (dual) medianPlan = computeMedianPlan(spec, roadY);
    buildRoadRenderPath(spec, roadY.empty() ? nullptr : &roadY,
                        medianPlan.empty() ? nullptr : &medianPlan, u.path);
    return u;
}

bool UnderRoute::surfaceAt(float x, float z, float* outY) const {
    if (path.size() < 2) return false;
    float bestD2 = 1e18f, y = 0.0f, half = halfSpanM;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const RoadRenderStation& a = path[i];
        const RoadRenderStation& b = path[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float L2 = dx * dx + dz * dz;
        if (L2 < 1e-9f) continue;
        const float t = std::max(0.0f, std::min(1.0f,
            ((x - a.x) * dx + (z - a.z) * dz) / L2));
        const float qx = a.x + dx * t, qz = a.z + dz * t;
        const float d2 = (x - qx) * (x - qx) + (z - qz) * (z - qz);
        if (d2 >= bestD2) continue;
        bestD2 = d2;
        y = a.y + (b.y - a.y) * t;
        half = halfSpanM;
        if (dual) {
            const float mh = a.medianHalf + (b.medianHalf - a.medianHalf) * t;
            half = mh + 2.0f * kFwyPavedHalfM;
        }
    }
    if (bestD2 > half * half) return false;
    if (outY) *outY = y + kPaveProud;
    return true;
}

// ---------------------------------------------------------------------------
// THE NEW MECHANISM — multi-route gap authoring
// ---------------------------------------------------------------------------
FlyoverPlan planFlyoverGaps(RoadSpec& over, float startDatum, float endDatum,
                            const std::vector<UnderRoute>& under,
                            float structDepthM, float clearM,
                            float maxGrade, float maxGradeRate,
                            float forceLevelY) {
    FlyoverPlan out;
    const size_t n = over.x.size();
    if (n < 8 || over.z.size() != n) {
        out.whyNot = "degenerate flyover centreline";
        return out;
    }

    std::vector<float> U(n, 0.0f), gnd(n, 0.0f);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dx = over.x[i+1] - over.x[i], dz = over.z[i+1] - over.z[i];
        U[i+1] = U[i] + std::sqrt(dx * dx + dz * dz);
    }
    for (size_t i = 0; i < n; ++i) gnd[i] = naturalAt(over.x[i], over.z[i]);
    const float L = U[n - 1];

    // ---- EVERY crossing, by measurement ------------------------------------
    // One pass per underlying surface: a node is "over" it when surfaceAt()
    // answers, and a crossing is a maximal RUN of such nodes. The run's
    // highest surface sample is what the level has to clear.
    // PER NODE, not per flyover. The first cut asked one LEVEL to clear
    // everything the ramp ever passes over and hold it across the whole pile,
    // and the geometry threw it out: an L4 ramp is over SOMETHING for 470 m
    // of its 1400, and 470 m of dead-flat deck at +21 m leaves too little
    // road to climb onto it (gate S4: "level plateau -305 m"). Real stacks do
    // not do that either — a directional ramp is a CREST, and it only has to
    // be at the top level where it crosses the top thing. So: measure what
    // each NODE has to clear, and let the profile fall away from the crest
    // over the outer crossings, which only ever ask for a fraction of it.
    float need = -1e18f, uLo = 1e18f, uHi = -1e18f;
    std::vector<uint8_t> overSomething(n, 0);
    std::vector<float>   surf(n, -1e18f);   // highest surface under each node
    for (uint32_t k = 0; k < (uint32_t)under.size(); ++k) {
        bool inRun = false;
        float runHi = -1e18f, runU = 0.0f, runX = 0.0f, runZ = 0.0f;
        for (size_t i = 0; i <= n; ++i) {
            float sy = 0.0f;
            const bool hit = (i < n) && under[k].surfaceAt(over.x[i], over.z[i], &sy);
            if (hit) {
                overSomething[i] = 1;
                surf[i] = std::max(surf[i], sy);
                if (!inRun) { inRun = true; runHi = -1e18f; }
                if (sy > runHi) { runHi = sy; runU = U[i]; runX = over.x[i]; runZ = over.z[i]; }
                uLo = std::min(uLo, U[i]);
                uHi = std::max(uHi, U[i]);
            } else if (inRun) {
                inRun = false;
                StackCrossing c;
                c.under = k; c.u = runU; c.x = runX; c.z = runZ; c.surfaceY = runHi;
                out.cross.push_back(c);
                need = std::max(need, runHi);
            }
        }
    }
    if (out.cross.empty()) { out.whyNot = "the flyover crosses nothing"; return out; }
    out.pileM = uHi - uLo;
    out.uLoM = uLo; out.uHiM = uHi; out.lengthM = L;
    const float derived = need + clearM + kStackBearingM + structDepthM;
    out.levelY = (forceLevelY > 0.0f) ? forceLevelY : derived;

    // ---- THE AUTHORED VERTICAL ALIGNMENT -----------------------------------
    // The walk is the diamond's (interchange.cpp): a K-rate-limited TRACKING
    // WALK from each pinned end with dv clamped against the AVERAGE of the two
    // adjacent segment lengths (the per-segment clamp leaked 1.43x of the rate
    // at a station-spacing transition — measured, then killed). What is new is
    // the TARGET it tracks: not a trapezoid but the upper envelope of three
    // grade-limited CONES —
    //   * one rising out of each crossing's own requirement (surface + the
    //     16.5 ft law + bearing + structure), so a crossing over a freeway at
    //     grade costs 7 m and a crossing over another ramp's deck costs 21,
    //     and the profile between them is allowed to fall;
    //   * one from each terminal datum, so the ramp never dives below the
    //     road it merges with.
    // A cone is `req_j - maxGrade * |u - u_j|`: the lowest a <=maxGrade
    // profile through req_j can be here. Their max is therefore the LOWEST
    // feasible profile — a crest, exactly the shape a directional ramp is.
    // Where a level is FORCED (the two ramps of one level must be coplanar at
    // the crest) the top requirements are lifted to it and the cones follow.
    std::vector<float> req(n, -1e18f);
    for (size_t i = 0; i < n; ++i) {
        if (!overSomething[i]) continue;
        req[i] = surf[i] + clearM + kStackBearingM + structDepthM;
        if (forceLevelY > 0.0f && req[i] > derived - 0.30f)
            req[i] = forceLevelY;                 // the crest, shared by the pair
    }
    // the crest window, for the log and for gate S4
    float crestLo = 1e18f, crestHi = -1e18f;
    for (size_t i = 0; i < n; ++i)
        if (req[i] > out.levelY - 0.30f) {
            crestLo = std::min(crestLo, U[i]); crestHi = std::max(crestHi, U[i]);
        }
    // A ride height over the bare requirement: the rate-limited walk cannot
    // turn a corner instantly, so it shaves a crest. 0.35 m buys that back
    // and the clearance is MEASURED off the finished profile anyway.
    constexpr float kRide = 0.35f;
    std::vector<float> T(n, -1e18f);
    for (size_t i = 0; i < n; ++i)
        if (req[i] > -1e17f) T[i] = req[i] + kRide;
    {   // THE K-AWARE CONE. Spreading a requirement outward at maxGrade gives
        // a target that is grade-feasible and RATE-infeasible: a road cannot
        // leave a crest at 6% instantly, it has to turn the corner over
        // g/rate = 133 m first. A cone that forgets that asks the walk for a
        // 12 m-wide spike 9 m above its neighbours, and the walk misses it by
        // 3.46 m — measured, gate S4. So the kernel is the real one: a
        // parabola of curvature `rate` for the first g/rate metres, then a
        // straight g. Swept forward and backward, its upper envelope is the
        // LOWEST profile that can legally pass over every requirement.
        auto sweep = [&](bool forward) {
            float p = -1e18f, v = 0.0f;
            for (size_t c = 0; c < n; ++c) {
                const size_t i = forward ? c : (n - 1 - c);
                const size_t j = forward ? (c ? c - 1 : 0) : (c ? n - c : n - 1);
                const float li = (c == 0) ? 0.0f : std::fabs(U[i] - U[j]);
                p -= v * li;
                v = std::min(maxGrade, v + maxGradeRate * li);
                if (T[i] > p) { p = T[i]; v = 0.0f; }
                T[i] = p;
            }
        };
        sweep(true);
        sweep(false);
    }
    // ...and a FLOOR at the datum you came from before the crest, at the one
    // you are going to after it. Without these the cone dips well under both
    // terminals in the outer third and the profile would leave the freeway
    // by diving into a hole before climbing out of it. With them the shape is
    // exactly a ramp's: flat off the merge, up at grade, over the crest, down
    // at grade, flat onto the merge.
    for (size_t i = 0; i < n; ++i)
        T[i] = std::max(T[i], (U[i] <= 0.5f * (crestLo + crestHi)) ? startDatum : endDatum);
    // THE PROFILE IS THE ENVELOPE — the diamond ran a TRACKING WALK against
    // its target, and two cuts of that here proved a walk is the wrong tool
    // for this shape. A rate-limited controller trailing a rising target
    // either overshoots a plateau by v^2/2r (measured: L3 decks 2.6 m above
    // their own level) or, with the approach capped, lags a 6% climb by a
    // permanent 4 m and misses the crest requirement outright (measured:
    // -4.18 m, gates S3 and S4). The envelope T is already the LOWEST profile
    // that clears every requirement at <= maxGrade with crest curvature
    // exactly the K limit, so the profile IS T. The only thing T lacks is
    // SAG curvature: where two cones meet in a dip, or where a descending
    // cone lands on the terminal floor, the corner is convex and infinitely
    // sharp. One raise-only relaxation fills those to the same K limit —
    // raising can never break the clearance requirements it sits on.
    out.profile = T;
    out.profile.front() = startDatum;
    out.profile.back()  = endDatum;
    {
        std::vector<float>& P = out.profile;
        for (int iter = 0; iter < 4000; ++iter) {
            float moved = 0.0f;
            for (int dir = 0; dir < 2; ++dir)
                for (size_t c = 1; c + 1 < n; ++c) {
                    const size_t i = (dir == 0) ? c : (n - 1 - c);
                    if (i == 0 || i + 1 >= n) continue;
                    const float l0 = std::max(0.5f, U[i] - U[i-1]);
                    const float l1 = std::max(0.5f, U[i+1] - U[i]);
                    // (P[i+1]-P[i])/l1 - (P[i]-P[i-1])/l0 <= rate*(l0+l1)/2
                    const float lo = (P[i+1] / l1 + P[i-1] / l0
                                      - maxGradeRate * 0.5f * (l0 + l1))
                                   / (1.0f / l0 + 1.0f / l1);
                    if (P[i] < lo - 1e-6f) { moved += lo - P[i]; P[i] = lo; }
                }
            if (moved < 1e-4f) break;
        }
    }
    for (size_t i = 0; i + 1 < n; ++i)
        if (out.profile[i] >= out.levelY - 0.05f && out.profile[i+1] >= out.levelY - 0.05f)
            out.plateauM += U[i+1] - U[i];

    // ---- THE ELEVATED RUNS: where the deck actually leaves the ground ------
    // Two metres of air is the honest abutment line: below it the ramp is a
    // graded cut or embankment the ribbon can lay; above it there is nothing
    // under the road but piers, and the STRUCTURE owns the reach.
    //
    // PER RUN, not one span from the first air to the last. The first cut
    // took the whole range and called it deck, which put "viaduct" over
    // stretches where the authored profile runs 3.5 m UNDER the hillside —
    // and the pier planner then reported 341 m unsupported spans over ground
    // it was standing in (measured: "mid-span air -3.5 m", gate S9). A ramp
    // that dips into a cutting between two flyovers is a real thing and the
    // structure has to stop and start with it.
    std::vector<uint8_t> elev(n, 0);
    for (size_t i = 0; i < n; ++i)
        elev[i] = (out.profile[i] - gnd[i] > 2.0f || overSomething[i]) ? 1 : 0;
    // Bridge short interruptions (a 30 m touchdown between two flyovers is an
    // abutment pair nobody wants) and drop slivers too short to be a bridge.
    auto runLenFrom = [&](size_t a, size_t b) { return U[b] - U[a]; };
    for (size_t i = 0; i < n; ) {
        if (elev[i]) { ++i; continue; }
        size_t j = i;
        while (j < n && !elev[j]) ++j;
        if (i > 0 && j < n && runLenFrom(i, j) < 60.0f)
            for (size_t k = i; k < j; ++k) elev[k] = 1;
        i = j;
    }
    for (size_t i = 0; i < n; ) {
        if (!elev[i]) { ++i; continue; }
        size_t j = i;
        while (j < n && elev[j]) ++j;
        if (runLenFrom(i, j - 1) < 40.0f)
            for (size_t k = i; k < j; ++k) elev[k] = 0;
        i = j;
    }
    elev.front() = 0; elev.back() = 0;         // the pinned landings stay on the ground

    // ---- ONE GAP PER SEGMENT, carrying the authored profile ----------------
    over.gaps.clear();
    uint32_t gLo = 0, gHi = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (!elev[i] || !elev[i + 1]) continue;
        RoadSpec::Gap g;
        g.i0 = (uint32_t)i; g.i1 = (uint32_t)(i + 1);
        g.y0 = out.profile[i]; g.y1 = out.profile[i + 1];
        over.gaps.push_back(g);
        if (!gLo) gLo = (uint32_t)i;
        gHi = (uint32_t)(i + 1);
    }
    if (over.gaps.empty()) {
        out.whyNot = "no elevated reach — the flyover never leaves the ground";
        return out;
    }
    over.pinY = out.profile;                  // the at-grade reaches are authored too
    out.gapI0 = gLo; out.gapI1 = gHi;

    // ---- measure what was authored -----------------------------------------
    for (size_t i = 0; i + 1 < n; ++i) {
        const float li = std::max(1.0f, U[i+1] - U[i]);
        out.maxGradePct = std::max(out.maxGradePct,
            std::fabs(out.profile[i+1] - out.profile[i]) / li * 100.0f);
    }
    for (size_t i = 1; i + 1 < n; ++i) {
        const float l0 = std::max(1.0f, U[i] - U[i-1]);
        const float l1 = std::max(1.0f, U[i+1] - U[i]);
        const float ga = (out.profile[i]   - out.profile[i-1]) / l0;
        const float gb = (out.profile[i+1] - out.profile[i])   / l1;
        out.maxGradeRate = std::max(out.maxGradeRate,
            std::fabs(gb - ga) / (0.5f * (l0 + l1)));
    }
    // THE CLEARANCE, measured off the FINISHED profile at every node that is
    // over anything — not off the nominal level, and not only at the sample
    // the crossing record happens to name. A profile is what gets built; a
    // level is what was asked for, and the diamond's own history is a story
    // about the difference between those two (its first clearance gate read
    // 9.05 ft of quantisation and called it structure).
    auto profAt = [&](float u) {
        for (size_t i = 0; i + 1 < n; ++i) {
            if (u > U[i+1] && i + 2 < n) continue;
            const float span = std::max(1e-4f, U[i+1] - U[i]);
            const float t = std::max(0.0f, std::min(1.0f, (u - U[i]) / span));
            return out.profile[i] + (out.profile[i+1] - out.profile[i]) * t;
        }
        return out.profile.back();
    };
    for (size_t i = 0; i < n; ++i) {
        if (!overSomething[i]) continue;
        const float cl = out.profile[i] - structDepthM - surf[i];
        out.minClearanceM = std::min(out.minClearanceM, cl);
        out.crestDeficitM = std::min(out.crestDeficitM, out.profile[i] - req[i]);
    }
    for (StackCrossing& c : out.cross) {
        c.clearanceM = profAt(c.u) - structDepthM - c.surfaceY;
    }
    out.crestLoM = crestLo; out.crestHiM = crestHi;
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// REGISTRATION
// ---------------------------------------------------------------------------
StackResult registerStack(const RoadSpec& fwySpec,
                          const std::vector<float>& fwyRoadY,
                          const std::vector<const RoadSpec*>* avoid) {
    StackResult out;
    const size_t fn = fwySpec.x.size();
    if (fn < 8 || fwyRoadY.size() != fn || fwySpec.z.size() != fn ||
        !fwySpec.dualCarriageway) {
        out.whyNot = "freeway spec/datum missing or not a dual carriageway";
        x3::logError(std::string("stack: ") + out.whyNot);
        return out;
    }
    const std::vector<float> medianPlan = computeMedianPlan(fwySpec, fwyRoadY);
    if (medianPlan.size() != fn) {
        out.whyNot = "median plan failed";
        x3::logError(std::string("stack: ") + out.whyNot);
        return out;
    }
    out.aSpec = fwySpec;
    out.aRoadY = fwyRoadY;

    std::vector<float> U(fn, 0.0f);
    for (size_t i = 0; i + 1 < fn; ++i) {
        const float dx = fwySpec.x[i+1] - fwySpec.x[i];
        const float dz = fwySpec.z[i+1] - fwySpec.z[i];
        U[i+1] = U[i] + std::sqrt(dx * dx + dz * dz);
    }
    auto tangentAt = [&](size_t i, float& tx, float& tz) {
        const size_t ip = (i + 1 < fn) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        tx = fwySpec.x[ip] - fwySpec.x[im];
        tz = fwySpec.z[ip] - fwySpec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
    };

    // ---- THE SITE, by measurement (NO_SLOP rule 9) -------------------------
    // The diamond's tests with the numbers a Stack actually needs: the deck
    // footprint genuinely straight, the whole zone only gently bending (the
    // ramp terminals are derived from the freeway's REAL geometry, so a few
    // degrees of sweep costs nothing), a WIDE median for the centre pier,
    // flat country under the ramp footprints, and — the one new test — CLEAR
    // OF THE DIAMOND'S OWN ZONE. Two grade-separated interchanges 400 m apart
    // is not a network, it is a pile-up.
    size_t J = SIZE_MAX;
    float bestScore = 1e18f;
    const float kWin = kStackSiteWindowM;
    for (size_t j = 4; j + 4 < fn; j += 2) {
        float t0x, t0z, t1x, t1z, tjx, tjz;
        size_t a = j, b = j;
        while (a > 1 && U[j] - U[a] < kWin) --a;
        while (b + 2 < fn && U[b] - U[j] < kWin) ++b;
        if (U[j] - U[a] < kWin * 0.9f || U[b] - U[j] < kWin * 0.9f) continue;
        tangentAt(a, t0x, t0z);
        tangentAt(b, t1x, t1z);
        tangentAt(j, tjx, tjz);
        const float dotAB = std::max(-1.0f, std::min(1.0f, t0x * t1x + t0z * t1z));
        const float zoneBendDeg = std::acos(dotAB) * 57.29578f;
        if (zoneBendDeg > 8.0f) continue;
        {   // the deck footprint itself must be genuinely straight
            size_t da = j, db = j;
            while (da > 1 && U[j] - U[da] < 170.0f) --da;
            while (db + 2 < fn && U[db] - U[j] < 170.0f) ++db;
            float dax, daz, dbx, dbz;
            tangentAt(da, dax, daz);
            tangentAt(db, dbx, dbz);
            const float dd = std::max(-1.0f, std::min(1.0f, dax * dbx + daz * dbz));
            if (std::acos(dd) * 57.29578f > 4.0f) continue;
        }
        if (medianPlan[j] < 9.0f) continue;
        if (distToNearestRoadJunction(fwySpec.x[j], fwySpec.z[j]) < 1300.0f) continue;
        if (inInterchangeZone(fwySpec.x[j], fwySpec.z[j])) continue;
        const float px = -tjz, pz = tjx;
        const float yJ = fwyRoadY[j];
        float fit = 0.0f;
        bool clear = true;
        for (int k = -16; k <= 16 && clear; ++k) {          // the crossing freeway
            const float s = (float)k * (kBHalfLenM / 16.0f);
            const float qx = fwySpec.x[j] + px * s;
            const float qz = fwySpec.z[j] + pz * s;
            if (std::fabs(s) <= 960.0f) fit = std::max(fit, std::fabs(naturalAt(qx, qz) - yJ));
            if (inInterchangeZone(qx, qz)) { clear = false; break; }
            if (avoid)
                for (const RoadSpec* av : *avoid)
                    if (av && distToSpec(*av, qx, qz) < 2.0f * kFwyPavedHalfM + 40.0f)
                        { clear = false; break; }
        }
        if (!clear) continue;
        for (int qa = -1; qa <= 1 && clear; qa += 2)        // the ramp pinwheel
            for (int qb = -1; qb <= 1 && clear; qb += 2)
                for (int r = 0; r < 3 && clear; ++r) {
                    const float rr[3] = { 320.0f, 520.0f, 720.0f };
                    const float ex = fwySpec.x[j] + tjx * (float)qa * rr[r] * 0.7071f
                                   + px * (float)qb * rr[r] * 0.7071f;
                    const float ez = fwySpec.z[j] + tjz * (float)qa * rr[r] * 0.7071f
                                   + pz * (float)qb * rr[r] * 0.7071f;
                    fit = std::max(fit, std::fabs(naturalAt(ex, ez) - yJ));
                    if (inInterchangeZone(ex, ez)) { clear = false; break; }
                    if (avoid)
                        for (const RoadSpec* av : *avoid)
                            if (av && distToSpec(*av, ex, ez) < 2.0f * kFwyPavedHalfM + 40.0f)
                                { clear = false; break; }
                }
        if (!clear) continue;
        const float score = fit + zoneBendDeg * 1.5f;
        if (score < bestScore) { bestScore = score; J = j; }
    }
    if (J == SIZE_MAX) {
        out.whyNot = "no freeway node passes the Stack site tests "
                     "(straight/median/terrain/clear of the diamond)";
        x3::logError(std::string("stack: ") + out.whyNot);
        return out;
    }

    out.fwyNode = (uint32_t)J;
    out.siteScore = bestScore;
    out.cx = fwySpec.x[J]; out.cz = fwySpec.z[J];
    tangentAt(J, out.tX, out.tZ);
    out.pX = -out.tZ; out.pZ = out.tX;
    out.medianHalfA = medianPlan[J];

    auto fwySurface = [&](float qx, float qz) {
        return datumNear(fwySpec, fwyRoadY, qx, qz) + kPaveProud;
    };
    const float fwyPavedEdge = out.medianHalfA + 2.0f * kFwyPavedHalfM;
    out.abutS = fwyPavedEdge + 12.0f;

    // L2 — the deck datum: the highest freeway pavement anywhere under the
    // twin deck's footprint, plus the law, plus the structure.
    {
        float hi = -1e18f;
        for (float lat = -(fwyPavedEdge + 6.0f); lat <= fwyPavedEdge + 6.1f; lat += 4.0f)
            for (float s = -out.abutS; s <= out.abutS + 0.1f; s += 6.0f)
                hi = std::max(hi, fwySurface(out.cx + out.pX * s + out.tX * lat,
                                             out.cz + out.pZ * s + out.tZ * lat));
        out.baseSurfaceY = hi;
        out.levelY[0] = hi + kStackClearM + kStackBearingM + kStackMainDepthM;
    }

    // ---- L2: THE CROSSING FREEWAY ------------------------------------------
    {
        RoadSpec s;
        s.name             = "stack crossing freeway";
        s.dualCarriageway  = true;
        s.halfWidth        = kFwyDualMaxHalfM;
        s.falloff          = 18.0f;
        s.maxGrade         = 0.05f;
        s.minTurnRadiusM   = 250.0f;
        s.maxDeflectionDeg = 3.0f;
        std::vector<float> st;
        auto stepAt = [&](float sp) {
            return (std::fabs(std::fabs(sp) - out.abutS) < 260.0f) ? 12.0f : 30.0f;
        };
        for (float sp = -kBHalfLenM; sp < -out.abutS - 1.0f; sp += stepAt(sp)) st.push_back(sp);
        st.push_back(-out.abutS);
        st.push_back(0.0f);
        st.push_back(+out.abutS);
        for (float sp = out.abutS + stepAt(out.abutS + 1.0f);
             sp <= kBHalfLenM + 1.0f; sp += stepAt(sp)) st.push_back(sp);
        std::sort(st.begin(), st.end());
        st.erase(std::unique(st.begin(), st.end(),
                 [](float a, float b) { return std::fabs(a - b) < 4.0f; }), st.end());
        // The abutment nodes are found by SEARCH, not by an exact station
        // match. The station list is decimated (4 m tolerance) after it is
        // built, so the node placed at exactly +-abutS can be the one the
        // decimation eats — and then the gap index silently stays 0 and the
        // "deck" runs from the far end of the freeway. Measured: a 130 m
        // deck that reported as 1465 m of viaduct with no piers under it.
        uint32_t gi0 = 0, gi1 = 0;
        float d0 = 1e18f, d1 = 1e18f;
        for (float sp : st) {
            const uint32_t idx = (uint32_t)s.x.size();
            if (std::fabs(sp + out.abutS) < d0) { d0 = std::fabs(sp + out.abutS); gi0 = idx; }
            if (std::fabs(sp - out.abutS) < d1) { d1 = std::fabs(sp - out.abutS); gi1 = idx; }
            s.x.push_back(out.cx + out.pX * sp);
            s.z.push_back(out.cz + out.pZ * sp);
        }
        if (gi1 <= gi0) {
            out.whyNot = "the crossing freeway's deck gap is degenerate";
            x3::logError(std::string("stack: ") + out.whyNot);
            return out;
        }
        RoadSpec::Gap g;
        g.i0 = gi0; g.i1 = gi1;
        g.y0 = g.y1 = out.levelY[0];    // the deck is LEVEL, like Bridge No. 1
        s.gaps.push_back(g);
        // THE AUTHORED VERTICAL ALIGNMENT — the diamond's, verbatim in spirit:
        // a grade-limited ground-envelope fit anchored at the deck pin, then
        // a K-rate-limited tracking walk outward with dv clamped on the
        // AVERAGE of the adjacent segment lengths.
        {
            const float kVgMax = s.maxGrade;
            s.pinY.assign(s.x.size(), std::numeric_limits<float>::quiet_NaN());
            const float reach = s.halfWidth + 0.8f;
            const float sA0 = st[gi0], sA1 = st[gi1];
            for (int side = -1; side <= 1; side += 2) {
                const float edge = (side < 0) ? sA0 : sA1;
                std::vector<size_t> order;
                for (size_t i = 0; i < st.size(); ++i)
                    if ((side < 0 && st[i] < edge - 0.5f) ||
                        (side > 0 && st[i] > edge + 0.5f))
                        order.push_back(i);
                if (side < 0) std::reverse(order.begin(), order.end());
                const size_t m = order.size();
                if (m < 2) continue;
                std::vector<float> E(m), Ls(m);
                float dPrev = 0.0f;
                for (size_t k = 0; k < m; ++k) {
                    const size_t oi = order[k];
                    const float d = std::fabs(st[oi] - edge);
                    Ls[k] = std::max(1.0f, d - dPrev);
                    dPrev = d;
                    float hi = -1e18f;
                    for (int q = -4; q <= 4; ++q) {
                        const float off = (float)q * reach / 4.0f;
                        hi = std::max(hi, naturalAt(s.x[oi] + out.tX * off,
                                                    s.z[oi] + out.tZ * off));
                    }
                    E[k] = hi;
                }
                std::vector<float> T(E);
                float prev = out.levelY[0];
                for (size_t k = 0; k < m; ++k) {
                    T[k] = std::max(prev - kVgMax * Ls[k],
                                    std::min(prev + kVgMax * Ls[k], T[k]));
                    prev = T[k];
                }
                float p = out.levelY[0], v = 0.0f, Lp = Ls[0];
                for (size_t k = 0; k < m; ++k) {
                    const float dvCap = kVRate * 0.5f * (Lp + Ls[k]);
                    const float want = (T[k] - p) / Ls[k];
                    v = std::max(v - dvCap, std::min(v + dvCap, want));
                    v = std::max(-kVgMax, std::min(kVgMax, v));
                    p += v * Ls[k];
                    s.pinY[order[k]] = p;
                    Lp = Ls[k];
                }
            }
        }
        out.bSpec = s;
        out.bRoad = registerRoad(out.bSpec, &out.bRoadY);
        if (!out.bRoad.ok) {
            out.whyNot = "crossing freeway registration failed";
            x3::logError(std::string("stack: ") + out.whyNot);
            return out;
        }
        const std::vector<float> mpB = computeMedianPlan(out.bSpec, out.bRoadY);
        out.medianHalfB = mpB.empty() ? kFwyMedianMinHalfM : mpB[gi0];
        float worst = 1e18f;
        const float soffit = out.levelY[0] - kStackMainDepthM;
        for (float lat = -fwyPavedEdge; lat <= fwyPavedEdge + 0.1f; lat += 2.0f)
            for (float l = -out.abutS; l <= out.abutS + 0.1f; l += 4.0f) {
                const float qx = out.cx + out.pX * lat + out.tX * l;
                const float qz = out.cz + out.pZ * lat + out.tZ * l;
                worst = std::min(worst, soffit - fwySurface(qx, qz));
            }
        out.bClearanceM = worst;
        out.minClearanceM = std::min(out.minClearanceM, worst);
    }

    // ---- L3 / L4: THE FOUR DIRECTIONAL FLYOVERS ----------------------------
    const float rampOffset = fwyPavedEdge + kRampOffsetPadM;
    const float arcR       = kArcSaM + rampOffset;
    const float termS      = kArcSaM + kRampLeadM;
    const float rampHalfW  = kPavedHalfM * 0.5f + 1.0f;
    out.rampOffsetM = rampOffset;
    out.arcRadiusDesignM = arcR;

    auto frameW = [&](float u, float v, float& x, float& z) {
        x = out.cx + out.tX * u + out.pX * v;
        z = out.cz + out.tZ * u + out.pZ * v;
    };
    // A POINT ON FREEWAY A, by ARC LENGTH from the crossing, offset laterally
    // by its OWN local tangent — not by the Stack's frame.
    //
    // The frame is a straight-line abstraction and freeway A is not straight:
    // the site test only asks for <= 4 deg through the deck footprint and
    // <= 22 deg across the whole window. Over the 750 m out to a ramp
    // terminal that is tens of metres of drift, and the first cut placed the
    // terminals in the frame anyway — so a terminal meant to sit 18 m outside
    // A's apron landed ON A's pavement, the gap planner correctly reported
    // the ramp as "over the freeway" for its entire length, and the level
    // plateau could never be reached (gate S4: "pile u 665..1521 of 1521").
    // Deriving the terminal from A's real geometry fixes it at the source and
    // is also what makes the gore taper meet the ramp: the taper follows A.
    const float aTotal = U[fn - 1];
    auto alongA = [&](float s, float lat, float& x, float& z, float& tx, float& tz) {
        float t = std::fmod(U[J] + s, aTotal);
        if (t < 0.0f) t += aTotal;
        size_t i = 0;
        while (i + 2 < fn && U[i + 1] < t) ++i;
        const float span = std::max(1e-4f, U[i + 1] - U[i]);
        const float f = std::max(0.0f, std::min(1.0f, (t - U[i]) / span));
        x = fwySpec.x[i] + (fwySpec.x[i+1] - fwySpec.x[i]) * f;
        z = fwySpec.z[i] + (fwySpec.z[i+1] - fwySpec.z[i]) * f;
        tangentAt(f < 0.5f ? i : i + 1, tx, tz);
        x += (-tz) * lat;
        z += ( tx) * lat;
    };
    // The pinwheel: ramp q is the base shape rotated q * 90 degrees. Even q
    // runs A -> B, odd q runs B -> A; between them the four left turns.
    auto makeRampSpec = [&](int q) {
        auto rot = [&](float u, float v, float& ru, float& rv) {
            switch (q & 3) {
                case 0: ru =  u; rv =  v; break;
                case 1: ru = -v; rv =  u; break;
                case 2: ru = -u; rv = -v; break;
                default: ru =  v; rv = -u; break;
            }
        };
        float w0u, w0v, w2u, w2v, d1u, d1v, d2u, d2v;
        rot(-termS,     rampOffset, w0u, w0v);   // start terminal
        rot(rampOffset, -termS,     w2u, w2v);   // end terminal
        rot( 1.0f,  0.0f, d1u, d1v);             // leg 1 travel direction
        rot( 0.0f, -1.0f, d2u, d2v);             // leg 2 travel direction
        // Whichever leg runs along A takes A's real geometry; the other runs
        // along B, which IS straight in the frame because we authored it so.
        float p0x, p0z, d0x, d0z, p1x, p1z, e1x, e1z;
        auto place = [&](float wu, float wv, float du, float dv,
                         float& px, float& pz, float& dx, float& dz) {
            if (std::fabs(wu) > std::fabs(wv)) {          // on freeway A
                float tx, tz;
                alongA(wu, wv, px, pz, tx, tz);
                const float sgn = (du >= 0.0f) ? 1.0f : -1.0f;
                dx = sgn * tx; dz = sgn * tz;
            } else {                                       // on freeway B
                frameW(wu, wv, px, pz);
                dx = out.tX * du + out.pX * dv;
                dz = out.tZ * du + out.pZ * dv;
            }
        };
        place(w0u, w0v, d1u, d1v, p0x, p0z, d0x, d0z);
        place(w2u, w2v, d2u, d2v, p1x, p1z, e1x, e1z);
        // Elbow: where the two leg lines meet. p0 + d0*mu == p1 - e1*lam.
        float ex, ez;
        {
            const float det = d0x * e1z - d0z * e1x;
            const float rx = p1x - p0x, rz = p1z - p0z;
            const float mu = (std::fabs(det) < 1e-5f) ? (termS + rampOffset)
                                                      : (rx * e1z - rz * e1x) / det;
            ex = p0x + d0x * mu; ez = p0z + d0z * mu;
        }
        std::vector<CourseWaypoint> wp(3);
        wp[0].x = p0x; wp[0].z = p0z;
        wp[1].x = ex;  wp[1].z = ez;
        wp[2].x = p1x; wp[2].z = p1z;
        wp[1].fillet = arcR;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "stack flyover q%d", q);
        RoadSpec rs = makeRoadFromWaypoints(nm, wp, kRampSpacingM, /*closed=*/false);
        // RAMP CLASS: half the base cross-section (two 12 ft lanes), a 6%
        // ceiling, and a 150 m radius FLOOR — the high-speed band the owner
        // asked for, and deliberately UNDER the 200 m freeway class floor so
        // the authored 284 m sweep survives untouched (per-spec floors are
        // design; the diamond set that precedent at 48 m).
        rs.widthScale       = 0.5f;
        rs.halfWidth        = rampHalfW;
        rs.falloff          = 12.0f;
        rs.maxGrade         = 0.06f;
        rs.minTurnRadiusM   = kStackArcMinR;
        rs.maxDeflectionDeg = 2.0f;
        smoothHorizontalCurves(rs);
        return rs;
    };

    std::vector<UnderRoute> underL3, underL4;
    underL3.push_back(UnderRoute::fromRoute("inner tour (L1)", fwySpec, fwyRoadY,
                                            true, fwyPavedEdge));
    underL3.push_back(UnderRoute::fromRoute("crossing freeway (L2)", out.bSpec,
                                            out.bRoadY, true,
                                            out.medianHalfB + 2.0f * kFwyPavedHalfM));
    underL4 = underL3;

    int built = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int level = pass == 0 ? 3 : 4;
        const int qs[2] = { pass == 0 ? 0 : 1, pass == 0 ? 2 : 3 };
        std::vector<UnderRoute>& under = (level == 3) ? underL3 : underL4;
        // PASS ONE — plan both ramps to find the level they BOTH need. The
        // two decks of one level must be COPLANAR (they are congruent by
        // construction and a stack that steps between them reads broken), so
        // the level is the max of what each ramp asks for, not each ramp's
        // own answer.
        float lvl = 0.0f;
        float startY[2] = { 0, 0 }, endY[2] = { 0, 0 };
        // Which freeway a terminal belongs to is decided by MEASUREMENT, not
        // by index: even ramps run A -> B and odd ramps run B -> A, and a
        // hard-coded "front is A" gets half of them wrong.
        auto hostDatum = [&](float x, float z) {
            return (distToSpec(fwySpec, x, z) <= distToSpec(out.bSpec, x, z))
                 ? datumNear(fwySpec, fwyRoadY, x, z)
                 : datumNear(out.bSpec, out.bRoadY, x, z);
        };
        for (int k = 0; k < 2; ++k) {
            RoadSpec probe = makeRampSpec(qs[k]);
            startY[k] = hostDatum(probe.x.front(), probe.z.front());
            endY[k]   = hostDatum(probe.x.back(),  probe.z.back());
            FlyoverPlan p = planFlyoverGaps(probe, startY[k], endY[k], under,
                                            kStackRampDepthM, kStackClearM,
                                            0.06f, kVRate, 0.0f);
            if (!p.ok) {
                x3::logError(std::string("stack: L") + std::to_string(level) +
                             " probe failed — " + p.whyNot);
                continue;
            }
            lvl = std::max(lvl, p.levelY);
        }
        if (lvl <= 0.0f) { out.whyNot = "a flyover level could not be found"; return out; }
        out.levelY[level - 2] = lvl;
        // PASS TWO — author, register, and hand the finished deck to the
        // level above as one more thing IT has to clear.
        for (int k = 0; k < 2; ++k) {
            const int q = qs[k];
            StackResult::Ramp& rp = out.ramp[q];
            rp.level = level;
            RoadSpec rs = makeRampSpec(q);
            measureMaxDeflectionDeg(rs, &rp.arcRadiusM);
            rp.termAy = startY[k]; rp.termBy = endY[k];
            rp.plan = planFlyoverGaps(rs, startY[k], endY[k], under,
                                      kStackRampDepthM, kStackClearM,
                                      0.06f, kVRate, lvl);
            if (!rp.plan.ok) {
                x3::logError(std::string("stack: flyover q") + std::to_string(q) +
                             " gap authoring failed — " + rp.plan.whyNot);
                continue;
            }
            rp.spec = rs;
            rp.road = registerRoad(rp.spec, &rp.roadY);
            if (!rp.road.ok || rp.roadY.empty()) {
                x3::logError(std::string("stack: flyover q") + std::to_string(q) +
                             " registration failed");
                continue;
            }
            // The gore mouths are JUNCTIONS: no barrier, no median wall, and
            // the prism skirt feathers through them — the same discipline
            // every branch in the network obeys.
            {
                const float ax = rp.spec.x.front(), az = rp.spec.z.front();
                const float bx = rp.spec.x.back(),  bz = rp.spec.z.back();
                float jax = ax, jaz = az, jbx = bx, jbz = bz;
                {   // nearest centreline point on each host
                    float bd = 1e18f;
                    for (size_t i = 0; i < fwySpec.x.size(); ++i) {
                        const float d = (fwySpec.x[i]-ax)*(fwySpec.x[i]-ax) +
                                        (fwySpec.z[i]-az)*(fwySpec.z[i]-az);
                        if (d < bd) { bd = d; jax = fwySpec.x[i]; jaz = fwySpec.z[i]; }
                    }
                    bd = 1e18f;
                    for (size_t i = 0; i < out.bSpec.x.size(); ++i) {
                        const float d = (out.bSpec.x[i]-bx)*(out.bSpec.x[i]-bx) +
                                        (out.bSpec.z[i]-bz)*(out.bSpec.z[i]-bz);
                        if (d < bd) { bd = d; jbx = out.bSpec.x[i]; jbz = out.bSpec.z[i]; }
                    }
                }
                registerRoadJunctionThroat(ax, az, jax, jaz, rp.termAy);
                registerRoadJunctionThroat(bx, bz, jbx, jbz, rp.termBy);
            }
            rp.built = true;
            ++built;
            out.crossingCount += (uint32_t)rp.plan.cross.size();
            out.minClearanceM = std::min(out.minClearanceM, rp.plan.minClearanceM);
            if (level == 3) {
                char nm[64];
                std::snprintf(nm, sizeof(nm), "flyover L3 q%d", q);
                underL4.push_back(UnderRoute::fromRoute(nm, rp.spec, rp.roadY, false,
                                                        rampHalfW + kStackParapetW));
            }
        }
    }
    if (built < 4) {
        out.whyNot = "a flyover ramp failed";
        char eb[96];
        std::snprintf(eb, sizeof(eb), "stack: only %d of 4 flyovers built", built);
        x3::logError(eb);
        return out;
    }

    // The zone: no median crossover, no work zone, anywhere in the Stack.
    noteInterchangeZone(out.cx, out.cz, kStackZoneR);

    out.built = true;
    char bfr[560];
    std::snprintf(bfr, sizeof(bfr),
        "stack: FOUR-LEVEL MEGA STACK at freeway node %u (%.0f, %.0f) — L1 at "
        "grade, L2 deck +%.1f ft, L3 +%.1f ft, L4 +%.1f ft; %u crossings "
        "measured, worst clearance %.2f ft (law %.1f ft); sweep radius %.0f m "
        "design / %.0f m measured; ramp grades %.1f/%.1f/%.1f/%.1f%%; crossing "
        "freeway %.2f miles; median %.1f m for the pier; site fit %.1f",
        out.fwyNode, out.cx, out.cz,
        (out.levelY[0] - out.baseSurfaceY) * kMToFt,
        (out.levelY[1] - out.baseSurfaceY) * kMToFt,
        (out.levelY[2] - out.baseSurfaceY) * kMToFt,
        out.crossingCount, out.minClearanceM * kMToFt, kStackClearM * kMToFt,
        arcR,
        std::min(std::min(out.ramp[0].arcRadiusM, out.ramp[1].arcRadiusM),
                 std::min(out.ramp[2].arcRadiusM, out.ramp[3].arcRadiusM)),
        out.ramp[0].road.maxGradePct, out.ramp[1].road.maxGradePct,
        out.ramp[2].road.maxGradePct, out.ramp[3].road.maxGradePct,
        out.bRoad.lengthM / 1609.34f, out.medianHalfA, bestScore);
    x3::logInfo(bfr);
    return out;
}

// ---------------------------------------------------------------------------
// THE STRUCTURE PLAN — pure, so the gates measure what ships
// ---------------------------------------------------------------------------
StackStructurePlan planStackStructure(const StackResult& st) {
    StackStructurePlan out;
    if (!st.built) return out;

    // L2 — TWIN decks, one per carriageway. Real freeway overpasses are twin
    // structures, and here it also keeps the median open so the L3/L4 piers
    // have somewhere to come down.
    {
        std::vector<std::vector<StackDeckStation>> runs;
        deckRunsOf(st.bSpec, st.bRoadY, runs);
        const float lateral = st.medianHalfB + kFwyPavedHalfM;
        for (const auto& base : runs)
            for (int cw = 0; cw < 2; ++cw) {
                StackDeckRun r;
                r.s = base;
                const float side = (cw == 0) ? -1.0f : 1.0f;
                for (StackDeckStation& d : r.s) {
                    d.x += (-d.tz) * side * lateral;
                    d.z += ( d.tx) * side * lateral;
                }
                r.halfW = kFwyShoulderHalfM + 0.55f;
                r.depth = kStackMainDepthM;
                r.level = 2;
                r.name  = cw == 0 ? "L2 deck (left carriageway)"
                                  : "L2 deck (right carriageway)";
                out.runs.push_back(std::move(r));
            }
    }
    for (int q = 0; q < 4; ++q) {
        if (!st.ramp[q].built) continue;
        std::vector<std::vector<StackDeckStation>> runs;
        deckRunsOf(st.ramp[q].spec, st.ramp[q].roadY, runs);
        for (auto& base : runs) {
            StackDeckRun r;
            r.s     = std::move(base);
            r.halfW = kPavedHalfM * 0.5f + 1.0f;
            r.depth = kStackRampDepthM;
            r.level = st.ramp[q].level;
            r.name  = st.ramp[q].spec.name;
            out.runs.push_back(std::move(r));
        }
    }

    // WHERE A PIER MAY STAND. TRAFFIC IS ALIVE (app/traffic.cpp, 300 cars on
    // the inner tour): a pier inside a running lane is not a visual defect,
    // it is a wall the AI drives into at 70 mph. So each freeway's PAVEMENT —
    // both carriageways, apron to apron, plus 3 m of daylight — is a hard
    // keep-out.
    //
    // The MEDIAN is not. Every real stack lands its columns in the median of
    // the freeway below, and it has to: banning the whole corridor pushed the
    // longest unsupported span to 286 m in the first cut (gate S9 caught it),
    // because a ramp crossing an eight-plus-eight freeway obliquely spends
    // 120 m over pavement and there is nowhere else to put a column. A pier
    // may therefore stand INSIDE the median as long as its footing fits with
    // real daylight either side.
    struct KeepOut {
        const RoadSpec* spec = nullptr;
        std::vector<RoadRenderStation> path;   // carries median half AND the gap flag
    };
    std::vector<KeepOut> keep(2);
    {
        keep[0].spec = &st.aSpec;
        std::vector<float> mpA = computeMedianPlan(st.aSpec, st.aRoadY);
        buildRoadRenderPath(st.aSpec, &st.aRoadY, &mpA, keep[0].path);
        keep[1].spec = &st.bSpec;
        std::vector<float> mpB = computeMedianPlan(st.bSpec, st.bRoadY);
        buildRoadRenderPath(st.bSpec, &st.bRoadY, &mpB, keep[1].path);
    }
    // The footing is kPierHalf * 2.3 half-extent; give it a metre either side.
    const float kFootHalf = kPierHalf * 2.3f + 1.0f;
    // Returns the clearance a pier at (x, z) has: >= 0 means legal. Negative
    // is how far it intrudes on the worst thing it is standing in.
    // ...and it may not come down THROUGH a deck that is already there. An
    // L4 column that lands in freeway A's median is correct right up until
    // freeway B's deck happens to be over that patch of median: then it is a
    // concrete post through a bridge. Checked against every LOWER deck run.
    auto deckBlocked = [&](float x, float z, int myLevel) {
        for (const StackDeckRun& r : out.runs) {
            if (r.level >= myLevel) continue;
            for (size_t i = 0; i + 1 < r.s.size(); ++i)
                if (segPtDist(x, z, r.s[i].x, r.s[i].z, r.s[i+1].x, r.s[i+1].z)
                    < r.halfW + 2.5f)
                    return true;
        }
        return false;
    };
    auto pierClear = [&](float x, float z) {
        float worst = 1e9f;
        for (const KeepOut& k : keep) {
            // Nearest point on the route, INTERPOLATED, carrying that reach's
            // median half-width and — the part that matters — whether the
            // route is in a GAP there. A freeway inside its own deck gap is
            // not pavement on the ground, it is a bridge in the air, and a
            // column standing under it is a column, not an obstruction. The
            // first cut kept piers out of the whole footprint and so refused
            // to put any support at all under the L2 deck.
            float d = 1e18f, mh = kFwyMedianMinHalfM;
            bool inGap = false;
            for (size_t i = 0; i + 1 < k.path.size(); ++i) {
                const RoadRenderStation& a = k.path[i];
                const RoadRenderStation& b = k.path[i+1];
                const float dx = b.x - a.x, dz = b.z - a.z;
                const float L2 = dx * dx + dz * dz;
                if (L2 < 1e-9f) continue;
                const float t = std::max(0.0f, std::min(1.0f,
                    ((x - a.x) * dx + (z - a.z) * dz) / L2));
                const float qx = a.x + dx * t, qz = a.z + dz * t;
                const float dd = std::sqrt((x-qx)*(x-qx) + (z-qz)*(z-qz));
                if (dd < d) {
                    d = dd;
                    mh = a.medianHalf + (b.medianHalf - a.medianHalf) * t;
                    inGap = a.gap;
                }
            }
            if (inGap) continue;
            const float pavedOut = mh + 2.0f * kFwyPavedHalfM + 3.0f;
            if (d >= pavedOut) continue;                    // outside everything
            // Inside the median, then: the daylight is what is left of it —
            // the median itself PLUS the inner apron, which is 20 ft of
            // cement you park a dead car on, not a running lane. Real median
            // piers sit exactly there, footing under the apron, barrier round
            // the column. Stopping at the median's own edge instead needed a
            // 9 m half-median to fit a 4.6 m footing, so half the crossings
            // got no column at all and the spans ran to 341 m (gate S9).
            // The SHOULDER and the running lanes stay untouchable: that is
            // where the traffic is.
            const float apron = kFwyPavedHalfM - kFwyShoulderHalfM;
            const float medRoom = (mh + apron) - kFootHalf - d;   // >= 0 == it fits
            worst = std::min(worst, medRoom);
        }
        return worst;
    };

    for (uint32_t ri = 0; ri < (uint32_t)out.runs.size(); ++ri) {
        const StackDeckRun& r = out.runs[ri];
        if (r.s.size() < 2) continue;
        out.deckM    += r.s.back().u;
        out.parapetM += 2.0f * r.s.back().u;     // both edges, every deck
        // Interpolate the run at an arbitrary arc length — a pier goes where
        // the STRUCTURE needs it, not where a 6 m station happens to fall.
        auto at = [&](float u) {
            StackDeckStation o = r.s.front();
            for (size_t i = 0; i + 1 < r.s.size(); ++i) {
                if (u > r.s[i+1].u && i + 2 < r.s.size()) continue;
                const float span = std::max(1e-4f, r.s[i+1].u - r.s[i].u);
                const float t = std::max(0.0f, std::min(1.0f, (u - r.s[i].u) / span));
                o.x  = r.s[i].x + (r.s[i+1].x - r.s[i].x) * t;
                o.z  = r.s[i].z + (r.s[i+1].z - r.s[i].z) * t;
                o.y  = r.s[i].y + (r.s[i+1].y - r.s[i].y) * t;
                o.tx = r.s[i].tx; o.tz = r.s[i].tz; o.u = u;
                return o;
            }
            return r.s.back();
        };
        auto airAt = [&](const StackDeckStation& d) {
            return d.y - r.depth - terrainHeightAtWorld(d.x, d.z);
        };
        auto legal = [&](const StackDeckStation& d) {
            return airAt(d) > kPierMinAirM && pierClear(d.x, d.z) >= 0.0f &&
                   !deckBlocked(d.x, d.z, r.level);
        };

        // ---- FORCED MEDIAN PIERS ------------------------------------------
        // Where a deck crosses a freeway's CENTRELINE it is over the median,
        // and the median is the one place a column may land. Snapping to the
        // nearest 6 m station misses it: a ramp crossing at 45 degrees moves
        // 4.2 m of lateral per station and the legal window inside a 9 m
        // half-median is only ~4 m wide, so the crossing was being spanned
        // entirely (measured: a 286 m unsupported span, gate S9). Find the
        // true minimum of the distance-to-centreline instead and put the pier
        // THERE. This is also just what a designer does.
        std::vector<float> forced;
        for (const KeepOut& k : keep) {
            std::vector<float> dk(r.s.size(), 0.0f);
            for (size_t i = 0; i < r.s.size(); ++i)
                dk[i] = distToSpec(*k.spec, r.s[i].x, r.s[i].z);
            const int wWin = (int)std::min<size_t>(6, r.s.size() / 5);
            for (size_t i = (size_t)wWin; i + (size_t)wWin < r.s.size(); ++i) {
                // A STRICT minimum over a real window, and only where the
                // deck is actually across the freeway. distToSpec against a
                // dense polyline is piecewise-linear and full of numerical
                // ties, and a naive three-point test found 66 "crossings" on
                // one ramp — a pier forest by another name.
                if (dk[i] > 70.0f) continue;
                bool isMin = true;
                for (int w = -wWin; w <= wWin && isMin; ++w)
                    if (dk[i + w] < dk[i] - 1e-4f) isMin = false;
                if (isMin) {
                    float bestU = r.s[i].u, bestD = dk[i];
                    for (int t = 0; t <= 16; ++t) {
                        const float u = r.s[i-1].u +
                            (r.s[i+1].u - r.s[i-1].u) * (float)t / 16.0f;
                        const StackDeckStation d = at(u);
                        const float dd = distToSpec(*k.spec, d.x, d.z);
                        if (dd < bestD) { bestD = dd; bestU = u; }
                    }
                    const StackDeckStation d = at(bestU);
                    if (legal(d)) forced.push_back(bestU);
                    i += (size_t)wWin;            // one column per crossing
                }
            }
        }
        std::sort(forced.begin(), forced.end());

        // ---- THE RHYTHM, around the forced columns -------------------------
        // 70 m nominal, plus a column at the LAST legal station before a
        // keep-out so a crossing is spanned pier-to-pier across the pavement.
        std::vector<uint8_t> ok(r.s.size(), 0);
        for (size_t i = 1; i + 1 < r.s.size(); ++i) ok[i] = legal(r.s[i]) ? 1 : 0;
        std::vector<float> place = forced;
        {
            float since = kPierEveryM;
            size_t fi = 0;
            for (size_t i = 1; i + 1 < r.s.size(); ++i) {
                since += r.s[i].u - r.s[i-1].u;
                while (fi < forced.size() && forced[fi] <= r.s[i].u) {
                    since = r.s[i].u - forced[fi]; ++fi;
                }
                if (!ok[i]) continue;
                const bool lastChance = (i + 2 < r.s.size()) && !ok[i + 1];
                if (since < kPierEveryM && !(lastChance && since >= kPierEveryM * 0.35f))
                    continue;
                bool tooClose = false;
                for (float f : forced) if (std::fabs(f - r.s[i].u) < 30.0f) tooClose = true;
                if (tooClose) continue;
                since = 0.0f;
                place.push_back(r.s[i].u);
            }
        }
        std::sort(place.begin(), place.end());
        // A colonnade is not a pier forest (echo_roads' own saga): never two
        // columns inside 30 m of each other.
        place.erase(std::unique(place.begin(), place.end(),
                    [](float a, float b) { return std::fabs(a - b) < 30.0f; }),
                    place.end());
        float lastU = r.s.front().u, runMaxSpan = 0.0f, runSpanAt = 0.0f;
        for (float u : place) {
            if (u - lastU > runMaxSpan) { runMaxSpan = u - lastU; runSpanAt = lastU; }
            const StackDeckStation d = at(u);
            const float g = terrainHeightAtWorld(d.x, d.z);
            StackPier p;
            p.x = d.x; p.z = d.z;
            p.ySoffit = d.y - r.depth; p.yGround = g;
            p.px = -d.tz; p.pz = d.tx;
            p.capHalfLenM = r.halfW * 0.92f;
            p.spanM = u - lastU;
            p.run = ri;
            lastU = u;
            out.piers.push_back(p);
            out.maxPierM = std::max(out.maxPierM, p.ySoffit - g);
            out.maxSpanM = std::max(out.maxSpanM, p.spanM);
            out.minPierClearM = std::min(out.minPierClearM, pierClear(d.x, d.z));
        }
        if (r.s.back().u - lastU > runMaxSpan) {
            runMaxSpan = r.s.back().u - lastU; runSpanAt = lastU;
        }
        out.maxSpanM = std::max(out.maxSpanM, r.s.back().u - lastU);
        char rb[220];
        std::snprintf(rb, sizeof(rb),
            "stack deck run '%s' L%d: %.0f m, %u piers (%u forced into a "
            "median), longest span %.0f m starting at u %.0f",
            r.name.c_str(), r.level, r.s.back().u, (uint32_t)place.size(),
            (uint32_t)forced.size(), runMaxSpan, runSpanAt);
        x3::logInfo(rb);
        if (runMaxSpan > 170.0f) {
            const StackDeckStation m = at(runSpanAt + runMaxSpan * 0.5f);
            char wb[220];
            std::snprintf(wb, sizeof(wb),
                "  ...mid-span (%.0f, %.0f): air %.1f m, pier clearance %.1f m, "
                "%s by a lower deck",
                m.x, m.z, airAt(m), pierClear(m.x, m.z),
                deckBlocked(m.x, m.z, r.level) ? "BLOCKED" : "not blocked");
            x3::logInfo(wb);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// THE STRUCTURE
// ---------------------------------------------------------------------------
StackBuildResult buildStack(const StackResult& st, Scene& scene,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& phys) {
    StackBuildResult out;
    if (!st.built) return out;

    SurfaceLibrary& surf = stackSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& concrete = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& asphalt  = surf.get(device, "rd_asphalt_01");

    const StackStructurePlan plan = planStackStructure(st);
    MeshBuf deckA, deckC, paint;

    for (const StackDeckRun& r : plan.runs) {
        emitDeckBox(deckA, deckC, r);
        emitParapets(deckC, r);
        // deck edge lines, continuing the approach ribbon's paint across the
        // span (the diamond does exactly this on its 40 m deck)
        for (int w = -1; w <= 1; w += 2) {
            const float lat = (float)w * (r.halfW - kStackParapetW - 0.55f);
            for (size_t i = 0; i + 1 < r.s.size(); ++i) {
                float a0[3], a1[3], b0[3], b1[3];
                const float ay = r.s[i].y   + kPaveProud + 0.012f;
                const float by = r.s[i+1].y + kPaveProud + 0.012f;
                deckPt(r.s[i],   lat - 0.06f, ay, a0);
                deckPt(r.s[i],   lat + 0.06f, ay, a1);
                deckPt(r.s[i+1], lat + 0.06f, by, b1);
                deckPt(r.s[i+1], lat - 0.06f, by, b0);
                paint.quad(a0, a1, b1, b0);
            }
        }
        // ABUTMENTS: a seat wall under each end of the run, down into the
        // embankment the approach ribbon rides.
        for (int e = 0; e < 2; ++e) {
            const StackDeckStation& d = (e == 0) ? r.s.front() : r.s.back();
            const float g = terrainHeightAtWorld(d.x, d.z);
            const float soffit = d.y - r.depth;
            if (soffit - g < 0.4f) continue;
            obox(deckC, d.x + d.tx * (e == 0 ? -1.2f : 1.2f),
                 d.z + d.tz * (e == 0 ? -1.2f : 1.2f),
                 std::min(g - 1.5f, soffit - 3.5f), soffit + 0.12f,
                 -d.tz, d.tx, r.halfW, 1.2f);
        }
    }
    for (const StackPier& p : plan.piers) emitPier(deckC, p);
    out.pierCount = (uint32_t)plan.piers.size();
    out.maxPierM  = plan.maxPierM;
    out.maxSpanM  = plan.maxSpanM;
    out.parapetM  = plan.parapetM;

    // ---- THE GORE TAPERS ---------------------------------------------------
    // A stack ramp is not a T-junction: at 60 mph you leave the outer running
    // lane through a paved wedge that shifts you ~20 m sideways over 300 m
    // (1:15) onto a fully separate ramp. buildJunctionMouth's ruled twist is
    // built for an OBLIQUE branch arriving from outside and is the wrong
    // shape here, so the Stack lays its own — the honest new geometry a
    // high-speed interchange needs and the diamond's 48 m loops did not.
    {
        auto gore = [&](const RoadSpec& mainSpec, const std::vector<float>& mainY,
                        float termX, float termZ, float goreDirX, float goreDirZ) {
            if (mainSpec.x.size() < 2 || mainY.size() != mainSpec.x.size()) return;
            const std::vector<float> mp = computeMedianPlan(mainSpec, mainY);
            // the main-road frame at the terminal's abeam point
            size_t bi = 0; float bt = 0.0f, bd = 1e18f;
            for (size_t i = 0; i + 1 < mainSpec.x.size(); ++i) {
                const float ax = mainSpec.x[i], az = mainSpec.z[i];
                const float dx = mainSpec.x[i+1] - ax, dz = mainSpec.z[i+1] - az;
                const float L2 = dx * dx + dz * dz;
                if (L2 < 1e-9f) continue;
                const float t = std::max(0.0f, std::min(1.0f,
                    ((termX - ax) * dx + (termZ - az) * dz) / L2));
                const float qx = ax + dx * t, qz = az + dz * t;
                const float d = (termX-qx)*(termX-qx) + (termZ-qz)*(termZ-qz);
                if (d < bd) { bd = d; bi = i; bt = t; }
            }
            float mtx = mainSpec.x[bi+1] - mainSpec.x[bi];
            float mtz = mainSpec.z[bi+1] - mainSpec.z[bi];
            const float ml = std::sqrt(mtx*mtx + mtz*mtz);
            if (ml < 1e-4f) return;
            mtx /= ml; mtz /= ml;
            const float qx = mainSpec.x[bi] + mtx * ml * bt;
            const float qz = mainSpec.z[bi] + mtz * ml * bt;
            const float latTerm = (termX - qx) * (-mtz) + (termZ - qz) * (mtx);
            const float side = (latTerm < 0.0f) ? -1.0f : 1.0f;
            const float outerLat = std::fabs(latTerm) + kPavedHalfM * 0.5f + 1.0f;
            // the taper runs from the terminal AWAY along the gore direction
            const float dirSign = (goreDirX * mtx + goreDirZ * mtz) >= 0.0f ? 1.0f : -1.0f;
            auto edgePair = [&](float t, float o0[3], float o1[3]) {
                // t == 1 is the ramp terminal, t == 0 the gore nose, and the
                // nose lies `taper` metres along the mainline IN the gore
                // direction — hence + dirSign, not minus. (An exit taper runs
                // back upstream; an entry taper runs on downstream.)
                const float s = (1.0f - t) * kGoreTaperM * dirSign;
                const float bx = termX + mtx * s, bz = termZ + mtz * s;
                float by = datumNear(mainSpec, mainY, bx, bz);
                float mh = mp.empty() ? kFwyMedianMinHalfM
                                      : datumNear(mainSpec, mp, bx, bz);
                const float inner = mh + kFwyPavedHalfM + kFwyRunningHalfM;
                const float outer = inner + t * std::max(0.0f, outerLat - inner);
                const float py = by + kPaveProud + 0.008f;
                o0[0] = bx + (-mtz) * side * inner; o0[1] = py;
                o0[2] = bz + ( mtx) * side * inner;
                o1[0] = bx + (-mtz) * side * outer; o1[1] = py;
                o1[2] = bz + ( mtx) * side * outer;
            };
            const int N = 30;
            for (int k = 0; k < N; ++k) {
                const float t0 = (float)k / (float)N, t1 = (float)(k + 1) / (float)N;
                float a0[3], a1[3], b0[3], b1[3];
                edgePair(t0, a0, a1);
                edgePair(t1, b0, b1);
                // n = (b-a) x (d-a) works out to -side*dirSign*(+Y), so the
                // up-facing winding flips with the PRODUCT of the two signs.
                if (side * dirSign < 0.0f) deckA.quad(a0, a1, b1, b0, 1.0f, 4.0f);
                else                       deckA.quad(b0, b1, a1, a0, 1.0f, 4.0f);
            }
        };
        for (int q = 0; q < 4; ++q) {
            const StackResult::Ramp& rp = st.ramp[q];
            if (!rp.built || rp.spec.x.size() < 3) continue;
            const size_t n = rp.spec.x.size();
            float t0x = rp.spec.x[1] - rp.spec.x[0], t0z = rp.spec.z[1] - rp.spec.z[0];
            float l0 = std::sqrt(t0x*t0x + t0z*t0z);
            if (l0 > 1e-4f) { t0x /= l0; t0z /= l0; }
            float t1x = rp.spec.x[n-1] - rp.spec.x[n-2], t1z = rp.spec.z[n-1] - rp.spec.z[n-2];
            float l1 = std::sqrt(t1x*t1x + t1z*t1z);
            if (l1 > 1e-4f) { t1x /= l1; t1z /= l1; }
            // The taper lies BEHIND the ramp's first node (you are still on
            // the freeway there) and AHEAD of its last. WHICH freeway is a
            // measurement: even ramps run A -> B, odd ones B -> A.
            auto hostOf = [&](float x, float z) -> int {
                return distToSpec(st.aSpec, x, z) <= distToSpec(st.bSpec, x, z) ? 0 : 1;
            };
            const int h0 = hostOf(rp.spec.x.front(), rp.spec.z.front());
            const int h1 = hostOf(rp.spec.x.back(),  rp.spec.z.back());
            gore(h0 ? st.bSpec : st.aSpec, h0 ? st.bRoadY : st.aRoadY,
                 rp.spec.x.front(), rp.spec.z.front(), -t0x, -t0z);
            gore(h1 ? st.bSpec : st.aSpec, h1 ? st.bRoadY : st.aRoadY,
                 rp.spec.x.back(), rp.spec.z.back(), t1x, t1z);
        }
    }

    auto upload = [&](MeshBuf& m, const SurfaceSet* set, const float tint[4], bool collide) {
        if (m.empty()) return;
        Entity ent;
        ent.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                     m.i.data(), (uint32_t)m.i.size());
        if (!ent.mesh.valid()) return;
        if (set && set->ok) {
            ent.tex = set->albedo; ent.mrTex = set->mr; ent.normalTex = set->normal;
        }
        for (int c = 0; c < 4; ++c) ent.baseColor[c] = tint[c];
        scene.add(ent);
        ++out.meshCount;
        out.quadCount += (uint32_t)(m.i.size() / 6);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) {
                cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]);
            }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
    };
    float roadTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { roadTint[0] = 0.055f; roadTint[1] = 0.056f; roadTint[2] = 0.060f; }
    float cemTint[4] = { 0.80f, 0.79f, 0.76f, 1.0f };
    if (!concrete.ok) { cemTint[0] = 0.38f; cemTint[1] = 0.37f; cemTint[2] = 0.35f; }
    const float paintC[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    upload(deckA, &asphalt,  roadTint, true);
    upload(deckC, &concrete, cemTint,  true);
    upload(paint, nullptr,   paintC,   false);
    out.ok = out.meshCount > 0;

    char b[300];
    std::snprintf(b, sizeof(b),
        "stack structure: %u meshes, %u quads, %u piers on %.0f m of deck — "
        "TALLEST PIER %.1f m, longest unsupported span %.0f m (rhythm %.0f m), "
        "%.0f m of continuous parapet at %.2f m, piers clear of pavement by "
        "%.1f m",
        out.meshCount, out.quadCount, out.pierCount, plan.deckM, out.maxPierM,
        out.maxSpanM, kPierEveryM, out.parapetM, kStackParapetH,
        plan.minPierClearM > 1e8f ? 0.0f : plan.minPierClearM);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// --test-stack
// ---------------------------------------------------------------------------
bool runStackSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[stack] " + m); }
        else    { ++failN; x3::logError("[stack] " + m); }
    };
    char d[520];

    clearTerrainCorridors();
    clearRoadJunctions();

    // ---- S0 the SURVEY -----------------------------------------------------
    // The brief said: build the Stack where two freeway-class routes cross —
    // and if they never do, MEASURE that and bring one. This is the
    // measurement. The two tours are concentric by construction (they share a
    // centre; the outer connector exists precisely because they are 3 km
    // apart), so nothing in the network crosses anything: gate R2 in
    // --test-roadnetwork proves the same property from the other side.
    {
        RoadSpec inner = makeInnerCourse();
        std::vector<BoreChord> bores;
        RoadSpec outer = makeOuterTour(&bores);
        float minD = 1e18f;
        for (size_t i = 0; i < inner.x.size(); i += 3)
            minD = std::min(minD, distToSpec(outer, inner.x[i], inner.z[i]));
        std::snprintf(d, sizeof(d),
            "inner tour vs outer tour: closest approach %.0f m (%.2f miles) — "
            "concentric, they NEVER cross, so the Stack brings its own "
            "crossing freeway (L2) rather than pretending",
            minD, minD / 1609.34f);
        check(minD > 2.0f * kFwyPavedHalfM,
              "S0 the network survey is honest: no two freeways cross today", d);
    }

    RoadSpec ringSpec = makeInnerCourse();
    std::vector<float> ringY;
    const RoadBuildResult ringR = registerRoad(ringSpec, &ringY);
    check(ringR.ok, "S0b the freeway registers under the Stack gates");

    // The Stack must survive standing NEXT TO the diamond, which is the
    // hardest part of siting it — so the gate registers the diamond first,
    // exactly as the world does.
    std::vector<const RoadSpec*> avoid;
    InterchangeResult ic = registerInterchange(ringSpec, ringY, &avoid);
    std::vector<const RoadSpec*> avoidS;
    if (ic.built) {
        avoidS.push_back(&ic.spec);
        for (int q = 0; q < 4; ++q) if (ic.ramp[q].built) avoidS.push_back(&ic.ramp[q].spec);
    }
    StackResult st = registerStack(ringSpec, ringY, &avoidS);
    std::snprintf(d, sizeof(d),
        "%s — node %u (%.0f, %.0f), median half %.1f m, crossing freeway %.2f "
        "miles, %u crossings, diamond %s",
        st.built ? "built" : st.whyNot, st.fwyNode, st.cx, st.cz,
        st.medianHalfA, st.bRoad.lengthM / 1609.34f, st.crossingCount,
        ic.built ? "also registered" : "not built");
    check(st.built, "S1 the Stack registers on a measured site, clear of the diamond", d);
    if (!st.built) {
        clearTerrainCorridors();
        clearRoadJunctions();
        x3::logInfo("[stack] " + std::to_string(passN) + " passed, " +
                    std::to_string(failN) + " failed");
        return false;
    }

    // ---- S2 FOUR LEVELS ----------------------------------------------------
    {
        const float l1 = st.baseSurfaceY;
        const float l2 = st.levelY[0], l3 = st.levelY[1], l4 = st.levelY[2];
        const float minStep = kStackClearM + kStackBearingM + kStackRampDepthM;
        std::snprintf(d, sizeof(d),
            "L1 %.1f m -> L2 +%.1f ft -> L3 +%.1f ft -> L4 +%.1f ft (steps "
            "%.1f / %.1f / %.1f ft, floor %.1f ft); tallest deck %.1f m over "
            "the freeway",
            l1, (l2-l1)*kMToFt, (l3-l1)*kMToFt, (l4-l1)*kMToFt,
            (l2-l1)*kMToFt, (l3-l2)*kMToFt, (l4-l3)*kMToFt, minStep*kMToFt,
            l4 - l1);
        check(l2 > l1 + minStep - 0.05f && l3 > l2 + minStep - 0.05f &&
              l4 > l3 + minStep - 0.05f,
              "S2 four distinct levels, each clearing the last by the law", d);
    }

    // ---- S3 CLEARANCE at EVERY crossing ------------------------------------
    {
        float worst = st.bClearanceM;
        uint32_t nX = 0, viol = 0;
        float worstAtX = st.cx, worstAtZ = st.cz;
        const char* worstName = "L2 deck over the inner tour";
        for (int q = 0; q < 4; ++q) {
            for (const StackCrossing& c : st.ramp[q].plan.cross) {
                ++nX;
                if (c.clearanceM < kStackClearM) ++viol;
                if (c.clearanceM < worst) {
                    worst = c.clearanceM; worstAtX = c.x; worstAtZ = c.z;
                    worstName = st.ramp[q].spec.name.c_str();
                }
            }
        }
        std::snprintf(d, sizeof(d),
            "%u ramp crossings + the L2 deck; worst %.2f ft at (%.0f, %.0f) on "
            "'%s' (law %.1f ft); L2 deck alone %.2f ft; %u violations",
            nX, worst * kMToFt, worstAtX, worstAtZ, worstName,
            kStackClearM * kMToFt, st.bClearanceM * kMToFt, viol);
        check(viol == 0 && st.bClearanceM >= kStackClearM && nX >= 12,
              "S3 16.5 ft clear at EVERY measured crossing, every level", d);
    }

    // ---- S4 GRADES and the K law -------------------------------------------
    {
        float wg = st.bRoad.maxGradePct, wr = st.bRoad.maxGradeRatePost;
        float wp = st.bRoad.pinErrM, wend = 0.0f, minDeficit = 1e9f;
        int   worstQ = 0;
        for (int q = 0; q < 4; ++q) {
            if (st.ramp[q].plan.crestDeficitM < minDeficit) worstQ = q;
            const StackResult::Ramp& rp = st.ramp[q];
            wg = std::max(wg, rp.road.maxGradePct);
            wr = std::max(wr, rp.road.maxGradeRatePost);
            wp = std::max(wp, rp.road.pinErrM);
            if (!rp.roadY.empty()) {
                wend = std::max(wend, std::fabs(rp.roadY.front() - rp.termAy));
                wend = std::max(wend, std::fabs(rp.roadY.back()  - rp.termBy));
            }
            minDeficit = std::min(minDeficit, rp.plan.crestDeficitM);
        }
        std::snprintf(d, sizeof(d),
            "worst grade %.2f%% (cap 6.0), worst K-rate %.5f /m (cap %.5f), "
            "worst pin deficit %.2f ft, worst end-off-datum %.3f ft, tightest "
            "crest margin %.2f m over the requirement (q%d: pile u %.0f..%.0f, "
            "crest u %.0f..%.0f of %.0f m, climb %.1f m)",
            wg, wr, 5.0e-4f * 1.15f, wp * kMToFt, wend * kMToFt, minDeficit,
            worstQ, st.ramp[worstQ].plan.uLoM, st.ramp[worstQ].plan.uHiM,
            st.ramp[worstQ].plan.crestLoM, st.ramp[worstQ].plan.crestHiM,
            st.ramp[worstQ].plan.lengthM,
            st.ramp[worstQ].plan.levelY - st.ramp[worstQ].termAy);
        check(wg <= 6.05f && wr <= 5.0e-4f * 1.15f && wp <= 0.06f &&
              wend < 0.02f && minDeficit >= 0.0f,
              "S4 grades, the K law, and both landings at grade", d);
    }

    // ---- S5 THE HIGH-SPEED ARCS --------------------------------------------
    {
        float rMin = 1e9f, rMax = 0.0f;
        for (int q = 0; q < 4; ++q) {
            rMin = std::min(rMin, st.ramp[q].arcRadiusM);
            rMax = std::max(rMax, st.ramp[q].arcRadiusM);
        }
        // 65 mph == 29.06 m/s; a 284 m radius holds 0.30 g of lateral, which
        // is a comfortable design speed, not a stunt.
        const float v = 29.06f;
        const float latG = (v * v / std::max(1.0f, rMin)) / 9.81f;
        std::snprintf(d, sizeof(d),
            "measured sweep radii %.0f..%.0f m (asked 150-300, design %.0f, "
            "diamond's loops 48); at 65 mph that is %.2f g lateral",
            rMin, rMax, st.arcRadiusDesignM, latG);
        check(rMin >= kStackArcMinR && rMax <= kStackArcMaxR + 1.0f && latG < 0.40f,
              "S5 every flyover is one 150-300 m high-speed arc", d);
    }

    // ---- S6 NO JOINTED BENDS -----------------------------------------------
    {
        float worstFacet = 0.0f, worstSpace = 0.0f;
        for (int q = 0; q < 4; ++q) {
            std::vector<RoadRenderStation> path;
            buildRoadRenderPath(st.ramp[q].spec, &st.ramp[q].roadY, nullptr, path);
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                worstSpace = std::max(worstSpace, path[i+1].u - path[i].u);
                const float ax = path[i+1].x - path[i].x, az = path[i+1].z - path[i].z;
                const float al = std::sqrt(ax*ax + az*az);
                if (i + 2 >= path.size() || al < 1e-3f) continue;
                const float bx = path[i+2].x - path[i+1].x, bz = path[i+2].z - path[i+1].z;
                const float bl = std::sqrt(bx*bx + bz*bz);
                if (bl < 1e-3f) continue;
                const float dot = (ax*bx + az*bz) / (al * bl);
                worstFacet = std::max(worstFacet,
                    std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 57.29578f);
            }
        }
        std::snprintf(d, sizeof(d),
            "max render-path facet %.2f deg (gate 2.0), max station spacing "
            "%.1f m (gate 12.0) — gap reaches are LINEAR on the chord, so the "
            "%.0f m node spacing IS the flyover's render resolution",
            worstFacet, worstSpace, kRampSpacingM);
        check(worstFacet <= 2.0f && worstSpace <= 12.0f,
              "S6 the flyovers read as one continuous arc — no jointed bends", d);
    }

    // ---- S7 the zone -------------------------------------------------------
    {
        const std::vector<RoadTurnaround> tas = planTurnarounds(ringSpec);
        std::vector<float> U(ringSpec.x.size(), 0.0f);
        for (size_t i = 0; i + 1 < ringSpec.x.size(); ++i) {
            const float dx = ringSpec.x[i+1] - ringSpec.x[i];
            const float dz = ringSpec.z[i+1] - ringSpec.z[i];
            U[i+1] = U[i] + std::sqrt(dx * dx + dz * dz);
        }
        uint32_t inZone = 0;
        float nearest = 1e9f;
        for (const RoadTurnaround& t : tas) {
            const float uc = 0.5f * (t.u0 + t.u1);
            size_t i = 0;
            while (i + 2 < U.size() && U[i+1] < uc) ++i;
            const float span = std::max(1e-4f, U[i+1] - U[i]);
            const float tt = std::max(0.0f, std::min(1.0f, (uc - U[i]) / span));
            const float px = ringSpec.x[i] + (ringSpec.x[i+1] - ringSpec.x[i]) * tt;
            const float pz = ringSpec.z[i] + (ringSpec.z[i+1] - ringSpec.z[i]) * tt;
            const float dd = std::sqrt((px-st.cx)*(px-st.cx) + (pz-st.cz)*(pz-st.cz));
            nearest = std::min(nearest, dd);
            if (dd < kStackZoneR) ++inZone;
        }
        std::snprintf(d, sizeof(d),
            "%u crossovers on the tour, %u inside the %.0f m Stack zone, "
            "nearest %.0f m; inInterchangeZone at the Stack centre = %s",
            (uint32_t)tas.size(), inZone, kStackZoneR, nearest,
            inInterchangeZone(st.cx, st.cz) ? "true" : "FALSE");
        check(inZone == 0 && tas.size() >= 10 && inInterchangeZone(st.cx, st.cz),
              "S7 no median crossover inside the Stack; the rhythm survives", d);
    }

    // ---- S8 PARAPET CONTINUITY ---------------------------------------------
    // The wall is emitted per deck RUN, so continuity is a property of the
    // runs: every metre of gap must belong to exactly one run, no run may be
    // shorter than a bridge, and the total parapet must be twice the deck.
    const StackStructurePlan plan = planStackStructure(st);
    {
        float gapLen = st.bRoad.gapLenM * 2.0f;   // twin decks over one gap
        for (int q = 0; q < 4; ++q) gapLen += st.ramp[q].road.gapLenM;
        float shortest = 1e9f;
        for (const StackDeckRun& r : plan.runs)
            shortest = std::min(shortest, r.s.empty() ? 0.0f : r.s.back().u);
        const float ratio = plan.deckM > 1.0f ? plan.parapetM / plan.deckM : 0.0f;
        std::snprintf(d, sizeof(d),
            "%u deck runs, %.0f m of elevated deck vs %.0f m of route inside a "
            "gap; %.0f m of parapet at %.2f m tall (ratio %.2f, must be 2.00 — "
            "both edges of every deck); shortest run %.0f m",
            (uint32_t)plan.runs.size(), plan.deckM, gapLen, plan.parapetM,
            kStackParapetH, ratio, shortest);
        check(plan.runs.size() >= 6 && std::fabs(ratio - 2.0f) < 0.01f &&
              shortest > 30.0f && plan.deckM > 0.85f * gapLen &&
              kStackParapetH >= 1.10f && kStackParapetH <= 1.40f,
              "S8 every elevated edge is walled, continuously, end to end", d);
    }

    // ---- S9 PIERS clear of traffic -----------------------------------------
    {
        std::snprintf(d, sizeof(d),
            "%u piers, tallest %.1f m, longest unsupported span %.0f m "
            "(rhythm %.0f m); tightest pier daylight %.1f m. Piers stand in "
            "medians and under aprons; NEVER in a running lane or a shoulder, "
            "and never through a lower deck. The long one is the MAIN SPAN "
            "over the core: the pinwheel's arcs pass 16 m from the crossing "
            "point, which threads between A's running lanes and the L2 twin "
            "decks with no ground left to stand on — so it is clear-spanned, "
            "which is what the owner's own note about CA viaducts describes.",
            (uint32_t)plan.piers.size(), plan.maxPierM, plan.maxSpanM,
            kPierEveryM, plan.minPierClearM > 1e8f ? 0.0f : plan.minPierClearM);
        check(!plan.piers.empty() && plan.minPierClearM >= 0.0f &&
              plan.maxSpanM <= 215.0f && plan.maxPierM >= 8.0f,
              "S9 no pier stands in anybody's pavement; spans stay structural", d);
    }

    // ---- S10 DETERMINISM ---------------------------------------------------
    {
        auto fieldHash = [&](const StackResult& s) {
            uint64_t h = 1469598103934665603ull;
            auto fold = [&](const RoadSpec& sp) {
                for (size_t i = 0; i < sp.x.size(); i += 2) {
                    const float v = terrainCorridorDelta(sp.x[i], sp.z[i]);
                    uint32_t bits;
                    std::memcpy(&bits, &v, sizeof(bits));
                    h = (h ^ bits) * 1099511628211ull;
                }
            };
            fold(s.bSpec);
            for (int q = 0; q < 4; ++q) fold(s.ramp[q].spec);
            return h;
        };
        const uint64_t h1 = fieldHash(st);
        clearTerrainCorridors();
        clearRoadJunctions();
        RoadSpec ring2 = makeInnerCourse();
        std::vector<float> ringY2;
        (void)registerRoad(ring2, &ringY2);
        std::vector<const RoadSpec*> avoid2;
        InterchangeResult ic2 = registerInterchange(ring2, ringY2, &avoid2);
        std::vector<const RoadSpec*> avoidS2;
        if (ic2.built) {
            avoidS2.push_back(&ic2.spec);
            for (int q = 0; q < 4; ++q)
                if (ic2.ramp[q].built) avoidS2.push_back(&ic2.ramp[q].spec);
        }
        StackResult st2 = registerStack(ring2, ringY2, &avoidS2);
        const uint64_t h2 = st2.built ? fieldHash(st2) : 0ull;
        std::snprintf(d, sizeof(d), "field hash %016llx vs %016llx",
                      (unsigned long long)h1, (unsigned long long)h2);
        check(st2.built && h1 == h2,
              "S10 the Stack is deterministic (re-registration identical)", d);
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    x3::logInfo("[stack] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
