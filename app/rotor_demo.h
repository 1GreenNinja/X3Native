#pragma once
// Rotor-spin prototype — procedural spinning propellers/rotors.
//
// Game/slice content code only — engine/ stays pure. CLEAN-ROOM, original work:
// built ONLY from X3Native's own Scene / mesh_prims + the engine RHI interface +
// hand-rolled matrix math. No RBDOOM / id Tech / Doom / Quake — or any other
// game-engine — source consulted. NO renderer/engine changes.
//
// WHY THIS EXISTS: mechanical rotors (quadcopter props, fan blades, turbines) spin
// at a constant angular velocity that should ALSO scale with throttle/thrust — a
// thing a baked, fixed-rate animation clip can't do well, and which the in-game
// `Characters/Drone.glb` doesn't do at all (it loads as a single fused, static mesh
// with no separable rotor nodes). The right tool is a PROCEDURAL node-spin: each
// frame, rewrite the rotor entity's model transform to `Translate(hub)·Rotate(axis,
// θ(t))`. Scene::render then draws it spinning — no skinning, no clip, no new engine
// code (Entity already carries a column-major `transform[16]`, and Scene::update
// leaves bodyless entities' transforms untouched, so a rotor's transform is ours to
// drive).
//
// REUSE: `RotorSpin` is content-agnostic — `addRotor(entity, hub, axis, rps)` attaches
// the spin to ANY existing Scene entity, so once a real drone GLB is re-exported with
// its rotors as SEPARATE nodes, point this at those rotor-node entities and they spin.
// `buildGrayboxQuad()` is the self-contained demo/proof: a box body + 4 counter-
// rotating blade rotors, verifiable headlessly (--test-rotorspin) with no window.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3

#include <cstdint>
#include <vector>

namespace x3::game {

// A graybox quadcopter has 4 rotors. Spin rate for the demo (radians/sec); ~3.2
// rev/s reads clearly as a fast prop. VISUAL TUNING.
constexpr uint32_t kQuadRotorCount = 4;
constexpr float    kRotorDemoRps   = 20.0f;

// Build a column-major model matrix that spins a mesh authored at its LOCAL origin
// by `angle` radians about unit `axis`, then places it at world `hub`:
//   out = Translate(hub) · Rotate(axis, angle)
// `axis` is normalized internally (a zero axis falls back to +Y). Pure + testable.
void rotorSpinMatrix(const x3::phys::Vec3& hub, const x3::phys::Vec3& axis,
                     float angle, float out[16]);

// One spinning rotor: the Scene entity to drive, its hub (world pivot), spin axis
// (unit), angular velocity (rad/s; sign = direction), and accumulated angle.
struct Rotor {
    uint32_t       entity = kNoLink;
    x3::phys::Vec3 hub{};
    x3::phys::Vec3 axis{ 0.0f, 1.0f, 0.0f };
    float          radiansPerSec = kRotorDemoRps;
    float          angle         = 0.0f;   // accumulated, kept in [0, 2π)
};

// Procedural rotor-spin system. Owns a list of rotors; tick() advances each angle
// and rewrites its entity's transform. Content-agnostic: addRotor() attaches to any
// entity; buildGrayboxQuad() is a self-contained demo body + 4 rotors.
class RotorSpin {
public:
    // Build the graybox quadcopter demo at `origin`: a static box body + 4 blade
    // rotors on the arms (counter-rotating pairs, like a real quad). Returns the
    // body entity id. The rotor blade mesh is shared across the 4 rotor entities;
    // each rotor gets its own entity (its own transform). `physics` is unused (the
    // demo is purely visual) but kept for signature parity with the other systems.
    uint32_t buildGrayboxQuad(Scene& scene, x3::rhi::IRenderDevice& device,
                              const x3::phys::Vec3& origin);

    // Attach a procedural spin to an existing Scene entity (e.g. a real drone's
    // rotor node). `axis` need not be unit (normalized in tick). Returns its index.
    uint32_t addRotor(uint32_t entity, const x3::phys::Vec3& hub,
                      const x3::phys::Vec3& axis, float radiansPerSec);

    // Advance every rotor one frame: angle += rps·dt (wrapped to [0,2π)), then
    // rebuild the rotor entity's transform = Translate(hub)·Rotate(axis, angle).
    // No-op for a rotor whose entity is out of range.
    void tick(float dt, Scene& scene);

    // ---- Queries (host HUD + the self-test) -------------------------------
    uint32_t count() const { return (uint32_t)m_rotors.size(); }
    const Rotor& rotor(uint32_t i) const { return m_rotors[i]; }
    uint32_t bodyEntity() const { return m_body; }
    bool built() const { return m_body != kNoLink; }

private:
    std::vector<Rotor> m_rotors;
    uint32_t           m_body = kNoLink;   // graybox body entity (kNoLink until built)
};

// Headless self-test (--test-rotorspin). Builds the graybox quad on a HeadlessDevice
// + Scene and asserts: 4 counter-rotating rotors build (bodyless, visible, valid
// mesh) with angle 0 at load; rotorSpinMatrix is correct (axis point invariant, an
// off-axis point rotates by the expected angle, columns stay orthonormal); ticking
// accumulates each angle by rps·dt (counter-rotating signs progress oppositely); a
// full 2π revolution returns the transform to its start; and the result is
// deterministic. Prints "rotorspin: X/Y passed"; returns true iff all pass. No
// window / Vulkan.
bool runRotorSpinSelfTest();

} // namespace x3::game
