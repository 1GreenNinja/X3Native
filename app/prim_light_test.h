#pragma once
// --test-primlight: ONE LIGHTING PATH. Asserts a graybox PRIM surface and a GLB
// surface with the same albedo, geometry and light receive the SAME radiance —
// the invariant KNOWN_BUGS R1 fixed by hand and nothing has guarded since.
// Ships with a negative control (inverted normals) that proves the probe can fail.
#include "engine/rhi/IRenderDevice.h"

#include <string>

namespace x3::game {

// Renders the probe on the REAL device, writes `outPath`, reads it back and
// measures it. Returns 0 when every assertion holds.
int runPrimLightTest(x3::rhi::IRenderDevice& device, const std::string& outPath);

} // namespace x3::game
