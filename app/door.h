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
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace x3::game {

// Door animation state machine. A single progress cursor `t` in [0, duration]
// drives the lerp closed(t=0) -> open(t=duration): Opening increases t, Closing
// decreases it, so a toggle mid-slide reverses seamlessly without a pop.
enum class DoorState : uint32_t {
    Closed  = 0,
    Opening = 1,
    Open    = 2,
    Closing = 3,
};

// One animated door. `entity` indexes the Scene; `body` is its physics box.
// closedPos/openPos are the body-center world positions at the two extremes;
// the door lerps between them over `duration` seconds while Opening.
//
// Lockable / open-on-event (Level 1 / §6.4): a `locked` door refuses to open via
// startOpening() (and therefore via tryUse() on its button) until unlock() is
// called. This models Door C (locked until the player is armed) and Door E
// (opens only on Martinez's death) — game state, not a wall button. unlock()
// alone does not open the door; the host calls startOpening() after unlocking
// (or uses unlockAndOpen()).
struct Door {
    uint32_t          entity   = kNoLink;
    x3::phys::BodyId  body;
    x3::phys::Vec3    closedPos{};
    x3::phys::Vec3    openPos{};
    float             duration = 1.0f;   // seconds Closed -> Open
    float             t        = 0.0f;   // animation cursor [0..duration]
    DoorState         state    = DoorState::Closed;
    bool              locked   = false;  // §6.4: refuse to open until unlock()
    int               code     = 0;      // keypad code (0 = no keypad); a LOCKED door with
                                         // code != 0 opens when the matching code is entered
    // ---- Visual GLB placement (the real SM_Door_A slab is drawn over the
    // collision-only box). These let drawMeshes() orient + size the shared GLB to
    // fit this door's opening and follow the slide animation. ----
    uint32_t          axis     = 0;      // 0 = AlongZ (thin in X), 1 = AlongX (thin in Z)
    float             halfWidth = 0.6f;  // doorway half-width (== DoorSpec::halfWidth)
    float             height    = 2.1f;  // door slab height (== DoorSpec::height)
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

    // Begin opening a door (no-op if already Opening/Open, OR if the door is
    // locked). Returns true if a transition Closed -> Opening happened.
    bool startOpening(Door& d) const;

    // Toggle a door open <-> closed — the E "use" path (Tim: "closing is
    // important"). Closed -> Opening (unless locked), Open -> Closing, and a
    // mid-slide Opening <-> Closing reversal (the shared `t` cursor makes the
    // reverse seamless). Returns true if a transition happened (false only for a
    // locked, fully-Closed door).
    bool toggle(Door& d) const;

    // Clear a door's locked flag (does NOT open it). After unlock() the door can
    // be opened by startOpening()/its button. Idempotent.
    void unlock(Door& d) const { d.locked = false; }

    // Unlock + immediately start opening (the "open on event" path). Used by the
    // host for Door E on Martinez's death. Returns true if the door began opening.
    bool unlockAndOpen(Door& d) const { unlock(d); return startOpening(d); }

    // Advance every animating door one frame: move the `t` cursor (Opening +dt,
    // Closing -dt), lerp the body between closedPos/openPos via setBodyPosition(),
    // then sync the owning Entity's transform translation. Settles to Open at
    // t>=duration and Closed at t<=0.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Load the shared real-door GLB (SM_Door_A) ONCE from `convertedGlbDir` (the
    // loose-GLB root, e.g. "G:/GameModels/converted_glb"), bound to `device`. Safe
    // to call repeatedly (idempotent). On failure the door keeps its (now hidden)
    // procedural box look fallback — drawMeshes() simply draws nothing. Called by
    // buildLevelDoor() so the visual swap happens automatically.
    void loadDoorMesh(x3::rhi::IRenderDevice& device, std::string_view convertedGlbDir);

    // Draw the shared real door GLB at EVERY door's CURRENT world transform (so the
    // mesh follows the slide animation), oriented + scaled to fit each door's
    // opening. No-op if the GLB failed to load. Mirrors EnvArtSystem::draw().
    void drawMeshes(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // True once the shared door GLB has loaded ok (a real mesh will be drawn).
    bool hasDoorMesh() const { return m_meshOk; }

private:
    std::vector<Door> m_doors;

    // ---- Shared real-door GLB (loaded once; reused for every door). Owns the
    // asset source + loader so the GPU handles stay valid for the app lifetime,
    // exactly like EnvArtSystem. ----
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_doorModel;
    std::vector<x3::asset::ModelDrawable>    m_doorDrawables;
    bool m_meshOk = false;
};

// Resolve which door (if any) a use-ray is aiming at: raycast from `eye` along
// `dir` (normalized internally) up to `maxDist`; if it hits the door slab itself
// OR a Button linked to a door, return that Door (else nullptr). Shared by
// tryUse() (to toggle) and the HUD interaction prompt (to know what/whether to
// show). Pure query — does not change door state.
Door* pickAimedDoor(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
                    Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics);

// Handle a "use" press: pickAimedDoor() then TOGGLE it (open if closed, close if
// open) — closing matters as much as opening. Returns true if a door changed
// state. Aiming at not-a-door / out of range, or at a locked closed door, does
// nothing.
bool tryUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
            Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics);

// Build a sliding door (filling the S2 doorway gap) + a wall button linked to
// it, registering render meshes/textures via `device`, a static collision body
// via `physics`, and Entities in `scene`. Records the door in `doors`. Returns
// the button's entity id (the door's entity id is doors.at(last).entity).
uint32_t buildDoorAndButton(Scene& scene, DoorSystem& doors,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics);

// Which axis the door's host wall runs along — determines the door slab's thin
// axis and the side a button mounts on.
enum class DoorAxis : uint32_t {
    AlongZ = 0,  // wall runs along Z (its plane is X = const); door is thin in X
    AlongX = 1,  // wall runs along X (its plane is Z = const); door is thin in Z
};

// Parameters for a single Level-1 door (generalized buildDoorAndButton). The door
// is a portcullis slab that fills a doorway gap in a wall and slides UP to open.
struct DoorSpec {
    x3::phys::Vec3 doorwayCenter{};        // world center of the doorway opening at floor level
    DoorAxis       axis     = DoorAxis::AlongX;
    float          halfWidth = 0.6f;       // half the doorway opening width (along the wall run)
    float          height    = 2.1f;       // door slab height (passage clear height)
    float          thickness = 0.12f;      // door slab thickness — THINNER than the 0.2 wall so the
                                           // slab slides up hidden INSIDE the wall (no z-fight shimmer)
    float          duration  = 1.0f;       // seconds Closed -> Open
    bool           locked    = false;      // §6.4 lockable
    int            code      = 0;          // keypad code (0 = none); locked + code => keypad door
    bool           withButton = true;      // place a linked wall button beside it
    float          tint[4]   = { 0.85f, 0.30f, 0.18f, 1.0f };  // door slab color
};

// Build a generalized door (+ optional linked button) per `spec`, registering
// render meshes via `device`, a static collision body via `physics`, and Entities
// in `scene`. Records the Door in `doors`. Returns the door's index in `doors`
// (NOT an entity id). The button (if any) is linked to the door entity, so tryUse
// opens it exactly like the original buildDoorAndButton.
uint32_t buildLevelDoor(Scene& scene, DoorSystem& doors,
                        x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        const DoorSpec& spec);

// Headless self-test (--test-interact). Builds a minimal room + door + button +
// a body, asserts T1-T4, logs PASS/FAIL T#, returns true iff all pass. No
// window/Vulkan: render meshes are created on a headless device stub. Mirrors
// runPhysicsSelfTest()/runPlayerSelfTest().
bool runInteractSelfTest();

} // namespace x3::game
