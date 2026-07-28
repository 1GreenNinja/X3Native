#pragma once
// PACK-HARVEST ARACHNIDS — the first enemy in the bestiary whose skeleton AND
// animation came straight out of Tim's licensed Unity asset library instead of
// being authored/baked here.
//
// WHY THIS ROW EXISTS: every "creature" enemy before this one rode either a
// single-bone fake rig (tools/animate_creature.py bakes a core gait onto ONE
// root bone) or a retargeted humanoid. A spider is neither — it needs eight
// independently articulated legs, which no procedural gait we own can fake. The
// pack ships exactly that: a 70-bone arachnid rig with authored idle / walk /
// attack / death, already game-scale, for free. Harvesting it is the proof of
// concept for mining the rest of the pack library the same way.
//
// SOURCE: "Spiders - characters with animations" (licensed, owned) — every
// species FBX in that pack is self-contained (mesh + all six anim stacks on the
// same 70-bone rig), so one headless Blender pass per spider is the whole
// pipeline. See tools/prep_pack_spider.py for the three gotchas it fixes
// (transparent-by-default FBX material, non-canonical clip names, backwards
// facing) and PROVENANCE.md for the harvest record.
//
// CLEAN-ROOM: built ONLY from X3Native's own monster.* roster idiom (the same
// shape as canon_aliens.* / the Act-2 roster). NO other game-engine source.
//
// WHAT THIS MODULE DOES: hands back spawn-ready MonsterSystem::Tuning rows for
// the arachnids, pointed at their harvested GLBs in assets/rigged_glb. Spawn
// them exactly like any other roster row (MonsterManager::spawn /
// buildMonsterTuned). No new engine fields, no new AI lane — the pack's clips
// are RENAMED at harvest time to the names MonsterSystem's fuzzy clip resolver
// already looks for (Idle/Walk/Run/Attack/Attack2/Death), so every clip slot
// binds with ZERO MonsterSystem::overrideClip calls at the call site.

#include "monster.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// One row per harvested arachnid. Both ride the SAME 70-bone rig and the SAME
// clip set (they are two species out of the one pack), so they differ only in
// stats, scale and skin — which is exactly what makes pack harvesting cheap.
enum class PackSpider : uint32_t {
    LabSkitterer = 0,  // brown tarantula — fast, fragile swarm melee
    VenomBrood   = 1,  // blue tarantula — big, slow, heavy-hitting bruiser
    Count        = 2
};

// Human-readable name (logs / --test-packspiders trace / HUD).
const char* packSpiderTypeName(PackSpider t);

// One roster row. Same shape as CanonAlienDef / MonsterDef / Act2EnemyDef.
struct PackSpiderDef {
    PackSpider            type;
    const char*           name;
    MonsterSystem::Tuning tuning;
};

// The full arachnid table (one row per PackSpider, in enum order). Built once.
const std::vector<PackSpiderDef>& packSpiderDefs();
// Fetch one row by enum id (asserts enum order; defensive linear fallback).
const PackSpiderDef& packSpiderDef(PackSpider t);
// Convenience: a spawn-ready Tuning copy.
MonsterSystem::Tuning packSpiderTuning(PackSpider t);

// Headless self-test (--test-packspiders). No window, no Vulkan. Asserts:
//   (T0) the roster is complete + ordered.
//   (T1) every row BUILDS on a HeadlessDevice + Jolt world with its table stats.
//   (T2) LabSkitterer: hostile melee Guard, fragile, faster than the default chase.
//   (T3) VenomBrood: hostile melee, tankier + harder-hitting + visibly bigger.
//   (T4) rows are DISTINCT.
//   (T5) ART ATTACHED — every row names a harvested GLB that is present on disk.
//   (T6) THE HARVEST GATE — the GLB actually parses as a RIGGED, MULTI-CLIP
//        arachnid: >= kMinSpiderJoints skinned joints (a fake single-bone rig
//        can never pass) and every canonical clip present with real motion.
//   (T7) CLIP BINDING — a built spider resolves Idle/Walk/Run/Attack/Attack2/
//        Death through MonsterSystem's own fuzzy resolver (no overrideClip), and
//        the locomotion blend is armed.
// Prints "packspiders: X/Y passed"; returns true iff all pass.
bool runPackSpidersSelfTest();

} // namespace x3::game
