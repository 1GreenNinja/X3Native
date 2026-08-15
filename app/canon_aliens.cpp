// EFLZ canon-alien roster — see canon_aliens.h.
//
// Clean-room: built ONLY from X3Native's own monster.* roster + engine
// interfaces. NO game-engine source consulted.
#include "canon_aliens.h"
#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <filesystem>
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

// CANON ALIENS art pass: point a roster row at its Rodin-built GLB in
// rigged_glb, preferring the multi-clip "<stem>_anim.glb" (Idle/Walk/Run baked
// by tools/animate_creature.py, core gait) when present — mirrors monster.cpp's
// defRigged(). The canon GLBs are authored Y-up, feet at y=0, and PRE-SCALED so
// authoredHeight x the row's designed modelScale = the lore height (Grey 1.2 m,
// Nordic 2.0 m, Mantis 2.2 m, Saurian 2.4 m; Warlord reads bigger via its own
// modelScale on the same GLB) — the tested tunings stay untouched.
void setCanonModel(MonsterSystem::Tuning& t, const char* stem) {
    namespace fs = std::filesystem;
    const std::string riggedDir = riggedGlbRoot();
    const std::string base(stem);
    std::string chosen = base + ".glb";
    std::error_code ec;
    if (fs::exists(fs::path(riggedDir) / (base + "_anim.glb"), ec))
        chosen = base + "_anim.glb";
    t.modelFile        = chosen;
    t.modelDirOverride = riggedDir;
    t.standUpZtoY      = false;   // canon alien GLBs are authored Y-up
}

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
    t.modelScale     = 1.00f;                                // 6–8 ft canon
    t.tint[0] = 0.55f; t.tint[1] = 0.50f; t.tint[2] = 0.30f; t.tint[3] = 1.0f;
    setCanonModel(t, "canon_saurian");             // authored 2.396 m -> x1.00 = 2.4 m lore
    return t;
}

// SAURIAN WARLORD — Reptilian BOSS ("Zeth-Var"). 3-phase Boss with the
// memory-flash vulnerability window aligned to the Adaptive-Hide design (each
// phase transition opens a brief amplified-damage window — the player rotates
// damage types to exploit). Phase 3 summons Grey adds (canon).
// FORWARD-SPEC: full Adaptive Hide (last-damage-type 60% resist for 8 s) needs a
// new MonsterSystem::Tuning::adaptiveHideResist field + a per-frame
// last-damage tracker on MonsterSystem (monster.* lane). Until that lands, the
// phase machine + memoryFlash already gives the "rotate damage type" rhythm.
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
    t.modelScale            = 1.09f;
    t.tint[0] = 0.45f; t.tint[1] = 0.40f; t.tint[2] = 0.25f; t.tint[3] = 1.0f;
    // Phase machine — defaults (Phase2 @ 66%, Phase3 @ 33%).
    t.phase2Frac            = 0.66f;
    t.phase3Frac            = 0.33f;
    t.phase2SpeedMul        = 1.30f;
    t.phase2DamageMul       = 1.40f;
    t.phase3SpeedMul        = 1.60f;
    t.phase3DamageMul       = 1.70f;
    t.phase3SummonCount     = 2;                              // P3 summons Grey adds
    // Memory-flash = the "rotate damage type" vulnerability beat (1.2 s @ 1.5x).
    t.memoryFlashTime       = 1.2f;
    t.memoryFlashDamageMul  = 1.5f;
    setCanonModel(t, "canon_saurian");             // same GLB as the Soldier; x1.09 = 2.62 m boss bulk
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
    setCanonModel(t, "canon_grey");                // authored 1.6 m -> x0.75 = 1.2 m lore
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
    setCanonModel(t, "canon_nordic");              // authored 1.905 m -> x1.05 = 2.0 m lore
    return t;
}

// MANTIS ARBITER — insectoid stealth assassin (wildcard mini-boss). Built on
// the Verthani strafe-heavy melee, sped up to assassin tier, with a 3-phase
// rage escalation (each transition the carapace bruises darker, speed + damage
// climb, and a brief memory-flash vulnerability window opens). The canon
// "send-off" log line on death is hooked via the existing BossPhase machine —
// runs as the Phase3->dead transition. (Veil stealth + Mind-Spike armor-pierce
// = engine extensions still pending.)
MonsterSystem::Tuning mantisArbiterTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::Verthani);
    t.hp             = 220;                                  // mini-boss tier (~Warlord/2.5)
    t.type           = MonsterType::Boss;                    // promoted from Guard for the phase machine
    t.ranged         = false;
    t.damage         = combat::kMeleeDamageDefault;          // 8 — psychic spike (TODO: pierce)
    t.attackRange    = combat::kMeleeRange;
    t.attackCooldown = combat::kMeleeCooldownMin;            // 1.0 — fast strikes
    t.chaseSpeed     = kChaseSpeed * 1.50f;                  // very fast base
    t.aiStrafeBias   = 0.90f;                                // extreme flanker
    t.modelScale     = 1.05f;                                // 6–7 ft canon
    t.tint[0] = 0.45f; t.tint[1] = 0.62f; t.tint[2] = 0.30f; t.tint[3] = 1.0f; // mantis green base
    // ---- Phase machine: rage escalation. ----
    t.phase2Frac      = 0.66f;
    t.phase3Frac      = 0.33f;
    t.phase2SpeedMul  = 1.40f;                               // faster
    t.phase3SpeedMul  = 1.70f;                               // desperate-fast
    t.phase2DamageMul = 1.30f;                               // harder strikes
    t.phase3DamageMul = 1.60f;                               // berserker
    t.phase2ScaleMul  = 1.00f;                               // no growth — keep silhouette
    t.phase3ScaleMul  = 1.00f;
    // Carapace bruises darker as phases escalate (subtle red push — angrier insect).
    // phaseTintMul defaults to {1,1,1}; tinted per-phase in the host's onPhase hook.
    // Memory-flash: a brief 0.8 s amplified-vulnerability window per phase
    // transition — the canon "she's open" beat the player rotates types into.
    t.memoryFlashTime      = 0.8f;
    t.memoryFlashDamageMul = 1.35f;
    setCanonModel(t, "canon_mantis");              // authored 2.095 m -> x1.05 = 2.2 m lore
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
    // ---- T6b: Mantis Arbiter is now a Boss with the rage-phase machine + a
    // memory-flash vulnerability window per phase (canon-aliens c8 polish). ----
    {
        const auto& d = canonAlienDef(CanonAlien::MantisArbiter);
        const bool isBoss   = d.tuning.type == MonsterType::Boss;
        const bool hpUp     = d.tuning.hp >= 200;                                 // mini-boss tier
        const bool phases   = d.tuning.phase2Frac > d.tuning.phase3Frac &&
                              d.tuning.phase3Frac > 0.0f && d.tuning.phase2Frac < 1.0f;
        const bool escalate = d.tuning.phase3SpeedMul  > d.tuning.phase2SpeedMul &&
                              d.tuning.phase3DamageMul > d.tuning.phase2DamageMul &&
                              d.tuning.phase2SpeedMul  > 1.0f &&
                              d.tuning.phase2DamageMul > 1.0f;
        const bool flash    = d.tuning.memoryFlashTime > 0.0f &&
                              d.tuning.memoryFlashDamageMul > 1.0f;
        check(isBoss && hpUp && phases && escalate && flash,
              "T6b Mantis Arbiter mini-boss: Boss + HP>=200 + rage phase machine (escalating) + memory-flash window");
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
    // ---- T8: ART ATTACHED — every row names a real canon GLB on disk (the
    // CANON ALIENS wave: no row rides the alien_crawler fallback any more). ----
    {
        namespace fs = std::filesystem;
        int wired = 0;
        for (uint32_t i = 0; i < (uint32_t)CanonAlien::Count; ++i) {
            const CanonAlienDef& d = canonAlienDef((CanonAlien)i);
            const bool named  = !d.tuning.modelFile.empty() &&
                                !d.tuning.modelDirOverride.empty();
            std::error_code ec;
            const bool onDisk = named &&
                fs::exists(fs::path(d.tuning.modelDirOverride) / d.tuning.modelFile, ec);
            if (named && onDisk) ++wired;
            else x3::logError(std::string("[canonaliens-test] model missing: ") + d.name +
                              " -> " + d.tuning.modelFile);
        }
        check(wired == (int)CanonAlien::Count,
              "T8 every canon-alien row has its canon GLB wired + present on disk");
    }

    // ---- T9: SHOOTABLE — every HOSTILE canon alien takes player fire. Regression
    // guard for the "some monsters cannot be shot" playtest bug: the planet aliens
    // ship with freshly Meshy-rigged GLBs (new authored heights) AND the app spawns
    // them into their OWN MonsterManager (app_run canonAliens). This test proves two
    // things at once: (a) the Enemy-layer hit body derived for each new GLB actually
    // covers the torso (a shot at chest height REGISTERS), and (b) routing a shot
    // through the manager that OWNS the alien lands damage. The shipped bug was (b):
    // canonAliens was left out of app_run's fire-dispatch chain, so shots passed
    // through. Fire each hostile row via its manager and assert HP drops. The allied
    // Nordic Steward is EXCLUDED (startAllied — never meant to be shot). ----
    {
        using HeadlessDevice = x3::game::HeadlessRenderDevice;
        HeadlessDevice fdev;
        const CanonAlien hostiles[] = {
            CanonAlien::SaurianSoldier, CanonAlien::GreyTasked,
            CanonAlien::MantisArbiter,  CanonAlien::SaurianWarlord,
        };
        int shot = 0;
        for (CanonAlien ca : hostiles) {
            const CanonAlienDef& d = canonAlienDef(ca);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            Scene scene;
            MonsterManager mgr;
            const uint32_t idx = mgr.spawn(scene, fdev, *w, riggedGlbRoot(),
                                           x3::phys::Vec3{ 0.0f, 0.0f, 0.0f }, d.tuning);
            const int hp0 = mgr.at(idx).maxHp();
            // Eye 3 m in front (toward the alien along +Z) at torso height. The ground
            // hit box spans feet-0.4 .. feet + ~2*scale, so a chest-height ray hits.
            const x3::phys::Vec3 eye{ 0.0f, 1.0f, -3.0f };
            const x3::phys::Vec3 dir{ 0.0f, 0.0f, 1.0f };
            FireResult r = mgr.fire(eye, dir, scene, *w, 30, x3::DamageType::Kinetic);
            const bool damaged = r.hitMonster && mgr.at(idx).hp() < hp0;
            if (damaged) ++shot;
            else x3::logError(std::string("[canonaliens-test] NOT SHOOTABLE: ") + d.name +
                              " (hitMonster=" + std::to_string(r.hitMonster) +
                              " hp " + std::to_string(mgr.at(idx).hp()) + "/" +
                              std::to_string(hp0) + ")");
            mgr.shutdown();
            w->shutdown();
        }
        check(shot == (int)(sizeof(hostiles) / sizeof(hostiles[0])),
              "T9 every HOSTILE canon alien takes player fire (chest-height ray drops HP)");
    }

    // ---- T10: DISPATCH root-cause reproduction. The shipped bug was NOT a bad hit
    // body (T9 proves the body is fine) — it was that the player-fire dispatch chain
    // OMITTED the canonAliens manager. MonsterManager::fire() casts ONE Enemy-layer
    // ray and only damages a body it OWNS; a hit on another manager's Enemy body is
    // kept as a mere geometry hit (hitMonster=false, no damage) — so an alien routed
    // to the WRONG manager silently absorbs the shot. This test reproduces exactly
    // that: fire through a manager that does NOT own the alien (a decoy monster sits
    // behind it, so the manager's ray still returns the alien as the nearest Enemy
    // body) -> the alien takes NO damage. Then fire through the alien's OWN manager
    // -> it DOES. That is the before/after of the app_run.cpp routing fix. ----
    {
        using HeadlessDevice = x3::game::HeadlessRenderDevice;
        HeadlessDevice fdev;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        Scene scene;
        MonsterManager alienMgr;   // owns the canon alien (app_run `canonAliens`)
        MonsterManager decoyMgr;   // a DIFFERENT group that receives the shot instead

        const CanonAlienDef& d = canonAlienDef(CanonAlien::MantisArbiter);
        const uint32_t ai = alienMgr.spawn(scene, fdev, *w, riggedGlbRoot(),
                                           x3::phys::Vec3{ 0.0f, 0.0f, 0.0f }, d.tuning);
        // Decoy sits BEHIND the alien (further along +Z) so the shared ray's nearest
        // Enemy body is still the alien — the decoy manager "sees" a foreign hit.
        decoyMgr.spawn(scene, fdev, *w, riggedGlbRoot(),
                       x3::phys::Vec3{ 0.0f, 0.0f, 6.0f },
                       tuningFor(EnemyType::DominionTrooper));

        const x3::phys::Vec3 eye{ 0.0f, 1.0f, -3.0f };
        const x3::phys::Vec3 dir{ 0.0f, 0.0f, 1.0f };
        const int hpFull = alienMgr.at(ai).maxHp();

        // BUG path: shot routed only to the decoy manager -> alien absorbs, no damage.
        FireResult bug = decoyMgr.fire(eye, dir, scene, *w, 40, x3::DamageType::Kinetic);
        const bool bugAbsorbed = !bug.hitMonster && alienMgr.at(ai).hp() == hpFull;

        // FIX path: shot routed to the alien's OWN manager -> damage lands.
        FireResult fix = alienMgr.fire(eye, dir, scene, *w, 40, x3::DamageType::Kinetic);
        const bool fixDamages = fix.hitMonster && alienMgr.at(ai).hp() < hpFull;

        check(bugAbsorbed && fixDamages,
              "T10 dispatch: wrong-manager shot is absorbed (no dmg); owning-manager shot damages");
        alienMgr.shutdown();
        decoyMgr.shutdown();
        w->shutdown();
    }

    x3::logInfo(std::string("canonaliens: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
