#pragma once
// ============================================================================
// THE WORLD / PLACE SELECTION MENU (owner: "I want a menu for world selection").
//
// A full-screen IMGUI-lite overlay listing EVERY destination in the registry
// (app/destinations.h), grouped, each with a name + a one-line description + an
// HONEST status:
//
//   TELEPORT      green  — this world can put you there right now. No load.
//   LOADS WORLD   amber  — it is a separate `--world` build; picking it tears the
//                          current world down and builds that one.
//   UNAVAILABLE   grey   — greyed out, NOT clickable, and the footer says WHY
//                          (e.g. "the strata are not built in this world").
//
// It does NOT decide reachability itself — the HOST does, through the `ReachFn`
// callback, because the host is the only thing that knows what is actually built
// right now. The menu never guesses and never silently fails.
//
// Drawn with the EXISTING UiContext widget layer (app/ui.h — button/label/quad/
// text), the same one the Settings menu and the world map screen use. No new UI
// system. Lives in the canon game loop next to the world map (app_run.cpp) and in
// the rifthub dev host.
//
// CLEAN-ROOM: our own screen, our own model, on our own widgets.
// ============================================================================

#include "destinations.h"
#include "ui.h"

#include <functional>
#include <string>

namespace x3::game {

// How the HOST says a destination can (or cannot) be reached from where we are.
enum class DestReach : uint8_t {
    Teleport = 0,   // an anchor exists in THIS world: move the player, no load
    WorldLoad,      // a `--world` build: the host must load-and-place
    Unavailable,    // cannot be reached at runtime — `reason` says why
};

struct DestStatus {
    DestReach   reach  = DestReach::Unavailable;
    std::string reason;   // shown in the footer when Unavailable (and only then)
    bool        here   = false;   // "you are here" — drawn, never selectable
};

class WorldMenu {
public:
    // The host answers this for every row, every frame the menu is open.
    using ReachFn = std::function<DestStatus(const Destination&)>;

    void open()        { m_open = true;  m_chosen = -1; }
    void close()       { m_open = false; }
    bool isOpen() const { return m_open; }
    void toggle()      { if (m_open) close(); else open(); }

    // Draw + drive one frame. Returns the registry index of the destination the
    // player picked THIS frame, or -1. Picking closes the menu.
    // Call between beginFrame/endFrame, with `ui` already begun.
    int draw(x3::ui::UiContext& ui, float dt, const ReachFn& reach);

private:
    bool  m_open   = false;
    int   m_chosen = -1;
    float m_clock  = 0.0f;
};

// Headless self-test (folded into --test-ui): drives the menu on a headless
// UiContext and asserts every registry entry gets a row, that UNAVAILABLE rows
// take no focus slot (they cannot be picked), and that keyboard-activating a
// focused row returns the right registry index.
bool runWorldMenuSelfTest();

} // namespace x3::game
