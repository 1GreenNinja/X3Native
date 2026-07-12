#pragma once
// Holographic terminal (Tim: Jake wakes in his cell facing a terminal that is
// "old and blank" — make it a live holographic translucent screen with readout
// text + typed input). A thin TRANSLUCENT EMISSIVE screen quad in the world + a
// text model (readout lines + an input line + blinking cursor) + an input state
// machine. The host renders the text over the screen via worldToScreen +
// drawHudText (the door-prompt / keypad pattern) and routes typed chars in.
//
// Game/slice code only; engine/ stays pure.
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
        m_textColor[0] = 0.85f; m_textColor[1] = 0.97f; m_textColor[2] = 1.0f; m_textColor[3] = 1.0f;
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
    uint32_t entity() const { return m_entity; }
    bool built() const { return m_entity != kNoLink; }

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
    x3::rhi::TextureHandle screenTexture() const { return m_holoTex; }

private:
    // Re-bake the readout (static lines + live input line) INTO the hologram texture
    // and re-upload it, then point both the base + scanline quads at the new handle.
    // Called from update() when m_texDirty (input/readout changed) — NOT every frame.
    void regenTexture();

    uint32_t       m_entity = kNoLink;
    x3::phys::Vec3 m_pos{};
    float          m_width = 1.4f, m_height = 0.9f;
    // Scene + screen-entity bookkeeping for the time-driven shimmer (set in build()).
    // update() modulates this entity's emissive each frame; null Scene => no shimmer
    // (the headless self-test path, which never calls build()).
    Scene*         m_scene = nullptr;
    // (The scrolling "scanline overlay quad" is GONE. It was a second GLASS pane in
    //  front of the screen, and glass writes depth in the depth pre-pass, so it
    //  DEPTH-REJECTED the readout instead of compositing over it — the featureless
    //  blue slab. See build(). Never put geometry in front of the screen.)
    float          m_clock = 0.0f;           // shimmer animation clock (seconds)
    float          m_emBase[4] = { 1.0f, 1.0f, 1.0f, 2.1f };    // base screen emissive (x texel)
    x3::rhi::TextureHandle m_holoTex{};       // the procedural hologram UI texture
    // ON-GLASS TEXT bake state. The readout is rasterized into m_holoTex via
    // stb_truetype so it sits ON the glass (tilts with the panel) like a Babylon
    // DynamicTexture — NOT as a camera-facing 2D overlay. We keep the device + texture
    // resolution so regenTexture() can re-create + re-upload on change.
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
    float          m_textColor[4] = { 0.85f, 0.97f, 1.0f, 1.0f };  // bright cyan-white, high contrast
    bool           m_inkOverride = false;   // W4-2: bake body rows with m_textColor (VIGIL orange)
    Layout         m_layout = Layout::Cell; // Cell = the approved detention composition
    std::vector<uint32_t> m_decor;        // bezel / arm / trace entity ids (visual only)
    std::vector<x3::rhi::MeshHandle> m_meshes;  // every mesh build() created (shutdown frees them)
    static constexpr size_t kMaxInput = 72;   // freeform questions need room (was 32)
};

// HEADLESS INK PROBE (no device, no Scene). Bakes the hologram texture for `lines` +
// `inputLine` exactly as the real panel does, and returns the fraction of pixels that
// carry INK (luminance above a legibility threshold) inside the readout's text zone.
// A featureless slab returns ~0; a panel with a real readout returns a healthy few
// percent. This is what lets a test assert "the screen HAS CONTENT" without a GPU.
float holoReadoutInkFraction(const std::vector<std::string>& lines,
                             const std::string& inputLine = "", bool wideReadout = false);

// Headless self-test (--test-holoterm): boot readout is present (not blank), typing
// builds the input line, backspace edits it, submit calls the sink with the value
// and clears (accept) / keeps a reject line, and the cursor blinks. Asserts H0-H4.
bool runHoloTerminalSelfTest();

} // namespace x3::game
