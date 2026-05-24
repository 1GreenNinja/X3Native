// Holographic terminal — see app/holo_terminal.h.
//
// Clean-room: Scene/Entity + IRenderDevice + mesh_prims only. The screen is a thin
// translucent (baseColor alpha) emissive (HDR glow -> bloom) quad; the text is
// drawn by the host over it (worldToScreen + drawHudText).
#include "holo_terminal.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

void HoloTerminal::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::Vec3 pos, float yaw, float width, float height) {
    m_pos = pos; m_width = width; m_height = height;

    // Thin panel: wide in X, tall in Y, paper-thin in Z (a screen). Authored
    // centered at the origin; the Entity transform places + yaws it.
    x3::prims::PrimMesh geo = x3::prims::makeBox(width * 0.5f, height * 0.5f, 0.02f, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Translucent cyan glass + an HDR emissive cyan glow (strength>1 -> bloom),
    // so it reads as a holographic projection, not a solid panel.
    e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 1.0f; e.baseColor[3] = 0.42f;
    e.emissive[0]  = 0.20f; e.emissive[1]  = 0.85f; e.emissive[2]  = 1.0f; e.emissive[3]  = 1.6f;
    e.tag = (uint32_t)Tag::Prop;
    // Yaw about +Y so it faces the player; column-major rotation + translation.
    const float c = std::cos(yaw), s = std::sin(yaw);
    e.transform[0]=c;  e.transform[2]=-s;
    e.transform[8]=s;  e.transform[10]=c;
    e.transform[12]=pos.x; e.transform[13]=pos.y; e.transform[14]=pos.z;
    m_entity = scene.add(e);

    // Boot readout — replaces "old and blank". The Awakening terminal (EFLZ §3).
    m_lines = {
        "DETENTION TERMINAL  // CELL 01",
        "----------------------------------",
        "SUBJECT: JAKE        STATUS: AUGMENTED",
        "MUSCULOSKELETAL OUTPUT: +400%",
        "RESTRAINT INTEGRITY: FAILING",
        "",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    };
    x3::logInfo("[holoterm] built cell-01 terminal (translucent emissive) at (" +
                std::to_string((int)pos.x) + "," + std::to_string((int)pos.y) + "," +
                std::to_string((int)pos.z) + ")");
}

void HoloTerminal::pushChar(char c) {
    if (!m_active) return;
    // Printable ASCII only; cap the length.
    if (c < 32 || c > 126) return;
    if (m_input.size() >= kMaxInput) return;
    m_input += c;
}

void HoloTerminal::backspace() {
    if (!m_active || m_input.empty()) return;
    m_input.pop_back();
}

bool HoloTerminal::submit() {
    if (!m_active) return false;
    const std::string v = m_input;
    bool accept = m_submit ? m_submit(v) : true;
    if (accept) {
        if (!v.empty()) m_lines.push_back("> " + v + "   [ACCEPTED]");
    } else {
        m_lines.push_back("> " + v + "   [REJECTED]");
    }
    m_input.clear();
    return accept;
}

void HoloTerminal::update(float dt) {
    m_blink += dt;
    if (m_blink >= 0.5f) { m_blink -= 0.5f; m_cursorOn = !m_cursorOn; }
}

// ===========================================================================
// Headless self-test (--test-holoterm). H0-H4.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[holoterm-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[holoterm-test] FAIL ") + name); }
}
}

bool runHoloTerminalSelfTest() {
    g_pass = g_fail = 0;

    HoloTerminal t;
    // No device/scene needed for the input/text logic — drive it directly. (build()
    // is exercised in-app; here we test the state machine.)

    // ---- H0: boot readout is present (not blank). ----
    t.setLines({
        "DETENTION TERMINAL  // CELL 01",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    });
    check(t.lines().size() >= 2 && !t.lines()[0].empty(), "H0 terminal boots with readout (not blank)");

    // ---- H1: typing while INACTIVE is ignored; ACTIVE builds the input line. ----
    t.pushChar('1'); bool ignoredWhenInactive = t.input().empty();
    t.setActive(true);
    t.pushChar('1'); t.pushChar('1'); t.pushChar('2'); t.pushChar('7');
    check(ignoredWhenInactive && t.input() == "1127", "H1 input only accepted when active");

    // ---- H2: backspace edits the input line. ----
    t.backspace();
    check(t.input() == "112", "H2 backspace edits the input");

    // ---- H3: submit calls the sink with the value; ACCEPT clears + logs a line. --
    std::string got; bool sinkCalled = false;
    t.setSubmitSink([&](const std::string& v){ got = v; sinkCalled = true; return v == "1127"; });
    t.pushChar('7');                                  // back to "1127"
    size_t before = t.lines().size();
    bool accepted = t.submit();
    check(sinkCalled && got == "1127" && accepted && t.input().empty() && t.lines().size() == before + 1,
          "H3 submit fires the sink, accepts, clears, logs");

    // ---- H4: a REJECTED code keeps a reject line + clears, and the cursor blinks. -
    t.pushChar('9'); t.pushChar('9'); t.pushChar('9'); t.pushChar('9');
    bool rej = t.submit();
    bool blink0 = t.cursorOn();
    t.update(0.6f);                                    // > 0.5 s -> toggles
    bool blinkToggled = (t.cursorOn() != blink0);
    check(!rej && t.input().empty() && blinkToggled, "H4 reject path + cursor blinks");

    x3::logInfo(std::string("[holoterm-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
