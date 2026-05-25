// Holographic terminal — see app/holo_terminal.h.
//
// Clean-room: Scene/Entity + IRenderDevice + mesh_prims only. The screen is a thin
// translucent (baseColor alpha) emissive (HDR glow -> bloom) quad; the text is
// drawn by the host over it (worldToScreen + drawHudText).
#include "holo_terminal.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {
// ---------------------------------------------------------------------------
// PROCEDURAL HOLOGRAM UI TEXTURE (RGBA8). The screen is drawn as a strongly
// EMISSIVE quad whose COLOR comes from this texture, so the panel reads as a
// projected sci-fi UI (env-art console vibe) instead of a flat MS-Paint slab:
//   * a deep-blue glass base with a vertical inner GRADIENT (brighter at the top,
//     like backlit glass),
//   * fine cyan SCANLINES (horizontal) + a faint vertical GRID (data-readout feel),
//   * a bright HEADER bar across the top (a title strip),
//   * a soft inner VIGNETTE + ROUNDED-CORNER fade (the corners dim toward black so,
//     with the strong emissive + bloom, the panel edges read as a translucent
//     hologram that falls off at rounded corners rather than a hard rectangle).
// The main mesh pass is opaque, so translucency is FAKED by darkening the texture
// (dark + emissive*color -> glows over the dark scene = reads like glowing glass).
// Tileable is irrelevant (one quad, UV 0..1). Output srgb=true (color texture).
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeHologramRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const float fn = (float)n;
    const float r2corner = 0.30f;          // rounded-corner radius (fraction of half-size)
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const float u = (x + 0.5f) / fn;          // 0..1 left->right
            const float v = (y + 0.5f) / fn;          // 0..1 top->bottom

            // Deep-blue glass base with a top->bottom backlit gradient.
            float br = 0.05f, bg = 0.16f, bb = 0.34f;   // base glass (low, so emissive carries it)
            const float grad = 1.0f - v * 0.55f;        // brighter toward the top
            br *= grad; bg *= grad; bb *= grad;

            // Fine horizontal SCANLINES (every few px) + faint vertical GRID lines.
            const float scan = 0.78f + 0.22f * ((y % 4u) < 2u ? 1.0f : 0.55f);
            br *= scan; bg *= scan; bb *= scan;
            if ((x % 28u) < 1u || (y % 28u) < 1u) {     // sparse data-grid
                bg += 0.10f; bb += 0.16f;
            }

            // Bright HEADER strip across the top (a title bar) + a thin underline.
            if (v < 0.12f)              { bg += 0.30f; bb += 0.42f; br += 0.06f; }
            else if (v < 0.135f)        { bg += 0.55f; bb += 0.70f; }   // header underline glow

            // Soft inner VIGNETTE so the center reads brightest.
            const float cx = (u - 0.5f), cy = (v - 0.5f);
            const float rad = std::sqrt(cx * cx + cy * cy) * 1.42f;     // 0 center -> ~1 corner
            const float vig = 1.0f - 0.45f * rad * rad;
            br *= vig; bg *= vig; bb *= vig;

            // ROUNDED-CORNER fade: distance into each corner past the rounding radius
            // crushes toward black so the silhouette reads as a rounded translucent
            // panel (the strong emissive makes the lit area glow; the corners die).
            const float ax = std::fabs(cx) * 2.0f, ay = std::fabs(cy) * 2.0f; // 0..1 each axis
            float fade = 1.0f;
            const float inx = 1.0f - r2corner, iny = 1.0f - r2corner;
            if (ax > inx && ay > iny) {
                const float dx = (ax - inx) / r2corner, dy = (ay - iny) / r2corner;
                const float cd = std::sqrt(dx * dx + dy * dy);           // 0 at radius -> >1 outside
                fade = 1.0f - cd;
                if (fade < 0.0f) fade = 0.0f;
            }
            // Also fade the extreme outer rim a touch so even straight edges feather.
            const float edge = 1.0f - 0.6f * std::max(0.0f, std::max(ax, ay) - 0.92f) / 0.08f;
            fade *= (edge < 0.0f ? 0.0f : edge);

            br *= fade; bg *= fade; bb *= fade;

            auto to8 = [](float c) -> uint8_t {
                int v = (int)(c * 255.0f + 0.5f);
                return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            };
            p[0] = to8(br); p[1] = to8(bg); p[2] = to8(bb); p[3] = 255;
        }
    }
    return px;
}

// A FLAT panel quad (front face only, +Z normal) with cut/rounded corners: a
// center-fan over a ring of perimeter points that follow rounded corners, so the
// SILHOUETTE reads as a rounded rectangle (not a hard MS-Paint box). UVs map the
// panel rect to 0..1 so the hologram texture lands square on it. Half-extents hw,hh.
x3::prims::PrimMesh makeRoundedPanel(float hw, float hh, float corner) {
    x3::prims::PrimMesh m;
    const float r = std::min(corner, std::min(hw, hh) * 0.9f);
    // Perimeter points (CCW), rounding each corner with a small arc.
    std::vector<float> ring;  // x,y pairs
    auto pt = [&](float x, float y){ ring.push_back(x); ring.push_back(y); };
    const int seg = 4;        // arc segments per corner
    // corner centers
    struct C { float cx, cy, a0; } corners[4] = {
        {  hw - r,  hh - r, 0.0f             },   // top-right
        { -hw + r,  hh - r, 3.14159265f*0.5f },   // top-left
        { -hw + r, -hh + r, 3.14159265f      },   // bottom-left
        {  hw - r, -hh + r, 3.14159265f*1.5f },   // bottom-right
    };
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s <= seg; ++s) {
            const float a = corners[c].a0 + (3.14159265f * 0.5f) * ((float)s / (float)seg);
            pt(corners[c].cx + std::cos(a) * r, corners[c].cy + std::sin(a) * r);
        }
    }
    const uint32_t rn = (uint32_t)(ring.size() / 2);
    auto push = [&](float x, float y){
        // UV: panel rect [-hw,hw]x[-hh,hh] -> [0,1]; v flips so row 0 = top.
        const float uu = (x + hw) / (2.0f * hw);
        const float vv = 1.0f - (y + hh) / (2.0f * hh);
        m.verts.push_back({{x, y, 0.0f}, {0.0f, 0.0f, 1.0f}, {uu, vv}});
    };
    push(0.0f, 0.0f);                 // center vertex (index 0)
    for (uint32_t i = 0; i < rn; ++i) push(ring[i*2], ring[i*2+1]);
    for (uint32_t i = 0; i < rn; ++i) {
        const uint32_t a = 1 + i, b = 1 + ((i + 1) % rn);
        // CCW so it faces +Z (matches VK_FRONT_FACE_COUNTER_CLOCKWISE).
        m.index.push_back(0); m.index.push_back(a); m.index.push_back(b);
    }
    return m;
}
} // namespace

void HoloTerminal::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::Vec3 pos, float yaw, float width, float height,
                         float ceilingY) {
    m_pos = pos; m_width = width; m_height = height;
    m_scene = &scene;
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

    // Build the procedural hologram UI texture once (shared by the screen quad).
    {
        const uint32_t TN = 256;
        std::vector<uint8_t> holo = makeHologramRGBA(TN);
        m_holoTex = device.createTexture(holo.data(), TN, TN, /*srgb*/true);
    }

    // ---- Thin emissive sci-fi BEZEL: four slim glowing rounded-look rails framing
    // the glass (NOT a chunky gray box). A faint dark backplate sits just behind so
    // the glow rim reads against it. The rails are thin + bright so they bloom like
    // the env-art console trim. ----
    m_decor.push_back(addBox(hw+0.05f, hh+0.05f, 0.010f, 0,0,-0.018f,
                             0.02f,0.05f,0.10f, 1.0f,  0.05f,0.18f,0.32f, 0.5f));  // dark backplate (glow catcher)
    m_decor.push_back(addBox(hw+0.06f, 0.012f, 0.018f, 0,  hh+0.045f, -0.004f, 0,0,0,1, 0.25f,0.85f,1.0f, 2.6f)); // top rail
    m_decor.push_back(addBox(hw+0.06f, 0.012f, 0.018f, 0, -hh-0.045f, -0.004f, 0,0,0,1, 0.25f,0.85f,1.0f, 2.6f)); // bottom rail
    m_decor.push_back(addBox(0.012f, hh+0.045f, 0.018f, -hw-0.045f, 0, -0.004f, 0,0,0,1, 0.25f,0.85f,1.0f, 2.6f)); // left rail
    m_decor.push_back(addBox(0.012f, hh+0.045f, 0.018f,  hw+0.045f, 0, -0.004f, 0,0,0,1, 0.25f,0.85f,1.0f, 2.6f)); // right rail
    // Bright corner accent nubs (the rounded-corner "cap" highlights).
    const float cnx = hw+0.045f, cny = hh+0.045f;
    m_decor.push_back(addBox(0.022f,0.022f,0.020f,  cnx, cny,-0.004f, 0,0,0,1, 0.5f,0.95f,1.0f, 3.0f));
    m_decor.push_back(addBox(0.022f,0.022f,0.020f, -cnx, cny,-0.004f, 0,0,0,1, 0.5f,0.95f,1.0f, 3.0f));
    m_decor.push_back(addBox(0.022f,0.022f,0.020f,  cnx,-cny,-0.004f, 0,0,0,1, 0.5f,0.95f,1.0f, 3.0f));
    m_decor.push_back(addBox(0.022f,0.022f,0.020f, -cnx,-cny,-0.004f, 0,0,0,1, 0.5f,0.95f,1.0f, 3.0f));

    // ---- Glass ARM from the ceiling down to the top of the screen, carrying
    // emissive traces (fiber-optic cyan + copper amber) visible inside the glass. ----
    const float ceil = (ceilingY > 0.0f) ? ceilingY : pos.y + 1.7f;
    const float armTopY = ceil;
    const float armBotY = pos.y + hh + 0.05f;
    const float armH = (armTopY - armBotY) * 0.5f;
    if (armH > 0.05f) {
        const float armMidY = (armTopY + armBotY) * 0.5f - pos.y;  // local oy
        const float armBackZ = -0.06f;                              // behind the screen plane
        // Slim glass spar (faint blue, slight emissive sheen) — not a fat gray bar.
        m_decor.push_back(addBox(0.035f, armH, 0.035f, 0, armMidY, armBackZ,
                                 0.10f,0.20f,0.30f, 1.0f,  0.12f,0.35f,0.55f, 0.6f));
        // Fiber-optic trace (cyan) + copper trace (amber) threaded inside the glass.
        m_decor.push_back(addBox(0.007f, armH, 0.007f, -0.014f, armMidY, armBackZ - 0.002f,
                                 0,0,0,1,  0.2f,0.9f,1.0f, 2.8f));   // cyan fiber
        m_decor.push_back(addBox(0.007f, armH, 0.007f,  0.014f, armMidY, armBackZ - 0.002f,
                                 0,0,0,1,  1.0f,0.55f,0.2f, 2.2f));  // copper trace
    }

    // ---- The HOLOGRAM screen: a rounded-corner glass quad, dark base color so the
    // EMISSIVE hologram texture + glow carry the look (reads as projected translucent
    // UI, not a solid slab). The texture supplies scanlines/grid/header/vignette +
    // rounded-corner fade; emissive makes it glow + feed bloom. update() pulses the
    // emissive strength + scrolls a scanline overlay for the live shimmer. ----
    x3::prims::PrimMesh geo = makeRoundedPanel(hw, hh, std::min(hw, hh) * 0.30f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.tex = m_holoTex;                                  // procedural hologram UI
    // Dark, slightly-blue base so the lit albedo stays low and the EMISSIVE glow is
    // what you see (a translucent-looking glowing hologram, not an opaque cyan box).
    e.baseColor[0] = 0.45f; e.baseColor[1] = 0.70f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
    m_emBase[0] = 0.22f; m_emBase[1] = 0.72f; m_emBase[2] = 1.0f; m_emBase[3] = 1.9f;
    e.emissive[0]=m_emBase[0]; e.emissive[1]=m_emBase[1]; e.emissive[2]=m_emBase[2]; e.emissive[3]=m_emBase[3];
    e.tag = (uint32_t)Tag::Prop;
    e.transform[0]=cs;  e.transform[2]=-sn;
    e.transform[8]=sn;  e.transform[10]=cs;
    e.transform[12]=pos.x; e.transform[13]=pos.y; e.transform[14]=pos.z;
    m_entity = scene.add(e);

    // ---- A second, slightly-in-front SCANLINE overlay quad: a thin bright cyan
    // emissive sheet whose emissive STRENGTH is animated in update() to scroll a
    // shimmer band down the glass (the moving "projector refresh" line). Same rounded
    // shape, nudged toward the viewer so it composites over the base. ----
    x3::prims::PrimMesh sgeo = makeRoundedPanel(hw*0.98f, hh*0.98f, std::min(hw, hh) * 0.28f);
    Entity se;
    se.mesh = device.createMesh(sgeo.verts.data(), (uint32_t)sgeo.verts.size(),
                                sgeo.index.data(), (uint32_t)sgeo.index.size());
    se.tex = m_holoTex;
    se.baseColor[0]=0.0f; se.baseColor[1]=0.0f; se.baseColor[2]=0.0f; se.baseColor[3]=1.0f;
    se.emissive[0]=0.3f; se.emissive[1]=0.9f; se.emissive[2]=1.0f; se.emissive[3]=0.0f;  // pulsed in update()
    se.tag = (uint32_t)Tag::Prop;
    const float foZ = 0.014f;                            // toward the viewer (front face +Z local)
    const float fwx = cs*0.0f + sn*foZ, fwz = -sn*0.0f + cs*foZ;
    se.transform[0]=cs; se.transform[2]=-sn; se.transform[8]=sn; se.transform[10]=cs;
    se.transform[12]=pos.x+fwx; se.transform[13]=pos.y; se.transform[14]=pos.z+fwz;
    m_scanEntity = scene.add(se);

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

    m_clock += dt;

    // ---- Holographic SHIMMER (only when built into a Scene; the self-test path
    // never calls build() so m_scene stays null and this is skipped). ----
    if (!m_scene || m_entity == kNoLink || m_entity >= m_scene->size()) return;

    // Slow emissive PULSE on the base glass (a gentle breathing glow, ~0.27 Hz) plus
    // a faint higher-frequency flicker (the projector instability). Subtle, bounded.
    const float pulse   = 0.86f + 0.14f * std::sin(m_clock * 1.7f);
    const float flicker = 0.97f + 0.03f * std::sin(m_clock * 13.0f);
    const float k = pulse * flicker;
    Entity& screen = m_scene->get(m_entity);
    screen.emissive[0] = m_emBase[0];
    screen.emissive[1] = m_emBase[1];
    screen.emissive[2] = m_emBase[2];
    screen.emissive[3] = m_emBase[3] * k;

    // Scrolling SCANLINE band: ramp the overlay quad's emissive strength up + down
    // (a moving brightness sweep). Triangle wave over ~2.2 s, peaking modestly so it
    // reads as a refresh sweep, not a strobe.
    if (m_scanEntity != kNoLink && m_scanEntity < m_scene->size()) {
        const float t = std::fmod(m_clock * 0.45f, 1.0f);      // 0..1 sweep phase
        const float band = 0.5f - 0.5f * std::cos(t * 6.2831853f); // smooth 0->1->0
        Entity& scan = m_scene->get(m_scanEntity);
        scan.emissive[3] = 0.20f + 0.55f * band;               // gentle additive sweep
    }
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
