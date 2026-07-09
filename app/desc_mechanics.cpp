// DESC MECHANICS impl + --test-descmech (W9-1). See desc_mechanics.h.
#include "desc_mechanics.h"

#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// build — register the five Tier-A interact points onto the loaded tower.
// Anchor offsets mirror the room_dressing desc-gold placements (the coolant
// console at cx+1.6/cz+1.2, the Power Junction bench crate at cx-1.5/cz+0.8,
// the Central Control Hub console arc's center console at cx/cz-2.6).
// ---------------------------------------------------------------------------
bool DescMechanics::build(const CanonFloor& floor, CanonPlay& play, StoryFlags& flags) {
    m_floor = &floor;
    m_play  = &play;
    m_flags = &flags;

    m_coolantRoom    = floor.roomByName("Coolant System");
    m_coldRoom       = floor.roomByName("Cold Room");
    m_deconRoom      = floor.roomByName("Decontamination");
    m_pharmacyRoom   = floor.roomByName("Pharmacy");
    m_quarantineRoom = floor.roomByName("Quarantine Zone");
    const uint32_t junctionRoom = floor.roomByName("Power Junction");
    const uint32_t hubRoom      = floor.roomByName("Central Control Hub");

    auto anchor = [&](uint32_t room, float dx, float dz) {
        const CanonRoom& R = floor.rooms[room];
        return x3::phys::Vec3{ R.cx + dx, R.y0() + 1.0f, R.cz + dz };
    };

    // 1. COOLANT SABOTAGE (F4). "Liquid nitrogen. Sabotage = boss weakness."
    if (m_coolantRoom != kNoRoom) {
        InteractPoint p;
        p.id     = "coolant_console";
        p.pos    = anchor(m_coolantRoom, 1.6f, 1.2f);
        p.radius = 2.6f;
        p.prompt = "[E] SABOTAGE COOLANT FLOW";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("f4.coolant_sabotaged");
            m_play->applyCoolantSabotage();
            m_coolantGlowKill = true;   // host edge: the console's glow dies
            return "COOLANT PURGED - THE COLLECTIVE'S THERMAL SHIELDING IS FAILING";
        };
        m_points.add(std::move(p));
    }

    // 2. EMP CRAFT (F4). "EMP device craftable here."
    if (junctionRoom != kNoRoom) {
        InteractPoint p;
        p.id     = "emp_bench";
        p.pos    = anchor(junctionRoom, -1.5f, 0.8f);
        p.radius = 2.6f;
        p.prompt = "[E] ASSEMBLE EMP DEVICE";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("f4.emp_crafted");
            m_items.add(kItemEmp);
            return "EMP DEVICE ASSEMBLED - PRESS E IN THE OPEN TO DISCHARGE "
                   "(STUNS SYNTHETICS)";
        };
        m_points.add(std::move(p));
    }

    // 3. MASTER HACK (F5). "Master hack terminal. Sarah's objective."
    if (hubRoom != kNoRoom) {
        InteractPoint p;
        p.id     = "master_hack";
        p.pos    = anchor(hubRoom, 0.0f, -2.6f);
        p.radius = 3.0f;
        p.prompt = "[E] RUN MASTER HACK";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("f5.hacked");
            const uint32_t n = m_play->setDroneSpeciesDocile(*m_floor, 5);
            return "MASTER HACK COMPLETE - DRONE NETWORK POWERED DOWN (" +
                   std::to_string(n) + " UNITS DOCILE)";
        };
        m_points.add(std::move(p));
    }

    // 5. ANTIDOTE CRAFT (F2). "Antidote components." + "Infection research."
    if (m_pharmacyRoom != kNoRoom) {
        InteractPoint p;
        p.id     = "pharmacy_bench";
        p.pos    = anchor(m_pharmacyRoom, 0.0f, 0.0f);
        p.radius = 2.8f;
        p.prompt = "[E] SYNTHESIZE ANTIDOTE";
        p.requiresFlags = { "f2.pharmacy_components", "f2.quarantine_research" };
        p.missingBark   = "MISSING: PHARMACY COMPONENTS + QUARANTINE INFECTION RESEARCH";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("antidote.crafted");
            m_items.add(kItemAntidote);
            return "ANTIDOTE SYNTHESIZED - PRESS E TO ADMINISTER WHEN INFECTED";
        };
        m_points.add(std::move(p));
    }
    // (4. Cold Room and the Decontamination cure are roomAt-driven in tick().)

    // A loaded save may already carry the sabotage flag — re-apply the boss
    // multiplier (idempotent) so the weakness survives save/load.
    if (flags.has("f4.coolant_sabotaged")) play.applyCoolantSabotage();

    x3::logInfo("[descmech] built: " + std::to_string(m_points.count()) +
                " interact points (coolant/EMP/hack/antidote), cold room " +
                (m_coldRoom != kNoRoom ? "armed" : "absent") + ", decon " +
                (m_deconRoom != kNoRoom ? "armed" : "absent"));
    return m_points.count() > 0;
}

// ---------------------------------------------------------------------------
// tick — roomAt-driven verbs + pickup->flag polling + the DoT tickers.
// ---------------------------------------------------------------------------
void DescMechanics::tick(float dt, const x3::phys::Vec3& eye, IDamageSink* player) {
    if (!built() || dt <= 0.0f) return;

    const uint32_t rm = m_floor->roomAt(eye.x, eye.y, eye.z);

    // 4. COLD ROOM (F3). "-40C. Timer: 30s before damage."
    const bool inCold = (m_coldRoom != kNoRoom && rm == m_coldRoom);
    if (inCold) {
        if (!m_inColdRoom)
            queueBark("WARNING: -40 C - EXPOSURE DAMAGE IN 30 SECONDS");
        m_coldDwell += dt;
        if (m_coldDwell >= kColdRoomGraceSec && !m_status.chillActive()) {
            m_status.setChill(true);
            queueBark("HYPOTHERMIA SETTING IN - GET OUT OF THE COLD");
        }
    } else {
        m_coldDwell = 0.0f;
        if (m_status.chillActive()) m_status.setChill(false);
    }
    m_inColdRoom = inCold;

    // Tier-B #8 (free once status exists): DECONTAMINATION kills infection.
    if (m_deconRoom != kNoRoom && rm == m_deconRoom && m_status.infected()) {
        m_status.cureInfection();
        queueBark("DECONTAMINATION CYCLE COMPLETE - INFECTION PURGED");
    }

    // 5. Pickup -> flag polling (the Pharmacy components + the Quarantine
    // research note are existing CanonItem pickups; grabbing them arms the
    // antidote bench's gate). Cheap linear scan, skipped once both flags hold.
    if (m_pharmacyRoom != kNoRoom && !m_flags->has("f2.pharmacy_components")) {
        for (const CanonItem& it : m_play->upperItems()) {
            if (it.room != m_pharmacyRoom || !it.taken) continue;
            if (it.kind != CanonItemKind::Health && it.kind != CanonItemKind::NanoBooster)
                continue;
            m_flags->set("f2.pharmacy_components");
            queueBark("ANTIDOTE COMPONENTS COLLECTED (PHARMACY)");
            break;
        }
    }
    if (m_quarantineRoom != kNoRoom && !m_flags->has("f2.quarantine_research")) {
        for (const CanonItem& it : m_play->upperItems()) {
            if (it.room != m_quarantineRoom || !it.taken) continue;
            if (it.kind != CanonItemKind::LoreTerminal) continue;
            m_flags->set("f2.quarantine_research");
            queueBark("INFECTION RESEARCH DOWNLOADED (QUARANTINE ZONE)");
            break;
        }
    }

    // The DoT tickers (chill / infection) — damage lands through the player's
    // takeDamage, so the pain cue + HUD damage flash fire exactly like a hit.
    m_status.tick(dt, player);
}

bool DescMechanics::onUse(const x3::phys::Vec3& eye, std::string* barkOut) {
    if (!built()) return false;
    return m_points.onUse(eye, *m_flags, barkOut);
}

bool DescMechanics::onUseItem(const x3::phys::Vec3& eye, std::string* barkOut) {
    if (!built()) return false;
    // Antidote first — only meaningful while infected (never wasted otherwise).
    if (m_status.infected() && m_items.has(kItemAntidote)) {
        m_items.consume(kItemAntidote);
        m_status.cureInfection();
        if (barkOut) *barkOut = "ANTIDOTE ADMINISTERED - INFECTION NEUTRALIZED";
        return true;
    }
    // The EMP: discharge only when it would DO something (no accidental waste
    // from an E pressed at nothing — the charge is held).
    if (m_items.has(kItemEmp)) {
        const uint32_t n = m_play->empStun(eye, kEmpRadius, kEmpStunSecs);
        if (n == 0) {
            if (barkOut) *barkOut = "EMP: NO SYNTHETIC SIGNATURES IN RANGE - CHARGE HELD";
            return true;
        }
        m_items.consume(kItemEmp);
        if (barkOut) *barkOut = "EMP DISCHARGED - " + std::to_string(n) +
                                " SYNTHETIC(S) DISABLED";
        return true;
    }
    return false;
}

void DescMechanics::onCue(const GameCue& cue) {
    if (!built() || m_status.infected()) return;
    // A landed enemy hit on the player carries the attacker species and the
    // player position (monster.cpp emits Melee/BulletImpact at the target).
    if (cue.kind != CueKind::MeleeImpact && cue.kind != CueKind::BulletImpact) return;
    if (cue.species != (uint32_t)EnemyType::Verthani) return;   // creature species
    const uint32_t rm = m_floor->roomAt(cue.pos.x, cue.pos.y, cue.pos.z);
    if (rm == kNoRoom || rm >= m_floor->roomFloorNum.size()) return;
    const int fn = m_floor->roomFloorNum[rm];
    if (fn != 2 && fn != 3) return;   // the infection floors (Medical / Genetics)
    // Small per-hit chance (deterministic LCG stream, test-overridable).
    m_rng = m_rng * 1664525u + 1013904223u;
    const float roll = (float)((m_rng >> 8) & 0xFFFFu) / 65536.0f;
    if (roll >= m_infectChance) return;
    m_status.infect();
    queueBark("YOUR VEINS BURN - INFECTED. FIND THE ANTIDOTE OR DECONTAMINATION.");
}

std::string DescMechanics::takeBark() {
    if (m_barks.empty()) return std::string();
    std::string b = m_barks.front();
    m_barks.erase(m_barks.begin());
    return b;
}

std::string DescMechanics::hudStatusLine() const {
    std::string s;
    auto append = [&s](const char* tag) {
        if (!s.empty()) s += "  |  ";
        s += tag;
    };
    if (m_items.has(kItemEmp))      append("EMP READY");
    if (m_items.has(kItemAntidote)) append("ANTIDOTE");
    if (m_status.infected())        append("INFECTED");
    if (m_status.chillActive())     append("FREEZING");
    return s;
}

bool DescMechanics::coolantGlowKillPending() {
    const bool p = m_coolantGlowKill;
    m_coolantGlowKill = false;
    return p;
}

void DescMechanics::queueBark(std::string b) {
    m_barks.push_back(std::move(b));
    x3::logInfo("[descmech] bark: " + m_barks.back());
}

// ---------------------------------------------------------------------------
// killRoomGlow — the sabotaged console's glow dies (faint red emergency ember).
// ---------------------------------------------------------------------------
uint32_t killRoomGlow(std::vector<CanonLight>& lights, uint32_t room) {
    uint32_t n = 0;
    for (CanonLight& l : lights) {
        if (l.room != room) continue;
        l.light.color[0] = 0.30f;   // dead-console ember (was the cyan coolant glow)
        l.light.color[1] = 0.05f;
        l.light.color[2] = 0.04f;
        ++n;
    }
    x3::logInfo("[descmech] coolant glow killed (" + std::to_string(n) +
                " room light(s) dropped to the emergency ember)");
    return n;
}

// =====================================================================================
// Headless self-test (--test-descmech). No window / Vulkan.
// =====================================================================================
namespace {

int g_dpass = 0, g_dfail = 0;
void dcheck(bool cond, const char* name) {
    if (cond) { ++g_dpass; x3::logInfo(std::string("[descmech-test] PASS ") + name); }
    else      { ++g_dfail; x3::logError(std::string("[descmech-test] FAIL ") + name); }
}

// Trivial damage sink: counts hits + total damage (never dies, no iframes).
struct SinkStub : public IDamageSink {
    int hits = 0, total = 0;
    x3::phys::Vec3 at{};
    bool takeDamage(int amount) override { ++hits; total += amount; return true; }
    x3::phys::Vec3 damageTargetPos() const override { return at; }
    bool isAlive() const override { return true; }
};

x3::phys::Vec3 roomEye(const CanonFloor& f, uint32_t room) {
    const CanonRoom& R = f.rooms[room];
    return x3::phys::Vec3{ R.cx, R.y0() + 1.6f, R.cz };
}

} // namespace

bool runDescMechSelfTest() {
    g_dpass = g_dfail = 0;

    // ---- D1: ItemStore (the minimal carryable interface). ----
    {
        ItemStore inv;
        bool ok = inv.empty() && !inv.has("emp_device");
        inv.add("emp_device");
        inv.add("antidote", 2);
        ok = ok && inv.has("emp_device") && inv.count("antidote") == 2 && !inv.empty();
        ok = ok && !inv.consume("emp_device", 2);        // more than held -> refused
        ok = ok && inv.consume("emp_device") && !inv.has("emp_device");
        ok = ok && inv.consume("antidote") && inv.count("antidote") == 1;
        dcheck(ok, "D1 ItemStore add/has/count/consume (refuses over-consume)");
    }

    // ---- D2: Interactables (register / prompt / one-shot / flag gate). ----
    {
        StoryFlags flags;
        Interactables ix;
        int fired = 0;
        InteractPoint p;
        p.id = "t1"; p.pos = { 0, 0, 0 }; p.radius = 2.0f;
        p.prompt = "[E] TEST";
        p.onUse  = [&fired](StoryFlags& f) { ++fired; f.set("t1.done"); return std::string("OK"); };
        ix.add(std::move(p));
        InteractPoint g;
        g.id = "t2"; g.pos = { 10, 0, 0 }; g.radius = 2.0f;
        g.prompt = "[E] GATED";
        g.requiresFlags = { "t1.done" };
        g.missingBark   = "NEED T1";
        g.onUse = [](StoryFlags&) { return std::string("G-OK"); };
        ix.add(std::move(g));

        bool ok = ix.prompt({ 0.5f, 0, 0 }) == "[E] TEST" &&
                  ix.prompt({ 5, 0, 0 }).empty() &&
                  ix.prompt({ 0, 5, 0 }).empty();          // 3D radius: floor above misses
        std::string bark;
        // Gated point refuses while the flag is missing (missingBark surfaces).
        ok = ok && ix.onUse({ 10, 0, 0 }, flags, &bark) && bark == "NEED T1" &&
             !flags.has("t1.done");
        // The open point fires, sets its flag, and one-shots.
        ok = ok && ix.onUse({ 0.5f, 0, 0 }, flags, &bark) && bark == "OK" &&
             fired == 1 && flags.has("t1.done");
        ok = ok && ix.prompt({ 0.5f, 0, 0 }).empty();      // consumed -> no prompt
        ok = ok && !ix.onUse({ 0.5f, 0, 0 }, flags, &bark);   // consumed -> E falls through
        // The gate now passes.
        ok = ok && ix.onUse({ 10, 0, 0 }, flags, &bark) && bark == "G-OK";
        dcheck(ok, "D2 Interactables prompt/radius/one-shot/flag-gate");
    }

    // ---- D3: StatusEffects (chill + infection tickers with source tags). ----
    {
        SinkStub sink;
        StatusEffects st;
        st.tick(10.0f, &sink);                    // nothing active -> no damage
        bool ok = sink.total == 0;
        st.setChill(true);
        for (int i = 0; i < 61; ++i) st.tick(0.1f, &sink);    // 6.1 s -> 3 ticks x2
        ok = ok && st.chillActive() && sink.total == 6 && st.chillDamageDealt() == 6;
        st.setChill(false);
        st.tick(10.0f, &sink);
        ok = ok && sink.total == 6;               // cleared -> no further chill
        st.infect();
        for (int i = 0; i < 91; ++i) st.tick(0.1f, &sink);    // 9.1 s -> 3 ticks x1
        ok = ok && st.infected() && st.infectionDamageDealt() == 3 && sink.total == 9;
        st.cureInfection();
        st.tick(10.0f, &sink);
        ok = ok && !st.infected() && sink.total == 9;
        dcheck(ok, "D3 StatusEffects chill 2dmg/2s + infection 1dmg/3s, cure/clear stop them");
    }

    // ---- D4: EMP stun freezes a synth; docile stops targeting; kill still counts.
    // (Direct MonsterSystem drive — the framework-level truth the AoE rides.) ----
    {
        HeadlessRenderDevice device;
        std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
        physics->init();
        Scene scene;
        MonsterManager mm;
        const uint32_t mi = mm.spawn(scene, device, *physics, riggedGlbRoot(),
                                     x3::phys::Vec3{ 0, 0, 0 },
                                     tuningFor(EnemyType::BlueSynth));
        MonsterSystem& m = mm.at(mi);
        SinkStub sink;
        sink.at = { 1.5f, 0.5f, 0 };              // point-blank target
        const x3::phys::Vec3 p0 = m.pos();

        // STUN: frozen in place, no attack lands, timer expires on schedule.
        m.stun(2.0f);
        bool ok = m.stunned() && m.species() == EnemyType::BlueSynth;
        for (int i = 0; i < 60; ++i)
            m.update(1.0f / 60.0f, scene, *physics, sink.at, &sink, AttackFxFn{});
        const x3::phys::Vec3 p1 = m.pos();
        ok = ok && m.stunned() && sink.hits == 0 &&
             std::fabs(p1.x - p0.x) < 1e-3f && std::fabs(p1.z - p0.z) < 1e-3f;
        for (int i = 0; i < 90; ++i)              // +1.5 s -> past the 2 s stun
            m.update(1.0f / 60.0f, scene, *physics, sink.at, &sink, AttackFxFn{});
        ok = ok && !m.stunned();

        // DOCILE: permanently inert (no attacks over 3 s in reach)...
        m.setDocile(true);
        sink.hits = 0;
        for (int i = 0; i < 180; ++i)
            m.update(1.0f / 60.0f, scene, *physics, sink.at, &sink, AttackFxFn{});
        ok = ok && m.docile() && sink.hits == 0 && m.alive();
        // ...but killing it still works/counts (fire() path untouched).
        for (int shot = 0; shot < 8 && m.alive(); ++shot) {
            const x3::phys::Vec3 cp = m.pos();
            const x3::phys::Vec3 eye{ cp.x - 1.6f, cp.y, cp.z };
            m.fire(eye, x3::phys::Vec3{ 1, 0, 0 }, scene, *physics, 100000);
        }
        ok = ok && !m.alive();
        dcheck(ok, "D4 stun freezes a synth (no move/attack, expires); docile is inert but killable");
        mm.shutdown();
        physics->shutdown();
    }

    // ---- The tower-level verbs (skip-as-PASS without the canonical JSON). ----
    CanonFloor tower = loadCanonTower(canonProjectJsonPath());
    if (!tower.valid()) {
        x3::logInfo("--test-descmech: tower JSON absent — framework-only run");
        x3::logInfo("--test-descmech: " + std::to_string(g_dpass) + " passed, " +
                    std::to_string(g_dfail) + " failed");
        return g_dfail == 0;
    }

    HeadlessRenderDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    Scene scene;
    buildCanonFloor(tower, scene, device, *physics);
    CanonPlay play;
    play.build(tower, scene, device, *physics, riggedGlbRoot(), canonGirlsDialogPath());

    StoryFlags flags;
    DescMechanics dm;
    const bool builtOk = dm.build(tower, play, flags);

    // ---- D5: all four interact points registered onto real rooms. ----
    {
        bool ok = builtOk && dm.points().count() == 4 &&
                  dm.points().find("coolant_console") && dm.points().find("emp_bench") &&
                  dm.points().find("master_hack") && dm.points().find("pharmacy_bench");
        // Each anchor resolves back to its own room via roomAt.
        if (ok) {
            const uint32_t coolant = tower.roomByName("Coolant System");
            const InteractPoint* cp = dm.points().find("coolant_console");
            ok = tower.roomAt(cp->pos.x, cp->pos.y, cp->pos.z) == coolant;
        }
        dcheck(ok, "D5 all four interact points registered on their canon rooms");
    }

    // ---- D6: COOLANT SABOTAGE — interact fires, flag sets, The Collective takes
    // x1.5 (two identical real shots: post-sabotage damage = 1.5x pre), glow dies. --
    {
        MonsterSystem* col = play.findLadderBoss("The Collective");
        bool ok = col && col->alive();
        int d1 = 0, d2 = 0;
        if (ok) {
            // Two identical point-blank shots at the body CENTER (below the
            // headshot zone), one each side of the sabotage. 100 base damage.
            auto shoot = [&]() -> int {
                const int before = col->hp();
                const x3::phys::Vec3 cp = col->pos();
                const x3::phys::Vec3 eye{ cp.x - 1.8f, cp.y, cp.z };
                col->fire(eye, x3::phys::Vec3{ 1, 0, 0 }, scene, *physics, 100);
                return before - col->hp();
            };
            d1 = shoot();
            std::string bark;
            const x3::phys::Vec3 at = dm.points().find("coolant_console")->pos;
            ok = dm.onUse(at, &bark) && flags.has("f4.coolant_sabotaged") &&
                 play.coolantSabotaged() && !bark.empty();
            d2 = shoot();
            ok = ok && d1 > 0 && d2 * 2 == d1 * 3;   // exactly x1.5
            ok = ok && dm.coolantGlowKillPending() && !dm.coolantGlowKillPending();
        }
        // The glow-kill lever on a synthetic light list (2 in-room + 1 elsewhere).
        {
            const uint32_t coolant = tower.roomByName("Coolant System");
            std::vector<CanonLight> ls(3);
            ls[0].room = coolant; ls[1].room = coolant; ls[2].room = coolant + 1;
            for (CanonLight& l : ls) { l.light.color[0] = l.light.color[1] = l.light.color[2] = 3.0f; }
            ok = ok && killRoomGlow(ls, coolant) == 2 &&
                 ls[0].light.color[2] < 0.1f && ls[2].light.color[2] > 1.0f;
        }
        x3::logInfo("    D6 shots: pre=" + std::to_string(d1) + " post=" + std::to_string(d2));
        dcheck(ok, "D6 coolant sabotage: interact -> flag -> Collective x1.5 -> glow dies");
    }

    // ---- D7: EMP — bench interact grants the item; discharge stuns synths in
    // range (Drone Bay Alpha) and consumes the charge; held when nothing in range. --
    {
        std::string bark;
        bool ok = dm.onUse(dm.points().find("emp_bench")->pos, &bark) &&
                  dm.items().has(DescMechanics::kItemEmp) && flags.has("f4.emp_crafted");
        // Nothing synthetic near Jake's Cell -> the charge is HELD, not wasted.
        const uint32_t cell = tower.roomByName("Jake's Cell");
        ok = ok && cell != kNoRoom &&
             dm.onUseItem(roomEye(tower, cell), &bark) &&
             dm.items().has(DescMechanics::kItemEmp) &&
             bark.find("CHARGE HELD") != std::string::npos;
        // In the F5 drone bay the discharge lands: stuns >= 1, consumes the EMP.
        const uint32_t bay = tower.roomByName("Drone Bay Alpha");
        ok = ok && bay != kNoRoom &&
             dm.onUseItem(roomEye(tower, bay), &bark) &&
             !dm.items().has(DescMechanics::kItemEmp) &&
             bark.find("EMP DISCHARGED") != std::string::npos;
        x3::logInfo("    D7 discharge bark: " + bark);
        dcheck(ok, "D7 EMP: craft -> held-when-empty-range -> AoE discharge stuns + consumes");
    }

    // ---- D8: MASTER HACK — interact sets f5.hacked + dociles F5 drone species. --
    {
        std::string bark;
        bool ok = dm.onUse(dm.points().find("master_hack")->pos, &bark) &&
                  flags.has("f5.hacked") &&
                  bark.find("POWERED DOWN") != std::string::npos;
        // The interact reported > 0 units (F5 spawns BlueSynth-heavy squads).
        ok = ok && bark.find("(0 UNITS") == std::string::npos;
        // Idempotence: a direct re-run dociles nobody new.
        ok = ok && play.setDroneSpeciesDocile(tower, 5) == 0;
        x3::logInfo("    D8 hack bark: " + bark);
        dcheck(ok, "D8 master hack: flag + F5 drone species docile (idempotent)");
    }

    // ---- D9: COLD ROOM — entry warning, 30 s grace, chill ticks inside, clears on exit. --
    {
        SinkStub sink;
        const uint32_t cold = tower.roomByName("Cold Room");
        bool ok = cold != kNoRoom;
        const x3::phys::Vec3 inCold = roomEye(tower, cold);
        dm.tick(0.1f, inCold, &sink);
        std::string firstBark = dm.takeBark();
        ok = ok && firstBark.find("-40") != std::string::npos;     // the entry warning
        ok = ok && !dm.status().chillActive();
        for (int i = 0; i < 320; ++i) dm.tick(0.1f, inCold, &sink);   // +32 s inside
        ok = ok && dm.status().chillActive() && sink.total >= 2;      // ticks landed
        const int atExit = sink.total;
        const uint32_t cell = tower.roomByName("Jake's Cell");
        dm.tick(0.1f, roomEye(tower, cell), &sink);                   // step out
        ok = ok && !dm.status().chillActive() && dm.coldDwell() == 0.0f;
        for (int i = 0; i < 100; ++i) dm.tick(0.1f, roomEye(tower, cell), &sink);
        ok = ok && sink.total == atExit;                              // no ticks outside
        x3::logInfo("    D9 chill damage dealt inside: " + std::to_string(atExit));
        dcheck(ok, "D9 cold room: entry warning + 30s grace + chill DoT inside + clears on exit");
    }

    // ---- D10: INFECTION + ANTIDOTE — creature hit infects (F2/F3 only), the
    // gated bench refuses until both pickups, then crafts; the antidote cures. --
    {
        SinkStub sink;
        std::string bark;
        // The bench is GATED before the pickups.
        bool ok = dm.onUse(dm.points().find("pharmacy_bench")->pos, &bark) &&
                  !flags.has("antidote.crafted") &&
                  bark.find("MISSING") != std::string::npos;
        // Grab the real pickups through the real proximity path: stand on them.
        const uint32_t pharm = tower.roomByName("Pharmacy");
        const uint32_t quar  = tower.roomByName("Quarantine Zone");
        for (const CanonItem& it : play.upperItems()) {
            const bool want = (it.room == pharm) ||
                              (it.room == quar && it.kind == CanonItemKind::LoreTerminal);
            if (want) play.tick(0.016f, scene, *physics, it.pos, nullptr, AttackFxFn{});
        }
        dm.tick(0.016f, roomEye(tower, pharm), &sink);   // poll -> flags
        ok = ok && flags.has("f2.pharmacy_components") && flags.has("f2.quarantine_research");
        // The bench now crafts.
        ok = ok && dm.onUse(dm.points().find("pharmacy_bench")->pos, &bark) &&
             flags.has("antidote.crafted") && dm.items().has(DescMechanics::kItemAntidote);
        // A Verthani hit on F2 infects (chance forced to 1); a trooper hit does not.
        dm.setInfectChance(1.0f);
        GameCue hit; hit.kind = CueKind::MeleeImpact;
        hit.pos = roomEye(tower, quar);
        hit.species = (uint32_t)EnemyType::DominionTrooper;
        dm.onCue(hit);
        ok = ok && !dm.status().infected();               // wrong species -> clean
        hit.species = (uint32_t)EnemyType::Verthani;
        dm.onCue(hit);
        ok = ok && dm.status().infected();
        // Infection ticks; the antidote (use-key path) cures + consumes.
        for (int i = 0; i < 70; ++i) dm.tick(0.1f, roomEye(tower, pharm), &sink);
        ok = ok && sink.total >= 2;
        ok = ok && dm.onUseItem(roomEye(tower, pharm), &bark) &&
             bark.find("ANTIDOTE ADMINISTERED") != std::string::npos &&
             !dm.status().infected() && !dm.items().has(DescMechanics::kItemAntidote);
        dcheck(ok, "D10 infection (F2/F3 creature hits) + gated antidote craft + cure");
    }

    // ---- D11: DECONTAMINATION also cures (Tier-B #8). + F1 hits never infect. --
    {
        SinkStub sink;
        // An F1 Verthani hit must NOT infect (floor gate).
        const uint32_t hall = tower.roomByName("Main Hall");
        GameCue hit; hit.kind = CueKind::MeleeImpact;
        hit.species = (uint32_t)EnemyType::Verthani;
        hit.pos = roomEye(tower, hall);
        dm.onCue(hit);
        bool ok = !dm.status().infected();
        // Infect directly, walk into Decontamination -> cured.
        dm.status().infect();
        const uint32_t decon = tower.roomByName("Decontamination");
        ok = ok && decon != kNoRoom;
        dm.tick(0.1f, roomEye(tower, decon), &sink);
        ok = ok && !dm.status().infected();
        bool sawDecon = false;
        for (std::string b = dm.takeBark(); !b.empty(); b = dm.takeBark())
            if (b.find("DECONTAMINATION") != std::string::npos) sawDecon = true;
        ok = ok && sawDecon;
        dcheck(ok, "D11 decontamination cures infection; F1 creature hits never infect");
    }

    // ---- D12: the HUD status tag reflects held items / active statuses. ----
    {
        bool ok = dm.hudStatusLine().empty();     // everything used/cured by now
        dm.items().add(DescMechanics::kItemEmp);
        dm.status().infect();
        const std::string s = dm.hudStatusLine();
        ok = ok && s.find("EMP READY") != std::string::npos &&
             s.find("INFECTED") != std::string::npos;
        dm.items().consume(DescMechanics::kItemEmp);
        dm.status().cureInfection();
        ok = ok && dm.hudStatusLine().empty();
        dcheck(ok, "D12 HUD status tag (EMP READY / INFECTED) tracks state");
    }

    play.shutdown();
    physics->shutdown();
    x3::logInfo("--test-descmech: " + std::to_string(g_dpass) + " passed, " +
                std::to_string(g_dfail) + " failed");
    return g_dfail == 0;
}

} // namespace x3::game
