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
#include <unordered_map>
#include <vector>

namespace x3::game {

// Sentinel for Entity::link meaning "not linked to anything".
constexpr uint32_t kNoLink = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Generation-counter handle (netcode Phase 0; spec NETCODE-architecture §4.1).
//
// A bare entity id is an array INDEX, and indices are RECYCLED (TerrainStreamer
// reuses evicted tile slots via scene.get(id) = e). A uint32_t index held across
// a stream-out/in can therefore silently alias a DIFFERENT entity — the latent
// "index-recycling" defect. SceneHandle fixes it the netcode-correct way: a handle
// pairs the slot index with the slot's GENERATION at the time the handle was
// minted. Recycling a slot bumps its generation (Scene::recycle), so a stale
// handle (carrying the OLD generation) is detectable and rejected by valid()/
// getChecked(). This also gives a stable identity for the net entity layer.
//
// This is ADDITIVE: existing uint32_t-id call sites (level1/monster/terrain/etc.)
// keep working unchanged via add()/get(uint32_t). New code that needs to survive
// slot recycling uses handle()/getChecked(). generation 0 == invalid handle.
// ---------------------------------------------------------------------------
struct SceneHandle {
    uint32_t index      = 0xFFFFFFFFu;  // slot index into the flat vector
    uint32_t generation = 0;            // 0 == invalid; matched against the slot's gen
    bool valid() const { return generation != 0 && index != 0xFFFFFFFFu; }
    bool operator==(const SceneHandle& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const SceneHandle& o) const { return !(*this == o); }
};

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
    // Generic gameplay linkage (S4): the entity id this one targets, or kNoLink.
    // A Button stores the entity id of the Door it opens. Chosen over a separate
    // side-table because the link is 1:1 and stays cache-local with the entity.
    uint32_t               link = kNoLink;
};

class Scene {
public:
    // Append an entity; returns its stable id (== index into the flat vector).
    uint32_t add(const Entity& e);

    // Mutable access by id. Caller must pass a valid id (< size()).
    Entity&       get(uint32_t id);
    const Entity& get(uint32_t id) const;

    uint32_t size() const { return (uint32_t)m_entities.size(); }

    // ---- Generation-checked handle API (netcode Phase 0; spec §4.1) --------
    // Mint a CURRENT handle for an existing slot (its index + the slot's live
    // generation). Use this to remember an entity that may outlive a stream-out/in
    // — the handle detects if the slot was recycled out from under it.
    SceneHandle handle(uint32_t index) const;

    // True iff `h` still refers to the entity it was minted for (index in range AND
    // the slot's generation still matches). A recycled slot fails this.
    bool valid(SceneHandle h) const;

    // Generation-checked access: returns &entity iff valid(h), else nullptr. This
    // is the safe path for any reference held across possible slot recycling.
    Entity*       getChecked(SceneHandle h);
    const Entity* getChecked(SceneHandle h) const;

    // Scene-owned slot recycle: overwrite slot `index` with `e` AND bump its
    // generation so any SceneHandle minted before this call becomes stale. The
    // BodyId->entity reverse map is updated. Returns a fresh handle to the reused
    // slot. This is the recycle path the TerrainStreamer uses so the generation
    // counter actually advances on reuse (without it, the fix would be inert for
    // the real bug path). Caller must pass a valid index (< size()).
    SceneHandle recycle(uint32_t index, const Entity& e);

    // Current generation of a slot (diagnostics / tests). 0 if out of range.
    uint32_t generationOf(uint32_t index) const;

    // Resolve a physics BodyId (e.g. from a rayCast hit) back to the entity id
    // that owns it. Returns kNoLink if no entity has that body. The map is
    // maintained by add(); bodies with id 0 (invalid) are skipped.
    uint32_t entityForBody(x3::phys::BodyId body) const;

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
    // Per-slot generation counter, parallel to m_entities (1:1 by index). A fresh
    // slot starts at generation 1 (so a minted handle is never the invalid gen 0);
    // recycle() bumps it. Additive — untouched by the legacy uint32_t-id path.
    std::vector<uint32_t> m_generation;
    // BodyId.id -> entity id, for rayCast-hit -> entity resolution. Built in add().
    std::unordered_map<uint32_t, uint32_t> m_bodyToEntity;
};

} // namespace x3::game
