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
#include "echo_sea.h"          // THE sea datum: keel draft / boat freeboard / land clearance
#include "echo_water.h"        // echoShipPose — hulls ride the ACTUAL Gerstner surface
#include "echo_interiors.h"    // condo-room sub-region + vendor dressing
#include "echo_roads.h"        // V8: the city block/lot/frontage generator
#include "../hackables.h"      // WD2 camera saturation (builders register cams)
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

// ===========================================================================
// V8 — LOT-DRIVEN PLACEMENT (Lane 4).
//
// WHAT THIS REPLACED. Four independent, geometry-blind placement systems that
// guessed at positions and were reconciled afterwards by DELETION:
//   * four concentric POLAR HASH RINGS around (-20,760), yaw = ring angle
//     + 90 deg +/- 34 deg of jitter (facing an imaginary circular road that
//     does not exist), radius jitter +/-23 m with NO min-spacing test at all,
//     asset picked by `(r + k*2) % 5` (a periodic ABCDE cycle), ground contact
//     from ONE heightAt() probe at the pivot;
//   * the district prefab pads, seated at MAX terrain height + 1 m;
//   * 36 skyline towers sharing ONE baked transform and ONE height probe;
//   * 20 hand-traced literal (x,z,yaw) waterfront triples.
// and `EchoRoads::corridorHits()` at four sites, deleting whatever landed on a
// road. All four vetoes are GONE: a building now comes from a LOT or from a
// FRONTAGE POINT, both of which are outside every road corridor by
// construction. See echo_roads.h's CITY BLOCKS section and --test-cityblocks.
// ===========================================================================

// A placed footprint, for the min-spacing test the rings never had. Stored as
// an oriented box: centre, local +X axis, half extents.
struct PlacedBox { float cx, cz, ax, az, halfW, halfD; };

inline void obbCorners(const PlacedBox& b, float c[4][2]) {
    // local +X = (ax,az); local +Z = (-az? no) — yaw's +Z is the perpendicular
    // (-az, ax) is +X rotated 90; we store +X directly and derive +Z.
    const float fx = -b.az, fz = b.ax;              // local +Z
    c[0][0] = b.cx + b.ax*b.halfW + fx*b.halfD; c[0][1] = b.cz + b.az*b.halfW + fz*b.halfD;
    c[1][0] = b.cx + b.ax*b.halfW - fx*b.halfD; c[1][1] = b.cz + b.az*b.halfW - fz*b.halfD;
    c[2][0] = b.cx - b.ax*b.halfW - fx*b.halfD; c[2][1] = b.cz - b.az*b.halfW - fz*b.halfD;
    c[3][0] = b.cx - b.ax*b.halfW + fx*b.halfD; c[3][1] = b.cz - b.az*b.halfW + fz*b.halfD;
}
bool obbOverlap(const PlacedBox& A, const PlacedBox& B) {
    float a[4][2], b[4][2];
    obbCorners(A, a); obbCorners(B, b);
    for (int poly = 0; poly < 2; ++poly) {
        const float (*p)[2] = poly ? b : a;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            float nx = -(p[j][1] - p[i][1]), nz = p[j][0] - p[i][0];
            const float l = std::sqrt(nx*nx + nz*nz);
            if (l < 1e-6f) continue;
            nx /= l; nz /= l;
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            for (int k = 0; k < 4; ++k) {
                const float pa = a[k][0]*nx + a[k][1]*nz, pb = b[k][0]*nx + b[k][1]*nz;
                a0 = std::min(a0, pa); a1 = std::max(a1, pa);
                b0 = std::min(b0, pb); b1 = std::max(b1, pb);
            }
            if (a1 <= b0 || b1 <= a0) return false;
        }
    }
    return true;
}

// The occupancy set for one region's placement pass. Everything that takes up
// ground goes in here — buildings, AND the crown's five hardcoded straight
// "crown lanes", which are a SECOND road system EchoRoads knows nothing about
// (they predate it). Registering them as occupancy instead of adding a fifth
// veto site keeps "no building on a road" a single mechanism.
struct Occupancy {
    std::vector<PlacedBox> boxes;
    bool free(float cx, float cz, float yaw, float halfW, float halfD) const {
        PlacedBox q{ cx, cz, std::cos(yaw), -std::sin(yaw), halfW, halfD };
        for (const PlacedBox& b : boxes) if (obbOverlap(q, b)) return false;
        return true;
    }
    void take(float cx, float cz, float yaw, float halfW, float halfD) {
        boxes.push_back({ cx, cz, std::cos(yaw), -std::sin(yaw), halfW, halfD });
    }
    void takeAxis(float cx, float cz, float dx, float dz, float halfLen, float halfWid) {
        const float l = std::sqrt(dx*dx + dz*dz);
        if (l < 1e-5f) return;
        boxes.push_back({ cx, cz, dx / l, dz / l, halfLen, halfWid });
    }
};

// One palette entry. `footW`/`footD` are the FOOTPRINT TABLE: authored nominal
// metres at the placement scale, refined at boot from the loaded GLB where the
// loader can report a real extent. The point of the table is that
// "does this building fit on this lot?" is a LOOKUP, not load-then-veto.
struct BuildingAsset {
    const char* glb;
    float weight;          // seeded WEIGHTED draw — replaces the `% 5` cycle
    float footW, footD;    // footprint (m) in local X / Z after `scale`
    float height;          // nominal height (m) — the ECHO_CITY_PROXY blockout
    float lift;            // pivot lift above the terrain seat
};
struct BuildingPalette {
    const char*          dir;
    const BuildingAsset* a;
    int                  n;
    float                scale;
};

// CROWN NEIGHBOURHOOD (HouseForge, cm-scale GLBs at 0.01). The five `lift`
// values are the host's originals. The footprints are AUTHORED nominal sizes:
// EnvArtSystem::worldBounds reports the AABB of drawable ORIGINS, not mesh
// extents, so it cannot measure a single-node prefab — see measurePalette().
const BuildingAsset kCrownHouseAssets[] = {
    { "PF_MetalHouse01.glb",     1.0f, 12.0f, 10.0f,  7.0f, 11.73f },
    { "PF_MetalHouse02.glb",     1.0f, 11.5f,  9.5f,  6.5f,  2.13f },
    { "PF_PrimitiveHouse01.glb", 1.7f, 10.5f,  9.0f,  6.0f,  3.17f },
    { "PF_PrimitiveHouse02.glb", 1.7f, 10.0f,  8.5f,  5.5f,  1.00f },
    { "PF_PrimitiveHouse03.glb", 1.3f, 11.0f,  9.5f,  6.5f,  8.59f },
};

// WATERFRONT PROMENADE (Urban Night City glass towers at 0.34).
const BuildingAsset kGlassTowerAssets[] = {
    { "Building 01.glb", 1.0f, 26.0f, 26.0f, 60.0f, 0.0f },
    { "Building 05.glb", 1.0f, 24.0f, 24.0f, 52.0f, 0.0f },
    { "Building 14.glb", 1.0f, 28.0f, 24.0f, 66.0f, 0.0f },
    { "Building 21.glb", 0.8f, 22.0f, 22.0f, 48.0f, 0.0f },
    { "Building 24.glb", 0.8f, 25.0f, 25.0f, 58.0f, 0.0f },
    { "Building 33.glb", 1.0f, 27.0f, 23.0f, 72.0f, 0.0f },
    { "Building 38.glb", 0.7f, 23.0f, 23.0f, 44.0f, 0.0f },
    { "Building 43.glb", 0.7f, 26.0f, 22.0f, 64.0f, 0.0f },
};

// ECHO_CITY_LEGACY=1 — THE FALLBACK CVAR (standing requirement 5). Restores the
// pre-V8 placement exactly: the four polar hash rings with their index-derived
// seeds, ring-angle-plus-jitter yaws, `% 5` asset cycle, single-probe seat and
// no min-spacing test — AND the corridorHits veto that used to clean up after
// them, reproduced locally (legacyCorridorHit below) so the comparison is
// honest rather than flattering. It is what the before/after captures in
// docs/screenshots/city-blocks were shot with; one binary, one camera.
bool cityLegacyOn() {
    static const bool on = [](){ const char* e = std::getenv("ECHO_CITY_LEGACY");
                                 return e && *e && *e != '0'; }();
    return on;
}

// The DELETED #34a corridor audit, kept alive ONLY inside the ECHO_CITY_LEGACY
// path. Byte-for-byte the test that used to live in EchoRoads::corridorHits:
// sample circles of radius (half corridor + clear + step/2) over every edge.
bool legacyCorridorHit(const EchoRoads* roads, float x, float z, float clear) {
    if (!roads) return false;
    for (const x3::game::RoadEdge& e : roads->graph().edges) {
        const bool elevated = e.cls == x3::game::RoadClass::Freeway ||
                              e.cls == x3::game::RoadClass::Ramp;
        const float half = e.width * 0.5f + (elevated ? 3.2f + 0.35f : 0.5f);
        const float r = half + clear + 2.0f;      // kShoulderW/kBarrierW/kSampleStep*0.5
        const float r2 = r * r;
        for (const x3::game::RoadSample& s : e.center) {
            const float dx = s.x - x, dz = s.z - z;
            if (dx * dx + dz * dz < r2) return true;
        }
    }
    return false;
}

// ECHO_CITY_PROXY=1 — draw a blockout mass at each building's exact footprint
// and yaw instead of the GLB. This exists so the LAYOUT can be photographed on
// a checkout where the building packs are absent (they live under D:/Assets on
// the authoring box and are not in the repo). OFF by default: unset, this file
// behaves exactly as if the proxy code were not here.
// How far a building's seat may sit from the finished surface of the street it
// fronts. Steps, driveways and a retaining course are metres; the crown-rim
// spill was 50-190 m. Generous enough that honest terrain is never refused.
constexpr float kMaxStreetOffset = 12.0f;

bool cityProxyOn() {
    static const bool on = [](){ const char* e = std::getenv("ECHO_CITY_PROXY");
                                 return e && *e && *e != '0'; }();
    return on;
}

// Boot-time refinement of the footprint table. Loads each palette entry ONCE
// (the loader caches by path, so the placement pass below re-uses the upload)
// and takes a real extent from it when the GLB has enough nodes for
// worldBounds to mean anything. Everything after this is table lookups.
struct FootprintTable {
    std::vector<float> w, d, wt;
    int   measured = 0, authored = 0, missing = 0;
    void build(EchoRegionCtx& ctx, const BuildingPalette& p) {
        w.resize(p.n); d.resize(p.n); wt.resize(p.n);
        for (int i = 0; i < p.n; ++i) {
            w[i] = p.a[i].footW; d[i] = p.a[i].footD; wt[i] = p.a[i].weight;
            EnvArtSystem probe;
            const float S[16] = { p.scale,0,0,0, 0,p.scale,0,0, 0,0,p.scale,0, 0,0,0,1 };
            if (!probe.buildFromGlbAt(ctx.device, p.dir, p.a[i].glb, S)) {
                // Absent asset: never drawn from — UNLESS the proxy blockout is
                // on, whose entire job is to stand in for exactly this.
                if (!cityProxyOn()) wt[i] = 0.0f;
                ++missing; ++authored;
                continue;
            }
            float mn[3], mx[3]; probe.worldBounds(mn, mx);
            const float ew = mx[0] - mn[0], ed = mx[2] - mn[2];
            if (ew > 1.0f && ed > 1.0f) { w[i] = ew; d[i] = ed; ++measured; }
            else ++authored;
        }
    }
    // THE LOOKUP: the largest-weighted asset that fits, drawn by seeded weight
    // from those that do. Returns -1 when nothing in the palette fits the lot.
    int pick(uint32_t seed, float availW, float availD) const {
        std::vector<float> ok(wt.size(), 0.0f);
        bool any = false;
        for (size_t i = 0; i < wt.size(); ++i)
            if (wt[i] > 0.0f && w[i] <= availW && d[i] <= availD) { ok[i] = wt[i]; any = true; }
        if (!any) return -1;
        return x3::game::seedWeighted(seed, ok.data(), (int)ok.size());
    }
};

// Place ONE building. Every placement in this file goes through here, so the
// footprint-corner terrain seat (and the plinth under an overhang) is not
// something a call site can forget.
// `roadY`, when given, is the finished surface of the street this building
// ADDRESSES. A building that sits far off its own street is not fronting it:
// the frontage walk offsets ~10-15 m sideways from the centreline, which on the
// crown rim steps clean off a near-vertical 190 m wall, so points meant for a
// clifftop street landed part-way down the face. See kMaxStreetOffset.
bool placeSeated(EchoRegion& region, EchoRegionCtx& ctx, const BuildingPalette& p,
                 const FootprintTable& ft, int idx,
                 float cx, float cz, float yaw, EnvArtSystem* pack,
                 const float* roadY = nullptr) {
    const float halfX = ft.w[idx] * 0.5f, halfZ = ft.d[idx] * 0.5f;
    const x3::game::FootprintSeat seat =
        x3::game::seatFootprint(ctx.hf, cx, cz, yaw, halfX, halfZ);
    // CLIFF-EDGE REJECT. A footprint straddling the crown rim reads ~190 m of
    // relief; seating it at MAX and plinthing the drop built a tower-wide
    // pedestal all the way down the sea wall. Such a lot is not a building
    // site — decline it and let the caller pick another asset or skip.
    if (seat.ok && !seat.buildable) {
        static int rejects = 0;
        if (++rejects <= 12)
            x3::logInfo("[region] lot rejected — cliff grade " +
                        std::to_string(seat.grade) + " (" +
                        std::to_string(seat.spread) + " m across footprint) at (" +
                        std::to_string(cx) + ", " + std::to_string(cz) + ")" +
                        (rejects == 12 ? " [further rejects not logged]" : ""));
        return false;
    }
    const float gy = seat.ok ? seat.y : 190.0f;
    // OFF-ITS-OWN-STREET REJECT. The grade test above rejects a footprint that
    // STRADDLES the wall; this rejects one that found a flat ledge part-way
    // DOWN it, which no local probe can distinguish from honest ground.
    if (seat.ok && roadY && std::fabs(gy - *roadY) > kMaxStreetOffset) {
        static int offRoad = 0;
        if (++offRoad <= 12)
            x3::logInfo("[region] placement rejected — seats " +
                        std::to_string(gy - *roadY) + " m off its own street (y=" +
                        std::to_string(gy) + ", road=" + std::to_string(*roadY) + ") at (" +
                        std::to_string(cx) + ", " + std::to_string(cz) + ")" +
                        (offRoad == 12 ? " [further rejects not logged]" : ""));
        return false;
    }
    const float s = p.scale, c = std::cos(yaw), sn = std::sin(yaw);
    const float T[16] = { c*s, 0, -sn*s, 0,  0, s, 0, 0,  sn*s, 0, c*s, 0,
                          cx, gy + p.a[idx].lift, cz, 1 };
    bool placed = false;
    if (pack) placed = pack->addGlbInstance(p.a[idx].glb, T);
    else {
        auto e = std::make_unique<EnvArtSystem>();
        if (e->buildFromGlbAt(ctx.device, p.dir, p.a[idx].glb, T)) {
            region.addArt(std::move(e)); placed = true;
        }
    }
    if (ctx.roads) {
        // PLINTH: the footprint-corner probe measured how far the ground drops
        // away under this building. Seating at MAX stops it sinking; the
        // plinth stops it floating. One heightAt() at the pivot could not tell
        // you either way — that is the bug this replaces.
        // Bounded: past kMaxPlinthDrop this stops being a pedestal under an
        // overhang and becomes a wall down a hillside. The grade reject above
        // catches the cliff case; this bounds what survives it.
        if (seat.ok && seat.plinth) {
            const float base = std::max(seat.yMin - 0.4f, gy - x3::game::kMaxPlinthDrop);
            ctx.roads->addPlinth(cx, cz, yaw, halfX * 1.04f, halfZ * 1.04f,
                                 base, gy + 0.10f);
        }
        // ECHO_CITY_PROXY=1: a blockout mass at the exact footprint/yaw, for
        // photographing the LAYOUT on checkouts where the building GLBs are
        // not present. Off by default; emits nothing otherwise.
        if (cityProxyOn() && !placed) {
            ctx.roads->addMassingBox(cx, cz, yaw, halfX, halfZ, gy - 0.5f,
                                     gy + p.a[idx].height);
            placed = true;
        }
    }
    return placed;
}

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
    // V8 PAD SEAT: same MAX-over-the-pad rule (a dense 20 m scan beats the
    // 5-point footprint probe on a 300 m slab), but the MINIMUM is tracked too
    // — the pad seated at MAX over sloping ground is exactly what made the
    // district "float as a mesa with a hard asphalt edge". The drop is now
    // filled with a plinth skirt instead of being left as an air gap.
    float gy = ctx.hf.ok() ? ctx.hf.heightAt(ccx, ccz) : 190.0f;
    float gyMin = gy;
    if (ctx.hf.ok()) {
        for (float ox = -cw*0.5f; ox <= cw*0.5f; ox += 20.0f)
            for (float oz = -cl*0.5f; oz <= cl*0.5f; oz += 20.0f) {
                const float s = ctx.hf.heightAt(ccx + ox, ccz + oz);
                gy = std::max(gy, s); gyMin = std::min(gyMin, s);
            }
        gy += 1.0f;   // safety: heightfield bumps narrower than the stride
    }
    placeDeck(region, ctx, "road_asphalt.glb", ccx, ccz, gy - 0.05f, 0.0f, cw, cl);
    if (ctx.roads && gy - gyMin > 0.30f) {
        ctx.roads->addPlinth(ccx, ccz, 0.0f, cw * 0.5f, cl * 0.5f, gyMin - 0.5f, gy - 0.04f);
        x3::logInfo(std::string("[region] DISTRICT ") + tag + " — pad plinth " +
                    std::to_string((int)(gy - gyMin)) + " m (terrain drops away under the slab)");
    }
    const float S = padScale, pc = std::cos(padYaw) * S, ps = std::sin(padYaw) * S;
    const float P[9] = { pc, 0.0f, -ps,   0.0f, S, 0.0f,   ps, 0.0f, pc };
    int placed = 0, cams = 0;
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
        // V8: the #34a corridor VETO that used to stand here is GONE. It was
        // one of four sites that deleted whatever a geometry-blind placement
        // dropped onto a road. This pack is a REPLAYED UNITY LAYOUT — its
        // piece positions are authored data, not a guess, and the pad it sits
        // on is chosen clear of the network; there is nothing left to veto.
        const float T[16] = { W[0],W[1],W[2],0, W[3],W[4],W[5],0, W[6],W[7],W[8],0, tx,ty,tz,1 };
        if (d->addGlbInstance(name, T)) {
            ++placed;
            // WD2 CAMERA SATURATION: every ~40th placed piece hosts a street/
            // wall camera at head-of-wall height ("cameras IN and outside of
            // most buildings, just like Watch Dogs 2").
            if (ctx.hax && (placed % 40) == 0) {
                HackableObject cam;
                cam.type = HackableType::Camera;
                cam.pos = { tx, ty + 3.6f, tz };
                cam.label = std::string(tag) + " CAM " + std::to_string(++cams);
                ctx.hax->add(cam);
            }
        }
        // (Light harvesting is NOT done here — see this function's doc
        // comment: harvestDistrictLights(), WP-3's echo_woodlands.*, does the
        // SAME per-piece name classification in its own pass over this file.)
    }
    float mn[3], mx[3]; d->worldBounds(mn, mx);
    if (cams > 0)
        x3::logInfo(std::string("[region] DISTRICT ") + tag + " — " +
                    std::to_string(cams) + " WD2 cameras registered");
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
    // ============ REAL BUILDINGS — LOT-DRIVEN (V8, was: polar hash rings) ====
    // The rings are gone. Houses come from the road graph now: first every LOT
    // the block/lot generator found (Tier 1), then a FRONTAGE WALK down the
    // streets that bound no closed block (Tier 0) — which is most of the crown,
    // where the avenues are radial spokes rather than a grid. Both paths give a
    // yaw that faces the street with ZERO jitter, a footprint that is known to
    // fit before anything loads, a min-spacing test (the rings had none), and a
    // four-corner terrain seat. Nothing is vetoed afterwards because nothing is
    // ever placed on a road.
    {
        const std::string hdir = ctx.houseForgeDir.empty() ?
            "D:/Assets/_glb/prefab_buildings/HouseForge" : ctx.houseForgeDir;
        const BuildingPalette pal{ hdir.c_str(), kCrownHouseAssets,
                                   (int)(sizeof(kCrownHouseAssets)/sizeof(kCrownHouseAssets[0])),
                                   0.01f };
        FootprintTable ft; ft.build(ctx, pal);

        // ONE EnvArtSystem for the whole neighbourhood: the district loader's
        // instancing path. The old code built one system PER HOUSE.
        auto pack = std::make_unique<EnvArtSystem>();
        EnvArtSystem* packPtr = pack->beginFromDir(ctx.device, hdir) ? pack.get() : nullptr;

        Occupancy occ;
        // The crown's five hardcoded straight "crown lanes" (CITY
        // INFRASTRUCTURE, below) are a second road system EchoRoads knows
        // nothing about. Reserve their footprints so placement avoids them the
        // same way it avoids other buildings — one mechanism, not a fifth veto.
        {
            const struct { float sx,sz,dx,dz,len; } rlanes[] = {
                {-330,702,1,0,620},{290,742,-1,0,620},{-330,818,1,0,620},
                {2,560,0,1,400},{-150,960,0,-1,400},
            };
            for (const auto& L : rlanes)
                occ.takeAxis(L.sx + L.dx*L.len*0.5f, L.sz + L.dz*L.len*0.5f,
                             L.dx, L.dz, L.len*0.5f, 9.5f);   // 15 m road + verge
        }

        // ---- ECHO_CITY_LEGACY=1: the pre-V8 polar hash rings, verbatim ----
        if (cityLegacyOn()) {
            static const int kCat[5] = { 0, 1, 2, 3, 4 };
            const float ringR[4] = { 135.0f, 215.0f, 300.0f, 395.0f };
            int built = 0, vetoed = 0, neigh = 0;
            auto addLegacy = [&](int a, float x, float z, float yaw) {
                if (legacyCorridorHit(ctx.roads, x, z, 12.0f)) { ++vetoed; return; }
                // ONE heightAt() probe at the pivot — the old ground contact.
                const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
                const float s = 0.01f, c = std::cos(yaw), sn = std::sin(yaw);
                const float T[16] = { c*s,0,-sn*s,0, 0,s,0,0, sn*s,0,c*s,0,
                                      x, gy + kCrownHouseAssets[a].lift, z, 1 };
                bool ok = packPtr ? packPtr->addGlbInstance(kCrownHouseAssets[a].glb, T) : false;
                if (!ok && cityProxyOn() && ctx.roads) {
                    ctx.roads->addMassingBox(x, z, yaw, ft.w[a] * 0.5f, ft.d[a] * 0.5f,
                                             gy - 0.5f, gy + kCrownHouseAssets[a].height);
                    ok = true;
                }
                if (ok) ++built;
            };
            addLegacy(0,  60.0f, 700.0f, 0.4f); addLegacy(1, 125.0f, 745.0f, 2.1f);
            addLegacy(2,  10.0f, 675.0f, 3.6f); addLegacy(3, 150.0f, 690.0f, 5.0f);
            addLegacy(4,  85.0f, 640.0f, 1.2f);
            for (int r = 0; r < 4; ++r) {
                const int cnt = 7 + r * 3;
                for (int k = 0; k < cnt; ++k) {
                    const uint32_t seed = (uint32_t)(r * 101 + k);   // INDEX-derived
                    const float ang = ((float)k + hh(seed) * 0.7f) * (6.2831853f / cnt);
                    const float rr  = ringR[r] + (hh(seed * 7u + 3u) - 0.5f) * 46.0f;
                    const float x = -20.0f + std::cos(ang) * rr;
                    const float z = 760.0f + std::sin(ang) * rr;
                    const float gy = ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f;
                    if (gy < 34.0f) continue;
                    const int a = kCat[(uint32_t)(r + k * 2) % 5u];   // periodic ABCDE
                    const float yaw = ang + 1.5708f + (hh(seed * 13u + 5u) - 0.5f) * 1.2f;
                    addLegacy(a, x, z, yaw);
                    ++neigh;
                }
            }
            x3::logWarn("[region] ECHO_CITY_LEGACY=1 — PRE-V8 PLACEMENT: " +
                        std::to_string(built) + " houses on 4 polar hash rings (" +
                        std::to_string(neigh) + " ring candidates, " +
                        std::to_string(vetoed) + " deleted by the corridor audit)");
            if (packPtr) region.addArt(std::move(pack));
        } else {   // ---- V8: the structural path ----

        int fromLots = 0, fromFrontage = 0, noFit = 0, spaced = 0, wet = 0, cams = 0;
        auto camAt = [&](float x, float z, float gy) {
            if (!ctx.hax) return;
            ++cams;
            if ((cams % 4) != 0) return;
            HackableObject cam;
            cam.type = HackableType::Camera;
            cam.pos = { x, gy + 3.2f, z };
            cam.label = "NEIGHBORHOOD CAM " + std::to_string(cams / 4);
            ctx.hax->add(cam);
        };
        auto landOk = [&](float x, float z) {
            return !ctx.hf.ok() || ctx.hf.heightAt(x, z) >= echoLandSafe();   // sea + 2.50 m
        };

        // ---- TIER 1: one building per LOT --------------------------------
        if (ctx.roads) {
            x3::game::CityPlanRules rules;
            const x3::game::CityPlan& plan = ctx.roads->cityPlan(rules, x3::game::kRcGround);
            for (const x3::game::CityLot& lot : plan.lots) {
                // Front the building on its street: push in from the fronting
                // edge by half the depth, so the block gets a street WALL
                // rather than a row of centred islands.
                const uint32_t seed = x3::game::seedAt(lot.cx, lot.cz);
                const int idx = ft.pick(seed, lot.halfW * 2.0f, lot.halfD * 2.0f);
                if (idx < 0) { ++noFit; continue; }
                const float halfD = ft.d[idx] * 0.5f, halfW = ft.w[idx] * 0.5f;
                float px = lot.frontX + lot.nx * halfD;
                float pz = lot.frontZ + lot.nz * halfD;
                // Keep the footprint inside the lot's guaranteed-inside OBB.
                const float du = (px - lot.cx) * lot.ax + (pz - lot.cz) * lot.az;
                const float dv = (px - lot.cx) * lot.nx + (pz - lot.cz) * lot.nz;
                const float cu = std::max(-(lot.halfW - halfW), std::min(lot.halfW - halfW, du));
                const float cv = std::max(-(lot.halfD - halfD), std::min(lot.halfD - halfD, dv));
                px = lot.cx + lot.ax * cu + lot.nx * cv;
                pz = lot.cz + lot.az * cu + lot.nz * cv;
                if (!landOk(px, pz)) { ++wet; continue; }
                if (!occ.free(px, pz, lot.frontYaw, halfW, halfD)) { ++spaced; continue; }
                if (!placeSeated(region, ctx, pal, ft, idx, px, pz, lot.frontYaw, packPtr))
                    continue;
                occ.take(px, pz, lot.frontYaw, halfW, halfD);
                ++fromLots;
                camAt(px, pz, ctx.hf.ok() ? ctx.hf.heightAt(px, pz) : 190.0f);
            }

            // ---- TIER 0: FRONTAGE WALK for the streets that bound no block -
            // This is the direct replacement for the four polar rings: instead
            // of guessing an angle around a point, walk the real centreline and
            // set the house down where a house goes.
            std::vector<x3::game::Frontage> fr;
            ctx.roads->sampleFrontage(x3::game::kRcGround, 26.0f, 3.5f, fr);
            for (const x3::game::Frontage& f : fr) {
                const uint32_t seed = x3::game::seedAt(f.x, f.z);
                // Deterministic thinning FROM POSITION — a gappy street reads
                // as a neighbourhood; a solid wall of prefabs does not.
                if (x3::game::seedFloat(x3::game::seedMix(seed, 11u)) > 0.62f) continue;
                const int idx = ft.pick(seed, 18.0f, 16.0f);
                if (idx < 0) { ++noFit; continue; }
                const float halfW = ft.w[idx] * 0.5f, halfD = ft.d[idx] * 0.5f;
                // The frontage point is the front FACE; the pivot sits halfD in.
                const float px = f.x + f.nx * halfD, pz = f.z + f.nz * halfD;
                if (!landOk(px, pz)) { ++wet; continue; }
                if (!occ.free(px, pz, f.yaw, halfW + 1.2f, halfD + 1.2f)) { ++spaced; continue; }
                if (!placeSeated(region, ctx, pal, ft, idx, px, pz, f.yaw, packPtr, &f.roadY))
                    continue;
                occ.take(px, pz, f.yaw, halfW + 1.2f, halfD + 1.2f);
                ++fromFrontage;
                camAt(px, pz, ctx.hf.ok() ? ctx.hf.heightAt(px, pz) : 190.0f);
            }
            x3::logInfo("[region] REAL BUILDINGS — " + std::to_string(fromLots) +
                        " on lots + " + std::to_string(fromFrontage) +
                        " on street frontage (" + std::to_string(noFit) +
                        " lots no palette entry fit, " + std::to_string(spaced) +
                        " min-spacing, " + std::to_string(wet) + " off dry land); " +
                        std::to_string(ft.measured) + "/" +
                        std::to_string(ft.measured + ft.authored) +
                        " footprints measured at boot, rest authored");
        } else {
            x3::logWarn("[region] REAL BUILDINGS — no road graph: the crown "
                        "neighbourhood is road-derived and has nothing to sit on");
        }

        // Five HERO houses, hand-placed on the crown foothills since Phase B.
        // The positions are authored and kept; what changes is that the yaw is
        // now taken from the nearest street's tangent instead of an authored
        // number, and the seat is a four-corner probe.
        {
            static const struct { int asset; float x, z, fallbackYaw; } kHero[] = {
                { 0,  60.0f, 700.0f, 0.4f }, { 1, 125.0f, 745.0f, 2.1f },
                { 2,  10.0f, 675.0f, 3.6f }, { 3, 150.0f, 690.0f, 5.0f },
                { 4,  85.0f, 640.0f, 1.2f },
            };
            std::vector<x3::game::Frontage> fr;
            if (ctx.roads) ctx.roads->sampleFrontage(x3::game::kRcGround, 8.0f, 3.5f, fr);
            int heroes = 0;
            for (const auto& h : kHero) {
                float yaw = h.fallbackYaw, best = 60.0f * 60.0f;
                for (const x3::game::Frontage& f : fr) {
                    const float dx = f.x - h.x, dz = f.z - h.z;
                    const float d = dx*dx + dz*dz;
                    if (d < best) { best = d; yaw = f.yaw; }
                }
                const float halfW = ft.w[h.asset] * 0.5f, halfD = ft.d[h.asset] * 0.5f;
                if (!occ.free(h.x, h.z, yaw, halfW, halfD)) continue;
                if (!placeSeated(region, ctx, pal, ft, h.asset, h.x, h.z, yaw, packPtr))
                    continue;
                occ.take(h.x, h.z, yaw, halfW, halfD);
                ++heroes;
            }
            x3::logInfo("[region] HERO HOUSES — " + std::to_string(heroes) +
                        "/5 seated (yaw from the nearest street tangent)");
        }
        if (packPtr) region.addArt(std::move(pack));
        }   // end of the V8 branch (see ECHO_CITY_LEGACY above)
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
        int towersBuilt = 0, reseated = 0, onRoad = 0;
        for (const char* b : kBld) {
            auto t = std::make_unique<EnvArtSystem>();
            if (!t->buildFromGlbAt(ctx.device, cdir, std::string(b) + ".glb", M)) continue;
            float mn[3], mx[3]; t->worldBounds(mn, mx);
            // V8 removed the corridorHitsAABB VETO from this site, on the V8
            // rationale that "a building now comes from a LOT or a FRONTAGE
            // POINT, both outside every road corridor by construction". THESE 36
            // TOWERS COME FROM NEITHER. They are one BAKED Unity layout dropped
            // through a single scene transform `M`; nothing in that layout has
            // ever heard of the road graph, so removing the veto removed the
            // only thing keeping them off it — Tim's "buildings are still ON the
            // freeway". (Re-seating each tower in Y, which the V8 note is really
            // about, is a different axis and is kept below: it fixes float/bury,
            // not footprint overlap.) The veto is BACK, and it tests the FULL
            // road set — Freeway and Ramp included, which the lot system never
            // sees because buildCityPlan is called with kRcGround.
            {
                const float bcx = (mn[0] + mx[0]) * 0.5f, bcz = (mn[2] + mx[2]) * 0.5f;
                const float hx = (mx[0] - mn[0]) * 0.5f, hz = (mx[2] - mn[2]) * 0.5f;
                bool hit = false;
                for (int cx = -1; cx <= 1 && !hit; ++cx)
                    for (int cz = -1; cz <= 1 && !hit; ++cz)
                        hit = legacyCorridorHit(ctx.roads, bcx + hx * (float)cx,
                                                bcz + hz * (float)cz, 0.0f);
                if (hit) { ++onRoad; continue; }
            }
            if (ctx.hf.ok() && mx[0] >= mn[0]) {
                const float bcx = (mn[0] + mx[0]) * 0.5f, bcz = (mn[2] + mx[2]) * 0.5f;
                const float hx = std::max(4.0f, (mx[0] - mn[0]) * 0.5f);
                const float hz = std::max(4.0f, (mx[2] - mn[2]) * 0.5f);
                const x3::game::FootprintSeat seat =
                    x3::game::seatFootprint(ctx.hf, bcx, bcz, 0.0f, hx, hz);
                const float dy = seat.y - mn[1];
                if (std::fabs(dy) > 0.05f) {
                    const float M2[16] = { ts,0,0,0, 0,ts,0,0, 0,0,ts,0,
                                           Tx, gy + tlift + dy, Tz, 1 };
                    t->setInstanceTransform(0, M2);
                    t->worldBounds(mn, mx);
                    ++reseated;
                }
                if (ctx.roads && seat.ok && seat.plinth)
                    ctx.roads->addPlinth(bcx, bcz, 0.0f, hx, hz,
                                         seat.yMin - 0.6f, seat.y + 0.1f);
            }
            // WD2 CAMERA SATURATION: one high camera per skyline tower.
            if (ctx.hax) {
                HackableObject cam;
                cam.type = HackableType::Camera;
                cam.pos = { (mn[0] + mx[0]) * 0.5f,
                            mn[1] + (mx[1] - mn[1]) * 0.75f,
                            (mn[2] + mx[2]) * 0.5f };
                cam.label = std::string(b) + " CAM";
                ctx.hax->add(cam);
            }
            region.addArt(std::move(t)); ++towersBuilt;
        }
        x3::logInfo("[region] DOWNTOWN SKYLINE — " +
                    std::to_string(towersBuilt) + " Urban Night City towers on the crown (" +
                    std::to_string(reseated) + " re-seated on their own terrain; " +
                    std::to_string(onRoad) + " REFUSED — footprint in a road/freeway corridor)");
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
        // STREETS: the five crown car lanes used to be placed HERE as flat
        // 620x15 m road_asphalt/road_curbs slabs at the single probe height
        // `kCarY + 0.06`. One height for 620 m of rolling mesa left 78-95% of
        // each lane hanging over the grass (190 m of it off the cliff) and gave
        // their six mutual crossings no junction geometry whatsoever — the
        // "streets are bare strips on grass / intersections just cross" report.
        // The SAME five centrelines are now seeded into EchoRoads (see
        // echo_roads.cpp "1d-bis. THE CROWN GRID"), which drapes them on the
        // terrain, curbs and paints them, and builds a real junction patch at
        // every crossing. The car lanes are unchanged in plan; only who owns
        // them moved, so there is one road system on the crown instead of two.
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

    // ===================== LIVING CONDOS — THE SHELLS (interiors pillar) ====
    // Tim's #1 sweep slop (2026-07-29): the lit rooms rendered SHELL-LESS —
    // floating window grids in daylight. The ROOMS moved to the gated
    // `int_condo_rooms` sub-region (echo_interiors.cpp — bit-identical
    // loops); buildCrown now owns only the EXTERIOR: one textured tower body
    // per stack (TheHotel_Model, bbox-verified 21.2x23.0x25.7m base-centered,
    // non-uniformly scaled to wrap the 22.5m-wide room grid) + a ctOS
    // "entry kiosk" beside each ground-floor door.
    {
        const char* kMega = "D:/Assets/_glb/prefab_buildings/Mega Open World City Pack/Assets/Mega City Environment/Models";
        int shells = 0;
        auto shell = [&](float wx, float wz, int floors){
            const float gy = ctx.hf.ok()?ctx.hf.heightAt(wx,wz):190.0f;
            const float sx = 24.0f / 21.237f;                       // wrap 22.5m grid + margin
            const float sy = (floors * 6.5f + 2.5f) / 22.998f;      // grid height + parapet
            const float sz = 12.0f / 25.665f;                       // rooms run wz..wz+5.7
            const float T[16] = { sx,0,0,0, 0,sy,0,0, 0,0,sz,0,
                                  wx, gy, wz + 2.9f, 1 };
            auto e = std::make_unique<EnvArtSystem>();
            if (e->buildFromGlbAt(ctx.device, kMega, "TheHotel_Model.glb", T)) {
                region.addArt(std::move(e)); ++shells;
            }
            // Entry kiosk (the door marker; the walk-in lobby lives in the
            // int_condo_rooms sub-region — see echo_interiors.cpp).
            const float K[16] = { -0.842f,0,0,0, 0,0.842f,0,0, 0,0,-0.842f,0,
                                  wx - 4.5f, gy, wz + 6.5f, 1 };
            auto k = std::make_unique<EnvArtSystem>();
            if (k->buildFromGlbAt(ctx.device,
                    "D:/GameDev/EchoHarbor/assets/meshy/props", "ctos_terminal.glb", K)) {
                region.addArt(std::move(k)); ++shells;
            }
        };
        shell(-100.0f, 842.0f, 5);
        shell( -20.0f, 842.0f, 6);   // the stack with the hidden lab
        shell(  60.0f, 842.0f, 5);
        x3::logInfo("[region] LIVING CONDOS — " + std::to_string(shells) +
                    " shell/kiosk pieces (rooms -> int_condo_rooms sub-region)");
    }

    // VENDOR DRESSING (always-visible street furniture: Tess's stall + sign,
    // Fixer's table, Preacher's torches) — echo_interiors.cpp owns the set.
    buildVendorDressing(region, ctx);

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
    // M-A DEVIATION (WP-0): street lamps + lampScene STAY HOST-SIDE for
    // milestone A — the per-frame streetLamps.selectLights() nearest-K query
    // feeding device->setPointLights has no home in the static addLights()
    // contract (the INTEGRATOR note above), and building a SECOND lamp set
    // here would draw the lamps twice. The host keeps its streetLamps/
    // lampScene/selectLights machinery untouched. Revisit at M-B alongside
    // the light-selection redesign.
    Scene*        lampScenePtr   = nullptr;   // kept null — see M-A deviation
    StreetLights* streetLampsPtr = nullptr;
#if 0   // M-A: lamps stay host-side — see deviation note
    lampScenePtr = new Scene();
    streetLampsPtr = new StreetLights();
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
#endif

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
    // M-A DEVIATION (WP-0): the beam block above the INTEGRATOR note is NOT
    // compiled — the beam + its cityLightsOn night-gate STAY HOST-SIDE
    // (host_echotropolis.cpp keeps beam/poseBeam/fissure + the gated draws)
    // precisely because this port cannot reproduce the gate and would break
    // the milestone-A byte-compare (golden TOD draws NO beam in the host).
    // Revisit at M-B by threading a per-frame cityLightsOn into the region
    // update/draw path.
#if 0   // M-A: beam stays host-side — see deviation note
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
#endif

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
// #34b WATERFRONT ROW (Tim: "sleek night city glass, placed along the water
// in a streamlined fashion"): real Urban Night City glass towers in an evenly
// spaced promenade row on the TRUE terrain waterline — spots traced offline
// from the live height PNG (~55 m spacing, 26 m inland, yaw facing the
// water). Each tower's baked layout offset is cancelled after load (origin
// AABB) so the GLB seats exactly on its spot. Corridor-audited (#34a).
void buildWaterfrontRow(EchoRegion& region, EchoRegionCtx& ctx) {
    static const struct { float x, z, yaw; } kRow[] = {
        {  849.2f, -240.1f,  2.182f }, {  870.4f, -189.1f,  2.251f },
        {  926.8f, -114.7f,  2.391f }, {  990.6f,  -48.6f,  2.531f },
        { 1030.9f,   34.0f,  2.670f }, { 1017.3f,  134.7f,  2.810f },
        {  967.7f,  191.3f,  2.880f }, {  929.0f,  236.9f,  2.950f },
        {  901.4f,  306.3f,  3.089f }, {  896.3f,  369.0f, -3.054f },
        {  882.6f,  429.9f, -2.915f }, {  868.2f,  490.5f, -2.775f },
        {  841.8f,  547.2f, -2.635f }, {  791.8f,  587.6f, -2.496f },
        {  721.5f,  601.5f, -2.356f }, {  642.6f,  585.6f, -2.217f },
        {  570.2f,  546.9f, -2.077f }, {  509.6f,  525.1f, -1.868f },
        {  453.3f,  518.0f, -1.588f }, {  403.4f,  569.5f, -1.379f },
    };
    const std::string cdir =
        "D:/Assets/_glb/tech/Urban Night City - Open World/Assets/GeeZyyGames/buildings/FBX";
    const BuildingPalette pal{ cdir.c_str(), kGlassTowerAssets,
                               (int)(sizeof(kGlassTowerAssets)/sizeof(kGlassTowerAssets[0])),
                               0.34f };
    FootprintTable ft; ft.build(ctx, pal);

    // V8: the promenade is now a FRONTAGE WALK down the Harbor Boulevard's
    // SEAWARD side instead of 20 hand-traced (x,z,yaw) triples whose yaws were
    // eyeballed off a height PNG and 30% of which the corridor audit deleted.
    // The boulevard is already probed onto the true waterline by the road
    // module, so walking it puts the towers exactly where the literals were
    // trying to be — but square to the street, evenly spaced, and never on it.
    // The literals remain as the no-road-graph fallback.
    // hasRoad is false for the authored-literal fallback below, which has no
    // parent street to measure against — those keep their original behaviour.
    struct Spot { float x, z, yaw, roadY; bool hasRoad; };
    std::vector<Spot> spots;
    bool fromRoads = false;
    if (ctx.roads && !cityLegacyOn()) {   // fallback cvar: the authored literals
        std::vector<x3::game::Frontage> fr;
        ctx.roads->sampleFrontage(x3::game::kRcAvenue, 55.0f, 12.0f, fr);
        for (const x3::game::Frontage& f : fr) {
            // Seaward side only: the frontage point must be closer to water
            // than the centreline is. (heightAt < land threshold outboard.)
            if (!ctx.hf.ok()) continue;
            const float probe = ctx.hf.heightAt(f.x + f.nx * 30.0f, f.z + f.nz * 30.0f);
            const float here  = ctx.hf.heightAt(f.x, f.z);
            if (here < echoLandSafe()) continue;       // the pad itself is wet
            if (probe > echoLandSafe()) continue;      // inland side — skip
            spots.push_back({ f.x, f.z, f.yaw, f.roadY, true });
        }
        fromRoads = !spots.empty();
    }
    if (!fromRoads)
        for (const auto& r : kRow) spots.push_back({ r.x, r.z, r.yaw, 0.0f, false });

    Occupancy occ;
    int placed = 0, spacedOut = 0, noFit = 0;
    for (const Spot& s : spots) {
        // ECHO_CITY_LEGACY: the deleted #34a veto, reproduced so the A/B is fair.
        if (cityLegacyOn() && legacyCorridorHit(ctx.roads, s.x, s.z, 10.0f)) { ++spacedOut; continue; }
        const uint32_t seed = x3::game::seedAt(s.x, s.z);
        const int idx = ft.pick(seed, 40.0f, 40.0f);
        if (idx < 0) { ++noFit; continue; }
        const float halfW = ft.w[idx] * 0.5f, halfD = ft.d[idx] * 0.5f;
        // Pivot sits halfD inland of the frontage face, facing the street.
        const float fx = std::sin(s.yaw), fz = std::cos(s.yaw);
        const float px = s.x - fx * halfD, pz = s.z - fz * halfD;
        if (!occ.free(px, pz, s.yaw, halfW + 3.0f, halfD + 3.0f)) { ++spacedOut; continue; }
        if (!placeSeated(region, ctx, pal, ft, idx, px, pz, s.yaw, nullptr,
                         s.hasRoad ? &s.roadY : nullptr)) continue;
        occ.take(px, pz, s.yaw, halfW + 3.0f, halfD + 3.0f);
        ++placed;
        // WD2 CAMERA SATURATION: a promenade cam on every glass tower.
        if (ctx.hax) {
            HackableObject cam;
            cam.type = HackableType::Camera;
            cam.pos = { px, (ctx.hf.ok() ? ctx.hf.heightAt(px, pz) : 0.0f) + 9.0f, pz };
            cam.label = "PROMENADE CAM " + std::to_string(placed);
            ctx.hax->add(cam);
        }
    }
    x3::logInfo("[region] WATERFRONT ROW — " + std::to_string(placed) +
                " glass towers, " + (fromRoads ? "frontage-walked down the boulevard"
                                               : "from the authored fallback row") +
                " (" + std::to_string(spacedOut) + " min-spacing, " +
                std::to_string(noFit) + " no fit); zero corridor vetoes — "
                "the promenade cannot land on the road it is walking");
}

void buildDistrictUrban(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "URBAN DISTRICT");
    buildWaterfrontRow(region, ctx);   // #34b: the sleek glass promenade
}
void buildDistrictRecife(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "RECIFE 2050");
}
void buildDistrictHivemind(EchoRegion& region, EchoRegionCtx& ctx) {
    buildOneDistrict(region, ctx, "HIVEMIND CYBERCITY");
}

// ===========================================================================
// COASTLINE GATE (Tim 2026-08-12: "Ships still sail in to the city and cliff")
//
// WHY THIS EXISTS. The three harbour lanes below are AUTHORED straight segments.
// They were authored against a bake of the island that no longer exists: the
// shipped default is `assets/island_mesa` (the 20260728 mesa-rim rebake, see
// host_echotropolis.cpp's ECHO_ISLAND_DIR block), and on THAT heightfield the
// south-bay eastbound lane climbs a 130 m headland — the hero tall ship sails
// straight through the cliff and the cliffside village. Sampling the shipped PNG
// along the authored polylines: lane A is above -1.5 m for 68 of 153 samples
// (peak +130.0 m), lane C for 33 of 85 (peak +5.0 m), lane B for 12 of 153.
//
// WHY THE EARLIER FLOOD-FILL AUDIT PASSED ANYWAY. That audit answered a
// different question: it flood-filled the WET CELLS from open ocean and reported
// that 99.7% of them are ocean-connected and that "all three lanes" are
// reachable. Connectivity of the water body says nothing about whether the
// AUTHORED POLYLINE between two reachable endpoints stays in the water — a
// straight line between two connected harbour cells is free to cross a
// peninsula, and here it does. Reachability was never the invariant; "every
// point of the swept path is navigable" is.
//
// WHAT THIS DOES. Before a lane is used it is re-validated against the ACTIVE
// heightfield (ctx.hf — the same PNG the rendered GLB was meshed from): sample
// the centreline plus +/- a beam-clearance corridor every kProbeStep metres,
// keep the LONGEST CONTIGUOUS RUN whose terrain is deeper than the keel draft
// everywhere across the corridor, and sail only that. A lane whose surviving run
// is shorter than kMinLaneLen is DROPPED and logged rather than beached. Same
// doctrine the fjord coastline-audit note asks for: authored placements
// re-validate vs the active hf, then relocate or skip+log.
//
// This makes the fleet self-healing across re-bakes: change the island and the
// lanes clip themselves instead of sailing into the new landform.
// ===========================================================================
namespace {

struct LaneClip { float startD = 0.0f, len = 0.0f; bool ok = false; };

// Keel draft: the hull rides echoBoatY() (= sea + 0.60 m freeboard) on the
// Gerstner surface, so terrain shallower than this would breach a hull.
// Deliberately generous (a grounded ship reads far worse than a lane that hugs
// deeper water). Stated as a DEPTH BELOW THE DATUM so moving the sea moves the
// navigable envelope with it — it used to be the absolute -4.0.
constexpr float kKeelDraft   = echoKeelDraft();   // = sea - 4.00 m (echo_sea.h)
constexpr float kLaneHalfW   = 35.0f;   // corridor half-width probed either side (m)
constexpr float kProbeStep   = 5.0f;    // centreline sample spacing (m)
constexpr float kMinLaneLen  = 150.0f;  // below this a lane is dropped, not sailed

// True when the full beam corridor at `d` metres along the lane is navigable.
bool laneWetAt(const Heightfield& hf, float sx, float sz, float dx, float dz, float d) {
    const float px = -dz, pz = dx;                  // unit perpendicular
    const float x = sx + dx * d, z = sz + dz * d;
    for (float o = -kLaneHalfW; o <= kLaneHalfW + 0.01f; o += kLaneHalfW * 0.5f)
        if (hf.heightAt(x + px * o, z + pz * o) > kKeelDraft) return false;
    return true;
}

// Longest contiguous navigable run along an authored lane, in lane-local metres.
LaneClip clipLaneToWater(const Heightfield& hf, float sx, float sz,
                         float dx, float dz, float len) {
    LaneClip best{};
    if (!hf.ok()) { best.startD = 0.0f; best.len = len; best.ok = true; return best; }  // no hf -> legacy behavior
    float runStart = -1.0f;
    for (float d = 0.0f; d <= len + 0.01f; d += kProbeStep) {
        if (laneWetAt(hf, sx, sz, dx, dz, d)) {
            if (runStart < 0.0f) runStart = d;
        } else if (runStart >= 0.0f) {
            const float runLen = (d - kProbeStep) - runStart;
            if (runLen > best.len) { best.startD = runStart; best.len = runLen; }
            runStart = -1.0f;
        }
    }
    if (runStart >= 0.0f && (len - runStart) > best.len) {
        best.startD = runStart; best.len = len - runStart;
    }
    // Inset one probe step at each end so a hull never straddles the last wet
    // sample into the first dry one.
    if (best.len > 2.0f * kProbeStep) {
        best.startD += kProbeStep;
        best.len    -= 2.0f * kProbeStep;
    }
    best.ok = best.len >= kMinLaneLen;
    return best;
}

} // namespace

void buildHarborBay(EchoRegion& region, EchoRegionCtx& ctx) {
    const float kBoatYaw = [](){ const char* e=std::getenv("ECHO_BOAT_YAW"); return e?(float)std::atof(e):0.0f; }();
    // Hull origin above STILL WATER. Was the absolute 0.6; now sea + freeboard,
    // so it tracks the datum. ECHO_BOAT_Y still overrides it as an absolute
    // (it is a debug lever, and an absolute is what a debugger wants).
    const float kBoatY   = [](){ const char* e=std::getenv("ECHO_BOAT_Y");
                                 return e?(float)std::atof(e):echoBoatY(); }();
    // ECHO_SHIP_FLATBOB=1 restores the pre-fix flat sin() bob — the A/B lever
    // for the "hulls sit proud of the surface" repro, from one binary.
    const bool kFlatBob  = [](){ const char* e=std::getenv("ECHO_SHIP_FLATBOB"); return e && *e=='1'; }();
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
    // halfLen/halfBeam are MEASURED off the loaded mesh (see addBoat), not
    // guessed per asset — FleetDef carries no hull dims and inventing them
    // would put the wave-sampling footprint somewhere the art is not.
    struct BoatPose { EnvArtSystem* body; float sx,sz,dx,dz,len,speed,off,scale;
                      float halfLen, halfBeam; };
    std::vector<BoatPose> boatPoses;
    // ---- UNTEXTURED-HERO REPAIR (Tim 2026-08-12: the ship "renders near-white").
    // MedievalShip_.glb is the only vessel in the fleet with no material
    // authoring: 28 of its 62 materials carry the bare glTF/Blender default
    // (baseColorFactor 0.8 grey, metallic 0, roughness 0.5) with NO
    // baseColorTexture — and their TEXCOORD_0 accessors have NO bufferView, i.e.
    // every UV is (0,0), so the mesh CANNOT be textured without a re-unwrap
    // (the pack's own thumbnail ships it grey). 0.8 linear grey under a noon sun
    // + ACES + bloom is what reads as a pale, near-translucent blob. The other
    // five hulls are fully textured and are untouched by this.
    //
    // So: give the hero real material CONSTANTS by glTF material name (see
    // EnvArtSystem::setMaterialOverride). Values are linear albedo. Note that
    // roughness/metallic can only reach the shader through an MR map, which this
    // asset has none of — so the untextured parts stay on mesh.frag's dielectric
    // path (fixed satin roughness 0.5), and colour is the whole lever. Honest
    // limitation, recorded here rather than papered over: this is flat-colour
    // PBR, not textured PBR.
    static const std::vector<EnvArtSystem::MaterialOverride> kShipMats = [](){
        auto rule = [](const char* sub, float r, float g, float b) {
            EnvArtSystem::MaterialOverride o; o.nameSub = sub;
            o.setBaseColor = true; o.baseColor[0]=r; o.baseColor[1]=g; o.baseColor[2]=b; o.baseColor[3]=1.0f;
            return o;
        };
        return std::vector<EnvArtSystem::MaterialOverride>{
            rule("m_ship_body", 0.078f, 0.046f, 0.028f),   // tarred oak hull
            rule("m_prow",      0.100f, 0.061f, 0.035f),   // bowsprit, lighter oak
            rule("m_mast",      0.115f, 0.072f, 0.041f),   // spars / yards
            rule("m_fencing",   0.070f, 0.043f, 0.025f),   // rails
            rule("m_ladders",   0.080f, 0.050f, 0.029f),
            rule("m_wheel",     0.065f, 0.038f, 0.022f),
            rule("m_sails",     0.300f, 0.262f, 0.205f),   // weathered canvas (NOT 0.8 white)
            rule("m_flag",      0.180f, 0.028f, 0.028f),   // deep red
            rule("m_barrel",    0.085f, 0.052f, 0.030f),
            rule("cannon",      0.022f, 0.021f, 0.020f),   // cast iron
        };
    }();

    auto addBoat = [&](const FleetDef& fd, float sx, float sz, float dx, float dz,
                       float len, float speed, float off){
        const float L = std::sqrt(dx*dx + dz*dz); dx/=L; dz/=L;
        auto b = std::make_unique<EnvArtSystem>();
        // SPAWN AT THE LANE OFFSET, not the lane head. The build transform used
        // to ignore `off`, so all three vessels of a lane were stacked on top of
        // one another at its start — invisible in the live game (the first pose
        // update separates them) but the ONLY thing a still ever showed, because
        // the capture path never ticks the streamer that drives those updates.
        // Spawning correctly makes a still an honest picture of the fleet.
        const float hx = sx + dx * off, hz = sz + dz * off;
        const float hh = std::atan2(dx, dz) + kBoatYaw;
        const float hc = std::cos(hh), hs = std::sin(hh), hsc = fd.scale;
        const float I[16] = { hc*hsc,0,-hs*hsc,0, 0,hsc,0,0, hs*hsc,0,hc*hsc,0, hx, kBoatY, hz, 1 };
        // ECHO_SHIP_LEGACY_MAT=1 keeps the pack's bare 0.8 grey (the A/B lever
        // for the near-white-hero repro).
        static const bool legacyMat = [](){ const char* e=std::getenv("ECHO_SHIP_LEGACY_MAT"); return e && *e=='1'; }();
        if (!legacyMat && std::string(fd.glb).rfind("MedievalShip", 0) == 0)
            b->setMaterialOverride(kShipMats);
        if (b->buildFromGlbAt(ctx.device, fd.dir, fd.glb, I)) {
            // HULL FOOTPRINT, measured. echoShipPose samples the wave surface at
            // the bow/stern/port/starboard, so it needs real half-extents. The
            // loaded AABB is in WORLD space with the heading already applied;
            // all three authored lanes are axis-aligned (dx,dz in {0,+/-1}), so
            // the AABB's X/Z extents ARE the hull's length and beam. For a
            // diagonal lane this would over-estimate slightly, which errs
            // toward a gentler tilt — the safe direction.
            float mn[3], mx[3]; b->worldBounds(mn, mx);
            const float ex = (mx[0] - mn[0]) * 0.5f, ez = (mx[2] - mn[2]) * 0.5f;
            const float halfLen  = std::max(0.5f, std::max(ex, ez));
            const float halfBeam = std::max(0.25f, std::min(ex, ez));
            boatPoses.push_back({ b.get(), sx, sz, dx, dz, len, speed*fd.speedMul, off,
                                  fd.scale, halfLen, halfBeam });
            region.addArt(std::move(b));
        }
    };
    {
        struct BL { const char* name; float sx,sz,dx,dz,len,speed; int n; };
        // RE-AUTHORED 2026-08-12 against the shipped `assets/island_mesa` bake.
        // The previous table (z=330 EB / z=240 WB / x=-560 z 260..680 NB) was
        // authored for a bake whose coastline no longer exists and ran straight
        // over the headland. These three sit in the open bay north of the island
        // and survive the coastline gate INTACT on BOTH shipped bakes (mesa and
        // fjord) — only the safety inset is trimmed. They also stay 140-250 m off
        // the cliffside village, so the fleet reads as harbour traffic passing
        // the waterfront instead of docking in it.
        const BL bl[] = {
            { "outer bay eastbound", -600.0f,  170.0f,  1.0f,  0.0f, 1240.0f, 15.0f, 3 },
            { "outer bay westbound",  640.0f,  110.0f, -1.0f,  0.0f, 1240.0f, 13.0f, 3 },
            { "west inlet northbnd", -550.0f,  -70.0f,  0.0f,  1.0f,  540.0f, 12.0f, 3 },
        };
        // A/B LEVER: ECHO_BOAT_LEGACY_LANES=1 restores the exact pre-fix table
        // AND bypasses the coastline gate, so the beached-galleon repro and its
        // fix can be captured from ONE binary at identical framing.
        const BL blLegacy[] = {
            { "south bay eastbound (LEGACY)", -400.0f, 330.0f,  1.0f, 0.0f, 760.0f, 15.0f, 3 },
            { "south bay westbound (LEGACY)",  340.0f, 240.0f, -1.0f, 0.0f, 760.0f, 13.0f, 3 },
            { "SW inlet northbound (LEGACY)", -560.0f, 260.0f,  0.0f, 1.0f, 420.0f, 12.0f, 3 },
        };
        const bool legacyLanes = [](){ const char* e = std::getenv("ECHO_BOAT_LEGACY_LANES"); return e && *e=='1'; }();
        const BL* table = legacyLanes ? blLegacy : bl;
        int vi = 0, dropped = 0;
        for (int li = 0; li < 3; ++li) {
            const BL& l = table[li];
            const float L = std::sqrt(l.dx*l.dx + l.dz*l.dz);
            const float ux = l.dx / L, uz = l.dz / L;
            LaneClip c;
            if (legacyLanes) { c.startD = 0.0f; c.len = l.len; c.ok = true; }
            else             c = clipLaneToWater(ctx.hf, l.sx, l.sz, ux, uz, l.len);
            if (!c.ok) {
                x3::logWarn(std::string("[region] LANE DROPPED (coastline gate) — ") + l.name +
                            ": only " + std::to_string((int)c.len) + " m of " +
                            std::to_string((int)l.len) + " m is navigable on the active heightfield");
                vi += l.n; ++dropped; continue;
            }
            if (c.len < l.len - 1.0f)
                x3::logInfo(std::string("[region] lane clipped to water — ") + l.name + ": " +
                            std::to_string((int)l.len) + " m -> " + std::to_string((int)c.len) +
                            " m (starts " + std::to_string((int)c.startD) + " m in)");
            const float sx = l.sx + ux * c.startD, sz = l.sz + uz * c.startD;
            for (int k = 0; k < l.n; ++k) {
                addBoat(kFleet[vi % 6], sx, sz, ux, uz, c.len, l.speed, c.len * (float)k / (float)l.n);
                ++vi;
            }
        }
        x3::logInfo("[region] HARBOR FLEET — " + std::to_string(boatPoses.size()) +
                    " vessels on " + std::to_string(3 - dropped) + "/3 coastline-gated lanes");
    }
    // ---- LIVING BAY: HULLS RIDE THE ACTUAL SURFACE ------------------------
    // echo_water.h shipped echoShipPose() — heave/pitch/roll sampled off the
    // SAME Gerstner sum shaders/water.vert draws — but nothing ever called it.
    // The hulls used a flat `kBoatY + sin(t*0.7)*0.35` bob: a single sine on a
    // fixed clock, unrelated in phase, frequency, direction OR amplitude to the
    // water under them. So a boat rose while the sea under it fell, and for
    // most of each cycle it sat visibly proud of (or sunk into) the surface.
    //
    // WHY THIS BELONGS TO THE SEA-LEVEL LANE AND NOT AN EARLIER ONE: the bob
    // was centred on 0.6 while the drawn sea was at 0.10 and the terrain said
    // 0.0. There was no single surface to ride. Now there is exactly one, and
    // `kSwellHarbor` is both what applyOcean feeds the GPU and what this
    // samples, so the hull and the water are evaluations of one function.
    // `t` is the shared water clock (host: waterTime -> both applyOcean and
    // regionSet.updateAll).
    const WaterTuning swell = kSwellHarbor;
    region.setUpdate([boatPoses, kBoatYaw, kBoatY, kFlatBob, swell](float /*dt*/, float t) {
        for (const auto& b : boatPoses) {
            if (!b.body) continue;
            const float d = std::fmod(b.off + t * b.speed, b.len);
            const float x = b.sx + b.dx * d, z = b.sz + b.dz * d;
            const float heading = std::atan2(b.dx, b.dz) + kBoatYaw;
            const float s = b.scale;
            if (kFlatBob) {   // A/B: the pre-fix flat bob, verbatim
                const float y = kBoatY + std::sin(t * 0.7f + b.off) * 0.35f;
                const float ch = std::cos(heading), sh = std::sin(heading);
                const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
                b.body->setInstanceTransform(0, M);
                continue;
            }
            ShipWaveState w;
            echoShipPose(x, z, heading, b.halfLen, b.halfBeam, t, swell, w);
            const float y = kBoatY + w.heaveY;
            // R = Ry(heading) * Rx(-pitch) * Rz(-roll). The negations carry
            // echoShipPose's stated sign convention (+pitch = bow UP, +roll =
            // starboard DOWN) into this basis, where local +Z is forward and
            // local +X is starboard: Rx(+p) would push +Z down, and Rz(+r)
            // would push +X up — both backwards.
            const float ch = std::cos(heading),  sh = std::sin(heading);
            const float cp = std::cos(-w.pitchRad), sp = std::sin(-w.pitchRad);
            const float cr = std::cos(-w.rollRad),  sr = std::sin(-w.rollRad);
            // Row-major 3x3 products, written out (no matrix type in this TU).
            const float Ry[9] = {  ch, 0.0f,  sh,
                                  0.0f, 1.0f, 0.0f,
                                  -sh, 0.0f,  ch };
            const float Rx[9] = { 1.0f, 0.0f, 0.0f,
                                  0.0f,   cp,  -sp,
                                  0.0f,   sp,   cp };
            const float Rz[9] = {   cr,  -sr, 0.0f,
                                    sr,   cr, 0.0f,
                                  0.0f, 0.0f, 1.0f };
            auto mul3 = [](const float A[9], const float B[9], float O[9]) {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        O[r*3+c] = A[r*3+0]*B[0*3+c] + A[r*3+1]*B[1*3+c] + A[r*3+2]*B[2*3+c];
            };
            float RyRx[9], R[9];
            mul3(Ry, Rx, RyRx);
            mul3(RyRx, Rz, R);
            // Column-major 4x4 for the device: column k = R * e_k, scaled.
            const float M[16] = {
                R[0]*s, R[3]*s, R[6]*s, 0.0f,
                R[1]*s, R[4]*s, R[7]*s, 0.0f,
                R[2]*s, R[5]*s, R[8]*s, 0.0f,
                x,      y,      z,      1.0f,
            };
            b.body->setInstanceTransform(0, M);
        }
    });
}

} // namespace x3::game
