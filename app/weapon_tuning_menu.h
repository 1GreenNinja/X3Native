#pragma once
// ============================================================================
// THE WEAPON TUNING PANEL (F7) — Tim, 2026-08-18:
//   "we need to get the weapon test in a NICE looking GUI Menu"
//   "The console and menu should stay consistent across x3native no matter what
//    game.. colors can change but functions should all be there."
//
// The weapon tuning shipped as console commands (`wtest <weapon>` + the w_*
// cvars). They work, and they still do — this is a PANEL OVER THEM, not a
// second implementation:
//
//   * the SOAK button runs the SAME `wtest <name>` command the console runs, so
//     the trigger pin, the cadence gate, the rays, the FX, the audio and the
//     restore-on-stop are all the shipped code path. There is no second trigger.
//   * the SLIDERS write the SAME w_* cvars, which applyWeaponFxCVars() already
//     syncs into fxTuning() every frame — so a drag lands on the very next frame
//     through the existing path, with the console showing the same value.
//   * every slider's range/step/default comes from the ONE dial table in fx.h
//     (kDial*), and the per-weapon muzzle-flash cvar name comes from
//     weaponFlashCVar() / weaponFxKindName() — so the panel's sliders CANNOT
//     drift from the WeaponFxKind enum the way a hand-written list would.
//
// WHERE IT LIVES. It is shared infrastructure, not a canonlevel feature:
// HostShell (app/world_hosts/host_shell.cpp) owns one, which gives all 28
// shell-wired world hosts the panel for free, exactly like the console; and the
// canon game loop (app_run.cpp), which predates HostShell and wires its own
// console, owns one too. ONE class, both places — that is the "same functions in
// every world" property.
//
// DEGRADES GRACEFULLY. A world with no weapon roster (a showroom, a driving
// host) installs no Source, or one with count()==0: the panel still opens and
// the four global FX dials still work (fxTuning() is process-wide), and the
// weapon column says so instead of vanishing or crashing.
//
// WHY NOT ImGui: ImGui is linked only into X3LevelArchitect, and the game's own
// UiContext already has sliders with drag capture, keyboard nudge, toggles and
// buttons in Tim's approved look. This panel is built from those widgets and the
// shared hud_panel.h glass, so it needs no new dependency and creates no second
// input-capture path.
//
// LAYOUT LAW: LEFT-anchored. The FP viewmodel sits +0.30 m right / 0.30 m below
// the eye (weapon.h kVmDefRight/kVmDefDown), so the muzzle flash renders in the
// LOWER-RIGHT of the screen — a right-hand panel would cover the exact thing
// being tuned. The panel is also vertically placed to clear the left HUD stack
// (objective/enemies at the top, HP at the bottom), and it deliberately does NOT
// freeze the sim: the whole workflow is "soak it, watch the beam, drag the
// slider", so the soak must keep firing while this is open.
// ============================================================================

#include "ui.h"

#include "engine/core/IConsole.h"

#include <functional>
#include <string>

namespace x3::game {

// How the HOST tells the panel about its weapons. Every field is optional; a
// host with no weapons simply leaves `count` at 0 (see "degrades gracefully").
struct WeaponTuningSource {
    // Roster size and per-index accessors (index 0..count-1).
    int count = 0;
    std::function<std::string(int)> name;    // roster name, e.g. "lightning"
    std::function<int(int)>         fxKind;  // (int)WeaponFxKind for that weapon

    // Live state the panel reflects. -1 means "none".
    std::function<int()> held;               // weapon currently in hand
    std::function<int()> soaking;            // weapon currently soaking

    // THE SOAK BUTTON. The host wires this to the `wtest` code path — normally
    // literally `console.exec("wtest " + name)`, which is a toggle: running it on
    // the weapon already soaking stops it. The panel never pins a trigger itself.
    std::function<void(int)> toggleSoak;
};

class WeaponTuningMenu {
public:
    void open()         { m_open = true; m_justOpened = true; }
    void close()        { m_open = false; }
    bool isOpen() const { return m_open; }
    void toggle()       { if (m_open) close(); else open(); }

    // Install the host's weapon view. Safe to call once at startup, or never.
    void setSource(WeaponTuningSource src) { m_src = std::move(src); }
    bool hasSource() const { return m_src.count > 0; }

    // Draw + drive one frame. Call between UiContext::begin/end (the caller owns
    // the UiInput snapshot, so the panel works in HostShell and in the canon loop
    // without either having to agree on an input backend).
    //
    // Reads and writes the w_* cvars on `console` directly — that IS the live
    // path (applyWeaponFxCVars syncs them into fxTuning() every frame).
    // `dt` drives the "FIRING" pulse (dt-scaled, so it beats the same at 60 and
    // 165 Hz). Returns UiContext::focusCount(), which is finalised in
    // UiContext::end() — so it is the count as of the LAST COMPLETED frame, not
    // this one. A caller that wants this frame's tally reads focusCount() itself
    // after end().
    int draw(x3::ui::UiContext& ui, x3::con::IConsole& console, float dt);

    // The weapon whose per-kind muzzle-flash slider is currently shown. Follows
    // the soaking weapon, else the weapon in hand, until the user picks one.
    int selectedWeapon() const { return m_sel; }

private:
    // One slider's worth of "read the cvar, draw the row, write it back".
    // Returns true if the value changed this frame.
    bool dial(x3::ui::UiContext& ui, x3::con::IConsole& console,
              const struct WeaponFxDial& spec, const char* cvarName,
              float x, float y, float w, float h);

    WeaponTuningSource m_src;
    bool  m_open       = false;
    bool  m_justOpened = false;   // snap m_sel to the held weapon on open
    int   m_sel        = -1;      // weapon whose flash slider is shown
    float m_clock      = 0.0f;    // seconds accumulator (the FIRING pulse)

    // Tabs + collapsing sections (WEAPONS is the first page, not the only one).
    int   m_tab         = 0;
    bool  m_openWeapons = true;
    bool  m_openDials   = true;

    // The plate is sized from what the CONTENT measured LAST frame. A panel with
    // tabs and collapsing sections has no fixed height, and a hard-coded total is
    // a number that goes wrong the first time a row is added; one frame of lag on
    // a section toggle is invisible, a plate that does not fit its rows is not.
    float m_lastContentH = 320.0f;
};

// Headless self-test (folded into --test-ui). Drives the panel on a headless
// UiContext + a real console and asserts:
//   * it enumerates the FULL roster it is given (no truncation),
//   * the per-weapon flash slider targets the cvar built from weaponFxKindName()
//     for that weapon's kind — the anti-drift property,
//   * activating a weapon row calls toggleSoak with THAT index (the wtest path),
//   * a slider write lands on the cvar, clamped to the shared dial range,
//   * RESET restores exactly the dial table's default,
//   * a source-less host (count 0) still builds the four global dials and does
//     not crash — the "every world gets it" degradation.
bool runWeaponTuningMenuSelfTest();

} // namespace x3::game
