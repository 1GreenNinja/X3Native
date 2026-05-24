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

    // Build the screen in the world: a thin translucent emissive panel centered at
    // `pos`, facing -Z by default (toward Jake's spawn), `width` x `height` metres.
    // Registers the render entity in `scene` via `device`. Seeds the boot readout
    // (so it's no longer blank).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::Vec3 pos, float yaw = 0.0f, float width = 1.4f, float height = 0.9f);

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

    // Advance the cursor blink. Call each frame.
    void update(float dt);
    bool cursorOn() const { return m_cursorOn; }

    // World anchor (panel center) for the host's worldToScreen text placement.
    x3::phys::Vec3 anchor() const { return m_pos; }
    uint32_t entity() const { return m_entity; }
    bool built() const { return m_entity != kNoLink; }

private:
    uint32_t       m_entity = kNoLink;
    x3::phys::Vec3 m_pos{};
    float          m_width = 1.4f, m_height = 0.9f;

    std::vector<std::string> m_lines;     // readout
    std::string    m_input;               // the editable input line
    bool           m_active = false;
    float          m_blink = 0.0f;        // cursor blink timer
    bool           m_cursorOn = true;
    SubmitFn       m_submit;
    static constexpr size_t kMaxInput = 32;
};

// Headless self-test (--test-holoterm): boot readout is present (not blank), typing
// builds the input line, backspace edits it, submit calls the sink with the value
// and clears (accept) / keeps a reject line, and the cursor blinks. Asserts H0-H4.
bool runHoloTerminalSelfTest();

} // namespace x3::game
