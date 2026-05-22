#pragma once
// Advanced elevator (core) — a cab platform that travels between floor stops and
// carries the player. Game/slice code only; engine/ stays pure.
// Full design + the player-carry approach: specs/ELEVATOR.spec.md.
//
// Mirrors DoorSystem's proven motion: the cab is a Static-layer body (mass 0)
// repositioned each frame via setBodyPosition() (so while still it blocks like
// ground), animated between an ordered list of stop heights. The host carries
// riders by adding the cab's per-frame vertical delta (returned by update()) to
// any body standing on the cab (see playerRiding()).

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

enum class ElevState : uint32_t { Idle = 0, Moving = 1 };

class ElevatorSystem {
public:
    // Build one cab at shaft XZ, sitting at stopsCenterY[startStop]. cabHalf* are
    // the platform half-extents (m); stopsCenterY = ordered cab-CENTER world Y per
    // stop (low -> high). Returns true on success. Call once.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float shaftX, float shaftZ, float cabHalfX, float cabHalfY, float cabHalfZ,
               const std::vector<float>& stopsCenterY, int startStop = 0);

    // Request travel to a stop index (clamped). No-op while already moving.
    void callTo(int stopIndex);
    // Cycle to the next stop, wrapping high->low. Simple "call" verb for the core.
    void callNext();

    // True if `feet` (player capsule reference point) is on the cab: XZ within the
    // footprint (+margin) and Y near the cab top. Generous window for ride detection.
    bool playerRiding(const x3::phys::Vec3& feet) const;

    // Advance the cab toward its target; returns the cab's vertical delta this frame
    // (0 when idle). The host adds this to every rider's Y to carry them.
    float update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

    bool  built() const { return m_built; }
    bool  moving() const { return m_state == ElevState::Moving; }
    int   targetStop() const { return m_target; }
    int   stopCount() const { return (int)m_stopsY.size(); }
    x3::phys::Vec3 cabCenter() const { return m_pos; }
    float cabTopY() const { return m_pos.y + m_halfY; }

    // Tuning (m/s). The ride feel is dialed in live (see spec §3).
    void  setSpeed(float s) { m_speed = s; }

private:
    bool     m_built = false;
    uint32_t m_entity = kNoLink;
    x3::phys::BodyId m_body;
    x3::phys::Vec3   m_pos{};                 // cab CENTER world pos
    float    m_halfX = 1.5f, m_halfY = 0.15f, m_halfZ = 1.5f;
    std::vector<float> m_stopsY;              // cab-center Y per stop (low -> high)
    int      m_target = 0;
    ElevState m_state = ElevState::Idle;
    float    m_speed = 3.0f;                  // m/s travel speed (tune live)
};

} // namespace x3::game
