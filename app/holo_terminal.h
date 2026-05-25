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

    void setSubmitSink(SubmitFn fn) { m_submit = std::move(fn); }

    // ---- Readout (the displayed lines above the input field). ----
    void setLines(std::vector<std::string> lines) { m_lines = std::move(lines); }
    void addLine(const std::string& s) { m_lines.push_back(s); }
    const std::vector<std::string>& lines() const { return m_lines; }

    // ---- Interaction / input mode. ----
    bool active() const { return m_active; }
    void setActive(bool on) { m_active = on; }          // host toggles on E near the screen
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

private:
    uint32_t       m_entity = kNoLink;
    x3::phys::Vec3 m_pos{};
    float          m_width = 1.4f, m_height = 0.9f;
    // Scene + screen-entity bookkeeping for the time-driven shimmer (set in build()).
    // update() modulates this entity's emissive each frame; null Scene => no shimmer
    // (the headless self-test path, which never calls build()).
    Scene*         m_scene = nullptr;
    uint32_t       m_scanEntity = kNoLink;   // the scrolling scanline overlay quad
    float          m_clock = 0.0f;           // shimmer animation clock (seconds)
    float          m_emBase[4] = { 0.18f, 0.70f, 1.0f, 1.9f };  // base screen emissive
    x3::rhi::TextureHandle m_holoTex{};       // the procedural hologram UI texture

    std::vector<std::string> m_lines;     // readout
    std::string    m_input;               // the editable input line
    bool           m_active = false;
    float          m_blink = 0.0f;        // cursor blink timer
    bool           m_cursorOn = true;
    SubmitFn       m_submit;
    float          m_textColor[4] = { 0.85f, 0.97f, 1.0f, 1.0f };  // bright cyan-white, high contrast
    std::vector<uint32_t> m_decor;        // bezel / arm / trace entity ids (visual only)
    static constexpr size_t kMaxInput = 32;
};

// Headless self-test (--test-holoterm): boot readout is present (not blank), typing
// builds the input line, backspace edits it, submit calls the sink with the value
// and clears (accept) / keeps a reject line, and the cursor blinks. Asserts H0-H4.
bool runHoloTerminalSelfTest();

} // namespace x3::game
