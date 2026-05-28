#pragma once
// NPCSystem — non-combatant world NPCs (bartender Danny, captive Aria/Keisha/Emily
// before rescue, DockWorker, Mechanic, DrJohnson, civilians). The game already has:
//   * app/player.{h,cpp}   — first-person player (input + physics capsule + camera)
//   * app/monster.{h,cpp}  — combat enemy (chase / attack / HP / ragdoll)
//   * app/rescue.{h,cpp}   — F2 rescue victims (captive -> companion-follow on E)
// NPCs are NONE of those: no PlayerInput, no combat AI / HP, no squad/reflex brain.
// They stand at a bar (fixed pose), patrol a closed waypoint loop, look at the
// player when in range, or are captive (rescuable on E like the F2 victims, but
// generalized so the floor module doesn't have to know which captive subsystem
// owns them).
//
// Clean-room: built from Scene / IRenderDevice / IPhysicsWorld / IModelLoader /
// IAssetSource only — the same interfaces MonsterSystem / RescueVictim use. No
// purchased C# / id Tech / RBDOOM source consulted.
//
// Physics: an INERT KINEMATIC capsule (Jolt static-by-mass on Layer::Enemy). NPCs
// don't fall, don't get pushed by combat, but block player movement — the
// monster pattern (mass 0 -> Static motion, movable by setBodyPosition, same
// teleport trick the S4 door + monster chase use). DOES NOT take damage; the
// only way to remove an NPC from the world is shutdownRagdoll() / markRescued().
//
// Locomotion: Patrol walks a closed waypoint loop at walkSpeed (Lerp toward the
// next waypoint; advance when within ~0.5 m; loop). LookAtPlayer mode bakes a
// yaw toward the player into the entity transform when the player enters
// lookAtRange. Captive mode is a special render variant (slightly desaturated
// tint) + cannot patrol/look; markRescued() flips it to Idle and raises the
// "rescued" flag so the host knows to give the player a companion.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// NPC mode (drives behaviour + render variant). The host picks the initial mode
// via Tuning::initialMode; setPatrol() flips to Patrol, setFixedPose() to Idle,
// and the player approaching within lookAtRange flips to LookAtPlayer (and back
// to the prior mode when the player leaves).
class NPCSystem {
public:
    enum class Mode : uint32_t {
        Idle = 0,          // stand at spawn, face default yaw
        Patrol = 1,        // walk a closed waypoint loop
        LookAtPlayer = 2,  // face the player (set automatically on player approach)
        Interact = 3,      // talking to the player (host-driven; halt movement, face player)
        Captive = 4,       // rescuable on E (markRescued -> Idle + wasRescued() true)
    };

    // Spawn / model / behaviour parameters. modelFile + modelDirOverride mirror
    // MonsterSystem::Tuning so the same GLB pipeline is reused; the rest are
    // NPC-only knobs.
    struct Tuning {
        // GLB filename to load (e.g. "BartenderDanny.glb"). Empty => fallback box.
        std::string modelFile;
        // Loose-dir to load modelFile from. Empty => fall back to the modelDir
        // passed to build(). Typically riggedGlbRoot().
        std::string modelDirOverride;
        // Uniform model scale (humanoid rigged GLBs read ~1 at scale 1.0).
        float       modelScale = 1.0f;
        // The converted character GLBs are authored Z-up (lying flat); set true
        // to rotate -90deg about X so they stand upright in our Y-up world. The
        // rigged_glb humanoids are already Y-up — leave false for them.
        bool        standUpZtoY = false;
        // Default look-at trigger range (meters). When the player crosses inside
        // this radius (planar), mode flips to LookAtPlayer; outside, back to the
        // base mode (Idle / Patrol). Setter overrides this at runtime.
        float       lookAtRange = 4.0f;
        // Initial mode set at build time (host typically uses Idle / Patrol /
        // Captive; LookAtPlayer is auto-driven and Interact is host-poked).
        Mode        initialMode = Mode::Idle;
    };

    // Per-NPC interact callback (Slice C/D dialogue + rescue hooks). Empty => no-op.
    using InteractFn = std::function<void(NPCSystem&)>;

    // ---- Build / update / draw -------------------------------------------------
    // Build the NPC: load `t.modelFile` from `t.modelDirOverride` (or `modelDir`
    // if the override is empty) via a fresh IAssetSource + the model loader,
    // upload its drawables through `device`, add an INERT Enemy-layer collision
    // box body via `physics` (mass 0 — kinematic; same as the monster's hitbox
    // but with no combat AI), and register a Tag::Prop entity at `spawn` in
    // `scene`. On model load failure a procedural box stands in (clean checkout
    // safe). Call once. After this the NPC is at `spawn` in its initial mode.
    void build(Scene& scene, x3::rhi::IRenderDevice& dev, x3::phys::IPhysicsWorld& phys,
               const x3::phys::Vec3& spawn, const Tuning& t);

    // Advance one frame:
    //   * Patrol: Lerp the body toward the next waypoint at walkSpeed; advance
    //     when within ~0.5 m; loop the closed sequence.
    //   * LookAtPlayer / Interact: bake yaw toward the player into the entity
    //     transform (no movement).
    //   * Idle / Captive: no movement; yaw stays at the fixed pose or default.
    //   * Whichever mode we're in, if the player crossed in/out of lookAtRange,
    //     flip to/from LookAtPlayer (preserving the base mode for restore).
    // Updates the physics body position (setBodyPosition) and the Scene entity
    // transform so it stays in sync with rendering. No-op if not built.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& phys,
                const x3::phys::Vec3& playerPos);

    // Draw all primitives of the NPC's model at its transform, with a tint that
    // dims slightly in Captive mode (a desaturated read so a captive looks
    // distinct from a free NPC at a glance). Call alongside scene.render() each
    // frame. No-op once shutdown (entity hidden + body removed).
    void drawNPC(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fr, Scene& scene);

    // ---- Authoring API ---------------------------------------------------------
    // Set a closed-loop patrol route. The NPC walks waypoint[0] -> [1] -> ... ->
    // [N-1] -> [0] at `walkSpeed` (m/s, default 1.4 = walking pace). Calling this
    // sets the mode to Patrol. An empty list (or <2 points) is a no-op (use
    // setFixedPose for a stationary NPC). y is preserved per-waypoint so a
    // patrol can go up stairs if the level allows.
    void setPatrol(std::initializer_list<x3::phys::Vec3> waypoints, float walkSpeed = 1.4f);

    // Fixed pose: face this direction (yaw in radians; CONVENTIONS facing law
    // — local -Z under that yaw points at the player). Stops any patrol and
    // sets the mode to Idle.
    void setFixedPose(float yaw);

    // Override the look-at trigger range (meters). The base Tuning value already
    // sets this at build time; the setter is for runtime tuning (HUD/console).
    void setLookAtRange(float r);

    // Slice C/D dialogue hook: callback invoked when the host triggers an
    // interact (e.g. player presses E while close). Empty => no-op. The callback
    // gets a non-const ref so the host can flip modes, etc.
    void setOnInteract(InteractFn cb);

    // Fire the interact callback (if set). Returns true if a callback ran.
    bool triggerInteract();

    // ---- Captive -> companion handoff (generalize the F2 mechanic) ------------
    // True iff the NPC was built (or set) into Captive mode and has not yet been
    // marked rescued. Distinct from mode() == Captive AFTER rescue: see
    // wasRescued().
    bool isCaptive() const;

    // Transition Captive -> Idle and raise the rescued flag. No-op if not
    // currently Captive. The host reads wasRescued() to spawn a companion or
    // tick a rescue counter. Returns true if a captive was actually rescued.
    bool markRescued();

    // True once markRescued() has been called on a Captive (sticky — survives
    // mode changes, so the host can poll it after the fact).
    bool wasRescued() const;

    // ---- Queries (HUD, dialogue, tests) ---------------------------------------
    x3::phys::Vec3 pos() const { return m_pos; }
    float          yaw() const { return m_yaw; }
    bool           alive() const { return m_alive; }
    Mode           mode() const { return m_mode; }
    bool           builtModel() const { return m_built; }
    bool           usingRealModel() const { return m_usingReal; }

    // Force the active mode (host control). Safe to call any time; sticky until
    // overridden. Captive is the one mode you should set via Tuning (the
    // captive tint + the wasRescued() lifecycle expect a clean start).
    void setMode(Mode m);

    // Patrol introspection (for the self-test / HUD).
    uint32_t patrolWaypointCount() const { return (uint32_t)m_patrol.size(); }
    uint32_t patrolNextIndex() const { return m_patrolNext; }

    // Entity / body ids (for the host: HUD prompts, dialog focus, etc.).
    uint32_t         entity() const { return m_entity; }
    x3::phys::BodyId body()   const { return m_body; }

    // ---- Cleanup --------------------------------------------------------------
    // Remove the NPC from the world: hide the entity, remove the physics body
    // (idempotent), and free the loaded model. The name mirrors MonsterSystem
    // for grep symmetry — no real ragdoll is involved (NPCs don't combat-die).
    // Safe to call before physics->shutdown() (mirrors MonsterManager::shutdown).
    void shutdownRagdoll(x3::phys::IPhysicsWorld& phys);

private:
    // Draw helper: model = world transform; tint multiplies each primitive's
    // base color.
    void drawAt(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fr,
                const float model[16], const float tint[4]) const;

    // Bake yaw + scale + pos into the entity transform (column-major). The
    // rigged humanoids are authored facing +Z, so the visual yaw is m_yaw+pi
    // (mirrors RescueVictim::bakeTransform) for the rendered facing to point
    // local -Z along the heading vector — i.e. correctly at the player.
    void bakeTransform(Scene& scene);

    // Loaded model + per-primitive draw records. Loader kept alive so GPU
    // handles in m_drawables stay valid for the NPC's lifetime.
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_model;
    std::vector<x3::asset::ModelDrawable>    m_drawables;
    bool                                     m_usingReal = false;
    float                                    m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // Scene / physics handles.
    uint32_t         m_entity = kNoLink;
    x3::phys::BodyId m_body;
    bool             m_alive = true;     // false after shutdownRagdoll()
    bool             m_built = false;    // false until build() runs

    // Pose state.
    x3::phys::Vec3 m_pos{};
    float          m_yaw       = 0.0f;          // current heading (radians)
    float          m_baseYaw   = 0.0f;          // fixed-pose default yaw (Idle)
    float          m_modelScale = 1.0f;

    // Mode + look-at.
    Mode  m_mode      = Mode::Idle;
    Mode  m_baseMode  = Mode::Idle;             // restored when player leaves lookAtRange
    float m_lookAtRange = 4.0f;
    bool  m_lookingAtPlayer = false;            // true => m_mode forced to LookAtPlayer this frame

    // Patrol (closed-loop waypoints).
    std::vector<x3::phys::Vec3> m_patrol;       // empty unless setPatrol()
    uint32_t                    m_patrolNext = 0;
    float                       m_walkSpeed  = 1.4f;

    // Captive / rescue lifecycle.
    bool m_rescued = false;                     // sticky once markRescued() fires

    // Dialogue hook (Slice C/D).
    InteractFn m_onInteract;

    // Tint applied in drawNPC: base (1,1,1,1); Captive desaturates to a cool
    // bluish-gray so a captive reads distinct without an animation pass.
    float m_baseTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
};

// ---------------------------------------------------------------------------
// Headless self-test (--test-npc). No window / Vulkan; uses HeadlessRenderDevice
// + a fresh physics world. Asserts (>= 6):
//   T1 build() places the NPC at the spawn point + registers a body/entity.
//   T2 update() at the spawn (no patrol, no player nearby) leaves pos/yaw stable.
//   T3 LookAtPlayer triggers when player enters lookAtRange (mode flips) AND yaw
//      rotates to face the player; flips back to base mode when the player leaves.
//   T4 setPatrol() cycles waypoints: position advances toward [0], then [1], ...,
//      and patrolNextIndex() advances through the loop (returns to 0).
//   T5 Captive lifecycle: built in Captive mode -> isCaptive() == true and
//      wasRescued() == false; markRescued() flips mode to Idle, sets wasRescued()
//      true; subsequent markRescued() is a no-op (returns false).
//   T6 shutdownRagdoll() cleans up: entity hidden, body invalid, alive() == false;
//      idempotent (safe to call twice). update() after shutdown is a no-op.
// Logs PASS/FAIL T#; returns true iff all pass. Lives in npc.cpp.
bool runNpcSelfTest();

} // namespace x3::game
