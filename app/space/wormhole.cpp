// app/space/wormhole.cpp — see app/space/wormhole.h for the design rationale
// and the full ledger of what was LIFTED from app/rifthub.* versus built new.
//
// THE FOUR THINGS TAKEN STRAIGHT FROM THE RIFT HUB'S PORTAL SYSTEM
// ----------------------------------------------------------------
//  1. TWO CLOCKS OFF ONE TIMER. rifthub.cpp:2335-2349 runs its kawoosh surge on
//     an EXPONENTIAL envelope for brightness and a LINEAR one for animation
//     position, because driving the animation off the exponential stalls the
//     film on its last frames. Every phase here does the same: `phaseFrac()` is
//     linear (shape), the intensity curves are shaped (light).
//  2. THE EMISSIVE CAP LAW (rifthub.cpp:2329 `capped`, caps at :449-501). Every
//     emissive strength written below goes through `capped()`. Every colour
//     change is a LERP BETWEEN TWO NAMED COLOURS — never an additive lift of all
//     three channels, which is precisely how you get a flat white disc after
//     ACES. The instability read slides the hue toward violet; it does not
//     brighten toward white.
//  3. THE SEAM-FREE POLAR NOISE EMBEDDING (rifthub.cpp:854-889, makeThroatRGBA).
//     You cannot feed atan2 into 2D noise — the -pi/+pi wrap leaves a visible
//     radial seam. Embedding the angle on the unit circle
//     (sx = cos(ang)*k, sy = sin(ang)*k) and sampling THERE is continuous. That
//     one line is the most reusable thing in the hub and it is used here.
//  4. THE PER-GATE POINT LIGHT (rifthub.cpp:2406-2430). The gate is the key
//     light of its bay. Kept as the headline feature. The hub's NUMBERS do not
//     transfer — it lights a 40 m enclosed hall from 2-3 m away, where
//     `1/(d^2+1)` attenuation demands intensity in the tens. In open space the
//     hull is 200-600 m out with no bounce at all, so the reasoning transfers
//     and the constants are re-derived from that same attenuation law below.
//
// AND THE ONE PIECE OF HUB DOCTRINE DELIBERATELY NOT FOLLOWED
// -----------------------------------------------------------
// The hub deleted every hot-centre burst term after two rounds of art review
// (rifthub.cpp:843-853 — "the fake dot is gone"). That verdict was about
// painting a white blob into the middle of a FLAT DISC you stand next to, where
// a centre dot has nothing to be the centre OF and reads as a decal. The
// Bajoran read needs a convergence, so this effect keeps one — but it is not a
// painted dot: it is a REAL SMALL DISC at the FAR END of the receding layer
// stack, at kThroatLayers deep. It is a place in the geometry, so it parallaxes
// with the camera and occludes correctly. That is the difference between a
// convergence and a sticker, and it is why the layered throat had to exist.

#include "wormhole.h"

#include "../ship_comms.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x3::space {

namespace {

constexpr float kPi  = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// THE CAP (rifthub.cpp:2329). Every emissive strength in this file goes through it.
inline float capped(float v, float cap) { return v > cap ? cap : (v < 0.0f ? 0.0f : v); }

inline float fract1(float x) { return x - std::floor(x); }

// ---- Deterministic noise toolkit -----------------------------------------
// Local copies rather than a dependency on mesh_prims' detail:: namespace, so
// this TU bakes without pulling the prim library's lattice conventions in.
// Same shape as rifthub's (value noise -> fbm -> ridged), same lacunarity
// discipline (non-integer, so nothing lines up on the axes).

inline float hash01(uint32_t x, uint32_t y, uint32_t salt) {
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ salt * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}

float valueNoise(float x, float y, uint32_t salt) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - (float)xi, fy = y - (float)yi;
    auto lat = [&](int ix, int iy) {
        return hash01((uint32_t)(ix & 1023), (uint32_t)(iy & 1023), salt);
    };
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = lat(xi, yi),     b = lat(xi + 1, yi);
    const float c = lat(xi, yi + 1), d = lat(xi + 1, yi + 1);
    const float ab = a + (b - a) * sx;
    const float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}

float fbm(float x, float y, int octaves, uint32_t salt) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x * freq, y * freq, salt + (uint32_t)o * 131u);
        amp *= 0.5f; freq *= 2.02f;
    }
    return sum;
}

// RIDGED fbm: 1-|2n-1| squared per octave. Sharp bright creases on a dark
// field = the filamentary structure the brief asks for.
float ridged(float x, float y, int octaves, uint32_t salt) {
    float sum = 0.0f, amp = 0.55f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        float n = valueNoise(x * freq, y * freq, salt + (uint32_t)o * 977u);
        n = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += amp * n * n;
        amp *= 0.55f; freq *= 2.15f;
    }
    return sum;
}

// ---- Deterministic, framerate-independent waver ---------------------------
// THE 165 Hz LAW. This is a function of ACCUMULATED TIME ONLY — no rand(), no
// frame counter, no per-frame state. A 60 Hz run and a 165 Hz run that reach
// t = 2.0 s sample the identical value, which is what makes the equivalence
// test in runWormholeFieldSelfTest() pass rather than merely be asserted.
// Three incommensurable sines beat against each other so the wander never
// repeats on a human timescale.
float waver(float t, uint32_t salt) {
    const float a = std::sin(t * 5.13f + (float)(salt % 17u) * 0.61f);
    const float b = std::sin(t * 11.71f + (float)(salt % 29u) * 1.31f);
    const float c = std::sin(t * 2.37f + (float)(salt % 41u) * 2.11f);
    return 0.48f * a + 0.32f * b + 0.20f * c;   // in [-1, 1]
}

// ---- Geometry -------------------------------------------------------------
struct Prim {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

// Inner radius of the shared annulus, as a fraction of the outer radius. The
// throat layers are scaled copies of this one ring, so the hole in the middle
// is what lets you SEE the next layer down — this constant is the parallax.
// THE LAYERS TILE; THEY DO NOT STACK. This was 0.62 for three capture rounds
// and that is what produced the "purple donut": at 0.62 the NEAREST ring is an
// opaque plate covering 62%-100% of the radius, drawn last, painting its single
// flat mouth-tint over every deeper layer underneath it. The interior could only
// show through inside r<0.62, which is exactly the blue ball that kept appearing
// inside a violet plate.
//
// At 0.88 each ring is a NARROW BAND roughly 3x the radial spacing between
// consecutive layers: wide enough that they overlap with no seams, narrow enough
// that each one occupies its own annulus. The 30 rings then TILE the disc from
// the rim down to the convergence and the whole aperture becomes one continuous
// radial gradient — violet fringe at the grazing edge, deep blue through the
// throat, white-hot at the convergence — instead of a plate plus a ball.
constexpr float kRingInner = 0.88f;

// The RIM is a separate, THIN ring. Reusing the throat annulus for it (38% of
// the radius wide) painted a second big disc over the mouth instead of an edge —
// which is most of what made the first capture read as a flat lilac plate.
constexpr float kRimInner = 0.93f;

// A DOUBLE-WOUND unit annulus in the local XY plane (normal +Z), inner radius
// kRingInner, outer 1.0. Double winding is the hub's law (rifthub.cpp:734-750):
// one entity that reads correctly from both sides, so flying THROUGH the throat
// does not black the far side out.
// UV: u = angle around the ring [0,1], v = radial position [0,1] (0 inner).
Prim makeAnnulus(uint32_t segs, float innerR) {
    Prim m;
    segs = std::max<uint32_t>(segs, 8u);
    innerR = clampf(innerR, 0.0f, 0.99f);
    m.verts.reserve((size_t)(segs + 1) * 2);
    m.index.reserve((size_t)segs * 12);
    for (uint32_t s = 0; s <= segs; ++s) {
        const float u  = (float)s / (float)segs;
        const float th = u * kTau;
        const float c = std::cos(th), sn = std::sin(th);
        rhi::MeshVertex vi{};   // inner
        vi.pos[0] = c * innerR; vi.pos[1] = sn * innerR; vi.pos[2] = 0.0f;
        vi.normal[2] = 1.0f; vi.uv[0] = u; vi.uv[1] = 0.0f;
        rhi::MeshVertex vo{};   // outer
        vo.pos[0] = c; vo.pos[1] = sn; vo.pos[2] = 0.0f;
        vo.normal[2] = 1.0f; vo.uv[0] = u; vo.uv[1] = 1.0f;
        m.verts.push_back(vi);
        m.verts.push_back(vo);
    }
    for (uint32_t s = 0; s < segs; ++s) {
        const uint32_t i0 = s * 2, o0 = s * 2 + 1, i1 = s * 2 + 2, o1 = s * 2 + 3;
        m.index.insert(m.index.end(), { i0, o0, o1 });   // front
        m.index.insert(m.index.end(), { i0, o1, i1 });
        m.index.insert(m.index.end(), { i0, o1, o0 });   // back (reversed winding)
        m.index.insert(m.index.end(), { i0, i1, o1 });
    }
    return m;
}

// A DOUBLE-WOUND unit disc (triangle fan), normal +Z. UV inscribes the disc in
// [0,1]^2 centred at (0.5, 0.5) — the core bake is radial, so it reads the
// distance from that centre.
Prim makeDisc(uint32_t segs) {
    Prim m;
    segs = std::max<uint32_t>(segs, 8u);
    rhi::MeshVertex ctr{};
    ctr.normal[2] = 1.0f; ctr.uv[0] = 0.5f; ctr.uv[1] = 0.5f;
    m.verts.push_back(ctr);
    for (uint32_t s = 0; s <= segs; ++s) {
        const float th = (float)s * (kTau / (float)segs);
        const float c = std::cos(th), sn = std::sin(th);
        rhi::MeshVertex v{};
        v.pos[0] = c; v.pos[1] = sn; v.pos[2] = 0.0f;
        v.normal[2] = 1.0f;
        v.uv[0] = 0.5f + 0.5f * c; v.uv[1] = 0.5f - 0.5f * sn;
        m.verts.push_back(v);
    }
    for (uint32_t s = 0; s < segs; ++s) {
        const uint32_t a = 1 + s, b = 2 + s;
        m.index.insert(m.index.end(), { 0u, a, b });
        m.index.insert(m.index.end(), { 0u, b, a });
    }
    return m;
}

// ---- The bakes ------------------------------------------------------------

// THROAT map. u = angle around the ring, v = radial position across the annulus
// (0 = inner edge, 1 = outer edge).
//
// The structure is the hub's throat recipe (rifthub.cpp:854-889) re-aimed at an
// annulus rather than a disc:
//   * spiral twist on the angle BEFORE embedding, so the filaments curve into
//     the throat rather than radiating flat;
//   * the SEAM-FREE UNIT-CIRCLE EMBEDDING of that angle (the transferable trick);
//   * ridged noise = the filaments, fbm = the low-frequency body/cloud;
//   * threshold + gain to turn soft noise into DISTINCT tendrils instead of haze;
//   * a radial envelope that is brightest at the INNER edge — because on an
//     annulus the inner edge is the one facing down the throat, so the stack of
//     layers builds a continuous brightness gradient toward the convergence.
std::vector<uint8_t> bakeThroatRGBA(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : (int)v)); };
    for (uint32_t y = 0; y < h; ++y) {
        const float v = ((float)y + 0.5f) / (float)h;       // radial across the ring
        for (uint32_t x = 0; x < w; ++x) {
            const float u = ((float)x + 0.5f) / (float)w;   // angle around the ring
            float ang = u * kTau;
            ang += v * 3.4f;                                 // spiral twist
            // THE SEAM-FREE EMBEDDING (rifthub.cpp:862-864).
            const float sx = std::cos(ang) * 5.0f;
            const float sy = std::sin(ang) * 5.0f;
            // ANISOTROPY IS WHAT MAKES IT A TUNNEL. The radial coordinate is fed
            // in only WEAKLY (0.30, not 1.7), so a filament stays coherent as v
            // sweeps outward: the structure becomes SPOKES running radially in
            // toward the convergence, curved by the spiral twist above.
            // With the radial coordinate at full strength the filaments decorrelate
            // ring-by-ring and the result is concentric banding, which the eye
            // reads as a convex DOME rather than a throat you could fall into.
            // That is the single difference between the two reads.
            const float fil  = ridged(sx + v * 0.30f + 5.1f, sy + v * 0.30f + 8.7f, 5, 0x09E4u);
            const float body = fbm(sx * 0.5f + 1.3f, sy * 0.5f + v * 0.9f + 6.6f, 4, 0x7EAAu);
            // Threshold + gain: distinct tendrils, not grey haze.
            // Threshold + gain, then SQUARED: the extra sharpening is what keeps
            // the filaments distinct tendrils on a near-black field instead of a
            // grey haze. The multiply by an HDR baseColorFactor amplifies whatever
            // contrast the bake has, so the bake has to HAVE contrast.
            float t = fil - 0.44f;
            if (t < 0.0f) t = 0.0f;
            t *= 2.4f; if (t > 1.0f) t = 1.0f;
            t = t * t;
            // Brightest at the INNER edge (v -> 0), alive to the outer rim.
            const float env = 0.26f + 0.74f * (1.0f - v) * (1.0f - v);
            // Deep blue body -> white-blue filaments. The BODY is deliberately
            // dark: it is multiplied by an HDR baseColorFactor downstream, so a
            // bright body becomes a washed plate while a dark one lets only the
            // filaments reach bloom. First capture had the body at B=104..222 and
            // read as a flat pearl; this is a quarter of that.
            const float baseR =  3.0f +  13.0f * body;
            const float baseG = 10.0f +  33.0f * body;
            const float baseB = 34.0f +  74.0f * body;
            const float filR = 208.0f, filG = 234.0f, filB = 255.0f;
            const float R = (baseR + (filR - baseR) * t) * env;
            const float G = (baseG + (filG - baseG) * t) * env;
            const float B = (baseB + (filB - baseB) * t) * env;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = clamp8(R); p[1] = clamp8(G); p[2] = clamp8(B); p[3] = 255;
        }
    }
    return px;
}

// CORE map: a radial white-hot falloff with a little filament structure so the
// convergence is not a clean airbrushed dot. Used for the Spark point, the
// Bloom flare and the disc that caps the far end of the layer stack.
std::vector<uint8_t> bakeCoreRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : (int)v)); };
    for (uint32_t y = 0; y < n; ++y) {
        const float fy = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f;
        for (uint32_t x = 0; x < n; ++x) {
            const float fx = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            // Soft gaussian-ish core with a long shoulder so bloom has something
            // to grab beyond the hard edge.
            float core = std::exp(-r * r * 3.4f);
            // A touch of filament so it is not a clean airbrush dot.
            float ang = std::atan2(fy, fx) + r * 1.4f;
            const float fil = ridged(std::cos(ang) * 2.6f + 3.3f,
                                     std::sin(ang) * 2.6f + 1.7f, 3, 0x4C0Eu);
            core *= 0.80f + 0.42f * fil;
            const float edge = clampf(1.0f - r, 0.0f, 1.0f);
            core *= edge * edge;
            const float R = 236.0f * core;
            const float G = 246.0f * core;
            const float B = 255.0f * core;
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            p[0] = clamp8(R); p[1] = clamp8(G); p[2] = clamp8(B); p[3] = 255;
        }
    }
    return px;
}

// Compose a column-major 4x4 from an orthonormal basis + origin + uniform scale.
void composeXform(float m[16], const float cx[3], const float cy[3], const float cz[3],
                  const float t[3], float sx, float sy, float sz) {
    m[0] = cx[0] * sx; m[1] = cx[1] * sx; m[2] = cx[2] * sx; m[3] = 0.0f;
    m[4] = cy[0] * sy; m[5] = cy[1] * sy; m[6] = cy[2] * sy; m[7] = 0.0f;
    m[8] = cz[0] * sz; m[9] = cz[1] * sz; m[10] = cz[2] * sz; m[11] = 0.0f;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1.0f;
}

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    else { v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f; }
}

void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

// Build an orthonormal basis whose Z column is `axis`.
void basisFromAxis(const float axis[3], float outX[3], float outY[3], float outZ[3]) {
    outZ[0] = axis[0]; outZ[1] = axis[1]; outZ[2] = axis[2];
    normalize3(outZ);
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (std::fabs(outZ[1]) > 0.94f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }
    cross3(up, outZ, outX); normalize3(outX);
    cross3(outZ, outX, outY); normalize3(outY);
}

// ---- The palette knobs, re-derived for OPEN SPACE -------------------------
//
// The hub's numbers (base 11, range 9.5) are for a light 2-3 m off gunmetal in
// an enclosed hall. Here the nearest thing the wormhole must light is the
// player's hull at 150-600 m with NO bounce whatsoever. mesh.frag's point
// attenuation is 1/(d^2+1) against an energy-conserving diffuse lobe, so
// landing a VISIBLE wash at 300 m needs the intensity to carry the d^2: the
// hub's reasoning, three orders of magnitude of distance later.
constexpr float kLightRangeMul   = 26.0f;    // range = radius * this  (~680 m at r=26)
constexpr float kLightBaseInt    = 4200.0f;  // held-open intensity multiplier
constexpr float kLightBloomInt   = 9000.0f;  // the Bloom flare peak — the event
constexpr float kLightBreatheHz  = 0.42f;    // slow hum-synced breathe when held
constexpr float kLightBreatheAmp = 0.16f;    // +/- fraction around base

// The instability tint target. The hue SLIDES here; nothing is lifted.
constexpr float kUnstableTint[3] = { 0.94f, 0.30f, 1.00f };   // hot violet-magenta

} // namespace

// ---------------------------------------------------------------------------
const char* wormholePhaseName(WormholePhase p) {
    switch (p) {
        case WormholePhase::Dormant: return "DORMANT";
        case WormholePhase::Spark:   return "SPARK";
        case WormholePhase::Bloom:   return "BLOOM";
        case WormholePhase::Unfurl:  return "UNFURL";
        case WormholePhase::Held:    return "HELD";
        case WormholePhase::Closing: return "CLOSING";
    }
    return "?";
}

// ===========================================================================
// Wormhole — the phase machine
// ===========================================================================

void Wormhole::configure(const char* name, const float pos[3], const float axis[3],
                         bool stable, int id, const WormholeTuning& t) {
    std::snprintf(m_name, sizeof(m_name), "%s", name ? name : "WORMHOLE");
    if (pos) { m_pos[0] = pos[0]; m_pos[1] = pos[1]; m_pos[2] = pos[2]; }
    if (axis) { m_axis[0] = axis[0]; m_axis[1] = axis[1]; m_axis[2] = axis[2]; }
    normalize3(m_axis);
    m_stable = stable;
    m_id     = id;
    m_tuning = t;
    // Guard the durations so a zero-length phase cannot spin the drain loop.
    if (!(m_tuning.sparkSec  > 0.0f)) m_tuning.sparkSec  = 0.01f;
    if (!(m_tuning.bloomSec  > 0.0f)) m_tuning.bloomSec  = 0.01f;
    if (!(m_tuning.unfurlSec > 0.0f)) m_tuning.unfurlSec = 0.01f;
    if (!(m_tuning.closeSec  > 0.0f)) m_tuning.closeSec  = 0.01f;
    if (!(m_tuning.radius    > 0.0f)) m_tuning.radius    = 1.0f;
    if (!(m_tuning.throatDepth > 0.0f)) m_tuning.throatDepth = 1.0f;
}

float Wormhole::phaseDuration(WormholePhase p) const {
    switch (p) {
        case WormholePhase::Spark:   return m_tuning.sparkSec;
        case WormholePhase::Bloom:   return m_tuning.bloomSec;
        case WormholePhase::Unfurl:  return m_tuning.unfurlSec;
        case WormholePhase::Held:    return m_tuning.heldSec;   // < 0 => forever
        case WormholePhase::Closing: return m_tuning.closeSec;
        default: return -1.0f;                                   // Dormant: forever
    }
}

void Wormhole::enter(WormholePhase p) { m_phase = p; m_phaseT = 0.0f; }

void Wormhole::open() {
    if (m_phase == WormholePhase::Dormant) enter(WormholePhase::Spark);
}

void Wormhole::close() {
    if (m_phase == WormholePhase::Dormant || m_phase == WormholePhase::Closing) return;
    enter(WormholePhase::Closing);
}

void Wormhole::forceHeld() { enter(WormholePhase::Held); }

// THE DT LAW. dt is DRAINED across phase boundaries: a 100 ms hitch that spans
// the end of Spark and half of Bloom lands at exactly the Bloom time a 165 Hz
// run reaches at the same wall clock, because the leftover is carried, not
// dropped. This is the whole reason the 60/165 Hz equivalence test can pass on
// an exact comparison rather than a loose one.
void Wormhole::update(float dt) {
    if (!(dt > 0.0f)) return;
    if (m_phase != WormholePhase::Dormant) m_time += dt;

    float remaining = dt;
    // Bounded: each iteration either consumes all the remaining dt or advances
    // one phase, and the phase chain is finite (Closing -> Dormant, which has
    // infinite duration), so this cannot spin.
    for (int guard = 0; guard < 16 && remaining > 0.0f; ++guard) {
        const float dur = phaseDuration(m_phase);
        if (dur < 0.0f) {                     // this phase holds forever
            m_phaseT += remaining;
            remaining = 0.0f;
            break;
        }
        const float room = dur - m_phaseT;
        if (remaining < room) {
            m_phaseT += remaining;
            remaining = 0.0f;
            break;
        }
        // Consume the rest of this phase and carry the remainder forward.
        remaining -= room;
        switch (m_phase) {
            case WormholePhase::Spark:   enter(WormholePhase::Bloom);   break;
            case WormholePhase::Bloom:   enter(WormholePhase::Unfurl);  break;
            case WormholePhase::Unfurl:  enter(WormholePhase::Held);    break;
            case WormholePhase::Held:    enter(WormholePhase::Closing); break;
            case WormholePhase::Closing: enter(WormholePhase::Dormant); m_time = 0.0f; break;
            default:                     remaining = 0.0f;              break;
        }
    }
}

float Wormhole::instability() const {
    if (m_stable) return 0.0f;
    // A live wavering signal in [0,1], deterministic in accumulated time.
    // Biased to sit around 0.45 so an unstable hole is visibly restless even at
    // the quiet part of the beat, and spikes hard at the crests.
    const float w = waver(m_time, (uint32_t)(m_id < 0 ? 0 : m_id) * 7u + 3u);
    return clampf(0.45f + 0.55f * w, 0.0f, 1.0f);
}

float Wormhole::aperture() const {
    float a = 0.0f;
    switch (m_phase) {
        case WormholePhase::Dormant:
        case WormholePhase::Spark:
            return 0.0f;                       // no throat yet — just a point
        case WormholePhase::Bloom: {
            // The flare is bright but the aperture has barely cracked: a hint of
            // a ring inside the glare, so Unfurl has something to grow FROM.
            const float u = clampf(m_phaseT / m_tuning.bloomSec, 0.0f, 1.0f);
            a = 0.16f * u * u;
            break;
        }
        case WormholePhase::Unfurl: {
            // Ease-out with a small overshoot past 1.0 and a settle back — the
            // aperture snaps open and rings, which is what sells "it OPENED"
            // rather than "it faded up".
            const float u = clampf(m_phaseT / m_tuning.unfurlSec, 0.0f, 1.0f);
            const float e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
            const float ring = 0.09f * std::sin(u * kPi * 2.4f) * (1.0f - u);
            a = 0.16f + 0.84f * e + ring;
            break;
        }
        case WormholePhase::Held:
            a = 1.0f;
            break;
        case WormholePhase::Closing: {
            const float u = clampf(m_phaseT / m_tuning.closeSec, 0.0f, 1.0f);
            a = (1.0f - u) * (1.0f - u);       // collapses fast, tails off
            break;
        }
    }
    // Breathing. A STABLE hole breathes ~2% and reads as steady and inviting.
    // An UNSTABLE one beats against itself at up to ~14% and reads as dangerous
    // — the aperture literally will not hold still.
    const float amp = m_stable ? 0.02f : 0.14f;
    const float w = waver(m_time * (m_stable ? 0.55f : 1.0f),
                          (uint32_t)(m_id < 0 ? 0 : m_id) * 13u + 11u);
    a *= 1.0f + amp * w;
    return clampf(a, 0.0f, 1.35f);
}

float Wormhole::coreIntensity() const {
    float k = 0.0f;
    switch (m_phase) {
        case WormholePhase::Dormant: return 0.0f;
        case WormholePhase::Spark: {
            // A point of light kindling. Rises steeply — it is small, so it has
            // to be bright to register at all at range.
            const float u = clampf(m_phaseT / m_tuning.sparkSec, 0.0f, 1.0f);
            k = 0.18f + 2.4f * u * u;
            break;
        }
        case WormholePhase::Bloom: {
            // THE EVENT. Hub doctrine (rifthub.cpp:2335-2349): fast attack, then
            // an EXPONENTIAL decay for brightness while the SHAPE runs linear.
            const float u = clampf(m_phaseT / m_tuning.bloomSec, 0.0f, 1.0f);
            const float attack = clampf(u / 0.18f, 0.0f, 1.0f);
            k = kCoreEmissiveCap * attack * std::exp(-2.6f * u);
            break;
        }
        case WormholePhase::Unfurl: {
            // Settling out of the flare into the held value as the throat opens.
            const float u = clampf(m_phaseT / m_tuning.unfurlSec, 0.0f, 1.0f);
            k = 5.6f + (3.4f - 5.6f) * u;
            break;
        }
        case WormholePhase::Held:
            k = 3.4f;
            break;
        case WormholePhase::Closing: {
            // A last flare as it pinches shut, then out.
            const float u = clampf(m_phaseT / m_tuning.closeSec, 0.0f, 1.0f);
            k = 3.4f * (1.0f - u) + 3.0f * std::sin(u * kPi) * (1.0f - u);
            break;
        }
    }
    // Live breathe / dropout. Stable = a gentle hum. Unstable = real dropouts,
    // down to ~40% — the light stutters on the hull, which is the tell.
    const float w = waver(m_time * 1.3f, (uint32_t)(m_id < 0 ? 0 : m_id) * 19u + 5u);
    k *= m_stable ? (1.0f + 0.07f * w) : (1.0f - 0.30f + 0.30f * w * 0.5f + 0.30f);
    if (!m_stable) {
        // A hard, brief dropout at the crests — the flicker.
        const float d = waver(m_time * 2.1f, (uint32_t)(m_id < 0 ? 0 : m_id) * 23u + 9u);
        if (d > 0.62f) k *= 0.42f;
    }
    return capped(k, kCoreEmissiveCap);
}

float Wormhole::layerIntensity(int layer) const {
    if (m_phase == WormholePhase::Dormant || m_phase == WormholePhase::Spark) return 0.0f;
    const float ap = aperture();
    if (ap <= 0.001f) return 0.0f;
    const int L = std::max(1, kThroatLayers - 1);
    const float d = clampf((float)layer / (float)L, 0.0f, 1.0f);  // 0 = mouth, 1 = deep

    // Deeper layers are HOTTER: the stack builds a gradient toward the
    // convergence, which is what makes a set of flat rings read as a tunnel
    // with a bright far end rather than as a pile of stickers.
    //
    // The mouth end is driven MUCH darker than the obvious 0.85 baseline,
    // because these annuli sit only metres from the wormhole's own point light
    // and get its full inverse-square blast. At the first tuning they rendered
    // as a solid violet plate with no filament structure at all, regardless of
    // emissive, because the LIT term was doing it. Near-black albedo at the
    // mouth is what lets the throat read as an opening rather than a disc.
    float k = 0.62f + 3.2f * d * d;

    // Rolling internal motion: a wave travelling DOWN the throat. Each layer is
    // phase-offset by its depth, so the brightness crest runs away from the
    // camera and the throat reads as flowing inward.
    const float flow = std::sin(m_time * 2.3f - d * 7.4f
                                + (float)(m_id < 0 ? 0 : m_id) * 0.7f);
    k *= 0.78f + 0.34f * flow;

    // The aperture gates the whole stack, so the throat lights up AS it opens.
    k *= ap;

    // Instability chews at the deep layers hardest — the far end of an unstable
    // throat is where the collapse would start.
    if (!m_stable) {
        const float w = waver(m_time * 1.7f + d * 3.1f,
                              (uint32_t)(m_id < 0 ? 0 : m_id) * 31u + (uint32_t)layer);
        k *= clampf(1.0f + (0.20f + 0.45f * d) * w, 0.15f, 1.6f);
    }
    return capped(k, kLayerEmissiveCap);
}

void Wormhole::layerTint(int layer, float outRgb[3]) const {
    const int L = std::max(1, kThroatLayers - 1);
    const float d = clampf((float)layer / (float)L, 0.0f, 1.0f);

    // SPECTRAL FRINGING. The ramp runs violet fringe at the mouth -> deep blue
    // through the mid throat -> blue-white at the convergence. Because each
    // layer is real geometry at a real depth, this is a chromatic gradient
    // through actual space, not a painted vignette: the fringe sits where the
    // grazing edge is and the white sits at the far end.
    const float* fringe = m_tuning.fringeColor;
    const float* wall   = m_tuning.wallColor;
    const float* core   = m_tuning.coreColor;
    // The fringe is a NARROW band at the very mouth. Given half the throat it
    // reads as a purple donut, which is a different effect from the one wanted:
    // the Bajoran throat is overwhelmingly blue-white and only FRINGES at the
    // grazing edge. 0.15 is the width that keeps the spectrum without letting it
    // own the shot.
    constexpr float kFringeBand = 0.15f;
    for (int c = 0; c < 3; ++c) {
        float v;
        if (d < kFringeBand) {
            const float u = d / kFringeBand;
            v = fringe[c] + (wall[c] - fringe[c]) * u;
        } else {
            const float u = (d - kFringeBand) / (1.0f - kFringeBand);
            v = wall[c] + (core[c] - wall[c]) * u;
        }
        outRgb[c] = v;
    }

    // INSTABILITY SLIDES THE HUE — the hub's law, restated (rifthub.cpp:2597-2604:
    // "we SHIFT the hue, we never lift all three channels; that is how you get
    // white"). An unstable throat goes violet-magenta and WRONG, not brighter.
    const float ins = instability();
    if (ins > 0.001f) {
        for (int c = 0; c < 3; ++c)
            outRgb[c] += (kUnstableTint[c] - outRgb[c]) * ins * 0.75f;
    }
}

int Wormhole::collectLights(rhi::PointLight* out, int maxOut) const {
    if (!out || maxOut <= 0) return 0;
    if (m_phase == WormholePhase::Dormant) return 0;

    const float core = coreIntensity();
    if (core <= 0.001f) return 0;

    // THE LIGHT SITS SLIGHTLY IN FRONT OF THE MOUTH, on the -axis side, so the
    // spill lands on whatever is approaching rather than being buried in the
    // throat geometry. This is the difference between "the effect is bright"
    // and "the effect lights the world".
    rhi::PointLight L;
    const float standoff = m_tuning.radius * 0.35f;
    L.pos[0] = m_pos[0] - m_axis[0] * standoff;
    L.pos[1] = m_pos[1] - m_axis[1] * standoff;
    L.pos[2] = m_pos[2] - m_axis[2] * standoff;
    L.range  = m_tuning.radius * kLightRangeMul;

    // Intensity: the Bloom flare is the loudest moment in the sequence by a
    // wide margin (that IS the event), settling to a breathing held value.
    float inten = kLightBaseInt * (core / 3.4f);
    if (m_phase == WormholePhase::Bloom) {
        const float u = clampf(m_phaseT / m_tuning.bloomSec, 0.0f, 1.0f);
        inten = kLightBloomInt * clampf(u / 0.18f, 0.0f, 1.0f) * std::exp(-2.4f * u);
    }
    // Slow hum-synced breathe when held (hub: kCoreLightFreqHz).
    if (m_phase == WormholePhase::Held) {
        inten *= 1.0f + kLightBreatheAmp *
                 std::sin(m_time * kTau * kLightBreatheHz +
                          (float)(m_id < 0 ? 0 : m_id) * 1.7f);
    }

    // The spill takes the SAME hue slide the throat does, so an unstable hole
    // washes the hull violet and a stable one washes it blue-white. Lerp between
    // named colours; never lift all three.
    const float ins = instability();
    for (int c = 0; c < 3; ++c) {
        const float calm = m_tuning.coreColor[c];
        const float hot  = kUnstableTint[c];
        L.color[c] = (calm + (hot - calm) * ins) * inten;
    }
    out[0] = L;
    return 1;   // == kLightsPerWormhole, by construction
}

float Wormhole::distanceTo(const float p[3]) const {
    const float dx = p[0] - m_pos[0], dy = p[1] - m_pos[1], dz = p[2] - m_pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Wormhole::contains(const float p[3]) const {
    // You cannot fall into a hole that has not opened. The trigger needs a real
    // aperture, which excludes Dormant/Spark/Bloom outright.
    const float ap = aperture();
    if (ap < 0.55f) return false;
    const float dx = p[0] - m_pos[0], dy = p[1] - m_pos[1], dz = p[2] - m_pos[2];
    // Axial distance (how far through the mouth plane) and radial offset.
    const float along = dx * m_axis[0] + dy * m_axis[1] + dz * m_axis[2];
    const float rx = dx - m_axis[0] * along;
    const float ry = dy - m_axis[1] * along;
    const float rz = dz - m_axis[2] * along;
    const float rad = std::sqrt(rx * rx + ry * ry + rz * rz);
    const float mouthR = m_tuning.radius * ap;
    // A slab around the mouth plane, one aperture-radius thick, so a fast ship
    // cannot tunnel through the trigger between frames at 165 Hz.
    return rad < mouthR * 0.92f && std::fabs(along) < mouthR;
}

// ===========================================================================
// WormholeField
// ===========================================================================

void WormholeField::init(rhi::IRenderDevice& dev) {
    if (m_initialized) return;

    Prim ring = makeAnnulus(72, kRingInner);
    m_ringMesh = dev.createMesh(ring.verts.data(), (uint32_t)ring.verts.size(),
                                ring.index.data(), (uint32_t)ring.index.size());
    Prim rim = makeAnnulus(96, kRimInner);
    m_rimMesh = dev.createMesh(rim.verts.data(), (uint32_t)rim.verts.size(),
                               rim.index.data(), (uint32_t)rim.index.size());
    Prim disc = makeDisc(64);
    m_discMesh = dev.createMesh(disc.verts.data(), (uint32_t)disc.verts.size(),
                                disc.index.data(), (uint32_t)disc.index.size());

    // 512 (angle) x 256 (radial): the angular axis carries the filament detail,
    // so it gets the resolution. Linear, not sRGB — these texels are multiplied
    // by an HDR baseColorFactor on the emissive path.
    std::vector<uint8_t> tp = bakeThroatRGBA(512, 256);
    m_throatTex = dev.createTexture(tp.data(), 512, 256, /*srgb=*/false);
    std::vector<uint8_t> cp = bakeCoreRGBA(256);
    m_coreTex = dev.createTexture(cp.data(), 256, 256, /*srgb=*/false);

    m_initialized = m_ringMesh.valid() && m_rimMesh.valid() && m_discMesh.valid() &&
                    m_throatTex.valid() && m_coreTex.valid();
    if (!m_initialized) x3::logError("WormholeField::init: GPU resource creation failed");
}

void WormholeField::shutdown(rhi::IRenderDevice& dev) {
    if (m_ringMesh.valid())  { dev.destroyMesh(m_ringMesh);    m_ringMesh = {}; }
    if (m_rimMesh.valid())   { dev.destroyMesh(m_rimMesh);     m_rimMesh = {}; }
    if (m_discMesh.valid())  { dev.destroyMesh(m_discMesh);    m_discMesh = {}; }
    if (m_throatTex.valid()) { dev.destroyTexture(m_throatTex); m_throatTex = {}; }
    if (m_coreTex.valid())   { dev.destroyTexture(m_coreTex);   m_coreTex = {}; }
    m_initialized = false;
}

int WormholeField::add(const Wormhole& w) {
    if ((int)m_holes.size() >= kMaxWormholes) return -1;
    m_holes.push_back(w);
    return (int)m_holes.size() - 1;
}

void WormholeField::update(float dt) {
    for (auto& w : m_holes) w.update(dt);
}

int WormholeField::liveCount() const {
    int n = 0;
    for (const auto& w : m_holes) if (w.live()) ++n;
    return n;
}

void WormholeField::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                           const float eye[3]) const {
    if (!m_initialized) return;
    for (const auto& w : m_holes) {
        if (!w.live()) continue;
        drawOne(dev, fr, w, eye);
    }
}

// THE THROAT DRAW. kThroatLayers concentric annuli receding along +axis with
// shrinking radii, drawn FAR-TO-NEAR so the additive stack composites in depth
// order, each with its OWN spin rate and its OWN spectral tint. The parallax,
// the rolling motion and the fringing all fall out of the layer transforms —
// no custom pipeline required, which is the same constraint sky_stars and
// wormhole_vfx were built under.
void WormholeField::drawOne(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                            const Wormhole& w, const float eye[3]) const {
    (void)eye;
    const WormholeTuning& t = w.tuning();
    const float ap = w.aperture();

    float bx[3], by[3], bz[3];
    basisFromAxis(w.axis(), bx, by, bz);
    const float* P = w.pos();

    // ---- The layered throat (far end first) -------------------------------
    if (ap > 0.001f) {
        for (int layer = kThroatLayers - 1; layer >= 0; --layer) {
            const float k = w.layerIntensity(layer);
            if (k <= 0.002f) continue;
            const int L = std::max(1, kThroatLayers - 1);
            const float d = (float)layer / (float)L;      // 0 = mouth, 1 = deep

            // Radius tapers toward the far end -> a funnel, not a cylinder. The
            // taper is AGGRESSIVE (0.86) so the funnel's silhouette stays inside
            // the mouth's: at a gentler taper the throat visibly protrudes THROUGH
            // the aperture when viewed off-axis, and a hole you can see the tunnel
            // sticking out of is not a hole.
            // LINEAR taper, not quadratic. A quadratic taper is nearly flat near
            // d=0, so a third of the stack piled up at almost the same radius and
            // painted one large uniformly-dark annulus — the "purple donut" that
            // dominated three capture rounds. Linear spreads the ring radii evenly
            // from the mouth down to the convergence, and since each ring spans
            // 0.62..1.0 of its own radius, consecutive rings overlap heavily and
            // the disc fills with a CONTINUOUS radial gradient instead of a plate
            // plus a ball.
            const float rad = t.radius * ap * (1.0f - 0.88f * d);
            // Depth placement: d^1.5 spreads the stack more evenly than d^2 did
            // (which crowded everything at the mouth and left the deep end a
            // sparse set of visibly separate hoops).
            const float depth = t.throatDepth * d * std::sqrt(d);

            // Per-layer spin: deeper layers turn FASTER. Two rings at different
            // rates seen through each other is what makes the interior churn.
            const float rate = t.spinRate * (1.0f + 2.4f * d);
            const float ang  = w.time() * rate + d * 2.1f
                             + (float)(w.id() < 0 ? 0 : w.id()) * 0.9f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            // Rotate the ring's own X/Y basis about the axis.
            float rx[3], ry[3];
            for (int c = 0; c < 3; ++c) {
                rx[c] = bx[c] * ca + by[c] * sa;
                ry[c] = -bx[c] * sa + by[c] * ca;
            }
            const float pos[3] = {
                P[0] + bz[0] * depth, P[1] + bz[1] * depth, P[2] + bz[2] * depth
            };
            float m[16];
            composeXform(m, rx, ry, bz, pos, rad, rad, 1.0f);

            float tint[3];
            w.layerTint(layer, tint);
            // baseColorFactor multiplies the baked texel: bright filaments land
            // in HDR and bloom, dark valleys stay near black, so the throat keeps
            // its internal contrast instead of washing to a flat field.
            const float baseFactor[4] = { tint[0] * k, tint[1] * k, tint[2] * k, 1.0f };
            // PER-OBJECT EMISSIVE IS A THIN CHROMATIC FLOOR, NOT THE BRIGHTNESS.
            // mesh.frag adds `emissive.rgb * emissive.a` UNIFORMLY across the
            // annulus, so it cannot carry structure — it can only wash it away.
            // The first pass of this effect drove it at k*0.45 (up to ~2.7) and
            // the capture came back a flat lilac plate with a smooth pearl in the
            // middle: no filaments, no throat, everything at RGB ~230,228,249.
            // sky_stars.cpp:357 and wormhole_vfx.cpp:302 both pass emissive ZERO
            // for exactly this reason and say so in comments. This keeps a small
            // TINTED floor (so the deep layers, which the mouth light barely
            // reaches, do not go black) and hands every bit of structure back to
            // the texture * baseColorFactor product.
            const float floorEm = capped(k * 0.06f, 0.55f);
            const float em[4] = { tint[0], tint[1], tint[2], floorEm };
            dev.drawMeshEmissive(fr, m_ringMesh, m_throatTex, baseFactor, em, m);
        }
    }

    // ---- The event-horizon RIM --------------------------------------------
    // A thin, hot ring hugging the mouth. This is what gives the aperture a
    // crisp silhouette instead of a fuzzy fade (the hub learned the same thing:
    // its throat map goes dark at the rim precisely because the fresnel torus
    // owns the edge).
    if (ap > 0.02f) {
        const float rimR = t.radius * ap * 1.045f;
        const float ang = w.time() * -t.spinRate * 0.6f;
        const float ca = std::cos(ang), sa = std::sin(ang);
        float rx[3], ry[3];
        for (int c = 0; c < 3; ++c) {
            rx[c] = bx[c] * ca + by[c] * sa;
            ry[c] = -bx[c] * sa + by[c] * ca;
        }
        float m[16];
        composeXform(m, rx, ry, bz, P, rimR, rimR, 1.0f);
        float tint[3];
        w.layerTint(0, tint);
        // The rim takes a FRACTION of the core drive, not all of it — the hub's
        // "layers escalate at different rates" discipline (its rim takes 55% of
        // the kawoosh while the disk takes 100%), so the surge has internal
        // structure instead of every layer spiking together.
        const float k = capped(w.coreIntensity() * 0.30f + 0.35f, kRimEmissiveCap);
        const float baseFactor[4] = { tint[0] * k, tint[1] * k, tint[2] * k * 1.06f, 1.0f };
        // Same law as the layers: a thin tinted floor, never the brightness. The
        // rim is the one place a flat field is most visible, because it is the
        // silhouette — a hot uniform rim is a drawn-on outline.
        const float em[4] = { tint[0], tint[1], tint[2], capped(k * 0.10f, 0.9f) };
        dev.drawMeshEmissive(fr, m_rimMesh, m_throatTex, baseFactor, em, m);
    }

    // ---- The CONVERGENCE / SPARK core -------------------------------------
    // NOT a painted dot (the failure mode the hub litigated twice). This is a
    // real disc at a real place: during Spark/Bloom it sits AT the mouth and is
    // the whole effect; once the throat exists it lives at the FAR END of the
    // layer stack, so it parallaxes and occludes like the geometry it is.
    {
        const float core = w.coreIntensity();
        if (core > 0.002f) {
            float coreR, depth;
            const WormholePhase ph = w.phase();
            if (ph == WormholePhase::Spark) {
                // A point of light. Deliberately tiny: at range it is a star
                // that was not there a second ago.
                const float u = clampf(w.phaseTime() / t.sparkSec, 0.0f, 1.0f);
                coreR = t.radius * (0.012f + 0.045f * u);
                depth = 0.0f;
            } else if (ph == WormholePhase::Bloom) {
                // The flare: expands FAST out of the point.
                const float u = clampf(w.phaseTime() / t.bloomSec, 0.0f, 1.0f);
                const float e = 1.0f - (1.0f - u) * (1.0f - u);
                coreR = t.radius * (0.057f + 1.35f * e);
                depth = 0.0f;
            } else {
                // Settled: the convergence at the bottom of the throat.
                coreR = t.radius * ap * 0.30f;
                depth = t.throatDepth * 0.96f;
            }
            if (coreR > 0.0001f) {
                const float pos[3] = {
                    P[0] + bz[0] * depth, P[1] + bz[1] * depth, P[2] + bz[2] * depth
                };
                float m[16];
                composeXform(m, bx, by, bz, pos, coreR, coreR, 1.0f);
                float tint[3] = { t.coreColor[0], t.coreColor[1], t.coreColor[2] };
                const float ins = w.instability();
                for (int c = 0; c < 3; ++c)
                    tint[c] += (kUnstableTint[c] - tint[c]) * ins * 0.6f;
                const float k = capped(core, kCoreEmissiveCap);
                const float baseFactor[4] = { tint[0] * k, tint[1] * k, tint[2] * k, 1.0f };
                // The core is the ONE place a per-object emissive is defensible —
                // the SPARK has to be visible before any light exists to reveal
                // it — but it is still held well under the texture-driven term so
                // the convergence keeps its filament structure instead of
                // becoming the white pearl the first capture produced.
                const float em[4] = { tint[0], tint[1], tint[2],
                                      capped(k * 0.14f, 1.8f) };
                dev.drawMeshEmissive(fr, m_discMesh, m_coreTex, baseFactor, em, m);
            }
        }
    }
}

int WormholeField::collectLights(rhi::PointLight* out, int maxOut) const {
    if (!out || maxOut <= 0) return 0;
    // THE BOUND. Three independent limits, and the smallest wins: the caller's
    // buffer, the field-wide constant, and the per-wormhole constant. A
    // light-emitting effect with an unbounded light count is a real failure
    // mode, so it is impossible by construction here and asserted by the suite.
    const int cap = std::min(maxOut, kMaxWormholeLights);
    int n = 0;
    for (const auto& w : m_holes) {
        if (n >= cap) break;
        n += w.collectLights(out + n, std::min(cap - n, kLightsPerWormhole));
    }
    return n;
}

int WormholeField::buildCommsRows(x3::game::CommsPortal* rows, int maxRows) const {
    if (!rows || maxRows <= 0) return 0;
    int n = 0;
    for (const auto& w : m_holes) {
        if (n >= maxRows) break;
        if (!w.live()) continue;      // a dormant hole has nothing to advise about
        x3::game::CommsPortal& r = rows[n++];
        r.name = w.name();
        r.id   = w.id();
        // The instrument reading, not the label: an authored-stable wormhole
        // whose aperture is currently thrashing reports UNSTABLE. Same two-layer
        // resolution the rift-hub adapter does (ship_comms.cpp:754-776) —
        // authored baseline, then a live downgrade.
        r.stable = w.stable() && w.instability() <= 0.5f;
        r.pos[0] = w.pos()[0]; r.pos[1] = w.pos()[1]; r.pos[2] = w.pos()[2];
    }
    return n;
}

void WormholeField::publishToComms(const float eye[3]) const {
    x3::game::CommsPortal rows[x3::game::kCommsMaxPortals];
    const int n = buildCommsRows(rows, x3::game::kCommsMaxPortals);
    x3::game::commsBus().publishPortals(rows, n, eye);
}

int WormholeField::entered(const float p[3]) const {
    if (!p) return -1;
    for (int i = 0; i < (int)m_holes.size(); ++i)
        if (m_holes[(size_t)i].contains(p)) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// The authored roster for --world space.
//
// TWO holes, one of each stability, because the whole point of the `stable`
// field existing is that the player can tell them apart from the cockpit and
// AEGIS says the right word about each. They sit out along +X past the decor
// fleet (which lives at x in [30,80]) so an approaching ship sees them open
// against empty sky, and they face BACK toward the origin so the mouth — and
// therefore the light — is aimed at the player.
// ---------------------------------------------------------------------------
void seedSpaceWormholes(WormholeField& field) {
    {
        WormholeTuning t{};
        t.radius      = 30.0f;
        t.throatDepth = 48.0f;
        t.spinRate    = 0.26f;
        t.heldSec     = -1.0f;          // holds open — this is the usable one
        Wormhole w;
        // Placed just BEYOND the decor fleet (x in [30,80]) rather than far out:
        // at ~130 m its spill actually dominates the fleet's key light instead of
        // being a rounding error on it. pointAtten is w^2/(d^2+1) (see
        // shaders/inc/mesh_lighting.glsl:36) — inverse-square is unforgiving, and
        // "it lights the world" has to survive that arithmetic, not just be
        // asserted. Sited so an approaching ship sees it open against empty sky.
        const float pos[3]  = { 170.0f, 25.0f, -55.0f };
        const float axis[3] = { 0.86f, 0.06f, -0.51f };   // throat recedes away from origin
        w.configure("THE GAMMA CORRIDOR", pos, axis, /*stable=*/true, /*id=*/900, t);
        field.add(w);
    }
    {
        WormholeTuning t{};
        t.radius      = 22.0f;
        t.throatDepth = 34.0f;
        t.spinRate    = 0.34f;
        t.heldSec     = -1.0f;
        Wormhole w;
        const float pos[3]  = { 190.0f, -46.0f, 150.0f };
        const float axis[3] = { 0.62f, -0.22f, 0.75f };
        w.configure("THE DERELICT APERTURE", pos, axis, /*stable=*/false, /*id=*/901, t);
        field.add(w);
    }
}

// ===========================================================================
// --test-wormholes
// ===========================================================================

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  PASS ") + what); }
    else    { ++g_fail; x3::logError(std::string("  FAIL ") + what); }
}
} // namespace

bool runWormholeFieldSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("running space WORMHOLE field --test-wormholes self-test...");

    auto mk = [](bool stable, int id) {
        Wormhole w;
        WormholeTuning t{};
        t.heldSec = 3.0f;                 // finite hold so the full arc is testable
        const float p[3] = { 100.0f, 0.0f, 0.0f };
        const float a[3] = { 1.0f, 0.0f, 0.0f };
        w.configure(stable ? "STABLE HOLE" : "ROUGH HOLE", p, a, stable, id, t);
        return w;
    };

    // ---- W1..W4: the staged phase machine ---------------------------------
    {
        Wormhole w = mk(true, 1);
        check(w.phase() == WormholePhase::Dormant, "W1 a fresh wormhole is DORMANT");
        w.open();
        check(w.phase() == WormholePhase::Spark, "W2 open() arms the SPARK");
        // Walk the whole arc at 165 Hz and record the order phases are seen in.
        std::vector<WormholePhase> seen{ w.phase() };
        const float h = 1.0f / 165.0f;
        for (int i = 0; i < 165 * 12; ++i) {
            w.update(h);
            if (w.phase() != seen.back()) seen.push_back(w.phase());
        }
        const bool order = seen.size() == 6 &&
            seen[0] == WormholePhase::Spark  && seen[1] == WormholePhase::Bloom &&
            seen[2] == WormholePhase::Unfurl && seen[3] == WormholePhase::Held  &&
            seen[4] == WormholePhase::Closing&& seen[5] == WormholePhase::Dormant;
        check(order, "W3 the arc runs SPARK->BLOOM->UNFURL->HELD->CLOSING->DORMANT in order");
        check(w.phase() == WormholePhase::Dormant, "W4 the arc ends back at DORMANT");
    }

    // ---- W5: the opening is STAGED, not a fade ----------------------------
    {
        Wormhole w = mk(true, 2);
        w.open();
        // Sample at the middle of each phase.
        w.update(0.20f);
        const float apSpark = w.aperture(), coSpark = w.coreIntensity();
        w.update(0.50f);                                  // into Bloom
        const float apBloom = w.aperture(), coBloom = w.coreIntensity();
        w.update(0.60f);                                  // into Unfurl
        const float apUnfurl = w.aperture();
        w.update(1.30f);                                  // into Held
        const float apHeld = w.aperture();
        check(apSpark == 0.0f && coSpark > 0.1f,
              "W5a SPARK is a point of LIGHT with no aperture yet");
        check(coBloom > coSpark * 1.5f && apBloom < 0.25f,
              "W5b BLOOM flares far brighter than the spark, still barely open");
        check(apUnfurl > apBloom * 2.0f && apUnfurl < apHeld * 1.35f,
              "W5c UNFURL opens the aperture out of the bloom");
        check(apHeld > 0.85f, "W5d HELD settles at a full throat");
    }

    // ---- W6: THE 165 Hz LAW -----------------------------------------------
    // The headline dt assertion: run the identical wormhole at 60 Hz and at
    // 165 Hz and compare at MATCHED WALL-CLOCK TIMES, not matched frame counts.
    {
        Wormhole a = mk(true, 3), b = mk(true, 3);
        a.open(); b.open();
        const float h60 = 1.0f / 60.0f, h165 = 1.0f / 165.0f;
        // 3.0 s of wall clock: 180 steps at 60 Hz, 495 at 165 Hz.
        for (int i = 0; i < 180; ++i) a.update(h60);
        for (int i = 0; i < 495; ++i) b.update(h165);
        const float dTime  = std::fabs(a.time() - b.time());
        const float dAp    = std::fabs(a.aperture() - b.aperture());
        const float dCore  = std::fabs(a.coreIntensity() - b.coreIntensity());
        check(a.phase() == b.phase(), "W6a 60 Hz and 165 Hz reach the SAME phase at t=3s");
        check(dTime < 1e-3f, "W6b accumulated time agrees across framerates");
        check(dAp < 1e-3f && dCore < 5e-3f,
              "W6c aperture and core intensity agree across framerates");
    }
    // W7: and one 100 ms HITCH must not desync it — dt is drained, not dropped.
    {
        Wormhole a = mk(true, 4), b = mk(true, 4);
        a.open(); b.open();
        for (int i = 0; i < 495; ++i) b.update(1.0f / 165.0f);   // smooth 3.0 s
        for (int i = 0; i < 30; ++i)  a.update(0.100f);          // hitchy 3.0 s
        check(a.phase() == b.phase() && std::fabs(a.time() - b.time()) < 1e-3f,
              "W7 a 100 ms hitch lands on the same phase as a smooth 165 Hz run");
        check(std::fabs(a.aperture() - b.aperture()) < 1e-3f,
              "W7b ... and the same aperture (dt is DRAINED across phase edges)");
    }

    // ---- W8: STABLE vs UNSTABLE render distinguishably ---------------------
    // These two must HOLD for the whole sample window: mk()'s finite heldSec
    // would walk them into Closing partway through, and the collapse ramp would
    // swamp the very breathing signal this case is measuring.
    {
        auto mkHeld = [](bool stable, int id) {
            Wormhole w;
            WormholeTuning t{};
            t.heldSec = -1.0f;             // holds open for the whole window
            const float p[3] = { 100.0f, 0.0f, 0.0f };
            const float a[3] = { 1.0f, 0.0f, 0.0f };
            w.configure(stable ? "STABLE HOLE" : "ROUGH HOLE", p, a, stable, id, t);
            return w;
        };
        Wormhole s = mkHeld(true, 5), u = mkHeld(false, 5);
        s.forceHeld(); u.forceHeld();
        float apMinS = 1e9f, apMaxS = -1e9f, apMinU = 1e9f, apMaxU = -1e9f;
        float coMinU = 1e9f, coMaxU = -1e9f;
        float maxTintGap = 0.0f;
        for (int i = 0; i < 400; ++i) {
            s.update(1.0f / 60.0f); u.update(1.0f / 60.0f);
            apMinS = std::min(apMinS, s.aperture()); apMaxS = std::max(apMaxS, s.aperture());
            apMinU = std::min(apMinU, u.aperture()); apMaxU = std::max(apMaxU, u.aperture());
            coMinU = std::min(coMinU, u.coreIntensity());
            coMaxU = std::max(coMaxU, u.coreIntensity());
            float ts[3], tu[3];
            s.layerTint(3, ts); u.layerTint(3, tu);
            float g = 0.0f;
            for (int c = 0; c < 3; ++c) g += std::fabs(ts[c] - tu[c]);
            maxTintGap = std::max(maxTintGap, g);
        }
        const float swingS = apMaxS - apMinS, swingU = apMaxU - apMinU;
        check(s.instability() == 0.0f && u.instability() > 0.0f,
              "W8a a stable hole has zero instability; an unstable one does not");
        check(swingU > swingS * 3.0f,
              "W8b the UNSTABLE aperture wavers far more than the stable one");
        check(coMaxU > coMinU * 1.6f,
              "W8c the UNSTABLE core intensity visibly drops out (the flicker)");
        check(maxTintGap > 0.25f,
              "W8d the UNSTABLE spectral tint slides off blue (hue shift, not lift)");
        check(swingS < 0.10f, "W8e the STABLE aperture reads as steady");
    }

    // ---- W9: THE EMISSIVE CAP LAW -----------------------------------------
    {
        Wormhole u = mk(false, 6);
        u.open();
        bool coreOk = true, layerOk = true, blueOk = true;
        for (int i = 0; i < 165 * 10; ++i) {
            u.update(1.0f / 165.0f);
            if (u.coreIntensity() > kCoreEmissiveCap + 1e-4f) coreOk = false;
            for (int L = 0; L < kThroatLayers; ++L) {
                if (u.layerIntensity(L) > kLayerEmissiveCap + 1e-4f) layerOk = false;
                float t[3]; u.layerTint(L, t);
                // The hue may slide to violet, but nothing may run away toward
                // an all-channel white lift — blue must stay the top channel.
                if (t[2] < t[1]) blueOk = false;
            }
        }
        check(coreOk,  "W9a core intensity never exceeds kCoreEmissiveCap");
        check(layerOk, "W9b no throat layer exceeds kLayerEmissiveCap");
        check(blueOk,  "W9c the tint slides hue but blue stays dominant (no white lift)");
    }

    // ---- W10: real throat DEPTH -------------------------------------------
    {
        Wormhole w = mk(true, 7);
        w.forceHeld();
        w.update(1.0f / 60.0f);
        // The gradient toward the convergence is what makes flat rings read as
        // a tunnel; if every layer were equal it would be a stack of stickers.
        const float near = w.layerIntensity(0);
        const float deep = w.layerIntensity(kThroatLayers - 1);
        check(kThroatLayers >= 8, "W10a the throat is built from many concentric layers");
        check(deep > near * 1.5f,
              "W10b deeper layers are hotter — a real gradient toward the convergence");
        float tn[3], td[3];
        w.layerTint(0, tn); w.layerTint(kThroatLayers - 1, td);
        float gap = 0.0f;
        for (int c = 0; c < 3; ++c) gap += std::fabs(tn[c] - td[c]);
        check(gap > 0.30f, "W10c mouth and convergence carry different spectra (fringing)");
    }

    // ---- W11: THE LIGHT CONTRIBUTION EXISTS AND IS BOUNDED -----------------
    {
        WormholeField f;
        for (int i = 0; i < 8; ++i) {          // fill the field to capacity
            Wormhole w = mk(true, 100 + i);
            w.forceHeld();
            f.add(w);
        }
        f.update(1.0f / 60.0f);
        rhi::PointLight buf[64];
        const int n = f.collectLights(buf, 64);
        check(n > 0, "W11a a live wormhole DOES contribute light");
        check(n <= kMaxWormholeLights,
              "W11b the field-wide light count is bounded by kMaxWormholeLights");
        bool posOk = true, intOk = true;
        for (int i = 0; i < n; ++i) {
            if (!(buf[i].range > 0.0f)) posOk = false;
            const float m = std::max(buf[i].color[0],
                                     std::max(buf[i].color[1], buf[i].color[2]));
            if (!(m > 0.0f) || !std::isfinite(m)) intOk = false;
        }
        check(posOk && intOk, "W11c every emitted light has a finite range and intensity");
        // A small buffer must be respected too.
        rhi::PointLight tiny[2];
        check(f.collectLights(tiny, 2) <= 2, "W11d a caller's smaller buffer is respected");
        // A dormant field emits nothing at all.
        WormholeField dead;
        dead.add(mk(true, 200));
        check(dead.collectLights(buf, 64) == 0, "W11e a DORMANT wormhole emits no light");
    }
    // W12: the light is aimed at the approach side and reaches across the gap.
    {
        Wormhole w = mk(true, 8);
        w.forceHeld();
        w.update(1.0f / 60.0f);
        rhi::PointLight L[1];
        const int n = w.collectLights(L, 1);
        const float* P = w.pos();
        // Standoff must be on the -axis side (toward an approaching ship).
        const float along = (L[0].pos[0] - P[0]) * w.axis()[0] +
                            (L[0].pos[1] - P[1]) * w.axis()[1] +
                            (L[0].pos[2] - P[2]) * w.axis()[2];
        check(n == 1 && along < 0.0f,
              "W12a the light stands off on the APPROACH side of the mouth");
        // The ship sits at the origin, 100 m away in this fixture: the light's
        // range must actually reach it, or "it lights the world" is a lie.
        const float origin[3] = { 0, 0, 0 };
        check(L[0].range > w.distanceTo(origin),
              "W12b the light's range reaches the player ship's position");
    }

    // ---- W13: the comms rows and their stability --------------------------
    {
        WormholeField f;
        Wormhole s = mk(true, 10), u = mk(false, 11);
        s.forceHeld(); u.forceHeld();
        f.add(s); f.add(u);
        f.update(1.0f / 60.0f);
        x3::game::CommsPortal rows[x3::game::kCommsMaxPortals];
        const int n = f.buildCommsRows(rows, x3::game::kCommsMaxPortals);
        check(n == 2, "W13a both live wormholes publish a comms row");
        bool named = n == 2 && rows[0].id == 10 && rows[1].id == 11 &&
                     rows[0].name && rows[1].name && rows[0].name[0] != '\0';
        check(named, "W13b the rows carry the wormholes' ids and names");
        check(n == 2 && rows[0].stable, "W13c the STABLE hole reports stable");
        // The unstable hole must report unstable at least sometimes — it is a
        // live instrument reading, so sample across the beat.
        bool sawUnstable = false;
        for (int i = 0; i < 600; ++i) {
            f.update(1.0f / 60.0f);
            x3::game::CommsPortal r[4];
            if (f.buildCommsRows(r, 4) == 2 && !r[1].stable) { sawUnstable = true; break; }
        }
        check(sawUnstable, "W13d the UNSTABLE hole reports UNSTABLE to the advisory");
        // Dormant holes are not advised about.
        WormholeField d;
        d.add(mk(true, 12));
        x3::game::CommsPortal dr[4];
        check(d.buildCommsRows(dr, 4) == 0, "W13e a DORMANT wormhole publishes no row");
    }
    // W14: the publish actually reaches the bus the device drains.
    {
        x3::game::commsBus().reset();
        WormholeField f;
        Wormhole s = mk(true, 20);
        s.forceHeld();
        f.add(s);
        f.update(1.0f / 60.0f);
        // Stand just inside the advisory range so the director will fire.
        const float eye[3] = { 100.0f - 300.0f, 0.0f, 0.0f };
        f.publishToComms(eye);
        x3::game::CommsDevice dev;
        x3::game::CommsSnapshot snap;
        x3::game::commsBus().drain(dev, snap);
        check(snap.portalCount == 1, "W14a publishToComms lands one portal on the bus");
        check(snap.portalCount == 1 && snap.portals && snap.portals[0].stable,
              "W14b the drained row carries the right stability");
        x3::game::CommsDirector dir;
        dir.update(dev, snap, 1.0f / 60.0f);
        check(dir.advisoriesFor(20) > 0,
              "W14c the AEGIS director posts a wormhole advisory for it");
        x3::game::commsBus().reset();
    }
    // W15: an UNSTABLE hole's advisory names it unstable.
    {
        x3::game::commsBus().reset();
        WormholeField f;
        Wormhole u = mk(false, 21);
        u.forceHeld();
        f.add(u);
        // Advance to a moment the waver reads clearly unstable.
        bool posted = false;
        x3::game::CommsDevice dev;
        x3::game::CommsDirector dir;
        for (int i = 0; i < 600 && !posted; ++i) {
            f.update(1.0f / 60.0f);
            x3::game::CommsPortal r[4];
            if (f.buildCommsRows(r, 4) == 1 && !r[0].stable) {
                const float eye[3] = { -200.0f, 0.0f, 0.0f };
                x3::game::commsBus().publishPortals(r, 1, eye);
                x3::game::CommsSnapshot snap;
                x3::game::commsBus().drain(dev, snap);
                dir.update(dev, snap, 1.0f / 60.0f);
                posted = dir.advisoriesFor(21) > 0;
            }
        }
        check(posted, "W15a an UNSTABLE wormhole gets its own advisory");
        // The device's own text must say UNSTABLE, not STABLE — the payoff for
        // the `stable` field existing at all.
        bool saidUnstable = false;
        for (int i = 0; i < dev.size(); ++i) {
            const std::string& s = dev.at(i).text;
            if (s.find("UNSTABLE") != std::string::npos) saidUnstable = true;
        }
        check(saidUnstable, "W15b the advisory TEXT names it UNSTABLE");
        x3::game::commsBus().reset();
    }

    // ---- W16: the transit trigger -----------------------------------------
    {
        WormholeField f;
        Wormhole w = mk(true, 30);
        f.add(w);
        const float mouth[3] = { 100.0f, 0.0f, 0.0f };
        const float far[3]   = { 0.0f, 0.0f, 0.0f };
        check(f.entered(mouth) == -1,
              "W16a a DORMANT wormhole cannot be entered");
        f.at(0).open();
        f.update(0.30f);                       // still SPARK
        check(f.entered(mouth) == -1,
              "W16b a wormhole that has not opened yet cannot be entered");
        f.at(0).forceHeld();
        f.update(1.0f / 60.0f);
        check(f.entered(mouth) == 0, "W16c an OPEN throat is entered at the mouth");
        check(f.entered(far) == -1, "W16d a ship far from the mouth is not in transit");
        // Off-axis but at the same distance must NOT count — the mouth is a
        // disc, not a sphere.
        const float offAxis[3] = { 100.0f, 60.0f, 0.0f };
        check(f.entered(offAxis) == -1, "W16e the trigger is the MOUTH, not a sphere");
    }

    // ---- W17: the authored space roster ------------------------------------
    {
        WormholeField f;
        seedSpaceWormholes(f);
        check(f.count() == 2, "W17a --world space seeds two wormholes");
        bool haveStable = false, haveUnstable = false;
        for (int i = 0; i < f.count(); ++i)
            (f.at(i).stable() ? haveStable : haveUnstable) = true;
        check(haveStable && haveUnstable,
              "W17b the roster carries one STABLE and one UNSTABLE hole");
    }

    x3::logInfo("--test-wormholes: " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::space
