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

    // ---- TEXT ENTRY (R8: the rift console takes typed values, not only drags) ----
    // The host pushes the printable characters typed THIS FRAME (GLFW char callback
    // or a polled key edge) plus the edit keys. Widgets that own keyboard focus
    // consume them; everything else ignores them, so the existing menus are
    // untouched. The host must ALSO gate gameplay input while a field has focus
    // (the cell-terminal discipline: typing never fires the weapon).
    static constexpr int kMaxTyped = 16;
    char  typed[kMaxTyped] = {};   // printable chars typed this frame
    int   typedCount = 0;
    bool  backspace  = false;      // rising edge
    bool  enter      = false;      // rising edge (commit the field)
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

    // General-range slider row: same widget contract as slider() but the value
    // lives in [minV,maxV], mouse drags SNAP to `step` (so "rain 3.5" is a value
    // you can actually land on, not 3.47), keyboard navLeft/navRight nudge by
    // exactly one step, and the right-hand readout is the CALLER-FORMATTED
    // `readout` string ("3.5  DOWNPOUR", "14:30", "0.88") instead of a percent —
    // the on-screen number IS the console value, never a rescaled proxy. Built
    // for the weather/lighting control panels (host_menu.h); generic on purpose.
    bool sliderEx(const char* label, float& value, float minV, float maxV, float step,
                  const char* readout, float x, float y, float w, float h);

    // =======================================================================
    // R8 — THE GLOWING CONTROL SURFACE (Tim: "the sliders glow").
    //
    // These three are the rift console's physical controls. They are the SAME
    // immediate-mode contract as slider()/toggle()/button() (call order = focus
    // slot; mouse drag + keyboard nav both work) but they render as EMISSIVE
    // HOLOGRAPHIC light on black glass rather than painted grey widgets, and
    // every one of them takes a `danger01` in [0,1] that drives its glow:
    //
    //     danger 0.0  -> calm BLUE/GREEN, steady
    //     danger 0.5  -> AMBER, a slow pulse
    //     danger 1.0  -> RED, fast hot flicker
    //
    // That is the readability channel: the player must SEE the control get angry
    // under their hand before the rift warps the room / tears time / implodes.
    // The glow obeys the project's cap law — it never clips to white, so blue and
    // green stay chromatic (same discipline as the membrane's emissive caps).
    //
    // `clock` is a seconds accumulator the caller advances (drives the pulse).
    // =======================================================================

    // Glowing slider: lit CHANNEL, the filled portion charged up to a brighter
    // HANDLE, the empty portion dim. Drag along the track, or navLeft/navRight.
    bool glowSlider(const char* label, float& value, float x, float y, float w, float h,
                    float danger01, float clock);

    // Glowing ROTARY KNOB (the right metaphor for anything cyclic: FREQUENCY,
    // PHASE). A lit dial ring with glowing tick marks and a bright indicator mark
    // at the current angle; the value sweeps 240 degrees (7 o'clock -> 5 o'clock).
    // ANGULAR DRAG: while held, the value follows the cursor's angle about the
    // knob center (shortest-path, so it never jumps across the dead zone).
    bool knob(const char* label, float& value, float cx, float cy, float radius,
              float danger01, float clock);

    // Glowing TEXT FIELD: lit border + underline, glowing typed text, blinking
    // caret while focused. Consumes UiInput::typed / backspace while it owns
    // focus. Returns true on the frame ENTER commits it (the caller parses the
    // buffer — typing an out-of-range value on purpose is how a player reaches
    // the truly dangerous outcomes, so the widget does NOT clamp).
    bool textField(const char* label, char* buf, int cap, float x, float y,
                   float w, float h, float danger01, float clock);

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

    // Is the cursor inside this rect? Public so a SCREEN (not just a widget) can ask
    // — the world menu needs it to know which row the mouse is hovering, so its footer
    // can describe that row. Read-only: it claims no focus and draws nothing.
    bool pointIn(float x, float y, float w, float h) const;

private:

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
    // DRAG CAPTURE (sliderEx). A slider claims the pointer on the press inside
    // its row and KEEPS it until the button comes up, so a drag that wanders
    // off the 34 px row — which every real drag does — still moves the value
    // instead of freezing at the row edge. Persists across frames; cleared in
    // begin() the moment the mouse is no longer down. -1 = nobody holds it.
    int  m_dragWidget     = -1;
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
    // CHARGE weapon (Lightning Gun): when isCharge is true the weapon block shows a
    // "CHARGE" readout + blue bar instead of "MAG / RESERVE". chargeCur is the live
    // charge, chargeCap the stacking ceiling (bar = chargeCur/chargeCap).
    bool  isCharge     = false;
    int   chargeCur    = 0;
    int   chargeCap    = 0;
    // PASSIVE REGEN readout. The HUD MUST NOT LIE about this (it already printed a
    // false ammo model once — "200 / 600" for a gun with neither a mag nor a reserve —
    // and that ambiguity is why nobody could tell what the gun was doing):
    //   chargeRegen     — the pool is refilling RIGHT NOW (label reads RECHARGING).
    //   chargeRegenSlow — ...and it is in the HALF-SPEED band (>= chargeSlowAbove), so
    //                     the label says SLOW rather than implying one uniform speed.
    //   chargeSlowAbove — the charge at which regen halves; drawn as a NOTCH on the bar
    //                     so the crawl-point is visible, not folklore. 0 = no notch.
    bool  chargeRegen     = false;
    bool  chargeRegenSlow = false;
    int   chargeSlowAbove = 0;
    const char* objective = "";  // current objective text (may be empty)
    int   enemiesRemaining = -1; // live enemy count under the objective; <0 = hide
    float damageFlash  = 0.0f;   // [0,1] red hit flash strength
    bool  showCrosshair= true;
    bool  alive        = true;
    int   dispW        = 0;      // live framebuffer size (drives the menu RESOLUTION readout)
    int   dispH        = 0;

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

    // World-map WAYPOINT (app/world_map.*): drawn on the radar as a magenta
    // marker, or — when it lies beyond the radar radius — as an EDGE-CLAMPED
    // chevron with a distance readout under the box. The host feeds the world
    // XZ while a waypoint is set (wpValid=false leaves the radar unchanged).
    bool  wpValid = false;
    float wpX = 0.0f;
    float wpZ = 0.0f;

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
    float musicVol = 0.0f;         // "Music Volume" [0,1] -> setMusicVolume (playtest: MUTED by default; raise via slider)
    float sfxVol   = 1.0f;         // "SFX Volume"   [0,1] -> setMasterSfxVolume

    // ---- Flight mode (Act-3 space pilot): 0=Arcade, 1=Assist, 2=Loose. The
    // "Flight Mode" row CYCLES this; the host bridges it to the game's shared
    // flight-mode latch + persists it. Kept a plain int so this UI layer stays
    // engine/game-agnostic (no space_pilot.h dependency).
    int   flightMode = 0;

    // ---- Advanced (dev) group -------------------------------------------
    // Collapsed by default so the shipping Settings panel reads unchanged.
    // advancedOpen is pure UI state (NOT persisted); the rows nested under it
    // are dev conveniences the host persists individually.
    bool  advancedOpen = false;    // "ADVANCED" section expanded this session
    bool  skipIntro    = false;    // "Skip Intro" -> host skips the intro sequence
                                   // (persisted; equivalent to --skipintro. F9
                                   // still skips a running intro at any time.)

    // ---- Row visibility (all default TRUE = the campaign screen, unchanged).
    // A --world host reusing this screen hides the rows it has no consumer for
    // (NO_SLOP rule 6: a knob wired to nothing is a lie, so it must not draw).
    // host_menu.h (the tunnel/world game menu) turns off audio (no music
    // system), flight mode (no spaceflight), set-default (no settings file)
    // and the dev Advanced group.
    bool  showAudio      = true;   // Music toggle + Music/SFX volume sliders
    bool  showFlightMode = true;   // the Flight Mode cycle row
    bool  showAdvanced   = true;   // the collapsed ADVANCED (dev) group
    bool  showSetDefault = true;   // the resolution row's SET DEFAULT button
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
// Worlds = the player picked TRAVEL / WORLD SELECT: the host opens the world/place
// selection menu (app/world_menu.*). The pause screen is where it belongs — it is the
// game's own menu, not a dev console.
enum class PauseAction : uint8_t {
    None = 0, Save = 1, Load = 2, Worlds = 3, Editor = 4,
    // ---- rows only a --world host offers (see PauseRows) ----
    WeatherPanel = 5, LightingPanel = 6, WorldMap = 7, Console = 8, QuitToDesktop = 9
};

// WHICH ROWS THIS PAUSE SCREEN OFFERS.
//
// There is ONE pause menu in this game. Before this struct existed the tunnel
// host had grown a second `PAUSED` panel with its own copy of the chrome maths
// (panel width, title size, row height, gap, dim quad) — two implementations of
// one screen, which is precisely the drift NO_SLOP rule 4 is about. The rows a
// host can't honour are the reason a copy gets made, so the rows are data now.
//
// Every field defaults to TODAY'S CAMPAIGN SCREEN, so `update(ui, action)` and
// `update(ui, action, showEditor)` are unchanged — same rows, same geometry, to
// the pixel (the panel-height formula below is pinned to reproduce the 6-row
// number exactly). A --world host switches the campaign-only rows off and its
// own on: there is no save system, no main menu to quit to and no TRAVEL in the
// tunnel world, and a row wired to nothing is a lie (NO_SLOP rule 6).
struct PauseRows {
    bool resume        = true;
    bool travel        = true;   // TRAVEL / WORLDS   -> PauseAction::Worlds
    bool save          = true;   // SAVE CHECKPOINT   -> PauseAction::Save
    bool load          = true;   // LOAD CHECKPOINT   -> PauseAction::Load
    bool settings      = true;   // SETTINGS          -> GameState::Settings
    bool editor        = false;  // LEVEL EDITOR      -> PauseAction::Editor (dev, cvar ui_editor)
    bool quitToMenu    = true;   // QUIT TO MENU      -> GameState::MainMenu
    // ---- world-host rows. All default OFF: the campaign screen never grows a
    // row it cannot service, and no existing capture moves.
    bool weatherPanel  = false;  // WEATHER PANEL (F4)  -> PauseAction::WeatherPanel
    bool lightingPanel = false;  // LIGHTING PANEL (F5) -> PauseAction::LightingPanel
    bool worldMap      = false;  // WORLD MAP (M)       -> PauseAction::WorldMap
    bool console       = false;  // CONSOLE (~)         -> PauseAction::Console
    bool quitToDesktop = false;  // QUIT TO DESKTOP     -> PauseAction::QuitToDesktop
    // Optional single hint line under the rows (null = none, as the campaign has).
    const char* hint   = nullptr;
    // Rows actually drawn, in the fixed order update() emits them.
    int count() const {
        return (int)resume + travel + save + load + settings + editor + quitToMenu
             + weatherPanel + lightingPanel + worldMap + console + quitToDesktop;
    }
};

// The pause overlay (drawn over a frozen, dimmed scene).
class PauseMenu {
public:
    // Returns: Paused (stay), Playing (RESUME), Settings (SETTINGS), MainMenu
    // (QUIT TO MENU). `outAction` is set on the frame an action row is
    // activated (the returned state stays Paused in that case).
    GameState update(UiContext& ui, PauseAction& outAction, const PauseRows& rows);
    // Campaign convenience: today's row set, with the dev LEVEL EDITOR row
    // optional (cvar ui_editor). Most players never need to edit a level, and a
    // shipping pause menu should not offer them a level editor.
    GameState update(UiContext& ui, PauseAction& outAction, bool showEditor = false) {
        PauseRows r{}; r.editor = showEditor;
        return update(ui, outAction, r);
    }
};

// True the frame the user picked LEVEL EDITOR in the pause menu.


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

    // True the frame the user picked TRAVEL / WORLDS in the pause menu. The host
    // opens the world/place selection menu (app/world_menu.h), then clears this and
    // takes the game out of Paused so the menu owns the screen.
    bool wantWorldMenu() const { return m_pendingAction == PauseAction::Worlds; }
    // True the frame the user picked LEVEL EDITOR (dev builds only — see showEditor).
    bool wantEditor() const { return m_pendingAction == PauseAction::Editor; }
    // Host turns the LEVEL EDITOR row on (cvar ui_editor). Default OFF: a shipping
    // pause menu should not offer a level editor to someone who just wants to play.
    void setShowEditorRow(bool on) { m_showEditorRow = on; }
    void clearEditorRequest() { m_pendingAction = PauseAction::None; }
    void clearWorldMenuRequest() { m_pendingAction = PauseAction::None; }
    // Force the controller back to Playing (the host does this when the world menu
    // takes over from the pause screen, and after a world load lands).
    void resumePlaying() { m_state = GameState::Playing; }
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
    bool          m_showEditorRow = false;   // cvar ui_editor (host-driven)
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
