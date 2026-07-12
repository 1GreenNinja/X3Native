#pragma once
// RIFT CONSOLE (ROUND 8, section E) — the mad-scientist control surface bolted to
// every rift in the hub. See docs/RIFTHUB_ART_TARGET.md ROUND 8 + addenda 1-3.
//
// Owner, verbatim:
//   "let the user interact with each portal ... they can do wonderful or
//    disasterous things with the console to it"
//   "changing certain values will warp the room, and others will cause a temporal
//    rift .. others an implosion"
//   "Sliders, knobs... etc can be changed on this terminal"     (addendum 1)
//   "text too" / "input"                                        (addendum 2)
//   "the sliders glow"                                          (addendum 3)
//
// THE MODEL
// ---------
// A console is SIX TUNABLE PARAMETERS, not a menu of commands:
//
//   POWER        slider   how hard the generator is driven
//   FREQUENCY    knob     the carrier (cyclic -> a rotary is the honest metaphor)
//   PHASE        knob     alignment against the carrier (cyclic)
//   APERTURE     slider   how wide the throat is forced open
//   CONTAINMENT  slider   the field that keeps the whole thing in the ring
//   TARGET       text     the destination designation (typed: a world name or a code)
//
// Each numeric parameter declares a SAFE WINDOW. Dial inside every window and the
// rift does something wonderful. Dial outside and it does something spectacular.
//
// THE OUTCOME TABLE IS DATA (kRiftRules in the .cpp): each rule is a set of
// per-parameter windows plus the outcome it fires, evaluated in priority order —
// catastrophes first, NOMINAL last. Adding a new parameter/outcome combination is
// a table row, not a code change. That is the brief's requirement.
//
// THE TELEGRAPH
// -------------
// instability() is a CONTINUOUS 0..1 read of how angry the rift is RIGHT NOW, and
// it is computed from the live parameter values — not from the outcome. Everything
// the player can see is driven off it: the control's own glow (blue-green -> amber
// -> red, pulsing faster as it climbs), the tube's LED readouts, the ratchet track,
// the membrane's colour and churn, the gate light, the alarm. So the rift visibly
// gets angry UNDER THE PLAYER'S HAND, before it blows. Danger is readable.
//
// Clean-room: our own parameter/rule model, drawn on X3Native's own UiContext.
#include "ui.h"

#include <cstdint>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Parameters. Order is the authoring order AND the UI order.
// ---------------------------------------------------------------------------
enum RiftParamId : uint32_t {
    RP_Power = 0,
    RP_Frequency,
    RP_Phase,
    RP_Aperture,
    RP_Containment,
    RP_Count,            // TARGET is the typed field, not a numeric slot
};

// How a numeric parameter presents itself on the glass.
enum class RiftControl : uint8_t { Slider = 0, Knob = 1 };

struct RiftParamSpec {
    const char* name;
    RiftControl control;
    float       initial;
    float       safeLo;      // the NOMINAL window, normalized [0,1]
    float       safeHi;
    const char* unit;        // readout flavour ("MW", "THz", "rad", "m", "%")
    float       unitMin;     // value -> display scale
    float       unitMax;
};

const RiftParamSpec& riftParamSpec(uint32_t id);

// ---------------------------------------------------------------------------
// Outcomes. Each is REAL and VISIBLE (see rifthub.cpp for what each one does to
// the geometry / lights / membrane / audio).
// ---------------------------------------------------------------------------
enum class RiftOutcome : uint8_t {
    None = 0,
    Nominal,        // open + stabilise (and re-target / widen if TARGET+APERTURE say so)
    Misfire,        // outside the windows but not catastrophic: it surges and fails
    RoomWarp,       // the hub bends around the player — disorienting, survivable
    TemporalRift,   // time distorts: slow-motion, stutter, pitch-bent audio
    Implosion,      // the membrane inverts and sucks inward; the gate ends up DEAD
    Breach,         // something comes THROUGH  (STUBBED: alarms + log, no spawn yet)
};

const char* riftOutcomeName(RiftOutcome o);

// ---------------------------------------------------------------------------
// One portal's console state.
// ---------------------------------------------------------------------------
struct RiftConsole {
    float value[RP_Count] = {};      // live normalized parameter values
    char  target[24] = {};           // typed destination / override code
    char  entry[16]  = {};           // typed EXACT numeric value for the focused param
    bool  dirty = false;             // a control moved this frame (host: re-tune audio)

    // Latched results of the last ENGAGE.
    RiftOutcome lastOutcome = RiftOutcome::None;
    std::string status = "STANDBY";  // shown on the holoterminal glass + the LCD

    void reset();

    // 0..1 — how far the CURRENT dial-in sits outside its safe windows. Continuous:
    // this is the telegraph the whole hub reads (glow, LEDs, membrane, alarm).
    float instability() const;
    // Per-parameter danger (drives THAT control's own glow) — 0 inside its window,
    // ramping to 1 at the far end of the range.
    float paramDanger(uint32_t id) const;

    // Evaluate the DATA table (kRiftRules) against the current values + the typed
    // TARGET. Pure: no side effects — the caller applies the outcome.
    RiftOutcome evaluate() const;

    // If TARGET names a real --world slice, return it (else nullptr). A NOMINAL
    // engage with a valid TARGET re-points the rift at that destination.
    const char* targetWorld() const;
    // If TARGET is a recognised OVERRIDE CODE, return the outcome it forces (else
    // None). Typed codes are the gate on the most spectacular effects — the owner's
    // "precision is how you do something catastrophic on purpose".
    RiftOutcome targetOverride() const;
};

// ---------------------------------------------------------------------------
// The control surface. Draws the black-glass console over the live frame using
// the R8 GLOWING widgets (ui.h: glowSlider / knob / textField) and edits `c` in
// place. Returns true on the frame the player commits ENGAGE.
//
// `clock` advances the glow pulse. `portalName` + `destination` head the panel.
// `dead` renders the whole surface dark and refuses input (an imploded gate stays
// imploded — persistent consequence).
// ---------------------------------------------------------------------------
bool drawRiftConsole(x3::ui::UiContext& ui, RiftConsole& c, const char* portalName,
                     const char* destination, float clock, bool dead);

// Headless self-test (--test-riftconsole is folded into --test-rifthub): asserts
// the safe defaults evaluate NOMINAL, that each catastrophic rule is REACHABLE and
// that the rules are ordered so a catastrophe always beats NOMINAL, that
// instability() is monotone as you dial away from safety, and that a typed override
// code forces its outcome.
bool runRiftConsoleSelfTest();

} // namespace x3::game
