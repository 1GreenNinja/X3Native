#pragma once
// Companion Slice B: CompanionSquad -- owns <=7 Player-driven companion instances.
// Builds a CompanionContext from the live scene each tick, ticks CompanionBrain,
// maps the resulting CompanionCommand to PlayerInput, and drives Player::update.
// Also implements the downed/revive state machine and the --test-companion-squad
// headless self-test.
//
// Clean-room: X3Native systems only. No engine source consulted.
// Namespace: x3::game

#include "companion.h"
#include "player.h"
#include "monster.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"
#include "scene.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace x3::game {

// Per-companion alive/downed/reviving state.
enum class DownedState : uint8_t {
    Alive   = 0,  // fighting normally
    Downed  = 1,  // HP hit 0; incapacitated, can be revived
    Dead    = 2,  // not used yet -- placeholder for finish-off
};

// One companion slot: the Player controller, a reflex brain, and state.
struct CompanionSlot {
    Player         player;
    CompanionBrain brain;
    DownedState    downed      = DownedState::Alive;
    float          reviveTimer = 0.0f;   // >0 while a non-downed companion is reviving this slot
    bool           spawned     = false;
    // Current ammo (tracked host-side; companions always have ammo in v1 -- deferred reload).
    int            ammoInMag   = 30;
};

// Max companions per squad.
constexpr uint32_t kSquadMaxSize = 7;

// Revive proximity radius (m): a companion must be within this to revive an ally.
constexpr float kSquadReviveRange = 1.5f;
// Time (s) a companion must hold the revive action before the ally stands up.
constexpr float kSquadReviveTime  = 2.0f;
// HP fraction restored on a successful revive.
constexpr float kSquadReviveHp    = 0.40f;   // restore to 40% of maxHp

class CompanionSquad {
public:
    // Add a companion spawned at (x, y, z) with a starting aim yaw. Returns the slot
    // index (0-based). Returns ~0u if at capacity (>= kSquadMaxSize).
    uint32_t addCompanion(x3::phys::IPhysicsWorld& physics,
                          float x, float y, float z, float startYaw = 0.0f);

    // Advance the squad one tick: per-slot build context -> brain.tick -> apply
    // command -> Player::update -> downed/revive state machine. `playerPos` is
    // the human player's world position (used in Follow + context). `threats`
    // is the live enemy array (size `threatCount`); pointers must remain valid
    // for the duration of this call. `dt` is the frame step (seconds).
    // playerHpFrac: human player's hp()/maxHp(), used for anyAllyDowned context.
    // `playerDowned`: true if the human player is at 0 HP.
    void tick(float dt,
              x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& playerPos,
              float playerHpFrac,
              bool  playerDowned,
              MonsterSystem* const* threats, uint32_t threatCount);

    // Fire a raycast from `slotIdx` companion's eye toward its aim at Layer::Enemy
    // and call MonsterSystem::fire() on each threat. Called by tick() when cmd.fire.
    void doCompanionFire(uint32_t slotIdx,
                         x3::phys::IPhysicsWorld& physics,
                         Scene& scene,
                         MonsterSystem* const* threats, uint32_t threatCount);

    // Draw stub -- companions share the Player physics body; character models
    // are deferred to a future character-attachment pass.
    void draw(x3::rhi::IRenderDevice& device,
              const x3::rhi::FrameContext& frame,
              const Scene& scene) const { (void)device; (void)frame; (void)scene; }

    // Shut down all companion physics bodies (call before physics->shutdown()).
    void shutdown(x3::phys::IPhysicsWorld& physics);

    // Accessors.
    uint32_t             count() const { return (uint32_t)m_slots.size(); }
    CompanionSlot&       slot(uint32_t i)       { return m_slots[i]; }
    const CompanionSlot& slot(uint32_t i) const { return m_slots[i]; }

    // Set a shared scene pointer (used by doCompanionFire).
    void setScene(Scene* s) { m_scene = s; }

private:
    // NOTE: CompanionSlot contains a Player (by value). Player contains a BodyId
    // (plain uint32_t wrapper -- trivially copyable), so vector reallocation is safe.
    // v2: upgrade to unique_ptr<CompanionSlot> if Player grows non-trivial resources.
    std::vector<CompanionSlot> m_slots;
    Scene*                     m_scene = nullptr;

    // Build a CompanionContext for slot `i` given the live world state.
    // NOTE: uses a static thread-local scratch buffer for threats. Safe because
    // tick() processes companions sequentially (not job-parallel in v1); the buffer
    // is consumed by brain.tick(ctx) before the next companion overwrites it.
    // Flag for v2 job-parallel upgrade: give each slot its own buffer.
    CompanionContext buildContext(uint32_t i,
                                 const x3::phys::Vec3& playerPos,
                                 float playerHpFrac,
                                 bool  playerDowned,
                                 MonsterSystem* const* threats,
                                 uint32_t threatCount) const;

    // Map a CompanionCommand to a PlayerInput (moveFwd / moveStrafe / sprint).
    // The companion's facing is set via Player::setLook before calling Player::update.
    static PlayerInput commandToInput(const CompanionCommand& cmd,
                                      float selfYaw, float targetYaw);
};

// Headless self-test (--test-companion-squad). Scripted scenario:
//  T1: With a visible threat at 12m, the companion brain picks Engage.
//  T2: Artificially zeroing a companion's HP causes it to enter Downed state.
//  T3: With an ally downed nearby (within revive range), the brain picks Revive+reviveAction.
//  T4: Driving the squad revive timer for kSquadReviveTime seconds restores the downed companion.
// Logs T# PASS/FAIL, returns true iff all pass. No window/Vulkan.
bool runCompanionSquadSelfTest();

} // namespace x3::game
