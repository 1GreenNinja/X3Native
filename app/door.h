#pragma once
// Button -> door interaction (S4).
//
// Game/slice code only — engine/ stays pure. Builds on S2 (Scene/Entity/Tag),
// S3 (Player camera forward), and the M3 physics interface (rayCast +
// setBodyPosition). A Button entity is linked (via Entity::link) to a Door
// entity; pressing "use" while aiming at the button starts the door sliding
// open. The door is a procedural box that fills the S2 doorway gap when closed.
//
// Door body motion: the door box is created as a Layer::Static body (mass 0).
// That keeps it in a non-moving broadphase slot the Player capsule collides
// with (Player<->Static collide in the matrix), so while CLOSED it blocks the
// character like a wall. We then animate it purely by setBodyPosition() — which
// calls Jolt SetPosition(...Activate) and works on any body, character or not.
// This is a teleport (no swept contact), which is fine because the door slides
// AWAY from the player as it opens; it never needs to push the character.
//
// Slide direction: UP (portcullis / garage-door). The door rises by its full
// height so the passage clears. Cosmetic caveat: a fully-open door pokes a
// little above the 3 m wall (graybox-acceptable; see slice summary).

#include "scene.h"

#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Door animation state machine. Open is terminal (closing is out of scope for S4).
enum class DoorState : uint32_t {
    Closed  = 0,
    Opening = 1,
    Open    = 2,
};

// One animated door. `entity` indexes the Scene; `body` is its physics box.
// closedPos/openPos are the body-center world positions at the two extremes;
// the door lerps between them over `duration` seconds while Opening.
struct Door {
    uint32_t          entity   = kNoLink;
    x3::phys::BodyId  body;
    x3::phys::Vec3    closedPos{};
    x3::phys::Vec3    openPos{};
    float             duration = 1.0f;   // seconds Closed -> Open
    float             t        = 0.0f;   // animation cursor [0..duration]
    DoorState         state    = DoorState::Closed;
};

// Registry of doors in a level. Kept in the game layer (not the Scene) so the
// Scene stays a pure render/physics container. Indexed lookups by door entity id.
class DoorSystem {
public:
    // Register a door (built elsewhere via buildDoorAndButton). Returns its index.
    uint32_t add(const Door& d);

    uint32_t count() const { return (uint32_t)m_doors.size(); }
    Door&       at(uint32_t i)       { return m_doors[i]; }
    const Door& at(uint32_t i) const { return m_doors[i]; }

    // Find the door whose `entity` == entityId, or nullptr if none.
    Door*       findByEntity(uint32_t entityId);
    const Door* findByEntity(uint32_t entityId) const;

    // Begin opening a door (no-op if already Opening/Open). Returns true if a
    // transition Closed -> Opening happened.
    bool startOpening(Door& d) const;

    // Advance every animating door one frame: lerp the body toward openPos via
    // setBodyPosition(), then sync the owning Entity's transform translation.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

private:
    std::vector<Door> m_doors;
};

// Handle a "use" press: raycast from the eye along `dir` (need not be unit;
// normalized internally) up to `maxDist`. If the hit body resolves to a Button
// entity that links to a door, start that door Opening. Returns true if a door
// began opening. Aiming at not-a-button / out of range does nothing.
bool tryUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
            Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics);

// Build a sliding door (filling the S2 doorway gap) + a wall button linked to
// it, registering render meshes/textures via `device`, a static collision body
// via `physics`, and Entities in `scene`. Records the door in `doors`. Returns
// the button's entity id (the door's entity id is doors.at(last).entity).
uint32_t buildDoorAndButton(Scene& scene, DoorSystem& doors,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics);

// Headless self-test (--test-interact). Builds a minimal room + door + button +
// a body, asserts T1-T4, logs PASS/FAIL T#, returns true iff all pass. No
// window/Vulkan: render meshes are created on a headless device stub. Mirrors
// runPhysicsSelfTest()/runPlayerSelfTest().
bool runInteractSelfTest();

} // namespace x3::game
