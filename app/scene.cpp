// Scene/entity layer implementation (S2). See app/scene.h.
#include "scene.h"

namespace x3::game {

uint32_t Scene::add(const Entity& e) {
    uint32_t id;
    if (!m_freeSlots.empty()) {
        // World-streaming slot reuse: recycle INTO a released slot instead of growing
        // the vector (recycle() bumps the generation + fixes the body map), so the
        // scene's size stays constant across region stream-out/in cycles.
        id = m_freeSlots.back();
        m_freeSlots.pop_back();
        recycle(id, e);
    } else {
        id = (uint32_t)m_entities.size();
        m_entities.push_back(e);
        // Parallel generation counter: a fresh slot starts at generation 1 so a handle
        // minted for it is never the invalid generation 0 (netcode Phase 0, §4.1).
        m_generation.push_back(1);
        // Maintain the BodyId -> entity reverse map for rayCast hit resolution.
        // Skip invalid bodies (purely visual / static-no-collision entities).
        if (e.body.valid())
            m_bodyToEntity[e.body.id] = id;
    }
    if (m_capture) m_capture->push_back(id);
    return id;
}

void Scene::releaseSlot(uint32_t id) {
    if (id >= m_entities.size()) return;
    // Empty the slot via recycle() so the generation bumps (stale handles die) and
    // the old body unmaps. The empty entity has no mesh/body and is invisible, so a
    // parked slot costs nothing in update()/render().
    Entity empty{};
    empty.visible = false;
    recycle(id, empty);
    m_freeSlots.push_back(id);
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
        e.transform[13] = p.y - e.bodyVisualOffsetY;  // raised-hitbox body -> visual origin
        e.transform[14] = p.z;
        e.transform[15] = 1.0f;
    }
}

void Scene::setVisibleRooms(const uint32_t* rooms, uint32_t count) {
    m_visibleRooms.clear();
    for (uint32_t i = 0; i < count; ++i) m_visibleRooms.insert(rooms[i]);
    // The cull only becomes ACTIVE once a non-empty set is installed: an empty set
    // would otherwise hide the whole data-driven level (every room culled), which is
    // never the intent. Empty set => cull stays inactive (everything draws).
    m_roomCullActive = (count > 0);
}

uint32_t Scene::drawnCount() const {
    uint32_t n = 0;
    for (const Entity& e : m_entities)
        if (e.visible && e.mesh.valid() && roomVisible(e.roomId)) ++n;
    return n;
}

void Scene::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    m_lastRoomCulled = 0;
    for (const Entity& e : m_entities) {
        if (!e.visible || !e.mesh.valid())
            continue;
        // Per-room occlusion cull (data-driven level loader): skip entities whose room
        // is not in the current visible set. roomVisible() returns true for kNoRoom and
        // whenever the cull is inactive, so this is a no-op for every existing entity.
        // Skips are COUNTED (lastRoomCulled) for the unified vis stats block: they are
        // the "rooms" stage of the conserving rooms -> frustum -> hzb -> drawn chain.
        if (!roomVisible(e.roomId)) {
            ++m_lastRoomCulled;
            continue;
        }
        // Translucent glass entities route through the dedicated transparent pass (real
        // see-through glass), keeping their emissive glow; everything else is opaque. The
        // default emissive {0,0,0,0} makes the opaque path identical to the old drawMesh()
        // for every existing entity; club1127's neon/crystal
        // boxes set a non-zero emissive so they glow + feed the bloom chain.
        if (e.transparent) {
            device.drawMeshGlass(frame, e.mesh, e.tex, e.baseColor, e.emissive, e.glass, e.transform);
        } else if (e.emissiveTex.valid()) {
            // Entity carries an EMISSIVE map (holo-terminal / HoloPanel screen pane):
            // route through drawMeshPBR so the per-object emissive is gated by the map
            // (glowing text/line-art on a near-black pane). No normal/MR maps.
            device.drawMeshPBR(frame, e.mesh, e.tex, x3::rhi::TextureHandle{}, e.mrTex,
                               e.baseColor, e.emissive, e.transform,
                               /*alphaMask*/false, /*alphaBlend*/false, e.emissiveTex);
        } else if (e.mrTex.valid()) {
            // Entity carries a metallic-roughness map: full PBR path (Cook-Torrance
            // + IBL/SSR reflections). No normal map (geometry normal is used).
            device.drawMeshPBR(frame, e.mesh, e.tex, x3::rhi::TextureHandle{}, e.mrTex,
                               e.baseColor, e.emissive, e.transform);
        } else {
            device.drawMeshEmissive(frame, e.mesh, e.tex, e.baseColor, e.emissive, e.transform);
        }
    }
}

} // namespace x3::game
