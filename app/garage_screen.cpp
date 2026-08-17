// The garage screen — see garage_screen.h.

#include "garage_screen.h"
#include "hud.h"            // hudRoundRect / hudDisc — curves, not boxes

#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::game {

namespace {
constexpr float kSpinRadPerSec = 0.42f;   // ~15 s a turn: slow enough to read
constexpr float kTau           = 6.28318530718f;
constexpr float kRevealPerSec  = 3.2f;    // ~0.3 s to open

float nmToFtLb(float nm) { return nm * 0.737562f; }
float kgToLb  (float kg) { return kg * 2.20462f; }
float mToIn   (float m)  { return m * 39.3701f; }

const char* driveName(Drivetrain d) {
    switch (d) {
        case Drivetrain::AWD: return "AWD";
        case Drivetrain::FWD: return "FWD";
        default:              return "RWD";
    }
}
}  // namespace

void GarageScreen::build(const CarCatalog& cat) {
    m_cars.clear();
    for (const CarSpec& c : cat.all())
        if (!c.glb.empty()) m_cars.push_back(&c);
    if (m_cursor >= (int)m_cars.size()) m_cursor = 0;
}

void GarageScreen::setOpen(bool o) {
    // Refusing to open on an empty roster is not defensive noise: a chooser
    // with nothing in it draws an empty frame and reads as a broken feature.
    if (o && m_cars.empty()) {
        x3::logWarn("[garage] no cars with art — chooser stays shut");
        return;
    }
    m_open = o;
}

void GarageScreen::moveCursor(int delta) {
    if (m_cars.empty()) return;
    const int n = (int)m_cars.size();
    m_cursor = ((m_cursor + delta) % n + n) % n;   // wraps both ways
}

const CarSpec* GarageScreen::highlighted() const {
    if (m_cars.empty()) return nullptr;
    return m_cars[(size_t)std::clamp(m_cursor, 0, (int)m_cars.size() - 1)];
}

const CarSpec* GarageScreen::at(size_t i) const {
    return i < m_cars.size() ? m_cars[i] : nullptr;
}

void GarageScreen::selectByGlb(const std::string& glbRelPath) {
    for (size_t i = 0; i < m_cars.size(); ++i)
        if (m_cars[i]->glb == glbRelPath) { m_cursor = (int)i; return; }
}

void GarageScreen::tick(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    // The turntable only turns while you are looking at it. A platform
    // spinning behind a closed menu is work nobody sees.
    if (m_open) {
        m_spin += dt * kSpinRadPerSec;
        if (m_spin >= kTau) m_spin -= kTau * std::floor(m_spin / kTau);
    }
    const float target = m_open ? 1.0f : 0.0f;
    const float step   = dt * kRevealPerSec;
    if (m_reveal < target)      m_reveal = std::min(target, m_reveal + step);
    else if (m_reveal > target) m_reveal = std::max(target, m_reveal - step);
}

void GarageScreen::fleetRange(float (*get)(const CarSpec&), float& lo, float& hi) const {
    lo = 1e30f; hi = -1e30f;
    for (const CarSpec* c : m_cars) {
        const float v = get(*c);
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    if (lo > hi) { lo = 0.0f; hi = 1.0f; }
}

void GarageScreen::drawCard(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame) const {
    const CarSpec* c = highlighted();
    if (!c || m_reveal <= 0.001f) return;

    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;

    const float a = m_reveal;                       // fade + slide together
    // Height is the CONTENT height, not a round number: header 80 + figures 47
    // + five bars 85 + up to four wrapped note lines 60 + footer 34. Sized to
    // fit left a dead band under the note that read as an unfinished panel.
    const float cardW = 430.0f, cardH = 316.0f;
    // Slides in from the left edge as it fades up — motion tells you the panel
    // ARRIVED rather than blinking into being.
    const float cx = 34.0f - (1.0f - a) * 60.0f;
    const float cy = (float)h * 0.5f - cardH * 0.5f;

    const float plate[4] = { 0.02f, 0.025f, 0.035f, 0.90f * a };
    const float bezel[4] = { 0.40f, 0.44f, 0.52f, 0.55f * a };
    const float gold [4] = { 1.00f, 0.84f, 0.38f, 1.00f * a };
    const float text [4] = { 0.88f, 0.92f, 0.98f, 1.00f * a };
    const float dim  [4] = { 0.58f, 0.63f, 0.72f, 1.00f * a };
    const float barBg[4] = { 0.10f, 0.12f, 0.16f, 0.95f * a };
    const float barFg[4] = { 0.30f, 0.78f, 1.00f, 1.00f * a };

    hudRoundRect(device, frame, cx - 1.0f, cy - 1.0f, cardW + 2.0f, cardH + 2.0f, 11.0f, bezel);
    hudRoundRect(device, frame, cx, cy, cardW, cardH, 10.0f, plate);

    // ---- header: the name, and a rule under it ----------------------------
    float y = cy + 16.0f;
    device.drawHudText(frame, c->name.c_str(), cx + 20.0f, y, 22.0f, gold);
    y += 30.0f;
    char sub[96];
    std::snprintf(sub, sizeof(sub), "%s   %d of %d",
                  driveName(c->drive), m_cursor + 1, (int)m_cars.size());
    device.drawHudText(frame, sub, cx + 20.0f, y, 12.0f, dim);
    y += 20.0f;
    device.drawHudQuad(frame, cx + 20.0f, y, cardW - 40.0f, 1.0f, bezel);
    y += 14.0f;

    // ---- the figures, in Tim's units --------------------------------------
    // These are the values the car is BUILT with, not decoration.
    char line[128];
    std::snprintf(line, sizeof(line), "%.0f ft-lb  at  %.0f rpm",
                  (double)nmToFtLb(c->torqueNm), (double)c->maxRpm);
    device.drawHudText(frame, line, cx + 20.0f, y, 15.0f, text);
    y += 21.0f;
    std::snprintf(line, sizeof(line), "%.0f lb        CoM %.1f in",
                  (double)kgToLb(c->massKg), (double)mToIn(c->comHeight));
    device.drawHudText(frame, line, cx + 20.0f, y, 15.0f, text);
    y += 26.0f;

    // ---- fleet-relative bars ----------------------------------------------
    // Scaled across the cars YOU HAVE. An absolute bar against an invented
    // maximum is the thing every racing game does and it tells you nothing;
    // "this is the heaviest thing in your garage" is a fact you can act on.
    struct Row { const char* label; float (*get)(const CarSpec&); bool higherIsMore; };
    static const Row rows[] = {
        { "POWER",  [](const CarSpec& s){ return s.torqueNm; },      true },
        { "REVS",   [](const CarSpec& s){ return s.maxRpm; },        true },
        { "GRIP",   [](const CarSpec& s){ return s.gripScale; },     true },
        { "LIGHT",  [](const CarSpec& s){ return -s.massKg; },       true },
        { "LOW",    [](const CarSpec& s){ return -s.comHeight; },    true },
    };
    for (const Row& r : rows) {
        float lo = 0.0f, hi = 1.0f;
        fleetRange(r.get, lo, hi);
        const float v = r.get(*c);
        const float t = (hi - lo) > 1e-6f ? (v - lo) / (hi - lo) : 0.5f;
        device.drawHudText(frame, r.label, cx + 20.0f, y + 1.0f, 11.0f, dim);
        const float bx = cx + 92.0f, bw = cardW - 112.0f, bh = 9.0f;
        hudRoundRect(device, frame, bx, y, bw, bh, bh * 0.5f, barBg);
        if (t > 0.02f)
            hudRoundRect(device, frame, bx, y, bw * t, bh, bh * 0.5f, barFg);
        y += 17.0f;
    }
    y += 6.0f;

    // ---- the note: why this car feels the way it does ---------------------
    // Word-wrapped by hand because the HUD text call has no wrapping and a
    // sentence running off the card is worse than no sentence.
    if (!c->note.empty()) {
        const float px = 11.0f;
        // The HUD font advance is EXACTLY the requested pixel size (it is the
        // embedded 8x8 bitmap scaled), not some fraction of it. Assuming 0.62
        // of it put ~60 characters on a line that fits 35 and ran the sentence
        // clean off the card and across the bay.
        const int   perLine = std::max(8, (int)((cardW - 40.0f) / px));
        // FIRST SENTENCE ONLY. `note` is the spec's provenance — it carries
        // reconciliation history and dates, which belong in the file and the
        // diff, not on a card you glance at while choosing a car. The opening
        // sentence is the one that says what the car IS.
        std::string s = c->note;
        const size_t stop = s.find(". ");
        if (stop != std::string::npos) s = s.substr(0, stop + 1);
        size_t i = 0;
        int lines = 0;
        while (i < s.size() && lines < 4) {
            size_t take = std::min((size_t)perLine, s.size() - i);
            if (i + take < s.size()) {
                const size_t sp = s.rfind(' ', i + take);
                if (sp != std::string::npos && sp > i) take = sp - i;
            }
            device.drawHudText(frame, s.substr(i, take).c_str(),
                               cx + 20.0f, y, px, dim);
            y += px + 4.0f;
            i += take;
            while (i < s.size() && s[i] == ' ') ++i;
            ++lines;
        }
    }

    // ---- controls ---------------------------------------------------------
    const float fy = cy + cardH - 24.0f;
    device.drawHudQuad(frame, cx + 20.0f, fy - 10.0f, cardW - 40.0f, 1.0f, bezel);
    // 36 characters at 10 px = 360, inside the 390 of usable card width. The
    // first version was 46 characters at 11 px (506) and ran "G close" out
    // over the shop floor — the same advance mistake as the note above.
    device.drawHudText(frame, "A/D browse   ENTER take it   G close",
                       cx + 20.0f, fy, 10.0f, dim);
}

// ===========================================================================
// --test-garage
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void gcheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
}  // namespace

bool runGarageSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- garage screen self-test ---");
    char b[288];

    const CarCatalog& cat = CarCatalog::builtin();
    GarageScreen g;
    g.build(cat);

    std::snprintf(b, sizeof(b), "G0 the chooser is non-empty: %u selectable cars",
                  (uint32_t)g.count());
    gcheck(g.count() == 11, b);

    // G1: only cars WITH ART are offered. cars.json's handling targets (the
    // Plaid/NSX/Cobra rows) have no GLB — offering one would put a name on the
    // turntable with nothing standing on it.
    {
        CarCatalog k = CarCatalog::builtin();
        k.loadJson(R"({"cars":[{"id":"ghost","name":"NO ART","massKg":1200}]})");
        GarageScreen g2; g2.build(k);
        bool none = true;
        for (size_t i = 0; i < g2.count(); ++i)
            if (g2.at(i)->id == "ghost") none = false;
        std::snprintf(b, sizeof(b),
            "G1 a spec with no GLB is not offered (%u in catalog, %u selectable)",
            (uint32_t)k.size(), (uint32_t)g2.count());
        gcheck(none && g2.count() == g.count(), b);
    }

    // G2: the cursor WRAPS both ways.
    {
        g.moveCursor(0);
        const int n = (int)g.count();
        g.moveCursor(-1);
        const bool backWraps = (g.cursor() == n - 1);
        g.moveCursor(1);
        const bool fwdWraps = (g.cursor() == 0);
        std::snprintf(b, sizeof(b),
            "G2 the cursor wraps both ways (0 -> %d -> 0)", n - 1);
        gcheck(backWraps && fwdWraps, b);
    }

    // G3: selecting by GLB lands on the car being driven, so opening the
    // screen starts where you are rather than at the top of the list.
    {
        g.selectByGlb("Vehicles/Truck.glb");
        const CarSpec* h = g.highlighted();
        std::snprintf(b, sizeof(b), "G3 selectByGlb lands on the driven car (%s)",
                      h ? h->name.c_str() : "<none>");
        gcheck(h && h->id == "truck", b);
    }

    // G4: the turntable is DETERMINISTIC and only turns while open — no clock
    // reads, no rand, and no work behind a closed menu.
    {
        GarageScreen a, c2;
        a.build(cat); c2.build(cat);
        for (int i = 0; i < 120; ++i) { a.tick(1.0f / 60.0f); c2.tick(1.0f / 60.0f); }
        const bool shutStill = (a.spinRad() == 0.0f);
        a.setOpen(true); c2.setOpen(true);
        for (int i = 0; i < 120; ++i) { a.tick(1.0f / 60.0f); c2.tick(1.0f / 60.0f); }
        std::snprintf(b, sizeof(b),
            "G4 turntable is deterministic and still while shut (spin %.4f rad after 2 s open)",
            (double)a.spinRad());
        gcheck(shutStill && a.spinRad() > 0.5f && a.spinRad() == c2.spinRad(), b);
    }

    // G5: the spin WRAPS instead of growing without bound. A float angle that
    // climbs for an hour loses its low bits and the platform visibly stutters.
    {
        GarageScreen s; s.build(cat); s.setOpen(true);
        for (int i = 0; i < 60 * 600; ++i) s.tick(1.0f / 60.0f);   // 10 minutes
        std::snprintf(b, sizeof(b), "G5 spin stays wrapped after 10 minutes (%.3f rad)",
                      (double)s.spinRad());
        gcheck(s.spinRad() >= 0.0f && s.spinRad() < 6.2832f, b);
    }

    // G6: THE NEGATIVE CONTROL. An empty roster must REFUSE to open rather
    // than presenting an empty frame that reads as a broken feature.
    {
        CarCatalog empty;                       // default-constructed: no cars
        GarageScreen e; e.build(empty);
        e.setOpen(true);
        gcheck(e.count() == 0 && !e.open() && e.highlighted() == nullptr,
               "G6 an empty roster refuses to open (negative control)");
    }

    // G7: the comparison bars are scaled across the FLEET, so at least one car
    // sits at each end of every axis. A bar that never reaches either end is
    // measuring against an invented maximum.
    {
        bool ok = true;
        auto span = [&](float (*get)(const CarSpec&)) {
            float lo = 0.0f, hi = 0.0f;
            g.fleetRange(get, lo, hi);
            return hi - lo;
        };
        ok &= span([](const CarSpec& s){ return s.torqueNm; })  > 1.0f;
        ok &= span([](const CarSpec& s){ return s.maxRpm; })    > 1.0f;
        ok &= span([](const CarSpec& s){ return s.gripScale; }) > 0.01f;
        ok &= span([](const CarSpec& s){ return s.massKg; })    > 1.0f;
        ok &= span([](const CarSpec& s){ return s.comHeight; }) > 0.01f;
        gcheck(ok, "G7 every comparison bar has a real fleet spread to scale against");
    }

    // G8: the reveal ease runs 0->1 and back, and is frame-rate independent
    // enough that a slow frame does not overshoot past 1.
    {
        GarageScreen r; r.build(cat);
        r.setOpen(true);
        for (int i = 0; i < 5; ++i) r.tick(0.5f);        // brutal 2 fps
        const bool clamped = (r.reveal() <= 1.0f);
        r.setOpen(false);
        for (int i = 0; i < 5; ++i) r.tick(0.5f);
        std::snprintf(b, sizeof(b),
            "G8 reveal clamps to [0,1] even at 2 fps (closed reveal %.2f)",
            (double)r.reveal());
        gcheck(clamped && r.reveal() >= 0.0f && r.reveal() <= 0.001f, b);
    }

    std::snprintf(b, sizeof(b), "--- garage screen self-test: %d passed, %d failed ---",
                  g_pass, g_fail);
    if (g_fail) x3::logError(b); else x3::logInfo(b);
    return g_fail == 0;
}

} // namespace x3::game
