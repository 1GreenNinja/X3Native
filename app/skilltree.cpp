// SKILL TREE + STAT-MOD LAYER (W9-3 RPG layer) — see skilltree.h.
#include "skilltree.h"
#include "progression.h"
#include "player.h"        // --test-skilltree: mods reach a real Player
#include "asset_root.h"
#include "json_mini.h"
#include "story_ops.h"     // asciiFold

#include "engine/core/x3_log.h"

#include <algorithm>

namespace x3::game {

std::string skillTreeJsonPath() { return assetRoot() + "/skills/skilltree.json"; }

const SkillNode* SkillTree::find(std::string_view id) const {
    for (const SkillNode& n : m_nodes) if (n.id == id) return &n;
    return nullptr;
}

bool SkillTree::unlocked(std::string_view id) const {
    const SkillNode* n = find(id);
    if (!n || owned(id)) return false;
    for (const std::string& p : n->prereqs) if (!owned(p)) return false;
    return true;
}

bool SkillTree::canBuy(std::string_view id, const Progression& prog) const {
    const SkillNode* n = find(id);
    return n && unlocked(id) && prog.skillPoints() >= n->cost;
}

bool SkillTree::buy(std::string_view id, Progression& prog) {
    const SkillNode* n = find(id);
    if (!n || !canBuy(id, prog)) return false;
    if (!prog.spendPoints(n->cost)) return false;
    m_owned.insert(std::string(id));
    x3::logInfo("[skills] bought " + n->name + " (" + n->id + ") for " +
                std::to_string(n->cost) + " pt");
    return true;
}

int SkillTree::ownedCost() const {
    int c = 0;
    for (const std::string& id : m_owned)
        if (const SkillNode* n = find(id)) c += n->cost;
    return c;
}

PlayerStatMods SkillTree::mods() const {
    PlayerStatMods m;
    for (const std::string& id : m_owned) {
        const SkillNode* n = find(id);
        if (!n) continue;
        m.damageMult    += n->damageMult;
        m.speedMult     += n->speedMult;
        m.reloadMult    -= n->reloadMult;      // reload fractions REDUCE the time
        m.ammoCapMult   += n->ammoCapMult;
        m.critChance    += n->critChance;
        m.xpMult        += n->xpMult;
        m.ammoYieldMult += n->ammoYieldMult;
        m.maxHpBonus    += n->maxHpBonus;
        if (!n->unlock.empty()) m.unlocks.push_back(n->unlock);
    }
    if (m.reloadMult < 0.35f) m.reloadMult = 0.35f;   // floor: reloads never <35% time
    return m;
}

void foldItemEffect(PlayerStatMods& mods, const ItemEffect& fx) {
    mods.damageMult  += fx.damageMult;
    mods.reloadMult  -= fx.reloadMult;
    mods.ammoCapMult += fx.ammoCapMult;
    mods.critChance  += fx.critChance;
    if (mods.reloadMult < 0.35f) mods.reloadMult = 0.35f;
}

int rpgScaleDamage(int base, const PlayerStatMods& mods, uint32_t& rng) {
    float dmg = (float)base * mods.damageMult;
    if (mods.critChance > 0.0f) {
        // xorshift32 -> [0,1)
        uint32_t x = rng ? rng : 0x9E3779B9u;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rng = x;
        const float roll = (float)(x & 0xFFFFFFu) / 16777216.0f;
        if (roll < mods.critChance) dmg *= 2.0f;
    }
    const int out = (int)(dmg + 0.5f);
    return out < 1 ? 1 : out;
}

std::string SkillTree::serializeOwned() const {
    // Deterministic order (sorted) so saves diff cleanly.
    std::vector<std::string> ids(m_owned.begin(), m_owned.end());
    std::sort(ids.begin(), ids.end());
    std::string out;
    for (const std::string& id : ids) { out += "skill "; out += id; out += '\n'; }
    return out;
}

bool SkillTree::load(std::string_view jsonPath) {
    const std::unordered_set<std::string> prevOwned = m_owned;
    m_nodes.clear();
    m_branches.clear();
    m_owned.clear();
    m_fromJson = false;

    const std::string text = jmini::readFile(std::string(jsonPath));
    if (!text.empty()) {
        jmini::JReader rd(text);
        jmini::JVal root = rd.parse();
        const jmini::JVal* nodes = root.get("nodes");
        if (rd.ok && nodes && nodes->t == jmini::JVal::Arr) {
            for (const jmini::JVal& j : nodes->arr) {
                if (j.t != jmini::JVal::Obj) continue;
                SkillNode n;
                n.id = j.sval("id");
                if (n.id.empty()) continue;
                n.name   = asciiFold(j.sval("name", n.id));
                n.desc   = asciiFold(j.sval("desc"));
                n.branch = j.sval("branch", "combat");
                n.cost   = j.inum("cost", 1); if (n.cost < 1) n.cost = 1;
                n.tier   = j.inum("tier", 0);
                if (const jmini::JVal* p = j.get("prereqs"); p && p->t == jmini::JVal::Arr)
                    for (const jmini::JVal& pv : p->arr)
                        if (pv.t == jmini::JVal::Str) n.prereqs.push_back(pv.str);
                if (const jmini::JVal* e = j.get("effect"); e && e->t == jmini::JVal::Obj) {
                    n.damageMult    = e->fnum("damageMult", 0.0f);
                    n.speedMult     = e->fnum("speedMult", 0.0f);
                    n.reloadMult    = e->fnum("reloadMult", 0.0f);
                    n.ammoCapMult   = e->fnum("ammoCapMult", 0.0f);
                    n.critChance    = e->fnum("critChance", 0.0f);
                    n.xpMult        = e->fnum("xpMult", 0.0f);
                    n.ammoYieldMult = e->fnum("ammoYieldMult", 0.0f);
                    n.maxHpBonus    = e->inum("maxHp", 0);
                    n.unlock        = e->sval("unlock");
                }
                m_nodes.push_back(std::move(n));
            }
        }
        if (!m_nodes.empty()) m_fromJson = true;
    }
    if (m_nodes.empty()) bakeFallback();

    // Branch list in first-appearance order.
    for (const SkillNode& n : m_nodes)
        if (std::find(m_branches.begin(), m_branches.end(), n.branch) == m_branches.end())
            m_branches.push_back(n.branch);

    // Restore prior ownership for ids that still exist.
    for (const std::string& id : prevOwned)
        if (find(id)) m_owned.insert(id);

    x3::logInfo(std::string("[skills] ") + (m_fromJson ? "loaded " : "baked ") +
                std::to_string(m_nodes.size()) + " nodes / " +
                std::to_string(m_branches.size()) + " branches" +
                (m_fromJson ? (" from " + std::string(jsonPath)) : std::string()));
    return m_fromJson;
}

// Baked fallback — mirrors assets/skills/skilltree.json (the authored superset).
void SkillTree::bakeFallback() {
    auto add = [&](const char* id, const char* name, const char* branch, int tier,
                   int cost, std::vector<std::string> prereqs, SkillNode fx0,
                   const char* desc) {
        SkillNode n = fx0;
        n.id = id; n.name = name; n.branch = branch; n.tier = tier; n.cost = cost;
        n.prereqs = std::move(prereqs); n.desc = desc;
        m_nodes.push_back(std::move(n));
    };
    SkillNode fx;
    // COMBAT
    fx = {}; fx.damageMult = 0.10f;
    add("cmb_dmg1", "Dead Aim", "combat", 0, 1, {}, fx, "+10% weapon damage.");
    fx = {}; fx.damageMult = 0.15f;
    add("cmb_dmg2", "Executioner", "combat", 1, 2, { "cmb_dmg1" }, fx, "+15% weapon damage.");
    fx = {}; fx.critChance = 0.10f;
    add("cmb_crit", "Weak Points", "combat", 1, 2, { "cmb_dmg1" }, fx, "10% chance to crit for double damage.");
    fx = {}; fx.reloadMult = 0.20f;
    add("cmb_reload", "Combat Hands", "combat", 2, 2, { "cmb_dmg2" }, fx, "20% faster reloads.");
    // SURVIVAL
    fx = {}; fx.maxHpBonus = 25;
    add("sur_hp1", "Thick Skin", "survival", 0, 1, {}, fx, "+25 max HP.");
    fx = {}; fx.maxHpBonus = 25;
    add("sur_hp2", "Lab-Grown Grit", "survival", 1, 2, { "sur_hp1" }, fx, "+25 more max HP.");
    fx = {}; fx.speedMult = 0.10f;
    add("sur_speed1", "Runner", "survival", 1, 1, { "sur_hp1" }, fx, "+10% move speed.");
    fx = {}; fx.speedMult = 0.10f;
    add("sur_speed2", "Escape Artist", "survival", 2, 2, { "sur_speed1" }, fx, "+10% more move speed.");
    // TECH
    fx = {}; fx.ammoCapMult = 0.25f;
    add("tech_ammo", "Deep Pockets", "tech", 0, 1, {}, fx, "+25% reserve ammo capacity.");
    fx = {}; fx.unlock = "emp_efficiency";
    add("tech_emp", "EMP Doctrine", "tech", 1, 2, { "tech_ammo" }, fx,
        "EMP devices from the F4 bench are twice as efficient. (Feeds the EMP craft system.)");
    fx = {}; fx.unlock = "master_hack";
    add("tech_hack", "Ghost Protocol", "tech", 2, 3, { "tech_emp" }, fx,
        "Master-hack affinity: F5 terminal hacks resolve faster. (Feeds the hack system.)");
    // SALVAGE
    fx = {}; fx.ammoYieldMult = 0.50f;
    add("sal_ammo", "Scrounger", "salvage", 0, 1, {}, fx, "+50% rounds from ammo packs.");
    fx = {}; fx.xpMult = 0.10f;
    add("sal_xp1", "Field Study", "salvage", 1, 1, { "sal_ammo" }, fx, "+10% XP from all sources.");
    fx = {}; fx.xpMult = 0.15f;
    add("sal_xp2", "Salvari Insight", "salvage", 2, 2, { "sal_xp1" }, fx, "+15% more XP from all sources.");
}

// ===========================================================================
// --test-skilltree
// ===========================================================================
static int g_spass = 0, g_sfail = 0;
static void scheck(bool ok, const char* what) {
    if (ok) { ++g_spass; x3::logInfo(std::string("  PASS S") + std::to_string(g_spass + g_sfail) + " " + what); }
    else    { ++g_sfail; x3::logError(std::string("  FAIL S") + std::to_string(g_spass + g_sfail) + " " + what); }
}

bool runSkillTreeSelfTest() {
    g_spass = g_sfail = 0;

    // S1: the tree resolves (JSON or baked) with 4 branches and the core nodes.
    SkillTree t;
    const bool fromJson = t.load(skillTreeJsonPath());
    scheck(t.count() >= 12 && t.branches().size() >= 4 &&
           t.find("cmb_dmg1") && t.find("sur_hp1") && t.find("tech_ammo") && t.find("sal_ammo"),
           "skill tree resolves (json-or-baked): >=12 nodes across 4 branches");
    x3::logInfo(std::string("    (nodes: ") + std::to_string(t.count()) +
                (fromJson ? ", from JSON)" : ", baked fallback)"));

    // S2: prereq gating. cmb_dmg2 needs cmb_dmg1; buying out of order refuses.
    Progression p;
    p.addXp(100 + 175 + 250 + 325 + 400);   // -> level 6 = 5 points
    scheck(p.skillPoints() == 5 && !t.canBuy("cmb_dmg2", p) && !t.buy("cmb_dmg2", p) &&
           t.buy("cmb_dmg1", p) && t.canBuy("cmb_dmg2", p) && t.buy("cmb_dmg2", p),
           "prereqs gate purchase order (child refused before parent)");

    // S3: cost deduction + insufficient points refused.
    scheck(p.skillPoints() == 2 && !t.buy("tech_hack", p) /* prereqs missing anyway */ &&
           t.buy("sur_hp1", p) && p.skillPoints() == 1 && !t.buy("cmb_reload", p),
           "costs deduct points; 2-pt node refused on a 1-pt balance");

    // S4: the stat fold composes owned effects.
    {
        const PlayerStatMods m = t.mods();   // cmb_dmg1 (0.10) + cmb_dmg2 (0.15) + sur_hp1 (25)
        scheck(m.damageMult > 1.249f && m.damageMult < 1.251f && m.maxHpBonus == 25 &&
               m.speedMult == 1.0f,
               "mods() folds owned nodes (damage x1.25, +25 HP, speed untouched)");
    }

    // S5: the multiplier REACHES the player + a real damage value.
    {
        const PlayerStatMods m = t.mods();
        Player pl;
        pl.setMaxHpBonus(m.maxHpBonus);
        pl.setSpeedMult(m.speedMult);
        uint32_t rng = 1234;
        const int dmg = rpgScaleDamage(20, m, rng);   // 20 * 1.25 = 25 (no crit owned)
        scheck(pl.maxHp() == kPlayerMaxHp + 25 && pl.hp() == kPlayerMaxHp + 25 &&
               dmg == 25,
               "stat layer reaches the live Player (maxHp 125) + the fire-path damage (20->25)");
    }

    // S6: mod ITEMS fold through the same pipeline; crit doubles at chance 1.
    {
        PlayerStatMods m;
        ItemEffect fx;
        fx.damageMult = 0.15f; fx.reloadMult = 0.20f; fx.ammoCapMult = 0.25f;
        foldItemEffect(m, fx);
        m.critChance = 1.0f;   // force the crit branch deterministically
        uint32_t rng = 7;
        const int dmg = rpgScaleDamage(20, m, rng);   // 20*1.15=23 -> crit x2 = 46
        scheck(m.damageMult > 1.149f && m.reloadMult < 0.801f && m.ammoCapMult > 1.249f &&
               dmg == 46,
               "weapon-mod item folds into the same block; guaranteed crit doubles damage");
    }

    // S7: ownership serialize + load-time restore recomputes spent.
    {
        std::string ser = t.serializeOwned();
        SkillTree t2;
        t2.load(skillTreeJsonPath());
        // Parse "skill <id>" lines the way the RPG save loader does.
        size_t pos = 0;
        while (pos < ser.size()) {
            size_t nl = ser.find('\n', pos);
            if (nl == std::string::npos) nl = ser.size();
            std::string line = ser.substr(pos, nl - pos);
            if (line.rfind("skill ", 0) == 0) t2.setOwned(line.substr(6));
            pos = nl + 1;
        }
        scheck(t2.ownedCount() == 3 && t2.owned("cmb_dmg1") && t2.owned("cmb_dmg2") &&
               t2.owned("sur_hp1") && t2.ownedCost() == 4,
               "owned set serializes + restores; ownedCost recomputes spent (1+2+1=4)");
    }

    x3::logInfo("--test-skilltree: " + std::to_string(g_spass) + " passed, " +
                std::to_string(g_sfail) + " failed");
    return g_sfail == 0;
}

} // namespace x3::game
