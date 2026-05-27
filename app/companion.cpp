#include "companion.h"
#include "engine/core/x3_log.h"   // for x3::logInfo / x3::logError
#include <cmath>
#include <string>

namespace x3::game {

static x3::phys::Vec3 sub(const x3::phys::Vec3&a,const x3::phys::Vec3&b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static float len(const x3::phys::Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
static const CompanionThreat* bestTarget(const CompanionContext& ctx, float range) {
    const CompanionThreat* best=nullptr;
    for (int i=0;i<ctx.threatCount;++i){ const CompanionThreat& t=ctx.threats[i];
        if (!t.losToSelf || t.dist>range) continue; if (!best || t.dist<best->dist) best=&t; }
    return best;
}

float CompanionBrain::score(CompanionBehavior b, const CompanionContext& ctx) const {
    switch (b) {
    case CompanionBehavior::Follow: { float d = len(sub(ctx.playerPos, ctx.selfPos)); return 0.2f + 0.05f * d; }
    case CompanionBehavior::Engage: { const CompanionThreat* t=bestTarget(ctx,60.0f); if(!t) return 0.0f; return 1.0f + (60.0f - t->dist)*0.02f; }
    case CompanionBehavior::TakeCover: { const CompanionThreat* t=bestTarget(ctx,60.0f); if(!t||!ctx.nearCover) return 0.0f; return (1.0f-ctx.selfHpFrac)*3.0f; }
    case CompanionBehavior::Retreat: { const CompanionThreat* t=bestTarget(ctx,60.0f); if(!t) return 0.0f; if(ctx.selfHpFrac>0.15f||ctx.nearCover) return 0.0f; return 2.5f; }
    case CompanionBehavior::Revive: { if(!ctx.anyAllyDowned) return 0.0f; const CompanionThreat* t=bestTarget(ctx,8.0f); float danger=t?1.5f:0.0f; return 2.0f - danger; }
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
    else if (best == CompanionBehavior::Engage) {
        const CompanionThreat* t = bestTarget(ctx, 60.0f);
        if (t) { x3::phys::Vec3 to=sub(t->pos,ctx.selfPos);
            c.aimYaw=std::atan2(to.z,to.x); float horiz=std::sqrt(to.x*to.x+to.z*to.z);
            c.aimPitch=std::atan2(to.y,horiz); c.fire = !ctx.inPlayerLineOfFire && ctx.ammoInMag>0; }
    }
    else if (best == CompanionBehavior::TakeCover) { if (ctx.nearCover){ x3::phys::Vec3 to=sub(ctx.coverPos,ctx.selfPos); if(len(to)>0.5f) c.moveFwd=1.0f; } }
    else if (best == CompanionBehavior::Retreat) { const CompanionThreat* t=bestTarget(ctx,60.0f); if(t){ c.moveFwd=1.0f; c.sprint=true; } }
    else if (best == CompanionBehavior::Revive) { x3::phys::Vec3 to=sub(ctx.downedAllyPos,ctx.selfPos); float d=len(to); c.reviveAction=true; if(d>1.2f) c.moveFwd=1.0f; }
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
    // C2: threat 10 m, LOS -> Engage + fire.
    {
        total++;
        CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3};
        CompanionThreat t; t.pos={0,0,10}; t.dist=10.0f; t.losToSelf=true; t.toPlayer=13.0f;
        ctx.threats=&t; ctx.threatCount=1;
        CompanionCommand c = brain.tick(ctx);
        bool ok = (c.chosen == CompanionBehavior::Engage) && c.fire;
        if (ok) pass++; else x3::logError("[companion-test] C2 engage FAILED");
    }
    // C3: low HP + exposed + cover -> TakeCover.
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3}; ctx.selfHpFrac=0.25f;
      CompanionThreat t; t.pos={0,0,12}; t.dist=12.0f; t.losToSelf=true; ctx.threats=&t; ctx.threatCount=1;
      ctx.nearCover=true; ctx.coverPos={4,0,0};
      CompanionCommand c=brain.tick(ctx);
      if (c.chosen==CompanionBehavior::TakeCover) pass++; else x3::logError("[companion-test] C3 cover FAILED"); }
    // C4: critical HP, no cover -> Retreat.
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3}; ctx.selfHpFrac=0.1f;
      CompanionThreat t; t.pos={0,0,6}; t.dist=6.0f; t.losToSelf=true; ctx.threats=&t; ctx.threatCount=1; ctx.nearCover=false;
      CompanionCommand c=brain.tick(ctx);
      if (c.chosen==CompanionBehavior::Retreat) pass++; else x3::logError("[companion-test] C4 retreat FAILED"); }
    // C5: ally downed, no close threat -> Revive + reviveAction.
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3};
      ctx.anyAllyDowned=true; ctx.downedAllyPos={5,0,0}; ctx.selfHpFrac=0.8f;
      CompanionThreat t; t.pos={0,0,20}; t.dist=20.0f; t.losToSelf=true; ctx.threats=&t; ctx.threatCount=1;
      CompanionCommand c=brain.tick(ctx);
      bool ok=(c.chosen==CompanionBehavior::Revive)&&c.reviveAction;
      if (ok) pass++; else x3::logError("[companion-test] C5 revive FAILED"); }
    x3::logInfo("companion: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
