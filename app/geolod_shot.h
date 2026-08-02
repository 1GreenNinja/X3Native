#pragma once
// ===========================================================================
// --screenshot-geolod <dir> — DISCRETE MESH LOD proof capture (Lane 5).
//
// Builds a scene out of REAL art (assets/converted_glb, via a CPU-side re-read
// so the geometry can be decimated — app/glb_cpu_read.h), gives every distinct
// mesh a generated LOD chain, and captures the SAME camera at three distances
// with r_meshlod 0 (off) and 1 (on). It prints, per capture:
//
//   * the exact triangle count submitted (device RenderStats + the CPU-side
//     per-level roll-up from Scene::lodStats)
//   * the per-level entity histogram
//   * mean GPU frame time over a settled window, LOD off vs on
//
// The A/B pairs are what the "no visible pop" verdict is read off.
// ===========================================================================

#include <string>

namespace x3 { namespace rhi { class IRenderDevice; } }

namespace x3::game {

// Returns 0 on success, 1 if the scene could not be built or a capture failed.
int runGeoLodShot(x3::rhi::IRenderDevice& device, const std::string& outDir);

} // namespace x3::game
