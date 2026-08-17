// WORLD GAME MENU + WEATHER / LIGHTING PANELS — see host_menu.h for the brief.
#include "host_menu.h"
#include "world_host_common.h"   // pushLiveHostCVarsToDevice (instant apply)
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x3::apphost {

// ===========================================================================
// The rain/snow scale — ONE mapping (see the header). These numbers are the
// ones the original host-local `rain` command shipped with; the command now
// calls through here, so the slider and the typed command cannot drift.
// ===========================================================================
namespace {
constexpr float kWxOffBelow   = 0.5f;   // v < this -> weather off
constexpr float kWxStormAt    = 8.5f;   // rain v >= this -> storm cell (lightning)
constexpr float kWxMultBase   = 0.4f;   // wx_precip_mult = base + v * slope
constexpr float kWxMultSlope  = 0.42f;

float multFromScale(float v)  { return kWxMultBase + v * kWxMultSlope; }
float scaleFromMult(float m)  {
    return std::clamp((m - kWxMultBase) / kWxMultSlope, 0.0f, 10.0f);
}
} // namespace

void applyRainScale(x3::con::IConsole& con, float v) {
    v = std::clamp(v, 0.0f, 10.0f);
    if (v < kWxOffBelow) { con.set("wx", "off"); return; }
    con.set("wx", v >= kWxStormAt ? "storm" : "rain");
    char mb[32]; std::snprintf(mb, sizeof(mb), "%.2f", (double)multFromScale(v));
    con.set("wx_precip_mult", mb);
}

void applySnowScale(x3::con::IConsole& con, float v) {
    v = std::clamp(v, 0.0f, 10.0f);
    if (v < kWxOffBelow) {
        // Only stand down if snow owns the sky — a snow slider parked at 0
        // must not cancel the rain the other slider just asked for.
        if (con.getString("wx") == "snow") con.set("wx", "off");
        return;
    }
    con.set("wx", "snow");
    char mb[32]; std::snprintf(mb, sizeof(mb), "%.2f", (double)multFromScale(v));
    con.set("wx_precip_mult", mb);
}

float rainScaleFromCVars(x3::con::IConsole& con) {
    const std::string wx = con.getString("wx");
    if (wx != "rain" && wx != "storm") return 0.0f;
    float v = scaleFromMult(con.getFloat("wx_precip_mult"));
    // A forced storm is 8.5+ by definition — keep the readback on the scale.
    if (wx == "storm") v = std::max(v, kWxStormAt);
    return v;
}

float snowScaleFromCVars(x3::con::IConsole& con) {
    if (con.getString("wx") != "snow") return 0.0f;
    return scaleFromMult(con.getFloat("wx_precip_mult"));
}

const char* rainBandName(float v) {
    if (v < kWxOffBelow) return "OFF";
    if (v >= kWxStormAt) return "MONSOON";
    if (v >= 6.5f)       return "HEAVY";
    if (v >= 3.5f)       return "DOWNPOUR";
    return "SPRINKLE";
}

const char* snowBandName(float v) {
    if (v < kWxOffBelow) return "OFF";
    if (v >= kWxStormAt) return "WHITEOUT";
    if (v >= 6.5f)       return "HEAVY";
    if (v >= 3.5f)       return "STEADY";
    return "FLURRY";
}

void registerWeatherConsole(x3::con::IConsole& con) {
    con.registerCVar("wx", "off",
        "weather: off | clear | cloudy | rain | storm | fog | snow");
    con.registerCVar("wx_snow_in", "0",
        "lying snow depth to prime, INCHES (applied when wx changes)");
    con.registerCVar("wx_hour", "14",
        "time of day, 0-24: re-seeds the in-world clock (drives temperature, and "
        "— once you have touched it — the sun)");
    con.registerCVar("wx_precip_mult", "2.4",
        "precipitation density multiplier (raises how much of the particle pool a given rain/snow state uses)");
    // ---- F4-panel lanes (new; the panel is UI over exactly these) ----------
    con.registerCVar("wx_cloud", "-1",
        "cloud cover override 0-1; -1 = follow the weather state (AUTO)");
    con.registerCVar("wx_wind", "0",
        "wind speed, m/s — leans the falling rain/snow columns");
    con.registerCVar("wx_winddir", "225",
        "wind direction, degrees (0 = +X east, 90 = +Z)");

    // Capture the console by POINTER (the reference parameter dies with this
    // call; the console object outlives it — same convention as the host's
    // own registerCommand sites).
    x3::con::IConsole* pc = &con;
    con.registerCommand("rain", [pc](const std::vector<std::string>& args) {
        if (args.empty()) { pc->print("rain 0-10: 0 off | 1-3 sprinkle | 4-6 downpour | 7-8 heavy | 9-10 MONSOON"); return; }
        const float v = std::clamp((float)std::atof(args[0].c_str()), 0.0f, 10.0f);
        applyRainScale(*pc, v);                      // ONE mapping (host_menu.h)
        if (v < kWxOffBelow) { pc->print("rain: off"); return; }
        pc->print(std::string("rain ") + args[0] +
                  (v >= kWxStormAt ? "  (MONSOON - storm cell, lightning live)"
                   : v >= 6.5f ? "  (heavy)" : v >= 3.5f ? "  (downpour)" : "  (sprinkle)"));
    }, "rain 0-10 — Sprinkle to Downpour to MONSOON, with every in-between (F4 slider = same scale)");

    con.registerCommand("snow", [pc](const std::vector<std::string>& args) {
        if (args.empty()) { pc->print("snow 0-10: 0 off | 1-3 flurry | 4-6 steady | 7-8 heavy | 9-10 whiteout (wx_snow_in primes lying depth)"); return; }
        const float v = std::clamp((float)std::atof(args[0].c_str()), 0.0f, 10.0f);
        applySnowScale(*pc, v);                      // ONE mapping (host_menu.h)
        if (v < kWxOffBelow) { pc->print("snow: off"); return; }
        pc->print(std::string("snow ") + args[0] + "  (" + snowBandName(v) + ")");
    }, "snow 0-10 — flurry to whiteout, same scale as rain (F4 slider = same scale)");
}

// ===========================================================================
// Lighting values -> console + device, with receipts.
// ===========================================================================
void applyLightingValues(x3::con::IConsole& con, x3::rhi::IRenderDevice* dev,
                         const LightingValues& v, const char* who) {
    char b[64];
    auto setF = [&](const char* n, float f) {
        std::snprintf(b, sizeof(b), "%.4g", (double)f);
        con.set(n, b);
    };
    con.set("r_ddgi",            v.ddgi ? "1" : "0");
    setF("r_ddgi_intensity",     v.ddgiIntensity);
    std::snprintf(b, sizeof(b), "%d", v.ddgiRays); con.set("r_ddgi_rays", b);
    con.set("r_rtreflections",   v.rtReflections ? "1" : "0");
    con.set("r_rtao",            v.rtao ? "1" : "0");
    con.set("r_ssao",            v.ssao ? "1" : "0");
    setF("r_exposure",           v.exposure);

    // PRINT WHAT WAS APPLIED (the owner asked for exactly this). Console AND
    // log — a capture of the panel does not survive cropping, the log does.
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%s: r_ddgi %d | r_ddgi_intensity %.2f | r_ddgi_rays %d | "
                  "r_rtreflections %d | r_rtao %d | r_ssao %d | r_exposure %.2f",
                  who, v.ddgi ? 1 : 0, (double)v.ddgiIntensity, v.ddgiRays,
                  v.rtReflections ? 1 : 0, v.rtao ? 1 : 0, v.ssao ? 1 : 0,
                  (double)v.exposure);
    con.print(line);
    x3::logInfo(std::string("[lighting] ") + line);

    // Straight to the device — the click must be visible THIS frame, not
    // whenever the shell's 15-frame cvar hash next rolls over.
    if (dev) pushLiveHostCVarsToDevice(con, *dev);
}

// ===========================================================================
// WorldGameMenu
// ===========================================================================
namespace {
// The menu's palette (matches app/ui.cpp's module palette — same look).
constexpr float kPanelCol[4] = { 0.05f, 0.06f, 0.08f, 0.92f };
constexpr float kTitleCol[4] = { 0.40f, 0.88f, 1.0f, 1.0f };
constexpr float kHintCol[4]  = { 0.55f, 0.58f, 0.64f, 1.0f };
constexpr float kDimCol[4]   = { 0.0f, 0.0f, 0.0f, 0.55f };
constexpr float kNoteCol[4]  = { 0.72f, 0.86f, 0.94f, 1.0f };
} // namespace

void WorldGameMenu::draw(const x3::rhi::FrameContext& frame, const x3::ui::UiInput& in,
                         float dt, float todHours) {
    if (m_screen == Screen::None || !m_dev || !m_con) return;
    m_clock += dt;
    m_ui.begin(*m_dev, frame, in);
    const float w = (float)m_ui.screenW(), h = (float)m_ui.screenH();
    if (w > 0.0f && h > 0.0f) {
        switch (m_screen) {
            case Screen::Menu:     drawMenu(w, h);               break;
            case Screen::Settings: drawSettings(w, h);           break;
            case Screen::Weather:  drawWeather(w, h, todHours);  break;
            case Screen::Lighting: drawLighting(w, h);           break;
            default: break;
        }
    }
    m_ui.end();
}

void WorldGameMenu::drawMenu(float w, float h) {
    m_ui.quad(0, 0, w, h, kDimCol);

    const float pw = std::min(440.0f, w * 0.55f);
    const float bh = std::max(38.0f, h * 0.062f);
    const float gap = bh * 0.24f;
    const float titlePx = std::max(24.0f, pw / 14.0f);
    const int   rows = 7;
    const float ph = titlePx + 22.0f + rows * bh + (rows - 1) * gap + 74.0f;
    const float px = w * 0.5f - pw * 0.5f;
    const float py = h * 0.5f - ph * 0.5f;

    m_ui.panel(px, py, pw, ph, kPanelCol);
    m_ui.textCentered("PAUSED", w * 0.5f, py + 22.0f, titlePx, kTitleCol,
                      x3::ui::UiContext::FontRole::Title);

    const float bw = pw - 48.0f;
    float by = py + 22.0f + titlePx + 22.0f;
    auto row = [&](const char* label) {
        const bool hit = m_ui.button(label, px + 24.0f, by, bw, bh);
        by += bh + gap;
        return hit;
    };

    if (row("RESUME"))                 m_screen = Screen::None;
    if (row("WEATHER PANEL   (F4)"))   m_screen = Screen::Weather;
    if (row("LIGHTING PANEL  (F5)"))   m_screen = Screen::Lighting;
    if (row("SETTINGS")) { seedSettingsFromCVars(); m_screen = Screen::Settings; }
    if (row("WORLD MAP       (M)"))  { m_wantMap = true;     m_screen = Screen::None; }
    if (row("CONSOLE         (~)"))  { m_wantConsole = true; m_screen = Screen::None; }
    if (row("QUIT TO DESKTOP"))        m_wantQuit = true;

    m_ui.textCentered("ESC resumes  -  the sim is stopped  -  F4/F5 panels tune LIVE while you drive",
                      w * 0.5f, by + 8.0f, std::max(11.0f, h * 0.015f), kHintCol);
}

void WorldGameMenu::seedSettingsFromCVars() {
    if (!m_con) return;
    m_model = x3::ui::SettingsModel{};
    m_model.bloom   = m_con->getInt("r_bloom") != 0;
    m_model.ssao    = m_con->getInt("r_ssao") != 0;
    m_model.ssgi    = m_con->getInt("r_ssgi") != 0;
    // In a --world host "shadows" IS the cascaded sun-shadow chain (r_csm) —
    // there is no campaign r_shadows consumer here.
    m_model.shadows = m_con->getInt("r_csm") != 0;
    m_model.rtao    = m_con->getInt("r_rtao") != 0;
    m_model.vsync   = m_vsyncOn;
    // Rows with no consumer in a world host DO NOT DRAW (NO_SLOP rule 6):
    // no music system, no spaceflight, no settings file, no dev intro.
    m_model.showAudio      = false;
    m_model.showFlightMode = false;
    m_model.showAdvanced   = false;
    m_model.showSetDefault = false;
    if (m_dev) { uint32_t dw = 0, dh = 0; m_dev->hudSize(dw, dh); m_model.dispW = dw; m_model.dispH = dh; }
}

void WorldGameMenu::applySettingsToCVars() {
    if (!m_con) return;
    m_con->set("r_bloom", m_model.bloom ? "1" : "0");
    m_con->set("r_ssao",  m_model.ssao  ? "1" : "0");
    m_con->set("r_ssgi",  m_model.ssgi  ? "1" : "0");
    m_con->set("r_csm",   m_model.shadows ? "1" : "0");
    m_con->set("r_rtao",  m_model.rtao  ? "1" : "0");
    if (m_dev) {
        m_dev->setVsync(m_model.vsync);
        m_vsyncOn = m_model.vsync;
        pushLiveHostCVarsToDevice(*m_con, *m_dev);   // visible THIS frame
    }
}

void WorldGameMenu::drawSettings(float w, float h) {
    if (m_dev) { uint32_t dw = 0, dh = 0; m_dev->hudSize(dw, dh); m_model.dispW = dw; m_model.dispH = dh; }
    bool changed = false;
    // The REAL main-game settings screen (app/ui.cpp), reused whole. `back`
    // is spelled Paused in its vocabulary; we map it onto our Menu screen.
    const x3::ui::GameState next =
        m_settingsScreen.update(m_ui, m_model, x3::ui::GameState::Paused, changed);
    if (changed) applySettingsToCVars();
    if (next != x3::ui::GameState::Settings) m_screen = Screen::Menu;
}

// ---------------------------------------------------------------------------
// The F4 / F5 panels. Left-anchored so the gauge cluster (bottom-right) and
// the minimap (top-right) stay visible while you tune — the world is the
// preview, and the panel must never cover the thing it is changing.
// ---------------------------------------------------------------------------
namespace {
struct PanelLayout {
    float px, py, pw, rh, gap, rx, rw;
    float rowY(int i) const { return py + 58.0f + (rh + gap) * (float)i; }
};
PanelLayout panelLayout(float w, float h, int rows) {
    PanelLayout L{};
    L.pw  = std::min(520.0f, w * 0.42f);
    L.rh  = std::max(34.0f, h * 0.045f);
    L.gap = L.rh * 0.22f;
    const float ph = 58.0f + rows * (L.rh + L.gap) + 56.0f;
    L.px = 18.0f;
    L.py = std::max(60.0f, h * 0.5f - ph * 0.5f);
    L.rx = L.px + 16.0f;
    L.rw = L.pw - 32.0f;
    return L;
}
} // namespace

void WorldGameMenu::weatherRowTrackPoint(float w, float h, int rowIndex, float frac01,
                                         float& outX, float& outY) {
    // PAIRED NUMBERS: 6+1 rows = drawWeather's layout call; 150/132/16 are
    // sliderEx()'s labelW / readout reserve / right inset (app/ui.cpp).
    PanelLayout L = panelLayout(w, h, 6 + 1);
    const float trackX = L.rx + 150.0f;
    const float trackR = L.rx + L.rw - 132.0f - 16.0f;
    outX = trackX + (trackR - trackX) * std::clamp(frac01, 0.0f, 1.0f);
    outY = L.rowY(rowIndex) + L.rh * 0.5f;
}

void WorldGameMenu::drawWeather(float w, float h, float todHours) {
    auto& con = *m_con;
    const int kRows = 6;                       // rain snow cloud wind dir time
    PanelLayout L = panelLayout(w, h, kRows + 1);   // +1: the AUTO/off button row
    const float ph = 58.0f + (kRows + 1) * (L.rh + L.gap) + 56.0f;
    m_ui.panel(L.px, L.py, L.pw, ph, kPanelCol);
    m_ui.text("WEATHER", L.px + 16.0f, L.py + 16.0f, 22.0f, kTitleCol,
              x3::ui::UiContext::FontRole::Title);
    {   // current state, right of the title — the cvar IS the state.
        const std::string wx = con.getString("wx");
        m_ui.text(("wx: " + wx).c_str(), L.px + L.pw - 16.0f -
                  x3::ui::UiContext::textWidth(x3::ui::UiContext::FontRole::HudMono,
                                               ("wx: " + wx).c_str(), 15.0f),
                  L.py + 22.0f, 15.0f, kNoteCol, x3::ui::UiContext::FontRole::HudMono);
    }

    char ro[48];
    int r = 0;

    // RAIN 0-10 — the owner's scale. Slider -> the same mapping as `rain N`.
    {
        float v = rainScaleFromCVars(con);
        std::snprintf(ro, sizeof(ro), "%.1f %s", (double)v, rainBandName(v));
        if (m_ui.sliderEx("RAIN", v, 0.0f, 10.0f, 0.5f, ro, L.rx, L.rowY(r), L.rw, L.rh))
            applyRainScale(con, v);
    }
    ++r;
    // SNOW 0-10 — same scale, same mapping discipline.
    {
        float v = snowScaleFromCVars(con);
        std::snprintf(ro, sizeof(ro), "%.1f %s", (double)v, snowBandName(v));
        if (m_ui.sliderEx("SNOW", v, 0.0f, 10.0f, 0.5f, ro, L.rx, L.rowY(r), L.rw, L.rh))
            applySnowScale(con, v);
    }
    ++r;
    // CLOUD COVER 0-100% — wx_cloud; -1 = AUTO (follow the weather state).
    {
        const float cv = con.getFloat("wx_cloud");
        float v = (cv < 0.0f) ? 0.42f : cv;    // AUTO parks at the demo default
        std::snprintf(ro, sizeof(ro), cv < 0.0f ? "AUTO" : "%d%%",
                      (int)(v * 100.0f + 0.5f));
        if (m_ui.sliderEx("CLOUD", v, 0.0f, 1.0f, 0.05f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.2f", (double)v);
            con.set("wx_cloud", b);
        }
    }
    ++r;
    // WIND 0-25 m/s — leans the falling rain/snow columns (precip_fx wind).
    {
        float v = con.getFloat("wx_wind");
        std::snprintf(ro, sizeof(ro), "%.0f m/s", (double)v);
        if (m_ui.sliderEx("WIND", v, 0.0f, 25.0f, 1.0f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.0f", (double)v);
            con.set("wx_wind", b);
        }
    }
    ++r;
    {
        float v = con.getFloat("wx_winddir");
        std::snprintf(ro, sizeof(ro), "%.0f deg", (double)v);
        if (m_ui.sliderEx("WIND DIR", v, 0.0f, 360.0f, 15.0f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.0f", (double)v);
            con.set("wx_winddir", b);
        }
    }
    ++r;
    // TIME OF DAY — the slider sits where the in-world clock actually is
    // (it advances at 10 real minutes per day); dragging re-seeds wx_hour.
    {
        float v = todHours;
        std::snprintf(ro, sizeof(ro), "%02d:%02d", (int)v,
                      (int)((v - std::floor(v)) * 60.0f) % 60);
        if (m_ui.sliderEx("TIME", v, 0.0f, 24.0f, 0.25f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.2f", (double)v);
            con.set("wx_hour", b);
        }
    }
    ++r;
    {   // the button row: cloud back to AUTO / everything off.
        const float half = (L.rw - 10.0f) * 0.5f;
        if (m_ui.button("CLOUD: AUTO", L.rx, L.rowY(r), half, L.rh))
            con.set("wx_cloud", "-1");
        if (m_ui.button("ALL WEATHER OFF", L.rx + half + 10.0f, L.rowY(r), half, L.rh)) {
            con.set("wx", "off");
            con.set("wx_cloud", "-1");
            con.set("wx_wind", "0");
        }
    }

    m_ui.text("drag with mouse - arrows nudge - the world stays LIVE while you tune",
              L.rx, L.py + ph - 44.0f, 12.0f, kHintCol);
    m_ui.text("F4 closes - rain 0 SPRINKLE ---- 5 DOWNPOUR ---- 10 MONSOON",
              L.rx, L.py + ph - 26.0f, 12.0f, kHintCol);
}

void WorldGameMenu::drawLighting(float w, float h) {
    auto& con = *m_con;
    const int kRows = 9;   // 5 toggles + 3 sliders + 1 button row
    PanelLayout L = panelLayout(w, h, kRows);
    const float ph = 58.0f + kRows * (L.rh + L.gap) + 56.0f;
    m_ui.panel(L.px, L.py, L.pw, ph, kPanelCol);
    m_ui.text("LIGHTING", L.px + 16.0f, L.py + 16.0f, 22.0f, kTitleCol,
              x3::ui::UiContext::FontRole::Title);

    // Every row reads the cvar and writes the cvar — the console and this
    // panel are two views of one value (NO_SLOP rule 4). Toggles push to the
    // device immediately so a click is visible THIS frame.
    auto pushNow = [&] { if (m_dev) pushLiveHostCVarsToDevice(con, *m_dev); };
    auto toggleCVar = [&](const char* label, const char* cvar, int rowI) {
        const bool on = con.getInt(cvar) != 0;
        if (m_ui.toggle(label, on, L.rx, L.rowY(rowI), L.rw, L.rh)) {
            con.set(cvar, on ? "0" : "1");
            con.print(std::string(cvar) + " = " + (on ? "0" : "1"));
            pushNow();
        }
    };

    char ro[32];
    int r = 0;
    toggleCVar("DDGI GI (ray traced)", "r_ddgi", r++);
    {
        float v = con.getFloat("r_ddgi_intensity");
        std::snprintf(ro, sizeof(ro), "%.2f", (double)v);
        if (m_ui.sliderEx("GI INTENSITY", v, 0.0f, 3.0f, 0.05f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.2f", (double)v);
            con.set("r_ddgi_intensity", b); pushNow();
        }
    }
    ++r;
    {
        float v = (float)con.getInt("r_ddgi_rays");
        if (v <= 0.0f) v = 96.0f;
        std::snprintf(ro, sizeof(ro), "%d", (int)v);
        if (m_ui.sliderEx("GI RAYS/PROBE", v, 16.0f, 128.0f, 8.0f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%d", (int)v);
            con.set("r_ddgi_rays", b); pushNow();
        }
    }
    ++r;
    toggleCVar("RT REFLECTIONS",       "r_rtreflections", r++);
    toggleCVar("RT AMBIENT OCCLUSION", "r_rtao",          r++);
    toggleCVar("SSAO (screen space)",  "r_ssao",          r++);
    toggleCVar("BLOOM",                "r_bloom",         r++);
    {
        float v = con.getFloat("r_exposure");
        std::snprintf(ro, sizeof(ro), "%.2f", (double)v);
        if (m_ui.sliderEx("EXPOSURE", v, 0.30f, 2.00f, 0.02f, ro, L.rx, L.rowY(r), L.rw, L.rh)) {
            char b[16]; std::snprintf(b, sizeof(b), "%.2f", (double)v);
            con.set("r_exposure", b); pushNow();
        }
    }
    ++r;
    {   // THE BUTTONS. Suggested = DDGI on with sane intensity/rays + RT AO
        // (the flat washed-out ambient is DDGI OFF); Restore = the registered
        // cvar defaults. Both print their applied values to the console.
        const float half = (L.rw - 10.0f) * 0.5f;
        if (m_ui.button("SUGGESTED SETTINGS", L.rx, L.rowY(r), half, L.rh))
            applySuggested();
        if (m_ui.button("RESTORE DEFAULTS", L.rx + half + 10.0f, L.rowY(r), half, L.rh))
            applyDefaults();
    }

    m_ui.text("SUGGESTED = probe-grid GI on: the fix for the flat washed-out ambient",
              L.rx, L.py + ph - 44.0f, 12.0f, kHintCol);
    m_ui.text("applied values print to the console (~) - F5 closes",
              L.rx, L.py + ph - 26.0f, 12.0f, kHintCol);
}

} // namespace x3::apphost
