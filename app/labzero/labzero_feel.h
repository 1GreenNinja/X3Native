#pragma once
// ============================================================================
// labzero_feel — the JUMP FEEL state machine, extracted (LABZERO_3D_ADDENDUM P1).
//
// The addendum's requirement, verbatim: "Consumed by BOTH the headless test
// suite and the 3D host. One implementation, two clients — the tests test THE
// code the game runs, not a copy."
//
// That is the whole point of this header. Before it, LabZeroSim owned the
// coyote/buffer logic and the 3D host did its own `jumpHeld && !prevJumpHeld`
// edge check — so the 9/9 green suite was proving the feel of code the game
// did not run. A press landing on a frame where the accumulator ran zero fixed
// steps was silently dropped, and running off a ledge gave no forgiveness at
// all. Now both drive this.
//
// PURE: no physics, no rendering, no dimensions. It counts steps and answers
// one question per fixed step — "does a jump happen now?" — which is why it
// ports unchanged from a 2D px sim to a 3D metres rail. Coyote and buffer are
// TIME, and time does not care about units (addendum A1: "unchanged (they are
// time, not distance)").
//
// The tuned windows are NOT arbitrary and must not be "cleaned up": the suite
// pins them from the other side (T3 proves a jump 5 late steps after leaving
// ground still fires and 6 is blocked; T4 proves a press 4 steps early buffers,
// 9 does not, and a held key never re-fires).
// ============================================================================

#include <cstdint>

namespace labzero {

// Step windows. Defaults mirror LabZeroSim's K::COYOTE_STEPS / K::BUFFER_STEPS;
// the sim passes its own so there is exactly one authority for the numbers.
struct LzFeelConfig {
    int coyoteSteps = 6;
    int bufferSteps = 5;
};

// The whole state: two counters and the previous button level. Small enough to
// hash into a determinism check, which is exactly what T1 does.
struct LzFeelState {
    int  coyote       = 0;
    int  jumpBuffer   = 0;
    bool prevJumpHeld = false;
};

// What the caller must do THIS step. The state machine never applies anything
// itself — a 2D sim sets vy, a 3D host issues a jump impulse through the
// character controller, and neither has to know about the other.
enum class LzJumpAction : uint8_t {
    None      = 0,
    Jump      = 1,   // grounded-or-coyote jump fires now
    ToggleJet = 2,   // airborne edge claimed by the jetpack instead
};

// Advance one FIXED step.
//
//   grounded    — is the character on the ground at the start of this step
//   jumpHeld    — controls v2: (Space || J), merged by the caller
//   jetEligible — the jetpack may claim an AIRBORNE edge (hasJetpack &&
//                 (jetActive || fuel >= REARM)). Pass false when there is no
//                 jetpack, which is every P0/P1 rail case today.
//
// ORDER IS LOAD-BEARING and is preserved from the C# original: coyote is
// refreshed/decayed BEFORE the press is read. Reading the press first would
// let a press on the frame you leave the ground consume a coyote window that
// had not been granted yet — a one-frame difference that T3 would catch.
inline LzJumpAction lzFeelStep(LzFeelState& s, const LzFeelConfig& cfg,
                               bool jumpHeld, bool grounded, bool jetEligible) {
    if (grounded)          s.coyote = cfg.coyoteSteps;
    else if (s.coyote > 0) --s.coyote;

    const bool edge = jumpHeld && !s.prevJumpHeld;
    s.prevJumpHeld = jumpHeld;

    // The jetpack intercepts an AIRBORNE edge only; grounded always jumps.
    if (edge && !grounded && jetEligible) {
        s.jumpBuffer = 0;
        return LzJumpAction::ToggleJet;
    }

    if (edge)                  s.jumpBuffer = cfg.bufferSteps;
    else if (s.jumpBuffer > 0) --s.jumpBuffer;

    if (s.jumpBuffer <= 0) return LzJumpAction::None;
    if (s.coyote > 0) {
        s.coyote     = 0;
        s.jumpBuffer = 0;
        return LzJumpAction::Jump;
    }
    return LzJumpAction::None;
}

} // namespace labzero
