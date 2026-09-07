// THE UNDERGROUND RIVER — see underground_river.h. The trench itself is
// terrain.cpp's carve; this file builds the rock vault, the water and the
// light, and carries the --test-underriver gate.

#include "underground_river.h"
#include "terrain.h"
#include "asset_root.h"
#include "river_rapids.h"     // reach table + boulders (X3_RIVER_RAPIDS door)
#include "mesh_prims.h"       // makeUVSphere: the boulders' base geometry
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Deterministic value jitter (the mine_fx bore's trick): rocky displacement
// that is identical every boot and on every thread.
inline float rj(float a, float b) {
    uint32_t h = (uint32_t)((int)(a * 37.31f) * 374761393
                          + (int)(b * 17.77f) * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
}

// THE VAULT'S INNER SURFACE — the cavern's ceiling. ONE formula with TWO
// callers (NO_SLOP rule 4): buildStrip() draws it, gate U9 measures headroom
// against it. `jitter=false` returns the LOWEST the rough ceiling can ever
// hang at that point, which is what a clearance gate has to assume.
//   ground = worldPreUnderRiverHeight (the surface the lid restores)
//   waterY = the river surface here;  u = 0..1 across the span
// Displacement only ever hangs DOWN into the void, scaled by the room there,
// and tapers to nothing at the rim so the lid meets the country flush.
inline float vaultCeilingY(float ground, float waterY, float u,
                           float s, int k, bool jitter) {
    const float archT = std::sin(u * 3.14159265f);
    const float room  = std::max(ground - (waterY + kURShelfLift), 0.0f);
    const float amp   = std::min(2.6f, room * 0.12f) * archT;
    const float j = jitter ? (rj(s * 0.13f + 3.7f, (float)k * 2.1f) * 0.5f + 0.5f)
                           : 1.0f;
    return ground - 0.30f * archT - j * amp;
}

// THE APRON'S LATERAL SAMPLE TABLE — ONE producer, two consumers (build() and
// gate U10; NO_SLOP rule 4). Uniform samples across the carved band PLUS a
// vertex planted exactly on each of the carve's two CREASES.
//
// Why the creases matter: terrain.cpp shapes the trench with two smoothsteps
// that meet — bed->beach over [bedW, shelfW], beach->country over [shelfW,
// kURWallOutW]. Where they meet the field is C1-discontinuous, a real kink,
// and ANY flat chord spanning a concave kink passes under the ground. Measured
// at 2.2 m uniform spacing: 175 of 6720 apron cells pierced, worst 0.280 m —
// which is what drew a diagonal band of splat GRASS along the beach in three
// consecutive captures while every other gate stayed green. A bigger lift only
// buries the player deeper; a denser mesh only shrinks the sag. A vertex ON
// the crease removes it.
inline void apronLats(std::vector<float>& out) {
    out.clear();
    constexpr int kUniform = 41;
    for (int k = 0; k < kUniform; ++k)
        out.push_back(((float)k / (kUniform - 1) * 2.0f - 1.0f) * kURWallOutW);
    // The creases, both banks. bedW/shelfW mirror the carve exactly; they are
    // constants now only because kURHalfWidth is (see terrain.h).
    const float bedW   = std::max(kURBedHalfW,   kURHalfWidth + 0.8f);
    const float shelfW = std::max(kURShelfHalfW, bedW + 3.5f);
    for (float c : { bedW, shelfW }) { out.push_back(c); out.push_back(-c); }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(),
                          [](float a, float b) { return std::fabs(a - b) < 0.05f; }),
              out.end());
}

struct CpuMesh {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
};

// MITERED CROSS-SECTION. Both the lid and the apron are laid out as (along,
// lateral) strips, and the naive lateral axis is the containing SEGMENT's
// perpendicular. But the CARVE is defined by true distance to the polyline, so
// at a bend the two disagree: on the outside of a turn a vertex placed 44 m
// perpendicular sits nearer than 44 m to the polyline, and the strip's rim
// falls SHORT of the carved rim. The apron therefore stopped before the trench
// wall did and left a one-sided band of carved-but-unskinned ground showing
// the height/slope splat — GRASS up the right bank of every bend, with a
// dead-straight mesh edge against the rock. (Same defect would leave the vault
// lid short of the trench mouth, i.e. daylight into the cavern.)
//
// The fix is the standard one for a swept ribbon: near a node blend the
// lateral axis toward the MITER of the two segment perpendiculars and scale
// the offset by 1/cos(half-turn), which puts the rim back on true distance.
struct Frame { float x, z, px, pz, scale; };

// Walk the chain: position/value interpolation at along-length s, plus the
// unit direction of the containing segment.
struct ChainWalk {
    const UnderRiverChain& c;
    explicit ChainWalk(const UnderRiverChain& uc) : c(uc) {}
    void at(float s, float& x, float& z, float& w, float& nat,
            float& dx, float& dz) const {
        s = std::clamp(s, 0.0f, c.cum[c.n - 1]);
        int i = 0;
        while (i + 2 < c.n && c.cum[i + 1] < s) ++i;
        const float seg = std::max(c.cum[i + 1] - c.cum[i], 1e-3f);
        const float t = std::clamp((s - c.cum[i]) / seg, 0.0f, 1.0f);
        x = c.x[i] + (c.x[i + 1] - c.x[i]) * t;
        z = c.z[i] + (c.z[i + 1] - c.z[i]) * t;
        w = c.w[i] + (c.w[i + 1] - c.w[i]) * t;
        nat = c.natural[i] + (c.natural[i + 1] - c.natural[i]) * t;
        dx = (c.x[i + 1] - c.x[i]) / seg;
        dz = (c.z[i + 1] - c.z[i]) / seg;
    }

    // Unit direction of segment i (clamped at the ends).
    void segDir(int i, float& ux, float& uz) const {
        i = std::clamp(i, 0, c.n - 2);
        const float dx = c.x[i + 1] - c.x[i], dz = c.z[i + 1] - c.z[i];
        const float L = std::max(std::sqrt(dx * dx + dz * dz), 1e-4f);
        ux = dx / L; uz = dz / L;
    }

    Frame frameAt(float s) const {
        s = std::clamp(s, 0.0f, c.cum[c.n - 1]);
        int i = 0;
        while (i + 2 < c.n && c.cum[i + 1] < s) ++i;
        const float seg = std::max(c.cum[i + 1] - c.cum[i], 1e-3f);
        const float t = std::clamp((s - c.cum[i]) / seg, 0.0f, 1.0f);
        Frame f{};
        f.x = c.x[i] + (c.x[i + 1] - c.x[i]) * t;
        f.z = c.z[i] + (c.z[i + 1] - c.z[i]) * t;
        float ux, uz; segDir(i, ux, uz);
        const float px = -uz, pz = ux;              // this segment's perpendicular
        // Blend toward the miter over the last/first `blend` metres of the run
        // into each node, so the lateral axis turns smoothly through the bend.
        const float blend = std::min(28.0f, seg * 0.5f);
        float bx = px, bz = pz;
        auto mixToward = [&](int other, float k) {
            float ox, oz; segDir(other, ox, oz);
            const float qx = -oz, qz = ox;
            bx += (qx - px) * k; bz += (qz - pz) * k;
        };
        const float dIn  = s - c.cum[i];            // distance past this node
        const float dOut = c.cum[i + 1] - s;        // distance to the next node
        if (i > 0 && dIn < blend)
            mixToward(i - 1, 0.5f * (1.0f - dIn / blend));
        if (i + 2 < c.n && dOut < blend)
            mixToward(i + 1, 0.5f * (1.0f - dOut / blend));
        const float bl = std::max(std::sqrt(bx * bx + bz * bz), 1e-4f);
        f.px = bx / bl; f.pz = bz / bl;
        // Offsets along the mitered axis must grow by 1/cos(half-turn) to land
        // back on the same TRUE distance the carve uses. Clamped so a hairpin
        // cannot blow the strip up.
        const float cosH = std::max(f.px * px + f.pz * pz, 0.35f);
        f.scale = 1.0f / cosH;
        return f;
    }
};

} // namespace

UndergroundRiver::Result UndergroundRiver::build(
        Scene& scene, x3::rhi::IRenderDevice& device,
        SurfaceLibrary* surfIn, std::vector<x3::rhi::PointLight>* outLights,
        x3::phys::IPhysicsWorld* phys) {
    Result r{};
    const UnderRiverChain& uc = worldUnderRiverChain();
    if (uc.n < 2) return r;
    const float total = uc.cum[uc.n - 1];
    r.portalX = uc.x[uc.n - 1]; r.portalZ = uc.z[uc.n - 1];

    SurfaceLibrary localSurf;
    SurfaceLibrary& surf = surfIn ? *surfIn : localSurf;
    if (!surf.mounted()) surf.mount(assetRoot() + "/surface_library");
    // THE VAULT'S SKIN. This was cv_rock_wet, which despite its name is a
    // tiled MASONRY texture — dressed blocks with mortar lines — and at the
    // lid's 9 m x 20 m tile it photographed as brown ceiling PANELS with the
    // grout showing (the owner's first note on the Great Hall still). A cave
    // is carved from the country it sits in, so the inner face wears the same
    // natural rock the back of the lid and the hillside do: no seams that
    // repeat, no block edges, and the lamps' pools land on stone.
    const SurfaceSet& innerS = surf.get(device, "terrain_rock");   // cave rock (natural)
    const SurfaceSet& outerS = surf.get(device, "terrain_rock");   // dry country rock

    // ---- THE VAULT: the LID that puts the hillside back. ------------------
    // Cut-and-cover's cover. The trench carve pulled the country down to the
    // water; this restores the surface it removed, so from outside the hill is
    // shut and from inside the void between carved floor and restored surface
    // IS the cavern — a lens, tallest over the channel, closing to nothing at
    // the band edge. That is why every lid vertex SAMPLES
    // worldPreUnderRiverHeight (the ground before this trench existed) rather
    // than arching between two feet: the first build arched from foot to foot
    // through a single crown, which on the west valley's sloped flanks bulged
    // the crown up to 15 m above the hillside it was supposed to hide.
    const float vaultEnd = total - kURGorgeLen;
    const ChainWalk walk(uc);
    constexpr int   kAcross = 13;      // ring verts across the lid
    constexpr float kStep = 10.0f;     // ring spacing (m)
    const float kFootOut = kURWallOutW + 2.0f;   // feet just outside the carve
    auto buildStrip = [&](float s0, float s1, bool inner) {
        CpuMesh m;
        const int rings = std::max(2, (int)((s1 - s0) / kStep) + 1);
        std::vector<float> ringC;                   // (cx, w, cz) per ring
        ringC.reserve((size_t)rings * 3);
        for (int rg = 0; rg < rings; ++rg) {
            const float s = s0 + (s1 - s0) * ((float)rg / (float)(rings - 1));
            float cx, cz, w, nat, dx, dz;
            walk.at(s, cx, cz, w, nat, dx, dz);
            ringC.insert(ringC.end(), { cx, w, cz });
            const Frame fr = walk.frameAt(s);       // mitered: see Frame
            const float px = fr.px, pz = fr.pz;
            for (int k = 0; k < kAcross; ++k) {
                const float u = (float)k / (float)(kAcross - 1);   // 0..1 across
                const float lat = (u * 2.0f - 1.0f) * kFootOut * fr.scale;
                const bool  foot = (k == 0 || k == kAcross - 1);
                // Lateral jitter first: the lid is sampled AT the jittered
                // point, so a rough edge still lands on the real ground.
                const float latJ = lat + (foot ? 0.0f
                                        : rj((float)k * 5.3f, s * 0.21f) * 1.6f);
                const float vx = cx + px * latJ, vz = cz + pz * latJ;
                const float ground = worldPreUnderRiverHeight(vx, vz);
                // EVERY offset tapers to nothing at the rim (sin over the
                // span). The carve already eases back to the natural country
                // by kURWallOutW, so a lid that stayed a constant 0.35 m proud
                // would draw a 1.7 km rock lip along both edges of the
                // corridor; converging both skins onto the ground there closes
                // the lens instead, and the feet tuck the seam under.
                const float archT = std::sin(u * 3.14159265f);
                float y;
                if (foot) {
                    y = ground - 1.4f;                 // tucked under the country
                } else if (inner) {
                    y = vaultCeilingY(ground, w, u, s, k, true);   // see the helper
                } else {
                    y = ground + (0.35f + rj(s * 0.13f + 9.1f, (float)k * 2.1f)
                                          * 0.28f) * archT;
                }
                x3::rhi::MeshVertex v{};
                v.pos[0] = vx;
                v.pos[1] = y;
                v.pos[2] = vz;
                // Placeholder orientation; the real geometric normals are
                // solved from the finished grid below.
                v.normal[0] = 0.0f; v.normal[1] = inner ? -1.0f : 1.0f; v.normal[2] = 0.0f;
                // ~9 m per tile BOTH ways. The across axis used to be 4.6 tiles
                // over the ~92 m span (a 20 m tile), a 2:1 stretch that turned
                // every feature in the texture into a long panel.
                v.uv[0] = s * 0.11f;                    // ~0.11 tiles/m along
                v.uv[1] = u * 10.0f;                    // across the lid
                m.v.push_back(v);
            }
        }
        // GEOMETRIC NORMALS. The lid used to carry a flat (0,-1,0) on every
        // inner vertex: fine under the sky-IBL wash that used to fill the
        // vault, but the room is lit by point lamps now and a wall's response
        // to a lamp IS its normal — with a flat one the whole 30 m flank
        // shaded as one ceiling and the normal map had nothing to hang off.
        // Central differences over the (ring, across) grid; at the feet the
        // rim is tucked under, so one-sided there.
        for (int rg = 0; rg < rings; ++rg) {
            for (int k = 0; k < kAcross; ++k) {
                auto P = [&](int r, int kk) -> const float* {
                    r  = std::clamp(r, 0, rings - 1);
                    kk = std::clamp(kk, 0, kAcross - 1);
                    return m.v[(size_t)(r * kAcross + kk)].pos;
                };
                const float* a0 = P(rg - 1, k); const float* a1 = P(rg + 1, k);
                const float* b0 = P(rg, k - 1); const float* b1 = P(rg, k + 1);
                const float tA[3] = { a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2] };
                const float tB[3] = { b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2] };
                float n[3] = { tA[1] * tB[2] - tA[2] * tB[1],
                               tA[2] * tB[0] - tA[0] * tB[2],
                               tA[0] * tB[1] - tA[1] * tB[0] };
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                x3::rhi::MeshVertex& v = m.v[(size_t)(rg * kAcross + k)];
                if (len < 1e-6f) continue;              // keep the placeholder
                n[0] /= len; n[1] /= len; n[2] /= len;
                // Face the room: the inner skin looks IN at the water under
                // the crown (a sign test on y alone is ambiguous on the near-
                // vertical flanks, where the normal is nearly horizontal);
                // the outer skin looks UP at the sky.
                const float* c = &ringC[(size_t)rg * 3];
                const float toC[3] = { c[0] - v.pos[0], c[1] - v.pos[1], c[2] - v.pos[2] };
                const bool flip = inner ? (n[0] * toC[0] + n[1] * toC[1] + n[2] * toC[2] < 0.0f)
                                        : (n[1] < 0.0f);
                if (flip) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
            }
        }
        for (int rg = 0; rg + 1 < rings; ++rg)
            for (int k = 0; k + 1 < kAcross; ++k) {
                const uint32_t a = (uint32_t)(rg * kAcross + k);
                const uint32_t b = a + 1;
                const uint32_t c2 = a + kAcross;
                const uint32_t d2 = c2 + 1;
                if (inner) m.i.insert(m.i.end(), { a, c2, b,  b, c2, d2 });
                else       m.i.insert(m.i.end(), { a, b, c2,  b, d2, c2 });
            }
        return m;
    };
    // Chunked (~500 m per entity) so distance culling has something to cull.
    const float kChunk = 500.0f;
    for (float s0 = 0.0f; s0 < vaultEnd - 1.0f; s0 += kChunk) {
        const float s1 = std::min(s0 + kChunk, vaultEnd);
        for (int inner = 0; inner < 2; ++inner) {
            CpuMesh m = buildStrip(s0, s1, inner == 0);
            if (m.v.empty()) continue;
            Entity e;
            e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                       m.i.data(), (uint32_t)m.i.size());
            const SurfaceSet& ss = inner == 0 ? innerS : outerS;
            e.tex = ss.albedo; e.normalTex = ss.normal; e.mrTex = ss.mr;
            const float tint = inner == 0 ? 0.42f : 0.80f;   // cave rock darker
            e.baseColor[0] = tint; e.baseColor[1] = tint;
            e.baseColor[2] = tint; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            ++r.vaultChunks;
        }
    }

    // ---- THE ROCK BEACHES -------------------------------------------------
    // The shelves themselves are HEIGHT FIELD (that is the whole point of
    // cut-and-cover: the trench IS the cavern floor, so collision, streaming
    // and the CONTACT LAW need no special case down here). But the terrain
    // splat picks its material from world height + slope, and the shelf is
    // flat lowland — it would paint the owner's rock beaches GRASS, indoors.
    // So the beaches get an APRON: an authored water-worn rock skin laid 7 cm
    // over the carved shelf, the same "rides proud" idiom as the arrival decks
    // (ENGINE_GOTCHAS 3.5). Collision stays the terrain underneath, so the
    // apron cannot lift a boot off the ground it is standing on.
    // terrain_rock_grey: photographic water-worn COBBLE, matte (roughness
    // 0.90, metallic 0). NOT cv_rock_flume, whose NAME says water-worn rock
    // flume and whose PIXELS are a pictorial AI illustration of a waterfall —
    // pools, boulders and falling water baked into one scene. Tiled every few
    // metres across a beach it drew a repeating grid of little glossy
    // landscapes. I checked that file had bytes and never opened it; the brief
    // said "check what is published" and bytes are not what that means.
    // X3_WORLD_RULES rule 0 / NO_SLOP rule 2: LOOK at the asset.
    const SurfaceSet& beachS = surf.get(device, "terrain_rock_grey");
    {
        // DENSER THAN THE FIELD IT HUGS. At 13 verts over 56 m the apron's
        // chords were 4.3 m — coarser than the terrain's own 1 m LOD0 cells —
        // so across the convex knee where the beach turns up into the wall the
        // chord cut BELOW the ground and the terrain poked through, drawing a
        // band of splat GRASS along the whole run. An offset cannot fix a
        // resolution problem; matching the field can.
        std::vector<float> lats; apronLats(lats);
        const int kAcross = (int)lats.size();
        // 2.5 m, not 6. Sagitta goes as the chord SQUARED, and near a bend a
        // line of constant lateral offset sweeps ACROSS the carve's crease, so
        // the along-run chord sagged 1.04 m at 6 m spacing and cut a wedge of
        // bare splat straight through the apron (seen first as a magenta-probe
        // hole, then measured by U10 once it learned to check both axes).
        constexpr float kStep = 2.5f;      // ~2.5 m along
        // THE WHOLE CARVED BAND. Stopping short left a wedge of splat GRASS
        // between the apron's rim and the wall, and no offset or texture
        // choice fixes ground the apron simply does not cover. Out at
        // kURWallOutW the carve has met the natural country and the vault's
        // lid has converged onto it, so the join is hidden by the lid — the
        // splat is never visible from inside the cavern at all.
        const float outW = kURWallOutW;
        const float kChunkB = 320.0f;
        for (float s0 = 0.0f; s0 < total - 1.0f; s0 += kChunkB) {
            const float s1 = std::min(s0 + kChunkB, total);
            CpuMesh m;
            const int rings = std::max(2, (int)((s1 - s0) / kStep) + 1);
            for (int rg = 0; rg < rings; ++rg) {
                const float s = s0 + (s1 - s0) * ((float)rg / (float)(rings - 1));
                float cx, cz, w, nat, dx, dz;
                walk.at(s, cx, cz, w, nat, dx, dz);
                const Frame fr = walk.frameAt(s);   // mitered: see Frame
                const float px = fr.px, pz = fr.pz;
                for (int k = 0; k < kAcross; ++k) {
                    const float lat = lats[k] * fr.scale;
                    const float vx = cx + px * lat, vz = cz + pz * lat;
                    x3::rhi::MeshVertex v{};
                    v.pos[0] = vx;
                    // FIT TO WHAT IS DRAWN, NOT TO THE ANALYTIC FIELD. The
                    // renderer draws the terrain as a LOD tile mesh (1/2/4 m
                    // chords), and across a concave stretch a chord rides
                    // ABOVE the smooth field it samples — so an apron fitted
                    // to the field is pierced by the mesh, and because LOD is
                    // chosen by camera distance the defect is VIEW-DEPENDENT:
                    // the same beach was clean from eye level and showed a
                    // lobe of splat grass from 12 m up. Taking the local MAX
                    // over a 4 m stencil is what a coarse chord approaches, so
                    // the apron rises only where the mesh actually can, and
                    // stays tight to the ground everywhere else.
                    float gy = terrainHeightAtWorld(vx, vz);
                    for (int sx = -1; sx <= 1; ++sx)
                        for (int sz = -1; sz <= 1; ++sz) {
                            if (!sx && !sz) continue;
                            gy = std::max(gy, terrainHeightAtWorld(vx + sx * 4.0f,
                                                                   vz + sz * 4.0f));
                        }
                    float ay = gy + kURApronLift;
                    // INSIDE THE CHANNEL the skin must stay UNDER the river.
                    // The stencil is a deliberate over-lift and at the
                    // waterline it walked the beach out over the water and
                    // pinched the river; clamped below the surface instead, the
                    // same rock reads as the submerged BED through clarity,
                    // which is what it is. Restricting the stencil to the banks
                    // was tried first and put the grass lobe straight back —
                    // the LOD crease this is fixing sits AT the waterline.
                    if (std::fabs(lat) < kURHalfWidth) ay = std::min(ay, w - 0.05f);
                    v.pos[1] = ay;
                    v.pos[2] = vz;
                    // The apron climbs the trench wall's toe, which is a ~56
                    // deg face — a flat +Y normal there would shade the wall
                    // like a floor. Take the field's own normal.
                    terrainNormalAtWorld(vx, vz, v.normal);
                    v.uv[0] = s * 0.25f; v.uv[1] = lat * 0.25f;   // ~4 m cobble tile
                    m.v.push_back(v);
                }
            }
            for (int rg = 0; rg + 1 < rings; ++rg)
                for (int k = 0; k + 1 < kAcross; ++k) {
                    const uint32_t a = (uint32_t)(rg * kAcross + k);
                    const uint32_t b = a + 1, c2 = a + kAcross, d2 = c2 + 1;
                    // UP-FACING winding — the vault's OUTER order, not its
                    // inner one. This was copied from the inner (ceiling)
                    // strip, so every beach triangle faced the floor and was
                    // backface-culled: the apron drew nothing and both banks
                    // rendered as the terrain splat's GRASS, indoors, under a
                    // rock ceiling. Invisible to every numeric gate; obvious
                    // in the first capture.
                    m.i.insert(m.i.end(), { a, b, c2,  b, d2, c2 });
                }
            if (m.v.empty()) continue;
            Entity e;
            e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                       m.i.data(), (uint32_t)m.i.size());
            e.tex = beachS.albedo; e.normalTex = beachS.normal; e.mrTex = beachS.mr;
            // Darker than daylight rock: the cavern's ambient is unshadowed sky
        // (the lid blocks the sun, not the IBL), so a 0.74 beach washed the
        // room out and the bank lights had nothing to read against.
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.50f;
            e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            ++r.beachChunks;
        }
    }

    // ---- THE WATER is NOT drawn here. -------------------------------------
    // It is drawn by the SAME pass as the surface river: host_tunnel's
    // applyRiverWater switches WaterParams' polyline to worldUnderRiverChain()
    // whenever the focus is inside this corridor, so the cavern channel gets
    // the real Gerstner surface, clarity (you see the carved bed through it),
    // Fresnel, contact foam and caustics.
    //
    // WHAT WAS HERE, AND WHY IT WENT. The first build drew the channel with
    // CaveRiver — the club's grotto ribbon: an opaque tinted quad strip with a
    // travelling emissive crest. In a 3 m tube lit by crystals that reads as
    // water. In an 88 m cavern it photographed as flat cornflower-blue
    // construction paper with hard triangular edges and a wedge notch where
    // the strip ended: no depth, no transparency, no flow, no shading. Two
    // water implementations in one world is the duplicate rule 1 exists to
    // stop, and the one we kept is the one JOB 1 already made honest.
    // r.waterSegs stays in Result as the count of nodes handed to that pass.
    r.waterSegs = uc.n;

    // ---- THE MIST: emitters on the steps and the pools. -------------------
    // Density follows the water's own character: a rushing reach throws spray,
    // a pool just breathes. Both come from the derived rush/pool table, so a
    // reroute moves the mist with the river and nothing goes stale. With the
    // rapids on (river_rapids.h) the reach table's turbulence joins the
    // derived rush — the gorge and the plunge throw spray because the water
    // there is whitewater, and the surface the water pass draws there IS
    // whitewater; one table for both.
    const bool rapids = riverRapidsEnabled();
    m_mist.clear();
    for (float s = 0.0f; s <= total; ) {
        float cx, cz, w, nat, dx, dz;
        walk.at(s, cx, cz, w, nat, dx, dz);
        int ni = 0;
        while (ni + 1 < uc.n && uc.cum[ni + 1] < s) ++ni;
        const float t = std::clamp((s - uc.cum[ni]) /
                        std::max(uc.cum[ni + 1] - uc.cum[ni], 1e-3f), 0.0f, 1.0f);
        float rush = uc.rush[ni] +
                     (uc.rush[std::min(ni + 1, uc.n - 1)] - uc.rush[ni]) * t;
        // A reach the rapids table calls turbulent throws spray of its own:
        // twice as many sources (13 m apart, not 26), each a low, small,
        // dim burst — many droplets skimming the crests, not a few bright
        // blobs (lead's review of v1). The derived rush table still wins
        // where it is already rushing (the steps).
        const float reachTurb = rapids ? riverReachTurbulenceAt(s, total) : 0.0f;
        const bool  reachSpray = reachTurb > 0.3f;
        MistSource ms;
        if (reachSpray && 0.55f * reachTurb > rush) {
            rush = 0.55f * reachTurb;
            ms.sprayP = 1.0f; ms.sprayScale = 0.5f;
        }
        s += reachSpray ? 13.0f : 26.0f;
        const bool pool = (t < 0.5f ? uc.pool[ni] : uc.pool[std::min(ni + 1, uc.n - 1)]);
        if (rush < 0.35f && !pool) continue;      // still water does not steam
        ms.x = cx; ms.y = w + 0.25f; ms.z = cz;
        ms.dx = dx; ms.dz = dz; ms.rush = rush;
        m_mist.push_back(ms);
    }

    // ---- THE BOULDERS (feat/river-rapids). --------------------------------
    // river_rapids.h owns WHERE (fast reaches, seated on the carved bed,
    // crowns kBoulderShow above the water) and hands the same table to the
    // water pass for the foam wakes; this is the rock itself. Geometry is the
    // prim library's UV sphere, squashed (river cobble is flatter than a
    // ball), roughened by two octaves of the deterministic jitter every other
    // rock in this cavern uses, in the beach's own water-worn cobble set —
    // a boulder the river has been rolling IS a big piece of that beach. No
    // asset file: the mine's boulder GLBs live in converted_glb/, which the
    // repo does not carry, and a fresh clone must build the same river. Solid
    // as a static Jolt sphere (mass 0) when a physics world is handed over,
    // ours to remove (releaseBodies). A spray source sits in each one's lee.
    m_bodies.clear();
    if (rapids) {
        RiverBoulder rocks[x3::rhi::IRenderDevice::WaterParams::kMaxRocks];
        const uint32_t rn = underRiverBoulders(uc, rocks, x3::rhi::IRenderDevice::WaterParams::kMaxRocks);
        const SurfaceSet& rockS = surf.get(device, "terrain_rock_grey");
        for (uint32_t bi = 0; bi < rn; ++bi) {
            const RiverBoulder& rb = rocks[bi];
            x3::prims::PrimMesh sph = x3::prims::makeUVSphere(14, 22);
            CpuMesh m;
            m.v.reserve(sph.verts.size());
            const float yaw = rj(rb.s * 0.31f, 7.7f) * 3.1416f;
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            for (const x3::rhi::MeshVertex& sv : sph.verts) {
                // radial roughness: a low octave for the rock's lumps, a
                // high one for the grain; never more than +-22 % so the
                // physics sphere stays an honest hull
                const float n0 = rj(sv.pos[0] * 2.1f + rb.s, sv.pos[2] * 2.3f + sv.pos[1] * 1.7f);
                const float n1 = rj(sv.pos[0] * 6.3f + rb.lat, sv.pos[1] * 5.9f - sv.pos[2] * 6.1f);
                const float rad = rb.radius * (1.0f + 0.16f * n0 + 0.06f * n1);
                const float lx = sv.pos[0] * rad, ly = sv.pos[1] * rad * kBoulderSquash, lz = sv.pos[2] * rad;
                x3::rhi::MeshVertex o = sv;
                o.pos[0] = rb.x + cy * lx + sy * lz;
                o.pos[1] = rb.y + ly;
                o.pos[2] = rb.z - sy * lx + cy * lz;
                // ~4 m cobble tile, like the apron (uv = arc length / 4)
                o.uv[0] = sv.uv[0] * 6.2832f * rb.radius * 0.25f;
                o.uv[1] = sv.uv[1] * 3.1416f * rb.radius * 0.25f;
                m.v.push_back(o);
            }
            m.i = sph.index;
            // smooth normals from the displaced faces (the sphere's radial
            // normals would shade the lumps as if they were not there)
            std::vector<float> acc(m.v.size() * 3, 0.0f);
            for (size_t k = 0; k + 2 < m.i.size(); k += 3) {
                const float* A = m.v[m.i[k]].pos; const float* B = m.v[m.i[k + 1]].pos; const float* C = m.v[m.i[k + 2]].pos;
                const float e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
                const float e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
                const float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int q = 0; q < 3; ++q) for (int c = 0; c < 3; ++c) acc[m.i[k + q] * 3 + c] += fn[c];
            }
            for (size_t v = 0; v < m.v.size(); ++v) {
                float nx = acc[v * 3], ny = acc[v * 3 + 1], nz = acc[v * 3 + 2];
                const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
                else { nx = m.v[v].pos[0] - rb.x; ny = m.v[v].pos[1] - rb.y; nz = m.v[v].pos[2] - rb.z;
                       const float l2 = std::max(std::sqrt(nx * nx + ny * ny + nz * nz), 1e-4f); nx /= l2; ny /= l2; nz /= l2; }
                m.v[v].normal[0] = nx; m.v[v].normal[1] = ny; m.v[v].normal[2] = nz;
            }
            Entity e;
            e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                       m.i.data(), (uint32_t)m.i.size());
            e.tex = rockS.albedo; e.normalTex = rockS.normal; e.mrTex = rockS.mr;
            // a shade darker than the apron: wet rock
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.42f;
            e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            if (phys) {
                // the hull: the squashed rock's mean radius, centred a touch
                // low so the crown is not a step you clip through
                const float hr = rb.radius * (0.5f * (1.0f + kBoulderSquash));
                m_bodies.push_back(phys->addSphere(hr, x3::phys::Vec3{ rb.x, rb.y - 0.1f, rb.z },
                                                   0.0f, x3::phys::Layer::Static));
            }
            // spray in the lee: water piling on a rock throws it — all of
            // it spray (sprayP 1), small and dim (scale 0.45), ~4 bursts a
            // second (rush 0.6); 0.95 read as a bright blob per rock.
            MistSource ms;
            ms.x = rb.x + rb.dirX * rb.radius * 1.2f; ms.y = rb.y + kBoulderSquash * rb.radius * 0.4f;
            ms.z = rb.z + rb.dirZ * rb.radius * 1.2f;
            ms.dx = rb.dirX; ms.dz = rb.dirZ; ms.rush = 0.6f;
            ms.sprayP = 1.0f; ms.sprayScale = 0.45f;
            m_mist.push_back(ms);
            ++r.boulders;
        }
    }
    m_puffs.assign(m_mist.empty() ? 0u : 420u, Puff{});
    r.mistSources = (int)m_mist.size();

    // ---- THE LIGHT: cool accents down the whole cavern run. ---------------
    // Kept in OUR OWN array, not pushed into the host's boot-time block: the
    // host re-uploads one merged light array every frame and a boot push does
    // not survive the first of them (see nearestLights()). Dense enough that
    // wherever you stand down here something is lit, because only the nearest
    // handful are ever uploaded.
    m_lights.clear();
    for (float s = 30.0f; s < total; s += 42.0f) {
        float cx, cz, w, nat, dx, dz;
        walk.at(s, cx, cz, w, nat, dx, dz);
        int ni = 0;
        while (ni + 1 < uc.n && uc.cum[ni + 1] < s) ++ni;
        const bool nearPool = uc.pool[ni] ||
                              uc.pool[std::min(ni + 1, uc.n - 1)];
        x3::rhi::PointLight l;
        // Off the spine and up: a wash across the beach and the wall, not a
        // lamp hanging over the middle of the channel.
        const float px = -dz, pz = dx;
        // 13 m out puts them over the BEACH, clear of the channel floor even
        // where a pool widens it to 10.3 m (the carve's max(kURBedHalfW,
        // hw+0.8) — PAIRED with terrain.cpp's UR carve).
        const float side = (std::fmod(s / 42.0f, 2.0f) < 1.0f) ? 1.0f : -1.0f;
        l.pos[0] = cx + px * 13.0f * side;
        // HEIGHT sets the SIZE of the pool, not just its brightness: a lamp
        // 3 m over the beach lit a 3 m hot spot and nothing else (inverse
        // square — the ground 10 m out got 1/12 of the centre). 9 m up, the
        // same lamp lays an even pool ~20 m across on beach and wall, which
        // is a lit BANK, not a spotlight on a pebble.
        l.pos[1] = w + 9.0f;
        l.pos[2] = cz + pz * 13.0f * side;
        // Same blue family as the club grotto's pool banks so the accents
        // and the water read as ONE source — but not the same MAGNITUDE. Those
        // are (0.10,0.22,0.85)@12 m, tuned for the club's little grotto; this
        // cavern is 88 m across and up to 38 m tall, and that lamp would light
        // a puddle of it. Scaled to the room, still blue-dominant.
        // The open GORGE (past vaultEnd) has the sky; there the lamps go back
        // to being accents, or in daylight they are blue spotlights burning
        // on a sunlit wall.
        const float gorgeK = (s > total - kURGorgeLen) ? 0.30f : 1.0f;
        const float k = (nearPool ? 1.35f : 1.0f) * gorgeK;
        // POOLS OF LIGHT, not a wash: brighter and SHORTER than a spacing-wide
        // range reads as a lit bank with dark between, which is what a cave
        // wants. MAGNITUDE: these lamps used to be accents on top of a daylight
        // sky IBL that lit the whole vault (see applyAtmosphere — that IBL is
        // now a trace under the lid), so they were tuned to be SEEN, not to
        // LIGHT. Now they are the room's only real source, 9 m up: sized so
        // the beach under one reads ~0.5 radiance and the pool is still
        // ~0.1 at 15 m out (pointAtten is w/(d^2+1) — see mesh_lighting.glsl),
        // with the reach to match, so each one lays a pool on beach and wall
        // and the water between them has lamps to reflect. Less saturated
        // than before (blue:red 3:1, not 4:1) so wet rock under a lamp reads
        // cool-white stone, not neon.
        l.range = nearPool ? 44.0f : 34.0f;
        l.color[0] = 14.0f * k; l.color[1] = 24.0f * k; l.color[2] = 42.0f * k;
        m_lights.push_back(l);
    }
    r.lightCount = (int)m_lights.size();
    (void)outLights;   // this run's lights are delivered per-frame, nearest-K

    m_built = r.waterSegs > 0;
    r.built = m_built;
    char b[300];
    std::snprintf(b, sizeof(b),
        "[under-river] built: %.0f m run, %d vault chunks (gorge open last %.0f m), "
        "%d beach chunks, %d water segs, %d mist sources, %d lights, %d boulders (%zu solid); "
        "portal at (%.0f, %.0f)",
        uc.cum[uc.n - 1], r.vaultChunks, kURGorgeLen, r.beachChunks, r.waterSegs,
        r.mistSources, r.lightCount, r.boulders, m_bodies.size(), r.portalX, r.portalZ);
    x3::logInfo(b);
    return r;
}

void UndergroundRiver::releaseBodies(x3::phys::IPhysicsWorld& phys) {
    for (const x3::phys::BodyId& id : m_bodies)
        if (id.valid()) phys.removeBody(id);
    m_bodies.clear();
}

bool UndergroundRiver::insideCorridor(const float p[3]) {
    const UnderRiverChain& uc = worldUnderRiverChain();
    return uc.n >= 2 && p[0] > uc.bx0 && p[0] < uc.bx1
                     && p[2] > uc.bz0 && p[2] < uc.bz1;
}

bool UndergroundRiver::underVault(const float p[3]) {
    if (!insideCorridor(p)) return false;
    // Station along the chain of the closest point on it; under the lid while
    // that station is upstream of where the vault ends (buildStrip's vaultEnd).
    const UnderRiverChain& uc = worldUnderRiverChain();
    float bestD2 = 3.4e38f, bestS = 0.0f;
    for (int i = 0; i + 1 < uc.n; ++i) {
        const float ax = uc.x[i], az = uc.z[i];
        const float bx = uc.x[i + 1] - ax, bz = uc.z[i + 1] - az;
        const float L2 = std::max(bx * bx + bz * bz, 1e-6f);
        const float t = std::clamp(((p[0] - ax) * bx + (p[2] - az) * bz) / L2, 0.0f, 1.0f);
        const float dx = p[0] - (ax + bx * t), dz = p[2] - (az + bz * t);
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; bestS = uc.cum[i] + (uc.cum[i + 1] - uc.cum[i]) * t; }
    }
    return bestS < uc.cum[uc.n - 1] - kURGorgeLen;
}

void UndergroundRiver::applyAtmosphere(x3::rhi::IRenderDevice& device) const {
    // A CAVE IS DARK. The exterior recipe's daylight IBL lit the vault from
    // wall to wall as if the lid were glass; the lamps are the light here.
    // The IBL is not zeroed — the run is open at both ends and a trace of
    // sky does reach down it — but it is a trace, an order of magnitude
    // under the point-light pools, so the eye reads lit banks in a dark
    // room rather than a brown room with some blue in it.
    device.setIblIntensity(0.08f);
    device.setAmbient(0.010f, 0.012f, 0.016f);
    // Cold, thin haze: enough that the far reach of an 88 m hall goes to
    // black instead of showing its back wall, thin enough that a lamp 40 m
    // off still has a pool under it. Colour is the vault's own near-black.
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled  = true;
    fog.color[0] = 0.004f; fog.color[1] = 0.006f; fog.color[2] = 0.009f;
    fog.density  = 0.006f;
    fog.start    = 6.0f;
    fog.maxOpacity = 0.92f;
    device.setFog(fog);
}

uint32_t UndergroundRiver::nearestLights(const float cam[3],
                                         x3::rhi::PointLight* out,
                                         uint32_t maxN) const {
    if (!m_built || !out || maxN == 0 || m_lights.empty()) return 0;
    struct Scored { float d2; uint32_t i; };
    std::vector<Scored> s;
    s.reserve(m_lights.size());
    for (uint32_t i = 0; i < (uint32_t)m_lights.size(); ++i) {
        const float dx = m_lights[i].pos[0] - cam[0];
        const float dy = m_lights[i].pos[1] - cam[1];
        const float dz = m_lights[i].pos[2] - cam[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 340.0f * 340.0f) s.push_back({ d2, i });
    }
    const uint32_t k = std::min<uint32_t>((uint32_t)s.size(), maxN);
    if (k == 0) return 0;
    std::partial_sort(s.begin(), s.begin() + k, s.end(),
                      [](const Scored& a, const Scored& b) { return a.d2 < b.d2; });
    for (uint32_t i = 0; i < k; ++i) out[i] = m_lights[s[i].i];
    return k;
}

// ---------------------------------------------------------------------------
// THE MIST — RiverLife's wake-puff system aimed underground (rule 1). Spray
// off the whitewater steps is ADDITIVE (it catches the bank lights and feeds
// bloom); the cold breath lying on the pools is ALPHA haze. Both are cheap
// billboards through the one particle pass the rain and the wake already use.
// ---------------------------------------------------------------------------
void UndergroundRiver::update(float dt, Scene& scene) {
    if (!m_built) return;
    if (m_puffs.empty()) return;
    auto rnd = [&]() {
        m_seed = m_seed * 1664525u + 1013904223u;
        return (float)((m_seed >> 8) & 0xFFFF) / 65535.0f;
    };
    for (MistSource& ms : m_mist) {
        // A rushing step throws several puffs a second; a pool exhales one.
        const float rate = 0.7f + 5.5f * ms.rush;
        ms.acc += dt * rate;
        while (ms.acc >= 1.0f) {
            ms.acc -= 1.0f;
            Puff& p = m_puffs[m_puffNext];
            m_puffNext = (m_puffNext + 1) % (uint32_t)m_puffs.size();
            const float px = -ms.dz, pz = ms.dx;
            const float lat = (rnd() * 2.0f - 1.0f) * (2.5f + 3.0f * ms.rush);
            p.x = ms.x + px * lat + ms.dx * (rnd() * 8.0f - 4.0f);
            p.y = ms.y + rnd() * 0.6f;
            p.z = ms.z + pz * lat + ms.dz * (rnd() * 8.0f - 4.0f);
            // (the legacy branch keeps its short-circuit so its rnd()
            // sequence — and every door-shut puff — is byte-identical)
            if (ms.sprayP >= 0.0f) p.spray = rnd() < ms.sprayP;
            else                   p.spray = ms.rush > 0.5f && rnd() < ms.rush;
            const float sc = p.spray ? ms.sprayScale : 1.0f;
            // Spray is thrown downstream and up; pool haze barely moves.
            const float thr = p.spray ? (1.4f + 2.2f * ms.rush) * (0.5f + 0.5f * sc) : 0.15f;
            p.vx = ms.dx * thr + (rnd() - 0.5f) * 0.5f;
            p.vz = ms.dz * thr + (rnd() - 0.5f) * 0.5f;
            p.vy = p.spray ? (0.9f + 1.4f * rnd()) * (0.5f + 0.5f * sc) : (0.10f + 0.14f * rnd());
            p.life = p.spray ? (1.3f + 1.2f * rnd()) : (5.0f + 4.0f * rnd());
            p.size0 = p.spray ? (0.5f + 0.7f * rnd()) * sc : (2.4f + 2.4f * rnd());
            p.bright = sc;
            p.age = 0.0f;
        }
    }
    for (Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        p.age += dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        // Spray arcs and falls back; haze keeps drifting up into the dark.
        if (p.spray) p.vy -= 2.6f * dt;
        p.vx *= (1.0f - 1.1f * dt); p.vz *= (1.0f - 1.1f * dt);
    }
}

void UndergroundRiver::roomIrradianceAt(float x, float y, float z, float out[3]) const {
    // The vault's own floor: what the dark rock scatters back everywhere
    // (paired with the cavern horizonColor in world_water.cpp, x PI).
    out[0] = 0.019f; out[1] = 0.025f; out[2] = 0.038f;
    for (const x3::rhi::PointLight& l : m_lights) {
        const float dx = x - l.pos[0], dy = y - l.pos[1], dz = z - l.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= l.range * l.range) continue;
        // KEEP IN SYNC with pointAtten() in shaders/inc/mesh_lighting.glsl
        // (and roomAtten() in water.frag): w = (1 - (d/range)^4)^2 / (d^2 + 1).
        const float r4 = (d2 * d2) / (l.range * l.range * l.range * l.range);
        const float win = std::max(1.0f - r4, 0.0f);
        const float att = (win * win) / (d2 + 1.0f);
        out[0] += l.color[0] * att; out[1] += l.color[1] * att; out[2] += l.color[2] * att;
    }
}

void UndergroundRiver::render(x3::rhi::IRenderDevice& device,
                              const x3::rhi::FrameContext&) {
    if (!m_built || m_puffs.empty()) return;
    m_hazeOut.clear(); m_sprayOut.clear();
    for (const Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        const float t = p.age / p.life;
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = p.x; pi.pos[1] = p.y; pi.pos[2] = p.z;
        // LIT BY THE LAMPS. These colours used to be absolute — fine under the
        // daylight IBL that used to fill the vault, but in a dark cave a
        // constant 0.6 grey billboard is a glowing ghost and additive spray at
        // 0.3 outshines the rock it is falling on. Mist is albedo: the same
        // room irradiance the water and the rock receive, so a plume under a
        // lamp catches it (the owner's "mist catching light") and the breath
        // on a pool between lamps is barely there — which is what cold mist
        // in a dark cave looks like.
        float E[3];
        roomIrradianceAt(p.x, p.y, p.z, E);
        if (p.spray) {
            pi.size = p.size0 * (1.0f + 1.5f * t);
            // Fade IN over the first fifth, then out: a droplet burst that
            // pops into existence at full brightness reads as a sprite bug.
            const float a = std::min(t * 5.0f, 1.0f) * (1.0f - t) * 0.55f * p.bright;
            // Spray is forward-scattering: it throws back more than a flat
            // albedo would (the 3.0), which is why a lit plume reads white.
            pi.color[0] = 0.52f * a * E[0] * 3.0f;
            pi.color[1] = 0.60f * a * E[1] * 3.0f;
            pi.color[2] = 0.70f * a * E[2] * 3.0f;
            pi.color[3] = 1.0f;
            m_sprayOut.push_back(pi);
        } else {
            pi.size = p.size0 * (1.0f + 1.8f * t);
            pi.color[0] = 0.62f * E[0]; pi.color[1] = 0.68f * E[1]; pi.color[2] = 0.76f * E[2];
            pi.color[3] = 0.16f * std::min(t * 6.0f, 1.0f) * (1.0f - t) * (1.0f - t);
            m_hazeOut.push_back(pi);
        }
    }
    if (!m_hazeOut.empty())
        device.submitParticles(m_hazeOut.data(), (uint32_t)m_hazeOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
    if (!m_sprayOut.empty())
        device.submitParticles(m_sprayOut.data(), (uint32_t)m_sprayOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
}

// ---------------------------------------------------------------------------
// --test-underriver — the gate. Headless, no GPU textures needed (4.3 law:
// asserts structure, not pixels).
UndergroundRiver::Headroom UndergroundRiver::measureHeadroom() {
    const UnderRiverChain& uc = worldUnderRiverChain();
    Headroom h;
    const ChainWalk walk(uc);
    const float vaultEnd = uc.cum[uc.n - 1] - kURGorgeLen;
    h.vaultLen = vaultEnd;
    for (float s = 10.0f; s < vaultEnd; s += 20.0f) {
        float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
        const float px = -dz, pz = dx;
        const float span = kURWallOutW + 2.0f;      // the lid's foot-to-foot
        for (int k = 1; k < 12; ++k) {              // skip the buried feet
            const float u = (float)k / 12.0f;
            const float lat = (u * 2.0f - 1.0f) * span;
            const float vx = cx + px * lat, vz = cz + pz * lat;
            const float ceil = vaultCeilingY(worldPreUnderRiverHeight(vx, vz),
                                             w, u, s, k, false);
            const float head = ceil - terrainHeightAtWorld(vx, vz);
            ++h.probes;
            if (head < h.minHead) { h.minHead = head; h.atX = vx; h.atZ = vz; }
            h.maxHead = std::max(h.maxHead, head);
            // Standing room is only owed over ground you can stand on —
            // the channel and its beaches, not the rim where the lens shuts.
            if (std::fabs(lat) <= kURShelfHalfW + 3.0f)
                h.minBeachHead = std::min(h.minBeachHead, head);
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
bool UndergroundRiver::runSelfTest() {
    int passN = 0, failN = 0;
    char d[300];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[underriver] " + m); }
        else    { ++failN; x3::logError("[underriver] " + m); }
    };

    const UnderRiverChain& uc = worldUnderRiverChain();

    // ROUTE SCAN (X3_UR_SCAN=1) — the pre-corridor country the route is picked
    // ON (NO_SLOP rule 9: the first authored route was drawn from the map and
    // measured 228 m of massif over nodes 7-8, which cut-and-cover cannot
    // express; every node below is now chosen off THIS grid). Diagnostic only.
    if (const char* sc = std::getenv("X3_UR_SCAN"); sc && sc[0]) {
        // Default window = the west valley. A full "x0,x1,z0,z1,step" is taken
        // only if ALL FIVE parse: X3_UR_SCAN=1 is the obvious thing to type,
        // and a partial scanf would have left x0=1 > x1 and printed an empty
        // grid that looks like "the terrain is gone".
        float x0 = -1250, x1 = -850, z0 = -900, z1 = 1120, st = 50;
        float a0, a1, b0, b1, ss;
        if (std::sscanf(sc, "%f,%f,%f,%f,%f", &a0, &a1, &b0, &b1, &ss) == 5) {
            x0 = a0; x1 = a1; z0 = b0; z1 = b1; st = std::max(ss, 1.0f);
        }
        std::snprintf(d, sizeof(d),
                      "[underriver] SCAN pre-UR ground: x %.0f..%.0f (columns) "
                      "z %.0f..%.0f, step %.0f", x0, x1, z0, z1, st);
        x3::logInfo(d);
        { std::string hdr = "        ";
          char cell[16];
          for (float x = x0; x <= x1; x += st) {
              std::snprintf(cell, sizeof(cell), "%5.0f", x); hdr += cell; }
          x3::logInfo("[underriver] x=  " + hdr); }
        for (float z = z1; z >= z0; z -= st) {
            std::string row;
            char cell[16];
            std::snprintf(cell, sizeof(cell), "z%+6.0f:", z);
            row = cell;
            for (float x = x0; x <= x1; x += st) {
                std::snprintf(cell, sizeof(cell), "%5.0f",
                              worldPreUnderRiverHeight(x, z));
                row += cell;
            }
            x3::logInfo("[underriver] " + row);
        }
    }

    // The measured table, printed whole — the numbers this lane is tuned by.
    for (int i = 0; i < uc.n; ++i) {
        std::snprintf(d, sizeof(d),
            "[underriver]   node %2d (%7.1f, %7.1f) natural %7.2f water %7.2f "
            "cover %6.2f hw %.1f rush %.1f%s",
            i, uc.x[i], uc.z[i], uc.natural[i], uc.w[i],
            uc.natural[i] - uc.w[i], uc.hw[i], uc.rush[i],
            uc.pool[i] ? " POOL" : "");
        x3::logInfo(d);
    }

    // U1 — the river FLOWS: strictly descending, and it actually falls.
    {
        bool desc = uc.n >= 2;
        for (int i = 0; i + 1 < uc.n; ++i)
            if (uc.w[i + 1] >= uc.w[i]) desc = false;
        const float fall = uc.w[0] - uc.w[uc.n - 1];
        std::snprintf(d, sizeof(d), "%d nodes, total fall %.1f m over %.0f m",
                      uc.n, fall, uc.cum[uc.n - 1]);
        check(desc && fall > 6.0f, "U1 the underground river descends, head to plunge pool", d);
    }

    // U2 — UNDER the mountain: water below the pre-carve ground everywhere,
    // with real rock cover on the vaulted run (portal pool excepted).
    {
        float minCover = 1e9f; int at = -1;
        for (int i = 0; i + 1 < uc.n; ++i) {   // exclude the portal node
            const float cover = uc.natural[i] - uc.w[i];
            if (cover < minCover) { minCover = cover; at = i; }
        }
        std::snprintf(d, sizeof(d), "min cover %.2f m at node %d; portal cover %.2f m",
                      minCover, at, uc.natural[uc.n - 1] - uc.w[uc.n - 1]);
        check(minCover >= 0.85f, "U2 the run stays under the ground it bores", d);
    }

    // U3 — THE TRENCH IS CARVED, AND ITS BEACHES ARE WALKABLE ROCK. Two
    // invariants, not a shape: (1) the bed under the spine is below the water,
    // so the ribbon floats on a floor rather than over the countryside, and
    // (2) the beach is DRY and STANDABLE — the owner asked for rock beaches
    // and NO_SLOP rule 11 says a surface a character can be on is a surface
    // that has to hold them up.
    //
    // What this gate deliberately does NOT assert is a beach of constant
    // height. An earlier version wanted the shelf within 1.4 m of the water
    // everywhere and failed on the outside of bends — where the polyline's
    // nearest point is the VERTEX, the effective distance exceeds the lateral
    // offset, and the beach correctly narrows into the wall. That is a real
    // river bank, not a defect; the tolerance was the bug.
    {
        const ChainWalk walk(uc);
        int wetBeach = 0, steepBeach = 0, badBed = 0, stations = 0;
        float worstBed = 0, worstLift = 1e9f, worstNy = 1.0f;
        float wsx = 0, wsz = 0;
        for (float s = 6.0f; s < uc.cum[uc.n - 1] - 6.0f; s += 18.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            ++stations;
            const float bedT = terrainHeightAtWorld(cx, cz);
            const float bedErr = bedT - w;              // want < 0 (bed under water)
            if (bedErr > -0.8f) { ++badBed; worstBed = std::max(worstBed, bedErr); }
            // The beach: outside even a POOL's widened channel floor (the carve
            // starts the beach at max(kURShelfHalfW, hw+0.8+3.5) = 13.8 m at
            // the 9.5 m pools), so this lands on shelf everywhere on the run.
            const float bd = kURShelfHalfW + 3.0f;
            for (int side = -1; side <= 1; side += 2) {
                const float sx = cx + px * bd * (float)side;
                const float sz = cz + pz * bd * (float)side;
                const float lift = terrainHeightAtWorld(sx, sz) - w;
                if (lift < worstLift) { worstLift = lift; wsx = sx; wsz = sz; }
                if (lift < 0.1f) ++wetBeach;            // beach under the water
                float n[3]; terrainNormalAtWorld(sx, sz, n);
                worstNy = std::min(worstNy, n[1]);
                if (n[1] < 0.72f) ++steepBeach;         // > ~44 deg: not standable
            }
        }
        std::snprintf(d, sizeof(d),
            "%d stations; bed %d bad (worst err %.2f); beach %d wet / %d too steep "
            "(lowest beach %.2f m over the water at (%.0f, %.0f), worst normal.y %.2f)",
            stations, badBed, worstBed, wetBeach, steepBeach, worstLift, wsx, wsz, worstNy);
        check(badBed == 0 && wetBeach == 0 && steepBeach == 0 && stations > 50,
              "U3 bed under the water, beaches dry and walkable, whole run", d);
    }

    // U4 — ONE WATER TRUTH: worldWaterLevelAt on the spine IS the table; a
    // point past the water half-width on the beach shelf is DRY.
    {
        const ChainWalk walk(uc);
        float worst = 0.0f; int wetShelf = 0;
        for (float s = 6.0f; s < uc.cum[uc.n - 1] - 6.0f; s += 15.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float q = worldWaterLevelAt(cx, cz);
            if (q > kWorldWaterDry + 1.0f)
                worst = std::max(worst, std::fabs(q - w));
            else
                worst = 1e9f;                            // dry mid-channel = broken
            const float px = -dz, pz = dx;
            const float dry = kURShelfHalfW + 3.0f;   // > every node half-width
            if (worldWaterLevelAt(cx + px * dry, cz + pz * dry) >
                kWorldWaterDry + 1.0f) ++wetShelf;
        }
        std::snprintf(d, sizeof(d),
            "largest spine drawn-vs-query gap %.4f m; %d wet probes on the dry shelf",
            worst, wetShelf);
        check(worst < 0.01f && wetShelf == 0, "U4 the query and the table are ONE truth", d);
    }

    // U5 — the owner's RUSHING WATER is really in the table. rush is derived
    // from the gradient the country forces (terrain.h), so this gate is a
    // statement about the ROUTE: it must actually fall somewhere, not glide.
    {
        int rushN = 0, poolN = 0; float maxRush = 0.0f;
        for (int i = 0; i < uc.n; ++i) {
            if (uc.rush[i] >= 0.55f) ++rushN;
            maxRush = std::max(maxRush, uc.rush[i]);
            if (uc.pool[i]) ++poolN;
        }
        std::snprintf(d, sizeof(d), "%d rushing reaches (max rush %.2f), %d pools",
                      rushN, maxRush, poolN);
        check(rushN >= 2 && maxRush >= 0.9f && poolN >= 2,
              "U5 the run rushes at the steps and stills at the pools", d);
    }

    // U7 — THE COVER BUDGET. Cut-and-cover's walls have to climb out of the
    // trench inside the wall band; cover IS the cavern's height and is bounded
    // by what that band can carry (terrain.h's mechanism note). A route that
    // busts this gets MOVED — forcing it is how you get a 200 m slot.
    {
        float wall = 0.0f, minCover = 1e9f; int atWall = -1, atMin = -1;
        for (int i = 0; i < uc.n; ++i) {
            const float cov = uc.floorMin[i] - uc.w[i];       // roof over the water
            const float wal = uc.floorMax[i] - uc.w[i];       // the climb out
            if (wal > wall) { wall = wal; atWall = i; }
            if (cov < minCover) { minCover = cov; atMin = i; }
        }
        const float deg = std::atan(wall / (kURWallOutW - kURShelfHalfW))
                        * 57.2957795f;
        std::snprintf(d, sizeof(d),
            "thinnest roof %.1f m (node %d); tallest trench wall %.1f m (node %d) "
            "over the %.0f m band = %.1f deg (limit %.0f)",
            minCover, atMin, wall, atWall, kURWallOutW - kURShelfHalfW,
            deg, kURWallMaxDeg);
        check(deg <= kURWallMaxDeg && minCover >= kURCoverMin - 0.01f,
              "U7 the route stays inside what cut-and-cover can build", d);
    }

    // U8 — THE CARVE GUARD. Every authored cut is multiplied by the facility /
    // city-pad / outpost guard, so a spine that strays into one is NOT DUG
    // while worldWaterLevelAt still reports wet — water hanging in mid-air
    // over solid ground, which is JOB 1's defect reintroduced underground.
    // Measured across the WHOLE band, not just the spine.
    {
        const ChainWalk walk(uc);
        float worst = 1.0f, wx = 0, wz = 0;
        for (float s = 0.0f; s <= uc.cum[uc.n - 1]; s += 12.0f) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const float px = -dz, pz = dx;
            for (int k = -3; k <= 3; ++k) {
                const float lat = (float)k * (kURWallOutW / 3.0f);
                const float g = worldCarveGuardAt(cx + px * lat, cz + pz * lat);
                if (g < worst) { worst = g; wx = cx + px * lat; wz = cz + pz * lat; }
            }
        }
        std::snprintf(d, sizeof(d),
                      "weakest carve guard across the corridor %.3f at (%.0f, %.0f)",
                      worst, wx, wz);
        check(worst >= 0.999f, "U8 the whole corridor is allowed to be dug", d);
    }

    // U6 — determinism: a second read of the chain is bit-identical (the
    // derivation must not depend on corridor registration or call order).
    {
        const UnderRiverChain& uc2 = worldUnderRiverChain();
        bool same = uc2.n == uc.n;
        for (int i = 0; same && i < uc.n; ++i)
            same = uc2.w[i] == uc.w[i] && uc2.natural[i] == uc.natural[i];
        check(same, "U6 the derived table is stable");
    }

    // U9 — IT IS A CAVERN YOU CAN STAND IN. The vault had no gate at all: the
    // lid is a mesh, the floor is the height field, and nothing checked that
    // there is a VOID between them. Walks the vaulted reach and measures the
    // ceiling (vaultCeilingY at its LOWEST — no jitter luck) against the built
    // ground, across the channel and both beaches. Two ways this can fail and
    // both have to be caught: a ceiling that dives into the floor (no cavern),
    // and a ceiling low enough to walk into (CONTACT LAW's other direction —
    // the beaches are walkable ground, so they need standing room over them).
    {
        const Headroom h = measureHeadroom();
        std::snprintf(d, sizeof(d),
            "%d probes over %.0f m of vault; headroom %.2f..%.2f m "
            "(tightest at (%.0f, %.0f)); over the beaches at least %.2f m",
            h.probes, h.vaultLen, h.minHead, h.maxHead, h.atX, h.atZ, h.minBeachHead);
        check(h.minHead > 0.05f && h.minBeachHead >= 2.5f && h.probes > 500,
              "U9 there is a cavern in there, and you can stand up in it", d);
    }

    // U10 — THE BEACH APRON ACTUALLY COVERS THE GROUND. The apron is a flat-
    // chorded mesh laid over a curved field; where a chord spans a CONCAVE
    // stretch it passes UNDER the ground and the terrain pokes through, and
    // what pokes through is the height/slope splat — i.e. GRASS, indoors,
    // exactly the defect the apron exists to hide. Two captures were spent
    // guessing at this (a bigger offset, a wider apron, a denser mesh) before
    // anyone measured the sag. So: sample the midpoint of every apron cell,
    // compare the chord against the real field, and report the worst.
    {
        const ChainWalk walk(uc);
        std::vector<float> lats; apronLats(lats);   // the SAME table build() uses
        const int kAcross = (int)lats.size();
        const float kLift = kURApronLift;
        float worstSag = 0.0f, wx = 0, wz = 0; int cells = 0, pierced = 0;
        constexpr float kRing = 2.5f;               // PAIRED with build()'s kStep
        // EVERY ring, not every fourth: an 11 m stride over 2.5 m rings tested
        // 23% of the apron and reported it clean while a probe capture showed
        // a hole in it.
        for (float s = 2.0f; s < uc.cum[uc.n - 1] - 2.0f; s += kRing) {
            float cx, cz, w, nat, dx, dz; walk.at(s, cx, cz, w, nat, dx, dz);
            const Frame fr = walk.frameAt(s);       // the SAME frame build() uses
            const float px = fr.px, pz = fr.pz;
            for (int k = 0; k + 1 < kAcross; ++k) {
                const float l0 = lats[k] * fr.scale, l1 = lats[k+1] * fr.scale;
                const float lm = 0.5f * (l0 + l1);
                const float y0 = terrainHeightAtWorld(cx + px * l0, cz + pz * l0);
                const float y1 = terrainHeightAtWorld(cx + px * l1, cz + pz * l1);
                const float ym = terrainHeightAtWorld(cx + px * lm, cz + pz * lm);
                const float chord = 0.5f * (y0 + y1) + kLift;
                float sag = ym - chord;             // >0 : ground pokes through
                // ALONG the run as well as across it. The first version of this
                // gate only sampled the LATERAL midpoint and passed clean while
                // a magenta-tinted probe capture showed a wedge of bare ground
                // straight through the apron: the rings are 6 m apart and
                // sagitta goes as the chord SQUARED, so the along-run chord
                // sagged ~9x harder than the 2 m lateral one. A gate that only
                // measures one axis of a two-axis mesh is not a gate.
                {
                    const float sN = std::min(s + kRing, uc.cum[uc.n - 1]);
                    const Frame f2 = walk.frameAt(sN);
                    float nx, nz, nw, nnat, ndx, ndz; walk.at(sN, nx, nz, nw, nnat, ndx, ndz);
                    const float la = lats[k] * fr.scale, lb = lats[k] * f2.scale;
                    const float ax = cx + px * la,       az = cz + pz * la;
                    const float bx2 = nx + f2.px * lb,   bz2 = nz + f2.pz * lb;
                    const float ya = terrainHeightAtWorld(ax, az);
                    const float yb = terrainHeightAtWorld(bx2, bz2);
                    const float ymid = terrainHeightAtWorld(0.5f*(ax+bx2), 0.5f*(az+bz2));
                    sag = std::max(sag, ymid - (0.5f * (ya + yb) + kLift));
                }
                ++cells;
                if (sag > 0.0f) {
                    ++pierced;
                    if (sag > worstSag) {
                        worstSag = sag; wx = cx + px * lm; wz = cz + pz * lm;
                    }
                }
            }
        }
        std::snprintf(d, sizeof(d),
            "%d apron cells; %d pierced by the ground (worst %.3f m at (%.0f, %.0f)); "
            "lift %.2f m over %.1f m chords",
            cells, pierced, worstSag, wx, wz, kLift,
            (2.0f * kURWallOutW) / (float)(kAcross - 1));
        // WHAT THIS GATE DOES NOT SEE — the residual, written down so the next
        // pass starts ahead of where this one finished (rule 10). U10 compares
        // the apron against the ANALYTIC height field. The renderer draws a
        // LOD-approximated TILE MESH of that field (1 / 2 / 4 m chords), and
        // across a concave crease a coarse chord sits ABOVE the analytic
        // surface by roughly c^2*|f''|/8 — which at the shelf crease measures
        // |f''| ~ 0.28/m, i.e. ~0.56 m at 4 m LOD, far more than this lift.
        // A lobe of splat GRASS still shows on the Great Hall's east bank in
        // captures for what is almost certainly that reason: the apron chunks
        // are provably complete (finite, 5805 v / 33792 i each, AABB covers
        // the lobe), the ground is provably not above them analytically (this
        // gate), the forest is not the cause (X3_FOREST=0 is identical), and
        // floating the apron 3 m does not cover it. Fitting the apron to the
        // TILE MESH rather than to the analytic field is the fix; a bigger
        // lift is not, because every centimetre here is a centimetre the
        // player's boots stand below what they can see.
        check(pierced == 0, "U10 the rock apron covers the beach it is laid on", d);
    }

    std::snprintf(d, sizeof(d), "--test-underriver: %d/%d passed", passN, passN + failN);
    if (failN) x3::logError(d); else x3::logInfo(d);
    return failN == 0;
}

} // namespace x3::game
