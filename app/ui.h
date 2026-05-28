#pragma once
// X3 GENERAL game-UI layer (every genre needs it).
//
// Two cooperating pieces, both built ONLY on the public render API
// (IRenderDevice::drawHudQuad / drawHudText / hudSize) + GLFW-agnostic input
// snapshots — so this is reusable by ANY game on the engine (FPS / MMORPG / TD /
// Adventure), not tied to EFLZ:
//
//   1) UiContext — a tiny immediate-mode (IMGUI-lite) helper: per-frame mouse +
//      key snapshot in, then label()/button()/toggle()/bar()/panel() draw widgets
//      over the 2D path and report hit-testing/click results. No per-frame heap
//      allocation in the draw path (fixed scratch buffers); engine-agnostic.
//
//   2) Menus + production HUD built on top of UiContext:
//        * MainMenu   — title + START / QUIT (+ extensible items), mouse + keyboard.
//        * PauseMenu  — RESUME / SETTINGS / QUIT TO MENU.
//        * SettingsMenu — toggles wired to cvars/render params (bloom/SSAO/SSGI/
//                         shadows/vsync) + a resolution note; reflects live state.
//        * GameHud    — production HUD: HP bar, weapon + ammo, objective text,
//                       crosshair, and a minimap stub box. Clean layout.
//
//   3) GameState / UiController — the Menu <-> Playing <-> Paused state machine the
//      host drives: routes input, decides what to draw, and exposes whether the sim
//      should be frozen (paused) and whether the cursor should be shown (menus).
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces. No
// id Tech / RBDOOM source consulted. The bitmap font is the engine's embedded
// public-domain font8x8 set.

#include "engine/rhi/IRenderDevice.h"
#include "engine/core/IConsole.h"

#include <cstdint>
#include <string>

namespace x3::ui {

// ---------------------------------------------------------------------------
// Per-frame input snapshot handed to the UI each frame. The host fills this from
// whatever input backend it uses (GLFW in app/main.cpp); the UI never touches
// GLFW directly, so it stays engine/windowing-agnostic.
// ---------------------------------------------------------------------------
struct UiInput {
    float mouseX      = 0.0f;   // cursor position in framebuffer pixels (top-left origin)
    float mouseY      = 0.0f;
    bool  mouseDown   = false;  // left button currently held
    bool  mousePressed= false;  // left button went down THIS frame (rising edge)
    // Keyboard nav edges (rising-edge booleans the host supplies).
    bool  navUp       = false;  // move selection up (W / Up arrow)
    bool  navDown     = false;  // move selection down (S / Down arrow)
    bool  navActivate = false;  // activate the focused item (Enter / Space)
    bool  navLeft     = false;  // adjust focused item left (A / Left)
    bool  navRight    = false;  // adjust focused item right (D / Right)
    bool  navBack     = false;  // back / cancel (Esc) — consumed by the menu screens
};

// ---------------------------------------------------------------------------
// Immediate-mode UI context. Begin() each frame with the device, frame, and the
// input snapshot; call widget functions; the widgets draw + return interaction.
//
// Keyboard focus: widgets are implicitly indexed in call order (0,1,2,...). The
// context tracks a focused index that nav up/down moves; a focused button can be
// activated by navActivate. focusCount() reflects how many focusable widgets were
// emitted last frame (so the host can clamp). The same widget is also clickable
// by mouse (hover sets focus, so mouse + keyboard agree).
// ---------------------------------------------------------------------------
class UiContext {
public:
    // Begin a UI frame. `frame` may be an invalid FrameContext in headless logic
    // tests (then draws are skipped but hit-testing/focus still works for tests).
    void begin(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
               const UiInput& input);
    // End the frame: finalizes focus bookkeeping (clamps focus to the emitted set
    // and applies queued nav-up/down). Call after all widgets.
    void end();

    // ---- Layout helpers (pixel space) -------------------------------------
    uint32_t screenW() const { return m_w; }
    uint32_t screenH() const { return m_h; }

    // Project a world point to framebuffer pixels via the live device's camera
    // (same projection the host uses for door/holo-terminal anchors). Returns false
    // if the point is behind the camera / off-screen (or in headless tests). Lets the
    // HUD layer place world-anchored labels (enemy nameplates) without touching GLFW.
    bool worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const {
        return m_device ? m_device->worldToScreen(wx, wy, wz, sx, sy) : false;
    }

    // ---- Font roles --------------------------------------------------------
    // Convenience aliases so call sites read clearly. These map onto the RHI's
    // FontRole. PROPORTIONAL roles (Title/Menu/Enemy) advance per-glyph; mono
    // roles (News/Console/HudMono) keep fixed cells.
    using FontRole = x3::rhi::FontRole;

    // ---- Primitive draws (thin wrappers; no allocation) -------------------
    void quad(float x, float y, float w, float h, const float rgba[4]) const;
    // Text at (x,y) top-left, glyph size px, in the given font role. Returns the
    // TRUE pixel width drawn (proportional-aware). The role defaults to Menu (the
    // general HUD/label font) so existing call sites read as Space Grotesk.
    float text(const char* s, float x, float y, float px, const float rgba[4],
               FontRole role = FontRole::Menu) const;
    // Centered text: x is the CENTER x; returns the left x used. Role-aware.
    float textCentered(const char* s, float cx, float y, float px, const float rgba[4],
                       FontRole role = FontRole::Menu) const;
    // TRUE pixel width a string occupies at glyph size px for `role` — proportional
    // roles sum per-glyph advances, mono roles return N*px. Reads live device font
    // metrics (set in begin()) so centering/right-align stays pixel-exact. The
    // role-less overload defaults to Menu for back-compat with existing callers.
    static float textWidth(FontRole role, const char* s, float px);
    static float textWidth(const char* s, float px) { return textWidth(FontRole::Menu, s, px); }

    // ---- Widgets -----------------------------------------------------------
    // A non-interactive label (left/center). Does not take focus. Role-aware
    // (defaults to Menu — the general label font).
    void label(const char* s, float x, float y, float px, const float rgba[4],
               FontRole role = FontRole::Menu) const;

    // A clickable button filling [x,y,w,h] with centered `text`. Returns true on
    // the frame it is activated (mouse click inside OR keyboard-activate while
    // focused). Hovering (mouse or keyboard focus) highlights it. Takes one focus
    // slot (in call order).
    bool button(const char* text, float x, float y, float w, float h);

    // A labelled on/off toggle row in [x,y,w,h]: draws `label` left, an ON/OFF
    // pill right (green ON / grey OFF). Returns true on the frame it is toggled
    // (click anywhere in the row, or keyboard activate / left / right while
    // focused). `value` is the current state (for display only). Takes one focus slot.
    bool toggle(const char* label, bool value, float x, float y, float w, float h);

    // A labelled 0..1 horizontal slider row in [x,y,w,h]: draws `label` left, then a
    // track + a handle whose position reflects `value` (clamped to [0,1]), and a
    // right-aligned "NN%" readout. INTERACTIVE: click/drag along the track sets the
    // value by mouse; while keyboard-focused, navLeft/navRight nudge by ~5%. Writes
    // the new value through `value` and returns true on any frame it changed. Takes
    // one focus slot (in call order). Generic — usable for any normalized scalar.
    bool slider(const char* label, float& value, float x, float y, float w, float h);

    // A filled progress/stat bar (e.g. HP). Non-interactive, no focus. `frac` in
    // [0,1]; draws a dark track + a `fill`-colored bar + an optional `caption`
    // overlaid (nullptr = none).
    void bar(float x, float y, float w, float h, float frac,
             const float fill[4], const char* caption = nullptr) const;

    // A translucent backing panel (rounded look approximated by a plain quad +
    // a 1px bright top edge). Non-interactive.
    void panel(float x, float y, float w, float h, const float rgba[4]) const;

    // An enemy nameplate / threat label: `s` drawn CENTERED at screen (cx, top) in
    // the Enemy font (Tektur Condensed — the aggressive, condensed threat voice),
    // with a drop shadow. `px` is the cap height; `rgba` tints it. The monster/HUD
    // lane calls this once enemies expose a world->screen anchor. Returns the drawn
    // width. (Wired now so the role + helper exist for the combat lane to adopt.)
    float enemyNameplate(const char* s, float cx, float top, float px,
                         const float rgba[4]) const;

    // ---- Focus ------------------------------------------------------------
    int  focus() const { return m_focus; }
    void setFocus(int i) { m_focus = i; }
    int  focusCount() const { return m_lastFocusCount; }

private:
    bool pointIn(float x, float y, float w, float h) const;

    // Device used by the STATIC textWidth() to query true per-role glyph metrics.
    // Set in begin() to the live device so centering/right-align is pixel-exact for
    // proportional fonts. Null (e.g. before the first begin) => textWidth falls back
    // to N*px (the legacy monospace estimate), which keeps headless layout tests
    // deterministic. A static pointer is fine: the UI is single-threaded per frame.
    static x3::rhi::IRenderDevice* s_metricsDevice;

    x3::rhi::IRenderDevice* m_device = nullptr;
    x3::rhi::FrameContext   m_frame{};
    bool                    m_draw = false;   // false in headless logic tests
    uint32_t                m_w = 0, m_h = 0;

    UiInput m_in{};

    int  m_focus          = 0;   // currently focused widget index
    int  m_widgetIndex    = 0;   // running index assigned to focusable widgets this frame
    int  m_lastFocusCount = 0;   // focusable widgets emitted last completed frame
    bool m_mouseMovedFocus= false; // a hover claimed focus this frame
};

// ===========================================================================
// GAME-STATE MACHINE + MENUS + PRODUCTION HUD (built on UiContext)
// ===========================================================================

// Top-level game state. The host drives transitions via UiController; the menus
// only appear in the interactive windowed path (headless test/screenshot paths
// never construct/route a UiController, so they are unaffected).
enum class GameState : uint8_t {
    MainMenu,   // launch screen: title + START / QUIT
    Playing,    // in the game; HUD draws; sim runs
    Paused,     // Esc while Playing: sim frozen; pause menu draws
    Settings,   // settings screen (reachable from Pause)
    Quit,       // the host should exit
};

// What the production HUD needs each frame. Plain values pushed by the host so the
// HUD is decoupled from any specific game's classes (EFLZ fills it from Player /
// Arsenal / ObjectiveSystem; another game fills it differently).
struct HudModel {
    int   hp           = 100;
    int   maxHp        = 100;
    const char* weapon = "";     // current weapon display name
    int   ammoInMag    = 0;
    int   ammoReserve  = 0;
    bool  reloading    = false;
    const char* objective = "";  // current objective text (may be empty)
    int   enemiesRemaining = -1; // live enemy count under the objective; <0 = hide
    float damageFlash  = 0.0f;   // [0,1] red hit flash strength
    bool  showCrosshair= true;
    bool  alive        = true;
    int   dispW        = 0;      // live framebuffer size (drives the menu RESOLUTION readout)
    int   dispH        = 0;
    bool  showVersion  = true;   // tiny build-id watermark in a screen corner (set by host)

    // -----------------------------------------------------------------------
    // MINIMAP RADAR + ENEMY NAMEPLATE feed. Plain arrays (fixed cap, no heap) the
    // host (main.cpp) fills each frame from the live world. All XZ are WORLD meters;
    // the HUD does the player-relative translate+rotate (so "up" = forward) itself.
    // Leave radarValid=false (default) to keep the old minimap stub + skip nameplates
    // (headless test/screenshot paths that don't feed it are unaffected).
    // -----------------------------------------------------------------------
    static constexpr int kMaxBlips = 32;   // enemies / allies drawn on the radar
    static constexpr int kMaxRooms = 16;   // faint room outlines

    bool  radarValid = false;       // host filled the radar feed this frame
    float playerX = 0.0f;           // player world X (radar center)
    float playerZ = 0.0f;           // player world Z
    float playerYaw = 0.0f;         // player yaw (rad, 0 looks toward +X) -> rotates radar so up=forward

    int   enemyCount = 0;
    float enemyX[kMaxBlips] = {};   // enemy world X
    float enemyY[kMaxBlips] = {};   // enemy world Y (body center; nameplate adds head offset)
    float enemyZ[kMaxBlips] = {};   // enemy world Z
    const char* enemyLabel[kMaxBlips] = {};  // short threat label per enemy (nullptr -> "HOSTILE")
    bool  enemyVisible[kMaxBlips] = {};  // eye line-of-sight: NAMEPLATE shows only if true.
                                         // The minimap blip IGNORES this (radar sees through walls).

    int   allyCount = 0;
    float allyX[kMaxBlips] = {};    // companion world X
    float allyZ[kMaxBlips] = {};    // companion world Z

    // Secret-trapdoor objective marker (gold, pulsing) — the cell floor hatch. The
    // host sets trapValid + the world XZ while the hatch exists so it's findable.
    bool  trapValid = false;
    float trapX = 0.0f;
    float trapZ = 0.0f;

    int   roomCount = 0;
    float roomCx[kMaxRooms] = {};   // room center X (world)
    float roomCz[kMaxRooms] = {};   // room center Z (world)
    float roomHx[kMaxRooms] = {};   // room half-extent X (meters)
    float roomHz[kMaxRooms] = {};   // room half-extent Z (meters)
};

// Render-setting state the SettingsMenu reflects + toggles. Mirrors the engine's
// live render params (SSAO/SSGI via setSsaoParams/setGiParams), the swapchain
// vsync, plus bloom/shadows (cvar-backed) and a resolution note. The host applies
// changed values to the device/cvars (see UiController::applySettings).
struct SettingsModel {
    bool bloom   = true;
    bool ssao    = true;
    bool ssgi    = true;
    bool shadows = true;
    bool vsync   = true;
    bool rtao    = false;          // hardware ray-traced AO (r_rtao) — OFF by default; no-op without RT
    uint32_t width  = 1280;        // the "set as default" startup size (persisted)
    uint32_t height = 720;
    uint32_t dispW  = 0;           // LIVE framebuffer size (host sets each frame) -> Settings readout
    uint32_t dispH  = 0;
    bool saveDefault = false;      // Settings "SET DEFAULT" button -> host persists the current size

    // ---- Audio (applied LIVE to the audio system by the host) -------------
    bool  musicOn  = true;         // "Music ON/OFF" -> setMusicEnabled
    float musicVol = 0.25f;        // "Music Volume" [0,1] -> setMusicVolume (matches the launch bed)
    float sfxVol   = 1.0f;         // "SFX Volume"   [0,1] -> setMasterSfxVolume
};

// The main menu screen. Pure UI: returns an action via the state it requests.
class MainMenu {
public:
    // Draw + handle input. Returns the next GameState (MainMenu = stay; Playing =
    // START chosen; Quit = QUIT chosen). `title`/`subtitle` are display strings.
    // dispW/dispH = the LIVE framebuffer size (shown as the resolution readout, updates
    // as the window is dragged). outSaveDefault is set true the frame the "SET AS
    // DEFAULT" button is clicked (host writes the settings file).
    GameState update(UiContext& ui, const char* title, const char* subtitle,
                     int dispW, int dispH, bool& outSaveDefault);
};

// A save/load action the pause menu can request back to the host (it is NOT a
// GameState — saving/loading keeps you in the Paused screen). The host polls the
// UiController for these (wantSave()/wantLoad()) and performs the file I/O itself.
enum class PauseAction : uint8_t { None = 0, Save = 1, Load = 2 };

// The pause overlay (drawn over a frozen, dimmed scene).
class PauseMenu {
public:
    // Returns: Paused (stay), Playing (RESUME), Settings (SETTINGS), MainMenu
    // (QUIT TO MENU). `outAction` is set to Save/Load on the frame the SAVE/LOAD
    // button is activated (the returned state stays Paused in that case).
    GameState update(UiContext& ui, PauseAction& outAction);
};

// The settings screen. Edits a SettingsModel in place (reflecting + toggling) and
// reports whether anything changed this frame (so the host can apply live). The
// `from` arg is the state to return to on Back (Pause from in-game, MainMenu if
// ever reached from the title).
class SettingsMenu {
public:
    // Returns the state to switch to (Settings = stay, or `back` on Back). Sets
    // `outChanged` true on any frame a value flipped.
    GameState update(UiContext& ui, SettingsModel& model, GameState back, bool& outChanged);
};

// The production in-game HUD. Stateless beyond a tiny pulse clock; draws from a
// HudModel. Reuses the existing crosshair/health visual language.
class GameHud {
public:
    void draw(UiContext& ui, const HudModel& m, float dt);

private:
    float m_t = 0.0f;   // animation clock (low-ammo pulse etc.)
};

// ---------------------------------------------------------------------------
// UiController — owns the GameState + the screens + the settings model, and wires
// settings changes onto the render device + a console (cvars). The host creates
// ONE of these for the interactive path, feeds it a UiInput + HudModel each frame,
// and reads back shouldFreezeSim() / showCursor() / state().
// ---------------------------------------------------------------------------
class UiController {
public:
    // Bind the device + console. Registers the settings cvars (ui_bloom, r_ssao,
    // r_ssgi, r_shadows, r_vsync) seeded from `initial`, and snapshots the initial
    // SettingsModel (also applied to the device so state is consistent). The
    // console is OPTIONAL (nullptr -> cvars not registered; settings still work
    // via the in-memory model). Starts in MainMenu.
    void init(x3::rhi::IRenderDevice& device, x3::con::IConsole* console,
              const SettingsModel& initial);

    // Set the title strings shown on the main menu.
    void setTitle(const char* title, const char* subtitle);

    // Advance one frame: route input, run the active screen / HUD. `hud` is only
    // used while Playing. `dt` advances HUD animation. Call between beginFrame and
    // endFrame (so widgets draw into the live frame). Esc handling: while Playing,
    // navBack pauses; while Paused/Settings the screens consume it.
    void update(const UiInput& input, x3::rhi::IRenderDevice& device,
                const x3::rhi::FrameContext& frame, const HudModel& hud, float dt);

    // ---- Host queries ------------------------------------------------------
    GameState state() const { return m_state; }
    // True while the sim/fixed-step should be frozen (Paused or Settings, or while
    // sitting in the main menu before the game starts).
    bool shouldFreezeSim() const {
        return m_state == GameState::MainMenu || m_state == GameState::Paused ||
               m_state == GameState::Settings;
    }
    // True when the OS cursor should be visible (any menu screen).
    bool showCursor() const { return m_state != GameState::Playing; }
    // True the frame the user picked QUIT (host should exit).
    bool wantQuit() const { return m_state == GameState::Quit; }
    // True while the game is actually being played (HUD shown, sim runs).
    bool playing() const { return m_state == GameState::Playing; }

    // ---- Save/Load requests (pause-menu affordance) -----------------------
    // True the frame the user picked SAVE / LOAD in the pause menu. The host reads
    // these after update(), performs the file I/O, then calls clearSaveLoadRequest().
    bool wantSave() const { return m_pendingAction == PauseAction::Save; }
    bool wantLoad() const { return m_pendingAction == PauseAction::Load; }
    void clearSaveLoadRequest() { m_pendingAction = PauseAction::None; }
    // True the frame the user clicked "SET AS DEFAULT" on the main menu — the host
    // writes the settings file (window size + r_exposure), then calls clearSaveDefaults().
    bool wantSaveDefaults() const { return m_saveDefaults; }
    void clearSaveDefaults() { m_saveDefaults = false; }
    bool m_saveDefaults = false;

    // Force a state (used by the host, e.g. when the player dies -> back to menu,
    // or by tests). Does NOT apply settings.
    void setState(GameState s) { m_state = s; }

    SettingsModel&       settings()       { return m_settings; }
    const SettingsModel& settings() const { return m_settings; }

    // Apply the current SettingsModel to the device + console cvars (called on init
    // and whenever a setting changes). Public so tests can verify cvar wiring.
    void applySettings(x3::rhi::IRenderDevice& device, x3::con::IConsole* console);

private:
    GameState m_state = GameState::MainMenu;

    UiContext     m_ui;
    MainMenu      m_main;
    PauseMenu     m_pause;
    SettingsMenu  m_settingsScreen;
    GameHud       m_hud;
    SettingsModel m_settings{};

    PauseAction   m_pendingAction = PauseAction::None;  // SAVE/LOAD requested via the pause menu

    x3::con::IConsole* m_console = nullptr;   // bound in init (may be null)

    std::string m_title    = "X3 ENGINE";
    std::string m_subtitle = "a general game-UI demo";
};

// Headless self-test (--test-ui): button hit-test, menu state transitions
// (Menu<->Playing<->Paused), and a settings toggle flipping a cvar. No window /
// Vulkan. Returns true iff all checks pass.
bool runUiSelfTest();

} // namespace x3::ui
