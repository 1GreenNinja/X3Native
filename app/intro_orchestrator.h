#pragma once
// ============================================================================
// INTRO ORCHESTRATOR — the interactive branching cold-open's heart.
//
// Spec: docs/design/INTERACTIVE_INTRO_DESIGN.md §3-§5, §8.
// Plan: docs/design/INTERACTIVE_INTRO_PLAN.md Phase 3.
//
// A thin BEAT STATE MACHINE that owns the intro and hands off between the pure,
// deterministic CutscenePlayer (app/cutscene.{h,cpp}) for cinematic clips and
// the space-combat gameplay stack (SpacePilotController + EnemyShipManager +
// TargetingSystem) for bounded INTERACTIVE WINDOWS. It deliberately does NOT
// bolt interactivity into the passive cutscene loop — cutscene.cpp / space_pilot
// .cpp internals are UNTOUCHED; this unit calls only their public APIs.
//
// What it does, end to end:
//   1. Runs an ordered list of beats. Each beat is either:
//        * CutsceneClip      — play a named span of the cold_open cutscene via
//                              CutscenePlayer, blocking + K-skippable.
//        * InteractiveWindow — hand control to the space-combat stack for a
//                              bounded encounter with an explicit exit condition
//                              + timeout; INPUT STATE IS CLEARED on every
//                              cinematic<->interactive hand-off so no key sticks.
//   2. Accumulates a normalized skillScore in [0,1] from the interactive windows
//      (subsystems destroyed, final hull %, salvos dodged, hit accuracy,
//      time-to-cripple — see kSkillWeights in the .cpp for the exact weighting).
//   3. Computes p = clamp(0.07 + skillScore*(0.40-0.07), 0.07, 0.40) and rolls
//      the EXISTING deterministic x3::game::chanceRoll(seed, "intro.outcome") < p
//      -> Escaped, else ShotDown (save-seed deterministic; not frame RNG).
//   4. Writes the result to StoryFlags key "intro.outcome" = "escaped"|"shot_down".
//
// This is "game/slice" code; engine/ stays pure. The interactive-window HOSTING
// (drawing the live space scene, reading GLFW input) is driven by the host that
// owns the device/window; the deterministic outcome/skill/branch CORE here is
// fully headless + unit-tested via runIntroOrchestratorSelfTest (--test-introorch).
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace x3 { namespace apphost { struct HostContext; } }
namespace x3 { namespace game { class StoryFlags; } }

namespace x3::intro {

// The branch the intro forks the game start on (spec §1, §5).
enum class IntroOutcome { ShotDown, Escaped };

// The StoryFlags key the orchestrator writes the outcome to (spec §5). app_run.cpp
// (Phase 4) reads this to select the Act-1 build (cell vs surface landing).
inline constexpr const char* kIntroOutcomeFlag = "intro.outcome";
inline constexpr const char* kIntroOutcomeEscaped  = "escaped";
inline constexpr const char* kIntroOutcomeShotDown = "shot_down";

// SURFACE HAND-OFF marker (Phase 6 -> Phase 7). On the ESCAPED branch, after the
// antimatter-drain stinger, the orchestrator runs the ion-pulse atmo-descent
// (app/space/descent.*, on-rails orbit->ground, the glass facility growing below).
// When that on-rails descent reaches the surface it sets THIS StoryFlags key. The
// Phase-7 surface-landing Act-1 (and app_run's branch select) reads it to confirm
// the descent completed and the player is to spawn OUTSIDE the glass facility (the
// rescuer start) rather than inside the canon cell. It is only ever set on the
// escape path that ran the full descent; ShotDown never sets it (canon -> cell).
inline constexpr const char* kIntroLandedFlag = "intro.landed";

// The deterministic-roll node key (fed to chanceRoll, mirrors a chat-tree nodeKey).
inline constexpr const char* kIntroRollKey = "intro.outcome";

// p-mapping bounds (spec §4). p = clamp(kFloorP + skill*(kCeilP-kFloorP)).
inline constexpr float kFloorP = 0.07f;   // ~7% floor (canon: almost always shot down)
inline constexpr float kCeilP  = 0.40f;   // ~40% ceiling for a flawless run

// ---------------------------------------------------------------------------
// Beat model
// ---------------------------------------------------------------------------
enum class BeatKind { CutsceneClip, InteractiveWindow };

// One beat in the intro sequence. A CutsceneClip names a [start,end] span of the
// cold-open cutscene to play (blocking, K-skip). An InteractiveWindow hands control
// to the combat stack with a spawn count + timeout + a clear exit condition.
struct Beat {
    BeatKind    kind = BeatKind::CutsceneClip;
    std::string id;            // human-readable beat id (sequencing assertions/logs)

    // --- CutsceneClip fields ---
    float       clipStart = 0.0f;   // cutscene-time span start (s)
    float       clipEnd   = 0.0f;   // cutscene-time span end   (s); <=start => to end

    // --- InteractiveWindow fields ---
    int         enemyCount = 0;      // how many enemy fighters to spawn
    float       timeoutSec = 0.0f;   // bounded-encounter timeout (exit condition)
    bool        isClimax   = false;  // the dogfight window whose skill drives the roll
};

// The default authored beat sequence (spec §3 "Beat flow"): cinematic flight ->
// cinematic reveal -> interactive dodge -> cinematic charge -> interactive dogfight
// (climax) -> outcome. Exposed so the self-test can assert the order without
// running the live windows.
std::vector<Beat> defaultIntroBeats();

// ---------------------------------------------------------------------------
// Skill metrics — what the interactive windows produce, fed into skillScore().
// ---------------------------------------------------------------------------
struct SkillMetrics {
    int   subsystemsDestroyed = 0;   // capital-ship subsystems downed (0..kMaxSubsystems)
    float finalHullFrac       = 1.0f;// player hull fraction at the climax end [0,1]
    int   salvosDodged        = 0;   // enemy salvos evaded (no hit landed)
    int   salvosFaced         = 0;   // total enemy salvos faced (dodged + hit)
    int   shotsHit            = 0;   // player laser bolts that hit a target
    int   shotsFired          = 0;   // player laser bolts fired
    float timeToCrippleSec    = 0.0f;// time to cripple the capital ship (s); 0 = never
    float windowDurationSec   = 1.0f;// the climax window length (s; normalizer for time)
};

// Number of destructible capital-ship subsystems (mirrors x3::space::Subsystem::Count).
inline constexpr int kMaxSubsystems = 4;

// Map raw metrics -> a normalized skillScore in [0,1] (clamped). Pure; the weights
// are documented at kSkillWeights in the .cpp. Exposed for the self-test.
float skillScore(const SkillMetrics& m);

// Map a skillScore in [0,1] -> the effective success probability p (spec §4).
// p = clamp(kFloorP + clamp(skill,0,1)*(kCeilP-kFloorP), kFloorP, kCeilP).
float outcomeProbability(float skillScore01);

// Roll the outcome deterministically from (seed, skillScore) using the SAME
// chanceRoll the dialog/mission {chance} op uses. p computed via outcomeProbability.
// Pure: same (seed, skill) always yields the same IntroOutcome.
IntroOutcome rollOutcome(uint32_t seed, float skillScore01);

// Write the outcome to StoryFlags under kIntroOutcomeFlag (escaped|shot_down).
void writeOutcomeFlag(x3::game::StoryFlags& flags, IntroOutcome outcome);

// Read the outcome back from StoryFlags (mirror of writeOutcomeFlag). Returns
// the encoded outcome; defaults to ShotDown (canon) when neither key is present
// (so a missing/cleared flag is the safe canon path). app_run.cpp (Phase 4) uses
// this to select the Act-1 build.
IntroOutcome readOutcomeFlag(const x3::game::StoryFlags& flags);

// The persistent file the interactive intro writes the outcome to (the x3::game
// StoryFlags narrative lane app_run reads). Distinct from the cut::StoryFlags
// intro_complete file. Honors LOCALAPPDATA like the cutscene flags path.
std::string defaultGameStoryFlagsPath();

// ---------------------------------------------------------------------------
// The entry point (spec §3). Runs the beat sequence (cinematic clips blocking via
// CutscenePlayer, interactive windows handing control to the combat stack with
// input cleared on hand-off), accumulates skillScore from the windows, rolls the
// skill-biased deterministic outcome, writes intro.outcome to StoryFlags, and
// returns the branch. `hc` provides the device/window for the live windows; in a
// headless context the live windows degrade to a deterministic time-advance (the
// outcome/branch core still runs).
// ---------------------------------------------------------------------------
IntroOutcome runInteractiveIntro(x3::apphost::HostContext& hc);

// F9 — SKIP THE WHOLE SPACE INTRO (owner dev shortcut: "make a key to skip the
// space intro.. f9"). Any intro surface (film clip or interactive beat) that sees
// F9 calls requestSkipAllIntro(); the orchestrator's beat loop bails at the next
// boundary, skips the roll + stinger, and returns the canon ShotDown outcome —
// straight to waking in the cell. Reset at each runInteractiveIntro entry.
void requestSkipAllIntro();
bool skipAllIntroRequested();

// --test-introorch self-test (headless, deterministic, no window/Vulkan): asserts
// beat sequencing order; deterministic outcome for a fixed (seed, skill); the
// skill->p mapping bounds (skill 0 -> p=0.07, skill 1 -> p=0.40, monotonic); the
// StoryFlags["intro.outcome"] write per outcome; and input/state cleared on the
// cinematic<->interactive hand-off. Returns true iff all sub-checks pass.
bool runIntroOrchestratorSelfTest();

// --test-introbranch self-test (Phase 4, headless, deterministic, no window):
// asserts the branch-selection logic app_run.cpp drives — forcing the outcome to
// escaped writes/reads intro.outcome=escaped (the surface-stub path); forcing
// shot_down writes/reads intro.outcome=shot_down (the canon cell); a default
// (no force) headless run is deterministic per seed and round-trips through the
// persisted StoryFlags; and the seed thread is honored (different seeds can roll
// differently; the same seed+force is stable). Returns true iff all sub-checks pass.
bool runIntroBranchSelfTest();

} // namespace x3::intro
