// Cascaded Shadow Maps — cascade fitting math (device-independent).
//
// CLEAN-ROOM, original work. Written from the published technique only:
//   * Zhang, Sun, Xu, Lu, "Parallel-Split Shadow Maps for Large-scale Virtual
//     Environments" (2006) — the practical split scheme blending the
//     logarithmic and uniform distributions with a weight `lambda`.
//   * The standard cascaded-shadow-map literature (Real-Time Rendering 4th ed.,
//     ch. 7; the public Vulkan/D3D CSM sample descriptions) for the two
//     stability techniques applied here: sizing each cascade from a bounding
//     SPHERE of its frustum slice (rotation-invariant extent) and SNAPPING the
//     light-space origin to the shadow-map texel grid (kills edge swimming).
//   * The Vulkan 1.3 spec for the depth conventions.
// No GPL / id Tech / RBDOOM / Unreal source was consulted. See
// docs/CLEANROOM_PROCESS.md.
//
// WHY THIS FILE IS VULKAN-FREE: every number that decides shadow quality is
// computed here from plain camera/sun parameters, so `--test-csm` can assert on
// splits, containment, snapping and rotation-invariance WITHOUT a GPU, a device
// or a swapchain. The renderer (vk_passes.cpp) only consumes the result.
//
// CONVENTIONS (must match the renderer):
//   * Right-handed world; glm with GLM_FORCE_DEPTH_ZERO_TO_ONE (Vulkan [0,1] Z).
//   * `sunDir` points TOWARD the sun (same vector mesh.frag and the sky disk use).
//   * The returned viewProj already has the reverse-Y clip flip applied
//     (proj[1][1] *= -1), exactly like computeLightViewProj().
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>

namespace x3::csm {

// Cascade count. ONE constant to retune: everything (the GPU array layers, the
// UBO array, the shader loop) is sized from it. 4 is the sweet spot for an
// open-world/racing camera — 3 leaves the far cascade too coarse at a 250 m
// shadow distance, 5 costs a full extra depth pass for a band the eye cannot
// resolve. Must stay <= kMaxCascades.
static constexpr int kMaxCascades = 4;
static constexpr int kNumCascades = 4;
static_assert(kNumCascades >= 1 && kNumCascades <= kMaxCascades, "kNumCascades out of range");

// Practical-split blend weight (Zhang et al. 2006). 0 = uniform split (equal
// world-space slices; wastes resolution near the eye), 1 = logarithmic split
// (theoretically optimal for perspective aliasing but the first slice becomes
// absurdly thin at a 0.1 m near plane). 0.75 keeps the near cascade tight
// without collapsing it.
static constexpr float kDefaultLambda = 0.75f;

// How far from the eye cascades are fitted, in meters. The legacy single
// cascade reached 45 m; racing needs the shadowed region to outrun the car.
static constexpr float kDefaultShadowDistance = 250.0f;

// The near plane cascades start from. Fitting from the camera's true 0.1 m near
// plane would make split 0 microscopic under a logarithmic blend and buy
// nothing (nothing self-shadows meaningfully inside a metre).
static constexpr float kCascadeNear = 0.5f;

// Depth padding along the sun direction, in meters, added in FRONT of each
// cascade's slice so geometry that is off-screen but still casts INTO the slice
// (a cliff, a building above the road) is rasterized into the map.
static constexpr float kCasterMargin = 120.0f;

// Inputs. All plain data — no engine types, so the test can build one directly.
struct Params {
    glm::vec3 camPos{ 0.0f };
    glm::vec3 camFwd{ 0.0f, 0.0f, -1.0f };   // normalized
    glm::vec3 camUp { 0.0f, 1.0f, 0.0f };    // normalized, orthogonal-ish to fwd
    float     fovYDeg = 60.0f;
    float     aspect  = 16.0f / 9.0f;        // width / height
    float     zNear   = kCascadeNear;
    float     zFar    = kDefaultShadowDistance;
    glm::vec3 sunDir{ 0.4f, 1.0f, 0.3f };    // TOWARD the sun (normalized inside)
    float     lambda  = kDefaultLambda;
    uint32_t  shadowDim = 2048;              // per-cascade square resolution
    int       count   = kNumCascades;
};

struct Cascade {
    glm::mat4 viewProj{ 1.0f };  // world -> this cascade's shadow clip (reverse-Y applied)
    float splitNear = 0.0f;      // view-space depth this cascade starts at (m)
    float splitFar  = 0.0f;      // view-space depth it ends at (m)
    float radius    = 0.0f;      // bounding-sphere radius of the slice (m) — rotation INVARIANT
    float halfExtent = 0.0f;     // ortho half-width actually used (radius + one texel of pad)
    float texelWorld = 0.0f;     // world meters per shadow texel (the snap quantum)
    // Light-space center AFTER texel snapping. The stability test asserts on
    // exactly these two numbers: sub-texel camera motion must not change them.
    float snappedX = 0.0f;
    float snappedY = 0.0f;
    float depthBias  = 0.0f;     // constant bias in light-clip depth units
    float normalBias = 0.0f;     // world-space offset along the normal (m)
};

struct Result {
    Cascade  c[kMaxCascades]{};
    int      count = 0;
    // Far view-space depth of each cascade, in the layout the shader wants
    // (vec4 lane i = cascade i's far split). Lanes beyond `count` hold a huge
    // value so the selection loop always terminates on the last real cascade.
    glm::vec4 splitFar{ 0.0f };
};

// The practical (parallel-split) scheme: a per-index blend of the logarithmic
// and uniform distributions. i in [0, count].
float splitDistance(float zNear, float zFar, int i, int count, float lambda);

// Fit `p.count` cascades. Deterministic and side-effect free.
Result compute(const Params& p);

// The negative control used by --test-csm to prove the stability and rotation
// assertions can actually FAIL: identical fitting EXCEPT the extent comes from
// the axis-aligned bounding box of the frustum-slice corners in light space
// (rotation dependent) and the light-space origin is NOT snapped to the texel
// grid. This is the naive CSM everyone writes first. It must fail the tests the
// real implementation passes; see docs/design/LANE_DISPATCH_PLAN.md standing
// requirement 2.
Result computeNaive(const Params& p);

// The LEGACY single-cascade ortho light matrix (what r_csm 0 renders and what
// mesh.frag's legacy branch projects with). Extracted here VERBATIM from the
// original computeLightViewProj() so that (a) the renderer and the test call the
// exact same code, and (b) --test-csm can assert it is bit-identical to the
// historical expression — the guarantee the md5/screenshot gates rest on.
glm::mat4 legacyOrthoViewProj(const glm::vec3& center, const glm::vec3& sunDirNorm,
                              float ortho, float depthHalf);

// Shared helper: the fixed light-space rotation for a sun direction. A PURE
// rotation about the world origin — it depends only on `sunDir`, never on the
// camera. That is what makes the texel grid a stable world-anchored lattice
// (snapping in a frame that itself slid with the camera would snap nothing).
glm::mat4 lightRotation(const glm::vec3& sunDirNorm);

} // namespace x3::csm
