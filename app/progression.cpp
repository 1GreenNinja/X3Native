// XP + LEVELS (W9-3 RPG layer) — see progression.h.
#include "progression.h"

#include "engine/core/x3_log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace x3::game {

static int levelForXp(int xp) {
    int lvl = 1, need = 0;
    while (lvl < kMaxLevel) {
        need += xpToNext(lvl);
        if (xp < need) break;
        ++lvl;
    }
    return lvl;
}

int Progression::addXp(int amount) {
    if (amount <= 0) return 0;
    const int scaled = (int)((float)amount * m_xpMult + 0.5f);
    const int before = level();
    m_xp += scaled;
    const int after = level();
    return after - before;
}

int Progression::level() const { return levelForXp(m_xp); }

int Progression::xpIntoLevel() const {
    int lvl = 1, base = 0;
    while (lvl < level()) { base += xpToNext(lvl); ++lvl; }
    return m_xp - base;
}

int Progression::skillPoints() const {
    const int earned = (level() - 1) * kPointsPerLevel;
    const int left = earned - m_spent;
    return left > 0 ? left : 0;
}

bool Progression::spendPoints(int n) {
    if (n <= 0 || skillPoints() < n) return false;
    m_spent += n;
    return true;
}

std::string Progression::serialize() const {
    std::ostringstream ss;
    ss << "xp " << m_xp << "\nspent " << m_spent << '\n';
    return ss.str();
}

void Progression::deserialize(std::string_view text) {
    reset();
    std::istringstream ss{ std::string(text) };
    std::string line;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string tag; ls >> tag;
        if (tag == "xp")    ls >> m_xp;
        if (tag == "spent") ls >> m_spent;
    }
    if (m_xp < 0) m_xp = 0;
    if (m_spent < 0) m_spent = 0;
}

// ===========================================================================
// --test-progression
// ===========================================================================
static int g_xpass = 0, g_xfail = 0;
static void xcheck(bool ok, const char* what) {
    if (ok) { ++g_xpass; x3::logInfo(std::string("  PASS X") + std::to_string(g_xpass + g_xfail) + " " + what); }
    else    { ++g_xfail; x3::logError(std::string("  FAIL X") + std::to_string(g_xpass + g_xfail) + " " + what); }
}

bool runProgressionSelfTest() {
    g_xpass = g_xfail = 0;

    // X1: fresh state = level 1, no points; the curve is monotonic.
    Progression p;
    bool monotonic = true;
    for (int l = 1; l < kMaxLevel; ++l) if (xpToNext(l + 1) <= xpToNext(l)) monotonic = false;
    xcheck(p.level() == 1 && p.skillPoints() == 0 && p.xp() == 0 && monotonic,
           "fresh progression: level 1, 0 points, monotonic XP curve");

    // X2: XP -> level -> points. 100 XP = exactly level 2 (+1 point); four kills
    // at 25 XP land it.
    int gained = 0;
    for (int i = 0; i < 4; ++i) gained += p.addXp(kXpKill);
    xcheck(gained == 1 && p.level() == 2 && p.skillPoints() == kPointsPerLevel &&
           p.xpIntoLevel() == 0 && p.xpForLevel() == xpToNext(2),
           "4 kills (100 XP) -> level 2 -> +1 skill point, progress resets into L2");

    // X3: spend gating. 1 point: spending 2 refuses, 1 succeeds, then 0 left.
    xcheck(!p.spendPoints(2) && p.spendPoints(1) && p.skillPoints() == 0 && !p.spendPoints(1),
           "spendPoints gated by the earned-minus-spent balance");

    // X4: xpMult scales awards (Salvage skill layer). 1.5x on 100 XP -> 150.
    {
        Progression q;
        q.setXpMult(1.5f);
        q.addXp(100);
        xcheck(q.xp() == 150, "xpMult scales awarded XP (100 * 1.5 = 150)");
    }

    // X5: a big award crosses multiple levels in ONE call and reports them.
    {
        Progression q;
        const int lv = q.addXp(xpToNext(1) + xpToNext(2) + 10);   // past L3
        xcheck(lv == 2 && q.level() == 3 && q.skillPoints() == 2 * kPointsPerLevel,
               "one award crossing two levels reports both + grants both points");
    }

    // X6: save/load round-trip through a real temp file (the RPG-save lane).
    {
        const std::string tmp = "test_progression.tmp.txt";
        Progression a;
        a.addXp(555);
        a.spendPoints(1);
        { std::ofstream f(tmp, std::ios::binary); f << a.serialize(); }
        std::string text;
        { std::ifstream f(tmp, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); text = ss.str(); }
        Progression b;
        b.deserialize(text);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        xcheck(b.xp() == a.xp() && b.level() == a.level() &&
               b.skillPoints() == a.skillPoints() && b.spentPoints() == a.spentPoints(),
               "serialize -> file -> deserialize round-trip (xp/level/points/spent)");
    }

    x3::logInfo("--test-progression: " + std::to_string(g_xpass) + " passed, " +
                std::to_string(g_xfail) + " failed");
    return g_xfail == 0;
}

} // namespace x3::game
