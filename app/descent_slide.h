#pragma once
// descent_slide — THE DESCENT RIDE (feast Wave 2C) + the generic TRACK layer
// underneath it (owner directive: the spline/rider tech seeds the Cedar Point
// 2000 coaster system, so the track definition + rider controller are DATA-
// DRIVEN and ride-agnostic; DescentSlide is the first authored track).
//
// Canon (docs/design/TEXTURE_DESIGN_STRATEGY_TOWER_CAVES_DESCENT.md §3, Tim's
// 2026-07-11 rulings): the ride starts in the facility BASEMENT (B1, y=0),
// winds down a curving chute system, and ejects into the lower caves at the
// −178 m crystal horizon — which still sits ABOVE Club 1127 (world bottom is
// −700 m). Ride grammar is coaster-grade per the owner's Cedar-Point bar:
// lift-crest tension → committed first drop → overbanked turn → airtime hills
// (real unweight) → tunnel-burst rhythm (tight bore vs open void) → headchopper
// beams → the SL1/SL2/SL3 glimpse-window triple-flash → cavern-burst finale →
// brake run. Camera note: setCamera() is yaw/pitch only (no roll, engine-wide),
// so overbank is modeled in geometry/physics and the camera stays level —
// renderer view-up roll is a flagged follow-up for the coaster system.
//
// Self-contained: prim geometry + Scene entities + static collision; no edits
// to Player (the host teleports via Player::setFeetPosition on ride exit).

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// GENERIC TRACK LAYER (reusable: chutes, coasters — same math, different car)
// ---------------------------------------------------------------------------

// Per-segment ride behavior. Applied to sample RANGES of the track.
enum class TrackSegType : uint8_t {
    Crest,    // lift-hill crest: rider speed clamped to a slow crawl (tension)
    Drop,     // committed steep descent (no special handling; gravity does it)
    Curve,    // banked turn (bank carried per-sample)
    Airtime,  // floor falls away faster than gravity: rider unweights (floats)
    Bore,     // tight enclosed tube (roof + headchopper beams)
    Burst,    // open void: channel on trestles, no walls/roof (speed-by-contrast)
    Brake,    // strong decel run (the bowl)
};

// One sampled track frame (~2.5 m spacing). right/up are the BANKED lateral
// frame (bank rotates them around tangent); cumLen is arc length from sample 0.
struct TrackFrame {
    x3::phys::Vec3 pos{};
    x3::phys::Vec3 tan{};      // unit tangent (direction of travel)
    x3::phys::Vec3 right{};    // unit lateral, banked
    x3::phys::Vec3 up{};       // unit channel-up, banked
    float          bankDeg = 0.0f;
    float          cumLen  = 0.0f;
    TrackSegType   type    = TrackSegType::Curve;
};

// A complete sampled track + its authored feature points. Pure data (no GPU /
// physics), so the headless self-test builds and simulates it for free.
struct TrackSpec {
    std::vector<TrackFrame> frames;
    float totalLen  = 0.0f;
    float totalDrop = 0.0f;                 // start y − end y (positive down)
    // Feature bookkeeping for gates + the geometry builder:
    uint32_t airtimeZones = 0, overbankSamples = 0, chopperCount = 0;
    struct Window { x3::phys::Vec3 pos; float hue[3]; float depthY; };
    std::vector<Window> windows;            // glimpse-windows (5: geode/mine/SL1/SL2/SL3)
};

// Rider state: capsule-on-track with tangent velocity, lateral steer, and a
// soft unweight spring for Airtime zones. Same controller drives the slide,
// a coaster train (fixed seat = steer 0), and the headless gate sim.
struct TrackRider {
    float arcLen  = 0.0f;    // distance along the track (m)
    float speed   = 2.2f;    // m/s (starts at crawl)
    float lateral = 0.0f;    // steer offset across the channel (m, ±kSteerMax)
    float floatUp = 0.0f;    // unweight offset above the channel floor (m)
    bool  done    = false;

    // Advance one step. steer ∈ [-1,1]. Fills camera pos + yaw/pitch + fov.
    // Pure function of the spec — no GPU, no physics world (gate-simulable).
    void tick(const TrackSpec& spec, float dt, float steer,
              float camOut[3], float& yawOut, float& pitchOut, float& fovOut);
};

// ---------------------------------------------------------------------------
// THE DESCENT SLIDE (the authored B1 → −178 m ride)
// ---------------------------------------------------------------------------

class DescentSlide {
public:
    // Build the canon track spec ONLY (pure data — the self-test's entry point).
    static TrackSpec buildTrackSpec();

    // Build geometry (channel plates/walls/roofs/beams/trestles/ribbons, the
    // glimpse-window alcoves, the crystal cavern + brake bowl) as Scene entities
    // + static collision. Surface-set names are one-line swappable constants in
    // the .cpp (sl_chute_steel / cv_rock_flume land from the 2A forge later).
    bool build(x3::rhi::IRenderDevice& device, Scene& scene,
               x3::phys::IPhysicsWorld& physics);

    const TrackSpec& spec() const { return m_spec; }
    // Ride anchors for the host: the mouth platform (walk start / entry trigger)
    // and the bowl (exit teleport target).
    x3::phys::Vec3 mouth() const;
    x3::phys::Vec3 bowl()  const;
    // Point lights the host uploads (windows + crystals + spaced shoulders; ≤64
    // with room to spare).
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

    uint32_t entityCount() const { return m_entities; }
    void shutdown(x3::phys::IPhysicsWorld& physics);

private:
    TrackSpec m_spec;
    std::vector<x3::rhi::MeshHandle>    m_meshes;
    std::vector<x3::rhi::TextureHandle> m_textures;
    std::vector<x3::phys::BodyId>       m_bodies;
    std::vector<x3::rhi::PointLight>    m_lights;
    x3::rhi::IRenderDevice*             m_device = nullptr;
    uint32_t                            m_entities = 0;
};

// Headless self-test (--test-descentslide): spec sanity (drop ≥170 m, monotonic
// descent to the bowl, winding heading changes, first-drop steepness ≤ −70°,
// ≥1 overbank >90°, ≥2 airtime zones, ≥2 headchoppers, 5 windows with the SL
// trio in the −170/−174/−177 bands) + a full bounded rider sim that must reach
// the brake run in 15–90 s with real unweight observed. ≥8 checks.
bool runDescentSlideSelfTest();

} // namespace x3::game
