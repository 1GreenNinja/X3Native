#pragma once
// NPC CHARACTER — animated citizen BODIES for the living city (SEAM: bodies only).
// ===========================================================================
// This is the clean interface the living-city BEHAVIOR system (schedules, spawn
// logic, the robbery, scan-cards — a SEPARATE branch/owner) drives to put real,
// skinned, animated, ragdoll-capable humanoid bodies in the world WITHOUT knowing
// anything about rig internals, skinning, or physics ragdolls.
//
//   behavior code owns: WHERE a citizen is, WHAT it's doing, WHEN it spawns.
//   NpcCharacter owns:   the mesh + skeleton + anim-set + skinning + ragdoll.
//
// The two fold together through exactly these calls:
//   spawn(archetypeId, pos, yaw)  -> a skinned animated body stands up in the Scene
//   setAnimState(state, speed)    -> Idle / Walk / Run / Talk / Flee (locomotion)
//   moveTo(pos)                   -> teleport-style move; auto-drives walk/run blend
//   setFacing(yaw)                -> logical heading (mesh 180deg visual-flip handled)
//   triggerRagdoll(impulse)       -> hit-react / death FLOP (physical, settles on floor)
//   update(dt)                    -> advances the skinner + reads the ragdoll back
//   draw(...) / despawn(...)      -> render the multi-primitive body / free w/o leak
//
// It is a deliberate, rescue-stripped GENERALIZATION of RescueVictim/MonsterSystem:
// same engine machinery (x3::anim::Skinner + x3::game::RagdollSkin + x3::phys::
// IRagdoll), none of the rescue/combat/boss lifecycle. See docs/NPC_CHARACTER_INTERFACE.md.
//
// Bodies come from the SHARED rigged-humanoid GLBs in assets/rigged_glb (marcus_webb,
// AnnaCasual, chief_martinez — each carrying the shared Idle/Walk/Run(/Talk) clip set
// baked via tools/retarget_from_jake.py). An archetype id selects body + tint + scale
// for crowd variety. Unrigged / clip-less models degrade gracefully to a static idle
// (never a broken level). GPU compute-skinning is used when the device supports it so
// a crowd of dozens holds 60 fps; a headless / non-compute device falls back to CPU
// linear-blend-skinning transparently (so the self-test runs with no window/Vulkan).
//
// Coords per docs/CONVENTIONS.md: +X right, +Y up, -Z forward; face a target with
// yaw = atan2(-dirX,-dirZ). Game/slice code only — engine/ stays pure.

#include "scene.h"
#include "anim.h"      // x3::anim::Skinner (skeletal animation + skinning)
#include "ragdoll.h"   // x3::game::RagdollSkin (rigid bone->skin driver)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Ragdoll.h"          // phys::IRagdoll + makeHumanoidRagdollBones
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// The animation STATE a citizen body plays. Behavior code sets this; the body maps
// it onto the shared clip set / ragdoll:
//   Idle  -> locomotion blend at speed 0 (the Idle clip; breathing sway)
//   Walk  -> locomotion blend at walk pace
//   Run   -> locomotion blend at run pace
//   Flee  -> Run semantics (sprint; used by the robbery/alert scatter)
//   Talk  -> a Talk/gesture clip when the rig has one, else Idle (dialog / scan-card)
//   HitReact / Death -> realized PHYSICALLY via the ragdoll (triggerRagdoll). The
//     ragdoll IS the hit/death reaction (the AAA approach — no canned hit clip). Set
//     via setAnimState() as a convenience, or call triggerRagdoll() directly.
enum class NpcAnimState : uint32_t {
    Idle = 0, Walk, Run, Flee, Talk, HitReact, Death
};
const char* npcAnimStateName(NpcAnimState s);

// A citizen archetype: which shared body GLB + a look (tint) + a size multiplier for
// crowd variety. The table is resolved by id in npc_character.cpp.
struct NpcArchetypeDef {
    const char* modelFile;    // a rigged humanoid in assets/rigged_glb
    float       scaleMul;     // MULTIPLIES the height-fitted base scale (1.0 = ~1.8 m)
    float       tint[4];      // per-drawable base-color multiply (skin/clothing variety)
    const char* label;        // for logs / the HUD
};
uint32_t                npcArchetypeCount();
const NpcArchetypeDef&  npcArchetype(uint32_t id);   // clamped to a valid id

// One animated citizen body. Self-contained like RescueVictim: owns its loaded GLB
// (kept alive for the body's lifetime), a Tag::Prop bookkeeping Entity, a static-by-
// mass collision box, its own Skinner (so it breathes/walks independently), and — once
// triggerRagdoll() fires — a Jolt humanoid ragdoll driving the skin. Drives off the
// EXISTING skinned-mesh path; behavior code only sets state + position.
class NpcCharacter {
public:
    // Spawn archetype `archetypeId` at `pos` (FEET on the ground) facing `yaw`
    // (radians, logical heading). Loads the body from `modelDir` (default
    // assets/rigged_glb), binds the Skinner, height-fits the scale, and stands a
    // collision box + Entity up. On load failure a procedural box stands in (the city
    // never breaks). Idempotent-safe: a second spawn() on a live body is ignored.
    void spawn(uint32_t archetypeId, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               const x3::phys::Vec3& pos, float yaw);

    // Advance one frame: drive the Skinner (locomotion blend / Talk clip) OR — once
    // ragdolled — read the ragdoll bones back out of the SHARED physics world (which
    // the host steps) and flop the skin to match. Bakes the facing + placement each
    // frame. No-op if not spawned. `physics` must be the world the host steps.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Behavior interface -----------------------------------------------------
    // Set the animation state. `moveSpeed` (m/s) drives the Walk/Run/Flee blend; it is
    // ignored for Idle/Talk. HitReact/Death forward to triggerRagdoll() (physical).
    void setAnimState(NpcAnimState state, float moveSpeed = 0.0f);
    NpcAnimState animState() const { return m_animState; }

    // Set the logical facing yaw (radians; the +Z-authored rig's 180deg visual flip is
    // applied internally). Behavior code that steers a citizen sets this.
    void setFacing(float yaw) { m_yaw = yaw; }

    // Teleport-style move to `pos` (the living-city movement idiom, like the monster
    // chase / companion follow). Measures the planar speed since the last moveTo and
    // auto-selects Idle/Walk/Run so a moving citizen visibly walks. Also faces travel.
    // No-op once ragdolled.
    void moveTo(Scene& scene, x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& pos);

    // Collapse into a physics ragdoll: snapshot the current animated pose, build a Jolt
    // humanoid ragdoll placed/yawed/scaled to match this body, seed it seamlessly from
    // that pose, add it to the SHARED world, and kick it with `impulse` (world m/s; a
    // hit direction * strength, or {0,0,0} for a gentle topple). From then on update()
    // flops the skinned MESH to the bodies, and the ragdoll SETTLES on whatever static
    // floor is under it (per-instance — NOT hardwired y=0). Idempotent; a no-op on an
    // unrigged model (it stays standing). This realizes both HitReact and Death.
    //
    // The ragdoll BODIES are built on the NEXT update() (which owns the Scene +
    // IPhysicsWorld this citizen belongs to), so triggerRagdoll needs no physics arg
    // and can be called from anywhere the behavior layer holds the body. The one-frame
    // latency is imperceptible (and the flop is seeded from the frozen pose, so no pop).
    void triggerRagdoll(const x3::phys::Vec3& impulse);

    // Draw the multi-primitive body at its transform (the Entity render mesh is left
    // invalid; this owns the draw, like MonsterSystem::drawMonster / RescueVictim).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Free everything without a leak: remove the ragdoll + collision bodies from the
    // world, hand the GPU skinned-mesh registration back to the device
    // (disableGpuSkinning), and hide the Entity. Idempotent. Call when the behavior
    // system despawns a citizen (out of view / schedule end).
    void despawn(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Queries (behavior HUD / self-test) ------------------------------------
    bool spawned()   const { return m_spawned; }
    bool despawned() const { return m_despawned; }
    bool skinnable() const { return m_animActive; }            // a usable clip drives it
    bool ragdolled() const { return m_ragdolled; }             // a physics ragdoll drives the skin
    // The ragdoll has come to rest (all bodies asleep) — the corpse settled on the floor.
    bool ragdollSettled() const;
    // Lowest ragdoll bone Y (settle proof: it lands on the floor, not floating/at y=0).
    float ragdollLowestY() const;
    uint32_t archetype() const { return m_archetype; }
    x3::phys::Vec3 pos() const { return m_pos; }
    uint32_t entity() const { return m_entity; }
    float groundY() const { return m_groundY; }                // spawn-time ground raycast

    // Read-only handles for the self-test (assert the palette animates / diverges).
    const x3::anim::Skinner& skinner() const { return m_skinner; }
    const x3::asset::Model&  model()   const { return m_model; }
    const x3::phys::IRagdoll* ragdoll() const { return m_deathRagdoll.get(); }

private:
    void bakeTransform(Scene& scene);                 // yaw + scale + pos -> Entity xform
    void driveAnim(float dt);                          // Skinner locomotion / Talk clip
    void driveSkinFromRagdoll();                       // ragdoll bones -> skin (external pose)
    void fitScale();                                   // measure skeleton height -> base scale
    // Build the Jolt ragdoll in the citizen's OWN world (called from update() so it has
    // the Scene + IPhysicsWorld). Freezes the entity, seeds + kicks the ragdoll.
    void buildRagdoll(Scene& scene, x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& impulse);

    // ---- Loaded body ----
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_model;
    std::vector<x3::asset::ModelDrawable>    m_drawables;
    bool                                     m_usingReal = false;

    // ---- Skeletal animation (own Skinner; per-body phase so a crowd isn't lockstep) ----
    x3::anim::Skinner        m_skinner;
    x3::rhi::IRenderDevice*  m_device      = nullptr;
    int   m_idleClip   = -1;
    int   m_walkClip   = -1;
    int   m_runClip    = -1;
    int   m_talkClip   = -1;
    bool  m_useLocoBlend = false;
    bool  m_animActive   = false;
    float m_animTime     = 0.0f;    // single-clip (Talk) playback time
    float m_phaseOffset  = 0.0f;    // per-body start phase so bodies don't sync
    float m_locoSpeed    = 0.0f;    // current locomotion speed command (m/s)

    // ---- Physics ragdoll (collapse) — same machinery as MonsterSystem/RescueVictim ----
    std::unique_ptr<x3::phys::IRagdoll>      m_deathRagdoll;
    RagdollSkin                              m_ragdollSkin;
    std::vector<x3::phys::RagdollBoneDesc>   m_ragdollBones;
    std::vector<float>                       m_ragPartInit;
    std::vector<float>                       m_ragWorldScratch;
    std::vector<float>                       m_ragPartCur;
    std::vector<float>                       m_ragNodeGlobals;
    float                                    m_deathModelInv[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool                                     m_ragdolled = false;
    bool                                     m_pendingRagdoll = false;   // triggerRagdoll latched; built next update()
    x3::phys::Vec3                           m_pendingImpulse{};         // the kick to apply on build

    // ---- Placement / bookkeeping ----
    uint32_t         m_archetype  = 0;
    NpcAnimState     m_animState  = NpcAnimState::Idle;
    x3::phys::Vec3   m_pos{};
    x3::phys::Vec3   m_prevMovePos{};   // last moveTo() position (locomotion-speed measure)
    uint32_t         m_entity     = kNoLink;
    x3::phys::BodyId m_body;
    float            m_modelScale = 1.0f;
    float            m_fittedScale = 0.0f;
    float            m_yaw = 0.0f;
    float            m_groundY = 0.0f;
    float            m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float            m_tint[4] = { 1, 1, 1, 1 };
    bool             m_spawned   = false;
    bool             m_despawned = false;
};

// ---------------------------------------------------------------------------
// NpcCrowd — a thin owner for a set of NpcCharacter bodies. This is the SEAM the
// living-city behavior system holds: it spawns bodies by archetype at positions the
// behavior layer decides, drives their state, and updates/draws them as a batch. It
// deliberately holds NO schedule/robbery/scan logic (that's the other branch) — just
// the pool of bodies + a couple of batch conveniences.
// ---------------------------------------------------------------------------
class NpcCrowd {
public:
    // Spawn `count` citizens on a ring of radius `radius` around (centerX,groundY,
    // centerZ), cycling the archetype table for variety. Returns the number spawned.
    uint32_t spawnRing(uint32_t count, Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                       float centerX, float groundY, float centerZ, float radius);

    // Spawn a single citizen; returns its index (or a growing pool). The behavior
    // system uses this to place a body wherever its spawn logic decides.
    uint32_t spawn(uint32_t archetypeId, Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                   const x3::phys::Vec3& pos, float yaw);

    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;
    void despawnAll(Scene& scene, x3::phys::IPhysicsWorld& physics);

    uint32_t count() const { return (uint32_t)m_bodies.size(); }
    NpcCharacter&       body(uint32_t i)       { return *m_bodies[i]; }
    const NpcCharacter& body(uint32_t i) const { return *m_bodies[i]; }

private:
    std::vector<std::unique_ptr<NpcCharacter>> m_bodies;
};

// Headless self-test (--test-npcchar). Builds citizens on a HeadlessDevice + Jolt
// world (with a NON-ZERO static floor to prove per-instance settle) and asserts:
//   N1 spawn loads a rigged, skinnable body (or gracefully a static box on a clean
//      checkout with no rigged GLB present — still PASS).
//   N2 anim STATES switch: Idle vs Walk produce DIFFERENT joint palettes (the body
//      actually animates, not frozen), and Talk resolves when the rig has it.
//   N3 triggerRagdoll spawns a live ragdoll that FALLS and SETTLES on the non-zero
//      floor (lowest bone lands near the floor Y, NOT y=0, NOT floating — no NaN).
//   N4 the ragdoll drove the SKIN (the palette diverged from the animated pose).
//   N5 despawn/respawn leaks nothing (bodies removed, GPU skin freed, entity hidden).
//   N6 a small CROWD ticks + draws headlessly and despawns clean (no leaked bodies).
// Logs PASS/FAIL N#, returns true iff all pass. No window/Vulkan.
bool runNpcCharacterSelfTest();

} // namespace x3::game
