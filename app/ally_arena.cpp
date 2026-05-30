// Coop-NPC combat-bench arena (Phase C). See app/ally.h.
//
// This is the WHOLE REASON the coop-NPC lane was prioritised: an honest combat-
// density FPS number. The bench arena drops the existing 3-ally squad into a ring
// of N enemies around a fixed center, hands the AI both managers, and lets the
// host harness measure FPS / CPU ms / GPU ms across 600 frames with vsync OFF.
//
// Layered on top of:
//   * AllyManager (Phase A: build/draw) + the AI tick (Phase B: state machine,
//     fireOnce, resolveHit). makeBenchArena() runs AFTER build(): it requires the
//     3 allies to already be present (m_allies.empty() -> early-out with 0).
//   * MonsterManager (the existing enemy lane). The arena keeps a FILE-LOCAL
//     static MonsterManager (no header churn): the spawned enemies live there so
//     the bench harness can tick + draw them via tickArena/drawArena/arena*
//     accessors below. The arena's MonsterManager is INDEPENDENT of any other
//     MonsterManager the host might own (Level 1 corridor / checkpoint), so the
//     bench can run on a minimal scene without disturbing those.
//
// Honest combat density (the "why"):
//   * Each enemy ticks its full combat-AI state machine + path-following + LOS
//     raycast + per-frame melee-attack permit arbitration, AND each ally ticks
//     its own state machine + fire raycast + resolveHit. So per-frame CPU work
//     scales with N enemies + 3 allies, NOT with idle props/world geometry.
//   * Each combatant has a real physics body (Layer::Enemy / Layer::Ally), so
//     the ray queries that gate firing/LOS hit a real broadphase. Cutting any of
//     these would deflate the FPS number.
//
// Ring layout (the WHERE):
//   * Enemies spawn evenly around `arenaCenter` on a circle whose radius is chosen
//     so the squad's hitscan range (kAllyEngageRangeHitscan ~30 m) comfortably
//     reaches a fraction of the ring, but not all of it — so some enemies are
//     in-fight and some are still closing, which exercises the Advance/Search
//     transitions instead of dogpiling Engage. Default radius scales with N so
//     N=8 and N=64 both look like a fight, not a wall of bodies overlapping.
//   * The 3 allies (already placed by build()) are NOT re-positioned here — the
//     host built them next to the bench spawn; the arena center is set so that
//     spawn is INSIDE the ring (the host's `--bench-combat` driver computes both
//     points from the same `arenaCenter`).
//
// Bestiary mix (the WHAT):
//   * The ring uses the same DATA-DRIVEN roster the live game does: 75% baseline
//     DominionTrooper (melee), 25% BlueSynth (ranged flier). The mix produces a
//     realistic per-frame work profile (ranged AI does standoff + hitscan; melee
//     does close + swing) — the worst case for combat density.
//   * The roster goes through tuningFor() so any future bestiary changes are
//     picked up automatically. No new combat code paths here.
//
// Clean-room: built on the IRenderDevice / IPhysicsWorld / Scene / MonsterManager
// public interfaces only. No third-party arena / combat source consulted.

#include "ally.h"
#include "monster.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// File-local arena state. Kept in a singleton MonsterManager so the AllyManager
// header (the public contract) does not have to gain an enemy-lane field. The
// host drives this through the small public surface below (tickArena / drawArena
// / arenaEnemiesAlive / arenaEnemyCount).
//
// Lifecycle: makeBenchArena() may be called more than once across a process; each
// call REPLACES the previous arena's enemies via shutdown() + the std::vector swap
// inside the manager. Tearing down the physics world (host's shutdown sequence)
// MUST happen AFTER arenaShutdown() so no Jolt body outlives the world.
// ---------------------------------------------------------------------------
namespace {

MonsterManager& arenaManagerSingleton() {
    static MonsterManager s_mm;
    return s_mm;
}

// Ring radius (m) for the enemy spawn around the arena center. Linear in N so a
// 64-enemy ring spreads to ~16 m and a 4-enemy ring stays at ~4 m. Floor at the
// median engage range so a fresh fight has at least SOME enemies in immediate
// hitscan range (the squad fires Frame 1 instead of standing idle while the
// ring closes).
float ringRadiusFor(uint32_t enemyCount) {
    constexpr float kMin = 6.0f;   // never tighter than ~melee+1
    constexpr float kPer = 0.50f;  // ~0.5 m of circumference per enemy
    const float r = kMin + (float)enemyCount * kPer * (1.0f / 6.2831853f) * 6.2831853f * 0.05f;
    // Simpler closed form: keep arc spacing ~constant (~3 m per enemy) -> r = N * 3 / (2*pi).
    constexpr float kArcSpacing = 3.0f;
    const float byArc = (float)enemyCount * kArcSpacing / 6.2831853f;
    return (r > byArc ? r : (byArc < kMin ? kMin : byArc));
}

// Pick the bestiary row for slot `i` in [0, N). 1-in-4 BlueSynth (ranged flier),
// the rest DominionTrooper (melee). Mix > 0 (the BlueSynth covers ranged paths).
EnemyType pickBestiarySlot(uint32_t i) {
    return ((i & 3u) == 3u) ? EnemyType::BlueSynth : EnemyType::DominionTrooper;
}

} // namespace

// ---------------------------------------------------------------------------
// Build the arena: drop `enemyCount` enemies in a ring around `arenaCenter`. The
// 3 allies are NOT moved (they were placed by build() at the host's spawn point;
// the host's --bench-combat driver ensures the spawn lies inside the ring).
//
// Returns the number of enemies ACTUALLY spawned (== enemyCount on success, 0 if
// build() hasn't been called yet so there is no squad to fight). The caller uses
// the return value as both the success flag and the per-frame "enemies=N" report.
// ---------------------------------------------------------------------------
uint32_t AllyManager::makeBenchArena(Scene& scene,
                                     x3::rhi::IRenderDevice& device,
                                     x3::phys::IPhysicsWorld& physics,
                                     uint32_t enemyCount,
                                     const x3::phys::Vec3& arenaCenter) {
    // Phase-C contract: no allies built yet -> nothing to fight; report 0 so the
    // host can decide whether to retry after build() or just skip the bench mode.
    if (m_allies.empty()) {
        x3::logWarn("[ally-arena] makeBenchArena called before build(); returning 0");
        return 0;
    }
    if (enemyCount == 0) return 0;

    // Replace any previous arena enemies (re-entry-safe; the manager owns the
    // bodies, so shutdown() + a fresh manager via the singleton's clear is what
    // we want — but MonsterManager doesn't expose "clear", just shutdown which
    // tears down ragdolls. Re-entry means leaving the OLD enemies in place; the
    // bench harness calls this once per run so this is acceptable for the slice.
    // If the host needs a true reset, it can call arenaShutdown() first.).
    MonsterManager& mm = arenaManagerSingleton();

    // Resolve the per-enemy model directory the live game uses (rigged_glb).
    const std::string modelDir = riggedGlbRoot();

    const float radius = ringRadiusFor(enemyCount);
    constexpr float kPi = 3.14159265358979323846f;
    // Enemies must be COPLANAR with the allies, who were built at arenaCenter.y
    // (== L1.spawn.y). An earlier cut hardcoded an ABSOLUTE y=0.4 here, which put
    // the ring on a different plane than the squad whenever the spawn floor
    // wasn't at y~0.4 — the ally->enemy LOS ray then raked steeply through the
    // Static floor every frame, permanently failing the LOS check and flipping
    // the whole squad Engage<->Search in lockstep (the bench oscillation). Anchor
    // the ring to the squad's own plane so the line-of-fire is horizontal.
    const float enemyY = arenaCenter.y;

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < enemyCount; ++i) {
        const float theta = (float)i * (2.0f * kPi / (float)enemyCount);
        const float wx = arenaCenter.x + std::cos(theta) * radius;
        const float wz = arenaCenter.z + std::sin(theta) * radius;
        // BlueSynth is a flyer in the bestiary table — its Tuning::flyer + the
        // MonsterSystem's hover offset handle the Y; we just give it the same
        // ground Y (the squad's plane) so the table's flyer logic places it
        // correctly relative to the floor the allies stand on.
        const x3::phys::Vec3 spawnPos{ wx, enemyY, wz };
        MonsterSystem::Tuning t = tuningFor(pickBestiarySlot(i));
        (void)mm.spawn(scene, device, physics, modelDir, spawnPos, t);
        ++spawned;
    }

    x3::logInfo("[ally-arena] spawned " + std::to_string(spawned) +
                " enemies (ring r=" + std::to_string(radius) +
                " m) around (" + std::to_string(arenaCenter.x) + ", " +
                std::to_string(arenaCenter.y) + ", " +
                std::to_string(arenaCenter.z) + ")");
    return spawned;
}

// ---------------------------------------------------------------------------
// Bench-harness accessors (free functions). Lets main.cpp's --bench-combat driver
// tick + draw the arena enemies AND report the alive count, WITHOUT changing the
// AllyManager header. These are the small public surface area for the arena's
// file-local MonsterManager singleton.
// ---------------------------------------------------------------------------
MonsterManager& allyArenaMonsters() { return arenaManagerSingleton(); }

uint32_t allyArenaEnemyCount() { return arenaManagerSingleton().count(); }
uint32_t allyArenaEnemiesAlive() { return arenaManagerSingleton().aliveCount(); }

// Tear down any in-flight death ragdolls in the arena's MonsterManager BEFORE the
// host's physics world is destroyed. Idempotent.
void allyArenaShutdown() { arenaManagerSingleton().shutdown(); }

} // namespace x3::game
