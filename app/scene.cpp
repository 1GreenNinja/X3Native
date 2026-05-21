// Scene/entity layer implementation (S2). See app/scene.h.
#include "scene.h"

namespace x3::game {

uint32_t Scene::add(const Entity& e) {
    uint32_t id = (uint32_t)m_entities.size();
    m_entities.push_back(e);
    return id;
}

Entity& Scene::get(uint32_t id) {
    return m_entities[id];
}

const Entity& Scene::get(uint32_t id) const {
    return m_entities[id];
}

void Scene::update(const x3::phys::IPhysicsWorld& physics) {
    for (Entity& e : m_entities) {
        if (!e.body.valid())
            continue;
        // Position-only sync: overwrite the translation column (12..14), keep
        // the authored upper-left 3x3 (rotation + scale). The physics interface
        // has no rotation getter, so dynamic bodies retain their authored
        // orientation. For our axis-aligned falling box this matches exactly.
        x3::phys::Vec3 p = physics.getBodyPosition(e.body);
        e.transform[12] = p.x;
        e.transform[13] = p.y;
        e.transform[14] = p.z;
        e.transform[15] = 1.0f;
    }
}

void Scene::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    for (const Entity& e : m_entities) {
        if (!e.visible || !e.mesh.valid())
            continue;
        device.drawMesh(frame, e.mesh, e.tex, e.baseColor, e.transform);
    }
}

} // namespace x3::game
