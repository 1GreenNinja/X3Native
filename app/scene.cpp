// Scene/entity layer implementation (S2). See app/scene.h.
#include "scene.h"

namespace x3::game {

uint32_t Scene::add(const Entity& e) {
    uint32_t id = (uint32_t)m_entities.size();
    m_entities.push_back(e);
    // Maintain the BodyId -> entity reverse map for rayCast hit resolution.
    // Skip invalid bodies (purely visual / static-no-collision entities).
    if (e.body.valid())
        m_bodyToEntity[e.body.id] = id;
    return id;
}

uint32_t Scene::entityForBody(x3::phys::BodyId body) const {
    if (!body.valid()) return kNoLink;
    auto it = m_bodyToEntity.find(body.id);
    return it == m_bodyToEntity.end() ? kNoLink : it->second;
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
        // Emissive-aware draw. The default emissive {0,0,0,0} makes this identical
        // to the old drawMesh() for every existing entity; club1127's neon/crystal
        // boxes set a non-zero emissive so they glow + feed the bloom chain.
        device.drawMeshEmissive(frame, e.mesh, e.tex, e.baseColor, e.emissive, e.transform);
    }
}

} // namespace x3::game
