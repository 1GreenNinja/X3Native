// tunnel_corridor — see tunnel_corridor.h for the design + the BL provenance.
// Clean-room, original work. No BL / third-party engine source was consulted or
// transcribed; the technique is documented in docs/design/BL_WORLD_PORT.md
// §2.2/§2.3/§3.2/§3.3/§4.3/§4.4 and re-implemented here from first principles.
#include "tunnel_corridor.h"

#include <cstdlib>
#include "mesh_prims.h"
#include "asset_root.h"        // assetRoot() — the surface_library mount point

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {
namespace {

// ---------------------------------------------------------------------------
// THE ROUTE, and why it is where it is.
//
// Picked by SAMPLING the canonical height field (worldTerrainConfig) — not
// guessed. A grid sweep over x,z in [-1600, 1600] at 48 m, x 8 azimuths, scoring
// (lowest ground in the middle 110 m) - (highest ground in the outer 70 m at
// either end), rejecting anything inside the four authored flat pads, within
// 200 m of the river spline, or dipping below +6 m (the coastal/basin band).
// The winner by a clear margin:
//
//   centre (-592, -352), heading 157.5 deg, prominence 29.5 m
//   spine profile every 20 m over +/-280 m:
//     12 13 17 20 20 22 23 30 35 39 45 48 52 55 55 54 53 51 47 43 37 31 21 14
//     13 15 15 16 17
//
// That is a clean isolated 55 m dome sitting on ~13-17 m country, ~1.3 km
// south-west of the facility pad, clear of every authored feature. The rise is
// long on the SW approach and short on the NE one, so the two portals do not
// look like mirror images — which is exactly what you want in a proof shot.
// ---------------------------------------------------------------------------
constexpr float kRouteCX = -592.0f, kRouteCZ = -352.0f;
constexpr float kRouteDirX = -0.92388f, kRouteDirZ = 0.38268f;   // 157.5 deg
constexpr float kRouteHalfLen = 320.0f;                          // +/- about the centre
constexpr int   kRouteNodes   = TerrainCorridor::kMaxNodes;      // 32

// The bore threshold: how much natural cover a station needs before the reach
// may be roofed instead of left as an open cutting.
constexpr float kBoreCut = kTcTubeCrownH + kTcShellThick + kTcMinSoilCover;   // 12.0 m

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Tiny mesh scratch buffer. Everything below emits into one of these and then
// uploads once — createMesh + (optionally) addStaticMesh from the same triangles.
// ---------------------------------------------------------------------------
struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t>            i;

    void vert(const float p[3], const float n[3], float u, float w) {
        x3::rhi::MeshVertex mv{};
        mv.pos[0] = p[0]; mv.pos[1] = p[1]; mv.pos[2] = p[2];
        mv.normal[0] = n[0]; mv.normal[1] = n[1]; mv.normal[2] = n[2];
        mv.uv[0] = u; mv.uv[1] = w;
        v.push_back(mv);
    }
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              const float n[3], float u0, float u1, float w0, float w1) {
        const uint32_t base = (uint32_t)v.size();
        vert(a, n, u0, w0); vert(b, n, u1, w0); vert(c, n, u1, w1); vert(d, n, u0, w1);
        i.push_back(base + 0); i.push_back(base + 1); i.push_back(base + 2);
        i.push_back(base + 0); i.push_back(base + 2); i.push_back(base + 3);
    }
    bool empty() const { return i.empty(); }
};

// Front faces are CCW (VK_FRONT_FACE_COUNTER_CLOCKWISE). Rather than reason
// about winding at every emit site, author the NORMALS (which the shading needs
// anyway) and let this pass reconcile the triangles to them — the same
// self-correcting trick app/mesh_prims.h's makeRamp uses.
void fixWinding(MeshBuf& mb) {
    for (size_t t = 0; t + 2 < mb.i.size(); t += 3) {
        const x3::rhi::MeshVertex& a = mb.v[mb.i[t]];
        const x3::rhi::MeshVertex& b = mb.v[mb.i[t + 1]];
        const x3::rhi::MeshVertex& c = mb.v[mb.i[t + 2]];
        const float e0[3] = { b.pos[0]-a.pos[0], b.pos[1]-a.pos[1], b.pos[2]-a.pos[2] };
        const float e1[3] = { c.pos[0]-a.pos[0], c.pos[1]-a.pos[1], c.pos[2]-a.pos[2] };
        const float g[3] = { e0[1]*e1[2]-e0[2]*e1[1], e0[2]*e1[0]-e0[0]*e1[2], e0[0]*e1[1]-e0[1]*e1[0] };
        const float n[3] = { a.normal[0]+b.normal[0]+c.normal[0],
                             a.normal[1]+b.normal[1]+c.normal[1],
                             a.normal[2]+b.normal[2]+c.normal[2] };
        if (g[0]*n[0] + g[1]*n[1] + g[2]*n[2] < 0.0f) std::swap(mb.i[t + 1], mb.i[t + 2]);
    }
}

// One oriented box in the corridor's constant frame (right / up / axis).
void obox(MeshBuf& mb, const float c[3], const float right[3], const float axis[3],
          float hr, float hu, float ha, float uvScale) {
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    auto P = [&](float r, float u, float a, float out[3]) {
        out[0] = c[0] + right[0]*r + up[0]*u + axis[0]*a;
        out[1] = c[1] + right[1]*r + up[1]*u + axis[1]*a;
        out[2] = c[2] + right[2]*r + up[2]*u + axis[2]*a;
    };
    float p[8][3];
    P(-hr, -hu, -ha, p[0]); P( hr, -hu, -ha, p[1]); P( hr,  hu, -ha, p[2]); P(-hr,  hu, -ha, p[3]);
    P(-hr, -hu,  ha, p[4]); P( hr, -hu,  ha, p[5]); P( hr,  hu,  ha, p[6]); P(-hr,  hu,  ha, p[7]);
    const float nR[3] = {  right[0],  right[1],  right[2] };
    const float nL[3] = { -right[0], -right[1], -right[2] };
    const float nU[3] = { 0.0f, 1.0f, 0.0f }, nD[3] = { 0.0f, -1.0f, 0.0f };
    const float nA[3] = {  axis[0],  axis[1],  axis[2] };
    const float nB[3] = { -axis[0], -axis[1], -axis[2] };
    const float su = hr * 2.0f * uvScale, sv = hu * 2.0f * uvScale, sa = ha * 2.0f * uvScale;
    mb.quad(p[7], p[6], p[2], p[3], nU, 0, su, 0, sa);
    mb.quad(p[0], p[1], p[5], p[4], nD, 0, su, 0, sa);
    mb.quad(p[1], p[2], p[6], p[5], nR, 0, sv, 0, sa);
    mb.quad(p[4], p[7], p[3], p[0], nL, 0, sv, 0, sa);
    mb.quad(p[5], p[6], p[7], p[4], nA, 0, su, 0, sv);
    mb.quad(p[0], p[3], p[2], p[1], nB, 0, su, 0, sv);
}

// ---------------------------------------------------------------------------
// The tunnel CROSS-SECTION (BL_WORLD_PORT.md §2.3's arch idea, re-derived).
// An open polyline in the (lateral, up) plane, walked from the +right springing
// over the crown to the -right springing. Short vertical walls carry the arch,
// which is a half-ellipse — a road bore, not a semicircular pipe.
// ---------------------------------------------------------------------------
struct Profile {
    static constexpr int kArch = 13;
    static constexpr int kN = kArch + 4;    // 2 floor points + 2 springings + arch
    float px[kN], py[kN];                   // section-space points
    float nx[kN], ny[kN];                   // OUTWARD unit normals
    float u [kN];                           // cumulative arc length (for UVs)
};

Profile makeProfile(float halfW, float wallH, float crownH) {
    Profile pr{};
    int k = 0;
    pr.px[k] = halfW; pr.py[k] = 0.0f;   ++k;
    pr.px[k] = halfW; pr.py[k] = wallH;  ++k;
    for (int a = 1; a <= Profile::kArch; ++a) {
        const float t = (float)a / (float)(Profile::kArch + 1);
        const float ang = t * 3.14159265f;
        pr.px[k] = halfW * std::cos(ang);
        pr.py[k] = wallH + (crownH - wallH) * std::sin(ang);
        ++k;
    }
    pr.px[k] = -halfW; pr.py[k] = wallH;  ++k;
    pr.px[k] = -halfW; pr.py[k] = 0.0f;   ++k;

    // Outward normals from the local tangent, disambiguated against the section
    // centroid so "outward" cannot flip on the arch.
    const float cx = 0.0f, cy = wallH * 0.55f;
    for (int j = 0; j < Profile::kN; ++j) {
        const int a = std::max(0, j - 1), b = std::min(Profile::kN - 1, j + 1);
        float tx = pr.px[b] - pr.px[a], ty = pr.py[b] - pr.py[a];
        const float tl = std::sqrt(tx*tx + ty*ty); if (tl > 1e-6f) { tx /= tl; ty /= tl; }
        float nx = ty, ny = -tx;
        if ((pr.px[j] - cx) * nx + (pr.py[j] - cy) * ny < 0.0f) { nx = -nx; ny = -ny; }
        pr.nx[j] = nx; pr.ny[j] = ny;
    }
    pr.u[0] = 0.0f;
    for (int j = 1; j < Profile::kN; ++j) {
        const float dx = pr.px[j] - pr.px[j-1], dy = pr.py[j] - pr.py[j-1];
        pr.u[j] = pr.u[j-1] + std::sqrt(dx*dx + dy*dy);
    }
    return pr;
}

// Offset a profile outward by `t` (the shell's outer skin). The two floor
// endpoints are pushed sideways only, so the shell still lands flat on y = 0.
Profile offsetProfile(const Profile& in, float t) {
    Profile out = in;
    for (int j = 0; j < Profile::kN; ++j) {
        out.px[j] = in.px[j] + in.nx[j] * t;
        out.py[j] = in.py[j] + in.ny[j] * t;
    }
    out.py[0] = 0.0f; out.py[Profile::kN - 1] = 0.0f;
    return out;
}

struct Frame { float p[3]; float s; };

// Sweep an open profile along the frames. `sign` = +1 to keep the profile's
// outward normals (an OUTER skin), -1 to face them inward (the drivable bore).
void emitSweep(MeshBuf& mb, const std::vector<Frame>& fr, const float right[3],
               const Profile& pr, float sign, float uvScale) {
    if (fr.size() < 2) return;
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const uint32_t base = (uint32_t)mb.v.size();
    for (size_t j = 0; j < fr.size(); ++j) {
        for (int k = 0; k < Profile::kN; ++k) {
            const float p[3] = {
                fr[j].p[0] + right[0]*pr.px[k] + up[0]*pr.py[k],
                fr[j].p[1] + right[1]*pr.px[k] + up[1]*pr.py[k],
                fr[j].p[2] + right[2]*pr.px[k] + up[2]*pr.py[k] };
            const float n[3] = {
                sign * (right[0]*pr.nx[k] + up[0]*pr.ny[k]),
                sign * (right[1]*pr.nx[k] + up[1]*pr.ny[k]),
                sign * (right[2]*pr.nx[k] + up[2]*pr.ny[k]) };
            mb.vert(p, n, pr.u[k] * uvScale, fr[j].s * uvScale);
        }
    }
    const uint32_t P = (uint32_t)Profile::kN;
    for (uint32_t j = 0; j + 1 < (uint32_t)fr.size(); ++j) {
        for (uint32_t k = 0; k + 1 < P; ++k) {
            const uint32_t a = base + j*P + k, b = a + 1, c = b + P, d = a + P;
            mb.i.push_back(a); mb.i.push_back(b); mb.i.push_back(c);
            mb.i.push_back(a); mb.i.push_back(c); mb.i.push_back(d);
        }
    }
}

// The PORTAL HEADWALL face: a flat ring from the bore's inner profile out to a
// rectangle, so the earth face the corridor's depth step leaves at the mouth is
// covered by concrete rather than showing as a raw terrain cliff. BL builds its
// portal from two pillars + a lintel (§3.2 buildArch); a swept ring is the same
// idea done once and correctly for an arched section.
void emitPortalFace(MeshBuf& mb, const float origin[3], const float right[3],
                    const float outN[3], const Profile& inner,
                    float rectHalfW, float rectTop, float rectBottom) {
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float ccx = 0.0f, ccy = kTcTubeWallH * 0.55f;
    float qx[Profile::kN], qy[Profile::kN];
    for (int k = 0; k < Profile::kN; ++k) {
        float dx = inner.px[k] - ccx, dy = inner.py[k] - ccy;
        const float dl = std::sqrt(dx*dx + dy*dy); if (dl > 1e-6f) { dx /= dl; dy /= dl; }
        float t = 1e9f;
        if (dx >  1e-4f) t = std::min(t, ( rectHalfW - ccx) / dx);
        if (dx < -1e-4f) t = std::min(t, (-rectHalfW - ccx) / dx);
        if (dy >  1e-4f) t = std::min(t, ( rectTop    - ccy) / dy);
        if (dy < -1e-4f) t = std::min(t, ( rectBottom - ccy) / dy);
        qx[k] = ccx + dx * t; qy[k] = ccy + dy * t;
    }
    // Pin the two floor corners to the rectangle's side edges AT ROAD LEVEL.
    // Left to the ray mapping they land part-way up the side, which turns the
    // slab silhouette into a lopsided wedge; taken straight to the rectangle's
    // bottom corner they become a diagonal that sticks out of the ground
    // wherever the trench floor sits below the road.
    qx[0] =  rectHalfW; qy[0] = 0.0f;
    qx[Profile::kN - 1] = -rectHalfW; qy[Profile::kN - 1] = 0.0f;
    auto W = [&](float sx, float sy, float out[3]) {
        out[0] = origin[0] + right[0]*sx + up[0]*sy;
        out[1] = origin[1] + right[1]*sx + up[1]*sy;
        out[2] = origin[2] + right[2]*sx + up[2]*sy;
    };
    for (int k = 0; k + 1 < Profile::kN; ++k) {
        float a[3], b[3], c[3], d[3];
        W(inner.px[k],   inner.py[k],   a);
        W(inner.px[k+1], inner.py[k+1], b);
        W(qx[k+1],       qy[k+1],       c);
        W(qx[k],         qy[k],         d);
        mb.quad(a, b, c, d, outN, inner.u[k]*0.2f, inner.u[k+1]*0.2f, 0.0f, 1.0f);
    }
    // FOOTINGS: the trench floor beside the road is not flat (the corridor
    // removes a constant depth, so the floor keeps the hillside's lateral
    // slope). Carry the slab down below road level on both flanks or you see
    // straight under the headwall on the downhill side.
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float xi = (float)sgn * inner.px[0];          // +/- tube half-width
        const float xo = (float)sgn * rectHalfW;
        float a[3], b[3], c[3], d[3];
        W(xi, 0.0f, a); W(xo, 0.0f, b); W(xo, rectBottom, c); W(xi, rectBottom, d);
        mb.quad(a, b, c, d, outN, 0.0f, 1.0f, 0.0f, 1.0f);
    }
}

// Upload one buffer as a Scene entity (+ optional static collision).
struct Material {
    x3::rhi::TextureHandle alb, mr, nrm;
    float tint[4] = { 1, 1, 1, 1 };
    float emissive[4] = { 0, 0, 0, 0 };
};

} // namespace

// ---------------------------------------------------------------------------
// TunnelRoute queries
// ---------------------------------------------------------------------------
float TunnelRoute::roadYAt(float s) const {
    if (st.empty()) return 0.0f;
    if (s <= st.front().s) return st.front().roadY;
    if (s >= st.back().s)  return st.back().roadY;
    for (size_t i = 1; i < st.size(); ++i) {
        if (s <= st[i].s) {
            const float t = (s - st[i-1].s) / std::max(1e-4f, st[i].s - st[i-1].s);
            return st[i-1].roadY + (st[i].roadY - st[i-1].roadY) * t;
        }
    }
    return st.back().roadY;
}

void TunnelRoute::posAt(float s, float out[3]) const {
    // `s` is arc length from node 0, which sits at -kRouteHalfLen about the
    // scan centre; fold it back to the centre-relative parameter.
    out[0] = kRouteCX + dirX * (s - kRouteHalfLen);
    out[2] = kRouteCZ + dirZ * (s - kRouteHalfLen);
    out[1] = roadYAt(s);
}

// ---------------------------------------------------------------------------
// registerTunnelCorridor — the BOOT step. Sample, grade, register, verify.
// ---------------------------------------------------------------------------
const TunnelRoute& registerTunnelCorridor() {
    static TunnelRoute route;
    static bool built = false;
    if (built) return route;
    built = true;

    route.dirX = kRouteDirX; route.dirZ = kRouteDirZ;
    const float rx = -kRouteDirZ, rz = kRouteDirX;    // unit lateral in XZ
    const float ds = (2.0f * kRouteHalfLen) / (float)(kRouteNodes - 1);
    route.totalLen = 2.0f * kRouteHalfLen;

    // --- 1) Sample the NATURAL field on the spine and across the corridor band.
    route.st.resize(kRouteNodes);
    for (int i = 0; i < kRouteNodes; ++i) {
        TunnelStation& n = route.st[i];
        const float sc = -kRouteHalfLen + ds * (float)i;   // centre-relative
        n.s = ds * (float)i;                               // arc length from node 0
        n.x = kRouteCX + kRouteDirX * sc;
        n.z = kRouteCZ + kRouteDirZ * sc;
        n.ground = terrainHeightAtWorld(n.x, n.z);
        n.latMin = n.latMax = n.ground;
        // Across the tube's footprint (+/- outer half-width + margin), because
        // the DEPTH is constant across the corridor's flat floor but the natural
        // ground under it is not — the depression follows the hillside's lateral
        // slope. The open cut must clear the HIGHEST ground in the band and the
        // bore must keep cover under the LOWEST.
        for (int k = -3; k <= 3; ++k) {
            if (k == 0) continue;
            const float off = (float)k * (kTcTubeHalfWidth + kTcShellThick + 0.6f) / 3.0f;
            const float h = terrainHeightAtWorld(n.x + rx * off, n.z + rz * off);
            n.latMin = std::min(n.latMin, h);
            n.latMax = std::max(n.latMax, h);
        }
    }

    // --- 2) Grade the road. Start on the surface (a shallow groove), then apply
    // a LOWERING-ONLY grade limit: a station may not sit higher than its
    // neighbour plus kTcMaxGrade*ds. Sweeping both ways to a fixed point yields
    // the highest profile that respects both the terrain and the grade, which is
    // exactly a road cutting. (Same shape as EchoRoads' raise-only relaxation,
    // inverted, because here the ground comes DOWN to the road.)
    for (int i = 0; i < kRouteNodes; ++i)
        route.st[i].roadY = route.st[i].latMax - kTcMinCut;
    const float rise = kTcMaxGrade * ds;
    for (int pass = 0; pass < 64; ++pass) {
        for (int i = 1; i < kRouteNodes; ++i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i-1].roadY + rise);
        for (int i = kRouteNodes - 2; i >= 0; --i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i+1].roadY + rise);
    }
    // Light smoothing so the ribbon has no node-to-node kink, still capped by
    // the terrain and re-grade-limited afterwards.
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<float> tmp(kRouteNodes);
        for (int i = 0; i < kRouteNodes; ++i) {
            const int a = std::max(0, i - 1), b = std::min(kRouteNodes - 1, i + 1);
            tmp[i] = 0.25f*route.st[a].roadY + 0.5f*route.st[i].roadY + 0.25f*route.st[b].roadY;
        }
        for (int i = 0; i < kRouteNodes; ++i)
            route.st[i].roadY = std::min(tmp[i], route.st[i].latMax - kTcMinCut);
        for (int i = 1; i < kRouteNodes; ++i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i-1].roadY + rise);
        for (int i = kRouteNodes - 2; i >= 0; --i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i+1].roadY + rise);
    }

    // --- 3) The DEPTH PROFILE — the two regimes, and the portal step between.
    for (int i = 0; i < kRouteNodes; ++i) {
        TunnelStation& n = route.st[i];
        const float coverAvail = n.latMin - n.roadY;      // usable cover over the tube
        if (coverAvail >= kBoreCut) {
            // BORED reach: take only the surplus above the tube's crown + soil,
            // and never more than kTcMaxScar — that cap is what keeps the ridge
            // a dip instead of a canyon (BL_WORLD_PORT.md §4.3b).
            n.depth = std::min(coverAvail - kBoreCut, kTcMaxScar);
            n.bore = true;
        } else {
            // OPEN CUTTING: clear the HIGHEST ground across the band down to the
            // road, so the ribbon is never buried on the uphill shoulder.
            n.depth = std::max(0.0f, n.latMax - n.roadY);
            n.bore = false;
        }
    }

    // --- 4) Register. One corridor, per the primitive's boot-time contract.
    TerrainCorridor c{};
    c.nodeCount = kRouteNodes;
    c.halfWidth = kTcCorridorHalfW;
    c.falloff   = kTcCorridorFall;
    for (int i = 0; i < kRouteNodes; ++i) {
        c.x[i] = route.st[i].x; c.z[i] = route.st[i].z; c.depth[i] = route.st[i].depth;
    }
    const bool ok = registerTerrainCorridor(c);
    if (!ok) { x3::logError("tunnel corridor: registerTerrainCorridor REJECTED the route"); return route; }

    // --- 5) Find the genuinely ENCLOSED span by re-reading the FINAL field.
    // The union-of-capsules formulation rounds the depth step over ~halfWidth,
    // so where the roof actually closes is NOT where the design intent says. Ask
    // the terrain, at 2 m along the spine and across the tube's footprint.
    const float needTop = kTcTubeCrownH + kTcShellThick + 1.2f;
    constexpr float kSampleDs = 2.0f;
    constexpr float kOpenTol  = 0.9f;   // "the trench floor is still at road level"
    const int NS = (int)(route.totalLen / kSampleDs) + 1;
    std::vector<float> clearance((size_t)NS, 0.0f);   // MIN over the tube band
    std::vector<float> roadBury ((size_t)NS, 0.0f);   // MAX over the ROAD band
    for (int i = 0; i < NS; ++i) {
        const float s = kSampleDs * (float)i;
        const float sc = -kRouteHalfLen + s;
        const float px = kRouteCX + kRouteDirX * sc, pz = kRouteCZ + kRouteDirZ * sc;
        const float ry = route.roadYAt(s);
        float hMin = 1e9f, hRoadMax = -1e9f;
        for (int k = -2; k <= 2; ++k) {
            const float off = (float)k * (kTcTubeHalfWidth + kTcShellThick) / 2.0f;
            hMin = std::min(hMin, terrainHeightAtWorld(px + rx*off, pz + rz*off));
        }
        for (int k = -2; k <= 2; ++k) {
            const float off = (float)k * kTcRoadHalfWidth / 2.0f;
            hRoadMax = std::max(hRoadMax, terrainHeightAtWorld(px + rx*off, pz + rz*off));
        }
        clearance[(size_t)i] = hMin - ry;
        roadBury [(size_t)i] = hRoadMax - ry;
    }
    // (a) the longest fully-covered run ...
    int bestA = 0, bestB = 0, runA = -1;
    for (int i = 0; i < NS; ++i) {
        const bool closed = clearance[(size_t)i] >= needTop;
        if (closed && runA < 0) runA = i;
        if ((!closed || i == NS - 1) && runA >= 0) {
            const int b = closed ? i : i - 1;
            if (b - runA > bestB - bestA) { bestA = runA; bestB = b; }
            runA = -1;
        }
    }
    route.coverS0 = kSampleDs * (float)bestA;
    route.coverS1 = kSampleDs * (float)bestB;
    route.boreValid = (route.coverS1 - route.coverS0) > 30.0f;
    // (b) ... grown outward over the PORTAL RAMPS, to the last station where the
    // ROAD ITSELF is genuinely clear. The test has to be the MAX over the road
    // band, not the min over the tube band: the corridor removes a constant
    // depth, so its floor keeps the hillside's lateral tilt, and the downhill
    // shoulder can be well below the road while the centreline is already
    // buried. Anything in between must be inside the shell or the driver runs
    // into a grass bank in front of a concrete portal.
    int a2 = bestA, b2 = bestB;
    while (a2 > 0      && (clearance[(size_t)(a2 - 1)] > kOpenTol ||
                           roadBury [(size_t)(a2 - 1)] > 0.30f)) --a2;
    while (b2 < NS - 1 && (clearance[(size_t)(b2 + 1)] > kOpenTol ||
                           roadBury [(size_t)(b2 + 1)] > 0.30f)) ++b2;
    route.boreS0 = kSampleDs * (float)a2;
    route.boreS1 = kSampleDs * (float)b2;
    // How much of the road inside each mouth is still under an earth ramp —
    // the honest residual of the technique. Reported, not hidden.
    float buriedIn = 0.0f;
    // "Under an earth ramp" means the ground is above the road but still inside
    // the drivable envelope — deeper than that and it is honest soil cover.
    for (int i = a2; i <= b2; ++i)
        if (roadBury[(size_t)i] > 0.30f && roadBury[(size_t)i] < kTcTubeCrownH) buriedIn += kSampleDs;
    route.buriedRoadLen = buriedIn;

    char b[512];
    std::snprintf(b, sizeof(b),
        "tunnel corridor: registered 1 corridor, %d nodes, %.0f m, halfWidth %.1f/%.1f | "
        "road %.1f..%.1f m | max cut %.1f m | SHELL s=%.0f..%.0f (%.0f m), of which "
        "fully buried s=%.0f..%.0f (%.0f m); %.0f m of road inside the shell still "
        "carries an earth ramp (the portal-sweep residual)",
        kRouteNodes, route.totalLen, kTcCorridorHalfW, kTcCorridorFall,
        route.st.front().roadY, route.st.back().roadY,
        [&]{ float m = 0; for (auto& n : route.st) m = std::max(m, n.latMax - n.roadY); return m; }(),
        route.boreS0, route.boreS1, route.boreS1 - route.boreS0,
        route.coverS0, route.coverS1, route.coverS1 - route.coverS0, route.buriedRoadLen);
    x3::logInfo(b);
    if (!route.boreValid)
        x3::logWarn("tunnel corridor: no enclosed span found — the demo will be an open cutting only");
    return route;
}

// ---------------------------------------------------------------------------
// TunnelCorridorWorld::build
// ---------------------------------------------------------------------------
bool TunnelCorridorWorld::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& physics, const TunnelRoute& route) {
    if (route.st.size() < 2) return false;
    const float axis[3]  = { route.dirX, 0.0f, route.dirZ };
    const float right[3] = { -route.dirZ, 0.0f, route.dirX };

    // ---- Materials. Procedural, so the demo stands up on a bare checkout. ----
    auto tex = [&](std::vector<uint8_t> px, uint32_t n, bool srgb) {
        x3::rhi::TextureHandle t = device.createTexture(px.data(), n, n, srgb);
        if (t.valid()) m_textures.push_back(t);
        return t;
    };
    auto solid1 = [&](uint8_t r, uint8_t g, uint8_t bb, bool srgb) {
        const uint8_t p[4] = { r, g, bb, 255 };
        x3::rhi::TextureHandle t = device.createTexture(p, 1, 1, srgb);
        if (t.valid()) m_textures.push_back(t);
        return t;
    };
    // BL's material table (§3.1) as VALUES, not code: asphalt ~ (0.07,0.07,0.08)
    // rough 0.92; concrete wall a running-bond grey ~#5e5a56 / #4a4744; ceiling
    // darker and rougher than the wall.
    const x3::rhi::TextureHandle asphaltTex  = tex(x3::prims::makeCheckerRGBA(128, 16, 31, 31, 34, 27, 27, 30), 128, true);
    // Light, low-contrast concrete: a portal headwall is a VERTICAL face, so it
    // only ever catches a grazing fraction of an overhead sun. BL's #5e5a56
    // value read fine under Babylon's ambient-heavy setup; here it renders
    // near-black, so the albedo is raised rather than the scene lighting faked.
    const x3::rhi::TextureHandle concreteTex = tex(x3::prims::makeCheckerRGBA(256, 32, 188, 183, 175, 173, 168, 161), 256, true);
    const x3::rhi::TextureHandle paintTex    = solid1(232, 232, 224, true);
    const x3::rhi::TextureHandle roughMR     = solid1(0, 235, 0, false);   // rough 0.92, metal 0
    const x3::rhi::TextureHandle wallMR      = solid1(0, 205, 0, false);   // rough 0.80, metal 0

    // ---- REAL ART. The two checkers above are the BARE-CHECKOUT FALLBACK and
    // nothing more; when assets/surface_library is present the concrete is
    // dressed from authored PBR sets (albedo + normal + mr) instead.
    //
    //   BORE LINING -> cv_shotcrete_break. A road bore is lined with SHOTCRETE
    //     — sprayed concrete straight onto the excavated face — so this is the
    //     literally-correct material, not a lookalike. It is also the only
    //     concrete-family set in the library that ships a REAL multi-channel mr
    //     map (589 KB of actual roughness variation; sr_concrete_01,
    //     sr_concrete_a, cc_porous_cement and both mw_concrete_panels sets all
    //     ship an 8x8 constant), which matters here because the bore is the one
    //     surface the player is inside for 300 m under six point lights. And its
    //     value sits well under the placeholder's near-white 188/183/175, which
    //     is what was blowing the interior out to a white tube.
    //
    //   PORTAL HEADWALLS -> mw_concrete_panels_a. A headwall is CAST IN
    //     FORMWORK, and this set carries the signature of exactly that: panel
    //     joints and form-tie holes on a ~2.5 m grid. Deliberately a DIFFERENT
    //     set from the bore, because the portal is a different pour from the
    //     lining and reading them as one continuous material is what made the
    //     old checker slab look pasted on.
    //
    //   Neither is sr_concrete_01 — that set is already the terrain splat's ROCK
    //     layer (terrain.cpp makeGroundTexture), so using it here would make the
    //     concrete and the rock cutting the same surface and erase the portal.
    //
    //   NOT WIRED, and honestly: the ROAD. There is no asphalt set in the
    //     library (sr_rubberfloor is studded rubber matting, cc_porous_cement is
    //     a pale cement), so the ribbon keeps its procedural low-contrast dark
    //     checker rather than being dressed in something that is not asphalt.
    m_surf.mount(x3::game::assetRoot() + "/surface_library");
    // BORE LINING SET. X3_BORE_SET overrides for A/B sweeps (dev only).
    const char* boreSetName = [](){ const char* e = std::getenv("X3_BORE_SET");
                                    return (e && *e) ? e : "mw_concrete_panels_b"; }();
    const float boreUvOverride = [](){ const char* e = std::getenv("X3_BORE_UV");
                                       return (e && *e) ? (float)std::atof(e) : 0.0f; }();
    const SurfaceSet& boreSet   = m_surf.get(device, boreSetName);
    const SurfaceSet& portalSet = m_surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& roadSet   = m_surf.get(device, "rd_asphalt_01");
    if (!boreSet.ok || !portalSet.ok)
        x3::logWarn("tunnel corridor: surface_library set(s) unavailable — "
                    "falling back to the procedural checker concrete");

    auto upload = [&](MeshBuf& mb, const Material& mat, bool collide) {
        if (mb.empty()) return;
        fixWinding(mb);
        Entity e;
        e.mesh = device.createMesh(mb.v.data(), (uint32_t)mb.v.size(),
                                   mb.i.data(), (uint32_t)mb.i.size());
        if (e.mesh.valid()) m_meshes.push_back(e.mesh);
        e.tex       = mat.alb;
        e.mrTex     = mat.mr;
        e.normalTex = mat.nrm;
        for (int k = 0; k < 4; ++k) { e.baseColor[k] = mat.tint[k]; e.emissive[k] = mat.emissive[k]; }
        e.tag = (uint32_t)Tag::Static;
        if (collide) {
            std::vector<float> cv; cv.reserve(mb.v.size() * 3);
            for (const auto& v : mb.v) { cv.push_back(v.pos[0]); cv.push_back(v.pos[1]); cv.push_back(v.pos[2]); }
            x3::phys::BodyId bid = physics.addStaticMesh(cv.data(), (uint32_t)mb.v.size(),
                                                         mb.i.data(), (uint32_t)mb.i.size());
            // The body is kept in m_bodies, NOT on the entity: the vertices are
            // already world-baked, and Scene::update() would re-anchor a
            // body-bearing entity's transform onto the body origin.
            if (bid.valid()) m_bodies.push_back(bid);
        }
        scene.add(e);
        ++m_entities;
    };

    // ---- Frames along the whole route (2 m), and the bore sub-range. --------
    auto frameAt = [&](float s) {
        Frame f{}; f.s = s;
        const float sc = -kRouteHalfLen + s;
        f.p[0] = kRouteCX + kRouteDirX * sc;
        f.p[2] = kRouteCZ + kRouteDirZ * sc;
        f.p[1] = route.roadYAt(s);
        return f;
    };
    std::vector<Frame> roadFrames;
    for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 4.0f) roadFrames.push_back(frameAt(s));

    // ================= 1) THE ROAD RIBBON ===================================
    // A swept slab. It is thick (kSlabDrop) on purpose: the corridor's depth is
    // constant across its flat floor, so the floor still carries the hillside's
    // LATERAL slope. The slab's skirt swallows that mismatch and reads as a
    // retaining kerb where the cut is deepest.
    {
        constexpr float kSlabProud = 0.14f;
        MeshBuf mb;
        const float up[3] = { 0.0f, 1.0f, 0.0f };
        auto P = [&](const Frame& f, float r, float u, float out[3]) {
            out[0] = f.p[0] + right[0]*r + up[0]*u;
            out[1] = f.p[1] + right[1]*r + up[1]*u;
            out[2] = f.p[2] + right[2]*r + up[2]*u;
        };
        // The slab's SKIRT is not a constant thickness: the corridor removes a
        // constant DEPTH, so its floor still carries the hillside's lateral
        // slope and the trench bottom on the downhill shoulder can sit several
        // metres below the road datum. Reach the skirt down to the actual ground
        // at each edge and the ribbon reads as an embankment instead of a slab
        // hovering over a gap.
        auto edgeBottom = [&](const Frame& f, float latOff) {
            const float ex = f.p[0] + right[0]*latOff, ez = f.p[2] + right[2]*latOff;
            const float g = terrainHeightAtWorld(ex, ez);
            return clampf(std::min(f.p[1] - 1.2f, g - 0.6f), f.p[1] - 16.0f, f.p[1] - 1.2f);
        };
        auto PA = [&](const Frame& f, float r, float absY, float out[3]) {
            out[0] = f.p[0] + right[0]*r;
            out[1] = absY;
            out[2] = f.p[2] + right[2]*r;
        };
        const float hw = kTcRoadHalfWidth;
        for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
            const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
            float aL[3], aR[3], bL[3], bR[3], aLd[3], aRd[3], bLd[3], bRd[3];
            P(a, -hw, kSlabProud, aL); P(a, hw, kSlabProud, aR);
            P(b, -hw, kSlabProud, bL); P(b, hw, kSlabProud, bR);
            PA(a, -hw, edgeBottom(a, -hw), aLd); PA(a, hw, edgeBottom(a, hw), aRd);
            PA(b, -hw, edgeBottom(b, -hw), bLd); PA(b, hw, edgeBottom(b, hw), bRd);
            const float nU[3] = { 0, 1, 0 };
            mb.quad(aL, aR, bR, bL, nU, 0.0f, 1.0f, a.s * 0.08f, b.s * 0.08f);
            const float nR[3] = {  right[0], 0.0f,  right[2] };
            const float nL[3] = { -right[0], 0.0f, -right[2] };
            mb.quad(aR, aRd, bRd, bR, nR, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            mb.quad(aL, aLd, bLd, bL, nL, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            const float nD[3] = { 0, -1, 0 };
            mb.quad(aLd, aRd, bRd, bLd, nD, 0.0f, 1.0f, a.s * 0.08f, b.s * 0.08f);
        }
        // REAL ASPHALT. The note above ("NOT WIRED, and honestly: the ROAD") is
        // now resolved: rd_asphalt_01 is a 2K albedo + tangent normal + MR set
        // built from a purchased pack, with the source's Unity HDRP maskmap
        // transposed into glTF channel order (roughness = 1 - smoothness, and
        // metallic carried through rather than assumed). Falls back to the old
        // procedural checker if the set is absent, so a thin checkout still runs.
        Material m;
        if (roadSet.ok) { m.alb = roadSet.albedo; m.mr = roadSet.mr; m.nrm = roadSet.normal; }
        else            { m.alb = asphaltTex;     m.mr = roughMR; }
        upload(mb, m, /*collide*/true);

        // ---- Lane markings. BL §3.4 as DATA: solid white edge lines at
        // +/-(w/2 - 0.5) every segment, dashed centre on a 5 m grid (dash 60 %).
        MeshBuf paint;
        for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
            const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
            const float nU[3] = { 0, 1, 0 };
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const float e0 = (float)sgn * (hw - 0.5f) - 0.09f;
                const float e1 = (float)sgn * (hw - 0.5f) + 0.09f;
                float p0[3], p1[3], p2[3], p3[3];
                P(a, e0, kSlabProud + 0.02f, p0); P(a, e1, kSlabProud + 0.02f, p1);
                P(b, e1, kSlabProud + 0.02f, p2); P(b, e0, kSlabProud + 0.02f, p3);
                paint.quad(p0, p1, p2, p3, nU, 0, 1, 0, 1);
            }
            const int cell = (int)(a.s / 5.0f);
            if ((cell & 1) == 0) {
                float p0[3], p1[3], p2[3], p3[3];
                P(a, -0.09f, kSlabProud + 0.02f, p0); P(a, 0.09f, kSlabProud + 0.02f, p1);
                P(b,  0.09f, kSlabProud + 0.02f, p2); P(b, -0.09f, kSlabProud + 0.02f, p3);
                const float nU2[3] = { 0, 1, 0 };
                paint.quad(p0, p1, p2, p3, nU2, 0, 1, 0, 1);
            }
        }
        Material pm; pm.alb = paintTex; pm.mr = roughMR;
        pm.emissive[0] = pm.emissive[1] = pm.emissive[2] = 0.9f; pm.emissive[3] = 0.05f;
        upload(paint, pm, /*collide*/false);
    }

    // ================= 2) THE TUNNEL SHELL ==================================
    if (route.boreValid) {
        const Profile inner = makeProfile(kTcTubeHalfWidth, kTcTubeWallH, kTcTubeCrownH);
        const Profile outer = offsetProfile(inner, kTcShellThick);
        // The tube runs the full enclosed span, plus a short canopy at each end
        // so the concrete (not the raw terrain step) is what you drive into.
        constexpr float kCanopy = 1.5f;
        const float s0 = std::max(0.0f, route.boreS0 - kCanopy);
        const float s1 = std::min(route.totalLen, route.boreS1 + kCanopy);
        std::vector<Frame> bf;
        for (float s = s0; s <= s1 + 0.01f; s += 3.0f) bf.push_back(frameAt(s));
        if (bf.back().s < s1 - 0.01f) bf.push_back(frameAt(s1));

        MeshBuf shell;
        // TEXEL DENSITY. 0.14 (one tile per 7.1 m) was set for the procedural
        // checker, where tile size is arbitrary. On a real 2K shotcrete albedo it
        // is wrong by about 2x: the set's largest crack spans half the tile, so at
        // 7.1 m it draws a 3.5 m fissure and the bore reads as a collapsed cavern
        // rather than a lined tunnel. 0.30 (one tile per 3.3 m) puts that same
        // crack at ~1.6 m — shotcrete crazing at the scale a driver would see it.
        const float kBoreUV = boreUvOverride > 0.0f ? boreUvOverride
                                                    : (boreSet.ok ? 0.16f : 0.14f);
        emitSweep(shell, bf, right, inner, -1.0f, kBoreUV);   // drivable bore surface
        emitSweep(shell, bf, right, outer, +1.0f, kBoreUV);   // buried outer skin
        Material sm;
        if (boreSet.ok) { sm.alb = boreSet.albedo; sm.mr = boreSet.mr; sm.nrm = boreSet.normal; }
        else            { sm.alb = concreteTex;    sm.mr = wallMR; }
        upload(shell, sm, /*collide*/true);

        // ---- Portal headwalls. The corridor's depth step leaves a real earth
        // face at each mouth; this is what caps it.
        {
            MeshBuf portals;
            const float rectHalfW = kTcCorridorHalfW + 4.0f;
            // Deep enough that the headwall's bottom edge is always buried, even
            // on the downhill shoulder where the trench floor falls away.
            const float rectBot   = -6.0f;
            for (int end = 0; end < 2; ++end) {
                const float sEnd = (end == 0) ? s0 : s1;
                const float sBack = (end == 0) ? s0 + 1.6f : s1 - 1.6f;
                // Size the headwall to the GROUND it is capping. A fixed height
                // either leaves the earth face showing (too short) or stands the
                // slab proud of the hillside like a billboard (too tall) — the
                // two ends of this route sit on very different gradients.
                float rectTop = kTcTubeCrownH + kTcShellThick + 0.8f;
                {
                    const Frame fe = frameAt(sEnd);
                    float hMin = 1e9f;
                    for (int k = -2; k <= 2; ++k) {
                        const float off = (float)k * (kTcTubeHalfWidth + kTcShellThick) / 2.0f;
                        hMin = std::min(hMin, terrainHeightAtWorld(fe.p[0] + right[0]*off,
                                                                   fe.p[2] + right[2]*off));
                    }
                    rectTop = clampf(hMin - fe.p[1] + 1.4f,
                                     kTcTubeCrownH + kTcShellThick + 0.8f,
                                     kTcTubeCrownH + kTcShellThick + 3.4f);
                }
                const Frame fa = frameAt(sEnd), fb = frameAt(sBack);
                const float outN[3] = { (end == 0 ? -axis[0] : axis[0]), 0.0f,
                                        (end == 0 ? -axis[2] : axis[2]) };
                const float inN[3]  = { -outN[0], 0.0f, -outN[2] };
                emitPortalFace(portals, fa.p, right, outN, inner, rectHalfW, rectTop, rectBot);
                emitPortalFace(portals, fb.p, right, inN,  inner, rectHalfW, rectTop, rectBot);
                // Rim strip so the slab has thickness from a grazing angle.
                const float up[3] = { 0, 1, 0 };
                auto W = [&](const Frame& f, float sx, float sy, float out[3]) {
                    out[0] = f.p[0] + right[0]*sx + up[0]*sy;
                    out[1] = f.p[1] + right[1]*sx + up[1]*sy;
                    out[2] = f.p[2] + right[2]*sx + up[2]*sy;
                };
                const float corner[4][2] = { { -rectHalfW, rectBot }, { rectHalfW, rectBot },
                                             { rectHalfW, rectTop }, { -rectHalfW, rectTop } };
                for (int k = 0; k < 4; ++k) {
                    const int k2 = (k + 1) & 3;
                    float a[3], b[3], c[3], d[3];
                    W(fa, corner[k][0],  corner[k][1],  a);
                    W(fa, corner[k2][0], corner[k2][1], b);
                    W(fb, corner[k2][0], corner[k2][1], c);
                    W(fb, corner[k][0],  corner[k][1],  d);
                    float mx = (corner[k][0] + corner[k2][0]) * 0.5f;
                    float my = (corner[k][1] + corner[k2][1]) * 0.5f;
                    const float ml = std::sqrt(mx*mx + my*my); if (ml > 1e-4f) { mx /= ml; my /= ml; }
                    const float n[3] = { right[0]*mx, my, right[2]*mx };
                    portals.quad(a, b, c, d, n, 0, 1, 0, 1);
                }
            }
            Material pm;
            if (portalSet.ok) {
                pm.alb = portalSet.albedo; pm.mr = portalSet.mr; pm.nrm = portalSet.normal;
                // valueTint() is the library's own reflectance-band correction
                // (surface_library.h): a set already in band returns exactly 1.0,
                // so a correctly-authored headwall is untouched.
                const float v = portalSet.valueTint();
                pm.tint[0] = pm.tint[1] = pm.tint[2] = v;
            } else {
                pm.alb = concreteTex; pm.mr = wallMR;
                pm.tint[0] = 0.86f; pm.tint[1] = 0.86f; pm.tint[2] = 0.84f;
            }
            upload(portals, pm, /*collide*/true);
        }

        // ---- DRESSING, deliberately thin (BL_WORLD_PORT.md §3.3 + §4.4).
        // BL runs a fluorescent strip every 15 m and hangs a real PointLight on
        // every 3rd one. On a 250 m bore that is ~5 lights here but ~23 on BL's
        // 350 m South tunnel, and all four BL tunnels together want 48 of the
        // engine's 64 forward point lights (§4.4). So: keep BL's 15 m STRIP
        // interval as EMISSIVE GEOMETRY (free — no light-budget cost at all) and
        // spend only kTcMaxBoreLights (6) real point lights, spread evenly.
        {
            MeshBuf strips;
            const float up[3] = { 0, 1, 0 };
            uint32_t nStrip = 0;
            for (float s = s0 + 7.5f; s <= s1 - 7.5f; s += 15.0f, ++nStrip) {
                const Frame f = frameAt(s);
                const float c[3] = { f.p[0] + up[0]*(kTcTubeCrownH - 0.35f),
                                     f.p[1] + up[1]*(kTcTubeCrownH - 0.35f),
                                     f.p[2] + up[2]*(kTcTubeCrownH - 0.35f) };
                obox(strips, c, right, axis, kTcTubeHalfWidth * 0.30f, 0.06f, 0.22f, 0.5f);
            }
            Material em; em.alb = paintTex; em.mr = wallMR;
            em.emissive[0] = 1.0f; em.emissive[1] = 0.94f; em.emissive[2] = 0.76f; em.emissive[3] = 3.2f;
            upload(strips, em, /*collide*/false);

            const float span = s1 - s0;
            for (uint32_t k = 0; k < kTcMaxBoreLights; ++k) {
                const float s = s0 + span * ((float)k + 0.5f) / (float)kTcMaxBoreLights;
                const Frame f = frameAt(s);
                x3::rhi::PointLight pl{};
                pl.pos[0] = f.p[0]; pl.pos[1] = f.p[1] + kTcTubeCrownH - 1.1f; pl.pos[2] = f.p[2];
                pl.range = 34.0f;
                pl.color[0] = 2.6f; pl.color[1] = 2.35f; pl.color[2] = 1.75f;
                m_lights.push_back(pl);
            }
            char b[256];
            std::snprintf(b, sizeof(b),
                "tunnel corridor: bore %.0f m | %u emissive strips @15 m | %u REAL point lights "
                "(BL's every-3rd-strip ratio would want %u — see BL_WORLD_PORT.md 4.4)",
                span, nStrip, (unsigned)m_lights.size(), (unsigned)(nStrip / 3 + 1));
            x3::logInfo(b);
        }

        // ---- HONEST SELF-CHECK: does the ground actually stay off the shell?
        // The corridor's depth profile is authored at 32 nodes and interpolated;
        // the natural field between them is not linear, so soil cover can pinch.
        // Walk the buried reach at 0.5 m against the OUTER arch and report the
        // worst clearance — a negative number means terrain is inside the tube
        // and the demo is lying.
        {
            const float aOut = kTcTubeHalfWidth + kTcShellThick;
            const float bOut = (kTcTubeCrownH - kTcTubeWallH) + kTcShellThick;
            float worst = 1e9f, worstS = 0.0f;
            for (float s = route.coverS0; s <= route.coverS1 + 0.01f; s += 0.5f) {
                const Frame f = frameAt(s);
                for (int k = 0; k <= 10; ++k) {
                    const float lx = -aOut + 2.0f * aOut * (float)k / 10.0f;
                    const float r2 = 1.0f - (lx / aOut) * (lx / aOut);
                    const float top = f.p[1] + kTcTubeWallH + bOut * std::sqrt(r2 < 0.0f ? 0.0f : r2);
                    const float g = terrainHeightAtWorld(f.p[0] + right[0]*lx, f.p[2] + right[2]*lx);
                    if (g - top < worst) { worst = g - top; worstS = s; }
                }
            }
            char cb[256];
            std::snprintf(cb, sizeof(cb),
                "tunnel corridor: worst soil cover over the shell = %.2f m at s=%.0f "
                "(negative == terrain inside the bore)", worst, worstS);
            if (worst < 0.0f) x3::logWarn(cb); else x3::logInfo(cb);
        }
    }

    physics.optimizeBroadphase();
    return true;
}

void TunnelCorridorWorld::shutdown(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    for (auto b : m_bodies) physics.removeBody(b);
    m_bodies.clear();
    for (auto m : m_meshes) device.destroyMesh(m);
    m_meshes.clear();
    for (auto t : m_textures) device.destroyTexture(t);
    m_textures.clear();
    m_surf.destroyAll(device);   // the library owns the bore/portal set textures
    m_lights.clear();
}

void TunnelCorridorWorld::showcaseCamera(const TunnelRoute& route, int which, float cam[5]) const {
    const float yawFwd  = std::atan2(route.dirZ, route.dirX);
    const float yawBack = yawFwd + 3.14159265f;
    // The road GRADES (up to kTcMaxGrade). A level camera inside a climbing bore
    // sees the floor rise into the crown and the tunnel looks blocked, so every
    // eye-level pose is pitched along the local road slope.
    auto gradeAt = [&](float s) {
        const float a = clampf(s - 25.0f, 0.0f, route.totalLen);
        const float b = clampf(s + 25.0f, 0.0f, route.totalLen);
        if (b - a < 1.0f) return 0.0f;
        return std::atan2(route.roadYAt(b) - route.roadYAt(a), b - a);
    };

    if (which == 3) {
        // High and back, looking down the whole corridor — the shot that shows
        // the SADDLE the technique leaves in the ridge. Judge it honestly.
        const float mid = (route.coverS0 + route.coverS1) * 0.5f;
        float p[3]; route.posAt(std::max(0.0f, mid - 300.0f), p);
        cam[0] = p[0]; cam[1] = p[1] + 190.0f; cam[2] = p[2];
        cam[3] = yawFwd; cam[4] = -0.46f;
        return;
    }
    if (which == 4) {
        // Three-quarter close-up of the entrance mouth: the junction between the
        // concrete portal and the raw terrain is the seam this whole technique
        // lives or dies on, so it gets its own frame.
        const float sEye = std::max(4.0f, route.boreS0 - 26.0f);
        float e[3]; route.posAt(sEye, e);
        const float lat = 15.0f;
        e[0] += -route.dirZ * lat; e[2] += route.dirX * lat; e[1] += 5.0f;
        float t[3]; route.posAt(route.boreS0 + 2.0f, t);
        t[1] += 4.0f;
        const float dx = t[0]-e[0], dy = t[1]-e[1], dz = t[2]-e[2];
        const float hl = std::sqrt(dx*dx + dz*dz);
        cam[0] = e[0]; cam[1] = e[1]; cam[2] = e[2];
        cam[3] = std::atan2(dz, dx); cam[4] = std::atan2(dy, std::max(0.01f, hl));
        return;
    }

    float s = 0.0f; const float eyeUp = 2.4f; float yaw = yawFwd, pitch = 0.0f;
    switch (which) {
        case 0: s = std::max(6.0f, route.boreS0 - 85.0f);  yaw = yawFwd;  pitch =  gradeAt(s); break;
        case 1: s = (route.coverS0 + route.coverS1) * 0.5f; yaw = yawFwd; pitch = gradeAt(s); break;
        case 2: s = std::min(route.totalLen - 6.0f, route.boreS1 + 60.0f); yaw = yawBack; pitch = -gradeAt(s); break;
        default: break;
    }
    float p[3]; route.posAt(clampf(s, 0.0f, route.totalLen), p);
    cam[0] = p[0]; cam[1] = p[1] + eyeUp; cam[2] = p[2];
    cam[3] = yaw;  cam[4] = pitch;
}

} // namespace x3::game
