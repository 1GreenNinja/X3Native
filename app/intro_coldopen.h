#pragma once
// INTRO COLD-OPEN (--world intro / default lead-in). Game/slice code only — engine/ stays pure.
//
// NARRATIVE (project canon + docs/design/INTRO_COLD_OPEN.md): Jake is flying a small ship for
// ~45 seconds through space; a LARGER enemy ship slides in behind him and shoots him down with an
// energy pulse; the screen goes WHITE on the crash; then a "6 MONTHS LATER" title card; then the
// host hands off to the start of the canon cell (where Jake wakes a captive). The point: this
// establishes WHY Jake begins the game locked in a detention cell — he was shot down + CAPTURED.
//
// This is a SELF-CONTAINED scripted/cinematic prologue (NOT a 6DOF flight sim — the SpacePilot
// 6DOF controller lives on a sibling branch and is intentionally not depended on here). The ship
// flies forward on rails through a procedural starfield; the enemy ship appears, fires the pulse,
// and the sequence runs a clean phase machine to the title card and the handoff.
//
// SPLIT (mirrors ui.cpp / canon_play.cpp): a pure LOGIC class (IntroSequence) advances the phase
// state machine from dt + a skip flag — no window / Vulkan / GLFW — so --test-intro drives it
// headless and asserts the phases progress (Flight -> Hit -> Whiteout -> TitleCard -> Done) and
// that a skip jumps straight to the handoff. A separate windowed driver (runIntro) renders the
// 2D starfield + ship/enemy + the white-out + the "6 MONTHS LATER" card via the public render API
// (drawHudQuad / drawHudTextF) and runs the same logic. Headless hosts SKIP the windowed driver
// entirely (no window = no-op / instant fast-forward), so smoketests + every other --world mode
// are unaffected.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

struct GLFWwindow;   // forward decl; the windowed driver takes the host's GLFW window

namespace x3::intro {

// The cold-open phases, in order. The logic advances strictly forward; Done is terminal and is
// the host's cue to build + spawn the cell.
enum class Phase : uint8_t {
    Flight    = 0,   // Jake's ship flies forward through the starfield (~45s, skippable)
    Hit       = 1,   // the enemy ship has fired; the energy pulse strikes Jake's ship
    Whiteout  = 2,   // the crash: the screen blows out to white
    TitleCard = 3,   // "6 MONTHS LATER" fades in over black
    Done      = 4,   // terminal — hand off to the cell start
    Count     = 5,
};

// Per-phase durations (seconds). Flight is the long beat; the rest are short cinematic punches.
// A skip collapses whatever remains and runs the short tail beats so the player always sees the
// "6 MONTHS LATER" card (the narrative payload) before the handoff.
struct IntroTiming {
    float flight    = 45.0f;   // forward flight before the enemy fires
    float enemyAt   = 38.0f;   // the enemy ship slides into view this far into the flight
    float hit       = 1.2f;    // pulse-impact beat (ship shudders / sparks)
    float whiteout  = 1.0f;    // full white blow-out hold
    float titleCard = 3.5f;    // "6 MONTHS LATER" hold
    // When skipped, the tail beats are compressed so the title card still reads briefly.
    float skipWhiteout  = 0.35f;
    float skipTitleCard = 1.4f;
};

// Pure logic phase machine — no rendering. tick(dt) advances the clock + phase; skip() collapses
// the current beat and fast-forwards to the title-card tail (still showing the card, then Done).
// Fully deterministic + headless-testable. The windowed driver wraps this; --test-intro drives it
// directly.
class IntroSequence {
public:
    explicit IntroSequence(const IntroTiming& timing = {}) : m_t(timing) {}

    // Advance `dt` seconds. Drives the phase transitions on the per-phase timers (and the skip
    // tail). Idempotent once Done.
    void tick(float dt);

    // Request a skip (any key / Esc). On the FIRST skip during Flight/Hit it jumps to a short
    // Whiteout->TitleCard tail so the "6 MONTHS LATER" card still shows before Done; a SECOND
    // skip (during the title card) finishes immediately to Done.
    void skip();

    // ---- Queries (rendering + the self-test) ------------------------------
    Phase phase() const { return m_phase; }
    bool  done()  const { return m_phase == Phase::Done; }
    bool  skipped() const { return m_skipped; }

    // Seconds elapsed within the CURRENT phase (drives fades / animation in the driver).
    float phaseElapsed() const { return m_phaseT; }
    // [0,1] progress through the current phase (clamped). Convenience for fades.
    float phaseFrac() const;
    // Total seconds since the sequence began (drives the starfield scroll + ship drift).
    float totalElapsed() const { return m_total; }

    // True once the enemy ship should be visible/closing (during Flight, past enemyAt; and
    // through Hit). Lets the driver pop the enemy ship in + start its pulse.
    bool enemyVisible() const;
    // True the instant the pulse should be firing/landing (Hit phase, or the last second of
    // Flight as the enemy charges). Drives the pulse beam + screen shake in the driver.
    bool pulseActive() const;

private:
    void enter(Phase p);

    IntroTiming m_t{};
    Phase  m_phase   = Phase::Flight;
    float  m_phaseT  = 0.0f;   // elapsed in the current phase
    float  m_total   = 0.0f;   // elapsed overall
    bool   m_skipped = false;
};

// Run the windowed cold-open to completion (blocking). Renders the scripted flythrough +
// white-out + "6 MONTHS LATER" card on `device` using the public 2D + mesh API, polling `window`
// for the skip (any key / mouse / Esc). Returns when the sequence reaches Done OR the window is
// closed. HEADLESS GUARD: if `window` is null (no window), this returns IMMEDIATELY (no-op) — the
// caller's smoketest/screenshot/other --world paths never see it. Returns true if the sequence
// completed normally (false if the window was closed mid-intro, so the host can exit cleanly).
bool runIntro(x3::rhi::IRenderDevice& device, GLFWwindow* window, const IntroTiming& timing = {});

// Headless self-test (--test-intro): drives IntroSequence with synthetic dt and asserts the phase
// machine advances Flight -> Hit -> Whiteout -> TitleCard -> Done, that the enemy/pulse flags fire
// in the right window, and that a skip jumps to the title-card tail then Done. No window / Vulkan.
// Returns true iff all checks pass.
bool runIntroSelfTest();

} // namespace x3::intro
