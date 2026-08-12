#pragma once
// ============================================================================
// ECHO WATER V2 "LIVING BAY" (task #36) — host-side helpers riding on top of
// the engine's Gerstner ocean patch (engine/rhi/IRenderDevice.h WaterParams,
// shaders/water.vert/.frag). Tim: "Water needs to be a living moving body of
// water.. waves, splashes.. wetness on the beach."
//
// OWNERSHIP: this header + echo_water.cpp are a NEW, standalone TU. They do
// NOT include or touch host_echotropolis.cpp, CMakeLists.txt, echo_roads.*, or
// echo_region_builders.* — the integrator wires these helpers INTO those files
// (see the call-site notes on each function below) and adds echo_water.cpp to
// app/CMakeLists.txt's SOURCES list next to the other world_hosts/*.cpp files
// (~line 156-159, alongside echo_regions.cpp / echo_region_builders.cpp).
//
// COMPILE-CLEAN STANDALONE: only engine/rhi/IRenderDevice.h (WaterParams /
// ParticleInstance / ParticleBlend / submitParticles), engine/physics/
// IPhysicsWorld.h (x3::phys::Vec3), and echo_heightfield.h (the island terrain
// sampler already shared by echo_regions.h's EchoRegionCtx::hf). No Vulkan
// types, no GLFW, no host_context.h.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "echo_heightfield.h"
#include "echo_sea.h"   // kEchoSeaLevelY — the ONE datum these presets ride on

#include <cstdint>

namespace x3::game {

// ---------------------------------------------------------------------------
// WaterTuning — the swell-shape SUBSET of IRenderDevice::WaterParams (the
// fields shaders/water.vert's UBO p0/p1 actually consume: seaLevel, amplitude,
// steepness, waveLength, speed). A standalone POD (not the full engine
// WaterParams) so gameplay code can sample waves WITHOUT depending on the RHI
// device or the scene's sun/color state — those stay owned by
// host_echotropolis.cpp's applyOcean() (day/night color grading), which the
// integrator continues to call unchanged. To drive the ACTUAL device each
// frame, copy these 5 fields onto an IRenderDevice::WaterParams:
//   wp.seaLevel = tune.seaLevel;  wp.amplitude = tune.amplitude;
//   wp.steepness = tune.steepness; wp.waveLength = tune.waveLength;
//   wp.speed = tune.speed;         (wp.enabled/time/colors/sun stay as applyOcean sets them)
struct WaterTuning {
    float seaLevel   = kEchoSeaLevelY;   // world +Y (m) — DERIVED, never authored
    float amplitude  = 0.16f;   // wave height scale (m); matches applyOcean's current 0.16f
    float steepness  = 0.50f;   // 0..1 Gerstner sharpness; matches applyOcean's 0.5f
    float waveLength = 14.0f;   // base wavelength (m); matches applyOcean's 14.0f
    float speed      = 1.0f;    // phase/scroll rate multiplier
};

// ---------------------------------------------------------------------------
// echoWaveHeight — replicates shaders/water.vert's Gerstner Y sum EXACTLY (see
// water.vert lines 87-107: N=4 waves, the same dirs[]/lenMul[]/ampMul[] table,
// the same deep-water dispersion phi = sqrt(9.81*w)*speed, w = 2*pi/L). Only
// the Y (height) component is reproduced — the vertex shader's X/Z horizontal
// "pinch" (the Q/steepness term) displaces a grid vertex sideways but does NOT
// change how high the water surface sits above a given (x,z); gameplay only
// ever needs "how high is the water HERE", so this intentionally skips the
// pinch math (and thus does not need dPdx/dPdz either).
//
// `t` must be the SAME clock the host feeds to WaterParams.time (applyOcean's
// `t` argument) so the CPU-sampled wave and the GPU-drawn wave stay in sync.
float echoWaveHeight(float x, float z, float t, const WaterTuning& tune);

// ---------------------------------------------------------------------------
// ShipWaveState — heave/pitch/roll a boat should ride, sampled off the SAME
// Gerstner surface echoWaveHeight() evaluates.
struct ShipWaveState {
    float heaveY   = 0.0f;   // ADD to the boat's still-water base Y (m)
    float pitchRad = 0.0f;   // rotation about the boat's local RIGHT/beam axis; + = bow rides up
    float rollRad  = 0.0f;   // rotation about the boat's local FORWARD axis; + = starboard down
};

// echoShipPose — samples bow/stern/port/starboard + the hull center on the
// echoWaveHeight() surface and derives heave/pitch/roll from the height
// differences (a 4-point "raft on the surface" approximation — good enough for
// hull lengths well under a wavelength, which is the case for every boat in
// the fleet: kFleet's tall ship is ~43m against a 14m base wavelength, so this
// deliberately does NOT try to be exact for a hull many wavelengths long).
//
// `headingRad` / `halfLength` / `halfBeam` mirror echo_region_builders.cpp's
// buildHarborBay conventions: heading = atan2(dx, dz) (see its addBoat/M[16]
// build), half-length along the boat's forward axis, half-beam along its
// right axis. `baseX`/`baseZ` are the lane position (b.sx + b.dx*d style);
// `tune` should be the SAME WaterTuning driving the visible WaterParams that
// frame (e.g. one of the swell presets below, or applyOcean's live values).
//
// INTEGRATION (buildHarborBay, echo_region_builders.cpp ~903-914): replace
//     const float y = kBoatY + std::sin(t * 0.7f + b.off) * 0.35f;   // gentle bob
//     const float heading = std::atan2(b.dx, b.dz) + kBoatYaw;
//     const float s = b.scale, ch = std::cos(heading), sh = std::sin(heading);
//     const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
// with a call to echoShipPose(x, z, heading, halfLen, halfBeam, t, tune) to get
// {heaveY, pitchRad, rollRad}, then compose M = T(x, kBoatY+heaveY, z) *
// Ry(heading) * Rx(pitchRad) * Rz(rollRad) * S(scale) instead of the flat
// Ry(heading)*S(scale) above (a full 3x3 rotation compose — the boat's own
// half-length/half-beam can come from a per-fleet-entry constant, since
// FleetDef doesn't carry hull dims today).
void echoShipPose(float baseX, float baseZ, float headingRad,
                  float halfLength, float halfBeam,
                  float t, const WaterTuning& tune,
                  ShipWaveState& outState);

// Convenience value-returning overload (same math; for callers that prefer an
// expression over out-params).
ShipWaveState echoShipPose(float baseX, float baseZ, float headingRad,
                           float halfLength, float halfBeam,
                           float t, const WaterTuning& tune);

// ---------------------------------------------------------------------------
// EchoSplashes — bounded, CPU-simulated splash-particle emitter (bow spray +
// shore lapping), mirroring app/fx.h's CombatFx particle-pool conventions
// EXACTLY: a fixed ring (no per-frame heap alloc), update(dt) integrates
// pos/vel/gravity/drag/life, submit() streams the live set to the device as
// IRenderDevice::ParticleInstance via ONE submitParticles() call (see
// app/fx.cpp CombatFx::submit ~602-627 for the pattern this follows). Spawns
// only from motion/shoreline events, so an idle scene submits nothing (zero
// GPU cost), same doctrine as CombatFx.
//
// INTEGRATION: the host owns one EchoSplashes (or one per streamed region).
// Each frame, AFTER computing each boat's ShipWaveState (so spray originates
// at the ACTUAL rocking bow, not the flat base pose):
//   splashes.spawnBowSpray(bowWorldPos, boatVelocity);
// and, for the visible shoreline arc (a coarse set of world-space points along
// the beach — e.g. sampled every ~8m along the camera-facing shore, using the
// SAME Heightfield the host already loads for the island):
//   splashes.spawnShoreLapping(hf, shoreSegA, shoreSegB, t, tune);
// then once per frame:
//   splashes.update(dt);
//   splashes.submit(device);   // between beginFrame/endFrame, like CombatFx::submit
class EchoSplashes {
public:
    static constexpr int kMaxSplashParticles = 1024;  // bounded pool; oldest slot recycled
    static constexpr int kShoreProbesPerCall = 12;    // sample points along a->b per spawnShoreLapping() call

    // Bow-spray burst at a boat's bow, biased along `vel`. Speed-gated: a
    // near-stationary boat (docked, or between lane loops) spawns nothing —
    // idle harbor traffic costs 0 particles/frame, matching CombatFx's
    // "spawned ONLY from combat events" doctrine for FX.
    void spawnBowSpray(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel);

    // Shore-lapping foam: probes the heightfield `hf` at several points along
    // world-space segment a->b (e.g. a chunk of the visible shoreline) and, at
    // any probe whose TERRAIN height sits within the current animated tide
    // band (echoWaveHeight(t) +/- a small margin), spawns a soft lapping puff.
    // `t`/`tune` should match the frame's live WaterTuning so the foam line
    // tracks the actual moving waterline instead of a static y=0 ring.
    void spawnShoreLapping(const Heightfield& hf, const x3::phys::Vec3& a,
                           const x3::phys::Vec3& b, float t, const WaterTuning& tune);

    // Integrate the live pool (gravity/drag/life). Call once per frame.
    void update(float dt);

    // Stream the live particles to the device (ALPHA blend — spray/foam is
    // translucent white/pale-blue, not additive/glowing like combat sparks).
    // Call between beginFrame/endFrame. No-op when the pool is empty.
    void submit(x3::rhi::IRenderDevice& device) const;

    int liveCount() const;

private:
    struct SplashParticle {
        x3::phys::Vec3 pos{};
        x3::phys::Vec3 vel{};
        float life    = 0.0f;   // remaining seconds; <=0 == free slot
        float maxLife = 1.0f;
        float size0   = 0.05f;  // half-extent at birth (m)
        float size1   = 0.12f;  // half-extent at death (m) — spray puffs EXPAND as they fall/fade
        float r = 0.85f, g = 0.90f, b = 0.95f;   // pale foam/spray tint (linear)
        float a0      = 0.55f;  // opacity at birth (translucent, not opaque white)
        float gravity = 1.0f;   // * world gravity (m/s^2 along -Y)
        float drag    = 1.2f;   // per-second velocity damping
    };
    SplashParticle m_particles[kMaxSplashParticles];
    int m_nextParticle = 0;

    uint32_t m_rng = 0xA53F1234u;   // deterministic xorshift32 (headless-capture repeatable)
    float frand();                  // [0,1)
    float frandSym();               // [-1,1)
    int   spawnParticle(const SplashParticle& p);

    // Per-frame submit scratch (member-owned; no per-frame heap/stack alloc of
    // the instance array, matching CombatFx's m_scratchAdd/m_scratchAlpha).
    mutable x3::rhi::IRenderDevice::ParticleInstance m_scratch[kMaxSplashParticles];
};

// ============================================================================
// SWELL TUNING TABLE — 3 presets Tim can pick by feel. Every one rides
// kEchoSeaLevelY; NONE of them states a sea height of its own any more.
//
// THE MARGIN MATH now lives in echo_sea.h (echoWorstTroughY / echoMaxAmplitude)
// so it is one formula instead of three hand-computed comments that could
// disagree with the code they describe — which is precisely how the seaLevel
// 0.10 in this table drifted from the 0.0 the rest of the world used.
//
//     worstTroughY  = kEchoSeaLevelY - amplitude * 1.92
//     amplitude_max = (kEchoSeaLevelY - kEchoOceanRingY - margin) / 1.92
//                   = (0.0 - (-0.4) - 0.05) / 1.92  =  0.1823 m
//
// WHAT MOVING THE DATUM TO 0 COST, stated plainly: the sea dropped 0.10 m, so
// every preset lost 0.10 m of headroom over the ring. CALM and HARBOR still
// clear it comfortably and are UNCHANGED. STORM did not — 0.22 amplitude now
// troughs at -0.4224, i.e. THROUGH the ring — so its amplitude is cut to 0.175.
// That is a real 20% loss of storm wave height and it is the price of the
// unification; it is not hidden in a rounding. If Tim wants the big storm back
// the lever is the RING, not the sea: re-bake with OCEAN_Y = -1.0 (invisible,
// since the ring is only ever seen from above 140 m or at night) and
// echoMaxAmplitude() rises to 0.49 on its own. The gate below will fail the
// instant anyone raises an amplitude without doing that.
struct SwellPreset {
    const char* name;
    WaterTuning tune;
    float worstTroughY;   // == echoWorstTroughY(tune.amplitude); gate re-derives it
    float ringMarginM;    // worstTroughY - kEchoOceanRingY
};

// CALM — glassy harbor water at rest (dawn/dead-calm bay).
// Worst trough: 0.0 - 0.08*1.92 = -0.1536  =>  margin 0.2464 m (wide open).
inline constexpr WaterTuning kSwellCalm{
    /*seaLevel*/ kEchoSeaLevelY, /*amplitude*/ 0.08f, /*steepness*/ 0.35f,
    /*waveLength*/ 18.0f, /*speed*/ 0.60f,
};

// HARBOR — the shipped "living water" default; the one applyOcean drives.
// Worst trough: 0.0 - 0.16*1.92 = -0.3072  =>  margin 0.0928 m.
inline constexpr WaterTuning kSwellHarbor{
    /*seaLevel*/ kEchoSeaLevelY, /*amplitude*/ 0.16f, /*steepness*/ 0.50f,
    /*waveLength*/ 14.0f, /*speed*/ 1.00f,
};

// STORM — the roughest swell that still clears the ring. Amplitude cut 0.22 ->
// 0.175 by the datum move (see above). Worst trough: 0.0 - 0.175*1.92 =
// -0.336  =>  margin 0.064 m. The choppy read is carried mostly by the short
// 11 m wavelength, the 0.62 steepness (peaked crests) and the 1.35x scroll,
// which are untouched — but it IS a shorter sea than it was.
inline constexpr WaterTuning kSwellStorm{
    /*seaLevel*/ kEchoSeaLevelY, /*amplitude*/ 0.175f, /*steepness*/ 0.62f,
    /*waveLength*/ 11.0f, /*speed*/ 1.35f,
};

// Table form for UI/debug pickers ("pick by feel"). Defined in echo_water.cpp
// (constinit-friendly aggregate; worstTroughY/ringMarginM are the documented,
// precomputed values from the comments above — NOT recomputed at runtime).
extern const SwellPreset kSwellPresets[3];

} // namespace x3::game
