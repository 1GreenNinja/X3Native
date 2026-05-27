#include "companion.h"
#include "engine/core/x3_log.h"   // for x3::logInfo / x3::logError
#include <cmath>
#include <string>

namespace x3::game {

namespace {
    // Behavior scoring weights and tuning constants.
    constexpr float kEngageRange     = 60.0f;
    constexpr float kEngageBase      = 1.0f;
    constexpr float kEngageDistGain  = 0.02f;
    constexpr float kCoverWeight     = 3.0f;
    constexpr float kRetreatWeight   = 2.5f;
    constexpr float kRetreatHpThresh = 0.15f;
    constexpr float kReviveBase      = 2.0f;
    constexpr float kReviveDanger    = 1.5f;
    constexpr float kReviveDangerRange = 8.0f;
    constexpr float kReviveRange     = 1.5f;
    constexpr float kReloadWeight    = 2.3f;
    constexpr float kFollowBase      = 0.2f;
    constexpr float kFollowDistGain  = 0.05f;
    constexpr float kHoldBase        = 0.1f;
    // Soft nudge cap: a suggestion cannot override a reflex decision
    // whose natural margin exceeds this value.
    constexpr float kSuggestionBias  = 0.6f;
}

static x3::phys::Vec3 sub(const x3::phys::Vec3&a,const x3::phys::Vec3&b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static float len(const x3::phys::Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
static const CompanionThreat* bestTarget(const CompanionContext& ctx, float range) {
    if (!ctx.threats) return nullptr;  // Fix 1: null-deref guard
    const CompanionThreat* best=nullptr;
    for (int i=0;i<ctx.threatCount;++i){ const CompanionThreat& t=ctx.threats[i];
        if (!t.losToSelf || t.dist>range) continue; if (!best || t.dist<best->dist) best=&t; }
    return best;
}

float CompanionBrain::score(CompanionBehavior b, const CompanionContext& ctx) const {
    switch (b) {
    case CompanionBehavior::Follow:   { float d=len(sub(ctx.playerPos,ctx.selfPos)); return kFollowBase + kFollowDistGain*d; }
    case CompanionBehavior::Engage:   { if(ctx.ammoInMag<=0) return 0.0f;  // Fix 2: empty mag can't win Engage
                                        const CompanionThreat* t=bestTarget(ctx,kEngageRange); if(!t) return 0.0f;
                                        return kEngageBase + (kEngageRange-t->dist)*kEngageDistGain; }
    case CompanionBehavior::TakeCover:{ const CompanionThreat* t=bestTarget(ctx,kEngageRange); if(!t||!ctx.nearCover) return 0.0f; return (1.0f-ctx.selfHpFrac)*kCoverWeight; }
    case CompanionBehavior::Retreat:  { const CompanionThreat* t=bestTarget(ctx,kEngageRange); if(!t) return 0.0f; if(ctx.selfHpFrac>kRetreatHpThresh||ctx.nearCover) return 0.0f; return kRetreatWeight; }
    case CompanionBehavior::Revive:   { if(!ctx.anyAllyDowned && !ctx.playerDowned) return 0.0f;  // Fix 3: honor playerDowned too
                                        const CompanionThreat* t=bestTarget(ctx,kReviveDangerRange); float danger=t?kReviveDanger:0.0f; return kReviveBase-danger; }
    case CompanionBehavior::Reload:   return ctx.ammoInMag<=0 ? kReloadWeight : 0.0f;  // Fix 2: Reload scores high when empty
    case CompanionBehavior::Hold:     return kHoldBase;
    default: return 0.0f;
    }
}

CompanionCommand CompanionBrain::tick(const CompanionContext& ctx) const {
    CompanionBehavior best = CompanionBehavior::Follow; float bestScore = -1.0f;
    for (int i = 0; i < (int)CompanionBehavior::Count; ++i) {
        float s = score((CompanionBehavior)i, ctx);
        // kSuggestionBias is a SOFT nudge: it cannot override a reflex decision with a natural margin > kSuggestionBias.
        if (ctx.suggestion.prefer == (CompanionBehavior)i) s += kSuggestionBias;
        if (s > bestScore) { bestScore = s; best = (CompanionBehavior)i; }
    }
    CompanionCommand c; c.chosen = best;
    if (best == CompanionBehavior::Follow) {
        x3::phys::Vec3 to = sub(ctx.playerPos, ctx.selfPos);
        if (len(to) > 1.5f) c.moveFwd = 1.0f;
    }
    else if (best == CompanionBehavior::Engage) {
        const CompanionThreat* t = bestTarget(ctx, kEngageRange);
        if (t) {
            x3::phys::Vec3 to=sub(t->pos,ctx.selfPos);
            float horiz=std::sqrt(to.x*to.x+to.z*to.z);
            // Fix 4: degenerate-aim guard — only set aim angles when target is not coincident.
            if (horiz > 1e-4f) {
                c.aimYaw=std::atan2(to.z,to.x);
                c.aimPitch=std::atan2(to.y,horiz);
            }
            c.fire = !ctx.inPlayerLineOfFire && ctx.ammoInMag>0;
        }
    }
    else if (best == CompanionBehavior::TakeCover) { if (ctx.nearCover){ x3::phys::Vec3 to=sub(ctx.coverPos,ctx.selfPos); if(len(to)>0.5f) c.moveFwd=1.0f; } }
    else if (best == CompanionBehavior::Retreat) { const CompanionThreat* t=bestTarget(ctx,kEngageRange); if(t){ c.moveFwd=1.0f; c.sprint=true; } }
    else if (best == CompanionBehavior::Revive) {
        // Fix 3: only reviveAction when in range; otherwise approach.
        float d=len(sub(ctx.downedAllyPos,ctx.selfPos));
        if (d <= kReviveRange) c.reviveAction=true; else c.moveFwd=1.0f;
    }
    else if (best == CompanionBehavior::Reload) { c.reloadAction = true; }  // Fix 2: Reload branch
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
    // C5: ally downed at 5 m (outside kReviveRange) -> Revive chosen, companion APPROACHES (not reviveAction yet).
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3};
      ctx.anyAllyDowned=true; ctx.downedAllyPos={5,0,0}; ctx.selfHpFrac=0.8f;
      CompanionThreat t; t.pos={0,0,20}; t.dist=20.0f; t.losToSelf=true; ctx.threats=&t; ctx.threatCount=1;
      CompanionCommand c=brain.tick(ctx);
      bool ok=(c.chosen==CompanionBehavior::Revive)&&(c.moveFwd>0.5f)&&!c.reviveAction;
      if (ok) pass++; else x3::logError("[companion-test] C5 revive FAILED"); }
    // C5b: downed ally in range (1.0 m) -> actually revive.
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3};
      ctx.anyAllyDowned=true; ctx.downedAllyPos={1,0,0}; ctx.selfHpFrac=0.8f;
      CompanionCommand c=brain.tick(ctx);
      bool ok=(c.chosen==CompanionBehavior::Revive)&&c.reviveAction;
      if (ok) pass++; else x3::logError("[companion-test] C5b revive-in-range FAILED"); }
    // C6: a Hold suggestion makes Hold win over the default Follow.
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,8};
      ctx.suggestion.prefer=CompanionBehavior::Hold;
      CompanionCommand c=brain.tick(ctx);
      if (c.chosen==CompanionBehavior::Hold) pass++; else x3::logError("[companion-test] C6 suggestion FAILED"); }
    // C7: empty mag with a threat present -> Reload (not a frozen Engage).
    { total++; CompanionContext ctx; ctx.selfPos={0,0,0}; ctx.playerPos={0,0,-3}; ctx.ammoInMag=0;
      CompanionThreat t; t.pos={0,0,10}; t.dist=10.0f; t.losToSelf=true; ctx.threats=&t; ctx.threatCount=1;
      CompanionCommand c=brain.tick(ctx);
      bool ok=(c.chosen==CompanionBehavior::Reload)&&c.reloadAction&&!c.fire;
      if (ok) pass++; else x3::logError("[companion-test] C7 reload FAILED"); }
    x3::logInfo("companion: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
