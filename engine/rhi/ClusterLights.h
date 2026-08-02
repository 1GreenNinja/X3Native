#pragma once
// ===========================================================================
// ClusterLights.h — CLUSTERED (froxel) FORWARD LIGHTING: the light-assignment
// half. Pure math + std containers; NO Vulkan, NO GLM-in-the-interface, so the
// headless self-test (--test-clusterlights) drives exactly the code the device
// runs, with no GPU in the loop.
//
// THE PROBLEM THIS SOLVES
// ----------------------
// Forward lighting here was a fixed 64-entry point-light array in a UBO, looped
// per fragment for EVERY pixel (shaders/mesh.frag, glass.frag). No culling, no
// clustering: at 1440p that is ~236M light evaluations per frame before overdraw,
// and the hard 64-light cap meant one tunnel's dressing would eat 48 slots.
//
// THE TECHNIQUE
// -------------
// Subdivide the view frustum into a 3D grid of "froxels" (frustum voxels):
// GX x GY tiles across the screen, GZ slices in depth. For each light, find the
// froxels its sphere of influence overlaps and append its index to those froxels'
// lists. Each fragment then computes its own froxel from gl_FragCoord + its view
// depth and iterates ONLY that froxel's list.
//
// Depth is sliced EXPONENTIALLY, not linearly. A linear slice near the camera is
// enormous in screen-space terms and would lump the whole near field into one
// froxel; the exponential law gives every slice the same depth RATIO, so near
// slices are thin and far slices are thick — which is what actually matches how
// froxels project. This is the standard formulation:
//
//     slice(z) = floor( log(z) * sliceScale + sliceBias )
//     sliceScale =  GZ / log(zFar / zNear)
//     sliceBias  = -GZ * log(zNear) / log(zFar / zNear)
//     zAtSlice(k) = zNear * (zFar / zNear) ^ (k / GZ)
//
// CLEAN-ROOM PROVENANCE (docs/CLEANROOM_PROCESS.md)
// -------------------------------------------------
// Written from the PUBLISHED technique only:
//   * Olsson & Assarsson, "Clustered Deferred and Forward Shading" (HPG 2012) —
//     the froxel-grid idea and the exponential depth subdivision.
//   * Standard published froxel-grid / tiled-forward write-ups for the
//     sphere-vs-froxel-AABB assignment test.
// The view-space froxel AABB construction and the assignment loop below were
// derived here from the perspective projection itself (see froxelBoundsView).
// NO GPL / id Tech / RBDOOM / Unreal source was consulted.
// ===========================================================================

#include <cstdint>

namespace x3 { namespace rhi {

struct PointLight;   // engine/rhi/IRenderDevice.h

// ---------------------------------------------------------------------------
// GRID DIMENSIONS — the tunables. Retune HERE and nowhere else: the shader reads
// the dimensions out of the per-frame UBO (Camera::clusterGrid), so C++ is the
// single source of truth and there is no second copy to drift.
//
// 16 x 9 x 24 = 3456 froxels.
//   * 16 x 9 matches the 16:9 frame, so a froxel is SQUARE on screen (80x80 px at
//     1280x720, 120x120 at 1920x1080). Square froxels keep the sphere-vs-AABB
//     test tight in both axes; a 16x16-tile grid on a 16:9 frame gives tall thin
//     froxels that over-include along Y.
//   * 24 depth slices: with the engine's 0.1 m near plane and a typical 2 km far
//     plane, each slice is a depth RATIO of (zFar/zNear)^(1/24) ~ 1.51, i.e. every
//     slice is ~51% deeper than the one before it. That puts ~half the slices
//     inside the first few metres (where a lamp's sphere is screen-huge and
//     needs the resolution) and spends the rest on the long tail.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kClusterGridX = 16;
inline constexpr uint32_t kClusterGridY = 9;
inline constexpr uint32_t kClusterGridZ = 24;
inline constexpr uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;  // 3456

// Per-cluster list capacity. The list buffer is FIXED-STRIDE (cluster i owns
// indices [i*kMaxLightsPerCluster, (i+1)*kMaxLightsPerCluster)) rather than a
// prefix-summed compaction: it costs 3456 * 64 * 4 B = 884 KB per frame-in-flight,
// which is nothing, and in exchange the assignment needs no second pass and the
// shader needs no indirection table.
inline constexpr uint32_t kMaxLightsPerCluster = 64;

// Scene-wide light cap. This is the number the whole feature exists to raise:
// the legacy path's cap was 64 (kMaxPointLights, still enforced on the legacy
// UBO so r_clusterlights 0 stays bit-exact). 1024 covers Echo Harbor's neon
// night city, a fully dressed tunnel, opulent interiors and car underglow with
// room to spare, at 1024 * 32 B = 32 KB of light SSBO.
inline constexpr uint32_t kMaxSceneLights = 1024;

inline constexpr uint32_t clusterIndex(uint32_t ix, uint32_t iy, uint32_t iz) {
    return (iz * kClusterGridY + iy) * kClusterGridX + ix;
}

// ---------------------------------------------------------------------------
// The camera the grid is built against. An ORTHONORMAL view basis + the
// projection's half-angles, which is all the froxel geometry needs — no matrices,
// so the self-test can construct one by hand and reason about it.
//
// The basis MUST match glm::lookAt(eye, eye + fwd, up):
//     right = normalize(cross(fwd, up))
//     up'   = cross(right, fwd)
//     viewX = dot(P - eye, right)
//     viewY = dot(P - eye, up')
//     viewZ = dot(P - eye, fwd)          // POSITIVE in front of the camera
// makeClusterView() below does exactly that so callers cannot get it wrong.
// ---------------------------------------------------------------------------
struct ClusterView {
    float camPos[3]   = { 0, 0, 0 };
    float camRight[3] = { 1, 0, 0 };
    float camUp[3]    = { 0, 1, 0 };
    float camFwd[3]   = { 0, 0, 1 };
    float tanHalfFovX = 1.0f;      // tan(horizontal FOV / 2)
    float tanHalfFovY = 1.0f;      // tan(vertical   FOV / 2)
    float zNear       = 0.1f;      // MUST be > 0 (the log slicing needs it)
    float zFar        = 2000.0f;

    // Derived exponential-slice constants (filled by makeClusterView).
    float sliceScale  = 0.0f;
    float sliceBias   = 0.0f;

    // ---- Precomputed lookup tables (filled by makeClusterView) -------------
    // The froxel-AABB test runs once per (light, candidate froxel) pair, so the
    // inner loop must not contain a pow() or a divide. These turn
    // clusterFroxelBoundsView into six multiplies and a few min/max.
    //   sliceZ[k]  = view depth at the near face of slice k (k in [0, GZ])
    //   edgeX[i]   = view x per unit depth at tile boundary i (i in [0, GX])
    //   edgeY[j]   = view y per unit depth at tile boundary j (j in [0, GY]);
    //                DESCENDING — j = 0 is the TOP of the screen (view +Y).
    float sliceZ[kClusterGridZ + 1] = {};
    float edgeX[kClusterGridX + 1] = {};
    float edgeY[kClusterGridY + 1] = {};
};

// Build a ClusterView from a camera the way the device has one: eye, a forward
// direction, a world up hint, the VERTICAL fov in degrees, the aspect ratio, and
// the near/far planes. Orthonormalizes exactly like glm::lookAt and fills the
// slice constants.
ClusterView makeClusterView(const float eye[3], const float fwd[3], const float up[3],
                            float fovYDegrees, float aspect, float zNear, float zFar);

// Depth slice for a LINEAR view depth (viewZ = dot(P - eye, camFwd)).
// Clamped to [0, kClusterGridZ-1]; anything nearer than zNear lands in slice 0
// and anything past zFar in the last slice, so a fragment can never index outside
// the grid. The shader implements the identical formula.
uint32_t clusterSliceForViewZ(const ClusterView& v, float viewZ);

// The view depth at the NEAR face of slice k (k in [0, kClusterGridZ]).
// zAtSlice(0) == zNear, zAtSlice(kClusterGridZ) == zFar.
float clusterSliceNearZ(const ClusterView& v, uint32_t k);

// Axis-aligned bounds of froxel (ix,iy,iz) in VIEW space (x = right, y = up,
// z = forward). Exposed for the self-test, which uses it to independently
// re-derive which froxels a light should have landed in.
void clusterFroxelBoundsView(const ClusterView& v, uint32_t ix, uint32_t iy, uint32_t iz,
                             float outMin[3], float outMax[3]);

// ---------------------------------------------------------------------------
// Assignment result. `overflows` is the whole point of counting: a dropped light
// is a light that visibly does not illuminate a froxel, and a silent drop is
// exactly the bug the old 64-light truncation was (332 fixtures in, 64 out, not
// one word logged).
// ---------------------------------------------------------------------------
struct ClusterBuildResult {
    uint32_t lightsConsidered   = 0;  // lights fed in (after the kMaxSceneLights clamp)
    uint32_t lightsVisible      = 0;  // lights whose sphere touched at least one froxel
    uint32_t lightsCulled       = 0;  // lights entirely outside the frustum -> free
    uint32_t assignments        = 0;  // (light, froxel) pairs actually written
    uint32_t overflows          = 0;  // pairs DROPPED because a froxel list was full
    uint32_t clustersOverflowed = 0;  // distinct froxels that hit the cap
    uint32_t maxClusterLoad     = 0;  // deepest froxel list (<= kMaxLightsPerCluster)
};

// ---------------------------------------------------------------------------
// Build the per-froxel light index lists.
//
//   outCounts  : kClusterCount uint32s        — list length per froxel
//   outIndices : kClusterCount * kMaxLightsPerCluster uint32s — fixed-stride lists
// Both are fully overwritten (counts are zeroed first); no residue from the
// previous frame can leak through.
//
// OVERFLOW POLICY (documented, deterministic, counted):
//   Lights are visited in ASCENDING LIGHT INDEX and appended to each overlapping
//   froxel's list. When a froxel already holds kMaxLightsPerCluster lights, the
//   candidate is DROPPED and `overflows` is incremented (and the froxel is counted
//   once in `clustersOverflowed`). So the winners are the LOWEST-INDEXED lights
//   that reach that froxel. That is a deliberate choice over "nearest wins" or
//   "brightest wins":
//     * it is O(1) with no per-froxel sort,
//     * it is DETERMINISTIC, which the repo's md5 / screenshot gates require —
//       a parallel scatter or a GPU atomic append would reorder frame to frame
//       and break every image gate in the tree,
//     * and it gives hosts a usable contract: put the lights that matter first.
//   The device logs a rate-limited warning whenever `overflows` is non-zero and
//   surfaces the counters in RenderStats, so this is never silent.
//
// The sphere test is CONSERVATIVE: the light radius is padded by
// kClusterRadiusPad before the sphere-vs-AABB test, so a fragment can never fall
// just outside an assigned froxel because of TAA's sub-pixel jitter (which moves
// gl_FragCoord, and therefore the froxel a fragment resolves to, by up to half a
// pixel). Over-inclusion costs a few wasted iterations; under-inclusion is a
// visible unlit block, so the bias is deliberate.
// ---------------------------------------------------------------------------
inline constexpr float kClusterRadiusPad = 0.02f;   // metres, absolute
inline constexpr float kClusterRadiusPadRel = 0.01f; // + 1% of the range

ClusterBuildResult buildClusterLightLists(const ClusterView& v,
                                          const PointLight* lights, uint32_t count,
                                          uint32_t* outCounts, uint32_t* outIndices);

}} // namespace x3::rhi
