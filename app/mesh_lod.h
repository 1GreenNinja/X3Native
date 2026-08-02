#pragma once
// ============================================================================
// Discrete mesh LOD — SCREEN-SPACE-ERROR selection with hysteresis.
//
// CLEAN-ROOM, original work. Written from the published technique only:
//   * The standard screen-space geometric-error formulation used by terrain and
//     progressive-mesh LOD since Lindstrom et al. (1996) / Hoppe (1997):
//     project a world-space geometric deviation through the perspective divide
//     and compare it to a PIXEL budget.
//   * Real-Time Rendering 4th ed. ch. 19 (LOD switching, hysteresis) for the
//     dead-band treatment that stops threshold flicker.
// No GPL / id Tech / RBDOOM / Unreal source consulted. See
// docs/CLEANROOM_PROCESS.md.
//
// ---------------------------------------------------------------------------
// WHY SCREEN-SPACE ERROR AND NOT DISTANCE
// ---------------------------------------------------------------------------
// Distance-band LOD (what app/space/lod.h does, and what terrain.cpp does) swaps
// every object at the same camera distance regardless of how big it is. So a
// 90 m skyline tower and a 0.4 m crate both drop to LOD1 at, say, 25 m — the
// tower visibly pops (its LOD1 deviation is ~1 m, which is ~13 px at 25 m) while
// the crate could have dropped two levels 20 m earlier and saved triangles.
//
// The error metric fixes both ends at once. Each LOD level carries a MODEL-SPACE
// geometric error (metres of vertex displacement, measured by the decimator —
// app/mesh_decimate.h). Projected:
//
//     pixelError_i = error_i * maxScale * projScale / dist
//     projScale    = viewportH / (2 * tan(fovY/2))          [px per unit at 1 m]
//     dist         = max(|eye - worldCentre| - worldRadius, kMinDist)
//
// and the selector picks the COARSEST level whose pixelError is within budget.
// Because error_i scales with the object, a big object holds LOD0 far longer
// than a small one at the same distance — the property --test-geolod asserts and
// the property the distance-only negative control fails.
//
// `dist` uses the NEAREST POINT of the bounding sphere, not its centre: for a
// large object the centre can be far behind the visible surface, which would
// under-estimate the error of exactly the objects that pop worst.
//
// ---------------------------------------------------------------------------
// HYSTERESIS
// ---------------------------------------------------------------------------
// Selecting the coarsest level under a hard threshold makes an object parked at
// the threshold flip every frame as sub-pixel camera jitter moves `dist`. The
// selector therefore takes the PREVIOUS level and applies an asymmetric dead
// band around the budget T:
//
//     refine  (level -> level-1)  only when pixelError(level)   >  T*(1+h)
//     coarsen (level -> level+1)  only when pixelError(level+1) <= T*(1-h)
//
// With h > 0 the two conditions cannot both hold, so there is a distance band of
// width (1+h)/(1-h) in which NOTHING changes. h defaults to 0.15 (a 1.35x band).
//
// RHI-free and deterministic: --test-geolod exercises all of it with no GPU.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

inline constexpr uint32_t kMaxLodLevels = 4;

// A populated LOD chain for one mesh. Level 0 is full detail. All levels share
// ONE vertex buffer on the device (see IRenderDevice::createMeshLodChain); they
// differ only in their index buffer, which is why the chain is cheap.
struct MeshLodChain {
    x3::rhi::MeshHandle mesh[kMaxLodLevels]{};
    // Model-space geometric error of each level relative to level 0, in metres.
    // error[0] is 0 by construction. Ascending.
    float    error[kMaxLodLevels] = { 0, 0, 0, 0 };
    uint32_t triangles[kMaxLodLevels] = { 0, 0, 0, 0 };   // receipts for the perf readout
    float    center[3] = { 0, 0, 0 };   // model-space bounding-sphere centre
    float    radius    = 0.0f;          // model-space bounding-sphere radius
    uint32_t levels    = 0;             // populated count; 0 == no chain

    bool valid() const { return levels > 0 && mesh[0].valid(); }
    bool hasChain() const { return levels > 1; }
};

// Camera/projection state the metric needs. Fed from the render device.
struct LodView {
    float    eye[3]    = { 0, 0, 0 };
    float    fovYDeg   = 60.0f;
    uint32_t viewportH = 1080;

    // Pixels per world unit for a feature 1 m from the eye.
    float projScale() const;
};

// Tunables (cvar-backed; see app/mesh_lod.cpp for the r_meshlod* names).
struct LodPolicy {
    bool  enabled     = true;    // r_meshlod   — 0 restores today's behaviour exactly
    float pixelError  = 1.5f;    // r_meshlod_err  — the screen-space budget, px
    float hysteresis  = 0.15f;   // r_meshlod_hyst — dead-band half-width, fraction of the budget

    // NEGATIVE CONTROL ONLY — never enabled in the shipping path. Selects by raw
    // camera distance with fixed bands, i.e. the classic wrong behaviour.
    // --test-geolod turns this on to prove the large-vs-small assertion is real.
    bool  distanceOnly    = false;
    float distanceBand[3] = { 25.0f, 60.0f, 140.0f };
};

// Distance from the eye to the NEAREST POINT of the instance's bounding sphere,
// clamped away from zero. `model` is a column-major 4x4 (Entity::transform).
float lodDistance(const LodView& v, const MeshLodChain& c, const float model[16]);

// Largest axis scale of `model` (conservative for non-uniform scale) — the same
// rule the renderer's worldSphere() uses so CPU LOD and GPU cull agree.
float lodMaxScale(const float model[16]);

// Projected pixel error of rendering `level` of `c` at `model`.
float lodPixelError(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                    const float model[16], uint32_t level);

// STATELESS ideal level: the coarsest level whose pixel error is within budget.
// Monotonically non-decreasing in camera distance (the property --test-geolod
// asserts). Always returns 0 for a chain with no extra levels.
uint32_t lodSelect(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                   const float model[16]);

// HYSTERETIC level: same metric, but only leaves `prev` when the error clears the
// dead band. Pass the value this instance returned last frame.
uint32_t lodSelectHysteretic(const LodView& v, const LodPolicy& p, const MeshLodChain& c,
                             const float model[16], uint32_t prev);

// ---- Process-wide policy + cvar wiring -------------------------------------
// One policy, read by Scene::render every frame. A process global (rather than a
// Scene member) because the cvar sync hub in app_run.cpp does not know about any
// particular Scene, and every host in the engine shares one camera anyway.
//
// FALLBACK CONTRACT: r_meshlod 0 makes lodSelect/lodSelectHysteretic return 0 for
// every mesh, i.e. always LOD0 — which is exactly what the engine did before this
// lane. A mesh with no chain also always returns 0, so with no authored/generated
// chains in the scene the two settings are indistinguishable.
LodPolicy& lodPolicy();

// Per-frame receipts, so the triangle win is a measurement and not a claim.
struct LodFrameStats {
    uint32_t chained      = 0;   // entities that had a chain to choose from
    uint32_t perLevel[kMaxLodLevels] = { 0, 0, 0, 0 };
    uint64_t trisSelected = 0;   // triangles actually submitted for chained entities
    uint64_t trisLod0     = 0;   // triangles those entities would have submitted at LOD0
    uint32_t switches     = 0;   // entities whose level CHANGED this frame
};

// Fill `v` from whatever camera the device is about to render with.
LodView lodViewFromDevice(const x3::rhi::IRenderDevice& device);

// cvar wiring. registerLodCVars() is called once alongside the other r_* cvars;
// applyLodCVars() is called from the per-frame cvar sync hub and copies the
// values into lodPolicy(). Declared with a forward-declared console so scene.h
// (which includes this header everywhere) does not pull in IConsole.h.
} // namespace x3::game

namespace x3 { namespace con { class IConsole; } }

namespace x3::game {
void registerLodCVars(x3::con::IConsole& console);
void applyLodCVars(const x3::con::IConsole& console);

} // namespace x3::game
