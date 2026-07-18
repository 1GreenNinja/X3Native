#pragma once
// VIGIL BARKS — the AMBIENT companion layer.
//
// VIGIL (the facility intelligence) is a snarky sidekick riding shotgun in Jake's
// head. At a HoloTerminal he holds a real conversation (chat_tree + the LLM). OUT
// in the world he "barks": short, in-character one-liners triggered by real game
// events (alert changes, first combat, low HP, entering the elevator/club, boredom)
// and shown as a styled on-screen toast — "VIGIL: ..." in his terminal-orange voice.
//
// CANON GATE: the barks are the IN-EAR layer, and Jake can only hear VIGIL in his
// head once he has the NEURAL LINK (the vigilLink StoryFlag; Floor-4 Cybernetics).
// Before the link VIGIL is TERMINAL-ONLY — setEnabled(false) and fire() emits
// nothing. The terminal conversation works throughout; this is strictly the
// proactive ambient overlay that the link unlocks.
//
// DESIGN: authored one-liner POOLS per event, picked at random with NO immediate
// repeat, a global COOLDOWN so he is characterful and not constant, and a
// chattiness setting (Off / Occasional / Chatty) exposed as the vigil_chatter cvar.
// The pools are the reliable path; if the live model is loaded the host may inject
// a model-generated line via setLine() (best-effort garnish over the same toast).
#include <cstdint>
#include <string>

namespace x3::game {

// The game-event triggers. Each maps to an authored line POOL.
enum class VigilEvent : uint32_t {
    AlertRising = 0,   // facility alert level went UP (kill squad / lockdown)
    AlertClear,        // alert fell back to 0 (heat's off)
    FirstCombat,       // first hostile engaged / first shot fired
    LowHealth,         // HP crossed the low threshold (concern-with-attitude)
    EnterArea,         // generic new-area entry (areaName flavors it)
    EnterElevator,     // stepped into the glass elevator cab
    EnterClub,         // reached Club 1127 at the bottom
    PickupSidearm,     // picked up the sidearm (first weapon)
    Trapdoor,          // the cell floor hatch opened (code accepted)
    Idle,              // Jake's stood still too long; VIGIL gets bored
    Count
};

// vigil_chatter cvar: 0 off, 1 occasional, 2 chatty. Off silences barks even with
// the link; Occasional stretches the cooldown; Chatty uses the base cooldown.
enum class VigilChatter : uint32_t { Off = 0, Occasional = 1, Chatty = 2 };

// How long a fired bark stays on screen, and its trailing fade (seconds).
constexpr float kVigilBarkHold = 5.0f;
constexpr float kVigilBarkFade = 1.2f;

class VigilBarks {
public:
    VigilBarks();

    // ---- Configuration -----------------------------------------------------
    // Master gate = the vigilLink flag. Barks are silent until the link is set.
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }
    void setChatter(VigilChatter c) { m_chatter = c; }
    VigilChatter chatter() const { return m_chatter; }
    void setCooldown(float seconds) { m_cooldown = seconds; }
    float cooldown() const { return m_cooldown; }
    void setIdleThreshold(float seconds) { m_idleThreshold = seconds; }
    void setSeed(uint32_t s) { m_rng = s ? s : 0x9E3779B9u; }

    // ---- Firing ------------------------------------------------------------
    // Fire an event at time `now` (seconds, monotonic). Returns true and stores a
    // fresh toast if the bark passed the gate (enabled + chatter + cooldown +
    // per-event debounce + no-immediate-repeat). Returns false otherwise.
    bool fire(VigilEvent e, float now) { return fire(e, now, nullptr); }
    bool fire(VigilEvent e, float now, const char* areaName);

    // Inject a model-generated line for an event (best-effort). Bypasses pool
    // selection but still obeys the gate/cooldown. `line` should NOT include the
    // "VIGIL:" prefix (the toast adds it).
    bool setLine(VigilEvent e, const std::string& line, float now);

    // ---- Idle handling -----------------------------------------------------
    // Call each frame. Advances the idle timer and, when Jake has been still long
    // enough, fires an Idle bark. noteActivity() resets the timer (movement,
    // shooting, interacting, or any other bark firing).
    void update(float dt, float now);
    void noteActivity() { m_idleTimer = 0.0f; }

    // ---- Toast (for the HUD) ----------------------------------------------
    bool toastActive(float now) const;
    const std::string& toastText() const { return m_toast; }   // includes no prefix
    float toastAlpha(float now) const;                          // 1..0 fade tail

    // ---- Introspection (tests / logs) -------------------------------------
    const std::string& lastLine() const { return m_lastLine; }
    VigilEvent lastEvent() const { return m_lastEvent; }
    // The prompt-context hint for an event (what the LLM would be asked to riff on).
    static const char* eventContext(VigilEvent e);

private:
    bool emit(VigilEvent e, const std::string& line, float now);
    float effectiveCooldown() const;
    uint32_t nextRand();

    bool         m_enabled = false;                 // vigilLink gate
    VigilChatter m_chatter = VigilChatter::Occasional;
    float        m_cooldown = 9.0f;                 // base seconds between barks
    float        m_idleThreshold = 22.0f;           // still-time before boredom
    float        m_idleTimer = 0.0f;
    float        m_lastBarkTime = -1.0e9f;          // for the global cooldown
    float        m_toastTime = -1.0e9f;             // when the current toast fired
    std::string  m_toast;                           // current on-screen line (no prefix)
    std::string  m_lastLine;                        // last emitted line (any event)
    VigilEvent   m_lastEvent = VigilEvent::Count;
    int          m_lastPick[(int)VigilEvent::Count]; // last pool index per event (no-repeat)
    uint32_t     m_rng = 0x9E3779B9u;
};

// --test-vigil: headless self-test of the bark system (trigger -> line selection,
// cooldown gate, no-immediate-repeat, chatter levels, the vigilLink master gate,
// and idle firing). Returns true if all checks pass. No GPU / window needed.
bool runVigilBarkSelfTest();

} // namespace x3::game
