#pragma once
// XP + LEVELS (W9-3 RPG layer).
//
// XP sources: enemy kills (the death-FX funnel), rescues, objectives (clone
// down / the win), lore terminals. XP climbs a quadratic-ish curve; each level
// grants skill points the SkillTree spends. State persists as a text blob in
// the RPG save file that rides ALONGSIDE the binary checkpoint (the same
// additive-file pattern StoryFlags uses — the checkpoint format stays
// untouched).
//
// Pure logic, headless-testable (--test-progression).

#include <cstdint>
#include <string>
#include <string_view>

namespace x3::game {

// ---- XP awards (tuning targets; Tim balances in playtest) ------------------
constexpr int kXpKill      = 25;    // any hostile death
constexpr int kXpRescue    = 200;   // a girl freed (companion/extracted)
constexpr int kXpObjective = 100;   // story objective beats
constexpr int kXpBoss      = 300;   // the F7 clone down
constexpr int kXpSecret    = 75;    // secret discoveries
constexpr int kXpLore      = 30;    // a lore terminal read
constexpr int kXpWin       = 500;   // Sarah extracted (the win)

constexpr int kMaxLevel        = 20;
constexpr int kPointsPerLevel  = 1;   // skill points granted per level gained

// XP needed to go FROM `level` TO `level+1` (level starts at 1).
// 100, 175, 250, 325, ... — a gentle ramp; ~2.5k XP to level 10.
inline int xpToNext(int level) { return 100 + (level - 1) * 75; }

class Progression {
public:
    // Award XP (scaled by the xpMult the skill layer sets — Salvage branch).
    // Returns the number of LEVELS gained by this award (0 almost always) so
    // the host can toast "LEVEL UP".
    int addXp(int amount);

    int  xp() const { return m_xp; }                 // lifetime XP
    int  level() const;                              // derived from m_xp (1..kMaxLevel)
    int  xpIntoLevel() const;                        // XP progress inside the current level
    int  xpForLevel() const { return xpToNext(level()); }   // span of the current level

    // Skill points: level-1 levels gained * kPointsPerLevel, minus spent.
    int  skillPoints() const;
    bool spendPoints(int n);                         // false + untouched if not enough
    int  spentPoints() const { return m_spent; }
    void setSpentPoints(int n) { m_spent = n < 0 ? 0 : n; }   // load-time restore

    // XP multiplier from the skill layer (>= 1). Applied inside addXp.
    void  setXpMult(float m) { m_xpMult = m < 0.1f ? 0.1f : m; }
    float xpMult() const { return m_xpMult; }

    void reset() { m_xp = 0; m_spent = 0; m_xpMult = 1.0f; }

    // ---- Persistence (text lines: "xp N" / "spent N") ----------------------
    std::string serialize() const;
    void deserialize(std::string_view text);

private:
    int   m_xp     = 0;
    int   m_spent  = 0;
    float m_xpMult = 1.0f;
};

// Headless self-test (--test-progression): curve monotonic, XP->level->points,
// spend gating, xpMult, and a save/load round-trip through a real temp file.
// Logs PASS/FAIL X#, returns true iff all pass.
bool runProgressionSelfTest();

} // namespace x3::game
