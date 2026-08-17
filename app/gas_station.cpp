// GAS STATIONS — see app/gas_station.h for the phase contract and the law.
#include "gas_station.h"

#include "asset_root.h"
#include "env_art.h"
#include "glb_cpu_read.h"
#include "road_network.h"
#include "scene.h"
#include "surface_library.h"
#include "terrain.h"

#include "engine/core/IConsole.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace x3::game {

namespace {

// The structure, converted from the licensed pack by tools/w5_build_station_glb.py
// and published to the asset store. Relative to convertedGlbRoot().
constexpr const char* kStationGlb = "GasStation/gas_station_mega.glb";

// PAD CARVE — four parallel corridors across the lot, spines 11 m apart with a
// 6 m flat half-width each, so their flats abut and the union covers the whole
// 43 m width. ONE wide corridor would not do: a TerrainCorridor lowers the
// ground by a CONSTANT depth across its flat (terrain.cpp corridorSegment) — it
// does not level to a datum — so a single spine leaves the CROSS-slope intact
// and a hill corner pushes up through the apron. Four spines, each carrying the
// deepest cut its own 12 m band needs, level both axes to within the band's
// residual, and DEEPEST-WINS means the overlap never digs twice.
constexpr int   kPadLanes      = 4;
constexpr float kPadLaneStepX  = 11.0f;
constexpr float kPadLaneHalfW  = 6.0f;
constexpr float kPadFalloff    = 13.0f;
constexpr int   kPadNodes      = 30;      // <= TerrainCorridor::kMaxNodes
// Corridors this module needs per station: kPadLanes + 1 driveway.
constexpr uint32_t kCorridorsPerStation = (uint32_t)kPadLanes + 1u;
// Leave the registry headroom rather than starving a later producer.
constexpr uint32_t kCorridorReserve = 6;

// The natural (pre-corridor) surface — the roads have already carved by the
// time we plan, and scoring a site against their own cutting would call every
// road cut "flat ground". Same trick river_bridge.cpp uses.
float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

// A route's OUTER paved-edge offset from its centreline. A dual carriageway is
// two full roadways either side of the median, so its edge is much further out
// than a single road's — a station sited at kPavedHalfM off a freeway would
// stand in the far carriageway.
float pavedEdgeOf(const RoadSpec& s) {
    return s.dualCarriageway
             ? (kFwyMedianMaxHalfM + 2.0f * kFwyPavedHalfM)
             : kPavedHalfM;
}

// Arc length along a spec, and the node nearest a given arc length.
float specLength(const RoadSpec& s) {
    float u = 0.0f;
    for (size_t i = 1; i < s.x.size(); ++i)
        u += std::hypot(s.x[i] - s.x[i-1], s.z[i] - s.z[i-1]);
    return u;
}

// Sample a spec at arc length `u`: position, unit tangent and (if roadY is
// given) the graded datum. Returns the node index at or before u.
uint32_t sampleSpec(const RoadSpec& s, const std::vector<float>* roadY, float u,
                    float& outX, float& outZ, float& outTX, float& outTZ,
                    float& outY) {
    outX = s.x[0]; outZ = s.z[0]; outTX = 1.0f; outTZ = 0.0f;
    outY = (roadY && !roadY->empty()) ? (*roadY)[0] : naturalAt(outX, outZ);
    float acc = 0.0f;
    for (size_t i = 1; i < s.x.size(); ++i) {
        const float dx = s.x[i] - s.x[i-1], dz = s.z[i] - s.z[i-1];
        const float len = std::hypot(dx, dz);
        if (len <= 1e-4f) continue;
        if (acc + len >= u || i + 1 == s.x.size()) {
            const float t = std::min(1.0f, std::max(0.0f, (u - acc) / len));
            outX = s.x[i-1] + dx * t;
            outZ = s.z[i-1] + dz * t;
            outTX = dx / len; outTZ = dz / len;
            if (roadY && roadY->size() == s.x.size())
                outY = (*roadY)[i-1] + ((*roadY)[i] - (*roadY)[i-1]) * t;
            else
                outY = naturalAt(outX, outZ);
            return (uint32_t)(i - 1);
        }
        acc += len;
    }
    return 0;
}

// Would a forecourt whose origin is `o` and whose local +X is `d` sit on ground
// this route can afford? Scores the WORST cut and fill over a sampled grid of
// the lot plus its driveway. Lower `score` = flatter = better.
struct SiteScore { float cut = 0.0f, fill = 0.0f, score = 1e9f; };

SiteScore scoreFootprint(float ox, float oz, float dx, float dz, float padY,
                         float driveLen) {
    SiteScore s;
    const float px = -dz, pz = dx;            // local +Z in world
    float worstCut = 0.0f, worstFill = 0.0f;
    for (int i = -4; i <= 4; ++i) {
        for (int j = -6; j <= 6; ++j) {
            const float lx = (float)i / 4.0f * kStationHalfX;
            const float lz = (float)j / 6.0f * kStationHalfZ;
            const float wx = ox + dx * lx + px * lz;
            const float wz = oz + dz * lx + pz * lz;
            const float g  = naturalAt(wx, wz) - padY;
            worstCut  = std::max(worstCut,  g);      // ground above the pad: cut
            worstFill = std::max(worstFill, -g);     // ground below: the skirt covers it
        }
    }
    // The driveway matters as much as the lot: a forecourt on a shelf with a
    // cliff between it and the road is not connected to anything.
    for (int i = 0; i <= 6; ++i) {
        const float lx = kStationHalfX + driveLen * (float)i / 6.0f;
        for (int j = -1; j <= 1; ++j) {
            const float lz = (float)j * kDriveHalfZ;
            const float wx = ox + dx * lx + px * lz;
            const float wz = oz + dz * lx + pz * lz;
            const float g  = naturalAt(wx, wz) - padY;
            worstCut  = std::max(worstCut,  g);
            worstFill = std::max(worstFill, -g);
        }
    }
    s.cut = worstCut; s.fill = worstFill;
    // Fill is cheaper than cut (the slab's own skirt hides it; a cut is a hole
    // in the hillside the player sees from the road), so weight it lower.
    s.score = worstCut + 0.45f * worstFill;
    return s;
}

// Try both sides of the route at arc length u, keep the better one.
bool trySite(const RoadSpec& spec, const std::vector<float>* roadY, float u,
             const char* host, const char* name, GasStationSite& out) {
    float rx, rz, tx, tz, ry;
    sampleSpec(spec, roadY, u, rx, rz, tx, tz, ry);
    const float edge = pavedEdgeOf(spec);
    const float driveLen = kStationClearM + 3.0f;      // + the lap onto the road apron
    const float offs = edge + kStationClearM + kStationHalfX;

    GasStationSite best;
    float bestScore = 1e9f;
    for (int side = -1; side <= 1; side += 2) {
        // Outward normal of the route on this side; the station's local +X is
        // the REVERSE of it, because the open frontage must face the road.
        const float nx = -tz * (float)side, nz = tx * (float)side;
        const float ox = rx + nx * offs, oz = rz + nz * offs;
        const float padY = ry;                        // level with the pavement
        const SiteScore sc = scoreFootprint(ox, oz, -nx, -nz, padY, driveLen);
        if (sc.score >= bestScore) continue;
        bestScore = sc.score;
        best = GasStationSite{};
        best.ok = true;
        best.name = name;
        best.x = ox; best.z = oz;
        best.dirX = -nx; best.dirZ = -nz;
        best.padY = padY;
        best.roadX = rx; best.roadZ = rz;
        best.roadEdgeM = edge;
        best.driveLenM = driveLen;
        best.cutM = sc.cut; best.fillM = sc.fill;
        best.host = host;
    }
    // THE HONEST REFUSAL. A forecourt is a flat 43x65 m slab; if the country
    // under it needs a 12 m cut, that is a quarry, not a gas station, and the
    // right answer is to say so rather than ship a shelf gouged out of a
    // mountain. (Never silently: the number goes in the log.)
    if (!best.ok || bestScore > 34.0f) {
        out = GasStationSite{};
        out.name = name; out.host = host;
        out.whyNot = "no side of this reach is flat enough for a forecourt";
        out.cutM = best.cutM; out.fillM = best.fillM;
        return false;
    }
    out = best;
    return true;
}

SurfaceLibrary& stationSurfaces() { static SurfaceLibrary lib; return lib; }

// The apron mesh builder. Vertices are WORLD-BAKED (the same choice the road
// ribbon and the tunnel shell make), so the collider shares this index buffer
// and the entity carries no physics body — attaching one would make
// Scene::update() re-anchor the transform onto the body origin.
struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t>            i;
    // WINDING: a,b,c,d counter-clockwise seen from the side the normal points
    // at. Get it backwards on a drivable surface and you get the bridge's exact
    // double bug — invisible from above AND the wheel rays fall through it,
    // because Jolt culls back faces on the very same triangles.
    void quad(const float a[3], const float b[3], const float c[3],
              const float d[3], float uScale) {
        const uint32_t base = (uint32_t)v.size();
        float e0[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        float e1[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float n[3] = { e0[1]*e1[2]-e0[2]*e1[1], e0[2]*e1[0]-e0[0]*e1[2],
                       e0[0]*e1[1]-e0[1]*e1[0] };
        const float l = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (l > 1e-6f) { n[0]/=l; n[1]/=l; n[2]/=l; } else { n[0]=0; n[1]=1; n[2]=0; }
        const float* p[4] = { a, b, c, d };
        const float us[4] = { 0, 1, 1, 0 }, ws[4] = { 0, 0, 1, 1 };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=p[k][0]; mv.pos[1]=p[k][1]; mv.pos[2]=p[k][2];
            mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
            mv.uv[0]=us[k]*uScale; mv.uv[1]=ws[k]*uScale;
            v.push_back(mv);
        }
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }
    bool empty() const { return i.empty(); }
};

} // namespace

GasStationWorld::GasStationWorld() = default;
GasStationWorld::~GasStationWorld() = default;

// ===========================================================================
// PHASE 1 — siting
// ===========================================================================
uint32_t GasStationWorld::plan(const RoadSpec* ring,  const std::vector<float>* ringY,
                               const RoadSpec* river, const std::vector<float>* riverY,
                               const RoadSpec* conn,  const std::vector<float>* connY) {
    m_sites.clear();

    // ---- A. THE FREEWAY STATION — off a turnaround crossover of the inner
    // tour. planTurnarounds() is the authored list of paved median gaps (one
    // every ~1.7 km plus one at every junction landing): a crossover is exactly
    // where a driver can already change direction, so it is where a service
    // station belongs. Every crossover is a candidate; the flattest wins.
    if (ring && ring->x.size() >= 2) {
        const std::vector<RoadTurnaround> ta = planTurnarounds(*ring);
        GasStationSite best; float bestScore = 1e9f; uint32_t tried = 0;
        for (const RoadTurnaround& t : ta) {
            GasStationSite s;
            const float u = (t.u0 + t.u1) * 0.5f;
            ++tried;
            if (!trySite(*ring, ringY, u, "inner tour (freeway)", "Freeway Services", s))
                continue;
            const float sc = s.cutM + 0.45f * s.fillM;
            if (sc < bestScore) { bestScore = sc; best = s; }
        }
        if (best.ok) {
            m_sites.push_back(best);
        } else {
            GasStationSite s; s.name = "Freeway Services"; s.host = "inner tour (freeway)";
            s.whyNot = tried ? "every turnaround's surroundings measured too steep"
                             : "the inner tour planned no turnarounds";
            m_sites.push_back(s);
        }
    }

    // ---- B. THE TOWN-APPROACH STATION. The Small Mountain Town is Lane 4's
    // build zone; a station dropped inside it would collide with their layout.
    // So this one stands on the APPROACH — the valley road, on the half of it
    // furthest from the ring (the town side per docs/design/
    // ROAD_NETWORK_SKETCH_V2.png, where the yellow road runs east off the
    // massif to the town). Candidates are stepped along that half and the
    // flattest wins, so the site moves with the road rather than being pinned
    // to a coordinate the town might later want.
    if (river && river->x.size() >= 2) {
        const float L = specLength(*river);
        GasStationSite best; float bestScore = 1e9f;
        for (int k = 0; k <= 10; ++k) {
            const float u = L * (0.55f + 0.35f * (float)k / 10.0f);
            GasStationSite s;
            if (!trySite(*river, riverY, u, "valley road (town approach)",
                         "Mountain Town Fuel", s)) continue;
            const float sc = s.cutM + 0.45f * s.fillM;
            if (sc < bestScore) { bestScore = sc; best = s; }
        }
        if (best.ok) m_sites.push_back(best);
        else {
            GasStationSite s; s.name = "Mountain Town Fuel";
            s.host = "valley road (town approach)";
            s.whyNot = "the town approach measured too steep along its whole east half";
            m_sites.push_back(s);
        }
    }

    // ---- C. THE COUNTRY CROSSROADS STATION — on the spawn connector. A real
    // crossroads is where two routes meet, and the connector already carries
    // noted junctions (the range-circuit access, the ring landing). Candidates
    // are the connector points NEAREST a noted junction but outside its
    // barrier-exclusion zone, so the station sits at the crossroads without
    // standing in the mouth.
    if (conn && conn->x.size() >= 2) {
        const float L = specLength(*conn);
        GasStationSite best; float bestScore = 1e9f;
        for (int k = 0; k <= 12; ++k) {
            const float u = L * (0.15f + 0.65f * (float)k / 12.0f);
            float rx, rz, tx, tz, ry;
            sampleSpec(*conn, connY, u, rx, rz, tx, tz, ry);
            const float dj = distToNearestRoadJunction(rx, rz);
            // Inside the exclusion zone the mouth owns the ground; far outside
            // it is not a crossroads any more. The band between is the spot.
            if (dj < kJunctionBarrierClearM + 25.0f || dj > 420.0f) continue;
            GasStationSite s;
            if (!trySite(*conn, connY, u, "spawn connector (country)",
                         "Crossroads Fuel", s)) continue;
            const float sc = s.cutM + 0.45f * s.fillM + 0.004f * dj;
            if (sc < bestScore) { bestScore = sc; best = s; }
        }
        if (!best.ok) {   // no crossroads in reach: fall back to mid-connector
            for (int k = 0; k <= 8; ++k) {
                const float u = L * (0.30f + 0.40f * (float)k / 8.0f);
                GasStationSite s;
                if (!trySite(*conn, connY, u, "spawn connector (country)",
                             "Crossroads Fuel", s)) continue;
                const float sc = s.cutM + 0.45f * s.fillM;
                if (sc < bestScore) { bestScore = sc; best = s; }
            }
        }
        if (best.ok) m_sites.push_back(best);
        else {
            GasStationSite s; s.name = "Crossroads Fuel";
            s.host = "spawn connector (country)";
            s.whyNot = "no connector reach measured flat enough";
            m_sites.push_back(s);
        }
    }

    uint32_t ok = 0;
    for (const GasStationSite& s : m_sites) {
        char b[256];
        if (s.ok) {
            ++ok;
            std::snprintf(b, sizeof(b),
                "[gas] %-20s on %-28s at (%.0f, %.0f) y %.1f  facing (%+.2f,%+.2f)  "
                "cut %.1f m / fill %.1f m",
                s.name.c_str(), s.host, (double)s.x, (double)s.z, (double)s.padY,
                (double)s.dirX, (double)s.dirZ, (double)s.cutM, (double)s.fillM);
        } else {
            std::snprintf(b, sizeof(b), "[gas] %-20s NOT SITED on %s — %s "
                          "(best cut %.1f m / fill %.1f m)",
                          s.name.c_str(), s.host, s.whyNot,
                          (double)s.cutM, (double)s.fillM);
        }
        x3::logInfo(b);
    }
    return ok;
}

// ===========================================================================
// PHASE 2 — carve the pads, note the mouths
// ===========================================================================
uint32_t GasStationWorld::registerPads() {
    uint32_t made = 0;
    for (GasStationSite& s : m_sites) {
        if (!s.ok) continue;
        if (terrainCorridorCount() + kCorridorsPerStation + kCorridorReserve
                > kMaxTerrainCorridors) {
            s.ok = false;
            s.whyNot = "terrain-corridor registry full — pad not carved";
            x3::logWarn(std::string("[gas] ") + s.name + ": " + s.whyNot);
            continue;
        }
        const float dx = s.dirX, dz = s.dirZ;
        const float px = -dz, pz = dx;                  // local +Z in world
        const float apronY = s.padY;

        // --- the lot: four parallel spines along local +Z ------------------
        for (int lane = 0; lane < kPadLanes; ++lane) {
            const float lx = ((float)lane - (kPadLanes - 1) * 0.5f) * kPadLaneStepX;
            TerrainCorridor c;
            c.nodeCount = kPadNodes;
            c.halfWidth = kPadLaneHalfW;
            c.falloff   = kPadFalloff;
            for (int n = 0; n < kPadNodes; ++n) {
                const float lz = -kStationHalfZ
                               + 2.0f * kStationHalfZ * (float)n / (float)(kPadNodes - 1);
                const float wx = s.x + dx * lx + px * lz;
                const float wz = s.z + dz * lx + pz * lz;
                c.x[n] = wx; c.z[n] = wz;
                // The depth this node must carry is the deepest cut anywhere in
                // its own lateral BAND, not just on the spine — a corridor
                // lowers by a constant across its flat, so a spine-only sample
                // leaves the band's high shoulder standing proud of the apron.
                float deepest = 0.0f;
                for (int b = -2; b <= 2; ++b) {
                    const float bx = lx + (float)b * (kPadLaneHalfW * 0.5f);
                    const float sx = s.x + dx * bx + px * lz;
                    const float sz = s.z + dz * bx + pz * lz;
                    deepest = std::max(deepest, naturalAt(sx, sz) - apronY);
                }
                c.depth[n] = std::max(0.0f, deepest);
            }
            if (registerTerrainCorridor(c)) ++made;
        }

        // --- the driveway: frontage out to the road's paved edge -----------
        {
            TerrainCorridor c;
            c.nodeCount = 10;
            c.halfWidth = kDriveHalfZ + 1.0f;
            c.falloff   = 11.0f;
            for (int n = 0; n < c.nodeCount; ++n) {
                const float t  = (float)n / (float)(c.nodeCount - 1);
                // Start INSIDE the lot and finish ON the road centreline side of
                // the paved edge, so neither joint can leave an unflattened lip.
                const float lx = kStationHalfX - 3.0f
                               + (s.driveLenM + 6.0f) * t;
                const float wx = s.x + dx * lx, wz = s.z + dz * lx;
                c.x[n] = wx; c.z[n] = wz;
                float deepest = 0.0f;
                for (int b = -2; b <= 2; ++b) {
                    const float lz = (float)b * (kDriveHalfZ * 0.5f);
                    deepest = std::max(deepest,
                        naturalAt(wx + px * lz, wz + pz * lz) - apronY);
                }
                c.depth[n] = std::max(0.0f, deepest);
            }
            if (registerTerrainCorridor(c)) ++made;
        }

        // THE MOUTH NOTE. planRoadBarriers() (inside buildRoadRibbon) refuses to
        // place a rail or a jersey wall within kJunctionBarrierClearM of a noted
        // junction, and the ribbon feathers its prism skirt to a drivable batter
        // through the same zone. Without this the freeway's continuous median
        // and offside barriers run straight across the driveway and the station
        // is a walled compound you can see but never enter.
        noteRoadJunction(s.roadX, s.roadZ);
    }
    m_pads = made;
    char b[128];
    std::snprintf(b, sizeof(b), "[gas] %u forecourt corridors carved (registry now %u/%u)",
                  made, terrainCorridorCount(), kMaxTerrainCorridors);
    x3::logInfo(b);
    return made;
}

// ===========================================================================
// PHASE 3 — apron, driveway, collision, structures
// ===========================================================================
GasStationBuildResult GasStationWorld::build(Scene& scene,
                                             x3::rhi::IRenderDevice& device,
                                             x3::phys::IPhysicsWorld& phys) {
    GasStationBuildResult out;
    if (m_built) return out;
    m_built = true;

    SurfaceLibrary& surf = stationSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    // One world, one concrete — the same family the tunnel portals and the
    // bridge wear. The driveway lap wears the road's own asphalt so the joint
    // between forecourt and pavement is a material change, not a seam.
    const SurfaceSet& concrete = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& asphalt  = surf.get(device, "rd_asphalt_01");

    auto upload = [&](MeshBuf& m, const SurfaceSet* set, const float tint[4],
                      bool collide) {
        if (m.empty()) return;
        Entity e;
        e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                   m.i.data(), (uint32_t)m.i.size());
        if (!e.mesh.valid()) return;
        if (set && set->ok) { e.tex = set->albedo; e.mrTex = set->mr; e.normalTex = set->normal; }
        for (int c = 0; c < 4; ++c) e.baseColor[c] = tint[c];
        scene.add(e);
        ++out.meshCount;
        out.triCount += (uint32_t)(m.i.size() / 3);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) {
                cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]);
            }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
            out.colliderTris += (uint32_t)(m.i.size() / 3);
        }
    };

    const float pale[4] = { 0.90f, 0.89f, 0.86f, 1.0f };
    const float dark[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    for (const GasStationSite& s : m_sites) {
        if (!s.ok) continue;
        const float dx = s.dirX, dz = s.dirZ;
        const float px = -dz, pz = dx;
        // THE PAVEMENT DATUM PAIR (NO_SLOP rule 4). kPaveProud is the road
        // ribbon's own lift off the graded datum (road_network.h); the forecourt
        // rides the SAME value, so pavement and apron are one continuous surface
        // and the car crosses the joint without a step. A change to one IS a
        // change to both.
        const float topY = s.padY + kPaveProud;
        auto W = [&](float lx, float lz, float y, float o[3]) {
            o[0] = s.x + dx * lx + px * lz;
            o[1] = y;
            o[2] = s.z + dz * lx + pz * lz;
        };

        MeshBuf slab, skirt, drive;

        // --- the forecourt slab, 6 x 10 panels (UV density ~7 m per tile) ---
        constexpr int kNx = 6, kNz = 10;
        for (int i = 0; i < kNx; ++i) {
            for (int j = 0; j < kNz; ++j) {
                const float x0 = -kStationHalfX + 2.0f*kStationHalfX*(float)i/kNx;
                const float x1 = -kStationHalfX + 2.0f*kStationHalfX*(float)(i+1)/kNx;
                const float z0 = -kStationHalfZ + 2.0f*kStationHalfZ*(float)j/kNz;
                const float z1 = -kStationHalfZ + 2.0f*kStationHalfZ*(float)(j+1)/kNz;
                float a[3],b[3],c[3],d[3];
                W(x0,z0,topY,a); W(x1,z0,topY,b); W(x1,z1,topY,c); W(x0,z1,topY,d);
                slab.quad(a,b,c,d, 3.0f);       // normal +Y
            }
        }

        // --- the SKIRT: a poured edge, not a floating sheet -----------------
        // Vertical face 0.35 m, then a battered face reaching 1.1 m out and down
        // to 0.5 m BELOW the carved ground, so wherever the country falls away
        // the slab still reads as a structure with a base. Open on the frontage
        // (that edge is the driveway).
        auto skirtRun = [&](float lx0, float lz0, float lx1, float lz1,
                            float outX, float outZ) {
            constexpr int kSeg = 12;
            for (int k = 0; k < kSeg; ++k) {
                const float t0 = (float)k / kSeg, t1 = (float)(k+1) / kSeg;
                const float ax = lx0 + (lx1-lx0)*t0, az = lz0 + (lz1-lz0)*t0;
                const float bx = lx0 + (lx1-lx0)*t1, bz = lz0 + (lz1-lz0)*t1;
                float a[3],b[3],c[3],d[3];
                W(ax,az,topY,a);        W(bx,bz,topY,b);
                W(bx,bz,topY-0.35f,c);  W(ax,az,topY-0.35f,d);
                skirt.quad(a,b,c,d, 2.0f);
                // battered face down to under the ground
                float g0[3], g1[3];
                W(ax + outX*1.1f, az + outZ*1.1f, 0.0f, g0);
                W(bx + outX*1.1f, bz + outZ*1.1f, 0.0f, g1);
                const float y0 = std::min(topY - 0.6f, terrainHeightAtWorld(g0[0], g0[2]) - 0.5f);
                const float y1 = std::min(topY - 0.6f, terrainHeightAtWorld(g1[0], g1[2]) - 0.5f);
                W(ax,az,topY-0.35f,a);                  W(bx,bz,topY-0.35f,b);
                W(bx + outX*1.1f, bz + outZ*1.1f, y1, c);
                W(ax + outX*1.1f, az + outZ*1.1f, y0, d);
                skirt.quad(a,b,c,d, 2.0f);
            }
        };
        skirtRun(-kStationHalfX, -kStationHalfZ, -kStationHalfX,  kStationHalfZ, -1.0f, 0.0f);
        skirtRun(-kStationHalfX,  kStationHalfZ,  kStationHalfX,  kStationHalfZ, 0.0f,  1.0f);
        skirtRun( kStationHalfX, -kStationHalfZ, -kStationHalfX, -kStationHalfZ, 0.0f, -1.0f);

        // --- the DRIVEWAY: frontage -> the road's cement apron --------------
        // A trapezoid that narrows toward the road (a real mouth flares), level
        // at the forecourt datum, and LAPPED onto the pavement — a few
        // millimetres proud over the last 3 m, never coplanar, or the two
        // surfaces z-fight along the joint (the junction-mouth rule).
        {
            const float endX = kStationHalfX + s.driveLenM;
            constexpr int kSeg = 8;
            for (int k = 0; k < kSeg; ++k) {
                const float t0 = (float)k / kSeg, t1 = (float)(k+1) / kSeg;
                const float x0 = kStationHalfX + (endX - kStationHalfX) * t0;
                const float x1 = kStationHalfX + (endX - kStationHalfX) * t1;
                const float w0 = kDriveHalfZ * (1.0f - 0.25f * t0);
                const float w1 = kDriveHalfZ * (1.0f - 0.25f * t1);
                const float y0 = topY + (t0 > 0.75f ? 0.006f : 0.0f);
                const float y1 = topY + (t1 > 0.75f ? 0.006f : 0.0f);
                float a[3],b[3],c[3],d[3];
                W(x0,-w0,y0,a); W(x1,-w1,y1,b); W(x1,w1,y1,c); W(x0,w0,y0,d);
                drive.quad(a,b,c,d, 2.0f);
                // driveway skirts, both sides
                for (int side = -1; side <= 1; side += 2) {
                    const float l0 = w0 * side, l1 = w1 * side;
                    float g0[3], g1[3];
                    W(x0, l0 + 0.9f*side, 0.0f, g0);
                    W(x1, l1 + 0.9f*side, 0.0f, g1);
                    const float b0 = std::min(topY - 0.5f, terrainHeightAtWorld(g0[0], g0[2]) - 0.4f);
                    const float b1 = std::min(topY - 0.5f, terrainHeightAtWorld(g1[0], g1[2]) - 0.4f);
                    float q[4][3];
                    W(x0, l0, y0, q[0]); W(x1, l1, y1, q[1]);
                    W(x1, l1 + 0.9f*side, b1, q[2]); W(x0, l0 + 0.9f*side, b0, q[3]);
                    if (side < 0) drive.quad(q[0], q[1], q[2], q[3], 2.0f);
                    else          drive.quad(q[3], q[2], q[1], q[0], 2.0f);
                }
            }
        }

        upload(slab,  &concrete, pale, true);
        upload(skirt, &concrete, pale, true);
        upload(drive, &asphalt,  dark, true);
        ++out.stations;
    }

    // --- THE STRUCTURES ---------------------------------------------------
    // Visual instances through the EnvArt overlay (the pattern road_trees uses),
    // plus a CPU read of the same GLB to hand its triangles to the physics
    // world. Two reads of one file, on purpose: the engine loader uploads
    // straight to the GPU and hands back opaque handles by design, so a
    // colliding prop needs the CPU-side triangles from app/glb_cpu_read.h.
    if (out.stations > 0) {
        m_art = std::make_unique<EnvArtSystem>();
        // The pack's atlases bake metallic near 1 on the kiosk trim; unclamped
        // that renders BLACK under ACES (X3_WORLD_RULES rule 5, the black-prop
        // plague). 0.25 keeps a little sheen and lets the albedo light.
        m_art->setMetallicClamp(0.25f);
        const bool mounted = m_art->beginFromDir(device, convertedGlbRoot());

        const std::string glbPath = convertedGlbRoot() + std::string("/") + kStationGlb;
        GlbModel cpu = readGlbForLod(glbPath, /*minTriangles=*/1);
        if (!cpu.ok)
            x3::logWarn(std::string("[gas] structure collision unavailable: ") + cpu.error);

        for (const GasStationSite& s : m_sites) {
            if (!s.ok) continue;
            const float dx = s.dirX, dz = s.dirZ;
            // Column-major: col0 = local +X -> the road, col2 = col0 x +Y.
            const float T[16] = {
                dx,   0.0f, dz,   0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                -dz,  0.0f, dx,   0.0f,
                s.x,  s.padY + kPaveProud + 0.015f, s.z, 1.0f
            };
            if (mounted && m_art->addGlbInstance(kStationGlb, T)) ++out.structures;

            if (cpu.ok) {
                for (const GlbPrimitive& p : cpu.prims) {
                    std::vector<float> cv; cv.reserve(p.verts.size() * 3);
                    for (const auto& mv : p.verts) {
                        const float lx = mv.pos[0], ly = mv.pos[1], lz = mv.pos[2];
                        cv.push_back(T[0]*lx + T[8]*lz + T[12]);
                        cv.push_back(ly + T[13]);
                        cv.push_back(T[2]*lx + T[10]*lz + T[14]);
                    }
                    phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                                       p.idx.data(), (uint32_t)p.idx.size());
                    out.colliderTris += (uint32_t)(p.idx.size() / 3);
                }
            }
        }
    }

    out.ok = out.stations > 0;
    char b[256];
    std::snprintf(b, sizeof(b),
        "[gas] %u stations: %u apron meshes / %u tris, %u structures, %u collider tris",
        out.stations, out.meshCount, out.triCount, out.structures, out.colliderTris);
    x3::logInfo(b);
    if (out.stations > 0 && out.structures == 0)
        x3::logWarn("[gas] forecourts built but the structure GLB did not load — "
                    "run tools/w5_build_station_glb.py and asset_store.py fetch --all");
    return out;
}

uint32_t GasStationWorld::draw(x3::rhi::IRenderDevice& device,
                               const x3::rhi::FrameContext& frame) const {
    return m_art ? m_art->draw(device, frame) : 0u;
}

void GasStationWorld::shutdown(x3::rhi::IRenderDevice& device) {
    if (m_art) { m_art->destroy(device); m_art.reset(); }
}

void GasStationWorld::keepOutDiscs(std::vector<float>& outXZR) const {
    for (const GasStationSite& s : m_sites) {
        if (!s.ok) continue;
        // One disc over the lot, one over the driveway — a tree through the
        // canopy or standing in the mouth is exactly what this prevents.
        outXZR.push_back(s.x); outXZR.push_back(s.z);
        outXZR.push_back(std::max(kStationHalfX, kStationHalfZ) + 6.0f);
        const float mx = s.x + s.dirX * (kStationHalfX + s.driveLenM * 0.5f);
        const float mz = s.z + s.dirZ * (kStationHalfX + s.driveLenM * 0.5f);
        outXZR.push_back(mx); outXZR.push_back(mz);
        outXZR.push_back(kDriveHalfZ + 6.0f);
    }
}

// ===========================================================================
// PER-FRAME — proximity, prompt, fuel
// ===========================================================================
bool GasStationWorld::update(float dt, float carX, float carZ, float distanceM,
                             float load01, bool eHeld) {
    m_prompt  = nullptr;
    m_at      = -1;
    m_flowing = false;

    // CONSUMPTION. Off unless armed by `fuel_on 1` — the campaign's switch. The
    // load term keeps it honest: a car at full throttle drinks, one coasting
    // does not, so the number on the gauge tracks how the car is being driven
    // rather than how far it has travelled.
    if (m_fuel.consume && distanceM > 0.0f && m_fuel.litres > 0.0f) {
        const float load = 0.55f + 0.85f * std::min(1.0f, std::fabs(load01));
        m_fuel.litres = std::max(0.0f,
            m_fuel.litres - (distanceM * 0.001f) * (m_fuel.burnLPer100 * 0.01f) * load);
    }

    // PROXIMITY. The trigger is the space UNDER THE CANOPY, in station-local
    // coordinates — an oriented box, not a radius, because a forecourt is a
    // rectangle and a circle around its centre would arm the pump while the car
    // is still out on the driveway or parked against the kiosk.
    for (size_t i = 0; i < m_sites.size(); ++i) {
        const GasStationSite& s = m_sites[i];
        if (!s.ok) continue;
        const float rx = carX - s.x, rz = carZ - s.z;
        const float lx =  rx * s.dirX + rz * s.dirZ;          // local +X
        const float lz = -rx * s.dirZ + rz * s.dirX;          // local +Z
        if (lx < kCanopyMinX || lx > kCanopyMaxX) continue;
        if (lz < -kCanopyHalfZ || lz > kCanopyHalfZ) continue;
        m_at = (int)i;
        break;
    }

    if (m_at < 0) { m_fuel.pumpedL = 0.0f; return false; }

    const bool full = m_fuel.litres >= m_fuel.capacityL - 0.05f;
    if (full) {
        m_prompt = "TANK FULL";
    } else if (eHeld) {
        const float before = m_fuel.litres;
        m_fuel.litres = std::min(m_fuel.capacityL,
                                 m_fuel.litres + m_fuel.refuelLPerS * dt);
        m_fuel.pumpedL += m_fuel.litres - before;
        m_flowing = m_fuel.litres > before;
        // ARMING THE GAUGE. The lane spec: the fuel readout appears once the
        // mechanic has actually been used, so an unarmed stub adds nothing to
        // the screen. `fuel_on 1` arms it too (syncCVars).
        if (m_flowing) m_fuel.armed = true;
        m_prompt = "REFUELLING...";
    } else {
        m_prompt = "E  REFUEL";
    }
    return m_flowing;
}

// ===========================================================================
// THE PUMP'S HUD — see gas_station.h for why it is not inline in the host.
// ===========================================================================
void drawPumpPrompt(x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame, const char* text) {
    if (!text || !*text) return;
    uint32_t hw = 0, hh = 0;
    device.hudSize(hw, hh);
    if (!hw || !hh) return;
    // The host's existing prompt geometry, verbatim: drawHudText is MONO, so
    // strlen * px is the exact width. Shadow first at +1,+1, then the face —
    // the house style everywhere in this HUD.
    const float px = std::floor((float)hh * 0.026f);
    const float tw = (float)std::strlen(text) * px;
    const float tx = ((float)hw - tw) * 0.5f, ty = (float)hh * 0.86f;
    const float sh[4]  = { 0.0f, 0.0f, 0.0f, 0.75f };
    const float fgc[4] = { 1.0f, 0.93f, 0.72f, 1.0f };
    device.drawHudText(frame, text, tx + 1.0f, ty + 1.0f, px, sh);
    device.drawHudText(frame, text, tx, ty, px, fgc);
}

void drawFuelBar(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 const FuelTank& tank, bool flowing, float R, float gcx, float gcy) {
    // HIDDEN UNTIL ARMED — the first refuel, or `fuel_on 1`. Same reasoning as
    // the thermometer beside it: a gauge pinned at full, for a tank nothing
    // drains, teaches the player to stop looking at gauges.
    if (!tank.armed) return;
    const float R2 = R * 0.70f;
    const float bw = R * 1.34f, bh = R * 0.13f;
    const float bx = gcx - R * 0.67f;
    const float by = gcy + R + R * 0.12f + R2 * 0.10f + R * 0.34f;
    const float f  = std::min(1.0f, std::max(0.0f, tank.frac()));
    const float plate[4] = { 0.02f, 0.025f, 0.035f, 0.80f };
    const float track[4] = { 0.09f, 0.10f, 0.13f, 1.0f };
    // Amber at a quarter, red under an eighth — the colour ramp IS the low-fuel
    // warning, so an empty tank reads without a separate lamp.
    const float low = f < 0.125f ? 1.0f : (f < 0.25f ? 0.5f : 0.0f);
    const float fill[4] = { 0.35f + 0.62f * low, 0.85f - 0.55f * low,
                            0.95f - 0.80f * low, 1.0f };
    device.drawHudQuad(frame, bx - 3.0f, by - 3.0f, bw + 6.0f, bh + 6.0f, plate);
    device.drawHudQuad(frame, bx, by, bw, bh, track);
    device.drawHudQuad(frame, bx, by, bw * f, bh, fill);
    const float lp = R * 0.085f;
    const float lc[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
    device.drawHudText(frame, "E", bx - lp * 1.4f, by - lp * 0.1f, lp, lc);
    device.drawHudText(frame, "F", bx + bw + lp * 0.4f, by - lp * 0.1f, lp, lc);
    if (flowing) {
        char fbuf[48];
        std::snprintf(fbuf, sizeof(fbuf), "+%.0f L", (double)tank.pumpedL);
        const float fc[4] = { 0.55f, 1.0f, 0.7f, 1.0f };
        device.drawHudText(frame, fbuf, bx + bw * 0.5f - lp * 1.5f,
                           by - lp * 1.35f, lp, fc);
    }
}

// ===========================================================================
// CONSOLE
// ===========================================================================
void registerFuelCVars(x3::con::IConsole& console) {
    console.registerCVar("fuel_on",   "0",
        "arm the fuel mechanic: 1 = the car burns fuel and the gauge shows (0 = refuel-only stub)");
    console.registerCVar("fuel_burn", "14.5",
        "fuel consumption, litres per 100 km at full load (fuel_on 1)");
    console.registerCVar("fuel_cap",  "68",
        "fuel tank capacity, litres");
    console.registerCVar("fuel_rate", "22",
        "pump flow while holding E at a station, litres per second");
}

void GasStationWorld::syncCVars(const x3::con::IConsole& console) {
    // THE ARG CONVENTION does not apply here — these are cvars, read by value.
    m_fuel.consume     = console.getInt("fuel_on") != 0;
    m_fuel.burnLPer100 = std::max(0.0f, console.getFloat("fuel_burn"));
    const float cap    = console.getFloat("fuel_cap");
    if (cap > 1.0f && std::fabs(cap - m_fuel.capacityL) > 0.01f) {
        m_fuel.capacityL = cap;
        m_fuel.litres = std::min(m_fuel.litres, cap);
    }
    m_fuel.refuelLPerS = std::max(0.1f, console.getFloat("fuel_rate"));
    // Arming the mechanic shows the gauge immediately — a burn nobody can see
    // is NO_SLOP rule 6 with extra steps.
    if (m_fuel.consume) m_fuel.armed = true;
}

void GasStationWorld::registerConsole(x3::con::IConsole& console) {
    x3::con::IConsole* con = &console;
    GasStationWorld* self = this;
    // THE ARG CONVENTION (engine/core/IConsole.h): Console::exec STRIPS the
    // command name, so the first argument is args[0]. Written against args[1],
    // every command in this block would silently print its usage line forever.
    con->registerCommand("fuel", [con, self](const std::vector<std::string>& args) {
        if (!args.empty()) {
            const float v = (float)std::atof(args[0].c_str());
            self->fuel().litres = std::min(self->fuel().capacityL, std::max(0.0f, v));
            self->fuel().armed = true;
        }
        char b[160];
        std::snprintf(b, sizeof(b), "fuel %.1f / %.0f L (%.0f%%)  burn %s  gauge %s",
                      (double)self->fuel().litres, (double)self->fuel().capacityL,
                      (double)(self->fuel().frac() * 100.0f),
                      self->fuel().consume ? "ON" : "off",
                      self->fuel().armed ? "shown" : "hidden");
        con->print(b);
    }, "fuel [litres] - print or set the tank; see fuel_on / fuel_burn / fuel_cap");

    con->registerCommand("fuel_stations", [con, self](const std::vector<std::string>&) {
        if (self->sites().empty()) { con->print("fuel_stations: none planned"); return; }
        for (const GasStationSite& s : self->sites()) {
            char b[256];
            if (s.ok)
                std::snprintf(b, sizeof(b), "%-20s (%.0f, %.0f) y %.1f  %s  cut %.1f fill %.1f",
                              s.name.c_str(), (double)s.x, (double)s.z, (double)s.padY,
                              s.host, (double)s.cutM, (double)s.fillM);
            else
                std::snprintf(b, sizeof(b), "%-20s NOT SITED - %s", s.name.c_str(), s.whyNot);
            con->print(b);
        }
    }, "fuel_stations - list the sited gas stations and their measured cut/fill");
}

// ===========================================================================
// --test-gasstation
// ===========================================================================
bool runGasStationSelfTest() {
    bool pass = true;
    auto check = [&](bool cond, const char* what) {
        if (!cond) { pass = false; x3::logError(std::string("[gas][FAIL] ") + what); }
        else       { x3::logInfo (std::string("[gas][ok]   ") + what); }
    };

    clearTerrainCorridors();
    clearRoadJunctions();

    // Build the network the driving host builds, in the same order.
    std::vector<float> ringY;
    RoadSpec ringSpec = makeInnerCourse();
    const RoadBuildResult rr = registerRoad(ringSpec, &ringY);
    check(rr.ok, "inner tour registered");
    if (!rr.ok) return false;

    GasStationWorld gs;
    const uint32_t sited = gs.plan(&ringSpec, &ringY, nullptr, nullptr, nullptr, nullptr);
    check(sited >= 1, "at least one station sited off the freeway turnarounds");
    if (sited == 0) return false;

    const GasStationSite& s = gs.sites().front();

    // A1 — OFF THE PAVEMENT. The forecourt's nearest edge must clear the road's
    // outer paved edge, or the apron is laid over the running lanes.
    {
        float best = 1e9f;
        for (size_t i = 0; i < ringSpec.x.size(); ++i)
            best = std::min(best, std::hypot(ringSpec.x[i] - s.x, ringSpec.z[i] - s.z));
        check(best > s.roadEdgeM + kStationHalfX * 0.5f,
              "forecourt origin clears the road's paved edge");
    }

    // A2 — CONNECTED. The frontage must be within a driveway's length of the
    // pavement; a station you cannot drive onto is the whole defect this lane
    // exists to avoid.
    {
        const float fx = s.x + s.dirX * kStationHalfX;
        const float fz = s.z + s.dirZ * kStationHalfX;
        float best = 1e9f;
        for (size_t i = 0; i < ringSpec.x.size(); ++i)
            best = std::min(best, std::hypot(ringSpec.x[i] - fx, ringSpec.z[i] - fz));
        check(best <= s.roadEdgeM + kStationClearM + 6.0f,
              "frontage is within one driveway of the pavement");
    }

    // A3 — the mouth is NOTED, so the barrier planner leaves it open.
    const uint32_t jBefore = roadJunctionCount();
    const uint32_t pads = gs.registerPads();
    check(pads >= kCorridorsPerStation, "forecourt pads carved");
    check(roadJunctionCount() > jBefore, "driveway mouth noted as a road junction");
    check(distToNearestRoadJunction(s.roadX, s.roadZ) < 1.0f,
          "the noted junction is at the driveway mouth");

    // A4 — THE CARVE ACTUALLY FLATTENS IT. Measure the carved field over the
    // footprint: no point may stand above the apron top, or the slab has a hill
    // poking through it. (This is the gate that would have caught a corridor
    // registered with a spine-only depth profile.)
    {
        const float topY = s.padY + kPaveProud;
        float worst = -1e9f;
        for (int i = -4; i <= 4; ++i)
            for (int j = -6; j <= 6; ++j) {
                const float lx = (float)i / 4.0f * kStationHalfX;
                const float lz = (float)j / 6.0f * kStationHalfZ;
                const float wx = s.x + s.dirX*lx - s.dirZ*lz;
                const float wz = s.z + s.dirZ*lx + s.dirX*lz;
                worst = std::max(worst, terrainHeightAtWorld(wx, wz) - topY);
            }
        char b[128];
        std::snprintf(b, sizeof(b), "carved ground stays below the apron (worst %+.2f m)",
                      (double)worst);
        check(worst <= 0.35f, b);
    }

    // A5 — the fuel stub: the tank fills, the gauge arms, consumption is off by
    // default and burns when armed.
    {
        GasStationWorld f;
        f.plan(&ringSpec, &ringY, nullptr, nullptr, nullptr, nullptr);
        const GasStationSite& fs = f.sites().front();
        f.fuel().litres = 10.0f;
        check(!f.fuel().armed, "gauge starts hidden");
        // Not at a station: E does nothing.
        f.update(0.1f, fs.x + 500.0f, fs.z, 0.0f, 0.0f, true);
        check(f.fuel().litres == 10.0f && f.prompt() == nullptr,
              "E away from a station does nothing and says nothing");
        // Under the canopy: the hint, then the flow.
        const float cx = fs.x + fs.dirX * -1.6f, cz = fs.z + fs.dirZ * -1.6f;
        f.update(0.1f, cx, cz, 0.0f, 0.0f, false);
        check(f.prompt() && std::strcmp(f.prompt(), "E  REFUEL") == 0,
              "under the canopy the HUD offers E  REFUEL");
        f.update(1.0f, cx, cz, 0.0f, 0.0f, true);
        check(f.fuel().litres > 10.0f, "holding E fills the tank");
        check(f.fuel().armed, "the gauge arms on the first refuel");
        // Consumption default OFF.
        const float before = f.fuel().litres;
        f.update(1.0f, fs.x + 500.0f, fs.z, 1000.0f, 1.0f, false);
        check(f.fuel().litres == before, "fuel_on 0: driving burns nothing");
        f.fuel().consume = true;
        f.update(1.0f, fs.x + 500.0f, fs.z, 1000.0f, 1.0f, false);
        check(f.fuel().litres < before, "fuel_on 1: driving burns fuel");
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    x3::logInfo(pass ? "[gas] --test-gasstation PASS" : "[gas] --test-gasstation FAIL");
    return pass;
}

} // namespace x3::game
