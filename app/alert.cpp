// FACILITY ALERT LEVEL implementation — see app/alert.h for the design.
// Game/slice code only — engine/ stays pure.

#include "alert.h"
#include "asset_root.h"
#include "json_mini.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

namespace {
inline float dist2XZ(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}
} // namespace

const char* alertLevelName(int level) {
    switch (level < 0 ? 0 : (level > 4 ? 4 : level)) {
        case 0: return "CALM";
        case 1: return "INVESTIGATE";
        case 2: return "SEARCH";
        case 3: return "LOCKDOWN";
        case 4: return "KILL SQUAD";
    }
    return "?";
}

AlertConfig defaultAlertConfig() { return AlertConfig{}; }

std::string alertJsonPath() { return assetRoot() + "/world/alert.json"; }

AlertConfig loadAlertConfig(std::string_view jsonPath) {
    using namespace jmini;
    const std::string path(jsonPath);
    const std::string text = readFile(path);
    AlertConfig cfg;   // header defaults
    if (text.empty()) {
        x3::logInfo("alert: no config at " + path + " — using built-in defaults");
        return cfg;
    }
    JReader r(text);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) {
        x3::logError("alert: unparseable " + path + " — using built-in defaults");
        return cfg;
    }
    cfg.heatGunshot       = root.fnum("heatGunshot",       cfg.heatGunshot);
    cfg.heatCorpse        = root.fnum("heatCorpse",        cfg.heatCorpse);
    cfg.heatWitness       = root.fnum("heatWitness",       cfg.heatWitness);
    cfg.heatTerminalHack  = root.fnum("heatTerminalHack",  cfg.heatTerminalHack);
    cfg.heatWitnessPerSec = root.fnum("heatWitnessPerSec", cfg.heatWitnessPerSec);
    if (const JVal* v = root.get("thresholds"); v && v->t == JVal::Arr && v->arr.size() >= 4)
        for (int k = 0; k < 4; ++k) cfg.up[k] = (float)v->arr[k].num;
    cfg.downFrac          = root.fnum("downFrac",          cfg.downFrac);
    cfg.heatMax           = root.fnum("heatMax",           cfg.heatMax);
    cfg.floorGunshot      = root.inum("floorGunshot",      cfg.floorGunshot);
    cfg.floorHack         = root.inum("floorHack",         cfg.floorHack);
    cfg.floorCorpse       = root.inum("floorCorpse",       cfg.floorCorpse);
    cfg.floorWitness      = root.inum("floorWitness",      cfg.floorWitness);
    cfg.gunshotRadius     = root.fnum("gunshotRadius",     cfg.gunshotRadius);
    cfg.corpseRadius      = root.fnum("corpseRadius",      cfg.corpseRadius);
    cfg.decayDelay        = root.fnum("decayDelay",        cfg.decayDelay);
    if (const JVal* v = root.get("decayPerSec"); v && v->t == JVal::Arr && v->arr.size() >= 5)
        for (int k = 0; k < 5; ++k) cfg.decayPerSec[k] = (float)v->arr[k].num;
    cfg.searchSpawns      = root.inum("searchSpawns",      cfg.searchSpawns);
    cfg.killSquadSpawns   = root.inum("killSquadSpawns",   cfg.killSquadSpawns);
    if (const JVal* v = root.get("patrolSpeedMul"); v && v->t == JVal::Arr && v->arr.size() >= 5)
        for (int k = 0; k < 5; ++k) cfg.patrolSpeedMul[k] = (float)v->arr[k].num;
    x3::logInfo("alert: loaded tunables from " + path);
    return cfg;
}

// ---------------------------------------------------------------------------
// AlertSystem
// ---------------------------------------------------------------------------

float AlertSystem::redShift() const {
    if (m_level < 3) return 0.0f;
    return m_level >= 4 ? 1.0f : 0.7f;
}

void AlertSystem::reportGunshot(const x3::phys::Vec3& pos) {
    // Queued; resolved against the observers at the next update() — a shot no
    // guard could hear raises nothing (the witness-vs-unseen rule).
    if (m_pendingShots.size() < 16) m_pendingShots.push_back({pos});
}

void AlertSystem::reportTerminalHack(const x3::phys::Vec3& pos) {
    addHeat(m_cfg.heatTerminalHack, m_cfg.floorHack, pos, "terminal_hack");
}

void AlertSystem::registerCorpse(const x3::phys::Vec3& pos) {
    for (const Corpse& c : m_corpses)
        if (dist2XZ(c.pos, pos) < 1.5f * 1.5f) return;   // dedupe
    m_corpses.push_back({pos, false});
}

uint32_t AlertSystem::discoveredCorpseCount() const {
    uint32_t n = 0;
    for (const Corpse& c : m_corpses) if (c.discovered) ++n;
    return n;
}

int AlertSystem::takeSpawnRequests() {
    const int n = m_pendingSpawns;
    m_pendingSpawns = 0;
    return n;
}

bool AlertSystem::takeInvestigatePos(x3::phys::Vec3& out) {
    if (!m_investigatePending) return false;
    m_investigatePending = false;
    out = m_investigatePos;
    return true;
}

void AlertSystem::setLevel(int lv) {
    lv = std::clamp(lv, 0, 4);
    if (lv == m_level) return;
    const int old = m_level;
    m_level = lv;
    // Entering SEARCH spawns extra guards; entering KILL SQUAD spawns the squad.
    if (old < 2 && lv >= 2) m_pendingSpawns += m_cfg.searchSpawns;
    if (old < 4 && lv >= 4) m_pendingSpawns += m_cfg.killSquadSpawns;
    // The scripting-convention event (always logged) + the optional C++ sink.
    x3::logInfo("x3.fire(\"alert_changed\",{level=" + std::to_string(lv) + "})  ["
                + alertLevelName(old) + " -> " + alertLevelName(lv) + "]");
    if (m_sink) m_sink(lv, old);
}

void AlertSystem::addHeat(float h, int floorLevel, const x3::phys::Vec3& where,
                          const char* what) {
    m_quietTime = 0.0f;
    // The floor guarantees the stimulus reads at its severity even from cold.
    if (floorLevel >= 1 && m_heat < m_cfg.up[floorLevel - 1])
        m_heat = m_cfg.up[floorLevel - 1];
    m_heat = std::min(m_heat + h, m_cfg.heatMax);
    // Rise instantly through every threshold the heat now clears.
    int lv = m_level;
    for (int k = 0; k < 4; ++k) if (m_heat >= m_cfg.up[k]) lv = k + 1;
    if (floorLevel > lv) lv = floorLevel;
    m_investigatePos = where;
    m_investigatePending = true;
    if (lv != m_level) setLevel(lv);
    (void)what;
}

void AlertSystem::debugForceLevel(int level) {
    level = std::clamp(level, 0, 4);
    m_heat = (level == 0) ? 0.0f : m_cfg.up[level - 1] + 1.0f;
    m_quietTime = 0.0f;
    if (level != m_level) setLevel(level);
}

void AlertSystem::update(float dt, const x3::phys::Vec3& playerPos,
                         const x3::phys::Vec3* observers, uint32_t observerCount,
                         bool playerSeen) {
    // ---- Resolve queued gunshots: did any guard hear them? ----
    if (!m_pendingShots.empty()) {
        const float r2 = m_cfg.gunshotRadius * m_cfg.gunshotRadius;
        for (const PendingShot& s : m_pendingShots) {
            bool heard = false;
            for (uint32_t i = 0; i < observerCount && !heard; ++i)
                if (dist2XZ(observers[i], s.pos) <= r2) heard = true;
            if (heard) addHeat(m_cfg.heatGunshot, m_cfg.floorGunshot, s.pos, "gunshot");
        }
        m_pendingShots.clear();
    }

    // ---- Corpse discovery: a guard walking onto an unfound body ----
    if (!m_corpses.empty() && observerCount > 0) {
        const float r2 = m_cfg.corpseRadius * m_cfg.corpseRadius;
        for (Corpse& c : m_corpses) {
            if (c.discovered) continue;
            for (uint32_t i = 0; i < observerCount; ++i) {
                if (dist2XZ(observers[i], c.pos) <= r2) {
                    c.discovered = true;
                    addHeat(m_cfg.heatCorpse, m_cfg.floorCorpse, c.pos, "corpse_found");
                    break;
                }
            }
        }
    }

    // ---- Witness sightings ----
    if (playerSeen) {
        if (!m_seenLastFrame)   // a fresh engagement
            addHeat(m_cfg.heatWitness, m_cfg.floorWitness, playerPos, "witness");
        else if (m_level >= 1) {
            // Sustained contact keeps building pressure (3 -> 4 happens here).
            m_quietTime = 0.0f;
            m_heat = std::min(m_heat + m_cfg.heatWitnessPerSec * dt, m_cfg.heatMax);
            int lv = m_level;
            for (int k = 0; k < 4; ++k) if (m_heat >= m_cfg.up[k]) lv = k + 1;
            if (lv != m_level) setLevel(lv);
        }
    }
    m_seenLastFrame = playerSeen;

    // ---- Decay: hiding out (quiet + unseen) drains the heat ----
    if (!playerSeen) {
        m_quietTime += dt;
        if (m_quietTime >= m_cfg.decayDelay && m_level > 0) {
            m_heat = std::max(0.0f, m_heat - m_cfg.decayPerSec[m_level] * dt);
            // Step down through the hysteresis band.
            const float down = m_cfg.up[m_level - 1] * m_cfg.downFrac;
            if (m_heat <= down) setLevel(m_level - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// AlertDoorLock
// ---------------------------------------------------------------------------

void AlertDoorLock::update(const AlertSystem& alert, DoorSystem& doors) {
    const bool want = alert.lockdownActive();
    if (want && !m_active) {
        // LOCKDOWN: lock every fully-closed, not-already-locked door; remember
        // exactly which ones so release restores only OUR locks.
        m_locked.clear();
        for (uint32_t i = 0; i < doors.count(); ++i) {
            Door& d = doors.at(i);
            if (d.state == DoorState::Closed && !d.locked) {
                d.locked = true;
                m_locked.push_back(i);
            }
        }
        m_active = true;
        x3::logInfo("alert: LOCKDOWN — " + std::to_string(m_locked.size())
                    + " zone door(s) locked");
    } else if (!want && m_active) {
        for (uint32_t i : m_locked)
            if (i < doors.count()) doors.at(i).locked = false;
        x3::logInfo("alert: lockdown released — " + std::to_string(m_locked.size())
                    + " door(s) unlocked");
        m_locked.clear();
        m_active = false;
    }
}

// ===========================================================================
// Headless self-test (--test-alert)
// ===========================================================================

namespace {
int a_pass = 0, a_fail = 0;
void acheck(bool cond, const char* name) {
    if (cond) { ++a_pass; x3::logInfo(std::string("[alert-test] PASS ") + name); }
    else      { ++a_fail; x3::logError(std::string("[alert-test] FAIL ") + name); }
}
} // namespace

bool runAlertSelfTest() {
    a_pass = a_fail = 0;
    const float dt = 1.0f / 60.0f;

    AlertSystem alert;
    alert.configure(defaultAlertConfig());
    const AlertConfig& cfg = alert.config();

    // Event collector (A7 asserts the x3.fire sequence).
    std::vector<int> events;
    alert.setEventSink([&](int nw, int) { events.push_back(nw); });

    // One stationary guard at the origin = the facility's ears.
    const x3::phys::Vec3 guard{0, 0, 0};
    const x3::phys::Vec3 farAway{500, 0, 500};
    x3::phys::Vec3 player{10, 0, 0};

    auto tick = [&](float seconds, const x3::phys::Vec3* obs, uint32_t nObs, bool seen) {
        const int frames = (int)(seconds / dt);
        for (int f = 0; f < frames; ++f) alert.update(dt, player, obs, nObs, seen);
    };

    // ---- A2 first (from cold): an UNHEARD gunshot changes nothing ----
    alert.reportGunshot(farAway);   // 700 m from the only guard
    tick(0.5f, &guard, 1, false);
    acheck(alert.level() == 0 && alert.heat() == 0.0f && events.empty(),
           "A2 unheard gunshot (out of every guard's earshot) raises nothing");

    // ---- A1: a HEARD gunshot -> level 1 + investigate pos + event ----
    alert.reportGunshot(x3::phys::Vec3{12, 0, 0});   // 12 m from the guard
    tick(0.2f, &guard, 1, false);
    x3::phys::Vec3 inv{};
    const bool gotInv = alert.takeInvestigatePos(inv);
    acheck(alert.level() == 1 && gotInv && std::fabs(inv.x - 12.0f) < 0.01f
               && events.size() == 1 && events[0] == 1,
           "A1 heard gunshot -> INVESTIGATE (level 1) + investigate pos + x3.fire");

    // ---- A3: a corpse raises SEARCH only when a patrol finds it ----
    alert.registerCorpse(x3::phys::Vec3{30, 0, 0});   // body dumped 30 m out
    tick(1.0f, &guard, 1, false);
    const bool notFoundYet = (alert.level() == 1 && alert.discoveredCorpseCount() == 0);
    // The patrol walks onto the body.
    const x3::phys::Vec3 patrolAtBody{29, 0, 0};
    tick(0.2f, &patrolAtBody, 1, false);
    const int spawnsOn2 = alert.takeSpawnRequests();
    acheck(notFoundYet && alert.level() == 2 && alert.discoveredCorpseCount() == 1
               && spawnsOn2 == cfg.searchSpawns,
           "A3 corpse found on the patrol path -> SEARCH (level 2) + extra-guard spawns");

    // ---- A4: sustained witness LOS -> LOCKDOWN; doors lock (pre-locked kept) ----
    DoorSystem doors;
    Door dA; dA.state = DoorState::Closed; dA.locked = false; doors.add(dA);
    Door dB; dB.state = DoorState::Closed; dB.locked = true;  doors.add(dB);   // pre-locked (Door C style)
    Door dC; dC.state = DoorState::Open;   dC.locked = false; doors.add(dC);   // open: not locked by lockdown
    AlertDoorLock lockdown;
    lockdown.update(alert, doors);
    const bool noLockYet = !doors.at(0).locked;
    tick(6.0f, &guard, 1, /*seen=*/true);   // witnessed + sustained contact
    lockdown.update(alert, doors);
    acheck(noLockYet && alert.level() >= 3 && alert.lockdownActive()
               && doors.at(0).locked && doors.at(1).locked && !doors.at(2).locked
               && lockdown.lockedCount() == 1 && alert.redShift() > 0.0f
               && alert.alarmOn(),
           "A4 sustained witness -> LOCKDOWN (level 3): closed doors lock, red shift, alarm");

    // ---- A6: continued contact escalates to KILL SQUAD (level 4) ----
    tick(8.0f, &guard, 1, /*seen=*/true);
    const int squad = alert.takeSpawnRequests();
    acheck(alert.level() == 4 && squad >= cfg.killSquadSpawns,
           "A6 continued contact at lockdown -> KILL SQUAD (level 4) + squad spawns");

    // ---- A5: hiding decays 4 -> 0 inside the expected windows; doors release ----
    {
        // Expected per-step hide times from heatMax: delay + drain to each band.
        // Just assert each step lands inside a generous window and order holds.
        float t = 0.0f;
        int   last = alert.level();
        float stepTime[5] = { -1, -1, -1, -1, -1 };   // time the level DROPPED TO k
        const float maxSim = 120.0f;
        while (t < maxSim && alert.level() > 0) {
            alert.update(dt, player, &guard, 1, /*seen=*/false);
            t += dt;
            if (alert.level() != last) {
                stepTime[alert.level()] = t;
                last = alert.level();
                lockdown.update(alert, doors);
            }
        }
        lockdown.update(alert, doors);
        // Sanity: heat 320 -> (delay 4 s) drain 12/s down to 144 (=240*.6) is
        // ~18.7 s for the 4->3 step; every later step adds seconds, strictly
        // ordered. Assert the first step lands in a generous window.
        const bool ordered = stepTime[3] > 5.0f && stepTime[2] > stepTime[3]
                          && stepTime[1] > stepTime[2] && stepTime[0] > stepTime[1];
        const bool released = !doors.at(0).locked && doors.at(1).locked;   // pre-lock kept
        acheck(alert.level() == 0 && ordered && stepTime[3] > 15.0f && stepTime[3] < 25.0f
                   && released,
               "A5 hiding decays 4->3->2->1->0 on the per-level clocks; lockdown releases");
    }

    // ---- A7: the x3.fire sequence covered every transition in order ----
    {
        const std::vector<int> expect = { 1, 2, 3, 4, 3, 2, 1, 0 };
        acheck(events == expect, "A7 x3.fire(\"alert_changed\") fired per transition, in order");
    }

    // ---- A8: debugForceLevel — the canon lockdown STAGING path (the
    // --screenshot-alert proof shot + console cheats drive the machine through
    // this instead of simulated stimuli). Force 3 on a fresh system + door set:
    // lockdown/alarm/red-shift engage and an AlertDoorLock locks the closed
    // door; force 0 releases exactly that lock. Fired events ride the sink. ----
    {
        AlertSystem staged;
        staged.configure(defaultAlertConfig());
        int stagedEvents = 0;
        staged.setEventSink([&](int, int) { ++stagedEvents; });
        DoorSystem sdoors;
        Door sd; sd.state = DoorState::Closed; sd.locked = false; sdoors.add(sd);
        AlertDoorLock slock;
        staged.debugForceLevel(3);
        slock.update(staged, sdoors);
        const bool engaged = staged.level() == 3 && staged.lockdownActive() &&
                             staged.alarmOn() && staged.redShift() > 0.0f &&
                             sdoors.at(0).locked && slock.lockedCount() == 1;
        staged.debugForceLevel(0);
        slock.update(staged, sdoors);
        const bool releasedOk = staged.level() == 0 && !staged.lockdownActive() &&
                                staged.redShift() == 0.0f && !sdoors.at(0).locked;
        acheck(engaged && releasedOk && stagedEvents >= 2,
               "A8 debugForceLevel stages LOCKDOWN (doors lock, red shift) + releases clean");
    }

    x3::logInfo("alert: " + std::to_string(a_pass) + "/"
                + std::to_string(a_pass + a_fail) + " passed");
    return a_fail == 0;
}

} // namespace x3::game
