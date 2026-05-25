#pragma once
// EFLZ MORALITY / TIMELINE BACKBONE (X3_WORLD_BLUEPRINT §3 "Infection timer ->
// branching" + §4.5 "the 12 endings", docs/design/EFLZ_NARRATIVE.md §4-5).
// Game/slice code only — engine/ stays pure; this module owns NO renderer/physics
// state and pulls in NO heavy headers, so it is trivially unit-testable headless.
//
// PURPOSE. The 50/100-level campaign branches on a small, well-defined moral state:
//   1. a per-captive INFECTION/TRANSFORMATION timer (the "unsaved becomes a boss"
//      motif) with a 4-stage progression and a stage-dependent CURE rate;
//   2. a four-way TIMELINE (Omega / Alpha / Beta / Gamma) locked at Floor 7 from who
//      was saved on F2 (Aria/Keisha/Emily) + whether (and WHEN) Sarah was saved;
//   3. MORALITY AXES — Humanity 0..100, Karma -100..+100, plus Trust / Mercy / Love /
//      Redemption, and Alliance points (recruited allies);
//   4. ENDING ELIGIBILITY — a pure map from (timeline x axes x ally count x a finale
//      P4 choice) to which of the 12 endings is reachable, queried by the finale.
//
// DESIGN STANCE — INGEST + EXPOSE, never reach in. This module does NOT restructure
// or even depend on rescue.* / level1_game.* / spire_top.*. It is fed EVENTS (a
// captive was rescued / killed / cured, a moral choice was made, Sarah's fate at F7,
// an ally joined) and it EXPOSES state + queries. Existing systems can OPTIONALLY
// push one-line notifications through the free `notify*` hooks at the bottom (a thin,
// non-invasive bridge to a process-wide singleton) — but the type works standalone
// and the self-test drives it directly with deterministic event sequences.
//
// Mirrors the EFLZ canon tuning verbatim:
//   * cure rate by stage = [0, 90, 60, 30, 0] % (blueprint §3 x3-infection.js).
//   * Humanity 0..100 (the F4 meter in spire_mid.h: kHumanityMax=100); Karma -100..+100.
//   * 4 stages: Healthy(0) -> Early(1) -> Spreading(2) -> Critical(3) -> Lost/expired.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// ===========================================================================
// 1. INFECTION / TRANSFORMATION TIMER (per captive)
// ===========================================================================

// The four alien-DNA infection stages a captive passes through as the timer runs,
// plus the terminal Lost state once the timer expires (the captive "becomes a boss").
// The numeric value indexes kCureRateByStage.
enum class InfectionStage : uint32_t {
    Healthy   = 0,  // freshly captured; full cure chance window not yet open (rate 0)
    Early     = 1,  // infection takes hold; BEST cure window (rate 90%)
    Spreading = 2,  // DNA rewrite advancing (rate 60%)
    Critical  = 3,  // near-total conversion; last chance (rate 30%)
    Lost      = 4,  // timer expired -> transformed; uncurable, becomes a boss (rate 0%)
};
constexpr uint32_t kInfectionStageCount = 5;

// Cure-success rate (percent, 0..100) by stage — EFLZ canon [0,90,60,30,0]
// (blueprint §3). Index with (uint32_t)InfectionStage.
inline constexpr std::array<int, kInfectionStageCount> kCureRateByStage =
    { 0, 90, 60, 30, 0 };

// Default full transformation time (seconds) before a captive is Lost. The canon F2
// values disagree across docs (5/7/4 min cutscene vs 15/20/9 controller); this is a
// tunable per-captive default — the self-test overrides it for fast determinism.
constexpr float kInfectionFullTime = 300.0f;  // 5 min

// A single captive's infection clock. Self-contained: ticks down on dt, derives its
// stage from elapsed fraction, exposes the cure rate for its current stage, and
// latches a "becameBoss" flag the instant it expires (the unsaved-becomes-a-boss
// motif). Curing it freezes it Healthy; rescuing freezes wherever it is (alive).
class InfectionTimer {
public:
    // Arm the timer with a total transformation time. Starts Healthy, full time left,
    // running. (A captive only counts down once the host says the clock is live; the
    // host gates that — same pattern as RescueSystem's hub gate — by simply not
    // ticking until then.)
    void arm(float fullTime = kInfectionFullTime);

    // Advance one frame while still infecting. No-op once cured/rescued/Lost. Returns
    // true the FRAME the timer crosses zero (the captive transforms -> spawn its boss).
    bool tick(float dt);

    // Cure the captive: succeeds with the CURRENT stage's cure rate. `roll01` is a
    // deterministic [0,1) sample (the caller supplies it so the test is repeatable);
    // success freezes the timer Healthy and marks cured. Returns true on success.
    bool tryCure(float roll01);

    // Rescue (extract alive without curing): freezes the timer at its current stage,
    // alive. Idempotent; no-op if already Lost.
    void rescue();

    // ---- Queries ----------------------------------------------------------
    InfectionStage stage() const;                 // derived from elapsed fraction
    int   cureRate() const;                       // % for the current stage
    float timeLeft() const { return m_timeLeft; }
    float fullTime() const { return m_fullTime; }
    bool  running()  const { return m_running; }  // still counting down
    bool  cured()    const { return m_cured; }
    bool  rescued()  const { return m_rescued; }
    bool  lost()     const { return m_lost; }     // expired -> transformed
    // Latched the instant the timer expired (one source of truth for "spawn a boss").
    bool  becameBoss() const { return m_lost; }

private:
    float m_fullTime = kInfectionFullTime;
    float m_timeLeft = kInfectionFullTime;
    bool  m_running  = false;
    bool  m_cured    = false;
    bool  m_rescued  = false;
    bool  m_lost     = false;
};

// ===========================================================================
// 2. THE FOUR-WAY TIMELINE (locked at Floor 7)
// ===========================================================================

// The branching spine (EFLZ_NARRATIVE §4). Computed from the F2 triage + Sarah's F7
// fate + the ORDER (beelining to Sarah first is Beta). Locked once at F7.
enum class Timeline : uint32_t {
    Unset = 0,  // before the F7 lock
    Omega = 1,  // perfect: all 3 women saved AND Sarah saved
    Alpha = 2,  // good: >=2 women saved but Sarah lost (the family timeline)
    Beta  = 3,  // Sarah-first: Sarah saved but the women were left to transform
    Gamma = 4,  // failure: Sarah lost AND the triage failed (<2 women)
};
const char* timelineName(Timeline t);

// The three F2 captives whose fate feeds the timeline (index == HUD/order slot, and
// matches rescue.h VictimId values 0/1/2 so the host can pass them straight through).
enum class Woman : uint32_t { Aria = 0, Keisha = 1, Emily = 2 };
constexpr uint32_t kWomanCount = 3;

// ===========================================================================
// 3. MORALITY AXES
// ===========================================================================

// The tracked moral state (blueprint §4.5 axes + §3 karma/humanity). Humanity ties to
// the existing F4 meter (spire_mid.h kHumanityMax=100); Karma is the wide -100..+100
// axis; the four relationship axes (Trust/Mercy/Love/Redemption) are 0..100; Alliance
// is an unbounded-up ally counter. All clamped on update.
struct MoralityAxes {
    int humanity   = 100;  // 0..100  (save vs abandon; F4 augments cost it)
    int karma      = 0;    // -100..+100 (good vs evil acts)
    int trust      = 50;   // 0..100  (ally vs attack the Salvari)
    int mercy      = 50;   // 0..100  (cure vs kill the infected)
    int love       = 50;   // 0..100  (support vs abandon Sarah)
    int redemption = 50;   // 0..100  (save vs kill Dr. Chen)
    int alliance   = 0;    // recruited allies (Alliance Points), >= 0
};

// Axis bounds (clamped on every adjust).
constexpr int kHumanityMin = 0,   kHumanityMaxAxis = 100;
constexpr int kKarmaMin    = -100, kKarmaMax        = 100;
constexpr int kRelMin      = 0,   kRelMax           = 100;  // trust/mercy/love/redemption

// ===========================================================================
// 4. THE FINALE P4 CHOICE + THE 12 ENDINGS
// ===========================================================================

// The L100 phase-4 non-combat choice (blueprint §4.4/§4.5). One of these is picked at
// the finale and folds into ending eligibility.
enum class FinaleChoice : uint32_t {
    Destroy   = 0,  // annihilate the Overlord network
    Negotiate = 1,  // parley / coexistence
    Sacrifice = 2,  // someone gives their life to deliver the cure-virus
    Alliance  = 3,  // unify the conquered worlds under a new alliance
};

// The 12 endings (EFLZ_NARRATIVE §5 / blueprint §4.5), 1-indexed in the docs; here the
// enum value == ending number for an unambiguous mapping.
enum class Ending : uint32_t {
    None              = 0,
    Golden            = 1,   // Omega + all allies + Salvari + Chen sacrifice + Destroy
    Good              = 2,   // Alpha/Omega + most allies + Salvari + Destroy
    Bittersweet       = 3,   // Alpha + some allies lost + Salvari + Destroy (costly)
    Tragic            = 4,   // Omega + high Love + Sacrifice
    Fractured         = 5,   // Alpha + strained relationships
    Dark              = 6,   // failed rescues + low Humanity + Alliance/absorb power
    Nightmare         = 7,   // Gamma + Sarah=The Bride + no allies
    SoloVictory       = 8,   // alone defeats the Overlord, loses everyone
    KtharaRomance     = 9,   // Beta + K'thara together
    PolyamorousFamily = 10,  // Alpha + all 3 women saved + family intact
    ChensRedemption   = 11,  // Chen survives (high Redemption) + devotes to the cure
    NewBeginning      = 12,  // minimum victory; Jake + one ally start fresh
};
constexpr uint32_t kEndingCount = 12;
const char* endingName(Ending e);

// ===========================================================================
// 5. THE TIMELINE STATE MACHINE (ingest events, expose state + queries)
// ===========================================================================

// One captive's fate as fed in by the host (decoupled from rescue.* internals).
enum class CaptiveFate : uint32_t { Pending = 0, Saved = 1, Cured = 2, Lost = 3 };

// The morality/timeline backbone. INGESTS events; EXPOSES the timeline, the axes, the
// per-captive infection model, ally count, and the ending-eligibility query the finale
// calls. Owns nothing heavy; copyable POD-ish state so save/load can blit it.
class TimelineState {
public:
    TimelineState();

    // ---- Ingest: the F2 triage (the three women) --------------------------
    // Arm a woman's infection clock (host reached the F2 hub). `fullTime` lets the
    // host pass the canon per-room value; defaults to kInfectionFullTime.
    void armCaptive(Woman w, float fullTime = kInfectionFullTime);
    // Tick all armed captive clocks one frame; any that expire flip to Lost and latch
    // becameBoss (the host reads bossPending() to spawn the boss). Returns the count
    // that transformed THIS frame.
    uint32_t tickCaptives(float dt);
    // The host rescued a woman alive (E-in-range): mark Saved, freeze her clock,
    // +Humanity/+Karma/+Mercy. Idempotent once non-Pending.
    void onWomanSaved(Woman w);
    // The host cured a transformed/infected woman with the late-game cure. `roll01` is
    // a deterministic sample; on success she is Cured (+Mercy/+Redemption), else stays.
    // Returns true on cure success.
    bool onWomanCured(Woman w, float roll01);
    // The host killed (or the timer expired into) a transformed woman: Lost
    // (-Humanity/-Karma). Called automatically on expiry too.
    void onWomanLost(Woman w);

    // ---- Ingest: Sarah (F7) + the timeline lock ---------------------------
    // The host's F7 outcome. Call EXACTLY one of these. `sarahSavedFirst` on save:
    // true iff the player beelined to Sarah before the women's clocks ran (-> Beta).
    void onSarahSaved(bool sarahSavedFirst);
    void onSarahLost();   // Sarah transformed into The Bride (Gamma/Alpha feeder)
    // Lock the timeline from the accumulated F2 + F7 state. Called by the host at the
    // F7 climax (or implicitly by the first ending query if not yet locked). Idempotent
    // — recomputes only while Unset. Returns the locked timeline.
    Timeline lockTimeline();
    bool timelineLocked() const { return m_timeline != Timeline::Unset; }
    Timeline timeline() const { return m_timeline; }

    // ---- Ingest: generic moral choices + allies ---------------------------
    // Adjust any axis by a delta (clamped). Returned value is the new clamped value.
    int adjustHumanity(int d);
    int adjustKarma(int d);
    int adjustTrust(int d);
    int adjustMercy(int d);
    int adjustLove(int d);
    int adjustRedemption(int d);
    // An ally joined the cause (+1 Alliance point, +Trust). The host calls this when a
    // companion is secured / a faction allies.
    void onAllyJoined();
    // Chen's fate (redemption axis hinge): saved -> +Redemption, cure quality high;
    // killed -> -Redemption.
    void onChenSaved();
    void onChenKilled();
    // The F4 augmentation choice: taking an augment costs Humanity (canon 20/chair),
    // refusing keeps it. Mirrors spire_mid.h kAugmentHumanityCost.
    void onAugment(bool accepted);
    // The Salvari trust choice (ally vs attack): ally -> +Trust +Alliance; attack ->
    // -Trust -Karma.
    void onSalvariChoice(bool allied);

    // ---- State queries ----------------------------------------------------
    const MoralityAxes& axes() const { return m_axes; }
    CaptiveFate fate(Woman w) const { return m_fate[(uint32_t)w]; }
    const InfectionTimer& infection(Woman w) const { return m_inf[(uint32_t)w]; }
    uint32_t savedWomen() const;     // Saved or Cured (alive on the player's side)
    uint32_t lostWomen()  const;     // Lost (became bosses)
    uint32_t allyCount()  const { return (uint32_t)m_axes.alliance; }
    bool sarahSaved() const { return m_sarahSaved; }
    bool chenSaved()  const { return m_chenSaved; }
    // Any captive that transformed and whose boss has not yet been consumed by the host.
    bool bossPending() const;
    // Clear the boss-pending latch for a woman (the host spawned her boss).
    void consumeBoss(Woman w) { m_bossConsumed[(uint32_t)w] = true; }

    // ---- THE ENDING-ELIGIBILITY QUERY (the finale calls this) -------------
    // Pure function of the current state + a finale P4 choice: returns EVERY ending the
    // player is eligible for under that choice (most states qualify for 1-3). The
    // finale presents these; the player's prior arc already narrowed them. Locks the
    // timeline first if still Unset.
    std::vector<Ending> eligibleEndings(FinaleChoice choice);
    // Convenience: the single BEST (lowest-numbered = most golden) eligible ending for
    // a choice, or Ending::None if somehow none qualify (never happens — NewBeginning
    // is the floor). Locks the timeline first if still Unset.
    Ending bestEnding(FinaleChoice choice);
    // True iff `e` is reachable under `choice` from the current state.
    bool endingReachable(Ending e, FinaleChoice choice);

    // ---- Serialize (plain POD blit for the future save layer) -------------
    struct SaveState {
        uint32_t timeline;
        MoralityAxes axes;
        uint32_t fate[kWomanCount];
        float    infTimeLeft[kWomanCount];
        bool     infCured[kWomanCount];
        bool     infRescued[kWomanCount];
        bool     infLost[kWomanCount];
        bool     bossConsumed[kWomanCount];
        bool     sarahSaved;
        bool     sarahSavedFirst;
        bool     chenSaved;
    };
    SaveState serialize() const;
    void deserialize(const SaveState& s);

private:
    Timeline       m_timeline = Timeline::Unset;
    MoralityAxes   m_axes;
    CaptiveFate    m_fate[kWomanCount]        = { CaptiveFate::Pending, CaptiveFate::Pending, CaptiveFate::Pending };
    InfectionTimer m_inf[kWomanCount];
    bool           m_bossConsumed[kWomanCount] = { false, false, false };
    bool           m_sarahDecided   = false;   // an onSarah* was called
    bool           m_sarahSaved     = false;
    bool           m_sarahSavedFirst= false;
    bool           m_chenSaved      = false;
};

// ===========================================================================
// 6. OPTIONAL THIN NOTIFY HOOKS (the non-invasive bridge for existing systems)
// ===========================================================================
// A process-wide TimelineState other game systems CAN feed with a single line, so
// rescue.*/level1_game.*/spire_top.* need at most a one-line `notify*` call (and only
// if it's clean to add — the module works without them). Lazily created; thread-affine
// to the game thread like the rest of the slice. Tests use a LOCAL TimelineState and
// never touch this global.
TimelineState& globalTimeline();
void resetGlobalTimeline();   // for tests / new-game

// One-line bridges (map straight onto TimelineState methods on the global instance):
inline void notifyWomanSaved(Woman w)          { globalTimeline().onWomanSaved(w); }
inline void notifyWomanLost(Woman w)           { globalTimeline().onWomanLost(w); }
inline void notifySarahSaved(bool first)       { globalTimeline().onSarahSaved(first); }
inline void notifySarahLost()                  { globalTimeline().onSarahLost(); }
inline void notifyAllyJoined()                 { globalTimeline().onAllyJoined(); }
inline void notifyAugment(bool accepted)       { globalTimeline().onAugment(accepted); }

// ===========================================================================
// Headless self-test (--test-timeline). No window / Vulkan / physics.
// ===========================================================================
// Drives deterministic event sequences and asserts:
//   * timeline selection: save 2/3 women + lose Sarah -> Alpha; save all + Sarah ->
//     Omega; Sarah-first -> Beta; fail -> Gamma;
//   * the eligible 12-ending set per timeline + finale choice;
//   * axis math (Humanity/Karma/etc. update + clamp correctly);
//   * the infection-timer 4-stage progression + the [0,90,60,30,0] cure rates +
//     expiry -> becameBoss;
//   * serialize/deserialize round-trips.
// Prints "timeline: X/Y passed"; returns true iff all pass.
bool runTimelineSelfTest();

} // namespace x3::game
