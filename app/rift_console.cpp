// RIFT CONSOLE — see rift_console.h.
//
// Clean-room: our own parameter/rule model + our own UiContext widgets. No
// third-party engine or UI source consulted.
#include "rift_console.h"

#include "destinations.h"   // the ONE destination registry (re-target + the cycle)
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// THE PARAMETERS. Safe windows are deliberately NARROW on the cyclic ones — a
// rotary is a fussy thing, and getting the phase right should feel like tuning a
// radio, not picking from a list.
// ---------------------------------------------------------------------------
constexpr RiftParamSpec kSpecs[RP_Count] = {
    // name          control              init  safeLo safeHi  unit    min     max
    { "POWER",       RiftControl::Slider, 0.45f, 0.30f, 0.72f, "MW",     0.0f, 400.0f },
    { "FREQUENCY",   RiftControl::Knob,   0.50f, 0.38f, 0.62f, "THz",    0.0f,  12.0f },
    { "PHASE",       RiftControl::Knob,   0.50f, 0.42f, 0.58f, "rad",   -3.14f,  3.14f },
    { "APERTURE",    RiftControl::Slider, 0.50f, 0.20f, 0.80f, "m",      0.0f,   4.0f },
    { "CONTAINMENT", RiftControl::Slider, 0.80f, 0.55f, 1.00f, "%",      0.0f, 100.0f },
};

// ---------------------------------------------------------------------------
// THE OUTCOME TABLE — *DATA*. First match wins, so the order IS the priority:
// catastrophes are listed before NOMINAL, and NOMINAL's rule is simply "every
// parameter inside its own safe window".
//
// A window of [0,1] on a parameter means DON'T CARE. Add an outcome by adding a
// row — no code changes (the brief: "Author the parameter->outcome mapping as DATA
// so new combos need no code").
// ---------------------------------------------------------------------------
struct RiftRule {
    RiftOutcome outcome;
    float       lo[RP_Count];
    float       hi[RP_Count];
    const char* status;      // what the glass reads out when it fires
};

constexpr float A = 0.0f, Z = 1.0f;   // "don't care" window

constexpr RiftRule kRiftRules[] = {
    // --- CATASTROPHES (priority order) ---------------------------------------
    // IMPLOSION: the field can't hold what the generator is pushing. The membrane
    // inverts and eats itself — and the gate stays dead afterwards.
    { RiftOutcome::Implosion,
      { 0.82f, A, A, A,     0.00f },
      { 1.00f, Z, Z, Z,     0.18f },
      "CONTAINMENT COLLAPSE - IMPLOSION" },

    // CONTAINMENT BREACH: a wide throat behind a weak field. Something comes
    // through.  *** STUBBED *** — the alarm + the log + the dead-gate bookkeeping
    // are live; the thing that walks out is not authored yet (see rifthub.cpp).
    { RiftOutcome::Breach,
      { A, A, A, 0.78f,     0.00f },
      { Z, Z, Z, 1.00f,     0.34f },
      "CONTAINMENT BREACH - INBOUND MASS" },

    // TEMPORAL RIFT: a hot carrier dialled hard OUT of phase. Time stops agreeing
    // with itself. (Two rows: phase can be wrong in either direction — the table's
    // whole point is that this costs a row, not a branch.)
    { RiftOutcome::TemporalRift,
      { A, 0.74f, 0.00f, A, A },
      { Z, 1.00f, 0.16f, Z, Z },
      "PHASE INVERSION - TEMPORAL RIFT" },
    { RiftOutcome::TemporalRift,
      { A, 0.74f, 0.84f, A, A },
      { Z, 1.00f, 1.00f, Z, Z },
      "PHASE INVERSION - TEMPORAL RIFT" },

    // ROOM WARP: the throat forced wider than the carrier can shape. Space bends.
    { RiftOutcome::RoomWarp,
      { A, 0.00f, A, 0.86f, A },
      { Z, 0.30f, Z, 1.00f, Z },
      "APERTURE OVERRUN - SPATIAL DISTORTION" },
    { RiftOutcome::RoomWarp,
      { A, 0.70f, A, 0.86f, A },
      { Z, 1.00f, Z, 1.00f, Z },
      "APERTURE OVERRUN - SPATIAL DISTORTION" },

    // --- NOMINAL: every parameter inside its declared safe window ------------
    { RiftOutcome::Nominal,
      { kSpecs[0].safeLo, kSpecs[1].safeLo, kSpecs[2].safeLo,
        kSpecs[3].safeLo, kSpecs[4].safeLo },
      { kSpecs[0].safeHi, kSpecs[1].safeHi, kSpecs[2].safeHi,
        kSpecs[3].safeHi, kSpecs[4].safeHi },
      "RIFT STABLE - TRAVERSAL READY" },
};
constexpr uint32_t kRuleCount = (uint32_t)(sizeof(kRiftRules) / sizeof(kRiftRules[0]));

// TYPED OVERRIDE CODES. The sliders physically cannot reach some of these states;
// typing the code can. Precision as a weapon (owner's addendum 2).
struct RiftCode { const char* code; RiftOutcome outcome; };
constexpr RiftCode kCodes[] = {
    { "SINGULARITY", RiftOutcome::Implosion    },  // collapse it on purpose
    { "CHRONOS",     RiftOutcome::TemporalRift },
    { "MOBIUS",      RiftOutcome::RoomWarp     },
    { "OPEN SESAME", RiftOutcome::Nominal      },  // force-stabilise from anywhere
};
constexpr uint32_t kCodeCount = (uint32_t)(sizeof(kCodes) / sizeof(kCodes[0]));

// (The old hardcoded kWorlds[8] whitelist lived here. It listed act2caves / act2 /
//  destruct / ragdoll — two of which have had NO --world host since the Act-2 split —
//  and it was the ONLY thing a rift could be re-targeted at, so 8 gates could reach 8
//  names, four of them dead. It is GONE: a rift now re-targets at anything in the
//  DESTINATION REGISTRY (app/destinations.h), which is the one list the world menu,
//  the hub's fast-travel resolver and this console all read. That is how the hub
//  reaches every place in the game.)

bool inWindow(float v, float lo, float hi) { return v >= lo && v <= hi; }

// Case/space-insensitive compare of a typed buffer against a token.
bool eqLoose(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

} // namespace

const RiftParamSpec& riftParamSpec(uint32_t id) {
    return kSpecs[id < RP_Count ? id : 0];
}

const char* riftOutcomeName(RiftOutcome o) {
    switch (o) {
        case RiftOutcome::Nominal:      return "NOMINAL";
        case RiftOutcome::Misfire:      return "MISFIRE";
        case RiftOutcome::RoomWarp:     return "ROOM WARP";
        case RiftOutcome::TemporalRift: return "TEMPORAL RIFT";
        case RiftOutcome::Implosion:    return "IMPLOSION";
        case RiftOutcome::Breach:       return "CONTAINMENT BREACH";
        default:                        return "NONE";
    }
}

void RiftConsole::reset() {
    for (uint32_t i = 0; i < RP_Count; ++i) value[i] = kSpecs[i].initial;
    target[0] = 0;
    entry[0] = 0;
    dirty = false;
    lastOutcome = RiftOutcome::None;
    status = "STANDBY";
}

float RiftConsole::paramDanger(uint32_t id) const {
    if (id >= RP_Count) return 0.0f;
    const RiftParamSpec& s = kSpecs[id];
    const float v = value[id];
    if (v >= s.safeLo && v <= s.safeHi) return 0.0f;
    // How far outside, normalized by the distance to that end of the range. A
    // parameter pinned at the far rail reads 1.0 — "this is as wrong as it gets".
    if (v < s.safeLo) {
        const float room = s.safeLo;                 // s.safeLo - 0
        return room > 1e-4f ? (s.safeLo - v) / room : 1.0f;
    }
    const float room = 1.0f - s.safeHi;
    return room > 1e-4f ? (v - s.safeHi) / room : 1.0f;
}

float RiftConsole::instability() const {
    // The worst single offender...
    float worst = 0.0f;
    for (uint32_t i = 0; i < RP_Count; ++i) {
        const float d = paramDanger(i);
        if (d > worst) worst = d;
    }
    // ...plus the COUPLING that actually kills you: power the field can't hold.
    // This is why the gate can start snarling while every individual dial still
    // looks survivable — and it is the tell that precedes the implosion.
    const float over = value[RP_Power] - value[RP_Containment];
    const float couple = over > 0.0f ? over : 0.0f;
    float d = worst + couple * 0.85f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}

const char* RiftConsole::targetWorld() const {
    if (!target[0]) return nullptr;
    // An OVERRIDE CODE is not a destination — it must never be mistaken for one
    // (the codes are the road to the catastrophes; re-pointing on them would be a lie).
    if (targetOverride() != RiftOutcome::None) return nullptr;
    // Anything in the DESTINATION REGISTRY. The returned pointer is the registry's
    // own static key literal, so the caller may hold it (RiftPortal::worldName does).
    const Destination* d = findDestination(target);
    return d ? d->key : nullptr;
}

RiftOutcome RiftConsole::targetOverride() const {
    if (!target[0]) return RiftOutcome::None;
    for (uint32_t i = 0; i < kCodeCount; ++i)
        if (eqLoose(target, kCodes[i].code)) return kCodes[i].outcome;
    return RiftOutcome::None;
}

RiftOutcome RiftConsole::evaluate() const {
    // A typed override code beats the dials outright (that is the point of it).
    const RiftOutcome forced = targetOverride();
    if (forced != RiftOutcome::None) return forced;

    for (uint32_t r = 0; r < kRuleCount; ++r) {
        const RiftRule& rule = kRiftRules[r];
        bool match = true;
        for (uint32_t p = 0; p < RP_Count && match; ++p)
            if (!inWindow(value[p], rule.lo[p], rule.hi[p])) match = false;
        if (match) return rule.outcome;
    }
    // Outside the safe windows but no catastrophe rule claimed it: it surges and
    // fails. (An honest middle state — not every wrong dial-in should kill you.)
    return RiftOutcome::Misfire;
}

// The status line the glass shows for an outcome (pulled from the same table, so
// a new rule brings its own copy with it).
static const char* statusFor(RiftOutcome o) {
    for (uint32_t r = 0; r < kRuleCount; ++r)
        if (kRiftRules[r].outcome == o) return kRiftRules[r].status;
    if (o == RiftOutcome::Misfire) return "MISFIRE - RIFT WILL NOT HOLD";
    return "STANDBY";
}

// ===========================================================================
// THE CONTROL SURFACE
// ===========================================================================
bool drawRiftConsole(x3::ui::UiContext& ui, RiftConsole& c, const char* portalName,
                     const char* destination, float clock, bool dead) {
    const float W = (float)ui.screenW();
    const float H = (float)ui.screenH();
    if (W <= 0.0f || H <= 0.0f) return false;

    // ---- The BLACK GLASS SLAB (the canonical holo look: black glass, blue/green
    //      light, a round-pipe frame). The world-space holoterminal in front of the
    //      portal is the same object; this is what you see when you lean into it.
    const float pw = 760.0f, ph = 470.0f;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    const float glass[4]  = { 0.012f, 0.018f, 0.028f, 0.93f };
    const float scrim[4]  = { 0.0f, 0.0f, 0.0f, 0.45f };
    ui.quad(0, 0, W, H, scrim);
    ui.quad(px, py, pw, ph, glass);

    const float inst = dead ? 0.0f : c.instability();
    // The FRAME is the coarse tell: it is the biggest lit thing on screen and it
    // goes amber/red with the rift.
    float frame[4];
    if (dead) { frame[0] = 0.25f; frame[1] = 0.25f; frame[2] = 0.27f; frame[3] = 0.55f; }
    else {
        const float calm[3] = { 0.10f, 0.62f, 0.95f };
        const float hot[3]  = { 1.00f, 0.20f, 0.12f };
        for (int i = 0; i < 3; ++i) frame[i] = calm[i] + (hot[i] - calm[i]) * inst;
        const float hz = 0.6f + 6.0f * inst * inst;
        const float s  = 0.5f * (std::sin(clock * 6.2831853f * hz) + 1.0f);
        frame[3] = 0.70f + 0.28f * inst * s;
    }
    const float ft = 3.0f;
    ui.quad(px - ft, py - ft, pw + 2 * ft, ft, frame);
    ui.quad(px - ft, py + ph, pw + 2 * ft, ft, frame);
    ui.quad(px - ft, py, ft, ph, frame);
    ui.quad(px + pw, py, ft, ph, frame);

    // ---- Header: WHERE THIS PORTAL GOES ------------------------------------
    const float blue[4]  = { 0.35f, 0.80f, 1.00f, 0.95f };
    const float green[4] = { 0.30f, 1.00f, 0.65f, 0.95f };
    const float dim[4]   = { 0.30f, 0.50f, 0.60f, 0.80f };
    char head[96];
    std::snprintf(head, sizeof(head), "RIFT GENERATOR  %s", portalName ? portalName : "?");
    ui.text(head, px + 20.0f, py + 16.0f, 20.0f, blue, x3::ui::UiContext::FontRole::Title);
    char dst[96];
    std::snprintf(dst, sizeof(dst), "DESTINATION: %s", destination ? destination : "UNSET");
    ui.text(dst, px + 20.0f, py + 44.0f, 15.0f, green, x3::ui::UiContext::FontRole::HudMono);

    if (dead) {
        const float red[4] = { 1.0f, 0.25f, 0.20f, 0.95f };
        ui.textCentered("GATE DESTROYED", px + pw * 0.5f, py + ph * 0.42f, 34.0f, red,
                        x3::ui::UiContext::FontRole::Title);
        ui.textCentered("no containment field remains. this rift is gone.",
                        px + pw * 0.5f, py + ph * 0.42f + 44.0f, 14.0f, dim,
                        x3::ui::UiContext::FontRole::HudMono);
        ui.textCentered("[E] STEP BACK", px + pw * 0.5f, py + ph - 34.0f, 14.0f, dim,
                        x3::ui::UiContext::FontRole::HudMono);
        return false;
    }

    // ---- The CONTROLS ------------------------------------------------------
    // Sliders stack down the left; the two rotaries sit on the right (cyclic
    // things get a dial). Every one of them glows with ITS OWN danger, so the
    // player can see exactly WHICH dial is the one that is about to bite.
    bool moved = false;
    float rowY = py + 78.0f;
    const float rowH = 34.0f, rowW = 430.0f;
    for (uint32_t i = 0; i < RP_Count; ++i) {
        const RiftParamSpec& s = kSpecs[i];
        if (s.control != RiftControl::Slider) continue;
        if (ui.glowSlider(s.name, c.value[i], px + 20.0f, rowY, rowW, rowH,
                          c.paramDanger(i), clock))
            moved = true;
        // Live engineering readout under the row (the value in its real unit).
        char ro[48];
        std::snprintf(ro, sizeof(ro), "%.1f %s",
                      s.unitMin + (s.unitMax - s.unitMin) * c.value[i], s.unit);
        ui.text(ro, px + 172.0f, rowY + rowH - 4.0f, 11.0f, dim,
                x3::ui::UiContext::FontRole::HudMono);
        rowY += rowH + 18.0f;
    }

    float knobX = px + pw - 170.0f;
    float knobY = py + 130.0f;
    for (uint32_t i = 0; i < RP_Count; ++i) {
        const RiftParamSpec& s = kSpecs[i];
        if (s.control != RiftControl::Knob) continue;
        if (ui.knob(s.name, c.value[i], knobX, knobY, 46.0f, c.paramDanger(i), clock))
            moved = true;
        knobX += 0.0f;
        knobY += 128.0f;
    }

    // ---- TEXT ENTRY --------------------------------------------------------
    // TARGET takes a DESTINATION (re-point the rift at any place in the registry)
    // OR an override code (the road to the outcomes the sliders physically cannot
    // reach). Typing is the precise way in; the CYCLE below is the fast way.
    rowY += 6.0f;
    bool committedTarget =
        ui.textField("TARGET", c.target, (int)sizeof(c.target),
                     px + 20.0f, rowY, rowW, rowH, 0.0f, clock);
    ui.text("any destination, or an override code", px + 172.0f, rowY + rowH - 4.0f, 11.0f,
            dim, x3::ui::UiContext::FontRole::HudMono);

    // ---- THE DESTINATION CYCLE (the owner: the hub "SHOULD TAKE US TO ALL THE
    //      WORLD PLACES"). Eight gates, one registry: PREV/NEXT walks the WHOLE
    //      destination table, so any gate can be aimed at any place in the game
    //      without knowing how to spell it. Committing is the SAME re-target path a
    //      typed TARGET takes (it counts as an ENGAGE), so the hanging glass, the
    //      HUD prompt and the gate's traversal all update together — the console can
    //      never disagree with the world.
    {
        rowY += rowH + 8.0f;
        const float cyW = 76.0f, cyH = 26.0f;
        // Where the cycle currently sits: whatever the TARGET field resolves to, else
        // the rift's live destination.
        const Destination* cur = findDestination(c.target[0] ? c.target : destination);
        const char* curName = cur ? cur->name : (destination ? destination : "?");
        ui.text("DEST", px + 20.0f, rowY + 6.0f, 13.0f, dim,
                x3::ui::UiContext::FontRole::HudMono);
        int step = 0;
        if (ui.button("< PREV", px + 76.0f, rowY, cyW, cyH))                step = -1;
        if (ui.button("NEXT >", px + 76.0f + cyW + 6.0f, rowY, cyW, cyH))   step = +1;
        ui.text(curName, px + 76.0f + 2 * cyW + 20.0f, rowY + 6.0f, 13.0f, green,
                x3::ui::UiContext::FontRole::HudMono);
        if (step != 0) {
            const Destination& nd = cycleDestination(c.target[0] ? c.target : destination,
                                                     step);
            std::snprintf(c.target, sizeof(c.target), "%s", nd.key);
            committedTarget = true;   // cycling COMMITS — same path as typing + Enter
        }
    }

    // ---- STATUS + ENGAGE ---------------------------------------------------
    const RiftOutcome pending = c.evaluate();
    const bool danger = pending != RiftOutcome::Nominal;
    float st[4];
    if (danger) { st[0] = 1.0f; st[1] = 0.45f; st[2] = 0.12f; st[3] = 0.95f; }
    else        { st[0] = green[0]; st[1] = green[1]; st[2] = green[2]; st[3] = green[3]; }
    // The panel PREDICTS the outcome before you pull the lever — the danger must be
    // readable, not a surprise (the brief's telegraph law).
    char pre[96];
    std::snprintf(pre, sizeof(pre), "PREDICTED: %s", statusFor(pending));
    ui.text(pre, px + 20.0f, py + ph - 78.0f, 14.0f, st,
            x3::ui::UiContext::FontRole::HudMono);
    char ist[64];
    std::snprintf(ist, sizeof(ist), "INSTABILITY %3d%%", (int)(inst * 100.0f + 0.5f));
    ui.text(ist, px + 20.0f, py + ph - 58.0f, 14.0f, st,
            x3::ui::UiContext::FontRole::HudMono);

    const bool engaged = ui.button("ENGAGE", px + pw - 200.0f, py + ph - 74.0f,
                                   180.0f, 44.0f) || committedTarget;
    ui.textCentered("[E] STEP BACK   -   drag sliders / turn dials / type a TARGET",
                    px + pw * 0.5f, py + ph - 22.0f, 12.0f, dim,
                    x3::ui::UiContext::FontRole::HudMono);

    c.dirty = moved;
    if (engaged) {
        c.lastOutcome = pending;
        c.status = statusFor(pending);
        x3::logInfo(std::string("[rift-console] ") + (portalName ? portalName : "?") +
                    " ENGAGE -> " + riftOutcomeName(pending) +
                    " (instability " + std::to_string((int)(inst * 100.0f)) + "%)");
        return true;
    }
    return false;
}

// ===========================================================================
// Self-test
// ===========================================================================
namespace {
int rc_pass = 0, rc_fail = 0;
void rcCheck(bool cond, const char* name) {
    if (cond) { ++rc_pass; x3::logInfo(std::string("[riftconsole-test] PASS ") + name); }
    else      { ++rc_fail; x3::logError(std::string("[riftconsole-test] FAIL ") + name); }
}
} // namespace

bool runRiftConsoleSelfTest() {
    rc_pass = rc_fail = 0;

    // C1 — the shipped defaults are SAFE: a player who touches nothing and hits
    //      ENGAGE gets a working rift, not a crater.
    {
        RiftConsole c; c.reset();
        rcCheck(c.evaluate() == RiftOutcome::Nominal && c.instability() < 0.01f,
                "C1 default dial-in is NOMINAL and reads zero instability");
    }

    // C2 — every catastrophic outcome is REACHABLE from the dials (a rule nobody
    //      can trigger is a dead rule).
    {
        RiftConsole c;
        c.reset(); c.value[RP_Power] = 0.95f; c.value[RP_Containment] = 0.05f;
        const bool imp = c.evaluate() == RiftOutcome::Implosion;

        c.reset(); c.value[RP_Frequency] = 0.90f; c.value[RP_Phase] = 0.05f;
        const bool tmp = c.evaluate() == RiftOutcome::TemporalRift;

        c.reset(); c.value[RP_Aperture] = 0.95f; c.value[RP_Frequency] = 0.10f;
        const bool wrp = c.evaluate() == RiftOutcome::RoomWarp;

        c.reset(); c.value[RP_Aperture] = 0.90f; c.value[RP_Containment] = 0.20f;
        const bool brc = c.evaluate() == RiftOutcome::Breach;

        rcCheck(imp && tmp && wrp && brc,
                "C2 implosion / temporal rift / room warp / breach are all reachable");
    }

    // C3 — PRIORITY: a catastrophe always beats NOMINAL. Dial a state that satisfies
    //      the implosion rule AND leaves the other four params inside their windows;
    //      the table must still fire the implosion (rule ORDER is the contract).
    {
        RiftConsole c; c.reset();
        c.value[RP_Power] = 0.90f;          // outside NOMINAL's power window anyway...
        c.value[RP_Containment] = 0.10f;
        rcCheck(c.evaluate() == RiftOutcome::Implosion,
                "C3 rule order: a catastrophe outranks NOMINAL");
    }

    // C4 — the TELEGRAPH is monotone: as CONTAINMENT is wound down under a high
    //      POWER, instability() must rise every step. This is the whole readability
    //      contract — the player must be able to SEE it coming.
    {
        RiftConsole c; c.reset();
        c.value[RP_Power] = 0.90f;
        bool monotone = true;
        float prev = -1.0f;
        for (int s = 10; s >= 0; --s) {
            c.value[RP_Containment] = (float)s / 10.0f;
            const float d = c.instability();
            if (d < prev - 1e-5f) monotone = false;
            prev = d;
        }
        // ...and it must actually SATURATE before the gate blows (so the panel is
        // screaming red by the time you pull the lever).
        rcCheck(monotone && prev > 0.9f,
                "C4 instability() rises monotonically toward 1.0 as the field is cut");
    }

    // C5 — a per-parameter danger is ZERO inside its own window and 1.0 at the rail
    //      (this is what each control's glow reads).
    {
        RiftConsole c; c.reset();
        bool ok = true;
        for (uint32_t i = 0; i < RP_Count; ++i) {
            const RiftParamSpec& s = riftParamSpec(i);
            c.value[i] = (s.safeLo + s.safeHi) * 0.5f;
            if (c.paramDanger(i) > 1e-5f) ok = false;
            c.value[i] = 1.0f;
            if (s.safeHi < 1.0f && c.paramDanger(i) < 0.99f) ok = false;
            c.value[i] = s.initial;
        }
        rcCheck(ok, "C5 paramDanger(): 0 inside the safe window, 1 at the rail");
    }

    // C6 — TEXT ENTRY (addendum 2). A typed world name RE-TARGETS; a typed override
    //      code FORCES its outcome even from a perfectly safe dial-in (precision as
    //      a weapon); garbage does neither.
    {
        RiftConsole c; c.reset();
        std::snprintf(c.target, sizeof(c.target), "club");
        const bool retarget = c.targetWorld() != nullptr &&
                              std::string(c.targetWorld()) == "club" &&
                              c.evaluate() == RiftOutcome::Nominal;

        c.reset();
        std::snprintf(c.target, sizeof(c.target), "singularity");   // case-insensitive
        const bool forced = c.evaluate() == RiftOutcome::Implosion;

        c.reset();
        std::snprintf(c.target, sizeof(c.target), "zzzz");
        const bool inert = c.targetWorld() == nullptr &&
                           c.targetOverride() == RiftOutcome::None &&
                           c.evaluate() == RiftOutcome::Nominal;

        rcCheck(retarget && forced && inert,
                "C6 typed TARGET: world name re-targets, override code forces, junk is inert");
    }

    x3::logInfo("riftconsole: " + std::to_string(rc_pass) + "/" +
                std::to_string(rc_pass + rc_fail) + " passed");
    return rc_fail == 0;
}

} // namespace x3::game
