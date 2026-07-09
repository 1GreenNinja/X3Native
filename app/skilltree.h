#pragma once
// SKILL TREE + STAT-MOD LAYER (W9-3 RPG layer).
//
// A data-driven skill tree (assets/skills/skilltree.json, x3.skills/1): nodes
// with prereqs + skill-point costs across four thematic branches (Combat /
// Survival / Tech / Salvage), each carrying a stat effect. Owned nodes fold
// into a PlayerStatMods block of MULTIPLIERS LAYERED ON BASE — the WeaponDef
// table and player tuning constants are never mutated; the host applies the
// mods at the read points (fire-site damage, Player::setMaxHpBonus/
// setSpeedMult, Arsenal::setReloadMult/setAmmoCapMult, Progression::setXpMult).
//
// Weapon-mod ITEMS (ItemCategory::Mod) fold into the same block via
// foldItemEffect — one stat pipeline for skills AND mods.
//
// Pure logic — --test-skilltree drives it headlessly.

#include "item_db.h"       // ItemEffect (weapon mods fold into the same stat block)

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace x3::game {

class Progression;

// The folded live stat block. Multipliers start at identity; bonuses at 0.
struct PlayerStatMods {
    float damageMult   = 1.0f;   // weapon damage (fire sites)
    float speedMult    = 1.0f;   // player move speed
    float reloadMult   = 1.0f;   // reload TIME multiplier (< 1 = faster)
    float ammoCapMult  = 1.0f;   // reserve ammo cap
    float critChance   = 0.0f;   // probability a shot crits (double damage)
    float xpMult       = 1.0f;   // XP award scale
    float ammoYieldMult= 1.0f;   // rounds gained from ammo items
    int   maxHpBonus   = 0;      // added to kPlayerMaxHp
    std::vector<std::string> unlocks;   // ability unlock tags ("emp_efficiency", ...)
};

// One skill node.
struct SkillNode {
    std::string id;         // stable id ("cmb_dmg1")
    std::string name;
    std::string desc;
    std::string branch;     // "combat" / "survival" / "tech" / "salvage"
    int         cost = 1;   // skill points
    int         tier = 0;   // depth in the branch (UI layout row)
    std::vector<std::string> prereqs;   // node ids that must be owned first
    // Effect (same additive-fraction vocabulary as ItemEffect's mod fields).
    float damageMult   = 0.0f;
    float speedMult    = 0.0f;
    float reloadMult   = 0.0f;
    float ammoCapMult  = 0.0f;
    float critChance   = 0.0f;
    float xpMult       = 0.0f;
    float ammoYieldMult= 0.0f;
    int   maxHpBonus   = 0;
    std::string unlock;     // ability tag granted (empty = none)
};

class SkillTree {
public:
    // Load from `jsonPath` (assets/skills/skilltree.json); baked fallback on
    // absence/failure (never leaves the tree empty). Owned set is preserved
    // across load (ids that vanished from the data are dropped).
    bool load(std::string_view jsonPath);
    bool fromJson() const { return m_fromJson; }

    uint32_t count() const { return (uint32_t)m_nodes.size(); }
    const SkillNode& at(uint32_t i) const { return m_nodes[i]; }
    const SkillNode* find(std::string_view id) const;

    // Branch names in authored order (for the UI columns).
    const std::vector<std::string>& branches() const { return m_branches; }

    // ---- Ownership ---------------------------------------------------------
    bool owned(std::string_view id) const { return m_owned.count(std::string(id)) != 0; }
    // True iff every prereq is owned, the node is not yet owned, and `prog` has
    // the points.
    bool canBuy(std::string_view id, const Progression& prog) const;
    // Prereqs owned + not owned yet (ignores points) — the UI "available" state.
    bool unlocked(std::string_view id) const;
    // Spend the points + mark owned. Returns false (untouched) if canBuy fails.
    bool buy(std::string_view id, Progression& prog);
    // Load-time restore: mark owned WITHOUT spending (the save carries the spent
    // total separately / recomputed). Unknown ids ignored.
    void setOwned(std::string_view id) { if (find(id)) m_owned.insert(std::string(id)); }
    void clearOwned() { m_owned.clear(); }
    // Total points the owned set cost (load-time spent-recompute).
    int ownedCost() const;
    int ownedCount() const { return (int)m_owned.size(); }

    // ---- The stat fold -----------------------------------------------------
    // Fold every OWNED node into a fresh PlayerStatMods.
    PlayerStatMods mods() const;

    // ---- Persistence (text lines: "skill <id>") -----------------------------
    std::string serializeOwned() const;

private:
    void bakeFallback();
    std::vector<SkillNode> m_nodes;
    std::vector<std::string> m_branches;
    std::unordered_set<std::string> m_owned;
    bool m_fromJson = false;
};

// Fold a weapon-mod ITEM's effect into an existing mods block (same pipeline
// as skill nodes; called by the host for each applied mod item).
void foldItemEffect(PlayerStatMods& mods, const ItemEffect& fx);

// Scale a base damage value by the mod block: multiplier + crit roll (crit =
// double). `rng` is a caller-owned xorshift32 state (advanced per call) so the
// roll is deterministic under test.
int rpgScaleDamage(int base, const PlayerStatMods& mods, uint32_t& rng);

// Resolve the skills JSON path (assetRoot() + "/skills/skilltree.json").
std::string skillTreeJsonPath();

// Headless self-test (--test-skilltree): load-or-bake; prereq/cost gating; the
// stat fold; the multiplier reaching a real Player (setMaxHpBonus/speed) and a
// damage value; mod-item folding. Logs PASS/FAIL S#, returns true iff all pass.
bool runSkillTreeSelfTest();

} // namespace x3::game
