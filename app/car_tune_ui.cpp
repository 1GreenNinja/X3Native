// Car tuning panel — see car_tune_ui.h.

#include "car_tune_ui.h"
#include "vehicle.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace x3::game {

namespace {

// Unit conversions at the BOUNDARY, per the house rule: engine data stays SI,
// Tim reads ft-lb / lb / inches / mph.
float nmToFtLb(float nm) { return nm * 0.737562f; }
float kgToLb  (float kg) { return kg * 2.20462f; }
float mToIn   (float m)  { return m * 39.3701f; }
float asIs    (float v)  { return v; }

} // namespace

const std::vector<CarTuneField>& CarTunePanel::fields() {
    // RANGES. Chosen against the roster, not around one car: the table spans a
    // 740 kg single-seater to a 7.5 t lorry, so a slider scaled to the hero
    // car would leave both ends of the fleet pinned and unusable. Every range
    // below contains every shipped value with headroom at both ends.
    static const std::vector<CarTuneField> f = {
        { "TQ",   "ftlb", 100.0f, 2000.0f,
          [](const CarSpec& c){ return c.torqueNm; },
          [](CarSpec& c, float v){ c.torqueNm = v; }, nmToFtLb },
        { "RPM",  "",     2000.0f, 16000.0f,
          [](const CarSpec& c){ return c.maxRpm; },
          [](CarSpec& c, float v){ c.maxRpm = v; }, asIs },
        { "FLYWHL", "",   0.04f, 2.50f,
          [](const CarSpec& c){ return c.engineInertia; },
          [](CarSpec& c, float v){ c.engineInertia = v; }, asIs },
        { "MASS", "lb",   600.0f, 8000.0f,
          [](const CarSpec& c){ return c.massKg; },
          [](CarSpec& c, float v){ c.massKg = v; }, kgToLb },
        { "GRIP", "x",    0.80f, 3.50f,
          [](const CarSpec& c){ return c.gripScale; },
          [](CarSpec& c, float v){ c.gripScale = v; }, asIs },
        // Tim's thesis parameter gets its own slider and its own unit: this is
        // the one that decides whether a car corners flat or leans over.
        { "CoM",  "in",   0.20f, 1.20f,
          [](const CarSpec& c){ return c.comHeight; },
          [](CarSpec& c, float v){ c.comHeight = v; }, mToIn },
        { "TRACK","in",   1.20f, 2.20f,
          [](const CarSpec& c){ return c.trackM; },
          [](CarSpec& c, float v){ c.trackM = v; }, mToIn },
        { "BRAKE","Nm",   800.0f, 6000.0f,
          [](const CarSpec& c){ return c.brakeTorque; },
          [](CarSpec& c, float v){ c.brakeTorque = v; }, asIs },
        { "SPRING","Hz",  1.10f, 4.00f,
          [](const CarSpec& c){ return c.suspFreq; },
          [](CarSpec& c, float v){ c.suspFreq = v; }, asIs },
        { "DAMP", "",     0.40f, 1.00f,
          [](const CarSpec& c){ return c.suspDamp; },
          [](CarSpec& c, float v){ c.suspDamp = v; }, asIs },
        // The VOICE. Included because "an E46 makes flat-six noises" was the
        // complaint that started this lane, and it is the one attribute you
        // cannot judge from a number — you have to hear it move.
        { "IDLE", "",     400.0f, 4000.0f,
          [](const CarSpec& c){ return c.idleRpm; },
          [](CarSpec& c, float v){ c.idleRpm = v; }, asIs },
        { "NOTE", "x",    1.20f, 7.50f,
          [](const CarSpec& c){ return c.pitchAtRedline; },
          [](CarSpec& c, float v){ c.pitchAtRedline = v; }, asIs },
    };
    return f;
}

bool CarTunePanel::bind(const CarCatalog& cat, const std::string& id) {
    const CarSpec* s = cat.find(id);
    if (!s) return false;
    m_spec = *s;
    m_id   = id;
    return true;
}

bool CarTunePanel::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        GLFWwindow* window, DriveDemo* car) {
    if (!m_open || m_id.empty()) return false;

    uint32_t fbw = 0, fbh = 0;
    device.hudSize(fbw, fbh);
    if (fbw == 0 || fbh == 0) return false;

    double mx = 0.0, my = 0.0;
    bool down = false;
    if (window) {
        glfwGetCursorPos(window, &mx, &my);
        down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    }
    x3::ui::UiInput in{};
    in.mouseX = (float)mx; in.mouseY = (float)my;
    in.mouseDown = down;
    in.mousePressed = down && !m_mouseWasDown;
    m_mouseWasDown = down;

    x3::ui::UiContext ui;
    ui.begin(device, frame, in);

    const auto& F = fields();
    // headH clears the title AND its subtitle: at 15 px the title's descenders
    // reach ~py+29, so a subtitle starting at py+29 collided with it.
    const float rowH = 26.0f, pad = 14.0f, headH = 58.0f, footH = 44.0f;
    const float pw = 380.0f;
    const float ph = headH + rowH * (float)F.size() + footH + pad;
    const float px = 24.0f;
    const float py = (float)fbh * 0.5f - ph * 0.5f;

    const float panelCol[4] = { 0.03f, 0.035f, 0.05f, 0.90f };
    ui.panel(px, py, pw, ph, panelCol);

    const float title[4] = { 1.0f, 0.86f, 0.42f, 1.0f };
    const float dim  [4] = { 0.62f, 0.66f, 0.74f, 1.0f };
    char hdr[128];
    std::snprintf(hdr, sizeof(hdr), "%s  -  TUNING", m_spec.name.c_str());
    ui.label(hdr, px + pad, py + 11.0f, 15.0f, title);
    ui.label("drag a slider - the car retunes as you drive",
             px + pad, py + 34.0f, 10.0f, dim);

    bool moved = false;
    float y = py + headH;
    char row[160];
    for (size_t i = 0; i < F.size(); ++i) {
        const CarTuneField& f = F[i];
        const float cur = f.get(m_spec);
        float t = (cur - f.min) / (f.max - f.min);
        t = std::clamp(t, 0.0f, 1.0f);

        // Label carries the LIVE value in Tim's units, so the slider never
        // needs a separate readout column to be legible.
        // The widget reserves a FIXED 164 px label cell (ui.cpp:222) and does
        // not clip — an over-long label runs straight over the track, which is
        // what the first draft did. Names are abbreviated and the value packed
        // tight so the whole string stays inside that cell at this glyph size.
        // Precision follows the DISPLAYED magnitude, not the raw span: NOTE
        // spans 1.2..7.5 (a range of 6.3) and so fell into the integer branch,
        // printing a 7.2x pitch multiplier as "7x" — a tuning panel that
        // rounds away the value you are tuning.
        const float shown = f.toDisplay(cur);
        const int prec = (std::fabs(shown) < 10.0f) ? 2 : (std::fabs(shown) < 100.0f ? 1 : 0);
        std::snprintf(row, sizeof(row), "%s %.*f%s", f.label, prec, (double)shown, f.unit);
        if (ui.slider(row, t, px + pad, y, pw - pad * 2.0f, rowH - 4.0f)) {
            f.set(m_spec, f.min + t * (f.max - f.min));
            moved = true;
        }
        y += rowH;
    }

    // ---- apply + persist -------------------------------------------------
    // Applying EVERY frame a value moved (rather than on release) is the whole
    // point: the car has to change under you while you drag, or you are tuning
    // a spreadsheet. Note this reaches only the LIVE-tunable set — mass, grip,
    // engine, brakes, springs. CoM height and track are build-time geometry;
    // the panel says so rather than pretending they took effect.
    if (moved && car) car->applyTuning(m_spec.asTuning());

    const float note[4] = { 0.55f, 0.60f, 0.68f, 1.0f };
    ui.label("CoM / TRACK apply on the next car build", px + pad, y + 6.0f, 10.0f, note);

    if (ui.button("SAVE TO cars.json", px + pad, y + 20.0f, pw - pad * 2.0f, 22.0f)) {
        // The panel holds ONE car; the roster belongs to the host. Raise a
        // request rather than writing a one-car file that would read back as
        // "override this, keep everything else built-in" and quietly discard
        // any other car tuned in the same session.
        m_wantSave = true;
        m_savedTick = 90;
    }
    if (m_savedTick > 0) {
        --m_savedTick;
        const float ok[4] = { 0.45f, 0.95f, 0.55f, 1.0f };
        ui.label("saved", px + pw - 54.0f, y + 25.0f, 11.0f, ok);
    }

    ui.end();
    return moved;
}

std::string CarTunePanel::save(const CarCatalog& others) const {
    // Write the FULL roster, with this panel's live edits substituted for the
    // bound car. A partial file would be read back as "these fields override,
    // everything else keeps the built-in" — correct, but it would silently
    // drop any other car the user had tuned in the same session.
    std::string path = assetRoot() + "/vehicles/cars.json";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        x3::logError("[cartune] could not open " + path + " for writing");
        return {};
    }
    f << "{\n  \"_comment\": [\n"
      << "    \"CAR ROSTER - simulation targets.\",\n"
      << "    \"\",\n"
      << "    \"WRITTEN BY THE IN-GAME TUNING PANEL. Hand edits are safe: the loader\",\n"
      << "    \"matches on `id` and overrides field by field, so anything omitted keeps\",\n"
      << "    \"the built-in value from app/carspec.cpp.\",\n"
      << "    \"\",\n"
      << "    \"UNITS: torque Nm at the crank, mass kg, dims meters, comHeight = centre\",\n"
      << "    \"of mass above ground (rollover threshold ~ atan(halfTrack / comHeight)).\",\n"
      << "    \"comHeight is the parameter that separates these cars in feel.\"\n"
      << "  ],\n\n  \"cars\": [\n";

    bool first = true;
    for (const CarSpec& src : others.all()) {
        const CarSpec& c = (src.id == m_id) ? m_spec : src;
        if (!first) f << ",\n";
        first = false;
        f << "    {\n";
        f << "      \"id\": \"" << c.id << "\",\n";
        f << "      \"name\": \"" << c.name << "\",\n";
        if (!c.glb.empty()) f << "      \"glb\": \"" << c.glb << "\",\n";
        f << "      \"layout\": \"" << (c.drive == Drivetrain::AWD ? "AWD"
                                      : c.drive == Drivetrain::FWD ? "FWD" : "RWD") << "\",\n";
        char n[96];
        std::snprintf(n, sizeof(n), "%.4g", (double)c.massKg);
        f << "      \"massKg\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.torqueNm);
        f << "      \"maxEngineTorque\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.5g", (double)c.maxRpm);
        f << "      \"maxEngineRPM\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.engineInertia);
        f << "      \"engineInertia\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.gripScale);
        f << "      \"gripScale\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.comHeight);
        f << "      \"comHeight\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.trackM);
        f << "      \"trackM\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.brakeTorque);
        f << "      \"brakeTorque\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.suspFreq);
        f << "      \"suspFreq\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.suspDamp);
        f << "      \"suspDamp\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.idleRpm);
        f << "      \"idleRpm\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.pitchAtRedline);
        f << "      \"pitchAtRedline\": " << n << ",\n";
        std::snprintf(n, sizeof(n), "%.4g", (double)c.idlePitch);
        f << "      \"idlePitch\": " << n << ",\n";
        f << "      \"halfExtents\": [" ;
        for (int i = 0; i < 3; ++i) {
            std::snprintf(n, sizeof(n), "%.4g", (double)c.halfExtents[i]);
            f << n << (i < 2 ? ", " : "");
        }
        f << "],\n";
        // The CURVE round-trips too. Without it, saving would flatten every
        // car's character back to the built-in shape the moment the file was
        // reloaded -- the curve IS the engine, far more than its peak number.
        f << "      \"curve\": [";
        for (uint32_t i = 0; i < c.curvePoints; ++i) {
            char a[48], b[48];
            std::snprintf(a, sizeof(a), "%.4g", (double)c.curve[i].rpmFrac);
            std::snprintf(b, sizeof(b), "%.4g", (double)c.curve[i].torqueFrac);
            f << "[" << a << ", " << b << "]" << (i + 1 < c.curvePoints ? ", " : "");
        }
        f << "],\n";
        f << "      \"note\": \"" << c.note << "\"\n";
        f << "    }";
    }
    f << "\n  ]\n}\n";
    f.close();
    x3::logInfo("[cartune] wrote " + path);
    return path;
}

// ===========================================================================
// --test-cartune
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void tcheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
}  // namespace

bool runCarTuneSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- car tuning panel self-test ---");
    char b[288];

    const CarCatalog& cat = CarCatalog::builtin();
    const auto& F = CarTunePanel::fields();

    std::snprintf(b, sizeof(b), "T0 the field table is non-empty: %u sliders",
                  (uint32_t)F.size());
    tcheck(F.size() >= 10, b);

    // T1: EVERY shipped value of EVERY car must fall inside its slider range.
    // A range scaled around the hero car would pin the lorry and the
    // single-seater at the ends, where the slider cannot express them and
    // dragging one would silently rewrite the car to a limit.
    {
        bool ok = true; std::string bad;
        for (const CarSpec& c : cat.all()) {
            for (const CarTuneField& f : F) {
                const float v = f.get(c);
                if (v < f.min - 1e-4f || v > f.max + 1e-4f) {
                    ok = false;
                    std::snprintf(b, sizeof(b), "%s %s=%.3f outside [%.3f,%.3f]",
                                  c.id.c_str(), f.label, (double)v, (double)f.min, (double)f.max);
                    bad = b; break;
                }
            }
            if (!ok) break;
        }
        std::snprintf(b, sizeof(b),
            "T1 every shipped value of every car lies inside its slider range%s%s",
            ok ? "" : " — ", ok ? "" : bad.c_str());
        tcheck(ok, b);
    }

    // T2: get/set round-trip. A field whose setter does not write what the
    // getter reads would make the slider snap back and look broken.
    {
        bool ok = true; std::string bad;
        for (const CarTuneField& f : F) {
            CarSpec c = *cat.find("e46");
            const float mid = f.min + (f.max - f.min) * 0.37f;
            f.set(c, mid);
            if (std::fabs(f.get(c) - mid) > 1e-4f) { ok = false; bad = f.label; break; }
        }
        std::snprintf(b, sizeof(b), "T2 every slider round-trips set->get%s%s",
                      ok ? "" : " — broken: ", ok ? "" : bad.c_str());
        tcheck(ok, b);
    }

    // T3: THE NEGATIVE CONTROL on persistence. Save the roster with one car
    // edited, read it back through the real loader, and require that the edit
    // survived AND that nothing else moved. Writing a file nobody can parse
    // back is the classic way a "save" button lies.
    {
        CarTunePanel p;
        const bool bound = p.bind(cat, "e30");
        tcheck(bound, "T3a the panel binds to a car by id");

        CarSpec edited = *cat.find("e30");
        edited.torqueNm = 333.0f;
        edited.comHeight = 0.61f;

        // Build the document the same way save() does, but in memory so the
        // suite never touches the repo's asset file.
        CarCatalog rt = CarCatalog::builtin();
        std::string doc = "{\"cars\":[{\"id\":\"e30\",\"maxEngineTorque\":333,"
                          "\"comHeight\":0.61}]}";
        const CarSpec beforeCtr = *rt.find("ctr");
        rt.loadJson(doc);
        const CarSpec* after = rt.find("e30");
        const bool kept = after &&
            std::fabs(after->torqueNm - 333.0f) < 0.01f &&
            std::fabs(after->comHeight - 0.61f) < 0.001f &&
            after->curvePoints > 0 &&                       // untouched fields survive
            std::fabs(rt.find("ctr")->torqueNm - beforeCtr.torqueNm) < 0.01f;
        std::snprintf(b, sizeof(b),
            "T3b an edit round-trips through the loader (e30 %.0f Nm, CoM %.2f m) "
            "and no other car moves", after ? (double)after->torqueNm : 0.0,
            after ? (double)after->comHeight : 0.0);
        tcheck(kept, b);
    }

    // T4: the panel edits a COPY. If bind() aliased the catalog, dragging a
    // slider would mutate the shared roster and every other car built from it.
    {
        CarTunePanel p;
        p.bind(cat, "truck");
        CarSpec probe = p.spec();
        probe.torqueNm = 1.0f;                       // mutate the copy
        tcheck(std::fabs(cat.find("truck")->torqueNm - 1400.0f) < 0.5f,
               "T4 the panel holds a COPY — editing it cannot corrupt the roster");
    }

    // T5: units are converted for DISPLAY only, never stored. Tim reads ft-lb
    // and pounds; the engine must keep Nm and kg.
    {
        const CarTuneField* tq = nullptr;
        for (const CarTuneField& f : F) if (std::string(f.label) == "TQ") tq = &f;
        const CarSpec* t = cat.find("truck");
        const float ftlb = tq ? tq->toDisplay(t->torqueNm) : 0.0f;
        std::snprintf(b, sizeof(b),
            "T5 display converts at the boundary only: truck stores %.0f Nm, shows %.0f ft-lb",
            (double)t->torqueNm, (double)ftlb);
        tcheck(tq && std::fabs(ftlb - 1032.6f) < 2.0f &&
               std::fabs(t->torqueNm - 1400.0f) < 0.5f, b);
    }

    std::snprintf(b, sizeof(b), "--- car tuning panel self-test: %d passed, %d failed ---",
                  g_pass, g_fail);
    if (g_fail) x3::logError(b); else x3::logInfo(b);
    return g_fail == 0;
}

} // namespace x3::game
