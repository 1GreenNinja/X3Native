// app/world_hosts/echo_region_builders.cpp — WP-2 (TIER2_STREAMING_PLAN.md §4).
//
// VERBATIM ports of host_echotropolis.cpp's build blocks (constants, ECHO_*
// env-var overrides, deterministic hashes, asset paths all unchanged) into
// the EchoRegion/EchoRegionCtx contract. Log lines are retagged "[region]"
// (was "--world echotropolis:"). Nothing here is a behavior change; where a
// host block reached into host state EchoRegionCtx doesn't expose, the gap
// is ported as far as possible and flagged with a loud `// INTEGRATOR:`
// comment at the exact spot — see the per-block notes below and in-file.
//
// PORT STATUS LEDGER (§1 "owns" column, WP-2's row):
//   crown             — DONE except streetProps (INTEGRATOR: needs npcLife,
//                        not in Ctx) and the streetLamps->selectLights() live
//                        query (INTEGRATOR: dynamic per-frame nearest-K, not
//                        the static addLights() slice contract).
//   west_shoulder     — DONE except beam's night-only draw gate (INTEGRATOR:
//                        needs TimeOfDay, not in Ctx — see the block below,
//                        this is the single highest-risk gap for the
//                        milestone-A byte-compare). Miners-crew re-attach is
//                        lifecycle wiring (plan §2), not builder content —
//                        out of scope for a build()-only function signature.
//   district_urban/
//   district_recife/
//   district_hivemind — DONE. Data-driven exactly like the host (reads
//                        districts.txt, tag-filtered). Meshes: this file's
//                        loadDistrictInto(). Lights: WP-3's echo_woodlands.h
//                        `harvestDistrictLights()` (the light-slicing plan §4
//                        WP-3 describes) — buildOneDistrict() below calls
//                        both and feeds the result to EchoRegion::addLights.
//                        No WP-2/WP-3 overlap: confirmed against the actual
//                        landed echo_woodlands.h/.cpp (not just the plan
//                        text) and wired to call it directly.
//   harbor_bay        — DONE, no gaps (boats need only ctx.device).
//   woodlands cells   — WP-3's (echo_woodlands.h `buildWoodlandsCell`), not
//                        attempted here; this file's only echo_woodlands.*
//                        dependency is `harvestDistrictLights` (above).
//
// OTHER INTEGRATOR NOTES (apply across multiple blocks, called out once here
// so they aren't repeated at every site):
//   - EchoRegion::setScene(Scene*) (per echo_regions.h, now landed from WP-1)
//     takes a raw, non-owning pointer with no destroy contract ("NOT owned",
//     its own doc comment). Every Scene this file registers (crown's
//     lampScene, west_shoulder's mineGlowScene) is `new Scene()`'d and
//     intentionally leaked, mirroring EnvArtSystem's own pre-WP-4 "no GPU
//     teardown" reality. Per echo_regions.h's RESIDENCY CONTRACT, the
//     registered builder callback runs only ONCE per EchoRegion's lifetime
//     (deactivate()->build() cycles reuse existing content — no rebuild, no
//     re-leak), so this does NOT leak on every M-C eviction/reactivation as
//     an earlier draft of this note claimed. It DOES leak once per true
//     destroy()->build() cycle (M-D territory, once WP-4's EnvArtSystem::
//     destroy() lands) — narrower than originally flagged, but still real:
//     EchoRegion/WP-4 should give Scene the same real-ownership treatment
//     destroy() gives EnvArtSystem before M-D eviction gets aggressive.
//   - `cityDir` in EchoRegionCtx (plan §3) had no single obvious literal to
//     map it to among this file's many hardcoded pack directories (towers,
//     districts x3, mine, condos, hackables, boats x6, drones x2 — every one
//     a distinct literal in the host, several with their own ECHO_*_DIR
//     overrides). Left unused here rather than guessing; `modelsDir`,
//     `districtsTxt`, `vegDir`, `houseForgeDir` ARE used where they map
//     cleanly to a single host constant (beam, districts.txt, mine
//     forest/houses respectively).

#include "echo_region_builders.h"
#include "echo_woodlands.h"   // WP-3's harvestDistrictLights() — see loadDistrictInto's note

#include "../env_art.h"
#include "../mine_fx.h"
#include "../street_lights.h"
#include "../scene.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// ---- deterministic hashes, reproduced verbatim from every host call site
// (h01/hh/hashi were all the SAME LCG-ish mix under different local names —
// hh() below IS h01()/hh(); hashi() is the raw-uint32 condo variant). ----
inline float hh(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15;
    return (float)(n & 0xffffffu) / (float)0x1000000;
}
inline uint32_t hashi(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15;
    return n;
}

// infradir carries no ECHO_* override in the host (a fixed literal at every
// call site) — baked in here rather than threaded through every signature.
constexpr const char* kInfraDir = "D:/GameDev/EchoHarbor/assets/infra";

// Place a flat plane GLB (local 1x1 in X/Z, +Y up) as a road/deck: centre
// (cx,cz), height y, oriented yaw=atan2(dir.x,dir.z), scaled width(X) x
// length(Z). (host_echotropolis.cpp ~1524 `placeDeck` — shared by crown's
// STREETS/METRO infra AND, via loadDistrictInto below, each district's pad
// slab; the host captured one lambda by reference for both, this file
// reproduces it once at file scope for the same reason.) Returns true if
// the deck was added (for the crown infra piece-count log).
bool placeDeck(EchoRegion& region, EchoRegionCtx& ctx, const char* glb,
               float cx, float cz, float y, float yaw, float width, float len) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float T[16] = { c*width,0,-s*width,0,  0,1,0,0,  s*len,0,c*len,0,  cx, y, cz, 1 };
    auto e = std::make_unique<EnvArtSystem>();
    if (e->buildFromGlbAt(ctx.device, kInfraDir, glb, T)) { region.addArt(std::move(e)); return true; }
    return false;
}

// A concrete support column from ground up to just under deck height topY.
// (host ~1547 `placePillar`; crown METRO only — the FREEWAY NETWORK's own
// pillar calls are NOT ported, see buildCrown's infra section.) `carY` is
// the caller's no-heightfield fallback ground (host's captured `kCarY`).
bool placePillar(EchoRegion& region, EchoRegionCtx& ctx, float x, float z,
                  float topY, float w, float carY) {
    const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : carY;
    const float h = std::max(2.0f, topY - gy);
    const float T[16] = { w,0,0,0, 0,h,0,0, 0,0,w,0, x, gy + h*0.5f, z, 1 };
    auto e = std::make_unique<EnvArtSystem>();
    if (e->buildFromGlbAt(ctx.device, kInfraDir, "pillar.glb", T)) { region.addArt(std::move(e)); return true; }
    return false;
}

// The METROPOLIS DISTRICTS loader (host_echotropolis.cpp ~1716-1824
// `loadDistrict` lambda) — MESH-INSTANCING HALF ONLY. The host's lambda did
// both the prefab placement AND the light harvest in one pass over
// `pieces`; that split cleanly along a WP-2/WP-3 file-ownership seam (plan
// §4: WP-3 "extract loadDistrict's light harvesting so each district
// builder returns its own vector<PointLight> slice"), and WP-3's
// echo_woodlands.h landed exactly that as `harvestDistrictLights()` — a
// second, independent pass over the SAME .layout file that returns only the
// PointLight slice (position-only; it deliberately skips the rotation math
// since a light's position never needed it — see that function's doc
// comment). `buildOneDistrict` below calls both: this function for meshes,
// `harvestDistrictLights` for lights. (The .layout file is read twice as a
// result — once per pass — a small, harmless perf cost for keeping the two
// WPs' code independently correct and independently testable; not a
// behavior change to what gets drawn/lit.) Every constant, the terrain-seat
// sampling, and the mesh-fix axis correction below are verbatim.
void loadDistrictInto(EchoRegion& region, EchoRegionCtx& ctx,
                       const char* layoutPath, const char* glbDir,
                       float padX, float padZ, float padYaw, float padScale,
                       const char* meshFix, float padYOff, const char* tag) {
    std::ifstream lf(layoutPath);
    if (!lf) { x3::logWarn(std::string("[region] district layout missing: ") + layoutPath); return; }
    auto d = std::make_unique<EnvArtSystem>();
    if (!d->beginFromDir(ctx.device, glbDir)) return;
    d->setMetallicClamp(0.22f);   // BLACK-PROP fix: packed MRAOH maps bake metallic~1

    struct Piece { std::string glb; float px,py,pz,qx,qy,qz,qw,sx,sy,sz; };
    std::vector<Piece> pieces; pieces.reserve(4096);
    float lminx = 1e9f, lmaxx = -1e9f, lminz = 1e9f, lmaxz = -1e9f;
    {
        std::string line; char name[256]; Piece pp;
        while (std::getline(lf, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (std::sscanf(line.c_str(), "%255s %f %f %f %f %f %f %f %f %f %f",
                            name,&pp.px,&pp.py,&pp.pz,&pp.qx,&pp.qy,&pp.qz,&pp.qw,&pp.sx,&pp.sy,&pp.sz) != 11) continue;
            pp.glb = name; pieces.push_back(pp);
            lminx = std::min(lminx, pp.px); lmaxx = std::max(lmaxx, pp.px);
            lminz = std::min(lminz, pp.pz); lmaxz = std::max(lmaxz, pp.pz);
        }
    }
    if (pieces.empty()) return;
    const float cw = (lmaxx - lminx) * padScale + 40.0f;   // slab = content + 20m margin
    const float cl = (lmaxz - lminz) * padScale + 40.0f;
    const float ccx = padX + (lminx + lmaxx) * 0.5f * padScale;
    const float ccz = padZ + (lminz + lmaxz) * 0.5f * padScale;
    float gy = ctx.hf.ok() ? ctx.hf.heightAt(ccx, ccz) : 190.0f;
    if (ctx.hf.ok()) {
        for (float ox = -cw*0.5f; ox <= cw*0.5f; ox += 20.0f)
            for (float oz = -cl*0.5f; oz <= cl*0.5f; oz += 20.0f)
                gy = std::max(gy, ctx.hf.heightAt(ccx + ox, ccz + oz));
        gy += 1.0f;   // safety: heightfield bumps narrower than the stride
    }
    placeDeck(region, ctx, "road_asphalt.glb", ccx, ccz, gy - 0.05f, 0.0f, cw, cl);
    const float S = padScale, pc = std::cos(padYaw) * S, ps = std::sin(padYaw) * S;
    const float P[9] = { pc, 0.0f, -ps,   0.0f, S, 0.0f,   ps, 0.0f, pc };
    int placed = 0;
    for (const Piece& pcs : pieces) {
        const char* name = pcs.glb.c_str();
        const float px=pcs.px, py=pcs.py, pz=pcs.pz, qx=pcs.qx, qy=pcs.qy, qz=pcs.qz, qw=pcs.qw,
                    sx=pcs.sx, sy=pcs.sy, sz=pcs.sz;
        float L0[9] = {
            (1-2*(qy*qy+qz*qz))*sx, (2*(qx*qy+qz*qw))*sx, (2*(qx*qz-qy*qw))*sx,   // col0
            (2*(qx*qy-qz*qw))*sy,   (1-2*(qx*qx+qz*qz))*sy, (2*(qy*qz+qx*qw))*sy, // col1
            (2*(qx*qz+qy*qw))*sz,   (2*(qy*qz-qx*qw))*sz,  (1-2*(qx*qx+qy*qy))*sz };
        float L[9];
        {
            const char ax = (meshFix && (*meshFix=='x'||*meshFix=='y'||*meshFix=='z')) ? *meshFix : 'x';
            const float deg = (meshFix && *meshFix) ?
                (float)std::atof((*meshFix=='x'||*meshFix=='y'||*meshFix=='z') ? meshFix+1 : meshFix) : 0.0f;
            const float a = deg * 0.01745329252f;
            const float ca = std::cos(a), sa = std::sin(a);
            float F[9];
            if      (ax=='y') { const float G[9]={ca,0,-sa, 0,1,0, sa,0,ca}; std::copy(G,G+9,F); }
            else if (ax=='z') { const float G[9]={ca,sa,0, -sa,ca,0, 0,0,1}; std::copy(G,G+9,F); }
            else              { const float G[9]={1,0,0, 0,ca,sa, 0,-sa,ca}; std::copy(G,G+9,F); }
            for (int j = 0; j < 3; ++j)
                for (int r = 0; r < 3; ++r)
                    L[j*3+r] = L0[0*3+r]*F[j*3+0] + L0[1*3+r]*F[j*3+1] + L0[2*3+r]*F[j*3+2];
        }
        float W[9];   // W = P * L (column-major multiply)
        for (int j = 0; j < 3; ++j)
            for (int r = 0; r < 3; ++r)
                W[j*3+r] = P[0*3+r]*L[j*3+0] + P[1*3+r]*L[j*3+1] + P[2*3+r]*L[j*3+2];
        const float tx = padX + P[0]*px + P[3]*py + P[6]*pz;
        const float ty = gy + padYOff + P[1]*px + P[4]*py + P[7]*pz;
        const float tz = padZ + P[2]*px + P[5]*py + P[8]*pz;
        const float T[16] = { W[0],W[1],W[2],0, W[3],W[4],W[5],0, W[6],W[7],W[8],0, tx,ty,tz,1 };
        if (d->addGlbInstance(name, T)) ++placed;
        // (Light harvesting is NOT done here — see this function's doc
        // comment: harvestDistrictLights(), WP-3's echo_woodlands.*, does the
        // SAME per-piece name classification in its own pass over this file.)
    }
    float mn[3], mx[3]; d->worldBounds(mn, mx);
    x3::logInfo(std::string("[region] DISTRICT ") + tag + " — " +
                std::to_string(placed) + " prefabs placed at (" +
                std::to_string((int)padX) + "," + std::to_string((int)padZ) + "), world bounds X[" +
                std::to_string((int)mn[0]) + "," + std::to_string((int)mx[0]) + "] Y[" +
                std::to_string((int)mn[1]) + "," + std::to_string((int)mx[1]) + "] Z[" +
                std::to_string((int)mn[2]) + "," + std::to_string((int)mx[2]) + "]");
    if (placed > 0) region.addArt(std::move(d));
}

// assets/districts/districts.txt row: `tag|layout|glbDir|padX|padZ|padYaw|
// padScale|meshFix|padYOff` (host ~1826-1841). Reads the SAME file the host
// reads (data-driven, so moving a district in the .txt still moves it here,
// no rebuild) and returns only the row matching `wantTag`.
struct DistrictRow {
    std::string layout, dir, fix = "0";
    float x = 0, z = 0, yaw = 0, sc = 1, yoff = 0;
    bool found = false;
};
DistrictRow findDistrictRow(const std::string& districtsTxt, const char* wantTag) {
    DistrictRow row;
    std::ifstream mf(districtsTxt);
    std::string line;
    while (mf && std::getline(mf, line)) {
        if (line.empty() || line[0] == '#') continue;
        char tag[96], lay[512], dir[512], fix[16] = "0"; float x, z, yaw, sc, yoff = 0.0f;
        const int got = std::sscanf(line.c_str(), "%95[^|]|%511[^|]|%511[^|]|%f|%f|%f|%f|%15[^|]|%f",
                                    tag, lay, dir, &x, &z, &yaw, &sc, fix, &yoff);
        if (got >= 7 && std::string(tag) == wantTag) {
            row.layout = lay; row.dir = dir; row.x = x; row.z = z; row.yaw = yaw; row.sc = sc;
            row.fix = (got >= 8 ? fix : "0"); row.yoff = (got >= 9 ? yoff : 0.0f);
            row.found = true;
            break;
        }
    }
    return row;
}

void buildOneDistrict(EchoRegion& region, EchoRegionCtx& ctx, const char* wantTag) {
    const std::string path = ctx.districtsTxt.empty() ? "assets/districts/districts.txt" : ctx.districtsTxt;
    DistrictRow row = findDistrictRow(path, wantTag);
    if (!row.found) {
        x3::logWarn(std::string("[region] district `") + wantTag + "` not found in " + path);
        return;
    }
    // Meshes (this file) + lights (WP-3's echo_woodlands.h) — see
    // loadDistrictInto's doc comment for why these are two independent
    // passes over the same .layout file rather than one combined pass.
    loadDistrictInto(region, ctx, row.layout.c_str(), row.dir.c_str(),
                      row.x, row.z, row.yaw, row.sc, row.fix.c_str(), row.yoff, wantTag);
    region.addLights(harvestDistrictLights(ctx, row.layout.c_str(),
                                            row.x, row.z, row.yaw, row.sc, row.yoff, wantTag));
}

} // namespace

// ===========================================================================
// buildCrown — towers, houses, condos, crown-portion infra (streets + metro
// deck + subwayTrain), hackProps/hackDrone/vtolPolice, streetProps
// (INTEGRATOR stub), streetLamps+lampScene, drones. See host_echotropolis.cpp
// ~951-2043 + ~2878-2911 for the source blocks.
// ===========================================================================
void buildCrown(EchoRegion& region, EchoRegionCtx& ctx) {
    // ===================== REAL BUILDINGS (Phase B) ===== (host ~945-1006)
    {
        const std::string hdir = ctx.houseForgeDir.empty() ?
            "D:/Assets/_glb/prefab_buildings/HouseForge" : ctx.houseForgeDir;
        int housesBuilt = 0;
        auto addHouse = [&](const char* glb, float x, float z, float yaw, float lift) {
            const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
            const float s = 0.01f, c = std::cos(yaw), sn = std::sin(yaw);
            const float T[16] = { c*s, 0.0f, -sn*s, 0.0f,  0.0f, s, 0.0f, 0.0f,
                                  sn*s, 0.0f,  c*s, 0.0f,   x, gy + lift, z, 1.0f };
            auto h = std::make_unique<EnvArtSystem>();
            if (h->buildFromGlbAt(ctx.device, hdir, glb, T)) { region.addArt(std::move(h)); ++housesBuilt; }
        };
        // Five hero houses (hand-placed on the crown foothills).
        addHouse("PF_MetalHouse01.glb",      60.0f, 700.0f, 0.4f, 11.73f);
        addHouse("PF_MetalHouse02.glb",     125.0f, 745.0f, 2.1f,  2.13f);
        addHouse("PF_PrimitiveHouse01.glb",  10.0f, 675.0f, 3.6f,  3.17f);
        addHouse("PF_PrimitiveHouse02.glb", 150.0f, 690.0f, 5.0f,  1.00f);
        addHouse("PF_PrimitiveHouse03.glb",  85.0f, 640.0f, 1.2f,  8.59f);

        // NEIGHBOURHOOD DRAPE: deterministic hash jitter (no rand — identical
        // every launch/capture) rings around the crown.
        struct HouseDef { const char* glb; float lift; };
        static const HouseDef kCat[5] = {
            {"PF_MetalHouse01.glb", 11.73f}, {"PF_MetalHouse02.glb", 2.13f},
            {"PF_PrimitiveHouse01.glb", 3.17f}, {"PF_PrimitiveHouse02.glb", 1.00f},
            {"PF_PrimitiveHouse03.glb", 8.59f},
        };
        const float ringR[4] = { 135.0f, 215.0f, 300.0f, 395.0f };
        int neigh = 0;
        for (int r = 0; r < 4; ++r) {
            const int cnt = 7 + r * 3;                     // fuller outer rings
            for (int k = 0; k < cnt; ++k) {
                const uint32_t seed = (uint32_t)(r * 101 + k);
                const float ang = ((float)k + hh(seed) * 0.7f) * (6.2831853f / cnt);
                const float rr  = ringR[r] + (hh(seed * 7u + 3u) - 0.5f) * 46.0f;
                const float x = -20.0f + std::cos(ang) * rr;
                const float z = 760.0f + std::sin(ang) * rr;
                const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
                if (gy < 34.0f) continue;                  // shoreline / water → skip
                const HouseDef& hd = kCat[(uint32_t)(r + k * 2) % 5u];
                const float yaw = ang + 1.5708f + (hh(seed * 13u + 5u) - 0.5f) * 1.2f;
                addHouse(hd.glb, x, z, yaw, hd.lift);
                ++neigh;
            }
        }
        x3::logInfo("[region] REAL BUILDINGS — " +
                    std::to_string(housesBuilt) + " textured HouseForge houses (" +
                    std::to_string(neigh) + " draped into neighbourhoods)");
    }

    // ===================== DOWNTOWN SKYLINE (Urban Night City) ===== (host ~1008-1047)
    {
        const std::string cdir =
            "D:/Assets/_glb/tech/Urban Night City - Open World/Assets/GeeZyyGames/buildings/FBX";
        const float ts    = [](){ const char* e=std::getenv("ECHO_TOWER_SCALE"); return e?(float)std::atof(e):0.34f; }();
        const float tlift = [](){ const char* e=std::getenv("ECHO_TOWER_LIFT");  return e?(float)std::atof(e):0.0f;  }();
        const float tcx   = [](){ const char* e=std::getenv("ECHO_TOWER_X");     return e?(float)std::atof(e):-20.0f; }();
        const float tcz   = [](){ const char* e=std::getenv("ECHO_TOWER_Z");     return e?(float)std::atof(e):760.0f; }();
        const float sceneCX = 120.5f, sceneCZ = 174.9f;      // baked layout centre
        const float gy = ctx.hf.ok() ? ctx.hf.heightAt(tcx, tcz) : 190.0f;
        const float Tx = tcx - ts * sceneCX;
        const float Tz = tcz - ts * sceneCZ;
        const float M[16] = { ts,0,0,0,  0,ts,0,0,  0,0,ts,0,  Tx, gy + tlift, Tz, 1 };
        static const char* kBld[] = {
            "Building 01","Building 02","Building 03","Building 04","Building 05",
            "Building 06","Building 07","Building 08","Building 09","Building 10",
            "Building 11","Building 12","Building 14","Building 15","Building 16",
            "Building 17","Building 19","Building 20","Building 21","Building 22",
            "Building 23","Building 24","Building 25","Building 26","Building 27",
            "Building 28","Building 29","Building 33","Building 34","Building 35",
            "Building 36","Building 38","Building 39","Building 40","Building 41",
            "Building 43",
        };
        int towersBuilt = 0;
        for (const char* b : kBld) {
            auto t = std::make_unique<EnvArtSystem>();
            if (t->buildFromGlbAt(ctx.device, cdir, std::string(b) + ".glb", M)) { region.addArt(std::move(t)); ++towersBuilt; }
        }
        x3::logInfo("[region] DOWNTOWN SKYLINE — " +
                    std::to_string(towersBuilt) + " Urban Night City towers on the crown");
    }

    // ===================== CITY INFRASTRUCTURE — crown portion ===== (host ~1511-1638)
    // STREETS (asphalt+curbs down the 5 car lanes) + METRO (deck/pillars/
    // platform/subwayTrain). The FREEWAY NETWORK block (host ~1566-1618,
    // placeDeckP-pitched deck segments looping the whole metropolis) is
    // explicitly Lane C / host-persistent per plan §1 ("freeway/road
    // segments of infra outside the crown") — NOT ported; it stays put in
    // host_echotropolis.cpp for the integrator.
    EnvArtSystem* subwayPtr = nullptr;
    bool subwayBuilt = false;
    struct SubwayLine { float x, y, z0, z1, scale; } subwayLine{};
    {
        const float kCarY = ctx.hf.ok() ? ctx.hf.heightAt(-20.0f, 760.0f) : 190.0f;   // crown ground (= tower bases)
        int infraBuilt = 0;
        auto pd = [&](const char* glb, float cx, float cz, float y, float yaw, float w, float len) {
            if (placeDeck(region, ctx, glb, cx, cz, y, yaw, w, len)) ++infraBuilt;
        };
        auto pp = [&](float x, float z, float topY, float w) {
            if (placePillar(region, ctx, x, z, topY, w, kCarY)) ++infraBuilt;
        };
        // STREETS: asphalt + neon curbs down each of the 5 car lanes.
        const struct { float sx,sz,dx,dz,len; } rlanes[] = {
            {-330,702,1,0,620},{290,742,-1,0,620},{-330,818,1,0,620},{2,560,0,1,400},{-150,960,0,-1,400},
        };
        for (auto& L : rlanes) {
            const float yaw = std::atan2(L.dx, L.dz);
            const float mx = L.sx + L.dx*L.len*0.5f, mz = L.sz + L.dz*L.len*0.5f;
            const float ry = kCarY + 0.06f;
            pd("road_asphalt.glb", mx, mz, ry,        yaw, 15.0f, L.len);
            pd("road_curbs.glb",   mx, mz, ry + 0.02f, yaw, 15.0f, L.len);
        }
        // METRO: elevated N-S rail line crossing the crown at +11m (over the freeway).
        const float mgx=-60.0f, mz0=540.0f, mz1=980.0f, mlen=mz1-mz0, mmz=(mz0+mz1)*0.5f;
        const float mgy = ctx.hf.ok()?ctx.hf.heightAt(mgx,mmz):kCarY;
        const float my  = mgy + 11.0f;
        pd("rail_deck.glb", mgx, mmz, my, 0.0f, 6.0f, mlen);
        for (float z=mz0+16.0f; z<mz1; z+=40.0f) pp(mgx, z, my-0.3f, 2.2f);
        pd("freeway_deck.glb", mgx+7.0f, 760.0f, my+0.05f, 0.0f, 8.0f, 60.0f);   // station platform deck
        x3::logInfo("[region] INFRASTRUCTURE (crown streets+metro) — " +
                    std::to_string(infraBuilt) + " road/deck/pillar pieces");

        // SUBWAY TRAIN: the metro car sliding the elevated line (poseTrain each frame).
        const float kSubScale = [](){ const char* e=std::getenv("ECHO_SUBWAY_SCALE"); return e?(float)std::atof(e):1.0f; }();
        auto subwayTrain = std::make_unique<EnvArtSystem>();
        const float sI[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, mgx, my+0.4f, mmz, 1 };
        if (subwayTrain->buildFromGlbAt(ctx.device,
                "D:/Assets/_glb/tech/Subway Train/Assets/SubwayTrain", "SubwayTrain.glb", sI)) {
            subwayBuilt = true;
            subwayLine = { mgx, my + 0.4f, mz0, mz1, kSubScale };
            subwayPtr = subwayTrain.get();
            region.addArt(std::move(subwayTrain));
        }
    }

    // ===================== LIVING CONDOS ===== (host ~1654-1696)
    {
        const std::string condodir = "D:/GameDev/EchoHarbor/assets/interiors";
        static const char* kRooms[] = { "cond_tv.glb", "cond_kitchen.glb", "cond_romance.glb",
                                        "cond_kids.glb", "cond_novelist.glb" };   // domestic (lab is hidden)
        int condosBuilt = 0;
        auto room = [&](const char* glb, float x, float y, float z, float yaw, float s){
            const float c=std::cos(yaw), sn=std::sin(yaw);
            const float T[16] = { c*s,0,-sn*s,0, 0,s,0,0, sn*s,0,c*s,0, x, y, z, 1 };
            auto e = std::make_unique<EnvArtSystem>();
            if (e->buildFromGlbAt(ctx.device, condodir, glb, T)) { region.addArt(std::move(e)); ++condosBuilt; }
        };
        auto condo = [&](float wx, float wz, float yaw, int cols, int floors, float s,
                         uint32_t seed, bool hasLab){
            const float gy = ctx.hf.ok()?ctx.hf.heightAt(wx,wz):190.0f;
            const float rw = 3.75f*s, rh = 3.25f*s, c=std::cos(yaw), sn=std::sin(yaw);
            for (int f=0; f<floors; ++f)
              for (int j=0; j<cols; ++j) {
                const float lx = (j - (cols-1)*0.5f) * rw;
                const float x = wx + c*lx, z = wz - sn*lx, y = gy + f*rh;
                const char* g = kRooms[hashi(seed + f*7u + j*13u) % 5];
                if (hasLab && f==floors-1 && j==cols-1) g = "cond_lab.glb";   // the hidden lab, top corner
                room(g, x, y, z, yaw, s);
              }
        };
        // STREET-FRONT HIGH-RISES lining the north avenue (z=818 lane).
        condo(-100.0f, 842.0f, 3.14159f, 3, 5, 2.0f, 11u, false);
        condo( -20.0f, 842.0f, 3.14159f, 3, 6, 2.0f, 29u, true);   // block WITH the hidden lab
        condo(  60.0f, 842.0f, 3.14159f, 3, 5, 2.0f, 47u, false);
        x3::logInfo("[region] LIVING CONDOS — " + std::to_string(condosBuilt) +
                    " lit rooms (TV/kitchen/romance/kids/novelist + 1 hidden lab)");
    }

    // ===================== HACKABLES (Meshy-generated ctOS props) ===== (host ~1862-1924)
    EnvArtSystem* hackDronePtr = nullptr;
    EnvArtSystem* vtolPolicePtr = nullptr;
    {
        const std::string hdir = "D:/GameDev/EchoHarbor/assets/hackables";
        auto envf2 = [](const char* k, float d){ const char* e = std::getenv(k); return e ? (float)std::atof(e) : d; };
        const float sB = envf2("ECHO_BOLLARD_SCALE", 1.0f);
        const float sJ = envf2("ECHO_JBOX_SCALE", 1.0f);
        const float sC = envf2("ECHO_CAM_SCALE", 1.0f);
        int hackPropsBuilt = 0;
        auto place2 = [&](const char* glb, float x, float z, float yaw, float s, float lift){
            const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
            const float c = std::cos(yaw), sn = std::sin(yaw);
            const float T[16] = { c*s,0,-sn*s,0, 0,s,0,0, sn*s,0,c*s,0, x, gy + lift, z, 1 };
            auto e = std::make_unique<EnvArtSystem>();
            if (e->buildFromGlbAt(ctx.device, hdir, glb, T)) { region.addArt(std::move(e)); ++hackPropsBuilt; }
        };
        for (int i = 0; i < 4; ++i)                       // bollard line across the avenue mouth
            place2("bollard.glb", -44.0f + i * 5.0f, 806.0f, 0.0f, sB, 0.0f);
        // Wall props on the condo blocks' REAL facade spans (x -111..-89, -31..-9, +49..+71).
        place2("junction_box.glb", -104.0f, 841.2f, 3.14159f, sJ, 0.6f);   // block A facade
        place2("junction_box.glb",   64.0f, 841.2f, 3.14159f, sJ, 0.6f);   // block C facade
        place2("cam_wall.glb",  -14.0f, 841.5f, 3.14159f, sC, 7.5f);       // block B, high corner
        place2("cam_wall.glb",   54.0f, 841.5f, 3.14159f, sC, 7.5f);       // block C, high corner

        auto hackDrone = std::make_unique<EnvArtSystem>();
        const float I2[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, -20, 230, 760, 1 };
        if (hackDrone->buildFromGlbAt(ctx.device, hdir, "drone_police.glb", I2)) {
            hackDronePtr = hackDrone.get();
            region.addArt(std::move(hackDrone));
        }
        auto vtolPolice = std::make_unique<EnvArtSystem>();
        if (vtolPolice->buildFromGlbAt(ctx.device, hdir, "vtol_police.glb", I2)) {
            vtolPolicePtr = vtolPolice.get();
            region.addArt(std::move(vtolPolice));
        }
        x3::logInfo("[region] HACKABLES — " + std::to_string(hackPropsBuilt) +
                    " props (bollards/jboxes/cams) + " + (hackDronePtr ? "police drone" : "NO drone"));
    }

    // crown's slice of districtLights (plan §1 table) is EMPTY BY
    // CONSTRUCTION: the districtLights vector is harvested only from the 3
    // district pads (Urban 700,350 / Recife 950,1250 / HIVEMIND 1340,1000 —
    // assets/districts/districts.txt), none of which sit inside crown's
    // footprint; each district builder below returns its own non-empty
    // slice instead (see loadDistrictInto / buildOneDistrict).
    region.addLights({});

    // ===================== DISTRICT STREET LAMPS ===== (host ~1926-1953)
    // Plan §1 assigns the WHOLE streetLamps/lampScene system to crown even
    // though all 5 lamp rows physically sit at the 3 district pads (there is
    // no crown-local lamp row in the host — verified: buildDistrictLamps is
    // the system's ONLY call site). Followed verbatim/as-assigned rather
    // than re-splitting it into district slices, since the plan is explicit.
    // INTEGRATOR: see the file-banner Scene-lifetime note (leaked once per
    // true destroy()->build() cycle, NOT per M-C deactivate/reactivate —
    // build() is idempotent per echo_regions.h) — applies to
    // `lampScenePtr`/`streetLampsPtr` here too.
    // INTEGRATOR: `streetLampsPtr->selectLights(camPos, out, 8)` (host
    // ~2483/~4016, combined with district PointLights via
    // appendDistrictLights into device->setPointLights each frame) is a
    // dynamic per-frame nearest-K query over an unbounded internal lamp
    // list — a fundamentally different mechanism than EchoRegion's static
    // `addLights()` slice / EchoRegionSet::appendNearLights contract (§3).
    // Not wired here; WP-0/WP-1 must decide how street-lamp light selection
    // joins the new per-frame light-aggregation path.
    auto* lampScenePtr = new Scene();
    auto* streetLampsPtr = new StreetLights();
    {
        auto seatOf = [&](float cx, float cz){
            float gy = ctx.hf.ok() ? ctx.hf.heightAt(cx, cz) : 190.0f;
            if (ctx.hf.ok()) {
                for (int sx = -1; sx <= 1; ++sx) for (int sz = -1; sz <= 1; ++sz)
                    gy = std::max(gy, ctx.hf.heightAt(cx + sx*180.0f, cz + sz*180.0f));
                gy += 0.4f;
            }
            return gy;
        };
        const float rs = seatOf(950.0f, 1250.0f);    // Recife pad seat
        const float us = seatOf(700.0f, 350.0f);     // Urban bay pad seat
        const float hs = seatOf(1340.0f, 1000.0f);   // HIVEMIND pad seat
        const float rows[][6] = {
            { 975.0f, 1222.0f, 1090.0f, 1222.0f, rs, 26.0f },   // Recife alley N row
            { 975.0f, 1258.0f, 1090.0f, 1258.0f, rs, 26.0f },   // Recife alley S row
            { 560.0f,  350.0f,  840.0f,  350.0f, us, 30.0f },   // Urban main drag E-W
            { 700.0f,  240.0f,  700.0f,  470.0f, us, 30.0f },   // Urban cross street N-S
            {1240.0f, 1000.0f, 1440.0f, 1000.0f, hs, 30.0f },   // HIVEMIND main street
        };
        streetLampsPtr->buildDistrictLamps(*lampScenePtr, ctx.device, rows, 5);
    }
    region.setScene(lampScenePtr);

    // ===================== SKY DRONES ===== (host ~2020-2053)
    struct DronePose { EnvArtSystem* body; float cx,cz,r,y,w,phase; };
    std::vector<DronePose> dronePoses;
    const float kDroneScale = [](){ const char* e=std::getenv("ECHO_DRONE_SCALE"); return e?(float)std::atof(e):7.0f; }();
    {
        const std::string dronedirA = "D:/Assets/_glb/tech/Sci-Fi-Drone/Assets/scifi-drone/mesh";
        const std::string dronedirB = "D:/Assets/_glb/tech/Sci fi Drones/Assets/Sci_fi_Drones/Models";
        auto addDrone = [&](const std::string& dir, const char* glb, float cx, float cz,
                            float r, float y, float w, float phase) {
            auto d = std::make_unique<EnvArtSystem>();
            const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, cx, y, cz, 1 };
            if (d->buildFromGlbAt(ctx.device, dir, glb, I)) {
                dronePoses.push_back({ d.get(), cx, cz, r, y, w, phase });
                region.addArt(std::move(d));
            }
        };
        addDrone(dronedirA, "drone.glb",                    -20.0f,  760.0f, 150.0f, 210.0f,  0.26f, 0.0f);
        addDrone(dronedirA, "drone.glb",                    110.0f,  660.0f, 120.0f, 250.0f, -0.32f, 1.7f);
        addDrone(dronedirB, "Robot_Scout_HyperX_Unity.glb", -160.0f, 840.0f, 180.0f, 190.0f,  0.22f, 3.1f);
        addDrone(dronedirA, "drone.glb",                    -60.0f,  900.0f, 130.0f, 285.0f,  0.30f, 4.4f);
        addDrone(dronedirB, "Robot_Scout_HyperX_Unity.glb",  60.0f,  800.0f, 200.0f, 165.0f, -0.24f, 5.5f);
        addDrone(dronedirA, "drone.glb",                    -220.0f, 700.0f, 110.0f, 300.0f, -0.28f, 2.3f);
        x3::logInfo("[region] SKY DRONES — " + std::to_string(dronePoses.size()) + " drones");
    }

    // ===================== REAL STREET PROPS (Meshy vendor carts) ===== (host ~2875-2911)
    // INTEGRATOR: the host loop walks `npcLife.agentCount()/agent(vi)` to
    // find each live HotDogVendor position and hides that agent's blockout-
    // cart entity in walkScene once the real cart loads. NpcLife is Lane C /
    // host-only and is NOT exposed via EchoRegionCtx (§3: device, hf,
    // walkScene, asset-root strings only) — walkScene alone isn't enough to
    // find the vendors. Cannot be ported without extending Ctx (e.g. a
    // resolved vendor-position list, or an `NpcLife*`) or a two-phase hook
    // from WP-0 after npcLife builds. The verbatim buildXf transform helper
    // is reproduced below, ready to wire back in; the placement loop itself
    // is left as a comment (not compiled) rather than guessed at.
    {
        auto buildXf = [&](float x, float z, float yaw, float s, float lift, float T[16]){
            const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
            const float c = std::cos(yaw), sn = std::sin(yaw);
            T[0]=c*s; T[1]=0; T[2]=-sn*s; T[3]=0;  T[4]=0; T[5]=s; T[6]=0; T[7]=0;
            T[8]=sn*s; T[9]=0; T[10]=c*s; T[11]=0; T[12]=x; T[13]=gy+lift; T[14]=z; T[15]=1;
        };
        (void)buildXf;   // unused until the loop below is wired back in — see INTEGRATOR note
        x3::logWarn("[region] streetProps (vendor carts) NOT ported — needs npcLife, see INTEGRATOR note in echo_region_builders.cpp");
        /* ORIGINAL (host_echotropolis.cpp ~2891-2911), needs npcLife + walkScene entity-hide:
        if (npcLifeBuilt) {
            for (uint32_t vi = 0; vi < npcLife.agentCount(); ++vi) {
                const auto& va = npcLife.agent(vi);
                if (va.arch != x3::game::Archetype::HotDogVendor) continue;
                float T[16];
                buildXf(va.pos.x + 1.1f, va.pos.z, va.yaw, 1.3f, 1.25f, T);
                auto cart = std::make_unique<EnvArtSystem>();
                if (cart->buildFromGlbAt(ctx.device, x3::game::assetRoot() + "/meshy/props",
                                         "hotdog_cart.glb", T)) {
                    if (va.propEntity != x3::game::kNoLink && va.propEntity < ctx.walkScene.size())
                        ctx.walkScene.get(va.propEntity).visible = false;   // bye, yellow box
                    region.addArt(std::move(cart));
                }
            }
        }
        */
    }

    // ---- crown's single combined per-frame update (subway train, hack
    // drone, police VTOL, street-lamp flicker machines, sky drones) ----
    region.setUpdate([subwayPtr, subwayBuilt, subwayLine, hackDronePtr, vtolPolicePtr,
                       lampScenePtr, streetLampsPtr, dronePoses, kDroneScale]
                      (float dt, float t) {
        // METRO TRAIN (host poseTrain, ~1640-1651).
        if (subwayBuilt && subwayPtr) {
            const float len = subwayLine.z1 - subwayLine.z0;
            const float u = std::fmod(t * 22.0f, 2.0f * len);
            const float d = (u < len) ? u : (2.0f*len - u);
            const float dir = (u < len) ? 1.0f : -1.0f;
            const float yaw = std::atan2(0.0f, dir);       // faces travel dir along +Z
            const float s = subwayLine.scale, c=std::cos(yaw), sn=std::sin(yaw);
            const float z = subwayLine.z0 + d;
            const float M[16] = { c*s,0,-sn*s,0, 0,s,0,0, sn*s,0,c*s,0, subwayLine.x, subwayLine.y, z, 1 };
            subwayPtr->setInstanceTransform(0, M);
        }
        // HACK DRONE (host poseHackDrone, ~1903-1911).
        if (hackDronePtr) {
            const float w = t * 0.12f, r = 130.0f;
            const float x = -20.0f + r * std::cos(w), z = 760.0f + r * std::sin(w);
            const float y = 228.0f + std::sin(t * 0.7f) * 3.0f;
            const float yaw = w + 1.5708f, c = std::cos(yaw), sn = std::sin(yaw);
            const float sD = [](){ const char* e = std::getenv("ECHO_DRONE2_SCALE"); return e ? (float)std::atof(e) : 1.2f; }();
            const float M[16] = { c*sD,0,-sn*sD,0, 0,sD,0,0, sn*sD,0,c*sD,0, x, y, z, 1 };
            hackDronePtr->setInstanceTransform(0, M);
        }
        // POLICE VTOL (host poseVtol, ~1913-1923).
        if (vtolPolicePtr) {
            const float w = -t * 0.07f + 2.6f, r = 300.0f;
            const float x = -20.0f + r * std::cos(w), z = 760.0f + r * std::sin(w);
            const float y = 262.0f + std::sin(t * 0.5f) * 4.0f;
            const float yaw = w - 1.5708f, c = std::cos(yaw), sn = std::sin(yaw);
            const float sV = [](){ const char* e = std::getenv("ECHO_VTOL_SCALE"); return e ? (float)std::atof(e) : 3.2f; }();
            const float M[16] = { c*sV,0,-sn*sV,0, 0,sV,0,0, sn*sV,0,c*sV,0, x, y, z, 1 };
            vtolPolicePtr->setInstanceTransform(0, M);
        }
        // STREET LAMPS flicker machines (host streetLamps.update, ~2480/4010).
        if (streetLampsPtr && lampScenePtr) streetLampsPtr->update(dt, *lampScenePtr);
        // SKY DRONES (host poseDrone, ~2044-2053).
        for (const auto& d : dronePoses) {
            if (!d.body) continue;
            const float a = d.phase + t * d.w;
            const float x = d.cx + std::cos(a) * d.r;
            const float z = d.cz + std::sin(a) * d.r;
            const float y = d.y + std::sin(t * 1.3f + d.phase) * 6.0f;   // hover bob
            const float heading = a + (d.w > 0.0f ? 1.5708f : -1.5708f);
            const float s = kDroneScale, ch = std::cos(heading), sh = std::sin(heading);
            const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
            d.body->setInstanceTransform(0, M);
        }
    });
}

// ===========================================================================
// buildWestShoulder — mineProps, mineForest, mineGlowScene, beam. See
// host_echotropolis.cpp ~889-913, ~1049-1135, ~1250-1271.
// ===========================================================================
void buildWestShoulder(EchoRegion& region, EchoRegionCtx& ctx) {
    const float kMineX = -480.0f, kMineZ = 850.0f;   // mine mouth — open west shoulder, clear of towers
    const float kLotX  = -556.0f, kLotZ  = 814.0f;   // truck lot — short trek SW
    const float kMineGy = ctx.hf.ok() ? ctx.hf.heightAt(kMineX, kMineZ) : 190.0f;
    const float kMineScale = [](){ const char* e=std::getenv("ECHO_MINE_SCALE"); return e?(float)std::atof(e):3.2f; }();
    const float kMineYaw   = [](){ const char* e=std::getenv("ECHO_MINE_YAW");   return e?(float)std::atof(e):2.35f; }();

    // ===================== GOLD MINE + TRUCK LOT ===== (host ~1049-1083)
    {
        const float kTruckScale = [](){ const char* e=std::getenv("ECHO_TRUCK_SCALE"); return e?(float)std::atof(e):1.0f; }();
        const float kMineLift   = [](){ const char* e=std::getenv("ECHO_MINE_LIFT");   return e?(float)std::atof(e):0.0f; }();
        int minePropsBuilt = 0;
        auto place = [&](const std::string& dir, const char* glb, float x, float z,
                         float yaw, float s, float lift) {
            const float gy = kMineGy;
            const float c = std::cos(yaw), sn = std::sin(yaw);
            const float T[16] = { c*s, 0, -sn*s, 0,  0, s, 0, 0,  sn*s, 0, c*s, 0,
                                  x, gy + lift, z, 1 };
            auto e = std::make_unique<EnvArtSystem>();
            if (e->buildFromGlbAt(ctx.device, dir, glb, T)) { region.addArt(std::move(e)); ++minePropsBuilt; }
        };
        place("D:/GameDev/EchoHarbor/assets/mine", "mine_site.glb",
              kMineX, kMineZ, kMineYaw, kMineScale, kMineLift);
        place("D:/Assets/_glb/tech/Industrial Small Truck Free/Assets/IndustrialSmallTruck/Art/fbx",
              "SmallTruck_1.glb", kLotX, kLotZ, 1.2f, kTruckScale, 0.0f);
        place("D:/Assets/_glb/tech/Mini Cargo Truck/Assets/MiniCargoTruck/FBX",
              "Truck1.glb", kLotX - 9.0f, kLotZ + 7.0f, 2.4f, kTruckScale, 0.0f);
        x3::logInfo("[region] GOLD MINE site + truck lot — " + std::to_string(minePropsBuilt) + " props");
    }

    // ===================== MINE FOREST ===== (host ~1085-1135)
    {
        static const char* kPines[] = {
            "tree_pineTallA.glb", "tree_pineTallB.glb", "tree_pineTallC.glb",
            "tree_pineDefaultA.glb", "tree_pineDefaultB.glb", "tree_pineRoundB.glb",
        };
        const std::string vdir = ctx.vegDir.empty() ? "D:/GameDev/EchoHarbor/assets/veg" : ctx.vegDir;
        const float cx = kMineX, cz = kMineZ;
        const float behind = std::atan2(cz - 760.0f, cx - (-20.0f));   // WNW, away from the city
        const float toCity = std::atan2(760.0f - cz, -20.0f - cx);     // approach wedge centre
        int mineForestBuilt = 0;
        auto plant = [&](float x, float z, float sc, float yaw, int variant){
            const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : kMineGy;
            if (ctx.hf.ok() && gy < 4.0f) return;                              // skip the sea only with real terrain
            const float dxm=x-cx, dzm=z-cz; if (dxm*dxm+dzm*dzm < 13.0f*13.0f) return;   // clear pad
            const float dxl=x-kLotX, dzl=z-kLotZ; if (dxl*dxl+dzl*dzl < 11.0f*11.0f) return; // clear lot
            const float c=std::cos(yaw), s=std::sin(yaw);
            const float T[16] = { c*sc,0,-s*sc,0, 0,sc,0,0, s*sc,0,c*sc,0, x, gy, z, 1 };
            auto e = std::make_unique<EnvArtSystem>();
            if (e->buildFromGlbAt(ctx.device, vdir, kPines[variant % 6], T)) {
                e->setFoliage(1.0f);                       // canopy wrap + back-translucency
                region.addArt(std::move(e));
                ++mineForestBuilt;
            }
        };
        uint32_t seed = 0;
        auto emit = [&](int count, float aCenter, float aSpread, float rMin, float rMax){
            for (int i=0;i<count;++i,++seed){
                float a = aCenter + (hh(seed*3u+1u)*2.0f-1.0f)*aSpread;
                float r = rMin + hh(seed*3u+2u)*(rMax-rMin);
                float x = cx + r*std::cos(a), z = cz + r*std::sin(a);
                float da = std::fabs(std::atan2(std::sin(a-toCity), std::cos(a-toCity)));
                if (da < 0.55f && r < 55.0f && hh(seed*3u+7u) < 0.7f) continue;  // thin the city-side approach
                float sc  = 10.0f + hh(seed*7u+5u)*16.0f;                        // 10-26 m
                float yaw = hh(seed*7u+3u)*6.2831853f;
                plant(x, z, sc, yaw, (int)(hh(seed*7u+9u)*6.0f));
            }
        };
        emit(74, 0.0f,   3.15159f, 15.0f,  80.0f);   // all-around inner ring (full circle)
        emit(64, behind, 1.9f,     20.0f, 125.0f);   // THICK behind (away from the city)
        emit(44, 0.0f,   3.15159f, 78.0f, 150.0f);   // outer belt, full circle
        x3::logInfo("[region] MINE FOREST — " + std::to_string(mineForestBuilt) + " pines ringing the pit");
    }

    // ===================== MINE MOUTH GLOW (authentic EoS arch) ===== (host ~1250-1271)
    // INTEGRATOR: see the file-banner Scene-lifetime note — `mineGlowScenePtr`
    // is `new Scene()`'d and intentionally leaked once per true destroy()->
    // build() cycle (M-D territory; NOT per M-C deactivate/reactivate) until
    // EchoRegion/WP-4 gains real Scene ownership.
    auto* mineGlowScenePtr = new Scene();
    {
        auto envf = [](const char* k, float d){ const char* e=std::getenv(k); return e?(float)std::atof(e):d; };
        const float lY  = envf("ECHO_GLOW_LY", 1.70f);    // mouth-centre height (GLB units)
        const float lZ  = envf("ECHO_GLOW_LZ", 0.90f);    // mouth depth (front of the adit = +Z local)
        const float lHW = envf("ECHO_GLOW_HW", 1.30f);    // half width  (GLB units)
        const float lHH = envf("ECHO_GLOW_HH", 1.55f);    // half height (GLB units)
        const float c = std::cos(kMineYaw), sn = std::sin(kMineYaw), s = kMineScale;
        const float gx = kMineX + s * (sn * lZ);          // same rotation as the mine place() transform
        const float gy = kMineGy + s * lY;
        const float gz = kMineZ + s * (c * lZ);
        const float gYaw = envf("ECHO_GLOW_YAW", kMineYaw);   // face outward along the mouth (+Z local)
        GoldMineWorld mineGlow;   // one-shot author step, matches host (not referenced again after build)
        mineGlow.buildMouthGlow(*mineGlowScenePtr, ctx.device, gx, gy, gz, s * lHW, s * lHH, gYaw);
        x3::logInfo("[region] MINE MOUTH GLOW (EoS arch) seated at mouth");
    }
    region.setScene(mineGlowScenePtr);

    // ===================== LIGHTHOUSE BEAM ===== (host ~889-913, poseBeam)
    // INTEGRATOR (single highest milestone-A risk in this file): the host
    // draws beam/fissure ONLY when `tod.sample().cityLightsOn`
    // (host_echotropolis.cpp ~2521-2525 headless capture, ~3715 pause menu,
    // ~4076-4080 main loop). EchoRegionCtx (§3) exposes no TimeOfDay /
    // cityLightsOn, so this port cannot reproduce that per-frame night gate:
    // as ported, `beam` is added unconditionally and sweeps/draws on EVERY
    // frame west_shoulder is resident, day or night. fissure stays host-
    // persistent (plan §1's Lane C list) and is untouched here.
    //   RISK: the default capture TOD is "golden" (host canonTodFraction ->
    //   elevation ~0.15; tod.cpp:196 sets cityLightsOn = elev < 0.08, so
    //   cityLightsOn == false at golden). The host therefore draws NO beam
    //   in the default byte-compare baseline; this port WOULD show one.
    //   Before trusting a milestone-A capture that includes west_shoulder,
    //   WP-0/WP-1 must either (a) thread a live `cityLightsOn` bool into
    //   EchoRegionCtx per frame and give EchoRegion a conditional-draw hook,
    //   or (b) gate this specific draw outside EchoRegionSet::drawAll as a
    //   stop-gap, or (c) pick capture cams/TODs that don't exercise it yet.
    {
        constexpr float kLightX = -493.24f, kLightY = -0.156f, kLightZ = 789.39f;
        constexpr float kLanternY = 25.75f;     // beam pivot above the props tower base
        constexpr float kBeamRate = 0.35f;      // rad/s sweep
        const char* mEnv = std::getenv("ECHO_MODELS_DIR");
        const std::string mDir = mEnv ? mEnv :
            (ctx.modelsDir.empty() ? "D:/GameDev/SimCityLLM2/refs/models" : ctx.modelsDir);
        auto beam = std::make_unique<EnvArtSystem>();
        const float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, kLightX,kLightY,kLightZ,1 };
        if (beam->buildFromGlbAt(ctx.device, mDir, "lighthouse_beam.glb", T)) {
            x3::logInfo("[region] lighthouse beam armed (ALWAYS-ON here — night-gate not portable, see INTEGRATOR note)");
            EnvArtSystem* beamPtr = beam.get();
            region.addArt(std::move(beam));
            region.setUpdate([beamPtr, kLightX, kLightY, kLightZ, kLanternY, kBeamRate](float /*dt*/, float t) {
                const float theta = t * kBeamRate;   // == host's poseBeam(waterTime * kBeamRate)
                const float c = std::cos(theta), s = std::sin(theta);
                const float M[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, kLightX, kLightY + kLanternY, kLightZ, 1 };
                beamPtr->setInstanceTransform(0, M);
            });
        }
    }

    // Miners crew re-attach on west_shoulder build/teardown (plan §2:
    // `minersSkin.deactivate(walkScene)` + stop `miners.update` on evict,
    // re-`build()` on rebuild) is Lane-C lifecycle wiring against the
    // persistent `minersSkin`/`miners` pool, not builder CONTENT — and
    // EchoRegion::build()'s signature (§3) has no teardown-hook parameter to
    // attach it to. Left entirely to WP-0/WP-1 integration; nothing to port.
}

// ===========================================================================
// buildDistrictUrban / buildDistrictRecife / buildDistrictHivemind — each the
// matching districts.txt row, data-driven exactly like the host. See
// host_echotropolis.cpp ~1698-1844 and assets/districts/districts.txt.
// ===========================================================================
void buildDistrictUrban(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "URBAN DISTRICT");
}
void buildDistrictRecife(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "RECIFE 2050");
}
void buildDistrictHivemind(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "HIVEMIND CYBERCITY");
}

// ===========================================================================
// buildHarborBay — boats (3 lanes) + pose updates. See host_echotropolis.cpp
// ~1956-2018. No INTEGRATOR gaps (needs only ctx.device).
// ===========================================================================
void buildHarborBay(EchoRegion& region, EchoRegionCtx& ctx) {
    const float kBoatYaw = [](){ const char* e=std::getenv("ECHO_BOAT_YAW"); return e?(float)std::atof(e):0.0f; }();
    const float kBoatY   = [](){ const char* e=std::getenv("ECHO_BOAT_Y");   return e?(float)std::atof(e):0.6f; }();
    struct FleetDef { const char* dir; const char* glb; float scale; float speedMul; };
    static const FleetDef kFleet[] = {
        { "D:/Assets/_glb/tech/Medieval Ship/Assets/MedievalShip/MedievalShip 3D_Model",
          "MedievalShip_.glb", 1.0f, 0.55f },                                   // hero tall ship (~43m)
        { "D:/Assets/_glb/tech/Oceanis 2024 Pro URP Water Framework/ARTnGAME/Oceanis/Oceanis URP/DEMO ASSETS/SHIPS/dutch_ship_large_02",
          "dutch_ship_large_02_2k.glb", 1.0f, 0.6f },                           // Dutch full-rigger (~34m)
        { "D:/Assets/_glb/tech/Asian Fishing Village Environment/Assets/LeartesStudios/Asian_Fishing_Village/HDRP/Art/Meshes",
          "SM_Boat_1_net_01.glb", 2.0f, 0.9f },                                 // fishing trawler w/ net
        { "D:/Assets/_glb/tech/Old Rowboat/Assets/Boats/Legacy_Content/Imports",
          "RowBoat.glb", 1.0f, 0.8f },                                          // detailed wooden rowboat
        { "D:/Assets/_glb/tech/Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City/HIVEMIND/CyberpunkCity/HDRP(Default)/Art/Meshes/Props",
          "SM_Boat_B.glb", 0.02f, 1.2f },                                       // sci-fi hover skiff (cm pack)
        { "D:/Assets/_glb/ancients/Pirate Island/Assets/Hivemind/PirateIsland/HDRP(Default)/Art/Meshes/Props/JollyBoat",
          "SM_JollyBoat_01.glb", 1.5f, 0.9f },                                  // jolly-boat tender
    };
    struct BoatPose { EnvArtSystem* body; float sx,sz,dx,dz,len,speed,off,scale; };
    std::vector<BoatPose> boatPoses;
    auto addBoat = [&](const FleetDef& fd, float sx, float sz, float dx, float dz,
                       float len, float speed, float off){
        const float L = std::sqrt(dx*dx + dz*dz); dx/=L; dz/=L;
        auto b = std::make_unique<EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, sx, kBoatY, sz, 1 };
        if (b->buildFromGlbAt(ctx.device, fd.dir, fd.glb, I)) {
            boatPoses.push_back({ b.get(), sx, sz, dx, dz, len, speed*fd.speedMul, off, fd.scale });
            region.addArt(std::move(b));
        }
    };
    {
        struct BL { float sx,sz,dx,dz,len,speed; int n; };
        const BL bl[] = {
            { -400.0f,  330.0f,  1.0f,  0.0f, 760.0f, 15.0f, 3 },   // south bay, eastbound
            {  340.0f,  240.0f, -1.0f,  0.0f, 760.0f, 13.0f, 3 },   // south bay, westbound
            { -560.0f,  260.0f,  0.0f,  1.0f, 420.0f, 12.0f, 3 },   // SW inlet, northbound
        };
        int vi = 0;
        for (const BL& l : bl)
            for (int k = 0; k < l.n; ++k) {
                addBoat(kFleet[vi % 6], l.sx, l.sz, l.dx, l.dz, l.len, l.speed, l.len * (float)k / (float)l.n);
                ++vi;
            }
        x3::logInfo("[region] HARBOR FLEET — " + std::to_string(boatPoses.size()) + " vessels");
    }
    region.setUpdate([boatPoses, kBoatYaw, kBoatY](float /*dt*/, float t) {
        for (const auto& b : boatPoses) {
            if (!b.body) continue;
            const float d = std::fmod(b.off + t * b.speed, b.len);
            const float x = b.sx + b.dx * d, z = b.sz + b.dz * d;
            const float y = kBoatY + std::sin(t * 0.7f + b.off) * 0.35f;   // gentle bob
            const float heading = std::atan2(b.dx, b.dz) + kBoatYaw;
            const float s = b.scale, ch = std::cos(heading), sh = std::sin(heading);
            const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
            b.body->setInstanceTransform(0, M);
        }
    });
}

} // namespace x3::game
