#pragma once
// THE CAR GAUGE CLUSTER — one implementation, two callers.
//
// WHY THIS FILE EXISTS (NO_SLOP rule 1, and rule 2's precondition). The whole
// cluster — tach, shift gate, boost dial, NOS arc, fuel bar, MPH, gear, shift
// lights, key hints, traction line — lived inline in host_tunnel's INTERACTIVE
// loop, and only there. That has two consequences and both bit:
//
//   * NO CAPTURE COULD EVER SHOW IT. `--screenshot-*` runs are headless and the
//     host early-returns long before the interactive loop, so every proof shot
//     in this project's history has a bare windscreen. The dial art could be
//     changed and nobody could look at the result in the game without playing
//     it. The minimap had exactly this problem and answered it by pasting a
//     second copy of its draw into the proof block — which then read a
//     moved-from vector for a whole release and drew NO roads (the receipt is
//     in host_tunnel's minimap comment). Copying is not the answer; this is.
//   * THE "HIDE ON FOOT" RULE HAD NO WITNESS. The owner reported the car gauges
//     staying up in Walk mode. The gating is one predicate now, at one call
//     site, and the proof set photographs it BOTH ways from the same code.
//
// So: the cluster draws here, the host calls it with live values, and the
// headless proof calls it with staged ones. If it ever drifts, it drifts for
// both at once — which is the only kind of drift a screenshot can catch.

#include "engine/rhi/IRenderDevice.h"
#include "gas_station.h"   // FuelTank + drawFuelBar (the fuel gauge is part of the cluster)

namespace x3::game {

// THE GAUGE-CLUSTER ANCHOR, in ONE place. dial (2R tall) + gap + gate (0.9R)
// = 3.0R of vertical room; anchoring on the dial alone pushed the gate and the
// traction line off the bottom of the screen. The interactive HUD, the fuel
// bar, and the headless proof capture all read the layout from here — two
// copies of this arithmetic is the drift that makes a proof shot stop proving
// anything (NO_SLOP rule 4).
inline void gaugeClusterAnchor(float fw, float fh, float& R, float& gcx, float& gcy) {
    R = 0.150f * fh;
    const float mar = 0.030f * fh;
    const float gateH = R * 0.90f;
    gcx = fw - mar - R;
    gcy = fh - mar - gateH - R * 0.12f - R;
}

// The five baked textures the cluster draws with (assets/ui/gauge_*.png, baked
// by tools/render_gauge_bezel.py -> tools/compose_gauge_dial.py +
// tools/make_gauge_textures.py). Any may be invalid; each is checked.
struct GaugeClusterTex {
    x3::rhi::TextureHandle dial{};    // gauge_dial.png    tach face
    x3::rhi::TextureHandle needle{};  // gauge_needle.png  8x8 atlas, 64 rotations
    x3::rhi::TextureHandle gate{};    // gauge_gate.png    shift gate
    x3::rhi::TextureHandle boost{};   // gauge_boost.png   boost face
    x3::rhi::TextureHandle nos{};     // gauge_nos.png     32-state 8x4 fill atlas
};

// One frame of car state, in the units the DIALS are drawn in. The caller reads
// these off the vehicle; nothing in here queries the car, so the proof capture
// can stage a value the way `fuel 24` stages the tank.
struct GaugeClusterState {
    float  rpm       = 0.0f;   // engine rpm
    float  mph       = 0.0f;   // road speed, mph (already abs)
    int    gear      = 0;      // <0 reverse, 0 neutral, 1.. forward
    float  boostPsi  = 0.0f;   // manifold pressure, psi (NEGATIVE in vacuum)
    float  nosFrac   = 0.0f;   // 0..1 tank level
    bool   nosActive = false;  // spraying (drives the flare tint)
    bool   tcOn      = true;   // traction control
    float  dt        = 1.0f / 60.0f;  // for the needle smoothing
    double now       = 0.0;    // seconds, for the limiter flash
};

// PAIRED VALUES (NO_SLOP rule 4). Each of these has a twin somewhere else and
// a change to one IS a change to both; the twin is named here and here is named
// at the twin.
//   * kGaugeMaxPsi / kGaugeMinPsi <-> tools/compose_gauge_dial.py's
//     BOOST_MAX_PSI / BOOST_MIN_PSI (the DRAWN scale) <-> TurboParams::maxPsi
//     in app/vehicle.h (the MODEL). The original sin was 35 psi of model under
//     a 20 psi dial: the needle pinned off the end while the digits counted on.
//   * kGaugeHotPsi <-> the red band start in the same script (BOOST_HOT_PSI).
//   * kGaugeRpmMax <-> RPM_MAX in the same script.
constexpr float kGaugeRpmMax = 8000.0f;
constexpr float kGaugeMinPsi = -10.0f;
constexpr float kGaugeMaxPsi =  40.0f;
constexpr float kGaugeHotPsi =  30.0f;

// Draw the whole cluster at the anchor for this framebuffer size. The CALLER
// owns the "should this be on screen at all" decision — on foot it is simply
// not called (owner: "in Walk mode.. the car gauges disappear").
void drawGaugeCluster(x3::rhi::IRenderDevice& device,
                      const x3::rhi::FrameContext& frame,
                      float fw, float fh,
                      const GaugeClusterTex& tex,
                      const GaugeClusterState& st,
                      const FuelTank& fuel, bool refuelling);

} // namespace x3::game
