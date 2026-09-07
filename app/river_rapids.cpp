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
// Standing waves: amplitude per (turbulence * speed). turb 0.9 at 1.6 m/s
// -> 0.29 m crests; the gorge (0.81, 2.0 m/s) -> 0.32 m. (0.11 read as a
// swell, not a wave train, from eye height — lead's review of the v1 stills.)
constexpr float kSwAmpPerVel = 0.20f;
constexpr float kSwAmpMax    = 0.50f;
// Froude wavelength lambda = 2*pi*v^2/g is 2.6 m at 2.0 m/s; a real train
// in a boulder rapid runs longer than the pure relation (the bed sets it),
// so x1.6. The water grid is 480 m / 191 = 2.5 m — but with the flow on
// water.vert warps the grid toward the camera (0.75 m cells near the eye,
// where the waves are judged), so the displaced wavelength can go to 3 m.
constexpr float kSwLenScale = 1.6f;
constexpr float kSwLenMin = 3.0f;
constexpr float kSwLenMax = 10.0f;

// The boulders: (s, lateral, radius, crown above the water). Fast reaches
// only — a rock in a calm pool is a pool with a rock in it, not a rapid.
// Lateral within the wet bed (|lat| < kURBedHalfW - r) so every one sits on
// the flat channel floor. Two per rapid stand 1.9-2.3 m proud so the water
// visibly piles on the upstream face and splits (lead's review: 1.25 m rocks
// read as dark lumps on flat water).
struct BoulderSpec { float s, lat, r, show; };
const BoulderSpec kBoulders[] = {
    {  240.0f,  1.9f, 1.9f, 1.4f }, {  290.0f, -2.2f, 1.7f, 2.0f }, {  322.0f,  0.4f, 1.6f, 1.25f },   // the first step
    {  455.0f, -1.6f, 2.0f, 2.2f }, {  505.0f,  2.3f, 1.8f, 1.5f }, {  540.0f, -0.5f, 1.6f, 1.25f },   // the big step
    { 1670.0f,  1.4f, 1.9f, 1.6f }, { 1705.0f, -2.1f, 1.7f, 2.3f }, { 1750.0f,  2.2f, 1.8f, 1.9f },   // the gorge
    { 1785.0f, -0.7f, 1.6f, 1.25f },
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
    f.waveLen = std::clamp(kSwLenScale * 6.2831853f * f.speed * f.speed / 9.81f, kSwLenMin, kSwLenMax);
    return f;
}

// ---- THE WHITEWATER MIRRORS. Each of these is ONE expression in
// shaders/water.frag (swCrest / wwMask / wwLaceOne / wwThreshold / wwCover)
// and the same expression here; the gate runs them, the GPU draws them.
// Edit both or break --test-riverrapids.

// PAIRED water.vert/frag swCrest(): the primary standing-wave train. A sine
// crest-sharpened by pow(., 2.2) — narrow crests, broad troughs, the shape
// of a real haystack — with a slight downstream bow toward the banks
// (0.5*ln^2) and a slow breath in time.
float riverStandingWaveCrest(float s, float latN, float waveLen, float time) {
    const float k   = 6.28318530718f / std::max(waveLen, 2.0f);
    const float ph1 = k * s + 0.5f * latN * latN + 0.35f * std::sin(time * 1.7f + s * 0.05f);
    const float h   = 0.5f + 0.5f * std::sin(ph1);
    return 2.0f * std::pow(h, 2.2f) - 1.0f;
}

// PAIRED water.frag wwMask(): crest caps carry the foam (a full-turbulence
// cap reaches mask 1: a solid broken-white band across the channel), the
// outer quarter of the width and the boulders the rest; 0.30*turb is the
// sparse lace on the dark water between. (v4 had 0.40 / 0.55 with the bank
// term starting at 60% of the half-width: the foam spread evenly over the
// whole surface and the crest bands drowned in it — eyes-on
// rapid_gorge_downstream, v3/v4.)
float riverWhitewaterMask(float turb, float latN, float crest, float wake) {
    const float bank = smoothstepf(0.75f, 1.00f, std::fabs(latN));
    const float cap  = smoothstepf(0.15f, 0.85f, crest);
    return std::clamp(0.30f * turb + 1.00f * cap * turb + 0.35f * bank * turb + wake, 0.0f, 1.0f);
}

// PAIRED water.frag wwLaceOne(): q = (along / kLaceStretch, across). n1 is
// the 2 m patchiness (domain-warping the rest so nothing lines up), n2 the
// 0.6 m cells, n3 the 0.2 m lace (faded by `fine` with view distance in the
// shader, 1 here), st the across-flow streaks that run long along the flow.
static float laceOne(float qx, float qy, float fine) {
    const float n1 = std::sin(qx * 1.9f + qy * 1.3f) * std::cos(qx * 1.1f - qy * 2.4f);
    qy += 0.35f * n1;
    const float n2 = std::sin(qx * 7.3f + qy * 9.1f) * std::cos(qx * 4.7f - qy * 11.2f)
                   + 0.7f * std::sin(qx * 5.1f - qy * 14.3f + 1.7f) * std::sin(qx * 9.7f + qy * 6.9f);
    const float n3 = std::sin(qx * 23.0f + qy * 31.0f) * std::cos(qx * 17.0f - qy * 37.0f)
                   + 0.6f * std::sin(qx * 29.0f - qy * 19.0f + 0.7f) * std::sin(qx * 13.0f + qy * 41.0f);
    const float st = std::sin(qy * 6.3f + 0.9f * std::sin(qy * 2.1f + qx * 0.35f))
                   + 0.6f * std::sin(qy * 17.0f + 1.2f * std::sin(qy * 4.7f - qx * 0.5f));
    return 0.5f + 0.06f * n1 + 0.28f * n2 + 0.30f * fine * n3 + 0.14f * st;
}
float riverWhitewaterLace(const float q[2], float fine) { return laceOne(q[0], q[1], fine); }
// PAIRED water.frag wwThreshold()/wwCover(): the threshold falls with the
// mask (0.92 -> 0.22: a full crest cap or a bow pile is a solid raft with
// dark holes through it, the water between a few percent of flecks); the
// 0.07 edge is the bubble line, not a fade. The steep fall is deliberate —
// the mask decides WHERE, and a place is either foaming or it is not; the
// even 0.87 -> 0.34 of v3/v4 made every square metre of a rapid half foam
// and the eye could find no crest, no wake, no dark water.
// The trailing smoothstep is "no mask, no foam": the lace peaks reach 1.5,
// so without it still water would grow a few flecks (R9 wants calm == 0).
float riverWhitewaterThreshold(float mask) { return 0.92f - 0.70f * mask; }
float riverWhitewaterCover(float mask, float lace) {
    const float th = riverWhitewaterThreshold(mask);
    return smoothstepf(th, th + 0.07f, lace) * smoothstepf(0.0f, 0.05f, mask);
}
// PAIRED water.frag wwCoverBlend(): threshold each phase, fade each hard
// about its own half-weight (0.4..0.6 of a 1.2 s ramp = ~0.25 s), keep the
// brighter. A raft therefore forms and dissolves in a quarter second — which
// is what foam in a rapid does — instead of haunting the frame as the grey
// ghost of the phase on its way out (v3); a max, not a sum, so a region
// both phases cover is white once and the coverage does not pulse.
float riverWhitewaterCoverBlend(float mask, float laceA, float laceB, float wA) {
    const float a = riverWhitewaterCover(mask, laceA), b = riverWhitewaterCover(mask, laceB);
    return std::max(a * smoothstepf(0.4f, 0.6f, wA), b * smoothstepf(0.4f, 0.6f, 1.0f - wA));
}

float riverFoamBaseAt(const UnderRiverChain& uc, float s, float lat) {
    if (uc.n < 2) return 0.0f;
    const RiverFlowSample f = underRiverFlowAt(uc, s);
    return riverWhitewaterMask(f.turbulence, lat / std::max(f.halfWidth, 1.0f), 1.0f, 0.0f);
}

float riverWhitewaterCoverage(const UnderRiverChain& uc, float s, float time) {
    if (uc.n < 2) return 0.0f;
    const RiverFlowSample f = underRiverFlowAt(uc, s);
    const float hw = std::max(f.halfWidth, 1.0f);
    const float L  = 3.0f * std::max(f.waveLen, 3.0f);
    // The flow frame IS (along, across) here — the mirror does not need the
    // world rotation the shader does through dir/per; advection is along.
    float offA[2], offB[2], wA;
    riverFlowAdvect(time, f.speed, 1.0f, 0.0f, offA, offB, wA);
    double sum = 0.0; int n = 0;
    for (float a = 0.0f; a < L; a += 0.1f) {
        for (float c = -0.6f * hw; c <= 0.6f * hw; c += 0.1f) {
            const float crest = f.waveAmp > 0.0005f ? riverStandingWaveCrest(s + a, c / hw, f.waveLen, time) : 0.0f;
            const float mask  = riverWhitewaterMask(f.turbulence, c / hw, crest, 0.0f);
            const float qA[2] = { (a + offA[0]) / kLaceStretch, c + offA[1] };
            const float qB[2] = { (a + offB[0]) / kLaceStretch, c + offB[1] };
            sum += riverWhitewaterCoverBlend(mask, riverWhitewaterLace(qA, 1.0f),
                                             riverWhitewaterLace(qB, 1.0f), wA); ++n;
        }
    }
    return n ? (float)(sum / n) : 0.0f;
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
        r.show = std::max(b.show, kBoulderShow);
        const float need = (c.w - bed + r.show + 0.30f) / (2.0f * kBoulderSquash);
        r.radius = std::max(b.r, need);
        const float latMax = std::max(kURBedHalfW - r.radius, 0.0f);
        if (std::fabs(r.lat) > latMax) {
            r.lat = (r.lat < 0.0f ? -latMax : latMax);
            r.x = uc.x[c.i] + dx * (b.s - uc.cum[c.i]) + px * r.lat;
            r.z = uc.z[c.i] + dz * (b.s - uc.cum[c.i]) + pz * r.lat;
        }
        r.y = c.w + r.show - kBoulderSquash * r.radius;
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

    // ---- R9 foam COVERAGE: whitewater, not milk --------------------------
    // The mean cover over a Rapid's centre (three wavelengths x the middle
    // 60% of the width) must land in [0.25, 0.50]: a Class III-IV rapid is
    // dark water with white lace and crest caps, not a white sheet (the v1
    // stills were ~70% and read as paint marbling). Calm must be exactly 0,
    // and a riffle well below a rapid. Three times: one near each phase's
    // full weight and one a quarter-cycle in (wA = 0.25, t = 0.3) with the
    // other phase mid-ramp. NOT the exact hand-over instant (t = 0.6): there
    // both phases sit at half brightness for ~0.1 s while the rafts
    // dissolve and re-form — by design (riverWhitewaterCoverBlend), and
    // the mean cover reads half for that instant.
    {
        float rapidMin = 1.0f, rapidMax = 0.0f, riffleMax = 0.0f, calmMax = 0.0f;
        for (uint32_t k = 0; k < kReachCount; ++k) {
            const RiverReach& r = kReaches[k];
            const float s1 = r.s1 <= 0.0f ? total : r.s1;
            const float mid = 0.5f * (r.s0 + s1);
            if (s1 - r.s0 < 2.0f * kRiverReachEdge + 10.0f) continue;
            static const float kTimes[3] = { 1.1f, 7.3f, 0.3f };
            for (int ti = 0; ti < 3; ++ti) {
                const float cov = riverWhitewaterCoverage(uc, mid, kTimes[ti]);
                if (r.kind == RiverReachKind::Calm) calmMax = std::max(calmMax, cov);
                else if (r.kind == RiverReachKind::Riffle) riffleMax = std::max(riffleMax, cov);
                else { rapidMin = std::min(rapidMin, cov); rapidMax = std::max(rapidMax, cov); }
            }
        }
        std::snprintf(d, sizeof(d), "rapid/plunge coverage %.2f..%.2f (want 0.25..0.50), riffle max %.2f, calm max %.3f",
                      rapidMin, rapidMax, riffleMax, calmMax);
        check(rapidMin >= 0.25f && rapidMax <= 0.50f && riffleMax < rapidMin && calmMax == 0.0f,
              "R9 a rapid is 25-50% foam at its centre, a riffle less, calm water none", d);
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
