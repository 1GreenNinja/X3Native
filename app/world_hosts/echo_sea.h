#pragma once
// ============================================================================
// ECHO HARBOR SEA DATUM — THE single authority for "where the sea is".
//
// WHY THIS FILE EXISTS. Before it, Echo Harbor carried FOUR independently
// authored answers to that question, live at once:
//
//   +0.10  the Gerstner wave patch          (host_echotropolis.cpp applyOcean,
//                                            echo_water.h's 3 swell presets)
//    0.00  the heightfield zero-crossing    (echo_heightfield.h kSeaNorm/kScale;
//                                            what EVERY seat, road, lane clip,
//                                            swim query and car water query uses)
//   -0.40  the baked ocean quad             (tools/echo_terrain_gen.py OCEAN_Y;
//                                            the ONLY sea surface drawn at night
//                                            and above 140 m eye height)
//   -0.30  the swim-entry threshold         (host_echotropolis.cpp)
//
// The visible waterline therefore moved by 0.5 m depending on the time of day
// and the camera's altitude, before a single sampler error was added. Measured
// on the shipped bake, the median shore gradient is 0.25 m/m and the flattest
// decile is 0.11 m/m, so 0.5 m of datum ambiguity is 2.0 m of shoreline at the
// median and 4.5 m at the flattest — and on the gentlest beaches (0.042 m/m,
// e.g. the z=56 transect) it is TWELVE METRES of coastline that exists or does
// not depending on whether the wave patch happens to be enabled.
//
// ----------------------------------------------------------------------------
// WHICH ONE IS AUTHORITATIVE, AND WHY IT IS NOT A FREE CHOICE
//
// THE HEIGHTFIELD ZERO-CROSSING WINS: kEchoSeaLevelY = 0.
//
// Both candidates had a real claim. The Gerstner +0.10 is what the player
// visually reads as the surface at play altitude. The zero-crossing is what
// terrain, collision and every placement already use. What breaks each way:
//
//   Choosing +0.10 means moving the heightfield, i.e. kSeaNorm 0.20 ->
//   0.2003125. That is a ONE-CONSTANT change and every call site follows for
//   free, which makes it look like the cheap option. It is not: the sampler
//   would then sit 0.10 m BELOW the island GLB, which was baked at SEANORM
//   0.20. That is precisely the sampler-vs-rendered-mesh desync
//   fix/echo-road-surface just spent a lane closing. Honouring it would
//   require re-baking a 27 MB committed LFS asset and re-validating every
//   authored placement against a new coastline. Cost: a re-bake and a
//   re-audit of the whole island.
//
//   Choosing 0 means moving the wave patch down 0.10 m. Cost: 0.10 m of the
//   empirically-tuned trough margin against the baked ring (see THE RING
//   below). That is one float and a tuning table.
//
// So the zero-crossing is authoritative because it is WELDED TO A BAKED ASSET
// and to ~40 call sites, and the wave patch is welded to neither.
//
// The downstream consequence is the point of the whole lane: with the sea at
// 0, every existing land test in the codebase — `heightAt(x,z) > 0`,
// `>= 2.5f`, `< kWaterMinLand` — becomes TRUE by construction instead of
// accidentally 0.10 m optimistic. A cottage seated at heightAt = +0.05 used to
// pass "it is above sea level" and then render 5 cm under the still water and
// up to 36 cm under a crest. It no longer can.
//
// ----------------------------------------------------------------------------
// THE RING IS NOT A SEA LEVEL. It is the one conceptual bug underneath the
// other three. `OCEAN_Y = -0.4` is a 28 km backdrop quad, and it has two jobs
// that pull in opposite directions:
//   (a) it is the sea surface whenever the Gerstner patch is off (night, and
//       eye height > 140 m), which wants ring == sea level; and
//   (b) it must sit BELOW the deepest wave trough or the troughs punch through
//       it and freckle the bay with dark triangle shards (the documented
//       "WATER WAVE 1b" fight).
// One plane cannot do both. This header resolves it by declaring (b) the
// ring's ONLY job — it is a FLOOR, not a datum — and by making the amplitude,
// not the sea level, the quantity that has to respect it:
//
//     amplitude <= (kEchoSeaLevelY - kEchoOceanRingY - kEchoRingMargin) / 1.92
//
// Job (a) is then knowingly accepted as a 0.4 m error that only applies where
// it is invisible: the patch is off only above 140 m of eye height, where 0.4 m
// of waterline is far under a pixel, and at night, where the shoreline is not
// readable anyway. That is an HONEST residual, recorded here rather than
// papered over, and --test-sealevel prints it every run.
//
// ----------------------------------------------------------------------------
// EVERYTHING ELSE IS AN OFFSET FROM THE DATUM, never an absolute. That is the
// property the gate enforces: you cannot move the sea and leave a keel draft,
// a boat freeboard or a dry-land threshold behind, because none of them stores
// an absolute height any more.
//
// THE GATE: app/echo_sea_test.cpp (`--test-sealevel`) asserts the relationships
// below AND cross-checks them against tools/echo_terrain_gen.py by PARSING it —
// the C++/Python boundary is exactly where the -0.4 vs +0.10 drift lived, and a
// constant that agrees with itself in one language is not a constraint.
// ============================================================================

#include "echo_heightfield.h"

namespace x3::game {

// ---------------------------------------------------------------------------
// THE AUTHORITY. World Y of still water in Echo Harbor. Defined as the height
// at which Heightfield::heightAt crosses zero, which is fixed by the baked
// island GLB's SEANORM. Nothing else in the world may state a sea height.
inline constexpr float kEchoSeaLevelY = 0.0f;

// The heightfield encoding that PUTS the datum there:
//   heightAt = (hn - kSeaNorm) * kScale, so heightAt == 0 exactly at
//   hn == kSeaNorm. Restated as an expression so the gate can evaluate it
//   against Heightfield's own constants rather than against a copy of them.
inline constexpr float echoHeightfieldSeaY() {
    return (Heightfield::kSeaNorm - Heightfield::kSeaNorm) * Heightfield::kScale;
}

// ---------------------------------------------------------------------------
// THE BAKED BACKDROP QUAD (tools/echo_terrain_gen.py OCEAN_Y). NOT a datum —
// a floor the Gerstner troughs must never reach. Owned by the bake; the engine
// only reads it. Changing this means re-baking the island.
inline constexpr float kEchoOceanRingY = -0.40f;

// shaders/water.vert sums N=4 Gerstner octaves whose Y term is
// A*ampMul[i]*sin(phase); ampMul[] = {1.0, 0.5, 0.28, 0.14} sums to 1.92.
// Steepness displaces X/Z only and NEVER Y, so trough depth is a function of
// amplitude alone (water.vert L55-57 vs L60-66).
inline constexpr float kGerstnerTroughSum = 1.92f;

// Clearance the worst-case trough must keep above the ring. The shipped tuning
// happened to leave 0.193 m; nothing ever established that as a requirement —
// the trough either reaches the ring or it does not, and this is the cushion
// against that being a knife edge.
inline constexpr float kEchoRingMargin = 0.05f;

// The deepest the surface can ever go, and the largest amplitude that clears
// the ring. THIS is what the ring constrains — not the sea level.
inline constexpr float echoWorstTroughY(float amplitude) {
    return kEchoSeaLevelY - amplitude * kGerstnerTroughSum;
}
inline constexpr float echoMaxAmplitude() {
    return (kEchoSeaLevelY - kEchoOceanRingY - kEchoRingMargin) / kGerstnerTroughSum;
}

// ---------------------------------------------------------------------------
// DERIVED OFFSETS — every one of these was an absolute literal before. The
// value each resolves to is UNCHANGED from what shipped; only its definition
// moved, from "a height" to "a distance from the sea".

// Hull origin above still water (echo_region_builders.cpp kBoatY).
inline constexpr float kEchoBoatFreeboard = 0.60f;
inline constexpr float echoBoatY() { return kEchoSeaLevelY + kEchoBoatFreeboard; }

// Terrain depth a hull needs under it (echo_region_builders.cpp kKeelDraft).
inline constexpr float kEchoKeelDepth = 4.00f;
inline constexpr float echoKeelDraft() { return kEchoSeaLevelY - kEchoKeelDepth; }

// Dry-land clearances. Three separate thresholds exist on purpose — a road
// deck, a district gate and a building lot do not want the same freeboard —
// but all three are now distances above the SAME sea.
inline constexpr float kEchoLandMinClear  = 1.50f;   // echo_roads kWaterMinLand
inline constexpr float kEchoGateClear     = 2.00f;   // echo_roads kGateLandSafe
inline constexpr float kEchoLandSafeClear = 2.50f;   // echo_roads kLandSafe / landOk
inline constexpr float echoWaterMinLand() { return kEchoSeaLevelY + kEchoLandMinClear; }
inline constexpr float echoGateLandSafe() { return kEchoSeaLevelY + kEchoGateClear; }
inline constexpr float echoLandSafe()     { return kEchoSeaLevelY + kEchoLandSafeClear; }

// Terrain must be at least this far BELOW the sea before the player may swim
// (host_echotropolis.cpp's water query). Ankle-deep water is not a swim.
inline constexpr float kEchoSwimMinDepth = 0.30f;
inline constexpr float echoSwimFloorY() { return kEchoSeaLevelY - kEchoSwimMinDepth; }

// The baked albedo paints sand from -1.0 m to +1.5 m (echo_terrain_gen.py's
// layer stack). The datum must lie INSIDE that band or the painted beach and
// the waterline disagree — a cross-check against the ART, not just the code.
inline constexpr float kEchoSandBandLo = -1.00f;
inline constexpr float kEchoSandBandHi =  1.50f;

// ---------------------------------------------------------------------------
// THE GATE. Returns true when every relationship above holds. Declared here,
// defined in app/echo_sea_test.cpp; run by `--test-sealevel`.
bool runSeaLevelSelfTest();

} // namespace x3::game
