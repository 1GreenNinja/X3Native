#pragma once
// --test-csm — headless self-test for cascaded shadow maps (Lane 3).
// Exercises engine/rhi/Csm.h with NO GPU, NO device and NO window: splits,
// frustum-slice containment, texel-snap stability, rotation invariance, the
// bit-exact r_csm 0 legacy matrix, per-cascade bias, and a NEGATIVE CONTROL
// that proves the stability/rotation assertions can fail.
//
// Clean-room, original work. No id Tech / RBDOOM source consulted.

namespace x3::game {

// Returns true when every assertion passed (--test-csm exit code 0).
bool runCsmSelfTest();

} // namespace x3::game
