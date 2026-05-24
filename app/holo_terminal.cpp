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
                         x3::phys::Vec3 pos, float yaw, float width, float height,
                         float ceilingY) {
    m_pos = pos; m_width = width; m_height = height;
    const float cs = std::cos(yaw), sn = std::sin(yaw);

    // Place a box child: half-extents (hx,hy,hz) at a LOCAL offset (ox,oy,oz) from
    // pos, yaw-rotated into world; translucent `alpha`, emissive {r,g,b,strength}.
    auto addBox = [&](float hx, float hy, float hz, float ox, float oy, float oz,
                      float r, float g, float b, float alpha, float er, float eg, float eb, float es) {
        x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=alpha;
        e.emissive[0]=er; e.emissive[1]=eg; e.emissive[2]=eb; e.emissive[3]=es;
        e.tag = (uint32_t)Tag::Prop;
        // world offset = R_y(yaw) * (ox,oy,oz)
        const float wx = cs*ox + sn*oz, wz = -sn*ox + cs*oz;
        e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=pos.x+wx; e.transform[13]=pos.y+oy; e.transform[14]=pos.z+wz;
        return scene.add(e);
    };

    const float hw = width * 0.5f, hh = height * 0.5f;

    // ---- Frame BEZEL behind the screen: a slightly larger dark glass slab with a
    // bright emissive cyan edge (evokes a shiny rounded-corner frame; true rounded
    // corners need a rounded-rect mesh — graybox approximates with the inset). ----
    m_decor.push_back(addBox(hw+0.07f, hh+0.07f, 0.015f, 0,0,-0.012f,
                             0.04f,0.10f,0.16f, 0.85f,  0.15f,0.70f,1.0f, 0.6f));  // bezel
    // Thin bright emissive border bars (top/bottom/left/right) = the glowing frame.
    m_decor.push_back(addBox(hw+0.07f, 0.02f, 0.02f, 0,  hh+0.05f, 0, 0,0,0,1, 0.2f,0.9f,1.0f, 2.2f));
    m_decor.push_back(addBox(hw+0.07f, 0.02f, 0.02f, 0, -hh-0.05f, 0, 0,0,0,1, 0.2f,0.9f,1.0f, 2.2f));
    m_decor.push_back(addBox(0.02f, hh+0.05f, 0.02f, -hw-0.05f, 0, 0, 0,0,0,1, 0.2f,0.9f,1.0f, 2.2f));
    m_decor.push_back(addBox(0.02f, hh+0.05f, 0.02f,  hw+0.05f, 0, 0, 0,0,0,1, 0.2f,0.9f,1.0f, 2.2f));

    // ---- Glass ARM from the ceiling down to the top of the screen, carrying
    // emissive traces (fiber-optic cyan + copper amber) visible inside the glass. ----
    const float ceil = (ceilingY > 0.0f) ? ceilingY : pos.y + 1.7f;
    const float armTopY = ceil;
    const float armBotY = pos.y + hh + 0.05f;
    const float armH = (armTopY - armBotY) * 0.5f;
    if (armH > 0.05f) {
        const float armMidY = (armTopY + armBotY) * 0.5f - pos.y;  // local oy
        const float armBackZ = -0.06f;                              // behind the screen plane
        // Clear glass tube (translucent, faint blue, slight emissive sheen).
        m_decor.push_back(addBox(0.06f, armH, 0.06f, 0, armMidY, armBackZ,
                                 0.55f,0.75f,0.85f, 0.22f,  0.3f,0.5f,0.7f, 0.3f));
        // Fiber-optic trace (cyan) + copper trace (amber) threaded inside the glass.
        m_decor.push_back(addBox(0.008f, armH, 0.008f, -0.02f, armMidY, armBackZ,
                                 0,0,0,1,  0.2f,0.9f,1.0f, 2.6f));   // cyan fiber
        m_decor.push_back(addBox(0.008f, armH, 0.008f,  0.02f, armMidY, armBackZ,
                                 0,0,0,1,  1.0f,0.55f,0.2f, 2.0f));  // copper trace
    }

    // ---- The SHINY translucent-blue screen panel (drawn last, in front). ----
    x3::prims::PrimMesh geo = x3::prims::makeBox(hw, hh, 0.02f, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Translucent blue glass + a strong HDR cyan glow (shiny bloom source).
    e.baseColor[0] = 0.16f; e.baseColor[1] = 0.62f; e.baseColor[2] = 1.0f; e.baseColor[3] = 0.46f;
    e.emissive[0]  = 0.18f; e.emissive[1]  = 0.70f; e.emissive[2]  = 1.0f; e.emissive[3]  = 1.8f;
    e.tag = (uint32_t)Tag::Prop;
    e.transform[0]=cs;  e.transform[2]=-sn;
    e.transform[8]=sn;  e.transform[10]=cs;
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
