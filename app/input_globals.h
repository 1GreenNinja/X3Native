#pragma once
// ============================================================================
// Shared input globals (#28 deep split). The mouse-wheel weapon-cycle
// accumulator + its GLFW scroll callback were file-scope in main.cpp but are
// consumed by BOTH the default render loop (main.cpp) AND the extracted
// --world streamed host. Moved VERBATIM into this header as inline globals so
// there is ONE definition shared across both TUs.
// ============================================================================
struct GLFWwindow;

namespace x3 { namespace apphost {

// Mouse-wheel accumulator (weapon cycling). The scroll callback adds the wheel
// delta; the main loop consumes it once per frame to switch weapons.
inline double g_weaponScroll = 0.0;
inline void scrollCallback(GLFWwindow* /*win*/, double /*xoff*/, double yoff) { g_weaponScroll += yoff; }

}} // namespace x3::apphost
