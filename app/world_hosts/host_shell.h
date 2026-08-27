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
#include "../weapon_tuning_menu.h"
#include "../ship_comms.h"
#include "../host_context.h"

#include <cstddef>
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
    bool inputEnabled() const {
        // The tuning panel owns the keyboard AND releases the cursor, so a host
        // that keeps integrating mouse deltas would spin the view while you drag
        // a slider — exactly the bug this flag exists to prevent for the console.
        //
        // The COMMS DEVICE joins them on the same terms. Routing it through the
        // shared pure gate (ship_comms.h) rather than repeating the condition is
        // what lets --test-comms assert the flight-input contract on the very
        // function the game runs.
        return x3::game::commsFlightInputEnabled(
            m_paused, m_hud.consoleOpen(), m_wtune.isOpen(), m_comms.focused());
    }

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

    // ESC FIRST-REFUSAL. Some hosts already give ESC a layered meaning — close
    // the confirm prompt, then the world map, then quit (host_streamed); cancel
    // the keypad entry (host_showroom). The shell takes ESC in the key callback,
    // so without this the host would simply never see it and its own modal
    // would become unclosable.
    //
    // Return true from the handler if the host consumed the press. The shell
    // only opens its menu when the handler declines. Not called for SHIFT+ESC,
    // which always quits, or while the console is open, where ESC closes the
    // console.
    void setEscapeHandler(std::function<bool()> fn) { m_onEscape = std::move(fn); }

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

    // ---- THE TUNING PANEL (F7) ---------------------------------------------
    // Tim: "The console and menu should stay consistent across x3native no
    // matter what game.. colors can change but functions should all be there."
    // So the panel lives HERE, next to the console, and every host that attaches
    // this shell gets it for the same zero lines the console costs. F7 toggles
    // it; it does NOT pause the sim (a soak has to keep firing while you tune),
    // but it does take the keyboard and show the cursor while it is up.
    //
    // A host with weapons calls setWeaponTuningSource() to hand over its roster
    // and its `wtest` hook; a host without weapons calls nothing and still gets
    // the global FX + HUD-glass dials.
    x3::game::WeaponTuningMenu& weaponTuning() { return m_wtune; }
    void setWeaponTuningSource(x3::game::WeaponTuningSource src) {
        m_wtune.setSource(std::move(src));
    }
    bool tuningPanelOpen() const { return m_wtune.isOpen(); }

    // ---- The ship comms device --------------------------------------------
    // Exposed so a host can post its own traffic directly. Most content arrives
    // through x3::game::commsBus() instead, which needs no shell pointer.
    x3::game::CommsDevice& comms() { return m_comms; }
    bool commsFocused() const { return m_comms.focused(); }

    // Convenience: register a command that reads one float argument. The huge
    // majority of tuning commands have this shape, and spelling out the arg
    // parsing 20 times per host is how hosts end up with none of them.
    void addFloatCommand(const char* name, const char* help,
                         const std::function<void(float)>& apply);
    // Same, for an on/off command. No argument toggles; "0"/"1" sets.
    void addToggleCommand(const char* name, const char* help,
                          const std::function<bool()>& get,
                          const std::function<void(bool)>& set);

    // ---- NOCLIP (D-CONSOLE fold) --------------------------------------------
    // attach() registers 'noclip'/'idclip' on this shell's console (the shared
    // catalog — app/engine_console.h) wired to a REAL detached freefly camera:
    // WASD + mouse, Shift = fast. It does not fight the host's own chase/
    // gameplay camera because the host stops driving the camera at all while
    // it is active — see overrideCamera() below.
    //
    // trackCamera(): call EVERY frame, before the host decides its own camera,
    // with whatever pose the host WOULD have used. Cheap; its only job is
    // seeding the flycam's start pose the instant noclip switches on, so the
    // view detaches cleanly instead of jumping to the origin.
    void trackCamera(float x, float y, float z, float yaw, float pitch);
    bool noclipActive() const { return m_fly.active; }
    // Where the host's own camera setup normally calls device->setCamera():
    // call overrideCamera() first. While noclip is active it advances the
    // flycam from WASD + mouse delta and calls setCamera() itself, returning
    // true — the host must skip its own camera work entirely that frame.
    // Returns false (touches nothing) when noclip is off, or while the
    // console/menu owns input.
    bool overrideCamera(float dt, float fovDeg);
    // The flycam's current pose, for a host that wants auxiliary per-position
    // systems (audio listener, weather/reverb probes) to track the free
    // camera too instead of going stale at wherever the car/Jake was left.
    // Only meaningful when noclipActive() and after overrideCamera() this frame.
    void flyCamPose(float& x, float& y, float& z, float& yaw, float& pitch) const;

    // ---- For the GLFW callbacks only (they are free functions and cannot be
    // friends of a class they are declared before). Not for host use.
    bool onKey(int glfwKey, int action, int mods);
    // Printable codepoints, for the tuning panel's ctrl+click-to-type fields.
    // Returns true if the shell consumed the character (the console already
    // consumes its own; this is the panel's half).
    bool onChar(unsigned int codepoint);
    x3::game::Hud& hudForCallbacks() { return m_hud; }

private:
    void setPaused(bool on);
    void drawPauseMenu(const x3::rhi::FrameContext& frame);
    void drawTuningPanel(const x3::rhi::FrameContext& frame, float dt);
    void drawComms(const x3::rhi::FrameContext& frame, float dt);

    GLFWwindow*             m_window  = nullptr;
    x3::rhi::IRenderDevice* m_device  = nullptr;
    x3::con::IConsole*      m_console = nullptr;
    x3::game::Hud           m_hud;
    x3::ui::UiContext       m_ui;
    x3::game::WeaponTuningMenu m_wtune;   // F7 tuning panel (every host gets one)
    // THE SHIP COMMS DEVICE (feat/ship-comms) — every host gets one, exactly like
    // the console and the tuning panel. It has its OWN UiContext rather than
    // sharing m_ui: UiContext carries a persistent focus ring, and the comms
    // device can be focused WHILE the tuning panel is open, so one shared ring
    // would let the two surfaces fight over the focused index.
    x3::game::CommsDevice   m_comms;
    x3::game::CommsDirector m_commsDirector;
    x3::ui::UiContext       m_commsUi;

    bool m_paused     = false;
    bool m_quit       = false;
    bool m_freezesSim = false;

    double m_lastTime = 0.0;    // shell's own frame clock (see draw(frame))
    float  m_dt       = 1.0f / 60.0f;

    std::function<bool()> m_onEscape;   // host first-refusal on ESC

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
    // The tuning panel needs the horizontal pair (sliders), Tab/Shift+Tab, and
    // the text-entry edges (ctrl+click a slider to TYPE a value). Latched the
    // same way, for the same reason: a polled edge drops presses on long frames.
    bool m_navLeft = false, m_navRight = false;
    bool m_navNext = false, m_navPrev = false;
    bool m_navEnter = false, m_navBackspace = false, m_navEscape = false;
    char m_typed[x3::ui::UiInput::kMaxTyped] = {};
    int  m_typedCount = 0;
    bool m_prevMouseForPanel = false;
    // The comms device tracks its own click edge: it can be focused while the
    // tuning panel is open, and one shared edge would let each steal the other's.
    bool m_prevMouseForComms = false;
    // True while a slider in the panel is in ctrl+click TYPE mode, so ESC can
    // cancel the edit before it closes the panel.
    bool m_editingInPanel = false;

    // ---- NOCLIP freefly state ------------------------------------------------
    struct FlyCam { float pos[3] = { 0.0f, 0.0f, 0.0f }; float yaw = 0.0f, pitch = 0.0f; bool active = false; };
    FlyCam m_fly;
    float  m_lastCam[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };   // x,y,z,yaw,pitch — see trackCamera()
    bool   m_haveLastCam = false;
    double m_flyLastMX = 0.0, m_flyLastMY = 0.0;
    bool   m_flyMouseSeeded = false;

    // ---- LIVE console -> device cvar apply (world_host_common.h), driven
    // automatically from draw() so every host attaching this shell gets the
    // owner's "type r_exposure 0.5 and SEE it" for free.
    unsigned    m_liveApplyFrame = 0;
    std::size_t m_liveApplyHash  = 0;
};

} // namespace x3::apphost
