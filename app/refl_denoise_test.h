// --test-refldenoise — the REFLECTION DENOISE filter self-test.
//
// Pure CPU: engine/rhi/ReflDenoise.{h,cpp} is deliberately Vulkan-free so the
// filter's PROPERTIES can be asserted with no device, no swapchain and no
// window. shaders/refl_denoise.comp is a transcription of the same function;
// the screenshots are the gate on that copy.
#pragma once

namespace x3::game {

// Returns true when every assertion passes. Includes a permanent NEGATIVE
// CONTROL (the same filter with the edge stops removed — a plain a-trous box)
// which is REQUIRED to fail the edge-preservation assertions; if the control
// ever passes them, the assertions have no teeth and the test fails.
bool runReflDenoiseSelfTest();

} // namespace x3::game
