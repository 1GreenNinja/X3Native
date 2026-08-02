#pragma once
// ===========================================================================
// --test-clusterlights — CLUSTERED (froxel) FORWARD LIGHTING acceptance gate.
//
// PART A (CPU, no GPU): the froxel grid + assignment in engine/rhi/ClusterLights.
//   Every assignment the production code produces is compared against an
//   INDEPENDENT BRUTE-FORCE sweep of all 3456 froxels, so the test does not
//   re-implement the fast path's shortcuts — it checks the fast path against the
//   definition. Plus the depth-slice law, lights outside the frustum, the
//   overflow policy, and a NEGATIVE CONTROL proving the comparator can go red.
//
// PART B (real Vulkan device): renders the SAME rig with r_clusterlights 0 and 1
//   and asserts the two captures are BIT-IDENTICAL when the scene fits in 64
//   lights. That is a genuine end-to-end proof of the fragment->froxel lookup:
//   a light assigned to the wrong froxel is a light missing from a pixel, and
//   the images diverge. Then it pushes past 64 lights, where the legacy path
//   physically cannot see them, and asserts the clustered path does.
// ===========================================================================

#include <string>

namespace x3 { namespace rhi { class IRenderDevice; } }

namespace x3::game {

// Returns 0 on success, 1 on any failed assertion. `outDir` receives the Part-B
// A/B captures.
int runClusterLightTest(x3::rhi::IRenderDevice& device, const std::string& outDir);

} // namespace x3::game
