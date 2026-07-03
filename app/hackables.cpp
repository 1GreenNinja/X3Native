// Watch-Dogs-2 environmental hacking — registry + effect dispatch + --test-hacking.
// See hackables.h. Clean-room X3Native game/slice code; engine/ stays pure.
#include "hackables.h"

#include "alert.h"        // self-test: heat routes through a REAL AlertSystem
#include "timeline.h"     // self-test: karma routes through a REAL TimelineState

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

const char* hackableTypeName(HackableType t) {
    switch (t) {
        case HackableType::Camera:        return "CAMERA";
        case HackableType::JunctionBox:   return "JUNCTION";
        case HackableType::ATM:           return "ATM";
        case HackableType::Vehicle:       return "VEHICLE";
        case HackableType::TrafficSignal: return "SIGNAL";
        case HackableType::Npc:           return "PROFILE";
        default:                          return "UNKNOWN";
    }
}

const char* hackableEffectVerb(HackableType t) {
    switch (t) {
        case HackableType::Camera:        return "SEE THROUGH";
        case HackableType::JunctionBox:   return "KILL LIGHTS";
        case HackableType::ATM:           return "SKIM CREDITS";
        case HackableType::Vehicle:       return "POP LOCKS";
        case HackableType::TrafficSignal: return "SPOOF SIGNAL";
        case HackableType::Npc:           return "SCAN + SKIM";
        default:                          return "HACK";
    }
}

namespace {
// Repeatable effects (Camera feed / signal spoof) can be re-triggered; the rest are
// one-shot (a box is cut once, an ATM/NPC is skimmed once, a car is popped once).
bool isRepeatable(HackableType t) {
    return t == HackableType::Camera || t == HackableType::TrafficSignal;
}
} // namespace

HackResult computeHackEffect(const HackableObject& o) {
    HackResult r;
    r.ok     = true;
    r.type   = o.type;
    r.object = o.entity;
    switch (o.type) {
        case HackableType::Camera:
            r.heat = 25; r.karma = 0;
            r.effect = "CAMERA HIJACKED - HOSTILES MARKED";
            break;
        case HackableType::JunctionBox:
            r.heat = 15; r.karma = 1;   // non-lethal stealth play — a hair of good karma
            r.effect = "GRID CUT - LIGHTS OUT";
            break;
        case HackableType::ATM:
            r.heat = 20; r.karma = -3;  // theft
            r.credits = o.credits;
            r.effect = "SKIMMED " + std::to_string(o.credits) + " CREDITS";
            break;
        case HackableType::Vehicle:
            r.heat = 30; r.karma = -1;
            r.effect = "VEHICLE POPPED - ALARM TRIPPED";
            break;
        case HackableType::TrafficSignal:
            r.heat = 10; r.karma = 0;
            r.effect = "SIGNAL SPOOFED - GRIDLOCK";
            break;
        case HackableType::Npc:
            r.heat = 15;
            // Karma: the flat profiler default (-2), UNLESS the object carries a per-archetype
            // override (NpcLife's moral texture — vulnerable NPCs cost more; neutral ones 0).
            r.karma = o.karmaSet ? o.karmaValue : -2;
            r.credits = o.credits;
            r.effect = "PROFILE SCANNED - SKIMMED " + std::to_string(o.credits);
            r.scanName       = o.label.empty()      ? "UNKNOWN"  : o.label;
            r.scanOccupation = o.occupation.empty() ? "CIVILIAN" : o.occupation;
            r.scanDetail     = o.detail;
            break;
        default: r.ok = false; break;
    }
    return r;
}

uint32_t HackableRegistry::add(const HackableObject& o) {
    m_objs.push_back(o);
    return (uint32_t)m_objs.size() - 1;
}

void HackableRegistry::nearby(const x3::phys::Vec3& pos, float radius,
                              std::vector<uint32_t>& out) const {
    const float r2 = radius * radius;
    for (uint32_t i = 0; i < m_objs.size(); ++i) {
        const x3::phys::Vec3& p = m_objs[i].pos;
        const float dx = p.x - pos.x, dy = p.y - pos.y, dz = p.z - pos.z;
        if (dx * dx + dy * dy + dz * dz <= r2) out.push_back(i);
    }
}

uint32_t HackableRegistry::lookTarget(const x3::phys::Vec3& eye, const x3::phys::Vec3& fwd,
                                      float maxDist, float maxCosAngle) const {
    float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (fl < 1e-6f) return kNoLink;
    const float fx = fwd.x / fl, fy = fwd.y / fl, fz = fwd.z / fl;
    uint32_t best = kNoLink;
    float    bestDist = maxDist;
    for (uint32_t i = 0; i < m_objs.size(); ++i) {
        const x3::phys::Vec3& p = m_objs[i].pos;
        const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 1e-4f || d > maxDist) continue;
        const float cosang = (dx * fx + dy * fy + dz * fz) / d;
        if (cosang < maxCosAngle) continue;       // outside the aim cone
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

HackResult HackableRegistry::hack(uint32_t i) {
    HackResult r;
    if (i >= m_objs.size()) return r;              // ok=false
    HackableObject& o = m_objs[i];
    if (o.hacked && !isRepeatable(o.type)) return r;  // one-shot already spent

    r = computeHackEffect(o);
    if (!r.ok) return r;

    // Route through the alert + karma systems (WD2: hacking is loud + moral).
    if (m_sinks.onHeat)  m_sinks.onHeat(o.pos, r.heat);
    if (m_sinks.onKarma) m_sinks.onKarma(r.karma);
    // Per-type world effect.
    switch (o.type) {
        case HackableType::JunctionBox: if (m_sinks.onLightsOut) m_sinks.onLightsOut(o.entity); break;
        case HackableType::Vehicle:     if (m_sinks.onVehicle)   m_sinks.onVehicle(o.entity);   break;
        default: break;
    }
    o.hacked = true;                               // latch (repeatable types ignore it above)
    if (m_sinks.onResult) m_sinks.onResult(r);
    return r;
}

uint32_t HackableRegistry::hackedCount() const {
    uint32_t n = 0;
    for (const auto& o : m_objs) if (o.hacked) ++n;
    return n;
}

uint32_t HackableRegistry::countType(HackableType t) const {
    uint32_t n = 0;
    for (const auto& o : m_objs) if (o.type == t) ++n;
    return n;
}

// ===========================================================================
// Headless self-test (--test-hacking).
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[hacking-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[hacking-test] FAIL " + name); }
}

// Populate a registry with one of every type (a slice of a real district).
void seed(HackableRegistry& reg) {
    HackableObject cam;  cam.type = HackableType::Camera;        cam.pos = { 10, 4, 0 };  cam.entity = 100; reg.add(cam);
    HackableObject jb;   jb.type  = HackableType::JunctionBox;   jb.pos  = { -8, 1, 3 };  jb.entity  = 101; reg.add(jb);
    HackableObject atm;  atm.type = HackableType::ATM;           atm.pos = { 4, 1, -6 };  atm.entity = 102; atm.credits = 250; reg.add(atm);
    HackableObject veh;  veh.type = HackableType::Vehicle;       veh.pos = { -3, 1, -3 }; veh.entity = 103; reg.add(veh);
    HackableObject sig;  sig.type = HackableType::TrafficSignal; sig.pos = { 0, 5, 12 };  sig.entity = 104; reg.add(sig);
    HackableObject npc;  npc.type = HackableType::Npc;           npc.pos = { 2, 1, 2 };   npc.entity = 105; npc.credits = 40;
    npc.label = "MARA VOSS"; npc.occupation = "GRID TECH"; npc.detail = "OWES THE SYNDICATE"; reg.add(npc);
}
} // namespace

bool runHackingSelfTest() {
    g_pass = g_fail = 0;

    // X0 — a mixed registry with every type present.
    HackableRegistry reg;
    seed(reg);
    {
        bool all = true;
        for (uint32_t t = 0; t < kHackableTypeCount; ++t)
            if (reg.countType((HackableType)t) == 0) all = false;
        check(reg.count() == kHackableTypeCount && all,
              "X0 registry populated with every hackable type");
    }

    // X1 — the NetHack highlight toggles.
    check(!reg.highlight(), "X1a highlight off by default");
    reg.setHighlight(true);
    check(reg.highlight(), "X1b highlight toggles on");

    // X2 — nearby() + lookTarget() select correctly.
    {
        std::vector<uint32_t> near;
        reg.nearby(x3::phys::Vec3{ 0, 1, 0 }, 6.0f, near);
        // Within 6 m of origin: junction(-8 far), atm(~7.3 far), vehicle(~4.4 in),
        // npc(~2.8 in), signal(13 far), camera(~10.8 far) => vehicle + npc.
        bool haveVeh = false, haveNpc = false;
        for (uint32_t i : near) {
            if (reg.at(i).type == HackableType::Vehicle) haveVeh = true;
            if (reg.at(i).type == HackableType::Npc)     haveNpc = true;
        }
        check(near.size() == 2 && haveVeh && haveNpc, "X2a nearby() returns the in-radius objects");

        // Look straight down +X from the origin eye: the camera at (10,4,0) is ahead.
        uint32_t tgt = reg.lookTarget(x3::phys::Vec3{ 0, 4, 0 }, x3::phys::Vec3{ 1, 0, 0 },
                                      40.0f, 0.9f);
        check(tgt != kNoLink && reg.at(tgt).type == HackableType::Camera,
              "X2b lookTarget() picks the aimed-at object");
        // Aim away (-X): nothing in the cone.
        uint32_t none = reg.lookTarget(x3::phys::Vec3{ 0, 4, 0 }, x3::phys::Vec3{ -1, 0, 0 },
                                       40.0f, 0.9f);
        // (-8,1,3) junction is behind-ish; ensure the camera is NOT selected looking back.
        check(none == kNoLink || reg.at(none).type != HackableType::Camera,
              "X2c lookTarget() rejects objects outside the aim cone");
    }

    // X3/X4 — a hack fires its effect + raises HEAT (real AlertSystem) + moves KARMA
    // (real TimelineState) + per-type dispatch all route through the sinks.
    AlertSystem   alert;  alert.configure(defaultAlertConfig());
    TimelineState timeline;
    int  lightsOutEntity = -1, vehicleEntity = -1, credits = 0;
    bool sawScanCard = false;
    HackResult    lastResult;
    HackSinks sinks;
    sinks.onHeat      = [&](const x3::phys::Vec3& p, int /*h*/) { alert.reportTerminalHack(p); };
    sinks.onKarma     = [&](int d) { timeline.adjustKarma(d); };
    sinks.onLightsOut = [&](uint32_t e) { lightsOutEntity = (int)e; };
    sinks.onVehicle   = [&](uint32_t e) { vehicleEntity = (int)e; };
    sinks.onResult    = [&](const HackResult& r) {
        lastResult = r; credits += r.credits;
        if (r.type == HackableType::Npc && !r.scanName.empty()) sawScanCard = true;
    };
    reg.setSinks(sinks);

    const int karma0 = timeline.adjustKarma(0);   // read current (delta 0 = no change)
    const float heat0 = alert.heat();

    // Hack the junction box (index 1) — lights out + a hair of good karma.
    HackResult jbR = reg.hack(1);
    check(jbR.ok && lightsOutEntity == 101, "X4a junction-box hack routes lights-out");
    check(alert.heat() > heat0 && alert.level() >= 1, "X3a a hack RAISES heat (alert level >= 1)");

    // Hack the ATM (index 2) — skims its credits, bad karma.
    HackResult atmR = reg.hack(2);
    check(atmR.ok && atmR.credits == 250 && credits == 250, "X4b ATM hack skims credits");

    // Hack the vehicle (index 3) — pop/alarm dispatch.
    reg.hack(3);
    check(vehicleEntity == 103, "X4c vehicle hack routes pop/alarm");

    // Hack the NPC (index 5) — the scan card + skim.
    HackResult npcR = reg.hack(5);
    check(npcR.ok && sawScanCard && npcR.scanName == "MARA VOSS" &&
          npcR.scanOccupation == "GRID TECH", "X4d NPC hack yields the scan card");

    check(timeline.adjustKarma(0) < karma0, "X3b hacks MOVE karma (net theft/profiling lowered it)");

    // X5 — one-shot latch vs repeatable.
    {
        const int creditsBefore = credits;
        HackResult atm2 = reg.hack(2);       // ATM already skimmed
        check(!atm2.ok && credits == creditsBefore, "X5a one-shot ATM does not re-skim");
        check(reg.at(2).hacked, "X5b one-shot object stays latched");
        // Camera is repeatable: two hacks both succeed.
        HackResult c1 = reg.hack(0), c2 = reg.hack(0);
        check(c1.ok && c2.ok, "X5c repeatable camera re-fires");
    }

    // X6 — hacked census + a clean rebuild (no state bleed).
    check(reg.hackedCount() >= 4, "X6a hacked census counts spent objects");
    {
        HackableRegistry fresh;
        seed(fresh);
        check(fresh.hackedCount() == 0 && fresh.count() == kHackableTypeCount,
              "X6b a fresh registry starts clean");
    }

    x3::logInfo(std::string("hacking: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
