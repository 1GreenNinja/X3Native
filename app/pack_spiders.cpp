// PACK-HARVEST ARACHNIDS — see pack_spiders.h.
//
// Clean-room: built ONLY from X3Native's own monster.* roster idiom + engine
// interfaces. NO other game-engine source consulted.
#include "pack_spiders.h"
#include "asset_root.h"
#include "headless_device.h"

#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

namespace x3::game {

const char* packSpiderTypeName(PackSpider t) {
    switch (t) {
        case PackSpider::LabSkitterer: return "Lab Skitterer";
        case PackSpider::VenomBrood:   return "Venom Brood";
        case PackSpider::Count:        return "?";
    }
    return "?";
}

namespace {

// The harvested GLB stems in assets/rigged_glb. Each is a SINGLE self-contained
// multi-clip file produced by tools/prep_pack_spider.py; the "_anim" suffix is
// the runtime's preferred-animated-model convention (see setCanonModel in
// canon_aliens.cpp / defRigged in monster.cpp), so we name the stem and let the
// same prefer-"_anim" rule pick it.
constexpr const char* kSkittererStem = "pack_spider";
constexpr const char* kBroodStem     = "pack_spider_blue";

// Point a roster row at its harvested GLB, preferring "<stem>_anim.glb".
void setSpiderModel(MonsterSystem::Tuning& t, const char* stem) {
    namespace fs = std::filesystem;
    const std::string riggedDir = riggedGlbRoot();
    const std::string base(stem);
    std::string chosen = base + ".glb";
    std::error_code ec;
    if (fs::exists(fs::path(riggedDir) / (base + "_anim.glb"), ec))
        chosen = base + "_anim.glb";
    t.modelFile        = chosen;
    t.modelDirOverride = riggedDir;
    t.standUpZtoY      = false;   // harvested Y-up, feet at y=0, facing +Z
}

// LAB SKITTERER — the brown tarantula. A fast, fragile rusher: it closes hard,
// flanks a little, and dies quickly, so a nest of them reads as a SWARM rather
// than a wall of HP. Authored leg span ~0.87 m, body ~0.21 m tall; modelScale 1
// keeps it at that "big enough to be horrible, small enough to skitter under a
// console" read.
MonsterSystem::Tuning labSkittererTuning() {
    MonsterSystem::Tuning t = tuningFor(EnemyType::Verthani);   // wildcard non-humanoid base
    t.hp             = 55;                                      // fragile — swarm fodder
    t.type           = MonsterType::Guard;
    t.ranged         = false;
    t.damage         = combat::kMeleeDamageMin;                 // 6 — a bite, not a haymaker
    t.attackRange    = combat::kMeleeRange - 0.3f;              // low, short-reach body
    t.attackCooldown = combat::kMeleeCooldownMin;               // 1.0 — rapid bites
    t.attackWindup   = combat::kMeleeWindup;
    t.chaseSpeed     = kChaseSpeed * 1.45f;                     // SKITTERS
    t.aiStrafeBias   = 0.45f;                                   // darts sideways, not a bee-line
    t.modelScale     = 1.00f;                                   // ~0.87 m leg span as authored
    t.species        = EnemyType::Verthani;
    t.tint[0] = 1.00f; t.tint[1] = 1.00f; t.tint[2] = 1.00f; t.tint[3] = 1.0f;  // pack skin as-is
    t.emissiveScale  = 0.0f;                                    // no glow on a real-world animal
    setSpiderModel(t, kSkittererStem);
    return t;
}

// VENOM BROOD — the blue tarantula, scaled to a room-filling bruiser. Slower and
// far tankier than the Skitterer, hits at the top of the melee band, and its
// 1.6x scale (~1.4 m leg span) makes it read as the thing the skitterers came
// out of. Same rig, same clips, different animal.
MonsterSystem::Tuning venomBroodTuning() {
    MonsterSystem::Tuning t = labSkittererTuning();             // same species/base wiring
    t.hp             = 170;                                     // bruiser tier
    t.damage         = combat::kMeleeDamageMax;                 // 10 — envenomed bite
    t.attackRange    = combat::kMeleeRange + 0.3f;              // bigger body, longer reach
    t.attackCooldown = combat::kMeleeCooldownMax;               // 1.3 — heavy, telegraphed
    t.attackWindup   = combat::kMeleeWindup * 1.6f;             // slower tell (dodgeable)
    t.chaseSpeed     = kChaseSpeed * 0.85f;                     // lumbers
    t.aiStrafeBias   = 0.20f;                                   // charges
    t.modelScale     = 1.60f;                                   // ~1.4 m leg span
    t.tint[0] = 0.82f; t.tint[1] = 0.88f; t.tint[2] = 1.00f; t.tint[3] = 1.0f;  // cold blue push
    setSpiderModel(t, kBroodStem);
    return t;
}

std::vector<PackSpiderDef> buildPackSpiderDefs() {
    std::vector<PackSpiderDef> defs;
    defs.reserve((size_t)PackSpider::Count);
    defs.push_back({ PackSpider::LabSkitterer, "Lab Skitterer", labSkittererTuning() });
    defs.push_back({ PackSpider::VenomBrood,   "Venom Brood",   venomBroodTuning()   });
    return defs;
}

} // namespace

const std::vector<PackSpiderDef>& packSpiderDefs() {
    static const std::vector<PackSpiderDef> defs = buildPackSpiderDefs();
    return defs;
}

const PackSpiderDef& packSpiderDef(PackSpider t) {
    const std::vector<PackSpiderDef>& defs = packSpiderDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];
    for (const PackSpiderDef& d : defs) if (d.type == t) return d;
    return defs[0];
}

MonsterSystem::Tuning packSpiderTuning(PackSpider t) {
    return packSpiderDef(t).tuning;
}

// ============================================================================
// Headless self-test (--test-packspiders).
// ============================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[packspiders-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[packspiders-test] FAIL ") + name); }
}

// THE POINT OF THE WHOLE EXERCISE: this rig is REAL. A single-bone fake rig (what
// tools/animate_creature.py produces for an unrigged mesh) has exactly 1 joint;
// the harvested arachnid has 70. Gate well above 1 but below 70 so a future
// re-harvest / decimation pass has room without silently regressing to a fake.
constexpr size_t kMinSpiderJoints = 32;
// Every canonical clip the engine's fuzzy resolver looks for, by name.
const char* kExpectedClips[] = { "Idle", "Walk", "Run", "Attack", "Attack2", "Death" };

bool tuningEq(const MonsterSystem::Tuning& a, const MonsterSystem::Tuning& b) {
    return a.hp == b.hp && a.damage == b.damage && a.type == b.type &&
           a.ranged == b.ranged && a.chaseSpeed == b.chaseSpeed &&
           a.attackRange == b.attackRange && a.modelScale == b.modelScale;
}

// Load a harvested GLB straight off disk (headless, null device) and report the
// rig/clip facts the harvest is supposed to have delivered.
struct HarvestFacts {
    bool   loaded   = false;
    size_t joints   = 0;
    size_t clips    = 0;
    bool   allClips = false;   // every kExpectedClips name present
    bool   allMoved = false;   // ...and each carries real motion (>2 keys, >0 s)
    bool   skinned  = false;   // at least one primitive is skinned
};

HarvestFacts inspectHarvest(const std::string& dir, const std::string& file) {
    HarvestFacts f;
    std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
    src->mountDir(dir, 0);
    std::unique_ptr<x3::asset::IModelLoader> loader(
        x3::asset::createModelLoader(nullptr, src.get()));   // headless: CPU skin data kept
    x3::asset::Model m = loader->load(file);
    if (!m.ok) return f;
    f.loaded = true;
    for (const auto& s : m.skins) f.joints = f.joints > s.joints.size() ? f.joints : s.joints.size();
    f.clips = m.animations.size();
    for (const auto& p : m.primitives) if (p.skinned) f.skinned = true;
    f.allClips = true;
    f.allMoved = true;
    for (const char* want : kExpectedClips) {
        const x3::asset::AnimationClip* found = nullptr;
        for (const auto& c : m.animations) if (c.name == want) { found = &c; break; }
        if (!found) { f.allClips = false; f.allMoved = false; continue; }
        // Real motion, not a 1-frame stub: a live channel with >2 keys over >0 s.
        bool moves = found->duration > 0.0f;
        size_t maxKeys = 0;
        for (const auto& ch : found->channels)
            maxKeys = maxKeys > ch.times.size() ? maxKeys : ch.times.size();
        if (!moves || maxKeys <= 2) f.allMoved = false;
    }
    return f;
}

} // namespace

bool runPackSpidersSelfTest() {
    g_pass = g_fail = 0;
    using HeadlessDevice = x3::game::HeadlessRenderDevice;
    HeadlessDevice device;

    // ---- T0: roster complete + ordered. ----
    {
        const auto& roster = packSpiderDefs();
        bool complete = roster.size() == (size_t)PackSpider::Count;
        bool ordered  = true;
        for (uint32_t i = 0; i < roster.size(); ++i)
            if ((uint32_t)roster[i].type != i) ordered = false;
        check(complete && ordered, "T0 arachnid roster has one row per PackSpider, in enum order");
    }

    // ---- T1: each row BUILDS with its table stats. ----
    {
        int builtOk = 0;
        for (uint32_t i = 0; i < (uint32_t)PackSpider::Count; ++i) {
            const PackSpiderDef& def = packSpiderDef((PackSpider)i);
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
            else x3::logError(std::string("[packspiders-test] build failed: ") + def.name);
            w->shutdown();
        }
        check(builtOk == (int)PackSpider::Count, "T1 each arachnid BUILDS with its table stats");
    }

    // ---- T2: Lab Skitterer — hostile melee Guard, fragile, faster than default. ----
    {
        const auto& d = packSpiderDef(PackSpider::LabSkitterer);
        check(d.tuning.damage > 0 && !d.tuning.ranged &&
              d.tuning.type == MonsterType::Guard &&
              d.tuning.hp < 100 && d.tuning.chaseSpeed > kChaseSpeed,
              "T2 Lab Skitterer: hostile melee Guard, fragile (HP<100), faster than default chase");
    }
    // ---- T3: Venom Brood — tankier + harder + bigger + slower than the Skitterer. ----
    {
        const auto& s = packSpiderDef(PackSpider::LabSkitterer).tuning;
        const auto& b = packSpiderDef(PackSpider::VenomBrood).tuning;
        check(b.hp > s.hp && b.damage > s.damage && b.modelScale > s.modelScale &&
              b.chaseSpeed < s.chaseSpeed && !b.ranged,
              "T3 Venom Brood: tankier + harder-hitting + bigger + slower than the Skitterer");
    }
    // ---- T4: rows are DISTINCT. ----
    {
        const auto& roster = packSpiderDefs();
        bool distinct = true;
        for (size_t i = 0; i < roster.size(); ++i)
            for (size_t j = i + 1; j < roster.size(); ++j)
                if (tuningEq(roster[i].tuning, roster[j].tuning)) distinct = false;
        check(distinct, "T4 arachnid rows are DISTINCT (no duplicate stat blocks)");
    }
    // ---- T5: ART ATTACHED — every row names a harvested GLB present on disk. ----
    {
        namespace fs = std::filesystem;
        int wired = 0;
        for (uint32_t i = 0; i < (uint32_t)PackSpider::Count; ++i) {
            const PackSpiderDef& d = packSpiderDef((PackSpider)i);
            const bool named = !d.tuning.modelFile.empty() && !d.tuning.modelDirOverride.empty();
            std::error_code ec;
            const bool onDisk = named &&
                fs::exists(fs::path(d.tuning.modelDirOverride) / d.tuning.modelFile, ec);
            if (named && onDisk) ++wired;
            else x3::logError(std::string("[packspiders-test] model missing: ") + d.name +
                              " -> " + d.tuning.modelFile);
        }
        check(wired == (int)PackSpider::Count,
              "T5 every arachnid row has its harvested GLB wired + present on disk");
    }
    // ---- T6: THE HARVEST GATE — a REAL rig with REAL clips, not a fake. ----
    {
        int good = 0;
        for (uint32_t i = 0; i < (uint32_t)PackSpider::Count; ++i) {
            const PackSpiderDef& d = packSpiderDef((PackSpider)i);
            const HarvestFacts f = inspectHarvest(d.tuning.modelDirOverride, d.tuning.modelFile);
            x3::logInfo(std::string("[packspiders-test] ") + d.name + " -> " + d.tuning.modelFile +
                        "  loaded=" + (f.loaded ? "1" : "0") +
                        " joints=" + std::to_string(f.joints) +
                        " clips=" + std::to_string(f.clips) +
                        " allNamedClips=" + (f.allClips ? "1" : "0") +
                        " allMove=" + (f.allMoved ? "1" : "0") +
                        " skinned=" + (f.skinned ? "1" : "0"));
            if (f.loaded && f.skinned && f.joints >= kMinSpiderJoints &&
                f.clips >= (sizeof(kExpectedClips) / sizeof(kExpectedClips[0])) &&
                f.allClips && f.allMoved)
                ++good;
        }
        check(good == (int)PackSpider::Count,
              "T6 harvested GLBs are REALLY rigged (>=32 joints, skinned) with all 6 canonical "
              "clips carrying real motion");
    }
    // ---- T7: CLIP BINDING — the engine's own fuzzy resolver lands every slot. ----
    {
        using CS = MonsterSystem::ClipSlot;
        int bound = 0;
        for (uint32_t i = 0; i < (uint32_t)PackSpider::Count; ++i) {
            const PackSpiderDef& def = packSpiderDef((PackSpider)i);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            Scene scene;
            MonsterSystem m;
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.0f, 0.0f }, def.tuning);
            const int idle = m.clipIndex(CS::Idle),  walk = m.clipIndex(CS::Walk);
            const int run  = m.clipIndex(CS::Run),   atk  = m.clipIndex(CS::Attack);
            const int atk2 = m.clipIndex(CS::Attack2), die = m.clipIndex(CS::Death);
            x3::logInfo(std::string("[packspiders-test] ") + def.name +
                        " slots idle=" + std::to_string(idle) + " walk=" + std::to_string(walk) +
                        " run=" + std::to_string(run) + " attack=" + std::to_string(atk) +
                        " attack2=" + std::to_string(atk2) + " death=" + std::to_string(die));
            // Distinct slots too: Attack must not have collapsed onto Attack2, and
            // Walk/Run must be different clips or the loco blend has nothing to sweep.
            if (idle >= 0 && walk >= 0 && run >= 0 && atk >= 0 && atk2 >= 0 && die >= 0 &&
                atk != atk2 && walk != run && idle != walk)
                ++bound;
            w->shutdown();
        }
        check(bound == (int)PackSpider::Count,
              "T7 every clip slot (idle/walk/run/attack/attack2/death) binds through the stock "
              "fuzzy resolver, with distinct attack-vs-attack2 and walk-vs-run");
    }

    x3::logInfo(std::string("packspiders: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
