// WP-3 implementation — see echo_woodlands.h for the deliverable summary.
//
// Everything under scatterWoodlands() below is a byte-for-byte port of
// host_echotropolis.cpp's WOODLANDS block (~lines 1152-1248, "island-wide
// conifer belts"): same hh() hash, same kPines table, same keep-out
// rects/circles, same freeway-corridor distance gate, same belt density
// bonus, same -1900..1900 step-13 iteration domain. Do not "clean up" any of
// these constants without re-checking against the host — the whole point of
// this file is that its output is a provable subset-partition of that loop.
//
// harvestDistrictLights() below is a port of loadDistrict's light-harvest
// half (host_echotropolis.cpp ~1716-1816), with the mesh-instancing half
// (addGlbInstance/slab/worldBounds/districts.push_back) deliberately dropped
// — that's WP-2's buildDistrict job, not this file's.

#include "echo_woodlands.h"

#include "../env_art.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace x3::game {

namespace {

// ===================== SHARED WOODLANDS SCATTER CONSTANTS ===================
// Verbatim from host_echotropolis.cpp's WOODLANDS block. See that file for
// the design rationale in comments; this file only needs the numbers.

inline float hh(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15;
    return (n & 0xffffffu) / (float)0x1000000;
}

constexpr const char* kPines[6] = {
    "tree_pineTallA.glb", "tree_pineTallB.glb", "tree_pineTallC.glb",
    "tree_pineDefaultA.glb", "tree_pineDefaultB.glb", "tree_pineRoundB.glb",
};

struct KeepOutRect { float x0, x1, z0, z1; };
constexpr KeepOutRect kKeepOut[3] = {
    { 935.0f - 25.0f, 1110.0f + 25.0f, 1180.0f - 25.0f, 1290.0f + 25.0f },  // Recife 2050 pad
    { 540.0f - 25.0f,  860.0f + 25.0f,  190.0f - 25.0f,  510.0f + 25.0f },  // Urban District pad
    { 1210.0f - 25.0f, 1470.0f + 25.0f, 870.0f - 25.0f, 1130.0f + 25.0f },  // HIVEMIND Cybercity pad
};
constexpr float kCrownCX = -20.0f, kCrownCZ = 760.0f, kCrownR = 470.0f;    // downtown crown
constexpr float kMineCX  = -480.0f, kMineCZ = 850.0f, kMineR = 175.0f;     // MINE FOREST's circle
constexpr float kMetroX0 = -75.0f, kMetroX1 = -45.0f, kMetroZ0 = 520.0f, kMetroZ1 = 1000.0f;

struct Wp { float x, z; };
constexpr Wp kFreeway[10] = {
    { -160.0f,  720.0f }, {  120.0f,  720.0f }, {  480.0f,  900.0f },
    {  820.0f, 1120.0f }, { 1060.0f,  900.0f }, {  980.0f,  560.0f },
    {  700.0f,  420.0f }, {  300.0f,  430.0f }, {  -60.0f,  560.0f },
    { -160.0f,  700.0f },
};
constexpr int kNumFwWp = (int)(sizeof(kFreeway) / sizeof(kFreeway[0]));

struct Belt { float x0, x1, z0, z1; };
constexpr Belt kBelts[3] = {
    { 150.0f, 900.0f,  950.0f, 1250.0f },    // belt1
    { 300.0f, 1200.0f, 540.0f,  880.0f },    // belt2
    { -900.0f, -150.0f, 1000.0f, 1550.0f },  // belt3
};

constexpr float kBaseKeep = 0.46f;

inline float distToSeg(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    float t = l2 > 1e-6f ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    const float sx = ax + t * dx, sz = az + t * dz, ex = px - sx, ez = pz - sz;
    return std::sqrt(ex * ex + ez * ez);
}

// ===================== CELL GRID (plan §1: 3x3 over island land) ============
// Nominal domain x in [-800,1600), z in [100,1500) -> cellW=800, cellH~466.67.
// The OUTER edge of each axis is opened to +/-infinity (rather than hard-clipped
// at -800/1600/100/1500) so EVERY surviving global placement maps to exactly
// one of the 9 cells, unconditionally -- this is what makes the bit-identical
// union claim provable rather than merely "true in practice because the real
// island's vegetated land happens to fit inside the nominal rect". If a future
// terrain edit ever put a woodlands placement outside the nominal rect, it
// still lands in the correct edge cell instead of silently vanishing from the
// union. cellIx: 0=west..2=east. cellIz: 0=south (low z)..2=north (high z).
// Integrator: map the 9 region ids (woodlands_NW/N/NE/W/C/E/SW/S/SE) to
// (cellIx,cellIz) pairs however the JSON wants; this file only indexes by ints.
struct CellRect { float x0, x1, z0, z1; };

constexpr float kWoodX0 = -800.0f, kWoodX1 = 1600.0f;
constexpr float kWoodZ0 = 100.0f,  kWoodZ1 = 1500.0f;
constexpr float kWoodCellW = (kWoodX1 - kWoodX0) / 3.0f;
constexpr float kWoodCellH = (kWoodZ1 - kWoodZ0) / 3.0f;

CellRect woodlandsCellRect(int cellIx, int cellIz) {
    const float inf = std::numeric_limits<float>::infinity();
    CellRect r;
    r.x0 = (cellIx <= 0) ? -inf : kWoodX0 + cellIx * kWoodCellW;
    r.x1 = (cellIx >= 2) ?  inf : kWoodX0 + (cellIx + 1) * kWoodCellW;
    r.z0 = (cellIz <= 0) ? -inf : kWoodZ0 + cellIz * kWoodCellH;
    r.z1 = (cellIz >= 2) ?  inf : kWoodZ0 + (cellIz + 1) * kWoodCellH;
    return r;
}

inline bool inCell(float jx, float jz, const CellRect& r) {
    return jx >= r.x0 && jx < r.x1 && jz >= r.z0 && jz < r.z1;
}

// One surviving placement (post every gate), matching exactly what the
// legacy host fed into `woodlands.addGlbInstance(kPines[variant], T)`.
struct WoodlandPlacement {
    float T[16];   // column-major 4x4; T[12]=jx, T[13]=h0(ground y), T[14]=jz
    int   variant; // already reduced mod 6 (kPines index)
};

// THE core scatter. `cellFilter == nullptr` reproduces the legacy single
// -system sequence exactly (used by the self-test's "global" pass and
// nowhere else — buildWoodlandsCell always passes a real filter). When
// non-null, the filter is applied strictly AFTER the identical keep-out /
// elevation / slope / density gate the legacy system used, so a cell's
// output is always an order-preserving SUBSET of the global sequence with
// the SAME transforms for the placements it keeps — never a re-derivation.
// hh(seed) is a pure function of `seed` alone (no shared mutable RNG state),
// so filtering some iterations out never perturbs any other iteration's math.
template <class HF, class Emit>
void scatterWoodlands(HF& hf, const CellRect* cellFilter, Emit&& emit) {
    uint32_t seed = 0;
    for (float x = -1900.0f; x <= 1900.0f; x += 13.0f) {
        for (float z = -1900.0f; z <= 1900.0f; z += 13.0f, ++seed) {
            const float jx = x + (hh(seed * 5u + 1u) * 2.0f - 1.0f) * 6.0f;
            const float jz = z + (hh(seed * 5u + 2u) * 2.0f - 1.0f) * 6.0f;
            if (!hf.ok()) continue;
            const float h0 = hf.heightAt(jx, jz);
            if (h0 < 24.0f || h0 > 172.0f) continue;
            const float hX = hf.heightAt(jx + 9.0f, jz), hZ = hf.heightAt(jx, jz + 9.0f);
            if (std::fabs(hX - h0) >= 7.0f || std::fabs(hZ - h0) >= 7.0f) continue;

            bool blocked = false;
            for (const KeepOutRect& r : kKeepOut)
                if (jx >= r.x0 && jx <= r.x1 && jz >= r.z0 && jz <= r.z1) { blocked = true; break; }
            if (!blocked) {
                const float dcx = jx - kCrownCX, dcz = jz - kCrownCZ;
                if (dcx * dcx + dcz * dcz < kCrownR * kCrownR) blocked = true;
            }
            if (!blocked) {
                const float dmx = jx - kMineCX, dmz = jz - kMineCZ;
                if (dmx * dmx + dmz * dmz < kMineR * kMineR) blocked = true;
            }
            if (!blocked && jx >= kMetroX0 && jx <= kMetroX1 && jz >= kMetroZ0 && jz <= kMetroZ1) blocked = true;
            if (!blocked)
                for (int w = 0; w + 1 < kNumFwWp; ++w)
                    if (distToSeg(jx, jz, kFreeway[w].x, kFreeway[w].z,
                                  kFreeway[w + 1].x, kFreeway[w + 1].z) < 30.0f) { blocked = true; break; }
            if (blocked) continue;

            float density = kBaseKeep;
            for (const Belt& b : kBelts)
                if (jx >= b.x0 && jx <= b.x1 && jz >= b.z0 && jz <= b.z1) { density *= 3.0f; break; }
            if (hh(seed * 11u + 13u) >= density) continue;

            // Everything above this line is IDENTICAL regardless of cellFilter
            // -- a placement either survives the legacy gate or it doesn't.
            if (cellFilter && !inCell(jx, jz, *cellFilter)) continue;

            const float sc  = 10.0f + hh(seed * 7u + 5u) * 16.0f;      // 10-26 m
            const float yaw = hh(seed * 7u + 3u) * 6.2831853f;
            const int variant = (int)(hh(seed * 7u + 9u) * 6.0f);
            const float c = std::cos(yaw), s = std::sin(yaw);
            WoodlandPlacement p;
            p.variant = variant % 6;
            const float T[16] = { c * sc, 0, -s * sc, 0,  0, sc, 0, 0,  s * sc, 0, c * sc, 0,  jx, h0, jz, 1 };
            std::copy(T, T + 16, p.T);
            emit(p);
        }
    }
}

} // anonymous namespace

// ===================== buildWoodlandsCell ====================================
void buildWoodlandsCell(int cellIx, int cellIz, EchoRegion& region, EchoRegionCtx& ctx) {
    if (cellIx < 0 || cellIx > 2 || cellIz < 0 || cellIz > 2) {
        x3::logError("--world echotropolis: WOODLANDS CELL bad index (" +
                     std::to_string(cellIx) + "," + std::to_string(cellIz) + ")");
        return;
    }

    auto cell = std::make_unique<x3::game::EnvArtSystem>();
    if (!cell->beginFromDir(ctx.device, ctx.vegDir)) {
        x3::logWarn("--world echotropolis: WOODLANDS CELL (" + std::to_string(cellIx) + "," +
                    std::to_string(cellIz) + ") — veg dir mount failed (" + ctx.vegDir + "), no trees");
        return;
    }

    const CellRect rect = woodlandsCellRect(cellIx, cellIz);
    uint32_t planted = 0;
    scatterWoodlands(ctx.hf, &rect, [&](const WoodlandPlacement& p) {
        if (cell->addGlbInstance(kPines[p.variant], p.T)) ++planted;
    });
    cell->setFoliage(1.0f);

    x3::logInfo("--world echotropolis: WOODLANDS CELL (" + std::to_string(cellIx) + "," +
                std::to_string(cellIz) + ") — " + std::to_string(planted) + " pines scattered");

    if (planted > 0) region.addArt(std::move(cell));
}

// ===================== harvestDistrictLights =================================
// Position-only port of loadDistrict's per-piece math: piece translation
// (px,py,pz) through the pad basis P, plus the same max-terrain-height slab
// seat (gy). The piece's ROTATION (quat -> L0, meshFix -> F, W = P*L) is
// deliberately NOT reproduced here -- it only ever affected a mesh instance's
// ORIENTATION, never a point light's world POSITION, so porting it would be
// dead code. If that assumption ever changes (e.g. a future light gets a
// directional/spot term keyed off piece orientation), this function needs
// the same L0/F/W block loadDistrict has (host_echotropolis.cpp ~1769-1792).
std::vector<x3::rhi::PointLight> harvestDistrictLights(
    EchoRegionCtx& ctx,
    const char* layoutPath,
    float padX, float padZ, float padYaw, float padScale,
    float padYOff, const char* tag)
{
    std::vector<x3::rhi::PointLight> lights;

    std::ifstream lf(layoutPath);
    if (!lf) {
        x3::logWarn(std::string("--world echotropolis: DISTRICT ") + tag +
                    " LIGHT SLICE — layout missing: " + layoutPath);
        return lights;
    }

    struct Piece { std::string glb; float px, py, pz; };
    std::vector<Piece> pieces; pieces.reserve(4096);
    float lminx = 1e9f, lmaxx = -1e9f, lminz = 1e9f, lmaxz = -1e9f;
    {
        std::string line; char name[256];
        float px, py, pz, qx, qy, qz, qw, sx, sy, sz;
        while (std::getline(lf, line)) {
            if (line.empty() || line[0] == '#') continue;
            // SAME 11-field line format loadDistrict parses (glb + TRS quat);
            // this function only needs the translation, but the sscanf format
            // string is kept identical so a malformed line is rejected the
            // same way in both places (no silent drift between the two ports).
            if (std::sscanf(line.c_str(), "%255s %f %f %f %f %f %f %f %f %f %f",
                            name, &px, &py, &pz, &qx, &qy, &qz, &qw, &sx, &sy, &sz) != 11) continue;
            pieces.push_back({ name, px, py, pz });
            lminx = std::min(lminx, px); lmaxx = std::max(lmaxx, px);
            lminz = std::min(lminz, pz); lmaxz = std::max(lmaxz, pz);
        }
    }
    if (pieces.empty()) return lights;

    // Content-sized slab extent, exactly as loadDistrict computes it (needed
    // only to bound the max-terrain-height seat scan below).
    const float cw = (lmaxx - lminx) * padScale + 40.0f;
    const float cl = (lmaxz - lminz) * padScale + 40.0f;
    const float ccx = padX + (lminx + lmaxx) * 0.5f * padScale;
    const float ccz = padZ + (lminz + lmaxz) * 0.5f * padScale;
    float gy = ctx.hf.ok() ? ctx.hf.heightAt(ccx, ccz) : 190.0f;
    if (ctx.hf.ok()) {
        for (float ox = -cw * 0.5f; ox <= cw * 0.5f; ox += 20.0f)
            for (float oz = -cl * 0.5f; oz <= cl * 0.5f; oz += 20.0f)
                gy = std::max(gy, ctx.hf.heightAt(ccx + ox, ccz + oz));
        gy += 1.0f;   // safety: heightfield bumps narrower than the stride
    }

    // Pad basis (position columns only — see the function comment above for
    // why the L/rotation columns loadDistrict also builds are dropped here).
    const float S = padScale, pc = std::cos(padYaw) * S, ps = std::sin(padYaw) * S;
    const float P[9] = { pc, 0.0f, -ps,   0.0f, S, 0.0f,   ps, 0.0f, pc };

    for (const Piece& pcs : pieces) {
        const float tx = padX + P[0] * pcs.px + P[3] * pcs.py + P[6] * pcs.pz;
        const float ty = gy + padYOff + P[1] * pcs.px + P[4] * pcs.py + P[7] * pcs.pz;
        const float tz = padZ + P[2] * pcs.px + P[5] * pcs.py + P[8] * pcs.pz;

        // Name-classified caster (SAME thresholds/colors as loadDistrict):
        // ONLY lamps + small neon signs cast; big flat glowing surfaces
        // (screens/holograms) stay emissive-only (a point light on one blows
        // out its own billboard face).
        auto has = [&](const char* k) {
            std::string n(pcs.glb);
            for (auto& c : n) c = (char)std::tolower((unsigned char)c);
            return n.find(k) != std::string::npos;
        };
        x3::rhi::PointLight pl; bool isLight = true; float lift = 1.6f;
        if (has("streetlamp") || has("lampwall") || has("poste") || has("lamp")) {
            pl.color[0] = 2.4f; pl.color[1] = 1.7f; pl.color[2] = 0.9f; pl.range = 15.0f; lift = 3.2f; // warm sodium
        } else if (has("letreiro") || has("luminoso") || (has("neon") && !has("screen"))) {
            pl.color[0] = 2.4f; pl.color[1] = 0.7f; pl.color[2] = 1.9f; pl.range = 9.0f;               // magenta neon
        } else {
            isLight = false;
        }
        if (isLight) {
            pl.pos[0] = tx; pl.pos[1] = ty + lift; pl.pos[2] = tz;
            lights.push_back(pl);
        }
    }

    x3::logInfo(std::string("--world echotropolis: DISTRICT ") + tag + " LIGHT SLICE — " +
                std::to_string(lights.size()) + " neon/lamp point lights");
    return lights;
}

// ===================== echoWoodlandsSliceSelfTest =============================
// Cost note for whoever wires this in: the -1900..1900 step-13 domain is
// ~293x293 ~= 85.8k grid cells; this self-test walks it TEN times (1 global +
// 9 cell-filtered passes) = ~858k iterations of cheap float math + one
// bilinear Heightfield::heightAt sample each -- sub-10ms on any dev machine,
// pure CPU, boot-time-once. Not a per-frame cost.
bool echoWoodlandsSliceSelfTest(Heightfield& hf) {
    if (!hf.ok()) {
        x3::logError("--world echotropolis: WOODLANDS SELF-TEST — heightfield not loaded, "
                     "cannot prove bit-identity (every placement would trivially fail the "
                     "terrain gate and count==0==0 would pass without proving anything)");
        return false;
    }

    bool ok = true;

    // ---- Global pass: the exact legacy single-system sequence ----
    uint64_t globalCount = 0;
    float globalFirst[16] = {}, globalLast[16] = {};
    scatterWoodlands(hf, (const CellRect*)nullptr, [&](const WoodlandPlacement& p) {
        if (globalCount == 0) std::copy(p.T, p.T + 16, globalFirst);
        std::copy(p.T, p.T + 16, globalLast);
        ++globalCount;
    });

    // ---- 9 sliced passes ----
    uint64_t slicedTotal = 0;
    float sliceFirst[3][3][16] = {}, sliceLast[3][3][16] = {};
    uint64_t sliceCount[3][3] = {};
    for (int iz = 0; iz < 3; ++iz) {
        for (int ix = 0; ix < 3; ++ix) {
            const CellRect rect = woodlandsCellRect(ix, iz);
            uint64_t n = 0;
            scatterWoodlands(hf, &rect, [&](const WoodlandPlacement& p) {
                if (n == 0) std::copy(p.T, p.T + 16, sliceFirst[iz][ix]);
                std::copy(p.T, p.T + 16, sliceLast[iz][ix]);
                ++n;
            });
            sliceCount[iz][ix] = n;
            slicedTotal += n;
        }
    }

    if (slicedTotal != globalCount) {
        x3::logError("--world echotropolis: WOODLANDS SELF-TEST FAILED — count mismatch: global=" +
                     std::to_string(globalCount) + " sliced-union=" + std::to_string(slicedTotal));
        ok = false;
    }

    if (globalCount == 0) {
        // Vacuously "matches" (0 == 0), but a real loaded island heightmap
        // should always produce several thousand placements (plan §1's
        // density survey: ~9000-12000) -- zero here almost certainly means
        // the wrong heightmap loaded, not a genuine proof. Flag it loudly
        // without failing the self-test on a technicality.
        x3::logWarn("--world echotropolis: WOODLANDS SELF-TEST — heightfield loaded but produced "
                    "ZERO placements; the count/transform match is vacuous, not a real proof. "
                    "Check the heightmap is the real island PNG, not a stub.");
    }

    if (globalCount > 0) {
        auto transformsEqual = [](const float a[16], const float b[16]) {
            for (int i = 0; i < 16; ++i)
                if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
            return true;
        };
        // Locate which single cell owns a given (already-surviving) placement
        // (jx=T[12], jz=T[14] per the T[16] layout scatterWoodlands emits),
        // by the SAME inCell() test buildWoodlandsCell uses — then confirm
        // that cell's own first/last transform matches the global one
        // exactly. This does not assume anything about the island's shape;
        // it just asks "whichever cell claims to own this placement, does it
        // actually reproduce it bit-for-bit".
        auto locate = [](const float T[16], int& ix, int& iz) {
            const float jx = T[12], jz = T[14];
            for (iz = 0; iz < 3; ++iz)
                for (ix = 0; ix < 3; ++ix)
                    if (inCell(jx, jz, woodlandsCellRect(ix, iz))) return;
            ix = iz = -1;   // unreachable: outer cells are open to +/-infinity
        };
        int fx, fz, lx, lz;
        locate(globalFirst, fx, fz);
        locate(globalLast, lx, lz);
        if (fx < 0 || sliceCount[fz][fx] == 0 || !transformsEqual(sliceFirst[fz][fx], globalFirst)) {
            x3::logError("--world echotropolis: WOODLANDS SELF-TEST FAILED — first-transform mismatch "
                         "(owning cell " + std::to_string(fx) + "," + std::to_string(fz) + ")");
            ok = false;
        }
        if (lx < 0 || sliceCount[lz][lx] == 0 || !transformsEqual(sliceLast[lz][lx], globalLast)) {
            x3::logError("--world echotropolis: WOODLANDS SELF-TEST FAILED — last-transform mismatch "
                         "(owning cell " + std::to_string(lx) + "," + std::to_string(lz) + ")");
            ok = false;
        }
    }

    if (ok) {
        x3::logInfo("--world echotropolis: WOODLANDS SELF-TEST PASSED — global=" +
                    std::to_string(globalCount) +
                    " instances; 9-cell union matches bit-identically (count + first/last transform)");
    }
    return ok;
}

} // namespace x3::game
