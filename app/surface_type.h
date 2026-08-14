#pragma once
// ===========================================================================
// SURFACE TYPE — the one place the game names what a surface IS, and one table
// of what that means. (Tim, 2026-08: "Lets definitely make a surface type enum.")
//
// WHY THIS EXISTS
// ---------------
// Three shipped bugs in this repo are the same bug: code decided where something
// sits relative to a surface using a number that was not the surface.
//   1. spawnDeathRagdoll passed originY=0 to makeHumanoidRagdollBones — which
//      places the PELVIS — and buried the corpse ~0.88 m in the floor.
//   2. echoShipPose existed to seat hulls on their own water and was NEVER
//      CALLED; hulls floated 0.606 m proud.
//   3. THIS one: the rescue girls are authored feet-at-origin and placed at a
//      level-wide `kGroundY` constant, while the floor they stand on is a slab
//      whose TOP is 0.15 m higher. Boots in the concrete.
// Every one of them was SILENT. The fix is not another careful call site; it is
// a named concept + a table + an assertion that shouts.
//
// THE DESIGN — enum + PROPERTY TABLE, never a switch
// --------------------------------------------------
// Consumers must NOT `switch (surf)`. N switches drift apart and every new
// surface silently misbehaves in the three places nobody updated. Consumers ask
// surfaceProps(s) and read a field. ADDING A SURFACE IS ONE TABLE ROW.
//
// SCOPE (deliberate): this header DEFINES the type + the table + a CPU
// classifier for terrain. The only consumer wired in this change is the
// grounding rule (app/grounding.h). Footstep audio, tyre grip and impact FX are
// designed for but NOT wired here — see "FUTURE CONSUMERS" below.
//
// HEADER-ONLY ON PURPOSE: app/CMakeLists.txt is owned by an in-flight 29-commit
// EXE split. A new .cpp would mean editing it. Everything here is constexpr /
// inline, so it costs nothing and conflicts with nothing.
// ===========================================================================

#include <cstdint>
#include <cmath>

namespace x3::game {

// ---------------------------------------------------------------------------
// The flat list. Keep it SMALL and keep Unknown LAST-RESORT-SAFE.
//
// `Unknown` is the value an unclassified surface gets — an arbitrary GLB
// interior floor, an authored slab, a prop deck. There is no data to classify
// those, and this header does NOT guess. Unknown is SOLID and non-penetrable,
// so an unclassified surface FAILS CLOSED: a character may not sink into it.
// That is exactly the behaviour that would have caught all three bugs above.
// ---------------------------------------------------------------------------
enum class SurfaceType : uint8_t {
    Unknown = 0,   // unclassified -> solid, safe defaults. MUST stay index 0.
    Concrete,
    Asphalt,
    Metal,
    Rock,
    Gravel,
    Wood,
    Glass,
    Grass,
    Dirt,
    Mud,
    Sand,
    Snow,
    Ice,
    Water,
    Lava,
    Count
};

// ---------------------------------------------------------------------------
// The property table row.
//
// footPenetration — METRES a grounded character's LOWEST extent may legitimately
//   sit below the surface. This is the field that serves Tim's rule directly:
//   "character feet can NOT enter the floor unless its water, sand, or lava".
//   0 = solid, feet may not enter, full stop.
// grip / wetGrip — friction coefficient dry / when wet. Provided for the vehicle
//   tyre + `inspx/wetness` lane to adopt; NOTHING in this change reads them.
// liquid — a fluid volume rather than a deformable solid. Changes what
//   footPenetration MEANS (submersion vs displacement) and gates splash FX.
// name / footstepTag / impactTag — stable string keys for audio + FX to bind to
//   later. footstepTag is a SET name, not a file: boot_audio.h currently loads
//   only footstepConcrete[4] and plays it unconditionally, so today every tag
//   would resolve to the same set. That is a gap, not a lie — see FUTURE.
// ---------------------------------------------------------------------------
struct SurfaceProps {
    float       footPenetration = 0.0f;
    float       grip            = 1.00f;
    float       wetGrip         = 0.75f;
    bool        liquid          = false;
    const char* name            = "unknown";
    const char* footstepTag     = "concrete";
    const char* impactTag       = "dust";
};

// ---------------------------------------------------------------------------
// THE TABLE. One row per SurfaceType, in enum order. Adding a surface = adding
// a row here and a value to the enum. Nothing else in the engine changes.
//
// PENETRATION VALUES — where they come from, and the one judgement call:
//   Tim named THREE penetrable surfaces: water, sand, lava. Those are set from
//   his list.
//   * Water 0.60 — mid-shin wading before it should be a swim/water-plane
//     problem rather than a grounding one.
//   * Sand  0.05 — a boot sinks about this far in dry beach sand.
//   * Lava  0.10 — crusted flow; a character wading lava has bigger problems.
//   MUD and SNOW are NOT on Tim's list, and I am flagging that I added them
//   rather than burying it: both are physically penetrable, SNOW is already a
//   named terrain splat band in shaders/inc/mesh_terrain.glsl (so the classifier
//   below can genuinely return it), and leaving them at 0 would make the rule
//   fire on legitimately correct art. Their values are deliberately SMALL
//   (0.05 / 0.06) — far under the 0.15 m defect this change fixes, so a real
//   sink still trips the assertion even on snow. If Tim wants his three and only
//   his three, set these two rows to 0.0f and nothing else moves.
//   ICE is 0 (it is solid; it is a GRIP problem, not a penetration problem).
// ---------------------------------------------------------------------------
inline constexpr SurfaceProps kSurfaceTable[(int)SurfaceType::Count] = {
    // penetration  grip   wetGrip liquid name        footstep    impact
    {  0.00f,       0.95f, 0.70f,  false, "unknown",  "concrete", "dust"     }, // Unknown  (SOLID by design)
    {  0.00f,       1.00f, 0.72f,  false, "concrete", "concrete", "dust"     }, // Concrete
    {  0.00f,       1.05f, 0.68f,  false, "asphalt",  "concrete", "dust"     }, // Asphalt
    {  0.00f,       0.90f, 0.55f,  false, "metal",    "metal",    "spark"    }, // Metal
    {  0.00f,       0.95f, 0.65f,  false, "rock",     "rock",     "chip"     }, // Rock
    {  0.00f,       0.85f, 0.70f,  false, "gravel",   "gravel",   "dust"     }, // Gravel
    {  0.00f,       0.92f, 0.62f,  false, "wood",     "wood",     "splinter" }, // Wood
    {  0.00f,       0.80f, 0.35f,  false, "glass",    "glass",    "shard"    }, // Glass
    {  0.00f,       0.88f, 0.70f,  false, "grass",    "grass",    "leaf"     }, // Grass
    {  0.00f,       0.85f, 0.60f,  false, "dirt",     "dirt",     "dust"     }, // Dirt
    {  0.05f,       0.60f, 0.45f,  false, "mud",      "mud",      "splat"    }, // Mud   (see note)
    {  0.05f,       0.70f, 0.65f,  false, "sand",     "sand",     "puff"     }, // Sand  (Tim)
    {  0.06f,       0.55f, 0.40f,  false, "snow",     "snow",     "puff"     }, // Snow  (see note)
    {  0.00f,       0.18f, 0.12f,  false, "ice",      "ice",      "chip"     }, // Ice
    {  0.60f,       0.40f, 0.40f,  true,  "water",    "water",    "splash"   }, // Water (Tim)
    {  0.10f,       0.50f, 0.50f,  true,  "lava",     "lava",     "ember"    }, // Lava  (Tim)
};

// The ONE accessor. Out-of-range folds to Unknown (safe: solid).
constexpr const SurfaceProps& surfaceProps(SurfaceType s) {
    const int i = (int)s;
    return kSurfaceTable[(i >= 0 && i < (int)SurfaceType::Count) ? i : 0];
}

// Convenience predicates, expressed through the table so they can never drift
// from it. THESE ARE THE RULE, in code:
//   "character feet can NOT enter the floor unless its water, sand, or lava"
constexpr bool surfaceIsPenetrable(SurfaceType s) {
    return surfaceProps(s).footPenetration > 0.0f;
}
constexpr float surfaceFootAllowance(SurfaceType s) {
    return surfaceProps(s).footPenetration;
}
constexpr const char* surfaceName(SurfaceType s) { return surfaceProps(s).name; }

// ---------------------------------------------------------------------------
// TERRAIN CLASSIFIER — CPU mirror of the GPU splat.
//
// shaders/inc/mesh_terrain.glsl decides grass / rock / snow / sand per fragment
// from WORLD height + slope + shoreline distance. That shader is the authority
// and it cannot be called from C++, so this is a deliberate MIRROR of its bands,
// not a second opinion. The band constants below are copied from that file and
// must track it; the shader's value-noise jitter is intentionally NOT mirrored
// (it is sub-metre cosmetic dither on the band edges and mirroring it would make
// this classifier non-deterministic for no gain).
//
// Returns the DOMINANT layer. Slope-rock wins where the ground is steep, exactly
// as the shader composites it.
// ---------------------------------------------------------------------------
namespace terrainsplat {
// --- copied from shaders/inc/mesh_terrain.glsl (keep in sync) ---
inline constexpr float kSeaLevel    = 4.0f;
inline constexpr float kSandTop     = 16.0f;
inline constexpr float kSnowBottom  = 180.0f;
inline constexpr float kSnowFull    = 265.0f;
inline constexpr float kAlpineLo    = 75.0f;
inline constexpr float kAlpineHi    = 140.0f;
inline constexpr float kSlopeRockLo = 0.82f;   // normal.y <= this -> full rock
inline constexpr float kSlopeRockHi = 0.94f;   // normal.y >= this -> no rock
inline constexpr float kShoreX      = 1100.0f, kShoreZ = -1350.0f;

inline float smoothstepf(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace terrainsplat

// worldY = ground height at (x,z); normalY = the surface normal's Y (1 = flat).
inline SurfaceType classifyTerrainSurface(float worldX, float worldY, float worldZ,
                                          float normalY) {
    using namespace terrainsplat;
    const float dx = worldX - kShoreX, dz = worldZ - kShoreZ;
    const float shoreDist = std::sqrt(dx * dx + dz * dz);
    const float shore  = 1.0f - smoothstepf(950.0f, 1500.0f, shoreDist);
    const float sand   = (1.0f - smoothstepf(kSeaLevel - 2.0f, kSandTop, worldY)) * shore;
    const float alpine = smoothstepf(kAlpineLo, kAlpineHi, worldY);
    const float snow   = smoothstepf(kSnowBottom, kSnowFull, worldY)
                       * smoothstepf(0.55f, 0.80f, normalY);
    const float rock   = 1.0f - smoothstepf(kSlopeRockLo, kSlopeRockHi, normalY);

    // Composite order matches the shader: grass base, then sand, alpine->rock,
    // snow cap, and slope-rock overrides everything where it is steep.
    if (rock   > 0.5f) return SurfaceType::Rock;
    if (snow   > 0.5f) return SurfaceType::Snow;
    if (sand   > 0.5f) return SurfaceType::Sand;
    if (alpine > 0.5f) return SurfaceType::Rock;
    return SurfaceType::Grass;
}

// ---------------------------------------------------------------------------
// FUTURE CONSUMERS — designed for, deliberately NOT wired in this change.
//
//  * FOOTSTEP AUDIO. I checked: there is no existing "what am I standing on"
//    query to hook. boot_audio.h loads footstepConcrete[0..3] and app/cues.h
//    fires CueKind::Footstep with a position and nothing else — every character
//    on every surface plays concrete. footstepTag above is the seam that fixes
//    that, but rebuilding footstep audio is its own lane.
//  * VEHICLE TYRE GRIP + RAIN. `inspx/wetness` is IN FLIGHT and owns this.
//    grip/wetGrip exist for it to adopt; wiring them here would collide.
//  * IMPACT FX / DECALS. impactTag is the key; no consumer yet.
// ---------------------------------------------------------------------------

} // namespace x3::game
