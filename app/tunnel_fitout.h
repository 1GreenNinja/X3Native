#pragma once
// ============================================================================
// TUNNEL INTERIOR FITOUT — the layout decisions, separated from the geometry.
//
// TUNNEL_INTERIOR_PLAN.md asks for a bore that reads as INFRASTRUCTURE rather
// than a tube: walkways, railings, lay-bys, doors, maintenance bays, signage,
// screens, and lighting with some of it burned out. Almost none of that is
// hard to draw. What is hard is deciding WHERE each thing goes so the result
// reads as a place someone built and maintains, instead of a corridor with
// props sprinkled down it — and that decision is pure data, so it lives here
// where it can be tested headless instead of squinted at in a screenshot.
//
// THE RULE THAT SHAPES EVERYTHING: nothing here is random. Every placement is a
// deterministic function of station and bore seed, because a burned-out lamp
// that moves between runs makes captures non-reproducible and turns any
// before/after comparison into a guess. `--test-tunnelfitout` asserts that.
//
// Order matters and the spec is explicit about it: the LAY-BYS change the
// cross-section, so their stations must be settled before walkways or railings
// are built against a profile that is about to move. Hence layByAt() is the
// primitive everything else queries, not an afterthought laid on top.
// ============================================================================
#include <cstdint>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// LAY-BYS (spec SH group). Tim: "SHOULDERS YOU CAN PULL OFF ON ... this is a
// DRIVING feature, not set dressing."
// ---------------------------------------------------------------------------
struct LayBy {
    float centreS   = 0.0f;   // station of the bay centre, metres along the route
    float halfLenM  = 0.0f;   // half the FULL-WIDTH portion (excludes tapers)
    float taperM    = 0.0f;   // taper run at each end
    int   side      = +1;     // +1 = right of travel, -1 = left
    float extraHalfW = 0.0f;  // metres of extra half-width at full bulge
};

struct FitoutConfig {
    // Spacing between lay-bys, metres. Real long bores put an emergency lay-by
    // every 1,000-1,650 ft; 300 m (~985 ft) sits at the frequent end because a
    // player who cannot find one when they want one will conclude there are
    // none at all.
    float layBySpacingM   = 300.0f;
    float layByHalfLenM   = 16.8f;   // 110 ft full-width bay (spec SH2)
    // Taper rate: spec SH4 caps the profile change at 1 ft of half-width per
    // 10 ft of arc. Sizing this as bulge/rate gives 36.6 m and is WRONG -- the
    // taper is a SMOOTHSTEP (so the wall meets the running section without a
    // crease), and smoothstep's peak slope is exactly 1.5x its linear average.
    // A 38 m run therefore peaked at 1.445 ft per 10 ft and blew the cap by
    // half. The gentler shape has to be paid for in LENGTH: 3.66 m of bulge at
    // the capped rate needs 36.6 m linear, so 55 m eased, rounded to 58.
    // Caught by F3, which measures the real query rather than trusting this
    // number -- which is the only reason it was caught at all.
    float layByTaperM     = 58.0f;
    float layByExtraHalfW = 3.66f;   // +12.0 ft of half-width at the bulge
    // Portals sit on tangent and must stay clean: no bay may intrude within
    // this distance of either mouth (spec: last 150 ft straight).
    float portalClearM    = 45.7f;   // 150 ft

    // ---- Lighting -------------------------------------------------------
    float lampSpacingM    = 12.0f;   // ~39 ft, a normal service run
    // Fraction of lamps dead. 6 % is the number that reads as "maintained but
    // not perfect"; at 20 % it reads as abandoned, which is a different story
    // than the one this tunnel is telling.
    float lampDeadFrac    = 0.06f;

    // ---- Signage + screens ----------------------------------------------
    float signSpacingM    = 75.0f;   // distance markers down the bore
    float screenSpacingM  = 220.0f;  // rarer: these are events, not wallpaper

    // ---- Doors + maintenance --------------------------------------------
    float doorSpacingM    = 165.0f;  // service doors, alternating sides
    float maintSpacingM   = 480.0f;  // plant/equipment bays
    float maintLenM       = 34.0f;
};

// What kind of thing sits at a station on the wall.
enum class FittingKind : uint32_t {
    Lamp     = 0,
    Sign     = 1,   // subway-style: bore name + distance remaining
    Screen   = 2,   // emissive panel, CP2077-style
    Door     = 3,   // service door + keypad
    SosNiche = 4,   // emergency point, always paired with a lay-by
    Count    = 5,
};

struct Fitting {
    FittingKind kind = FittingKind::Lamp;
    float s          = 0.0f;   // station, metres
    int   side       = +1;     // +1 right, -1 left; Lamp uses 0 for ceiling
    bool  dead       = false;  // Lamp only: burned out
    uint32_t variant = 0;      // sign/screen content selector
};

// The whole interior program for one bore, computed once at build time.
class TunnelFitout {
public:
    // `boreS0/boreS1` are the roofed span's stations; `seed` makes two bores in
    // the same world differ without either being random.
    void build(float boreS0, float boreS1, const FitoutConfig& cfg, uint32_t seed);

    const std::vector<LayBy>&  layBys()   const { return m_layBys; }
    const std::vector<Fitting>& fittings() const { return m_fittings; }
    const FitoutConfig& config() const { return m_cfg; }

    // EXTRA HALF-WIDTH at station `s`, in metres, including the tapers. This is
    // the primitive the shell/road/walkway geometry queries -- one function so
    // the tube, its floor and its walkway can never disagree about where the
    // wall is. Returns 0 outside any bay.
    float extraHalfWidthAt(float s, int& outSide) const;

    // Is the walkway interrupted here? A lay-by eats the walk on its side --
    // that is what makes the bay reachable from a stopped car instead of a
    // kerb you have to climb.
    bool walkwayBrokenAt(float s, int side) const;

    // Counts, for the self-test and the build log.
    uint32_t countOf(FittingKind k) const;
    uint32_t deadLampCount() const;

private:
    uint32_t hashAt(float s, uint32_t salt) const;

    FitoutConfig m_cfg{};
    std::vector<LayBy>   m_layBys;
    std::vector<Fitting> m_fittings;
    float m_s0 = 0.0f, m_s1 = 0.0f;
    uint32_t m_seed = 1u;
};

// --test-tunnelfitout. Headless: asserts determinism, that lay-bys never
// intrude on the portal tangent or overlap each other, that the taper obeys the
// spec's profile-change rate, that dead lamps are a believable minority and
// never gang up where a driver is stopped, and that the walkway breaks exactly
// where a bay is and nowhere else. Returns true on pass.
bool runTunnelFitoutSelfTest();

} // namespace x3::game
