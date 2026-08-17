#pragma once
// ===========================================================================
// CAR TUNING PANEL — sliders bound to the SELECTED car's own variables.
//
// Tim, 2026-08-16: "We need sliders in car settings for each car to change
// its attributes!"
//
// The console already had eight `car_*` float commands (torque, redline,
// grip, mass, brake, ride, springfreq, springdamp). Two things were wrong
// with them as an answer to this: they are GLOBAL — they retune whatever is
// driving and forget it the moment you switch — and you have to know the
// name and the units before you can touch anything. A slider shows you the
// range, the current value, and the effect, at once.
//
// This panel edits the CarSpec of the car being driven (app/carspec.h), so
// every car keeps its OWN numbers: move the truck's torque and you have
// moved the truck's torque, not the game's. Changes apply LIVE through
// DriveDemo::applyTuning — drive while dragging — and `save` writes the
// whole roster back out to assets/vehicles/cars.json in the same format the
// loader reads, so a session spent tuning by feel is not lost at exit.
//
// Built only on x3::ui::UiContext (the existing IMGUI-lite: panel/label/
// slider/button over drawHudQuad+drawHudText) — no new UI framework, and no
// second slider implementation.
// ===========================================================================

#include "carspec.h"
#include "ui.h"

#include <string>
#include <vector>

struct GLFWwindow;

namespace x3::rhi { class IRenderDevice; struct FrameContext; }

namespace x3::game {

class DriveDemo;

// One editable attribute: how to read it, how to write it, and the range a
// slider spans. Ranges are chosen so the SHIPPED value sits somewhere sane in
// the middle rather than pinned at an end, and so the extremes are still a
// drivable car rather than a physics accident.
struct CarTuneField {
    const char* label;
    const char* unit;
    float min, max;
    float (*get)(const CarSpec&);
    void  (*set)(CarSpec&, float);
    // Display conversion: the engine stores SI, Tim reads ft-lb / lb / inches.
    float (*toDisplay)(float);
};

class CarTunePanel {
public:
    // Bind to the catalog entry with this id. Returns false if unknown.
    bool bind(const CarCatalog& cat, const std::string& id);
    const std::string& boundId() const { return m_id; }

    bool open() const { return m_open; }
    void setOpen(bool o) { m_open = o; }
    void toggle() { m_open = !m_open; }

    // Draw + interact. Call between beginFrame/endFrame while the panel is
    // open. Applies any change to `car` immediately (nullptr = edit only).
    // Returns true if a value moved this frame.
    bool draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              GLFWwindow* window, DriveDemo* car);

    // The live edited spec (what the sliders are moving).
    const CarSpec& spec() const { return m_spec; }

    // True ONCE after the SAVE button is pressed. The panel cannot write the
    // file itself — it holds one car, and the roster is the host's — so it
    // raises a request and the host performs the save with the full catalog.
    bool takeSaveRequest() { const bool w = m_wantSave; m_wantSave = false; return w; }

    // Write the whole roster to assets/vehicles/cars.json. `others` supplies
    // every car except the bound one, which is taken from this panel's live
    // edits. Returns the path written, or "" on failure.
    std::string save(const CarCatalog& others) const;

    // The field table (also used by the headless suite).
    static const std::vector<CarTuneField>& fields();

private:
    CarSpec     m_spec;
    std::string m_id;
    bool        m_open = false;
    bool        m_mouseWasDown = false;
    bool        m_wantSave = false;   // SAVE pressed, host has not serviced it yet
    int         m_savedTick = 0;      // frames left showing the "saved" flash
};

// --test-cartune — the panel's headless suite (ranges, round-trip, negative
// control on the save format).
bool runCarTuneSelfTest();

} // namespace x3::game
