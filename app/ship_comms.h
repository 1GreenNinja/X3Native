#pragma once
// ============================================================================
// THE SHIP COMMS DEVICE — the player's handheld channel to the ships around him.
//
// Tim, 2026-08-24: "we need another sub-system... a space communicator/AI powered
// handheld device that shows at the right side of the screen - where messages come
// through from attackers, and the ship's ai. It can take keyboard focus, and you
// can click its buttons. It looks glassy, futuristic, translucent, high tech, and
// the ship AI will notify you of unstable or stable wormholes nearby"
//
// WHERE IT LIVES — THE SHELL, NOT A HOST. Tim's standing rule is that "the console
// and menu should stay consistent across x3native no matter what game.. colors can
// change but functions should all be there." So this is shared-shell infrastructure
// like the dev console and the F7 tuning panel: HostShell owns ONE CommsDevice and
// draws it for EVERY world. A host with nothing to say gets the idle device, never
// a missing surface and never a crash. That is also why host_shell_lint's R1 rule
// (every host references HostShell) is what makes the coverage total — no host has
// to opt in, and no host can quietly grow its own.
//
// TWO HALVES, BOTH HEADLESS-TESTABLE:
//   * CommsDevice — the message store + the focus model + the glass UI. Knows
//     nothing about space, wormholes or dreadnoughts.
//   * CommsDirector — the RULES: it is handed a CommsSnapshot of real game state
//     each frame and posts messages on RISING EDGES only. All the "what does the
//     ship AI say and when" logic lives here, as a pure function of (previous
//     state, new state), which is why --test-comms can prove that a line fires on
//     its event AND NOT OTHERWISE without a GPU or a world.
//
// NO IMGUI, NO HAND-ROLLED WIDGETS. Every interactive control is a call into
// x3::ui::UiContext (button/tabBar/focus ring) and every plate is a hudPanel().
// That was evaluated and settled for this project already; the game stack covers
// it, and going around it would fork the visual system.
// ============================================================================

#include <cstdint>
#include <deque>
#include <string>

#include "engine/rhi/IRenderDevice.h"

namespace x3::ui { class UiContext; struct UiInput; }

namespace x3::game {

// ---------------------------------------------------------------------------
// WHO IS TALKING. Two senders are required by the spec (HOSTILE and SHIP AI);
// System is the device talking about itself (channel opened, backlog trimmed) and
// deliberately reads as neither of the other two.
// ---------------------------------------------------------------------------
enum class CommsSender : uint8_t {
    ShipAI  = 0,   // the player's own onboard AI — navigation, systems, warnings
    Hostile = 1,   // attacker transmissions — taunts and threats
    System  = 2,   // the device itself
};

// Display name + ink for a sender. One table, so the feed, the filter tabs and
// the focus rim can never disagree about what colour "HOSTILE" is.
const char*  commsSenderName(CommsSender s);
const float* commsSenderInk(CommsSender s);      // linear RGBA[4]
const char*  commsSenderGlyph(CommsSender s);    // the tiny iconography column

// THE TWO CALLSIGNS. The player's onboard AI is AEGIS; the attacker on the
// hostile channel is the DREADNOUGHT (which is what the encounter's own existing
// barks already call it, so the feed and the callouts agree).
constexpr const char* kCommsShipAiName  = "AEGIS";
constexpr const char* kCommsHostileName = "DREADNOUGHT";

// One line in the feed.
struct CommsMessage {
    CommsSender sender = CommsSender::System;
    std::string from;          // the speaker ("AEGIS", "DREADNOUGHT", "COMMS")
    std::string text;          // the line itself
    uint32_t    beat   = 0;    // monotonic post index — the "timestamp" marker
    float       stamp  = 0.0f; // device clock at post — rendered as the MM:SS mark
    float       age    = 0.0f; // seconds on screen (drives the arrival glow)
    bool        acked  = false;
};

// THE BACKLOG CAP. A feed that grows without bound is a leak with a UI on it, and
// this one is fed by per-frame edge detection during a fight. 64 lines is ~8x what
// the panel can show at 1080p, so scrollback is real, but the store is O(1) bounded
// for the whole session. When the cap is hit the OLDEST line is dropped.
constexpr int kCommsBacklogCap = 64;

// The production HUD pins a 150 px MINIMAP RADAR box at the top-right corner
// (app/ui.cpp). The comms device starts below it so the two never overlap — this
// is a measured value from a capture that DID overlap, not a guess.
constexpr float kCommsMinimapReserve = 150.0f;

// ---------------------------------------------------------------------------
// THE DEVICE. Store + focus + draw.
//
// THE FOCUS MODEL — the highest-risk part of this feature, stated plainly:
//   * The device is ALWAYS VISIBLE and by default owns NOTHING. Keys fall through
//     to flight exactly as if it did not exist.
//   * F10 toggles FOCUS. While focused the device owns the keyboard, the cursor is
//     released, and HostShell::inputEnabled() goes false — which is the SAME one
//     gate the console and the tuning panel already use, so every host that
//     respects the console gets comms gating for free and none had to be edited.
//   * F10 again, or ESC, releases focus instantly and restores the cursor mode.
//   * The device NEVER pauses the sim (the fight has to keep happening) and NEVER
//     latches focus on its own — only an explicit key press moves it.
// A player can therefore never lose the ability to fly because a panel silently ate
// input: nothing but F10 grants focus, and two different keys revoke it.
// ---------------------------------------------------------------------------
class CommsDevice {
public:
    // ---- Store -------------------------------------------------------------
    // Post a line. Returns the beat number assigned. Trims to kCommsBacklogCap.
    uint32_t post(CommsSender sender, const char* from, const char* text);
    void     clear();

    int  size() const { return (int)m_log.size(); }
    const CommsMessage& at(int i) const { return m_log[(size_t)i]; }
    const CommsMessage* newest() const { return m_log.empty() ? nullptr : &m_log.back(); }
    int  unacked() const;

    // ---- Focus -------------------------------------------------------------
    bool focused() const { return m_focused; }
    void setFocused(bool on);
    void toggleFocus() { setFocused(!m_focused); }

    // ---- Filter / scroll (also driven by the on-screen buttons) ------------
    // 0 = ALL, 1 = ship AI only, 2 = hostile only. Matches the tab bar order.
    int  filter() const { return m_filter; }
    void setFilter(int f);
    void scroll(int lines);          // +1 = toward older, -1 = toward newer
    int  scrollOffset() const { return m_scroll; }

    // ---- Actions the buttons fire (public so the test can call them) -------
    void ackAll();                   // ACK — mark every line acknowledged
    void hail();                     // HAIL — see the contract on m_hailCount
    int  hailCount() const { return m_hailCount; }

    // ---- Per-frame ---------------------------------------------------------
    // dt-correct: ages every line and decays the arrival glow. 165 Hz safe.
    void update(float dt);

    // Draw the device on the RIGHT edge of the screen. Safe with an invalid
    // FrameContext (headless): hit-testing and focus still run, draws are skipped
    // by UiContext, so --test-comms exercises the real button code paths.
    void draw(x3::ui::UiContext& ui, x3::rhi::IRenderDevice& device,
              const x3::rhi::FrameContext& frame, float dt);

    // The panel's screen rect for the frame it last drew — so a host (or a test)
    // can prove it does not overlap the reticle or the hardpoint labels.
    void lastRect(float& x, float& y, float& w, float& h) const {
        x = m_rect[0]; y = m_rect[1]; w = m_rect[2]; h = m_rect[3];
    }

    // Rows the panel can show at the last drawn height (scroll clamp / tests).
    int visibleRows() const { return m_visRows; }

    // TRUE while the newest arrival is still glowing (drives the rim pulse).
    float arrivalGlow() const { return m_arrival; }

private:
    std::deque<CommsMessage> m_log;
    uint32_t m_beat    = 0;
    bool     m_focused = false;
    int      m_filter  = 0;
    int      m_scroll  = 0;
    int      m_hailCount = 0;
    float    m_arrival = 0.0f;   // 0..1 new-message glow, decays over ~1.2 s
    float    m_clock   = 0.0f;   // free-running, drives the sheen sweep
    float    m_rect[4] = { 0, 0, 0, 0 };
    int      m_visRows = 0;
    int      m_tab     = 0;      // tabBar's live index (mirrors m_filter)

    bool passesFilter(const CommsMessage& m) const;
};

// ---------------------------------------------------------------------------
// THE FLIGHT-INPUT GATE, hoisted to a pure function so it can be PROVEN instead
// of eyeballed. HostShell::inputEnabled() and HostShell::key() both return
// exactly this, and --test-comms drives the same function — so the thing the test
// asserts and the thing the game runs cannot drift apart.
//
// This is the highest-risk requirement in the feature ("never let a player lose
// the ability to fly because a panel silently ate input"), which is precisely why
// it is four named booleans in one place rather than a condition repeated twice.
// ---------------------------------------------------------------------------
constexpr bool commsFlightInputEnabled(bool paused, bool consoleOpen,
                                       bool tuningOpen, bool commsFocused) {
    return !paused && !consoleOpen && !tuningOpen && !commsFocused;
}

// ---------------------------------------------------------------------------
// THE KEY ROUTER. HostShell translates GLFW keys into these and calls
// commsRouteKey(); the device never sees GLFW and the test never needs a window.
//
// THE CONTRACT, which the self-test asserts directly:
//   * While UNFOCUSED the device consumes NOTHING except Focus. Every other key
//     falls through to the host — flight is untouched.
//   * While FOCUSED it consumes its own keys, and Escape or Focus releases.
// ---------------------------------------------------------------------------
enum class CommsKey : uint8_t {
    None = 0,
    Focus,        // F10 — toggle focus (works in both states)
    Escape,       // ESC — release focus (focused only)
    ScrollUp,     // Up / PageUp
    ScrollDown,   // Down / PageDown
    NextFilter,   // Tab
    Ack,          // Enter / Space
    Hail,         // H
    Other,        // any other key — consumed while focused so nothing leaks
};

// Returns TRUE if the device consumed the key (the host must not also see it).
bool commsRouteKey(CommsDevice& dev, CommsKey k);

// ---------------------------------------------------------------------------
// WORMHOLE PROXIMITY. The ship AI's headline duty per Tim's spec.
//
// ON STABILITY — STATED PLAINLY, because it is a data change: the destination
// registry had NO stability concept before this branch. `stable` is a new field
// on the destination rows (see app/destinations.h); every pre-existing row was
// given a sane authored value rather than a random one. Nothing else reads it yet,
// so the field is additive and inert outside this device.
// ---------------------------------------------------------------------------
struct CommsPortal {
    const char* name = "";
    float       pos[3] = { 0, 0, 0 };
    bool        stable = true;
    int         id     = -1;
};

// The advisory fires when the player closes inside this range, and re-arms only
// after he leaves the hysteresis band — so hovering on the boundary at 165 Hz
// cannot machine-gun the feed.
constexpr float kCommsPortalAdvisoryRange = 900.0f;
constexpr float kCommsPortalRearmRange    = 1200.0f;

// ---------------------------------------------------------------------------
// THE SNAPSHOT the director reads. The host fills whichever fields its world
// actually has; a default-constructed snapshot is a world with nothing to say, and
// the director must post nothing at all for it (that is the idle-device test).
// ---------------------------------------------------------------------------
struct CommsSnapshot {
    bool  inSpace = false;          // false => the space feeds stay silent

    float playerPos[3] = { 0, 0, 0 };

    // ---- Player ship state (ship AI systems chatter) -----------------------
    float shieldFrac = 1.0f;        // 0..1
    float hullFrac   = 1.0f;        // 0..1
    float weaponFrac = 1.0f;        // 0..1 weapon energy
    int   incomingFighters = 0;

    // ---- The encounter (hostile transmissions) -----------------------------
    int   phase              = 0;   // encounter phase index
    bool  playerTookFirstHit = false;
    int   mountsDestroyed    = 0;   // hardpoints sheared off the capital
    bool  baysLaunching      = false;
    bool  capitalShieldsDown = false;
    bool  reactorBreach      = false;

    // ---- Wormholes / portals in range --------------------------------------
    const CommsPortal* portals = nullptr;
    int                portalCount = 0;
};

// ---------------------------------------------------------------------------
// THE DIRECTOR — state deltas in, messages out. Owns ALL the authored lines.
//
// ON THE LLM SEAM: app/dialog.h's TtsProviderFn / VoiceId seam is deliberately NOT
// wired here. Every line below is authored and keyed to a real event, so the feed
// is deterministic and testable; the seam stays clean and available for the day the
// hostile channel gets a live voice. Tim's punchlist calls reactive/taunting NPCs
// "the thing GTA cannot do" — this is the space instance of that, and it should be
// swappable at the line level, not rebuilt.
// ---------------------------------------------------------------------------
class CommsDirector {
public:
    // Feed one frame of state. Posts zero or more lines to `dev` on rising edges.
    // Pure w.r.t. its own previous-state members: same snapshot twice in a row
    // posts exactly once.
    void update(CommsDevice& dev, const CommsSnapshot& snap, float dt);

    // Drop all edge memory (world switch — the next world re-arms every line).
    void reset();

    // Test accessor: how many advisories have been posted for portal id.
    int  advisoriesFor(int portalId) const;

private:
    bool  m_seenFirstHit   = false;
    bool  m_seenBays       = false;
    bool  m_seenShieldsDown= false;
    bool  m_seenBreach     = false;
    int   m_seenMounts     = 0;
    int   m_seenPhase      = -1;
    bool  m_lowShield      = false;
    bool  m_lowHull        = false;
    bool  m_lowWeapon      = false;
    int   m_lastFighters   = 0;
    bool  m_greeted        = false;

    // Per-portal arm state, parallel to the snapshot's portal array by id.
    struct PortalMemo { int id = -1; bool announced = false; int count = 0; };
    PortalMemo m_portals[16];
    int        m_portalMemos = 0;

    PortalMemo& memoFor(int id);
};

// ---------------------------------------------------------------------------
// THE PUBLISH BUS — how real content reaches the device without plumbing.
//
// The device lives on HostShell, but the systems that KNOW things (the space
// encounter's callout pump, the portal registry, the player's ship) are deep
// inside host functions and cannot see the shell. Threading a pointer down to all
// of them would be a large, risky edit across files this lane must not disturb.
//
// So publishers post to ONE process-global bus and HostShell drains it once per
// frame. A publisher needs a single #include and a single call; it learns nothing
// about the UI, and if no shell is attached the posts are simply dropped (the bus
// is bounded, so an un-drained bus in a headless test cannot grow without limit).
//
// SINGLE-THREADED BY CONTRACT: everything here runs on the game loop, the same
// thread the hosts and the shell draw on. This is not a concurrency primitive.
// ---------------------------------------------------------------------------
class CommsBus {
public:
    // A system that already HAS a line (the encounter's existing callouts).
    void post(CommsSender sender, const char* from, const char* text);

    // State publishers. Last writer before the drain wins; a field nobody
    // publishes keeps its default, which is the "nothing to say" value.
    void publishShip(bool inSpace, float shieldFrac, float hullFrac,
                     float weaponFrac, int incomingFighters);
    void publishEncounter(int phase, bool firstHit, int mountsDestroyed,
                          bool baysLaunching, bool capitalShieldsDown,
                          bool reactorBreach);
    // Portals in the world + where the player is, so the director can do the
    // proximity test. Copies at most kCommsMaxPortals rows.
    void publishPortals(const CommsPortal* portals, int count, const float eye[3]);

    // Drained by HostShell each frame: moves queued posts into `dev` and hands
    // back the accumulated snapshot for the director.
    void drain(CommsDevice& dev, CommsSnapshot& outSnap);

    // Forget everything (world switch / test isolation).
    void reset();

    // Queued-but-undrained line count (tests).
    int pending() const { return (int)m_queue.size(); }

private:
    struct Pending { CommsSender sender; std::string from; std::string text; };
    std::deque<Pending> m_queue;
    CommsSnapshot       m_snap;
    CommsPortal         m_portals[16];
    char                m_portalNames[16][32] = {};
    int                 m_portalCount = 0;
};

// The bounded queue depth. A publisher spamming an un-drained bus (a headless
// test, or a frame where no shell is attached) drops the OLDEST line rather than
// growing — same discipline as the device's own backlog.
constexpr int kCommsBusQueueCap = 64;
constexpr int kCommsMaxPortals  = 16;

CommsBus& commsBus();     // the process-global instance

// ---------------------------------------------------------------------------
// RIFTHUB ADAPTER. The rift hub is the one place in the game with REAL portal
// positions, so it is where the wormhole advisory is actually exercised. This
// walks its portals into CommsPortal rows and publishes them with the player's
// eye, resolving each gate's stability from the destination registry's new
// `stable` field (see destinations.h) and downgrading any gate that is collapsed
// or whose rift console is running hot — a live instrument reading, not a label.
//
// Declared here rather than in rifthub.h so the rift hub keeps knowing nothing
// about the comms device; the caller is one line in the host's frame loop.
class Rifthub;   // app/rifthub.h — forward-declared so this header stays light
void commsPublishRifthubPortals(const Rifthub& hub, const float eye[3]);

// --test-comms: headless self-test of the store, the focus model, the buttons,
// the backlog cap, the director's edges and the idle host. No GPU, no world.
bool runShipCommsSelfTest();

} // namespace x3::game
