// EFLZ canon-alien roster — see canon_aliens.h.
//
// Clean-room: built ONLY from X3Native's own monster.* roster + engine
// interfaces. NO game-engine source consulted.
#include "canon_aliens.h"
#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <memory>
#include <string>

namespace x3::game {

const char* canonAlienTypeName(CanonAlien t) {
    switch (t) {
        case CanonAlien::SaurianSoldier: return "Saurian Soldier";
        case CanonAlien::SaurianWarlord: return "Saurian Warlord";
        case CanonAlien::GreyTasked:     return "Grey Tasked";
        case CanonAlien::NordicSteward:  return "Nordic Steward";
        case CanonAlien::MantisArbiter:  return "Mantis Arbiter";
        case CanonAlien::Count:          return "?";
    }
    return "?";
}

namespace {

// SAURIAN SOLDIER — rank-and-file Reptilian Overlord enforcer. Built on the
// DominionTrooper melee Guard, scaled up to bruiser tier (apex predator).
// Visual: 6–8 ft armored saurian in a white exo-suit, scaled brown/green skin.
MonsterSystem::Tuning saurianSoldierTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::DominionTrooper);
    t.hp             = 160;                                  // bruiser tier (above Trooper)
    t.type           = MonsterType::Guard;
    t.ranged         = false;
    t.damage         = combat::kMeleeDamageMax;              // 10 — apex melee
    t.attackRange    = combat::kMeleeRange + 0.2f;           // tail reach
    t.attackCooldown = combat::kMeleeCooldownDefault;
    t.attackWindup   = combat::kMeleeWindup;
    t.chaseSpeed     = kChaseSpeed;
    t.aiStrafeBias   = 0.15f;                                // apex; charges, low strafe
    t.modelScale     = 1.10f;                                // 6–8 ft canon
    t.tint[0] = 0.55f; t.tint[1] = 0.50f; t.tint[2] = 0.30f; t.tint[3] = 1.0f;
    return t;
}

// SAURIAN WARLORD — Reptilian BOSS ("Zeth-Var"). 3-phase Boss with the
// memory-flash vulnerability window aligned to the Adaptive-Hide design (each
// phase transition opens a brief amplified-damage window — the player rotates
// damage types to exploit). Phase 3 summons Grey adds (canon).
// ADAPTIVE HIDE NOW LIVE (feat/adaptive-hide engine extension landed): the
// SaurianWarlord opts into the new MonsterSystem::Tuning::adaptiveHideResist
// (0.60 == 60% reduction on the matched-type window) + the canonical 8 s window.
// Memory-flash stays — stacks multiplicatively with the adaptive resist so a
// matched type during a flash is 1.5 * 0.4 = 0.6 (still reduced but less so —
// rotating during a flash is the "double-bonus" beat the spec wanted).
MonsterSystem::Tuning saurianWarlordTuning() {
    MonsterSystem::Tuning t = bossTuning(BossType::FailedExperiment7);
    t.hp                    = 540;                            // boss tier (~1.6x Martinez 340)
    t.type                  = MonsterType::Boss;
    t.ranged                = false;
    t.damage                = 18;                             // boss-tier melee (Martinez 15)
    t.attackRange           = combat::kMeleeRange + 0.4f;
    t.attackCooldown        = combat::kMeleeCooldownDefault;
    t.chaseSpeed            = kChaseSpeed * 1.10f;
    t.aiStrafeBias          = 0.10f;                          // apex bruiser
    t.modelScale            = 1.20f;
    t.tint[0] = 0.45f; t.tint[1] = 0.40f; t.tint[2] = 0.25f; t.tint[3] = 1.0f;
    // Phase machine — defaults (Phase2 @ 66%, Phase3 @ 33%).
    t.phase2Frac            = 0.66f;
    t.phase3Frac            = 0.33f;
    t.phase2SpeedMul        = 1.30f;
    t.phase2DamageMul       = 1.40f;
    t.phase3SpeedMul        = 1.60f;
    t.phase3DamageMul       = 1.70f;
    t.phase3SummonCount     = 2;                              // P3 summons Grey adds
    // Memory-flash = a brief amplified-vulnerability window per phase transition
    // (1.2 s @ 1.5x). Stacks multiplicatively with Adaptive Hide below.
    t.memoryFlashTime       = 1.2f;
    t.memoryFlashDamageMul  = 1.5f;
    // Adaptive Hide (canon) — 60% resist to the LAST incoming damage type for
    // 8 s. Player MUST rotate (Kinetic -> Energy -> Bio -> ...) to keep damage
    // flowing. Inert until the feat/adaptive-hide engine extension lands; once
    // it does, this is the canonical Warlord fight rhythm.
    t.adaptiveHideResist      = 0.60f;
    t.adaptiveHideDurationSec = 8.0f;
    return t;
}

// GREY TASKED — synthetic worker-drone serving the Reptilians. Ranged kiter,
// fragile, recon-leaning. Visual: 4–5 ft slender grey with large black eyes.
// (Liberation / Override post-MVP — engine extension on monster.*.)
MonsterSystem::Tuning greyTaskedTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::BlueSynth);
    t.hp             = 70;                                   // fragile (scaled from spec 40)
    t.type           = MonsterType::Drone;
    t.ranged         = true;
    t.damage         = combat::kRangedDamageMin;             // 4 — light beam
    t.attackRange    = kFireMaxDist;
    t.attackCooldown = combat::kRangedCooldownDefault;
    t.chaseSpeed     = kChaseSpeed * 0.80f;                  // kites, doesn't close
    t.standoff       = combat::kRangedStandoff * 1.40f;      // ~9.8 m — keeps distance
    t.aiStrafeBias   = 0.85f;                                // heavy kite/flank
    t.modelScale     = 0.75f;                                // 4–5 ft canon
    t.flyer          = false;
    t.tint[0] = 0.65f; t.tint[1] = 0.66f; t.tint[2] = 0.70f; t.tint[3] = 1.0f;
    return t;
}

// NORDIC STEWARD — peaceful mentor (allied). Tall human-like, white luminous
// suit, long blond hair. Built on the Salvari ally pattern: startAllied + 0
// damage + stationary marker. (Aegis Field / Foresight / Uplift = engine
// extensions; documented for the support-mech lane.)
MonsterSystem::Tuning nordicStewardTuning() {
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::SalvariAlly);
    t.hp             = 120;                                  // tankier than typical ally
    t.chaseSpeed     = 0.0f;                                 // stationary mentor (K'thara-like)
    t.modelScale     = 1.05f;                                // 6–7.5 ft canon
    t.startAllied    = true;                                 // (inherited; explicit for clarity)
    // Re-tint from bioluminescent teal -> luminous Nordic white.
    t.tint[0] = 0.92f; t.tint[1] = 0.94f; t.tint[2] = 0.98f; t.tint[3] = 1.0f;
    return t;
}

// MANTIS ARBITER — insectoid stealth assassin (wildcard). Built on the Verthani
// strafe-heavy melee, sped up to assassin tier. (Veil stealth + Mind-Spike
// armor-pierce = engine extensions.)
MonsterSystem::Tuning mantisArbiterTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::Verthani);
    t.hp             = 100;
    t.type           = MonsterType::Guard;
    t.ranged         = false;
    t.damage         = combat::kMeleeDamageDefault;          // 8 — psychic spike (TODO: pierce)
    t.attackRange    = combat::kMeleeRange;
    t.attackCooldown = combat::kMeleeCooldownMin;            // 1.0 — fast strikes
    t.chaseSpeed     = kChaseSpeed * 1.50f;                  // very fast
    t.aiStrafeBias   = 0.90f;                                // extreme flanker
    t.modelScale     = 1.05f;                                // 6–7 ft canon
    t.tint[0] = 0.45f; t.tint[1] = 0.62f; t.tint[2] = 0.30f; t.tint[3] = 1.0f; // mantis green
    return t;
}

// Build the static canon-alien table once. Mirrors buildAct2EnemyDefs().
std::vector<CanonAlienDef> buildCanonAlienDefs() {
    std::vector<CanonAlienDef> defs;
    defs.reserve((size_t)CanonAlien::Count);
    defs.push_back({ CanonAlien::SaurianSoldier, "Saurian Soldier", saurianSoldierTuning() });
    defs.push_back({ CanonAlien::SaurianWarlord, "Saurian Warlord", saurianWarlordTuning() });
    defs.push_back({ CanonAlien::GreyTasked,     "Grey Tasked",     greyTaskedTuning()    });
    defs.push_back({ CanonAlien::NordicSteward,  "Nordic Steward",  nordicStewardTuning() });
    defs.push_back({ CanonAlien::MantisArbiter,  "Mantis Arbiter",  mantisArbiterTuning() });
    return defs;
}

} // namespace

const std::vector<CanonAlienDef>& canonAlienDefs() {
    static const std::vector<CanonAlienDef> defs = buildCanonAlienDefs();
    return defs;
}

const CanonAlienDef& canonAlienDef(CanonAlien t) {
    const std::vector<CanonAlienDef>& defs = canonAlienDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];
    for (const CanonAlienDef& d : defs) if (d.type == t) return d;
    return defs[0];
}

MonsterSystem::Tuning canonAlienTuning(CanonAlien t) {
    return canonAlienDef(t).tuning;
}

// ============================================================================
// Headless self-test (--test-canonaliens).
// ============================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[canonaliens-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[canonaliens-test] FAIL ") + name); }
}

bool tuningEq(const MonsterSystem::Tuning& a, const MonsterSystem::Tuning& b) {
    return a.hp == b.hp && a.damage == b.damage && a.type == b.type &&
           a.ranged == b.ranged && a.chaseSpeed == b.chaseSpeed &&
           a.attackRange == b.attackRange && a.standoff == b.standoff &&
           a.aiStrafeBias == b.aiStrafeBias && a.startAllied == b.startAllied;
}

} // namespace

bool runCanonAliensSelfTest() {
    g_pass = g_fail = 0;
    using HeadlessDevice = x3::game::HeadlessRenderDevice;
    HeadlessDevice device;

    // ---- T0: roster complete + ordered. ----
    {
        const auto& roster = canonAlienDefs();
        bool complete = roster.size() == (size_t)CanonAlien::Count;
        bool ordered  = true;
        for (uint32_t i = 0; i < roster.size(); ++i)
            if ((uint32_t)roster[i].type != i) ordered = false;
        check(complete && ordered, "T0 canon roster has one row per CanonAlien, in enum order");
    }

    // ---- T1: each species BUILDS with its table stats. ----
    {
        int builtOk = 0;
        for (uint32_t i = 0; i < (uint32_t)CanonAlien::Count; ++i) {
            const CanonAlien et = (CanonAlien)i;
            const CanonAlienDef& def = canonAlienDef(et);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            Scene scene;
            MonsterSystem m;
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.0f, 0.0f }, def.tuning);
            const bool built = m.alive() && m.entity() != kNoLink &&
                               m.maxHp() == def.tuning.hp &&
                               m.ranged() == def.tuning.ranged &&
                               m.type() == def.tuning.type;
            if (built) ++builtOk;
            else x3::logError(std::string("[canonaliens-test] build failed: ") + def.name);
            w->shutdown();
        }
        check(builtOk == (int)CanonAlien::Count, "T1 each canon-alien BUILDS with its table stats");
    }

    // ---- T2: Saurian Soldier — hostile melee Guard, HP >= 100. ----
    {
        const auto& d = canonAlienDef(CanonAlien::SaurianSoldier);
        check(d.tuning.damage > 0 && !d.tuning.ranged && d.tuning.hp >= 100 &&
              d.tuning.type == MonsterType::Guard,
              "T2 Saurian Soldier: hostile melee Guard, HP >= 100");
    }
    // ---- T3: Saurian Warlord — Boss + phases + memory-flash. ----
    {
        const auto& d = canonAlienDef(CanonAlien::SaurianWarlord);
        check(d.tuning.type == MonsterType::Boss && d.tuning.hp >= 400 &&
              d.tuning.phase2Frac > 0.0f && d.tuning.phase3Frac > 0.0f &&
              d.tuning.memoryFlashTime > 0.0f && d.tuning.memoryFlashDamageMul > 1.0f,
              "T3 Saurian Warlord: Boss + HP>=400 + phase machine + memory-flash vulnerability");
    }
    // ---- T4: Grey Tasked — ranged Drone, fragile, high standoff. ----
    {
        const auto& d = canonAlienDef(CanonAlien::GreyTasked);
        check(d.tuning.ranged && d.tuning.damage > 0 && d.tuning.hp < 100 &&
              d.tuning.type == MonsterType::Drone &&
              d.tuning.standoff >= combat::kRangedStandoff * 1.2f,
              "T4 Grey Tasked: ranged Drone, fragile (HP<100), high standoff");
    }
    // ---- T5: Nordic Steward — allied + stationary. ----
    {
        const auto& d = canonAlienDef(CanonAlien::NordicSteward);
        check(d.tuning.startAllied && d.tuning.chaseSpeed == 0.0f,
              "T5 Nordic Steward: startAllied + stationary (chaseSpeed 0)");
    }
    // ---- T6: Mantis Arbiter — hostile, fast, extreme strafe. ----
    {
        const auto& d = canonAlienDef(CanonAlien::MantisArbiter);
        check(d.tuning.damage > 0 && !d.tuning.ranged &&
              d.tuning.chaseSpeed > kChaseSpeed &&
              d.tuning.aiStrafeBias > 0.8f,
              "T6 Mantis Arbiter: hostile, fast (chase > default), aggressive strafe (>0.8)");
    }
    // ---- T7: rows are DISTINCT. ----
    {
        const auto& roster = canonAlienDefs();
        bool distinct = true;
        for (size_t i = 0; i < roster.size(); ++i)
            for (size_t j = i + 1; j < roster.size(); ++j)
                if (tuningEq(roster[i].tuning, roster[j].tuning)) distinct = false;
        check(distinct, "T7 canon-alien rows are DISTINCT (no duplicate stat blocks)");
    }

    x3::logInfo(std::string("canonaliens: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
