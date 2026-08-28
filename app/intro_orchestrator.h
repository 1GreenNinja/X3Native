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
//   3. Decides the branch by what the player DID (EARNED, deterministic — the
//      original spec §4 hidden skill->p->chanceRoll was replaced 2026-07-27;
//      a flawless run can no longer coin-flip into shot-down):
//        * dreadnought hull 0                          -> CapitalKilled
//        * all 4 hardpoints down + pilot alive at end  -> Escaped
//        * anything less (died, bailed, timed out)     -> ShotDown (canon)
//      skillScore/outcomeProbability/rollOutcome survive as pure, self-tested
//      telemetry (difficulty tuning reads them); they no longer pick the branch.
//   4. Writes the result to StoryFlags key "intro.outcome" =
//      "escaped"|"shot_down"|"capital_killed".
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
//
// CapitalKilled is the THIRD outcome (owner canon, 2026-07-27: "kill big ship..
// it crashes... i land.. recover tech and prisoners from it.. break IN to Lab
// zero"). Unlike Escaped/ShotDown it is NOT a dice roll — it is EARNED: it fires
// if and only if the player actually drove the dreadnought's hull to 0 in the
// climax window. The capital then falls out of orbit and craters on the surface;
// Jake sets down at the wreck, salvages its tech, frees the prisoners it was
// carrying, and breaches Lab Zero from OUTSIDE — inverting the canon "wakes up
// captured in a cell" opening while keeping Lab Zero as the destination.
enum class IntroOutcome { ShotDown, Escaped, CapitalKilled };

// The StoryFlags key the orchestrator writes the outcome to (spec §5). app_run.cpp
// (Phase 4) reads this to select the Act-1 build (cell vs surface landing).
inline constexpr const char* kIntroOutcomeFlag = "intro.outcome";
inline constexpr const char* kIntroOutcomeEscaped  = "escaped";
inline constexpr const char* kIntroOutcomeShotDown = "shot_down";
inline constexpr const char* kIntroOutcomeCapitalKilled = "capital_killed";

// SURFACE HAND-OFF marker (Phase 6 -> Phase 7). On the ESCAPED branch, after the
// antimatter-drain stinger, the orchestrator runs the ion-pulse atmo-descent
// (app/space/descent.*, on-rails orbit->ground, the glass facility growing below).
// When that on-rails descent reaches the surface it sets THIS StoryFlags key. The
// Phase-7 surface-landing Act-1 (and app_run's branch select) reads it to confirm
// the descent completed and the player is to spawn OUTSIDE the glass facility (the
// rescuer start) rather than inside the canon cell. It is only ever set on the
// escape path that ran the full descent; ShotDown never sets it (canon -> cell).
inline constexpr const char* kIntroLandedFlag = "intro.landed";

// CRASH-SITE hand-off (the CapitalKilled branch). Set alongside kIntroLandedFlag
// when the player KILLED the dreadnought: the wreck is down on the surface near
// the facility, and Act-1 is to start at that crash site — salvage its tech,
// free the prisoners in its hold, then breach Lab Zero from outside. Act-1 reads
// this to pick the wreck start over the plain rescuer start.
inline constexpr const char* kIntroWreckFlag = "intro.wreck";

// The deterministic-roll node key (fed to chanceRoll, mirrors a chat-tree nodeKey).
inline constexpr const char* kIntroRollKey = "intro.outcome";

// p-mapping bounds (spec §4). p = clamp(kFloorP + skill*(kCeilP-kFloorP)).
// LEGACY/TELEMETRY since 2026-07-27: the mapping is pure, self-tested, and used
// for difficulty telemetry — it no longer decides the branch (outcomes are
// EARNED; see the header block above).
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
    // THE KILL. True iff the player drove the capital's HULL to 0 in a window.
    // This is not scored — it OVERRIDES the roll entirely and forces
    // IntroOutcome::CapitalKilled (the crash -> salvage -> Lab Zero breach).
    bool  capitalDestroyed    = false;
};

// Number of destructible capital-ship subsystems (mirrors x3::space::Subsystem::Count).
inline constexpr int kMaxSubsystems = 4;

// THE ARENA: how far down +X the dreadnought stands off from the player's
// spawn. ONE source of truth, because two places need it and they must not
// drift: the escort screen is staged as fractions of this lane, and the
// capital itself is placed at it. Widened from 2.6 km to 4.2 km for the item-G
// 4x rescale — a ~4.5 km hull ran off both edges of the frame at the old
// standoff, and item F needs the whole vessel visible to pick hardpoints off.
inline constexpr float kIntroArenaX = 4200.0f;

// Map raw metrics -> a normalized skillScore in [0,1] (clamped). Pure; the weights
// are documented at kSkillWeights in the .cpp. Exposed for the self-test.
float skillScore(const SkillMetrics& m);

// Map a skillScore in [0,1] -> the effective success probability p (spec §4).
// p = clamp(kFloorP + clamp(skill,0,1)*(kCeilP-kFloorP), kFloorP, kCeilP).
float outcomeProbability(float skillScore01);

// Roll the outcome deterministically from (seed, skillScore) using the SAME
// chanceRoll the dialog/mission {chance} op uses. p computed via outcomeProbability.
// Pure: same (seed, skill) always yields the same IntroOutcome.
// LEGACY/TELEMETRY since 2026-07-27: kept pure + self-tested; the live branch is
// the earned mapping in runInteractiveIntro, not this roll.
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

// [P0-1 EFLZ-GP-1B] SURFACE->FACILITY HANDOFF, flag carry-over (spec
// specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md §3.3.5 / H4). The arriving canon
// world's live StoryFlags start empty (chatTrees.loadDefault() loads dialog, not
// flags), so the intro's persisted narrative lane must be IMPORTED or the
// escaped outcome is unreadable after the world load. Reads `path` (default:
// defaultGameStoryFlagsPath()); iff the persisted outcome is ESCAPED, sets
// intro.outcome=escaped (+ intro.landed when present) on `into` and returns
// true. Any other state (missing file, shot_down) returns false and leaves
// `into` untouched — the ShotDown path stays byte-identical (spec §3.5).
bool importEscapedIntroFlags(x3::game::StoryFlags& into,
                             const std::string& path = std::string());

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
// (no force) headless run is deterministic, matches the EARNED mapping of its
// replayed metrics (kill -> capital_killed, cripple+survive -> escaped, else
// shot_down), and is SEED-INDEPENDENT (outcomes are earned, never rolled).
// Returns true iff all sub-checks pass.
bool runIntroBranchSelfTest();

} // namespace x3::intro
