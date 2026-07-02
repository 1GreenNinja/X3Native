#pragma once
// ============================================================================
// screenshot_hosts — the headless SCREENSHOT/CAPTURE handlers extracted out of
// main() (#28 deep split, Phase B). These run AFTER device init but BEFORE the
// --world host dispatch / the default render loop: each builds a small bespoke
// scene, renders a still (or capture sequence), writes a PNG/GIF, tears down the
// device + window + glfw, and returns the program exit code. Lifted VERBATIM
// (the only edits are the HostContext alias prelude + device.get() -> device).
//
// dispatchScreenshotHosts() runs each handler's `if (flag) {...}` in the SAME
// order main() did and returns the exit code, or -1 = "no capture flag set" so
// boot continues into the --world host dispatch.
// ============================================================================

namespace x3 { namespace apphost {

struct HostContext;

int dispatchScreenshotHosts(HostContext& hc);

}} // namespace x3::apphost
