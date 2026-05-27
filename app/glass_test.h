#pragma once
// Translucent-glass material self-test (--test-glass). Asserts the GlassMaterial /
// transparent-flag plumbing and the opaque-vs-transparent submission split without a
// window or Vulkan: a counting HeadlessRenderDevice records which draw path each
// Scene entity takes, so the test verifies glass entities route through
// drawMeshGlass (and opaque entities through drawMeshEmissive), that GlassMaterial
// params ride through unchanged, and that a back-to-front view-depth sort orders
// transparent draws correctly. Prints `glass: X/Y passed`; nonzero exit on fail.
//
// Game/slice code only; engine/ stays pure.
namespace x3::game {

bool runGlassSelfTest();

} // namespace x3::game
