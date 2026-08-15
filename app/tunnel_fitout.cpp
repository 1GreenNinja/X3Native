#include "tunnel_fitout.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

uint32_t TunnelFitout::hashAt(float s, uint32_t salt) const {
    // Position-quantised hash. Quantising to the centimetre matters: hashing a
    // raw float means a lamp's dead/alive state could flip if the station is
    // recomputed with a hair of floating-point difference, which is exactly the
    // non-reproducibility this whole module exists to avoid.
    uint32_t x = (uint32_t)(int32_t)std::lround(s * 100.0f);
    x ^= m_seed * 0x9E3779B9u;
    x ^= salt * 0x85EBCA6Bu;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

void TunnelFitout::build(float boreS0, float boreS1, const FitoutConfig& cfg, uint32_t seed) {
    m_cfg = cfg;
    m_seed = seed ? seed : 1u;
    m_s0 = boreS0; m_s1 = boreS1;
    m_layBys.clear();
    m_fittings.clear();
    if (boreS1 - boreS0 <= 1.0f) return;

    // ---- LAY-BYS FIRST. They own the cross-section, so nothing else may be
    // placed until their stations exist (spec: settle the profile BEFORE
    // building walkways and railings against it).
    const float usableLo = boreS0 + cfg.portalClearM;
    const float usableHi = boreS1 - cfg.portalClearM;
    // Full footprint of one bay, tapers included -- the figure that decides
    // whether two can coexist without their tapers overlapping into a
    // continuously-wide tube, which would read as a cavern, not a lay-by.
    const float footprint = 2.0f * (cfg.layByHalfLenM + cfg.layByTaperM);
    int side = +1;
    if (usableHi - usableLo > footprint) {
        // HOW MANY ACTUALLY FIT, not how many the spacing suggests. Deriving the
        // count from spacing alone put the outermost bays past the usable ends,
        // where the footprint test below then rejected them -- and on the demo
        // bore it rejected BOTH, so a 1,486 ft tunnel with room for a lay-by got
        // none at all while the maths looked reasonable. The run must fit
        // ENDS-INCLUSIVE: (n-1) gaps plus one full footprint inside the usable
        // length. Caught by driving it, not by the test, because F1/F2 only ever
        // asserted that whatever survived was legal -- never that anything did.
        const float usable = usableHi - usableLo;
        int n = (int)std::floor((usable - footprint) / cfg.layBySpacingM) + 1;
        if (n < 1) n = 1;
        const float span = (float)(n - 1) * cfg.layBySpacingM;
        const float start = (usableLo + usableHi) * 0.5f - span * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float c = start + (float)i * cfg.layBySpacingM;
            if (c - (cfg.layByHalfLenM + cfg.layByTaperM) < usableLo) continue;
            if (c + (cfg.layByHalfLenM + cfg.layByTaperM) > usableHi) continue;
            LayBy b;
            b.centreS = c;
            b.halfLenM = cfg.layByHalfLenM;
            b.taperM = cfg.layByTaperM;
            // ALTERNATE SIDES. Both-sides-at-once would double the widening at
            // one station and make the bore momentarily enormous; alternating
            // also means a driver always has one coming up on their own side.
            b.side = side; side = -side;
            b.extraHalfW = cfg.layByExtraHalfW;
            m_layBys.push_back(b);
        }
    }

    // ---- LAMPS. A continuous run down the crown, with a deterministic
    // minority dead.
    for (float s = boreS0 + cfg.lampSpacingM * 0.5f; s < boreS1; s += cfg.lampSpacingM) {
        Fitting f;
        f.kind = FittingKind::Lamp;
        f.s = s;
        f.side = 0;
        const uint32_t h = hashAt(s, 11u);
        f.dead = ((float)(h & 0xFFFFu) / 65535.0f) < cfg.lampDeadFrac;
        // TWO RULES that turn noise into maintenance. Without them the burnouts
        // read as random speckle rather than as a tunnel someone services.
        //  1. Never dark at a LAY-BY. That is where a driver stops, gets out and
        //     needs to see; it is the first place a crew replaces a tube.
        //  2. Never two dead in a row, anywhere. A double gap reads as a power
        //     fault -- a different, larger story than a failed lamp.
        if (f.dead) {
            for (const LayBy& b : m_layBys) {
                if (std::fabs(s - b.centreS) < b.halfLenM + b.taperM) { f.dead = false; break; }
            }
        }
        if (f.dead && !m_fittings.empty()) {
            const Fitting& prev = m_fittings.back();
            if (prev.kind == FittingKind::Lamp && prev.dead) f.dead = false;
        }
        m_fittings.push_back(f);
    }

    // ---- SIGNS. Subway-style: the bore's name and how much of it is left.
    // Alternating sides so one is always facing you on a curve.
    {
        int sgn = +1;
        for (float s = boreS0 + cfg.signSpacingM; s < boreS1 - 10.0f; s += cfg.signSpacingM) {
            Fitting f;
            f.kind = FittingKind::Sign;
            f.s = s; f.side = sgn; sgn = -sgn;
            f.variant = hashAt(s, 23u) & 3u;
            m_fittings.push_back(f);
        }
    }

    // ---- SCREENS. Rare on purpose: an emissive panel every 20 m is wallpaper,
    // one every 700 ft is an event you drive past.
    {
        int sgn = -1;
        for (float s = boreS0 + cfg.screenSpacingM * 0.6f; s < boreS1 - 20.0f; s += cfg.screenSpacingM) {
            Fitting f;
            f.kind = FittingKind::Screen;
            f.s = s; f.side = sgn; sgn = -sgn;
            f.variant = hashAt(s, 37u) & 7u;
            m_fittings.push_back(f);
        }
    }

    // ---- DOORS. Service doors with keypads, alternating sides. These are the
    // thresholds the rooms/halls hang off later; placing them now fixes where
    // that content can go and keeps it from being invented ad hoc.
    {
        int sgn = +1;
        for (float s = boreS0 + cfg.doorSpacingM * 0.5f; s < boreS1 - 15.0f; s += cfg.doorSpacingM) {
            Fitting f;
            f.kind = FittingKind::Door;
            f.s = s; f.side = sgn; sgn = -sgn;
            f.variant = hashAt(s, 53u) & 3u;
            m_fittings.push_back(f);
        }
    }

    // ---- SOS NICHES. One per lay-by, by construction rather than by spacing:
    // the emergency point belongs where you can actually stop.
    for (const LayBy& b : m_layBys) {
        Fitting f;
        f.kind = FittingKind::SosNiche;
        f.s = b.centreS;
        f.side = b.side;
        m_fittings.push_back(f);
    }
}

float TunnelFitout::extraHalfWidthAt(float s, int& outSide) const {
    outSide = 0;
    for (const LayBy& b : m_layBys) {
        const float d = std::fabs(s - b.centreS);
        if (d >= b.halfLenM + b.taperM) continue;
        outSide = b.side;
        if (d <= b.halfLenM) return b.extraHalfW;
        // Smooth taper. A LINEAR ramp would put a visible crease in the wall at
        // both ends of every bay; smoothstep leaves the wall tangent-continuous
        // where it meets the running section.
        const float t = 1.0f - (d - b.halfLenM) / b.taperM;
        return b.extraHalfW * (t * t * (3.0f - 2.0f * t));
    }
    return 0.0f;
}

bool TunnelFitout::walkwayBrokenAt(float s, int side) const {
    for (const LayBy& b : m_layBys) {
        if (b.side != side) continue;
        if (std::fabs(s - b.centreS) <= b.halfLenM) return true;
    }
    return false;
}

uint32_t TunnelFitout::countOf(FittingKind k) const {
    uint32_t n = 0;
    for (const Fitting& f : m_fittings) if (f.kind == k) ++n;
    return n;
}

uint32_t TunnelFitout::deadLampCount() const {
    uint32_t n = 0;
    for (const Fitting& f : m_fittings) if (f.kind == FittingKind::Lamp && f.dead) ++n;
    return n;
}

// ===========================================================================
// --test-tunnelfitout
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void fcheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
const float kFt = 3.28084f;
}  // namespace

bool runTunnelFitoutSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- tunnel interior fitout self-test ---");
    char buf[256];

    FitoutConfig cfg;
    const float s0 = 100.0f, s1 = 1600.0f;      // a 4,920 ft bore
    TunnelFitout ft; ft.build(s0, s1, cfg, 7u);

    std::snprintf(buf, sizeof(buf),
        "F0 a %.0f ft bore fitted out: %u lay-bys, %u lamps (%u dead), %u signs, %u screens, %u doors",
        (s1 - s0) * kFt, (uint32_t)ft.layBys().size(), ft.countOf(FittingKind::Lamp),
        ft.deadLampCount(), ft.countOf(FittingKind::Sign),
        ft.countOf(FittingKind::Screen), ft.countOf(FittingKind::Door));
    fcheck(!ft.layBys().empty() && ft.countOf(FittingKind::Lamp) > 10, buf);

    // F1: PORTALS STAY CLEAN. No bay, taper included, may reach the tangent.
    {
        bool clean = true;
        for (const LayBy& b : ft.layBys()) {
            if (b.centreS - (b.halfLenM + b.taperM) < s0 + cfg.portalClearM - 0.01f) clean = false;
            if (b.centreS + (b.halfLenM + b.taperM) > s1 - cfg.portalClearM + 0.01f) clean = false;
        }
        std::snprintf(buf, sizeof(buf),
            "F1 no lay-by (tapers included) intrudes on the %.0f ft portal tangent", cfg.portalClearM * kFt);
        fcheck(clean, buf);
    }

    // F2: bays never overlap -- overlapping tapers would make a permanently
    // wide tube, which is a cavern, not a lay-by.
    {
        bool sep = true;
        for (size_t i = 1; i < ft.layBys().size(); ++i) {
            const LayBy& a = ft.layBys()[i-1]; const LayBy& b = ft.layBys()[i];
            if (a.centreS + a.halfLenM + a.taperM > b.centreS - b.halfLenM - b.taperM) sep = false;
        }
        fcheck(sep, "F2 lay-by footprints never overlap (a continuously wide bore is a cavern)");
    }

    // F3: THE PROFILE-CHANGE RATE (spec SH4): <= 1 ft of half-width per 10 ft
    // of arc. Measured off the real query, not off the config.
    {
        float worst = 0.0f; int sideTmp = 0;
        for (float s = s0; s < s1; s += 0.5f) {
            const float a = ft.extraHalfWidthAt(s, sideTmp);
            const float b = ft.extraHalfWidthAt(s + 0.5f, sideTmp);
            worst = std::max(worst, std::fabs(b - a) / 0.5f);
        }
        std::snprintf(buf, sizeof(buf),
            "F3 worst profile change %.3f ft of half-width per 10 ft of arc (spec cap 1.0)", worst * 10.0f);
        fcheck(worst * 10.0f <= 1.0f, buf);
    }

    // F4: the bulge is real and reaches full width -- a taper that never
    // arrives is a bay you cannot park in.
    {
        int sd = 0;
        float peak = 0.0f;
        for (const LayBy& b : ft.layBys()) peak = std::max(peak, ft.extraHalfWidthAt(b.centreS, sd));
        std::snprintf(buf, sizeof(buf), "F4 a bay opens the full %.1f ft of extra half-width", peak * kFt);
        fcheck(std::fabs(peak - cfg.layByExtraHalfW) < 0.01f, buf);
    }

    // F5: DEAD LAMPS ARE MAINTENANCE, NOT NOISE.
    {
        const uint32_t lamps = ft.countOf(FittingKind::Lamp);
        const uint32_t dead = ft.deadLampCount();
        bool adjacent = false, atBay = false;
        const Fitting* prev = nullptr;
        for (const Fitting& f : ft.fittings()) {
            if (f.kind != FittingKind::Lamp) continue;
            if (prev && prev->dead && f.dead) adjacent = true;
            if (f.dead) {
                for (const LayBy& b : ft.layBys())
                    if (std::fabs(f.s - b.centreS) < b.halfLenM + b.taperM) atBay = true;
            }
            prev = &f;
        }
        std::snprintf(buf, sizeof(buf), "F5a %u of %u lamps dead (%.0f%%) -- maintained, not abandoned",
                      dead, lamps, 100.0f * (float)dead / (float)std::max(1u, lamps));
        fcheck(dead > 0 && dead < lamps / 4, buf);
        fcheck(!adjacent, "F5b never two dead lamps in a row (that reads as a power fault, not a bulb)");
        fcheck(!atBay,    "F5c never a dead lamp at a lay-by -- where you stop is where crews relamp first");
    }

    // F6: the walkway breaks EXACTLY at bays and nowhere else.
    {
        bool ok = true;
        for (float s = s0; s < s1; s += 1.0f) {
            for (int side = -1; side <= 1; side += 2) {
                bool expect = false;
                for (const LayBy& b : ft.layBys())
                    if (b.side == side && std::fabs(s - b.centreS) <= b.halfLenM) expect = true;
                if (ft.walkwayBrokenAt(s, side) != expect) ok = false;
            }
        }
        fcheck(ok, "F6 the walkway is interrupted at a bay and continuous everywhere else");
    }

    // F7: SOS niches pair with bays by construction.
    {
        std::snprintf(buf, sizeof(buf), "F7 one SOS niche per lay-by (%u / %u)",
                      ft.countOf(FittingKind::SosNiche), (uint32_t)ft.layBys().size());
        fcheck(ft.countOf(FittingKind::SosNiche) == (uint32_t)ft.layBys().size(), buf);
    }

    // F8: DETERMINISM, and that the seed actually varies the bore.
    {
        TunnelFitout a, b, c;
        a.build(s0, s1, cfg, 7u);
        b.build(s0, s1, cfg, 7u);
        c.build(s0, s1, cfg, 8u);
        bool same = a.fittings().size() == b.fittings().size();
        if (same) for (size_t i = 0; i < a.fittings().size(); ++i)
            if (a.fittings()[i].dead != b.fittings()[i].dead ||
                a.fittings()[i].s    != b.fittings()[i].s) { same = false; break; }
        bool differs = false;
        for (size_t i = 0; i < a.fittings().size() && i < c.fittings().size(); ++i)
            if (a.fittings()[i].dead != c.fittings()[i].dead) { differs = true; break; }
        fcheck(same, "F8a same seed -> identical fitout (captures stay reproducible)");
        fcheck(differs, "F8b a different seed -> a different bore (two tunnels are not twins)");
    }

    // F9: a SHORT bore degrades sanely rather than cramming bays into a portal.
    {
        TunnelFitout tiny; tiny.build(0.0f, 80.0f, cfg, 3u);
        std::snprintf(buf, sizeof(buf), "F9 a %.0f ft bore gets %u lay-bys (no room, and it does not force one)",
                      80.0f * kFt, (uint32_t)tiny.layBys().size());
        fcheck(tiny.layBys().empty(), buf);
    }

    // F10: A BORE WITH ROOM FOR A BAY GETS ONE. This is the assertion the suite
    // was missing, and its absence hid a real bug: F1/F2 only ever checked that
    // whatever SURVIVED placement was legal, never that anything did. The demo
    // bore (1,486 ft, room for one) was silently producing ZERO because the
    // count came from spacing rather than from what fits, and every existing
    // assertion passed on the empty set. A suite that is vacuously true on no
    // output is not testing the output.
    {
        const float demoS0 = 89.0f, demoS1 = 542.0f;    // the actual demo ridge bore
        TunnelFitout demo; demo.build(demoS0, demoS1, cfg, 0x7A11u);
        const float usable = (demoS1 - demoS0) - 2.0f * cfg.portalClearM;
        const float need   = 2.0f * (cfg.layByHalfLenM + cfg.layByTaperM);
        std::snprintf(buf, sizeof(buf),
            "F10 the demo bore (%.0f ft, %.0f ft usable vs %.0f ft needed) gets %u lay-by(s), not zero",
            (demoS1 - demoS0) * kFt, usable * kFt, need * kFt, (uint32_t)demo.layBys().size());
        fcheck(usable > need && !demo.layBys().empty(), buf);
    }

    std::snprintf(buf, sizeof(buf), "--- tunnel fitout self-test: %d passed, %d failed ---", g_pass, g_fail);
    if (g_fail) x3::logError(buf); else x3::logInfo(buf);
    return g_fail == 0;
}

}  // namespace x3::game
