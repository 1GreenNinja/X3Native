// tunnel_corridor — see tunnel_corridor.h for the design + the BL provenance.
// Clean-room, original work. No BL / third-party engine source was consulted or
// transcribed; the technique is documented in docs/design/BL_WORLD_PORT.md
// §2.2/§2.3/§3.2/§3.3/§4.3/§4.4 and re-implemented here from first principles.
#include "tunnel_corridor.h"
#include "tunnel_fitout.h"
#include "tunnel_rooms.h"
#include "lns_shop.h"     // LATE NIGHT SPEED shop kit (authored in club1127,
                          // shared): CMU walls, checker floor, neon letters

#include <functional>
#include "mesh_prims.h"
#include "asset_root.h"        // assetRoot() — the surface_library mount point
#include "vehicle.h"           // DriveDemo — the drive-through self-test rig
#include "headless_device.h"   // HeadlessRenderDevice — self-test, no Vulkan
#include <cstdlib>
#include <memory>

#include "engine/core/x3_log.h"

#include <algorithm>
#include <deque>   // routeStore: stable addresses across growth
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

// Hermite smoothstep, 0 at a, 1 at b, clamped. Same shape the corridor
// primitive uses for its shoulder, so the lid's blend and the terrain's blend
// are the same curve and the seam has no kink in the derivative either.
float sstep(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The shell's OUTER skin: height above the road datum at lateral offset `lat`.
// Zero outside the shell's footprint. This is the surface the backfill lid has
// to clear and the surface the portal's spandrel springs from, so both ask this
// one function rather than each re-deriving the ellipse.
float shellOuterTopAt(float lat) {
    const float a = kTcTubeHalfWidth + kTcShellThick;
    const float b = (kTcTubeCrownH - kTcTubeWallH) + kTcShellThick;
    const float al = std::fabs(lat);
    if (al >= a) return 0.0f;
    const float q = 1.0f - (al / a) * (al / a);
    return kTcTubeWallH + b * std::sqrt(q < 0.0f ? 0.0f : q);
}

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
// (An emissive-MAP field was tried here for the garage's neon and REMOVED: the
// lone emissiveTex entity rendered in `--screenshot` runs and vanished in
// `--screenshot-tunnel` runs — same world, same camera by pixel-diff. The sign
// is channel-letter GEOMETRY on the flat-emissive path instead; if a per-texel
// emissive is ever needed in this host, start from that repro.)

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

// P1 — THE FRAME FOLLOWS THE POLYLINE.
//
// These three were `origin + dir * s` against ONE route heading, which is what
// made every corridor a straight run. The stations always carried their own
// x/z/s, so the polyline was in the data the whole time; the frame just did not
// read it. (The CARVE never had this limitation — terrain.h calls
// TerrainCorridor "the polyline generalization of the river carve" and unions a
// capsule per segment, crease-free, with --test-terraincorridor C4 proving the
// joints. Only this layer was straight.)
//
// STRAIGHT ROUTES MUST COME OUT BIT-IDENTICAL. Every station of a straight route
// lies on `origin + dir * s` by construction, so interpolating between two of
// them along arc length returns the same point, and the segment tangent is dir.
// That is not a hope — --test-tunnelmouth (7/7) and --test-tunneldrive both run
// the straight demo route and would move if it were not so.

// Locate s in the station polyline. Returns the index of the segment END node
// (>= 1) and the 0..1 parameter along it. Extrapolates on the end segments so
// s outside [0, totalLen] runs straight out along that end's heading — the
// approach cuttings and the portal sweeps both rely on that.
void TunnelRoute::segmentAt(float s, uint32_t& outI, float& outT) const {
    const size_t n = st.size();
    if (n < 2) { outI = 1; outT = 0.0f; return; }
    if (s <= st[1].s) {                       // before/inside the first segment
        const float d = std::max(1e-4f, st[1].s - st[0].s);
        outI = 1; outT = (s - st[0].s) / d; return;
    }
    for (size_t i = 2; i < n; ++i) {
        if (s <= st[i].s) {
            const float d = std::max(1e-4f, st[i].s - st[i-1].s);
            outI = (uint32_t)i; outT = (s - st[i-1].s) / d; return;
        }
    }
    const float d = std::max(1e-4f, st[n-1].s - st[n-2].s);   // past the end
    outI = (uint32_t)(n - 1); outT = (s - st[n-2].s) / d;
}

void TunnelRoute::tangentAt(float s, float& outTx, float& outTz) const {
    if (st.size() < 2) { outTx = dirX; outTz = dirZ; return; }
    uint32_t i; float t;
    segmentAt(s, i, t);
    float tx = st[i].x - st[i-1].x, tz = st[i].z - st[i-1].z;
    const float len = std::sqrt(tx*tx + tz*tz);
    if (len < 1e-5f) { outTx = dirX; outTz = dirZ; return; }   // degenerate node pair
    outTx = tx / len; outTz = tz / len;
}

void TunnelRoute::posAt(float s, float out[3]) const {
    if (st.size() < 2) { out[0] = ox + dirX * s; out[2] = oz + dirZ * s; out[1] = roadYAt(s); return; }
    uint32_t i; float t;
    segmentAt(s, i, t);
    out[0] = st[i-1].x + (st[i].x - st[i-1].x) * t;
    out[2] = st[i-1].z + (st[i].z - st[i-1].z) * t;
    out[1] = roadYAt(s);
}

void TunnelRoute::worldAt(float s, float lat, float& outX, float& outZ) const {
    if (st.size() < 2) {
        outX = ox + dirX * s + (-dirZ) * lat;
        outZ = oz + dirZ * s + ( dirX) * lat;
        return;
    }
    float p[3]; posAt(s, p);
    float tx, tz; tangentAt(s, tx, tz);
    // Right of travel is (-tz, +tx) in this convention — the same sign the
    // straight form used with dir, so lateral offsets keep their meaning.
    outX = p[0] + (-tz) * lat;
    outZ = p[2] + ( tx) * lat;
}

// ---------------------------------------------------------------------------
// The PRE-corridor surface. terrainHeightAt() applies the registered corridors
// LAST (`h + delta`, delta <= 0), so subtracting the delta recovers the natural
// hillside exactly — and it does so identically before and after registration,
// which is what lets the derivation below run either side of the boot step
// without carrying a second copy of the height field.
// ---------------------------------------------------------------------------
float tunnelNaturalHeightAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

// ---------------------------------------------------------------------------
// THE BACKFILL LID surface.
//
// Over the ridge this is the natural hillside, untouched — the cut is filled
// back to the profile it had before anyone dug. Over a cut-and-cover EXTENSION
// (where the natural bank is lower than the tube) it is a battered mound over
// the tube, which is what backfill over a false tunnel actually looks like.
// Laterally the raise is blended out over the corridor's own shoulder curve, so
// at halfWidth + falloff — where terrainCorridorDelta() is EXACTLY 0 — the lid
// equals the untouched terrain to the last bit. That is why the outer seam is a
// property, not a tuning. Past the seam the lid runs on as an apron sunk by
// kTcLidSink so the streamed tiles (1 m at LOD0, 4 m at Quarter) always win the
// depth test instead of z-fighting a coincident surface.
// ---------------------------------------------------------------------------
float tunnelLidHeightAt(const TunnelRoute& route, float s, float lat) {
    float x = 0.0f, z = 0.0f;
    route.worldAt(s, lat, x, z);
    const float nat = tunnelNaturalHeightAt(x, z);
    const float al  = std::fabs(lat);
    const float W0  = kTcCorridorHalfW;
    const float W1  = kTcCorridorHalfW + kTcCorridorFall;
    if (al >= W1) return nat - kTcLidSink * sstep(W1, W1 + 1.2f, al);

    const float ry       = route.roadYAt(s);
    const float crownTot = kTcTubeCrownH + kTcShellThick + kTcLidCover;
    // Battered mound: flat over the tube, dying to nothing at the seam.
    float mound = crownTot * (1.0f - sstep(kTcTubeHalfWidth * 0.45f, W1, al));
    const float shellTop = shellOuterTopAt(al);
    if (shellTop > 0.0f) mound = std::max(mound, shellTop + kTcLidCover);

    float y = std::max(nat, ry + mound);
    y = nat + (y - nat) * (1.0f - sstep(W0, W1, al));

    // PORTAL END TAPER. Approaching a mouth, outside the headwall's width, ease
    // the backfill down onto the cut face. Inside the headwall's width nothing
    // changes (the shell still has its full cover); at the zero-delta seam
    // nothing changes either, because there the cut face IS the natural surface.
    // It only bites on the flanks, which is exactly where the wingwall would
    // otherwise have to retain full-height ground and would run out across the
    // hillside as a free-standing fin.
    if (route.boreValid && al > kTcPortalHalfW) {
        const float dEnd = std::min(s - route.boreS0, route.boreS1 - s);
        if (dEnd < kTcPortalTaper) {
            const float f = (1.0f - sstep(0.0f, kTcPortalTaper, std::max(0.0f, dEnd)))
                          * sstep(kTcPortalHalfW, kTcPortalHalfW + kTcPortalSplay, al);
            y += (terrainHeightAtWorld(x, z) - y) * f;
        }
    }
    return y;
}

// ---------------------------------------------------------------------------
// THE DERIVATION — sample, grade, CUT TO ROAD LEVEL, then prove it.
//
// PURE with respect to the corridor registry: it builds its own stack
// TerrainCorridor and evaluates it through terrainCorridorDepthAt(), so a whole
// route can be derived for a hillside that is not (and never will be)
// registered. --test-tunnelmouth M6 uses exactly that to re-derive the
// construction on three OTHER hillsides and assert the mouth is still clean.
// The fix has to be a property of the method, not of this one dome at
// (-592, -352), because the height field gets regenerated.
// ---------------------------------------------------------------------------
namespace {

struct RouteSeed {
    float cx = kRouteCX, cz = kRouteCZ;
    float dirX = kRouteDirX, dirZ = kRouteDirZ;
    float halfLen = kRouteHalfLen;
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// How far BELOW the road datum the trench floor is driven under the roadway.
// Not cosmetic: the road slab sits kSlabProud above the datum and its skirt
// hangs below it, so a floor left exactly AT the datum z-fights the slab for
// the whole 640 m. A fifth of a metre buys the whole route out of it.
constexpr float kFloorClear = 0.22f;

// CAP THE PLUG'S CARVE — the torn-mountain guard, referenced by the
// drive-through gate's BOUNDED CARVE check. Node depths are authored as
// (natural surface at the probe) - road, i.e. "cut the ground down to road
// level". Under the rock-relief massif that difference reaches ~100 m, which
// would gouge the ridge open; 24 m is deep enough to clear the portal and
// shallow enough to leave a mountain standing over it. Unclamped, B2 measures
// 44.6 m of carve against an 18.2 m need.
constexpr float kTcPlugMaxCut = 24.0f;

void deriveRoute(const RouteSeed& seed, TunnelRoute& route, TerrainCorridor& c) {
    route = TunnelRoute{};
    route.dirX = seed.dirX; route.dirZ = seed.dirZ;
    // The route's own IDENTITY, carried per route — declared in the header for
    // exactly this ("carrying them per route is what lets several coexist") and
    // then never actually written, so every route reported centre (0,0)
    // halfLen 0. Diagnostics that read them were silently reading defaults.
    route.cx = seed.cx; route.cz = seed.cz;
    route.halfLen = seed.halfLen;
    route.ox = seed.cx - seed.dirX * seed.halfLen;
    route.oz = seed.cz - seed.dirZ * seed.halfLen;
    route.totalLen = 2.0f * seed.halfLen;
    const float ds = route.totalLen / (float)(kRouteNodes - 1);

    // --- 1) Sample the NATURAL field over the corridor's FLOOR FOOTPRINT.
    // The sampling box is lateral AND longitudinal. The depth profile is
    // authored at 32 nodes and interpolated linearly along each segment, so a
    // node that only looked at its own cross-section would miss a bulge sitting
    // BETWEEN two nodes and leave that bulge standing in the road. Half a node
    // spacing of longitudinal reach makes the linear interpolation an upper
    // bound in practice; step 4 then stops arguing and proves it.
    route.st.resize(kRouteNodes);

    // PASS 1 — LAY THE SPINE. Every station's (s, x, z) is written BEFORE
    // anything reads the frame.
    //
    // This used to be one loop that called route.worldAt() to place each station
    // while the station array was still being filled. That was harmless while
    // worldAt was `origin + dir * s` and ignored the array — and it became a
    // circular read the moment the frame started following the polyline
    // (P1): resize() zero-initialises the stations, so worldAt was interpolating
    // between zeroes and the whole route landed at the origin. Both tunnel gates
    // caught it immediately.
    //
    // Splitting the passes is also what makes CURVED routes authorable: pass 1
    // is now the only place a spine is defined, so a future curved seed writes
    // x/z here and everything downstream follows without further change.
    for (int i = 0; i < kRouteNodes; ++i) {
        TunnelStation& n = route.st[i];
        n.s = ds * (float)i;
        // The seed spine is straight: centre-relative, along the seed heading.
        n.x = route.ox + route.dirX * n.s;
        n.z = route.oz + route.dirZ * n.s;
    }

    // PASS 2 — SAMPLE THE GROUND against the finished spine.
    for (int i = 0; i < kRouteNodes; ++i) {
        TunnelStation& n = route.st[i];
        n.ground = tunnelNaturalHeightAt(n.x, n.z);
        n.latMin = n.latMax = n.ground;
        // SAMPLE THE GROUND AT THE RESOLUTION THE INVARIANT IS CHECKED AT.
        //
        // This was a 7x3 grid spanning +-0.55*ds. With kRouteNodes pinned to
        // TerrainCorridor::kMaxNodes (32) over a 640 m route, ds is 20.6 m — so
        // the carve looked at the natural surface every ~11 m while M1 checks
        // every 0.5 m across the full roadway. On a smooth hummock that is
        // harmless. On the 5-range rocky massif, whose relief runs at 0.061
        // frequency (~16 m wavelength), peaks simply fall between samples: the
        // depth is then derived from a max that is not the max, and rock stands
        // on the road. That is the M1/M2/M6 failure, and it is why merely
        // DENSIFYING the old span (13x5 over the same +-0.56*ds) moved the
        // residual by 0.012 m — it was still 20x coarser than the probe.
        //
        // Two things are required, not one:
        //   (a) resolution — step ~1 m longitudinally and ~1.2 m laterally, at
        //       least as fine as the crag wavelength, over the full corridor
        //       width (the road is +-6 m, the corridor +-8.8 m);
        //   (b) OVERLAP — depth is INTERPOLATED between adjacent nodes, so a
        //       peak is only safe if BOTH bracketing nodes are deep enough for
        //       it. Each node therefore scans +-ds (its own span AND its
        //       neighbours'), which makes any linear blend of two adequate
        //       depths itself adequate.
        //
        // Boot-time only: 32 nodes x ~41 longitudinal x 15 lateral height
        // queries, once, against a pure function.
        {
            const float kLatReach  = kTcCorridorHalfW + 0.8f;
            // MATCH THE PROBE EXACTLY. M1 walks the roadway every 0.5 m
            // longitudinally and every kTcRoadHalfWidth/8 = 0.75 m laterally, so
            // anything coarser can miss a crag it will then measure. At ~1.2 m
            // lateral / ~1 m longitudinal the residual fell 2.0 ft -> 0.6 ft but
            // did not close; at the probe's own resolution there is no gap left
            // for a peak to hide in.
            const int   kLatSteps  = 13;                   // +-13 -> 27 samples, 0.74 m apart
            const int   kLongSteps = 41;                   // +-41 -> 83 samples, 0.50 m apart
            for (int k = -kLatSteps; k <= kLatSteps; ++k) {
                const float off = (float)k * kLatReach / (float)kLatSteps;
                for (int m = -kLongSteps; m <= kLongSteps; ++m) {
                    const float sl = clampf(n.s + (float)m * ds / (float)kLongSteps,
                                            0.0f, route.totalLen);
                    float qx = 0.0f, qz = 0.0f;
                    route.worldAt(sl, off, qx, qz);
                    const float h = tunnelNaturalHeightAt(qx, qz);
                    n.latMin = std::min(n.latMin, h);
                    n.latMax = std::max(n.latMax, h);
                }
            }
        }
    }

    // --- 2) Grade the road. UNCHANGED from the original build and still the
    // right shape: start on the surface (a shallow groove), then a LOWERING-ONLY
    // grade limit — a station may not sit higher than its neighbour plus
    // kTcMaxGrade*ds — swept both ways to a fixed point. That yields the highest
    // profile respecting both the terrain and the grade, which is exactly what a
    // road cutting is. (EchoRoads' raise-only relaxation, inverted, because here
    // the ground comes DOWN to the road.)
    for (int i = 0; i < kRouteNodes; ++i)
        route.st[i].roadY = route.st[i].latMax - kTcMinCut;
    const float rise = kTcMaxGrade * ds;
    for (int pass = 0; pass < 64; ++pass) {
        for (int i = 1; i < kRouteNodes; ++i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i-1].roadY + rise);
        for (int i = kRouteNodes - 2; i >= 0; --i)
            route.st[i].roadY = std::min(route.st[i].roadY, route.st[i+1].roadY + rise);
    }
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

    // --- 3) CUT TO ROAD LEVEL, EVERYWHERE. THIS IS THE FIX.
    //
    // The old build had two regimes and a step between them: cut to the road on
    // the approaches, cut only to (road + tube + cover) under the ridge so the
    // TERRAIN was the roof. That step is what buried the first ~9 m of road
    // inside each mouth, and no amount of sharpening removes it (see the proof
    // at the top of tunnel_corridor.h). One regime, cut to road level all the
    // way through the hill, and the ground can never stand on the roadway again
    // — on this route, or on any route, or after a terrain regeneration.
    // The hill goes back on top as a mesh (the backfill lid), the way a real
    // cut-and-cover tunnel is built.
    for (int i = 0; i < kRouteNodes; ++i) {
        TunnelStation& n = route.st[i];
        n.depth = std::max(0.0f, n.latMax - n.roadY + kFloorClear);
        n.bore  = false;                      // there is no bored regime any more
    }

    c = TerrainCorridor{};
    c.nodeCount = kRouteNodes;
    c.halfWidth = kTcCorridorHalfW;
    c.falloff   = kTcCorridorFall;
    for (int i = 0; i < kRouteNodes; ++i) {
        c.x[i] = route.st[i].x; c.z[i] = route.st[i].z; c.depth[i] = route.st[i].depth;
    }

    // --- 4) CLOSE THE LOOP — sampling boxes and linear interpolation are an
    // argument, this is a proof. Walk the FULL road width at 1 m against the
    // corridor just built and push up any node that still lets ground stand on
    // the roadway. Monotone (depths only rise), so it converges — in practice in
    // one or two passes — and it is deterministic, which is what makes the
    // result survive a regenerated height field with no hand-placed fudge.
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<float> add((size_t)kRouteNodes, 0.0f);
        float worst = -1e9f;
        for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 1.0f) {
            const float floorWant = route.roadYAt(s) - kFloorClear;
            for (int k = -4; k <= 4; ++k) {
                const float lat = (float)k * (kTcRoadHalfWidth + 0.9f) / 4.0f;
                float qx = 0.0f, qz = 0.0f;
                route.worldAt(s, lat, qx, qz);
                const float h = tunnelNaturalHeightAt(qx, qz) - terrainCorridorDepthAt(c, qx, qz);
                const float over = h - floorWant;
                if (over > worst) worst = over;
                if (over <= 0.0f) continue;
                const int i0 = clampi((int)std::floor(s / ds), 0, kRouteNodes - 1);
                const int i1 = std::min(i0 + 1, kRouteNodes - 1);
                add[(size_t)i0] = std::max(add[(size_t)i0], over);
                add[(size_t)i1] = std::max(add[(size_t)i1], over);
            }
        }
        if (worst <= 0.0f) break;
        for (int i = 0; i < kRouteNodes; ++i) {
            route.st[i].depth += add[(size_t)i];
            c.depth[i] = route.st[i].depth;
        }
    }

    // --- 5) THE ROOFED SPAN, decided against the NATURAL hillside.
    // The old build asked the POST-corridor field where the roof closed. Under
    // cut-and-cover that field is flat by construction, so it has nothing left
    // to say: the question "is there a tunnel here?" is a question about the
    // HILL, and the hill is the natural surface. Roof the reach where the
    // natural ground clears the shell's crown plus its cover, then run out past
    // each end as far as the bank still stands proud of the road — that
    // extension is the CUT-AND-COVER EXTENSION (a "false tunnel"), the thing
    // that puts the mouth out in daylight on a shallow bank instead of dragging
    // it back inside the hill.
    const float crownTot = kTcTubeCrownH + kTcShellThick + kTcLidCover;
    constexpr float kSpanDs = 1.0f;
    const int NS = (int)(route.totalLen / kSpanDs) + 1;
    std::vector<float> natCover((size_t)NS, 0.0f);
    for (int i = 0; i < NS; ++i) {
        const float s = kSpanDs * (float)i;
        const float ry = route.roadYAt(s);
        float m = 1e9f;
        for (int k = -3; k <= 3; ++k) {
            const float lat = (float)k * (kTcTubeHalfWidth + kTcShellThick) / 3.0f;
            float qx = 0.0f, qz = 0.0f;
            route.worldAt(s, lat, qx, qz);
            m = std::min(m, tunnelNaturalHeightAt(qx, qz) - ry);
        }
        natCover[(size_t)i] = m;
    }
    int bestA = 0, bestB = -1, runA = -1;
    for (int i = 0; i < NS; ++i) {
        const bool full = natCover[(size_t)i] >= crownTot;
        if (full && runA < 0) runA = i;
        if ((!full || i == NS - 1) && runA >= 0) {
            const int b = full ? i : i - 1;
            if (b - runA > bestB - bestA) { bestA = runA; bestB = b; }
            runA = -1;
        }
    }
    if (bestB >= bestA) {
        route.coverS0 = kSpanDs * (float)bestA;
        route.coverS1 = kSpanDs * (float)bestB;
        int a2 = bestA, b2 = bestB;
        const int maxExt = (int)(kTcPortalExtend / kSpanDs);
        while (a2 > 0 && (bestA - a2) < maxExt &&
               natCover[(size_t)(a2 - 1)] >= kTcPortalMinBank) --a2;
        while (b2 < NS - 1 && (b2 - bestB) < maxExt &&
               natCover[(size_t)(b2 + 1)] >= kTcPortalMinBank) ++b2;
        // Keep both portals off the very ends of the corridor: a portal needs an
        // approach cutting in front of it, and the corridor's own end caps round
        // the depth off over ~halfWidth there.
        route.boreS0 = clampf(kSpanDs * (float)a2, 24.0f, route.totalLen - 24.0f);
        route.boreS1 = clampf(kSpanDs * (float)b2, 24.0f, route.totalLen - 24.0f);
        route.boreValid = (route.boreS1 - route.boreS0) > 40.0f;
    }

    // --- 6) MEASURE THE DEFECT. Full road width, 0.5 m, against the final
    // field. Under cut-and-cover both numbers come out 0 / negative; they are
    // logged every boot and asserted by --test-tunnelmouth M1/M2 so a future
    // change to the grading or the falloff cannot quietly bring the ramp back.
    route.maxRoadBury = -1e9f;
    route.buriedRoadLen = 0.0f;
    for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 0.5f) {
        const float ry = route.roadYAt(s);
        float worst = -1e9f;
        for (int k = -6; k <= 6; ++k) {
            const float lat = (float)k * kTcRoadHalfWidth / 6.0f;
            float qx = 0.0f, qz = 0.0f;
            route.worldAt(s, lat, qx, qz);
            const float h = tunnelNaturalHeightAt(qx, qz) - terrainCorridorDepthAt(c, qx, qz);
            worst = std::max(worst, h - ry);
        }
        if (worst > 0.0f) route.buriedRoadLen += 0.5f;
        route.maxRoadBury = std::max(route.maxRoadBury, worst);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// registerTunnelCorridor — the BOOT step. Derive, register, report.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MULTI-ROUTE REGISTRY (registerTunnelCorridorFor / tunnelRouteCount /
// tunnelRouteAt). The module used to own exactly one route in a function-local
// static, which is what limited the game to a single tunnel; the city's four
// freeway bores each need their own.
//
// This rides the cut-and-cover derivation as-is. RouteSeed already carries
// exactly the parameters TunnelSpec does, and deriveRoute() is already a pure
// function of (seed) -> (route, corridor) — their own mouth gate re-derives on
// three extra hillsides through the same door. So there is nothing to
// generalize here beyond storage: seed it, derive it, register it, keep it.
// ---------------------------------------------------------------------------
namespace {
// Stable addresses: callers hold the returned pointer for the process lifetime.
// deque, not vector — a vector reallocation would dangle every pointer already
// handed out, and the whole contract of this API is that the pointer stays good.
std::deque<TunnelRoute>& routeStore() { static std::deque<TunnelRoute> v; return v; }
} // namespace

// ---------------------------------------------------------------------------
// THE SHARED TUNNEL SURFACE LIBRARY (CITY_BORES_PLAN B3, finally done).
//
// Every TunnelCorridorWorld used to own a SurfaceLibrary and mount it itself, so
// each dressed bore decoded the SAME 2K sets again — cc_cement_white alone is a
// 3.1 MB albedo, a 4.8 MB mr and a 9.4 MB normal. One showcase bore: invisible.
// Four city bores: four copies. Eight network bores (the ring roads bore through
// four ranges): eight copies of every set, and eight PNG-decode hitches.
//
// This is the same fix, and the same reasoning, that SurfaceLibrary's own header
// already documents for the WorldStreamer: "a streamer-lifetime library means
// textures are decoded ONCE per process, not per region realize (a city rebuild
// was a 2 s PNG-decode hitch)". Tunnels get the streamer's treatment.
//
// OWNERSHIP MOVES WITH IT. TunnelCorridorWorld::shutdown() used to call
// m_surf.destroyAll(). Against a shared library that is a double-free waiting to
// happen: the first bore to shut down would delete the textures the others are
// still drawing with. Release is now explicit and process-wide —
// shutdownTunnelSurfaces().
// ---------------------------------------------------------------------------
namespace {
SurfaceLibrary& tunnelSurfaces() { static SurfaceLibrary lib; return lib; }
} // namespace

void shutdownTunnelSurfaces(x3::rhi::IRenderDevice& device) {
    tunnelSurfaces().destroyAll(device);
}

// ---- the merged light pool (see kMaxTunnelLightsInFlight in the header) -----
namespace {
// Every BUILT bore, in build order. Raw pointers: a TunnelCorridorWorld owns its
// own lights and removes itself in shutdown(), so an entry can never outlive the
// object it points at.
std::vector<const TunnelCorridorWorld*>& liveTunnels() {
    static std::vector<const TunnelCorridorWorld*> v; return v;
}
} // namespace

void registerTunnelLightSource(const TunnelCorridorWorld* t) {
    if (!t) return;
    auto& v = liveTunnels();
    if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
}

void unregisterTunnelLightSource(const TunnelCorridorWorld* t) {
    auto& v = liveTunnels();
    v.erase(std::remove(v.begin(), v.end(), t), v.end());
}

uint32_t uploadTunnelLights(x3::rhi::IRenderDevice& device, const float camPos[3]) {
    const auto& v = liveTunnels();
    if (v.empty()) return 0;

    // Gather with the squared distance to the camera. Squared: the ordering is
    // identical and it avoids a sqrt per light per frame.
    struct Scored { float d2; x3::rhi::PointLight l; };
    static std::vector<Scored> pool;          // reused; this runs every frame
    pool.clear();
    for (const TunnelCorridorWorld* t : v) {
        for (const x3::rhi::PointLight& l : t->lights()) {
            const float dx = l.pos[0] - camPos[0];
            const float dy = l.pos[1] - camPos[1];
            const float dz = l.pos[2] - camPos[2];
            pool.push_back({ dx*dx + dy*dy + dz*dz, l });
        }
    }
    if (pool.empty()) return 0;

    const uint32_t k = (uint32_t)std::min<size_t>(pool.size(), kMaxTunnelLightsInFlight);
    // partial_sort, not sort: we need the nearest K in order, not all of them.
    std::partial_sort(pool.begin(), pool.begin() + k, pool.end(),
                      [](const Scored& a, const Scored& b) { return a.d2 < b.d2; });

    static std::vector<x3::rhi::PointLight> out;
    out.clear();
    out.reserve(k);
    for (uint32_t i = 0; i < k; ++i) out.push_back(pool[i].l);
    device.setPointLights(out.data(), k);
    return k;
}

const TunnelRoute* registerTunnelCorridorFor(const TunnelSpec& spec) {
    const float len = std::sqrt(spec.dirX * spec.dirX + spec.dirZ * spec.dirZ);
    if (!(len > 1e-4f) || !(spec.halfLen > 1.0f)) {
        x3::logError(std::string("tunnel corridor: '") + (spec.name ? spec.name : "?") +
                     "' is degenerate (zero heading or half-length) — not registered");
        return nullptr;
    }
    RouteSeed seed{};
    seed.cx = spec.cx; seed.cz = spec.cz;
    seed.dirX = spec.dirX / len; seed.dirZ = spec.dirZ / len;   // normalize on entry
    seed.halfLen = spec.halfLen;

    routeStore().emplace_back();
    TunnelRoute& route = routeStore().back();

    TerrainCorridor c{};
    deriveRoute(seed, route, c);
    // AFTER deriveRoute, not before: its first statement is `route = TunnelRoute{}`,
    // so a name assigned above this line was wiped every time and every tunnel
    // in the boot log was called ''. Which is why five misplaced tour bores
    // could not name themselves.
    route.name = spec.name ? spec.name : "tunnel";

    if (!registerTerrainCorridor(c)) {
        // Registry full (kMaxTerrainCorridors) — drop the half-built route rather
        // than hand back one whose carve never reached the height field.
        x3::logError(std::string("tunnel corridor: '") + route.name +
                     "' REJECTED by registerTerrainCorridor (registry full?)");
        routeStore().pop_back();
        return nullptr;
    }
    char b[256];
    std::snprintf(b, sizeof(b),
        "tunnel corridor: '%s' registered — %.0f m spine, roofed %s",
        route.name, route.totalLen, route.boreValid ? "YES" : "no (open cutting)");
    x3::logInfo(b);
    return &route;
}

// The demo ridge as a spec, so the drive-through gate registers it through the
// SAME door the city's four freeway bores use. If that door ever diverges from
// registerTunnelCorridor()'s own path, the gate is testing the wrong thing —
// which is exactly why it goes through here rather than reaching for the
// singleton.
TunnelSpec demoTunnelSpec() {
    TunnelSpec demo;
    demo.name = "demo ridge";
    demo.cx = kRouteCX;     demo.cz = kRouteCZ;
    demo.dirX = kRouteDirX; demo.dirZ = kRouteDirZ;
    demo.halfLen = kRouteHalfLen;
    return demo;
}

uint32_t tunnelRouteCount() { return (uint32_t)routeStore().size(); }

const TunnelRoute* tunnelRouteAt(uint32_t i) {
    return i < routeStore().size() ? &routeStore()[i] : nullptr;
}

const TunnelRoute& registerTunnelCorridor() {
    // DELEGATES to the multi-route door. This used to own a private static
    // TunnelRoute and register its own TerrainCorridor, which was correct while
    // one tunnel existed. With the city's freeway bores going through
    // registerTunnelCorridorFor(), keeping a second registration path meant the
    // demo ridge got carved TWICE — two overlapping corridors on one hill. That
    // is not a tidiness point: it broke both tunnel gates (the drive-through
    // stopped exiting the far portal, the mouth gate fell to 4/7).
    static const TunnelRoute* cached = nullptr;
    if (cached) return *cached;
    cached = registerTunnelCorridorFor(demoTunnelSpec());
    if (!cached) {                       // registry full / degenerate
        static TunnelRoute dead;
        dead.boreValid = false;
        return dead;
    }
    TunnelRoute& route = const_cast<TunnelRoute&>(*cached);


    float maxCut = 0.0f;
    for (const auto& n : route.st) maxCut = std::max(maxCut, n.latMax - n.roadY);
    char b[640];
    std::snprintf(b, sizeof(b),
        "tunnel corridor: CUT-AND-COVER — 1 corridor, %d nodes, %.0f m, floor %.1f/%.1f | "
        "road %.1f..%.1f m | max cut %.1f m | ROOFED s=%.0f..%.0f (%.0f m), of which the "
        "natural hillside alone covers s=%.0f..%.0f (%.0f m) and the rest is the "
        "cut-and-cover extension | road under earth: %.1f m (worst ground-above-road "
        "%.2f m — negative is the trench floor sitting under the slab, which is correct)",
        kRouteNodes, route.totalLen, kTcCorridorHalfW, kTcCorridorFall,
        route.st.front().roadY, route.st.back().roadY, maxCut,
        route.boreS0, route.boreS1, route.boreS1 - route.boreS0,
        route.coverS0, route.coverS1, route.coverS1 - route.coverS0,
        route.buriedRoadLen, route.maxRoadBury);
    x3::logInfo(b);
    if (route.buriedRoadLen > 0.0f)
        x3::logError("tunnel corridor: THE MOUTH DEFECT IS BACK — ground is standing on the "
                     "roadway. Run --test-tunnelmouth.");
    if (!route.boreValid)
        x3::logWarn("tunnel corridor: no roofed span found — the demo will be an open cutting only");
    return route;
}

// ---------------------------------------------------------------------------
// TunnelCorridorWorld::build
// ---------------------------------------------------------------------------
bool TunnelCorridorWorld::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& physics, const TunnelRoute& route,
                                x3::rhi::TextureHandle groundTex) {
    if (route.st.size() < 2) return false;

    // ---- THE FRAME EVERY PIECE OF DRESSING RIDES ON. -----------------------
    // Defined HERE, at the top, for one reason: the tripwire below has to
    // interrogate THIS function. A guard that re-derives the position from the
    // route (rather than asking the thing the geometry actually uses) can only
    // ever prove that the route agrees with itself, which is vacuous.
    //
    // THE FRAME FOLLOWS **THIS** ROUTE. This lambda used to read the file-scope
    // demo constants — `kRouteCX + kRouteDirX * (-kRouteHalfLen + s)` — which is
    // the tail end of the one-tunnel era the P1 note above describes. posAt(),
    // worldAt() and tangentAt() were all moved onto the route's own polyline;
    // this lambda was missed, and it is the one every piece of DRESSING goes
    // through (ribbon, shell, portals, lights, fitout).
    //
    // The result, measured with the AABB log below and X3_OUTER_RING=1: the
    // CUTTING geometry (which calls route.worldAt) landed correctly on each
    // outer-tour chord 7 km out, while the ribbon/shell/portals landed on the
    // DEMO axis over the spawn country — and the quads that joined the two were
    // stretched across the gap. All five tour bores measured 3.1-7.1 km of X
    // extent anchored at the demo spine's start corner (~-289, -476); that
    // stretched sheet, seen from the spawn probe, is the "kilometre floating
    // tunnel-shell tower + dark deck".
    //
    // posAt() is EXACTLY equivalent to the old expression for the demo route
    // (its stations are laid `ox + dir*s` with ox = cx - dir*halfLen), so the
    // demo bore is unchanged to the metre — and every other route now dresses
    // itself where it actually is.
    auto frameAt = [&](float s) {
        Frame f{}; f.s = s;
        route.posAt(s, f.p);
        return f;
    };

    // ---- THE TRIPWIRE, before a single texture or vertex exists. -----------
    // A missing tunnel is a defect you can drive past. A kilometre-scale shell
    // standing over the spawn country is not. Both limits are CALIBRATED ON
    // MEASUREMENT (X3_OUTER_RING=1 boot, the AABB log at the foot of this
    // function, before the frameAt fix):
    //
    //   healthy demo bore   : dressing overhangs its own spine by ~8 m in XZ;
    //                         built AABB 598 x 208 x 261 m
    //   the five tour bores : dressing anchored on the DEMO axis instead of
    //                         their own chords — 3.1 to 7.1 km of X extent
    //
    // 150 m of stray is ~19x the healthy overhang and a twentieth of the
    // smallest failure, so nothing legitimate is near it. The vertical cap is
    // the catch-all for a future defect that does not happen to displace the
    // route laterally. NOTE the brief proposed 120 m there: the healthy demo
    // bore MEASURES 208 m tall, because its backfill lid carries a real
    // hillside over it, so 120 m would have skipped the one bore that works.
    // 400 m is ~2x the measured healthy span.
    {
        constexpr float kBoreStrayMaxM  = 150.0f;
        constexpr float kBoreHeightMaxM = 400.0f;
        const char* nm = (route.name && route.name[0]) ? route.name : "(unnamed bore)";

        float sxMin = 1e30f, sxMax = -1e30f, szMin = 1e30f, szMax = -1e30f;
        for (const auto& st : route.st) {
            sxMin = std::min(sxMin, st.x); sxMax = std::max(sxMax, st.x);
            szMin = std::min(szMin, st.z); szMax = std::max(szMax, st.z);
        }
        float stray = 0.0f, datumMin = 1e30f, groundMax = -1e30f;
        const float step = std::max(4.0f, route.totalLen / 512.0f);
        for (float s = 0.0f; s <= route.totalLen + 0.01f; s += step) {
            // frameAt, NOT posAt — see the note above. This is the only version
            // of the question that can fail.
            const Frame fr = frameAt(s);
            const float* p = fr.p;
            stray = std::max(stray, std::max(sxMin - p[0], p[0] - sxMax));
            stray = std::max(stray, std::max(szMin - p[2], p[2] - szMax));
            datumMin = std::min(datumMin, p[1]);
            for (int k = -3; k <= 3; ++k) {
                float qx = 0.0f, qz = 0.0f;
                route.worldAt(s, (float)k * kTcCorridorHalfW / 3.0f, qx, qz);
                groundMax = std::max(groundMax, tunnelNaturalHeightAt(qx, qz));
            }
        }
        const float vSpan = groundMax - datumMin;
        if (stray > kBoreStrayMaxM || vSpan > kBoreHeightMaxM) {
            char tb[460];
            std::snprintf(tb, sizeof(tb),
                "tunnel corridor: REFUSING to dress bore '%s' — frame strays %.0f m from "
                "its own spine (limit %.0f) / vertical span %.0f m (limit %.0f). Chord "
                "centre (%.0f, %.0f) halfLen %.0f. Skipped: a missing tunnel you can "
                "drive past, a floating shell over the country you cannot.",
                nm, stray, kBoreStrayMaxM, vSpan, kBoreHeightMaxM,
                route.cx, route.cz, route.halfLen);
            x3::logError(tb);
            return false;
        }
    }

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
    //   BORE LINING -> cc_cement_white. c8d28eda picked cv_shotcrete_break here
    //     on the reasoning that a bore is lined with sprayed concrete, which is
    //     true, and that it was the only concrete-family set with a real
    //     multi-channel mr, which was true then. LOOK AT THE SET, though: it is
    //     shotcrete AFTER A FAILURE - a cracked face with loose rubble in the
    //     fissures. Swept over 330 m of arch it tiles into a field of scales and
    //     the bore reads as a lava tube, at every texel density tried (0.30,
    //     0.62, 1.05 - three passes on that number before the material itself
    //     turned out to be the problem). cc_cement_white is a smooth pale cement,
    //     ships the RICHEST maps in the whole library (a 5 MB multi-channel mr
    //     and a 11.8 MB normal, both far beyond the shotcrete set), and pale is
    //     also what a road tunnel is actually lined in - the lining is chosen to
    //     raise interior luminance so drivers adapt faster coming in from
    //     daylight. It is tinted down slightly because near-white under eight
    //     point lights clips.
    //   (kept for the record) cv_shotcrete_break. A road bore is lined with SHOTCRETE
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
    //   ROAD -> rd_asphalt_01. This comment used to read "NOT WIRED, and
    //     honestly: the ROAD. There is no asphalt set in the library" — true
    //     when it was written, and false now: rd_asphalt_01 landed on
    //     2026-08-10 with a full albedo/mr/normal set. The ribbon was still
    //     wearing a 128 px procedural checker for four days because nobody
    //     re-read the premise after the library grew. Falls back to that same
    //     checker when the set is absent, so a bare checkout is unchanged.
    // Mount is idempotent (it just records the root), and get() caches per set,
    // so the SECOND and later bores pay ZERO texture uploads for these.
    SurfaceLibrary& surf = tunnelSurfaces();
    surf.mount(x3::game::assetRoot() + "/surface_library");
    const SurfaceSet& boreSet   = surf.get(device, "cc_cement_white");
    const SurfaceSet& portalSet = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& roadSet   = surf.get(device, "rd_asphalt_01");
    if (!boreSet.ok || !portalSet.ok || !roadSet.ok)
        x3::logWarn("tunnel corridor: surface_library set(s) unavailable — "
                    "falling back to the procedural checker concrete/asphalt");

    // ---- THE BUILT-GEOMETRY AABB. Measured, not assumed: every mesh this
    // build uploads is folded into one box, and the box is logged with the
    // route's name at the end. It is the cheapest possible answer to "where did
    // this bore actually put itself", and it is what convicted the frameAt
    // defect below (an outer-tour bore whose meshes landed on the DEMO axis,
    // kilometres from its own chord, with a Y extent to match).
    float gMin[3] = {  1e30f,  1e30f,  1e30f };
    float gMax[3] = { -1e30f, -1e30f, -1e30f };
    auto upload = [&](MeshBuf& mb, const Material& mat, bool collide) {
        if (mb.empty()) return;
        for (const auto& v : mb.v)
            for (int k = 0; k < 3; ++k) {
                if (v.pos[k] < gMin[k]) gMin[k] = v.pos[k];
                if (v.pos[k] > gMax[k]) gMax[k] = v.pos[k];
            }
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
    std::vector<Frame> roadFrames;
    for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 4.0f) roadFrames.push_back(frameAt(s));

    // ================= 1) THE ROAD RIBBON ===================================
    // A swept slab. It is thick (kSlabDrop) on purpose: the corridor's depth is
    // constant across its flat floor, so the floor still carries the hillside's
    // LATERAL slope. The slab's skirt swallows that mismatch and reads as a
    // retaining kerb where the cut is deepest.
    {
        // 0.14 -> 0.02 m (0.46 ft -> 0.08 ft). Tim, driving it: "the road is
        // [1.8] feet in the air". He was reading the STEP at the road edge —
        // the slab standing kSlabProud ABOVE the road datum while the corridor
        // floor beside it is cut kFloorClear BELOW it. 0.46 + 0.72 = 1.18 ft of
        // vertical face right at the white line, with no shoulder bridging it.
        //
        // kSlabProud was buying nothing: it exists to keep the slab off the
        // floor, but kFloorClear already separates them by 0.72 ft on its own,
        // which is far more than depth precision needs at this range. Dropping
        // it to a hair leaves the markings proud of the slab and takes the step
        // down to ~0.74 ft — a kerb, which is what a road edge should look like.
        // 0.02 -> 0.07 (Tim's seam screenshot) -> BACK TO 0.02. The 0.07 lift
        // was masking, and it never masked the real thing: the "few-cm border
        // disagreement" theory was wrong. The strip was the CORRIDOR x TILE-LOD
        // WEDGE — a Half/Quarter terrain tile interpolating 2/4 m chords across
        // the carve's smoothstep shoulder, standing METRES above the carved
        // surface (measured 5.96 m of coarse-over-Full excess at the spawn
        // cutting; no slab lift can outrun that). The root fix is in
        // buildTileMeshAbs: cells inside a corridor's influence mesh at FULL
        // resolution at every LOD, so the coarse surface is bit-identical to
        // Full there (--test-terraincorridor C7, --test-tunnelmouth M7,
        // --test-roadnetwork W1/W1b all gate it). With the wedge dead, 2 cm is
        // all the slab ever needed — see the 0.14 -> 0.02 story above.
        constexpr float kSlabProud = 0.02f;
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
        // CONCRETE APRONS (Tim, reading 01_approach: "There is no concrete
        // apron in that shot"). The demo road was the ONLY route without
        // them — every road_network route carries kApronFt of cement each
        // side. Width here is budgeted by the CARVE, not the spec: the
        // corridor's flat floor is kTcCorridorHalfW (10.1 m) and the
        // pavement takes kTcRoadHalfWidth (7.32), leaving 2.78 m of floor
        // before the smoothstep shoulder rises — a wider apron would bury
        // its outer edge in the cut bank. PAIRED with kTcCorridorHalfW
        // (tunnel_corridor.h): widen the carve and this can grow toward
        // road_network's 20 ft.
        constexpr float kApronW    = 2.6f;   // ~8.5 ft of cement each side
        constexpr float kApronDrop = 0.01f;  // apron 1 cm under the slab lip
        const float hwA = hw + kApronW;
        const float apY = kSlabProud - kApronDrop;
        MeshBuf mbA;                          // cement: aprons + outer skirts
        for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
            const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
            const float nU[3] = { 0, 1, 0 };
            const float nR[3] = {  right[0], 0.0f,  right[2] };
            const float nL[3] = { -right[0], 0.0f, -right[2] };
            const float nD[3] = { 0, -1, 0 };
            // ---- pavement top at +/-hw, with a 1 cm lip face down to the
            // apron so there is no sliver of daylight at the joint.
            float aL[3], aR[3], bL[3], bR[3];
            P(a, -hw, kSlabProud, aL); P(a, hw, kSlabProud, aR);
            P(b, -hw, kSlabProud, bL); P(b, hw, kSlabProud, bR);
            mb.quad(aL, aR, bR, bL, nU, 0.0f, 1.0f, a.s * 0.08f, b.s * 0.08f);
            float aLl[3], aRl[3], bLl[3], bRl[3];
            P(a, -hw, apY, aLl); P(a, hw, apY, aRl);
            P(b, -hw, apY, bLl); P(b, hw, apY, bRl);
            mb.quad(aR, aRl, bRl, bR, nR, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            mb.quad(aL, aLl, bLl, bL, nL, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            // ---- cement apron tops, hw -> hwA each side.
            float aRo[3], bRo[3], aLo[3], bLo[3];
            P(a,  hwA, apY, aRo); P(b,  hwA, apY, bRo);
            P(a, -hwA, apY, aLo); P(b, -hwA, apY, bLo);
            mbA.quad(aRl, aRo, bRo, bRl, nU, 0.0f, 1.0f, a.s * 0.06f, b.s * 0.06f);
            mbA.quad(aLo, aLl, bLl, bLo, nU, 0.0f, 1.0f, a.s * 0.06f, b.s * 0.06f);
            // ---- outer skirts + slab bottom now hang from the APRON edge.
            float aLd[3], aRd[3], bLd[3], bRd[3];
            PA(a, -hwA, edgeBottom(a, -hwA), aLd); PA(a, hwA, edgeBottom(a, hwA), aRd);
            PA(b, -hwA, edgeBottom(b, -hwA), bLd); PA(b, hwA, edgeBottom(b, hwA), bRd);
            mbA.quad(aRo, aRd, bRd, bRo, nR, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            mbA.quad(aLo, aLd, bLd, bLo, nL, 0.0f, 1.0f, a.s * 0.15f, b.s * 0.15f);
            mbA.quad(aLd, aRd, bRd, bLd, nD, 0.0f, 1.0f, a.s * 0.08f, b.s * 0.08f);
        }
        // REAL ASPHALT when the library has it, the procedural checker when it
        // does not — same shape as the bore/portal sets above. The road is the
        // surface the player stares at for the entire drive, so a 128 px
        // two-tone checker was the most-looked-at placeholder in the game.
        Material m;
        if (roadSet.ok) { m.alb = roadSet.albedo; m.mr = roadSet.mr; m.nrm = roadSet.normal; }
        else            { m.alb = asphaltTex;     m.mr = roughMR; }
        upload(mb, m, /*collide*/true);
        // Aprons in the SAME cement as road_network's (mw_concrete_panels_a,
        // already loaded as portalSet) with the same warm tint, so the demo
        // road and the network routes read as one build standard.
        Material mA;
        if (portalSet.ok) { mA.alb = portalSet.albedo; mA.mr = portalSet.mr; mA.nrm = portalSet.normal; }
        else              { mA.alb = concreteTex;      mA.mr = wallMR; }
        mA.tint[0] = 0.86f; mA.tint[1] = 0.85f; mA.tint[2] = 0.82f;
        upload(mbA, mA, /*collide*/true);

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

        // ================= 1a) THE INTERIOR FITOUT =========================
        // Walkways, railings, the subway wall band, signage, screens and door
        // recesses. WHERE each thing goes was decided in tunnel_fitout.h -- and
        // decided there rather than here on purpose, because placement is the
        // part that can be wrong in ways a screenshot hides. This block only
        // DRAWS what that module already proved.
        //
        // Everything is gated on the ROOFED span. A walkway running out of the
        // portal and down the open approach cutting would be the same
        // built-but-not-thought-through shape as snow falling inside a tunnel.
        if (route.boreValid) {
            TunnelFitout fit;
            FitoutConfig fcfg;
            fit.build(route.boreS0, route.boreS1, fcfg, kTunnelFitoutSeed);

            MeshBuf walk, rail, band, panel, glow;
            const float deckY = kTcWalkKerbH;              // deck top, above the road datum
            const float hw    = kTcRoadHalfWidth;
            const float deckW = kTcWalkDeckW;

            // ---- WALKWAY: kerb face + deck top, both sides ----------------
            // Broken at a lay-by on that side, which is what makes a bay
            // reachable from a stopped car instead of a kerb you climb.
            for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
                const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
                if (a.s < route.boreS0 || b.s > route.boreS1) continue;
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    if (fit.walkwayBrokenAt(a.s, sgn)) continue;
                    const float i0 = (float)sgn * hw;            // inner (road) edge
                    const float o0 = (float)sgn * (hw + deckW);  // outer (wall) edge
                    float a0[3], a1[3], b0[3], b1[3], a0d[3], b0d[3];
                    // Kerb face, road level -> deck level.
                    P(a, i0, 0.0f,   a0d); P(b, i0, 0.0f,   b0d);
                    P(a, i0, deckY,  a0);  P(b, i0, deckY,  b0);
                    const float nIn[3] = { -(float)sgn * right[0], 0.0f, -(float)sgn * right[2] };
                    if (sgn > 0) walk.quad(a0d, a0, b0, b0d, nIn, 0, 1, a.s*0.3f, b.s*0.3f);
                    else         walk.quad(a0, a0d, b0d, b0, nIn, 0, 1, a.s*0.3f, b.s*0.3f);
                    // Deck top.
                    P(a, o0, deckY, a1); P(b, o0, deckY, b1);
                    const float nU[3] = { 0, 1, 0 };
                    if (sgn > 0) walk.quad(a0, a1, b1, b0, nU, 0, 1, a.s*0.25f, b.s*0.25f);
                    else         walk.quad(a1, a0, b0, b1, nU, 0, 1, a.s*0.25f, b.s*0.25f);
                }
            }

            // ---- RAILING. Tim: "Not decorative: this is what makes a tunnel
            // read as infrastructure rather than a tube." Posts on a 2.5 m
            // pitch with a top rail. Drawn as thin boxes rather than cylinders
            // because at 40 ft away across a lit bore the silhouette is all you
            // read, and a box costs a twelfth of the triangles.
            {
                const float postPitch = 2.5f;
                const float railH = 1.05f;          // 3.4 ft, a real handrail height
                const float lat = hw + deckW - 0.12f;
                for (float sPos = route.boreS0 + 1.0f; sPos < route.boreS1 - 1.0f; sPos += postPitch) {
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        if (fit.walkwayBrokenAt(sPos, sgn)) continue;
                        // EXACT STATION, not the nearest frame. That comment used
                        // to claim a proportional lookup was "exact to well under a
                        // post's width" and it was simply wrong: roadFrames are laid
                        // every 4 m (s += 4.0f at the build) while these posts are on
                        // a 2.5 m pitch, so the snap collapsed roughly one post in
                        // three onto its neighbour's frame -- doubled posts with a
                        // gap beside them, at 13 ft intervals down both sides of the
                        // bore. Found because the LNSS lane hit the same snap with a
                        // wall standoff and reported it; the railings were making the
                        // identical mistake one function away.
                        Frame fr = frameAt(sPos);
                        // obox is CENTRE + HALF-extents, so the post is placed
                        // at mid-height with half its length, not at its foot.
                        float mid[3];
                        P(fr, (float)sgn * lat, deckY + railH * 0.5f, mid);
                        const float ax[3] = { route.dirX, 0.0f, route.dirZ };
                        obox(rail, mid, right, ax, 0.035f, railH * 0.5f, 0.035f, 1.0f);
                    }
                }
                // The top rail: one continuous run per side, per unbroken span.
                for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
                    const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
                    if (a.s < route.boreS0 || b.s > route.boreS1) continue;
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        if (fit.walkwayBrokenAt(a.s, sgn)) continue;
                        float a0[3], a1[3], b0[3], b1[3];
                        P(a, (float)sgn*(lat-0.03f), deckY+railH, a0);
                        P(a, (float)sgn*(lat+0.03f), deckY+railH, a1);
                        P(b, (float)sgn*(lat+0.03f), deckY+railH, b1);
                        P(b, (float)sgn*(lat-0.03f), deckY+railH, b0);
                        const float nU[3] = { 0, 1, 0 };
                        rail.quad(a0, a1, b1, b0, nU, 0, 1, a.s*0.4f, b.s*0.4f);
                    }
                }
            }

            // ---- THE SUBWAY BAND. A tiled wainscot from just above the deck
            // to head height, proud of the shell by a centimetre. This is the
            // single cheapest thing that moves a bore from "concrete pipe" to
            // "station": real transit tunnels are tiled to head height and bare
            // above it, and the HORIZONTAL LINE that creates is what the eye
            // reads as a built interior. Without it the wall is one untouched
            // sweep from floor to crown and no amount of signage rescues it.
            {
                const float bandLo = deckY + 0.10f;
                const float bandHi = deckY + 2.35f;      // 7.7 ft: head height
                const float lat = kTcTubeHalfWidth - 0.012f;
                for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
                    const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
                    if (a.s < route.boreS0 || b.s > route.boreS1) continue;
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        float a0[3], a1[3], b0[3], b1[3];
                        P(a, (float)sgn*lat, bandLo, a0);
                        P(a, (float)sgn*lat, bandHi, a1);
                        P(b, (float)sgn*lat, bandHi, b1);
                        P(b, (float)sgn*lat, bandLo, b0);
                        const float nIn[3] = { -(float)sgn*right[0], 0.0f, -(float)sgn*right[2] };
                        if (sgn > 0) band.quad(a0, a1, b1, b0, nIn, 0, 1, a.s*0.22f, b.s*0.22f);
                        else         band.quad(a1, a0, b0, b1, nIn, 0, 1, a.s*0.22f, b.s*0.22f);
                    }
                }
            }

            // ---- WALL FITTINGS: signs, screens, door recesses -------------
            // One loop, because they are the same operation at different sizes
            // and colours: a quad on the wall at a station the fitout chose.
            {
                auto frameFor = [&](float sPos) -> const Frame& {
                    const size_t idx = (size_t)clampf(
                        (sPos / std::max(1e-3f, route.totalLen)) * (float)(roadFrames.size() - 1),
                        0.0f, (float)(roadFrames.size() - 1));
                    return roadFrames[idx];
                };
                for (const Fitting& fg : fit.fittings()) {
                    if (fg.kind == FittingKind::Lamp) continue;   // the light pool owns those
                    const Frame& fr = frameFor(fg.s);
                    const float sgn = (float)fg.side;
                    const float lat = kTcTubeHalfWidth - 0.03f;
                    float w = 0.9f, h = 0.6f, yc = deckY + 1.55f;
                    MeshBuf* into = &panel;
                    if (fg.kind == FittingKind::Screen) { w = 2.6f; h = 1.4f; yc = deckY + 1.9f; into = &glow; }
                    else if (fg.kind == FittingKind::Sign) { w = 1.6f; h = 0.45f; yc = deckY + 2.05f; into = &glow; }
                    else if (fg.kind == FittingKind::Door) { w = 1.1f; h = 2.1f; yc = deckY + 1.05f; }
                    else if (fg.kind == FittingKind::SosNiche) { w = 0.8f; h = 1.0f; yc = deckY + 1.1f; }
                    // The quad, spanning +/- w/2 ALONG the route.
                    const float half = w * 0.5f;
                    float p0[3], p1[3], p2[3], p3[3];
                    for (int k = 0; k < 4; ++k) {
                        const float ds = (k == 0 || k == 3) ? -half : half;
                        const float dy = (k < 2) ? -h * 0.5f : h * 0.5f;
                        float* out = (k == 0) ? p0 : (k == 1) ? p1 : (k == 2) ? p2 : p3;
                        // Offset along the route by ds using the route's own
                        // direction, so a fitting stays flat on the wall.
                        Frame tmp = fr;
                        tmp.p[0] += route.dirX * ds;
                        tmp.p[2] += route.dirZ * ds;
                        P(tmp, sgn * lat, yc + dy, out);
                    }
                    const float nIn[3] = { -sgn*right[0], 0.0f, -sgn*right[2] };
                    if (fg.side > 0) into->quad(p0, p3, p2, p1, nIn, 0, 1, 0, 1);
                    else             into->quad(p1, p2, p3, p0, nIn, 0, 1, 0, 1);
                }
            }

            // ---- Upload. Walkways COLLIDE (you can stand on them); the band,
            // signage and rails do not -- a railing you can catch a wing mirror
            // on turns a 100 mph tunnel run into a lottery, and nothing about
            // the fiction needs it solid.
            Material wm; if (portalSet.ok) { wm.alb = portalSet.albedo; wm.mr = portalSet.mr; wm.nrm = portalSet.normal; }
                         else { wm.alb = concreteTex; wm.mr = roughMR; }
            upload(walk, wm, /*collide*/true);

            Material rm; rm.alb = paintTex; rm.mr = solid1(0, 90, 210, false);   // metal: smooth, metallic
            for (int c = 0; c < 3; ++c) rm.tint[c] = 0.42f;
            upload(rail, rm, /*collide*/false);

            Material bm; if (boreSet.ok) { bm.alb = boreSet.albedo; bm.mr = boreSet.mr; bm.nrm = boreSet.normal; }
                         else { bm.alb = concreteTex; bm.mr = roughMR; }
            for (int c = 0; c < 3; ++c) bm.tint[c] = 0.93f;   // tile reads lighter than the raw shell
            upload(band, bm, /*collide*/false);

            Material dm; dm.alb = concreteTex; dm.mr = roughMR;
            dm.tint[0] = 0.30f; dm.tint[1] = 0.33f; dm.tint[2] = 0.38f;
            upload(panel, dm, /*collide*/false);

            // Signs and screens are EMISSIVE -- they are the only thing in the
            // bore that makes its own light, which is exactly why they read at
            // distance and why they must not also be lights in the pool (that
            // budget is spent, and 19 signs would blow it on their own).
            Material gm; gm.alb = paintTex; gm.mr = roughMR;
            gm.emissive[0] = 0.55f; gm.emissive[1] = 0.80f; gm.emissive[2] = 1.0f; gm.emissive[3] = 1.4f;
            upload(glow, gm, /*collide*/false);

            // ================= 1c) BEHIND THE DOORS =====================
            // The halls, rooms and stairs. WHERE they go, how much rock is over
            // them and which door earns a program was decided (and proved, 15/15)
            // in tunnel_rooms.h against the real lid height -- this draws it.
            //
            // Each space is an axis-aligned box in ROUTE coordinates, so it is
            // emitted INWARD-FACING: you are inside it, and the faces you would
            // see from the bore are the ones skipped. Two spaces that touch skip
            // their shared face, which is what makes a doorway without a single
            // boolean cut -- the openings ARE the omissions.
            {
                TunnelRoomProgram prog;
                prog.build(route, fit, TunnelTier::A);

                // ==========================================================
                // ★ THE VEHICLE BAY IS LATE NIGHT SPEED. Not "inspired by" —
                // the second premises of the SAME shop. The claim was already
                // made before this pass: tunnel_rooms.h inventories the bay as
                // "Tim's own shop, LATE NIGHT SPEED, inventoried by its owner"
                // (3 Rotary two-posts, the Hunter rack, Road Force, tire
                // machine, Miller, Robinair), and RACING_WORLD.md records that
                // Club 1127 IS the real Miami shop. A bay that carries the
                // owner's exact machine inventory but generic tunnel concrete
                // was the wrong half of a decision — so it now DRESSES from the
                // same authored kit (app/lns_shop.h, moved out of club1127):
                //   * painted CMU block walls (running bond + mortar normal),
                //   * the glossy checkerboard floor (wet MR 8/255 — light
                //     pools in it, "exactly like the photos"),
                //   * the industrial ceiling bones (trusses / purlins /
                //     conduit / a galvanised duct — the club's authored
                //     layout, re-set to this room's frame),
                //   * the LATE NIGHT SPEED neon on the far wall — the Miami
                //     mount says CLUB 1127 because that shop is a club by
                //     night; a bay under 323 ft of rock has no night trade,
                //     so it wears the day name.
                // LEFT BEHIND on purpose: the DIY dome party-projectors, the
                // ceiling starburst, the laser floor patterns and the mirror
                // ball. Those are Club 1127's after-dark signature; down here
                // the same checkerboard reflects WORKLIGHTS. Dressing a
                // working bay as a party is exactly the slop the anti-slop
                // line exists to stop.
                // ==========================================================
                MeshBuf room, fixture, fglow, baypaint;
                MeshBuf lnsWall, lnsFloor;                    // the shop shell
                MeshBuf lnsSteel, lnsDuct, lnsCond;           // ceiling bones
                MeshBuf lnsEquip, lnsLamp;                    // Rotary red / worklights
                MeshBuf lnsNeon, lnsCab;                      // channel letters / sign cabinet
                const auto& sp = prog.spaces();

                auto frameAtS = [&](float sPos) -> const Frame& {
                    const size_t idx = (size_t)clampf(
                        (sPos / std::max(1e-3f, route.totalLen)) * (float)(roadFrames.size() - 1),
                        0.0f, (float)(roadFrames.size() - 1));
                    return roadFrames[idx];
                };
                // Do two boxes touch on a face? Axis-aligned in (station, lateral),
                // so "touching" is one pair of bounds meeting while the other
                // overlaps. Tolerance is generous (4 in): these are authored
                // numbers, not solved ones, and a hairline gap between a hall and
                // its room would leave a wall standing across the doorway.
                auto touchS = [&](const TunnelSpace& a, const TunnelSpace& b, bool aHigh) {
                    const float av = aHigh ? a.s1 : a.s0;
                    const float bv = aHigh ? b.s0 : b.s1;
                    if (std::fabs(av - bv) > 0.1f) return false;
                    return !(a.latOut < b.latIn - 0.1f || b.latOut < a.latIn - 0.1f)
                        && a.side == b.side;
                };
                auto touchLat = [&](const TunnelSpace& a, const TunnelSpace& b, bool aOut) {
                    const float av = aOut ? a.latOut : a.latIn;
                    const float bv = aOut ? b.latIn  : b.latOut;
                    if (std::fabs(av - bv) > 0.1f) return false;
                    return !(a.s1 < b.s0 - 0.1f || b.s1 < a.s0 - 0.1f) && a.side == b.side;
                };

                for (size_t i = 0; i < sp.size(); ++i) {
                    const TunnelSpace& sc = sp[i];
                    const float fy = sc.floorY, cy = sc.floorY + sc.clearH;
                    const float sgn = (float)sc.side;
                    const Frame& fa = frameAtS(sc.s0);
                    const Frame& fb = frameAtS(sc.s1);

                    // ---- SOLID ROCK. THE WALL HAS A DOORWAY IN IT; IT IS NOT
                    // ABSENT. The first cut of this skipped a whole shared face
                    // whenever two spaces touched, which is right only when they
                    // are the same size. They are not: the hall is 6.6 ft wide
                    // and the rooms are 15-20 ft deep, so skipping the face left
                    // up to 13 ft of open hole -- and the hole does not look out
                    // onto anything, because the mountain is a HEIGHTFIELD and
                    // the volume behind these rooms is VOID. Walk through one and
                    // you leave the world.
                    //
                    // So every face is emitted MINUS the opening: four border
                    // quads around a rectangle. The result is watertight by
                    // construction, and the doorway is the size of the thing that
                    // actually connects rather than the size of the smaller room.
                    // `dst` picks the batch (the garage's walls go to the CMU
                    // buffer, everything else stays on the bore concrete) and
                    // `uvm` > 0 switches the face to METRIC UVs: texture
                    // coordinates are the face's own in-plane metres * uvm, so
                    // the CMU blocks come out 16x8 IN at any wall size and the
                    // strips around a doorway stay registered with each other
                    // (they all read the same absolute metres). uvm == 0 keeps
                    // the legacy stretch-to-face 0..1 mapping byte-identical.
                    auto faceWithHole = [&](MeshBuf& dst, float uvm,
                                            float uMin, float uMax, float yMin, float yMax,
                                            float oU0, float oU1, float oY0, float oY1,
                                            const float nrm[3], bool flip,
                                            const std::function<void(float,float,float*)>& place) {
                        // Clip the opening to the face; an empty clip means a
                        // solid wall, which is the common case.
                        oU0 = std::max(oU0, uMin); oU1 = std::min(oU1, uMax);
                        oY0 = std::max(oY0, yMin); oY1 = std::min(oY1, yMax);
                        const bool hasHole = (oU1 - oU0 > 0.01f) && (oY1 - oY0 > 0.01f);
                        auto strip = [&](float a0, float a1, float b0, float b1) {
                            if (a1 - a0 < 0.005f || b1 - b0 < 0.005f) return;
                            float P0[3], P1[3], P2[3], P3[3];
                            place(a0, b0, P0); place(a1, b0, P1);
                            place(a1, b1, P2); place(a0, b1, P3);
                            const float u0 = uvm > 0.0f ? a0 * uvm : 0.0f;
                            const float u1 = uvm > 0.0f ? a1 * uvm : 1.0f;
                            const float w0 = uvm > 0.0f ? b0 * uvm : 0.0f;
                            const float w1 = uvm > 0.0f ? b1 * uvm : 1.0f;
                            // The flipped branch swaps the w params so each
                            // vertex keeps u(a), w(b) — mirrored UVs would
                            // shear the block courses at every strip seam.
                            if (flip) dst.quad(P3, P2, P1, P0, nrm, u0, u1,
                                               uvm > 0.0f ? w1 : 0.0f, uvm > 0.0f ? w0 : 1.0f);
                            else      dst.quad(P0, P1, P2, P3, nrm, u0, u1, w0, w1);
                        };
                        if (!hasHole) { strip(uMin, uMax, yMin, yMax); return; }
                        strip(uMin, oU0, yMin, yMax);   // left of the opening
                        strip(oU1, uMax, yMin, yMax);   // right of it
                        strip(oU0, oU1, yMin, oY0);     // under the lintel-to-floor
                        strip(oU0, oU1, oY1, yMax);     // over the head
                    };

                    // Find the neighbour that opens onto a given face, and the
                    // extent of that opening. Returns false for a solid wall.
                    auto openingOn = [&](bool lateralFace, bool highSide,
                                         float& u0, float& u1, float& y0, float& y1) {
                        for (size_t j2 = 0; j2 < sp.size(); ++j2) {
                            if (j2 == i) continue;
                            const TunnelSpace& o = sp[j2];
                            if (o.side != sc.side) continue;
                            bool touches = lateralFace ? touchLat(sc, o, highSide)
                                                       : touchS(sc, o, highSide);
                            if (!touches) continue;
                            // The opening is the OVERLAP of the two spaces on the
                            // face's in-plane axes -- never the whole face.
                            if (lateralFace) { u0 = std::max(sc.s0, o.s0);   u1 = std::min(sc.s1, o.s1); }
                            else             { u0 = std::max(sc.latIn, o.latIn); u1 = std::min(sc.latOut, o.latOut); }
                            y0 = std::max(sc.floorY, o.floorY);
                            y1 = std::min(sc.floorY + sc.clearH, o.floorY + o.clearH);
                            return true;
                        }
                        return false;
                    };

                    const float li = sgn * sc.latIn, lo = sgn * sc.latOut;
                    float A[3], B[3], C[3], D[3];
                    const float nU[3] = { 0, 1, 0 }, nD[3] = { 0, -1, 0 };

                    // THE GARAGE WEARS THE SHOP. Its walls leave the bore-
                    // concrete batch for the painted CMU block, its floor for
                    // the glossy checkerboard — see the LNS banner above. The
                    // CMU texture spans 1.6 m (4 blocks x 8 courses of real
                    // 16x8 in units), the checker 8 m (8 one-metre tiles), so
                    // the metric-UV factors are 1/1.6 and 1/8. The RAMP stays
                    // concrete on purpose: it is a driveway through rock; the
                    // shop starts where the floor levels out.
                    const bool isShop = (sc.kind == SpaceKind::Garage);
                    MeshBuf&    wallDst = isShop ? lnsWall : room;
                    const float wallUvm = isShop ? (1.0f / 1.6f) : 0.0f;

                    // Floor + ceiling: never shared (a stair carries level change),
                    // so these are always solid.
                    PA(fa, li, fy, A); PA(fa, lo, fy, B); PA(fb, lo, fy, C); PA(fb, li, fy, D);
                    if (isShop) {
                        // Metric UVs (u = lateral metres, w = station metres,
                        // both * 1/8) so the checker tiles land 1 m square.
                        const float fu = 1.0f / 8.0f;
                        if (sc.side > 0) lnsFloor.quad(A, B, C, D, nU, li * fu, lo * fu,
                                                       sc.s0 * fu, sc.s1 * fu);
                        else             lnsFloor.quad(D, C, B, A, nU, li * fu, lo * fu,
                                                       sc.s1 * fu, sc.s0 * fu);
                    } else if (sc.side > 0) room.quad(A, B, C, D, nU, 0, 1, 0, 1);
                    else                    room.quad(D, C, B, A, nU, 0, 1, 0, 1);
                    // The ceiling deck stays bore concrete even in the shop —
                    // the club's deck is a plain slab too; what makes it an
                    // INDUSTRIAL ceiling is the steel slung under it (below).
                    PA(fa, li, cy, A); PA(fa, lo, cy, B); PA(fb, lo, cy, C); PA(fb, li, cy, D);
                    if (sc.side > 0) room.quad(D, C, B, A, nD, 0, 1, 0, 1);
                    else             room.quad(A, B, C, D, nD, 0, 1, 0, 1);

                    // ---- End walls (at s0 and s1). In-plane axis is LATERAL.
                    for (int e = 0; e < 2; ++e) {
                        const bool high = (e == 1);
                        const Frame& fe = high ? fb : fa;
                        float u0 = 0, u1 = 0, y0 = 0, y1 = 0;
                        const bool hole = openingOn(false, high, u0, u1, y0, y1);
                        const float n[3] = { high ? -route.dirX : route.dirX, 0.0f,
                                             high ? -route.dirZ : route.dirZ };
                        const float lmin = std::min(sc.latIn, sc.latOut);
                        const float lmax = std::max(sc.latIn, sc.latOut);
                        faceWithHole(wallDst, wallUvm, lmin, lmax, fy, cy,
                                     hole ? u0 : 1e9f, hole ? u1 : -1e9f, y0, y1,
                                     n, high != (sc.side > 0),
                                     [&](float u, float y, float* out) { PA(fe, sgn * u, y, out); });
                    }

                    // ---- Side walls (at latIn and latOut). In-plane axis is STATION.
                    for (int e = 0; e < 2; ++e) {
                        const bool outer = (e == 1);
                        const float lat = outer ? lo : li;
                        float u0 = 0, u1 = 0, y0 = 0, y1 = 0;
                        bool hole = openingOn(true, outer, u0, u1, y0, y1);
                        // The entry stub's INNER face is the doorway onto the
                        // bore: the one opening not shared with another space.
                        if (!outer && sc.kind == SpaceKind::EntryStub) {
                            hole = true;
                            u0 = sc.s0; u1 = sc.s1; y0 = fy; y1 = cy;
                        }
                        const float n[3] = { (outer ? -sgn : sgn) * right[0], 0.0f,
                                             (outer ? -sgn : sgn) * right[2] };
                        faceWithHole(wallDst, wallUvm, sc.s0, sc.s1, fy, cy,
                                     hole ? u0 : 1e9f, hole ? u1 : -1e9f, y0, y1,
                                     n, outer != (sc.side > 0),
                                     [&](float u, float y, float* out) {
                                         PA(frameAtS(u), lat, y, out);
                                     });
                    }

                    // ---- WHAT MAKES THE ROOM A ROOM. Tim: the rooms "are not
                    // empty volume -- they have a purpose and something to
                    // interact with." An empty concrete box behind a door is
                    // worse than no door, because it promises and then reveals
                    // nothing. Each role gets the hardware its NAME implies.
                    const float mid = (sc.s0 + sc.s1) * 0.5f;
                    const Frame& fm = frameAtS(mid);
                    const float latMid = sgn * (sc.latIn + sc.latOut) * 0.5f;
                    const float ax[3] = { route.dirX, 0.0f, route.dirZ };
                    if (sc.kind == SpaceKind::PlantRoom) {
                        // Two pump skids + a vent trunk. Drainage goes to the low
                        // point, so this is the room that has a reason to be here.
                        for (int k = -1; k <= 1; k += 2) {
                            float c[3]; PA(fm, latMid + (float)k * 1.3f, fy + 0.55f, c);
                            obox(fixture, c, right, ax, 0.55f, 0.55f, 0.9f, 1.0f);
                        }
                        float t[3]; PA(fm, sgn * (sc.latOut - 0.5f), cy - 0.45f, t);
                        obox(fixture, t, right, ax, 0.4f, 0.4f, (sc.s1 - sc.s0) * 0.42f, 1.0f);
                    } else if (sc.kind == SpaceKind::SignalRoom) {
                        // A rank of relay cabinets against the far wall, with the
                        // status lamps that make a dark room read as LIVE.
                        for (int k = 0; k < 4; ++k) {
                            float c[3];
                            PA(fm, sgn * (sc.latOut - 0.45f), fy + 0.95f, c);
                            c[0] += route.dirX * ((float)k - 1.5f) * 1.05f;
                            c[2] += route.dirZ * ((float)k - 1.5f) * 1.05f;
                            obox(fixture, c, right, ax, 0.35f, 0.95f, 0.45f, 1.0f);
                            float g[3] = { c[0], c[1] + 0.62f, c[2] };
                            obox(fglow, g, right, ax, 0.30f, 0.05f, 0.06f, 1.0f);
                        }
                    } else if (sc.kind == SpaceKind::Garage) {
                        // ---- THE VEHICLE BAY ---------------------------------
                        // Two rows of bays nose-in with a drive aisle between,
                        // two lifts, and a bench run down the far wall. The
                        // PARKED CARS are drawn by the host (it owns the model
                        // loader); what lives here is the room's own hardware,
                        // and the bay markings that make the empty floor read as
                        // a garage rather than as a big empty room.
                        const float gLen = sc.s1 - sc.s0, gDep = sc.latOut - sc.latIn;
                        const float aisle = fy + 0.005f;
                        // ---- WHEEL SPOTTING PLATES (Rotary SPOA10, ASYMMETRIC).
                        // The bay outlines that used to be here are gone: Tim
                        // does not paint bays, he spots wheels. A working shop
                        // marks where the CAR goes relative to the LIFT, not
                        // where a parking stall is -- the floor of a real bay is
                        // bare between the plates.
                        //
                        // ASYMMETRIC is the whole reason the plates sit where
                        // they do. An SPOA10's columns are rotated ~30 deg and
                        // its arms are unequal -- SHORT arms forward, LONG arms
                        // aft -- so the car is spotted BACK, roughly a third of
                        // its length behind the column line. That is what puts
                        // the centre of gravity between the arms and lets the
                        // doors open past the columns, which is the entire point
                        // of buying asymmetric. Spot it centred like a symmetric
                        // lift and you have paid for a feature you then cancelled.
                        //
                        // One yellow plate per side per lift, at the FRONT wheels.
                        for (uint32_t L = 0; L < kTrGarageTwoPost; ++L) {
                            const float ls = sc.s0 + gLen * (0.14f + 0.16f * (float)L);
                            const float ll = sc.latIn + gDep * 0.34f;
                            // Forward of the column line by the asymmetric offset.
                            const float spotAhead = 1.15f;      // 3.8 ft
                            for (int side3 = -1; side3 <= 1; side3 += 2) {
                                float pl[3];
                                PA(frameAtS(ls + spotAhead), sgn * (ll + (float)side3 * 0.95f),
                                   fy + 0.006f, pl);
                                obox(baypaint, pl, right, ax, 0.24f, 0.003f, 0.30f, 1.0f);
                            }
                        }

                        // ---- TWO-POST LIFTS (Rotary-style, 10,000 lb). Two
                        // columns either side of the car with swing arms that
                        // reach UNDER it to the frame. The wheels hang free --
                        // that is what a two-post is FOR, and it is why the bay
                        // has to be 15 ft clear: 11 ft of post plus a car on top
                        // of it does not fit under the control room's 8.5.
                        for (uint32_t L = 0; L < kTrGarageTwoPost; ++L) {
                            // Three bays down the long wall on ~14 ft centres --
                            // a two-post needs its neighbour far enough away that
                            // two doors can be open at once.
                            const float ls = sc.s0 + gLen * (0.14f + 0.16f * (float)L);
                            const float ll = sc.latIn + gDep * 0.34f;
                            for (int side2 = -1; side2 <= 1; side2 += 2) {
                                float post[3];
                                PA(frameAtS(ls), sgn * (ll + (float)side2 * 1.45f),
                                   fy + kTrLiftPostHM * 0.5f, post);
                                // Columns + beam wear ROTARY RED (club1127's
                                // kLiftRed shop-equipment red, via the lnsEquip
                                // material below) — same geometry, same count;
                                // this is paint, not inventory.
                                obox(lnsEquip, post, right, ax, 0.15f, kTrLiftPostHM * 0.5f, 0.15f, 1.0f);
                                // The overhead beam that ties the columns is the
                                // silhouette people actually recognise a lift by.
                                if (side2 < 0) {
                                    float beam[3];
                                    PA(frameAtS(ls), sgn * ll, fy + kTrLiftPostHM, beam);
                                    obox(lnsEquip, beam, right, ax, 1.60f, 0.10f, 0.13f, 1.0f);
                                }
                                // Swing arms, parked low and reaching inward.
                                for (int arm2 = -1; arm2 <= 1; arm2 += 2) {
                                    float arm[3];
                                    PA(frameAtS(ls + (float)arm2 * 0.85f),
                                       sgn * (ll + (float)side2 * 0.80f), fy + kTrLiftArmHM, arm);
                                    obox(fixture, arm, right, ax, 0.55f, 0.06f, 0.09f, 1.0f);
                                }
                            }
                        }

                        // ---- HUNTER-STYLE ALIGNMENT RACK. A DRIVE-ON runway,
                        // not a lift that grabs the frame: alignment is measured
                        // with the car's weight on its own wheels, so the deck
                        // stays under the tyres and the front pads are
                        // TURNPLATES that let the wheels swivel while you set
                        // toe. Modelling it as a third two-post would have been
                        // the easy thing and would have said the shop cannot do
                        // alignment.
                        {
                            const float rs2 = sc.s0 + gLen * 0.84f;
                            const float rl  = sc.latIn + gDep * 0.42f;
                            for (int run = -1; run <= 1; run += 2) {
                                float deck[3];
                                PA(frameAtS(rs2), sgn * (rl + (float)run * 0.95f),
                                   fy + kTrRackHM, deck);
                                obox(fixture, deck, right, ax,
                                     kTrRackWideM * 0.5f, 0.09f, kTrRackLenM * 0.5f, 1.0f);
                                // Approach ramp at the back of each runway.
                                float ramp[3];
                                PA(frameAtS(rs2 - kTrRackLenM * 0.5f - 0.9f),
                                   sgn * (rl + (float)run * 0.95f), fy + kTrRackHM * 0.5f, ramp);
                                obox(fixture, ramp, right, ax,
                                     kTrRackWideM * 0.5f, kTrRackHM * 0.5f, 0.9f, 1.0f);
                                // TURNPLATE at the front of each runway.
                                float plate[3];
                                PA(frameAtS(rs2 + kTrRackLenM * 0.30f),
                                   sgn * (rl + (float)run * 0.95f), fy + kTrRackHM + 0.10f, plate);
                                obox(fglow, plate, right, ax, 0.26f, 0.02f, 0.26f, 1.0f);
                                // The four support legs.
                                for (int lg = -1; lg <= 1; lg += 2) {
                                    float leg[3];
                                    PA(frameAtS(rs2 + (float)lg * kTrRackLenM * 0.38f),
                                       sgn * (rl + (float)run * 0.95f), fy + kTrRackHM * 0.5f, leg);
                                    obox(fixture, leg, right, ax, 0.10f, kTrRackHM * 0.5f, 0.10f, 1.0f);
                                }
                            }
                        }
                        // ---- THE TIRE BAY. Road Force balancer and the tire
                        // machine, SIDE BY SIDE, because that is the order the
                        // work happens in: the tire comes off the changer and
                        // goes straight onto the balancer. Splitting them across
                        // the room is the tell that nobody who has done the job
                        // laid it out.
                        {
                            const float ts = sc.s0 + gLen * 0.66f;
                            const float tl = sc.latIn + gDep * 0.80f;
                            // Road Force balancer: base, shaft, and the hood that
                            // swings down over the wheel.
                            float bal[3];
                            PA(frameAtS(ts), sgn * tl, fy + kTrBalancerHM * 0.45f, bal);
                            obox(fixture, bal, right, ax, 0.42f, kTrBalancerHM * 0.45f, 0.70f, 1.0f);
                            float hood[3];
                            PA(frameAtS(ts + 0.30f), sgn * tl, fy + kTrBalancerHM, hood);
                            obox(fixture, hood, right, ax, 0.46f, 0.10f, 0.52f, 1.0f);
                            // The read-out head is the bit you actually stand at.
                            float head[3];
                            PA(frameAtS(ts - 0.55f), sgn * tl, fy + 1.30f, head);
                            obox(fglow, head, right, ax, 0.26f, 0.20f, 0.06f, 1.0f);

                            // Tire machine: turntable low, tower up the back.
                            const float ms = ts + 2.4f;
                            float tt[3];
                            PA(frameAtS(ms), sgn * tl, fy + 0.30f, tt);
                            obox(fixture, tt, right, ax, 0.55f, 0.30f, 0.55f, 1.0f);
                            float tower[3];
                            PA(frameAtS(ms + 0.55f), sgn * (tl + 0.15f), fy + kTrTireMachHM * 0.5f, tower);
                            obox(fixture, tower, right, ax, 0.14f, kTrTireMachHM * 0.5f, 0.14f, 1.0f);
                            float armT[3];
                            PA(frameAtS(ms + 0.20f), sgn * (tl + 0.15f), fy + kTrTireMachHM, armT);
                            obox(fixture, armT, right, ax, 0.12f, 0.09f, 0.48f, 1.0f);
                        }

                        // ---- THE MILLER, on its cart, with the bottle strapped
                        // to it. A welder is not a box in the corner: it is a
                        // cart you drag to the car, which is why it sits in the
                        // open floor rather than against a wall.
                        {
                            const float ws2 = sc.s0 + gLen * 0.50f;
                            const float wl  = sc.latIn + gDep * 0.66f;
                            float cart[3];
                            PA(frameAtS(ws2), sgn * wl, fy + kTrWelderHM * 0.5f, cart);
                            obox(fixture, cart, right, ax, 0.34f, kTrWelderHM * 0.5f, 0.48f, 1.0f);
                            float bottle[3];
                            PA(frameAtS(ws2 - 0.42f), sgn * wl, fy + kTrBottleHM * 0.5f, bottle);
                            obox(fixture, bottle, right, ax, 0.13f, kTrBottleHM * 0.5f, 0.13f, 1.0f);
                        }

                        // ---- THE ROBINAIR A/C MACHINE. A wheeled recovery cart
                        // with the twin manifold gauges on its face -- the pair
                        // of dials is the entire silhouette people recognise it
                        // by, so they are modelled and lit rather than implied.
                        // Parked with the welder because both are machines you
                        // ROLL to the car, not fixtures you take the car to.
                        {
                            const float as2 = sc.s0 + gLen * 0.50f;
                            const float al  = sc.latIn + gDep * 0.78f;
                            float body[3];
                            PA(frameAtS(as2), sgn * al, fy + 0.55f, body);
                            obox(fixture, body, right, ax, 0.30f, 0.55f, 0.40f, 1.0f);
                            float mast[3];
                            PA(frameAtS(as2), sgn * al, fy + 1.05f, mast);
                            obox(fixture, mast, right, ax, 0.26f, 0.16f, 0.34f, 1.0f);
                            for (int g = -1; g <= 1; g += 2) {   // the twin gauges
                                float gg[3];
                                PA(frameAtS(as2 + (float)g * 0.12f), sgn * (al - 0.34f), fy + 1.10f, gg);
                                obox(fglow, gg, right, ax, 0.075f, 0.075f, 0.02f, 1.0f);
                            }
                        }

                        // BENCH RUN + toolboxes along the far wall. A workshop is
                        // a wall you can put things down on; without it the bay
                        // is a car park with a lift in it.
                        {
                            float bench[3];
                            PA(frameAtS((sc.s0 + sc.s1) * 0.5f), sgn * (sc.latOut - 0.45f), fy + 0.45f, bench);
                            obox(fixture, bench, right, ax, 0.42f, 0.45f, gLen * 0.40f, 1.0f);
                            for (int t = 0; t < 3; ++t) {
                                float tb[3];
                                PA(frameAtS(sc.s0 + gLen * (0.22f + 0.28f * (float)t)),
                                   sgn * (sc.latOut - 0.5f), fy + 1.28f, tb);
                                obox(fixture, tb, right, ax, 0.34f, 0.38f, 0.55f, 1.0f);
                            }
                        }

                        // EXACT station placement for the LNS dressing. The
                        // room kit places via frameAtS(), which SNAPS to the
                        // 4 m road-frame grid — fine for a pump skid, FATAL for
                        // layered sign work: the neon quad's 4 in wall standoff
                        // snapped to ZERO and the quad sat co-planar inside the
                        // end wall, winning or losing the depth test by capture
                        // mode (THAT was the "vanishing sign" — not a culler,
                        // my own quantized placement); the 3 m truss spacing
                        // snapped to the 4 m grid and stacked trusses onto each
                        // other. This helper offsets from the snapped frame
                        // along the route axis by the exact remainder.
                        auto PAx = [&](float sE, float r, float absY, float out[3]) {
                            const Frame& f2 = frameAtS(sE);
                            out[0] = f2.p[0] + ax[0] * (sE - f2.s) + right[0] * r;
                            out[1] = absY;
                            out[2] = f2.p[2] + ax[2] * (sE - f2.s) + right[2] * r;
                        };

                        // ---- THE INDUSTRIAL CEILING (club1127's "working-shop
                        // bones", re-set to this room's frame). Steel trusses on
                        // 3 m centres spanning the 43 ft depth, purlins tying
                        // the top chords, EMT conduit slung along both long
                        // walls, and one galvanised HVAC trunk with a crossing
                        // branch — the unmistakable shop-ceiling silhouette.
                        // All above the 11 ft lift posts (chords at 22-23 ft),
                        // decorative (the deck slab carries the collision).
                        {
                            const float yBot = fy + 6.72f, yTop = fy + 7.10f;   // 22.0 / 23.3 ft
                            const float yMid = (yBot + yTop) * 0.5f;
                            const float latMidRaw = sc.latIn + gDep * 0.5f;
                            const float dHalf = gDep * 0.5f - 0.30f;
                            for (float ts2 = sc.s0 + 1.74f; ts2 < sc.s1 - 1.0f; ts2 += 3.0f) {
                                float c[3];
                                PAx(ts2, sgn * latMidRaw, yBot, c);
                                obox(lnsSteel, c, right, ax, dHalf, 0.05f, 0.05f, 1.0f);   // bottom chord
                                PAx(ts2, sgn * latMidRaw, yTop, c);
                                obox(lnsSteel, c, right, ax, dHalf, 0.05f, 0.05f, 1.0f);   // top chord
                                for (int w2 = 0; w2 < 7; ++w2) {                            // Vierendeel webs
                                    const float wl2 = sc.latIn + 0.65f
                                                    + (float)w2 * (gDep - 1.3f) / 6.0f;
                                    PAx(ts2, sgn * wl2, yMid, c);
                                    obox(lnsSteel, c, right, ax, 0.035f, (yTop - yBot) * 0.5f, 0.035f, 1.0f);
                                }
                            }
                            // Purlins (four lines, running the bay's length).
                            for (int pn = 0; pn < 4; ++pn) {
                                const float pl2 = sc.latIn + gDep * (0.16f + 0.2266f * (float)pn);
                                float c[3];
                                PAx((sc.s0 + sc.s1) * 0.5f, sgn * pl2, yTop + 0.06f, c);
                                obox(lnsSteel, c, right, ax, 0.03f, 0.03f, gLen * 0.5f - 0.3f, 1.0f);
                            }
                            // EMT conduit, doubled runs under the chords along both long walls.
                            for (int cw = 0; cw < 2; ++cw) {
                                const float cl2 = cw ? (sc.latOut - 0.22f) : (sc.latIn + 0.22f);
                                float c[3];
                                PAx((sc.s0 + sc.s1) * 0.5f, sgn * cl2, yBot - 0.12f, c);
                                obox(lnsCond, c, right, ax, 0.028f, 0.028f, gLen * 0.5f - 0.6f, 1.0f);
                                PAx((sc.s0 + sc.s1) * 0.5f, sgn * cl2, yBot - 0.20f, c);
                                obox(lnsCond, c, right, ax, 0.022f, 0.022f, gLen * 0.5f - 0.6f, 1.0f);
                            }
                            // The HVAC trunk along the back wall + one branch
                            // crossing to the bore side, with the elbow boxed.
                            {
                                float c[3];
                                PAx((sc.s0 + sc.s1) * 0.5f, sgn * (sc.latOut - 0.75f), fy + 6.35f, c);
                                obox(lnsDuct, c, right, ax, 0.24f, 0.26f, gLen * 0.5f - 1.8f, 1.0f);
                                const float bs2 = sc.s0 + gLen * 0.30f;
                                PAx(bs2, sgn * latMidRaw, fy + 6.35f, c);
                                obox(lnsDuct, c, right, ax, dHalf - 0.9f, 0.24f, 0.22f, 1.0f);
                                PAx(bs2, sgn * (sc.latOut - 0.75f), fy + 6.35f, c);
                                obox(lnsDuct, c, right, ax, 0.30f, 0.30f, 0.30f, 1.0f);
                            }
                        }

                        // ---- WORKLIGHTS, not gels. Three caged shop lamps on
                        // drop wires over the work line (the club hangs ONE of
                        // these over its hoist; a 100 ft working bay earns
                        // three), each backed by a REAL point light so the
                        // checkerboard has something to pool. The pool uploader
                        // ranks lights by camera distance, so these cost the
                        // bore nothing while you are driving it.
                        for (int wl3 = 0; wl3 < 3; ++wl3) {
                            const float ws3 = sc.s0 + gLen * (0.22f + 0.28f * (float)wl3);
                            const float ll3 = sc.latIn + gDep * 0.45f;
                            float c[3];
                            PAx(ws3, sgn * ll3, fy + 5.85f, c);
                            obox(lnsLamp, c, right, ax, 0.09f, 0.06f, 0.09f, 1.0f);   // lamp head
                            PAx(ws3, sgn * ll3, fy + 6.30f, c);
                            obox(lnsCond, c, right, ax, 0.012f, 0.42f, 0.012f, 1.0f); // drop wire
                            x3::rhi::PointLight pl{};
                            PAx(ws3, sgn * ll3, fy + 5.35f, c);
                            pl.pos[0] = c[0]; pl.pos[1] = c[1]; pl.pos[2] = c[2];
                            pl.range = 16.0f;
                            // Warm shop key — club1127's hoist worklight hue.
                            pl.color[0] = 2.1f; pl.color[1] = 1.62f; pl.color[2] = 0.96f;
                            m_lights.push_back(pl);
                        }

                        // Which end you DRIVE IN at is read off the RAMP space
                        // (it abuts one end of the bay), not assumed from `dir`
                        // — the sign and the camera below both hang off it.
                        bool entryAtS0 = true;
                        for (const TunnelSpace& o : sp)
                            if (o.kind == SpaceKind::Ramp && o.side == sc.side)
                                entryAtS0 = (std::fabs(o.s1 - sc.s0) < std::fabs(o.s0 - sc.s1));

                        // ---- ★ THE NEON. "LATE NIGHT SPEED" across the far
                        // end wall — the wall that faces you for the whole 87 ft
                        // of ramp. Built as CHANNEL LETTERS: one small emissive
                        // block per lit glyph cell, rasterized from the SAME 5x7
                        // font the club's sign bakes (lns::makeSignRGBA at
                        // 1 px per cell), mounted on a dark cabinet 20 in off
                        // the block with standoff struts — the way a real sign
                        // hangs. GEOMETRY, not a texture, for a measured reason:
                        // the first cut was a quad with a per-texel emissive
                        // map, and that quad rendered in `--screenshot` runs but
                        // VANISHED in `--screenshot-tunnel` runs — same world,
                        // pixel-diff-identical camera (the sole emissiveTex
                        // entity in this host loses some per-run renderer coin
                        // toss the club never hits). Channel letters ride the
                        // same flat-emissive path as the lighting strips and
                        // bay markings, which render in every mode; and the
                        // blocky cell build is what a fat neon channel letter
                        // actually looks like from across a bay.
                        {
                            const float sFar  = entryAtS0 ? sc.s1 - 0.50f : sc.s0 + 0.50f;
                            const float hw2 = 5.5f;                            // 36 ft of letters on the 43 ft wall
                            const float cyS = fy + 4.6f;                       // 15 ft up the 24 ft wall
                            const float lC  = sgn * (sc.latIn + gDep * 0.5f);  // panel centre, signed lat
                            // Text runs the VIEWER's left-to-right. Facing the
                            // far wall the viewer faces +axis when the entry is
                            // at s0, so their screen-right is +right-of-travel
                            // (= increasing SIGNED lat); entry at s1 flips it.
                            const float eDir = entryAtS0 ? 1.0f : -1.0f;
                            // Rasterize the line at 1 px per font cell (16
                            // chars x 6 cells = 96 x 7) and emit a block per
                            // LIT core cell (halo texels stay off — the glow
                            // pass adds the halo in-shader via bloom).
                            const char* kSignLine = "LATE NIGHT SPEED";
                            const uint32_t sw2 = 96, sh2 = 7;
                            auto sgPx = lns::makeSignRGBA(sw2, sh2, kSignLine,
                                                          lns::kNeonRed[0], lns::kNeonRed[1], lns::kNeonRed[2]);
                            const float cell = 2.0f * hw2 / (float)sw2;        // 0.11 m — letters 2.6 ft tall
                            const float yTop2 = cyS + cell * (float)sh2 * 0.5f;
                            for (uint32_t iy = 0; iy < sh2; ++iy)
                                for (uint32_t ix = 0; ix < sw2; ++ix) {
                                    if (sgPx[((size_t)iy * sw2 + ix) * 4 + 0] < 200) continue;   // core cells only
                                    const float lat2 = lC + eDir * (-hw2 + cell * ((float)ix + 0.5f));
                                    const float y2   = yTop2 - cell * ((float)iy + 0.5f);
                                    float c[3];
                                    PAx(sFar, lat2, y2, c);
                                    obox(lnsNeon, c, right, ax, cell * 0.46f, cell * 0.46f, 0.045f, 1.0f);
                                }
                            // The CABINET the letters mount on, and the two
                            // standoff struts back to the block.
                            {
                                const float toWall = entryAtS0 ? 1.0f : -1.0f;   // deeper s = the wall side here
                                float c[3];
                                PAx(sFar + toWall * 0.14f, lC, cyS, c);
                                obox(lnsCab, c, right, ax, hw2 + 0.25f, cell * 4.5f, 0.07f, 1.0f);
                                for (int st2 = -1; st2 <= 1; st2 += 2) {
                                    PAx(sFar + toWall * 0.30f,
                                       lC + (float)st2 * hw2 * 0.7f, cyS, c);
                                    obox(lnsCond, c, right, ax, 0.03f, 0.03f, 0.16f, 1.0f);
                                }
                            }
                            // A red wash under the sign so the letters answer in
                            // the floor gloss, the way neon actually behaves.
                            x3::rhi::PointLight pl{};
                            float c[3];
                            PAx(sFar + (entryAtS0 ? -1.2f : 1.2f), lC, cyS - 0.6f, c);
                            pl.pos[0] = c[0]; pl.pos[1] = c[1]; pl.pos[2] = c[2];
                            pl.range = 10.0f;
                            pl.color[0] = 1.6f; pl.color[1] = 0.16f; pl.color[2] = 0.16f;
                            m_lights.push_back(pl);
                        }

                        // ---- The proof shot's camera: just inside the entry
                        // end, off the aisle, eye height, looking down the bay
                        // at the lifts / checker / neon. Stored for
                        // showcaseCamera(8); headless captures use it.
                        {
                            const float sEye = entryAtS0 ? sc.s0 + 3.0f : sc.s1 - 3.0f;
                            const float sTgt = entryAtS0 ? sc.s1 - 2.0f : sc.s0 + 2.0f;
                            float eye[3], tgt[3];
                            PAx(sEye, sgn * (sc.latIn + gDep * 0.24f), fy + 1.75f, eye);
                            PAx(sTgt, sgn * (sc.latIn + gDep * 0.52f), fy + 3.4f, tgt);
                            const float dx2 = tgt[0] - eye[0], dy2 = tgt[1] - eye[1], dz2 = tgt[2] - eye[2];
                            const float hl2 = std::sqrt(dx2 * dx2 + dz2 * dz2);
                            m_garageCam[0] = eye[0]; m_garageCam[1] = eye[1]; m_garageCam[2] = eye[2];
                            m_garageCam[3] = std::atan2(dz2, dx2);
                            m_garageCam[4] = std::atan2(dy2, std::max(0.01f, hl2));
                            m_garageCamValid = true;
                        }
                    } else if (sc.kind == SpaceKind::ControlRoom) {
                        // THE COMMAND CONSOLE -- a desk with a lit face. This is
                        // the thing the whole chain of door -> hall -> room exists
                        // to arrive at.
                        float c[3]; PA(fm, latMid, fy + 0.45f, c);
                        obox(fixture, c, right, ax, 0.7f, 0.45f, 1.6f, 1.0f);
                        float scr[3]; PA(fm, latMid, fy + 1.35f, scr);
                        obox(fglow, scr, right, ax, 0.06f, 0.42f, 1.45f, 1.0f);
                    }
                }

                // ---- KEYPADS at the doors that open. A denied door gets none,
                // which is the honest tell: no keypad means no way in, rather
                // than a keypad that silently refuses every code.
                for (const RoomDoor& d : prog.doors()) {
                    if (!d.hasProgram) continue;
                    const Frame& fd = frameAtS(d.s);
                    const float sgn = (float)d.side;
                    float k[3]; PA(fd, sgn * (kTcTubeHalfWidth - 0.10f),
                                   fd.p[1] + kTcWalkKerbH + 1.35f, k);
                    k[0] += route.dirX * 0.85f; k[2] += route.dirZ * 0.85f;
                    const float ax2[3] = { route.dirX, 0.0f, route.dirZ };
                    obox(fixture, k, right, ax2, 0.05f, 0.14f, 0.10f, 1.0f);
                    float kg[3] = { k[0], k[1], k[2] };
                    obox(fglow, kg, right, ax2, 0.055f, 0.09f, 0.065f, 1.0f);
                }

                Material rmm; if (boreSet.ok) { rmm.alb = boreSet.albedo; rmm.mr = boreSet.mr; rmm.nrm = boreSet.normal; }
                              else { rmm.alb = concreteTex; rmm.mr = roughMR; }
                for (int c = 0; c < 3; ++c) rmm.tint[c] = 0.74f;   // service spaces are dimmer than the bore
                upload(room, rmm, /*collide*/true);

                Material fm2; fm2.alb = concreteTex; fm2.mr = roughMR;
                fm2.tint[0] = 0.34f; fm2.tint[1] = 0.36f; fm2.tint[2] = 0.40f;
                upload(fixture, fm2, /*collide*/true);

                Material fg; fg.alb = paintTex; fg.mr = roughMR;
                fg.emissive[0] = 0.35f; fg.emissive[1] = 1.0f; fg.emissive[2] = 0.65f; fg.emissive[3] = 1.6f;
                upload(fglow, fg, /*collide*/false);

                Material bp; bp.alb = paintTex; bp.mr = roughMR;
                // SAFETY YELLOW. Spotting plates are the one thing on a shop
                // floor that is allowed to shout -- you are meant to find them
                // through a windscreen while creeping forward.
                bp.tint[0] = 1.00f; bp.tint[1] = 0.76f; bp.tint[2] = 0.06f;
                upload(baypaint, bp, /*collide*/false);

                // ---- THE LNS MATERIALS (only built when the bay exists —
                // Tier B/C programs leave every lns* buffer empty and upload()
                // skips empties, so other bores pay nothing).
                if (!lnsWall.empty() || !lnsFloor.empty()) {
                    // Painted CMU block: the shared kit's albedo + mortar-groove
                    // normal + matte MR. Tinted LIGHTER than the club's 0.36
                    // venue grey on purpose: same paint, different hour — the
                    // club dims its walls for the night trade, a working bay
                    // runs them at shop brightness under the worklights.
                    Material wm2;
                    wm2.alb = tex(lns::makeCmuBlockRGBA(256), 256, true);
                    wm2.nrm = tex(lns::makeCmuNormalRGBA(256), 256, false);
                    {
                        auto mrPx = lns::makeMr1x1(lns::kCmuRoughPx, lns::kCmuMetalPx);
                        x3::rhi::TextureHandle t = device.createTexture(mrPx.data(), 1, 1, false);
                        if (t.valid()) m_textures.push_back(t);
                        wm2.mr = t;
                    }
                    wm2.tint[0] = 0.62f; wm2.tint[1] = 0.62f; wm2.tint[2] = 0.68f;
                    upload(lnsWall, wm2, /*collide*/true);

                    // The glossy checkerboard, baked from the SAME two albedo
                    // numbers the club's tiles read (lns::kCheckerBright/Dark,
                    // linear -> srgb=false) over the SAME wet-mirror MR (8/255)
                    // — so the worklights and the neon POOL in the floor,
                    // "exactly like the photos". No beat-pulsed under-glow down
                    // here: that is the club's night layer, not the shop's.
                    Material fm3;
                    fm3.alb = tex(lns::makeCheckerFloorRGBA(512, 8), 512, false);
                    {
                        auto mrPx = lns::makeMr1x1(lns::kFloorRoughPx, lns::kFloorMetalPx);
                        x3::rhi::TextureHandle t = device.createTexture(mrPx.data(), 1, 1, false);
                        if (t.valid()) m_textures.push_back(t);
                        fm3.mr = t;
                    }
                    upload(lnsFloor, fm3, /*collide*/true);

                    // Ceiling bones — club1127's post-gamma self-read values,
                    // verbatim: enough emissive to lift the silhouette out of
                    // the dark, far too little to read as a lightbox.
                    Material sm2; sm2.alb = paintTex; sm2.mr = wallMR;
                    sm2.tint[0] = 0.085f; sm2.tint[1] = 0.088f; sm2.tint[2] = 0.100f;   // dark shop steel
                    sm2.emissive[0] = 0.130f; sm2.emissive[1] = 0.140f; sm2.emissive[2] = 0.170f; sm2.emissive[3] = 0.26f;
                    upload(lnsSteel, sm2, /*collide*/false);
                    Material dm2; dm2.alb = paintTex; dm2.mr = wallMR;
                    dm2.tint[0] = 0.300f; dm2.tint[1] = 0.310f; dm2.tint[2] = 0.340f;   // galvanised duct
                    dm2.emissive[0] = 0.170f; dm2.emissive[1] = 0.180f; dm2.emissive[2] = 0.200f; dm2.emissive[3] = 0.20f;
                    upload(lnsDuct, dm2, /*collide*/false);
                    Material cm2; cm2.alb = paintTex; cm2.mr = wallMR;
                    cm2.tint[0] = 0.110f; cm2.tint[1] = 0.095f; cm2.tint[2] = 0.075f;   // EMT conduit
                    cm2.emissive[0] = 0.115f; cm2.emissive[1] = 0.100f; cm2.emissive[2] = 0.078f; cm2.emissive[3] = 0.18f;
                    upload(lnsCond, cm2, /*collide*/false);

                    // Rotary red for the lift columns/beams (club1127's
                    // kLiftRed) + the warm caged worklight heads.
                    Material em2; em2.alb = paintTex; em2.mr = wallMR;
                    em2.tint[0] = 0.42f; em2.tint[1] = 0.045f; em2.tint[2] = 0.045f;
                    upload(lnsEquip, em2, /*collide*/true);
                    Material lm2; lm2.alb = paintTex; lm2.mr = wallMR;
                    lm2.emissive[0] = 1.0f; lm2.emissive[1] = 0.86f; lm2.emissive[2] = 0.55f; lm2.emissive[3] = 3.4f;
                    upload(lnsLamp, lm2, /*collide*/false);

                    // The sign: hot red channel-letter blocks (flat emissive —
                    // the path every strip and marking in this file already
                    // proves in every capture mode) over a near-black cabinet.
                    Material nm2; nm2.alb = paintTex; nm2.mr = wallMR;
                    nm2.tint[0] = 0.30f; nm2.tint[1] = 0.02f; nm2.tint[2] = 0.02f;
                    nm2.emissive[0] = lns::kNeonRed[0]; nm2.emissive[1] = lns::kNeonRed[1];
                    nm2.emissive[2] = lns::kNeonRed[2];
                    nm2.emissive[3] = 3.2f;   // neon-bright, same strength as the club mount
                    upload(lnsNeon, nm2, /*collide*/false);
                    Material cbm; cbm.alb = paintTex; cbm.mr = wallMR;
                    cbm.tint[0] = 0.022f; cbm.tint[1] = 0.020f; cbm.tint[2] = 0.026f;   // near-black cabinet
                    upload(lnsCab, cbm, /*collide*/false);
                    { char db[220]; std::snprintf(db, sizeof(db),
                        "tunnel garage: dressed as LATE NIGHT SPEED — CMU walls, glossy checker "
                        "floor, industrial ceiling, neon (%u letter cells; kit: app/lns_shop.h)",
                        (uint32_t)(lnsNeon.v.size() / 24));
                      x3::logInfo(db); }
                }

                char rb[240];
                uint32_t opened = 0;
                for (const RoomDoor& d : prog.doors()) if (d.hasProgram) ++opened;
                std::snprintf(rb, sizeof(rb),
                    "tunnel rooms: %u spaces behind %u of %u doors | worst cover %.0f ft",
                    (uint32_t)sp.size(), opened, (uint32_t)prog.doors().size(),
                    prog.worstRockCoverM() * 3.28084f);
                x3::logInfo(rb);
            }

            char fb[220];
            std::snprintf(fb, sizeof(fb),
                "tunnel fitout: %u lay-bys, %u signs, %u screens, %u doors, %u SOS | "
                "walkway %.1f ft deck, kerb %.1f ft, band to %.1f ft",
                (uint32_t)fit.layBys().size(), fit.countOf(FittingKind::Sign),
                fit.countOf(FittingKind::Screen), fit.countOf(FittingKind::Door),
                fit.countOf(FittingKind::SosNiche),
                kTcWalkDeckW * 3.28084f, kTcWalkKerbH * 3.28084f,
                (deckY + 2.35f) * 3.28084f);
            x3::logInfo(fb);
        }
    }

    // ================= 1b) THE SHOULDERS ===================================
    // The road slab was ending in mid-air over its own trench. The corridor
    // removes a CONSTANT depth, so its floor keeps the hillside's lateral tilt
    // and the downhill shoulder can sit several metres below the road datum;
    // the slab's skirt reached down into that but read as a black cliff hanging
    // off the carriageway (c8d28eda called this out and left it). A road does
    // not end at the white line, so:
    //   * OUTSIDE the roofed span the shoulder is a graded VERGE, swept from the
    //     slab edge out to the corridor floor's edge and never allowed to sink
    //     below the real ground, drawn with the TERRAIN SPLAT MARKER so it is
    //     the same grass/rock the cutting is made of.
    //   * INSIDE it is a flat concrete VERGE from the white line to the shell's
    //     springing, which is what a real bore has and which also closes the
    //     slab-to-shell gap the driver would otherwise see straight down.
    {
        MeshBuf verge, walk;
        const float lat0 = kTcRoadHalfWidth - 0.05f;      // just inside the slab edge
        const float latIn = kTcTubeHalfWidth;             // the shell's springing
        constexpr float kFillBatter = 1.5f;               // 1 down : 1.5 out, a road fill
        constexpr float kFillStep   = 0.9f;
        constexpr int   kFillMax    = 26;                 // ~23 m of reach, then give up
        auto qn = [](const float a[3], const float b[3], const float c[3], float out[3]) {
            const float e0[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            const float e1[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
            out[0] = e0[1]*e1[2] - e0[2]*e1[1];
            out[1] = e0[2]*e1[0] - e0[0]*e1[2];
            out[2] = e0[0]*e1[1] - e0[1]*e1[0];
            if (out[1] < 0.0f) { out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2]; }
            const float l = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
            if (l > 1e-5f) { out[0] /= l; out[1] /= l; out[2] /= l; }
        };
        auto ground = [&](float s, float lat) {
            float x = 0.0f, z = 0.0f; route.worldAt(s, lat, x, z);
            return terrainHeightAtWorld(x, z);
        };
        // Embankment surface at (s, lat): falls away from the slab edge at a road
        // fill batter and stops the moment it reaches the ground. On the uphill
        // shoulder the ground is already there and this is one thin quad; on the
        // downhill shoulder it reaches as far as it has to.
        auto fillY = [&](float s, float lat, float from) {
            const float ry = route.roadYAt(s) + 0.14f;
            return std::max(ry - (lat - from) / kFillBatter, ground(s, lat) + 0.05f);
        };
        const bool roofed = route.boreValid;
        for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
            const float sa = roadFrames[j].s, sb = roadFrames[j+1].s;
            const bool inBore = roofed && sa >= route.boreS0 - kTcCanopy
                                       && sb <= route.boreS1 + kTcCanopy;
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const float g = (float)sgn;
                auto P = [&](float s, float lat, float y, float out[3]) {
                    route.worldAt(s, g * lat, out[0], out[2]);
                    out[1] = y;
                };
                const float fillFrom = inBore ? latIn : lat0;
                if (inBore) {
                    const float ya = route.roadYAt(sa) + 0.14f;
                    const float yb = route.roadYAt(sb) + 0.14f;
                    float p0[3], p1[3], p2[3], p3[3], n[3];
                    P(sa, lat0,  ya, p0); P(sa, latIn, ya, p1);
                    P(sb, latIn, yb, p2); P(sb, lat0,  yb, p3);
                    qn(p0, p1, p2, n);
                    walk.quad(p0, p1, p2, p3, n, 0.0f, 1.0f, sa * 0.25f, sb * 0.25f);
                    // and drop its outer edge to the trench floor so nothing is
                    // ever seen under the verge from inside the bore
                    float q0[3], q1[3];
                    P(sa, latIn, ground(sa, latIn) - 0.4f, q0);
                    P(sb, latIn, ground(sb, latIn) - 0.4f, q1);
                    const float nf[3] = { g * right[0], 0.0f, g * right[2] };
                    walk.quad(p1, q0, q1, p2, nf, 0.0f, 1.0f, sa * 0.25f, sb * 0.25f);
                }
                {
                    // THE FILL EMBANKMENT. This is what the corridor cannot do
                    // for itself: the depression removes a CONSTANT depth, so on
                    // a side-slope its floor keeps the whole lateral tilt and the
                    // carriageway ends up on a shelf with a 10 m drop off one
                    // shoulder (d.png, pass 5 - and c8d28eda already called the
                    // symptom out as "the slab ends in mid-air"). A real road on a
                    // side-slope is FILLED out to a toe. So: fall away from the
                    // slab edge at a road batter and keep going until the ground
                    // is reached, however far that is.
                    // It runs on the roofed span too (starting outside the
                    // concrete verge, where it is buried under the backfill and
                    // costs only triangles): stopping it at the portal left an
                    // open wedge at the arch springing where the verge handed
                    // over to the embankment, and that wedge read as a black
                    // notch bitten out of the mouth.
                    for (int k = 0; k < kFillMax; ++k) {
                        const float l0 = fillFrom + kFillStep * (float)k;
                        const float l1 = l0 + kFillStep;
                        float p0[3], p1[3], p2[3], p3[3], n[3];
                        P(sa, l0, fillY(sa, l0, fillFrom), p0);
                        P(sa, l1, fillY(sa, l1, fillFrom), p1);
                        P(sb, l1, fillY(sb, l1, fillFrom), p2);
                        P(sb, l0, fillY(sb, l0, fillFrom), p3);
                        qn(p0, p1, p2, n);
                        // ...UNLESS a registered portal hole owns this patch.
                        // "Buried under the backfill and costs only triangles"
                        // (above) stopped being true the day the GARAGE moved in
                        // under the roofed span: this embankment follows ground()
                        // — the CARVED field — so over the bay it re-drew, WITH
                        // COLLISION, the exact rock band the tile mesher's hole
                        // had just dropped, as a grass wedge crossing the shop
                        // wall at truss height (found by capture 09_garage_lnss;
                        // survived a 20 m hole-margin experiment, which is what
                        // proved it was not the streamer's mesh). Same predicate,
                        // same registry, so a bore with no rooms is byte-identical.
                        const float cxq = (p0[0]+p1[0]+p2[0]+p3[0]) * 0.25f;
                        const float czq = (p0[2]+p1[2]+p2[2]+p3[2]) * 0.25f;
                        const float mYq = std::min(std::min(p0[1], p1[1]),
                                                   std::min(p2[1], p3[1]));
                        if (!terrainPortalHoleDrops(cxq, czq, mYq))
                            verge.quad(p0, p1, p2, p3, n, p0[0]*0.05f, p1[0]*0.05f,
                                       p0[2]*0.05f, p2[2]*0.05f);
                        if (fillY(sa, l1, fillFrom) <= ground(sa, l1) + 0.06f &&
                            fillY(sb, l1, fillFrom) <= ground(sb, l1) + 0.06f) break;
                    }
                }
            }
        }
        Material vm;
        if (groundTex.valid()) vm.alb = groundTex;   // the MARKER: terrain splat
        else if (roadSet.ok) { vm.alb = roadSet.albedo; vm.mr = roadSet.mr; vm.nrm = roadSet.normal; }
        else { vm.alb = asphaltTex; vm.mr = roughMR; }
        upload(verge, vm, /*collide*/true);
        Material km;
        if (portalSet.ok) {
            km.alb = portalSet.albedo; km.mr = portalSet.mr; km.nrm = portalSet.normal;
            const float v = portalSet.valueTint() * 0.86f;
            km.tint[0] = km.tint[1] = km.tint[2] = v;
        } else { km.alb = concreteTex; km.mr = wallMR; }
        upload(walk, km, /*collide*/true);
    }

    // ================= 2) THE TUNNEL SHELL ==================================
    if (route.boreValid) {
        const Profile inner = makeProfile(kTcTubeHalfWidth, kTcTubeWallH, kTcTubeCrownH);
        const Profile outer = offsetProfile(inner, kTcShellThick);
        // The tube runs the full ROOFED span plus a projecting CANOPY at each
        // end. In a real portal the arch ring stands proud of the headwall face,
        // and that few centimetres of relief is most of what makes a mouth read
        // as built rather than punched.
        const float s0 = std::max(0.0f, route.boreS0 - kTcCanopy);
        const float s1 = std::min(route.totalLen, route.boreS1 + kTcCanopy);
        std::vector<Frame> bf;
        for (float s = s0; s <= s1 + 0.01f; s += 3.0f) bf.push_back(frameAt(s));
        if (bf.back().s < s1 - 0.01f) bf.push_back(frameAt(s1));

        MeshBuf shell;
        // TEXEL DENSITY. 0.14 (one tile per 7.1 m) was set for the procedural
        // checker, where tile size is arbitrary. c8d28eda moved it to 0.30 (one
        // tile per 3.3 m) for the real 2K shotcrete set, reasoning the set's
        // largest crack would then draw at ~1.6 m. On the frames it still did not
        // read as shotcrete: at 1.6 m the crazing is BOULDER-sized and 300 m of
        // bore looks like a lava tube, which is what the first pass'
        // 07_inside_looking_out showed. 0.62 (one tile per 1.6 m) puts the same
        // crack at ~0.8 m. That STILL read as a repeating motif - fish scales
        // rather than concrete - and so did 1.05. The number was never the
        // problem: cv_shotcrete_break is a picture of BROKEN concrete and no
        // scale makes broken concrete read as a lining. With cc_cement_white the
        // surface has no motif to give away, so the density goes back to a sane
        // architectural 0.22 (one tile per 4.5 m) - low enough that the set's
        // fine detail stops aliasing into a moire on the walls at the grazing
        // angles a 330 m bore is nearly all made of.
        const float kBoreUV = boreSet.ok ? 0.22f : 0.14f;
        emitSweep(shell, bf, right, inner, -1.0f, kBoreUV);   // drivable bore surface
        emitSweep(shell, bf, right, outer, +1.0f, kBoreUV);   // buried outer skin
        // END-CAP ANNULUS at both mouths. Without it the sweep is a pair of
        // zero-thickness skins seen edge-on from outside and the arch has a
        // paper edge; with it the ring reads as a 0.9 m concrete section. It is
        // the cheapest detail on the whole portal and one of the loudest.
        {
            const float upv[3] = { 0.0f, 1.0f, 0.0f }; (void)upv;
            for (int e = 0; e < 2; ++e) {
                const Frame f = (e == 0) ? bf.front() : bf.back();
                const float n[3] = { (e == 0 ? -axis[0] : axis[0]), 0.0f,
                                     (e == 0 ? -axis[2] : axis[2]) };
                auto W = [&](const Profile& pr, int k, float out[3]) {
                    out[0] = f.p[0] + right[0] * pr.px[k];
                    out[1] = f.p[1] + pr.py[k];
                    out[2] = f.p[2] + right[2] * pr.px[k];
                };
                for (int k = 0; k + 1 < Profile::kN; ++k) {
                    float a[3], b2[3], c2[3], d2[3];
                    W(inner, k, a); W(inner, k + 1, b2); W(outer, k + 1, c2); W(outer, k, d2);
                    shell.quad(a, b2, c2, d2, n, 0.0f, 0.35f, 0.0f, 0.35f);
                }
            }
        }
        Material sm;
        if (boreSet.ok) {
            sm.alb = boreSet.albedo; sm.mr = boreSet.mr; sm.nrm = boreSet.normal;
            const float v = boreSet.valueTint() * 0.80f;   // pale lining, kept out of clip
            sm.tint[0] = sm.tint[1] = sm.tint[2] = v;
        } else { sm.alb = concreteTex; sm.mr = wallMR; }
        upload(shell, sm, /*collide*/true);

        // ================= 3) THE BACKFILL LID ==============================
        // The hillside over the tube, put back as geometry.
        //
        // This is the deliberate, LOCAL break of the single-value rule, and it
        // is the whole fix. The heightfield underneath is now a flat-floored
        // trench for the entire route (see deriveRoute step 3) so it can never
        // stand on the roadway again; this ONE swept mesh carries the overhang
        // the heightfield is not allowed to have. Everything that reads h(x,z)
        // — the streamer, the collision soup, the horizon ring, placeOnTerrain,
        // worldWaterLevelAt — is untouched and still sees a single-valued field.
        //
        // WHY IT IS INVISIBLE AS AN OBJECT: it is drawn with the terrain SPLAT
        // MARKER handle, so the renderer flags the draw as terrain and runs the
        // same height/slope grass/rock/snow/sand splat, on the same WORLD-SPACE
        // UVs, as the streamed tiles (vk_passes.cpp m_terrainMarkerId). It is
        // not a lookalike material — it is the same shading path.
        //
        // WHY THE SEAM IS EXACT: tunnelLidHeightAt() blends its raise out over
        // the corridor's own shoulder curve, so at halfWidth + falloff — where
        // terrainCorridorDelta() is EXACTLY 0.0f and the terrain is therefore
        // bit-identical to the natural field — the lid equals the terrain. Past
        // that it runs on as an apron sunk by kTcLidSink, so a streamed tile
        // meshing at 2 m or 4 m (Half/Quarter LOD) still wins the depth test
        // instead of z-fighting a coincident surface.
        //
        // COST: ~2 k quads, one draw, one static collision body. That is the
        // honest price of the overhang, and it is paid twice per world at most.
        {
            MeshBuf lid;
            const float WA = kTcCorridorHalfW + kTcCorridorFall + kTcLidApron;
            const int   NL = kTcLidLateral;
            std::vector<float> lats((size_t)NL);
            for (int k = 0; k < NL; ++k)
                lats[(size_t)k] = -WA + 2.0f * WA * (float)k / (float)(NL - 1);
            std::vector<float> ss;
            for (float s = route.boreS0; s <= route.boreS1 - 0.01f; s += kTcLidStep) ss.push_back(s);
            ss.push_back(route.boreS1);

            auto lidP = [&](float s, float lat, float out[3]) {
                float x = 0.0f, z = 0.0f;
                route.worldAt(s, lat, x, z);
                out[0] = x; out[1] = tunnelLidHeightAt(route, s, lat); out[2] = z;
            };
            const uint32_t lbase = (uint32_t)lid.v.size();
            for (size_t j = 0; j < ss.size(); ++j) {
                for (int k = 0; k < NL; ++k) {
                    float p[3]; lidP(ss[j], lats[(size_t)k], p);
                    // Normal from a central difference of the lid surface itself
                    // (not of the height field), so the lid's shading runs
                    // continuously into the terrain it is reconstructing — the
                    // splat keys off the world normal, and a mismatched normal
                    // is what would make the seam show as a band of rock.
                    const float e = 0.75f;
                    float a[3], b2[3], c2[3], d2[3];
                    lidP(ss[j] - e, lats[(size_t)k], a);
                    lidP(ss[j] + e, lats[(size_t)k], b2);
                    lidP(ss[j], lats[(size_t)k] - e, c2);
                    lidP(ss[j], lats[(size_t)k] + e, d2);
                    const float t1[3] = { b2[0]-a[0],  b2[1]-a[1],  b2[2]-a[2]  };
                    const float t2[3] = { d2[0]-c2[0], d2[1]-c2[1], d2[2]-c2[2] };
                    float n[3] = { t1[1]*t2[2] - t1[2]*t2[1],
                                   t1[2]*t2[0] - t1[0]*t2[2],
                                   t1[0]*t2[1] - t1[1]*t2[0] };
                    if (n[1] < 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                    const float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                    if (nl > 1e-5f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                    lid.vert(p, n, p[0] * 0.05f, p[2] * 0.05f);
                }
            }
            for (uint32_t j = 0; j + 1 < (uint32_t)ss.size(); ++j) {
                for (uint32_t k = 0; k + 1 < (uint32_t)NL; ++k) {
                    const uint32_t a  = lbase + j * (uint32_t)NL + k;
                    const uint32_t b2 = a + 1;
                    const uint32_t c2 = b2 + (uint32_t)NL;
                    const uint32_t d2 = a + (uint32_t)NL;
                    lid.i.push_back(a); lid.i.push_back(b2); lid.i.push_back(c2);
                    lid.i.push_back(a); lid.i.push_back(c2); lid.i.push_back(d2);
                }
            }
            Material lm;
            if (groundTex.valid()) {
                lm.alb = groundTex;   // the MARKER: shades through the terrain splat
            } else {
                x3::logWarn("tunnel corridor: no terrain splat marker passed to build() — "
                            "the backfill lid falls back to a flat earth tone and will read "
                            "as a tarpaulin over the hill");
                lm.alb = tex(x3::prims::makeCheckerRGBA(128, 32, 86, 98, 58, 74, 84, 50), 128, true);
                lm.mr  = roughMR;
            }
            upload(lid, lm, /*collide*/true);
            char lb[192];
            std::snprintf(lb, sizeof(lb),
                "tunnel corridor: backfill lid %.0f m x %.1f m, %u quads, splat=%s",
                route.boreS1 - route.boreS0, 2.0f * WA,
                (unsigned)((ss.size() - 1) * (size_t)(NL - 1)),
                groundTex.valid() ? "TERRAIN MARKER" : "fallback earth");
            x3::logInfo(lb);
        }

        // ================= 4) THE PORTALS ===================================
        // A real road-tunnel portal is three things at once, and the old build
        // only had the first:
        //   * a HEADWALL — a slab across the face with the bore cut through its
        //     spandrel, standing kTcPortalProud above the backfill it retains;
        //   * WINGWALLS — the same slab tapering out of the headwall and dying
        //     into the hillside, holding the cut face back on each flank. Here
        //     they taper to the LID's own surface over kTcPortalSplay and then
        //     follow it out to the zero-delta seam, where their height goes to
        //     nothing on its own. Nothing is trimmed to fit; the taper is the
        //     same function the lid uses, so they cannot disagree.
        //   * the projecting ARCH RING, already emitted above as the shell's
        //     canopy + end-cap annulus.
        // The spandrel's lower edge IS shellOuterTopAt() — the exact ellipse the
        // shell's outer skin was swept on — so the concrete meets the tube with
        // no gap and no overlap by construction rather than by tolerance.
        {
            MeshBuf portals;
            const float aOut = kTcTubeHalfWidth + kTcShellThick;
            const float W1   = kTcCorridorHalfW + kTcCorridorFall;
            // Column samples: dense across the arch (so the spandrel follows the
            // ellipse), coarser out along the wingwalls. ±aOut are sampled
            // EXACTLY, or a strip would slice across the springing.
            std::vector<float> lats;
            for (int k = -12; k <= 12; ++k) lats.push_back(aOut * (float)k / 12.0f);
            for (int k = 1; k <= 22; ++k) {
                const float t = aOut + (W1 - aOut) * (float)k / 22.0f;
                lats.insert(lats.begin(), -t);
                lats.push_back(t);
            }

            for (int end = 0; end < 2; ++end) {
                const float sFace = (end == 0) ? route.boreS0 : route.boreS1;
                const float sgn   = (end == 0) ? 1.0f : -1.0f;         // inward along s
                const float sBack = sFace + sgn * kTcPortalThick;
                const float outN[3] = { -sgn * axis[0], 0.0f, -sgn * axis[2] };
                const float inN [3] = {  sgn * axis[0], 0.0f,  sgn * axis[2] };
                const float upN [3] = { 0.0f, 1.0f, 0.0f };

                // The parapet line: the highest backfill the headwall has to
                // retain across its own width, plus the proud edge.
                float hwTop = -1e9f;
                for (int k = 0; k <= 24; ++k) {
                    const float lat = -kTcPortalHalfW + 2.0f * kTcPortalHalfW * (float)k / 24.0f;
                    hwTop = std::max(hwTop, tunnelLidHeightAt(route, sFace, lat));
                }
                hwTop += kTcPortalProud;

                // The wall's top profile: flat at hwTop across the headwall,
                // tapering out to the LID over kTcPortalSplay, and NEVER below
                // the lid, because the lid is the backfill this wall retains.
                // Pass 2 tried a plain monotone-descending clamp to kill the
                // ragged top edge; that clamp can drop the wall BELOW the lid on
                // a rising flank, and where it did you could see straight through
                // the portal into the void under the backfill (the green slot at
                // the top right of 08_exit_portal). Taking the max against the
                // lid is both the smooth-edge fix and the no-gap guarantee, and
                // it needs no clamp at all: inside the headwall the blend is
                // exactly hwTop, and hwTop is by definition the highest lid there.
                std::vector<float> tops(lats.size(), 0.0f);
                for (size_t k = 0; k < lats.size(); ++k) {
                    const float al = std::fabs(lats[k]);
                    const float lidY = tunnelLidHeightAt(route, sFace, lats[k]);
                    const float w = sstep(kTcPortalHalfW, kTcPortalHalfW + kTcPortalSplay, al);
                    tops[k] = std::max(hwTop + (lidY - hwTop) * w, lidY);
                }
                // Smooth the top edge, then re-assert the no-gap guarantee. The
                // wingwall inherits the hillside's small-scale noise otherwise
                // and its coping comes out as a staircase (a.png / e.png, pass 6).
                for (int it = 0; it < 3; ++it) {
                    std::vector<float> tmp = tops;
                    for (size_t k = 1; k + 1 < tops.size(); ++k)
                        tmp[k] = 0.25f*tops[k-1] + 0.5f*tops[k] + 0.25f*tops[k+1];
                    tops.swap(tmp);
                }
                for (size_t k = 0; k < lats.size(); ++k)
                    tops[k] = std::max(tops[k], tunnelLidHeightAt(route, sFace, lats[k]));
                auto colTop = [&](size_t k) { return tops[k]; };
                auto colBase = [&](float lat, float sAt) {
                    const float al = std::fabs(lat);
                    const float ry = route.roadYAt(sAt);
                    if (al <= aOut) return ry + shellOuterTopAt(al);
                    float x = 0.0f, z = 0.0f;
                    route.worldAt(sAt, lat, x, z);
                    return std::min(terrainHeightAtWorld(x, z), ry) - 1.4f;
                };
                auto P = [&](float sAt, float lat, float y, float out[3]) {
                    route.worldAt(sAt, lat, out[0], out[2]);
                    out[1] = y;
                };

                for (size_t k = 0; k + 1 < lats.size(); ++k) {
                    const float t0 = lats[k], t1 = lats[k + 1];
                    const float y0 = colTop(k),   y1 = colTop(k + 1);
                    const float b0 = colBase(t0, sFace), b1 = colBase(t1, sFace);
                    if (y0 - b0 < 0.06f && y1 - b1 < 0.06f) continue;
                    float p0[3], p1[3], p2[3], p3[3];
                    // Front face (the one the driver sees).
                    P(sFace, t0, b0, p0); P(sFace, t1, b1, p1);
                    P(sFace, t1, y1, p2); P(sFace, t0, y0, p3);
                    portals.quad(p0, p1, p2, p3, outN, t0 * 0.16f, t1 * 0.16f, 0.0f, 1.0f);
                    // Back face, one slab thickness in.
                    const float c0 = colBase(t0, sBack), c1 = colBase(t1, sBack);
                    P(sBack, t0, c0, p0); P(sBack, t1, c1, p1);
                    P(sBack, t1, y1, p2); P(sBack, t0, y0, p3);
                    portals.quad(p0, p1, p2, p3, inN, t0 * 0.16f, t1 * 0.16f, 0.0f, 1.0f);
                    // Coping across the top of the slab.
                    P(sFace, t0, y0, p0); P(sFace, t1, y1, p1);
                    P(sBack, t1, y1, p2); P(sBack, t0, y0, p3);
                    portals.quad(p0, p1, p2, p3, upN, 0.0f, 1.0f, 0.0f, 0.3f);
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

        // ================= 5) THE APPROACH CUTTINGS =========================
        // "The approach cutting is part of the tunnel." The reason the old build
        // had earth on the roadway is that the ground was allowed to climb
        // ACROSS the road; a real road holds it back BESIDE the road instead.
        // So each shoulder gets a retaining wall standing at the toe of the cut
        // batter, its top following the natural grade while the road stays flat,
        // and a BENCH from the wall back to the point where the batter genuinely
        // reaches that height — found by scanning the real field, so the bench's
        // back edge lands ON the ground instead of near it. Approaching the
        // portal the walls splay from the corridor's floor half-width out to the
        // headwall's, so the cutting funnels into the mouth as one structure.
        {
            MeshBuf walls, berm;
            struct WStat { float s = 0, off = 0, top = 0, base = 0, bench = 0; bool on = false; };
            const float upN[3] = { 0.0f, 1.0f, 0.0f };
            uint32_t wallStations = 0;

            for (int side = 0; side < 2; ++side) {
                const float sg = (side == 0) ? -1.0f : 1.0f;
                const float nFace[3] = { -sg * right[0], 0.0f, -sg * right[2] };
                const float nBack[3] = {  sg * right[0], 0.0f,  sg * right[2] };
                for (int run = 0; run < 2; ++run) {
                    const float rs0 = (run == 0) ? 8.0f : route.boreS1;
                    const float rs1 = (run == 0) ? route.boreS0 : route.totalLen - 8.0f;
                    if (rs1 - rs0 < 10.0f) continue;

                    std::vector<WStat> stn;
                    for (float s = rs0; s <= rs1 + 0.01f; s += 4.0f) {
                        WStat w; w.s = std::min(s, rs1);
                        const float dPortal = (run == 0) ? (rs1 - w.s) : (w.s - rs0);
                        w.off = kTcCorridorHalfW +
                                (kTcPortalHalfW - kTcCorridorHalfW) *
                                (1.0f - sstep(0.0f, kTcWallSplay, dPortal));
                        const float ry = route.roadYAt(w.s);
                        float x = 0.0f, z = 0.0f;
                        // The wall top is taken part-way up the cut batter, but
                        // capped by how far back the berm behind it is allowed to
                        // run. Uncapped, a wall on a flank where the batter is
                        // long produced a 15 m ledge that read as a loading dock
                        // (08_exit_portal, pass 2). Tall wall and narrow berm are
                        // in tension; this resolves it by lowering the WALL.
                        route.worldAt(w.s, sg * (w.off + kTcCorridorFall * kTcWallTopFrac), x, z);
                        w.top = terrainHeightAtWorld(x, z);
                        route.worldAt(w.s, sg * (w.off + kTcWallBenchMax), x, z);
                        w.top = std::min(w.top, terrainHeightAtWorld(x, z));
                        w.top = std::min(w.top, ry + kTcWallMaxH);
                        w.on  = (w.top >= ry + kTcWallMinH);
                        // Where does the batter ACTUALLY reach the wall top?
                        w.bench = w.off + kTcWallBench;
                        for (float o = w.off + kTcWallThick + 0.3f;
                             o <= w.off + kTcWallBenchMax + 0.01f; o += 0.25f) {
                            route.worldAt(w.s, sg * o, x, z);
                            if (terrainHeightAtWorld(x, z) >= w.top) { w.bench = o; break; }
                        }
                        w.bench = std::min(w.bench, w.off + kTcWallBenchMax);
                        route.worldAt(w.s, sg * w.off, x, z);
                        const float toe = terrainHeightAtWorld(x, z);
                        w.base = std::min(toe, ry) - 1.8f;
                        // A retaining wall belongs at the toe of a CUTTING. Where
                        // the shoulder is falling away instead - the fill side of
                        // a side-slope - there is nothing to retain, and a wall
                        // put there stands on nothing with its berm hanging in
                        // the air (the grass-topped slab beside the exit portal,
                        // passes 3-5). The embankment handles that side.
                        if (toe < ry - 2.2f) w.on = false;
                        stn.push_back(w);
                    }
                    // Kill runs too short to BE a wall. A one- or two-station
                    // fragment is a 4 m slab standing on its own in the middle of
                    // a hillside - the first pass shipped exactly that beside the
                    // exit portal, and it is the floating lit slab 08 showed.
                    for (size_t j = 0; j < stn.size(); ) {
                        if (!stn[j].on) { ++j; continue; }
                        size_t e = j; while (e < stn.size() && stn[e].on) ++e;
                        if ((int)(e - j) < kTcWallMinRun)
                            for (size_t q = j; q < e; ++q) stn[q].on = false;
                        j = e;
                    }
                    for (const WStat& w : stn) if (w.on) ++wallStations;

                    auto P = [&](const WStat& w, float lat, float y, float out[3]) {
                        route.worldAt(w.s, sg * lat, out[0], out[2]);
                        out[1] = y;
                    };
                    for (size_t j = 0; j + 1 < stn.size(); ++j) {
                        const WStat& a = stn[j]; const WStat& b = stn[j + 1];
                        if (!a.on || !b.on) continue;
                        float p0[3], p1[3], p2[3], p3[3];
                        // Face.
                        P(a, a.off, a.base, p0); P(b, b.off, b.base, p1);
                        P(b, b.off, b.top,  p2); P(a, a.off, a.top,  p3);
                        walls.quad(p0, p1, p2, p3, nFace, a.s * 0.10f, b.s * 0.10f, 0.0f, 1.0f);
                        // Coping lip: front, top, back.
                        P(a, a.off - 0.12f, a.top,         p0); P(b, b.off - 0.12f, b.top,         p1);
                        P(b, b.off - 0.12f, b.top + 0.20f, p2); P(a, a.off - 0.12f, a.top + 0.20f, p3);
                        walls.quad(p0, p1, p2, p3, nFace, a.s*0.10f, b.s*0.10f, 0.0f, 0.1f);
                        P(a, a.off - 0.12f, a.top + 0.20f, p0); P(b, b.off - 0.12f, b.top + 0.20f, p1);
                        P(b, b.off + kTcWallThick, b.top + 0.20f, p2);
                        P(a, a.off + kTcWallThick, a.top + 0.20f, p3);
                        walls.quad(p0, p1, p2, p3, upN, a.s*0.10f, b.s*0.10f, 0.0f, 0.2f);
                        P(a, a.off + kTcWallThick, a.top + 0.20f, p0);
                        P(b, b.off + kTcWallThick, b.top + 0.20f, p1);
                        P(b, b.off + kTcWallThick, b.top, p2);
                        P(a, a.off + kTcWallThick, a.top, p3);
                        walls.quad(p0, p1, p2, p3, nBack, a.s*0.10f, b.s*0.10f, 0.0f, 0.1f);
                        // The CATCH BERM behind the wall, back to where the
                        // batter genuinely reaches the wall top (found by
                        // scanning the real field, so its outer edge lands ON the
                        // ground). It is drawn as GROUND, not concrete: a real
                        // cutting bench is earth, and a concrete one this wide is
                        // what made the exit portal read as a loading dock.
                        P(a, a.off + kTcWallThick, a.top, p0); P(b, b.off + kTcWallThick, b.top, p1);
                        P(b, b.bench, b.top, p2);              P(a, a.bench, a.top, p3);
                        berm.quad(p0, p1, p2, p3, upN, p0[0]*0.05f, p1[0]*0.05f, p0[2]*0.05f, p2[2]*0.05f);
                        // Close the run's ends so the wedge under the bench is
                        // never open to the camera along the road.
                        const bool firstOn = (j == 0) || !stn[j - 1].on;
                        const bool lastOn  = (j + 2 >= stn.size()) || !stn[j + 2].on;
                        if (firstOn || lastOn) {
                            const WStat& w = firstOn ? a : b;
                            const float en[3] = { firstOn ? -axis[0] : axis[0], 0.0f,
                                                  firstOn ? -axis[2] : axis[2] };
                            P(w, w.off,   w.base,        p0);
                            P(w, w.off,   w.top + 0.20f, p1);
                            P(w, w.bench, w.top,         p2);
                            P(w, w.bench, w.base,        p3);
                            walls.quad(p0, p1, p2, p3, en, 0.0f, 0.5f, 0.0f, 0.5f);
                        }
                    }
                }
            }
            Material wm;
            if (portalSet.ok) {
                wm.alb = portalSet.albedo; wm.mr = portalSet.mr; wm.nrm = portalSet.normal;
                const float v = portalSet.valueTint() * 0.92f;   // a shade off the headwall pour
                wm.tint[0] = wm.tint[1] = wm.tint[2] = v;
            } else {
                wm.alb = concreteTex; wm.mr = wallMR;
                wm.tint[0] = 0.78f; wm.tint[1] = 0.78f; wm.tint[2] = 0.76f;
            }
            upload(walls, wm, /*collide*/true);
            Material bm;
            if (groundTex.valid()) bm.alb = groundTex;   // the MARKER: terrain splat
            else { bm.alb = concreteTex; bm.mr = wallMR; }
            upload(berm, bm, /*collide*/true);
            char wb[160];
            std::snprintf(wb, sizeof(wb),
                "tunnel corridor: approach cuttings retained — %u wall stations "
                "(splaying %.1f -> %.1f m off centre at the portals)",
                wallStations, kTcCorridorHalfW, kTcPortalHalfW);
            x3::logInfo(wb);
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
            // PORTAL LIGHTS. A real mouth is lit harder than the bore: the
            // threshold is where a driver's eyes have to adapt, so it is where
            // the lighting is densest. Two lights per mouth, just inside the
            // headwall, which is 8 of the engine's 64 forward point lights in
            // total for the whole tunnel.
            for (int end = 0; end < 2; ++end) {
                const float se = (end == 0) ? route.boreS0 + 5.0f : route.boreS1 - 5.0f;
                const Frame f = frameAt(clampf(se, 0.0f, route.totalLen));
                x3::rhi::PointLight pl{};
                pl.pos[0] = f.p[0]; pl.pos[1] = f.p[1] + kTcTubeCrownH - 0.9f; pl.pos[2] = f.p[2];
                pl.range = 30.0f;
                pl.color[0] = 3.1f; pl.color[1] = 2.9f; pl.color[2] = 2.3f;
                m_lights.push_back(pl);
            }
            char b[256];
            std::snprintf(b, sizeof(b),
                "tunnel corridor: bore %.0f m | %u emissive strips @15 m | %u REAL point lights "
                "(BL's every-3rd-strip ratio would want %u — see BL_WORLD_PORT.md 4.4)",
                span, nStrip, (unsigned)m_lights.size(), (unsigned)(nStrip / 3 + 1));
            x3::logInfo(b);
        }
        // JOIN THE MERGED POOL. From here the bore's lights are uploaded by
        // uploadTunnelLights() per frame against the camera, not owned by
        // whichever host happened to build last.
        registerTunnelLightSource(this);

        // ---- HONEST SELF-CHECK, rewritten for cut-and-cover. The old one
        // asked whether the TERRAIN stayed off the shell; under cut-and-cover
        // the terrain is a flat trench floor and the question is meaningless.
        // The two things that can now go wrong are (a) the BACKFILL LID cutting
        // into the bore and (b) the ground standing on the ROADWAY — the defect
        // this lane exists to kill. Both are walked at 0.5 m and both are
        // reported as numbers, not adjectives.
        {
            const float aOut = kTcTubeHalfWidth + kTcShellThick;
            float worstLid = 1e9f, worstLidS = 0.0f;
            for (float s = route.boreS0; s <= route.boreS1 + 0.01f; s += 0.5f) {
                const float ry = route.roadYAt(s);
                for (int k = 0; k <= 12; ++k) {
                    const float lx = -aOut + 2.0f * aOut * (float)k / 12.0f;
                    const float top = ry + shellOuterTopAt(lx);
                    const float g = tunnelLidHeightAt(route, s, lx);
                    if (g - top < worstLid) { worstLid = g - top; worstLidS = s; }
                }
            }
            float worstRoad = -1e9f, worstRoadS = 0.0f, buried = 0.0f;
            for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 0.5f) {
                const float ry = route.roadYAt(s);
                float w = -1e9f;
                for (int k = -6; k <= 6; ++k) {
                    const float lx = (float)k * kTcRoadHalfWidth / 6.0f;
                    float x = 0.0f, z = 0.0f;
                    route.worldAt(s, lx, x, z);
                    w = std::max(w, terrainHeightAtWorld(x, z) - ry);
                }
                if (w > 0.0f) buried += 0.5f;
                if (w > worstRoad) { worstRoad = w; worstRoadS = s; }
            }
            char cb[420];
            std::snprintf(cb, sizeof(cb),
                "tunnel corridor: backfill cover over the shell = %.2f m worst at s=%.0f "
                "(negative == lid inside the bore) | GROUND ON THE ROADWAY = %.1f m of "
                "road, worst %.2f m at s=%.0f (negative == clear, which is the fix)",
                worstLid, worstLidS, buried, worstRoad, worstRoadS);
            if (worstLid < 0.0f || buried > 0.0f) x3::logError(cb); else x3::logInfo(cb);
        }
    }

    // ---- WHERE DID THIS BORE ACTUALLY LAND? One line, every bore, always. ----
    if (gMax[1] >= gMin[1]) {
        char ab[420];
        std::snprintf(ab, sizeof(ab),
            "tunnel corridor AABB '%s': x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f] "
            "| extent %.1f x %.1f x %.1f m | chord centre (%.1f, %.1f) halfLen %.1f",
            route.name ? route.name : "(unnamed)",
            gMin[0], gMax[0], gMin[1], gMax[1], gMin[2], gMax[2],
            gMax[0] - gMin[0], gMax[1] - gMin[1], gMax[2] - gMin[2],
            route.cx, route.cz, route.halfLen);
        x3::logInfo(ab);
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
    // NOT the surface sets: they are SHARED across every bore now, so freeing
    // them here would pull the lining out from under the other tunnels. Released
    // process-wide by shutdownTunnelSurfaces().
    m_lights.clear();
    unregisterTunnelLightSource(this);   // leaves the merged pool
}

void TunnelCorridorWorld::showcaseCamera(const TunnelRoute& route, int which, float cam[5]) const {
    // Shot 8 — INSIDE THE LNS GARAGE. The pose was solved in build() while the
    // bay was being emitted (only the rooms program knows which door won it).
    // A bore with no garage falls back to the mid-bore frame instead of
    // pointing a camera at rock.
    if (which == 8) {
        if (m_garageCamValid) { for (int k = 0; k < 5; ++k) cam[k] = m_garageCam[k]; return; }
        which = 1;
    }
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
        // High and back, looking down the whole corridor. Under the old build
        // this was the shot that showed the SADDLE the bore scooped out of the
        // ridge. Under cut-and-cover the backfill lid puts the ridge back to its
        // pre-corridor profile, so what this frame is now for is the OPPOSITE
        // check: is the hill intact, and does the lid read as the same ground as
        // everything around it? Judge it honestly.
        // Framed so the lid can actually be JUDGED: 190 m up and 300 m back put
        // the whole ridge inside a few pixels and showed nothing but streamer
        // LOD. 85 m up, 210 m back, pitched into the hill.
        const float mid = (route.boreS0 + route.boreS1) * 0.5f;
        float p[3]; route.posAt(std::max(0.0f, mid - 210.0f), p);
        cam[0] = p[0]; cam[1] = p[1] + 85.0f; cam[2] = p[2];
        cam[3] = yawFwd; cam[4] = -0.30f;
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
    if (which == 5) {
        // HEAD-ON AND CLOSE. The whole portal in one frame: headwall, the
        // wingwalls tapering out of it, the projecting arch ring, the retaining
        // walls funnelling in. Close enough that a fudge would show.
        const float sEye = std::max(3.0f, route.boreS0 - 30.0f);
        float p[3]; route.posAt(sEye, p);
        cam[0] = p[0]; cam[1] = p[1] + 3.1f; cam[2] = p[2];
        cam[3] = yawFwd; cam[4] = 0.09f;
        return;
    }
    if (which == 6) {
        // INSIDE, LOOKING OUT at the entrance mouth. THIS is the frame the old
        // build could not survive: the earth ramp filled the bottom of the arch
        // from in here, and the daylight came through a slot over a dirt bank.
        // If anything is still wrong with the mouth it shows here first.
        const float sEye = route.boreS0 + 46.0f;
        float p[3]; route.posAt(clampf(sEye, 0.0f, route.totalLen), p);
        cam[0] = p[0]; cam[1] = p[1] + 2.0f; cam[2] = p[2];
        cam[3] = yawBack; cam[4] = 0.02f;
        return;
    }
    if (which == 7) {
        // The EXIT portal in three-quarter from up on the bank — the other
        // mouth, on the other gradient, from an angle that shows how the
        // wingwalls die into the hillside instead of ending in mid-air.
        const float sEye = std::min(route.totalLen - 4.0f, route.boreS1 + 40.0f);
        float e[3]; route.posAt(sEye, e);
        const float lat = -21.0f;
        e[0] += -route.dirZ * lat; e[2] += route.dirX * lat; e[1] += 16.0f;
        float t[3]; route.posAt(route.boreS1 - 4.0f, t);
        t[1] += 3.5f;
        const float dx = t[0]-e[0], dy = t[1]-e[1], dz = t[2]-e[2];
        const float hl = std::sqrt(dx*dx + dz*dz);
        cam[0] = e[0]; cam[1] = e[1]; cam[2] = e[2];
        cam[3] = std::atan2(dz, dx); cam[4] = std::atan2(dy, std::max(0.01f, hl));
        return;
    }

    float s = 0.0f; const float eyeUp = 2.4f; float yaw = yawFwd, pitch = 0.0f;
    switch (which) {
        case 0: s = std::max(6.0f, route.boreS0 - 85.0f);  yaw = yawFwd;  pitch =  gradeAt(s); break;
        case 1: s = (route.boreS0 + route.boreS1) * 0.5f;   yaw = yawFwd; pitch = gradeAt(s); break;
        case 2: s = std::min(route.totalLen - 6.0f, route.boreS1 + 60.0f); yaw = yawBack; pitch = -gradeAt(s); break;
        default: break;
    }
    float p[3]; route.posAt(clampf(s, 0.0f, route.totalLen), p);
    cam[0] = p[0]; cam[1] = p[1] + eyeUp; cam[2] = p[2];
    cam[3] = yaw;  cam[4] = pitch;
}

// ===========================================================================
// --test-tunnelmouth — THE DEFECT GATE.
//
// Headless, no device, no physics. The bug this file exists to close is a
// GEOMETRIC one, so the gate is geometric: it asks the real height field, the
// real corridor and the real lid function the same questions a camera would.
//
// M6 is the one that matters most for the long run. The old build's residual
// was reported as a property of a technique; it was really a property of a
// construction, and a construction has to survive the terrain being regenerated
// (inspx/island-regen). So M6 re-derives the ENTIRE route from scratch against
// three other hillsides — different centres, different headings, different
// gradients — and re-asserts M1 on each. It never touches the registry: the
// derivation evaluates its own stack corridor through the pure
// terrainCorridorDepthAt(), so a failing seed cannot corrupt the live world.
// ===========================================================================
bool runTunnelMouthSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name, const char* detail) {
        if (ok) { ++pass; x3::logInfo(std::string("  [ok]   ") + name + " — " + detail); }
        else    { ++fail; x3::logError(std::string("  [FAIL] ") + name + " — " + detail); }
    };
    char d[512];

    // The gate must see the world the game sees, so it goes through the real
    // boot entry point rather than a private copy of it.
    const TunnelRoute& route = registerTunnelCorridor();

    // ---- M0: the corridor registered and there is a tunnel here at all.
    std::snprintf(d, sizeof(d), "corridors=%u, roofed span %.0f..%.0f m (%.0f m), route %.0f m",
                  terrainCorridorCount(), route.boreS0, route.boreS1,
                  route.boreS1 - route.boreS0, route.totalLen);
    check(terrainCorridorCount() >= 1 && route.boreValid && route.st.size() >= 2,
          "M0 corridor registered + roofed span found", d);

    // ---- M1: THE DEFECT. Full road width, 0.5 m, against the live field.
    // Under the old two-regime build this fails by ~18 m of road.
    {
        float worst = -1e9f, worstS = 0.0f, buried = 0.0f, buriedInBore = 0.0f;
        for (float s = 0.0f; s <= route.totalLen + 0.01f; s += 0.5f) {
            const float ry = route.roadYAt(s);
            float w = -1e9f;
            for (int k = -8; k <= 8; ++k) {
                const float lat = (float)k * kTcRoadHalfWidth / 8.0f;
                float x = 0.0f, z = 0.0f;
                route.worldAt(s, lat, x, z);
                w = std::max(w, terrainHeightAtWorld(x, z) - ry);
            }
            if (w > 0.0f) {
                buried += 0.5f;
                if (s >= route.boreS0 && s <= route.boreS1) buriedInBore += 0.5f;
            }
            if (w > worst) { worst = w; worstS = s; }
        }
        std::snprintf(d, sizeof(d),
            "%.1f m of roadway has ground on it (%.1f m of it inside the shell); "
            "worst ground-above-road %+.3f m at s=%.0f",
            buried, buriedInBore, worst, worstS);
        check(buried == 0.0f && worst <= 0.0f,
              "M1 NO EARTH ON THE ROADWAY, anywhere on the route", d);
    }

    // ---- M2: the boot log's own number agrees with M1 (a stale reporter is a
    // lie the next engineer will believe).
    std::snprintf(d, sizeof(d), "route.buriedRoadLen=%.1f m, route.maxRoadBury=%+.3f m",
                  route.buriedRoadLen, route.maxRoadBury);
    check(route.buriedRoadLen == 0.0f && route.maxRoadBury <= 0.0f,
          "M2 the reported residual is 0 and honest", d);

    // ---- M3: the backfill lid never cuts into the bore.
    {
        const float aOut = kTcTubeHalfWidth + kTcShellThick;
        float worst = 1e9f, worstS = 0.0f;
        for (float s = route.boreS0; s <= route.boreS1 + 0.01f; s += 0.5f) {
            const float ry = route.roadYAt(s);
            for (int k = 0; k <= 16; ++k) {
                const float lat = -aOut + 2.0f * aOut * (float)k / 16.0f;
                const float clr = tunnelLidHeightAt(route, s, lat) - (ry + shellOuterTopAt(lat));
                if (clr < worst) { worst = clr; worstS = s; }
            }
        }
        std::snprintf(d, sizeof(d), "worst lid-over-shell clearance %.2f m at s=%.0f (want >= %.2f)",
                      worst, worstS, kTcLidCover * 0.8f);
        check(worst >= kTcLidCover * 0.8f, "M3 backfill clears the shell everywhere", d);
    }

    // ---- M4: the lid lands EXACTLY on the untouched terrain at the zero-delta
    // seam, and dives under it beyond. If this drifts, the hillside grows a
    // visible scar along both shoulders of every tunnel in the game.
    {
        const float W1 = kTcCorridorHalfW + kTcCorridorFall;
        float worstSeam = 0.0f, worstDelta = 0.0f, worstApron = -1e9f;
        for (float s = route.boreS0; s <= route.boreS1 + 0.01f; s += 2.0f) {
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const float lat = (float)sgn * W1;
                float x = 0.0f, z = 0.0f;
                route.worldAt(s, lat, x, z);
                worstDelta = std::max(worstDelta, std::fabs(terrainCorridorDelta(x, z)));
                worstSeam  = std::max(worstSeam,
                    std::fabs(tunnelLidHeightAt(route, s, lat) - terrainHeightAtWorld(x, z)));
                const float latA = (float)sgn * (W1 + kTcLidApron);
                route.worldAt(s, latA, x, z);
                worstApron = std::max(worstApron,
                    tunnelLidHeightAt(route, s, latA) - terrainHeightAtWorld(x, z));
            }
        }
        std::snprintf(d, sizeof(d),
            "|corridor delta| at the seam = %.6f m (must be 0), |lid - terrain| = %.6f m, "
            "apron sits %+.3f m relative to the terrain (must be negative)",
            worstDelta, worstSeam, worstApron);
        check(worstDelta == 0.0f && worstSeam <= 1e-3f && worstApron < 0.0f,
              "M4 lid meets the untouched terrain exactly, apron tucks under", d);
    }

    // ---- M5: the road is still a road. The grading is what makes the cut
    // depth finite; if it stopped limiting, the corridor would gouge.
    {
        float worstGrade = 0.0f, maxCut = 0.0f;
        for (size_t i = 1; i < route.st.size(); ++i) {
            const float ds = route.st[i].s - route.st[i-1].s;
            if (ds > 1e-3f)
                worstGrade = std::max(worstGrade,
                    std::fabs(route.st[i].roadY - route.st[i-1].roadY) / ds);
        }
        for (const auto& n : route.st) maxCut = std::max(maxCut, n.latMax - n.roadY);
        std::snprintf(d, sizeof(d), "worst grade %.2f%% (limit %.2f%%), deepest cut %.1f m",
                      worstGrade * 100.0f, kTcMaxGrade * 100.0f, maxCut);
        check(worstGrade <= kTcMaxGrade * 1.02f && maxCut > 5.0f,
              "M5 the graded road still respects kTcMaxGrade", d);
    }

    // ---- M6: REGENERATION PROOF. Re-derive the whole construction on other
    // hillsides and re-assert M1 on each, entirely off the registry.
    {
        const RouteSeed seeds[] = {
            { -592.0f, -352.0f,  kRouteDirX,  kRouteDirZ, kRouteHalfLen },   // the canonical one, un-registered
            {  980.0f,  640.0f,  0.70711f,    0.70711f,   280.0f },
            { -1450.0f, 820.0f,  0.0f,        1.0f,       300.0f },
            {  240.0f, -1180.0f, 0.38268f,   -0.92388f,   260.0f },
        };
        int seedFails = 0;
        float worstAll = -1e9f;
        for (const RouteSeed& sd : seeds) {
            TunnelRoute r; TerrainCorridor c{};
            deriveRoute(sd, r, c);
            float worst = -1e9f;
            for (float s = 0.0f; s <= r.totalLen + 0.01f; s += 1.0f) {
                const float ry = r.roadYAt(s);
                for (int k = -6; k <= 6; ++k) {
                    const float lat = (float)k * kTcRoadHalfWidth / 6.0f;
                    float x = 0.0f, z = 0.0f;
                    r.worldAt(s, lat, x, z);
                    // The live registry still holds the canonical corridor, so
                    // the natural field is recovered first and only THIS seed's
                    // own corridor is applied — the seeds never see each other.
                    const float h = tunnelNaturalHeightAt(x, z) - terrainCorridorDepthAt(c, x, z);
                    worst = std::max(worst, h - ry);
                }
            }
            worstAll = std::max(worstAll, worst);
            if (worst > 0.0f || r.buriedRoadLen > 0.0f) ++seedFails;
        }
        std::snprintf(d, sizeof(d),
            "%d re-derived hillsides, %d with ground on the roadway; worst ground-above-road "
            "across all of them %+.3f m", (int)(sizeof(seeds)/sizeof(seeds[0])), seedFails, worstAll);
        check(seedFails == 0 && worstAll <= 0.0f,
              "M6 the construction survives a regenerated terrain", d);
    }

    // ---- M7: THE TILE-LOD WEDGE, on the REAL spawn stretch. M1 proves the
    // FIELD is clear; the owner's green strip through the spawn-road pavement
    // was the MESH — a Half/Quarter tile interpolating 2/4 m chords across the
    // carve's shoulder and standing decimetres above the carved datum (it
    // survived kSlabProud 0.07 because no slab lift can outrun a mesh wedge).
    // Survey every tile the route touches through the REAL mesher at all three
    // LODs, rasterise the emitted surface triangles over the ROADWAY at 0.5 m,
    // and require max(terrainMeshY - corridorDatumY) <= -0.02 m — the terrain
    // mesh strictly BELOW the road datum, at every LOD, everywhere a slab lies.
    {
        const TerrainConfig& wcfg = worldTerrainConfig();
        const float pad = kTcCorridorHalfW + kTcCorridorFall;
        float bx0 = 1e9f, bx1 = -1e9f, bz0 = 1e9f, bz1 = -1e9f;
        for (const TunnelStation& n : route.st) {
            bx0 = std::min(bx0, n.x - pad); bx1 = std::max(bx1, n.x + pad);
            bz0 = std::min(bz0, n.z - pad); bz1 = std::max(bz1, n.z + pad);
        }
        // Datum under (x,z): project onto the spine polyline; off-roadway or
        // off-route answers "not a road sample".
        auto datumAt = [&](float x, float z, float& outDatum) {
            float bestD2 = 1e18f, bestS = 0.0f;
            for (size_t i = 0; i + 1 < route.st.size(); ++i) {
                const TunnelStation& A = route.st[i];
                const TunnelStation& B = route.st[i + 1];
                const float abx = B.x - A.x, abz = B.z - A.z;
                const float len2 = abx * abx + abz * abz;
                if (len2 < 1e-6f) continue;
                float t = ((x - A.x) * abx + (z - A.z) * abz) / len2;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                const float dx = x - (A.x + abx * t), dz = z - (A.z + abz * t);
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; bestS = A.s + (B.s - A.s) * t; }
            }
            const float hw = kTcRoadHalfWidth + 0.6f;   // pavement + a kerb rim
            if (bestD2 > hw * hw) return false;
            if (bestS < 1.0f || bestS > route.totalLen - 1.0f) return false;
            outDatum = route.roadYAt(bestS);
            return true;
        };
        const float ts = wcfg.tileSize;
        float worstAll = -1e9f, worstX = 0.0f, worstZ = 0.0f;
        int   worstLod = 0;
        std::vector<x3::rhi::MeshVertex> mv;
        std::vector<uint32_t> mi;
        for (int lod = 0; lod < 3; ++lod) {
            float worstLodM = -1e9f, lx = 0.0f, lz = 0.0f;
            for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
                for (float tx = std::floor(bx0 / ts) * ts; tx < bx1; tx += ts) {
                    uint32_t surfN = 0;
                    mv.clear(); mi.clear();
                    buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, mv, mi, &surfN);
                    for (uint32_t t3 = 0; t3 + 2 < surfN; t3 += 3) {
                        const auto& A = mv[mi[t3]]; const auto& B = mv[mi[t3 + 1]];
                        const auto& C = mv[mi[t3 + 2]];
                        const float minX = std::min(A.pos[0], std::min(B.pos[0], C.pos[0]));
                        const float maxX = std::max(A.pos[0], std::max(B.pos[0], C.pos[0]));
                        const float minZ = std::min(A.pos[2], std::min(B.pos[2], C.pos[2]));
                        const float maxZ = std::max(A.pos[2], std::max(B.pos[2], C.pos[2]));
                        const float den = (B.pos[0] - A.pos[0]) * (C.pos[2] - A.pos[2]) -
                                          (C.pos[0] - A.pos[0]) * (B.pos[2] - A.pos[2]);
                        if (std::fabs(den) < 1e-6f) continue;
                        for (float sz = std::ceil(minZ / 0.5f) * 0.5f; sz <= maxZ; sz += 0.5f) {
                            for (float sx = std::ceil(minX / 0.5f) * 0.5f; sx <= maxX; sx += 0.5f) {
                                const float w0 = ((B.pos[0] - sx) * (C.pos[2] - sz) -
                                                  (C.pos[0] - sx) * (B.pos[2] - sz)) / den;
                                const float w1 = ((C.pos[0] - sx) * (A.pos[2] - sz) -
                                                  (A.pos[0] - sx) * (C.pos[2] - sz)) / den;
                                const float w2 = 1.0f - w0 - w1;
                                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                                float datum = 0.0f;
                                if (!datumAt(sx, sz, datum)) continue;
                                const float meshY = w0 * A.pos[1] + w1 * B.pos[1] + w2 * C.pos[1];
                                const float err = meshY - datum;
                                if (err > worstLodM) { worstLodM = err; lx = sx; lz = sz; }
                                if (err > worstAll) {
                                    worstAll = err; worstX = sx; worstZ = sz; worstLod = lod;
                                }
                            }
                        }
                    }
                }
            }
            std::snprintf(d, sizeof(d),
                "  [m7] LOD%d worst terrainMeshY - datumY over the roadway: %+.3f m at (%.0f, %.0f)",
                lod, worstLodM, lx, lz);
            x3::logInfo(d);
        }
        std::snprintf(d, sizeof(d),
            "worst terrainMeshY - datumY %+.3f m at (%.0f, %.0f) LOD%d (gate <= -0.02 m)",
            worstAll, worstX, worstZ, worstLod);
        check(worstAll <= -0.02f,
              "M7 terrain mesh stays BELOW the road datum at EVERY tile LOD (the green-strip gate)", d);
    }

    char sum[160];
    std::snprintf(sum, sizeof(sum), "--test-tunnelmouth: %d/%d passed", pass, pass + fail);
    if (fail) x3::logError(sum); else x3::logInfo(sum);
    return fail == 0;
}


// runTunnelDriveSelfTest (--test-tunneldrive) — see tunnel_corridor.h.
//
// The whole point is that a SCREENSHOT cannot prove traversability: the bore
// looked finished in every capture while an earth ramp walled it off. This
// drives the real Jolt wheeled rig through the real streamed-terrain collision
// (the very tile mesher the game uses, portal holes and all) and the real
// road/shell collision, twice: once with X3_TUNNEL_PORTAL_CUT=0 as the
// NEGATIVE CONTROL (the ramp must STOP the car — a control that cannot fail
// is not a control) and once enabled (the car must come out the far portal).
// ===========================================================================
namespace {

void setPortalCutEnv(bool on) {
#ifdef _WIN32
    _putenv_s("X3_TUNNEL_PORTAL_CUT", on ? "1" : "0");
#else
    setenv("X3_TUNNEL_PORTAL_CUT", on ? "1" : "0", 1);
#endif
}

struct TunnelDriveResult {
    bool  built = false;
    float maxS = 0.0f;          // furthest arc-length progress along the spine
    float worstBoreDy = 0.0f;   // max |carY - roadY| while inside the bore span
    float residual = 0.0f;      // route.buriedRoadLen (the FIELD-level number)
    float boreS0 = 0.0f, boreS1 = 0.0f, coverS0 = 0.0f, coverS1 = 0.0f, totalLen = 0.0f;
    uint32_t holeCount = 0;
    // MOUNTAIN-INTACT survey (the torn-mountain regression guard): the deepest
    // excavation the registered corridors apply anywhere across the route band,
    // vs the deepest the OPEN-CUT profile legitimately asks for. An unclamped
    // portal plug shows up here as a carve far beyond any open-cut need
    // (measured 44.6 m vs 18.2 m open-cut max when the tear shipped).
    float maxCarve = 0.0f;
    float maxOpenNeed = 0.0f;
    // TUBE-INTRUSION survey (defect: the drivable rock wall). Triangles of the
    // REAL tile meshes (surface / skirt, per LOD) whose face reaches into the
    // tube interior. Skirts carry no collision, so an intruding skirt is a
    // render-only wall across the carriageway — invisible to every field-level
    // and drive-level check, which is exactly how it shipped.
    uint32_t intrSurf[3]  = { 0, 0, 0 };
    uint32_t intrSkirt[3] = { 0, 0, 0 };
    // ROAD-MOUNT survey (Lane 7's defect, measured here too): walking laterally
    // from the ditch toward the road across the real collision, the largest
    // single upward step between adjacent samples. A vertical skirt face shows
    // up as its full height; the ~19 deg fillet shows as ~0.14 m per 0.4 m.
    float maxMountStep = 0.0f;
    float maxMountStepS = 0.0f;
};

TunnelDriveResult driveTheDemoRoute(bool cutOn) {
    TunnelDriveResult res;
    setPortalCutEnv(cutOn);
    // The test owns both boot registries for its duration (the same contract
    // --test-terraincorridor uses); each phase re-registers the SAME route
    // against its own field.
    clearTerrainCorridors();
    clearTerrainPortalHoles();
    const TunnelRoute* route = registerTunnelCorridorFor(demoTunnelSpec());
    if (!route || !route->boreValid) return res;
    res.residual = route->buriedRoadLen;
    res.boreS0 = route->boreS0; res.boreS1 = route->boreS1;
    res.coverS0 = route->coverS0; res.coverS1 = route->coverS1;
    res.totalLen = route->totalLen;
    res.holeCount = terrainPortalHoleCount();
    // MOUNTAIN-INTACT survey. terrainCorridorDelta IS the excavation, so no
    // "natural" re-derivation is needed: walk the spine at 2 m across the full
    // influence band and record the deepest cut anywhere; compare against the
    // deepest cut the open-cut profile itself asks for (station depths — bored
    // stations are 0 by §4's fix, so this is exactly the open-cut max).
    {
        const float n0x = route->cx - route->dirX * route->halfLen;
        const float n0z = route->cz - route->dirZ * route->halfLen;
        const float rx = -route->dirZ, rz = route->dirX;
        const float latReach = kTcCorridorHalfW + kTcCorridorFall + 2.0f;
        for (float s = 0.0f; s <= route->totalLen + 0.01f; s += 2.0f) {
            const float px = n0x + route->dirX * s, pz = n0z + route->dirZ * s;
            for (float lat = -latReach; lat <= latReach + 0.01f; lat += 2.0f)
                res.maxCarve = std::max(res.maxCarve,
                                        -terrainCorridorDelta(px + rx * lat, pz + rz * lat));
        }
        for (const TunnelStation& n : route->st)
            res.maxOpenNeed = std::max(res.maxOpenNeed, n.depth);
    }
    // ---- TUBE-INTRUSION probe (defect A diagnosis) --------------------------
    // Build the real tile meshes over the bore at every LOD and report any
    // triangle whose surface reaches into the tube interior. Classifies the
    // emitter (surface vs skirt) — measurement, not hypothesis.
    if (route->boreValid) {
        const x3::game::TerrainConfig& tcfg = worldTerrainConfig();
        const float ts = tcfg.tileSize;
        const float n0x = route->cx - route->dirX * route->halfLen;
        const float n0z = route->cz - route->dirZ * route->halfLen;
        const float rx = -route->dirZ, rz = route->dirX;
        std::vector<std::pair<float,float>> origins;
        auto addTile = [&](float wx, float wz) {
            const float ox = std::floor(wx / ts) * ts, oz = std::floor(wz / ts) * ts;
            for (auto& o : origins) if (o.first == ox && o.second == oz) return;
            origins.push_back({ ox, oz });
        };
        for (float s = std::max(0.0f, route->boreS0 - 40.0f);
             s <= std::min(route->totalLen, route->boreS1 + 40.0f); s += 8.0f)
            for (float lat = -24.0f; lat <= 24.0f; lat += 8.0f)
                addTile(n0x + route->dirX * s + rx * lat, n0z + route->dirZ * s + rz * lat);
        std::vector<x3::rhi::MeshVertex> tv; std::vector<uint32_t> ti;
        for (int lod = 0; lod < 3; ++lod) {
            uint32_t nSurf = 0, nSkirt = 0;
            float sLo = 1e9f, sHi = -1e9f, worstDy = 1e9f, worstLat = 0, worstS = 0;
            for (auto& o : origins) {
                uint32_t surfIdx = 0;
                buildTileMeshAbs(tcfg, o.first, o.second, (TerrainLod)lod, tv, ti, &surfIdx);
                for (size_t t = 0; t + 2 < ti.size(); t += 3) {
                    const bool skirt = t >= surfIdx;
                    // Sample verts + edge midpoints + centroid: a large coarse
                    // triangle can slice the tube with no vertex inside it.
                    float px[7], py[7], pz[7];
                    for (int k = 0; k < 3; ++k) {
                        const auto& v = tv[ti[t + k]];
                        px[k] = v.pos[0]; py[k] = v.pos[1]; pz[k] = v.pos[2];
                    }
                    for (int k = 0; k < 3; ++k) {
                        const int k2 = (k + 1) % 3;
                        px[3+k] = (px[k]+px[k2])*0.5f; py[3+k] = (py[k]+py[k2])*0.5f; pz[3+k] = (pz[k]+pz[k2])*0.5f;
                    }
                    px[6] = (px[0]+px[1]+px[2])/3.0f; py[6] = (py[0]+py[1]+py[2])/3.0f; pz[6] = (pz[0]+pz[1]+pz[2])/3.0f;
                    for (int k = 0; k < 7; ++k) {
                        const float s   = (px[k]-n0x)*route->dirX + (pz[k]-n0z)*route->dirZ;
                        const float lat = (px[k]-n0x)*rx + (pz[k]-n0z)*rz;
                        if (s < route->boreS0 + 2.0f || s > route->boreS1 - 2.0f) continue;
                        if (std::fabs(lat) > kTcTubeHalfWidth) continue;
                        const float dy = py[k] - route->roadYAt(s);
                        if (dy < 0.2f || dy > kTcTubeCrownH) continue;
                        (skirt ? nSkirt : nSurf)++;
                        sLo = std::min(sLo, s); sHi = std::max(sHi, s);
                        if (dy < worstDy) { worstDy = dy; worstLat = lat; worstS = s; }
                        break;
                    }
                }
            }
            res.intrSurf[lod]  = nSurf;
            res.intrSkirt[lod] = nSkirt;
            char ib[240];
            if (nSurf + nSkirt > 0)
                std::snprintf(ib, sizeof(ib),
                    "[tunneldrive]   tube intrusion LOD%d: %u surface + %u skirt tris, "
                    "s=%.0f..%.0f, lowest dy=%.1f m at s=%.0f lat=%.1f",
                    lod, nSurf, nSkirt, sLo, sHi, worstDy, worstS, worstLat);
            else
                std::snprintf(ib, sizeof(ib), "[tunneldrive]   tube intrusion LOD%d: none", lod);
            x3::logInfo(ib);
        }
    }

    HeadlessRenderDevice dev;
    Scene scene;
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) return res;

    float start[3];
    route->posAt(std::max(8.0f, route->boreS0 - 60.0f), start);
    TerrainStreamer streamer;
    // jobs == nullptr => fully synchronous generation (the documented headless
    // path), so collision under the car is never still in flight.
    streamer.init(scene, dev, *phys, nullptr, worldTerrainConfig(),
                  start[0], start[2], /*radius=*/2);
    TunnelCorridorWorld world;
    world.build(scene, dev, *phys, *route);

    DriveDemo car;
    res.built = car.buildPhysics(*phys, start[0], start[1] + 1.4f, start[2]);
    if (res.built) {
        // Point it down the corridor (host_tunnel's exact quaternion).
        const float yaw = std::atan2(route->dirZ, route->dirX);
        const float q[4] = { 0.0f, std::sin(-yaw * 0.5f), 0.0f, std::cos(-yaw * 0.5f) };
        phys->setBodyRotation(car.chassis(), q);
    }
    phys->optimizeBroadphase();

    // ---- ROAD-MOUNT SURVEY (the ditch-to-road step) -------------------------
    // Raycast the REAL static collision (terrain tiles + ribbon + shoulder +
    // fillet) downward along lateral walks from 18 m out to the centreline,
    // 0.4 m apart, every 12 m of open route (the bore is skipped: the ray
    // would hit the lid). The metric is the largest single UPWARD step walking
    // toward the road — a vertical skirt face reads as its full height. Only
    // the streamed ring near the car start is resident, so survey the s-range
    // the streamer actually covers.
    {
        const float n0x = route->cx - route->dirX * route->halfLen;
        const float n0z = route->cz - route->dirZ * route->halfLen;
        const float rxl = -route->dirZ, rzl = route->dirX;
        const float sStart = std::max(8.0f, route->boreS0 - 60.0f);
        const float sLo = std::max(10.0f, sStart - 50.0f);
        const float sHi = std::min(route->boreS0 - 10.0f, sStart + 50.0f);
        for (float s = sLo; s <= sHi; s += 12.0f) {
            const float px = n0x + route->dirX * s, pz = n0z + route->dirZ * s;
            for (int side = -1; side <= 1; side += 2) {
                float prevH = 0.0f; bool have = false;
                // 17.93, not 18.00: every emitted seam here is a constant-lat
                // line swept along the route (slab edge 6.0, shoulder edge 7.5,
                // fillet steps 7.5+1.6k), and a vertical ray EXACTLY in such a
                // seam plane threads between the two meshes and reports the
                // trench floor — the survey measured its own grid, not a wall
                // (4.48 m "step" at lat 6.0 where the shoulder provably is).
                // An off-round grid never lands on an exact seam.
                for (float lat = 17.93f; lat >= 0.0f; lat -= 0.4f) {
                    const float wx = px + rxl * lat * (float)side;
                    const float wz = pz + rzl * lat * (float)side;
                    x3::phys::RayHit rh = phys->rayCastStrict(
                        { wx, route->roadYAt(s) + 120.0f, wz }, { 0.0f, -1.0f, 0.0f },
                        400.0f, x3::phys::Layer::Static);
                    if (!rh.hit) { have = false; continue; }
                    // Road-MOUNT means climbing from BELOW the road onto it, so
                    // only steps whose foot is at/below road level count —
                    // otherwise the metric reports craggy natural hillside high
                    // above the carriageway (measured: a 1.1 m rock knob 5 m
                    // over the road at lat 17.5), which no car needs to climb
                    // to reach the road.
                    if (have && prevH < route->roadYAt(s) + 0.5f) {
                        const float step = rh.point.y - prevH;   // + = up toward road
                        if (step > res.maxMountStep) { res.maxMountStep = step; res.maxMountStepS = s; }
                    }
                    prevH = rh.point.y; have = true;
                }
            }
        }
    }

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 90 && res.built; ++i) {   // settle onto the suspension
        x3::phys::VehicleInput in{};
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
    }

    // Route frame for progress/steering: arc length s + signed lateral offset.
    const float n0x = route->cx - route->dirX * route->halfLen;
    const float n0z = route->cz - route->dirZ * route->halfLen;
    const float rxl = -route->dirZ, rzl = route->dirX;
    float prevLat = 0.0f; bool havePrev = false;
    float stallRef = -1e9f; int stallTicks = 0;
    const int kMaxTicks = 60 * 120;               // 120 s of sim, hard cap
    for (int i = 0; i < kMaxTicks && res.built; ++i) {
        float p[3]; car.chassisPos(p);
        const float s   = (p[0] - n0x) * route->dirX + (p[2] - n0z) * route->dirZ;
        const float lat = (p[0] - n0x) * rxl + (p[2] - n0z) * rzl;
        const float latVel = havePrev ? (lat - prevLat) / dt : 0.0f;
        prevLat = lat; havePrev = true;
        res.maxS = std::max(res.maxS, s);
        if (s > route->boreS0 + 5.0f && s < route->boreS1 - 5.0f)
            res.worstBoreDy = std::max(res.worstBoreDy,
                                       std::fabs(p[1] - route->roadYAt(s)));
        if (s > route->boreS1 + 25.0f) break;     // through and out — done
        // Stall detection: the negative control's expected exit (the ramp).
        if (s > stallRef + 0.5f) { stallRef = s; stallTicks = 0; }
        else if (++stallTicks > 60 * 6) break;

        x3::phys::VehicleInput in{};
        in.throttle = car.forwardSpeed() < 18.0f ? 1.0f : 0.1f;
        // Centreline follower: lat is the signed offset toward the car's OWN
        // right (route right == chassis right when pointed down the spine), so
        // drifting right (lat rising) steers left. Damped by the lateral rate.
        in.steer = clampf(-(0.10f * lat + 0.45f * latVel), -0.6f, 0.6f);
        car.setInput(in); car.preStep(dt);
        streamer.update(scene, dev, *phys, p[0], p[2]);
        phys->step(dt); car.postStep(dt);
    }

    if (res.built) car.shutdown();
    world.shutdown(dev, *phys);
    streamer.shutdown(scene, dev, *phys);
    phys->shutdown();
    return res;
}

} // namespace

bool runTunnelDriveSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[tunneldrive] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[tunneldrive] FAIL ") + name); }
    };
    auto report = [&](const char* tag, const TunnelDriveResult& r) {
        char b[380];
        std::snprintf(b, sizeof(b),
            "[tunneldrive]   %s: maxS=%.0f m (shell %.0f..%.0f, route %.0f m) | "
            "field residual %.0f m | portal holes %u | worst bore |dY| %.2f m | "
            "worst mount step %.2f m at s=%.0f | max carve %.1f m (open-cut need %.1f)",
            tag, r.maxS, r.boreS0, r.boreS1, r.totalLen,
            r.residual, r.holeCount, r.worstBoreDy,
            r.maxMountStep, r.maxMountStepS, r.maxCarve, r.maxOpenNeed);
        x3::logInfo(b);
    };

    x3::logInfo("[tunneldrive] PHASE A — portal cut DISABLED (the negative control)...");
    const TunnelDriveResult a = driveTheDemoRoute(false);
    report("A(cut off)", a);
    check(a.built, "A0 rig + world built");
    check(a.holeCount == 0, "A1 disabled => zero portal holes (the fallback is exact)");
    check(a.residual > 8.0f, "A2 the defect is real: field residual > 8 m without the cut");
    check(a.maxS < a.coverS0,
          "A3 NEGATIVE CONTROL: the earth ramp stops the car before full cover");

    x3::logInfo("[tunneldrive] PHASE B — portal cut ENABLED...");
    const TunnelDriveResult b = driveTheDemoRoute(true);
    report("B(cut on)", b);
    check(b.built, "B0 rig + world built");
    check(b.holeCount == 2, "B1 one portal hole per mouth registered");
    // B2 — THE TORN-MOUNTAIN GUARD. This check used to demand the PLUG collapse
    // the FIELD residual to <= 6 m. That contract is impossible over a massif:
    // the natural rise at these mouths is ~1.2 m/m, so a field floor can only
    // cross the 0.3..7.6 m drivable band in <= ~3 m per mouth if it is pinned
    // at road level right up to a sharp face past the main corridor's 18.8 m
    // end-cap reach — which means carving latMax - roadY ~22.8 m in: 44.6 m of
    // mountain, the very tear this branch exists to fix (a road-width canyon
    // above the portal, sky through the peak, shard triangles on the rim). The
    // capped plug leaves that last sweep to the PORTAL HOLES, which clear it
    // from the MESH — drivability is proven by B3/B4 against real collision.
    // What the FIELD must now guarantee instead, and what this asserts:
    //   (i)  the carver never digs deeper anywhere than the open cut's own
    //        deepest need (+ the plug cap, + slack) — a bounded, mountain-
    //        preserving carve. Unclamped plugs fail this at 44.6 vs 18.2 m.
    //   (ii) each mouth's sweep (bore..full-cover) fits the portal holes' 30 m
    //        reach, so every buried metre the field keeps is inside a hole.
    check(b.maxCarve <= std::max(b.maxOpenNeed, kTcPlugMaxCut) + 0.5f &&
          (b.coverS0 - b.boreS0) <= 30.0f && (b.boreS1 - b.coverS1) <= 30.0f,
          "B2 BOUNDED CARVE: no deeper than the open cut needs, mouth sweeps inside the holes' reach");
    check(b.maxS > b.boreS1 + 20.0f,
          "B3 DRIVE-THROUGH: the car exits past the far portal");
    check(b.worstBoreDy < 5.0f,
          "B4 through the bore at road level (not over the hill)");
    // Lane 7's ditch-to-road defect, kept dead: their survey measured skirt
    // faces of 0.45..6.81 m against ~0.43 m chassis clearance. With the ~19 deg
    // fillet, no adjacent-sample step on the approach may exceed clearance.
    // (Surveyed on BOTH phases' geometry — the fillet is not part of the portal
    // cut and has no env gate, so this is a threshold check, not A/B.)
    check(b.maxMountStep > 0.01f,
          "B5a mount survey saw real geometry (a zero survey would be a broken probe)");
    check(b.maxMountStep <= 0.45f,
          "B5b ROAD-MOUNT: worst ditch-to-road step within chassis clearance (0.45 m)");
    // B6 — THE DRIVABLE ROCK WALL, kept dead. Skirts at tile borders over the
    // BORE used to hang ~55 m from the lid straight through the tube (delta is
    // 0 on a bored reach, so the old delta-gated skirt clamp never fired):
    // measured 74 full-LOD skirt triangles inside the demo tube, down to 0.3 m
    // above the road — a render-only wall (skirts have no collision) that no
    // field query and no drive-through could ever see. The mesher now clamps
    // skirts by corridor CONTAINMENT (terrainCorridorContains). Full-LOD mesh
    // must be completely clean; skirts must be clean at every LOD. Coarse-LOD
    // SURFACE slivers (decimation over the mouth trench, drawn only beyond
    // ~192 m) are reported by the survey but not gated here.
    check(b.intrSurf[0] == 0 && b.intrSkirt[0] == 0 &&
          b.intrSkirt[1] == 0 && b.intrSkirt[2] == 0,
          "B6 NOTHING IN THE TUBE: full-LOD mesh clean + no skirt intrusion at any LOD");

    // Registry + env hygiene: leave the process at defaults.
    clearTerrainCorridors();
    clearTerrainPortalHoles();
    setPortalCutEnv(true);
    x3::logInfo("[tunneldrive] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}


// ---------------------------------------------------------------------------
// runRouteFrameSelfTest (--test-routeframe) — P1's gate.
//
// The road network rests on TunnelRoute's frame following its polyline, so that
// capability gets its own test rather than being implied by the tunnel gates
// (which only ever drive a STRAIGHT route and so cannot see a curve at all).
//
// Two halves, and the first matters as much as the second:
//   * a straight route must still match the old closed form EXACTLY, because
//     every existing gate depends on that;
//   * a curved route must actually follow its arc, with a frame that stays
//     perpendicular through the bend.
// ---------------------------------------------------------------------------
bool runRouteFrameSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[routeframe] " + m); }
        else    { ++failN; x3::logError("[routeframe] " + m); }
    };
    char d[256];

    // ---- A) STRAIGHT: identical to origin + dir * s ------------------------
    {
        TunnelRoute r;
        r.ox = -592.0f; r.oz = -352.0f;
        r.dirX = -0.92388f; r.dirZ = 0.38268f;
        const int N = 32; const float ds = 20.645f;
        r.st.resize(N);
        for (int i = 0; i < N; ++i) {
            r.st[i].s = ds * (float)i;
            r.st[i].x = r.ox + r.dirX * r.st[i].s;
            r.st[i].z = r.oz + r.dirZ * r.st[i].s;
            r.st[i].roadY = 40.0f;
        }
        r.totalLen = ds * (float)(N - 1);
        float worstPos = 0.0f, worstLat = 0.0f;
        for (float s = -40.0f; s <= r.totalLen + 40.0f; s += 0.5f) {
            float p[3]; r.posAt(s, p);
            worstPos = std::max(worstPos, std::fabs(p[0] - (r.ox + r.dirX * s)));
            worstPos = std::max(worstPos, std::fabs(p[2] - (r.oz + r.dirZ * s)));
            for (float lat : { -8.8f, 0.0f, 6.0f }) {
                float wx, wz; r.worldAt(s, lat, wx, wz);
                worstLat = std::max(worstLat, std::fabs(wx - (r.ox + r.dirX*s + (-r.dirZ)*lat)));
                worstLat = std::max(worstLat, std::fabs(wz - (r.oz + r.dirZ*s + ( r.dirX)*lat)));
            }
        }
        std::snprintf(d, sizeof(d), "worst posAt %.4f ft, worst worldAt %.4f ft (incl. extrapolation past both ends)",
                      worstPos * 3.28084f, worstLat * 3.28084f);
        check(worstPos < 0.01f && worstLat < 0.01f,
              "F1 STRAIGHT route is unchanged by the polyline frame", d);
    }

    // ---- B) CURVED: follows the arc, frame stays perpendicular -------------
    {
        // A quarter circle of radius 400 m — a sweeper, not a hairpin.
        const float R = 400.0f;
        TunnelRoute r;
        const int N = 32;
        r.st.resize(N);
        float acc = 0.0f, px = R, pz = 0.0f;
        for (int i = 0; i < N; ++i) {
            const float a = (3.14159265f * 0.5f) * (float)i / (float)(N - 1);
            const float x = R * std::cos(a), z = R * std::sin(a);
            if (i > 0) acc += std::sqrt((x-px)*(x-px) + (z-pz)*(z-pz));
            r.st[i].x = x; r.st[i].z = z; r.st[i].s = acc; r.st[i].roadY = 0.0f;
            px = x; pz = z;
        }
        r.totalLen = acc;
        r.ox = r.st[0].x; r.oz = r.st[0].z;
        r.dirX = 0.0f; r.dirZ = 1.0f;

        // The sampled centreline must lie ON the circle. Chord sag between
        // 32 nodes over a quarter arc is the only expected error.
        float worstR = 0.0f;
        for (float s = 0.0f; s <= r.totalLen; s += 0.5f) {
            float p[3]; r.posAt(s, p);
            worstR = std::max(worstR, std::fabs(std::sqrt(p[0]*p[0] + p[2]*p[2]) - R));
        }
        std::snprintf(d, sizeof(d), "worst radial error %.2f ft over a %.0f ft radius arc (chord sag)",
                      worstR * 3.28084f, R * 3.28084f);
        check(worstR < 2.0f, "F2 CURVED route follows its arc", d);

        // The frame must stay perpendicular: a lateral offset moves the point
        // RADIALLY, so the offset centreline is a concentric arc.
        //
        // SIGN: this arc runs counterclockwise (from (R,0) toward (0,R)), so at
        // (R,0) the tangent is +Z and "right of travel" — (-tz, +tx) — points
        // at the ORIGIN. A positive lateral offset therefore moves INWARD, and
        // the expected radius is R - lat, not R + lat. Writing it the wrong way
        // round first produced a 131.65 ft error at a 66 ft offset, which is
        // 2 x lat exactly: the signature of a sign flip, and the reason this
        // check earns its place rather than just restating the implementation.
        float worstPerp = 0.0f;
        for (float s = 1.0f; s <= r.totalLen - 1.0f; s += 0.5f) {
            for (float lat : { -20.0f, 20.0f }) {
                float wx, wz; r.worldAt(s, lat, wx, wz);
                const float got = std::sqrt(wx*wx + wz*wz);
                worstPerp = std::max(worstPerp, std::fabs(got - (R - lat)));
            }
        }
        std::snprintf(d, sizeof(d), "worst %.2f ft at +-66 ft lateral — a constant offset stays parallel to the arc",
                      worstPerp * 3.28084f);
        check(worstPerp < 2.5f, "F3 the frame stays PERPENDICULAR through the bend", d);

        // And the tangent must actually turn: 90 degrees end to end.
        float t0x, t0z, t1x, t1z;
        r.tangentAt(0.0f, t0x, t0z);
        r.tangentAt(r.totalLen, t1x, t1z);
        const float dot = t0x*t1x + t0z*t1z;
        const float deg = std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 57.29578f;
        std::snprintf(d, sizeof(d), "tangent turns %.1f deg end to end (expected ~90)", deg);
        check(deg > 80.0f && deg < 100.0f, "F4 the TANGENT turns with the road", d);
    }

    x3::logInfo("[routeframe] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
