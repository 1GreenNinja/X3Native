#pragma once
// ===========================================================================
// HOST SELF-TESTS — the GPU/headless self-test runners that live in the app
// host (they drive the REAL Vulkan device / real game world rather than a
// library-side mock). Factored VERBATIM out of app/main.cpp (#28 monolith
// split) into x3::apphost so the --test-* dispatch in main() stays a thin
// one-liner per flag.
//
//   runFrustumCullSelfTest  --test-frustumcull (CPU frustum math; no GPU)
//   runGpuCullSelfTest      --test-gpucull     (headless device, GPU cull equiv)
//   runDebrisSelfTest       --test-debris      (headless device, GPU debris)
//   runGpuSkinSelfTest      --test-gpuskin     (headless device, compute skin)
//   runHatchChainSelfTest   --test-hatch       (real world + Lua + bindings)
// ===========================================================================

namespace x3::apphost {

bool runFrustumCullSelfTest();
bool runGpuCullSelfTest();
bool runDebrisSelfTest();
bool runGpuSkinSelfTest();
bool runHatchChainSelfTest();

} // namespace x3::apphost
