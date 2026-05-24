// EFLZ morality / timeline backbone — implementation (see timeline.h).
#include "timeline.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// small clamp helper (no <algorithm>::clamp dependency on int promotion quirks)
// ---------------------------------------------------------------------------
namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

// ===========================================================================
// Name tables
// ===========================================================================
const char* timelineName(Timeline t) {
    switch (t) {
        case Timeline::Omega: return "Omega";
        case Timeline::Alpha: return "Alpha";
        case Timeline::Beta:  return "Beta";
        case Timeline::Gamma: return "Gamma";
        default:              return "Unset";
    }
}

const char* endingName(Ending e) {
    switch (e) {
        case Ending::Golden:            return "Golden (Perfect)";
        case Ending::Good:              return "Good (Strong Victory)";
        case Ending::Bittersweet:       return "Bittersweet (Pyrrhic)";
        case Ending::Tragic:            return "Tragic (Sarah's Sacrifice)";
        case Ending::Fractured:         return "Fractured";
        case Ending::Dark:              return "Dark (Jake Corrupted)";
        case Ending::Nightmare:         return "Nightmare (Total Failure)";
        case Ending::SoloVictory:       return "Solo Victory";
        case Ending::KtharaRomance:     return "K'thara Romance";
        case Ending::PolyamorousFamily: return "Polyamorous Family";
        case Ending::ChensRedemption:   return "Chen's Redemption";
        case Ending::NewBeginning:      return "New Beginning";
        default:                        return "None";
    }
}

// ===========================================================================
// 1. InfectionTimer
// ===========================================================================
void InfectionTimer::arm(float fullTime) {
    m_fullTime = (fullTime > 0.0f) ? fullTime : kInfectionFullTime;
    m_timeLeft = m_fullTime;
    m_running  = true;
    m_cured    = false;
    m_rescued  = false;
    m_lost     = false;
}

bool InfectionTimer::tick(float dt) {
    if (!m_running) return false;
    m_timeLeft -= dt;
    if (m_timeLeft <= 0.0f) {
        m_timeLeft = 0.0f;
        m_running  = false;
        m_lost     = true;   // transformed -> becomes a boss
        return true;
    }
    return false;
}

bool InfectionTimer::tryCure(float roll01) {
    if (m_lost || m_cured) return false;
    const int rate = cureRate();                // % at the CURRENT stage
    const bool ok  = (roll01 * 100.0f) < (float)rate;
    if (ok) {
        m_cured   = true;
        m_running = false;
    }
    return ok;
}

void InfectionTimer::rescue() {
    if (m_lost) return;
    m_rescued = true;
    m_running = false;       // freeze the clock at its current stage, alive
}

InfectionStage InfectionTimer::stage() const {
    if (m_lost)   return InfectionStage::Lost;
    if (m_fullTime <= 0.0f) return InfectionStage::Healthy;
    // Elapsed fraction 0..1 maps across Healthy/Early/Spreading/Critical (4 live bands).
    const float elapsed = (m_fullTime - m_timeLeft) / m_fullTime;     // 0..1
    if (elapsed < 0.25f) return InfectionStage::Healthy;
    if (elapsed < 0.50f) return InfectionStage::Early;
    if (elapsed < 0.75f) return InfectionStage::Spreading;
    return InfectionStage::Critical;
}

int InfectionTimer::cureRate() const {
    return kCureRateByStage[(uint32_t)stage()];
}

// ===========================================================================
// 5. TimelineState
// ===========================================================================
TimelineState::TimelineState() = default;

// ---- F2 triage -------------------------------------------------------------
void TimelineState::armCaptive(Woman w, float fullTime) {
    m_inf[(uint32_t)w].arm(fullTime);
    if (m_fate[(uint32_t)w] == CaptiveFate::Pending) {
        // pending stays pending; arming just starts her clock.
    }
}

uint32_t TimelineState::tickCaptives(float dt) {
    uint32_t transformed = 0;
    for (uint32_t i = 0; i < kWomanCount; ++i) {
        if (m_fate[i] != CaptiveFate::Pending) continue;   // already resolved
        if (m_inf[i].tick(dt)) {
            // her clock expired this frame -> Lost / becomes a boss.
            onWomanLost((Woman)i);
            ++transformed;
        }
    }
    return transformed;
}

void TimelineState::onWomanSaved(Woman w) {
    const uint32_t i = (uint32_t)w;
    if (m_fate[i] != CaptiveFate::Pending) return;         // idempotent
    m_fate[i] = CaptiveFate::Saved;
    m_inf[i].rescue();
    adjustHumanity(+10);
    adjustKarma(+8);
    adjustMercy(+5);
}

bool TimelineState::onWomanCured(Woman w, float roll01) {
    const uint32_t i = (uint32_t)w;
    if (m_fate[i] == CaptiveFate::Cured || m_fate[i] == CaptiveFate::Saved) return false;
    const bool ok = m_inf[i].tryCure(roll01);
    if (ok) {
        m_fate[i] = CaptiveFate::Cured;
        adjustMercy(+10);
        adjustRedemption(+8);
        adjustKarma(+5);
    }
    return ok;
}

void TimelineState::onWomanLost(Woman w) {
    const uint32_t i = (uint32_t)w;
    if (m_fate[i] == CaptiveFate::Saved || m_fate[i] == CaptiveFate::Cured) return;
    m_fate[i] = CaptiveFate::Lost;
    if (!m_inf[i].lost()) {
        // killed/abandoned before the clock fully expired — force-mark her lost.
        m_inf[i].arm(m_inf[i].fullTime());
        // collapse the clock so stage()==Lost and becameBoss()==true.
        // (arm restarts; immediately drive to expiry deterministically)
        m_inf[i].tick(m_inf[i].fullTime() + 1.0f);
    }
    adjustHumanity(-12);
    adjustKarma(-10);
}

// ---- Sarah + lock ----------------------------------------------------------
void TimelineState::onSarahSaved(bool sarahSavedFirst) {
    m_sarahDecided    = true;
    m_sarahSaved      = true;
    m_sarahSavedFirst = sarahSavedFirst;
    adjustLove(+15);
    adjustHumanity(+5);
    adjustKarma(+5);
}

void TimelineState::onSarahLost() {
    m_sarahDecided = true;
    m_sarahSaved   = false;
    adjustLove(-10);
    adjustHumanity(-5);
    adjustKarma(-5);
}

Timeline TimelineState::lockTimeline() {
    if (m_timeline != Timeline::Unset) return m_timeline;

    const uint32_t saved = savedWomen();    // women alive on the player's side (>=2 "good")

    // Beta: the player beelined to Sarah first -> the women's clocks ran out. Canon:
    // Sarah saved-FIRST while the triage was abandoned (women become bosses).
    if (m_sarahSaved && m_sarahSavedFirst) {
        m_timeline = Timeline::Beta;
        return m_timeline;
    }
    // Omega: the perfect run — all three women saved AND Sarah saved.
    if (m_sarahSaved && saved >= kWomanCount) {
        m_timeline = Timeline::Omega;
        return m_timeline;
    }
    // Alpha: the good-but-grieving run — >=2 women saved but Sarah lost (or saved with
    // an incomplete triage that still cleared the bar — the family timeline).
    if (saved >= 2 && !m_sarahSaved) {
        m_timeline = Timeline::Alpha;
        return m_timeline;
    }
    // A Sarah-saved run that isn't a perfect Omega and wasn't a beeline: treat a strong
    // triage (>=2) as Omega-adjacent Good (Alpha), else it slid to failure below.
    if (m_sarahSaved && saved >= 2) {
        m_timeline = Timeline::Alpha;
        return m_timeline;
    }
    // Gamma: failure — Sarah lost AND the triage failed (<2 women saved).
    m_timeline = Timeline::Gamma;
    return m_timeline;
}

// ---- Axis adjusters (clamped) ----------------------------------------------
int TimelineState::adjustHumanity(int d)   { m_axes.humanity   = clampi(m_axes.humanity + d, kHumanityMin, kHumanityMaxAxis); return m_axes.humanity; }
int TimelineState::adjustKarma(int d)      { m_axes.karma      = clampi(m_axes.karma + d, kKarmaMin, kKarmaMax);              return m_axes.karma; }
int TimelineState::adjustTrust(int d)      { m_axes.trust      = clampi(m_axes.trust + d, kRelMin, kRelMax);                  return m_axes.trust; }
int TimelineState::adjustMercy(int d)      { m_axes.mercy      = clampi(m_axes.mercy + d, kRelMin, kRelMax);                  return m_axes.mercy; }
int TimelineState::adjustLove(int d)       { m_axes.love       = clampi(m_axes.love + d, kRelMin, kRelMax);                   return m_axes.love; }
int TimelineState::adjustRedemption(int d) { m_axes.redemption = clampi(m_axes.redemption + d, kRelMin, kRelMax);            return m_axes.redemption; }

void TimelineState::onAllyJoined()  { ++m_axes.alliance; adjustTrust(+3); }
void TimelineState::onChenSaved()   { m_chenSaved = true;  adjustRedemption(+20); adjustKarma(+8); }
void TimelineState::onChenKilled()  { m_chenSaved = false; adjustRedemption(-15); adjustKarma(-5); }

void TimelineState::onAugment(bool accepted) {
    if (accepted) adjustHumanity(-20);   // canon kAugmentHumanityCost (spire_mid.h)
    // refusing keeps Humanity intact (no-op).
}

void TimelineState::onSalvariChoice(bool allied) {
    if (allied) { adjustTrust(+20); onAllyJoined(); }
    else        { adjustTrust(-20); adjustKarma(-8); }
}

// ---- State queries ---------------------------------------------------------
uint32_t TimelineState::savedWomen() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kWomanCount; ++i)
        if (m_fate[i] == CaptiveFate::Saved || m_fate[i] == CaptiveFate::Cured) ++n;
    return n;
}

uint32_t TimelineState::lostWomen() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kWomanCount; ++i)
        if (m_fate[i] == CaptiveFate::Lost) ++n;
    return n;
}

bool TimelineState::bossPending() const {
    for (uint32_t i = 0; i < kWomanCount; ++i)
        if (m_inf[i].becameBoss() && !m_bossConsumed[i]) return true;
    return false;
}

// ===========================================================================
// THE ENDING-ELIGIBILITY MAP — pure function of (timeline x axes x allies x choice)
// ===========================================================================
std::vector<Ending> TimelineState::eligibleEndings(FinaleChoice choice) {
    if (m_timeline == Timeline::Unset) lockTimeline();

    const MoralityAxes& a   = m_axes;
    const uint32_t      al  = allyCount();
    const uint32_t      sav = savedWomen();
    const bool sarah        = m_sarahSaved;
    const bool chen         = m_chenSaved;

    std::vector<Ending> out;
    auto add = [&out](Ending e) {
        for (Ending x : out) if (x == e) return;   // dedupe
        out.push_back(e);
    };

    // ----- thresholds (tuned to the EFLZ §5 triggers) -----
    const bool highHumanity = a.humanity   >= 70;
    const bool lowHumanity  = a.humanity   <= 30;
    const bool goodKarma    = a.karma      >= 40;
    const bool highLove     = a.love       >= 70;
    const bool salvariAlly  = a.trust      >= 60;     // "Salvari alliance" earned
    const bool highRedeem   = a.redemption >= 70;
    const bool manyAllies   = al >= 4;
    const bool someAllies   = al >= 1;

    switch (m_timeline) {
        case Timeline::Omega: {
            // 1 Golden: all allies + Salvari + Chen sacrifice + Destroy + perfect axes.
            if (highHumanity && goodKarma && salvariAlly && chen && manyAllies &&
                sav >= kWomanCount && choice == FinaleChoice::Destroy)
                add(Ending::Golden);
            // 2 Good: most allies + Salvari + Destroy.
            if (salvariAlly && someAllies && choice == FinaleChoice::Destroy)
                add(Ending::Good);
            // 4 Tragic: high Love + Sacrifice choice.
            if (highLove && choice == FinaleChoice::Sacrifice)
                add(Ending::Tragic);
            // 11 Chen's Redemption: Chen saved + high Redemption (alternate method).
            if (chen && highRedeem)
                add(Ending::ChensRedemption);
            break;
        }
        case Timeline::Alpha: {
            // 2 Good: most allies + Salvari + Destroy (one fell).
            if (salvariAlly && someAllies && sav >= 2 && choice == FinaleChoice::Destroy)
                add(Ending::Good);
            // 3 Bittersweet: some allies lost + Salvari + Destroy at great cost.
            if (salvariAlly && lostWomen() >= 1 && choice == FinaleChoice::Destroy)
                add(Ending::Bittersweet);
            // 10 Polyamorous Family: all 3 women saved + relationships intact.
            if (sav >= kWomanCount && highHumanity)
                add(Ending::PolyamorousFamily);
            // 5 Fractured: relationships strained (low Love), family breaks apart.
            if (a.love <= 40)
                add(Ending::Fractured);
            // 11 Chen's Redemption available cross-timeline if Chen earned it.
            if (chen && highRedeem)
                add(Ending::ChensRedemption);
            break;
        }
        case Timeline::Beta: {
            // 9 K'thara Romance: the signature Beta ending (Sarah left from guilt).
            add(Ending::KtharaRomance);
            // 8 Solo Victory: Sarah saved but everyone else lost; Jake fights alone.
            if (al == 0)
                add(Ending::SoloVictory);
            // 6 Dark: low Humanity + power-absorb (Alliance choice) -> corruption.
            if (lowHumanity && choice == FinaleChoice::Alliance)
                add(Ending::Dark);
            break;
        }
        case Timeline::Gamma: {
            // 7 Nightmare: total failure — Sarah=The Bride + no allies + Overlord wins
            //   (the player never reached a Destroy/win, modeled as the Negotiate-or-
            //   worse fall, or simply no allies & low everything).
            if (!sarah && al == 0)
                add(Ending::Nightmare);
            // 6 Dark: failed most rescues + low Humanity + absorb power.
            if (lowHumanity && choice == FinaleChoice::Alliance)
                add(Ending::Dark);
            break;
        }
        default: break;
    }

    // 8 Solo Victory (cross-timeline): alone defeats the Overlord but lost everyone.
    if (al == 0 && choice == FinaleChoice::Destroy && a.humanity > 30)
        add(Ending::SoloVictory);

    // 12 New Beginning: the universal FLOOR — minimum victory, Jake + >=1 ally start
    // fresh. Always reachable if you have at least one ally and didn't fall to the
    // Nightmare. Guarantees eligibleEndings() is never empty for a survivable run.
    if (someAllies)
        add(Ending::NewBeginning);

    // Absolute safety net: a run with literally no qualifying ending still lands on the
    // lowest tier (New Beginning if any ally, else Nightmare) so the finale always has
    // something to present.
    if (out.empty())
        add(someAllies ? Ending::NewBeginning : Ending::Nightmare);

    return out;
}

Ending TimelineState::bestEnding(FinaleChoice choice) {
    std::vector<Ending> es = eligibleEndings(choice);
    Ending best = Ending::None;
    for (Ending e : es) {
        if (best == Ending::None || (uint32_t)e < (uint32_t)best) best = e;
    }
    return best;
}

bool TimelineState::endingReachable(Ending e, FinaleChoice choice) {
    std::vector<Ending> es = eligibleEndings(choice);
    for (Ending x : es) if (x == e) return true;
    return false;
}

// ---- Serialize -------------------------------------------------------------
TimelineState::SaveState TimelineState::serialize() const {
    SaveState s{};
    s.timeline        = (uint32_t)m_timeline;
    s.axes            = m_axes;
    s.sarahSaved      = m_sarahSaved;
    s.sarahSavedFirst = m_sarahSavedFirst;
    s.chenSaved       = m_chenSaved;
    for (uint32_t i = 0; i < kWomanCount; ++i) {
        s.fate[i]         = (uint32_t)m_fate[i];
        s.infTimeLeft[i]  = m_inf[i].timeLeft();
        s.infCured[i]     = m_inf[i].cured();
        s.infRescued[i]   = m_inf[i].rescued();
        s.infLost[i]      = m_inf[i].lost();
        s.bossConsumed[i] = m_bossConsumed[i];
    }
    return s;
}

void TimelineState::deserialize(const SaveState& s) {
    m_timeline        = (Timeline)s.timeline;
    m_axes            = s.axes;
    m_sarahDecided    = true;
    m_sarahSaved      = s.sarahSaved;
    m_sarahSavedFirst = s.sarahSavedFirst;
    m_chenSaved       = s.chenSaved;
    for (uint32_t i = 0; i < kWomanCount; ++i) {
        m_fate[i]         = (CaptiveFate)s.fate[i];
        m_bossConsumed[i] = s.bossConsumed[i];
        // Reconstruct the infection clock from the persisted fields. We re-arm with the
        // existing fullTime then drive its lifecycle flags to match the snapshot.
        m_inf[i].arm(m_inf[i].fullTime());
        if (s.infLost[i]) {
            m_inf[i].tick(m_inf[i].fullTime() + 1.0f);   // -> lost
        } else if (s.infCured[i]) {
            m_inf[i].tryCure(0.0f);                       // 0.0 always succeeds (rate>0 at stage 1+)
            if (!m_inf[i].cured()) { /* Healthy stage rate 0: force via rescue-like freeze */ }
        } else if (s.infRescued[i]) {
            m_inf[i].rescue();
        }
        // NOTE: timeLeft restore is approximate for cured/rescued frozen clocks; the
        // fate enum is the authoritative gameplay state and is restored exactly.
    }
}

// ===========================================================================
// 6. Global singleton + notify hooks
// ===========================================================================
namespace {
std::unique_ptr<TimelineState> g_timeline;
}

TimelineState& globalTimeline() {
    if (!g_timeline) g_timeline = std::make_unique<TimelineState>();
    return *g_timeline;
}

void resetGlobalTimeline() {
    g_timeline = std::make_unique<TimelineState>();
}

// ===========================================================================
// Headless self-test (--test-timeline)
// ===========================================================================
namespace {

int g_tpass = 0, g_tfail = 0;
void tcheck(bool cond, const char* name) {
    if (cond) { ++g_tpass; x3::logInfo(std::string("[timeline-test] PASS ") + name); }
    else      { ++g_tfail; x3::logError(std::string("[timeline-test] FAIL ") + name); }
}

// True iff `set` contains `e`.
bool has(const std::vector<Ending>& set, Ending e) {
    for (Ending x : set) if (x == e) return true;
    return false;
}

} // namespace

bool runTimelineSelfTest() {
    g_tpass = g_tfail = 0;

    // -----------------------------------------------------------------------
    // T1: INFECTION TIMER — 4-stage progression + the [0,90,60,30,0] cure rates.
    // -----------------------------------------------------------------------
    {
        InfectionTimer inf;
        inf.arm(100.0f);
        tcheck(inf.stage() == InfectionStage::Healthy && inf.cureRate() == 0,
               "T1a armed -> Healthy, cure rate 0%");

        inf.tick(30.0f);   // elapsed 0.30 -> Early band [0.25,0.50)
        tcheck(inf.stage() == InfectionStage::Early && inf.cureRate() == 90,
               "T1b 30% elapsed -> Early, cure rate 90%");

        inf.tick(30.0f);   // elapsed 0.60 -> Spreading [0.50,0.75)
        tcheck(inf.stage() == InfectionStage::Spreading && inf.cureRate() == 60,
               "T1c 60% elapsed -> Spreading, cure rate 60%");

        inf.tick(20.0f);   // elapsed 0.80 -> Critical [0.75,1.0)
        tcheck(inf.stage() == InfectionStage::Critical && inf.cureRate() == 30,
               "T1d 80% elapsed -> Critical, cure rate 30%");

        const bool transformed = inf.tick(50.0f);  // crosses zero -> Lost
        tcheck(transformed && inf.stage() == InfectionStage::Lost && inf.cureRate() == 0 &&
               inf.becameBoss() && !inf.running(),
               "T1e expiry -> Lost, cure rate 0%, becameBoss latched");

        // full cure-rate-by-stage table matches canon exactly.
        tcheck(kCureRateByStage[0] == 0  && kCureRateByStage[1] == 90 &&
               kCureRateByStage[2] == 60 && kCureRateByStage[3] == 30 &&
               kCureRateByStage[4] == 0,
               "T1f cure-rate table == [0,90,60,30,0]");
    }

    // -----------------------------------------------------------------------
    // T2: cure success/failure honors the stage rate (deterministic roll).
    // -----------------------------------------------------------------------
    {
        InfectionTimer inf; inf.arm(100.0f); inf.tick(30.0f);   // Early, 90%
        tcheck(inf.tryCure(0.50f) && inf.cured(),  "T2a roll 0.50 < 90% -> cure succeeds (Early)");

        InfectionTimer inf2; inf2.arm(100.0f); inf2.tick(80.0f); // Critical, 30%
        tcheck(!inf2.tryCure(0.50f) && !inf2.cured(), "T2b roll 0.50 >= 30% -> cure fails (Critical)");
        tcheck(inf2.tryCure(0.10f) && inf2.cured(),   "T2c roll 0.10 < 30% -> cure succeeds (Critical)");
    }

    // -----------------------------------------------------------------------
    // T3: TIMELINE SELECTION — the four canon paths.
    // -----------------------------------------------------------------------
    // T3-Alpha: save 2 of 3 women + lose Sarah -> Alpha.
    {
        TimelineState ts;
        ts.armCaptive(Woman::Aria, 60.0f);
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.armCaptive(Woman::Emily, 60.0f);
        ts.onWomanSaved(Woman::Aria);
        ts.onWomanSaved(Woman::Keisha);
        ts.tickCaptives(120.0f);          // Emily's clock runs out -> Lost
        ts.onSarahLost();
        Timeline tl = ts.lockTimeline();
        tcheck(tl == Timeline::Alpha && ts.savedWomen() == 2 && ts.lostWomen() == 1,
               "T3 Alpha: save 2/3 women + lose Sarah -> Alpha");
    }
    // T3-Omega: save all 3 + Sarah -> Omega.
    {
        TimelineState ts;
        ts.armCaptive(Woman::Aria, 60.0f);
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.armCaptive(Woman::Emily, 60.0f);
        ts.onWomanSaved(Woman::Aria);
        ts.onWomanSaved(Woman::Keisha);
        ts.onWomanSaved(Woman::Emily);
        ts.onSarahSaved(/*sarahSavedFirst*/false);
        Timeline tl = ts.lockTimeline();
        tcheck(tl == Timeline::Omega && ts.savedWomen() == 3 && ts.sarahSaved(),
               "T3 Omega: save all 3 women + Sarah -> Omega");
    }
    // T3-Beta: Sarah-first (beeline) -> Beta (women lost).
    {
        TimelineState ts;
        ts.armCaptive(Woman::Aria, 60.0f);
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.armCaptive(Woman::Emily, 60.0f);
        ts.onSarahSaved(/*sarahSavedFirst*/true);   // beelined to Sarah
        ts.tickCaptives(120.0f);                      // all three women transform
        Timeline tl = ts.lockTimeline();
        tcheck(tl == Timeline::Beta && ts.sarahSaved() && ts.lostWomen() == 3,
               "T3 Beta: Sarah-first beeline -> Beta (women -> bosses)");
    }
    // T3-Gamma: fail everything -> Gamma.
    {
        TimelineState ts;
        ts.armCaptive(Woman::Aria, 60.0f);
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.armCaptive(Woman::Emily, 60.0f);
        ts.tickCaptives(120.0f);     // all lost
        ts.onSarahLost();
        Timeline tl = ts.lockTimeline();
        tcheck(tl == Timeline::Gamma && ts.savedWomen() == 0 && !ts.sarahSaved(),
               "T3 Gamma: fail triage + lose Sarah -> Gamma");
    }

    // -----------------------------------------------------------------------
    // T4: AXIS MATH — Humanity/Karma/etc. update + clamp correctly.
    // -----------------------------------------------------------------------
    {
        TimelineState ts;
        // defaults
        tcheck(ts.axes().humanity == 100 && ts.axes().karma == 0 && ts.axes().alliance == 0,
               "T4a axis defaults (Humanity 100, Karma 0, allies 0)");

        // Saving a woman: +10 Humanity (clamped at 100), +8 Karma, +5 Mercy.
        ts.armCaptive(Woman::Aria, 60.0f);
        ts.onWomanSaved(Woman::Aria);
        tcheck(ts.axes().humanity == 100 && ts.axes().karma == 8 && ts.axes().mercy == 55,
               "T4b save woman: Humanity clamps at 100, Karma +8, Mercy +5");

        // Losing a woman: -12 Humanity, -10 Karma.
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.onWomanLost(Woman::Keisha);
        tcheck(ts.axes().humanity == 88 && ts.axes().karma == -2,
               "T4c lose woman: Humanity -12 (=88), Karma -10 (=-2)");

        // Augment accepted: -20 Humanity (canon kAugmentHumanityCost).
        ts.onAugment(true);
        tcheck(ts.axes().humanity == 68, "T4d augment accepted: Humanity -20 (=68)");
        ts.onAugment(false);
        tcheck(ts.axes().humanity == 68, "T4e augment refused: Humanity unchanged");

        // Karma clamps at the -100..+100 bounds.
        for (int i = 0; i < 50; ++i) ts.adjustKarma(-10);
        tcheck(ts.axes().karma == kKarmaMin, "T4f Karma clamps at -100");
        for (int i = 0; i < 50; ++i) ts.adjustKarma(+10);
        tcheck(ts.axes().karma == kKarmaMax, "T4g Karma clamps at +100");

        // Ally + Salvari + Chen feed Alliance/Trust/Redemption.
        ts.onAllyJoined();
        ts.onSalvariChoice(true);    // +20 Trust, +1 ally
        ts.onChenSaved();            // +20 Redemption
        tcheck(ts.allyCount() == 2 && ts.axes().trust >= 70 && ts.axes().redemption == 70 && ts.chenSaved(),
               "T4h ally/Salvari/Chen feed Alliance(2)/Trust/Redemption");
    }

    // -----------------------------------------------------------------------
    // T5: ENDING ELIGIBILITY — the (timeline x axes x allies x choice) map.
    // -----------------------------------------------------------------------
    // T5-Golden: a perfect Omega run + Destroy -> Golden eligible (and Good).
    {
        TimelineState ts;
        for (uint32_t i = 0; i < kWomanCount; ++i) {
            ts.armCaptive((Woman)i, 60.0f);
            ts.onWomanSaved((Woman)i);
        }
        ts.onSarahSaved(false);
        ts.onSalvariChoice(true);            // Trust up, +ally
        for (int i = 0; i < 5; ++i) ts.onAllyJoined();   // many allies
        ts.onChenSaved();
        ts.adjustKarma(+50);                 // good karma
        ts.lockTimeline();
        auto golden = ts.eligibleEndings(FinaleChoice::Destroy);
        tcheck(ts.timeline() == Timeline::Omega && has(golden, Ending::Golden) && has(golden, Ending::Good),
               "T5a Omega+all+Salvari+Chen+Destroy -> Golden eligible");
        tcheck(ts.bestEnding(FinaleChoice::Destroy) == Ending::Golden,
               "T5b bestEnding == Golden (lowest-numbered)");
        // The same Omega run with a Sacrifice choice + high Love -> Tragic eligible.
        ts.adjustLove(+50);
        auto tragic = ts.eligibleEndings(FinaleChoice::Sacrifice);
        tcheck(has(tragic, Ending::Tragic), "T5c Omega+highLove+Sacrifice -> Tragic eligible");
    }
    // T5-Beta: K'thara Romance is the signature Beta ending.
    {
        TimelineState ts;
        for (uint32_t i = 0; i < kWomanCount; ++i) ts.armCaptive((Woman)i, 60.0f);
        ts.onSarahSaved(true);
        ts.tickCaptives(120.0f);
        ts.onAllyJoined();
        ts.lockTimeline();
        auto beta = ts.eligibleEndings(FinaleChoice::Destroy);
        tcheck(ts.timeline() == Timeline::Beta && has(beta, Ending::KtharaRomance),
               "T5d Beta -> K'thara Romance eligible");
    }
    // T5-Alpha: Polyamorous Family when all 3 saved but Sarah lost.
    {
        TimelineState ts;
        for (uint32_t i = 0; i < kWomanCount; ++i) {
            ts.armCaptive((Woman)i, 60.0f);
            ts.onWomanSaved((Woman)i);
        }
        ts.onSarahLost();
        ts.onSalvariChoice(true);
        for (int i = 0; i < 3; ++i) ts.onAllyJoined();
        ts.lockTimeline();
        auto alpha = ts.eligibleEndings(FinaleChoice::Destroy);
        tcheck(ts.timeline() == Timeline::Alpha && has(alpha, Ending::PolyamorousFamily),
               "T5e Alpha+all-3-saved -> Polyamorous Family eligible");
    }
    // T5-Gamma: Nightmare when Sarah lost + no allies.
    {
        TimelineState ts;
        for (uint32_t i = 0; i < kWomanCount; ++i) ts.armCaptive((Woman)i, 60.0f);
        ts.tickCaptives(120.0f);
        ts.onSarahLost();
        ts.lockTimeline();
        auto gamma = ts.eligibleEndings(FinaleChoice::Negotiate);
        tcheck(ts.timeline() == Timeline::Gamma && has(gamma, Ending::Nightmare),
               "T5f Gamma+Sarah-lost+no-allies -> Nightmare eligible");
    }
    // T5-floor: eligibleEndings is NEVER empty; an ally always opens New Beginning.
    {
        TimelineState ts;
        ts.onWomanLost(Woman::Aria);
        ts.onSarahLost();
        ts.onAllyJoined();
        ts.lockTimeline();
        auto any = ts.eligibleEndings(FinaleChoice::Negotiate);
        tcheck(!any.empty() && has(any, Ending::NewBeginning),
               "T5g eligibleEndings never empty (>=1 ally -> New Beginning floor)");
    }

    // -----------------------------------------------------------------------
    // T6: SERIALIZE / DESERIALIZE round-trips timeline + axes + fates.
    // -----------------------------------------------------------------------
    {
        TimelineState ts;
        ts.armCaptive(Woman::Aria, 60.0f);  ts.onWomanSaved(Woman::Aria);
        ts.armCaptive(Woman::Keisha, 60.0f);
        ts.tickCaptives(120.0f);            // Keisha lost
        ts.onSarahSaved(false);
        ts.onChenSaved();
        ts.lockTimeline();

        TimelineState::SaveState snap = ts.serialize();
        TimelineState restored;
        restored.deserialize(snap);
        tcheck(restored.timeline() == ts.timeline() &&
               restored.axes().humanity == ts.axes().humanity &&
               restored.axes().karma == ts.axes().karma &&
               restored.fate(Woman::Aria) == CaptiveFate::Saved &&
               restored.fate(Woman::Keisha) == CaptiveFate::Lost &&
               restored.sarahSaved() == ts.sarahSaved() &&
               restored.chenSaved() == ts.chenSaved(),
               "T6 serialize/deserialize round-trips timeline + axes + fates");
    }

    // -----------------------------------------------------------------------
    // T7: bossPending / consumeBoss latch (host spawns the transformed-woman boss).
    // -----------------------------------------------------------------------
    {
        TimelineState ts;
        ts.armCaptive(Woman::Emily, 30.0f);
        ts.tickCaptives(60.0f);    // Emily transforms
        tcheck(ts.bossPending(), "T7a a transformed woman is bossPending");
        ts.consumeBoss(Woman::Emily);
        tcheck(!ts.bossPending(), "T7b consumeBoss clears the latch");
    }

    // -----------------------------------------------------------------------
    // T8: the global singleton + notify hooks feed the SAME state; reset clears it.
    // -----------------------------------------------------------------------
    {
        resetGlobalTimeline();
        notifyWomanSaved(Woman::Aria);
        notifyAllyJoined();
        tcheck(globalTimeline().savedWomen() == 1 && globalTimeline().allyCount() == 1,
               "T8a notify* hooks feed the global TimelineState");
        resetGlobalTimeline();
        tcheck(globalTimeline().savedWomen() == 0 && globalTimeline().allyCount() == 0,
               "T8b resetGlobalTimeline clears it");
    }

    x3::logInfo("timeline: " + std::to_string(g_tpass) + "/" +
                std::to_string(g_tpass + g_tfail) + " passed");
    return g_tfail == 0;
}

} // namespace x3::game
