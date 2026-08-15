#pragma once
// HOST SHELL — the console, the pause menu and the FPS overlay, for EVERY world.
//
// WHY THIS EXISTS. The engine already had all three of these, and had had them
// for a long time: x3::con::IConsole (D6) is a full cvar + command registry,
// x3::game::Hud draws the console panel and the FPS/stats readouts, and
// x3::ui::UiContext draws the menu chrome the main game uses. What was missing
// was WIRING: of the ~31 world hosts, 28 wired none of it. So every world you
// could actually launch and play was the one place in the engine with no way to
// type a command, no way to see a frame time, and no menu on ESC.
//
// The cost of that landed on tuning. Changing a torque figure, a grip scale or a
// glass tint meant edit -> rebuild -> relaunch -> drive back to the interesting
// bit, once per value, which is the wrong loop for anything that has to be
// judged by feel rather than by reading a number. With a console those are all
// one line, live, while moving.
//
// SO THIS IS A WIRING CLASS, NOT A NEW SYSTEM. It owns nothing you couldn't
// already build by hand in a host; it just makes doing so three lines instead of
// three hundred, so that adding a world no longer means re-deciding whether that
// world gets developer tools.
//
//   HostShell shell;
//   shell.attach(window, device);              // once, after the device exists
//   while (running) {
//       glfwPollEvents();
//       shell.beginFrame();                    // AFTER poll, BEFORE reading input
//       if (shell.wantQuit()) break;
//       if (!shell.paused()) { ...simulate, using shell.key() for gameplay keys... }
//       auto frame = device->beginFrame();
//       ...draw the world...
//       shell.draw(frame, dt);                 // console + menu + FPS on top
//       device->endFrame(frame);
//   }
//
// THE ONE RULE FOR HOSTS: read gameplay keys through shell.key(), not
// glfwGetKey(). It returns false whenever the console or the menu owns the
// keyboard, which is what stops `car.torque 2400` from also driving the car
// forward and handbraking it. A host that keeps calling glfwGetKey directly
// still compiles and still runs — it just steers while you type.

#include "engine/rhi/IRenderDevice.h"
#include "engine/core/IConsole.h"
#include "../hud.h"
#include "../ui.h"
#include "../host_context.h"

#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace x3::apphost {

class HostShell {
public:
    ~HostShell();

    // Binds to the window and creates the console + HUD. Installs GLFW char, key
    // and scroll callbacks, CHAINING to whatever the host already installed (a
    // few hosts set their own char callback, and GLFW only has one slot each).
    // Safe to call once per host; a second call on the same window is a no-op.
    void attach(GLFWwindow* window, x3::rhi::IRenderDevice* device);
    // Preferred form: also replays the command line's `--set <cvar> <value>`
    // pairs onto this console, which previously only the default host honoured.
    void attach(HostContext& hc);
    bool attached() const { return m_window != nullptr; }

    // Per-frame bookkeeping. Call right after glfwPollEvents() and before the
    // host reads any input, so open/close and pause edges land on this frame.
    void beginFrame();

    // ---- State the host must respect --------------------------------------
    bool consoleOpen() const { return m_hud.consoleOpen(); }
    bool paused()      const { return m_paused; }
    bool wantQuit()    const { return m_quit; }

    // False while the console or the menu owns input. Gate MOUSE-LOOK on this:
    // the cursor is released when the console opens, so a host that keeps
    // integrating (mx - lastMX) will spin the view across the screen while you
    // type. Multiply the deltas by it rather than branching, so the `const
    // float ddx = ...` hosts need no restructuring.
    bool inputEnabled() const { return !m_paused && !m_hud.consoleOpen(); }

    // Gameplay-safe key poll. False while the console or the pause menu holds
    // the keyboard; otherwise identical to glfwGetKey(...) == GLFW_PRESS.
    bool key(int glfwKey) const;

    // Raw poll that ignores the shell's focus rules. For the rare key that
    // should work even with the console open (nothing needs this today; it
    // exists so a host never has to reach around the shell to get it).
    bool keyRaw(int glfwKey) const;

    // Does this host actually FREEZE its simulation while the menu is up?
    // Hosts that skip their sim step when paused() should say so; the menu then
    // says PAUSED instead of MENU. A menu that claims the game is paused while
    // physics keeps running behind it is a lie, and hosts differ in whether
    // they can cheaply stop — so the shell asks rather than assumes.
    void setFreezesSim(bool yes) { m_freezesSim = yes; }

    // ---- Draw --------------------------------------------------------------
    // Console panel, pause menu and FPS/stats overlay, in that stacking order.
    // Call after the world has rendered and before endFrame().
    void draw(const x3::rhi::FrameContext& frame, float dt);
    // Same, using the shell's own frame clock. Hosts all spell their delta
    // differently (dt / fdt / frameDt / dtSec), and the overlay only needs it
    // for the FPS average and the console slide — so it keeps its own rather
    // than making 30 call sites agree on a name.
    void draw(const x3::rhi::FrameContext& frame) { draw(frame, m_dt); }
    float frameDt() const { return m_dt; }

    // The console, for registering host-specific commands and cvars. Never null
    // after attach(); null before it.
    x3::con::IConsole* console() { return m_console; }

    // Convenience: register a command that reads one float argument. The huge
    // majority of tuning commands have this shape, and spelling out the arg
    // parsing 20 times per host is how hosts end up with none of them.
    void addFloatCommand(const char* name, const char* help,
                         const std::function<void(float)>& apply);
    // Same, for an on/off command. No argument toggles; "0"/"1" sets.
    void addToggleCommand(const char* name, const char* help,
                          const std::function<bool()>& get,
                          const std::function<void(bool)>& set);

    // ---- For the GLFW callbacks only (they are free functions and cannot be
    // friends of a class they are declared before). Not for host use.
    bool onKey(int glfwKey, int action, int mods);
    x3::game::Hud& hudForCallbacks() { return m_hud; }

private:
    void setPaused(bool on);
    void drawPauseMenu(const x3::rhi::FrameContext& frame);

    GLFWwindow*             m_window  = nullptr;
    x3::rhi::IRenderDevice* m_device  = nullptr;
    x3::con::IConsole*      m_console = nullptr;
    x3::game::Hud           m_hud;
    x3::ui::UiContext       m_ui;

    bool m_paused     = false;
    bool m_quit       = false;
    bool m_freezesSim = false;

    double m_lastTime = 0.0;    // shell's own frame clock (see draw(frame))
    float  m_dt       = 1.0f / 60.0f;

    // Cursor mode to restore when the menu closes: hosts differ (a driving host
    // captures the cursor, a showroom may not), so the shell remembers rather
    // than assuming CURSOR_DISABLED and stealing the mouse from hosts that had
    // deliberately left it free.
    int  m_cursorModeBeforePause = 0;

    // Mouse edge state for the menu (UiInput wants a rising edge, GLFW gives a
    // level).
    bool m_mouseWasDown = false;
    // Menu keyboard nav edges, latched by the key callback and consumed by draw.
    bool m_navUp = false, m_navDown = false, m_navActivate = false;
};

} // namespace x3::apphost
