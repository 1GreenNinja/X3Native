// tunnel_corridor — see tunnel_corridor.h for the design + the BL provenance.
// Clean-room, original work. No BL / third-party engine source was consulted or
// transcribed; the technique is documented in docs/design/BL_WORLD_PORT.md
// §2.2/§2.3/§3.2/§3.3/§4.3/§4.4 and re-implemented here from first principles.
#include "tunnel_corridor.h"

#include <cstdlib>
#include "mesh_prims.h"
#include "asset_root.h"        // assetRoot() — the surface_library mount point
#include "vehicle.h"           // DriveDemo — the drive-through self-test rig
#include "headless_device.h"   // HeadlessRenderDevice — self-test, no Vulkan
#include <memory>

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
// Surplus cover required BEFORE the corridor stops excavating. Without it the
// cut ends the moment cover is barely adequate, and the ground's unavoidable
// sweep from road level up over the crown then happens INSIDE the bore — an
// earth ramp across the carriageway that blocks the tunnel outright. Pushing the
// transition this much deeper buries the sweep under real hill instead.
constexpr float kBoreCutMargin = 14.0f;

// --- THE PORTAL CUT (plug corridors + mesh portal holes) -------------------
// The margin above measurably did NOT clear the mouth (residual 18 m -> 16 m):
// the ramp is not made by WHERE the depth profile switches but by the corridor
// FIELD itself. Two mechanisms, both irreducible at the main corridor's scale:
//   1. Along the spine the depth interpolates node-to-node (~20 m apart), and
//      the union's rounded END CAPS hold full depth for halfWidth (8.8 m) past
//      a segment end and then decay over falloff (10 m) — so the ground's
//      sweep from road level up past the crown is smeared over ~19 m however
//      the node depths step.
//   2. Even a PERFECTLY vertical field step cannot be driven through: the tile
//      mesher joins road-level vertices to lid-level vertices with a
//      continuous CURTAIN of triangles, and that curtain has collision.
// The fix is therefore also two-part, registered per mouth:
//   * a PORTAL PLUG — a short full-cut corridor with a TIGHT falloff whose end
//     cap reaches past the main corridor's (so the sweep happens over
//     kTcPlugFall metres at a face the headwall dresses, not over 19 m of
//     carriageway);
//   * a PORTAL HOLE (app/terrain.h) — the mesher drops the curtain triangles
//     inside the tube envelope at the mouth, render and collision both. The
//     shell + headwall stand in the gap.
// X3_TUNNEL_PORTAL_CUT=0 disables both and restores the previous behaviour
// EXACTLY: no plug => the corridor field is bit-identical to the pre-cut
// build; no hole => the tile mesher emits the exact pre-hole triangle set.
constexpr float kTcPlugFall = 2.5f;   // the mouth transition happens over this
constexpr float kTcPlugBack = 8.0f;   // plug spine overlap back into the open cut
// Run past the last open-cut node. The plug's end cap holds full depth for
// kTcCorridorHalfW past its last node, so the actual face sits at
// kTcPlugRun + 8.8 m — which must exceed the MAIN corridor's influence reach
// (halfWidth + falloff = 18.8 m past the same node) or the main's gentle tail
// re-creates the shallow ramp beyond the plug's sharp face.
constexpr float kTcPlugRun  = 14.0f;
static_assert(kTcPlugRun + kTcCorridorHalfW > kTcCorridorHalfW + kTcCorridorFall,
              "the plug face must clear the main corridor's end-cap reach");
// CAP THE PLUG'S CARVE — the torn-mountain fix. The plug's node depths are
// authored as (latMax at the probe) - road: "cut the natural surface down to
// road level". Under the original 55 m hummock that difference was modest;
// under the rock-relief massif (RangeDef amp 285 / jagAmp 58, ~162 m peak)
// the FINAL node's face-anticipation probe (one cap-length deeper in) read
// 44.6 m above the road at the entry mouth, and the unclamped plug slashed a
// road-width canyon straight down through the mountain face — a vertical
// gash above the portal (floor up to 19 m BELOW road), with stalactite
// shards where the portal hole then dropped the canyon-wall triangles (a
// wall triangle's LOWEST vertex dips under yTop however high its top
// reaches). Same failure shape as the road-shaped summit trench
// (TUNNEL_NEXT.md §4, fixed by depth 0 on bored reaches), same cure as
// kTcMaxScar: a hard cap on removal.
//
// THE VALUE IS MEASURED, NOT TASTE, and it is a compromise between two
// owner-verified defects:
//   * too high (unclamped, 44.6) => the torn mountain;
//   * too low (first cut, 14.0) => the mouth zone (measured s=104..110 entry,
//     s=526..532 exit) keeps up to 7.6 m of field ABOVE the road — the
//     concrete apron/shoulder and the fillet at each mouth are buried under
//     grass ("the concrete aprons are gone"), and the trench look of the
//     approach cutting dies at the portal instead of running into it.
// The genuine cut-to-road needs through the mouth zones on this route are
// 12.7 / 17.8 / 25.6 (entry) and 7.5 / 10.9 / 18.2 (exit) over the corridor
// band, and <= 21.6 over the ROAD band — the same scale as the open cut's
// own deepest station need (18.2 m). 24 clears the road band and the aprons
// to the face and binds ONLY the face-anticipation overshoot (44.6 -> 24),
// so the portal face is a cutting wall of the scale the approach already
// shows, not a canyon. The residual sweep past the cap lands inside the
// portal holes' reach (B2 asserts it).
constexpr float kTcPlugMaxCut = 24.0f;

bool portalCutOn() {
    const char* e = std::getenv("X3_TUNNEL_PORTAL_CUT");
    return !(e && e[0] == '0');
}

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
    // `s` is arc length from node 0, which sits at -halfLen about this route's
    // own centre; fold it back to the centre-relative parameter. These were
    // file-scope constants, which is exactly what allowed only one tunnel.
    out[0] = cx + dirX * (s - halfLen);
    out[2] = cz + dirZ * (s - halfLen);
    out[1] = roadYAt(s);
}

// ---------------------------------------------------------------------------
// registerTunnelCorridor — the BOOT step. Sample, grade, register, verify.
// ---------------------------------------------------------------------------
namespace {
// Every route registered this boot. Fixed capacity to match the terrain
// registry's own contract (kMaxTerrainCorridors); stable addresses because
// callers hold the returned pointer.
std::vector<TunnelRoute>& routeStore() { static std::vector<TunnelRoute> v; return v; }
} // namespace

uint32_t           tunnelRouteCount()          { return (uint32_t)routeStore().size(); }
const TunnelRoute* tunnelRouteAt(uint32_t i)   { return i < routeStore().size() ? &routeStore()[i] : nullptr; }

const TunnelRoute* registerTunnelCorridorFor(const TunnelSpec& spec) {
    if (routeStore().size() >= kMaxTerrainCorridors) {
        x3::logError(std::string("tunnel corridor: registry full — '") + spec.name + "' not registered");
        return nullptr;
    }
    const float dmag = std::sqrt(spec.dirX*spec.dirX + spec.dirZ*spec.dirZ);
    if (!(dmag > 1e-4f) || !(spec.halfLen > 1.0f)) {
        x3::logError(std::string("tunnel corridor: degenerate spec '") + spec.name + "'");
        return nullptr;
    }
    routeStore().reserve(kMaxTerrainCorridors);
    routeStore().emplace_back();
    TunnelRoute& route = routeStore().back();

    const float kRouteCX = spec.cx, kRouteCZ = spec.cz;
    const float kRouteDirX = spec.dirX / dmag, kRouteDirZ = spec.dirZ / dmag;
    const float kRouteHalfLen = spec.halfLen;
    route.cx = kRouteCX; route.cz = kRouteCZ;
    route.halfLen = kRouteHalfLen; route.name = spec.name;
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
        // KEEP CUTTING PAST THE POINT OF ADEQUACY. Switching to zero excavation
        // the instant cover is barely sufficient is what plugs the mouth: the
        // ground still has to sweep from road level up over the crown, and if the
        // corridor stops cutting right there, that sweep lands INSIDE the bore.
        // The result is a drivable road with an earth ramp across it — the
        // "portal-sweep residual" this file has been reporting all along (18-24 m
        // of buried road). It is not cosmetic: it WALLS THE TUNNEL OFF. Tim drove
        // to the mouth and found it packed with grass.
        //
        // Requiring a MARGIN of surplus cover before excavation stops pushes that
        // transition deeper under the hill, where the sweep is buried by real
        // mountain instead of crossing the carriageway.
        if (coverAvail >= kBoreCut + kBoreCutMargin) {
            // BORED reach: take only the surplus above the tube's crown + soil,
            // and never more than kTcMaxScar — that cap is what keeps the ridge
            // a dip instead of a canyon (BL_WORLD_PORT.md §4.3b).
            // NOTHING TO REMOVE. The tube already fits under the natural ground
            // here, so the corridor must not excavate at all.
            //
            // This previously took `min(coverAvail - kBoreCut, kTcMaxScar)`,
            // reasoning that the surplus above the crown should be shaved and the
            // cap would keep "the ridge a dip instead of a canyon". That holds for
            // a 55 m hummock, where coverAvail is small. Under a 124 m mountain
            // coverAvail is enormous, so the term pinned at kTcMaxScar and the
            // corridor gouged a kTcMaxScar-deep ROAD-SHAPED TRENCH along the
            // summit — visible as a cutout across the mountain top, with its own
            // walls and tile skirts showing as dark strips down the slope.
            //
            // A tunnel does not carve the mountain above it. Depth 0.
            n.depth = 0.0f;
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
    if (!ok) { x3::logError("tunnel corridor: registerTerrainCorridor REJECTED the route"); return &route; }

    // --- 4b) PORTAL PLUGS — one tight full-cut corridor per mouth. ----------
    // See the block comment at kTcPlugFall for why the main corridor cannot do
    // this itself. Depths are authored against the same NATURAL samples the
    // main profile used (latMax interpolated between stations), never against
    // the already-cut field — corridor depths are relative to the natural
    // surface and combine by deepest-wins.
    uint32_t plugs = 0;
    if (portalCutOn()) {
        auto latMaxAt = [&](float s) {
            if (s <= route.st.front().s) return route.st.front().latMax;
            if (s >= route.st.back().s)  return route.st.back().latMax;
            for (size_t i = 1; i < route.st.size(); ++i)
                if (s <= route.st[i].s) {
                    const float t = (s - route.st[i-1].s) /
                                    std::max(1e-4f, route.st[i].s - route.st[i-1].s);
                    return route.st[i-1].latMax + (route.st[i].latMax - route.st[i-1].latMax) * t;
                }
            return route.st.back().latMax;
        };
        auto registerPlug = [&](float sOpen, float inward) {
            TerrainCorridor plug{};
            plug.nodeCount = 4;
            plug.halfWidth = kTcCorridorHalfW;
            plug.falloff   = kTcPlugFall;
            const float sA = sOpen - inward * kTcPlugBack;
            const float sB = sOpen + inward * kTcPlugRun;
            for (int k = 0; k < plug.nodeCount; ++k) {
                const float t = (float)k / (float)(plug.nodeCount - 1);
                const float s = sA + (sB - sA) * t;
                const float sc = -kRouteHalfLen + s;
                plug.x[k] = kRouteCX + kRouteDirX * sc;
                plug.z[k] = kRouteCZ + kRouteDirZ * sc;
                // The union's rounded end cap FREEZES the final node's depth
                // for halfWidth metres past it, while the mountain's natural
                // surface keeps rising — measured at ~1.2-2 m per metre on this
                // flank — so a locally-sampled final depth leaves the floor
                // rising at the natural slope across the whole cap zone (that
                // was the 118..124 m shallow ramp in the probe). The FINAL node
                // therefore anticipates: its depth is sampled at the FACE (one
                // cap-length further in), which over-cuts a scoop just outside
                // the portal but pins the floor at/below road level right up to
                // the face, collapsing the rise into the 2.5 m falloff band.
                const float sProbe = (k == plug.nodeCount - 1)
                                         ? s + inward * plug.halfWidth : s;
                // Clamped: cut to road level, but never remove more than
                // kTcPlugMaxCut of mountain — see the constant for the tear
                // this cap exists to prevent (measured unclamped: 44.6 m at
                // the entry face under the massif).
                plug.depth[k] = clampf(latMaxAt(sProbe) - route.roadYAt(s),
                                       0.0f, kTcPlugMaxCut);
            }
            char pb[192];
            std::snprintf(pb, sizeof(pb),
                "tunnel corridor: portal plug @s=%.0f (inward %+.0f) node depths "
                "%.1f / %.1f / %.1f / %.1f m",
                sOpen, inward, plug.depth[0], plug.depth[1], plug.depth[2], plug.depth[3]);
            x3::logInfo(pb);
            if (registerTerrainCorridor(plug)) ++plugs;
            else x3::logWarn(std::string("tunnel corridor: '") + route.name +
                             "' portal plug REJECTED (registry full?) — the mouth will ramp");
        };
        for (int i = 0; i + 1 < kRouteNodes; ++i) {
            if (route.st[i].bore == route.st[i+1].bore) continue;
            if (!route.st[i].bore) registerPlug(route.st[i].s,     +1.0f);   // entry mouth
            else                   registerPlug(route.st[i+1].s,   -1.0f);   // exit mouth
        }
    }

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
    {
        std::string runs;
        int runStart = -1;
        for (int i = a2; i <= b2 + 1; ++i) {
            const bool buried = i <= b2 && roadBury[(size_t)i] > 0.30f &&
                                roadBury[(size_t)i] < kTcTubeCrownH;
            if (buried) { buriedIn += kSampleDs; if (runStart < 0) runStart = i; }
            else if (runStart >= 0) {
                char rb[64];
                std::snprintf(rb, sizeof(rb), " s=%.0f..%.0f(max %.1f m)",
                              kSampleDs * (float)runStart, kSampleDs * (float)(i - 1),
                              [&]{ float m = 0; for (int k = runStart; k < i; ++k)
                                       m = std::max(m, roadBury[(size_t)k]); return m; }());
                runs += rb;
                runStart = -1;
            }
        }
        if (!runs.empty())
            x3::logInfo(std::string("tunnel corridor: earth-ramp runs inside the shell:") + runs);
    }
    route.buriedRoadLen = buriedIn;

    // --- 5b) PORTAL HOLES — clear the residual curtain from the tile MESH. --
    // Whatever residual the field still carries (the plug shrinks it to a few
    // metres; it can never be zero — see terrain.h TerrainPortalHole), the
    // mesher must not emit those triangles: they are terrain collision across
    // the carriageway. The hole spans the measured sweep at each mouth, from
    // just outside the shell start to just past full cover, capped so a
    // pathological measurement can't tear a hole down half the bore.
    uint32_t holes = 0;
    if (portalCutOn() && route.boreValid) {
        auto registerHole = [&](float sH0, float sH1) {
            if (sH1 - sH0 < 1.0f) return;
            TerrainPortalHole h{};
            const float scA = -kRouteHalfLen + sH0, scB = -kRouteHalfLen + sH1;
            h.x0 = kRouteCX + kRouteDirX * scA; h.z0 = kRouteCZ + kRouteDirZ * scA;
            h.x1 = kRouteCX + kRouteDirX * scB; h.z1 = kRouteCZ + kRouteDirZ * scB;
            h.halfWidth = kTcTubeHalfWidth + kTcShellThick + 0.15f;
            float ryMax = -1e9f;
            for (float s = sH0; s <= sH1 + 0.01f; s += 2.0f)
                ryMax = std::max(ryMax, route.roadYAt(s));
            h.yTop = ryMax + kTcTubeCrownH + kTcShellThick + 0.4f;
            if (registerTerrainPortalHole(h)) ++holes;
            else x3::logWarn(std::string("tunnel corridor: '") + route.name +
                             "' portal hole REJECTED (registry full?) — the mouth curtain stays");
        };
        constexpr float kHoleCap = 30.0f;   // never tear further than this
        registerHole(std::max(0.0f, route.boreS0 - 6.0f),
                     std::min(route.boreS0 + kHoleCap, route.coverS0 + 3.0f));
        registerHole(std::max(route.boreS1 - kHoleCap, route.coverS1 - 3.0f),
                     std::min(route.totalLen, route.boreS1 + 6.0f));
        if (route.coverS0 - route.boreS0 > kHoleCap || route.boreS1 - route.coverS1 > kHoleCap)
            x3::logWarn(std::string("tunnel corridor: '") + route.name +
                        "' mouth sweep exceeds the portal-hole cap — part of the ramp keeps its mesh");
    }

    char b[512];
    std::snprintf(b, sizeof(b),
        "tunnel corridor: registered %u corridor(s) (%u portal plug(s)) + %u portal hole(s), "
        "%d nodes, %.0f m, halfWidth %.1f/%.1f | road %.1f..%.1f m | max cut %.1f m | "
        "SHELL s=%.0f..%.0f (%.0f m), of which fully buried s=%.0f..%.0f (%.0f m); "
        "%.0f m of road inside the shell still carries an earth ramp in the FIELD "
        "(the portal-sweep residual; the portal holes clear it from the MESH)",
        1u + plugs, plugs, holes,
        kRouteNodes, route.totalLen, kTcCorridorHalfW, kTcCorridorFall,
        route.st.front().roadY, route.st.back().roadY,
        [&]{ float m = 0; for (auto& n : route.st) m = std::max(m, n.latMax - n.roadY); return m; }(),
        route.boreS0, route.boreS1, route.boreS1 - route.boreS0,
        route.coverS0, route.coverS1, route.coverS1 - route.coverS0, route.buriedRoadLen);
    x3::logInfo(b);
    if (!route.boreValid)
        x3::logWarn(std::string("tunnel corridor: '") + route.name +
                    "' found no enclosed span — no hill on this heading, so it is an open cutting only");
    return &route;
}

// The original single-tunnel demo (--world tunnel). Same authored hill, same
// constants, so the demo's geometry is unchanged by the generalisation.
TunnelSpec demoTunnelSpec() {
    TunnelSpec demo;
    demo.name = "demo ridge";
    demo.cx = kRouteCX;   demo.cz = kRouteCZ;
    demo.dirX = kRouteDirX; demo.dirZ = kRouteDirZ;
    demo.halfLen = kRouteHalfLen;
    return demo;
}

const TunnelRoute& registerTunnelCorridor() {
    static const TunnelRoute* cached = nullptr;
    if (cached) return *cached;
    cached = registerTunnelCorridorFor(demoTunnelSpec());
    if (!cached) { static TunnelRoute empty; return empty; }
    return *cached;
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
    const SurfaceSet& shoulderSet = m_surf.get(device, "cc_porous_cement");
    // The embankment FILLET is graded rubble, not poured concrete — terrain_scree
    // (Top Down Post Apocalyptic terrain set, curated 2026-08-13) so the slope
    // from the shoulder down to the trench floor reads as earthworks.
    const SurfaceSet& screeSet  = m_surf.get(device, "terrain_scree");
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
    // FEATHER THE ENDS INTO THE GROUND. The route stops at a hard boundary, and
    // the ribbon is a THICK slab (kSlabDrop) — so at s=0 and s=totalLen it was
    // simply cut off mid-section, leaving the slab's end face standing in the
    // air over the downhill shoulder with a triangular void beneath it. Against
    // convincing rock that reads as an unfinished mesh, which is exactly how it
    // was reported. A real road does not begin at a cliff edge: it EMERGES from
    // the surface. Easing the last few metres up to natural grade closes the
    // void because the slab's top meets the ground it was hanging over.
    {
        constexpr float kFeather = 16.0f;   // run over which the road surfaces
        for (Frame& f : roadFrames) {
            const float dEnd = std::min(f.s, route.totalLen - f.s);
            if (dEnd >= kFeather) continue;
            const float t = 1.0f - clampf(dEnd / kFeather, 0.0f, 1.0f);   // 1 AT the end
            const float g = terrainHeightAtWorld(f.p[0], f.p[2]);
            // Only ever raise: dropping the road here would re-open the gap on
            // the uphill side, and the corridor is already cut to this datum.
            if (g > f.p[1]) f.p[1] += (g - f.p[1]) * t * t;   // eased, no kink
        }
    }

    // ================= 1) THE ROAD RIBBON ===================================
    // A swept slab. It is thick (kSlabDrop) on purpose: the corridor's depth is
    // constant across its flat floor, so the floor still carries the hillside's
    // LATERAL slope. The slab's skirt swallows that mismatch and reads as a
    // retaining kerb where the cut is deepest.
    {
        constexpr float kSlabProud = 0.14f;
        MeshBuf mb;
        MeshBuf shoulder;   // flat concrete hard shoulder (own material)
        MeshBuf fillet;     // drivable embankment down to the trench floor (scree)
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

        // ---- SHOULDER + DRIVABLE EMBANKMENT FILLET --------------------------
        // History, because this block has now been wrong twice in opposite
        // directions: v1 battered concrete over the hillside (quarry apron),
        // v2 restricted the batter to FILL only — which stopped the apron
        // climbing cutting faces, but left the road edge in a CUT as a bare
        // VERTICAL face wherever the trench floor sits below the road (the
        // corridor removes a constant depth, so the downhill half of the floor
        // is below the datum). Lane 7 SURVEYED that face: 0.45 m min, 2.65 m
        // mean, 6.81 m max, against ~0.43 m of chassis clearance — the road
        // was unmountable from the ditch along its whole length. v2's batter
        // was also 0.80 m/m (~39 deg), unclimbable even where it did run.
        //
        // v3, this version: a flat concrete hard shoulder (cut and fill
        // alike), then a FILLET that always bridges the shoulder edge down to
        // whatever ground is below it — fill slope or over-cut trench floor —
        // at kFilletSlope (~19 deg; Lane 7 proved 20 deg climbable with the
        // current tyres, so this is a measured bound, not taste). The fillet
        // never climbs: the moment the ground at its inner edge has risen to
        // meet the rail (a cutting face), it stops — that guard is what keeps
        // v1's strips-up-the-mountain dead.
        {
            constexpr float kShoulderW    = 1.5f;    // concrete hard shoulder
            constexpr float kFilletSlope  = 0.34f;   // fall per metre out (~19 deg, drivable)
            constexpr float kFilletStep   = 1.6f;
            constexpr int   kFilletSteps  = 18;      // reach 28.8 m, drop capacity ~9.8 m
            for (size_t j = 0; j + 1 < roadFrames.size(); ++j) {
                const Frame& a = roadFrames[j]; const Frame& b = roadFrames[j+1];
                auto gAt = [&](const Frame& f, float lat) {
                    return terrainHeightAtWorld(f.p[0] + right[0]*lat,
                                                f.p[2] + right[2]*lat);
                };
                for (int side = -1; side <= 1; side += 2) {
                    const float sg = (float)side;
                    auto emitQuad = [&](MeshBuf& buf, float latA, float y0a, float y0b,
                                        float latB, float y1a, float y1b) {
                        float p0[3] = { a.p[0] + right[0]*latA, y0a, a.p[2] + right[2]*latA };
                        float p1[3] = { a.p[0] + right[0]*latB, y1a, a.p[2] + right[2]*latB };
                        float p2[3] = { b.p[0] + right[0]*latB, y1b, b.p[2] + right[2]*latB };
                        float p3[3] = { b.p[0] + right[0]*latA, y0b, b.p[2] + right[2]*latA };
                        const float nUp[3] = { 0, 1, 0 };
                        if (side < 0) buf.quad(p3, p2, p1, p0, nUp, 0,1, a.s*0.09f, b.s*0.09f);
                        else          buf.quad(p0, p1, p2, p3, nUp, 0,1, a.s*0.09f, b.s*0.09f);
                    };
                    // The flat hard shoulder, cut and fill alike.
                    emitQuad(shoulder, sg * hw,               a.p[1] + kSlabProud, b.p[1] + kSlabProud,
                                       sg * (hw + kShoulderW), a.p[1] + kSlabProud, b.p[1] + kSlabProud);
                    // The fillet: march outward, descending at kFilletSlope,
                    // clamped up to ground (a fill must not tunnel under grade),
                    // stopping when the ground has met the rail.
                    float ya = a.p[1] + kSlabProud, yb = b.p[1] + kSlabProud;
                    float oa = kShoulderW;
                    for (int st = 1; st <= kFilletSteps; ++st) {
                        const float o2   = kShoulderW + (float)st * kFilletStep;
                        const float latA = sg * (hw + oa), latB = sg * (hw + o2);
                        // LANDED (or a cutting face): the ground at the current
                        // edge is at/above the rail. Emitting further would climb
                        // the face — the v1 bug. Stop before emitting.
                        if (gAt(a, latA) >= ya - 0.05f && gAt(b, latA) >= yb - 0.05f) break;
                        const float drop = kFilletSlope * kFilletStep;
                        // Clamp UP to ground (a fill must not tunnel under
                        // grade) but never ABOVE the rail: where the far wall
                        // of a trench rises through the fillet mid-step, an
                        // unclamped max() emits a vertical sheet up the wall —
                        // the survey measured it at 4.48 m. Capped at the rail
                        // the last quad runs level into the wall and is buried.
                        const float y1a = std::min(ya, std::max(ya - drop, gAt(a, latB)));
                        const float y1b = std::min(yb, std::max(yb - drop, gAt(b, latB)));
                        emitQuad(fillet, latA, ya, yb, latB, y1a, y1b);
                        ya = y1a; yb = y1b; oa = o2;
                        if (y1a <= gAt(a, latB) + 1e-3f &&
                            y1b <= gAt(b, latB) + 1e-3f) break;   // both rails at grade
                    }
                }
            }
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
        // The shoulder is its own draw so it can be CONCRETE against the
        // asphalt — the edge has to read as a hard shoulder easing into dirt,
        // not as more road. Collides: it is walkable/driveable-onto ground.
        {
            Material sm2;
            if (shoulderSet.ok) { sm2.alb = shoulderSet.albedo; sm2.mr = shoulderSet.mr;
                                  sm2.nrm = shoulderSet.normal; }
            else                { sm2.alb = concreteTex; sm2.mr = wallMR; }
            upload(shoulder, sm2, /*collide*/true);
        }
        // The fillet collides too — it exists so a car in the ditch can DRIVE
        // back onto the road (Lane 7's road-mount acceptance), not just to
        // close the visual gap.
        {
            Material fm;
            if (screeSet.ok) { fm.alb = screeSet.albedo; fm.mr = screeSet.mr;
                               fm.nrm = screeSet.normal; }
            else             { fm.alb = concreteTex; fm.mr = wallMR;
                               fm.tint[0] = 0.62f; fm.tint[1] = 0.58f; fm.tint[2] = 0.52f; }
            upload(fillet, fm, /*collide*/true);
        }

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

                // ---- SPLAYED WING WALLS ------------------------------------
                // THE PORTAL-SWEEP RESIDUAL, fixed the way real portals fix it.
                // The corridor's depression spans halfWidth+falloff (8.8+10.0 =
                // 18.8 m from the spine) but the headwall only caps +/-12.8 m.
                // In that 6 m band either side the ground sweeps continuously
                // from road level up to natural grade — and because a
                // single-valued heightfield CANNOT step vertically, that sweep
                // necessarily crosses the bore. Visually: grass climbing into
                // the arch, which no amount of headwall height fixes (raising
                // it just stands the slab proud of the hill like a billboard).
                //
                // A real tunnel portal does not fight that with a taller slab.
                // It RETAINS the earth with wing walls splayed back from the
                // headwall edges. That is what these are: from each side of the
                // headwall, a panel running outward to the falloff edge and
                // BACKWARD along the cutting, holding the bank off the arch.
                {
                    // How far a wing may ever stand above the ground it retains.
                    // A retaining wall shows a course or two of freeboard; more
                    // than that and it reads as a slab dropped on the hillside.
                    constexpr float kWingProud = 2.6f;
                    const float wingOut = kTcCorridorHalfW + kTcCorridorFall;  // 18.8 m
                    const float wingRun = 11.0f;      // how far back it splays
                    const float wdir = (end == 0) ? -1.0f : 1.0f;
                    // SEGMENTED, because a bank is not a straight line. The first
                    // version sampled the ground only at the root and the tip and
                    // drew one quad between them: on a curved hillside that top
                    // edge cannot follow the bank, so the wall stood proud of the
                    // slope as a floating slab. Walking it in steps and sampling
                    // the terrain at each one lets the coping ride the ground.
                    const int kWingSegs = 8;
                    for (int side = -1; side <= 1; side += 2) {
                        float prev[4][3]; bool havePrev = false;
                        for (int i = 0; i <= kWingSegs; ++i) {
                            const float t  = (float)i / (float)kWingSegs;
                            const float sx = (float)side * (rectHalfW + (wingOut - rectHalfW) * t);
                            const Frame f  = frameAt(sEnd + wdir * wingRun * t);
                            const float px = f.p[0] + right[0]*sx, pz = f.p[2] + right[2]*sx;
                            const float g  = terrainHeightAtWorld(px, pz);
                            // Top: headwall height at the root, easing to a little
                            // above the local bank by the tip. Clamped so it can
                            // never stand more than kWingProud above the ground it
                            // is retaining — that clamp is what kills the billboard.
                            const float want = (f.p[1] + rectTop) * (1.0f - t) + (g + 1.0f) * t;
                            const float top  = std::min(want, g + kWingProud);
                            const float bot  = std::min(g, f.p[1]) - 5.0f;
                            float cur[4][3] = {
                                { px, bot, pz }, { px, top, pz },
                                { px + right[0]*0.55f*(float)side, top, pz + right[2]*0.55f*(float)side },
                                { px + right[0]*0.55f*(float)side, bot, pz + right[2]*0.55f*(float)side } };
                            if (havePrev) {
                                const float nx = -(float)side;
                                const float wn[3] = { right[0]*nx, 0.0f, right[2]*nx };
                                const float upn[3] = { 0, 1, 0 };
                                portals.quad(prev[0], prev[1], cur[1], cur[0], wn,  0,1,0,1); // inner face
                                portals.quad(cur[3],  cur[2],  prev[2], prev[3], wn, 0,1,0,1); // outer face
                                portals.quad(prev[1], prev[2], cur[2], cur[1], upn, 0,1,0,1); // coping
                            }
                            for (int k = 0; k < 4; ++k) for (int j = 0; j < 3; ++j) prev[k][j] = cur[k][j];
                            havePrev = true;
                        }
                    }
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
        // lives or dies on, so it gets its own frame. The lateral offset used
        // to be 15 m, which the portal cut's near-vertical trench walls
        // (full depth beyond ~8.8 m) now put INSIDE solid rock — stay on the
        // trench floor, off the centreline but inside the cutting.
        const float sEye = std::max(4.0f, route.boreS0 - 30.0f);
        float e[3]; route.posAt(sEye, e);
        const float lat = 5.5f;
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

// ===========================================================================
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

} // namespace x3::game
