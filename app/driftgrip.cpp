// driftgrip — DRIFT FEEL + SURFACE-DEPENDENT TRACTION. See driftgrip.h.
//                                                  [LANE: inspx/veh-cosmetics]
//
// CLEAN-ROOM, original work: built only on engine/physics/IVehicle.h + public
// vehicle-dynamics references (slip angle/ratio, load transfer — "Car Physics
// for Games"-level material). No other game or engine source consulted.

#include "driftgrip.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float kRad2Deg = 57.295779513f;
} // namespace

const char* driveSurfaceName(DriveSurface s) {
    switch (s) {
        case DriveSurface::Road:     return "road";
        case DriveSurface::Shoulder: return "shoulder";
        case DriveSurface::Dirt:     return "dirt";
        case DriveSurface::Grass:    return "grass";
        default:                     return "?";
    }
}

// Calibration discipline follows app/wetness.h: multipliers are RATIOS of
// measured road-surface mu, asphalt = 1. Dry asphalt mu ~0.9; packed
// dirt/gravel ~0.6-0.7 -> 0.67-0.78 of asphalt; grass ~0.5 -> ~0.55.
// longFlatten is the loose-surface term: on a shearing granular bed the
// friction force past peak slip does NOT fall off the way a polished tire on
// asphalt does — the surface keeps yielding and throwing material (the "dirt
// spits out" behaviour), so the high-slip plateau is raised toward the peak.
SurfaceGrip surfaceGripFor(DriveSurface s) {
    switch (s) {
        case DriveSurface::Shoulder: return { 0.97f, 0.95f, 0.05f, 0.12f };
        case DriveSurface::Dirt:     return { 0.78f, 0.62f, 0.65f, 1.00f };
        case DriveSurface::Grass:    return { 0.66f, 0.55f, 0.55f, 0.75f };
        case DriveSurface::Road:
        default:                     return { 1.0f, 1.0f, 0.0f, 0.0f };
    }
}

// ---------------------------------------------------------------------------
// Parts-catalog -> drift feel. The compound decides how the car breaks away
// and how tight the drift holds; suspension stiffness sharpens the response.
// ---------------------------------------------------------------------------
DriftParams driftParamsFor(const vehparts::Catalog& cat,
                           const vehparts::VehicleBuild& build) {
    DriftParams p;   // defaults = the "summer" street compound
    const std::string* tireId = build.installedIn("tires");
    const vehparts::Part* tire = tireId ? cat.find(*tireId) : nullptr;
    if (tire) {
        // Compound codes as authored in parts.json: AS (all-season touring),
        // PS (performance summer), RC (semi-slick), SL (slick).
        if (tire->compound == "AS") {
            p.entrySlipDeg = 7.0f;  p.exitSlipDeg = 4.0f;
            p.rearLatRetain = 0.60f; p.counterGain = 2.0f; p.blendRate = 5.0f;
        } else if (tire->compound == "RC") {
            p.entrySlipDeg = 11.0f; p.exitSlipDeg = 5.0f;
            p.rearLatRetain = 0.70f; p.counterGain = 2.4f; p.blendRate = 7.0f;
        } else if (tire->compound == "SL") {
            p.entrySlipDeg = 13.5f; p.exitSlipDeg = 5.5f;
            p.rearLatRetain = 0.74f; p.counterGain = 2.6f; p.blendRate = 8.0f;
        }
        // "PS" (and unknown compounds) keep the defaults.
    }
    const std::string* suspId = build.installedIn("suspension");
    const vehparts::Part* susp = suspId ? cat.find(*suspId) : nullptr;
    if (susp && susp->suspFreq >= 2.8f) {   // stiff coilovers: sharper catch
        p.counterGain += 0.2f;
        p.blendRate   += 1.0f;
    }
    return p;
}

// ---------------------------------------------------------------------------
// DriftGrip::update — one fixed step.
// ---------------------------------------------------------------------------
void DriftGrip::reset() {
    m_drifting = false; m_slipDeg = 0.0f; m_assist = 0.0f;
    m_rearLat = m_frontLat = 1.0f;
    for (uint32_t i = 0; i < kMaxWheels; ++i) { m_spray[i] = 0.0f; m_surface[i] = DriveSurface::Road; }
}

void DriftGrip::update(x3::phys::IVehicleController& ctl, float dt,
                       x3::phys::VehicleInput& eff) {
    const uint32_t n = std::min(ctl.wheelCount(), kMaxWheels);
    const bool surfaceActive = m_surfaceOn && (bool)m_query && n > 0;
    const bool active = m_driftOn || surfaceActive || m_external != 1.0f;

    if (!active) {
        // Fully off: never touch the controller — today's handling, bit-exact.
        // If we WERE active last tick, push identity once so a live cvar flip
        // doesn't leave stale modulation on the wheels.
        if (m_wasActive) {
            for (uint32_t i = 0; i < n; ++i) ctl.setWheelGripMod(i, {});
            reset();
            m_wasActive = false;
        }
        return;
    }
    m_wasActive = true;

    const float vF = ctl.forwardSpeed();
    const float vL = ctl.lateralSpeed();
    const float slipRad = std::atan2(vL, std::max(std::fabs(vF), 0.75f));
    m_slipDeg = slipRad * kRad2Deg;
    const float aslip = std::fabs(m_slipDeg);
    const float speed = std::fabs(vF);

    // ---- Per-wheel surface + spray -----------------------------------------
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        const bool haveState = ctl.wheelState(i, ws);
        if (surfaceActive && haveState && ws.hasContact)
            m_surface[i] = m_query(ws.worldTransform[12], ws.worldTransform[14]);
        if (surfaceActive && haveState && ws.hasContact) {
            const SurfaceGrip sg = surfaceGripFor(m_surface[i]);
            const float slipMag  = std::fabs(ctl.longitudinalSlip(i));
            const float latFac   = aslip / 35.0f;
            const float speedFac = clampf(speed / 22.0f, 0.15f, 1.0f);
            m_spray[i] = sg.spray * clampf(std::max(slipMag * 1.6f, latFac), 0.0f, 1.0f) * speedFac;
        } else {
            m_spray[i] = 0.0f;
        }
    }

    // ---- Drift state machine ------------------------------------------------
    float targetRear = 1.0f, targetFront = 1.0f;
    m_assist = 0.0f;
    if (m_driftOn) {
        const bool hbEntry = m_p.handbrakeEntry && eff.handBrake > 0.5f &&
                             std::fabs(eff.steer) > 0.25f && speed > m_p.minSpeed * 1.3f;
        if (!m_drifting) {
            if (speed > m_p.minSpeed && (aslip > m_p.entrySlipDeg || hbEntry))
                m_drifting = true;
        } else {
            if (speed < m_p.minSpeed * 0.7f ||
                (aslip < m_p.exitSlipDeg && eff.handBrake < 0.5f))
                m_drifting = false;
        }
        if (m_drifting) {
            // Weight transfer: throttle unloads the rear further; the
            // handbrake gives the classic entry kick.
            targetRear = m_p.rearLatRetain - m_p.throttleRearCut * clampf(eff.throttle, 0.0f, 1.0f);
            if (eff.handBrake > 0.5f) targetRear *= 0.8f;
            targetRear = std::max(targetRear, 0.30f);
            // ANTI-SPIN: past the stabilization angle the rear grip ramps back
            // toward full — the slide has a ceiling instead of swapping ends.
            if (aslip > m_p.stabSlipDeg) {
                const float t = clampf((aslip - m_p.stabSlipDeg) / std::max(1.0f, m_p.stabRangeDeg),
                                       0.0f, 1.0f);
                targetRear += (1.0f - targetRear) * t;
            }
            targetFront = m_p.frontLatRetain;
            // THE ASSIST WINDOW. Two halves:
            //  * BLUNT the aggravating input — steering further into the slide
            //    while already sideways is what turns a drift into a spin, so
            //    that component is scaled down. Countersteer passes through.
            //  * COUNTERSTEER ASSIST — a bounded steer term proportional to
            //    the slip angle, into the slide (the sign of the slip angle IS
            //    the direction the nose must steer to track velocity again).
            const float aggravDir = (slipRad < 0.0f) ? 1.0f : -1.0f;
            if (eff.steer * aggravDir > 0.0f)
                eff.steer *= (1.0f - clampf(m_p.steerBlunt, 0.0f, 1.0f));
            m_assist = clampf(m_p.counterGain * slipRad, -m_p.counterMax, m_p.counterMax);
            eff.steer = clampf(eff.steer + m_assist, -1.0f, 1.0f);
        }
    }

    // Ease the retained grips (dt-scaled, never per-frame), snapping to 1 when
    // converged so the engine can restore the EXACT baseline curves.
    const float k = std::min(1.0f, m_p.blendRate * dt);
    m_rearLat  += (targetRear  - m_rearLat)  * k;
    m_frontLat += (targetFront - m_frontLat) * k;
    if (std::fabs(1.0f - m_rearLat)  < 1e-3f && targetRear  == 1.0f) m_rearLat  = 1.0f;
    if (std::fabs(1.0f - m_frontLat) < 1e-3f && targetFront == 1.0f) m_frontLat = 1.0f;

    // ---- Compose + push the per-wheel modulation ---------------------------
    for (uint32_t i = 0; i < n; ++i) {
        const SurfaceGrip sg = surfaceActive ? surfaceGripFor(m_surface[i]) : SurfaceGrip{};
        const bool rear = (i == m_rearA || i == m_rearB);
        x3::phys::WheelGripMod m;
        m.longScale   = sg.longScale * m_external;
        m.latScale    = sg.latScale * m_external * (rear ? m_rearLat : m_frontLat);
        m.longFlatten = sg.longFlatten;
        ctl.setWheelGripMod(i, m);
    }
}

} // namespace x3::game
