#pragma once
// Minimal data-oriented scene/entity layer (S2).
//
// Game/slice code only — engine/ stays pure. A Scene is a flat array of
// Entities. Each Entity bundles its render resources (mesh + texture + tint),
// an optional physics body, and a column-major model transform.
//
// S3-S6 build on this: door/weapon/monster/button logic keys off Entity::tag,
// dynamic entities are driven by physics in update(), static entities keep
// their authored transform, and render() issues one drawMesh per visible entity.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Semantic tags for gameplay slices. S2 only uses None/Static/Prop; the rest
// are reserved so S4-S6 can find entities by role without a separate registry.
enum class Tag : uint32_t {
    None    = 0,
    Static  = 1,   // level geometry (floor/walls/step)
    Prop    = 2,   // dynamic visual prop (e.g. the falling box)
    Door    = 3,   // S4
    Button  = 4,   // S4
    Weapon  = 5,   // S5
    Monster = 6,   // S6
};

struct Entity {
    x3::rhi::MeshHandle    mesh;            // render mesh (invalid => not drawn)
    x3::rhi::TextureHandle tex;             // invalid => default white (flat color)
    float                  baseColor[4] = {1, 1, 1, 1}; // tint (multiplies texel)
    x3::phys::BodyId       body;            // invalid => purely visual / static
    float                  transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // column-major model
    bool                   visible = true;
    uint32_t               tag = (uint32_t)Tag::None;
};

class Scene {
public:
    // Append an entity; returns its stable id (== index into the flat vector).
    uint32_t add(const Entity& e);

    // Mutable access by id. Caller must pass a valid id (< size()).
    Entity&       get(uint32_t id);
    const Entity& get(uint32_t id) const;

    uint32_t size() const { return (uint32_t)m_entities.size(); }

    // Sync transforms FROM physics: for every entity with a valid body, rebuild
    // its model matrix from getBodyPosition() (translation). Entities without a
    // body keep their authored transform untouched.
    //
    // NOTE on orientation: IPhysicsWorld exposes position only (no rotation
    // getter), so update() preserves each dynamic entity's *authored* rotation +
    // scale (the upper-left 3x3 of its transform) and only overwrites the
    // translation column. For axis-aligned bodies (our falling box) this is
    // exact; a tumbling body would not show spin until the interface adds a
    // rotation getter. See the .cpp for details.
    void update(const x3::phys::IPhysicsWorld& physics);

    // Issue one drawMesh per visible entity with a valid mesh.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    std::vector<Entity>&       entities()       { return m_entities; }
    const std::vector<Entity>& entities() const { return m_entities; }

private:
    std::vector<Entity> m_entities;
};

} // namespace x3::game
