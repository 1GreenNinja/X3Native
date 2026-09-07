// RIVER RAPIDS — the flow field, the reach table, the boulders and the gate.
// See river_rapids.h for the model; this file is the ONE producer of every
// number the water shader, the mist and the boulder placer read.
#include "river_rapids.h"
#include "terrain.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace x3::game {

using WP = x3::rhi::IRenderDevice::WaterParams;

namespace {

int g_force = -1;   // riverRapidsForce(): -1 env decides

// ---- THE REACH TABLE -------------------------------------------------------
// Metres along worldUnderRiverChain(). Authored against the MEASURED table
// (--test-underriver prints it): the node-table grades are 0.024 over
// 192-380 m and 0.030 over 380-563 m (the two real steps the country forced),
// 0.004 (kURMinFall) everywhere else, with pools at 380 m (upper pool),
// 1089 m (the Great Hall) and 1857 m (the plunge pool). The vault ends at
// total-120 = 1737 m; the gorge runs from there into daylight.
//
// The table follows that shape and adds what 12 nodes cannot: the two steps
// are the two big rapids, the gorge is a rapid because a gorge IS one (a
// narrowing at the same fall — the water has nowhere to go but faster and
// rougher), the plunge is the drop into the last pool, and the long reaches
// between are the quiet water the owner asked for, broken by riffles so a
// 300 m pool does not read as a canal. Calm reaches are the Rev 11 look
// exactly (turbulence 0 -> the shader's rapids terms are all zero).
//
// Gate R2 proves the table covers [0, total] with no gaps/overlaps; gate R5
// proves every node-table step (grade >= 0.6*kURRushGrade) lies mostly under a
// Rapid — the authored table cannot contradict the physics it decorates.
const RiverReach kReaches[] = {
    {    0.0f,  150.0f, RiverReachKind::Calm,   1.00f, "the head grotto pool" },
    {  150.0f,  195.0f, RiverReachKind::Riffle, 0.50f, "the run out of the head pool" },
    {  195.0f,  345.0f, RiverReachKind::Rapid,  0.80f, "the first step (bed drops 4.6 m)" },
    {  345.0f,  425.0f, RiverReachKind::Calm,   1.00f, "the upper pool" },
    {  425.0f,  565.0f, RiverReachKind::Rapid,  1.00f, "the big step into the cavern (steepest grade)" },
    {  565.0f,  640.0f, RiverReachKind::Riffle, 0.60f, "run-out under the shallow roof" },
    {  640.0f,  830.0f, RiverReachKind::Calm,   1.00f, "the long reach" },
    {  830.0f,  880.0f, RiverReachKind::Riffle, 0.35f, "a gravel bar" },
    {  880.0f, 1180.0f, RiverReachKind::Calm,   1.00f, "the Great Hall pool" },
    { 1180.0f, 1250.0f, RiverReachKind::Riffle, 0.50f, "leaving the Hall" },
    { 1250.0f, 1560.0f, RiverReachKind::Calm,   1.00f, "the west-outpost bend" },
    { 1560.0f, 1640.0f, RiverReachKind::Riffle, 0.60f, "the gorge approach" },
    { 1640.0f, 1800.0f, RiverReachKind::Rapid,  0.90f, "THE GORGE (vault ends at 1737 m, runs into daylight)" },
    { 1800.0f, 1835.0f, RiverReachKind::Plunge, 1.00f, "the plunge" },
    { 1835.0f,   -1.0f, RiverReachKind::Calm,   1.00f, "the plunge pool" },
};
constexpr uint32_t kReachCount = (uint32_t)(sizeof(kReaches) / sizeof(kReaches[0]));

// ---- THE FLOW MODEL'S CONSTANTS ------------------------------------------
// v0: what a river at the minimum fall (kURMinFall, grade/kURRushGrade =
// 0.13) runs at through the reference section — with v1 that is ~1.0 m/s,
// a walking-pace current. v1: the gradient response; full whitewater grade
// (kURRushGrade) through the reference section runs at v0+v1 = 2.25 m/s and
// the plunge (2x that grade) at 3.0 m/s. Aref: the reference wet section,
// kURHalfWidth wide at the 2.0 m non-pool bed depth (terrain.cpp's UR carve).
// Pools carve to ~4.5 m, so continuity slows them to ~0.45 m/s — still water
// that still moves.
constexpr float kV0   = 0.35f;
constexpr float kV1   = 1.90f;
constexpr float kAref = 2.0f * kURHalfWidth * 2.0f;
constexpr float kVMin = 0.10f, kVMax = 4.5f;
// Standing waves: amplitude per (turbulence * speed). turb 1 at 2.25 m/s
// -> 0.25 m crests over a 7 m channel; the plunge at 3 m/s -> 0.33 m.
constexpr float kSwAmpPerVel = 0.11f;
constexpr float kSwAmpMax    = 0.40f;
// Froude wavelength lambda = 2*pi*v^2/g is 3.2 m at 2.25 m/s — but the water
// grid is 480 m / 191 cells = 2.5 m, and a vertex displacement shorter than
// ~2.5 cells is aliasing, not a wave. Floor the DISPLACED wavelength at 7 m;
// the finer 2-4 m churn lives in the fragment stage's detail normal, where
// there is a sample per pixel.
constexpr float kSwLenMin = 7.0f;
constexpr float kSwLenMax = 16.0f;

// The boulders: (s, lateral, radius). Fast reaches only — a rock in a calm
// pool is a pool with a rock in it, not a rapid. Lateral within the wet bed
// (|lat| < kURBedHalfW - r) so every one sits on the flat channel floor.
struct BoulderSpec { float s, lat, r; };
const BoulderSpec kBoulders[] = {
    {  240.0f,  1.9f, 1.9f }, {  290.0f, -2.2f, 1.7f }, {  322.0f,  0.4f, 1.6f },   // the first step
    {  455.0f, -1.6f, 2.0f }, {  505.0f,  2.3f, 1.8f }, {  540.0f, -0.5f, 1.6f },   // the big step
    { 1670.0f,  1.4f, 1.9f }, { 1705.0f, -2.1f, 1.7f }, { 1750.0f,  2.2f, 1.8f },   // the gorge
    { 1785.0f, -0.7f, 1.6f },
};
constexpr uint32_t kBoulderCount = (uint32_t)(sizeof(kBoulders) / sizeof(kBoulders[0]));

inline float smoothstepf(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The reach's ramp window at s: 0 outside, 1 inside, smooth over
// +-kRiverReachEdge at each boundary (the first reach has no upstream ramp,
// the last has no downstream one).
inline float reachWindow(const RiverReach& r, float s, float total, bool first, bool last) {
    const float s1 = r.s1 <= 0.0f ? total : r.s1;
    const float e = kRiverReachEdge;
    const float in  = first ? 1.0f : smoothstepf(r.s0 - e, r.s0 + e, s);
    const float out = last  ? 1.0f : 1.0f - smoothstepf(s1 - e, s1 + e, s);
    return in * out;
}

// Node-table interpolation on the chain at s: segment index, t, and the
// segment's own grade (fall per metre of run).
struct ChainAt { int i; float t; float grade; float hw; float depth; float w; };
ChainAt chainAt(const UnderRiverChain& uc, float s) {
    ChainAt c{};
    const float total = uc.cum[uc.n - 1];
    s = std::clamp(s, 0.0f, total);
    int i = 0;
    while (i + 2 < uc.n && uc.cum[i + 1] < s) ++i;
    const float len = std::max(uc.cum[i + 1] - uc.cum[i], 1.0f);
    c.i = i;
    c.t = std::clamp((s - uc.cum[i]) / len, 0.0f, 1.0f);
    c.grade = std::max((uc.w[i] - uc.w[i + 1]) / len, 0.0f);
    c.hw    = uc.hw[i] + (uc.hw[i + 1] - uc.hw[i]) * c.t;
    c.depth = uc.bedDrop[i] + (uc.bedDrop[i + 1] - uc.bedDrop[i]) * c.t;
    c.w     = uc.w[i] + (uc.w[i + 1] - uc.w[i]) * c.t;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
bool riverRapidsEnabled() {
    if (g_force >= 0) return g_force != 0;
    static const bool s_env = [] {
        const char* e = std::getenv("X3_RIVER_RAPIDS");
        return !(e && e[0] == '0');
    }();
    return s_env;
}
void riverRapidsForce(int state) { g_force = state; }

const RiverReach* underRiverReaches(uint32_t& count) { count = kReachCount; return kReaches; }

const char* riverReachKindName(RiverReachKind k) {
    switch (k) {
        case RiverReachKind::Calm:   return "Calm";
        case RiverReachKind::Riffle: return "Riffle";
        case RiverReachKind::Rapid:  return "Rapid";
        case RiverReachKind::Plunge: return "Plunge";
    }
    return "?";
}

// The sub-node effective gradient a kind stands for. Rapid = kURRushGrade by
// definition (terrain.h: "gradient that reads as full whitewater"); a Riffle
// is a gravel bar at ~40% of that; a Plunge is a step — twice it.
float riverReachKindGrade(RiverReachKind k) {
    switch (k) {
        case RiverReachKind::Calm:   return 0.0f;
        case RiverReachKind::Riffle: return 0.40f * kURRushGrade;
        case RiverReachKind::Rapid:  return kURRushGrade;
        case RiverReachKind::Plunge: return 2.0f * kURRushGrade;
    }
    return 0.0f;
}
float riverReachKindTurb(RiverReachKind k) {
    switch (k) {
        case RiverReachKind::Calm:   return 0.0f;
        case RiverReachKind::Riffle: return 0.45f;
        case RiverReachKind::Rapid:  return 0.90f;
        case RiverReachKind::Plunge: return 1.00f;
    }
    return 0.0f;
}

const RiverReach& riverReachAt(float s, float total) {
    for (uint32_t k = 0; k < kReachCount; ++k) {
        const float s1 = kReaches[k].s1 <= 0.0f ? total : kReaches[k].s1;
        if (s < s1 || k + 1 == kReachCount) return kReaches[k];
    }
    return kReaches[kReachCount - 1];
}

float riverReachTurbulenceAt(float s, float total) {
    float turb = 0.0f;
    for (uint32_t k = 0; k < kReachCount; ++k) {
        const RiverReach& r = kReaches[k];
        const float c = riverReachKindTurb(r.kind) * r.intensity;
        if (c <= 0.0f) continue;
        turb += c * reachWindow(r, s, total, k == 0, k + 1 == kReachCount);
    }
    return std::clamp(turb, 0.0f, 1.0f);
}

float riverReachGradeAt(float s, float total) {
    float g = 0.0f;
    for (uint32_t k = 0; k < kReachCount; ++k) {
        const RiverReach& r = kReaches[k];
        const float c = riverReachKindGrade(r.kind) * r.intensity;
        if (c <= 0.0f) continue;
        g += c * reachWindow(r, s, total, k == 0, k + 1 == kReachCount);
    }
    return g;
}

float riverFlowSpeed(float grade, float halfWidth, float depth) {
    const float gr = std::max(grade, 0.0f) / kURRushGrade;
    const float A  = std::max(2.0f * halfWidth * depth, 0.5f);
    return std::clamp((kV0 + kV1 * std::sqrt(gr)) * (kAref / A), kVMin, kVMax);
}

RiverFlowSample underRiverFlowAt(const UnderRiverChain& uc, float s) {
    RiverFlowSample f{};
    if (uc.n < 2) return f;
    const float total = uc.cum[uc.n - 1];
    const ChainAt c = chainAt(uc, s);
    f.halfWidth = c.hw;
    f.depth     = std::max(c.depth, 0.3f);
    f.grade     = std::max(c.grade, riverReachGradeAt(s, total));
    f.speed     = riverFlowSpeed(f.grade, f.halfWidth, f.depth);
    f.turbulence = riverReachTurbulenceAt(s, total);
    f.waveAmp = std::min(kSwAmpPerVel * f.turbulence * f.speed, kSwAmpMax);
    f.waveLen = std::clamp(6.2831853f * f.speed * f.speed / 9.81f, kSwLenMin, kSwLenMax);
    return f;
}

// PAIRED with water.frag rapidsFoamBase(): 0.75*turb + 0.5*bank*turb through
// smoothstep(0.12, 0.75). Mid-channel in a full Rapid that is 0.75 -> 1.0;
// a 0.35 riffle (turb 0.16) -> ~0.0; Calm -> exactly 0.
float riverFoamBaseAt(const UnderRiverChain& uc, float s, float lat) {
    if (uc.n < 2) return 0.0f;
    const RiverFlowSample f = underRiverFlowAt(uc, s);
    const float bank = smoothstepf(0.55f, 0.95f, std::fabs(lat) / std::max(f.halfWidth, 1.0f));
    return smoothstepf(0.08f, 0.70f, 0.75f * f.turbulence + 0.5f * bank * f.turbulence);
}

uint32_t underRiverBoulders(const UnderRiverChain& uc, RiverBoulder* out, uint32_t maxN) {
    if (uc.n < 2 || !riverRapidsEnabled()) return 0;
    uint32_t n = 0;
    for (uint32_t k = 0; k < kBoulderCount && n < maxN; ++k) {
        const BoulderSpec& b = kBoulders[k];
        const ChainAt c = chainAt(uc, b.s);
        const float len = std::max(uc.cum[c.i + 1] - uc.cum[c.i], 1.0f);
        const float dx = (uc.x[c.i + 1] - uc.x[c.i]) / len, dz = (uc.z[c.i + 1] - uc.z[c.i]) / len;
        const float px = -dz, pz = dx;
        RiverBoulder r{};
        r.s = b.s; r.lat = b.lat;
        r.dirX = dx; r.dirZ = dz;
        r.x = uc.x[c.i] + dx * (b.s - uc.cum[c.i]) + px * b.lat;
        r.z = uc.z[c.i] + dz * (b.s - uc.cum[c.i]) + pz * b.lat;
        // Seat it on the carved bed and make it break the surface by
        // kBoulderShow: the crown is at y + squash*r, the keel at y - squash*r.
        // Grow the radius if the bed is deeper than the authored rock could
        // stand in — a boulder that floats is worse than a big one. A grown
        // rock is then pulled toward the spine so it still sits inside the
        // flat bed (|lat| + r <= kURBedHalfW): the carve's side slope would
        // otherwise leave a bigger rock with one flank hanging off the wall.
        const float bed = terrainHeightAtWorld(r.x, r.z);
        const float need = (c.w - bed + kBoulderShow + 0.30f) / (2.0f * kBoulderSquash);
        r.radius = std::max(b.r, need);
        const float latMax = std::max(kURBedHalfW - r.radius, 0.0f);
        if (std::fabs(r.lat) > latMax) {
            r.lat = (r.lat < 0.0f ? -latMax : latMax);
            r.x = uc.x[c.i] + dx * (b.s - uc.cum[c.i]) + px * r.lat;
            r.z = uc.z[c.i] + dz * (b.s - uc.cum[c.i]) + pz * r.lat;
        }
        r.y = c.w + kBoulderShow - kBoulderSquash * r.radius;
        const RiverFlowSample f = underRiverFlowAt(uc, b.s);
        r.wakeLen = std::clamp(3.0f * r.radius + 2.5f * f.speed, 6.0f, 18.0f);
        out[n++] = r;
    }
    return n;
}

void bakeUnderRiverFlow(WP& wp) {
    if (!riverRapidsEnabled()) return;
    const UnderRiverChain& uc = worldUnderRiverChain();
    if (uc.n < 2) return;
    const float total = uc.cum[uc.n - 1];
    const uint32_t n = std::min<uint32_t>((uint32_t)uc.n, WP::kMaxRiverNodes);
    wp.flowSampleCount = WP::kFlowSamples;
    wp.flowLength = total;
    for (uint32_t i = 0; i < n; ++i) wp.riverNodeS[i] = uc.cum[i];
    for (uint32_t k = 0; k < WP::kFlowSamples; ++k) {
        const float s = total * (float)k / (float)(WP::kFlowSamples - 1);
        const RiverFlowSample f = underRiverFlowAt(uc, s);
        wp.flowLut[k][0] = f.speed;
        wp.flowLut[k][1] = f.turbulence;
        wp.flowLut[k][2] = f.waveAmp;
        wp.flowLut[k][3] = f.waveLen;
    }
    RiverBoulder rocks[WP::kMaxRocks];
    const uint32_t rn = underRiverBoulders(uc, rocks, WP::kMaxRocks);
    wp.rockCount = rn;
    for (uint32_t i = 0; i < rn; ++i) {
        wp.rocks[i][0] = rocks[i].x; wp.rocks[i][1] = rocks[i].z;
        wp.rocks[i][2] = rocks[i].radius; wp.rocks[i][3] = rocks[i].wakeLen;
    }
}

// The surface river is CALM end to end — the approved stills along it are
// the contract — but it still flows: speed from the risen table's own grade
// and the ribbon's section, turbulence 0, no standing waves, no rocks.
void bakeSurfaceRiverFlow(WP& wp) {
    if (!riverRapidsEnabled()) return;
    const uint32_t n = std::min(wp.riverNodeCount, WP::kMaxRiverNodes);
    if (n < 2) return;
    float cum[WP::kMaxRiverNodes] = {};
    for (uint32_t i = 1; i < n; ++i) {
        const float dx = wp.riverNodes[i][0] - wp.riverNodes[i - 1][0];
        const float dz = wp.riverNodes[i][1] - wp.riverNodes[i - 1][1];
        cum[i] = cum[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    const float total = cum[n - 1];
    if (total < 1.0f) return;
    wp.flowSampleCount = WP::kFlowSamples;
    wp.flowLength = total;
    for (uint32_t i = 0; i < n; ++i) wp.riverNodeS[i] = cum[i];
    for (uint32_t k = 0; k < WP::kFlowSamples; ++k) {
        const float s = total * (float)k / (float)(WP::kFlowSamples - 1);
        uint32_t i = 0;
        while (i + 2 < n && cum[i + 1] < s) ++i;
        const float len = std::max(cum[i + 1] - cum[i], 1.0f);
        const float grade = std::max((wp.riverNodes[i][2] - wp.riverNodes[i + 1][2]) / len, 0.0f);
        wp.flowLut[k][0] = riverFlowSpeed(grade, wp.riverHalfWidth, kWorldRiverBedDrop);
        wp.flowLut[k][1] = 0.0f; wp.flowLut[k][2] = 0.0f; wp.flowLut[k][3] = kSwLenMin;
    }
    wp.rockCount = 0;
}

void riverFlowAdvect(float time, float speed, float dirX, float dirZ,
                     float offA[2], float offB[2], float& weightA) {
    const float phA = time / kFlowCycle - std::floor(time / kFlowCycle);
    const float phB = phA + 0.5f - std::floor(phA + 0.5f);
    const float dA = speed * (phA - 0.5f) * kFlowCycle;
    const float dB = speed * (phB - 0.5f) * kFlowCycle;
    offA[0] = -dirX * dA; offA[1] = -dirZ * dA;
    offB[0] = -dirX * dB; offB[1] = -dirZ * dB;
    weightA = 1.0f - std::fabs(2.0f * phA - 1.0f);
}

// ===========================================================================
// --test-riverrapids
// ===========================================================================
bool runRiverRapidsSelfTest() {
    int passN = 0, failN = 0;
    char d[420];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" -- ") + detail;
        if (ok) { ++passN; x3::logInfo("[river-rapids] " + m); }
        else    { ++failN; x3::logError("[river-rapids] " + m); }
    };
    const int savedForce = g_force;
    riverRapidsForce(1);

    const UnderRiverChain& uc = worldUnderRiverChain();
    const float total = uc.n >= 2 ? uc.cum[uc.n - 1] : 0.0f;

    // ---- R1 the UBO block is what the shader declares -----------------------
    // (the static_assert in VulkanRenderDevice_internal.h is the hard gate; this
    // quotes the numbers so a drift shows in the log, not only in the build)
    {
        std::snprintf(d, sizeof(d), "WaterParams=%zu B, flow LUT %u x vec4, %u rocks, %u nodes",
                      sizeof(WP), WP::kFlowSamples, WP::kMaxRocks, WP::kMaxRiverNodes);
        check(WP::kFlowSamples == 64 && WP::kMaxRocks == 12 && sizeof(WP) > 2128,
              "R1 the flow block exists and its dimensions match water.{vert,frag}", d);
    }

    // ---- R2 the reach table is monotone and covers the run ------------------
    {
        bool mono = kReaches[0].s0 == 0.0f, cover = true;
        for (uint32_t k = 0; k + 1 < kReachCount; ++k) {
            if (!(kReaches[k].s1 > kReaches[k].s0)) mono = false;
            if (kReaches[k + 1].s0 != kReaches[k].s1) cover = false;
        }
        const bool tail = kReaches[kReachCount - 1].s1 <= 0.0f ||
                          kReaches[kReachCount - 1].s1 >= total;
        const bool inside = kReaches[kReachCount - 1].s0 < total;
        int calmN = 0, riffleN = 0, rapidN = 0, plungeN = 0;
        for (uint32_t k = 0; k < kReachCount; ++k) switch (kReaches[k].kind) {
            case RiverReachKind::Calm: ++calmN; break;      case RiverReachKind::Riffle: ++riffleN; break;
            case RiverReachKind::Rapid: ++rapidN; break;    case RiverReachKind::Plunge: ++plungeN; break;
        }
        std::snprintf(d, sizeof(d), "%u reaches over %.0f m: %d calm, %d riffle, %d rapid, %d plunge; "
                      "monotone=%d contiguous=%d tail-to-end=%d", kReachCount, total,
                      calmN, riffleN, rapidN, plungeN, mono ? 1 : 0, cover ? 1 : 0, tail ? 1 : 0);
        check(mono && cover && tail && inside && rapidN >= 2 && plungeN >= 1 && calmN >= 3,
              "R2 the reach table is monotone, gap-free and covers the whole run", d);
        for (uint32_t k = 0; k < kReachCount; ++k) {
            const RiverReach& r = kReaches[k];
            float x0, z0, x1, z1, w0, w1;   // world XZ and water level at each end
            {
                const ChainAt a = chainAt(uc, r.s0);
                x0 = uc.x[a.i] + (uc.x[a.i + 1] - uc.x[a.i]) * a.t;
                z0 = uc.z[a.i] + (uc.z[a.i + 1] - uc.z[a.i]) * a.t;
                w0 = a.w;
                const ChainAt b = chainAt(uc, r.s1 <= 0.0f ? total : r.s1);
                x1 = uc.x[b.i] + (uc.x[b.i + 1] - uc.x[b.i]) * b.t;
                z1 = uc.z[b.i] + (uc.z[b.i + 1] - uc.z[b.i]) * b.t;
                w1 = b.w;
            }
            const float mid = 0.5f * (r.s0 + (r.s1 <= 0.0f ? total : r.s1));
            const RiverFlowSample f = underRiverFlowAt(uc, mid);
            std::snprintf(d, sizeof(d), "  %-6s %4.0f-%4.0f m  (%.0f,%.1f,%.0f)->(%.0f,%.1f,%.0f)  i=%.2f  "
                          "v=%.2f m/s turb=%.2f swA=%.2f swL=%.1f  %s",
                          riverReachKindName(r.kind), r.s0, r.s1 <= 0.0f ? total : r.s1,
                          x0, w0, z0, x1, w1, z1, r.intensity, f.speed, f.turbulence, f.waveAmp, f.waveLen, r.name);
            x3::logInfo(std::string("[river-rapids]") + d);
        }
    }

    // ---- R3 speed rises with gradient and with narrowing -------------------
    {
        const float vFlat = riverFlowSpeed(kURMinFall, kURHalfWidth, 2.0f);
        const float vMid  = riverFlowSpeed(0.5f * kURRushGrade, kURHalfWidth, 2.0f);
        const float vFull = riverFlowSpeed(kURRushGrade, kURHalfWidth, 2.0f);
        const float vWide = riverFlowSpeed(kURRushGrade, kURHalfWidth * 1.5f, 2.0f);
        const float vNarrow = riverFlowSpeed(kURRushGrade, kURHalfWidth * 0.6f, 2.0f);
        const float vDeep = riverFlowSpeed(kURMinFall, kURHalfWidth, 4.5f);
        std::snprintf(d, sizeof(d), "grade %.3f/%.3f/%.3f -> %.2f/%.2f/%.2f m/s; hw x1.5/x1/x0.6 -> "
                      "%.2f/%.2f/%.2f m/s; pool depth 4.5 m -> %.2f m/s",
                      kURMinFall, 0.5f * kURRushGrade, kURRushGrade, vFlat, vMid, vFull,
                      vWide, vFull, vNarrow, vDeep);
        check(vFlat < vMid && vMid < vFull && vWide < vFull && vFull < vNarrow && vDeep < vFlat &&
              vFlat > 0.8f && vFull > 2.0f && vFull < 3.0f,
              "R3 flow speed rises where the bed steepens and where the channel narrows", d);
    }

    // ---- R4 foam: 0 in calm reaches, > 0.5 at the centre of every rapid ----
    {
        float calmMax = 0.0f, rapidMin = 1.0f, riffleMax = 0.0f;
        for (uint32_t k = 0; k < kReachCount; ++k) {
            const RiverReach& r = kReaches[k];
            const float s1 = r.s1 <= 0.0f ? total : r.s1;
            // measured INSIDE the ramp, where the reach is itself
            const float a = r.s0 + kRiverReachEdge, b = s1 - kRiverReachEdge;
            if (b <= a) continue;
            for (int q = 0; q <= 8; ++q) {
                const float s = a + (b - a) * (float)q / 8.0f;
                const float fm = riverFoamBaseAt(uc, s, 0.0f);
                if (r.kind == RiverReachKind::Calm) calmMax = std::max(calmMax, fm);
                else if (r.kind == RiverReachKind::Riffle) riffleMax = std::max(riffleMax, fm);
                else rapidMin = std::min(rapidMin, fm);
            }
        }
        const float bankBoost = riverFoamBaseAt(uc, 1720.0f, 6.0f) - riverFoamBaseAt(uc, 1720.0f, 0.0f);
        std::snprintf(d, sizeof(d), "calm max %.3f, riffle max %.3f, rapid/plunge centre min %.3f, "
                      "bank adds %.3f in the gorge", calmMax, riffleMax, rapidMin, bankBoost);
        check(calmMax == 0.0f && rapidMin > 0.5f && riffleMax < rapidMin && bankBoost >= 0.0f,
              "R4 foam is zero in calm water and > 0.5 down the middle of every rapid", d);
    }

    // ---- R5 the table agrees with the physics it decorates ----------------
    // Every node-table segment that falls at >= 0.6*kURRushGrade is mostly
    // under a Rapid; every Rapid/Plunge is faster than every Calm reach.
    {
        bool stepsCovered = true; int steps = 0;
        for (int i = 0; i + 1 < uc.n; ++i) {
            const float len = std::max(uc.cum[i + 1] - uc.cum[i], 1.0f);
            const float g = (uc.w[i] - uc.w[i + 1]) / len;
            if (g < 0.6f * kURRushGrade) continue;
            ++steps;
            float covered = 0.0f;
            for (float s = uc.cum[i]; s < uc.cum[i + 1]; s += 1.0f)
                if (riverReachAt(s, total).kind == RiverReachKind::Rapid) covered += 1.0f;
            if (covered / len < 0.5f) stepsCovered = false;
        }
        float calmVMax = 0.0f, fastVMin = 1e9f;
        for (uint32_t k = 0; k < kReachCount; ++k) {
            const RiverReach& r = kReaches[k];
            const float s1 = r.s1 <= 0.0f ? total : r.s1;
            const float mid = 0.5f * (r.s0 + s1);
            const float v = underRiverFlowAt(uc, mid).speed;
            if (r.kind == RiverReachKind::Calm) calmVMax = std::max(calmVMax, v);
            if (r.kind == RiverReachKind::Rapid || r.kind == RiverReachKind::Plunge) fastVMin = std::min(fastVMin, v);
        }
        std::snprintf(d, sizeof(d), "%d node-table steps, all >= 50%% under a Rapid: %d; calm v <= %.2f, "
                      "rapid/plunge v >= %.2f m/s", steps, stepsCovered ? 1 : 0, calmVMax, fastVMin);
        check(steps >= 2 && stepsCovered && fastVMin > calmVMax,
              "R5 every real step in the node table is a Rapid and rapids outrun every calm reach", d);
    }

    // ---- R6 the door: OFF leaves Rev 11's bytes alone ----------------------
    {
        WP on{}, off{};
        std::memset(&on, 0, sizeof(WP)); new (&on) WP();
        std::memset(&off, 0, sizeof(WP)); new (&off) WP();
        // the cavern channel as world_water.cpp hands it over (nodes + hw)
        auto legacy = [&](WP& w) {
            w.enabled = true; w.riverNodeCount = (uint32_t)uc.n; w.riverHalfWidth = kURHalfWidth;
            for (int i = 0; i < uc.n; ++i) { w.riverNodes[i][0] = uc.x[i]; w.riverNodes[i][1] = uc.z[i]; w.riverNodes[i][2] = uc.w[i]; }
            w.foam = 0.90f; w.clarity = 0.94f; w.enclosed = 1.0f;
        };
        legacy(on); legacy(off);
        riverRapidsForce(1); bakeUnderRiverFlow(on);
        riverRapidsForce(0); bakeUnderRiverFlow(off);
        // the legacy prefix (everything before the flow block) must be
        // byte-identical between ON and OFF: the bake writes ONLY its block
        const size_t prefix = offsetof(WP, flowSampleCount);
        const bool prefixSame = std::memcmp(&on, &off, prefix) == 0;
        // OFF: the whole flow block is zero (what a Rev 11 struct would hold)
        WP zero{}; std::memset(&zero, 0, sizeof(WP)); new (&zero) WP();
        const bool offZero = std::memcmp((const char*)&off + prefix, (const char*)&zero + prefix,
                                         sizeof(WP) - prefix) == 0;
        const bool onLive = on.flowSampleCount == WP::kFlowSamples && on.flowLength == total &&
                            on.rockCount >= 6 && on.flowLut[0][0] > 0.0f;
        riverRapidsForce(0);
        const bool doorOffNoRocks = underRiverBoulders(uc, nullptr, 0) == 0;
        riverRapidsForce(1);
        std::snprintf(d, sizeof(d), "legacy prefix %zu B identical=%d; OFF block all-zero=%d; ON: %u samples over "
                      "%.0f m, %u rocks; OFF places no boulders=%d", prefix, prefixSame ? 1 : 0,
                      offZero ? 1 : 0, on.flowSampleCount, on.flowLength, on.rockCount, doorOffNoRocks ? 1 : 0);
        check(prefixSame && offZero && onLive && doorOffNoRocks,
              "R6 X3_RIVER_RAPIDS=0 leaves every Rev 11 byte untouched and the flow block at zero", d);
    }

    // ---- R7 advection is dt-scaled ----------------------------------------
    // Two steps of dt and 2*dt from the same time move the pattern by d and
    // 2*d (within the phase they share), and the two phases cross-fade so the
    // one that wraps has zero weight at the wrap.
    {
        float a0[2], b0[2], a1[2], b1[2], a2[2], b2[2], w0, w1, w2;
        const float v = 2.0f, t0 = 0.30f;
        riverFlowAdvect(t0,          v, 1.0f, 0.0f, a0, b0, w0);
        riverFlowAdvect(t0 + 0.05f,  v, 1.0f, 0.0f, a1, b1, w1);
        riverFlowAdvect(t0 + 0.10f,  v, 1.0f, 0.0f, a2, b2, w2);
        const float d1 = a1[0] - a0[0], d2 = a2[0] - a0[0];
        const bool proportional = std::fabs(d2 - 2.0f * d1) < 1e-4f && std::fabs(std::fabs(d1) - v * 0.05f) < 1e-4f;
        // at the wrap of phase A (time = k*kFlowCycle) its weight is 0
        float aw[2], bw[2], ww;
        riverFlowAdvect(kFlowCycle * 3.0f, v, 1.0f, 0.0f, aw, bw, ww);
        float ah[2], bh[2], wh;
        riverFlowAdvect(kFlowCycle * 3.5f, v, 1.0f, 0.0f, ah, bh, wh);
        std::snprintf(d, sizeof(d), "v=%.1f m/s: dt 0.05 -> %.3f m, dt 0.10 -> %.3f m; weight at A's wrap %.3f, at B's wrap %.3f",
                      v, std::fabs(d1), std::fabs(d2), ww, wh);
        check(proportional && ww < 1e-4f && wh > 0.999f,
              "R7 the advection moves speed*dt metres per step and each phase wraps at zero weight", d);
    }

    // ---- R8 the boulders sit on the bed and break the surface --------------
    {
        RiverBoulder rocks[WP::kMaxRocks];
        const uint32_t rn = underRiverBoulders(uc, rocks, WP::kMaxRocks);
        bool ok = rn >= 6;
        float minShow = 1e9f, maxSink = -1e9f, maxLat = 0.0f, minTurb = 1.0f;
        for (uint32_t i = 0; i < rn; ++i) {
            const RiverBoulder& r = rocks[i];
            const float w = worldWaterLevelAt(r.x, r.z);
            const float bed = terrainHeightAtWorld(r.x, r.z);
            const float crown = r.y + kBoulderSquash * r.radius, keel = r.y - kBoulderSquash * r.radius;
            minShow = std::min(minShow, crown - w);
            maxSink = std::max(maxSink, keel - bed);            // negative = buried
            maxLat  = std::max(maxLat, std::fabs(r.lat) + r.radius);
            minTurb = std::min(minTurb, underRiverFlowAt(uc, r.s).turbulence);
            if (!(r.radius > 1.0f && r.wakeLen >= 6.0f)) ok = false;
        }
        std::snprintf(d, sizeof(d), "%u boulders: crown >= %.2f m above the water, keel <= %.2f m vs the bed "
                      "(neg = buried), |lat|+r <= %.1f m (bed half-width %.1f), turbulence >= %.2f under every one",
                      rn, minShow, maxSink, maxLat, kURBedHalfW, minTurb);
        check(ok && minShow >= kBoulderShow - 0.05f && maxSink <= -0.15f && maxLat <= kURBedHalfW + 0.01f && minTurb >= 0.6f,
              "R8 every boulder stands on the carved bed, breaks the surface, and sits in fast water", d);
    }

    riverRapidsForce(savedForce);
    std::snprintf(d, sizeof(d), "[river-rapids] %d/%d passed", passN, passN + failN);
    if (failN) x3::logError(d); else x3::logInfo(d);
    return failN == 0;
}

} // namespace x3::game
