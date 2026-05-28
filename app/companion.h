#pragma once
// Companion reflex AI (Slice A): a pure, deterministic decision unit. Scores a
// fixed behavior set against a tactical snapshot and emits one per-tick command.
// Game/slice code only; no engine/GPU/physics deps -> trivially unit-testable.
#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3 only
#include <cstdint>

namespace x3::game {

enum class CompanionBehavior : uint8_t {
    Follow = 0, Engage, TakeCover, Revive, Reload, Retreat, Hold, Count
};

struct CompanionThreat {
    x3::phys::Vec3 pos{};
    float          dist     = 0.0f;
    float          toPlayer = 0.0f;
    bool           losToSelf = true;
};

struct CompanionSuggestion {
    CompanionBehavior prefer = CompanionBehavior::Count; // Count == no suggestion
    x3::phys::Vec3    focusTarget{};
    bool              hasFocus = false;
};

struct CompanionContext {
    x3::phys::Vec3 selfPos{};
    x3::phys::Vec3 playerPos{};
    float          selfHpFrac   = 1.0f;
    int            ammoInMag    = 30;
    bool           playerDowned = false;
    bool           anyAllyDowned = false;
    x3::phys::Vec3 downedAllyPos{};
    bool           inPlayerLineOfFire = false;
    const CompanionThreat* threats = nullptr;
    int            threatCount = 0;
    CompanionSuggestion suggestion{};
    bool           nearCover  = false;
    x3::phys::Vec3 coverPos{};
};

struct CompanionCommand {
    float moveFwd = 0, moveStrafe = 0;
    bool  sprint = false, jumpPressed = false;
    float lookDX = 0, lookDY = 0;
    float aimYaw = 0, aimPitch = 0;
    bool  fire = false;
    bool  reviveAction = false;
    bool  reloadAction = false;
    CompanionBehavior chosen = CompanionBehavior::Follow;
};

class CompanionBrain {
public:
    CompanionCommand tick(const CompanionContext& ctx) const;
    float score(CompanionBehavior b, const CompanionContext& ctx) const;
};

bool runCompanionSelfTest();

} // namespace x3::game
