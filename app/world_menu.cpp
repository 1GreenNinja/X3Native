#include "world_menu.h"

#include "engine/core/x3_log.h"
#include "headless_device.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace x3::game {

using x3::ui::UiContext;

namespace {

// FOUR columns. Every place the game has fits on ONE screen at 720p with no
// scrolling — the owner asked for a MENU, not a scroll hunt. The dev worlds are the
// long list, so they take the last two columns; `devSeen` is how many DevWorld rows
// have been laid out so far (the caller counts them).
constexpr int kCols = 4;
int columnFor(DestGroup g, int devSeen, int devTotal) {
    switch (g) {
    case DestGroup::Hub:
    case DestGroup::Facility:   return 0;
    case DestGroup::Underworld:
    case DestGroup::Planet:
    case DestGroup::EchoHarbor: return 1;
    default:                    return (devSeen < (devTotal + 1) / 2) ? 2 : 3;
    }
}

const float kColGreen[4] = { 0.30f, 1.00f, 0.65f, 0.95f };
const float kColAmber[4] = { 1.00f, 0.72f, 0.25f, 0.95f };
const float kColGrey [4] = { 0.42f, 0.46f, 0.52f, 0.80f };
const float kColBlue [4] = { 0.35f, 0.80f, 1.00f, 0.95f };
const float kColDim  [4] = { 0.55f, 0.62f, 0.70f, 0.85f };

const float* tagColor(DestReach r) {
    switch (r) {
    case DestReach::Teleport:  return kColGreen;
    case DestReach::WorldLoad: return kColAmber;
    default:                   return kColGrey;
    }
}
const char* tagText(const DestStatus& s) {
    if (s.here) return "YOU ARE HERE";
    switch (s.reach) {
    case DestReach::Teleport:  return "TELEPORT";
    case DestReach::WorldLoad: return "LOADS WORLD";
    default:                   return "UNAVAILABLE";
    }
}

} // namespace

int WorldMenu::draw(UiContext& ui, float dt, const ReachFn& reach) {
    if (!m_open) return -1;
    m_clock += dt;
    m_chosen = -1;

    const float W = (float)ui.screenW();
    const float H = (float)ui.screenH();
    if (W <= 0.0f || H <= 0.0f) return -1;

    // ---- The slab. Same black-glass language as the rift console + the holo
    //      terminals: dark glass, a lit frame, blue/green light on it. ----
    const float scrim[4] = { 0.0f, 0.0f, 0.0f, 0.62f };
    const float glass[4] = { 0.012f, 0.018f, 0.028f, 0.94f };
    const float frame[4] = { 0.10f, 0.62f, 0.95f, 0.85f };
    ui.quad(0, 0, W, H, scrim);

    const float m   = 40.0f;
    const float pw  = W - 2 * m, ph = H - 2 * m;
    const float px  = m, py = m;
    ui.quad(px, py, pw, ph, glass);
    const float ft = 2.5f;
    ui.quad(px - ft, py - ft, pw + 2 * ft, ft, frame);
    ui.quad(px - ft, py + ph, pw + 2 * ft, ft, frame);
    ui.quad(px - ft, py, ft, ph, frame);
    ui.quad(px + pw, py, ft, ph, frame);

    ui.text("RIFT NETWORK  -  DESTINATION DIRECTORY", px + 22.0f, py + 16.0f, 22.0f,
            kColBlue, UiContext::FontRole::Title);
    ui.text("every place in the game.   green = teleport   amber = full world load   "
            "grey = unreachable",
            px + 22.0f, py + 46.0f, 12.0f, kColDim, UiContext::FontRole::HudMono);

    // ---- Rows. Buttons are emitted in a stable order, so a button's FOCUS SLOT is
    //      its position in `focusToDest` — that is how the footer knows which row the
    //      keyboard is on. UNAVAILABLE rows are drawn as labels: they take NO focus
    //      slot, so they are not merely dimmed, they are genuinely unpickable. ----
    const float gridTop = py + 78.0f;
    const float gap     = 14.0f;
    const float colW    = (pw - 44.0f - (kCols - 1) * gap) / (float)kCols;
    const float rowH    = 24.0f;
    const float hdrH    = 30.0f;

    // How many DevWorld rows there are (they split across the last two columns).
    int devTotal = 0;
    for (uint32_t i = 0; i < destinationCount(); ++i)
        if (destination(i).group == DestGroup::DevWorld) ++devTotal;
    int devSeen = 0;

    float colY[kCols];
    DestGroup lastGroup[kCols];
    for (int c = 0; c < kCols; ++c) { colY[c] = gridTop; lastGroup[c] = DestGroup::Count; }

    std::vector<int>        focusToDest;   // focus slot -> registry index
    std::vector<DestStatus> statusOf(destinationCount());

    int   hoverDest = -1;                  // the row the MOUSE is over

    for (uint32_t i = 0; i < destinationCount(); ++i) {
        const Destination& d = destination(i);
        const DestStatus   s = reach(d);
        statusOf[i] = s;

        const int   c  = columnFor(d.group, devSeen, devTotal);
        if (d.group == DestGroup::DevWorld) ++devSeen;
        const float cx = px + 22.0f + (float)c * (colW + gap);

        if (lastGroup[c] != d.group) {
            lastGroup[c] = d.group;
            if (colY[c] > gridTop) colY[c] += 10.0f;
            ui.text(destGroupName(d.group), cx, colY[c], 13.0f, kColBlue,
                    UiContext::FontRole::HudMono);
            const float rule[4] = { 0.10f, 0.40f, 0.60f, 0.55f };
            ui.quad(cx, colY[c] + 17.0f, colW, 1.0f, rule);
            colY[c] += hdrH;
        }
        const float ry = colY[c];

        // The status tag needs a real gutter: "LOADS WORLD" in HudMono at 10 px is
        // ~104 px, and a tag that overruns bleeds into the next column's rows.
        const float tagW = 106.0f;
        const float btnW = colW - tagW - 6.0f;

        const bool pickable = (s.reach != DestReach::Unavailable) && !s.here;
        if (pickable) {
            if ((int)focusToDest.size() == ui.focus()) hoverDest = (int)i;  // keyboard row
            if (ui.pointIn(cx, ry, btnW, rowH)) hoverDest = (int)i;         // mouse row
            focusToDest.push_back((int)i);
            if (ui.button(d.name, cx, ry, btnW, rowH)) m_chosen = (int)i;
        } else {
            const float dead[4] = { 0.06f, 0.07f, 0.09f, 0.75f };
            ui.quad(cx, ry, btnW, rowH, dead);
            ui.text(d.name, cx + 8.0f, ry + 5.0f, rowH * 0.42f, kColGrey);
            if (ui.pointIn(cx, ry, colW, rowH)) hoverDest = (int)i;
        }
        ui.text(tagText(s), cx + btnW + 6.0f, ry + 6.0f, 10.0f, tagColor(s.reach),
                UiContext::FontRole::HudMono);
        colY[c] += rowH + 3.0f;
    }

    // ---- Footer: the DESCRIPTION of the row under the cursor / keyboard focus, and
    //      — when it cannot be reached — the REASON. This is where the menu is honest. ----
    const float fy = py + ph - 76.0f;
    const float sep[4] = { 0.10f, 0.40f, 0.60f, 0.45f };
    ui.quad(px + 22.0f, fy - 10.0f, pw - 44.0f, 1.0f, sep);
    if (hoverDest >= 0) {
        const Destination& d = destination((uint32_t)hoverDest);
        const DestStatus&  s = statusOf[(uint32_t)hoverDest];
        ui.text(d.name, px + 22.0f, fy, 16.0f, tagColor(s.reach), UiContext::FontRole::Title);
        ui.text(d.desc, px + 22.0f, fy + 22.0f, 13.0f, kColDim, UiContext::FontRole::HudMono);
        char line[192];
        if (s.here)
            std::snprintf(line, sizeof(line), "you are already here.");
        else if (s.reach == DestReach::Unavailable)
            std::snprintf(line, sizeof(line), "UNAVAILABLE: %s",
                          s.reason.empty() ? "no anchor in this world and no world to load"
                                           : s.reason.c_str());
        else if (s.reach == DestReach::WorldLoad)
            std::snprintf(line, sizeof(line),
                          "picking this TEARS DOWN this world and loads --world %s.",
                          d.worldFlag);
        else
            std::snprintf(line, sizeof(line), "reachable from here: you will be moved. no load.");
        ui.text(line, px + 22.0f, fy + 40.0f, 13.0f,
                s.reach == DestReach::Unavailable ? kColGrey : tagColor(s.reach),
                UiContext::FontRole::HudMono);
    } else {
        ui.text("hover or arrow-key a destination.", px + 22.0f, fy + 22.0f, 13.0f, kColDim,
                UiContext::FontRole::HudMono);
    }
    ui.text("[Esc] close", px + pw - 120.0f, fy + 40.0f, 13.0f, kColDim,
            UiContext::FontRole::HudMono);

    if (m_chosen >= 0) close();
    return m_chosen;
}

// ===========================================================================
// Self-test (folded into --test-ui).
// ===========================================================================
namespace {
int wm_pass = 0, wm_fail = 0;
void wmCheck(bool cond, const char* name) {
    if (cond) { ++wm_pass; x3::logInfo(std::string("[worldmenu-test] PASS ") + name); }
    else      { ++wm_fail; x3::logError(std::string("[worldmenu-test] FAIL ") + name); }
}
// The headless device reports a 0x0 HUD (nothing to draw into), and every screen
// bails on a zero viewport. The UI tests solve this with a stub that reports a real
// 1280x720 so the LAYOUT MATH runs; same trick here (ui.cpp has its own).
class MenuStubDevice final : public HeadlessRenderDevice {
public:
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 1280; h = 720; }
};
} // namespace

bool runWorldMenuSelfTest() {
    wm_pass = wm_fail = 0;

    MenuStubDevice device;
    x3::rhi::FrameContext frame{};   // frame.valid == false -> logic runs, no draw calls

    // Every row reachable: how many focus slots does the menu emit?
    auto allTeleport = [](const Destination&) {
        DestStatus s; s.reach = DestReach::Teleport; return s;
    };
    // Nothing reachable: UNAVAILABLE rows must emit NO focus slots.
    auto noneReachable = [](const Destination&) {
        DestStatus s; s.reach = DestReach::Unavailable; s.reason = "test"; return s;
    };

    // W1 — the menu opens, and with everything reachable every registry entry gets a
    //      focusable row (nothing is quietly dropped from the directory).
    {
        WorldMenu menu; menu.open();
        UiContext ui;
        ui.begin(device, frame, x3::ui::UiInput{});
        const int picked = menu.draw(ui, 0.016f, allTeleport);
        ui.end();
        wmCheck(menu.isOpen() && picked < 0 &&
                ui.focusCount() == (int)destinationCount(),
                "W1 open menu emits one focusable row per registry destination");
    }

    // W2 — UNAVAILABLE rows are genuinely UNPICKABLE (no focus slot at all), not just
    //      painted grey. A menu that lies is worse than no menu.
    {
        WorldMenu menu; menu.open();
        UiContext ui;
        x3::ui::UiInput in{};
        in.navActivate = true;                    // try to activate whatever is focused
        ui.begin(device, frame, in);
        const int picked = menu.draw(ui, 0.016f, noneReachable);
        ui.end();
        wmCheck(menu.isOpen() && picked < 0 && ui.focusCount() == 0,
                "W2 unavailable destinations take no focus slot and cannot be picked");
    }

    // W3 — keyboard-activating the focused row returns THAT registry index. Walk the
    //      focus down N rows and assert the pick matches destination N.
    {
        const int target = 5;
        WorldMenu menu; menu.open();
        UiContext ui;
        // Frame 1: lay out (so focusCount is known), leaving focus at `target`.
        ui.begin(device, frame, x3::ui::UiInput{});
        menu.draw(ui, 0.016f, allTeleport);
        ui.end();
        ui.setFocus(target);
        // Frame 2: activate.
        x3::ui::UiInput in{};
        in.navActivate = true;
        ui.begin(device, frame, in);
        const int picked = menu.draw(ui, 0.016f, allTeleport);
        ui.end();
        wmCheck(picked == target && !menu.isOpen(),
                "W3 activating the focused row picks that destination and closes");
    }

    // W4 — a closed menu draws nothing and picks nothing.
    {
        WorldMenu menu;   // starts closed
        UiContext ui;
        x3::ui::UiInput in{};
        in.navActivate = true;
        ui.begin(device, frame, in);
        const int picked = menu.draw(ui, 0.016f, allTeleport);
        ui.end();
        wmCheck(picked < 0 && ui.focusCount() == 0 && !menu.isOpen(),
                "W4 a closed menu emits nothing");
    }

    x3::logInfo("worldmenu: " + std::to_string(wm_pass) + "/" +
                std::to_string(wm_pass + wm_fail) + " passed");
    return wm_fail == 0;
}

} // namespace x3::game
