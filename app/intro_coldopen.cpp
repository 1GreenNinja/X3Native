// INTRO COLD-OPEN implementation. See intro_coldopen.h for the design.
//
// The pure IntroSequence phase machine + the windowed 2D driver + the --test-intro self-test.
// Engine stays pure: this lives under app/ and uses only the public IRenderDevice 2D + mesh API
// (drawHudQuad / drawHudTextF / hudSize) and GLFW for the skip poll (windowed path only).

#include "intro_coldopen.h"

#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::intro {

// ---------------------------------------------------------------------------
// IntroSequence — pure phase machine.
// ---------------------------------------------------------------------------
void IntroSequence::enter(Phase p) {
    m_phase  = p;
    m_phaseT = 0.0f;
}

float IntroSequence::phaseFrac() const {
    float dur = 1.0f;
    switch (m_phase) {
        case Phase::Flight:    dur = m_t.flight; break;
        case Phase::Hit:       dur = m_t.hit; break;
        case Phase::Whiteout:  dur = m_skipped ? m_t.skipWhiteout  : m_t.whiteout; break;
        case Phase::TitleCard: dur = m_skipped ? m_t.skipTitleCard : m_t.titleCard; break;
        default: return 1.0f;
    }
    if (dur <= 1e-4f) return 1.0f;
    float f = m_phaseT / dur;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

bool IntroSequence::enemyVisible() const {
    if (m_phase == Phase::Flight) return m_phaseT >= m_t.enemyAt;
    return m_phase == Phase::Hit;   // still on-screen through the strike
}

bool IntroSequence::pulseActive() const {
    // The pulse charges in the last ~second of Flight (enemy locked on) and lands in Hit.
    if (m_phase == Phase::Flight) return m_phaseT >= (m_t.flight - 1.0f);
    return m_phase == Phase::Hit;
}

void IntroSequence::tick(float dt) {
    if (m_phase == Phase::Done) return;
    if (dt < 0.0f) dt = 0.0f;
    m_phaseT += dt;
    m_total  += dt;

    switch (m_phase) {
        case Phase::Flight:
            if (m_phaseT >= m_t.flight) enter(Phase::Hit);
            break;
        case Phase::Hit:
            if (m_phaseT >= m_t.hit) enter(Phase::Whiteout);
            break;
        case Phase::Whiteout:
            if (m_phaseT >= (m_skipped ? m_t.skipWhiteout : m_t.whiteout)) enter(Phase::TitleCard);
            break;
        case Phase::TitleCard:
            if (m_phaseT >= (m_skipped ? m_t.skipTitleCard : m_t.titleCard)) enter(Phase::Done);
            break;
        default:
            break;
    }
}

void IntroSequence::skip() {
    if (m_phase == Phase::Done) return;
    m_skipped = true;
    // A skip during the title card finishes immediately; otherwise collapse to the short
    // white-out -> title-card tail so the "6 MONTHS LATER" payload is always seen.
    if (m_phase == Phase::TitleCard) {
        enter(Phase::Done);
    } else if (m_phase == Phase::Whiteout) {
        // already in the white-out: just shorten (skipped flag handles the duration).
        m_phaseT = std::min(m_phaseT, m_t.skipWhiteout);
    } else {
        enter(Phase::Whiteout);
    }
}

// ---------------------------------------------------------------------------
// Windowed driver — renders the scripted flythrough on the public 2D + mesh API.
// ---------------------------------------------------------------------------
namespace {

// A deterministic procedural starfield: fixed-count stars on the unit screen, scrolled toward the
// camera (downward + outward from a vanishing point) so the ship reads as flying FORWARD.
struct Starfield {
    static constexpr int kStars = 220;
    float u[kStars];   // [0,1] horizontal seed
    float v[kStars];   // [0,1] vertical seed (scrolls)
    float z[kStars];   // [0.15,1] depth (drives size + speed + brightness)
    void init() {
        uint32_t s = 0x1337u;
        auto rnd = [&]() {
            s = s * 1664525u + 1013904223u;
            return (float)((s >> 8) & 0xFFFFFFu) / (float)0x1000000u;
        };
        for (int i = 0; i < kStars; ++i) {
            u[i] = rnd();
            v[i] = rnd();
            z[i] = 0.15f + rnd() * 0.85f;
        }
    }
};

// Draw a small filled rect in pixel space (thin wrapper).
inline void quad(x3::rhi::IRenderDevice& d, const x3::rhi::FrameContext& f,
                 float x, float y, float w, float h, float r, float g, float b, float a) {
    const float c[4] = { r, g, b, a };
    d.drawHudQuad(f, x, y, w, h, c);
}

} // namespace

bool runIntro(x3::rhi::IRenderDevice& device, GLFWwindow* window, const IntroTiming& timing) {
    // HEADLESS GUARD: no window => no-op (instant hand-off). Smoketests / screenshots / other
    // --world modes never run the cinematic.
    if (!window) return true;

    x3::logInfo("intro cold-open: Jake's last flight — press F8 (or any key / Esc) to skip");

    IntroSequence seq(timing);
    Starfield field; field.init();

    // Edge-detect the skip: any key down OR mouse button OR Esc.
    bool prevAnyKey = false;
    double prevTime = glfwGetTime();

    // The window's analytic sky stays off here — the cold-open is pure 2D over a black field, so
    // it is robust regardless of the device's 3D state and never fights the cell build that
    // follows. We do NOT spawn meshes/lights (no teardown to leak).
    while (!glfwWindowShouldClose(window) && !seq.done()) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = (float)(now - prevTime);
        prevTime = now;
        if (dt > 0.1f)  dt = 0.1f;   // clamp big hitches (alt-tab) so beats don't jump
        if (dt < 0.0f)  dt = 0.0f;

        // ---- Skip poll: dedicated F8 / any key / mouse / Esc (rising edge) ----
        bool anyKey = false;
        // A compact sweep of the common keys + Esc + space + the mouse, plus the
        // dedicated F8 = SKIP INTRO binding (the intro is pre-gameplay, so F8's in-editor
        // Edit<->Play toggle never overlaps this).
        for (int k : { GLFW_KEY_F8, GLFW_KEY_ESCAPE, GLFW_KEY_SPACE, GLFW_KEY_ENTER, GLFW_KEY_W, GLFW_KEY_A,
                       GLFW_KEY_S, GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_F, GLFW_KEY_LEFT_SHIFT,
                       GLFW_KEY_LEFT_CONTROL, GLFW_KEY_TAB }) {
            if (glfwGetKey(window, k) == GLFW_PRESS) { anyKey = true; break; }
        }
        if (!anyKey && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) anyKey = true;
        if (anyKey && !prevAnyKey) seq.skip();
        prevAnyKey = anyKey;

        seq.tick(dt);

        // ---- Render the current phase over the 2D path ----
        uint32_t W = 0, H = 0; device.hudSize(W, H);
        const float fw = (float)W, fh = (float)H;
        const float cx = fw * 0.5f, cy = fh * 0.5f;

        auto frame = device.beginFrame();
        if (frame.valid) {
            const Phase ph = seq.phase();

            // Backdrop: deep space (near-black) for flight/hit; the white-out + title card paint
            // their own field below.
            quad(device, frame, 0, 0, fw, fh, 0.01f, 0.01f, 0.03f, 1.0f);

            if (ph == Phase::Flight || ph == Phase::Hit) {
                // --- Starfield, scrolled toward the viewer (forward flight) ---
                const float scroll = seq.totalElapsed();
                for (int i = 0; i < Starfield::kStars; ++i) {
                    const float zz = field.z[i];
                    // Vertical scroll speed scales with closeness (parallax). Wrap [0,1).
                    float vv = field.v[i] + scroll * (0.04f + zz * 0.22f);
                    vv -= std::floor(vv);
                    // Spread outward from the screen center as it nears (mild streak toward edges).
                    float ux = field.u[i];
                    float px = (ux - 0.5f) * (0.7f + zz * 0.6f) + 0.5f;
                    float sx = px * fw;
                    float sy = vv * fh;
                    float sz = 1.0f + zz * 2.0f;             // star size in px
                    float br = 0.35f + zz * 0.65f;           // brightness by depth
                    quad(device, frame, sx, sy, sz, sz, br, br, br * 1.05f, 1.0f);
                }

                // --- Jake's ship: a simple emissive silhouette near center, gently drifting ---
                const float drift = std::sin(seq.totalElapsed() * 0.7f) * fh * 0.012f;
                float shudder = 0.0f;
                if (ph == Phase::Hit) {
                    // Violent shake on the strike, decaying over the Hit beat.
                    const float k = 1.0f - seq.phaseFrac();
                    shudder = std::sin(seq.phaseElapsed() * 60.0f) * fh * 0.02f * k;
                }
                const float shipCx = cx + shudder;
                const float shipCy = cy + fh * 0.18f + drift + shudder;
                const float sw = fw * 0.10f, sh = fh * 0.05f;
                // Hull (cool blue), cockpit glow, twin engine flares behind it.
                quad(device, frame, shipCx - sw * 0.5f, shipCy - sh * 0.5f, sw, sh, 0.30f, 0.42f, 0.62f, 1.0f);
                quad(device, frame, shipCx - sw * 0.16f, shipCy - sh * 0.3f, sw * 0.32f, sh * 0.4f, 0.55f, 0.85f, 1.0f, 1.0f);
                const float flare = 0.6f + 0.4f * std::sin(seq.totalElapsed() * 25.0f);
                quad(device, frame, shipCx - sw * 0.42f, shipCy + sh * 0.45f, sw * 0.22f, sh * 0.5f, 1.0f, 0.55f * flare, 0.15f, 1.0f);
                quad(device, frame, shipCx + sw * 0.20f, shipCy + sh * 0.45f, sw * 0.22f, sh * 0.5f, 1.0f, 0.55f * flare, 0.15f, 1.0f);

                // --- Enemy ship: a LARGER dark mass sliding down from the top, with a menacing
                //     red running-light. Pops in past enemyAt and looms through the strike. ---
                if (seq.enemyVisible()) {
                    float ein = ph == Phase::Hit ? 1.0f
                              : std::min(1.0f, (seq.phaseElapsed() - timing.enemyAt) /
                                               std::max(0.01f, timing.flight - timing.enemyAt - 1.0f));
                    float ey = -fh * 0.25f + ein * (fh * 0.40f);   // descends into the upper frame
                    const float ew = fw * 0.34f, eh = fh * 0.16f;  // ~3x the player ship
                    quad(device, frame, cx - ew * 0.5f, ey - eh * 0.5f, ew, eh, 0.06f, 0.07f, 0.10f, 1.0f);
                    quad(device, frame, cx - ew * 0.5f, ey + eh * 0.35f, ew, eh * 0.18f, 0.14f, 0.16f, 0.20f, 1.0f);
                    // Red eye / weapon charge.
                    float chg = seq.pulseActive() ? (0.6f + 0.4f * std::sin(seq.totalElapsed() * 30.0f)) : 0.25f;
                    quad(device, frame, cx - ew * 0.04f, ey, ew * 0.08f, eh * 0.3f, chg, 0.06f, 0.06f, 1.0f);

                    // --- Energy pulse: a bright beam from the enemy to Jake's ship ---
                    if (seq.pulseActive()) {
                        const float beamW = fw * 0.02f * (0.6f + 0.6f * std::sin(seq.totalElapsed() * 40.0f));
                        const float top = ey + eh * 0.3f;
                        const float bot = shipCy;
                        quad(device, frame, cx - beamW * 0.5f, top, beamW, bot - top, 1.0f, 0.35f, 0.25f, 1.0f);
                        quad(device, frame, cx - beamW * 0.18f, top, beamW * 0.36f, bot - top, 1.0f, 0.9f, 0.8f, 1.0f);
                    }
                }

                // Title hint (lower third) on the flight beat only.
                if (ph == Phase::Flight) {
                    const float hint[4] = { 0.7f, 0.75f, 0.8f, 0.6f };
                    const char* msg = "[F8] SKIP  -  OR PRESS ANY KEY";
                    float adv = device.textAdvance(x3::rhi::FontRole::News, msg, 16.0f);
                    device.drawHudTextF(frame, x3::rhi::FontRole::News, msg, cx - adv * 0.5f, fh - 40.0f, 16.0f, hint);
                }
            } else if (ph == Phase::Whiteout) {
                // Crash blow-out: fill to white, brightest at the start, easing toward the cut.
                float a = 1.0f - 0.25f * seq.phaseFrac();
                quad(device, frame, 0, 0, fw, fh, 1.0f, 1.0f, 1.0f, a);
            } else if (ph == Phase::TitleCard) {
                // Black field; "6 MONTHS LATER" fades in then holds.
                quad(device, frame, 0, 0, fw, fh, 0.0f, 0.0f, 0.0f, 1.0f);
                float f = seq.phaseFrac();
                float a = std::min(1.0f, f * 4.0f);                 // fade in over the first quarter
                if (f > 0.8f) a = std::max(0.0f, 1.0f - (f - 0.8f) * 5.0f);  // fade out at the tail
                const float col[4] = { 0.92f, 0.93f, 0.96f, a };
                const char* title = "6 MONTHS LATER";
                float px = std::min(fw, fh) * 0.07f;
                float adv = device.textAdvance(x3::rhi::FontRole::Title, title, px);
                device.drawHudTextF(frame, x3::rhi::FontRole::Title, title, cx - adv * 0.5f, cy - px * 0.5f, px, col);
            }
        }
        device.endFrame(frame);
    }

    const bool completed = seq.done();
    x3::logInfo(std::string("intro cold-open: ") +
                (completed ? (seq.skipped() ? "skipped -> handing off to the cell"
                                            : "complete -> handing off to the cell")
                           : "window closed during intro"));
    return completed;
}

// ---------------------------------------------------------------------------
// --test-intro self-test (headless: no window / Vulkan).
// ---------------------------------------------------------------------------
namespace {
int g_pass = 0, g_total = 0;
void check(bool cond, const char* name) {
    ++g_total;
    if (cond) { ++g_pass; x3::logInfo(std::string("[intro-test] PASS ") + name); }
    else      {           x3::logError(std::string("[intro-test] FAIL ") + name); }
}
} // namespace

bool runIntroSelfTest() {
    g_pass = g_total = 0;
    const IntroTiming T{};   // defaults

    // ---- 1) Natural progression: drive with fixed dt; assert each phase is visited in order
    //         and the enemy/pulse flags fire inside the right windows. ----
    {
        IntroSequence s(T);
        check(s.phase() == Phase::Flight, "starts in Flight");
        check(!s.enemyVisible(),          "enemy hidden at start of Flight");

        const float dt = 0.05f;
        bool sawHit = false, sawWhiteout = false, sawTitle = false;
        bool sawEnemy = false, sawPulse = false, enemyBeforeEnemyAt = false;
        // Run well past the full sequence length.
        const float limit = T.flight + T.hit + T.whiteout + T.titleCard + 5.0f;
        for (float t = 0.0f; t < limit && !s.done(); t += dt) {
            // The enemy must NOT be visible early in the flight.
            if (s.phase() == Phase::Flight && s.phaseElapsed() < (T.enemyAt - 0.5f) && s.enemyVisible())
                enemyBeforeEnemyAt = true;
            s.tick(dt);
            if (s.phase() == Phase::Hit)       sawHit = true;
            if (s.phase() == Phase::Whiteout)  sawWhiteout = true;
            if (s.phase() == Phase::TitleCard) sawTitle = true;
            if (s.enemyVisible())              sawEnemy = true;
            if (s.pulseActive())               sawPulse = true;
        }
        check(sawEnemy,             "enemy ship becomes visible during the flight");
        check(!enemyBeforeEnemyAt,  "enemy stays hidden until enemyAt");
        check(sawPulse,             "energy pulse fires (charge/strike)");
        check(sawHit,               "advances through Hit");
        check(sawWhiteout,          "advances through Whiteout (crash white-out)");
        check(sawTitle,             "advances through TitleCard (6 MONTHS LATER)");
        check(s.done(),             "reaches Done (handoff to the cell)");
        check(!s.skipped(),         "natural run is not flagged skipped");
    }

    // ---- 2) Skippable: a skip during Flight must still show the title card, then reach Done. ----
    {
        IntroSequence s(T);
        s.tick(2.0f);                       // a couple seconds into the flight
        check(s.phase() == Phase::Flight, "skip test: in Flight before skip");
        s.skip();
        check(s.skipped(),                "skip sets the skipped flag");
        check(s.phase() == Phase::Whiteout, "skip jumps to the white-out tail");
        // Tick through the compressed tail; the title card MUST still be seen.
        bool sawTitle = false;
        for (int i = 0; i < 400 && !s.done(); ++i) {
            s.tick(0.02f);
            if (s.phase() == Phase::TitleCard) sawTitle = true;
        }
        check(sawTitle, "skip still shows the 6 MONTHS LATER card");
        check(s.done(), "skip reaches Done");
    }

    // ---- 3) A second skip during the title card finishes immediately. ----
    {
        IntroSequence s(T);
        s.skip();                                  // -> Whiteout
        for (int i = 0; i < 200 && s.phase() != Phase::TitleCard; ++i) s.tick(0.02f);
        check(s.phase() == Phase::TitleCard, "second-skip test: reached TitleCard");
        s.skip();                                  // -> Done immediately
        check(s.done(), "second skip during the title card finishes immediately");
    }

    // ---- 4) Idempotent once Done. ----
    {
        IntroSequence s(T);
        for (int i = 0; i < 4000 && !s.done(); ++i) s.tick(0.05f);
        check(s.done(), "drives to Done");
        s.tick(1.0f); s.skip();
        check(s.done() && s.phase() == Phase::Done, "Done is terminal/idempotent");
    }

    x3::logInfo("[intro-test] " + std::to_string(g_pass) + "/" + std::to_string(g_total) + " passed");
    return g_pass == g_total;
}

} // namespace x3::intro
