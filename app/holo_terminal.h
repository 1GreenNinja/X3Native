#pragma once
// Holographic terminal (Tim: Jake wakes in his cell facing a terminal that is
// "old and blank" — make it a live holographic translucent screen with readout
// text + typed input). A thin TRANSLUCENT EMISSIVE screen quad in the world + a
// text model (readout lines + an input line + blinking cursor) + an input state
// machine. The host renders the text over the screen via worldToScreen +
// drawHudText (the door-prompt / keypad pattern) and routes typed chars in.
//
// Game/slice code only; engine/ stays pure.
#include "holo_panel.h"
#include "scene.h"

#include "engine/rhi/IRenderDevice.h"

#include <functional>
#include <string>
#include <vector>

namespace x3::game {

class HoloTerminal {
public:
    // Fires when the player submits the input line (Enter). `value` is the typed
    // text; return true to ACCEPT (clears the input + a confirmation line), false
    // to reject (flashes + clears). Optional — default accepts.
    using SubmitFn = std::function<bool(const std::string& value)>;

    // Build the screen in the world: a thin SHINY translucent-blue panel centered at
    // `pos` (facing -Z toward Jake's spawn), `width` x `height` m, with an emissive
    // rounded-look frame bezel, AND a glass ARM dropping from the ceiling (`ceilingY`,
    // 0 = auto pos.y+1.7) carrying emissive fiber-optic (cyan) + copper (amber) traces
    // inside the glass. Registers all render entities in `scene` via `device`. Seeds
    // the boot readout (so it's no longer blank).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::Vec3 pos, float yaw = 0.0f, float width = 1.4f, float height = 0.9f,
               float ceilingY = 0.0f);

    // High-contrast readout color the host uses for drawHudText (bright vs the blue
    // glass). Exposed so the in-app render layer matches the art direction.
    const float* textColor() const { return m_textColor; }
    // W4-2 (HoloPanel platform): host-settable readout INK. While set, the on-glass
    // bake tints body rows with this color (VIGIL presence = orange); resetTextColor()
    // returns to the authored cyan ink. Default path is untouched (basin-safe).
    void setTextColor(float r, float g, float b, float a = 1.0f) {
        m_textColor[0] = r; m_textColor[1] = g; m_textColor[2] = b; m_textColor[3] = a;
        m_inkOverride = true; m_texDirty = true;
    }
    void resetTextColor() {
        m_textColor[0] = 0.55f; m_textColor[1] = 0.80f; m_textColor[2] = 1.0f; m_textColor[3] = 1.0f;
        m_inkOverride = false; m_texDirty = true;
    }

    // Free every GPU resource build() created (the glass/bezel/arm/trace meshes +
    // the baked hologram texture). Added for the RIFTHUB consoles: the hub authors
    // EIGHT of these and its smoketest gates on allocationCount == 0, so the
    // platform needed a teardown path. Safe to call unbuilt / twice.
    void shutdown(x3::rhi::IRenderDevice& device);

    void setSubmitSink(SubmitFn fn) { m_submit = std::move(fn); }

    // ---- LAYOUT (HoloPanel platform) --------------------------------------------
    // Cell (default): the detention terminal's owner-approved composition — a narrow
    //   left data column, a center schematic node, a right column of warning icons and
    //   value bars. The text lives in the left ~27% of the glass.
    // Readout: TEXT FIRST. The center schematic is dropped and the text zone widens to
    //   most of the glass, so the type is roughly twice the size. For panels whose whole
    //   job is to be READ from a couple of metres away — the rifthub rift consoles,
    //   which must say where the portal goes plus five live parameter rows. The frame,
    //   header, warning icons and data strip are unchanged, so it is the same object.
    // Call before/after build(); it re-bakes on the next update().
    enum class Layout { Cell, Readout };
    void setLayout(Layout l) { if (m_layout != l) { m_layout = l; m_texDirty = true; } }
    Layout layout() const { return m_layout; }

    // ---- Readout (the displayed lines above the input field). ----
    // setLines/addLine mark the on-glass texture DIRTY so the next update() re-bakes
    // the readout into the hologram pixels (see regenTexture()). The text lives ON the
    // glass — it tilts with the panel — instead of as a flat camera-facing overlay.
    void setLines(std::vector<std::string> lines) { m_lines = std::move(lines); m_texDirty = true; }
    void addLine(const std::string& s) { m_lines.push_back(s); m_texDirty = true; }
    const std::vector<std::string>& lines() const { return m_lines; }
    // ---- Streaming readout (LLM freeform answers stream onto the glass). ----
    // Rewrite the LAST readout line in place (the host appends streamed tokens
    // to it as they arrive; the glass re-bakes on the host's throttle).
    void setLastLine(const std::string& s) {
        if (m_lines.empty()) m_lines.push_back(s); else m_lines.back() = s;
        m_texDirty = true;
    }
    // Keep at most `maxBody` body rows (line 0, the header TITLE, is always
    // kept). Old scrollback rows fall off the top, like a real terminal.
    void trimBody(size_t maxBody) {
        if (m_lines.size() <= 1 + maxBody) return;
        m_lines.erase(m_lines.begin() + 1,
                      m_lines.begin() + (std::ptrdiff_t)(m_lines.size() - maxBody));
        m_texDirty = true;
    }
    // Drop the typed input WITHOUT submitting (the freeform path consumes the
    // text itself and echoes it as a readout line instead).
    void clearInput() { if (!m_input.empty()) { m_input.clear(); m_texDirty = true; } }

    // ---- Interaction / input mode. ----
    bool active() const { return m_active; }
    void setActive(bool on);                             // host toggles on E near the screen
    // Feed input while active (rising-edge handled by the host):
    void pushChar(char c);                               // append a printable char
    void backspace();
    bool submit();                                       // commit the input line (calls the sink)
    const std::string& input() const { return m_input; }

    // Advance the cursor blink AND the holographic shimmer. Call each frame. When
    // built() (a Scene + screen entity exist), this also drives a slow time-based
    // EMISSIVE pulse + scanline-scroll on the glass so the hologram shimmers (subtle,
    // not seizure-y). Safe to call with no build (the self-test): shimmer is skipped.
    void update(float dt);
    bool cursorOn() const { return m_cursorOn; }

    // Animation clock (seconds) the host can read to scroll the on-glass scanlines /
    // pulse the text alpha in lockstep with the panel shimmer.
    float clock() const { return m_clock; }

    // World anchor (panel center) for the host's worldToScreen text placement.
    x3::phys::Vec3 anchor() const { return m_pos; }
    // Rigidly move the whole terminal (glass + frame + ceiling pipe + glow) to a new
    // anchor — for a MOVING mount like the elevator cab, so the control panel rides down
    // with the car. Keeps m_pos in sync for worldToScreen text placement.
    void reposition(x3::phys::Vec3 newPos) { m_panel.reposition(newPos); m_pos = newPos; }
    uint32_t entity() const;          // the platform's screen pane
    bool built() const;
    // The platform fixture (glow-light suggestion, pane entity, teardown).
    const HoloPanel& panel() const { return m_panel; }

    // True when the readout text is baked ON THE GLASS (stb_truetype rasterized it
    // into the hologram texture). The host uses this to SKIP its legacy 2D overlay so
    // the text isn't drawn twice. Only false if the embedded font failed to load, in
    // which case the host falls back to the worldToScreen overlay.
    bool textOnGlass() const { return m_textOnGlass; }

    // ---- REGRESSION GUARD: "the screen is a featureless blue slab" ----------------
    // This bug has been re-fixed ~10 times. It is now TESTABLE. True only when the
    // terminal is standing in a Scene AND its screen entity actually has the baked
    // readout texture BOUND, AND the readout has lines, AND the glyphs rasterized.
    // A blank screen — no texture, no text, no lines — returns false, so a caller
    // (--test-rifthub) can fail the build instead of shipping a blue rectangle.
    bool screenHasContent() const;
    // The live handle bound to the screen (for tests that want to compare identity).
    x3::rhi::TextureHandle screenTexture() const;

private:
    // Re-bake the readout (static lines + live input line) INTO the hologram texture
    // and re-upload it, then point both the base + scanline quads at the new handle.
    // Called from update() when m_texDirty (input/readout changed) — NOT every frame.
    void regenTexture();

    // THE FIXTURE. Black glass + chrome round-pipe frame + ceiling support pipe, plus
    // the shimmer and the teardown path — all of it lives in the platform. This class
    // owns CONTENT and INPUT; it owns no meshes, no textures and no entities.
    HoloPanel      m_panel;

    x3::phys::Vec3 m_pos{};
    float          m_width = 1.4f, m_height = 0.9f;
    float          m_clock = 0.0f;            // readout animation clock (seconds)
    x3::rhi::IRenderDevice* m_device = nullptr;
    uint32_t       m_texN = 1024;             // hologram texture resolution (square)
    bool           m_texDirty = false;        // set when lines/input change; cleared on regen
    bool           m_lastInputShown = false;  // whether the live input line was baked last regen
    bool           m_textOnGlass = false;     // true once text baked into the texture (font ok)

    std::vector<std::string> m_lines;     // readout
    std::string    m_input;               // the editable input line
    bool           m_active = false;
    float          m_blink = 0.0f;        // cursor blink timer
    bool           m_cursorOn = true;
    SubmitFn       m_submit;
    float          m_textColor[4] = { 0.55f, 0.80f, 1.0f, 1.0f };  // blue-white (NOT cyan)
    bool           m_inkOverride = false;   // W4-2: bake body rows with m_textColor (VIGIL orange)
    Layout         m_layout = Layout::Cell; // Cell = the approved detention composition
    static constexpr size_t kMaxInput = 72;   // freeform questions need room (was 32)
};

// HEADLESS INK PROBE (no device, no Scene). Bakes the hologram texture for `lines` +
// `inputLine` exactly as the real panel does, and returns the fraction of pixels that
// carry INK (luminance above a legibility threshold) inside the readout's text zone.
// A featureless slab returns ~0; a panel with a real readout returns a healthy few
// percent. This is what lets a test assert "the screen HAS CONTENT" without a GPU.
float holoReadoutInkFraction(const std::vector<std::string>& lines,
                             const std::string& inputLine = "", bool wideReadout = false);

// HEADLESS PALETTE PROBE. Bakes the readout and reports the fraction of the text zone
// that reads dominantly BLUE / GREEN / ORANGE — so a test can assert the OWNER'S
// PALETTE, not merely that the screen is lit. Cyan fails the blue test by construction
// (the blue predicate demands b > g by a real margin; cyan has g ~= b).
void holoReadoutPalette(const std::vector<std::string>& lines, bool wideReadout,
                        float& blueF, float& greenF, float& orangeF,
                        const float* inkOverride = nullptr);

// Headless self-test (--test-holoterm): boot readout is present (not blank), typing
// builds the input line, backspace edits it, submit calls the sink with the value
// and clears (accept) / keeps a reject line, and the cursor blinks. Asserts H0-H4.
// Bake a DARK-GLASS ROUNDED MEDICAL-VITALS MONITOR into an RGBA8 n x n buffer (the
// F2 rescue-room wall screen). Same black-glass line-art recipe as the flagship
// terminal: a near-black rounded pane with a faint scanline field + rounded bezel, a
// green ECG heart-rate trace, and glowing green/cyan vitals rows (HR / BP / SpO2 /
// TEMP / RESP) under the captive's NAME header. The bright texels bloom over the dark
// pane (drive it as an emissiveTex over a near-black albedo -- the ACES glow law).
std::vector<uint8_t> bakeMedicalMonitor(uint32_t n, const std::string& name);

bool runHoloTerminalSelfTest();

} // namespace x3::game
