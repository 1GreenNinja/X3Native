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
#include <unordered_set>
#include <vector>

namespace x3::game {

// Sentinel for Entity::link meaning "not linked to anything".
constexpr uint32_t kNoLink = 0xFFFFFFFFu;

// Sentinel for Entity::roomId meaning "no room — ALWAYS visible" (the default).
// Per-room occlusion culling (level_loader.*) tags every built room shell/prop with
// a real room id; everything else (sky, viewmodel, FX, the elevator shaft, legacy
// levels) keeps kNoRoom and is exempt from the cull, so render() is byte-identical to
// the old always-draw behaviour for every existing entity. See Scene::render +
// Scene::setVisibleRooms.
constexpr uint32_t kNoRoom = 0xFFFFFFFFu;

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
    // Optional per-entity HDR EMISSIVE term { r, g, b, strength } added on top of
    // the lit result (independent of light). Default {0,0,0,0} == no glow, so
    // Scene::render's drawMeshEmissive is identical to the old drawMesh for every
    // existing entity. strength > 1 makes the surface a bright bloom source — used
    // by club1127's neon strips / cave crystals. (See app/club1127.cpp.)
    float                  emissive[4] = {0, 0, 0, 0};
    x3::phys::BodyId       body;            // invalid => purely visual / static
    float                  transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // column-major model
    // Vertical offset (m) SUBTRACTED from the physics body Y when Scene::update syncs
    // the transform translation: lets a body sit at a raised CENTER (e.g. a monster's
    // generous feet..head hitbox is centered above the feet) while the rendered/queried
    // transform still anchors at the visual origin (the feet). Default 0 == no offset.
    float                  bodyVisualOffsetY = 0.0f;
    bool                   visible = true;
    // ---- Translucent GLASS material (transparent pass) --------------------
    // When `transparent` is set, Scene::render routes this entity through
    // device.drawMeshGlass instead of the opaque drawMeshEmissive: it renders in
    // the dedicated post-opaque, alpha-blended glass pass as real see-through glass
    // (design spec docs/superpowers/specs/2026-05-25-glass-material-design.md).
    // `glass` carries the per-instance material; `glass.opacity` is the primary
    // see-through dial (0 = clear, 1 = opaque) and overrides baseColor[3]. emissive
    // is still honored (holo glass keeps its glow). Default false == opaque, so every
    // existing entity renders exactly as before.
    bool                   transparent = false;
    x3::rhi::IRenderDevice::GlassMaterial glass{};
    uint32_t               tag = (uint32_t)Tag::None;
    // Generic gameplay linkage (S4): the entity id this one targets, or kNoLink.
    // A Button stores the entity id of the Door it opens. Chosen over a separate
    // side-table because the link is 1:1 and stays cache-local with the entity.
    uint32_t               link = kNoLink;
    // Per-room occlusion culling (level_loader.*): the id of the room this entity
    // belongs to, or kNoRoom == always-visible (the default — exempt from the cull).
    // When the Scene has a non-empty visible-room set (setVisibleRooms), render()
    // draws an entity ONLY if its roomId is kNoRoom OR is in that set. This is how
    // the data-driven level loader culls the 7-floor tower down to the player's
    // current room + its doorway-reachable neighbours (a simple portal PVS).
    uint32_t               roomId = kNoRoom;
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

    // ---- World-streaming support (region ownership ledgers, app/world_stream.*) ----
    // ENTITY CAPTURE: while a capture vector is installed, every add() also appends
    // the new entity's id into it. The world streamer brackets a region BUILDER call
    // with begin/end so the region's ownership ledger records exactly the entities
    // that builder created — without touching any builder's code. Nested captures are
    // not supported (the second begin overwrites the first). Pass nullptr to disable.
    void beginEntityCapture(std::vector<uint32_t>* out) { m_capture = out; }
    void endEntityCapture() { m_capture = nullptr; }

    // SLOT RELEASE + REUSE: releaseSlot() empties slot `id` (no mesh/body/draw; the
    // generation is bumped via recycle() so stale handles die) and parks it on an
    // internal free-list; the NEXT add() pops a freed slot and recycles INTO it
    // instead of growing the entity vector. This is what keeps Scene::size() (and the
    // per-entity allocation footprint) CONSTANT across region stream-out/in cycles.
    // Purely additive: until the first releaseSlot() the free-list is empty and add()
    // appends exactly as before. The caller must own the slot (region ledger) and
    // must already have destroyed the slot's GPU/physics resources.
    void releaseSlot(uint32_t id);
    uint32_t freeSlotCount() const { return (uint32_t)m_freeSlots.size(); }

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

    // Issue one drawMeshEmissive per visible entity with a valid mesh — gated by the
    // per-room occlusion cull (see setVisibleRooms). An entity is drawn iff
    // e.visible && e.mesh.valid() && roomVisible(e.roomId). With the room cull
    // disabled (the default — no visible-room set yet), roomVisible() is always true
    // and this is identical to the legacy "draw every visible entity" behaviour.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // ---- Per-room occlusion culling (data-driven level loader, PVS) ----------
    // The host updates the VISIBLE-ROOM SET once per frame from the camera position
    // (the level loader's PVS: current room + doorway-reachable neighbours). render()
    // then skips entities whose roomId is not in this set. Entities tagged kNoRoom
    // (sky / viewmodel / FX / legacy levels) are ALWAYS drawn (exempt from the cull).
    //
    // The cull is OPT-IN: until the first setVisibleRooms() with a non-empty set, the
    // cull is INACTIVE and every visible entity draws (so existing levels/tests are
    // unaffected). setRoomCullEnabled(false) hard-disables it (debug / always draw all).
    void setVisibleRooms(const uint32_t* rooms, uint32_t count);
    void setVisibleRooms(const std::vector<uint32_t>& rooms) {
        setVisibleRooms(rooms.data(), (uint32_t)rooms.size());
    }
    // Clear the visible set => cull inactive again (everything draws).
    void clearVisibleRooms() { m_visibleRooms.clear(); m_roomCullActive = false; }
    // Master switch: when false the room cull never culls (everything draws) even with
    // a visible set present. Default true.
    void setRoomCullEnabled(bool on) { m_roomCullEnabled = on; }
    bool roomCullEnabled() const { return m_roomCullEnabled; }
    // True iff a draw of an entity tagged `roomId` would happen under the current cull
    // state (kNoRoom always passes; cull inactive => everything passes). Pure query —
    // used by render() and by --test-canonlevel to count the drawn set without a GPU.
    bool roomVisible(uint32_t roomId) const {
        if (roomId == kNoRoom) return true;
        if (!m_roomCullEnabled || !m_roomCullActive) return true;
        return m_visibleRooms.count(roomId) != 0;
    }
    // Count how many of this scene's entities would be DRAWN under the current cull
    // state (visible + valid mesh + roomVisible). Diagnostics / the self-test proof.
    uint32_t drawnCount() const;

    std::vector<Entity>&       entities()       { return m_entities; }
    const std::vector<Entity>& entities() const { return m_entities; }

private:
    std::vector<Entity> m_entities;
    // Per-room occlusion cull state. m_roomCullActive flips true the first time a
    // non-empty visible set is installed; cleared by clearVisibleRooms(). Stored as a
    // set for O(1) membership in render()'s hot loop.
    std::unordered_set<uint32_t> m_visibleRooms;
    bool m_roomCullEnabled = true;
    bool m_roomCullActive  = false;
    // World-streaming support: optional add() capture sink + the released-slot
    // free-list add() reuses (see beginEntityCapture / releaseSlot above).
    std::vector<uint32_t>* m_capture = nullptr;
    std::vector<uint32_t>  m_freeSlots;
    // Per-slot generation counter, parallel to m_entities (1:1 by index). A fresh
    // slot starts at generation 1 (so a minted handle is never the invalid gen 0);
    // recycle() bumps it. Additive — untouched by the legacy uint32_t-id path.
    std::vector<uint32_t> m_generation;
    // BodyId.id -> entity id, for rayCast-hit -> entity resolution. Built in add().
    std::unordered_map<uint32_t, uint32_t> m_bodyToEntity;
};

} // namespace x3::game
