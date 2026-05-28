// Ally NPC AI state machine + fireOnce + resolveHit (coop-NPC PR, Phase B).
//
// See app/ally.h for the contract and the full design rationale. This file
// implements the BEHAVIOUR half of AllyManager: the per-frame state machine,
// the firing/raycast path with the "courteous allies" friendly-skip, the
// centralised hit-resolution rule for the friendly-fire cvar, plus the
// trivial accessors (setWeapon / aliveCount).
//
// Clean-room: built on the existing IRenderDevice / IModelLoader / IAssetSource
// / INavigation / IPhysicsWorld interfaces + scene.h + the combat:: balance
// namespace from monster.h. No purchased C# copied; no id Tech / RBDOOM /
// other game-engine source consulted.
//
// Patterns mirrored from monster.cpp (the canonical "AI ticks a state, slews a
// heading, writes the transform back, decrements timers" pattern):
//   * composeTRS / headingToFace / angWrap / slewAngle — same math, same
//     CONVENTIONS (model local -Z forward, yaw about +Y, w-LAST quat).
//   * fire(): raycast Layer::Enemy, scene.entityForBody() -> Tag check ->
//     apply damage path. Allies layer on a PRE-fire Layer::Player raycast so
//     they never plink a friend in their own line-of-fire.
//   * Per-frame transform writeback: bake yaw into the upper-left 3x3 via
//     composeTRS + setBodyPosition / setBodyRotation, so the physics body's
//     orientation tracks the visual heading exactly (the MonsterSystem fix).
//
// Phase split (so multiple agents can land this lane in parallel):
//   Phase A (ally.cpp)    — asset loading + build() + draw() + the GLB loaders.
//   Phase B (THIS FILE)   — AI state machine + fireOnce + resolveHit + setWeapon.
//   Phase C (scene/main)  — Tag::Ally + Layer::Ally + bench arena CLI wiring.
//
// All gameplay numbers are taken from combat:: (monster.h) so allies and enemies
// fight on the same balance grid. Per-weapon cooldowns/damage are LIGHTLY
// adapted from the player-weapon table feel (SMG/chaingun fast, shotgun slow).

#include "ally.h"
#include "scene.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Local math helpers (same names + semantics as monster.cpp; intentionally
// duplicated rather than re-exported so the two systems stay independently
// testable and ally_ai.cpp only depends on what it actually uses).
// ---------------------------------------------------------------------------
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Matches MonsterSystem's composeTRS exactly so the allies
// render through the same transform pipeline as the monsters/scene.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// CONVENTIONS facing math: model's local -Z is forward. headingToFace(dx,dz)
// returns the yaw (rad, about +Y) so the local -Z axis points along (dx,dz).
// Identical relationship monster.cpp's headingToFace uses.
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

float angWrap(float d) {
    while (d >  kPi)  d -= 2.0f * kPi;
    while (d <= -kPi) d += 2.0f * kPi;
    return d;
}

// Slew current angle `cur` toward `target` by at most rate*dt (shortest way).
float slewAngle(float cur, float target, float rate, float dt) {
    const float diff = angWrap(target - cur);
    const float step = rate * dt;
    if (diff >  step) return cur + step;
    if (diff < -step) return cur - step;
    return target;
}

// Per-weapon firing parameters. Pure DATA + DEFAULTS — kept here rather than
// in ally.h so a tweak to ally feel is one file. Numbers anchored on
// combat::kRangedDamageDefault / kMeleeDamageDefault so allies and enemies
// share a damage grid. Range comes from the ally.h engage-range constants.
struct WeaponParams {
    int   damage   = combat::kRangedDamageDefault;
    float cooldown = combat::kRangedCooldownDefault;
    float range    = kAllyEngageRangeHitscan;
};

WeaponParams weaponParams(AllyWeapon w) {
    WeaponParams p;
    switch (w) {
        case AllyWeapon::None:
            // Disarmed: cannot fire. Damage 0 + huge cooldown effectively gates
            // every shot. The state machine still runs (the ally TRIES to fire),
            // but the magazine never empties and nothing is dealt.
            p.damage = 0; p.cooldown = 5.0f; p.range = 0.0f; break;
        case AllyWeapon::Pistol:
            // Baseline: ranged-default damage on the default ranged cooldown.
            p.damage = combat::kRangedDamageDefault;
            p.cooldown = combat::kRangedCooldownDefault;
            p.range = kAllyEngageRangeHitscan; break;
        case AllyWeapon::SMG:
            // Fast cyclic, slightly under default damage.
            p.damage = combat::kRangedDamageMin + 1;
            p.cooldown = 0.18f;
            p.range = kAllyEngageRangeHitscan; break;
        case AllyWeapon::Shotgun:
            // Close-range bruiser: melee-default * 1.5 in one shot, slow.
            p.damage = (int)(combat::kMeleeDamageDefault * 1.5f + 0.5f);
            p.cooldown = 0.85f;
            p.range = kAllyEngageRangeShotgun; break;
        case AllyWeapon::Plasma:
            // Mid-range projectile (modeled hitscan here; still telegraphs slow).
            p.damage = combat::kRangedDamageMax + 2;
            p.cooldown = 0.65f;
            p.range = kAllyEngageRangePlasma; break;
        case AllyWeapon::Chaingun:
            // Highest cyclic; light per-shot damage.
            p.damage = combat::kRangedDamageMin;
            p.cooldown = 0.10f;
            p.range = kAllyEngageRangeHitscan; break;
        case AllyWeapon::PlasmaRifle:
            // A solid all-rounder between Pistol and Plasma.
            p.damage = combat::kRangedDamageMax;
            p.cooldown = 0.40f;
            p.range = kAllyEngageRangeHitscan; break;
        case AllyWeapon::Lightning:
            // Steady stream: medium damage, steady cooldown.
            p.damage = combat::kRangedDamageDefault;
            p.cooldown = 0.20f;
            p.range = kAllyEngageRangeHitscan; break;
        case AllyWeapon::Count: break;
    }
    return p;
}

// Find the nearest hostile within `radius` of `self`; returns true and fills
// `outPos` / `outDist` on success. Uses HostileQueryFn which the host supplies.
// No heap alloc in the hot path (fixed buffer).
bool nearestHostile(const HostileQueryFn& q, const x3::phys::Vec3& self,
                    float radius, x3::phys::Vec3& outPos, float& outDist) {
    if (!q) return false;
    x3::phys::Vec3 buf[16];
    uint32_t n = q(self, radius, buf, 16u);
    if (n == 0) return false;
    float bestD2 = 1e30f; int best = -1;
    for (uint32_t i = 0; i < n; ++i) {
        const float dx = buf[i].x - self.x, dz = buf[i].z - self.z;
        const float d2 = dx*dx + dz*dz;
        if (d2 < bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;
    outPos  = buf[best];
    outDist = std::sqrt(bestD2);
    return true;
}

// Hysteresis: once an ally enters TakeCover it stays there until BOTH a min
// time has elapsed AND HP has recovered above ~0.45 of max. Mirrors
// kAiRetreatExitFrac from monster.h.
constexpr float kAllyCoverExitFrac = 0.45f;
constexpr float kAllyCoverMinTime  = 1.5f;
constexpr float kAllyRepoStepTime  = 0.35f;   // brief Reposition dwell

} // namespace

const char* allyStateName(AllyState s) {
    switch (s) {
        case AllyState::Follow:     return "Follow";
        case AllyState::Engage:     return "Engage";
        case AllyState::Reposition: return "Reposition";
        case AllyState::Reload:     return "Reload";
        case AllyState::TakeCover:  return "TakeCover";
        case AllyState::Search:     return "Search";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// setWeapon — flip the per-ally weapon slot on EVERY ally of that kind.
// One-liner per the spec; iterates because m_allies is the source of truth.
// ---------------------------------------------------------------------------
void AllyManager::setWeapon(AllyKind k, AllyWeapon w) {
    for (auto& a : m_allies) if (a.kind == k) a.weapon = w;
}

uint32_t AllyManager::aliveCount() const {
    uint32_t n = 0;
    for (const auto& a : m_allies) if (a.alive) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// resolveHit — centralised "did a shot land on an ally?" rule. Called by the
// host's shoot-resolver to keep the friendly-fire decision in ONE place: a
// future toggle/cvar tweak edits only this function.
// ---------------------------------------------------------------------------
bool AllyManager::resolveHit(uint32_t bodyId, Faction shooterFaction,
                             int shotDamage, bool friendlyFireEnabled,
                             int& outDamage) {
    outDamage = 0;
    // Find which ally owns this physics body (linear; N=3 in canon, so no map).
    AllyInstance* hit = nullptr;
    for (auto& a : m_allies) {
        if (a.alive && a.bodyId != 0 && a.bodyId == bodyId) { hit = &a; break; }
    }
    if (!hit) return false;

    // Base hostility rule: factions on opposite sides damage each other; same
    // side does not. friendlyFireEnabled LAYERS on top: when true, the
    // Player->Ally edge becomes hostile too. Ally->Ally NEVER becomes hostile
    // (the "no surprise teamkill" floor); the friendly cvar only opens up
    // Player<->Ally, exactly as documented in faction.h.
    bool hostile = factionsHostile(shooterFaction, Faction::Ally);
    if (!hostile && friendlyFireEnabled && shooterFaction == Faction::Player) {
        hostile = true;
    }
    if (!hostile) return false;

    // Apply damage. Clamp at 0; on death, fire the death FX once + zero the
    // entity's body + hide the scene entity. The physics body itself is left
    // for the caller to remove (the host's shoot-resolver owns the body) —
    // mirrors the monster path where fire() removes the body, but here we
    // arrive AFTER the host already resolved the hit body, so the body is
    // expected to still exist until the host's removeBody on the next pass.
    hit->hp -= shotDamage;
    if (hit->hp < 0) hit->hp = 0;
    outDamage = shotDamage;

    if (hit->hp <= 0 && hit->alive) {
        hit->alive = false;
        if (m_deathFx) {
            const float p[3] = { hit->transform[12], hit->transform[13], hit->transform[14] };
            m_deathFx(p);
        }
        x3::logInfo(std::string("[ally] ") + allyKindName(hit->kind) +
                    " killed (friendly-fire=" + (friendlyFireEnabled ? "1" : "0") + ")");
    } else {
        x3::logInfo(std::string("[ally] ") + allyKindName(hit->kind) +
                    " took " + std::to_string(shotDamage) +
                    " — HP " + std::to_string(hit->hp));
    }
    return true;
}

// ---------------------------------------------------------------------------
// tickFollow — no hostile visible: hold a position kAllyFollowDistMin..Max
// behind the player. If a hostile appears in engage range, switch to Engage.
// ---------------------------------------------------------------------------
AllyState AllyManager::tickFollow(AllyInstance& a, float dt,
                                  const x3::phys::Vec3& playerPos,
                                  const HostileQueryFn& q) {
    (void)dt;
    // Check for a hostile within the broadest engage range first so the squad
    // engages as soon as ANY weapon could reach.
    x3::phys::Vec3 hPos; float hDist = 0.0f;
    if (nearestHostile(q, x3::phys::Vec3{a.transform[12], a.transform[13], a.transform[14]},
                       kAllyEngageRangeHitscan, hPos, hDist)) {
        for (int i = 0; i < 3; ++i) a.lastKnownHostile[i] = (&hPos.x)[i];
        return AllyState::Engage;
    }

    // Follow geometry: aim for a point between min..max behind the player along
    // -player_forward. We don't have a player forward here, so we trail along
    // the planar vector FROM player TO ally (i.e. stay where we are relative to
    // the player but pulled inside the [min,max] band). This avoids ever
    // standing in front of / inside the player's path.
    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };
    float dx = self.x - playerPos.x;
    float dz = self.z - playerPos.z;
    float dist = std::sqrt(dx*dx + dz*dz);
    if (dist < 1e-4f) { dx = 0.0f; dz = -1.0f; dist = 1.0f; }
    const float ux = dx / dist, uz = dz / dist;

    // Desired range: midpoint of the follow band when too close/far, else hold.
    float desired = dist;
    if (dist < kAllyFollowDistMin) desired = kAllyFollowDistMin;
    if (dist > kAllyFollowDistMax) desired = 0.5f * (kAllyFollowDistMin + kAllyFollowDistMax);
    const float targetX = playerPos.x + ux * desired;
    const float targetZ = playerPos.z + uz * desired;

    // Move toward the target at kAllyMoveSpeed. No nav grid: direct step.
    float mx = targetX - self.x, mz = targetZ - self.z;
    const float ml = std::sqrt(mx*mx + mz*mz);
    if (ml > 0.05f) {
        mx /= ml; mz /= ml;
        const float step = kAllyMoveSpeed * dt;
        a.transform[12] = self.x + mx * std::min(step, ml);
        a.transform[14] = self.z + mz * std::min(step, ml);
        a.yawTarget = headingToFace(mx, mz);
    } else {
        // Arrived — face the player (so the squad isn't standing back-to).
        a.yawTarget = headingToFace(playerPos.x - self.x, playerPos.z - self.z);
    }
    return AllyState::Follow;
}

// ---------------------------------------------------------------------------
// tickEngage — face nearest hostile, fire on cooldown. Handles transitions to
// Reposition (timer), Reload (mag empty), Search (lost LOS), TakeCover (low HP).
// ---------------------------------------------------------------------------
AllyState AllyManager::tickEngage(AllyInstance& a, float dt,
                                  const x3::phys::Vec3& playerPos,
                                  const HostileQueryFn& q,
                                  x3::phys::IPhysicsWorld& physics) {
    (void)playerPos;
    // Low HP -> TakeCover (entry side of the hysteresis; tickTakeCover handles exit).
    const float hpFrac = (float)a.hp / (float)kAllyHp;
    if (hpFrac <= kAllyTakeCoverFrac) return AllyState::TakeCover;

    // Need ammo to engage. Empty mag -> reload (return; the next tick is Reload).
    if (a.magRemaining <= 0.0f) { a.reloadTimer = kAllyReloadTime; return AllyState::Reload; }

    // Per-weapon engage range gates what counts as "in range".
    const WeaponParams wp = weaponParams(a.weapon);
    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };
    x3::phys::Vec3 hPos; float hDist = 0.0f;
    if (!nearestHostile(q, self, wp.range, hPos, hDist)) {
        // Lost the hostile — keep last-known and Search.
        return AllyState::Search;
    }
    for (int i = 0; i < 3; ++i) a.lastKnownHostile[i] = (&hPos.x)[i];

    // LOS: ray from our chest toward the hostile against Static walls; only
    // engage if the line is clear. (Mirror of monster.cpp's "self-skip then
    // rayCast Layer::Static" idiom; Layer::Static also collides with Enemy/Ally
    // bodies so we offset the ray origin past our own collision skirt.)
    const x3::phys::Vec3 muzzle{ self.x, self.y + 1.2f, self.z };
    x3::phys::Vec3 d{ hPos.x - muzzle.x, hPos.y - muzzle.y, hPos.z - muzzle.z };
    float dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    if (dl < 1e-4f) dl = 1e-4f;
    const x3::phys::Vec3 nd{ d.x/dl, d.y/dl, d.z/dl };
    const float skip = 0.55f;                          // past our own capsule
    const x3::phys::Vec3 from{ muzzle.x + nd.x*skip, muzzle.y + nd.y*skip,
                               muzzle.z + nd.z*skip };
    const float losLen = std::max(0.0f, dl - skip);
    x3::phys::RayHit wall = (losLen > 1e-3f)
        ? physics.rayCast(from, nd, losLen, x3::phys::Layer::Static)
        : x3::phys::RayHit{};
    if (wall.hit) return AllyState::Search;            // wall between us — lost LOS

    // Face the hostile + fire when the cooldown clears.
    a.yawTarget = headingToFace(hPos.x - self.x, hPos.z - self.z);
    a.fireCooldown -= dt;
    if (a.fireCooldown <= 0.0f) {
        fireOnce(a, physics, hPos);
        // fireOnce sets fireCooldown + decrements magRemaining (or skips on
        // friendly-in-cone). If the mag emptied, the next tick will Reload.
    }

    // Periodic reposition step so the ally doesn't camp one tile.
    a.repositionTimer -= dt;
    if (a.repositionTimer <= 0.0f) {
        a.repositionTimer = kAllyRepoStepTime;
        return AllyState::Reposition;
    }
    return AllyState::Engage;
}

// ---------------------------------------------------------------------------
// tickReposition — one lateral step perpendicular to the hostile-line, brief
// cooldown, then back to Engage. The hostile is re-queried so a missing target
// also returns to Engage (which will then re-evaluate to Search).
// ---------------------------------------------------------------------------
AllyState AllyManager::tickReposition(AllyInstance& a, float dt,
                                      const x3::phys::Vec3& playerPos,
                                      const HostileQueryFn& q) {
    (void)playerPos;
    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };
    x3::phys::Vec3 hPos; float hDist = 0.0f;
    if (!nearestHostile(q, self, kAllyEngageRangeHitscan, hPos, hDist)) {
        return AllyState::Engage;   // hostile gone; let Engage tick decide next
    }

    // Lateral (perpendicular) unit vector relative to the hostile line.
    float dx = hPos.x - self.x, dz = hPos.z - self.z;
    const float dl = std::sqrt(dx*dx + dz*dz);
    if (dl > 1e-4f) { dx /= dl; dz /= dl; }
    // Perpendicular (left = (-dz, +dx)). Pick a side using the ally entity id
    // hash so two allies don't always step the same way.
    const float side = ((a.entityId * 2654435761u) & 1u) ? 1.0f : -1.0f;
    const float px = -dz * side, pz = dx * side;
    const float step = kAllyMoveSpeed * dt;
    a.transform[12] = self.x + px * step;
    a.transform[14] = self.z + pz * step;
    // Keep facing the hostile so the next fire is well-aimed.
    a.yawTarget = headingToFace(hPos.x - a.transform[12], hPos.z - a.transform[14]);

    // Count down the brief reposition dwell, then back to Engage.
    a.repositionTimer -= dt;
    if (a.repositionTimer <= 0.0f) {
        a.repositionTimer = kAllyRepositionPeriod;     // re-arm long timer for Engage
        return AllyState::Engage;
    }
    return AllyState::Reposition;
}

// ---------------------------------------------------------------------------
// tickReload — count down reloadTimer; refill on 0 and return to Engage.
// ---------------------------------------------------------------------------
AllyState AllyManager::tickReload(AllyInstance& a, float dt) {
    a.reloadTimer -= dt;
    if (a.reloadTimer <= 0.0f) {
        a.reloadTimer  = 0.0f;
        a.magRemaining = kAllyMagSize;
        return AllyState::Engage;
    }
    return AllyState::Reload;
}

// ---------------------------------------------------------------------------
// tickTakeCover — back away from the hostile toward the player; only RETURN to
// Follow once HP has recovered above kAllyCoverExitFrac AND a min dwell time
// has elapsed (no insta-flip). Mirrors monster.h::kAiRetreatExitFrac.
// ---------------------------------------------------------------------------
AllyState AllyManager::tickTakeCover(AllyInstance& a, float dt,
                                     const x3::phys::Vec3& playerPos,
                                     const HostileQueryFn& q) {
    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };

    // Direction TOWARD the player (always a safe-ish retreat vector — the
    // player is the squad anchor). If a hostile exists, also push AWAY from it
    // so the move vector blends "to the player" + "away from the threat".
    float toPx = playerPos.x - self.x, toPz = playerPos.z - self.z;
    float tpL = std::sqrt(toPx*toPx + toPz*toPz);
    if (tpL > 1e-4f) { toPx /= tpL; toPz /= tpL; }

    float awayX = 0.0f, awayZ = 0.0f;
    x3::phys::Vec3 hPos; float hDist = 0.0f;
    const bool seeHostile = nearestHostile(q, self, kAllyEngageRangeHitscan, hPos, hDist);
    if (seeHostile) {
        float ax = self.x - hPos.x, az = self.z - hPos.z;
        const float al = std::sqrt(ax*ax + az*az);
        if (al > 1e-4f) { awayX = ax/al; awayZ = az/al; }
    }
    // Blend: 60% toward player, 40% away from threat (when present).
    float mx = 0.6f * toPx + 0.4f * awayX;
    float mz = 0.6f * toPz + 0.4f * awayZ;
    const float ml = std::sqrt(mx*mx + mz*mz);
    if (ml > 1e-4f) {
        mx /= ml; mz /= ml;
        const float step = kAllyMoveSpeed * dt;
        a.transform[12] = self.x + mx * step;
        a.transform[14] = self.z + mz * step;
        // Face the threat (so they read as covering, not fleeing blindly).
        if (seeHostile) {
            a.yawTarget = headingToFace(hPos.x - a.transform[12], hPos.z - a.transform[14]);
        } else {
            a.yawTarget = headingToFace(mx, mz);
        }
    }

    // losMemory doubles as the cover dwell timer here (already a per-instance
    // float; saves adding a field). Count UP via dt instead of down — when it
    // exceeds the min dwell AND HP recovered, exit cover.
    a.losMemory += dt;
    const float hpFrac = (float)a.hp / (float)kAllyHp;
    if (a.losMemory >= kAllyCoverMinTime && hpFrac > kAllyCoverExitFrac) {
        a.losMemory = 0.0f;
        return AllyState::Follow;
    }
    return AllyState::TakeCover;
}

// ---------------------------------------------------------------------------
// tickSearch — walk toward lastKnownHostile, sweep heading; on timer expiry
// return Follow; on hostile reappearing return Engage.
// ---------------------------------------------------------------------------
AllyState AllyManager::tickSearch(AllyInstance& a, float dt, const HostileQueryFn& q) {
    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };

    // Hostile reappeared? Engage.
    x3::phys::Vec3 hPos; float hDist = 0.0f;
    if (nearestHostile(q, self, kAllyEngageRangeHitscan, hPos, hDist)) {
        for (int i = 0; i < 3; ++i) a.lastKnownHostile[i] = (&hPos.x)[i];
        return AllyState::Engage;
    }

    // Walk to last-known.
    float dx = a.lastKnownHostile[0] - self.x;
    float dz = a.lastKnownHostile[2] - self.z;
    const float dl = std::sqrt(dx*dx + dz*dz);
    if (dl > 0.8f) {
        dx /= dl; dz /= dl;
        const float step = kAllyMoveSpeed * dt;
        a.transform[12] = self.x + dx * step;
        a.transform[14] = self.z + dz * step;
        a.yawTarget = headingToFace(dx, dz);
    } else {
        // Arrived — sweep the heading left/right ("scanning").
        a.yawTarget = a.yaw + std::sin(a.losMemory * 4.0f) * 0.5f;
    }

    // losMemory counts DOWN here (the search-memory timer from ally.h).
    a.losMemory -= dt;
    if (a.losMemory <= 0.0f) {
        a.losMemory = 0.0f;
        return AllyState::Follow;
    }
    return AllyState::Search;
}

// ---------------------------------------------------------------------------
// fireOnce — issue ONE shot from `a` toward `targetWorld`. The "courteous
// allies" rule: BEFORE we fire, we cast against Layer::Player along the same
// line; if any friendly body is in the firing cone (i.e. the player or another
// ally is hit before the target along this ray), the shot is SILENTLY SKIPPED.
// This is the anti-griefing floor — it holds even with g_friendlyfire=1.
//
// After the courtesy check we cast against Layer::Enemy. A hit on a live
// Faction::Enemy body costs that body `wp.damage` HP (the hit body's owner is
// resolved by the host's monster manager via its own resolveHit). Here we only
// log + decrement our magazine + re-arm the per-weapon cooldown. The actual
// damage applied to the enemy is wired through the host (so this file does
// not need to know about MonsterManager); the bench harness asserts the
// raycast LANDS on the enemy layer, which is the contract we honor.
// ---------------------------------------------------------------------------
void AllyManager::fireOnce(AllyInstance& a, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& targetWorld) {
    const WeaponParams wp = weaponParams(a.weapon);
    if (wp.damage <= 0 || wp.range <= 0.0f) return;     // disarmed: silently no-op

    const x3::phys::Vec3 self{ a.transform[12], a.transform[13], a.transform[14] };
    const x3::phys::Vec3 muzzle{ self.x, self.y + 1.2f, self.z };
    x3::phys::Vec3 d{ targetWorld.x - muzzle.x,
                      targetWorld.y - muzzle.y,
                      targetWorld.z - muzzle.z };
    float dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    if (dl < 1e-4f) return;
    const x3::phys::Vec3 nd{ d.x/dl, d.y/dl, d.z/dl };

    // ---- "Courteous allies": skip if a friendly is in the cone. ----
    // Offset past our own capsule before probing (same self-skip idiom as
    // monster.cpp uses), then raycast Layer::Player. A hit at any distance
    // <= the target distance means a friendly is between us and the enemy —
    // skip the shot SILENTLY (no log spam, no cooldown burn) so the ally just
    // re-tries next frame from a different angle (Reposition handles that).
    const float skip = 0.55f;
    const x3::phys::Vec3 from{ muzzle.x + nd.x*skip,
                               muzzle.y + nd.y*skip,
                               muzzle.z + nd.z*skip };
    const float losLen = std::max(0.0f, std::min(dl, wp.range) - skip);
    if (losLen > 1e-3f) {
        x3::phys::RayHit friendly =
            physics.rayCast(from, nd, losLen, x3::phys::Layer::Player);
        if (friendly.hit) {
            // A friendly body is in the line-of-fire. Hold fire this frame.
            return;
        }
    }

    // ---- Real fire: raycast Enemy layer, damage on a live enemy body. ----
    x3::phys::RayHit hit = (losLen > 1e-3f)
        ? physics.rayCast(from, nd, losLen, x3::phys::Layer::Enemy)
        : x3::phys::RayHit{};

    // Re-arm the per-weapon cooldown + spend a round whether we hit or missed
    // (a fired shot costs ammo regardless — only the courtesy SKIP above is
    // free, because the trigger was held in that case).
    a.fireCooldown = wp.cooldown;
    a.magRemaining -= 1.0f;

    if (hit.hit && hit.body.valid()) {
        // Log only; the host's monster-side resolveHit applies the actual HP
        // change to the enemy. (We can't reach MonsterManager from here
        // without a header dep; the ally fire-resolver in the host owns that
        // wiring — same pattern as the player fire path.)
        x3::logInfo(std::string("[ally] ") + allyKindName(a.kind) +
                    " fired " + allyWeaponName(a.weapon) +
                    " — hit enemy body (dmg=" + std::to_string(wp.damage) + ")");
    } else {
        x3::logInfo(std::string("[ally] ") + allyKindName(a.kind) +
                    " fired " + allyWeaponName(a.weapon) + " — miss");
    }
}

// ---------------------------------------------------------------------------
// update — per-frame entry point. Dispatches each ally to its current-state
// tick, applies the next-state return, slews yaw, writes transforms back to
// physics + the scene Entity. Mirrors MonsterSystem::update's transform
// writeback shape so the scene's per-frame physics sync preserves the facing.
// ---------------------------------------------------------------------------
void AllyManager::update(float dt,
                         Scene& scene,
                         x3::phys::IPhysicsWorld& physics,
                         const x3::phys::Vec3& playerPos,
                         const HostileQueryFn& hostileQuery,
                         x3::ai::INavigation* nav) {
    (void)nav;   // optional; Phase B doesn't path-find — direct steps only.

    for (auto& a : m_allies) {
        if (!a.alive) continue;

        // Dispatch by current state. Each tick returns the NEXT state (may
        // equal current). The tick is responsible for moving the ally and
        // setting yawTarget; we slew yaw + write transforms after the switch.
        AllyState next = a.state;
        switch (a.state) {
            case AllyState::Follow:
                next = tickFollow(a, dt, playerPos, hostileQuery); break;
            case AllyState::Engage:
                next = tickEngage(a, dt, playerPos, hostileQuery, physics); break;
            case AllyState::Reposition:
                next = tickReposition(a, dt, playerPos, hostileQuery); break;
            case AllyState::Reload:
                next = tickReload(a, dt); break;
            case AllyState::TakeCover:
                next = tickTakeCover(a, dt, playerPos, hostileQuery); break;
            case AllyState::Search:
                next = tickSearch(a, dt, hostileQuery); break;
        }
        if (next != a.state) {
            // On entering Search, prime the LOS-lost memory window.
            if (next == AllyState::Search)  a.losMemory = kAllyLosLostMemory;
            // On entering TakeCover, reset the dwell counter (re-used field).
            if (next == AllyState::TakeCover) a.losMemory = 0.0f;
            x3::logInfo(std::string("[ally] ") + allyKindName(a.kind) +
                        " " + allyStateName(a.state) + " -> " + allyStateName(next) +
                        " (hp=" + std::to_string(a.hp) + ")");
            a.state = next;
        }

        // Slew yaw toward the state's target heading.
        a.yaw = slewAngle(a.yaw, a.yawTarget, kAllyTurnRate, dt);

        // Bake the heading into the render transform's upper-left 3x3 (yaw
        // about +Y under composeTRS) and refresh the translation column from
        // the moved (x,z) values the tick wrote to a.transform.
        const float c = std::cos(a.yaw), s = std::sin(a.yaw);
        const x3::phys::Vec3 t{ a.transform[12], a.transform[13], a.transform[14] };
        composeTRS(a.transform,
                   x3::phys::Vec3{ c, 0.0f, -s },
                   x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                   x3::phys::Vec3{ s, 0.0f, c },
                   1.0f, t);

        // Push to the physics body so allied raycasts/positions agree with
        // what's drawn (mirror of monster.cpp's setBodyPosition + setBodyRotation
        // dual-write). yaw about +Y -> quat (0, sin h, 0, cos h), w LAST.
        if (a.bodyId != 0) {
            x3::phys::BodyId bid{ a.bodyId };
            physics.setBodyPosition(bid, t);
            const float h = a.yaw * 0.5f;
            const float q[4] = { 0.0f, std::sin(h), 0.0f, std::cos(h) };
            physics.setBodyRotation(bid, q);
        }

        // Push to the scene entity so render() / Scene::update reads the
        // correct transform (visibility/hide on death is handled by Phase A).
        if (a.entityId != 0 && a.entityId < scene.size()) {
            Entity& e = scene.get(a.entityId);
            for (int i = 0; i < 16; ++i) e.transform[i] = a.transform[i];
            if (!a.alive) e.visible = false;
        }
    }
}

} // namespace x3::game
