// world_water.h — ONE water implementation for every host that stands on the
// shared Keth'zar height field (tunnel dev world AND the canon level).
//
// WHY THIS FILE EXISTS. The tunnel host grew the engine water pass into the
// owner-approved look (clarity, foam, the stepped river polyline, the shoreline
// table, the cavern branch under the mountain) as a 180-line lambda inside
// host_tunnel.cpp. The canon level — the world the owner actually plays — had
// none of it: its river was a translucent GLASS ribbon and its sea an opaque
// blue slab (world_regions.cpp / ocean_base.cpp), so "clarity" could not exist
// there. Porting the lambda by copy would have made a second water to keep in
// step (the drive layer already paid for that lesson: shared code lives in its
// own file and both hosts call it). This is that file. The tunnel lambda now
// calls buildWorldWaterParams() and keeps only what is host-specific (its
// caustics plane and its rain-risen clock); canon calls the same function.
//
// WHAT IT DECIDES, per frame, from the focus point (camera / player / car XZ):
//   * inside the underground river's corridor -> the CAVERN channel
//     (worldUnderRiverChain polyline, enclosed, no sun, clarity 0.94);
//   * else, if the caller says the surface river exists -> the SURFACE channel
//     (worldRiverRisenNodes polyline + ocean basin + shoreline table,
//     clarity 0.82, live luminary);
//   * else nothing — returns false and leaves the device's water alone.
// The two channels are 1.5 km apart and the drawn patch is 480 m, so the
// switch can never pop: one polyline is always enough.
//
// seaLevel (the shader's underside-view gate) carries the LOCAL water level at
// the focus (worldWaterLevelAt); a dry focus inside the cavern falls back to
// the chain's own interpolated level, a dry focus on the surface to
// dryFallbackY when the host supplies one (the tunnel's bridge plan) or to the
// nearest surface-river node otherwise (canon, which has no bridge plan).
//
// X3_WATER_CLARITY=<v> (env, read once) overrides BOTH channels' clarity —
// 0 is the legacy opaque plane. It is a DISABLE door for the A/B receipt
// (docs/screenshots/canon_underriver), not a tuning knob: the approved values
// live in the .cpp and are asserted by --test-canonunderriver.
#pragma once

#include "engine/rhi/IRenderDevice.h"

namespace x3::game {

class UndergroundRiver;

struct WorldWaterInput {
    float time = 0.0f;                    // wave clock (host-owned, dt-scaled)
    float focusX = 0.0f, focusZ = 0.0f;   // this frame's focus XZ
    float sunDir[3] = { 0.4f, 1.0f, 0.3f }; // live luminary (surface channel only)
    bool  surfaceOn = true;               // false = host has no surface river (tunnel's X3_RIVER_ROAD=0)
    const float* dryFallbackY = nullptr;  // optional: seaLevel for a dry surface focus
    // THE CAVERN (cavern channel only). Enclosed water has no sun and no sky
    // to be lit by, so the recipe hands the water pass the run's bank lights
    // nearest the focus and water.frag lights the body, the foam and the
    // surface from them (WaterParams::roomLight*). The pick is made HERE, at
    // the level the recipe itself resolves for the focus (the host's focus is
    // usually a dry beach, where worldWaterLevelAt is the dry sentinel and a
    // host-side 3D pick finds nothing), from the same array
    // UndergroundRiver::nearestLights feeds the main pass — so the rock and
    // the water it laps see the same lamps. Null = ambient-only cavern water
    // (PI * horizonColor).
    const UndergroundRiver* cavern = nullptr;
};

// The approved values, named so the test and the report can quote them.
constexpr float kWorldWaterClaritySurface = 0.82f;
constexpr float kWorldWaterClarityCavern  = 0.94f;

// Pure: fills `out` and returns true when there is water to draw for this
// focus; returns false (and leaves `out` untouched) when there is none.
// `outInCavern` (optional) reports which channel was chosen.
bool buildWorldWaterParams(const WorldWaterInput& in,
                           x3::rhi::IRenderDevice::WaterParams& out,
                           bool* outInCavern = nullptr);

// buildWorldWaterParams + device.setWaterParams. Returns what build returned;
// `outParams` (optional) receives the params that were applied so the host can
// hang its caustics plane on out.seaLevel.
bool applyWorldWater(x3::rhi::IRenderDevice& device, const WorldWaterInput& in,
                     x3::rhi::IRenderDevice::WaterParams* outParams = nullptr);

// The clarity the env door resolved to for each channel (the approved constant
// unless X3_WATER_CLARITY is set). Exposed for the test and the A/B log line.
float worldWaterClarityFor(bool inCavern);

// --test-canonunderriver — headless. The underground river + ONE WATER as the
// CANON host stands them up (the canon freeway + interchange registered first,
// exactly as the canon boot slot does, so the field is the field the player
// walks). Gates:
//   CU1 the trench is ALREADY in the canon field (pre-UR ground above the bed
//       by >= kURCoverMin along the chain; worldWaterLevelAt answers the table)
//   CU2 the vault/beaches/water/lights/mist build (>0 each) into the scene,
//       every entity captured (nothing escapes the room stamp) and every mesh
//       owned by a captured entity (device shutdown frees all — no orphan)
//   CU3 headroom (U9's own measure on this field): a cavern you can stand in
//   CU4 ONE WATER switches channel with the focus: cavern polyline inside the
//       corridor, surface polyline + basin + shoreline outside, and back —
//       byte-identical on the return (no state carried)
//   CU5 clarity > 0 on BOTH channels at the approved values (0.82 / 0.94)
//   CU6 corridor clearance: the band never touches a registered corridor
//       (max |terrainCorridorDelta| over it == 0) and the measured distance
//       from the band edge to the canon freeway, the interchange crossroad,
//       the dealership, every city district mass, connector and tunnel plan
//       is logged and positive
bool runCanonUnderRiverSelfTest();

} // namespace x3::game
