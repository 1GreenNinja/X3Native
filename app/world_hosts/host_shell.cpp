// HOST SHELL — see host_shell.h for why this exists.
#include "host_shell.h"
#include "../hud_panel.h"   // applyHudPanelCVars (hud_glass_* / hud_radius)

#include <GLFW/glfw3.h>
#include "engine/core/x3_log.h"
#include "../engine_console.h"     // D-CONSOLE fold: the shared r_*/cheat catalog + noclip + help
#include "world_host_common.h"     // applyLiveHostRenderCVars (live console -> device push)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace x3::apphost {
namespace {

// The shell that owns the GLFW callbacks. The engine runs ONE window, and the
// callbacks are C function pointers with no user data of their own, so a file
// static is the honest way to reach the instance from them. Guarded by the
// window pointer so a stale shell (host exited, callbacks not yet replaced)
// cannot be driven by events for a window it no longer owns.
HostShell*      g_shell   = nullptr;
GLFWwindow*     g_window  = nullptr;

// Whatever the host had installed before us. GLFW keeps exactly one callback
// per event per window, so attaching without chaining would silently break the
// hosts that already set their own (host_echotropolis and host_rifthub both
// take char input).
GLFWcharfun     g_prevChar   = nullptr;
GLFWkeyfun      g_prevKey    = nullptr;
GLFWscrollfun   g_prevScroll = nullptr;

bool isConsoleToggleChar(unsigned int cp) { return cp == '`' || cp == '~'; }

} // namespace

// ---------------------------------------------------------------------------
// GLFW callbacks
// ---------------------------------------------------------------------------
static void shellCharCB(GLFWwindow* w, unsigned int cp) {
    if (g_shell && w == g_window && g_shell->consoleOpen() && !isConsoleToggleChar(cp)) {
        g_shell->hudForCallbacks().onChar(cp);
        return;                       // consumed: do NOT pass typing to the host
    }
    // The tuning panel's ctrl+click-to-type fields want printables too. Same
    // rule as the console: while it owns the keyboard the host must not also
    // see the character, or typing 1.47 into a slider also selects weapon 1.
    if (g_shell && w == g_window && g_shell->onChar(cp)) return;
    if (g_prevChar) g_prevChar(w, cp);
}

static void shellScrollCB(GLFWwindow* w, double dx, double dy) {
    if (g_shell && w == g_window && g_shell->consoleOpen()) {
        g_shell->hudForCallbacks().consoleScroll((int)(dy * 3.0));
        return;
    }
    if (g_prevScroll) g_prevScroll(w, dx, dy);
}

static void shellKeyCB(GLFWwindow* w, int key, int sc, int action, int mods) {
    if (g_shell && w == g_window && g_shell->onKey(key, action, mods)) return;
    if (g_prevKey) g_prevKey(w, key, sc, action, mods);
}

// ---------------------------------------------------------------------------
HostShell::~HostShell() {
    if (m_window && g_window == m_window) {
        // Put the host's own callbacks back before we go, so a host that
        // outlives the shell (or a second shell) is not left pointing at us.
        glfwSetCharCallback(m_window, g_prevChar);
        glfwSetKeyCallback(m_window, g_prevKey);
        glfwSetScrollCallback(m_window, g_prevScroll);
        g_shell = nullptr; g_window = nullptr;
        g_prevChar = nullptr; g_prevKey = nullptr; g_prevScroll = nullptr;
    }
    delete m_console;
    m_console = nullptr;
}

void HostShell::attach(GLFWwindow* window, x3::rhi::IRenderDevice* device) {
    if (m_window || !window) return;              // already attached / headless
    m_window = window;
    m_device = device;

    m_console = x3::con::createConsole();
    // Hud::init registers hud_fps / r_stats and the quit, fps, stats and
    // r_speeds commands, and points `quit` at our flag.
    m_hud.init(*m_console, &m_quit);

    // ---- D-CONSOLE fold: the SAME cvar/command catalog the campaign console
    // has (app/engine_console.h) — r_exposure, r_bloom*, r_taa, r_velocity,
    // r_fog*, r_rtao*, r_wetness*, r_csm*, combat_log/ai_log, boot_budget_ms,
    // ~90 cvars in all — plus noclip/idclip (REAL here: a detached freefly,
    // see trackCamera/overrideCamera) and the grouped/paged 'help'. Cheats
    // needing campaign state (god, idkfa/idfa, vigil_link, intro_play) are
    // left as null hooks, which register as "campaign only" stubs — never
    // "unknown: <cmd>". Before this call a --world host's console had NONE of
    // this: the owner's screenshot ("typing noclip/idclip -> unknown") was
    // taken against exactly this gap.
    {
        x3::game::EngineConsoleHooks hooks{};
        hooks.setNoclip = [this](bool on) {
            if (on && !m_fly.active && m_haveLastCam) {
                m_fly.pos[0] = m_lastCam[0]; m_fly.pos[1] = m_lastCam[1]; m_fly.pos[2] = m_lastCam[2];
                m_fly.yaw    = m_lastCam[3]; m_fly.pitch  = m_lastCam[4];
            }
            m_fly.active = on;
            m_flyMouseSeeded = false;   // re-seed the mouse delta so the first frame doesn't jump
        };
        hooks.getNoclip = [this]() { return m_fly.active; };
        x3::game::registerEngineConsole(*m_console, window, hooks);
    }
    m_console->print("~ console  |  ESC menu  |  F3 stats  |  noclip freefly  |  SHIFT+ESC quit");

    g_shell = this;
    g_window = window;
    g_prevChar   = glfwSetCharCallback(window, shellCharCB);
    g_prevKey    = glfwSetKeyCallback(window, shellKeyCB);
    g_prevScroll = glfwSetScrollCallback(window, shellScrollCB);
}

void HostShell::attach(HostContext& hc) {
    attach(hc.window, hc.device);
    if (!m_console) return;
    // Replay `--set <cvar> <value>` onto this console. Until now those only
    // reached the default host, so `--world tunnel --set r_exposure 0.7` parsed
    // fine and then did nothing at all.
    for (const auto& kv : hc.cliCVars) {
        m_console->set(kv.first, kv.second);
        m_console->print("--set " + kv.first + " " + kv.second);
    }
}

// Returns true if the shell consumed the key (the host must not also see it).
bool HostShell::onKey(int key, int action, int mods) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
    const bool shift = (mods & GLFW_MOD_SHIFT) != 0;

    // The console toggle works in every state, including paused — being able to
    // type a command while the menu is up is the point of having both.
    if (key == GLFW_KEY_GRAVE_ACCENT) {
        m_hud.toggleConsole();
        if (m_hud.consoleOpen() && !m_paused)
            glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        else if (!m_hud.consoleOpen() && !m_paused)
            glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        return true;
    }

    if (m_hud.consoleOpen()) {
        switch (key) {
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER:  m_hud.onEnter(*m_console);      return true;
            case GLFW_KEY_BACKSPACE: m_hud.onBackspace();            return true;
            case GLFW_KEY_UP:        m_hud.historyPrev();            return true;
            case GLFW_KEY_DOWN:      m_hud.historyNext();            return true;
            case GLFW_KEY_TAB:       m_hud.complete(*m_console);     return true;
            case GLFW_KEY_PAGE_UP:   m_hud.consoleScroll(+5);        return true;
            case GLFW_KEY_PAGE_DOWN: m_hud.consoleScroll(-5);        return true;
            // ESC closes the console rather than opening the menu — one press,
            // one obvious meaning, and it never strands you in two overlays.
            case GLFW_KEY_ESCAPE:
                m_hud.closeConsole();
                if (!m_paused) glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                return true;
            default: return true;    // swallow everything else while typing
        }
    }

    if (key == GLFW_KEY_ESCAPE) {
        // SHIFT+ESC still quits outright. Tim lost sessions to a bare ESC that
        // closed the window, so the reflex key is now the safe one and leaving
        // takes a deliberate two-key press (or the menu's QUIT row).
        if (shift) { m_quit = true; return true; }
        // The tuning panel is a modal too: ESC steps back out of it before it
        // means "open the menu". It also feeds the panel an escape edge first,
        // so ESC abandons a half-typed slider value rather than closing the whole
        // panel out from under the edit.
        if (m_wtune.isOpen()) {
            if (m_editingInPanel) { m_navEscape = true; return true; }
            m_wtune.close();
            if (!m_paused) glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            return true;
        }
        // The host gets first refusal, so its own modals stay closable — see
        // setEscapeHandler. Not offered while the menu is already up: ESC then
        // means "leave the menu", unambiguously.
        if (!m_paused && m_onEscape && m_onEscape()) return true;
        setPaused(!m_paused);
        return true;
    }

    if (key == GLFW_KEY_F3) {
        m_console->set("r_stats", m_console->getInt("r_stats") ? "0" : "1");
        return true;
    }

    // ---- F7: THE TUNING PANEL -----------------------------------------------
    // F7 was the only completely unbound function key in the project (F1/F2 are
    // the view toggles, F3 stats, F4 a map mode in host_tunnel, F5/F9 quick
    // save/load, F6 the world directory, F8 the cold-open skip). DELETE is
    // reserved for the space encounter's lock-on and is deliberately untouched.
    if (key == GLFW_KEY_F7) {
        m_wtune.toggle();
        // The panel takes the keyboard and needs the cursor, but it must NOT
        // pause: the whole workflow is "soak the weapon, watch the beam, drag the
        // slider", which dies the moment the sim freezes.
        if (!m_paused)
            glfwSetInputMode(m_window, GLFW_CURSOR,
                             m_wtune.isOpen() ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
        return true;
    }

    // While the panel is up it owns the keyboard (except the keys handled above:
    // the console toggle, ESC, F3 and F7 itself). The edges are LATCHED here
    // rather than polled in draw() for the same reason the menu's are — a poll
    // drops any press shorter than the frame.
    if (m_wtune.isOpen()) {
        switch (key) {
            case GLFW_KEY_W: case GLFW_KEY_UP:    m_navUp = true;    return true;
            case GLFW_KEY_S: case GLFW_KEY_DOWN:  m_navDown = true;  return true;
            case GLFW_KEY_A: case GLFW_KEY_LEFT:  m_navLeft = true;  return true;
            case GLFW_KEY_D: case GLFW_KEY_RIGHT: m_navRight = true; return true;
            case GLFW_KEY_TAB:
                if (shift) m_navPrev = true; else m_navNext = true;
                return true;
            case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
                m_navActivate = true; m_navEnter = true; return true;
            case GLFW_KEY_SPACE:     m_navActivate = true;  return true;
            case GLFW_KEY_BACKSPACE: m_navBackspace = true; return true;
            default: return true;    // the panel owns the rest; nothing leaks to the host
        }
    }

    if (m_paused) {
        switch (key) {
            case GLFW_KEY_W: case GLFW_KEY_UP:    m_navUp = true;       return true;
            case GLFW_KEY_S: case GLFW_KEY_DOWN:  m_navDown = true;     return true;
            case GLFW_KEY_ENTER: case GLFW_KEY_SPACE:
            case GLFW_KEY_KP_ENTER:               m_navActivate = true; return true;
            default: return true;    // the sim is frozen; nothing else applies
        }
    }
    return false;
}

void HostShell::setPaused(bool on) {
    if (m_paused == on) return;
    m_paused = on;
    if (on) {
        m_cursorModeBeforePause = glfwGetInputMode(m_window, GLFW_CURSOR);
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else {
        // Restore what the host had, rather than assuming CURSOR_DISABLED — a
        // showroom or an editor host may have deliberately left the mouse free.
        glfwSetInputMode(m_window, GLFW_CURSOR,
                         m_cursorModeBeforePause ? m_cursorModeBeforePause
                                                 : GLFW_CURSOR_DISABLED);
    }
}

void HostShell::beginFrame() {
    // Open/close, pause and nav all arrive as key EVENTS, which is what makes
    // them reliable. The hand-rolled version this replaces polled glfwGetKey
    // and tracked its own `escWasDown` edge, which drops a press whenever the
    // frame runs longer than the keypress.
    //
    // The only per-frame work is the clock, so draw(frame) can be called
    // without the host having to hand over its own delta under whichever of
    // four names it happens to use.
    const double t = glfwGetTime();
    if (m_lastTime <= 0.0) m_lastTime = t;
    m_dt = (float)(t - m_lastTime);
    m_lastTime = t;
    if (m_dt > 0.1f)  m_dt = 0.1f;      // hitch clamp, same as the hosts use
    if (m_dt < 0.0f)  m_dt = 0.0f;
}

bool HostShell::keyRaw(int glfwKey) const {
    return m_window && glfwGetKey(m_window, glfwKey) == GLFW_PRESS;
}

bool HostShell::key(int glfwKey) const {
    // The tuning panel joins the console and the menu as a keyboard owner: its
    // arrow/WASD nav must not also drive the car or the player. (The SIM keeps
    // running — only input is taken. That is the difference between this panel
    // and the pause menu, and it is deliberate: the soak has to keep firing.)
    if (!m_window || m_paused || m_hud.consoleOpen() || m_wtune.isOpen()) return false;
    return glfwGetKey(m_window, glfwKey) == GLFW_PRESS;
}

bool HostShell::onChar(unsigned int codepoint) {
    if (!m_wtune.isOpen()) return false;
    if (codepoint < 32 || codepoint > 126) return false;   // printable ASCII only
    if (m_typedCount < x3::ui::UiInput::kMaxTyped)
        m_typed[m_typedCount++] = (char)codepoint;
    return true;
}

// ---------------------------------------------------------------------------
// NOCLIP — a real detached freefly camera. Registered under 'noclip'/'idclip'
// by attach() above; this is the movement half.
// ---------------------------------------------------------------------------
void HostShell::trackCamera(float x, float y, float z, float yaw, float pitch) {
    m_lastCam[0] = x; m_lastCam[1] = y; m_lastCam[2] = z;
    m_lastCam[3] = yaw; m_lastCam[4] = pitch;
    m_haveLastCam = true;
}

bool HostShell::overrideCamera(float dt, float fovDeg) {
    if (!m_fly.active || !m_window || !m_device) return false;

    // Console/menu owns input: hold position, keep rendering from the last
    // pose rather than freezing the frame (the world stays visible around the
    // frozen flycam, exactly like every other paused-but-drawn overlay here).
    if (m_paused || m_hud.consoleOpen()) {
        m_device->setCamera(m_fly.pos[0], m_fly.pos[1], m_fly.pos[2], m_fly.yaw, m_fly.pitch, fovDeg);
        return true;
    }

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (!m_flyMouseSeeded) { m_flyLastMX = mx; m_flyLastMY = my; m_flyMouseSeeded = true; }
    const float ddx = (float)(mx - m_flyLastMX), ddy = (float)(my - m_flyLastMY);
    m_flyLastMX = mx; m_flyLastMY = my;

    m_fly.yaw   += ddx * 0.0025f;
    m_fly.pitch -= ddy * 0.0025f;
    if (m_fly.pitch >  1.5f) m_fly.pitch =  1.5f;
    if (m_fly.pitch < -1.5f) m_fly.pitch = -1.5f;

    // Same yaw/pitch -> direction convention every host in this file already
    // uses for its own chase cam (host_tunnel.cpp / host_drive.cpp), so the
    // freefly orients exactly like the camera it just detached from.
    const float fwd[3]   = { std::cos(m_fly.pitch) * std::cos(m_fly.yaw), std::sin(m_fly.pitch),
                             std::cos(m_fly.pitch) * std::sin(m_fly.yaw) };
    const float right[3] = { -fwd[2], 0.0f, fwd[0] };
    const float speed = (key(GLFW_KEY_LEFT_SHIFT) ? 40.0f : 12.0f) * dt;
    if (key(GLFW_KEY_W)) { m_fly.pos[0] += fwd[0]*speed;   m_fly.pos[1] += fwd[1]*speed;   m_fly.pos[2] += fwd[2]*speed; }
    if (key(GLFW_KEY_S)) { m_fly.pos[0] -= fwd[0]*speed;   m_fly.pos[1] -= fwd[1]*speed;   m_fly.pos[2] -= fwd[2]*speed; }
    if (key(GLFW_KEY_D)) { m_fly.pos[0] += right[0]*speed; m_fly.pos[2] += right[2]*speed; }
    if (key(GLFW_KEY_A)) { m_fly.pos[0] -= right[0]*speed; m_fly.pos[2] -= right[2]*speed; }
    if (key(GLFW_KEY_SPACE))        m_fly.pos[1] += speed;   // straight up, independent of pitch
    if (key(GLFW_KEY_LEFT_CONTROL)) m_fly.pos[1] -= speed;   // straight down

    m_device->setCamera(m_fly.pos[0], m_fly.pos[1], m_fly.pos[2], m_fly.yaw, m_fly.pitch, fovDeg);
    return true;
}

void HostShell::flyCamPose(float& x, float& y, float& z, float& yaw, float& pitch) const {
    x = m_fly.pos[0]; y = m_fly.pos[1]; z = m_fly.pos[2]; yaw = m_fly.yaw; pitch = m_fly.pitch;
}

void HostShell::draw(const x3::rhi::FrameContext& frame, float dt) {
    if (!m_device || !frame.valid) return;

    // LIVE CONSOLE -> DEVICE (D-CONSOLE fold, world_host_common.h). Every host
    // that calls shell.draw() gets this "for free": type `r_exposure 0.5` /
    // `r_bloom 0` / `r_taa 0` / `r_velocity 1` at THIS console and the frame
    // changes, without waiting for a re-launch with `--set`. Cheap — hashed,
    // re-applied only on an actual change, checked every ~15th frame.
    x3::apphost::applyLiveHostRenderCVars(*m_console, *m_device, m_liveApplyFrame++, m_liveApplyHash);

    // HUD GLASS (hud_glass_* / hud_radius) — the plate colour + the ONE corner
    // radius every panel below shares. Synced here so the F7 colour picker
    // restyles the whole family live, in every world host, not just the campaign.
    x3::game::applyHudPanelCVars(*m_console);

    if (m_paused) drawPauseMenu(frame);

    // The tuning panel sits under the console (which must stay reachable from
    // every state) and over the pause menu.
    if (m_wtune.isOpen()) drawTuningPanel(frame, dt);

    // Console over the menu: it is the one overlay that has to stay reachable
    // from every state.
    m_hud.drawConsole(*m_device, frame, *m_console, dt);
    m_hud.drawFps(*m_device, frame, *m_console, dt);
    m_hud.drawStats(*m_device, frame, *m_console, dt);
}

// ---------------------------------------------------------------------------
// THE TUNING PANEL (F7). Same UiContext, same widgets, same glass as the pause
// menu — the difference is that this one does NOT freeze anything: it takes the
// keyboard and shows the cursor, and the world keeps running behind it so a
// weapon soak stays lit while its sliders move.
// ---------------------------------------------------------------------------
void HostShell::drawTuningPanel(const x3::rhi::FrameContext& frame, float dt) {
    if (!m_window || !m_device || !m_console) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const bool down = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    x3::ui::UiInput in{};
    in.mouseX       = (float)mx;
    in.mouseY       = (float)my;
    in.mouseDown    = down;
    in.mousePressed = down && !m_prevMouseForPanel;
    m_prevMouseForPanel = down;

    in.navUp       = m_navUp;
    in.navDown     = m_navDown;
    in.navLeft     = m_navLeft;
    in.navRight    = m_navRight;
    in.navActivate = m_navActivate;
    in.navNext     = m_navNext;
    in.navPrev     = m_navPrev;
    in.enter       = m_navEnter;
    in.backspace   = m_navBackspace;
    in.escape      = m_navEscape;
    // CTRL is a LEVEL, not an edge (ctrl+click asks "was ctrl down on the click
    // frame"), so it is polled rather than latched.
    in.ctrlDown = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL)  == GLFW_PRESS ||
                  glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    in.typedCount = m_typedCount;
    for (int i = 0; i < m_typedCount; ++i) in.typed[i] = m_typed[i];

    m_navUp = m_navDown = m_navActivate = false;
    m_navLeft = m_navRight = false;
    m_navNext = m_navPrev = false;
    m_navEnter = m_navBackspace = m_navEscape = false;
    m_typedCount = 0;

    m_ui.begin(*m_device, frame, in);
    m_wtune.draw(m_ui, *m_console, dt);
    m_ui.end();
    // So ESC can cancel a half-typed value before it closes the panel (onKey).
    m_editingInPanel = m_ui.editingValue();
}

void HostShell::drawPauseMenu(const x3::rhi::FrameContext& frame) {
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(m_window, &fbw, &fbh);
    if (fbw <= 0 || fbh <= 0) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const bool down = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    x3::ui::UiInput in{};
    in.mouseX       = (float)mx;
    in.mouseY       = (float)my;
    in.mouseDown    = down;
    in.mousePressed = down && !m_mouseWasDown;
    in.navUp        = m_navUp;
    in.navDown      = m_navDown;
    in.navActivate  = m_navActivate;
    m_mouseWasDown  = down;
    m_navUp = m_navDown = m_navActivate = false;

    m_ui.begin(*m_device, frame, in);

    const float w = (float)fbw, h = (float)fbh;
    const float dim[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
    m_ui.quad(0, 0, w, h, dim);

    // NOT x3::ui::PauseMenu. That one is the CAMPAIGN menu and its rows are
    // RESUME / TRAVEL / SAVE CHECKPOINT / LOAD CHECKPOINT / SETTINGS / QUIT —
    // four of which have no meaning in a world host, which has no save system
    // and nowhere to travel to. Four dead buttons is a worse menu than three
    // live ones. What IS reused is everything that makes it look like the game:
    // the same UiContext, the same panel and button widgets, the same fonts.
    const float pw = std::min(420.0f, w * 0.55f);
    const float bh = std::max(38.0f, h * 0.075f);
    const float gap = bh * 0.26f;
    const float titlePx = std::max(24.0f, pw / 14.0f);
    const float ph = titlePx + 22.0f + 3.0f * bh + 2.0f * gap + 64.0f;
    const float px = w * 0.5f - pw * 0.5f;
    const float py = h * 0.5f - ph * 0.5f;

    const float panelCol[4] = { 0.05f, 0.06f, 0.08f, 0.92f };
    m_ui.panel(px, py, pw, ph, panelCol);

    const float titleCol[4] = { 0.40f, 0.88f, 1.0f, 1.0f };
    m_ui.textCentered(m_freezesSim ? "PAUSED" : "MENU", w * 0.5f, py + 22.0f,
                      titlePx, titleCol, x3::ui::UiContext::FontRole::Title);

    const float bw = pw - 48.0f;
    float by = py + 22.0f + titlePx + 22.0f;

    if (m_ui.button("RESUME", px + 24.0f, by, bw, bh)) setPaused(false);
    by += bh + gap;
    if (m_ui.button("CONSOLE  (~)", px + 24.0f, by, bw, bh)) {
        m_hud.toggleConsole();
        setPaused(false);
    }
    by += bh + gap;
    if (m_ui.button("QUIT TO DESKTOP", px + 24.0f, by, bw, bh)) m_quit = true;
    by += bh + gap;

    const float hint[4] = { 0.55f, 0.58f, 0.64f, 1.0f };
    m_ui.textCentered(m_freezesSim
                          ? "ESC resumes  -  the sim is stopped, this is not a freeze"
                          : "ESC resumes  -  input is released, the world keeps running",
                      w * 0.5f, by + 8.0f, std::max(11.0f, h * 0.016f), hint);

    m_ui.end();
}

// ---------------------------------------------------------------------------
// Command helpers
// ---------------------------------------------------------------------------
void HostShell::addFloatCommand(const char* name, const char* help,
                                const std::function<void(float)>& apply) {
    if (!m_console) return;
    auto* con = m_console;
    std::string nm = name;
    con->registerCommand(name, [con, nm, apply](const std::vector<std::string>& args) {
        // Console::exec STRIPS the command name before dispatch — the value is
        // args[0]. Written against args[1] originally: every float command in
        // the game printed 'needs a number' instead of acting, for two days.
        if (args.empty()) { con->print(nm + ": needs a number"); return; }
        const float v = (float)std::atof(args[0].c_str());
        apply(v);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s = %.4g", nm.c_str(), (double)v);
        con->print(buf);
    }, help);
}

void HostShell::addToggleCommand(const char* name, const char* help,
                                 const std::function<bool()>& get,
                                 const std::function<void(bool)>& set) {
    if (!m_console) return;
    auto* con = m_console;
    std::string nm = name;
    con->registerCommand(name, [con, nm, get, set](const std::vector<std::string>& args) {
        const bool v = args.empty() ? !get() : (std::atoi(args[0].c_str()) != 0);
        set(v);
        con->print(nm + " = " + (v ? "1" : "0"));
    }, help);
}

} // namespace x3::apphost
