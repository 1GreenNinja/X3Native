// Scene/entity layer implementation (S2). See app/scene.h.
#include "scene.h"

namespace x3::game {

uint32_t Scene::add(const Entity& e) {
    uint32_t id = (uint32_t)m_entities.size();
    m_entities.push_back(e);
    // Parallel generation counter: a fresh slot starts at generation 1 so a handle
    // minted for it is never the invalid generation 0 (netcode Phase 0, §4.1).
    m_generation.push_back(1);
    // Maintain the BodyId -> entity reverse map for rayCast hit resolution.
    // Skip invalid bodies (purely visual / static-no-collision entities).
    if (e.body.valid())
        m_bodyToEntity[e.body.id] = id;
    return id;
}

SceneHandle Scene::handle(uint32_t index) const {
    if (index >= m_generation.size()) return SceneHandle{};   // invalid
    return SceneHandle{ index, m_generation[index] };
}

uint32_t Scene::generationOf(uint32_t index) const {
    return index < m_generation.size() ? m_generation[index] : 0u;
}

bool Scene::valid(SceneHandle h) const {
    return h.valid() && h.index < m_generation.size() &&
           m_generation[h.index] == h.generation;
}

Entity* Scene::getChecked(SceneHandle h) {
    return valid(h) ? &m_entities[h.index] : nullptr;
}

const Entity* Scene::getChecked(SceneHandle h) const {
    return valid(h) ? &m_entities[h.index] : nullptr;
}

SceneHandle Scene::recycle(uint32_t index, const Entity& e) {
    // Bump the generation FIRST so any handle minted before this call is now stale.
    // (m_generation is 1:1 with m_entities; index is asserted valid by the caller.)
    uint32_t& gen = m_generation[index];
    gen = gen + 1;
    if (gen == 0) gen = 1;   // skip the invalid generation 0 on the (huge) wrap

    // Drop the OLD body mapping for this slot (if any) so a stale BodyId no longer
    // resolves to the recycled slot, then install the new one.
    const Entity& old = m_entities[index];
    if (old.body.valid()) {
        auto it = m_bodyToEntity.find(old.body.id);
        if (it != m_bodyToEntity.end() && it->second == index)
            m_bodyToEntity.erase(it);
    }
    m_entities[index] = e;
    if (e.body.valid())
        m_bodyToEntity[e.body.id] = index;

    return SceneHandle{ index, gen };
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
        device.drawMesh(frame, e.mesh, e.tex, e.baseColor, e.transform);
    }
}

} // namespace x3::game
