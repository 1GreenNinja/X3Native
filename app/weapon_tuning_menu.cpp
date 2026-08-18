// THE TUNING PANEL (F7). See app/weapon_tuning_menu.h for why it exists.
//
// Built entirely from the game's OWN widget layer — UiContext's buttons,
// sliders, tabs, collapsing headers, tooltips and colour editor — on the shared
// hud_panel.h glass. Dear ImGui is not linked; its interaction design is (see
// app/ui_widgets.cpp).
#include "weapon_tuning_menu.h"

#include "fx.h"            // the ONE dial table + weaponFxKindName/weaponFlashCVar
#include "hud_panel.h"     // shared glass palette, spacing scale, live glass tuning
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace x3::game {

namespace {

using UiContext = x3::ui::UiContext;
using Rect      = x3::ui::Rect;

// ---- Geometry ---------------------------------------------------------------
// 480 px is not a taste number: UiContext::sliderEx reserves a FIXED 150 px label
// cell and a FIXED 132 px readout cell, so a row narrower than ~420 px leaves the
// track too short to drag w_tracer_speed across 5..2000 with any precision. This
// is the narrowest panel that keeps a ~100 px track while still leaving the
// screen centre (the crosshair) and the lower-right (the muzzle flash) clear.
constexpr float kPanelW  = 480.0f;
constexpr float kRowH    = 20.0f;   // weapon button
constexpr float kDialH   = 24.0f;   // slider row
constexpr float kResetW  = 46.0f;   // the per-dial RESET chip
// THREE columns, not two. The canon roster is 12 weapons: at two columns that is
// six rows, which pushed the panel past the safe band and ran the FX dials off
// the bottom of their own plate (caught in the first 720p capture). Three
// columns is four rows and still leaves each cell wide enough for "flamethrower".
constexpr int   kCols    = 3;       // weapon grid columns

// A caption row for text of cap size `px`. A HUD glyph inks well past its
// nominal cap height (see hud_panel.h hudLineH), so a 10 px caption in a 12 px
// row prints into the row below it — which is exactly what the first capture
// showed under the weapon grid.
inline float capRow(float px) { return x3::game::hudLineH(px); }

// Largest cap size <= basePx at which `s` fits `maxW` in the mono face.
//
// Captions are the one place a HUD string is authored as PROSE, and prose grows
// with every edit. Measuring beats guessing: a caption that overruns the plate
// prints onto bare scene, which over a bright vantage is unreadable — the very
// failure the glass exists to prevent. Bounded loop, half-point steps, floor 6.5
// (below that the mono face stops being legible and the caption should be cut
// instead).
float fitPx(const char* s, float maxW, float basePx) {
    float px = basePx;
    for (int i = 0; i < 32 && px > 6.5f; ++i) {
        if (UiContext::textWidth(UiContext::FontRole::HudMono, s, px) <= maxW) break;
        px -= 0.5f;
    }
    return px;
}

// Draw a caption that is GUARANTEED to sit inside the content column, and
// advance the layout by the leading its final size actually needs.
void caption(UiContext& ui, const char* s, float basePx, const float* col) {
    const float px = fitPx(s, ui.layoutW(), basePx);
    const Rect r = ui.row(capRow(px));
    ui.text(s, r.x, r.y, px, col, UiContext::FontRole::HudMono);
}

// Vertical no-go bands, so the panel never lands on the left HUD stack. Top: the
// FPS chip (y=4, ~26 tall) plus the objective + enemies panels stacked under it.
// Bottom: the HP bar plate (its top edge sits at h - 58).
constexpr float kSafeTop = 196.0f;   // clears a THREE-LINE objective, the worst case
constexpr float kSafeBot = 66.0f;

constexpr float kDim[4] = { 0.55f, 0.62f, 0.68f, 1.00f };

const char* const kTabs[] = { "WEAPONS", "HUD GLASS" };
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

// The weapon-FX dials, in panel order. The per-weapon flash row is second —
// right under the arc thickness, because those two are what Tim actually drags.
struct DialRow { const WeaponFxDial* spec; bool perWeapon; };
const DialRow kFxRows[] = {
    { &kDialLightningThickness, false },
    { &kDialFlashKind,          true  },   // cvar name resolved per selected weapon
    { &kDialTracerLen,          false },
    { &kDialTracerSpeed,        false },
};
constexpr int kFxRowCount = (int)(sizeof(kFxRows) / sizeof(kFxRows[0]));

} // namespace

// ---------------------------------------------------------------------------
// One slider row: read the cvar, draw it, write any change straight back, offer
// RESET, and hang the console's own registered help text on it as a tooltip.
//
// Writing the CVAR (not fxTuning() directly) is the whole point — that is the
// path applyWeaponFxCVars() already syncs every frame, so the drag lands on the
// next frame AND the console shows the same number the panel does.
// ---------------------------------------------------------------------------
bool WeaponTuningMenu::dial(x3::ui::UiContext& ui, x3::con::IConsole& console,
                            const WeaponFxDial& spec, const char* cvarName,
                            float x, float y, float w, float h) {
    // An UNREGISTERED cvar reads as an empty string (a bare host without the
    // engine catalog). Show the shipped default rather than a false 0 — the same
    // "leave it alone" rule applyWeaponFxCVars uses.
    const std::string cur = console.getString(cvarName);
    float v = cur.empty() ? spec.def : console.getFloat(cvarName);
    v = std::clamp(v, spec.lo, spec.hi);

    const std::string readout = formatDialValue(v);
    float nv = v;
    const bool changed = ui.sliderEx(cvarName, nv, spec.lo, spec.hi, spec.step,
                                     readout.c_str(), x, y, w, h);
    // SELF-DOCUMENTING: the hover text is the string registerCVar() was given,
    // so the panel and `help <cvar>` can never say different things.
    const std::string tip = console.cvarHelp(cvarName);
    if (!tip.empty()) ui.tooltip(tip.c_str());
    if (changed) console.set(cvarName, formatDialValue(nv));

    // RESET: back to the dial table's default — the SAME number engine_console.cpp
    // registered the cvar with, so "reset" and "never touched it" are identical.
    if (ui.button("RESET", x + w + 6.0f, y, kResetW, h))
        console.set(cvarName, dialDefaultString(spec));

    return changed;
}

// ---------------------------------------------------------------------------
int WeaponTuningMenu::draw(x3::ui::UiContext& ui, x3::con::IConsole& console, float dt) {
    if (!m_open) return 0;

    m_clock += dt;                       // dt-scaled: the pulse beats the same at 60 and 165 Hz
    if (m_clock > 1.0e6f) m_clock = 0.0f;

    const int n    = std::max(0, m_src.count);
    const int held = (m_src.held    && n > 0) ? m_src.held()    : -1;
    const int soak = (m_src.soaking && n > 0) ? m_src.soaking() : -1;

    // SELECTION follows the world unless the user has picked: on open (and while
    // untouched) it is whatever is soaking, else whatever is in hand — so the
    // per-weapon flash slider is always about the gun you are looking at.
    if (m_justOpened || m_sel < 0 || m_sel >= n) {
        m_sel = (soak >= 0) ? soak : held;
        m_justOpened = false;
    }

    const float W = (float)ui.screenW();
    const float H = (float)ui.screenH();

    // ---- The plate. Its height is what the CONTENT measured last frame (see
    // m_lastContentH): a panel with collapsing sections and tabs has no fixed
    // height, and a hard-coded total is a number that goes wrong the first time
    // anyone adds a row. One frame of lag on a section toggle is invisible; a
    // plate that does not fit its rows is not.
    const float pw = std::min(kPanelW, std::max(260.0f, W - 2.0f * x3::game::kHudMargin));
    // The panel is placed to sit inside the safe band, but its HEIGHT is capped
    // only by the screen (a 24 px tail margin). Clipping the panel to the band
    // cut its own footer off — and a HUD instrument that hides its last row is
    // worse than one that briefly overlaps the HP plate on a short window.
    const float ph = std::clamp(m_lastContentH, 120.0f, std::max(120.0f, H - kSafeTop - 24.0f));
    const float px = x3::game::kHudMargin;
    const float py = std::clamp(H * 0.5f - ph * 0.5f, kSafeTop,
                                std::max(kSafeTop, H - kSafeBot - ph));

    // The SHARED near-black glass (live hud_glass_* fill, tiny shared radius) with
    // an amber accent bar — amber because this is an instrument that CHANGES the
    // game, not a status readout.
    ui.panel(px, py, pw, ph, x3::game::hudPanelTuning().fill, x3::game::kHudAccentAmber);

    const float cx = px + x3::game::kHudPadX;
    const float cw = pw - 2.0f * x3::game::kHudPadX;
    const float top = py + x3::game::kHudPadY;
    ui.beginLayout(cx, top, cw, kRowH, 4.0f);

    // ---- Title -------------------------------------------------------------
    {
        const Rect r = ui.row(capRow(15.0f));
        ui.text("TUNING", r.x, r.y, 17.0f, x3::game::kHudAccentAmber,
                UiContext::FontRole::Title);
        const char* hint = "[F7] CLOSE";
        const float hw = UiContext::textWidth(UiContext::FontRole::HudMono, hint, 10.0f);
        ui.text(hint, r.x + r.w - hw, r.y + 5.0f, 10.0f, kDim, UiContext::FontRole::HudMono);
    }

    // ---- Tabs. Weapons is the first page, not the only one. -----------------
    ui.tabBar(kTabs, kTabCount, m_tab, 22.0f);
    ui.spacing(4.0f);

    if (m_tab == 0) {
        // ================= WEAPONS =========================================
        // SOAK STATUS — the "it is firing right now" reminder. The soak
        // deliberately keeps running while this panel is open (that is the entire
        // workflow), so the panel owes the user an unmissable statement of it.
        {
            const Rect r = ui.row(capRow(13.0f));
            char line[128];
            const float* col = kDim;
            float alpha = 1.0f;
            if (soak >= 0 && soak < n && m_src.name) {
                std::snprintf(line, sizeof(line), "SOAK: %s  -  FIRING",
                              m_src.name(soak).c_str());
                col = x3::game::kHudAccentGreen;
                // Slow breathing pulse (dt-driven) so it reads as live, not painted.
                alpha = 0.68f + 0.32f * (0.5f + 0.5f * std::sin(m_clock * 5.0f));
            } else {
                std::snprintf(line, sizeof(line), "SOAK: off");
            }
            const float c[4] = { col[0], col[1], col[2], col[3] * alpha };
            ui.text(line, r.x, r.y, fitPx(line, r.w, 13.0f), c,
                    UiContext::FontRole::HudMono);
        }
        caption(ui, n > 0 ? "click a weapon to soak it - click again to stop"
                          : "no weapon roster here - the FX dials are still live",
                10.0f, kDim);
        ui.spacing(4.0f);

        // ---- Weapon roster -------------------------------------------------
        if (ui.collapsingHeader("WEAPONS", m_openWeapons)) {
            if (n > 0 && m_src.name) {
                const int gridRows = (n + kCols - 1) / kCols;
                for (int gr = 0; gr < gridRows; ++gr) {
                    const Rect r = ui.row(kRowH);
                    const float colW = (r.w - 8.0f) / (float)kCols;
                    for (int c = 0; c < kCols; ++c) {
                        const int i = gr * kCols + c;
                        if (i >= n) break;
                        const float bx = r.x + (float)c * (colW + 8.0f);
                        const std::string nm = m_src.name(i);

                        // The SOAK toggle IS the weapon row: activating it runs the
                        // host's `wtest <name>`, itself a toggle (same weapon twice
                        // = off). The panel never pins a trigger of its own.
                        if (ui.button(nm.c_str(), bx, r.y, colW, r.h)) {
                            m_sel = i;
                            if (m_src.toggleSoak) m_src.toggleSoak(i);
                        }
                        if (i == soak) ui.tooltip("SOAKING - firing continuously. Click to stop.");
                        else           ui.tooltip("Click to soak: hold this weapon's trigger down continuously (the `wtest` command).");

                        // State bars, drawn OVER the button: green = soaking (this
                        // one is firing), cyan = in hand. Bars rather than text so
                        // they never collide with the button's centred label.
                        if (i == soak)      ui.quad(bx, r.y, 3.0f, r.h, x3::game::kHudAccentGreen);
                        else if (i == held) ui.quad(bx, r.y, 3.0f, r.h, x3::game::kHudAccentCyan);
                        if (i == m_sel)
                            ui.quad(bx, r.y + r.h - 2.0f, colW, 2.0f, x3::game::kHudAccentAmber);
                    }
                }
                caption(ui, "green SOAKING   cyan IN HAND   amber SELECTED",
                        9.5f, kDim);
            } else {
                const Rect r = ui.row(16.0f);
                ui.text("(no weapon roster in this world)", r.x, r.y, 11.0f, kDim,
                        UiContext::FontRole::HudMono);
            }
        }

        // ---- The live FX dials ---------------------------------------------
        if (ui.collapsingHeader("FX DIALS", m_openDials)) {
            caption(ui, "drag, arrow-nudge, or ctrl+click a value to type it",
                    9.5f, kDim);
            for (int i = 0; i < kFxRowCount; ++i) {
                const WeaponFxDial& spec = *kFxRows[i].spec;
                std::string name;
                if (kFxRows[i].perWeapon) {
                    // THE ANTI-DRIFT PROPERTY: the per-weapon muzzle-flash cvar
                    // name is built from weaponFxKindName() via weaponFlashCVar(),
                    // exactly like the registration and the per-frame sync. The
                    // panel can never offer a w_flash_* row that does not exist,
                    // nor miss one the enum gained.
                    const int kind = (m_sel >= 0 && m_src.fxKind) ? m_src.fxKind(m_sel) : 0;
                    name = weaponFlashCVar(kind);
                    if (name.empty()) name = weaponFlashCVar(0);
                } else {
                    name = spec.cvar ? spec.cvar : "";
                }
                if (name.empty()) continue;
                const Rect r = ui.rowSplitPx(kResetW, kDialH);
                dial(ui, console, spec, name.c_str(), r.x, r.y, r.w, r.h);
            }
        }
    } else {
        // ================= HUD GLASS =======================================
        // The colour picker's real target. The glass fill and the corner radius
        // were BOTH settled by rebuild-and-look cycles; they are cvars now, so
        // they are settled under the cursor instead.
        caption(ui, "the plate under every HUD block, menu and console",
                10.0f, kDim);
        ui.spacing(2.0f);

        // Read the live tuning, edit it, write any change back to the cvars (the
        // per-frame sync hub then pulls them into hudPanelTuning() — so the panel
        // edits the same values the console does, never a private copy).
        float glass[4] = { x3::game::hudPanelTuning().fill[0],
                           x3::game::hudPanelTuning().fill[1],
                           x3::game::hudPanelTuning().fill[2],
                           x3::game::hudPanelTuning().fill[3] };
        if (ui.colorEdit4("PANEL GLASS", glass, true)) {
            static const char* kNames[4] = { "hud_glass_r", "hud_glass_g",
                                             "hud_glass_b", "hud_glass_a" };
            // THREE decimals, not formatDialValue's two: a colour channel steps by
            // 0.005, and at two decimals every nudge rounds straight back to the
            // value it started from — the slider would move and nothing would
            // change. (The glass fill is near-black, so the low bits are the
            // whole tint.)
            for (int c = 0; c < 4; ++c) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.3f", (double)glass[c]);
                console.set(kNames[c], buf);
            }
        }

        ui.spacing(4.0f);
        {
            // The corner radius, live, for the WHOLE panel family at once.
            const Rect r = ui.rowSplitPx(kResetW, kDialH);
            float rad = x3::game::hudPanelTuning().radius;
            const std::string ro = formatDialValue(rad);
            if (ui.sliderEx("hud_radius", rad, 0.0f, 24.0f, 1.0f, ro.c_str(),
                            r.x, r.y, r.w, r.h))
                console.set("hud_radius", formatDialValue(rad));
            const std::string tip = console.cvarHelp("hud_radius");
            if (!tip.empty()) ui.tooltip(tip.c_str());
            if (ui.button("RESET", ui.rest())) console.set("hud_radius", "3");
        }
        caption(ui, "below ~0.86 alpha light text washes out on bright scenes",
                9.5f, kDim);
    }

    // ---- Footer -------------------------------------------------------------
    ui.spacing(2.0f);
    caption(ui, "these ARE the cvars - the console shares them", 9.5f, kDim);

    // Measure for NEXT frame's plate (see the plate comment above).
    m_lastContentH = (ui.cursorY() - top) + x3::game::kHudPadY * 2.0f;

    return ui.focusCount();
}

// ===========================================================================
// Headless self-test (folded into --test-ui). See the contract in the header.
// ===========================================================================
namespace {

// A 3-weapon synthetic roster, deliberately NOT canonlevel's arsenal: the panel
// is shared infrastructure, so the gate must prove it builds from any host's
// roster, not from one particular game's.
struct FakeRoster {
    std::vector<std::string> names{ "pistol", "lightning", "flamethrower" };
    std::vector<int>         kinds{ (int)WeaponFxKind::Pistol,
                                    (int)WeaponFxKind::Lightning,
                                    (int)WeaponFxKind::Flame };
    int held = 0;
    int soak = -1;
    int lastToggle = -1;
    int toggleCount = 0;
};

WeaponTuningSource sourceFor(FakeRoster& r) {
    WeaponTuningSource s;
    s.count   = (int)r.names.size();
    s.name    = [&r](int i) { return r.names[(size_t)i]; };
    s.fxKind  = [&r](int i) { return r.kinds[(size_t)i]; };
    s.held    = [&r]()      { return r.held; };
    s.soaking = [&r]()      { return r.soak; };
    // Stands in for the host's `console.exec("wtest " + name)`: the panel must
    // call this and nothing else — it must never pin a trigger itself.
    s.toggleSoak = [&r](int i) {
        r.lastToggle = i; ++r.toggleCount;
        r.soak = (r.soak == i) ? -1 : i;
    };
    return s;
}

int pump(WeaponTuningMenu& m, x3::ui::UiContext& ui, x3::rhi::IRenderDevice& dev,
         x3::con::IConsole& con, const x3::ui::UiInput& in) {
    x3::rhi::FrameContext fc{};
    ui.begin(dev, fc, in);
    m.draw(ui, con, 1.0f / 60.0f);
    ui.end();
    // AFTER end(): focusCount() is finalised there, so draw()'s own return value
    // is the PREVIOUS frame's tally. Read it here or the first frame reports 0.
    return ui.focusCount();
}

// Walk keyboard focus to `target` by pumping navDown, then leave focus there.
void focusTo(WeaponTuningMenu& m, x3::ui::UiContext& ui, x3::rhi::IRenderDevice& dev,
             x3::con::IConsole& con, int target) {
    x3::ui::UiInput in{}; in.mouseX = 9000.0f; in.mouseY = 9000.0f;
    pump(m, ui, dev, con, in);              // settle
    int guard = 0;
    while (ui.focus() != target && guard++ < 128) {
        in.navDown = true;
        pump(m, ui, dev, con, in);
    }
}

} // namespace

bool runWeaponTuningMenuSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++pass; x3::logInfo(std::string("  PASS ") + name); }
        else    { ++fail; x3::logError(std::string("  FAIL ") + name); }
    };

    x3::game::HeadlessRenderDevice dev;
    x3::con::IConsole* con = x3::con::createConsole();
    // Register the SAME cvar set the game registers, through the same table.
    con->registerCVar(kDialLightningThickness.cvar, dialDefaultString(kDialLightningThickness),
                      "lightning arc thickness scale");
    con->registerCVar(kDialTracerLen.cvar,   dialDefaultString(kDialTracerLen),   "bullet streak length");
    con->registerCVar(kDialTracerSpeed.cvar, dialDefaultString(kDialTracerSpeed), "bullet streak speed");
    for (int k = 0; k < kWeaponFxKindCount; ++k)
        con->registerCVar(weaponFlashCVar(k), dialDefaultString(kDialFlashKind), "muzzle flash size");
    registerHudPanelCVars(*con);

    const float kOff = 9000.0f;   // park the mouse so hover never claims focus

    // ---- WG1..3: open/close ------------------------------------------------
    {
        WeaponTuningMenu m;
        check(!m.isOpen(), "WG1 panel starts closed");
        m.toggle();
        check(m.isOpen(), "WG2 toggle opens the panel");
        m.toggle();
        check(!m.isOpen(), "WG3 toggle closes the panel");
    }

    // ---- WG4: a closed panel emits nothing (costs no focus slots). ----------
    {
        WeaponTuningMenu m; x3::ui::UiContext ui;
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        check(pump(m, ui, dev, *con, in) == 0, "WG4 closed panel emits no widgets");
    }

    // ---- WG5/6: the FULL roster gets a row, plus tabs, headers, dials. ------
    {
        FakeRoster r; WeaponTuningMenu m; x3::ui::UiContext ui;
        m.setSource(sourceFor(r));
        m.open();
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        const int f = pump(m, ui, dev, *con, in);
        // 2 tabs + 2 collapsing headers + 3 weapons + 4 sliders + 4 RESETs.
        check(f == kTabCount + 2 + (int)r.names.size() + 4 + 4,
              "WG5 every tab, header, weapon and dial takes a focus slot");
        check(m.hasSource(), "WG6 an installed roster reports hasSource");
    }

    // ---- WG7/8: SOURCELESS HOST (the "every world gets it" degradation). ----
    // A showroom / driving host installs no roster: the panel must still open and
    // still build all four global dials, because fxTuning() is process-wide.
    {
        WeaponTuningMenu m; x3::ui::UiContext ui;
        m.open();
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        const int f = pump(m, ui, dev, *con, in);
        check(!m.hasSource(), "WG7 a host with no weapons reports no source");
        check(f == kTabCount + 2 + 4 + 4, "WG8 source-less host still gets every dial");
    }

    // ---- WG9..12: activating a weapon row runs the host's wtest toggle. -----
    {
        FakeRoster r; WeaponTuningMenu m; x3::ui::UiContext ui;
        m.setSource(sourceFor(r));
        m.open();
        // Focus slots: 0,1 = tabs; 2 = WEAPONS header; 3,4,5 = the weapons.
        focusTo(m, ui, dev, *con, 4);
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        in.navActivate = true;
        pump(m, ui, dev, *con, in);
        check(r.toggleCount == 1, "WG9 activating a weapon row calls the soak path once");
        check(r.lastToggle == 1, "WG10 it toggles THAT weapon (roster index 1)");
        check(r.soak == 1, "WG11 the host reports the weapon now soaking");
        check(m.selectedWeapon() == 1, "WG12 the activated weapon becomes the selected one");
    }

    // ---- WG13..16: THE ANTI-DRIFT PROPERTY. The per-weapon flash slider must
    // target the cvar built from weaponFxKindName() for the SELECTED weapon's
    // kind. Proven by moving the value and seeing THAT cvar change.
    {
        FakeRoster r; WeaponTuningMenu m; x3::ui::UiContext ui;
        r.held = 1;                     // lightning in hand -> w_flash_lightning
        m.setSource(sourceFor(r));
        m.open();
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        pump(m, ui, dev, *con, in);
        check(m.selectedWeapon() == 1, "WG13 selection follows the weapon in hand");

        const std::string flashCvar = weaponFlashCVar((int)WeaponFxKind::Lightning);
        con->set(flashCvar, "1");
        con->set(weaponFlashCVar((int)WeaponFxKind::Pistol), "1");
        // Slots: 0,1 tabs; 2 WEAPONS hdr; 3,4,5 weapons; 6 FX hdr; 7 dial0, 8 RESET0,
        // 9 = the per-weapon flash dial.
        focusTo(m, ui, dev, *con, 9);
        check(ui.focus() == 9, "WG14 focus reaches the per-weapon flash slider");
        in.navLeft = true;
        pump(m, ui, dev, *con, in);
        const float want = 1.0f - kDialFlashKind.step;
        check(std::fabs(con->getFloat(flashCvar) - want) < 1e-3f,
              "WG15 nudging the flash slider writes w_flash_<kind> for the held weapon");
        check(std::fabs(con->getFloat(weaponFlashCVar((int)WeaponFxKind::Pistol)) - 1.0f) < 1e-3f,
              "WG16 it did NOT touch another kind's flash cvar");
    }

    // ---- WG17/18: dials clamp to the SHARED range, and RESET restores the
    // shared default — panel and applyWeaponFxCVars can never disagree.
    {
        FakeRoster r; WeaponTuningMenu m; x3::ui::UiContext ui;
        m.setSource(sourceFor(r));
        m.open();
        const char* lt = kDialLightningThickness.cvar;
        con->set(lt, "1");
        focusTo(m, ui, dev, *con, 7);           // dial 0
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        in.navLeft = true;
        for (int i = 0; i < 400; ++i) pump(m, ui, dev, *con, in);
        check(std::fabs(con->getFloat(lt) - kDialLightningThickness.lo) < 1e-3f,
              "WG17 a dial clamps to the shared table's minimum");
        focusTo(m, ui, dev, *con, 8);           // its RESET
        x3::ui::UiInput act{}; act.mouseX = kOff; act.mouseY = kOff; act.navActivate = true;
        pump(m, ui, dev, *con, act);
        check(con->getString(lt) == dialDefaultString(kDialLightningThickness),
              "WG18 RESET restores the dial table's default string exactly");
    }

    // ---- WG19: the HUD GLASS tab drives the shared glass cvars. -------------
    {
        WeaponTuningMenu m; x3::ui::UiContext ui;
        m.open();
        x3::ui::UiInput in{}; in.mouseX = kOff; in.mouseY = kOff;
        pump(m, ui, dev, *con, in);
        // Tab 1 is the second tab: focus slot 1, activate.
        focusTo(m, ui, dev, *con, 1);
        x3::ui::UiInput act{}; act.mouseX = kOff; act.mouseY = kOff; act.navActivate = true;
        pump(m, ui, dev, *con, act);
        const int f = pump(m, ui, dev, *con, in);
        // 2 tabs + colorEdit4's 4 channel sliders + hud_radius + its RESET.
        check(f == kTabCount + 4 + 1 + 1, "WG19 the HUD GLASS tab builds the colour editor");

        // Nudge the ALPHA channel (slot 5: tabs 0,1 then R,G,B,A = 2,3,4,5).
        con->set("hud_glass_a", "0.86");
        applyHudPanelCVars(*con);
        focusTo(m, ui, dev, *con, 5);
        x3::ui::UiInput left{}; left.mouseX = kOff; left.mouseY = kOff; left.navLeft = true;
        pump(m, ui, dev, *con, left);
        check(con->getFloat("hud_glass_a") < 0.86f,
              "WG20 the colour editor writes the shared hud_glass_* cvars");
        con->set("hud_glass_a", "0.86");
        applyHudPanelCVars(*con);
    }

    // ---- WG21: the live glass tuning round-trips through the cvars. ---------
    {
        con->set("hud_radius", "9");
        applyHudPanelCVars(*con);
        check(std::fabs(hudPanelTuning().radius - 9.0f) < 1e-3f,
              "WG21 hud_radius syncs into the shared panel tuning");
        con->set("hud_radius", "3");
        applyHudPanelCVars(*con);
        check(std::fabs(hudPanelTuning().radius - 3.0f) < 1e-3f,
              "WG22 and back to the tiny shipped radius");
    }

    delete con;
    x3::logInfo(std::string("--test-ui weapon tuning: ") + std::to_string(pass) +
                " passed, " + std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::game
