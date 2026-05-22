// Advanced elevator (core). See app/elevator.h + specs/ELEVATOR.spec.md.
// Clean-room: built from the IPhysicsWorld + Scene + IRenderDevice interfaces only,
// mirroring DoorSystem's moved-static-body technique.
#include "elevator.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

bool ElevatorSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           float shaftX, float shaftZ,
                           float cabHalfX, float cabHalfY, float cabHalfZ,
                           const std::vector<float>& stopsCenterY, int startStop) {
    if (stopsCenterY.empty()) {
        x3::logError("[elevator] build: no stops");
        return false;
    }
    m_halfX = cabHalfX; m_halfY = cabHalfY; m_halfZ = cabHalfZ;
    m_stopsY = stopsCenterY;
    std::sort(m_stopsY.begin(), m_stopsY.end());     // low -> high
    m_target = std::clamp(startStop, 0, (int)m_stopsY.size() - 1);
    m_pos = x3::phys::Vec3{ shaftX, m_stopsY[m_target], shaftZ };
    m_state = ElevState::Idle;

    // Render mesh authored centered at the body origin (the Entity transform
    // translation drives world placement as the cab moves), like a door slab.
    x3::prims::PrimMesh geo = x3::prims::makeBox(m_halfX, m_halfY, m_halfZ, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Industrial cab platform: brushed metal.
    e.baseColor[0] = 0.40f; e.baseColor[1] = 0.42f; e.baseColor[2] = 0.46f; e.baseColor[3] = 1.0f;
    // Static body (mass 0): blocks/standable while still, repositioned via
    // setBodyPosition while Moving. Half-extents == render extents.
    e.body = physics.addBox(x3::phys::Vec3{ m_halfX, m_halfY, m_halfZ }, m_pos, 0.0f,
                            x3::phys::Layer::Static);
    e.transform[12] = m_pos.x;
    e.transform[13] = m_pos.y;
    e.transform[14] = m_pos.z;
    m_entity = scene.add(e);
    m_body = scene.get(m_entity).body;
    m_built = true;

    x3::logInfo("[elevator] built: " + std::to_string(m_stopsY.size()) + " stops at (" +
                std::to_string(shaftX) + ", " + std::to_string(shaftZ) + "), start stop " +
                std::to_string(m_target));
    return true;
}

void ElevatorSystem::callTo(int stopIndex) {
    if (!m_built || m_state == ElevState::Moving) return;
    int t = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
    if (t == m_target) return;
    m_target = t;
    m_state = ElevState::Moving;
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

void ElevatorSystem::callNext() {
    if (!m_built || m_state == ElevState::Moving || m_stopsY.size() < 2) return;
    m_target = (m_target + 1) % (int)m_stopsY.size();
    m_state = ElevState::Moving;
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

bool ElevatorSystem::playerRiding(const x3::phys::Vec3& feet) const {
    if (!m_built) return false;
    const float dx = feet.x - m_pos.x;
    const float dz = feet.z - m_pos.z;
    if (std::fabs(dx) > m_halfX + 0.35f) return false;
    if (std::fabs(dz) > m_halfZ + 0.35f) return false;
    // Y window: feet near/just above the cab top (generous for ride detection).
    const float top = m_pos.y + m_halfY;
    return feet.y >= top - 0.5f && feet.y <= top + 2.5f;
}

float ElevatorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || m_state != ElevState::Moving || dt <= 0.0f) return 0.0f;

    const float target = m_stopsY[m_target];
    const float dy = target - m_pos.y;
    const float step = m_speed * dt;
    float moved;
    if (std::fabs(dy) <= step) {            // arrive: snap to the stop
        moved = dy;
        m_pos.y = target;
        m_state = ElevState::Idle;
        x3::logInfo("[elevator] arrived at stop " + std::to_string(m_target));
    } else {
        moved = (dy > 0.0f) ? step : -step;
        m_pos.y += moved;
    }
    physics.setBodyPosition(m_body, m_pos);
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& e = scene.get(m_entity);
        e.transform[13] = m_pos.y;          // sync render transform to the body
    }
    return moved;
}

} // namespace x3::game
