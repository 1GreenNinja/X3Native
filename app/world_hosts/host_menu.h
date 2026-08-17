#pragma once
// WORLD GAME MENU + WEATHER / LIGHTING CONTROL PANELS (owner ask, 2026-08:
// "Lets add a button for weather, or hotkeys, with on screen sliders when
// active.. I suggest an F Key.. Maybe F4? Also Lighting.. we need the main
// game's menu with all the controls, and for raytraced lighting, lets have a
// button for your suggested settings too.")
//
// WHAT THIS IS. One module, three surfaces, all of them PURE UI OVER THE
// CONSOLE CVARS THAT ALREADY EXIST (NO_SLOP rule 4 — the slider and the typed
// command move the SAME value, so they can never disagree):
//
//   * THE GAME MENU (ESC): the main game's menu chrome — the same UiContext
//     widgets, fonts and panel the campaign menu draws with (app/ui.h), plus
//     the REAL x3::ui::SettingsMenu screen (Bloom/SSAO/SSGI/Shadows/VSync/
//     RT AO — the main game's controls), reused whole. Wired into a host via
//     HostShell::setEscapeHandler first-refusal: ESC opens/closes this, the
//     console (~) and SHIFT+ESC keep working through the shell untouched.
//     While the menu or settings screen is up the HOST FREEZES ITS SIM (the
//     same contract as HostShell::setFreezesSim) — the title says PAUSED and
//     it is true.
//
//   * WEATHER PANEL (F4): on-screen sliders over the wx_* console state —
//     rain 0-10 on the owner's scale (sprinkle -> downpour -> MONSOON), snow
//     0-10, cloud cover, wind speed/direction, time of day. Mouse-draggable,
//     arrow-key nudgeable, numeric value shown. The sim KEEPS RUNNING while
//     it is open — watching the rain arrive is the point — so WASD still
//     drives; only the mouse and the arrow keys belong to the panel.
//
//   * LIGHTING PANEL (F5): the ray-traced lighting controls (r_ddgi,
//     r_ddgi_intensity, r_ddgi_rays, r_rtreflections, r_rtao) plus exposure /
//     bloom / SSAO, and the one-click SUGGESTED SETTINGS + RESTORE DEFAULTS
//     buttons. Every applied value is printed to the console (receipts).
//     Same live-sim rule as F4.
//
// WINDOW-FREE ON PURPOSE: everything here takes an explicit UiInput snapshot
// and draws through UiContext, so the headless proof captures can stage these
// screens with synthetic input through the SAME code path the player uses
// (the X3_SHOT_PUMP pattern — never a mock-up).
#include "../ui.h"
#include "engine/core/IConsole.h"
#include "engine/rhi/IRenderDevice.h"

namespace x3::apphost {

// ===========================================================================
// THE RAIN/SNOW SCALE — ONE MAPPING. The `rain`/`snow` console commands and
// the F4 sliders all pass through these two functions; the numbers (0.5 off
// threshold, 8.5 storm threshold, mult = 0.4 + v * 0.42) exist HERE and
// nowhere else. The inverse reads the same pair of cvars back so the slider
// always shows what the console state actually is.
// ===========================================================================
void  applyRainScale(x3::con::IConsole& con, float v0to10);
void  applySnowScale(x3::con::IConsole& con, float v0to10);
float rainScaleFromCVars(x3::con::IConsole& con);   // 0 when wx is not rain/storm
float snowScaleFromCVars(x3::con::IConsole& con);   // 0 when wx is not snow
const char* rainBandName(float v0to10);             // OFF/SPRINKLE/DOWNPOUR/HEAVY/MONSOON
const char* snowBandName(float v0to10);             // OFF/FLURRY/STEADY/HEAVY/WHITEOUT

// Registers the weather console surface: the wx/wx_snow_in/wx_hour/
// wx_precip_mult cvars this host always had, the NEW wx_cloud (-1 = follow
// the weather state) / wx_wind (m/s) / wx_winddir (deg) cvars the F4 panel
// drives, and the `rain` / `snow` 0-10 commands. Shared by the interactive
// host AND the headless proof capture, so both consoles carry the same truth.
void registerWeatherConsole(x3::con::IConsole& con);

// ===========================================================================
// THE LIGHTING VALUE SET — the preset table. kSuggestedLighting is the
// "SUGGESTED SETTINGS" button (and the headless BEFORE/AFTER proof pair);
// kDefaultLighting is "RESTORE DEFAULTS" and matches the registered cvar
// defaults in app/engine_console.cpp (PAIRED — change both or the button
// lies). The flat washed-out ambient the owner keeps reporting is DDGI OFF:
// the suggested set turns the probe-grid GI on with sane intensity/rays,
// keeps RT reflections, and swaps SSAO for ray-traced AO.
// ===========================================================================
struct LightingValues {
    bool  ddgi;
    float ddgiIntensity;
    int   ddgiRays;
    bool  rtReflections;
    bool  rtao;
    bool  ssao;
    float exposure;
};
inline constexpr LightingValues kSuggestedLighting{ true,  1.15f, 96, true, true,  false, 0.88f };
inline constexpr LightingValues kDefaultLighting  { false, 1.0f,  96, true, false, true,  0.88f };

// Write a LightingValues set onto the console (the single source the panel
// reads back) and push it straight to the device, printing every applied
// value to the console + log. `who` names the button/caller in the receipt.
void applyLightingValues(x3::con::IConsole& con, x3::rhi::IRenderDevice* dev,
                         const LightingValues& v, const char* who);

// ===========================================================================
// WorldGameMenu — the ESC menu + F4/F5 panels for a --world host.
// ===========================================================================
class WorldGameMenu {
public:
    enum class Screen : uint8_t { None, Menu, Settings, Weather, Lighting };

    void init(x3::con::IConsole* con, x3::rhi::IRenderDevice* dev) {
        m_con = con; m_dev = dev;
    }

    Screen screen()  const { return m_screen; }
    bool anyOpen()   const { return m_screen != Screen::None; }
    // Menu/Settings freeze the host sim; the F4/F5 panels leave it running.
    bool blocksSim() const { return m_screen == Screen::Menu || m_screen == Screen::Settings; }
    bool panelOpen() const { return m_screen == Screen::Weather || m_screen == Screen::Lighting; }

    void toggleMenu() { m_screen = (m_screen == Screen::None) ? Screen::Menu : Screen::None; }
    void togglePanel(Screen which) { m_screen = (m_screen == which) ? Screen::None : which; }
    void closeAll() { m_screen = Screen::None; }
    // ESC with something open: peel one layer (Settings -> Menu, else close).
    void onEscape() {
        m_screen = (m_screen == Screen::Settings) ? Screen::Menu : Screen::None;
    }

    // One-frame requests raised by menu rows; the host performs them.
    bool takeQuitRequest()    { const bool v = m_wantQuit;    m_wantQuit = false;    return v; }
    bool takeConsoleRequest() { const bool v = m_wantConsole; m_wantConsole = false; return v; }
    bool takeMapRequest()     { const bool v = m_wantMap;     m_wantMap = false;     return v; }

    // Draw + interact the active screen. `in` is the host-assembled input
    // snapshot (mouse + arrow/enter edges); `todHours` is the live in-world
    // clock so the TIME slider sits where the day actually is.
    void draw(const x3::rhi::FrameContext& frame, const x3::ui::UiInput& in,
              float dt, float todHours);

    // The one-click buttons, also callable directly (headless proof pair).
    void applySuggested() { if (m_con) applyLightingValues(*m_con, m_dev, kSuggestedLighting, "SUGGESTED SETTINGS"); }
    void applyDefaults()  { if (m_con) applyLightingValues(*m_con, m_dev, kDefaultLighting,  "RESTORE DEFAULTS");  }

    // CAPTURE STAGING (X3_SHOT_UI=wx): the screen point `frac01` of the way
    // along the weather panel's row `rowIndex` slider track (0 = RAIN), so a
    // headless proof can park a held mouse there and photograph a REAL
    // mid-drag. PAIRED with panelLayout() and sliderEx()'s label/readout
    // reserves in the .cpp — change those, change this.
    static void weatherRowTrackPoint(float w, float h, int rowIndex, float frac01,
                                     float& outX, float& outY);

private:
    void drawMenu(float w, float h);
    void drawSettings(float w, float h);
    void drawWeather(float w, float h, float todHours);
    void drawLighting(float w, float h);
    void seedSettingsFromCVars();
    void applySettingsToCVars();

    x3::con::IConsole*      m_con = nullptr;
    x3::rhi::IRenderDevice* m_dev = nullptr;
    x3::ui::UiContext       m_ui;
    // BOTH menu screens are the GAME'S OWN, reused whole — not copies. The
    // pause screen is x3::ui::PauseMenu (what --screenshot-menu captures) with
    // a world-host PauseRows set; the settings screen is x3::ui::SettingsMenu.
    x3::ui::PauseMenu       m_pauseScreen;
    x3::ui::SettingsMenu    m_settingsScreen;
    x3::ui::SettingsModel   m_model{};
    // The device has no vsync GETTER, so the menu tracks what it last set
    // (worlds boot with vsync on — DeviceDesc default).
    bool m_vsyncOn = true;

    Screen m_screen = Screen::None;
    bool m_wantQuit = false, m_wantConsole = false, m_wantMap = false;
    float m_clock = 0.0f;
};

} // namespace x3::apphost
