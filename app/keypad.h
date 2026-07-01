#pragma once
// REALISTIC high-poly wall KEYPAD / access terminal for the canonical facility's
// locked doors (Tim's ask: "realistic high-poly door panels + real keypad button
// geometry, not flat quads"). Game/slice code only — engine/ stays pure.
//
// Each keypad is composed from many small boxes merged into ONE high-poly mesh
// (a recessed backplate, a chamfered bezel frame, a glowing screen inset, a 3x4
// grid of individually-raised + beveled keys, an Enter key, and a status LED),
// so it reads as a real device with depth and button relief rather than a flat
// quad. The whole unit is a single Scene entity (cull-friendly, one draw) tagged
// to the door's room; the glowing screen + status LED are emissive so they read
// as live electronics. A second emissive entity carries the screen glow.
//
// Placement: flush-mounted on a wall, facing along ±X or ±Z (matched to the door
// axis), centered at eye-reachable height (~1.4 m). No collision (decorative — the
// player interacts via the existing KeypadEntry/door-code state machine; the keypad
// is the visual anchor for that interaction). NO render artifacts: every box is
// inset so coplanar faces are offset (no z-fighting), the unit sits proud of the
// wall (no surface coincidence), and the bezel overlaps the backplate cleanly.

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Which way the keypad's FRONT faces (the readable side), matching the host wall:
//   PlusX  : front faces +X (mounted on a wall whose interior is to +X)
//   MinusX : front faces -X
//   PlusZ  : front faces +Z
//   MinusZ : front faces -Z
enum class KeypadFacing : uint32_t { PlusX = 0, MinusX = 1, PlusZ = 2, MinusZ = 3 };

// Status colour of the keypad screen + LED: Locked (red) or Unlocked/granted
// (green). Drives the emissive tint only (geometry is identical).
enum class KeypadStatus : uint32_t { Locked = 0, Unlocked = 1 };

// Build ONE realistic keypad at wall position (x,y,z) facing `facing`, and add it
// to `scene`. `roomId` tags both entities for the per-room cull (pass the door's
// room, or kNoRoom for always-visible). Returns the entity id of the keypad BODY
// (the screen-glow entity is the next id). The keypad is ~0.34 m wide x 0.46 m tall
// and stands ~0.06 m proud of the mount plane.
//
// `status` sets the initial screen/LED colour. The returned ids let the host recolour
// the screen on unlock (see setKeypadStatus) — e.g. flip to green when the code is
// accepted.
struct KeypadHandles { uint32_t body = kNoLink; uint32_t screen = kNoLink; };
KeypadHandles buildKeypad(Scene& scene, x3::rhi::IRenderDevice& device,
                          float x, float y, float z, KeypadFacing facing,
                          KeypadStatus status, uint32_t roomId);

// Recolour a keypad's screen + status LED to reflect a state change (Locked red ->
// Unlocked green). Safe no-op if the handles are invalid.
void setKeypadStatus(Scene& scene, const KeypadHandles& kp, KeypadStatus status);

// Headless self-test (--test-keypad). Asserts a keypad builds as a SINGLE high-poly
// body mesh (not a flat quad: many triangles, real Z depth front-to-back), the screen
// glow entity is emissive, the key grid produces the expected box count, and the unit
// stands proud of its mount plane (no wall-coincident faces => no z-fighting). Logs
// PASS/FAIL KP#, returns true iff all pass. No window/Vulkan.
bool runKeypadSelfTest();

} // namespace x3::game
