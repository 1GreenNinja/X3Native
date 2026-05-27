#include "companion.h"
#include "engine/core/x3_log.h"   // for x3::logInfo / x3::logError
#include <cmath>
#include <string>

namespace x3::game {

static x3::phys::Vec3 sub(const x3::phys::Vec3&a,const x3::phys::Vec3&b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static float len(const x3::phys::Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}

float CompanionBrain::score(CompanionBehavior b, const CompanionContext& ctx) const {
    switch (b) {
    case CompanionBehavior::Follow: { float d = len(sub(ctx.playerPos, ctx.selfPos)); return 0.2f + 0.05f * d; }
    default: return 0.0f;
    }
}

CompanionCommand CompanionBrain::tick(const CompanionContext& ctx) const {
    CompanionBehavior best = CompanionBehavior::Follow; float bestScore = -1.0f;
    for (int i = 0; i < (int)CompanionBehavior::Count; ++i) {
        float s = score((CompanionBehavior)i, ctx);
        if (s > bestScore) { bestScore = s; best = (CompanionBehavior)i; }
    }
    CompanionCommand c; c.chosen = best;
    if (best == CompanionBehavior::Follow) {
        x3::phys::Vec3 to = sub(ctx.playerPos, ctx.selfPos);
        if (len(to) > 1.5f) c.moveFwd = 1.0f;
    }
    return c;
}

bool runCompanionSelfTest() {
    x3::logInfo("running companion reflex-AI self-test...");
    int pass = 0, total = 0;
    CompanionBrain brain;
    // C1: no threats, player 8 m ahead -> Follow, move toward the player.
    {
        total++;
        CompanionContext ctx;
        ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,8};
        CompanionCommand c = brain.tick(ctx);
        bool ok = (c.chosen == CompanionBehavior::Follow) && (c.moveFwd > 0.5f);
        if (ok) pass++; else x3::logError("[companion-test] C1 follow FAILED");
    }
    x3::logInfo("companion: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
