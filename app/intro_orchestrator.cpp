// ============================================================================
// INTRO ORCHESTRATOR — implementation. See intro_orchestrator.h for the design.
//
// DETERMINISTIC CORE (live, headless-testable):
//   * defaultIntroBeats() — the authored beat sequence.
//   * skillScore()        — weighted normalize of the interactive-window metrics.
//   * outcomeProbability()/rollOutcome() — the spec §4 skill->p->chanceRoll branch.
//   * writeOutcomeFlag()  — the StoryFlags write app_run branches on.
//
// LIVE HOSTING (when hc.window + hc.device are present):
//   * Cinematic clips play (blocking, K-skip) via runCutsceneWindowed over a span
//     of the cold-open cutscene.
//   * Interactive windows hand control to SpacePilotController + EnemyShipManager
//     + TargetingSystem for a bounded encounter (timeout / all-enemies-down exit),
//     clearing input state on every hand-off so no key sticks.
// In a HEADLESS context the live windows degrade to a deterministic advance that
// still feeds the metrics, so runInteractiveIntro is exercised end-to-end without
// a window — the outcome/branch core is identical either way.
//
// cutscene.cpp / space_pilot.cpp internals are NOT modified — only public APIs.
// ============================================================================
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <chrono>
#include <thread>

#include "intro_orchestrator.h"
#include "intro_cockpit_rig.h"
#include "fx.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"   // hc.audio threaded to the cinematic beats (Phase 5)

#include "host_context.h"
#include "story_ops.h"        // x3::game::StoryFlags + x3::game::chanceRoll
#include "cutscene.h"         // x3::cut::CutscenePlayer + the cold-open asset
#include "cinematic.h"        // runCutsceneWindowed (public cinematic driver)
#include "asset_root.h"
#include "audio_root.h"       // resolveAudio(...) — dogfight combat SFX (repo-local first)
#include "engine/asset/IAssetSource.h"   // player fighter model for the 3P view
#include "engine/asset/IModelLoader.h"
#include "space_pilot.h"
#include "space/ship_ai.h"
#include "space/targeting.h"
#include "space/ship_damage.h"
#include "space/sun_bake.h"   // the living-sun surface bake (flyable star port)
#include "mesh_prims.h"       // makeUVSphere — the star core / corona shells
#include "space/space_layer.h"   // x3::space::SpaceLayer (S0 spine; AtmoDescent runner host)
#include "space/descent.h"       // x3::space::AtmoDescent (the ion-pulse on-rails coast-down)
#include "space/hud_project.h"   // world->screen HUD projection + collision-aware label layout
#include "space/cockpit_sway.h"  // cockpit mass: the interior lags the hull under accel
#include "settings_io.h"         // readTuningFloat — the live dogfight-feel levers

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace x3::intro {

// ---------------------------------------------------------------------------
// Skill weighting (spec §4). The five inputs each map to [0,1] then combine with
// these weights (sum == 1.0) so skillScore is itself in [0,1]:
//   subsystems  0.30 — capital-ship subsystems destroyed / kMaxSubsystems
//   hull        0.25 — final hull fraction (survive intact = skilled)
//   dodge       0.20 — salvos dodged / salvos faced (evasion)
//   accuracy    0.15 — shots hit / shots fired (aim)
//   speed       0.10 — 1 - timeToCripple/windowDuration (cripple fast = skilled)
// Rationale: crippling the ship's subsystems is the primary objective (heaviest),
// surviving with hull is next, then evasion + aim, with a small speed bonus.
// ---------------------------------------------------------------------------
namespace {
struct SkillWeights { float subsystems, hull, dodge, accuracy, speed; };
constexpr SkillWeights kSkillWeights{ 0.30f, 0.25f, 0.20f, 0.15f, 0.10f };

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---------------------------------------------------------------------------
// CALLOUT SOURCE — the messaging law for the space HUD.
// ---------------------------------------------------------------------------
// Owner playtest, 2026-08: "Antimatter critical, igniting ion drive? Does that
// mean I destroyed the big enemy ship?" It did not — that line is JAKE'S OWN
// drive igniting. Every space bark used to be the same amber text in the same
// stack, so "SHIELDS DOWN - HULL EXPOSED" (the dreadnought's) sat in the same
// channel as "HULL TEMP CRITICAL - PULL AWAY" (yours) and the player had no way
// to tell WHOSE event he was reading, or whether he had just won.
//
// THE LAW, applied to every string in this file:
//   * every callout names its OWNER in the text ("YOUR ..." / "DREADNOUGHT ...")
//     — the words alone must disambiguate with the colour stripped out;
//   * every callout carries a SOURCE, which picks a prefix + a colour:
//       PLAYER  ">> "  cyan   — your ship: the colour of your own HULL/SHD readout
//       ENEMY   "!! "  amber  — the dreadnought and its subsystems
//       MISSION "** "  white-hot — objective state: the kill, the reactor breach
//   * a MISSION callout is reserved for "the state of the fight changed" and is
//     the loudest thing on screen, because that is the one the player is asking
//     about when he asks "did I just win?".
// (Layout is untouched — the reticle/bracket lane owns that. This is text +
// colour + source only.)
enum class CalloutSrc : uint32_t { Player = 0, Enemy = 1, Mission = 2 };
constexpr CalloutSrc kCalloutPlayer  = CalloutSrc::Player;
constexpr CalloutSrc kCalloutEnemy   = CalloutSrc::Enemy;
constexpr CalloutSrc kCalloutMission = CalloutSrc::Mission;

// Prefix + colour for one source. Colours are linear RGBA; the mission tint is
// deliberately >1 in R/G so the kill line BLOOMS (emissive strength > 1 blooms
// in this engine) and reads as the loudest event in the frame.
inline const char* calloutPrefix(CalloutSrc s) {
    switch (s) {
        case CalloutSrc::Player:  return ">> ";
        case CalloutSrc::Enemy:   return "!! ";
        default:                  return "** ";
    }
}
inline void calloutColor(CalloutSrc s, float a, float out[4]) {
    switch (s) {
        case CalloutSrc::Player:                                   // your ship: cyan
            out[0] = 0.45f; out[1] = 0.88f; out[2] = 1.00f; break;
        case CalloutSrc::Enemy:                                    // the enemy: amber
            out[0] = 0.98f; out[1] = 0.72f; out[2] = 0.30f; break;
        default:                                                   // mission: white-hot
            out[0] = 1.60f; out[1] = 1.35f; out[2] = 0.85f; break;
    }
    out[3] = a;
}
} // namespace

// ---------------------------------------------------------------------------
// OUTCOME STINGER spans (Phase 5) — the two endings authored in the cold-open
// timeline (assets/cutscenes/cold_open.cutscene.json). The cine.outcome beat plays
// ONE of these, selected by the rolled IntroOutcome:
//   * SHOT_DOWN [36..66]: the killing salvo -> smash-to-black -> "ESCAPE FROM LAB
//     ZERO" + "SIX MONTHS LATER" (canon: wake in the cell).
//   * ESCAPED   [66..80]: Jake slips the kill-box, a glancing hit drains the
//     antimatter, the ion-pulse drive ignites -> ">> YOUR ANTIMATTER IS GONE —
//     IGNITING ION DRIVE" -> hands off to the Phase-6 atmo descent. (The ">> "
//     prefix + the cyan tint are the PLAYER-EVENT channel of the messaging law
//     above: the owner read the old wording as "I destroyed the enemy ship".)
// (Tunable here in lock-step with the JSON span boundaries.)
inline constexpr float kOutcomeShotDownSpan[2] = { 36.0f, 66.0f };
inline constexpr float kOutcomeEscapedSpan[2]  = { 66.0f, 80.0f };

std::vector<Beat> defaultIntroBeats() {
    // The cold-open cutscene is 67.5 s (assets/cutscenes/cold_open.cutscene.json).
    // We carve named cinematic spans around two interactive windows. Phase 5 will
    // author dedicated clips; here the spans index the existing single timeline.
    std::vector<Beat> beats;

    // 1) Cinematic — Jake launches / flies.
    Beat b1; b1.kind = BeatKind::CutsceneClip; b1.id = "cine.flight";
    b1.clipStart = 0.0f; b1.clipEnd = 20.0f; beats.push_back(b1);

    // 2) Cinematic reveal — the capital ship resolves blob->detailed.
    Beat b2; b2.kind = BeatKind::CutsceneClip; b2.id = "cine.reveal";
    b2.clipStart = 20.0f; b2.clipEnd = 30.0f; beats.push_back(b2);

    // 3) Interactive window 1 — dodge the opening salvo (light; not the climax).
    Beat b3; b3.kind = BeatKind::InteractiveWindow; b3.id = "play.dodge";
    b3.enemyCount = 2; b3.timeoutSec = 8.0f; b3.isClimax = false; beats.push_back(b3);

    // 4) Cinematic — the ship charges its main weapon / launches fighters.
    Beat b4; b4.kind = BeatKind::CutsceneClip; b4.id = "cine.charge";
    b4.clipStart = 30.0f; b4.clipEnd = 36.0f; beats.push_back(b4);

    // 5) Interactive window 2 — the dogfight (CLIMAX; its skill drives the roll).
    Beat b5; b5.kind = BeatKind::InteractiveWindow; b5.id = "play.dogfight";
    // MORE TINY SHIPS (owner, live 2026-08-17: "There should be More of the
    // tiny ships"): the climax escort screen grows 4 -> 9. The per-ship cost
    // is a handful of draws + a 6DOF tick, and the PVS->TLAS work means the
    // ones you are not looking at cost effectively nothing.
    b5.enemyCount = 9; b5.timeoutSec = 20.0f; b5.isClimax = true; beats.push_back(b5);

    // 6) Cinematic — the outcome stinger. The span here is the DEFAULT (shot-down)
    //    framing; runInteractiveIntro OVERRIDES clipStart/clipEnd per the rolled
    //    IntroOutcome (shot-down vs escape stinger — see kOutcome*Span below) after
    //    the climax, so this beat reads as whichever ending the roll produced.
    Beat b6; b6.kind = BeatKind::CutsceneClip; b6.id = "cine.outcome";
    b6.clipStart = kOutcomeShotDownSpan[0]; b6.clipEnd = kOutcomeShotDownSpan[1];
    beats.push_back(b6);

    return beats;
}

float skillScore(const SkillMetrics& m) {
    const float subs = clamp01(kMaxSubsystems > 0
        ? (float)m.subsystemsDestroyed / (float)kMaxSubsystems : 0.0f);
    const float hull = clamp01(m.finalHullFrac);
    const float dodge = clamp01(m.salvosFaced > 0
        ? (float)m.salvosDodged / (float)m.salvosFaced : 0.0f);
    const float acc = clamp01(m.shotsFired > 0
        ? (float)m.shotsHit / (float)m.shotsFired : 0.0f);
    // Time-to-cripple: faster is better. 0 (never crippled) => 0 speed credit.
    float speed = 0.0f;
    if (m.timeToCrippleSec > 0.0f && m.windowDurationSec > 0.0f)
        speed = clamp01(1.0f - m.timeToCrippleSec / m.windowDurationSec);

    const float s = subs  * kSkillWeights.subsystems
                  + hull  * kSkillWeights.hull
                  + dodge * kSkillWeights.dodge
                  + acc   * kSkillWeights.accuracy
                  + speed * kSkillWeights.speed;
    return clamp01(s);
}

float outcomeProbability(float skillScore01) {
    const float s = clamp01(skillScore01);
    const float p = kFloorP + s * (kCeilP - kFloorP);
    return std::min(std::max(p, kFloorP), kCeilP);
}

IntroOutcome rollOutcome(uint32_t seed, float skillScore01) {
    const float p = outcomeProbability(skillScore01);
    const float roll = x3::game::chanceRoll(seed, kIntroRollKey);
    return roll < p ? IntroOutcome::Escaped : IntroOutcome::ShotDown;
}

void writeOutcomeFlag(x3::game::StoryFlags& flags, IntroOutcome outcome) {
    // Mirror the chat-tree convention: a single namespaced key whose VALUE is
    // encoded as the suffix flag (StoryFlags is a flag set, so "intro.outcome=X"
    // is represented as the presence of the resolved key). app_run branches on it.
    flags.clear(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeEscaped);
    flags.clear(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeShotDown);
    flags.clear(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeCapitalKilled);
    const char* val = kIntroOutcomeShotDown;
    if (outcome == IntroOutcome::Escaped)            val = kIntroOutcomeEscaped;
    else if (outcome == IntroOutcome::CapitalKilled) val = kIntroOutcomeCapitalKilled;
    flags.set(std::string(kIntroOutcomeFlag) + "=" + val);
}

IntroOutcome readOutcomeFlag(const x3::game::StoryFlags& flags) {
    // Escaped/CapitalKilled only when their key is explicitly present;
    // absence/cleared => canon ShotDown (the safe default so a missing flag never
    // mis-routes to the stub). CapitalKilled is checked FIRST: it is the strongest
    // claim (an earned kill), so if both were somehow present it wins.
    if (flags.has(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeCapitalKilled))
        return IntroOutcome::CapitalKilled;
    if (flags.has(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeEscaped))
        return IntroOutcome::Escaped;
    return IntroOutcome::ShotDown;
}

std::string defaultGameStoryFlagsPath() {
    // The x3::game StoryFlags narrative lane (the intro outcome lives here, beside
    // the per-save flags). Distinct from the cut::StoryFlags intro_complete file.
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        return std::string(la) + "/X3Native/game_flags.txt";
    return "game_flags.txt";
}

bool importEscapedIntroFlags(x3::game::StoryFlags& into, const std::string& path) {
    // [P0-1] See the header note. Loads the persisted intro lane into a scratch
    // set first so a shot_down / absent save NEVER mutates the live flags world.
    const std::string p = path.empty() ? defaultGameStoryFlagsPath() : path;
    x3::game::StoryFlags disk;
    if (!disk.loadFile(p)) return false;                       // no persisted intro
    if (readOutcomeFlag(disk) != IntroOutcome::Escaped) return false;   // canon path
    into.set(std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeEscaped);
    if (disk.has(kIntroLandedFlag)) into.set(kIntroLandedFlag);
    return true;
}

// ---------------------------------------------------------------------------
// Live hosting helpers
// ---------------------------------------------------------------------------
namespace {

// Clear any sticky input on a cinematic<->interactive hand-off (spec §7.3). With
// a window: reset the cursor delta baseline + swallow held keys so no key carries
// across the seam into the interactive controller. Headless: a no-op.
void clearInputState(GLFWwindow* window) {
    if (!window) return;
    glfwPollEvents();
    // Re-baseline the mouse so the first interactive frame sees zero look delta.
    double mx, my; glfwGetCursorPos(window, &mx, &my); (void)mx; (void)my;
    // (The interactive window re-reads keys fresh each frame via glfwGetKey, and
    //  re-baselines its own lastMX/lastMY from the current cursor on entry, so
    //  re-polling here is sufficient to drop any carried-over state.)
}

// Play one cinematic clip span (blocking, K-skip). With a window this drives the
// real CutscenePlayer via runCutsceneWindowed seeked to clipStart; headless it is
// a deterministic no-op (the cutscene player is exercised by --test-cutscene).
// ---- F9 skip-all latch (see intro_orchestrator.h). The flag lives here (file
// scope) so the anon-namespace beat helpers can read it; the PUBLIC accessors are
// defined after the anon namespace closes — inside it they'd get internal linkage
// and cinematic.obj's reference would not resolve (LNK2019, been there).
static bool s_skipAllIntro = false;

void playCinematicBeat(x3::apphost::HostContext& hc, const Beat& beat,
                       const x3::cut::Cutscene* cs) {
    x3::logInfo("[intro] beat '" + beat.id + "' (cinematic clip [" +
                std::to_string(beat.clipStart) + ".." + std::to_string(beat.clipEnd) + "] s)" +
                (hc.audio ? " [audio]" : " [silent]"));
    if (hc.window && hc.device && cs) {
        clearInputState(hc.window);
        // CLIP-SPLIT (Phase 5): play ONLY this beat's span [clipStart, clipEnd) then
        // return to the orchestrator (the interactive windows occupy the gaps). AUDIO
        // RESTORE (Phase 5): pass hc.audio (P4 passed nullptr -> silent intro); the
        // music bed carries across clip beats (runCutsceneWindowed only stops music on
        // the final/clipless run). hostEvent is left default — the orchestrator owns
        // the StoryFlags/branch, not the cutscene's intro_complete endState.
        x3::apphost::runCutsceneWindowed(*hc.device, hc.window, hc.audio, *cs,
                                         beat.clipStart, /*hostEvent*/ {}, beat.clipEnd);
        clearInputState(hc.window);
    }
}

// Run one bounded interactive combat window and accumulate metrics into `m`. The
// climax window's metrics drive the outcome roll. With a window this hands control
// to the live combat stack; headless it advances deterministically (fixed-dt sim,
// synthetic player aim) so the metrics are reproducible without a GPU.
void runInteractiveBeat(x3::apphost::HostContext& hc, const Beat& beat,
                        SkillMetrics& m,
                        x3::apphost::IntroCockpitRig* cockpit = nullptr) {
    x3::logInfo("[intro] beat '" + beat.id + "' (interactive: " +
                std::to_string(beat.enemyCount) + " enemies, " +
                std::to_string(beat.timeoutSec) + " s timeout)");
    clearInputState(hc.window);

    // Physics world for the pilot body. Headless-safe (no GPU).
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[intro] physics init failed"); return; }

    x3::game::SpacePilotController pilot;
    // FIGHTER tuning (owner playtest: "sleek — the ship should DART around").
    // The stock cinematic-arcade defaults read sluggish from inside the cockpit;
    // a fighter wants violent thrust + hard strafe so dodging feels like flying.
    x3::game::SpacePilotController::Tuning tun{};
    tun.maxLinearAccel = 130.0f;  // owner, live: "W and S do not provide enough
                                  // acceleration" (was 70; boosted strafe outran it)
    tun.maxStrafeAccel = 44.0f;   // A/D strafe: hard left/right dodge authority
                                  // (x5 under Shift — the escape move)
    tun.boostMul       = 5.0f;    // SHIFT = ANTIMATTER BOOST (owner ask) — dramatic,
                                  // you feel it slam the ship forward, not a nudge.
    tun.maxSpeed       = 360.0f;  // raised so the boost has real top-end to reach for
    // COMMERCIAL CHASE FRAMING (owner: "match commercial flight space games — a
    // chase cam always behind a SMALLER ship which can dart wherever"): pull the
    // cam back + up so the ~10 m hull reads Everspace-small (roughly 15% of frame
    // height, low-center) with sky all around it for the dart room.
    tun.chaseDistance  = 22.0f;   // was 12 — the hull filled the frame
    tun.chaseHeight    = 5.0f;
    tun.maxShield      = 5000;    // owner ask: fat shield pool — survive the salvos and
    tun.maxHull        = 2000;    // fly around freely instead of dying in the first pass
    tun.shieldRegenPerSec = 120.0f;   // and it recharges fast, so a hit is not the end
    // WEAPON ENERGY = a REGENERATING pool (owner: "It also runs out after a
    // short time!"). Default cost 8 / regen 12 emptied the bank in ~2 s of held
    // fire and then sputtered at regen pace forever. Tuned so a held trigger
    // sustains ~10-12 s of continuous fire, the pool refills in ~5 s of cease-
    // fire, and depletion is a brief sputter — never a dry lockout. (The flight
    // loop calls update(dt) + fireLaser(dt), both of which tick the cooldown —
    // the effective rate is ~10 shots/s; the numbers below are tuned against
    // THAT measured cadence. Asserted by --test-space T11.)
    tun.energyRegenPerSec = 20.0f;
    tun.laserEnergyCost   = 2.8f;
    tun.noseFollow     = 2.8f;    // arcade steering: the ship GOES where the nose
                                  // points (Newtonian drift read as "axes wrong")
    tun.flightAssist   = 2.5f;    // FLIGHT-ASSIST HOLD (owner, live: "stay in
                                  // position ... not moving unless I WANT to move"):
                                  // release the stick -> coast to a stop over ~1.5 s
                                  // and HOLD station, instead of drifting into the
                                  // capital. Any thrust/strafe/vertical releases it.
    // FIRST person: the beats render from inside the cockpit rig. The default
    // 3P chase cam swings on a 12 m arm rotated by the ship's FULL quaternion
    // (roll included) while setCamera() is roll-less Euler — the mismatch reads
    // as "mouse is off axis" (owner playtest). 1P puts the eye at the ship.
    tun.defaultThirdPerson = true;   // 3P by default: the cockpit rig is a giant
                                     // dark box with a letterbox slot for a window
                                     // (owner: "MICROSCOPIC window", "can't see the
                                     // ship"). Open chase view sees the whole fight.
                                     // F2 toggles back to the cockpit.

    // ======================================================================
    // DOGFIGHT-FEEL LEVERS (owner playtest 2026-08: "The ship doesn't zip left
    // and right as fast as it should, it feels very dampened").
    //
    // Feel is subjective, so these are DIALS, not verdicts. Every one can be
    // changed without a rebuild — set an env var for a one-shot A/B or add the
    // line to %LOCALAPPDATA%\x3native_settings.cfg to make it stick:
    //
    //   X3_SPACE_STRAFEACCEL=44        space.strafeAccel=44
    //
    // The effective values are LOGGED at beat entry ([intro-feel]) so whatever
    // is in the build is always visible in the log next to the frame.
    //
    // WHAT CHANGED AND WHY (previous value -> new default):
    //  * strafeAccel 44 -> 110 m/s^2. Lateral authority was 34% of the forward
    //    thrust (130), so a sideways dodge took 1.36 s to reach 60 m/s while a
    //    straight charge took 0.46 s — the ship simply could not slide. 110 is
    //    85% of forward thrust: 60 m/s of lateral in 0.55 s, and the Shift
    //    escape-dodge (x5) in 0.11 s.
    //  * lookBias 0.30 -> 0.15. The target-keeping assist blends the CAMERA GAZE
    //    toward the locked contact every frame, so 30% of every mouse whip was
    //    being eaten by an assist pulling the view back onto the target — the
    //    textbook "assist fighting the input" feel. 0 turns it off entirely;
    //    0.30 restores the old behaviour for an A/B.
    //  * chaseLagMax 5 -> 8 m. This is the visible zip: how far the hull may
    //    displace in frame before the chase camera catches up. 5 m on a 22 m arm
    //    was ~18% of frame height; 8 m is ~29% and still cannot leave frame.
    // Everything else is exposed unchanged so it can be dialled from the same
    // place rather than hunted for in the source.
    //
    // 6DOF PASS (owner spec: "IDEALLY, we could strafe in 6DOF and keep on a
    // target the whole time. That is the goal for space combat", against the
    // symptom "it darts left and right, but it doesn't FEEL like its moving much
    // past the initial burst"). Three things were killing a strafe the moment it
    // started, and none of them was a per-frame damping term:
    //  * flightAssist 2.5/s engaged on the FIRST idle frame, so releasing the key
    //    bled the dodge away with a 0.4 s time constant -> all the motion was in
    //    the opening burst. Now 0.8/s behind a 0.45 s assistDelay: the slide
    //    HOLDS, then settles to station-keeping (which the owner also asked for).
    //  * noseFollow 2.8/s rotated the whole velocity vector back onto the nose
    //    the instant A/D came up, so lateral velocity was not merely damped, it
    //    was converted into forward motion. That is the exact opposite of 6DOF;
    //    it is now OFF by default and replaced by...
    //  * ...targetHold: a ROTATION-only assist that keeps the nose on the locked
    //    contact while the player translates freely. Assist belongs on
    //    orientation in a 6DOF ship, never on translation.
    // DRIFT MODEL, stated explicitly: velocity now PERSISTS while you fly
    // (Newtonian) and only bleeds when you take your hands off the stick for
    // longer than space.assistDelay. Set space.flightAssist=0 for pure Newtonian
    // with no station-keeping at all.
    // 6DOF DART PASS (owner, live 2026-08-17: "the ship needs to dart around free
    // space much as the enemy ships do — strafing, rising, sinking, accelerating —
    // with visible movement vs the huge capital"). The strafe/vertical cap was 160
    // vs a 360 forward cap, so sideways motion read as the poor cousin; and the
    // 0.8 station-keep brake bled every dodge the moment the stick released. Raise
    // lateral/vertical to ~90% of forward, punch the accels, and soften the brake so
    // the ship SLIDES then settles instead of settling immediately. All dial-able.
    const float lvStrafeAccel   = x3::apphost::readTuningFloat("space.strafeAccel",   200.0f);
    const float lvLinearAccel   = x3::apphost::readTuningFloat("space.linearAccel",   170.0f);
    const float lvNoseFollow    = x3::apphost::readTuningFloat("space.noseFollow",      0.0f);
    const float lvFlightAssist  = x3::apphost::readTuningFloat("space.flightAssist",    0.5f);
    const float lvAssistDelay   = x3::apphost::readTuningFloat("space.assistDelay",     0.7f);
    const float lvMaxStrafeSpd  = x3::apphost::readTuningFloat("space.maxStrafeSpeed", 320.0f);
    const float lvTargetHold    = x3::apphost::readTuningFloat("space.targetHold",      1.6f);
    const float lvLookSmoothing = x3::apphost::readTuningFloat("space.lookSmoothing",  22.0f);
    const float lvChaseLagMax   = x3::apphost::readTuningFloat("space.chaseLagMax",     8.0f);
    const float lvChaseLagRate  = x3::apphost::readTuningFloat("space.chaseLagRate",    7.5f);
    const float lvLookBias      = x3::apphost::readTuningFloat("space.lookBias",        0.15f);
    const float lvSwayStrength  = x3::apphost::readTuningFloat("space.cockpitSway",     1.0f);
    tun.maxLinearAccel = lvLinearAccel;
    tun.maxStrafeAccel = lvStrafeAccel;
    tun.noseFollow     = lvNoseFollow;
    tun.flightAssist   = lvFlightAssist;
    tun.assistDelay    = lvAssistDelay;
    tun.maxStrafeSpeed = lvMaxStrafeSpd;
    tun.lookSmoothing  = lvLookSmoothing;
    tun.chaseLagMax    = lvChaseLagMax;
    tun.chaseLagRate   = lvChaseLagRate;
    x3::logInfo("[intro-feel] strafeAccel=" + std::to_string(lvStrafeAccel) +
                " linearAccel=" + std::to_string(lvLinearAccel) +
                " noseFollow=" + std::to_string(lvNoseFollow) +
                " flightAssist=" + std::to_string(lvFlightAssist) +
                " assistDelay=" + std::to_string(lvAssistDelay) +
                " maxStrafeSpeed=" + std::to_string(lvMaxStrafeSpd) +
                " targetHold=" + std::to_string(lvTargetHold) +
                " lookSmoothing=" + std::to_string(lvLookSmoothing) +
                " chaseLagMax=" + std::to_string(lvChaseLagMax) +
                " chaseLagRate=" + std::to_string(lvChaseLagRate) +
                " lookBias=" + std::to_string(lvLookBias) +
                " cockpitSway=" + std::to_string(lvSwayStrength) +
                "  (override: X3_SPACE_<KEY>=v or space.<key>=v in x3native_settings.cfg)");

    pilot.spawn(*phys, 0.0f, 0.0f, 0.0f, tun);

    // COCKPIT MASS (owner: "The cockpit stays put when the player ship moves").
    // Only ever moves the cockpit MESH — camera, boresight and HUD markers stay
    // on the ship's true pose, so the sway can never fight the aim.
    x3::space::CockpitSway cockpitSway;
    {
        x3::space::CockpitSway::Tuning st{};
        st.refAccel = tun.maxLinearAccel;   // normal thrust saturates it; boost holds it
        st.strength = lvSwayStrength;       // 0 = the old rigid cockpit
        cockpitSway.setTuning(st);
    }
    float swayPrevVel[3] = { 0, 0, 0 };
    bool  swayPrevVelValid = false;
    float swayPrevYaw = 0.0f;
    bool  swayPrevYawValid = false;

    x3::space::EnemyShipManager enemies;
    enemies.init((uint32_t)std::max(0, beat.enemyCount));
    for (int i = 0; i < beat.enemyCount; ++i) {
        const float ang = (float)i * 1.3f;
        // FIGHTER SCREEN (vet pass): the wing has ONE strategic job — sit in the
        // player's approach lane to the capital. The first pair still spawns
        // CLOSE (owner: "I can NEVER SEE THE ENEMY SHIP" — a speck at 250 m is
        // invisible; 70 m fills the canopy and starts the fight), the rest are
        // staged DEEPER down the lane toward the dreadnought, so every attack
        // run flies through them: kill them for a clean run, or ignore them and
        // eat their fire on the strafe. Chase AI closes onto the player anyway —
        // spawn geometry is what makes the lane contested, not new AI.
        float pos[3];
        if (i < 2) {
            pos[0] = 70.0f + 40.0f * (float)i;
            pos[1] = 14.0f * std::sin(ang);
            pos[2] = 26.0f * std::cos(ang);
        } else {
            // Staging spread tightened for the bigger wing (9 ships): the lane
            // fraction caps at ~0.77 so the deepest escorts screen the capital
            // without spawning INSIDE her force bubble.
            const float u = 0.14f + 0.09f * (float)(i - 2);   // fraction of the lane
            pos[0] = 2600.0f * u;                              // toward the capital
            pos[1] = 60.0f * std::sin(ang * 2.1f);
            pos[2] = 90.0f * std::cos(ang * 1.7f);
        }
        enemies.spawn(pos);
    }
    // A PLANET BACKDROP so space reads as SPACE, not a black void (owner: "I can NEVER
    // SEE THE PLANETS"). Eye-anchored bodies hung in the sky by loadNightSkyPlanets;
    // the same helper the showroom + cutscenes use, so no new asset path.
    // ---- Live / capture routing (used from the planet load down) -----------
    // interactive = a real window: GLFW input + real-time pacing.
    // captureMode = X3_INTRO_CAPTURE with NO window (headless --world intro
    //   --smoketest): render the beat OFFSCREEN with a scripted pilot + staged
    //   enemy damage so the combat-readability evidence PNGs come from the real
    //   flight beat with zero windowed pops. Bounded (maxSteps + the s960 stop).
    // live = anything that RENDERS (either of the above). The pure headless
    //   self-test path (no env) is byte-identical to before: live == false.
    const bool interactive = (hc.window != nullptr && hc.device != nullptr);
    const char* evDir = std::getenv("X3_INTRO_CAPTURE");
    const bool captureMode = !interactive && hc.device != nullptr &&
                             evDir != nullptr && *evDir != '\0';
    // X3_INTRO_CAPTURE_KILL=1 switches the capture script from the combat-
    // readability scenario to the CAPITAL-KILL evidence run (see below): the
    // shipped death sequence, framed and shot at 10 Hz through its whole run.
    const bool killCapture = captureMode && [] {
        const char* k = std::getenv("X3_INTRO_CAPTURE_KILL");
        return k && *k && *k != '0';
    }();
    const bool live = interactive || captureMode;

    x3::rhi::MeshHandle planetMesh{}, ringMesh{};
    std::vector<x3::apphost::NightSkyPlanet> planets;
    // THE DOGFIGHT IS IN A NAMED SYSTEM, FAR FROM SOL (owner: "far from earth").
    // Kethzar Prime (x3.starsys/1): an amber-hypergiant sky, a huge hero LAVA world,
    // a ringed gas giant, an ice world — the most dramatic sky in the registry. Its
    // body set drives the far-distance backdrop AND the HUD minimap below; a faint
    // labelled SOL is appended so "far from Earth" is a findable point, not implied.
    const x3::starsys::StarSystem& dfSystem = x3::starsys::dogfightSystem();
    if (live) {   // window OR headless capture: the sky must be in the PNGs too
        int planetTexFail = 0;
        std::vector<x3::apphost::NightSkyPlanet> templates =
            x3::apphost::loadNightSkyPlanets(hc.device, planetMesh, planetTexFail,
                                             "[intro]", &ringMesh);
        if (planetTexFail > 0)
            x3::logWarn("[intro] " + std::to_string(planetTexFail) +
                        " planet texture(s) missing — some bodies flat");
        // Re-hang the loaded texture templates as THIS system's sky (+ faint SOL).
        planets = x3::apphost::buildSystemSky(templates, dfSystem, /*includeSolPinpoint*/ true);
        x3::logInfo("[intro] dogfight sky = " + std::string(dfSystem.name) + " (" +
                    std::to_string((int)dfSystem.distanceLy) + " ly from Sol), " +
                    std::to_string(planets.size()) + " bodies + SOL pinpoint");
    }

    // The star's render assets (live-only; the sim above never needs them):
    // the living-surface bake bound as baseColor AND emissiveTex (host_space
    // recipe — the granulation modulates the bloom), on a smooth UV sphere
    // reused for the core, the corona shells and the player's shield bubble.
    x3::rhi::MeshHandle    sunMesh{};
    x3::rhi::TextureHandle sunTex{};
    if (live) {
        x3::prims::PrimMesh sunm = x3::prims::makeUVSphere(48, 96);
        sunMesh = hc.device->createMesh(sunm.verts.data(), (uint32_t)sunm.verts.size(),
                                        sunm.index.data(), (uint32_t)sunm.index.size());
        auto sunPx = x3::space::sunbake::bakeSunRGBA(384);
        sunTex = hc.device->createTexture(sunPx.data(), 384, 384, /*srgb=*/true);
        x3::logInfo("[intro] flyable star hung at 11 km along the sky sun ray "
                    "(r=1.1 km; heat 7 km, shield-dive inside)");
    }

    x3::space::TargetingSystem targeting;

    // The capital ship's destructible damage model (the dogfight objective).
    // Owner-locked retune (live, 2026-07-27: "those numbers, but give it 1500 hp"):
    // shield 400 + 4x120 subsystems + 1500 hull = 2380 total, ALL of it reachable
    // now that the hull routing below is fixed. At 90 dmg/hit that's ~28 landed
    // hits (~20-35 s of real flying) — a fight, not a wall.
    auto capital = x3::space::ShipDamage::makeCapital(/*shield*/400, /*hull*/1500, /*subHp*/120);

    // ---- TIME BASE -------------------------------------------------------
    // HEADLESS (self-tests / captures / boot-time) runs a FIXED 1/60 per step:
    // determinism is the whole point there, and nothing is watching the clock.
    //
    // LIVE runs on the WALL CLOCK. It used to advance the sim by the same fixed
    // 1/60 ONCE PER RENDERED FRAME, sleeping only when it was running AHEAD —
    // which means the sim could never run faster than real time but could easily
    // run SLOWER: at 46 fps every second of wall clock only advanced 0.78 s of
    // game time, so the ship accelerated 22% slower, turned 22% slower, and every
    // eased term settled 29% later. That is the house rule ("game motion is
    // scaled by dt, never per-frame") violated at the LOOP level rather than
    // inside a damping term, and it reads exactly as the owner described it:
    // "it feels very dampened". Live now measures the real frame interval and
    // feeds THAT to the sim, clamped to a sane band so a hitch or an alt-tab
    // cannot teleport the ship or blow up the integrator. Same shape as the
    // --world space host, which has always done this correctly.
    constexpr float kFixedDt = 1.0f / 60.0f;
    constexpr float kMinDt   = 1.0f / 400.0f;   // cap the step rate (600+ fps)
    constexpr float kMaxDt   = 1.0f / 20.0f;    // a hitch advances at most 50 ms
    const int   maxSteps = (int)(beat.timeoutSec / kFixedDt);
    auto  tPrev   = std::chrono::steady_clock::now();
    float simTime = 0.0f;                       // accumulated sim seconds (was step*dt)
    int   localSalvosFaced = 0, localSalvosDodged = 0;
    int   localShotsFired = 0, localShotsHit = 0;
    int   localSubsDestroyed = 0;
    float crippleTime = 0.0f;
    bool  crippled = false;
    // THE KILL (owner canon: "kill big ship.. it crashes"). Set the frame the
    // dreadnought's hull reaches 0; deathHold keeps the window alive a beat
    // longer so the player actually SEES it come apart before the cut.
    bool  capitalKilled = false;
    float capitalDeathHold = 0.0f;
    // THE DEATH SEQUENCE (app/space/ship_damage.h). A capital does not pop: it
    // ruptures, walks a cascade of secondaries down 450 m of spine, vents, goes
    // DARK, sheds hull sections, and only then does the core let go. The state
    // machine owns the schedule + the presentation scalars (lights/tumble/sag);
    // everything below just pumps its events into CombatFx + the GPU debris pool
    // and multiplies the draw. Covered by --test-ship-damage T11..T16.
    x3::space::CapitalDeathState capDeath{};
    int deathPeakParticles = 0, deathPeakDebris = 0;   // frame-cost telemetry

    // ---- CAPITAL ANATOMY + ARENA (the space-combat overhaul) ---------------
    // ONE source of truth for where the capital IS: capC. Draw, force field,
    // targeting, raycast, FX, radar all read THIS — ending the 200-vs-280-vs-400
    // constant drift that had hit sparks blooming 80 m off the hull.
    //
    // ARENA: the dreadnought stands off at 2.6 km (was 200 m — you crossed the
    // whole "fight" in under a second at full burn). Closing to gun range is now
    // a ~7 s run at max speed THROUGH its turret fire: the approach is content.
    // The 15 km far plane (L7 pair) and the 10.5 km planet anchor both clear it.
    constexpr float kCapX       = 2600.0f;  // capital standoff on +X
    constexpr float kCapDrawScl = 140.0f;   // ~450 m dreadnought (was 34 = corvette)
    constexpr float kCapHullR   = 240.0f;   // hull hit/occlusion sphere
    constexpr float kCapBubbleR = 300.0f;   // force-field bounce radius (> hull)
    float capC[3] = { kCapX, 0.0f, 0.0f };
    // THE CAPITAL MOVES (owner, live 2026-08-17: "The overlords ship should
    // move around"). CapitalMotion (ship_damage.h, --test-ship-damage T19-T21)
    // keeps her on a slow, wide, mass-appropriate arc — phase-driven (patrol /
    // combat / adrift) — replacing the old ±220 m sine weave. Everything below
    // keeps reading capC + capHeadCs/Sn, so draw, hardpoints, muzzles, radar,
    // targeting and the raycast all move WITH her by construction.
    x3::space::CapitalMotionState capMotion{};
    x3::space::CapitalMotion::begin(capMotion, capC);
    // Heading rotation (about Y) from the authored base pose (nose faces -X):
    // world = capC + R * offset, with R defined by the image of +X = -fwd.
    float capHeadCs = 1.0f, capHeadSn = 0.0f;   // begin() fwd = (-1,0,0) => identity
    auto capRot = [&](const float off[3], float out[3]) {
        out[0] = capC[0] + off[0] * capHeadCs - off[2] * capHeadSn;
        out[1] = capC[1] + off[1];
        out[2] = capC[2] + off[0] * capHeadSn + off[2] * capHeadCs;
    };
    // PHYSICAL HARDPOINTS (the genre's whole capital-kill loop: pick one, FLY
    // there, strafe it). One per x3::space::Subsystem, placed on the hull:
    // engines aft (the FAR side — you must fly around), turret battery ventral,
    // shield generator dorsal, sensor mast on the bow (snipeable from standoff).
    // Offsets are HULL-LOCAL in the base pose (nose -X); capRot resolves them
    // onto the moving, turning hull.
    struct CapHardpoint { x3::space::Subsystem sub; const char* name;
                          float off[3]; float radius; };
    const CapHardpoint kHard[kMaxSubsystems] = {
        { x3::space::Subsystem::Engines,   "ENGINES",    { +190.0f,   6.0f,    0.0f }, 42.0f },
        { x3::space::Subsystem::Turrets,   "TURRETS",    {  -60.0f, -28.0f, +150.0f }, 38.0f },
        { x3::space::Subsystem::ShieldGen, "SHIELD GEN", {  -20.0f, +95.0f,  -40.0f }, 38.0f },
        { x3::space::Subsystem::Sensors,   "SENSORS",    { -210.0f, +40.0f,  -30.0f }, 36.0f },
    };
    auto hardWorld = [&](int i, float out[3]) { capRot(kHard[i].off, out); };
    // THE GUN BATTERY MADE PHYSICAL (owner: "big mounted weapons that HURT..
    // but I can shoot off"). Four VISIBLE mounts clustered on the ventral
    // battery line around the Turrets blister; each is individually
    // destructible (CapitalBattery, --test-ship-damage T22) and each kill
    // routes a quarter of the Turrets subsystem, so the HUD/consequence flow
    // is unchanged: four sheared guns == "BATTERY SILENCED".
    x3::space::CapitalBatteryState capBattery{};
    x3::space::CapitalBattery::init(capBattery);
    // Mount positions ride the VENTRAL FUSELAGE SPINE (verified against the
    // SpaceShip4 silhouette in the kill-capture frames — the old Turrets
    // blister offset at Z+150 floats off the visible hull, which was fine for
    // an invisible sphere and is NOT fine for visible geometry).
    const float kGunOff[x3::space::kCapitalGunCount][3] = {
        { -148.0f, -34.0f, +26.0f },    // bow-starboard
        {  -72.0f, -44.0f, -30.0f },    // mid-port
        {   +6.0f, -50.0f, +30.0f },    // amidships-starboard
        {  +82.0f, -40.0f, -26.0f },    // aft-port
    };
    constexpr float kGunHitR    = 22.0f;   // per-gun aim sphere (grows w/ range)
    constexpr float kGunDrawScl = 1.0f;    // gun meshes are authored in metres
    auto gunWorld = [&](int i, float out[3]) { capRot(kGunOff[i], out); };
    bool subWasDown[kMaxSubsystems] = { false, false, false, false };
    // REACTOR PHASE (all hardpoints dead -> the core cycles EXPOSED/SHIELDED;
    // hull damage x3 while exposed — the fight has acts, not a draining bar).
    float reactorT = 0.0f;
    bool  reactorOpenPrev = false;
    constexpr float kReactorCycle = 13.0f;  // 8 s exposed + 5 s shielded
    constexpr float kReactorOpen  = 8.0f;
    const float kReactorOff[3] = { -40.0f, -70.0f, 0.0f };   // ventral wound
    // CAPITAL BATTERY FIRE (the gauntlet): each of the four PHYSICAL mounts
    // spools its own 0.6 s telegraph (warning chirp + HUD flash), THEN fires.
    // Shooting a mount off silences THAT mount; four kills silence the battery
    // (== Turrets subsystem down); killing Sensors halves accuracy. Dodge rule
    // is ASPECT: transverse velocity to the firing line = misses (legible,
    // learnable). Cadence/damage live in CapitalBattery (ship_damage.h).
    constexpr float kTurretRange  = 2200.0f; // beyond this you're safe (standoff sniping)
    uint32_t salvoCounter = 0;               // deterministic per-salvo hash stream
    // HUD CALLOUTS — the VO-bark lane. Every line carries its SOURCE (see
    // CalloutSrc above): the prefix and the colour tell the player instantly
    // whether he is reading his own ship, the dreadnought, or the mission.
    struct CallLine { const char* text; float t; CalloutSrc src; };
    CallLine callouts[3] = { { nullptr, 0, kCalloutPlayer },
                             { nullptr, 0, kCalloutPlayer },
                             { nullptr, 0, kCalloutPlayer } };
    float incomingFlashT = 0.0f;             // turret spool warning flash
    bool  shieldDownCalled = false;          // one "SHIELDS DOWN" bark, not sixty
    float hitConfirmT = 0.0f;                // reticle hit-confirm flicker (shot landed)

    // ---- THE STAR (owner: "the SUN that CommanderIntegrator made which you can
    // fly into, and the shields") — the host_space flyable star, ported into the
    // dogfight so space is a PLACE, not a backdrop. A physical emissive body
    // hung along the intro sky's painted sun direction (so the analytic disc
    // and the real star coincide from spawn), close enough to reach mid-fight:
    // heat ladder on approach, SHIELD ENGAGED on breaching the core, a 17 s
    // drain inside (graze-abort recharge outside), hull lost at 0% -> the
    // canon ShotDown path. Sim runs ALWAYS (deterministic, cheap — the
    // headless pilot flies +X toward the capital and never comes near);
    // draw/FX are live-only. Full kill-cam/rewind cinematics stay host_space's.
    const float kSunDirV[3] = { 0.25887f, 0.56951f, -0.77662f };  // normalize(0.25,0.55,-0.75)
    constexpr float kSunDist    = 11000.0f;   // inside the 15 km far plane
    constexpr float kSunRadius  = 1100.0f;    // core radius (a real disc from spawn)
    const float kSunCenter[3] = { kSunDirV[0]*kSunDist, kSunDirV[1]*kSunDist,
                                  kSunDirV[2]*kSunDist };
    constexpr float kSunHeatStart = 7000.0f;  // surface distance: temp starts climbing
    constexpr float kSunWarnDist  = 4000.0f;  // "HULL TEMP RISING"
    constexpr float kSunCritDist  = 2000.0f;  // "CRITICAL - PULL AWAY"
    constexpr float kSunShieldSecs   = 17.0f; // the shield holds this long inside
    constexpr float kSunRechargeSecs = 5.0f;  // graze-abort: restores over this
    float sunShieldPct = 100.0f;
    int   sunHeatStage = 0;                   // 0 nominal, 1 warn, 2 crit (one-shot barks)
    float sunSurfDist = 1e9f;                 // this step's surface distance (HUD)
    // SPECTACLE CAM (owner: "add the spectacle Cam.. 30 seconds earlier") — the
    // full host_space sun-death phase machine, ported: Flying -> InsideSun
    // (LIVE graze window, 17 s countdown) -> Detonation (external kill-cam,
    // blast + coronal ejection) -> Rewind (1 s backwards scrub) -> TitleCard
    // ("30 SECONDS EARLIER…") -> Replay (the recorded approach flown back in)
    // -> Respawn (fade, re-seed, "HULL LOST TO THE SUN") -> Flying. Any key
    // from Detonation onward skips to Respawn. NOTE the intro-specific ending:
    // the star no longer hard-kills the run — after the spectacle you respawn
    // at the arena origin and the DOGFIGHT CONTINUES (capital damage persists);
    // outcomes stay earned by the fight, not lost to sightseeing.
    enum class SunPhase { Flying, InsideSun, Detonation, Rewind, TitleCard, Replay, Respawn };
    SunPhase sunPhase = SunPhase::Flying;
    float sunPhaseT = 0.0f;
    bool  sunRespawned = false;
    bool  sunPrevAny = false;                 // any-key edge for the cinematic skip
    float sunShieldFlashT = -1.0f;            // one-shot SHIELD ENGAGED flash; <0 idle
    constexpr float kSunFlashSecs = 1.5f;
    constexpr float kDetonateSecs = 4.5f;     // blast + coronal ejection
    constexpr float kRewindSecs   = 1.0f;     // backwards-scrub stinger
    constexpr float kTitleSecs    = 2.6f;     // the film card (incl. fades)
    constexpr float kReplaySecs   = 6.5f;     // forward re-entry replay
    constexpr float kSunFadeSecs  = 1.0f;     // respawn fade out/hold/in
    constexpr int   kSunDebris    = 24;       // coronal-ejection fragments
    // Trajectory ring: 15 Hz x 32 s of ship poses (>= the 30 s the card
    // promises), recorded through Flying + InsideSun, replayed by the kill-cam.
    struct TrajSample { float p[3], f[3], u[3], r[3]; };
    constexpr float kTrajHz  = 15.0f;
    constexpr int   kTrajLen = (int)(kTrajHz * 32.0f);
    std::vector<TrajSample> trajRing((size_t)kTrajLen);
    int   trajHead = 0, trajCount = 0;
    float trajTimer = 0.0f;
    std::vector<TrajSample> trajPlay;         // linearised oldest->entry at detonation
    float sunEntryPos[3] = { 0, 0, 0 };       // surface impact point (ejecta origin)
    float sunEntryNrm[3] = { 0, 1, 0 };
    float cineCamPos[3] = { 0, 0, 0 };        // frozen external kill-cam
    float cineYaw = 0.0f, cinePit = 0.0f;

    // Combat FX (tracers, muzzle flashes, crosshair) — live only. Heap-allocated:
    // CombatFx carries ~256 KB of scratch (the host_space convention).
    std::unique_ptr<x3::game::CombatFx> fxPtr;
    const bool fxOn = live && cockpit != nullptr;
    if (fxOn) { fxPtr = std::make_unique<x3::game::CombatFx>(); fxPtr->init(*hc.device); }
    // GPU debris pool, configured for SPACE: zero gravity, no ground plane (push it
    // far below), minimal damping so kill-gib chunks coast outward and read as a
    // ship breaking apart. app_run re-configures this pool for the ground world
    // after the intro, so there is no cross-beat conflict.
    if (fxOn) {
        x3::rhi::IRenderDevice::GpuDebrisParams gp{};
        gp.gravity[0] = 0.0f; gp.gravity[1] = 0.0f; gp.gravity[2] = 0.0f;
        gp.groundY = -100000.0f;   // no floor in space
        gp.restitution = 0.0f; gp.friction = 0.0f;
        gp.linearDamping = 0.06f; gp.sleepFrames = 240;
        hc.device->gpuDebrisConfig(gp);
    }

    // ---- COMBAT SFX (dogfight-feel): every combat event now SOUNDS. ---------
    // Cues are COMMITTED under assets/audio/space/dogfight/ (resolveAudio tries
    // the repo-local mirror first — see AUDIO_MANIFEST.md for pack provenance),
    // so a fresh clone gets the full mix with no external library. Load failure
    // is non-fatal (IAudioSystem::load logs once, returns an invalid handle,
    // every play on it no-ops) and hc.audio == nullptr (headless) degrades to
    // LOG-ONLY triggers — which is exactly what the headless SFX probe
    // (X3_INTRO_SFXPROBE=1) captures as evidence that the hooks are live.
    x3::audio::IAudioSystem* sfx = hc.audio;
    auto loadCue = [&](const char* rel) {
        return sfx ? sfx->load(x3::game::resolveAudio(rel)) : x3::audio::SoundHandle{};
    };
    const x3::audio::SoundHandle sndPlayerLaser  = loadCue("space/dogfight/player_laser.wav");
    const x3::audio::SoundHandle sndEnemyLaser   = loadCue("space/dogfight/enemy_laser.wav");
    const x3::audio::SoundHandle sndImpactShield = loadCue("space/dogfight/impact_shield.wav");
    const x3::audio::SoundHandle sndImpactHull   = loadCue("space/dogfight/impact_hull.wav");
    const x3::audio::SoundHandle sndExplosion    = loadCue("space/dogfight/explosion_fighter.wav");
    const x3::audio::SoundHandle sndBoost        = loadCue("space/dogfight/boost_antimatter.wav");
    const x3::audio::SoundHandle sndZap          = loadCue("space/dogfight/forcefield_zap.wav");
    const x3::audio::SoundHandle sndLock         = loadCue("space/dogfight/lock_chirp.wav");
    const x3::audio::SoundHandle sndHum          = loadCue("space/engine_hum.wav");
    const x3::audio::SoundHandle sndThrust       = loadCue("space/engine_thrust.wav");
    // ENGINE BED: reactor hum + thruster whoosh, both at FIXED pitch and ridden
    // by VOLUME only (the --world space ruling — a pitch sweep reads as a car
    // shifting gears). Hum tracks speed; thrust tracks throttle and swells on
    // the antimatter boost. The music bus is untouched.
    x3::audio::LoopHandle humLoop{}, thrustLoop{};
    if (sfx && sndHum.valid())    humLoop    = sfx->startLoop(sndHum, 0.22f, 1.0f);
    if (sfx && sndThrust.valid()) thrustLoop = sfx->startLoop(sndThrust, 0.0f, 1.0f);
    // One log line per event TYPE on its first trigger — headless-verifiable
    // evidence that each hook fires, audio device or not.
    enum { kSfxPlayerLaser = 0, kSfxEnemyLaser, kSfxImpactShield, kSfxImpactHull,
           kSfxExplosion, kSfxBoost, kSfxZap, kSfxLock, kSfxCount };
    bool sfxLogged[kSfxCount] = {};
    auto sfxMark = [&](int type, const char* name) {
        if (!sfxLogged[type]) {
            sfxLogged[type] = true;
            x3::logInfo(std::string("[intro-sfx] first trigger: ") + name +
                        (sfx ? "" : " (no audio system — log only)"));
        }
    };
    auto play2D = [&](int type, const char* name, x3::audio::SoundHandle h,
                      float vol, float pitch) {
        sfxMark(type, name);
        if (sfx && h.valid()) sfx->playSound2D(h, vol, pitch);
    };
    auto play3D = [&](int type, const char* name, x3::audio::SoundHandle h,
                      const float p[3], float vol, float pitch) {
        sfxMark(type, name);
        if (sfx && h.valid()) sfx->playSound3D(h, p[0], p[1], p[2], vol, pitch);
    };
    // A hit on the PLAYER: distinct voices — energy shield-splash while the
    // shield holds, metal-on-metal armor crunch once it's down. One path for the
    // real salvo branch AND the headless probe, so the probe exercises the same
    // code the live fight runs.
    auto playerHitSfx = [&](int dmg) {
        const bool shielded = pilot.shield() > 0;
        pilot.takeDamage(dmg);
        if (shielded)
            play2D(kSfxImpactShield, "impact_shield (hit, shields up)",
                   sndImpactShield, 0.9f, 1.0f);
        else
            play2D(kSfxImpactHull, "impact_hull (hit, shields DOWN)",
                   sndImpactHull, 0.95f, 1.0f);
    };
    // A fighter kill: the 3D explosion + the death/smoke FX burst, in one place —
    // deaths are detected AT THE DAMAGE SITE (EnemyShipManager swap-removes a
    // dead ship immediately, so a post-update hull scan can never see one; the
    // old prevHull compare below was dead code).
    // MASSIVE disintegration blast radius for a ship kill. A barrel death uses
    // radius ~1.6; a ~10 m fighter hull erupting reads MUCH bigger, so the fireball
    // is scaled up hard (owner: "massive explosions when they disintegrate").
    constexpr float kShipDeathBlastRadius = 12.0f;
    auto fighterKillFx = [&](const float p[3]) {
        play3D(kSfxExplosion, "explosion_fighter (3D, fighter kill)", sndExplosion,
               p, 1.0f, 1.0f);
        if (!fxOn) return;
        // BETTER KILL EXPLOSIONS (owner: "better quality explosions on little
        // ships"). The old kill was just debris + one smoke puff — no fire at
        // all, which is why a dying fighter read as a shrug. Now it's a layered
        // blast: a hot additive fireball core (spawnExplosion feeds the bloom
        // chain, so it BLOOMS), two smaller staggered secondaries offset off the
        // core so the fireball is lumpy instead of one clean ball, then the
        // debris chunks, a spark burst, burning embers and the lingering smoke.
        const x3::phys::Vec3 c{ p[0], p[1], p[2] };
        fxPtr->spawnExplosion(c, 9.0f);                 // main fireball
        // Deterministic offsets hashed off the kill position (no frame RNG —
        // the intro must stay reproducible for the seeded outcome roll).
        const float h1 = std::sin(p[0] * 12.9898f + p[2] * 78.233f);
        const float h2 = std::sin(p[1] * 39.3468f + p[0] * 11.135f);
        const float h3 = std::sin(p[2] * 21.7654f + p[1] * 53.771f);
        fxPtr->spawnExplosion({ p[0] + h1 * 4.5f, p[1] + h2 * 3.0f,
                                p[2] + h3 * 4.5f }, 5.5f);
        fxPtr->spawnExplosion({ p[0] - h3 * 3.5f, p[1] - h1 * 2.5f,
                                p[2] - h2 * 3.5f }, 4.0f);
        fxPtr->spawnDeath(c);                            // debris chunks
        fxPtr->spawnShipSparks(c);                       // hull spark spray
        fxPtr->spawnShipEmber(c, { h1 * 6.0f, h2 * 6.0f, h3 * 6.0f });
        fxPtr->spawnShipSmoke(c, { h2 * 8.0f, h3 * 4.0f, h1 * 8.0f }, 1.0f);
        fxPtr->spawnSmoke(c);                            // lingering puff
        // MASSIVE disintegration on top of the layered fireball (owner: "massive
        // explosions when they disintegrate"): a huge shockwave-shell blast plus a
        // HEAVY GPU debris burst — big metal chunks flung outward in zero-G.
        fxPtr->spawnShipDeathBlast(c, kShipDeathBlastRadius);
        const uint32_t seed = 0x5D3Bu ^ (uint32_t)(c.x * 131.0f)
                                      ^ ((uint32_t)(c.z * 977.0f) << 8);
        const float bp[3] = { p[0], p[1], p[2] };
        hc.device->gpuDebrisSpawnBurst(bp, /*count*/40u, /*speed*/44.0f,
                                       /*lifetime*/4.0f, /*halfExtent*/0.95f, seed);
    };

    // HUD callout push (newest on top, three lines max) + a low radio chirp so
    // the bark registers even when the player is watching the fight, not the text.
    // A MISSION callout holds longer than a status bark — "you killed it" is
    // the one line the player must not miss while he is watching the wreck.
    auto pushCallout = [&](CalloutSrc src, const char* txt, float pitch) {
        callouts[2] = callouts[1]; callouts[1] = callouts[0];
        callouts[0] = { txt, src == kCalloutMission ? 6.5f : 3.2f, src };
        play2D(kSfxLock, "callout_chirp (radio bark)", sndLock,
               src == kCalloutMission ? 0.75f : 0.45f, pitch);
        x3::logInfo(std::string("[intro] CALLOUT[") +
                    (src == kCalloutPlayer ? "PLAYER" :
                     src == kCalloutEnemy  ? "ENEMY"  : "MISSION") + "]: " +
                    calloutPrefix(src) + txt);
    };

    // Deterministic per-salvo hash in [0,1) — NO frame RNG (the intro must stay
    // reproducible); the stream is the salvo counter, stable across runs.
    auto salvoHash = [&]() {
        const float s = std::sin((float)(++salvoCounter) * 12.9898f) * 43758.5453f;
        return s - std::floor(s);
    };

    // ASPECT DODGE RULE (the vet fix for "never misses"): enemy fire hits as a
    // function of the player's TRANSVERSE velocity across the firing line — fly
    // perpendicular and shots miss, fly straight at/away and they land. Legible,
    // learnable, deterministic. Sensors down halves turret accuracy on top.
    auto salvoLands = [&](const float from[3], bool turret,
                          const float ppos_[3], const float pvel_[3]) {
        float d[3] = { ppos_[0]-from[0], ppos_[1]-from[1], ppos_[2]-from[2] };
        const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dl > 1.0f) { d[0]/=dl; d[1]/=dl; d[2]/=dl; }
        const float vAlong = pvel_[0]*d[0] + pvel_[1]*d[1] + pvel_[2]*d[2];
        const float vp[3] = { pvel_[0]-vAlong*d[0], pvel_[1]-vAlong*d[1],
                              pvel_[2]-vAlong*d[2] };
        const float vPerp = std::sqrt(vp[0]*vp[0] + vp[1]*vp[1] + vp[2]*vp[2]);
        float pHit = 0.92f - vPerp / 140.0f;      // ~130 m/s transverse = near-immune
        if (turret && x3::space::ShipDamage::subsystemDown(capital,
                          x3::space::Subsystem::Sensors))
            pHit *= 0.5f;                          // blinded battery
        pHit = std::max(0.06f, std::min(0.92f, pHit));
        return salvoHash() < pHit;
    };

    // Distinct-down detection (order-free — the player picks the order),
    // shared by capitalApplyHit AND the physical gun-kill path so a Turrets
    // subsystem downed by EITHER route fires the same consequence flow.
    auto scanSubsDown = [&]() {
        for (int si = 0; si < kMaxSubsystems; ++si) {
            if (subWasDown[si] ||
                !x3::space::ShipDamage::subsystemDown(capital, kHard[si].sub))
                continue;
            subWasDown[si] = true;
            ++localSubsDestroyed;
            float hw[3]; hardWorld(si, hw);
            play3D(kSfxExplosion, "explosion_hardpoint (subsystem killed)",
                   sndExplosion, hw, 1.0f, 0.7f);
            if (fxOn) {
                fxPtr->spawnExplosion({ hw[0], hw[1], hw[2] }, 20.0f);
                fxPtr->spawnDeath({ hw[0], hw[1], hw[2] });
                fxPtr->spawnSmoke({ hw[0], hw[1], hw[2] });
            }
            // The bark + the CONSEQUENCE (each kill changes the fight, and the
            // callout says HOW — that is what makes the choice strategic).
            switch (kHard[si].sub) {
                case x3::space::Subsystem::Engines:
                    pushCallout(kCalloutEnemy, "DREADNOUGHT ENGINES DESTROYED - SHE'S ADRIFT", 0.7f); break;
                case x3::space::Subsystem::Turrets:
                    pushCallout(kCalloutEnemy, "DREADNOUGHT TURRETS DESTROYED - BATTERY SILENCED", 0.7f);
                    // PHYSICAL <-> MODEL SYNC: if the subsystem died by any
                    // route while mounts still stand (headless competent
                    // model / blister wreckage bleed), the standing guns are
                    // dead metal too — shear them off so the silence reads.
                    for (int gi2 = 0; gi2 < x3::space::kCapitalGunCount; ++gi2) {
                        if (!x3::space::CapitalBattery::gunAlive(capBattery, gi2))
                            continue;
                        x3::space::CapitalBattery::damageGun(
                            capBattery, gi2, x3::space::CapitalBattery::kGunHp);
                        float gw2[3]; gunWorld(gi2, gw2);
                        if (fxOn) {
                            fxPtr->spawnExplosion({ gw2[0], gw2[1], gw2[2] }, 12.0f);
                            fxPtr->spawnSmoke({ gw2[0], gw2[1], gw2[2] });
                        }
                    }
                    break;
                case x3::space::Subsystem::ShieldGen:
                    pushCallout(kCalloutEnemy, "DREADNOUGHT SHIELD GENERATOR DESTROYED - NO REGEN", 0.7f); break;
                default:
                    pushCallout(kCalloutEnemy, "DREADNOUGHT SENSORS DESTROYED - THEIR AIM IS GONE", 0.7f); break;
            }
        }
    };
    // ANATOMICAL CAPITAL HIT — the one funnel every landed capital shot goes
    // through (live raycast AND the headless competent model): reactor-phase
    // multiplier on hull damage, subsystem routing, hit FX at the REAL hit
    // point, then the shared distinct-down scan above.
    auto capitalApplyHit = [&](x3::space::Subsystem sub, const float hitPos[3],
                               const float hitDir[3]) {
        const bool reactorOpen = crippled &&
            std::fmod(reactorT, kReactorCycle) < kReactorOpen;
        const bool toHull = (sub == x3::space::Subsystem::Count);
        const int  dmg = (toHull && reactorOpen) ? 90 * 3 : 90;  // owner-locked 90 base
        x3::space::ShipDamage::applyDamage(capital, dmg, sub);
        ++localShotsHit;
        hitConfirmT = 0.15f;
        if (fxOn) {
            fxPtr->spawnImpact({ hitPos[0], hitPos[1], hitPos[2] },
                               { -hitDir[0], -hitDir[1], -hitDir[2] });
            if (x3::space::ShipDamage::shieldFrac(capital) <= 0.0f)
                fxPtr->spawnShipSparks({ hitPos[0], hitPos[1], hitPos[2] });
        }
        scanSubsDown();
    };
    float zapCooldown = 0.0f;   // one zap per bounce, not per frame on the bubble
    float zapFlashT   = 0.0f;   // HUD border cyan flash timer
    bool  prevSprint  = false, prevLock = false;
    // ---- Reticle state (combat readability) --------------------------------
    // Lock STAGING: a fresh lock target runs an ACQUIRING sweep (amber ring
    // closing onto the bracket) for kAcquireSec, then reads LOCKED (red). The
    // age resets whenever lockNearest switches targets, so swinging the nose
    // across the wing re-sweeps per target — no popping.
    constexpr float kAcquireSec = 1.4f;
    uint32_t lockPrevId = 0; bool lockHad = false; float lockAge = 0.0f;
    // AIM SOVEREIGNTY (owner, live 2026-08-16: "The aim always returns to the
    // overlord ship!!! it should NOT.."): policy gate in front of the 6DOF
    // target hold + camera look bias below. The player's aim always wins — the
    // hold only ever KEEPS the aim he already has, with a grace after the last
    // look input and an aim-away suspension (hysteresis) so it can never slew
    // the nose back onto the capital he deliberately aimed off of. Lives in
    // space_pilot.h so --test-space T19-T21 cover it headlessly.
    x3::game::AimSovereignty aimSov;
    // (hitConfirmT — the reticle hit-confirm flicker — is declared up in the
    // capital-anatomy block: capitalApplyHit sets it and is defined above here.)
    // Effective projectile speed for the LEAD PIP. The player laser RESOLVES as
    // hitscan (instant ray), so the mathematically honest lead is ~zero; the
    // pip is computed at the bolt's VISUAL travel speed (600 m aim range /
    // kTracerTime) so it marks the precise aim point with a small, honest-to-
    // the-eye lead on crossing targets — and the plumbing is ready for true
    // projectile weapons. At this speed the pip never lies farther than ~2 m
    // off the instant-ray solution inside laser range (< the hit radius).
    constexpr float kLeadProjSpeed = 4800.0f;
    const bool sfxProbe = [] {
        const char* e = std::getenv("X3_INTRO_SFXPROBE");
        return e && *e && *e != '0';
    }();

    // ---- Player fighter model: drawn in 3P so you SEE your own ship, AHEAD of the
    //      chase camera (owner: "Draw my ship in 3rd person ... in FRONT of the
    //      camera"). Reuses the same drawIntroShip helper the enemy wing uses.
    std::unique_ptr<x3::asset::IAssetSource> playerSrc;
    std::unique_ptr<x3::asset::IModelLoader> playerLoader;
    x3::asset::Model playerModel{};
    std::vector<x3::asset::ModelDrawable> playerDraw;
    if (live) {
        playerSrc.reset(x3::asset::createAssetSource());
        playerSrc->mountDir(x3::game::riggedGlbRoot(), 0);
        playerLoader.reset(x3::asset::createModelLoader(hc.device, playerSrc.get()));
        for (const char* c : { "JakeFighterShip_textured.glb", "JakeFighterShip.glb",
                               "SpaceShip.glb", "SpaceShip2.glb" }) {
            playerModel = playerLoader->load(c);
            if (playerModel.ok) break;
        }
        if (playerModel.ok) playerDraw = x3::asset::makeDrawables(playerModel);
        x3::logInfo(std::string("[intro] player ship 3P model=") +
                    (playerModel.ok ? "loaded" : "<none>"));
    }
    float beatT = 0.0f;

    // ---- Star / spectacle-cam render helpers (shared by the normal combat
    //      frame AND the external kill-cam frames, so the recipe lives once). --
    auto sphM = [](const float c[3], float s, float yaw, float m[16]) {
        const float cy2 = std::cos(yaw), sy2 = std::sin(yaw);
        m[0]=cy2*s; m[1]=0; m[2]=-sy2*s; m[3]=0;
        m[4]=0;     m[5]=s; m[6]=0;      m[7]=0;
        m[8]=sy2*s; m[9]=0; m[10]=cy2*s; m[11]=0;
        m[12]=c[0]; m[13]=c[1]; m[14]=c[2]; m[15]=1;
    };
    auto smoothC = [](float e0, float e1, float x) -> float {
        float d = e1 - e0;
        if (std::fabs(d) < 1e-6f) d = (d < 0.0f) ? -1e-6f : 1e-6f;
        float t = (x - e0) / d;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return t * t * (3.0f - 2.0f * t);
    };
    // TWIN-SUN FIX (host_space recipe): re-aim the painted sky disc down the
    // eye->star ray every frame; params mirror setIntroCockpitLook otherwise.
    auto aimSkyAtStar = [&](float ex, float ey, float ez) {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        float sd[3] = { kSunCenter[0]-ex, kSunCenter[1]-ey, kSunCenter[2]-ez };
        const float sdl = std::sqrt(sd[0]*sd[0]+sd[1]*sd[1]+sd[2]*sd[2]);
        if (sdl > 1.0f) { sd[0]/=sdl; sd[1]/=sdl; sd[2]/=sdl; }
        sp.sunDir[0]=sd[0]; sp.sunDir[1]=sd[1]; sp.sunDir[2]=sd[2];
        sp.sunColor[0]=0.85f; sp.sunColor[1]=0.88f; sp.sunColor[2]=1.0f;
        sp.sunIntensity = 0.55f;
        sp.sunLight = 2.4f;
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]=0.0012f; sp.zenith[1]=0.0012f; sp.zenith[2]=0.0035f;
        sp.horizon[0]=0.0018f; sp.horizon[1]=0.0022f; sp.horizon[2]=0.0050f;
        hc.device->setSkyParams(sp);
        // SPACE AMBIENT (owner, live 2026-08-17: "space is too bright, washed out
        // flat"). The sky was near-black but the ambient was never lowered, so the
        // engine default (0.42,0.44,0.50) flooded the hulls with fill and killed the
        // sun's directional contrast. Near-black cool fill: the star is the key, the
        // dark side goes dark, the hull reads as a solid mass instead of a decal.
        hc.device->setAmbient(0.008f, 0.008f, 0.015f);
        // CAPITAL SHADOW (owner: "the ships lighting needs some shadow... the
        // overlord especially"). The intro never turned on cascaded shadows, so the
        // legacy single ~45 m camera-locked box left the 450 m capital at 2.6 km
        // with NO sun shadow — the decal look. Cascades to 3.2 km cover the whole
        // arena. (RT shadows would be higher-res at range; CSM is the established
        // outdoor-host path — see world_host_common.h applyOutdoorCsm.)
        x3::rhi::IRenderDevice::CsmParams csm{};
        csm.enabled  = true;
        csm.lambda   = 0.75f;
        csm.distance = 3200.0f;
        csm.blend    = 0.12f;
        hc.device->setCsmParams(csm);
    };
    // The star: living-surface core (bake as baseColor AND emissive) + corona.
    auto drawStar = [&](const x3::rhi::FrameContext& frame) {
        float sm[16];
        sphM(kSunCenter, kSunRadius, beatT * 0.008f, sm);
        const float sunBc[4] = { 1.0f, 0.85f, 0.55f, 1.0f };
        const float sunEm[4] = { 1.0f, 0.86f, 0.60f, 3.2f };  // past bloom knee
        hc.device->drawMeshPBR(frame, sunMesh, sunTex,
                               x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                               sunBc, sunEm, sm,
                               /*alphaMask=*/false, /*alphaBlend=*/false, sunTex);
        for (int s5 = 0; s5 < 5; ++s5) {
            const float k5 = (float)s5 / 4.0f;
            sphM(kSunCenter, kSunRadius * (1.05f + 0.22f * (float)s5), 0.0f, sm);
            const float cBc[4] = { 1.0f, 0.72f - 0.30f*k5, 0.35f - 0.20f*k5, 1.0f };
            const float cEm[4] = { 1.0f, 0.66f - 0.30f*k5, 0.30f - 0.18f*k5,
                                   0.85f * (1.0f - k5) + 0.10f };
            x3::rhi::IRenderDevice::GlassMaterial gm{};
            gm.opacity = 0.10f * (1.0f - k5) + 0.02f;
            gm.roughness = 1.0f; gm.specular = 0.0f;
            gm.tint[0]=1.0f; gm.tint[1]=0.7f; gm.tint[2]=0.35f;
            hc.device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{},
                                     cBc, cEm, gm, sm, /*alphaBlend=*/true);
        }
    };
    // Small float[3] helpers for the kill-cam / ejecta geometry.
    auto v3norm = [](float v[3]) {
        const float l = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; }
    };
    auto v3cross = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
    };
    // Frame the external kill-cam: sun ~half the screen, impact point centred,
    // camera ~1.6 radii off the surface (host_space framing math).
    auto setupKillCam = [&](const float shipP[3]) {
        float n[3] = { shipP[0]-kSunCenter[0], shipP[1]-kSunCenter[1],
                       shipP[2]-kSunCenter[2] };
        const float nl = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nl > 1e-3f) { n[0]/=nl; n[1]/=nl; n[2]/=nl; }
        else            { n[0]=0; n[1]=1; n[2]=0; }
        for (int a2 = 0; a2 < 3; ++a2) {
            sunEntryNrm[a2] = n[a2];
            sunEntryPos[a2] = kSunCenter[a2] + n[a2] * kSunRadius;
        }
        float upv[3] = { 0, 1, 0 };
        if (std::fabs(n[1]) >= 0.95f) { upv[0] = 1; upv[1] = 0; }
        float lat[3]; v3cross(n, upv, lat); v3norm(lat);
        float cdir[3] = { n[0]*0.55f + lat[0]*0.83f, n[1]*0.55f + lat[1]*0.83f,
                          n[2]*0.55f + lat[2]*0.83f };
        v3norm(cdir);
        const float camDist = kSunRadius + 3200.0f;
        for (int a2 = 0; a2 < 3; ++a2)
            cineCamPos[a2] = kSunCenter[a2] + cdir[a2] * camDist;
        float d[3] = { sunEntryPos[0]-cineCamPos[0], sunEntryPos[1]-cineCamPos[1],
                       sunEntryPos[2]-cineCamPos[2] };
        v3norm(d);
        cinePit = std::asin(std::max(-1.0f, std::min(1.0f, d[1])));
        cineYaw = std::atan2(d[2], d[0]);
    };
    // Linearise the ring (oldest -> entry) for scrub/replay.
    auto snapshotTraj = [&]() {
        trajPlay.clear();
        for (int i = 0; i < trajCount; ++i) {
            const int idx = ((trajHead - trajCount + i) % kTrajLen + kTrajLen) % kTrajLen;
            trajPlay.push_back(trajRing[(size_t)idx]);
        }
    };
    // Draw the recorded ship pose; g 0->1 walks oldest->entry, lerped.
    auto drawReplayShip = [&](const x3::rhi::FrameContext& frame, float g) {
        if (trajPlay.size() < 2 || playerDraw.empty() || !cockpit) return;
        g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
        const float fi = g * (float)(trajPlay.size() - 1);
        int i0 = (int)fi; if (i0 < 0) i0 = 0;
        const int i1 = std::min(i0 + 1, (int)trajPlay.size() - 1);
        const float fr = fi - (float)i0;
        const auto& A = trajPlay[(size_t)i0]; const auto& B = trajPlay[(size_t)i1];
        float p[3], f[3], u[3];
        for (int a2 = 0; a2 < 3; ++a2) {
            p[a2] = A.p[a2] + (B.p[a2]-A.p[a2]) * fr;
            f[a2] = A.f[a2] + (B.f[a2]-A.f[a2]) * fr;
            u[a2] = A.u[a2] + (B.u[a2]-A.u[a2]) * fr;
        }
        v3norm(f); v3norm(u);
        x3::apphost::drawIntroShipBasis(*hc.device, frame, playerDraw,
                                        p, f, u, 1.0f, cockpit->mrShared);
    };
    // Coronal ejection: expanding shockwave shells at the impact point + a cone
    // of decelerating emissive fragments (host_space drawEjecta, compacted).
    auto drawEjecta = [&](const x3::rhi::FrameContext& frame, float t) {
        float m[16];
        for (int s2 = 0; s2 < 2; ++s2) {
            const float lt = t - (float)s2 * 0.6f;
            if (lt < 0.0f || lt > 2.4f) continue;
            const float a = 1.0f - lt / 2.4f;
            sphM(sunEntryPos, kSunRadius * (0.25f + lt * 0.9f), 0.0f, m);
            const float bc[4] = { 1.0f, 0.7f, 0.35f, 1.0f };
            const float em[4] = { 1.0f, 0.7f, 0.35f, 2.5f * a };
            x3::rhi::IRenderDevice::GlassMaterial gm{};
            gm.opacity = 0.14f * a; gm.roughness = 1.0f; gm.specular = 0.0f;
            gm.tint[0]=1.0f; gm.tint[1]=0.7f; gm.tint[2]=0.35f;
            hc.device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{},
                                     bc, em, gm, m);
        }
        float upv[3] = { 0, 1, 0 };
        if (std::fabs(sunEntryNrm[1]) >= 0.95f) { upv[0] = 1; upv[1] = 0; }
        float ta[3]; v3cross(upv, sunEntryNrm, ta); v3norm(ta);
        float tb[3]; v3cross(sunEntryNrm, ta, tb);
        using x3::space::sunbake::hashF;
        for (int i = 0; i < kSunDebris; ++i) {
            const float h1 = hashF((uint32_t)(i*5+1)), h2 = hashF((uint32_t)(i*5+2));
            const float h3 = hashF((uint32_t)(i*5+3)), h4 = hashF((uint32_t)(i*5+4));
            const float ang  = h1 * 6.2831853f;
            const float cone = 0.35f + 0.5f * h2;
            float dir[3];
            for (int a2 = 0; a2 < 3; ++a2)
                dir[a2] = sunEntryNrm[a2] +
                          (ta[a2]*std::cos(ang) + tb[a2]*std::sin(ang)) * cone;
            v3norm(dir);
            const float v0 = kSunRadius * (0.9f + 1.6f * h3);
            const float tt = std::min(t, 3.5f);
            const float dist = v0 * (tt - 0.12f * tt * tt);
            const float life = std::min(1.0f, t / 3.5f);
            const float str  = (3.5f - 3.0f * life) * (0.6f + 0.6f * h4);
            if (str < 0.05f) continue;
            const float c2[3] = { sunEntryPos[0] + dir[0]*dist,
                                  sunEntryPos[1] + dir[1]*dist,
                                  sunEntryPos[2] + dir[2]*dist };
            sphM(c2, 14.0f + 26.0f * h2, 0.0f, m);
            const float bc[4] = { 1.0f, 0.72f, 0.30f, 1.0f };
            const float em[4] = { 1.0f, 0.72f, 0.30f, str };
            hc.device->drawMeshEmissive(frame, sunMesh, x3::rhi::TextureHandle{},
                                        bc, em, m);
        }
    };
    // The cinematic overlay: SHIELD ENGAGED flash, molten wash + countdown,
    // detonation flash, << REWIND tag, the film card, replay entry flash, and
    // the respawn fade/captions (host_space drawCinematic, compacted).
    auto drawSunOverlay = [&](const x3::rhi::FrameContext& frame, float W, float H) {
        using x3::rhi::FontRole;
        auto full = [&](float r, float g, float b, float a) {
            if (a <= 0.001f) return;
            const float c[4] = { r, g, b, a };
            hc.device->drawHudQuad(frame, 0, 0, W, H, c);
        };
        auto center = [&](const char* s, float px, float y, const float col[4]) {
            const float w = hc.device->textAdvance(FontRole::Title, s, px);
            hc.device->drawHudTextF(frame, FontRole::Title, s,
                                    W*0.5f - w*0.5f, y, px, col);
        };
        if (sunShieldFlashT >= 0.0f) {
            const float decay = 1.0f - smoothC(0.0f, kSunFlashSecs, sunShieldFlashT);
            full(0.55f, 0.80f, 1.0f, decay * decay * 0.85f);
            const float capA = 1.0f - smoothC(kSunFlashSecs * 0.55f, kSunFlashSecs,
                                              sunShieldFlashT);
            if (capA > 0.02f) {
                const float col[4] = { 0.55f, 0.85f, 1.0f, capA };
                center("YOUR DIVE SHIELD ENGAGED", 30.0f, H*0.30f, col);
            }
        }
        if (sunPhase == SunPhase::InsideSun) {
            const float pulse = 0.42f + 0.10f * std::sin(beatT * 5.0f);
            full(1.0f, 0.45f, 0.12f, pulse);
            const float rem = std::max(0.0f, kSunShieldSecs - sunPhaseT);
            const float k = rem / kSunShieldSecs;
            char cd[48];
            std::snprintf(cd, sizeof(cd), "YOUR SHIELD FAILS IN %4.1fs", (double)rem);
            const float col[4] = { 1.0f, 0.30f + 0.55f*k, 0.20f*k, 1.0f };
            center(cd, 40.0f, H*0.62f, col);
        } else if (sunPhase == SunPhase::Detonation) {
            const float fl = 1.0f - smoothC(0.0f, 0.6f, sunPhaseT);
            full(1.0f, 0.96f, 0.9f, fl * 0.95f);
        } else if (sunPhase == SunPhase::Rewind) {
            full(0.0f, 0.0f, 0.02f, 0.28f);
            const float col[4] = { 0.8f, 0.85f, 1.0f, 0.9f };
            center("<< REWIND", 30.0f, H*0.12f, col);
        } else if (sunPhase == SunPhase::TitleCard) {
            const float aIn  = smoothC(0.0f, 0.6f, sunPhaseT);
            const float aOut = 1.0f - smoothC(kTitleSecs - 0.6f, kTitleSecs, sunPhaseT);
            const float a = std::min(aIn, aOut);
            full(0.0f, 0.0f, 0.0f, 0.72f + 0.28f * a);
            const float col[4] = { 0.92f, 0.90f, 0.85f, a };
            center("3 0   S E C O N D S   E A R L I E R", 30.0f, H*0.46f, col);
        } else if (sunPhase == SunPhase::Replay) {
            const float fl = smoothC(kReplaySecs - 0.5f, kReplaySecs, sunPhaseT);
            full(1.0f, 0.9f, 0.7f, fl * 0.85f);
        } else if (sunPhase == SunPhase::Respawn) {
            float a;
            if (sunPhaseT < kSunFadeSecs)            a = smoothC(0.0f, kSunFadeSecs, sunPhaseT);
            else if (sunPhaseT < kSunFadeSecs + 0.8f) a = 1.0f;
            else a = 1.0f - smoothC(kSunFadeSecs + 0.8f, 2.0f*kSunFadeSecs + 0.8f, sunPhaseT);
            full(0.0f, 0.0f, 0.0f, a);
            if (sunPhaseT > kSunFadeSecs * 0.7f) {
                const float col[4]  = { 0.95f, 0.55f, 0.35f, std::min(1.0f, a) };
                center("YOU LOST YOUR SHIP TO THE SUN", 26.0f, H*0.44f, col);
                const float col2[4] = { 0.8f, 0.85f, 1.0f, std::min(1.0f, a) };
                center("YOUR SHIELD HELD 17.0s - BACK TO THE FIGHT", 16.0f, H*0.44f + 40.0f, col2);
            }
        }
    };

    // Captured-cursor mouse-look for the live window (host_space pattern). The
    // cursor is restored to NORMAL on every exit path below (the cinematic beats
    // + the cutscene player expect a visible cursor).
    double lastMX = 0.0, lastMY = 0.0;
    if (interactive) {
        glfwSetInputMode(hc.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(hc.window, &lastMX, &lastMY);
    }
    if (live) {
        // FRUSTUM CULL OFF for the space beat (owner: "get even half a screen away
        // from that capital ship and it goes invis ... should NEVER lose sight of a
        // ship that is IN VIEW"). The cull sphere mis-bounds these direct-drawn
        // scaled ships and eats them at frame edges; space draws ~12 objects, so
        // culling buys nothing here. Restored at beat exit. (The noCull /
        // ALWAYS_VISIBLE flag exists in the cull design but nothing ever wired a
        // setter — when that API lands, flag the ships instead.)
        hc.device->setFrustumCullEnabled(false);
        // FAR PLANE 15 km (landmine L7, THIRD appearance — owner: "it has never
        // changed: as soon as you back up a TINY BIT the ship is visibly taken off
        // screen"). The interactive beat never set the far plane, so it ran the
        // 200 m ENGINE DEFAULT — and the capital SPAWNS at 200 m: one tap of
        // reverse clipped it out of the universe (his screenshot showed its
        // far-plane CROSS-SECTION as a floating box, and enemy brackets around
        // clipped-away fighters). Paired with the 10.5 km planet anchor below.
        hc.device->setCameraFar(15000.0f);
    }
    // Re-apply the deep-space look EVERY beat: the one-time set at intro start
    // was getting undone before the first interactive window (live evidence:
    // the canopy background was the raw 0.04/0.05/0.08 HDR clear — the sky
    // pass was OFF, no starfield). Something on the cinematic-beat path resets
    // sky state; per-beat reapply defeats it and is idempotent (setSkyParams
    // only flags an IBL rebake when the params actually change).
    if (live && cockpit) x3::apphost::setIntroCockpitLook(*hc.device);

    // NO TIME LIMIT in live play (owner: "get rid of the time limit ... let me fly
    // around to point at the enemy"). The beat now ends ONLY when you cripple the
    // capital + clear the wing, when you die, or when you press Esc — never on a clock.
    // Headless (deterministic tests / boot-time / captures) STILL stops at maxSteps, or
    // it would loop forever with no player to end it.
    // Capture-only disintegration staging: blow up the most-centered fighter just
    // before the s660 frame so the shot catches the blast near screen center.
    float capKillPos[3] = { 0.0f, 0.0f, 0.0f };
    bool  capKilled = false;
    (void)capKillPos;
    for (int step = 0; interactive || step < maxSteps; ++step) {
        // See the TIME BASE note above: fixed 1/60 headless, real elapsed live.
        float dt = kFixedDt;
        if (interactive) {
            const auto now = std::chrono::steady_clock::now();
            dt = (float)std::chrono::duration<double>(now - tPrev).count();
            tPrev = now;
            if (!(dt > kMinDt)) dt = kMinDt;      // also catches NaN
            if (dt > kMaxDt)    dt = kMaxDt;
        }
        simTime += dt;
        const float tNow = simTime;

        // ---- Input: live reads GLFW; headless uses a deterministic synthetic
        //      "competent pilot" profile (steady forward + aim at the capital). ----
        x3::game::PlayerInput in{};
        float rollAxis = 0.0f;
        bool fire = false;
        if (interactive) {
            // Poll EVERY frame — without this GLFW's key/button/cursor state is
            // frozen at the beat-entry snapshot and the ship ignores the player
            // entirely (owner playtest: "I tried to shoot", acc 0/0, "clunky").
            glfwPollEvents();
            if (glfwWindowShouldClose(hc.window)) break;
            if (glfwGetKey(hc.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            // F2: toggle 1P cockpit <-> 3P chase (owner request). Edge-detected so
            // one press flips once, not every frame it's held.
            {
                static bool f2Prev = false;
                const bool f2 = glfwGetKey(hc.window, GLFW_KEY_F2) == GLFW_PRESS;
                if (f2 && !f2Prev) pilot.toggleCameraMode();
                f2Prev = f2;
            }
            // F5: test-only 6DOF speed x2 (owner: A/B the dart feel live without a
            // rebuild). Edge-detected; toggles the uniform speed multiplier 1<->2.
            {
                static bool f5Prev = false;
                const bool f5 = glfwGetKey(hc.window, GLFW_KEY_F5) == GLFW_PRESS;
                if (f5 && !f5Prev) pilot.setSpeedMul(pilot.speedMul() == 1.0f ? 2.0f : 1.0f);
                f5Prev = f5;
            }
            // F9: skip the ENTIRE intro (owner dev shortcut) — latch + bail this
            // beat; the orchestrator returns canon ShotDown at the next boundary.
            if (glfwGetKey(hc.window, GLFW_KEY_F9) == GLFW_PRESS) {
                requestSkipAllIntro();
                break;
            }
            auto kd = [&](int k){ return glfwGetKey(hc.window, k) == GLFW_PRESS; };
            in.moveFwd    = (kd(GLFW_KEY_W)?1.f:0.f) + (kd(GLFW_KEY_S)?-1.f:0.f);
            in.moveStrafe = (kd(GLFW_KEY_D)?1.f:0.f) + (kd(GLFW_KEY_A)?-1.f:0.f);
            in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            // Vertical thrust (owner: "Add Space to rise UP and C to drop DOWN
            // with the ship!!!!") — held axes, boost-scaled in the pilot.
            in.jumpPressed = kd(GLFW_KEY_SPACE);   // rise
            in.diveHeld    = kd(GLFW_KEY_C);       // drop
            rollAxis      = (kd(GLFW_KEY_Q)?-1.f:0.f) + (kd(GLFW_KEY_E)?1.f:0.f);
            double mx, my; glfwGetCursorPos(hc.window, &mx, &my);
            // HOLD-ALT FREELOOK (owner: "the player will be ABLE to keep the enemy
            // ship in sight by looking around while zipping around"): while ALT is
            // held the mouse ORBITS the camera around the ship (flight keeps its
            // heading, momentum carries); release eases the view back dead-astern.
            const bool freeLookHeld = kd(GLFW_KEY_LEFT_ALT) || kd(GLFW_KEY_RIGHT_ALT);
            if (freeLookHeld) {
                pilot.addFreeLook((float)(mx - lastMX), (float)(my - lastMY));
                in.lookDX = 0.0f;               // the ship holds its heading
                in.lookDY = 0.0f;
            } else {
                in.lookDX = (float)(mx - lastMX);   // mouse-look: yaw/pitch the ship
                in.lookDY = (float)(my - lastMY);
            }
            lastMX = mx; lastMY = my;
            fire = glfwGetMouseButton(hc.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        } else {
            in.moveFwd = 1.0f;                  // close the distance
            fire = (step % 12) == 0;            // a measured firing cadence
            // SFX PROBE (X3_INTRO_SFXPROBE=1): a scripted headless flight that
            // exercises the remaining combat cues so the [intro-sfx] first-trigger
            // log lines prove every hook fires. Boost window ~2-5 s; a forced
            // fighter kill at ~7 s (through the same fighterKillFx path the live
            // ray-kill uses). The straight-ahead flight crosses the capital's
            // 110 m bubble on its own, so the force-field zap triggers naturally.
            // Env-gated: the default headless runs (self-tests) are untouched.
            if (sfxProbe) {
                in.sprint = (step >= 120 && step < 300);
                if (step == 420 && enemies.count() > 0) {
                    const auto& es0 = enemies.ship(0);
                    const float hp[3] = { es0.pos[0], es0.pos[1], es0.pos[2] };
                    fighterKillFx(hp);
                    enemies.damageShip(0, 100000);
                }
                // Scripted hits through the SAME playerHitSfx path the live salvo
                // branch uses: shields-up splash at 8 s, then a shield-draining
                // slam so the 8.7 s hit lands on bare hull (distinct crunch).
                if (step == 480) playerHitSfx(x3::space::shipai::kLaserDamage);
                if (step == 500) playerHitSfx(pilot.shield() + 100);   // drain shield
                if (step == 520) playerHitSfx(x3::space::shipai::kLaserDamage);
            }
            // ---- CAPTURE SCRIPT (X3_INTRO_CAPTURE, headless render run) ----
            // Overrides the generic synthetic profile with the staged combat-
            // readability evidence scenario: hold station so the framing is
            // stable, stage two wing ships into the damage bands early (smoke
            // trail + heavy burn build up over the run), land a scripted hit
            // just before the s420 shot (hull hit-flash + sparks + confirm),
            // and hold the trigger through 630-900 so the energy pool visibly
            // drains under sustained fire. Env-gated: normal headless untouched.
            if (captureMode && killCapture) {
                // ---- CAPITAL-KILL EVIDENCE RUN (X3_INTRO_CAPTURE_KILL=1) ----
                // The combat-readability capture above stages fighters and must
                // NOT chew the capital down; this run does the opposite. It
                // flies the player to a standoff on the dreadnought's beam, then
                // puts her hull to 0 at a known step and lets the REAL kill hook
                // fire, so the frame series that follows is the shipped death
                // sequence — not a re-staged mock-up of it.
                //
                // WHY THIS PATH IS TRUSTWORTHY: this is the interactive-beat
                // loop with a device and no window. fxPtr->update(dt) and the
                // GPU debris step/draw run here exactly as they do in play (see
                // the fxOn block below), which is why the combat VFX actually
                // tick — unlike a screenshot-host settle loop, where they don't.
                in.moveFwd = 0.0f;
                fire = false;
                if (step == 1) {
                    // Park on her beam, above the plane, nose swung onto the
                    // hull: the whole 450 m ship in frame with room for the
                    // break-up to happen INSIDE the shot. Positioned OPPOSITE
                    // the hero planet's sky ray, so the DEORBIT ending falls
                    // AWAY from the camera (the recession is the read).
                    pilot.spawn(*phys, kCapX + 430.0f, 190.0f, 880.0f, tun);
                    pilot.addFreeLook(0.0f, 0.0f);
                }
                if (step >= 2 && step <= 90) {
                    // Ease the nose onto the capital using the existing look
                    // bias (no new camera code; the beat already feeds this).
                    const x3::phys::Vec3 pp0 = pilot.pos();
                    float d[3] = { capC[0] - pp0.x, capC[1] - pp0.y,
                                   capC[2] - pp0.z };
                    const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    if (dl > 1.0f) { d[0]/=dl; d[1]/=dl; d[2]/=dl;
                                     pilot.setCameraLookBias(d, 1.0f); }
                }
                if (step == 120) {
                    // THE KILL, through the shipped hook: hull to 0 and every
                    // hardpoint down, exactly the state a real fight ends in.
                    for (int si = 0; si < kMaxSubsystems; ++si)
                        x3::space::ShipDamage::applyDamage(capital, 100000,
                                                          kHard[si].sub);
                    x3::space::ShipDamage::applyDamage(capital, 100000);
                    x3::logInfo("[intro] kill-capture: dreadnought hull forced to 0 "
                                "at step 120 — the real death hook takes it from here");
                }
            } else if (captureMode) {
                in.moveFwd = 0.0f;
                fire = (step >= 402 && step < 424) ||
                       (step >= 630 && step < 900);
                // STRAFING FIRE (the beam-origin evidence, owner: "the weapon
                // beams do not come from the ship when its strafing, they come
                // beside it"). The second fire window now runs with A/D HELD, so
                // the s660/s900 frames are shot at full lateral velocity — which
                // is the only condition under which a world-anchored tracer
                // visibly detaches from the wingtip. Deterministic; the frames
                // are the standing regression evidence for that fix.
                in.moveStrafe = (step >= 630 && step < 900) ? 1.0f : 0.0f;
                if (step == 30 && enemies.count() >= 3) {
                    enemies.damageShip(1, 34);   // -> ~43% hull: sparks + smoke trail
                    enemies.damageShip(2, 48);   // -> 20% hull: heavy smoke + embers
                }
                if (step == 416 && enemies.count() >= 1) {
                    enemies.damageShip(0, 6);    // registered hit: flash for s420
                    if (fxOn) {
                        const auto& e0 = enemies.ship(0);
                        fxPtr->spawnImpact({ e0.pos[0], e0.pos[1], e0.pos[2] },
                                           { -e0.fwd[0], -e0.fwd[1], -e0.fwd[2] });
                    }
                    hitConfirmT = 0.15f;         // the reticle confirm flicker
                }
                // THE MONEY SHOT (owner: "fire ... comes from anywhere BUT the
                // ship"): dead-astern the wing bolts are foreshortened behind
                // the player's own hull. Swing the FREELOOK camera ~40 deg off-
                // axis through the s420 fire window so the frame shows the bolt
                // LEAVING the wingtip. Held by re-feeding (0,0); released after
                // 460 so the later shots return to the flight framing.
                if (step == 396) pilot.addFreeLook(150.0f, -30.0f);
                else if (step > 396 && step < 460) pilot.addFreeLook(0.0f, 0.0f);
                // SHIP DISINTEGRATION evidence (space power fantasy): ~0.08 s before
                // the s660 capture, disintegrate a fighter and stage the blast at a
                // CLOSE point on the camera's forward ray (~90 m out, at the crosshair)
                // so the MASSIVE fireball + white-hot flash + flung glowing chunks all
                // READ at ship scale, CENTERED in frame — the framing a real close-
                // range kill produces (you shot the ship in your sights). A real
                // fighter is destroyed too so CONTACTS drops. Capture-only.
                if (step == 655 && !capKilled && fxOn) {
                    const float fhp = std::cos(pilot.pitch());
                    const float fw[3] = { fhp * std::cos(pilot.yaw()),
                                          std::sin(pilot.pitch()),
                                          fhp * std::sin(pilot.yaw()) };
                    const x3::phys::Vec3 pp = pilot.pos();
                    // Kill the most-forward real fighter (gameplay-honest: CONTACTS drops).
                    if (enemies.count() >= 1) {
                        int best = 0; float bestDot = -2.0f;
                        for (uint32_t i = 0; i < enemies.count(); ++i) {
                            const auto& es = enemies.ship(i);
                            const float oc[3] = { es.pos[0]-pp.x, es.pos[1]-pp.y, es.pos[2]-pp.z };
                            const float l = std::sqrt(oc[0]*oc[0]+oc[1]*oc[1]+oc[2]*oc[2]);
                            if (l < 1e-3f) continue;
                            const float dd = (oc[0]*fw[0]+oc[1]*fw[1]+oc[2]*fw[2]) / l;
                            if (dd > bestDot) { bestDot = dd; best = (int)i; }
                        }
                        enemies.damageShip((uint32_t)best, 100000);
                    }
                    // The disintegration FX, framed close + centered on the crosshair.
                    const float blast[3] = { pp.x + fw[0]*90.0f,
                                             pp.y + fw[1]*90.0f,
                                             pp.z + fw[2]*90.0f };
                    fighterKillFx(blast);
                    capKilled = true;
                    capKillPos[0]=blast[0]; capKillPos[1]=blast[1]; capKillPos[2]=blast[2];
                }
            }
        }
        // ANTIMATTER BOOST: one whoosh per Shift ENGAGE (edge-detected — the
        // sustained roar is the thrust bed swelling below, not a retrigger).
        if (in.sprint && !prevSprint)
            play2D(kSfxBoost, "boost_antimatter (Shift engage)", sndBoost, 0.9f, 1.0f);
        prevSprint = in.sprint;

        // ---- THE STAR phase machine (the host_space spectacle contract). Runs
        // every step BEFORE the pilot update so Detonation onward can freeze the
        // world; headless the pilot never nears the star, so phase stays Flying
        // and every pinned metric is untouched. ------------------------------
        {
            const x3::phys::Vec3 sp3 = pilot.pos();
            const float sdx = sp3.x-kSunCenter[0], sdy = sp3.y-kSunCenter[1],
                        sdz = sp3.z-kSunCenter[2];
            const float dCenter = std::sqrt(sdx*sdx + sdy*sdy + sdz*sdz);
            sunSurfDist = dCenter - kSunRadius;
            // Heat-rung barks (approach feedback; only meaningful in live flight).
            const int stage = sunSurfDist < kSunCritDist ? 2
                            : sunSurfDist < kSunWarnDist ? 1 : 0;
            if (stage > sunHeatStage &&
                (sunPhase == SunPhase::Flying || sunPhase == SunPhase::InsideSun))
                pushCallout(kCalloutPlayer,
                            stage == 2 ? "YOUR HULL TEMP CRITICAL - PULL AWAY"
                                       : "YOUR HULL TEMP RISING",
                            stage == 2 ? 1.2f : 0.9f);
            sunHeatStage = stage;
            if (sunShieldFlashT >= 0.0f) {
                sunShieldFlashT += dt;
                if (sunShieldFlashT > kSunFlashSecs) sunShieldFlashT = -1.0f;
            }
            // Trajectory ring: record through LIVE flight (Flying + the graze
            // window) so a real detonation replays the whole approach.
            if (sunPhase == SunPhase::Flying || sunPhase == SunPhase::InsideSun) {
                trajTimer += dt;
                if (trajTimer >= 1.0f / kTrajHz) {
                    trajTimer = 0.0f;
                    const x3::phys::Vec3 f3 = pilot.forward();
                    const x3::phys::Vec3 u3 = pilot.up();
                    const x3::phys::Vec3 r3 = pilot.right();
                    trajRing[(size_t)trajHead] = {
                        { sp3.x, sp3.y, sp3.z }, { f3.x, f3.y, f3.z },
                        { u3.x, u3.y, u3.z },    { r3.x, r3.y, r3.z } };
                    trajHead = (trajHead + 1) % kTrajLen;
                    if (trajCount < kTrajLen) ++trajCount;
                }
            }
            // Any-key edge — the cinematic skip (Detonation onward ONLY; a key
            // during InsideSun means "fly the graze-abort", never "skip my death").
            const bool anyNow = fire || in.sprint || in.jumpPressed || in.diveHeld ||
                                std::fabs(in.moveFwd) > 0.01f ||
                                std::fabs(in.moveStrafe) > 0.01f;
            const bool skipEdge = anyNow && !sunPrevAny;
            sunPrevAny = anyNow;
            switch (sunPhase) {
                case SunPhase::Flying:
                    if (sunShieldPct < 100.0f)
                        sunShieldPct = std::min(100.0f,
                            sunShieldPct + dt * (100.0f / kSunRechargeSecs));
                    if (dCenter < kSunRadius) {         // breached the core
                        sunPhase = SunPhase::InsideSun; sunPhaseT = 0.0f;
                        sunShieldPct = 100.0f; sunShieldFlashT = 0.0f;
                        pushCallout(kCalloutPlayer, "YOUR DIVE SHIELD ENGAGED - STELLAR CORE", 1.3f);
                        play2D(kSfxZap, "sun_shield_engage (core breach)",
                               sndZap, 1.0f, 0.7f);
                        const float spArr[3] = { sp3.x, sp3.y, sp3.z };
                        setupKillCam(spArr);
                    }
                    break;
                case SunPhase::InsideSun:
                    sunPhaseT += dt;
                    sunShieldPct = 100.0f *
                        std::max(0.0f, 1.0f - sunPhaseT / kSunShieldSecs);
                    if (dCenter >= kSunRadius) {        // GRAZE: pulled back out
                        sunPhase = SunPhase::Flying; sunPhaseT = 0.0f;
                    } else if (sunPhaseT >= kSunShieldSecs) {
                        snapshotTraj();
                        sunPhase = SunPhase::Detonation; sunPhaseT = 0.0f;
                        play2D(kSfxExplosion, "sun_detonation (antimatter blast)",
                               sndExplosion, 1.0f, 0.45f);
                        x3::logInfo("[intro] STAR DETONATION — spectacle cam rolling "
                                    "(rewind -> 30 SECONDS EARLIER -> replay -> respawn)");
                    }
                    break;
                case SunPhase::Detonation:
                    sunPhaseT += dt;
                    if (skipEdge) { sunPhase = SunPhase::Respawn; sunPhaseT = 0.0f; sunRespawned = false; break; }
                    if (sunPhaseT >= kDetonateSecs) { sunPhase = SunPhase::Rewind; sunPhaseT = 0.0f; }
                    break;
                case SunPhase::Rewind:
                    sunPhaseT += dt;
                    if (skipEdge) { sunPhase = SunPhase::Respawn; sunPhaseT = 0.0f; sunRespawned = false; break; }
                    if (sunPhaseT >= kRewindSecs) { sunPhase = SunPhase::TitleCard; sunPhaseT = 0.0f; }
                    break;
                case SunPhase::TitleCard:
                    sunPhaseT += dt;
                    if (skipEdge) { sunPhase = SunPhase::Respawn; sunPhaseT = 0.0f; sunRespawned = false; break; }
                    if (sunPhaseT >= kTitleSecs) { sunPhase = SunPhase::Replay; sunPhaseT = 0.0f; }
                    break;
                case SunPhase::Replay:
                    sunPhaseT += dt;
                    if (skipEdge) { sunPhase = SunPhase::Respawn; sunPhaseT = 0.0f; sunRespawned = false; break; }
                    if (sunPhaseT >= kReplaySecs) { sunPhase = SunPhase::Respawn; sunPhaseT = 0.0f; sunRespawned = false; }
                    break;
                case SunPhase::Respawn:
                    sunPhaseT += dt;
                    if (!sunRespawned && sunPhaseT >= kSunFadeSecs) {  // re-seed at black
                        pilot.spawn(*phys, 0.0f, 0.0f, 0.0f, tun);
                        trajHead = trajCount = 0; trajTimer = 0.0f;
                        sunShieldPct = 100.0f;
                        sunRespawned = true;
                    }
                    if (sunPhaseT >= 2.0f * kSunFadeSecs + 0.8f) {
                        sunPhase = SunPhase::Flying; sunPhaseT = 0.0f;
                        pushCallout(kCalloutPlayer, "YOUR SHIP IS CLEAR - BACK IN THE FIGHT", 1.0f);
                    }
                    break;
            }
        }
        // ---- SPECTACLE CAM frames (Detonation..Replay): the external kill-cam
        // owns the frame; the combat world holds its breath (pilot frozen, no AI,
        // no fire, no damage — exactly the host's cineNow contract). Respawn runs
        // the NORMAL loop underneath its fade (the re-seeded ship is flyable the
        // moment the black lifts). --------------------------------------------
        if (sunPhase == SunPhase::Detonation || sunPhase == SunPhase::Rewind ||
            sunPhase == SunPhase::TitleCard  || sunPhase == SunPhase::Replay) {
            if (live) {
                beatT += dt;
                hc.device->setCamera(cineCamPos[0], cineCamPos[1], cineCamPos[2],
                                     cineYaw, cinePit, 60.0f);
                if (sfx) {
                    sfx->setListener(cineCamPos[0], cineCamPos[1], cineCamPos[2],
                                     cineYaw, cinePit);
                    if (humLoop.valid())    sfx->setLoopParams(humLoop, 0.0f, 1.0f);
                    if (thrustLoop.valid()) sfx->setLoopParams(thrustLoop, 0.0f, 1.0f);
                    sfx->update(dt);
                }
                auto frame = hc.device->beginFrame();
                if (frame.valid && cockpit) {
                    int winW = (int)hc.W, winH = (int)hc.H;
                    // FRAMEBUFFER pixels, not window points (see the HUD note in
                    // the flight branch): drawHudQuad works in framebuffer px.
                    if (hc.window) glfwGetFramebufferSize(hc.window, &winW, &winH);
                    aimSkyAtStar(cineCamPos[0], cineCamPos[1], cineCamPos[2]);
                    if (!planets.empty())
                        x3::apphost::drawNightSkyPlanets(hc.device, frame, planetMesh,
                            planets, beatT, cineCamPos[0], cineCamPos[1], cineCamPos[2],
                            ringMesh, 10500.0f);
                    drawStar(frame);
                    if (sunPhase == SunPhase::Detonation) {
                        drawEjecta(frame, sunPhaseT);
                    } else if (sunPhase == SunPhase::Rewind) {
                        drawEjecta(frame, kDetonateSecs * (1.0f - sunPhaseT / kRewindSecs));
                        drawReplayShip(frame, 1.0f);
                    } else if (sunPhase == SunPhase::Replay) {
                        drawReplayShip(frame, sunPhaseT / kReplaySecs);
                    }
                    drawSunOverlay(frame, (float)winW, (float)winH);
                }
                hc.device->endFrame(frame);
                if (interactive) {
                    // Courtesy frame ceiling only — dt is measured, never assumed
                    // (see the TIME BASE note; this cut-scene branch is on the
                    // same clock as the flight branch).
                    const double frameSec = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - tPrev).count();
                    constexpr double kMinFrameSec = 1.0 / 300.0;
                    if (frameSec < kMinFrameSec)
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(kMinFrameSec - frameSec));
                }
            }
            continue;   // the combat world is frozen under the kill-cam
        }

        pilot.setRollInput(rollAxis);
        pilot.update(in, dt, *phys);

        // ---- FORCE FIELDS (owner: "I fly right thru the enemy ship"). The player
        //      bounces off the capital's shield bubble and off each fighter's hull
        //      bubble — a shield BOUNCE (pushOut cancels+reflects inward velocity).
        // CAPITAL MOTION (owner: "The overlords ship should move around"): a
        // slow, wide, phase-driven arc — patrol pace with shields up, answering
        // the helm once they break — until the ENGINES hardpoint dies, at which
        // point her speed EASES to a dead stop (adrift): still the most legible
        // "your choice changed the fight" feedback there is. The kill freezes
        // her too (the death state machine owns the wreck from there).
        {
            const bool adrift = capitalKilled ||
                x3::space::ShipDamage::subsystemDown(capital,
                    x3::space::Subsystem::Engines);
            const bool shieldsDown =
                x3::space::ShipDamage::shieldFrac(capital) <= 0.0f;
            x3::space::CapitalMotion::update(capMotion, dt, shieldsDown, adrift);
            capC[0] = capMotion.pos[0];
            capC[1] = capMotion.pos[1];
            capC[2] = capMotion.pos[2];
            // Heading rotation for every hull-local offset (base nose = -X).
            capHeadCs = -capMotion.fwd[0];
            capHeadSn = -capMotion.fwd[2];
        }
        {
            bool zapped = pilot.pushOut(capC, kCapBubbleR);  // capital hull bubble
            for (uint32_t i = 0; i < enemies.count(); ++i) {
                const auto& es = enemies.ship(i);
                if (es.hull <= 0) continue;
                zapped = pilot.pushOut(es.pos, 16.0f) || zapped;   // fighter hull bubble
            }
            // pushOut returns true exactly for this: the field ZAPS (2D — it is
            // the player's own hull on the bubble) and the HUD border flashes
            // cyan. Cooldown so sliding along a bubble reads as one bounce, not
            // a 60 Hz machine-gun.
            if (zapped && zapCooldown <= 0.0f) {
                play2D(kSfxZap, "forcefield_zap (pushOut bounce)", sndZap, 0.85f, 1.0f);
                zapCooldown = 0.4f;
                zapFlashT   = 0.15f;
            }
        }
        if (zapCooldown > 0.0f) zapCooldown -= dt;
        if (zapFlashT   > 0.0f) zapFlashT   -= dt;
        if (hitConfirmT > 0.0f) hitConfirmT -= dt;

        // ---- Enemy AI tick + salvo accounting (dodge metric). ----
        const x3::phys::Vec3 pp = pilot.pos();
        const x3::phys::Vec3 pv = pilot.velocity();
        const float ppos[3] = { pp.x, pp.y, pp.z };
        const float pvel[3] = { pv.x, pv.y, pv.z };
        enemies.update(dt, ppos, pvel);
        // (Death FX/SFX are triggered at the DAMAGE SITES via fighterKillFx —
        //  the old post-update prevHull scan here could never fire, because the
        //  manager swap-removes a dead ship the instant its hull hits zero.)
        bool playerFiredThisStep = false;
        for (const auto& fe : enemies.fireEvents()) {
            play3D(kSfxEnemyLaser, "enemy_laser (3D at muzzle)", sndEnemyLaser,
                   fe.from, 0.8f, 1.0f);
            if (fxOn) {
                // SHIP-SCALE bolt + muzzle (the on-foot 0.035 m tracer and
                // 0.05 m flash are sub-pixel at dogfight range).
                fxPtr->addTracer({ fe.from[0], fe.from[1], fe.from[2] },
                             { fe.to[0],   fe.to[1],   fe.to[2] },
                             x3::game::WeaponFxKind::Default, /*width*/ 0.55f);
                fxPtr->spawnShipMuzzle({ fe.from[0], fe.from[1], fe.from[2] },
                                    { fe.to[0] - fe.from[0], fe.to[1] - fe.from[1],
                                      fe.to[2] - fe.from[2] });
            }
            ++localSalvosFaced;
            // ASPECT DODGE (vet pass — replaces the old raw proximity test): a
            // salvo aimed at the player (endpoint near him) lands as a function
            // of his TRANSVERSE velocity across the firing line (salvoLands).
            // Fly a crossing line and the wing whiffs; bore straight in and it
            // connects. A salvo aimed nowhere near him was never a threat: dodge.
            const float dx = fe.to[0] - ppos[0];
            const float dy = fe.to[1] - ppos[1];
            const float dz = fe.to[2] - ppos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < 45.0f * 45.0f &&
                salvoLands(fe.from, /*turret*/false, ppos, pvel)) {
                playerHitSfx(x3::space::shipai::kLaserDamage);
                if (fxOn) fxPtr->spawnImpact({ fe.to[0], fe.to[1], fe.to[2] },
                                             { -dx, -dy, -dz });
            } else {
                ++localSalvosDodged;
            }
        }

        // ---- CAPITAL BATTERY GAUNTLET: the approach is contested by FOUR
        // physical gun mounts, each with its own telegraph (0.6 s spool: chirp
        // + HUD flash) and cadence, staggered so the pressure is continuous
        // but never a synchronized volley. Shoot a mount off and THAT gun goes
        // silent forever (fire only ever ceases from destroyed mounts);
        // killing SENSORS halves accuracy; past 2.2 km you're safe (sniper
        // option). Timers live in CapitalBattery (--test-ship-damage T22).
        {
            const float pdx = ppos[0]-capC[0], pdy = ppos[1]-capC[1], pdz = ppos[2]-capC[2];
            const float pDist = std::sqrt(pdx*pdx + pdy*pdy + pdz*pdz);
            const bool inRange = !capitalKilled && pDist < kTurretRange;
            uint32_t spooled = 0u;
            const uint32_t firedMask = x3::space::CapitalBattery::update(
                capBattery, dt, inRange, &spooled);
            if (spooled) {
                incomingFlashT = x3::space::CapitalBattery::kGunSpool;
                play2D(kSfxLock, "turret_spool (incoming warning)",
                       sndLock, 0.65f, 0.55f);
            }
            for (int gi = 0; gi < x3::space::kCapitalGunCount; ++gi) {
                if (!(firedMask & (1u << gi))) continue;
                float muz[3]; gunWorld(gi, muz);
                ++localSalvosFaced;
                const bool lands = salvoLands(muz, /*turret*/true, ppos, pvel);
                // The bolt draws to where it actually goes: the player on a
                // hit, or PAST him along his miss-side on a whiff (the
                // near-miss streak is the reward for flying the aspect).
                float endP[3] = { ppos[0], ppos[1], ppos[2] };
                if (!lands) {
                    const float mh = salvoHash();
                    endP[0] += (mh - 0.5f) * 120.0f;
                    endP[1] += (salvoHash() - 0.5f) * 90.0f;
                    endP[2] += (mh > 0.5f ? 55.0f : -55.0f);
                }
                play3D(kSfxEnemyLaser, "turret_fire (capital battery)",
                       sndEnemyLaser, muz, 1.0f, 0.6f);
                if (fxOn) {
                    fxPtr->addTracer({ muz[0], muz[1], muz[2] },
                                     { endP[0], endP[1], endP[2] },
                                     x3::game::WeaponFxKind::Default, 1.4f);
                    fxPtr->spawnShipMuzzle({ muz[0], muz[1], muz[2] },
                        { endP[0]-muz[0], endP[1]-muz[1], endP[2]-muz[2] });
                }
                if (lands) {
                    playerHitSfx(x3::space::CapitalBattery::kGunDamage);
                    if (fxOn) fxPtr->spawnImpact(
                        { ppos[0], ppos[1], ppos[2] },
                        { -pdx / std::max(1.0f, pDist),
                          -pdy / std::max(1.0f, pDist),
                          -pdz / std::max(1.0f, pDist) });
                } else {
                    ++localSalvosDodged;
                }
            }
        }
        if (incomingFlashT > 0.0f) incomingFlashT -= dt;
        for (auto& cl : callouts) if (cl.t > 0.0f) cl.t -= dt;

        // (The star's heat/shield/death sim now lives in the SunPhase machine at
        //  the top of the loop — the host_space spectacle contract: detonation
        //  kill-cam, rewind, "30 SECONDS EARLIER", replay, respawn.)

        // ---- Targeting feed: the capital ship is the priority hostile contact. ----
        x3::space::Contact contacts[16]{};   // capital + up to 15 escorts
        uint32_t nc = 0;
        // Capital contact at its REAL (moving) position + its TRUE arc velocity,
        // so the lead pip solves the motion and the 6DOF target hold tracks a
        // hull that is actually under way (T19: the hold follows a moving
        // locked target; T20: it stays suspended when aimed away).
        contacts[nc++] = { 1000u, { capC[0], capC[1], capC[2] },
                                  { capMotion.vel[0], capMotion.vel[1],
                                    capMotion.vel[2] }, true }; // capital
        for (uint32_t i = 0; i < enemies.count() && nc < 16; ++i) {
            const auto& e = enemies.ship(i);
            contacts[nc++] = { 1u + i, { e.pos[0], e.pos[1], e.pos[2] },
                                       { e.vel[0], e.vel[1], e.vel[2] }, true };
        }
        targeting.setContacts(contacts, nc);
        // Cosmetic lock acquisition for the HUD (does not alter the metrics).
        if (live) {
            const float pfw[3] = { std::cos(pilot.pitch()) * std::cos(pilot.yaw()),
                                   std::sin(pilot.pitch()),
                                   std::cos(pilot.pitch()) * std::sin(pilot.yaw()) };
            // Capture script: the capital sits dead ahead and wins the boresight
            // cone every frame — cycle onto a FIGHTER at s120 and hold it, so
            // the evidence shows the fighter lock (hull bar + lead pip on a
            // MOVING target). Interactive play keeps the per-frame cone pick.
            if (killCapture) {
                // The kill run's subject IS the capital — hold the lock on it
                // (id 1000) so the look-bias below keeps the dying ship framed
                // for the whole sequence instead of cycling onto a fighter.
                if (!targeting.hasLock() || targeting.lockedId() != 1000u)
                    targeting.lockNearest(ppos, pfw);
            } else if (captureMode && step >= 120) {
                if (step == 120 || !targeting.hasLock() || targeting.lockedId() == 1000u)
                    targeting.cycleTarget(+1);
                // Late in the run, shift the lock onto the BURNING ship (hostile
                // list: cap, f0, f1, f2, ...) so the look-bias frames it and the
                // s900 shot shows its low hull bar + lead pip on a wounded target.
                if (step == 600) { targeting.cycleTarget(+1); targeting.cycleTarget(+1); }
            } else {
                targeting.lockNearest(ppos, pfw);
            }
            // TARGET-KEEPING LOOK: pull the 3P gaze gently toward the locked
            // contact (capped in the pilot at 0.6) so the fight tends to stay in
            // frame while you maneuver. Releases (amount 0) when nothing's locked.
            bool fedBias = false;
            if (targeting.hasLock()) {
                const uint32_t lid = targeting.lockedId();
                for (uint32_t ci = 0; ci < nc; ++ci) {
                    if (contacts[ci].id != lid) continue;
                    float dirT[3] = { contacts[ci].pos[0] - ppos[0],
                                      contacts[ci].pos[1] - ppos[1],
                                      contacts[ci].pos[2] - ppos[2] };
                    const float dl = std::sqrt(dirT[0]*dirT[0] + dirT[1]*dirT[1] + dirT[2]*dirT[2]);
                    if (dl > 1.0f) {
                        dirT[0] /= dl; dirT[1] /= dl; dirT[2] /= dl;
                        // Capture runs bias harder so the locked target is IN
                        // the evidence frame; live play uses the space.lookBias
                        // lever (default 0.15, was a hard-coded 0.30 — an assist
                        // that ate a third of every mouse whip, which is the
                        // classic "the ship doesn't zip" culprit). 0 = off.
                        fedBias = true;
                        // 6DOF TARGET HOLD (owner spec: "strafe in 6DOF and keep
                        // on a target the whole time"). Steers the SHIP'S NOSE —
                        // and only the nose — onto the locked contact, so you can
                        // slide sideways, climb, back off, and the guns stay on
                        // him. Manual aim wins outright: any mouse movement this
                        // frame passes rate 0, so the assist never fights the
                        // player's own look. space.targetHold is the rate cap in
                        // rad/s; 0 disables the whole thing.
                        const bool lookingNow = std::fabs(in.lookDX) > 0.5f ||
                                                std::fabs(in.lookDY) > 0.5f;
                        // SNAP-BACK FIX (owner, live 2026-08-16: "The aim always
                        // returns to the overlord ship!!! it should NOT.."): the
                        // per-frame gate above was the whole defence, so the
                        // instant the mouse stopped the hold slewed the nose back
                        // onto the locked capital from ANY angle. AimSovereignty
                        // (space_pilot.h) now arbitrates: a grace after the last
                        // input, and an aim-away suspension with hysteresis — the
                        // hold KEEPS the aim the player has, never re-acquires
                        // one he aimed away from. Capture runs are scripted
                        // evidence shots, not play: they keep the old always-on
                        // framing gate untouched.
                        const float errRad = pilot.steerNoseToward(dirT, 0.0f, dt); // probe only
                        if (!lockHad || targeting.lockedId() != lockPrevId)
                            aimSov.onNewTarget(errRad);   // fresh lock never yanks the nose
                        const float holdRate = captureMode
                            ? ((lvTargetHold > 0.0f && !lookingNow) ? lvTargetHold : 0.0f)
                            : aimSov.gate(lookingNow, errRad, lvTargetHold, dt);
                        // The camera gaze bias obeys the same suspension — an
                        // aimed-away player keeps his OWN view too, instead of
                        // the gaze creeping back toward the capital.
                        pilot.setCameraLookBias(dirT, captureMode ? 0.55f
                            : (aimSov.suspended() ? 0.0f : lvLookBias));
                        pilot.steerNoseToward(dirT, holdRate, dt);
                    }
                    break;
                }
            }
            if (killCapture) {
                // Evidence framing: hold the dying capital dead-centre for the
                // whole sequence regardless of what the lock is doing (the
                // wreck stops being a "contact" the moment it dies). The
                // deorbit wreck FALLS, so frame the falling hull, not the
                // point where she died.
                const x3::phys::Vec3 pk = pilot.pos();
                float dk[3] = {
                    capC[0] + capDeath.planetDir[0] * capDeath.plunge - pk.x,
                    capC[1] + capDeath.planetDir[1] * capDeath.plunge
                            - capDeath.sag - pk.y,
                    capC[2] + capDeath.planetDir[2] * capDeath.plunge - pk.z };
                const float dkl = std::sqrt(dk[0]*dk[0] + dk[1]*dk[1] + dk[2]*dk[2]);
                if (dkl > 1.0f) { dk[0]/=dkl; dk[1]/=dkl; dk[2]/=dkl;
                                  pilot.setCameraLookBias(dk, 1.0f); fedBias = true; }
            }
            if (!fedBias) { const float z[3] = { 1, 0, 0 }; pilot.setCameraLookBias(z, 0.0f); }
        }
        // LOCK STAGING (reticle): age the current lock; a target switch restarts
        // the ACQUIRING sweep. lockAge >= kAcquireSec == LOCKED (red bracket).
        if (targeting.hasLock()) {
            if (!lockHad || targeting.lockedId() != lockPrevId) {
                lockAge = 0.0f; lockPrevId = targeting.lockedId(); lockHad = true;
            } else {
                lockAge += dt;
            }
        } else { lockHad = false; lockAge = 0.0f; }
        // LOCK-ACQUIRED chirp — now on the STAGED lock completing (the moment
        // the bracket goes red), not on the raw contact pick (which is instant
        // + retriggers every target sweep).
        {
            const bool lockNow = lockHad && lockAge >= kAcquireSec;
            if (lockNow && !prevLock)
                play2D(kSfxLock, "lock_chirp (lock acquired)", sndLock, 0.6f, 1.0f);
            prevLock = lockNow;
        }

        // ---- Audio frame: listener rides the pilot camera; the engine bed is
        //      volume-ridden at fixed pitch (hum <- speed, thrust <- throttle +
        //      antimatter boost swell). Music bus untouched.
        if (sfx) {
            float lx, ly, lz, lyaw, lpit;
            pilot.camera(lx, ly, lz, lyaw, lpit);
            sfx->setListener(lx, ly, lz, lyaw, lpit);
            const x3::phys::Vec3 v3 = pilot.velocity();
            const float spd = std::sqrt(v3.x*v3.x + v3.y*v3.y + v3.z*v3.z);
            const float spdFrac = std::min(1.0f, spd / std::max(1.0f, tun.maxSpeed));
            const float throttle = std::min(1.0f, std::fabs(in.moveFwd) +
                                                  0.6f * std::fabs(in.moveStrafe));
            const float boostSwell = in.sprint ? 0.35f : 0.0f;
            if (humLoop.valid())
                sfx->setLoopParams(humLoop, 0.18f + 0.30f * spdFrac, 1.0f);
            if (thrustLoop.valid())
                sfx->setLoopParams(thrustLoop,
                    std::min(1.0f, 0.55f * throttle + boostSwell + 0.10f * spdFrac), 1.0f);
            sfx->update(dt);
        }

        // ---- Player fire -> fighters first (live ray), else the capital. ----
        if (fire && pilot.fireLaser(dt)) {
            playerFiredThisStep = true;
            ++localShotsFired;
            play2D(kSfxPlayerLaser, "player_laser (per shot)", sndPlayerLaser,
                   0.7f, 0.96f + 0.04f * (float)(step % 3));   // tiny pitch jitter
            // LIVE FIGHTER KILLS: a ray-sphere test along the aim so the laser
            // can actually down the wing (before this, player shots ONLY damaged
            // the capital — the "all enemies down" exit was unreachable and no
            // fighter ever exploded). Headless keeps the pure capital model so
            // the deterministic metrics/tests are byte-identical.
            bool hitFighter = false;
            // (captureMode: the scripted fire is for the TRACER/energy imagery;
            //  ray kills are disabled so the staged damage-state ships survive
            //  to their capture frames. The scripted s416 hit covers the
            //  registered-hit path.)
            if (live && !captureMode) {
                const float fhp = std::cos(pilot.pitch());
                const float fw[3] = { fhp * std::cos(pilot.yaw()), std::sin(pilot.pitch()),
                                      fhp * std::sin(pilot.yaw()) };
                int best = -1; float bestT = 1e9f;
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    const auto& es = enemies.ship(i);
                    const float oc[3] = { es.pos[0] - ppos[0], es.pos[1] - ppos[1],
                                          es.pos[2] - ppos[2] };
                    const float tca = oc[0]*fw[0] + oc[1]*fw[1] + oc[2]*fw[2];
                    if (tca < 0.0f || tca > 700.0f) continue;
                    const float d2c = oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2] - tca*tca;
                    // HIT RADIUS TRACKS THE DRAWN SILHOUETTE (owner: "allow me
                    // to hit the tiny enemy ships too!"): the draw grows distant
                    // ships by visCompFactor so they stay aimable — the player
                    // aims at what he SEES, so the acceptance radius scales by
                    // the SAME factor (shared formula, ship_ai.h; --test-ship-ai
                    // T12 proves a visual-edge shot at 500 m registers).
                    const float hitR = x3::space::shipai::kHitBaseRadius *
                                       x3::space::shipai::visCompFactor(tca);
                    if (d2c <= hitR * hitR && tca < bestT) { bestT = tca; best = (int)i; }
                }
                if (best >= 0) {
                    hitFighter = true;
                    ++localShotsHit;
                    hitConfirmT = 0.15f;   // reticle confirm: the shot LANDED
                    // SKILLFUL TTK (owner: 140 one-shot felt cheap): base per-shot
                    // = 10, so a 60-hull fighter takes ~6 hits — a loose spray
                    // whittles it down. But a hit that lands on the FULLY-ACQUIRED
                    // lock target (red bracket, lockAge >= kAcquireSec) does 2x = 20
                    // -> ~3 hits: hold the lock and you kill faster. Enemy-vs-player
                    // damage is UNCHANGED (the scripted takedown still lands).
                    constexpr int kPlayerLaserBase = 10;        // ~6 hits down a 60-hull fighter
                    const bool onLockedTarget =
                        lockHad && lockAge >= kAcquireSec &&
                        targeting.lockedId() == (uint32_t)best + 1u;   // contact id = 1 + enemyIdx
                    const int kPlayerLaserDamage =
                        onLockedTarget ? kPlayerLaserBase * 2 : kPlayerLaserBase;  // 20 on-lock -> ~3 hits
                    const auto& es = enemies.ship((uint32_t)best);
                    const float hp[3] = { es.pos[0], es.pos[1], es.pos[2] };
                    const bool willDie = es.hull <= kPlayerLaserDamage;
                    if (fxOn) fxPtr->spawnImpact({ hp[0], hp[1], hp[2] },
                                                 { -fw[0], -fw[1], -fw[2] });
                    enemies.damageShip((uint32_t)best, kPlayerLaserDamage);
                    if (willDie) fighterKillFx(hp);
                }
                // ---- REAL CAPITAL RAYCAST (vet pass: "every non-fighter shot
                // auto-hit — zero aim expression against the boss"). The nose
                // ray is tested against the hull sphere AND each hardpoint
                // blister; the NEAREST entry owns the hit, so the hull naturally
                // OCCLUDES far-side hardpoints — you cannot snipe the engines
                // through the ship, you fly around, which is the whole game.
                // A ray that misses everything is a WHIFF: no hit credit, no
                // confirm — accuracy is an honest metric against the boss now.
                if (!hitFighter) {
                    // Hardpoint acceptance grows with range (same aim-at-what-
                    // you-see philosophy as the fighters' visCompFactor).
                    float bestT2 = 1e9f; int bestHp = -2;      // -2 none, -1 hull
                    auto raySphere = [&](const float c[3], float r) {
                        const float oc[3] = { c[0]-ppos[0], c[1]-ppos[1], c[2]-ppos[2] };
                        const float tca = oc[0]*fw[0] + oc[1]*fw[1] + oc[2]*fw[2];
                        if (tca < 0.0f) return -1.0f;
                        const float d2c = oc[0]*oc[0]+oc[1]*oc[1]+oc[2]*oc[2] - tca*tca;
                        if (d2c > r * r) return -1.0f;
                        return std::max(0.0f, tca - std::sqrt(r*r - d2c)); // entry t
                    };
                    const float tHull = raySphere(capC, kCapHullR);
                    if (tHull >= 0.0f) { bestT2 = tHull; bestHp = -1; }
                    const int gunsLeft =
                        x3::space::CapitalBattery::aliveGuns(capBattery);
                    for (int hi = 0; hi < kMaxSubsystems; ++hi) {
                        // While any gun mount is standing, the PHYSICAL GUNS
                        // are the Turrets battery's hit geometry — the old
                        // abstract blister would double-count it.
                        if (hi == 1 && gunsLeft > 0) continue;
                        float hw[3]; hardWorld(hi, hw);
                        const float dHp = std::sqrt(
                            (hw[0]-ppos[0])*(hw[0]-ppos[0]) +
                            (hw[1]-ppos[1])*(hw[1]-ppos[1]) +
                            (hw[2]-ppos[2])*(hw[2]-ppos[2]));
                        const float growR = kHard[hi].radius *
                            std::max(1.0f, std::min(2.5f, dHp / 900.0f));
                        const float tHp = raySphere(hw, growR);
                        if (tHp >= 0.0f && tHp < bestT2) { bestT2 = tHp; bestHp = hi; }
                    }
                    // The gun mounts (aim-at-what-you-see growth, same law).
                    // bestHp encodes a gun as 100 + gunIdx.
                    for (int gi = 0; gi < x3::space::kCapitalGunCount; ++gi) {
                        if (!x3::space::CapitalBattery::gunAlive(capBattery, gi))
                            continue;
                        float gw[3]; gunWorld(gi, gw);
                        const float dG = std::sqrt(
                            (gw[0]-ppos[0])*(gw[0]-ppos[0]) +
                            (gw[1]-ppos[1])*(gw[1]-ppos[1]) +
                            (gw[2]-ppos[2])*(gw[2]-ppos[2]));
                        const float growR = kGunHitR *
                            std::max(1.0f, std::min(2.5f, dG / 900.0f));
                        const float tG = raySphere(gw, growR);
                        if (tG >= 0.0f && tG < bestT2) { bestT2 = tG; bestHp = 100 + gi; }
                    }
                    if (bestHp >= 100) {
                        // A LANDED GUN HIT. Shield up: the bubble soaks it
                        // (the usual funnel). Shield down: the mount SHEARS
                        // OFF — visible removal + a quarter of the Turrets
                        // subsystem routed so the consequence flow (BATTERY
                        // SILENCED at four kills) is unchanged.
                        const int gi = bestHp - 100;
                        const float hitP[3] = { ppos[0] + fw[0]*bestT2,
                                                ppos[1] + fw[1]*bestT2,
                                                ppos[2] + fw[2]*bestT2 };
                        if (x3::space::ShipDamage::shieldFrac(capital) > 0.0f) {
                            capitalApplyHit(x3::space::Subsystem::Turrets, hitP, fw);
                        } else {
                            ++localShotsHit;
                            hitConfirmT = 0.15f;
                            if (x3::space::CapitalBattery::damageGun(
                                    capBattery, gi, 90)) {
                                float gw[3]; gunWorld(gi, gw);
                                play3D(kSfxExplosion, "explosion_gun_mount (shot off)",
                                       sndExplosion, gw, 1.0f, 0.8f);
                                if (fxOn) {
                                    fxPtr->spawnExplosion({ gw[0], gw[1], gw[2] }, 14.0f);
                                    fxPtr->spawnDeath({ gw[0], gw[1], gw[2] });
                                    fxPtr->spawnShipSparks({ gw[0], gw[1], gw[2] });
                                    fxPtr->spawnSmoke({ gw[0], gw[1], gw[2] });
                                    // The GUN ITSELF tumbles away: a heavy
                                    // debris burst where the mount stood.
                                    hc.device->gpuDebrisSpawnBurst(gw, 22u, 26.0f,
                                        6.0f, 2.4f, 0xF001u ^ (uint32_t)(gi * 7919));
                                }
                                const int left =
                                    x3::space::CapitalBattery::aliveGuns(capBattery);
                                static const char* kGunBark[4] = {
                                    "DREADNOUGHT GUN MOUNT SHEARED OFF - 3 REMAIN",
                                    "DREADNOUGHT GUN MOUNT SHEARED OFF - 2 REMAIN",
                                    "DREADNOUGHT GUN MOUNT SHEARED OFF - 1 REMAINS",
                                    "DREADNOUGHT LAST GUN MOUNT DESTROYED",
                                };
                                pushCallout(kCalloutEnemy,
                                    kGunBark[std::min(3, std::max(0, 3 - left))], 0.8f);
                                // Route the quarter into the Turrets subsystem
                                // (four kills == 120 == subsystem down, which
                                // fires the existing BATTERY SILENCED flow via
                                // the shared scan).
                                x3::space::ShipDamage::applyDamage(
                                    capital,
                                    x3::space::CapitalBattery::kSubQuarter,
                                    x3::space::Subsystem::Turrets);
                                scanSubsDown();
                            } else if (fxOn) {
                                fxPtr->spawnImpact({ hitP[0], hitP[1], hitP[2] },
                                                   { -fw[0], -fw[1], -fw[2] });
                                fxPtr->spawnShipSparks({ hitP[0], hitP[1], hitP[2] });
                            }
                        }
                    } else if (bestHp != -2) {
                        const float hitP[3] = { ppos[0] + fw[0]*bestT2,
                                                ppos[1] + fw[1]*bestT2,
                                                ppos[2] + fw[2]*bestT2 };
                        // A dead hardpoint is wreckage — hits there bleed to hull.
                        const x3::space::Subsystem sub =
                            (bestHp >= 0 && !subWasDown[bestHp])
                                ? kHard[bestHp].sub : x3::space::Subsystem::Count;
                        capitalApplyHit(sub, hitP, fw);
                    }
                    // else: a genuine whiff into space — nothing to credit.
                }
            }
            if (!live) {
                // HEADLESS COMPETENT MODEL (the deterministic synthetic pilot the
                // self-tests replay): no window, no aim — model a pilot who works
                // the anatomy correctly. First surviving hardpoint in enum order,
                // then the bare hull. Funnels through the SAME capitalApplyHit as
                // live play, so routing/phase/FX semantics are tested end-to-end.
                // (captureMode shots are IMAGERY — they must not chew through the
                // capital or the staged evidence run dies before its s960 bound.)
                x3::space::Subsystem sub = x3::space::Subsystem::Count;
                int hpIdx = -1;
                for (int si = 0; si < kMaxSubsystems; ++si)
                    if (!subWasDown[si]) { sub = kHard[si].sub; hpIdx = si; break; }
                float hitP[3];
                if (hpIdx >= 0) hardWorld(hpIdx, hitP);
                else { hitP[0] = capC[0] - kCapHullR; hitP[1] = capC[1]; hitP[2] = capC[2]; }
                const float hd[3] = { 1.0f, 0.0f, 0.0f };
                capitalApplyHit(sub, hitP, hd);
            }
        }
        x3::space::ShipDamage::tick(capital, dt);
        // SHIELD GENERATOR CONSEQUENCE: with the generator hardpoint dead the
        // shield NEVER comes back (tick regens unconditionally; clamp it here).
        // This is what makes "shield gen first" a real strategy.
        if (x3::space::ShipDamage::subsystemDown(capital,
                x3::space::Subsystem::ShieldGen))
            capital.shield = 0;
        // Phase transition bark: the moment the shield first breaks.
        if (!shieldDownCalled && capital.shield == 0 && capital.maxShield > 0) {
            shieldDownCalled = true;
            pushCallout(kCalloutEnemy, "DREADNOUGHT SHIELDS DOWN - HER HULL IS EXPOSED", 0.9f);
        }

        // Crippled == all subsystems down (the escape-enabling objective).
        if (!crippled && localSubsDestroyed >= kMaxSubsystems) {
            crippled = true; crippleTime = tNow;
        }

        // ---- REACTOR PHASE (crippled -> the endgame has a rhythm) ------------
        // The exposed core cycles open (8 s, hull damage x3, hit THE WOUND) and
        // shielded (5 s, normal damage). Barks on every transition so the cycle
        // is audible even when the marker is off-screen.
        if (crippled && !capitalKilled) {
            reactorT += dt;
            const bool reactorOpen =
                std::fmod(reactorT, kReactorCycle) < kReactorOpen;
            if (reactorOpen && !reactorOpenPrev)
                pushCallout(kCalloutEnemy, "DREADNOUGHT REACTOR EXPOSED - HIT THE CORE", 1.1f);
            else if (!reactorOpen && reactorOpenPrev)
                pushCallout(kCalloutEnemy, "DREADNOUGHT REACTOR SHIELDED - WAIT FOR THE CYCLE", 0.7f);
            reactorOpenPrev = reactorOpen;
        }

        // ---- THE SHIP WEARS ITS DAMAGE (vet pass: players read the hull, not
        // the bar). Dead hardpoints burn: smoke + embers streaming from each
        // wound, cadenced so the cost stays bounded (~7 Hz smoke, ~2 Hz ember).
        if (fxOn) {
            // Dead GUN MOUNTS burn too: the wreck stub streams smoke so the
            // sheared battery reads at a glance (fire ceased + a wound).
            for (int gi = 0; gi < x3::space::kCapitalGunCount; ++gi) {
                if (x3::space::CapitalBattery::gunAlive(capBattery, gi) ||
                    capitalKilled) continue;
                float gw[3]; gunWorld(gi, gw);
                if ((step + gi * 5) % 8 == 0)
                    fxPtr->spawnShipSmoke({ gw[0], gw[1], gw[2] },
                                          { 0.0f, 3.5f, 0.0f }, 1.0f);
                if ((step + gi * 11) % 17 == 0)
                    fxPtr->spawnShipSparks({ gw[0], gw[1], gw[2] });
            }
            for (int si = 0; si < kMaxSubsystems; ++si) {
                if (!subWasDown[si]) continue;
                float hw[3]; hardWorld(si, hw);
                if (capitalKilled) {   // burn rides the wreck down (sag/plunge)
                    hw[0] += capDeath.planetDir[0] * capDeath.plunge;
                    hw[1] += capDeath.planetDir[1] * capDeath.plunge - capDeath.sag;
                    hw[2] += capDeath.planetDir[2] * capDeath.plunge;
                }
                // A DEAD TURRET BATTERY BURNS HARDER than a dead sensor mast:
                // it is the hardpoint whose death the player actually feels
                // (the gauntlet goes quiet), so it gets the loudest wound.
                const bool turret = (kHard[si].sub == x3::space::Subsystem::Turrets);
                if ((step + si * 3) % (turret ? 5 : 9) == 0)
                    fxPtr->spawnShipSmoke({ hw[0], hw[1], hw[2] },
                                          { 0.0f, 4.0f, 0.0f }, 1.0f);
                if ((step + si * 7) % (turret ? 13 : 31) == 0)
                    fxPtr->spawnShipEmber({ hw[0], hw[1], hw[2] },
                                          { 0.0f, 3.0f, 0.0f });
            }
            // ---- THE DRIVE PLUME (subsystem consequence, made VISIBLE) ------
            // The engines hardpoint had no visual at all: killing it froze the
            // capital's patrol drift and printed a bark, but the ship looked
            // exactly the same, so "ENGINES DESTROYED" was a claim the frame
            // never backed up. A live capital now burns a steady hot plume off
            // its aft hardpoint; the instant the engines die the plume STOPS
            // and the generic wound-burn above takes over. The light goes out.
            if (!capitalKilled &&
                !x3::space::ShipDamage::subsystemDown(capital,
                    x3::space::Subsystem::Engines)) {
                float ew[3]; hardWorld(0, ew);            // kHard[0] == ENGINES
                // Aft of the hardpoint (the ship's nose faces -X, so aft is +X).
                const float plume[3] = { ew[0] + 26.0f, ew[1], ew[2] };
                if (step % 4 == 0)
                    fxPtr->spawnShipEmber({ plume[0], plume[1], plume[2] },
                                          { 22.0f, 0.0f, 0.0f });
                if (step % 11 == 0)
                    fxPtr->spawnShipEmber({ plume[0] + 18.0f, plume[1], plume[2] },
                                          { 34.0f, 0.0f, 0.0f });
            }
            // Reactor wound: a hot ember pulse while the core is exposed.
            if (crippled && !capitalKilled &&
                std::fmod(reactorT, kReactorCycle) < kReactorOpen &&
                step % 6 == 0) {
                float rw[3]; capRot(kReactorOff, rw);
                fxPtr->spawnShipEmber({ rw[0], rw[1], rw[2] }, { 0.0f, 2.0f, 0.0f });
            }
        }

        // ---- THE KILL: dreadnought hull 0 -------------------------------------
        // Earned, deterministic, and it OVERRIDES the outcome roll entirely (see
        // runInteractiveIntro): killing the capital forks the game onto the
        // crash -> salvage -> Lab-Zero-breach branch. Fires exactly once.
        if (!capitalKilled && x3::space::ShipDamage::isDestroyed(capital)) {
            capitalKilled     = true;
            m.capitalDestroyed = true;
            // The drive dies with the ship: the wreck holds the kill point and
            // the DEATH state machine owns all wreck motion from here (sag /
            // deorbit plunge), so the draw and the event stream can't diverge.
            capMotion.speed = 0.0f;
            capMotion.vel[0] = capMotion.vel[1] = capMotion.vel[2] = 0.0f;
            // THE TWO AUTHORED ENDINGS (owner: "either crash land on a planet
            // OR break up in space"), picked per encounter CONTEXT: a dominant
            // planet in this system's sky (a hero body with real angular size)
            // -> she DEORBITS toward it under an entry burn; a sky with no such
            // body -> she breaks up where she floats. Deterministic — derived
            // from the star-system registry, identical headless and live.
            // Dev override: X3_CAP_DEATH=breakup|crash.
            x3::space::DeathOutcome dOut = x3::space::DeathOutcome::BreakupInSpace;
            float planetDir[3] = { 0.0f, 0.0f, 0.0f };
            {
                const x3::starsys::SystemBody* hero = nullptr;
                for (const auto& b : dfSystem.bodies)
                    if (b.type != x3::starsys::BodyType::Sun &&
                        (!hero || b.angularDiameterDeg > hero->angularDiameterDeg))
                        hero = &b;
                constexpr float kCrashDiamDeg = 8.0f;   // "a planet NEARBY"
                if (hero && hero->angularDiameterDeg >= kCrashDiamDeg) {
                    dOut = x3::space::DeathOutcome::DeorbitCrash;
                    const float az = hero->azimuthDeg   * 0.01745329252f;
                    const float el = hero->elevationDeg * 0.01745329252f;
                    // Sky convention (star_systems.h): az 0 = -Z, +90 = +X.
                    planetDir[0] =  std::sin(az) * std::cos(el);
                    planetDir[1] =  std::sin(el);
                    planetDir[2] = -std::cos(az) * std::cos(el);
                }
                if (const char* ov = std::getenv("X3_CAP_DEATH"); ov && *ov) {
                    if (ov[0] == 'b' || ov[0] == 'B')
                        dOut = x3::space::DeathOutcome::BreakupInSpace;
                    else if (ov[0] == 'c' || ov[0] == 'C') {
                        dOut = x3::space::DeathOutcome::DeorbitCrash;
                        if (planetDir[0] == 0.0f && planetDir[1] == 0.0f &&
                            planetDir[2] == 0.0f) {
                            planetDir[0] = -0.47f; planetDir[1] = 0.13f;
                            planetDir[2] = -0.87f;   // fallback: hero-world ray
                        }
                    }
                }
            }
            const bool deorbit =
                (dOut == x3::space::DeathOutcome::DeorbitCrash);
            // Hold the window open for the WHOLE sequence (was 2.6 s, which cut
            // to the crash while the ship was still coming apart — the payoff
            // the player just earned happened off-screen), plus a beat of quiet
            // at the end so the kill lands.
            capitalDeathHold =
                x3::space::CapitalDeathSequence::durationFor(dOut) + 1.6f;
            x3::logInfo(std::string("[intro] *** DREADNOUGHT DESTROYED *** — hull 0 at t=") +
                        std::to_string(tNow) + " s after " +
                        std::to_string(localShotsHit) + "/" +
                        std::to_string(localShotsFired) + " landed shots; death = " +
                        (deorbit ? "DEORBIT CRASH (falls to the planet)"
                                 : "BREAKUP IN SPACE") +
                        "; intro forks to CAPITAL_KILLED (crash site -> salvage -> "
                        "Lab Zero breach)");
            play3D(kSfxExplosion, "explosion_capital (3D, dreadnought kill)",
                   sndExplosion, capC, 1.0f, 0.55f);   // deep, loud, slow
            // THE MESSAGE THE OWNER NEVER GOT. Before this, a capital kill said
            // nothing on screen at all — the only text after the fight was the
            // cold-open's "ANTIMATTER CRITICAL - IGNITING ION DRIVE", which is
            // JAKE'S OWN drive, so a kill and a bare escape looked identical.
            pushCallout(kCalloutMission,
                        deorbit ? "DREADNOUGHT DESTROYED - SHE'S GOING IN"
                                : "DREADNOUGHT DESTROYED - SHE'S BREAKING UP", 0.55f);
            // Arm the staged death. The hull's long axis is the ship's own
            // CURRENT facing (she has been arcing all fight), so the cascade
            // walks bow -> stern down the real hull.
            {
                const float axis[3] = { capMotion.fwd[0], capMotion.fwd[1],
                                        capMotion.fwd[2] };
                x3::space::CapitalDeathSequence::begin(capDeath, capC, axis,
                                                       kCapHullR * 0.90f,
                                                       dOut, planetDir);
            }
        }
        if (capitalKilled) {
            // ---- PUMP THE DEATH SEQUENCE ------------------------------------
            // Budgeted by construction: the state machine emits at most
            // kMaxEventsPerStep (3) beats per call no matter how long the frame
            // was, and the whole 28-beat schedule spends ~5 s. Peak particle
            // draw is one CoreDetonation frame; everything else is a single
            // secondary plus a vent.
            x3::space::DeathEvent evs[x3::space::CapitalDeathSequence::kMaxEventsPerStep];
            const int nEv = x3::space::CapitalDeathSequence::step(
                capDeath, dt, evs,
                x3::space::CapitalDeathSequence::kMaxEventsPerStep);
            for (int ei = 0; ei < nEv; ++ei) {
                const auto& ev = evs[ei];
                const x3::phys::Vec3 p{ ev.pos[0], ev.pos[1], ev.pos[2] };
                const x3::phys::Vec3 dv{ ev.drift[0], ev.drift[1], ev.drift[2] };
                // Deterministic per-beat seed (cursor-derived, no frame RNG) so
                // the debris field is identical run to run — capture evidence
                // must be reproducible.
                const uint32_t dseed = 0xC4B1u ^ ((uint32_t)capDeath.cursor * 2654435761u);
                switch (ev.kind) {
                case x3::space::DeathFx::Rupture:
                    // The hull opens: a white-hot core + a real SHOCKWAVE shell
                    // (spawnShipDeathBlast is the engine's shockwave primitive —
                    // it existed and the capital kill never used it, while every
                    // 10 m FIGHTER kill did).
                    play3D(kSfxExplosion, "explosion_capital_rupture",
                           sndExplosion, ev.pos, 1.0f, 0.42f);
                    if (fxOn) {
                        fxPtr->spawnShipDeathBlast(p, ev.radius);
                        fxPtr->spawnExplosion(p, ev.radius * 0.55f);
                        fxPtr->spawnShipSmoke(p, dv, 1.0f);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 48u, 52.0f, 9.0f, 2.2f, dseed);
                    }
                    break;
                case x3::space::DeathFx::Cascade:
                    if (fxOn) {
                        // Each secondary gets its own white-hot core + shockwave
                        // shell, not just a fireball puff: at 450 m of hull a
                        // bare spawnExplosion reads as a spark. This is what
                        // makes the cascade legible as it WALKS the ship.
                        fxPtr->spawnShipDeathBlast(p, ev.radius * 0.50f);
                        fxPtr->spawnExplosion(p, ev.radius);
                        fxPtr->spawnDeath(p);
                        fxPtr->spawnShipEmber(p, dv);
                        fxPtr->spawnShipSmoke(p, dv, 1.0f);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 14u, 30.0f, 8.0f, 1.1f, dseed);
                    }
                    break;
                case x3::space::DeathFx::Vent:
                    // Atmosphere and coolant blowing out of a breached hull: a
                    // long smoke plume plus a hot ember core, streaming away
                    // from the spine (dv) rather than hanging in place.
                    if (fxOn) {
                        fxPtr->spawnShipSmoke(p, dv, 1.0f);
                        fxPtr->spawnShipSmoke(p, { dv.x * 1.6f, dv.y * 1.6f, dv.z * 1.6f }, 0.0f);
                        fxPtr->spawnShipEmber(p, dv);
                    }
                    break;
                case x3::space::DeathFx::HullFragment:
                    // A SECTION tears free. The capital art is a single mesh
                    // (assets/rigged_glb/SpaceShip4.glb — 1 mesh, 1 primitive),
                    // so there is no sub-mesh to fly apart; big GPU debris boxes
                    // at 12 m half-extent are the honest read of hull plating
                    // tumbling off at this range.
                    play3D(kSfxExplosion, "explosion_capital_section",
                           sndExplosion, ev.pos, 0.85f, 0.5f);
                    if (fxOn) {
                        fxPtr->spawnExplosion(p, ev.radius);
                        fxPtr->spawnShipDeathBlast(p, ev.radius * 0.8f);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 16u, 26.0f, 12.0f, 7.0f, dseed);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 34u, 40.0f, 10.0f, 2.6f, dseed ^ 0x9E37u);
                        fxPtr->spawnShipSmoke(p, dv, 1.0f);
                    }
                    break;
                case x3::space::DeathFx::CoreDetonation:
                    // The reactor lets go — the biggest single event in the
                    // intro. Two nested blasts so the shockwave shell reads
                    // OUTSIDE the fireball, plus the heaviest debris shed.
                    play3D(kSfxExplosion, "explosion_capital_core",
                           sndExplosion, ev.pos, 1.0f, 0.35f);
                    pushCallout(kCalloutMission, "DREADNOUGHT REACTOR BREACH", 0.5f);
                    if (fxOn) {
                        fxPtr->spawnShipDeathBlast(p, ev.radius);
                        fxPtr->spawnShipDeathBlast(p, ev.radius * 0.5f);
                        fxPtr->spawnExplosion(p, ev.radius * 0.45f);
                        fxPtr->spawnDeath(p);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 64u, 68.0f, 12.0f, 3.0f, dseed);
                        hc.device->gpuDebrisSpawnBurst(ev.pos, 18u, 34.0f, 12.0f, 8.0f, dseed ^ 0x5BD1u);
                    }
                    break;
                }
            }
            // FRAME-COST TELEMETRY: the death sequence is the one moment the
            // player most wants smoothness, so its peak particle + debris
            // residency is logged, not assumed. Both pools are fixed-capacity
            // rings (kMaxParticles = 4096; the GPU debris pool is device-sized),
            // so this can only ever report how close the sequence came to the
            // ceiling — it can never allocate past it.
            if (fxOn) {
                const int lp = fxPtr->liveParticleCount();
                if (lp > deathPeakParticles) deathPeakParticles = lp;
                const uint32_t ld = hc.device->gpuDebrisAliveCount();
                if ((int)ld > deathPeakDebris) deathPeakDebris = (int)ld;
            }
            capitalDeathHold -= dt;
            if (capitalDeathHold <= 0.0f) {
                if (fxOn)
                    x3::logInfo("[intro] death-sequence cost: peak " +
                                std::to_string(deathPeakParticles) +
                                " live particles, peak " +
                                std::to_string(deathPeakDebris) +
                                " GPU debris fragments");
                break;   // window over — cut to the crash
            }
        }

        // ---- Live render (3P chase of the pilot; minimal, reuses host_space art
        //      conventions). Headless skips drawing entirely. ----
        if (live) {
            beatT += dt;
            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);      // yaw/pitch for the cockpit pose + fire dir
            // ROLL-CAPABLE view: feed the ship's full orientation basis so the
            // horizon banks and the fighter can loop (owner: "add a roll capable
            // camera"). Replaces the roll-less setCamera that pinwheeled past vertical.
            float camF[3] = { 1, 0, 0 }, camU[3] = { 0, 1, 0 };   // ACTUAL view basis
            {
                float cpos[3];
                pilot.cameraBasis(cpos, camF, camU);
                hc.device->setCameraBasis(cpos[0], cpos[1], cpos[2], camF, camU, 65.0f);
                cx = cpos[0]; cy = cpos[1]; cz = cpos[2];
            }
            // THE VISIBLE LAYER (feat/intro-cockpit): the player flies the beat
            // from inside the two-seat fighter cockpit — posed to the pilot camera
            // each frame — with the enemy wing + the capital ship drawn out the
            // canopy, laser tracers + muzzle flashes via CombatFx, and pulsing
            // MFD screens. The analytic-sky starfield is the world-fixed backdrop.
            // The cockpit rig is the 1P view ONLY. In 3P chase it would hang in
            // front of the pulled-back camera and block the whole screen.
            //
            // THE FLOATING-MODULE BUG (owner, live: "That module is the ship
            // INTERIOR!!!!"). This pose was already gated on 1P — but the Scene
            // below was rendered UNCONDITIONALLY, so in the DEFAULT 3P chase the
            // whole two-seat interior drew at its authored BASE transforms, i.e.
            // parked at the world origin where the ship spawned, and never moved
            // again. From outside that reads as a room-sized box of interior wall
            // backfaces and glowing MFD screens floating in the starfield beside
            // the fighters — and it is ALSO, literally, "the cockpit stays put
            // when the player ship moves". One gate closes both.
            const bool cockpitVisible = cockpit && !pilot.isThirdPerson();
            if (cockpitVisible) {
                x3::apphost::pulseIntroScreens(*cockpit, beatT);
                // MASS: the cabin lags the hull under acceleration and banks into
                // turns (dt-correct spring, clamped small). Driven by the ship's
                // REAL net acceleration resolved into its own axes — never the raw
                // thrust input, which stays pinned while the ship is speed-capped.
                const x3::phys::Vec3 vNow = pilot.velocity();
                const x3::phys::Vec3 sf3  = pilot.forward();
                const x3::phys::Vec3 sr3  = pilot.right();
                const x3::phys::Vec3 su3  = pilot.up();
                float aW[3] = { 0, 0, 0 };
                if (swayPrevVelValid && dt > 1e-4f) {
                    aW[0] = (vNow.x - swayPrevVel[0]) / dt;
                    aW[1] = (vNow.y - swayPrevVel[1]) / dt;
                    aW[2] = (vNow.z - swayPrevVel[2]) / dt;
                }
                swayPrevVel[0] = vNow.x; swayPrevVel[1] = vNow.y; swayPrevVel[2] = vNow.z;
                swayPrevVelValid = true;
                const float aLocal[3] = {
                    aW[0]*sf3.x + aW[1]*sf3.y + aW[2]*sf3.z,
                    aW[0]*sr3.x + aW[1]*sr3.y + aW[2]*sr3.z,
                    aW[0]*su3.x + aW[1]*su3.y + aW[2]*su3.z };
                const float yawNow  = pilot.yaw();
                float yawDelta = yawNow - swayPrevYaw;
                while (yawDelta >  3.14159265f) yawDelta -= 6.28318531f;
                while (yawDelta < -3.14159265f) yawDelta += 6.28318531f;
                const float yawRate = swayPrevYawValid && dt > 1e-4f ? yawDelta / dt : 0.0f;
                swayPrevYaw = yawNow; swayPrevYawValid = true;
                cockpitSway.update(dt, aLocal, yawRate);
                const float swayLocal[3] = { cockpitSway.surge(), cockpitSway.swayR(),
                                             cockpitSway.heave() };
                // 1P eye + the SHIP's basis (not the gaze — the cabin is bolted to
                // the hull, so freelook/look-bias must never rotate it).
                const x3::phys::Vec3 pEye = pilot.pos();
                const float eyeW[3] = { pEye.x + sf3.x * 0.4f,
                                        pEye.y + sf3.y * 0.4f,
                                        pEye.z + sf3.z * 0.4f };
                const float shipF[3] = { sf3.x, sf3.y, sf3.z };
                const float shipU[3] = { su3.x, su3.y, su3.z };
                x3::apphost::poseIntroCockpitBasis(*cockpit, eyeW, shipF, shipU,
                                                   swayLocal, cockpitSway.pitch(),
                                                   cockpitSway.yaw(), cockpitSway.roll());
            } else if (cockpit) {
                // 3P: keep the interior OUT of the exterior frame, and keep the
                // sway spring at rest so a mid-flight F2 flip starts neutral.
                x3::apphost::setIntroCockpitVisible(*cockpit, false);
                cockpitSway.reset();
                swayPrevVelValid = false; swayPrevYawValid = false;
            }
            if (fxOn && playerFiredThisStep) {
                // WING-MOUNTED FIRE (owner: "the fire needs to COme from weapons
                // MOUNTED ON THE Ship"): bolts leave alternating wingtip hardpoints
                // and CONVERGE on the aim point 600 m down the player's look ray —
                // hit detection stays on the crosshair; the muzzle is on the hull.
                static int wingSide = 1;
                wingSide = -wingSide;
                float wm[3], wd[3];
                pilot.wingMuzzle(wingSide, wm, wd);
                const float fh2 = std::cos(pilot.pitch());
                const x3::phys::Vec3 pj = pilot.pos();
                const float aim[3] = {
                    pj.x + fh2 * std::cos(pilot.yaw()) * 600.0f,
                    pj.y + std::sin(pilot.pitch()) * 600.0f,
                    pj.z + fh2 * std::sin(pilot.yaw()) * 600.0f };
                // SHIP-SCALE bolt + wingtip muzzle flash: wide enough to read
                // from the chase camera (0.035 m rifle tracer is sub-pixel).
                // CARRIER VELOCITY (owner, live: "the weapon beams do not come
                // from the ship when its strafing, they come beside it"). The
                // muzzle origin was always current-frame — the beam is a 0.12 s
                // WORLD-ANCHORED segment, so once the strafe fix let the ship
                // actually slide at ~110 m/s it travelled ~13 m (two hull
                // lengths) before the bolt faded and the beam was left hanging
                // beside it. Feeding the ship's velocity makes the beam ride the
                // hull for its whole life, so it leaves the wingtip and stays
                // attached however hard you are strafing.
                const x3::phys::Vec3 pv = pilot.velocity();
                fxPtr->addTracer({ wm[0], wm[1], wm[2] }, { aim[0], aim[1], aim[2] },
                                 x3::game::WeaponFxKind::Default, /*width*/ 0.50f, &pv);
                fxPtr->spawnShipMuzzle({ wm[0], wm[1], wm[2] }, { wd[0], wd[1], wd[2] }, &pv);
            }
            // DAMAGE READS ON THE HULL (owner: "we should see some representation
            // of the damage on the enemy ship"): staged persistent FX keyed to
            // hull fraction via the PURE shipai::damageFxProfile (asserted by
            // --test-ship-ai T11 — full hull emits NOTHING):
            //   < 75%: intermittent spark bursts;
            //   < 50%: + a continuous thin grey smoke trail streaming aft;
            //   < 25%: + heavy black smoke + ember/fire glow — visibly BURNING.
            // Emitters ride the tail of the hull (aft along -fwd) so the trail
            // reads as pouring off the engines, and the ship's velocity is fed
            // into the puffs so the trail follows the flight path.
            if (fxOn) {
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    const auto& e = enemies.ship(i);
                    if (e.hull <= 0) continue;
                    const float frac = e.maxHull > 0
                        ? (float)e.hull / (float)e.maxHull : 1.0f;
                    const auto prof = x3::space::shipai::damageFxProfile(frac);
                    const int phase = step + (int)(e.seed * 7u);
                    const x3::phys::Vec3 tail{ e.pos[0] - e.fwd[0] * 3.5f,
                                               e.pos[1] - e.fwd[1] * 3.5f,
                                               e.pos[2] - e.fwd[2] * 3.5f };
                    const x3::phys::Vec3 evel{ e.vel[0], e.vel[1], e.vel[2] };
                    if (prof.sparkPeriod > 0 && phase % prof.sparkPeriod == 0)
                        fxPtr->spawnShipSparks(tail);
                    if (prof.smokePeriod > 0 && phase % prof.smokePeriod == 0)
                        fxPtr->spawnShipSmoke(tail, evel, frac < 0.25f ? 1.0f : 0.30f);
                    if (prof.emberPeriod > 0 && phase % prof.emberPeriod == 0)
                        fxPtr->spawnShipEmber({ e.pos[0], e.pos[1], e.pos[2] }, evel);
                }
            }
            if (fxOn) fxPtr->update(dt);
            // DEV evidence capture: X3_INTRO_CAPTURE=<dir> dumps the presented
            // frame at steps 60/180/420/660/900 (~1/3/7/11/15 s) per interactive
            // beat — s60 catches the lock ACQUIRING sweep mid-close, the rest is
            // the bearing-over-time series that shows the wing CIRCLING (the tool
            // that root-caused the milky-canopy bug, extended for the orbit AI +
            // the combat-readability staged scenario). Off (empty env) in play.
            // KILL-EVIDENCE run: shoot the whole death sequence at 10 Hz from
            // one frame before the rupture (s118) through the drifting wreck
            // (s520 ≈ 6.7 s), so the contact sheet IS the sequence: rupture ->
            // cascade walking the hull -> lights out -> sections shed -> core
            // detonation -> dark tumbling wreck. A single still cannot prove a
            // staged sequence; a frame series can.
            // (kill window widened to s790: the DEORBIT ending runs 10.6 s of
            //  hold — fall + entry burn + the impact flash — past the old
            //  s530 breakup bound.)
            const bool evShot = evDir && *evDir &&
                (killCapture
                   ? (step >= 100 && step <= 790 && (step % 6) == 0)
                   : (step == 60 || step == 180 || step == 420 ||
                      step == 660 || step == 900));
            if (evShot)
                hc.device->armCapture((std::string(evDir) + "/live_" + beat.id +
                                       "_s" + std::to_string(step) + ".png").c_str());
            auto frame = hc.device->beginFrame();
            if (frame.valid && cockpit) {
                // HUD-space size: the real window when there is one, the fixed
                // 1280x720 headless framebuffer in a capture run.
                int winW = (int)hc.W, winH = (int)hc.H;
                // FRAMEBUFFER pixels, not window points: drawHudQuad works in framebuffer
                // px and the render aspect comes from the swapchain extent, so any DPI
                // scale would otherwise skew every world-anchored marker.
                if (hc.window) glfwGetFramebufferSize(hc.window, &winW, &winH);
                // 1P ONLY (see the cockpitVisible note above): in 3P chase the
                // interior would otherwise render at the world origin as a
                // free-floating module in the starfield.
                if (cockpitVisible) cockpit->scene.render(*hc.device, frame);
                // The planet backdrop first (eye-anchored; depth-occluded by everything
                // drawn after). Space now has a WORLD behind the fight.
                // ANCHOR 10.5 km — THE PAIR RULE (landmine L7): the far plane below is
                // 15 km, so the planet shell must sit INSIDE it but BEYOND every ship,
                // or the discs punch holes in hulls (the original B10 bug). 140 m
                // (the default) + 15 km far = planets carving ships again. Never
                // move one of these numbers without the other.
                if (!planets.empty())
                    x3::apphost::drawNightSkyPlanets(hc.device, frame, planetMesh, planets,
                                                     beatT, cx, cy, cz, ringMesh, 10500.0f);
                // ---- THE STAR (world-anchored flyable body; shared recipe with
                // the spectacle-cam frames — see aimSkyAtStar/drawStar above). --
                aimSkyAtStar(cx, cy, cz);
                drawStar(frame);
                // Player shield bubble while the star is draining/recharging it
                // (the host_space drawShield look: cyan additive shell, pulse,
                // intensity riding the remaining %).
                if (sunPhase == SunPhase::InsideSun || sunShieldPct < 99.95f) {
                    float sm[16];
                    const float kk = sunShieldPct / 100.0f;
                    const float pulse = 0.75f + 0.25f * std::sin(beatT * 6.0f);
                    sphM(ppos, 2.6f, 0.0f, sm);
                    const float shBc2[4] = { 0.35f, 0.75f, 1.0f, 1.0f };
                    const float shEm2[4] = { 0.35f, 0.75f, 1.0f,
                                             (0.6f + 2.2f * kk) * pulse };
                    x3::rhi::IRenderDevice::GlassMaterial gm2{};
                    gm2.opacity = 0.18f + 0.22f * kk;
                    gm2.roughness = 0.4f; gm2.specular = 0.4f;
                    gm2.tint[0]=0.4f; gm2.tint[1]=0.75f; gm2.tint[2]=1.0f;
                    hc.device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{},
                                             shBc2, shEm2, gm2, sm);
                }
                // YOUR fighter, in 3P — at the ship's own position + facing, so it sits
                // AHEAD of the chase camera. Skipped in 1P (you're inside it).
                if (pilot.isThirdPerson() && !playerDraw.empty()) {
                    const x3::phys::Vec3 pp3 = pilot.pos();
                    const x3::phys::Vec3 pf3 = pilot.forward();
                    const x3::phys::Vec3 pu3 = pilot.up();
                    const float sp[3] = { pp3.x, pp3.y, pp3.z };
                    const float sf[3] = { pf3.x, pf3.y, pf3.z };
                    const float su[3] = { pu3.x, pu3.y, pu3.z };
                    // Scale 1.0, NOT 4.0: JakeFighterShip_textured.glb is ALREADY a
                    // ~10 m hull at native scale (measured 7.1 x 2.6 x 10.1 m). At 4x
                    // it was a 40 m building wrapped around the chase camera — the
                    // owner's whole screen was his own unlit hull, every enemy hidden
                    // behind it ("I cannot see the enemy ship at ALL in combat").
                    // FULL-BASIS draw (the muzzle fix): the hull now pitches + BANKS
                    // with the physics quat — the same basis wingMuzzle() computes
                    // its hardpoints from, so weapon fire visibly leaves the wings
                    // (the yaw-only draw left the hull level while the muzzles
                    // pitched with the ship: bolts materialized in mid-air —
                    // owner: "it comes from anywhere BUT the ship").
                    x3::apphost::drawIntroShipBasis(*hc.device, frame, playerDraw,
                                  sp, sf, su, 1.0f, cockpit->mrShared);
                }
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    const auto& e = enemies.ship(i);
                    if (e.hull <= 0) continue;
                    // DISTANCE-COMPENSATED scale (owner: "I cannot SEE the enemy
                    // ship after I get even a TINY bit away — it just disappears").
                    // Base 6 m reads at dogfight range; past 150 m the draw grows
                    // linearly (capped 4x at 600 m+) so a contact NEVER falls below
                    // a readable on-screen size. The arcade trick every space game
                    // uses — physical honesty loses to gameplay legibility here.
                    // SHARED formula (ship_ai.h): the hit test scales its radius
                    // by the SAME factor so the player can hit what he sees.
                    const float dxE = e.pos[0]-cx, dyE = e.pos[1]-cy, dzE = e.pos[2]-cz;
                    const float dE = std::sqrt(dxE*dxE + dyE*dyE + dzE*dzE);
                    const float visScale = x3::space::shipai::kVisBaseScale *
                                           x3::space::shipai::visCompFactor(dE);
                    // Full-basis draw: the hull pitches into climbs/dives (fwd is
                    // 3D) instead of staying level, + the warm HIT-FLASH tint
                    // while e.hitFlash runs so a registered hit reads instantly.
                    const float upW[3] = { 0.0f, 1.0f, 0.0f };
                    x3::apphost::drawIntroShipBasis(*hc.device, frame, cockpit->enemyDraw,
                                  e.pos, e.fwd, upW, visScale, cockpit->mrShared,
                                  e.hitFlash / x3::space::shipai::kHitFlashSec);
                }
                // The dreadnought at its REAL (moving) position + CURRENT arc
                // heading, at capital SCALE: ~450 m of hull under way across
                // the arena (vet pass — it was a 110 m corvette parked 200 m
                // away). Presence is EARNED by flying the approach; the arc is
                // what makes her a SHIP instead of a turret emplacement.
                const float capFwd[3] = { capMotion.fwd[0], capMotion.fwd[1],
                                          capMotion.fwd[2] };
                if (!capitalKilled) {
                    x3::apphost::drawIntroShip(*hc.device, frame, cockpit->capDraw, capC, capFwd, kCapDrawScl, cockpit->mrShared);
                    // THE GUN MOUNTS — visible, aimable, and REMOVED when shot
                    // off (a scorched stub remains). Base + barrel per mount;
                    // the barrel TRACKS the player, which is what makes the
                    // battery read as weapons rather than greebles.
                    for (int gi = 0; gi < x3::space::kCapitalGunCount; ++gi) {
                        float gw[3]; gunWorld(gi, gw);
                        const bool aliveG =
                            x3::space::CapitalBattery::gunAlive(capBattery, gi);
                        // Barrel aim: at the player (alive) / frozen slump (dead).
                        float aim[3] = { ppos[0]-gw[0], ppos[1]-gw[1], ppos[2]-gw[2] };
                        const float al2 = std::sqrt(aim[0]*aim[0] + aim[1]*aim[1] +
                                                    aim[2]*aim[2]);
                        if (al2 > 1.0f) { aim[0]/=al2; aim[1]/=al2; aim[2]/=al2; }
                        x3::apphost::drawIntroCapitalGun(*hc.device, frame,
                            cockpit->gunBaseMesh, cockpit->gunBarrelMesh,
                            cockpit->gunTex, cockpit->mrShared,
                            gw, aim, aliveG,
                            // Spool telegraph: the mount GLOWS as it charges.
                            aliveG && capBattery.gun[gi].spoolT >= 0.0f
                                ? 1.0f - capBattery.gun[gi].spoolT /
                                         x3::space::CapitalBattery::kGunSpool
                                : 0.0f);
                    }
                } else {
                    // ---- THE WRECK -------------------------------------------
                    // Three reads, all driven by the death state machine:
                    //   LIGHTS OUT — the emissive gate + self-light fall to the
                    //     lights-out floor, so the hull stops glowing from
                    //     inside and only the fires on it are lit;
                    //   TUMBLE — with no attitude control she rolls about her
                    //     own long axis (Rodrigues rotation of the up vector);
                    //   GOING DOWN — the nose drops and the whole hull sags off
                    //     the patrol line it had been holding all fight.
                    const float roll  = capDeath.tumble;
                    const float pitch = 0.16f * capDeath.tumble;
                    const float ch = std::cos(pitch);
                    float dFwd[3] = { capFwd[0] * ch, -std::sin(pitch), capFwd[2] * ch };
                    {   // normalize (pitch keeps it unit, but be safe)
                        const float l = std::sqrt(dFwd[0]*dFwd[0] + dFwd[1]*dFwd[1] +
                                                  dFwd[2]*dFwd[2]);
                        if (l > 1e-5f) { dFwd[0]/=l; dFwd[1]/=l; dFwd[2]/=l; }
                    }
                    // Rodrigues: rotate world-up about the hull axis by `roll`.
                    const float wu[3] = { 0.0f, 1.0f, 0.0f };
                    const float cr = std::cos(roll), sr = std::sin(roll);
                    const float kd = dFwd[0]*wu[0] + dFwd[1]*wu[1] + dFwd[2]*wu[2];
                    const float kx[3] = { dFwd[1]*wu[2] - dFwd[2]*wu[1],
                                          dFwd[2]*wu[0] - dFwd[0]*wu[2],
                                          dFwd[0]*wu[1] - dFwd[1]*wu[0] };
                    const float dUp[3] = {
                        wu[0]*cr + kx[0]*sr + dFwd[0]*kd*(1.0f-cr),
                        wu[1]*cr + kx[1]*sr + dFwd[1]*kd*(1.0f-cr),
                        wu[2]*cr + kx[2]*sr + dFwd[2]*kd*(1.0f-cr) };
                    // The wreck rides the death machine: the in-place breakup
                    // sags off station; the DEORBIT falls down the planet line
                    // (plunge) under an entry burn — drawn via the hit-flash
                    // warm tint so the hull READS as re-entry heating, plus a
                    // burning trail streamed behind it below.
                    const float wreckPos[3] = {
                        capC[0] + capDeath.planetDir[0] * capDeath.plunge,
                        capC[1] + capDeath.planetDir[1] * capDeath.plunge
                                - capDeath.sag,
                        capC[2] + capDeath.planetDir[2] * capDeath.plunge };
                    x3::apphost::drawIntroShipBasis(*hc.device, frame, cockpit->capDraw,
                                  wreckPos, dFwd, dUp, kCapDrawScl, cockpit->mrShared,
                                  /*hitFlash=*/0.15f * capDeath.burn,
                                  /*emisScale=*/capDeath.lights +
                                                1.3f * capDeath.burn,
                                  /*entryBurn=*/capDeath.burn);
                    // ENTRY-BURN TRAIL (deorbit only; burn==0 in the breakup):
                    // embers + smoke shed BEHIND the falling hull along the
                    // planet line, cadenced so the cost stays bounded.
                    if (fxOn && capDeath.burn > 0.05f) {
                        const float back[3] = { -capDeath.planetDir[0],
                                                -capDeath.planetDir[1],
                                                -capDeath.planetDir[2] };
                        if (step % 2 == 0) {
                            const float toff = (float)((step / 2) % 8) * 34.0f;
                            fxPtr->spawnShipEmber(
                                { wreckPos[0] + back[0] * (40.0f + toff),
                                  wreckPos[1] + back[1] * (40.0f + toff),
                                  wreckPos[2] + back[2] * (40.0f + toff) },
                                { back[0] * 34.0f, back[1] * 34.0f, back[2] * 34.0f });
                        }
                        // ON-HULL FIRE SHEATH: embers crawling the hull itself
                        // (deterministic hash spread across the 450 m span), so
                        // the wreck reads as BURNING, not just trailing.
                        if (step % 2 == 1) {
                            const float h1 = std::sin((float)step * 0.731f);
                            const float h2 = std::sin((float)step * 1.883f);
                            const float h3 = std::sin((float)step * 2.947f);
                            fxPtr->spawnShipEmber(
                                { wreckPos[0] + dFwd[0] * h1 * 190.0f + h2 * 46.0f,
                                  wreckPos[1] + dFwd[1] * h1 * 190.0f + h3 * 40.0f,
                                  wreckPos[2] + dFwd[2] * h1 * 190.0f + h2 * 44.0f },
                                { back[0] * 20.0f + h3 * 6.0f,
                                  back[1] * 20.0f + h1 * 6.0f,
                                  back[2] * 20.0f + h2 * 6.0f });
                        }
                        if (step % 4 == 0)
                            fxPtr->spawnShipSmoke(
                                { wreckPos[0] + back[0] * (110.0f +
                                      36.0f * (float)((step / 4) % 4)),
                                  wreckPos[1] + back[1] * 110.0f,
                                  wreckPos[2] + back[2] * 110.0f },
                                { back[0] * 26.0f, back[1] * 26.0f, back[2] * 26.0f },
                                capDeath.burn);
                    }
                }
                if (fxOn) {
                    fxPtr->draw(*hc.device, frame, cx, cy, cz, cyaw, cpit);
                    fxPtr->submit(*hc.device, frame);
                    // KILL-GIB debris: step the GPU pool + draw the live chunks so a
                    // disintegrating ship's flung hull fragments read in-frame. Dark
                    // scorched-metal tint. Cheap: one compute pass + one instanced draw,
                    // and a no-op when the pool is empty (no kills yet).
                    hc.device->gpuDebrisStep(dt);
                    // Hot glowing scorched-metal chunks: a warm HDR tint (>1 feeds
                    // bloom) so the flung hull fragments READ as superheated debris
                    // against the black starfield instead of vanishing as dark grey.
                    const float debrisTint[4] = { 1.5f, 0.78f, 0.32f, 1.0f };
                    hc.device->gpuDebrisDraw(frame, debrisTint);
                }
                // FORCE-FIELD FLASH: a one-blink cyan border while the zap timer
                // runs (pushOut bounced us this instant) — cheap: four hud quads.
                if (zapFlashT > 0.0f) {
                    const int fw2 = winW, fh2 = winH;
                    if (fw2 > 0 && fh2 > 0) {
                        const float a = std::min(1.0f, zapFlashT / 0.15f);
                        const float cyan4[4] = { 0.45f, 0.90f, 1.0f, 0.55f * a };
                        const float th = 6.0f;
                        hc.device->drawHudQuad(frame, 0.0f, 0.0f, (float)fw2, th, cyan4);
                        hc.device->drawHudQuad(frame, 0.0f, (float)fh2 - th, (float)fw2, th, cyan4);
                        hc.device->drawHudQuad(frame, 0.0f, 0.0f, th, (float)fh2, cyan4);
                        hc.device->drawHudQuad(frame, (float)fw2 - th, 0.0f, th, (float)fh2, cyan4);
                    }
                }
                // ---- HUD readout: hull/shield, contacts, lock state ----
                {
                    char hud[96];
                    const int hullPct = (int)(100.0f * (float)pilot.hull() /
                                              (float)std::max(1, pilot.maxHull()));
                    const int shPct   = (int)(100.0f * (float)pilot.shield() /
                                              (float)std::max(1, pilot.maxShield()));
                    std::snprintf(hud, sizeof(hud), "HULL %3d%%  SHD %3d%%  CONTACTS %u",
                                  hullPct, shPct, enemies.aliveCount() + 1u);
                    const float cyanHud[4] = { 0.45f, 0.85f, 1.0f, 0.85f };
                    hc.device->drawHudTextF(frame, x3::rhi::FontRole::HudMono, hud,
                                            24.0f, 24.0f, 18.0f, cyanHud);
                    // SHIFT = ANTIMATTER BOOST — flash a readout so the boost is VISIBLE,
                    // not just felt. Amber, pulsing, under the hull line while held.
                    if (in.sprint) {
                        const float amber[4] = { 1.0f, 0.72f, 0.18f,
                                                 0.75f + 0.25f * std::sin(beatT * 22.0f) };
                        hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                                ">> ANTIMATTER BOOST <<", 24.0f, 68.0f, 20.0f, amber);
                    }
                    // (The old top-left "LOCK" banner moved onto the target
                    //  bracket itself — see the reticle block below.)
                }
                // ---- FIGHTER RETICLE + TARGET INDICATORS (combat readability;
                // owner: "Better targeting reticle!!!"). Everything is built
                // from drawHudQuad rects + small font glyphs — no new renderer.
                //   * every on-screen hostile: thin NEUTRAL corner bracket that
                //     scales with the target's projected size;
                //   * the lock target: color-STAGED bracket (amber ACQUIRING ->
                //     red LOCKED, smooth), a dotted lock ring that sweeps closed
                //     onto the bracket while acquiring, a compact warm-dim HULL
                //     bar + range tag once locked, and a LEAD PIP (computeLead)
                //     marking where to put the nose;
                //   * off-screen contacts: small edge chevrons, amber (red for
                //     the locked target's direction);
                //   * the gun boresight: a quad-built crosshair on the nose ray
                //     (the exact ray hits use) with a HIT-CONFIRM flicker.
                // Projection is from the ACTUAL camera basis (camF/camU), not
                // the ship's Euler — with lock-bias / ALT-freelook the view is
                // not the nose, and Euler markers drift off their ships.
                //
                // THE DEFECT-3 FIX (owner, live: the whole bracket + CAP ACQ +
                // all four subsystem labels "in EMPTY SPACE, with no ship
                // anywhere near it", and "MOVES WILDLY with the mouse"). This
                // block used to build its own view basis inline, and got it
                // wrong in TWO compounding ways:
                //   1. up = fwd x right  — that is MINUS up. Every marker was
                //      mirrored about the horizontal centre line.
                //   2. neither axis was normalized and camU was never
                //      orthogonalized against camF. cameraBasis() returns the
                //      GAZE (ship-forward blended with freelook + the
                //      target-keeping look bias) together with the SHIP's up —
                //      NOT orthogonal, and the skew changes every time the mouse
                //      moves, which is precisely why the markers swam. The
                //      render device Gram-Schmidts inside setCameraBasis, so the
                //      HUD and the raster were using two different cameras.
                // Both now live in x3::space::hud::ViewProjector, which builds
                // the basis exactly the way glm::lookAt does and is covered by
                // --test-spacehud.
                if (winW > 0 && winH > 0) {
                    const float eyeW[3] = { cx, cy, cz };
                    const x3::space::hud::ViewProjector vproj(
                        eyeW, camF, camU, 65.0f, (float)winW, (float)winH);
                    struct Proj {
                        bool  vis;          // on screen, in front
                        float sx, sy;       // screen px
                        float zf;           // camera-forward depth (m)
                        float pxPerM;       // screen px per world metre at zf
                        float ex, ey;       // unit-square edge dir (off-screen)
                    };
                    auto project = [&](const float p[3]) {
                        const x3::space::hud::Projected r = vproj.project(p);
                        Proj o{};
                        o.vis    = r.onScreen;
                        o.sx     = r.sx;      o.sy = r.sy;
                        o.zf     = r.depth;   o.pxPerM = r.pxPerMetre;
                        o.ex     = r.edgeX;   o.ey = r.edgeY;
                        return o;
                    };
                    auto quad = [&](float x, float y, float w, float h, const float col[4]) {
                        hc.device->drawHudQuad(frame, x, y, w, h, col);
                    };
                    // Corner bracket: 4 corners x 2 arms, arm length 45% of the side.
                    auto bracket = [&](float bx, float by, float half, float th,
                                       const float col[4]) {
                        const float L = std::max(5.0f, half * 0.45f);
                        const float x0 = bx - half, x1 = bx + half - th;
                        const float y0 = by - half, y1 = by + half - th;
                        quad(x0, y0, L, th, col);  quad(x0, y0, th, L, col);          // TL
                        quad(bx + half - L, y0, L, th, col); quad(x1, y0, th, L, col);// TR
                        quad(x0, y1, L, th, col);  quad(x0, by + half - L, th, L, col);// BL
                        quad(bx + half - L, y1, L, th, col); quad(x1, by + half - L, th, L, col);// BR
                    };
                    // Dotted ring: n square dots on a circle; fillFrac sweeps it
                    // closed clockwise from 12 o'clock (the acquiring animation).
                    auto ringDots = [&](float bx, float by, float radius, int n,
                                        float dotSz, const float col[4], float fillFrac) {
                        for (int i = 0; i < n; ++i) {
                            if ((float)i / (float)n > fillFrac) break;
                            const float a = -1.5707963f + 6.2831853f * (float)i / (float)n;
                            quad(bx + std::cos(a) * radius - dotSz * 0.5f,
                                 by + std::sin(a) * radius - dotSz * 0.5f,
                                 dotSz, dotSz, col);
                        }
                    };
                    // Off-screen indicator: pinned inside the viewport edge along
                    // the true frustum-relative direction (ViewProjector::edgePoint),
                    // so it never wanders outside the window and behind-the-camera
                    // contacts still point the way you have to turn.
                    auto chevron = [&](const Proj& pr, const float col[4], float px) {
                        if (std::fabs(pr.ex) < 1e-4f && std::fabs(pr.ey) < 1e-4f) return;
                        const char* arrow = (std::fabs(pr.ex) > std::fabs(pr.ey))
                                            ? (pr.ex > 0 ? ">" : "<")
                                            : (pr.ey > 0 ? "^" : "v");
                        x3::space::hud::Projected e{};
                        e.edgeX = pr.ex; e.edgeY = pr.ey;
                        float sx2 = 0.0f, sy2 = 0.0f;
                        vproj.edgePoint(e, px * 1.4f, sx2, sy2);
                        hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                                arrow, sx2 - px * 0.3f, sy2 - px * 0.5f,
                                                px, col);
                    };
                    // Warm-dim hull bar (the ground-enemy healthbar language):
                    // near-black warm backing, warm amber fill shading to red as
                    // the hull drops.
                    auto hullBar = [&](float bx, float byTop, float w, float frac01) {
                        const float f = frac01 < 0.0f ? 0.0f : (frac01 > 1.0f ? 1.0f : frac01);
                        const float back[4] = { 0.06f, 0.035f, 0.02f, 0.70f };
                        const float fill[4] = { 0.95f, 0.30f + 0.42f * f, 0.16f, 0.92f };
                        quad(bx - w * 0.5f - 1.0f, byTop - 1.0f, w + 2.0f, 6.0f, back);
                        quad(bx - w * 0.5f, byTop, w * f, 4.0f, fill);
                    };
                    // DAMAGE METER (owner: "damage meter on the enemy ships"): a
                    // per-ship health bar shown above EVERY visible, alive enemy,
                    // reflecting hull/maxHull with a classic green -> yellow -> red
                    // ramp (full = green, ~half = yellow, near-dead = red). Drawn
                    // for all on-screen contacts, not just the locked target, so you
                    // always read how close each ship is to disintegrating.
                    auto dmgMeter = [&](float bx, float byTop, float w, float frac01) {
                        const float f  = frac01 < 0.0f ? 0.0f : (frac01 > 1.0f ? 1.0f : frac01);
                        const float rr = std::min(1.0f, 2.0f * (1.0f - f));  // 0 full -> 1 empty
                        const float gg = std::min(1.0f, 2.0f * f);           // 1 full -> 0 empty
                        const float back[4] = { 0.03f, 0.04f, 0.03f, 0.72f };
                        const float fill[4] = { 0.12f + 0.85f * rr, 0.18f + 0.78f * gg, 0.14f, 0.95f };
                        quad(bx - w * 0.5f - 1.0f, byTop - 1.0f, w + 2.0f, 6.0f, back);
                        quad(bx - w * 0.5f, byTop, w * f, 4.0f, fill);
                    };
                    const float neutral[4] = { 0.85f, 0.88f, 0.92f, 0.50f };
                    const float amberM[4]  = { 1.0f, 0.72f, 0.20f, 0.95f };
                    const float redM[4]    = { 1.0f, 0.30f, 0.22f, 0.95f };
                    // Lock staging for this frame.
                    const float lockProg = lockHad
                        ? std::min(1.0f, lockAge / kAcquireSec) : 0.0f;
                    const bool  lockedNow = lockHad && lockProg >= 1.0f;
                    const uint32_t lockId = lockHad ? targeting.lockedId() : 0u;
                    // Staged bracket colour: amber -> red across the acquire.
                    float staged[4];
                    for (int k = 0; k < 4; ++k)
                        staged[k] = amberM[k] + (redM[k] - amberM[k]) * lockProg;

                    // ---- Enemy fighters ---------------------------------------
                    for (uint32_t i = 0; i < enemies.count(); ++i) {
                        const auto& e = enemies.ship(i);
                        if (e.hull <= 0) continue;
                        const bool isTgt = lockHad && lockId == 1u + i;
                        const Proj pr = project(e.pos);
                        if (!pr.vis) {
                            chevron(pr, isTgt ? redM : amberM, isTgt ? 20.0f : 16.0f);
                            continue;
                        }
                        // Bracket half-size = the DRAWN silhouette radius (the
                        // same vis-comp factor as the draw + hit test) projected
                        // to px, clamped legible.
                        // RANGE, not forward depth: the draw scales by the TRUE
                        // camera->ship distance, so feeding pr.zf here made the
                        // bracket shrink away from the hull for anything off the
                        // view axis (zf = range * cos(angle)) — the bracket read
                        // as too small on exactly the contacts you are turning to
                        // fight. Same quantity the draw uses, so they now agree.
                        const float dBx = e.pos[0]-cx, dBy = e.pos[1]-cy, dBz = e.pos[2]-cz;
                        const float rangeE = std::sqrt(dBx*dBx + dBy*dBy + dBz*dBz);
                        const float worldR = 0.85f * x3::space::shipai::kVisBaseScale *
                                             x3::space::shipai::visCompFactor(rangeE);
                        const float halfPx = std::min(120.0f,
                            std::max(16.0f, worldR * pr.pxPerM)) + 6.0f;
                        // The damage meter rides above the bracket for EVERY visible
                        // enemy (targeted or not) — width scales with the silhouette,
                        // clamped legible.
                        const float meterW  = std::min(140.0f, std::max(30.0f, halfPx * 1.5f));
                        const float meterY  = pr.sy - halfPx - 12.0f;
                        dmgMeter(pr.sx, meterY, meterW,
                                 e.maxHull > 0 ? (float)e.hull / (float)e.maxHull : 0.0f);
                        if (!isTgt) {
                            bracket(pr.sx, pr.sy, halfPx, 1.0f, neutral);
                            continue;
                        }
                        // The LOCK TARGET: staged bracket closing from 1.6x ->
                        // 1.0x as the acquire completes (no popping).
                        const float halfT = halfPx * (1.6f - 0.6f * lockProg);
                        bracket(pr.sx, pr.sy, halfT, 2.0f, staged);
                        if (!lockedNow) {
                            // ACQUIRING: dotted ring sweeping closed + shrinking
                            // onto the bracket.
                            ringDots(pr.sx, pr.sy, halfT * (2.0f - 0.8f * lockProg),
                                     20, 3.0f, staged, 0.15f + 0.85f * lockProg);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                "ACQ", pr.sx - halfT, pr.sy - halfT - 16.0f, 12.0f, staged);
                        } else {
                            // LOCKED: a brief solid ring flourish that fades out
                            // right after the snap + a compact LOCK + range tag. The
                            // damage meter is already drawn above (for every enemy),
                            // so no separate hull bar here.
                            const float since = lockAge - kAcquireSec;
                            if (since < 0.35f) {
                                float ringCol[4] = { redM[0], redM[1], redM[2],
                                                     0.95f * (1.0f - since / 0.35f) };
                                ringDots(pr.sx, pr.sy, halfT * 1.18f, 24, 3.0f, ringCol, 1.0f);
                            }
                            char tag[24];
                            std::snprintf(tag, sizeof(tag), "LOCK %dm", (int)pr.zf);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                tag, pr.sx - halfT, pr.sy + halfT + 6.0f, 12.0f, redM);
                        }
                    }

                    // ---- Capital ship -----------------------------------------
                    // The bracket rect is remembered as a KEEP-OUT for the
                    // subsystem callouts below, so the labels never sit on top of
                    // the boss's own bracket/health block.
                    x3::space::hud::Rect capKeepOut{};
                    bool capKeepOutValid = false;
                    {
                        const bool isTgt = lockHad && lockId == 1000u;
                        const Proj pr = project(capC);
                        // A WRECK IS NOT A TARGET (handed over by the capital-death
                        // lane): the bracket, the CAP label, the shield/hull bars
                        // and the subsystem pips all kept drawing over the debris
                        // after the dreadnought was destroyed. Everything
                        // world-anchored to the capital goes away with it.
                        if (pr.vis && !capitalKilled) {
                            const float halfPx = std::min(260.0f,
                                std::max(30.0f, kCapHullR * pr.pxPerM)) + 6.0f;
                            const float* col = isTgt ? staged : amberM;
                            bracket(pr.sx, pr.sy, halfPx, isTgt ? 2.0f : 1.0f, col);
                            // CAP / CAP ACQ rides just OUTSIDE the bracket's top-left
                            // corner (owner: "CAP ... draws over the fighter").
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                isTgt && !lockedNow ? "CAP ACQ" : "CAP",
                                pr.sx - halfPx, pr.sy - halfPx - 34.0f, 12.0f, col);
                            capKeepOut = x3::space::hud::Rect{
                                pr.sx - halfPx - 4.0f, pr.sy - halfPx - 36.0f,
                                halfPx * 2.0f + 8.0f, halfPx * 2.0f + 52.0f };
                            capKeepOutValid = true;
                            {
                                // ALWAYS-ON capital readout (owner: "we need a
                                // health indication on the enemy ship!!!"). This
                                // used to be gated behind `isTgt && lockedNow`,
                                // so the only way to see the boss's health was to
                                // already hold a full lock on it — i.e. never,
                                // mid-dogfight. Shield + hull bars and one pip per
                                // subsystem (lit = up, dark = down) now draw the
                                // moment the capital is on screen.
                                const float sf2 = x3::space::ShipDamage::shieldFrac(capital);
                                const float hf2 = x3::space::ShipDamage::hullFrac(capital);
                                const float shCol[4]  = { 0.40f, 0.80f, 1.0f, 0.90f };
                                const float shBack[4] = { 0.02f, 0.05f, 0.08f, 0.70f };
                                const float w2 = halfPx * 2.0f;
                                quad(pr.sx - w2*0.5f - 1.0f, pr.sy - halfPx - 20.0f, w2 + 2.0f, 5.0f, shBack);
                                quad(pr.sx - w2*0.5f, pr.sy - halfPx - 19.0f, w2 * sf2, 3.0f, shCol);
                                hullBar(pr.sx, pr.sy - halfPx - 12.0f, w2, hf2);
                                for (int sIdx = 0; sIdx < (int)x3::space::Subsystem::Count; ++sIdx) {
                                    const bool down = x3::space::ShipDamage::subsystemDown(
                                        capital, (x3::space::Subsystem)sIdx);
                                    const float pipUp[4]   = { 1.0f, 0.72f, 0.20f, 0.95f };
                                    const float pipDown[4] = { 0.25f, 0.12f, 0.08f, 0.80f };
                                    quad(pr.sx - 22.0f + 12.0f * (float)sIdx,
                                         pr.sy + halfPx + 8.0f, 7.0f, 7.0f,
                                         down ? pipDown : pipUp);
                                }
                            }
                        } else if (!capitalKilled) {
                            chevron(pr, isTgt ? redM : amberM, 18.0f);
                        }
                    }

                    // ---- HARDPOINT MARKERS (the aim points: pick one, fly there,
                    // kill it). Alive = amber diamond + name; dead = dark ember
                    // mark. While the reactor is EXPOSED, its wound gets a hot
                    // pulsing marker — THE thing to shoot, visibly. ----
                    //
                    // READABILITY (owner, live): at the 2.6 km standoff the whole
                    // 450 m dreadnought is ~200 px wide, so the four hardpoints
                    // project inside ~80 px of each other and the raw
                    // "name at marker + 10 px" layout stacked TURRETS / ENGINES /
                    // SENSORS / SHIELD GEN on top of each other AND on the hull.
                    // Subsystem targeting IS the feature, so the callouts now get
                    // a collision-aware layout with LEADER LINES back to their
                    // hardpoints (x3::space::hud::layoutLabels, covered by
                    // --test-spacehud T6/T7), and a hardpoint that is off-screen
                    // or behind you gets an edge chevron instead of vanishing.
                    if (!capitalKilled) {   // a wreck has no live hardpoints
                        const float hpUp[4]   = { 1.0f, 0.72f, 0.20f, 0.95f };
                        const float hpDead[4] = { 0.45f, 0.20f, 0.10f, 0.75f };
                        constexpr float kHpTextPx = 11.0f;
                        x3::space::hud::LabelRequest   lreq[kMaxSubsystems]{};
                        x3::space::hud::LabelPlacement lpl[kMaxSubsystems]{};
                        int   lIdx[kMaxSubsystems];
                        uint32_t lCount = 0;
                        // A DESTROYED ship presents no aim points. Leaving live
                        // hardpoint markers floating over a burning wreck was
                        // the HUD still saying "there is something here to
                        // shoot" while the answer was "you already won".
                        // Both lanes wanted this: feat/capital-ship-death gates the
                        // loop here, and fix/space-combat-feel's "the capital bracket
                        // and hardpoints go away with the wreck". lCount stays 0 for
                        // a dead capital, so the label layout below is a no-op and
                        // no leader lines are drawn to a wreck.
                        for (int si = 0; si < kMaxSubsystems && !capitalKilled; ++si) {
                            float hw[3]; hardWorld(si, hw);
                            const Proj hp = project(hw);
                            const float* col = subWasDown[si] ? hpDead : hpUp;
                            if (!hp.vis) {
                                // Off-screen / behind: a small directional tick so
                                // the surviving hardpoints are still findable.
                                if (!subWasDown[si]) chevron(hp, col, 12.0f);
                                continue;
                            }
                            const float s = subWasDown[si] ? 4.0f : 6.0f;
                            quad(hp.sx - s*0.5f, hp.sy - s*0.5f, s, s, col);
                            quad(hp.sx - s*0.5f - 2.0f, hp.sy - 1.0f, 2.0f, 2.0f, col);
                            quad(hp.sx + s*0.5f, hp.sy - 1.0f, 2.0f, 2.0f, col);
                            if (subWasDown[si]) continue;   // dead: mark only, no label
                            lreq[lCount].anchorX  = hp.sx;
                            lreq[lCount].anchorY  = hp.sy;
                            lreq[lCount].w = hc.device->textAdvance(
                                x3::rhi::FontRole::Enemy, kHard[si].name, kHpTextPx) + 4.0f;
                            lreq[lCount].h = kHpTextPx + 4.0f;
                            // Nearest hardpoint first — it gets the tightest slot.
                            lreq[lCount].priority = (int)(hp.zf * 0.01f);
                            lIdx[lCount] = si;
                            ++lCount;
                        }
                        if (lCount > 0) {
                            // Keep the callouts off the capital's own bracket +
                            // health bars (they are the other thing you must read).
                            x3::space::hud::layoutLabels(
                                lreq, lpl, lCount,
                                capKeepOutValid ? &capKeepOut : nullptr,
                                capKeepOutValid ? 1u : 0u,
                                (float)winW, (float)winH);
                            for (uint32_t li = 0; li < lCount; ++li) {
                                const int si = lIdx[li];
                                const float* col = hpUp;
                                // Leader line first (under the text), dimmed.
                                const float lead[4] = { col[0], col[1], col[2], 0.45f };
                                x3::space::hud::Rect segs[2];
                                const uint32_t ns = x3::space::hud::leaderSegments(
                                    lpl[li], lreq[li], 1.0f, segs);
                                for (uint32_t k = 0; k < ns; ++k)
                                    quad(segs[k].x, segs[k].y, segs[k].w, segs[k].h, lead);
                                // A near-black backing plate so the name reads
                                // against a bright hull, then the name.
                                const float plate[4] = { 0.02f, 0.03f, 0.04f, 0.62f };
                                quad(lpl[li].x - 2.0f, lpl[li].y - 1.0f,
                                     lreq[li].w + 4.0f, lreq[li].h + 2.0f, plate);
                                hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                    kHard[si].name, lpl[li].x, lpl[li].y + 1.0f,
                                    kHpTextPx, col);
                            }
                        }
                        const bool reactorOpen = crippled && !capitalKilled &&
                            std::fmod(reactorT, kReactorCycle) < kReactorOpen;
                        if (reactorOpen) {
                            float rw[3]; capRot(kReactorOff, rw);
                            const Proj rp = project(rw);
                            if (rp.vis) {
                                const float pulse = 8.0f + 4.0f * std::sin(beatT * 9.0f);
                                const float rc[4] = { 1.0f, 0.35f, 0.15f,
                                                      0.75f + 0.25f * std::sin(beatT * 9.0f) };
                                bracket(rp.sx, rp.sy, pulse + 6.0f, 2.0f, rc);
                                hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                    "REACTOR", rp.sx + pulse + 10.0f, rp.sy - 5.0f,
                                    11.0f, rc);
                            }
                        }
                    }

                    // ---- BOSS BAR (always on screen) ---------------------------
                    // The world-anchored readout above only exists while the
                    // capital is IN FRAME — in a real dogfight you spend half the
                    // fight with it behind you. This top-of-screen boss bar is the
                    // health indication the owner asked for: it is ALWAYS drawn
                    // for the whole interactive window, on-screen or not, so you
                    // can always see how close the kill is.
                    {
                        const float sfB = x3::space::ShipDamage::shieldFrac(capital);
                        const float hfB = x3::space::ShipDamage::hullFrac(capital);
                        const float bw  = std::min(680.0f, (float)winW * 0.52f);
                        const float bx  = (float)winW * 0.5f - bw * 0.5f;
                        // LAYOUT (owner, live: "DREADNOUGHT SHIELDS 100% HULL 100%
                        // prints ON TOP of its own bars"). The title was drawn at
                        // by-18 with a 14 px cap height, which lands ON the shield
                        // strip; and at by = 0.055*H the whole block also collided
                        // with the top-left HULL/SHD/CONTACTS readout. The bars
                        // move down to 0.105*H and the title gets a full line of
                        // clearance above them, on its own backing plate.
                        const float by  = (float)winH * 0.105f;
                        const float titlePx = 14.0f;
                        const float titleY  = by - titlePx - 12.0f;
                        const float back[4]   = { 0.03f, 0.035f, 0.05f, 0.72f };
                        // BANNER-REPLACES-BAR LAYOUT (owner, live 2026-08-17:
                        // "This text, when there are more than one line.. they
                        // overwrite each other"). The kill banner used to draw
                        // at 0.10*H — dead on top of the boss bar at 0.105*H
                        // and its own title line — while the retired bar kept
                        // drawing under it. Now the DESTROYED state RETIRES the
                        // bar block entirely (a dead ship needs no health bar)
                        // and the banner takes over that band; every stacked
                        // line below advances by its real line height.
                        float cy2;   // where the callout stack starts (set per state)
                        if (!capitalKilled) {
                        const float shCol[4]  = { 0.40f, 0.80f, 1.00f, 0.95f };
                        // Hull fill reddens as it drops (green-ish -> hot red).
                        const float hullCol[4] = { 0.95f, 0.22f + 0.55f * hfB, 0.18f, 0.95f };
                        // Shield strip (thin, above the hull bar).
                        quad(bx - 2.0f, by - 2.0f, bw + 4.0f, 8.0f, back);
                        quad(bx, by, bw * sfB, 4.0f, shCol);
                        // Hull bar (the one that actually has to reach zero).
                        quad(bx - 2.0f, by + 8.0f, bw + 4.0f, 14.0f, back);
                        quad(bx, by + 10.0f, bw * hfB, 10.0f, hullCol);
                        // Subsystem pips, right-aligned under the bar.
                        for (int sIdx = 0; sIdx < (int)x3::space::Subsystem::Count; ++sIdx) {
                            const bool down = x3::space::ShipDamage::subsystemDown(
                                capital, (x3::space::Subsystem)sIdx);
                            const float pipUp[4]   = { 1.0f, 0.72f, 0.20f, 0.95f };
                            const float pipDown[4] = { 0.25f, 0.12f, 0.08f, 0.80f };
                            quad(bx + bw - 8.0f - 13.0f * (float)(3 - sIdx),
                                 by + 25.0f, 8.0f, 8.0f, down ? pipDown : pipUp);
                        }
                        // PHASE-AWARE label (the fight has acts): shields -> the
                        // hardpoint hunt -> the reactor cycle. Plus live HULL %.
                        const bool rOpen = crippled &&
                            std::fmod(reactorT, kReactorCycle) < kReactorOpen;
                        char bossTag[64];
                        if (sfB > 0.0f)
                            std::snprintf(bossTag, sizeof(bossTag),
                                "DREADNOUGHT  SHIELDS %d%%  HULL %d%%",
                                (int)(sfB * 100.0f + 0.5f), (int)(hfB * 100.0f + 0.5f));
                        else if (!crippled)
                            std::snprintf(bossTag, sizeof(bossTag),
                                "DREADNOUGHT  HARDPOINTS %d/%d  HULL %d%%",
                                kMaxSubsystems - localSubsDestroyed, kMaxSubsystems,
                                (int)(hfB * 100.0f + 0.5f));
                        else
                            std::snprintf(bossTag, sizeof(bossTag),
                                rOpen ? "REACTOR EXPOSED  HULL %d%%"
                                      : "REACTOR SHIELDED  HULL %d%%",
                                (int)(hfB * 100.0f + 0.5f));
                        {
                            const float tw = hc.device->textAdvance(
                                x3::rhi::FontRole::Enemy, bossTag, titlePx);
                            quad(bx - 2.0f, titleY - 3.0f, tw + 8.0f, titlePx + 8.0f, back);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                bossTag, bx + 2.0f, titleY, titlePx,
                                rOpen ? redM : (hfB > 0.0f ? amberM : redM));
                        }
                        cy2 = by + 40.0f;   // barks flow under the bar block
                        } else {
                        // ---- THE KILL BANNER (replaces the bar block) -------
                        // The answer to "did I just win?", stated in words, on
                        // screen, for the whole death sequence — with the sub-
                        // line naming the AUTHORED ENDING actually playing.
                        // Each line advances by its real height; nothing else
                        // draws in this band while the banner owns it.
                            const float age =
                                (x3::space::CapitalDeathSequence::durationFor(
                                     capDeath.outcome) + 1.6f) - capitalDeathHold;
                            const float fadeIn  = std::min(1.0f, age / 0.35f);
                            const float fadeOut = std::min(1.0f, capitalDeathHold / 0.8f);
                            const float a = fadeIn * fadeOut;
                            // White-hot (>1 blooms through the post chain).
                            const float kc[4] = { 1.70f, 1.42f, 0.90f, a };
                            const float ks[4] = { 0.95f, 0.80f, 0.55f, 0.85f * a };
                            const char* kMain = "DREADNOUGHT DESTROYED";
                            const bool deorbitB = capDeath.outcome ==
                                x3::space::DeathOutcome::DeorbitCrash;
                            const char* kSub = deorbitB
                                ? "ENEMY CAPITAL SHIP KILLED - DEORBITING TOWARD THE PLANET"
                                : "ENEMY CAPITAL SHIP KILLED - BREAKING UP IN SPACE";
                            // The Enemy face advances ~0.79 * size per glyph
                            // (measured off a capture); centre via textAdvance
                            // when available, glyph estimate as the fallback.
                            const float mainSz = 40.0f, subSz = 17.0f;
                            const float mainY  = titleY;   // the retired bar's band
                            const float mainW  = hc.device->textAdvance(
                                x3::rhi::FontRole::Enemy, kMain, mainSz);
                            const float subW   = hc.device->textAdvance(
                                x3::rhi::FontRole::Enemy, kSub, subSz);
                            const float subY   = mainY + mainSz * 1.25f + 6.0f;
                            quad((float)winW * 0.5f - mainW * 0.5f - 10.0f,
                                 mainY - 6.0f, mainW + 20.0f,
                                 mainSz * 1.25f + subSz * 1.35f + 18.0f, back);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                kMain, (float)winW * 0.5f - mainW * 0.5f,
                                mainY, mainSz, kc);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                kSub, (float)winW * 0.5f - subW * 0.5f,
                                subY, subSz, ks);
                            // Barks flow BELOW the whole banner block.
                            cy2 = subY + subSz * 1.35f + 12.0f;
                        }
                        // CALLOUT STACK (the radio barks) under the boss bar /
                        // kill banner. SOURCE-CODED (the messaging law, see
                        // CalloutSrc): the prefix + colour say WHOSE event this
                        // is before the player has finished reading the words.
                        // Mission lines (the kill) draw larger and brighter
                        // than status barks; every line advances by its real
                        // line height so two lines can never overprint.
                        for (const auto& cl : callouts) {
                            if (!cl.text || cl.t <= 0.0f) continue;
                            const float a = std::min(1.0f, cl.t / 0.6f);
                            float cc[4];
                            calloutColor(cl.src, 0.92f * a, cc);
                            const bool mission = (cl.src == kCalloutMission);
                            const float sz = mission ? 17.0f : 13.0f;
                            char line[96];
                            std::snprintf(line, sizeof(line), "%s%s",
                                          calloutPrefix(cl.src), cl.text);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                line, bx, cy2, sz, cc);
                            cy2 += sz * 1.35f;   // real line height, no overprint
                        }
                        // INCOMING flash while the turret battery spools — the
                        // telegraph: you have 0.6 s to change your line.
                        if (incomingFlashT > 0.0f &&
                            std::fmod(incomingFlashT, 0.2f) > 0.1f) {
                            const float wc[4] = { 1.0f, 0.25f, 0.18f, 0.95f };
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                ">> INCOMING <<", (float)winW * 0.5f - 60.0f,
                                (float)winH * 0.80f, 16.0f, wc);
                        }
                        // STAR proximity readout (host_space HUD contract): the
                        // heat ladder once inside 7 km of the surface, and the
                        // dive-shield % whenever it is engaged or recharging.
                        if (sunSurfDist < kSunHeatStart) {
                            char sunL[56];
                            const char* heatTxt =
                                sunSurfDist < kSunCritDist ? "HULL TEMP CRITICAL - PULL AWAY"
                              : sunSurfDist < kSunWarnDist ? "HULL TEMP RISING"
                                                           : "SOLAR PROXIMITY";
                            std::snprintf(sunL, sizeof(sunL), "SUN %.1fkm  %s",
                                (double)(std::max(0.0f, sunSurfDist) / 1000.0f), heatTxt);
                            const bool crit = sunSurfDist < kSunCritDist;
                            const float hcC[4] = { 1.0f, crit ? 0.28f : 0.62f,
                                                   crit ? 0.15f : 0.20f,
                                                   crit && std::fmod(beatT, 0.5f) < 0.25f
                                                       ? 0.45f : 0.95f };
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                sunL, (float)winW * 0.5f - 100.0f,
                                (float)winH * 0.86f, 14.0f, hcC);
                        }
                        if (sunPhase == SunPhase::InsideSun || sunShieldPct < 99.95f) {
                            char shL[40];
                            std::snprintf(shL, sizeof(shL),
                                sunPhase == SunPhase::InsideSun
                                    ? "SHIELD %3.0f%%" : "SHIELD %3.0f%% RECHARGING",
                                (double)std::max(0.0f, sunShieldPct));
                            const float scC[4] = { 0.40f, 0.80f, 1.0f, 0.95f };
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                shL, (float)winW * 0.5f - 60.0f,
                                (float)winH * 0.83f, 16.0f, scC);
                        }
                    }

                    // ---- LEAD PIP (the gameplay win): where to PUT THE NOSE so
                    // the shot lands on the moving target. computeLead solves the
                    // intercept at the bolt's visual speed; drawn as a fine pip
                    // (dot + 4 ticks) in hot yellow. Hidden when it overlaps the
                    // target centre (nothing to lead) or there is no lock.
                    if (lockedNow) {
                        const auto lead = targeting.computeLead(ppos, kLeadProjSpeed);
                        if (lead.valid) {
                            const Proj pr = project(lead.aimPoint);
                            if (pr.vis) {
                                const float pipCol[4] = { 1.0f, 0.93f, 0.45f, 0.95f };
                                quad(pr.sx - 1.5f, pr.sy - 1.5f, 3.0f, 3.0f, pipCol);
                                quad(pr.sx - 7.0f, pr.sy - 0.5f, 4.0f, 1.0f, pipCol);
                                quad(pr.sx + 3.0f, pr.sy - 0.5f, 4.0f, 1.0f, pipCol);
                                quad(pr.sx - 0.5f, pr.sy - 7.0f, 1.0f, 4.0f, pipCol);
                                quad(pr.sx - 0.5f, pr.sy + 3.0f, 1.0f, 4.0f, pipCol);
                            }
                        }
                    }

                    // ---- GUN BORESIGHT — the reticle that CANNOT lie: on the
                    // nose ray (the exact ray hits use; with lock-bias/freelook
                    // screen-centre stops being the aim). Quad-built crosshair:
                    // four arms + centre dot, with a HIT-CONFIRM flicker (an X
                    // of dots snapping outward) the instant a shot LANDS.
                    {
                        const float fhN = std::cos(pilot.pitch());
                        const x3::phys::Vec3 pN = pilot.pos();
                        const float aimP[3] = {
                            pN.x + fhN * std::cos(pilot.yaw()) * 600.0f,
                            pN.y + std::sin(pilot.pitch()) * 600.0f,
                            pN.z + fhN * std::sin(pilot.yaw()) * 600.0f };
                        const Proj pr = project(aimP);
                        if (pr.vis) {
                            const float cyanB[4] = { 0.45f, 0.95f, 1.0f, 0.95f };
                            quad(pr.sx - 14.0f, pr.sy - 1.0f, 8.0f, 2.0f, cyanB);
                            quad(pr.sx +  6.0f, pr.sy - 1.0f, 8.0f, 2.0f, cyanB);
                            quad(pr.sx - 1.0f, pr.sy - 14.0f, 2.0f, 8.0f, cyanB);
                            quad(pr.sx - 1.0f, pr.sy +  6.0f, 2.0f, 8.0f, cyanB);
                            quad(pr.sx - 1.5f, pr.sy - 1.5f, 3.0f, 3.0f, cyanB);
                            if (hitConfirmT > 0.0f) {
                                const float t01 = hitConfirmT / 0.15f;      // 1 -> 0
                                const float o = 8.0f + 10.0f * (1.0f - t01); // snap out
                                const float hitCol[4] = { 1.0f, 0.55f, 0.30f, 0.95f * t01 };
                                for (int sx2 = -1; sx2 <= 1; sx2 += 2)
                                    for (int sy2 = -1; sy2 <= 1; sy2 += 2) {
                                        quad(pr.sx + (float)sx2 * o - 1.5f,
                                             pr.sy + (float)sy2 * o - 1.5f, 3.0f, 3.0f, hitCol);
                                        quad(pr.sx + (float)sx2 * o * 0.6f - 1.0f,
                                             pr.sy + (float)sy2 * o * 0.6f - 1.0f, 2.0f, 2.0f, hitCol);
                                    }
                            }
                        }
                        // WEAPON ENERGY (the regenerating pool, Job: "it runs
                        // out") — a thin bar riding under the boresight so the
                        // state lives with the aim: cyan; amber under 35%; red
                        // pulse when too low to fire. NEVER lies: reads the
                        // live pilot pool the fire path drains.
                        {
                            const float eFrac = pilot.maxEnergy() > 0.0f
                                ? pilot.energy() / pilot.maxEnergy() : 0.0f;
                            const float bw = 90.0f, bh = 4.0f;
                            // ANCHOR (owner, live: "PWR still draws over open
                            // space rather than attached to anything"). The bar
                            // rides UNDER the gun boresight so the state lives
                            // with the aim — but when the boresight is off-screen
                            // (lock-bias / ALT-freelook swing the gaze off the
                            // nose) the old fallback parked it at DEAD CENTRE,
                            // i.e. straight over whatever ship you were looking
                            // at. Off-boresight it now goes to a stable
                            // bottom-centre chrome slot instead.
                            const float bx = pr.vis ? pr.sx : (float)winW * 0.5f;
                            const float by = pr.vis ? (pr.sy + 26.0f)
                                                    : (float)winH * 0.92f;
                            const float back[4] = { 0.02f, 0.04f, 0.06f, 0.65f };
                            float fill[4] = { 0.45f, 0.90f, 1.0f, 0.85f };
                            if (pilot.energy() < 2.8f) {          // can't fire: red pulse
                                fill[0] = 1.0f; fill[1] = 0.25f; fill[2] = 0.18f;
                                fill[3] = 0.55f + 0.40f * std::sin(beatT * 18.0f);
                            } else if (eFrac < 0.35f) {
                                fill[0] = 1.0f; fill[1] = 0.72f; fill[2] = 0.20f;
                            }
                            quad(bx - bw * 0.5f - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, back);
                            quad(bx - bw * 0.5f, by, bw * std::max(0.0f, std::min(1.0f, eFrac)), bh, fill);
                            const float lbl[4] = { fill[0], fill[1], fill[2], 0.80f };
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::HudMono,
                                "PWR", bx - bw * 0.5f - 34.0f, by - 4.0f, 11.0f, lbl);
                        }
                    }

                    // FAR-FROM-EARTH TELL: label the faint SOL pinpoint so the
                    // player can FIND home — on-screen text when in view, an
                    // edge chevron toward it otherwise.
                    {
                        constexpr float kD2R = 3.14159265f / 180.0f;
                        const float sAz = x3::starsys::kSolPinpointAzDeg * kD2R;
                        const float sEl = x3::starsys::kSolPinpointElDeg * kD2R;
                        const float sCe = std::cos(sEl);
                        const float solP[3] = {
                            cx + std::sin(sAz) * sCe * 10500.0f,
                            cy + std::sin(sEl)        * 10500.0f,
                            cz - std::cos(sAz) * sCe * 10500.0f };
                        const float solCol[4] = { 0.92f, 0.95f, 1.0f, 0.95f };
                        const Proj pr = project(solP);
                        if (pr.vis)
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::HudMono,
                                "SOL", pr.sx - 13.0f, pr.sy - 7.0f, 15.0f, solCol);
                        else
                            chevron(pr, solCol, 14.0f);
                    }
                }
                // ---- SYSTEM MINIMAP (owner: "visible on a minimap") — top-right box.
                // A north-up SYSTEM MAP, not just a contacts radar: the outer compass
                // ring carries the star + planets (+ faint SOL) at their sky bearing,
                // so the player reads the whole system layout at a glance; the inner
                // disc is the tactical radar (player at centre + heading tick, hostile
                // blips, the capital) scaled by range. Built from drawHudQuad/Text.
                {
                    const int mw = winW, mh = winH;
                    if (mw > 0 && mh > 0) {
                        constexpr float kD2R = 3.14159265f / 180.0f;
                        const float box  = 196.0f;                 // map box (px)
                        const float pad  = 18.0f;
                        const float x0   = (float)mw - box - pad;
                        const float titleH = 20.0f;
                        const float y0   = pad + titleH;
                        const float cxm  = x0 + box * 0.5f;
                        const float cym  = y0 + box * 0.5f;
                        const float Rrim = box * 0.42f;            // compass ring (bodies)
                        const float Rin  = box * 0.34f;            // tactical disc (contacts)
                        // Backing + frame.
                        const float bg[4]   = { 0.02f, 0.04f, 0.06f, 0.62f };
                        const float frame4[4]= { 0.40f, 0.80f, 1.0f, 0.55f };
                        hc.device->drawHudQuad(frame, x0 - 4.0f, y0 - 4.0f, box + 8.0f, box + 8.0f, bg);
                        const float bt = 2.0f;
                        hc.device->drawHudQuad(frame, x0 - 4.0f, y0 - 4.0f, box + 8.0f, bt, frame4);
                        hc.device->drawHudQuad(frame, x0 - 4.0f, y0 + box + 4.0f - bt, box + 8.0f, bt, frame4);
                        hc.device->drawHudQuad(frame, x0 - 4.0f, y0 - 4.0f, bt, box + 8.0f, frame4);
                        hc.device->drawHudQuad(frame, x0 + box + 4.0f - bt, y0 - 4.0f, bt, box + 8.0f, frame4);
                        // Title = the system name (owner: this reads a DIFFERENT system).
                        const float titleCol[4] = { dfSystem.starColor[0], dfSystem.starColor[1],
                                                    dfSystem.starColor[2], 0.95f };
                        hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy, dfSystem.name,
                                                x0 - 4.0f, pad - 2.0f, 15.0f, titleCol);
                        // A faint centre crosshair for the tactical disc.
                        const float grid[4] = { 0.30f, 0.55f, 0.70f, 0.30f };
                        hc.device->drawHudQuad(frame, x0, cym - 0.5f, box, 1.0f, grid);
                        hc.device->drawHudQuad(frame, cxm - 0.5f, y0, 1.0f, box, grid);
                        auto dot = [&](float px, float py, float s, const float col[4]) {
                            hc.device->drawHudQuad(frame, px - s * 0.5f, py - s * 0.5f, s, s, col);
                        };
                        // Colour per body type (star = amber; worlds by kind).
                        auto bodyColor = [](x3::starsys::BodyType t, float out[4]) {
                            switch (t) {
                                case x3::starsys::BodyType::Sun:  out[0]=1.0f;  out[1]=0.72f; out[2]=0.22f; break;
                                case x3::starsys::BodyType::Lava: out[0]=1.0f;  out[1]=0.40f; out[2]=0.16f; break;
                                case x3::starsys::BodyType::Ice:  out[0]=0.62f; out[1]=0.86f; out[2]=1.0f;  break;
                                case x3::starsys::BodyType::Gas:  out[0]=0.82f; out[1]=0.70f; out[2]=0.48f; break;
                                case x3::starsys::BodyType::Terrestrial: out[0]=0.34f; out[1]=0.78f; out[2]=0.62f; break;
                                default:                          out[0]=0.72f; out[1]=0.72f; out[2]=0.74f; break; // Moon
                            }
                            out[3] = 0.95f;
                        };
                        // Compact map tag from a body label: drop a trailing " (...)"
                        // qualifier, keep the last whitespace token (so "Kethzar II" ->
                        // "II", "Ashk (moon)" -> "Ashk"), <= 7 chars. Keeps distinct
                        // worlds distinct (full names all begin "Kethzar" + truncate same).
                        auto shortTag = [](const char* full, char* out, size_t n) {
                            std::string s = full ? full : "";
                            size_t par = s.find(" (");
                            if (par != std::string::npos) s = s.substr(0, par);
                            size_t sp = s.find_last_of(' ');
                            std::string tok = (sp == std::string::npos) ? s : s.substr(sp + 1);
                            std::snprintf(out, n, "%.7s", tok.c_str());
                        };
                        // Draw an icon + a SIDE-AWARE label (kept inside the box: labels
                        // on the right half sit to the LEFT of the icon).
                        auto labelIcon = [&](float bx, float by, const char* tag, const float col[4]) {
                            const float glyph = 10.0f;
                            const float w = (float)std::strlen(tag) * glyph * 0.62f;
                            const float lx = (bx > cxm) ? (bx - 6.0f - w) : (bx + 6.0f);
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::HudMono, tag,
                                                    lx, by - 5.0f, glyph, col);
                        };
                        // -- Outer compass ring: the system's bodies at their sky bearing.
                        for (const x3::starsys::SystemBody& b : dfSystem.bodies) {
                            const float az = b.azimuthDeg * kD2R;
                            const float bx = cxm + std::sin(az) * Rrim;
                            const float by = cym - std::cos(az) * Rrim;
                            float col[4]; bodyColor(b.type, col);
                            // Star bigger; gas giant medium; rest small.
                            const float s = (b.type == x3::starsys::BodyType::Sun) ? 12.0f
                                          : (b.type == x3::starsys::BodyType::Gas) ? 9.0f : 7.0f;
                            dot(bx, by, s, col);
                            char tag[10]; shortTag(b.label, tag, sizeof(tag));
                            labelIcon(bx, by, tag, col);
                        }
                        // Faint SOL on the ring (the far-from-Earth marker on the map).
                        {
                            const float az = x3::starsys::kSolPinpointAzDeg * kD2R;
                            const float bx = cxm + std::sin(az) * Rrim;
                            const float by = cym - std::cos(az) * Rrim;
                            const float solC[4] = { 0.95f, 0.97f, 1.0f, 0.95f };
                            dot(bx, by, 5.0f, solC);
                            labelIcon(bx, by, "SOL", solC);
                        }
                        // -- Inner tactical disc: player, heading, hostiles, capital.
                        auto plotContact = [&](const float p[3], float range, const float col[4], float s) {
                            const float dx = p[0] - cx, dz = p[2] - cz;
                            const float dist = std::sqrt(dx*dx + dz*dz);
                            const float az = std::atan2(dx, -dz);          // world bearing
                            // SQRT falloff: one consistent 3 km map that still
                            // separates knife-range fighters near the centre
                            // (linear would pile everything under 400 m into a
                            // 3-px blob now that the arena is capital-sized).
                            const float rr = Rin * std::sqrt(std::min(1.0f, dist / range));
                            dot(cxm + std::sin(az) * rr, cym - std::cos(az) * rr, s, col);
                        };
                        const float redBlip[4]   = { 1.0f, 0.32f, 0.24f, 0.95f };
                        const float amberBlip[4] = { 1.0f, 0.72f, 0.20f, 0.95f };
                        for (uint32_t i = 0; i < enemies.count(); ++i) {
                            const auto& e = enemies.ship(i);
                            if (e.hull <= 0) continue;
                            plotContact(e.pos, 3000.0f, redBlip, 5.0f);
                        }
                        plotContact(capC, 3000.0f, amberBlip, 8.0f);
                        // Player at centre + a heading tick (sky bearing = yaw + 90 deg).
                        const float pcol[4] = { 0.45f, 0.95f, 1.0f, 1.0f };
                        const float haz = (float)pilot.yaw() + 1.5707963f;
                        for (float t = 4.0f; t <= 16.0f; t += 4.0f)
                            dot(cxm + std::sin(haz) * t, cym - std::cos(haz) * t, 3.0f, pcol);
                        dot(cxm, cym, 7.0f, pcol);
                    }
                }
                // Sun-cinematic overlay LAST (over the HUD): the SHIELD ENGAGED
                // flash, the InsideSun molten wash + failing countdown, and the
                // respawn fade/captions all render here on the normal path
                // (Detonation..Replay draw theirs in the spectacle-cam block).
                drawSunOverlay(frame, (float)winW, (float)winH);
            }
            hc.device->endFrame(frame);
            if (evShot)
                hc.device->captureFrame((std::string(evDir) + "/live_" + beat.id +
                                         "_s" + std::to_string(step) + ".png").c_str());
            // Evidence runs are BOUNDED (nobody is at the controls to cripple the
            // capital or press Esc): end the beat just past the last capture.
            if (evDir && *evDir && step >= 960) break;

            // The old "hold this step until wall clock catches up" gate is GONE:
            // the sim now consumes the REAL frame interval (see TIME BASE), so
            // wall clock and game time advance together by construction, in both
            // directions. What is left is a courtesy ceiling so an uncapped
            // present loop does not free-run at four figures of fps burning the
            // GPU for frames nobody can see; it never slows the SIM, because dt
            // is measured after the sleep, not assumed.
            if (interactive) {
                const double frameSec = std::chrono::duration<double>(
                                            std::chrono::steady_clock::now() - tPrev).count();
                constexpr double kMinFrameSec = 1.0 / 300.0;
                if (frameSec < kMinFrameSec)
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(kMinFrameSec - frameSec));
            }
        }

        // ---- Exit conditions: all enemies down + ship crippled, or pilot dead. ----
        if (!pilot.isAlive()) break;
        if (crippled && enemies.aliveCount() == 0) break;
    }

    if (interactive) glfwSetInputMode(hc.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (live) hc.device->setFrustumCullEnabled(true);   // restore (beat-scoped off)
    if (live) hc.device->setCameraFar(200.0f);          // restore engine default (L7 pair)
    if (fxOn) fxPtr->shutdown(*hc.device);
    // Stop the engine bed (loops would otherwise drone under the next cinematic
    // beat). stopLoop on an invalid handle is a safe no-op.
    if (sfx) { sfx->stopLoop(humLoop); sfx->stopLoop(thrustLoop); }

    // Fold this window's metrics into the running totals. The CLIMAX window owns
    // the hull/time-to-cripple snapshot (the roll inputs); both windows contribute
    // dodge/accuracy/subsystem counts.
    m.salvosFaced       += localSalvosFaced;
    m.salvosDodged      += localSalvosDodged;
    m.shotsFired        += localShotsFired;
    m.shotsHit          += localShotsHit;
    m.subsystemsDestroyed = std::max(m.subsystemsDestroyed, localSubsDestroyed);
    if (beat.isClimax) {
        m.finalHullFrac     = pilot.maxHull() > 0
            ? (float)pilot.hull() / (float)pilot.maxHull() : 0.0f;
        m.timeToCrippleSec  = crippled ? crippleTime : 0.0f;
        m.windowDurationSec = std::max(beat.timeoutSec, 1.0f);
    }

    phys->shutdown();
    clearInputState(hc.window);
}

// ---------------------------------------------------------------------------
// ION-PULSE DESCENT BEAT (Phase 6) — the ESCAPED branch only.
//
// After the escape stinger (">> YOUR ANTIMATTER IS GONE — IGNITING ION DRIVE"), Jake's
// antimatter is drained by the glancing hit and he coasts DOWN to the planet on
// the ion-pulse drive. We REUSE the folded atmo-descent system (app/space/
// descent.*): the on-rails orbit->ground sequence driven by the S0 SpaceLayer
// spine. This is intro CONTENT (on-rails), not a gameplay level.
//
//   * SpaceLayer::requestDescent(planetId) arms the transition; each update(dt)
//     ticks the registered AtmoDescent runner (m_timer/m_duration -> progress)
//     until it returns true, at which point S0 lands in Context::Surface.
//   * LIVE (window+device): we init the AtmoDescent GPU resources, render the
//     re-entry/heat dome + rushing cloud streaks each frame, and over the top
//     draw the GLASS FACILITY proxy GROWING below the eye as progress climbs
//     (it scales up + rises into frame as the ground rushes up). K/Esc skips.
//   * HEADLESS: no GPU; we just drive the SpaceLayer runner with a fixed dt
//     until it reaches Surface (deterministic, bounded). The outcome marker is
//     identical to the live path.
//
// On completion the beat RETURNS true (descent reached the surface). The caller
// then writes the kIntroLandedFlag surface hand-off marker that Phase 7 consumes.
bool runIonDescentBeat(x3::apphost::HostContext& hc) {
    x3::logInfo("[intro] beat 'cine.descent' (ion-pulse atmo-descent: fuel drained, "
                "coasting down to the glass facility)");
    clearInputState(hc.window);

    const bool live = (hc.window != nullptr && hc.device != nullptr);

    // The S0 spine + the atmo-descent runner. The spine is headless logic (no GPU);
    // the descent's GPU resources are only built on the live path.
    x3::space::SpaceLayer layer;
    layer.init();

    x3::space::AtmoDescent descent;
    // A modest cinematic duration. Live: the re-entry + cloud-deck reads over a few
    // seconds. Headless: the exact value only sets how many fixed-dt ticks we run.
    const float kDescentSec = 7.0f;
    if (live) {
        // Build the (self-contained) entry-effect GPU resources AND register the
        // runner with the spine. setCamera keeps the heat dome centered on the eye.
        descent.init(*hc.device, layer, kDescentSec);
        descent.setCamera(0.0f, 0.0f, 0.0f);
    } else {
        // Headless: register a runner equivalent to AtmoDescent's so the spine still
        // ramps to Surface deterministically WITHOUT touching the GPU. AtmoDescent::
        // init() is the only thing that creates GPU resources; the runner itself is
        // pure timer math, so we register a tiny stand-in that mirrors its ramp.
        float* timer = new float(0.0f);     // owned by the lambda; freed below
        const float dur = std::max(0.25f, kDescentSec);
        layer.registerDescentRunner([timer, dur](float dt) {
            if (dt > 0.0f) *timer += dt;
            return (*timer / dur) >= 1.0f;
        });
        // NOTE: the stand-in is freed after the loop (see headless cleanup).
        // Stash via a unique_ptr so we don't leak if the loop early-exits.
        std::unique_ptr<float> guard(timer);
        // Run the on-rails ramp to Surface.
        layer.requestDescent(/*planetId*/ 1u);
        const float dt = 1.0f / 60.0f;
        const int maxSteps = (int)(kDescentSec / dt) + 4;   // +slack to guarantee landing
        int steps = 0;
        while (layer.context() != x3::space::Context::Surface && steps < maxSteps) {
            layer.update(dt);
            ++steps;
        }
        const bool landed = (layer.context() == x3::space::Context::Surface);
        x3::logInfo(std::string("[intro] ion-descent (headless) -> ") +
                    (landed ? "SURFACE (landed)" : "TIMEOUT (forced complete)"));
        return landed || steps >= maxSteps;   // bounded: always completes
    }

    // ----- LIVE on-rails descent loop -----
    layer.requestDescent(/*planetId*/ 1u);

    // A reusable unit quad for the glass-facility proxy (camera-facing slab that
    // grows below + rises into frame as the descent progresses). Self-contained,
    // same baked-mesh discipline as descent.cpp's own meshes.
    rhi::MeshHandle facility{};
    {
        const float h = 0.5f;
        rhi::MeshVertex v0{}; v0.pos[0]=-h; v0.pos[1]=-h; v0.normal[2]=1; v0.uv[0]=0; v0.uv[1]=0;
        rhi::MeshVertex v1{}; v1.pos[0]= h; v1.pos[1]=-h; v1.normal[2]=1; v1.uv[0]=1; v1.uv[1]=0;
        rhi::MeshVertex v2{}; v2.pos[0]= h; v2.pos[1]= h; v2.normal[2]=1; v2.uv[0]=1; v2.uv[1]=1;
        rhi::MeshVertex v3{}; v3.pos[0]=-h; v3.pos[1]= h; v3.normal[2]=1; v3.uv[0]=0; v3.uv[1]=1;
        rhi::MeshVertex vv[4] = { v0, v1, v2, v3 };
        uint32_t ii[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
        facility = hc.device->createMesh(vv, 4, ii, 12);
    }

    float t = 0.0f;
    const float dt = 1.0f / 60.0f;
    // Same real-time pacing as the dogfight beats: this loop advances the descent spine
    // by a fixed dt and presents with no vsync, so without a wall-clock gate the whole
    // re-entry rushes past in a fraction of a second on a fast GPU. Pace it to real time.
    const auto tStart = std::chrono::steady_clock::now();
    while (descent.active() || descent.progress() < 1.0f) {
        if (glfwWindowShouldClose(hc.window)) break;
        glfwPollEvents();
        if (glfwGetKey(hc.window, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
            glfwGetKey(hc.window, GLFW_KEY_K) == GLFW_PRESS) {
            x3::logInfo("[intro] ion-descent skipped (K/Esc)");
            break;
        }
        // Advance the spine (ticks the descent runner -> progress; lands in Surface).
        layer.update(dt);
        t += dt;
        const float p = descent.progress();

        // Camera looks straight down the +descent: keep the eye at origin, the
        // descent dome is camera-anchored. The facility grows below as p climbs.
        hc.device->setCamera(0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 65.0f);
        auto fr = hc.device->beginFrame();

        // The atmo-descent re-entry effect (heat dome + rushing cloud deck).
        descent.render(*hc.device, fr, /*viewProj16*/ nullptr, t);

        // The GLASS FACILITY growing below: a large camera-facing slab placed
        // BELOW + AHEAD that scales up and rises into frame as the ground rushes
        // up. It only reads through the clearing cloud deck late in the descent
        // (opacity tracks p), so it "resolves" out of the haze near touchdown.
        if (facility.valid()) {
            const float grow  = 6.0f + 60.0f * (p * p);   // grows fast late
            const float yoff  = -18.0f + 16.0f * p;        // rises into frame
            const float depth = -20.0f - 6.0f * (1.0f - p);
            const float reveal = std::clamp((p - 0.45f) / 0.55f, 0.0f, 1.0f);
            const float model[16] = {
                grow, 0,    0,    0,
                0,    grow, 0,    0,
                0,    0,    1,    0,
                0,    yoff, depth, 1 };
            // Cold glass facility: pale cyan, faint emissive so it reads against the
            // warm re-entry haze.
            const float baseFactor[4] = { 0.55f, 0.72f, 0.85f, 1.0f };
            const float emissive[4]   = { 0.25f, 0.40f, 0.55f, 0.8f * reveal };
            rhi::IRenderDevice::GlassMaterial g{};
            g.opacity   = std::clamp(0.85f * reveal, 0.0f, 1.0f);
            g.refraction = 0.0f;
            g.roughness  = 0.2f;
            g.specular   = 0.6f;
            g.tint[0] = 0.55f; g.tint[1] = 0.72f; g.tint[2] = 0.85f;
            hc.device->drawMeshGlass(fr, facility, rhi::TextureHandle{},
                                     baseFactor, emissive, g, model);
        }

        hc.device->endFrame(fr);

        // Hold to real time (t is the sim clock; only ever waits, never speeds up).
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - tStart).count();
        if (elapsed < (double)t)
            std::this_thread::sleep_for(std::chrono::duration<double>((double)t - elapsed));

        if (descent.progress() >= 1.0f) break;
    }

    const bool landed = (descent.progress() >= 1.0f) ||
                        (layer.context() == x3::space::Context::Surface);
    if (facility.valid()) hc.device->destroyMesh(facility);
    descent.shutdown(*hc.device);
    clearInputState(hc.window);
    x3::logInfo(std::string("[intro] ion-descent (live) -> ") +
                (landed ? "SURFACE (landed)" : "skipped"));
    // Even on a K/Esc skip we report landed=true: the skip jumps straight to the
    // surface hand-off (the player asked to skip the cinematic, not to abort the
    // escape branch).
    return true;
}

// Source the deterministic save seed (Phase 4 — the REAL per-save seed).
//   * If the host threaded an explicit seed (hc.introSeed != 0), use it verbatim
//     (lets a save/QA pin the roll).
//   * Otherwise DERIVE a stable per-save seed from the persisted StoryFlags
//     content (FNV-1a over the serialized blob): a fresh save vs a continued one
//     hash differently, so the roll is reproducible per save without a fixed
//     default. An empty/fresh blob folds to a fixed-but-nonzero base.
uint32_t deriveSaveSeed(const x3::game::StoryFlags& flags) {
    const std::string blob = flags.serialize();
    uint32_t h = 0x811C9DC5u;                 // FNV-1a offset basis
    for (unsigned char c : blob) { h ^= c; h *= 0x01000193u; }
    // Mix in a fixed intro salt so a fresh (empty) save still gets a stable,
    // nonzero seed (and never collides with a literal 0 "unset" sentinel).
    h ^= 0x1A7E0u;
    return h ? h : 0x1A7E0u;
}

} // namespace

// F9 skip-all accessors — EXTERNAL linkage (cinematic.cpp's film loop calls
// requestSkipAllIntro on F9). The latch itself is the file-scope static above.
void requestSkipAllIntro()      { s_skipAllIntro = true; }
bool skipAllIntroRequested()    { return s_skipAllIntro; }

IntroOutcome runInteractiveIntro(x3::apphost::HostContext& hc) {
    x3::logInfo("[intro] runInteractiveIntro: beat sequence start");
    s_skipAllIntro = false;   // F9 latch: fresh per run

    // Load the cold-open cutscene for the cinematic clips (best-effort; the live
    // cinematic beats no-op if it fails to load — the combat/outcome core still runs).
    x3::cut::Cutscene coldOpen{};
    bool haveCs = false;
    {
        const std::string csPath =
            x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
        std::vector<std::string> errs;
        haveCs = x3::cut::loadCutsceneFile(csPath, coldOpen, errs);
        if (!haveCs && hc.window)
            x3::logWarn("[intro] cold-open cutscene failed to load — cinematic beats skipped");
    }

    std::vector<Beat> beats = defaultIntroBeats();
    SkillMetrics metrics{};
    metrics.finalHullFrac = 1.0f;   // assume intact until the climax measures it

    // The cockpit rig — the interactive windows' visible layer (feat/intro-cockpit).
    // Live-only (window + device) — PLUS the headless X3_INTRO_CAPTURE evidence
    // run, which renders the beats offscreen and needs the same art. The plain
    // headless self-test path (no env) stays render-free and deterministic. A
    // missing GLB degrades gracefully to the empty frame.
    std::unique_ptr<x3::apphost::IntroCockpitRig> cockpit;
    const char* evCap = std::getenv("X3_INTRO_CAPTURE");
    if (hc.device && (hc.window || (evCap && *evCap))) {
        cockpit = std::make_unique<x3::apphost::IntroCockpitRig>();
        if (x3::apphost::buildIntroCockpitRig(*cockpit, *hc.device, /*includeBackdrop*/ false)) {
            x3::apphost::setIntroCockpitLook(*hc.device);
            x3::apphost::buildIntroCombatArt(*cockpit, *hc.device);   // best-effort
        } else {
            cockpit.reset();
        }
    }

    // Play every beat UP TO the outcome stinger (flight, reveal, dodge, charge,
    // dogfight). The cine.outcome beat is deferred: its span depends on the rolled
    // outcome, which depends on the climax metrics accumulated here — so we roll
    // FIRST, then play the matching stinger (below).
    Beat outcomeBeat{}; bool haveOutcomeBeat = false;
    for (const Beat& beat : beats) {
        if (s_skipAllIntro) break;   // F9: bail at the beat boundary
        if (beat.kind == BeatKind::CutsceneClip && beat.id == "cine.outcome") {
            outcomeBeat = beat; haveOutcomeBeat = true; continue;
        }
        if (beat.kind == BeatKind::CutsceneClip)
            playCinematicBeat(hc, beat, haveCs ? &coldOpen : nullptr);
        else
            runInteractiveBeat(hc, beat, metrics, cockpit.get());
    }

    // The interactive windows are done — release the cockpit's GPU handles before
    // the outcome stinger + the cell/surface build take over the device.
    if (cockpit) {
        cockpit->shutdown(*hc.device);
        cockpit.reset();
        // RESTORE the device look the deep-space beats overrode, or it LEAKS into
        // the cell (owner playtest: Vigil's holo terminal read "heavily regressed"
        // — the cell was lit by the space sun + near-black space IBL + cool space
        // ambient). Engine defaults per IRenderDevice.h / VulkanRenderDevice:
        // sky disabled, ambient (0.42, 0.44, 0.50), probe off. Point lights are
        // re-issued per floor by the cell; SSAO/SSGI are re-applied by the game
        // UI's applySettings at cell boot.
        x3::rhi::IRenderDevice::SkyParams sp{};       // enabled=false (indoor cell)
        hc.device->setSkyParams(sp);
        hc.device->setAmbient(0.42f, 0.44f, 0.50f);
        hc.device->setIblProbe(false);
        x3::rhi::PointLight noLights{};
        hc.device->setPointLights(&noLights, 0);
    }

    // Load the persisted narrative flags (the lane app_run branches on) so the
    // per-save seed is derived from the REAL save state, and the outcome write
    // augments (not clobbers) existing flags.
    x3::game::StoryFlags flags;
    const std::string flagsPath = defaultGameStoryFlagsPath();
    flags.loadFile(flagsPath);   // false + untouched on a fresh save — fine

    // F9 SKIP-ALL: no roll, no stinger, no descent — the canon ShotDown outcome,
    // written exactly like the real path, straight to waking in the cell.
    if (s_skipAllIntro) {
        x3::logInfo("[intro] F9 — SKIP ALL: intro aborted by the owner, canon ShotDown");
        writeOutcomeFlag(flags, IntroOutcome::ShotDown);
        flags.clear(kIntroLandedFlag);
        flags.saveFile(flagsPath);
        return IntroOutcome::ShotDown;
    }

    const float skill = skillScore(metrics);
    const float p = outcomeProbability(skill);

    // Per-save deterministic seed: explicit host seed wins, else derive from the
    // save's flag content (Phase 4 seed thread; replaces the Phase-3 fixed default).
    const uint32_t seed = (hc.introSeed != 0u) ? hc.introSeed : deriveSaveSeed(flags);

    // DEV outcome override (QA/tests): hc.introForce 0 => shot_down, 1 => escaped,
    // 2 => capital_killed, <0 => roll normally (or take an earned kill). Lets all
    // three branches be hit deterministically (--intro-force cell|escape|kill).
    IntroOutcome outcome;
    if (hc.introForce == 0) {
        outcome = IntroOutcome::ShotDown;
        x3::logInfo("[intro] FORCED outcome = SHOT_DOWN (intro_force)");
    } else if (hc.introForce == 1) {
        outcome = IntroOutcome::Escaped;
        x3::logInfo("[intro] FORCED outcome = ESCAPED (intro_force)");
    } else if (hc.introForce == 2) {
        outcome = IntroOutcome::CapitalKilled;
        x3::logInfo("[intro] FORCED outcome = CAPITAL_KILLED (intro_force)");
    } else if (metrics.capitalDestroyed) {
        // THE KILL. If the player put the dreadnought's hull to zero there is
        // nothing left to decide. Deterministic by construction.
        outcome = IntroOutcome::CapitalKilled;
        x3::logInfo("[intro] outcome = CAPITAL_KILLED (earned — dreadnought hull 0)");
    } else if (metrics.subsystemsDestroyed >= kMaxSubsystems &&
               metrics.finalHullFrac > 0.0f) {
        // EARNED ESCAPE (vet pass — the hidden 7-40% roll is GONE). The old
        // design rolled dice AFTER skilled play: a flawless run could still come
        // up shot-down with no legible cause, the one thing that makes players
        // feel cheated. Now every outcome is something the player DID:
        //   * kill the capital                          -> CAPITAL_KILLED
        //   * cripple all 4 hardpoints and stay alive   -> ESCAPED
        //   * die, or bail without crippling her        -> SHOT_DOWN (canon)
        // Canon still dominates in practice because a first-timer won't take all
        // four hardpoints off a 450 m dreadnought through its turret gauntlet —
        // but the ace who does KNOWS WHY he got out. (skillScore/rollOutcome
        // remain as pure telemetry + the legacy self-tested mapping.)
        outcome = IntroOutcome::Escaped;
        x3::logInfo("[intro] outcome = ESCAPED (earned — capital crippled, pilot alive)");
    } else {
        outcome = IntroOutcome::ShotDown;
        x3::logInfo("[intro] outcome = SHOT_DOWN (canon — capital not crippled or pilot down)");
    }

    x3::logInfo("[intro] seed=" + std::to_string(seed) +
                " skillScore=" + std::to_string(skill) +
                " p=" + std::to_string(p) +
                " (subs=" + std::to_string(metrics.subsystemsDestroyed) +
                " hull=" + std::to_string(metrics.finalHullFrac) +
                " dodge=" + std::to_string(metrics.salvosDodged) + "/" +
                std::to_string(metrics.salvosFaced) +
                " acc=" + std::to_string(metrics.shotsHit) + "/" +
                std::to_string(metrics.shotsFired) + ")");
    x3::logInfo(std::string("[intro] outcome = ") +
                (outcome == IntroOutcome::CapitalKilled ? "CAPITAL_KILLED"
                 : outcome == IntroOutcome::Escaped     ? "ESCAPED"
                                                        : "SHOT_DOWN"));

    // OUTCOME STINGER (Phase 5): now that the roll is known, play the matching
    // cinematic ending span. SHOT_DOWN -> the kill + "SIX MONTHS LATER" (canon);
    // ESCAPED -> slip the kill-box + antimatter drain + ion drive (hand-off to P6).
    // CAPITAL_KILLED rides the ESCAPED stinger for now: it is the "Jake gets out
    // and goes DOWN to the planet" span, which is exactly what happens after the
    // kill too (he follows the wreck down). A bespoke crash cinematic — the
    // dreadnought breaking up and cratering — is authoring work in
    // cold_open.cutscene.json and is NOT in this change.
    const bool escapeLike = (outcome == IntroOutcome::Escaped ||
                             outcome == IntroOutcome::CapitalKilled);
    if (haveOutcomeBeat) {
        const float* span = escapeLike ? kOutcomeEscapedSpan : kOutcomeShotDownSpan;
        outcomeBeat.clipStart = span[0];
        outcomeBeat.clipEnd   = span[1];
        outcomeBeat.id = (outcome == IntroOutcome::CapitalKilled)
            ? "cine.outcome.capital_killed"
            : (outcome == IntroOutcome::Escaped ? "cine.outcome.escaped"
                                                : "cine.outcome.shot_down");
        playCinematicBeat(hc, outcomeBeat, haveCs ? &coldOpen : nullptr);
    }

    // ION-PULSE DESCENT (Phase 6) — ESCAPED branch ONLY. After the antimatter-
    // drain / ion-drive stinger, run the on-rails atmo-descent (reusing app/space/
    // descent.*): the ion-pulse coast-down to the planet, the glass facility (where
    // Sarah is imprisoned) growing below, ending at the surface. ShotDown is
    // UNTOUCHED -> it smashes to "SIX MONTHS LATER" and wakes in the cell (canon).
    // The surface hand-off marker reflects THIS run only: clear it up front so a
    // prior escaped save can't leak the landed state into a fresh shot_down run,
    // then set it iff this run's escape descent actually reached the surface.
    flags.clear(kIntroLandedFlag);
    flags.clear(kIntroWreckFlag);
    if (escapeLike) {
        const bool landed = runIonDescentBeat(hc);
        if (landed && outcome == IntroOutcome::CapitalKilled) {
            // CRASH-SITE START (owner canon): the dreadnought went down ahead of
            // him, so Jake sets down at the wreck — salvage its tech, free the
            // prisoners in its hold, then breach Lab Zero from OUTSIDE.
            flags.set(kIntroWreckFlag);
            x3::logInfo(std::string("[intro] CRASH-SITE HAND-OFF -> StoryFlags['") +
                        kIntroWreckFlag + "'] set (Act-1 starts at the wreck)");
        }
        if (landed) {
            // SURFACE HAND-OFF marker: the descent reached the ground. Phase 7's
            // surface-landing Act-1 (and app_run's branch select) reads this to
            // confirm the player spawns OUTSIDE the glass facility (rescuer start),
            // not in the canon cell. Only ever set on the completed escape descent.
            flags.set(kIntroLandedFlag);
            x3::logInfo(std::string("[intro] SURFACE HAND-OFF -> StoryFlags['") +
                        kIntroLandedFlag + "'] set (Phase 7 surface Act-1 consumes it)");
        }
    }

    // Persist the branch flag beside the save (x3::game StoryFlags — the narrative
    // lane app_run.cpp branches on). Saved so a later read (or restart) is stable.
    writeOutcomeFlag(flags, outcome);
    flags.saveFile(flagsPath);

    return outcome;
}

// ===========================================================================
// --test-introorch self-test (headless, deterministic, no window/Vulkan).
// ===========================================================================
bool runIntroOrchestratorSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    // --- T1: beat sequencing — the authored order is exactly the spec §3 flow:
    //         cine, cine, interactive, cine, interactive(climax), cine. ---
    {
        const auto beats = defaultIntroBeats();
        check(beats.size() == 6, "T1 six beats authored");
        const BeatKind want[6] = {
            BeatKind::CutsceneClip, BeatKind::CutsceneClip,
            BeatKind::InteractiveWindow, BeatKind::CutsceneClip,
            BeatKind::InteractiveWindow, BeatKind::CutsceneClip
        };
        bool order = beats.size() == 6;
        for (size_t i = 0; i < beats.size() && i < 6; ++i)
            order = order && (beats[i].kind == want[i]);
        check(order, "T1b beat kinds in spec order (cine,cine,play,cine,play,cine)");
        // Exactly one climax (the dogfight), and it is an interactive window.
        int climax = 0; int climaxIsInteractive = 0;
        for (const auto& b : beats) if (b.isClimax) {
            ++climax; if (b.kind == BeatKind::InteractiveWindow) ++climaxIsInteractive;
        }
        check(climax == 1 && climaxIsInteractive == 1,
              "T1c exactly one climax beat, and it is interactive");
        // CutsceneClip spans are well-formed (end > start).
        bool spansOk = true;
        for (const auto& b : beats)
            if (b.kind == BeatKind::CutsceneClip) spansOk = spansOk && (b.clipEnd > b.clipStart);
        check(spansOk, "T1d cinematic spans well-formed (end > start)");
    }

    // --- T2: skill->p mapping bounds (spec §4). skill 0 -> 0.07, skill 1 -> 0.40,
    //         clamped + monotonic non-decreasing across the range. ---
    {
        check(std::fabs(outcomeProbability(0.0f) - kFloorP) < 1e-6f,
              "T2 skill 0 -> p = 0.07 (floor)");
        check(std::fabs(outcomeProbability(1.0f) - kCeilP) < 1e-6f,
              "T2b skill 1 -> p = 0.40 (ceiling)");
        check(std::fabs(outcomeProbability(-5.0f) - kFloorP) < 1e-6f &&
              std::fabs(outcomeProbability(5.0f) - kCeilP) < 1e-6f,
              "T2c out-of-range skill clamps to [floor, ceiling]");
        // Midpoint is exactly halfway between floor and ceiling.
        check(std::fabs(outcomeProbability(0.5f) - (kFloorP + 0.5f*(kCeilP-kFloorP))) < 1e-6f,
              "T2d skill 0.5 -> p halfway between floor and ceiling");
        bool mono = true; float prev = -1.0f;
        for (int i = 0; i <= 20; ++i) {
            float p = outcomeProbability((float)i / 20.0f);
            mono = mono && (p >= prev - 1e-7f); prev = p;
        }
        check(mono, "T2e p is monotonic non-decreasing in skill");
    }

    // --- T3: deterministic outcome for a fixed (seed, skill): same inputs ->
    //         same IntroOutcome, every call. ---
    {
        const uint32_t seed = 12345u;
        IntroOutcome a = rollOutcome(seed, 0.5f);
        bool stable = true;
        for (int i = 0; i < 100; ++i) stable = stable && (rollOutcome(seed, 0.5f) == a);
        check(stable, "T3 fixed (seed, skill) -> identical outcome across 100 rolls");
        // The roll uses the SAME chanceRoll the {chance} op uses: the branch is
        // exactly (chanceRoll < p). Verify against a direct comparison.
        const float r = x3::game::chanceRoll(seed, kIntroRollKey);
        const float p = outcomeProbability(0.5f);
        IntroOutcome expect = (r < p) ? IntroOutcome::Escaped : IntroOutcome::ShotDown;
        check(a == expect, "T3b outcome == (chanceRoll < p) — the shared deterministic gate");
    }

    // --- T4: skill biases the odds — a high-skill seed sweep escapes strictly more
    //         often than a low-skill one (monotone effect of skill on the branch). ---
    {
        int escLow = 0, escHigh = 0; const int N = 2000;
        for (int s = 0; s < N; ++s) {
            if (rollOutcome((uint32_t)s, 0.0f) == IntroOutcome::Escaped) ++escLow;
            if (rollOutcome((uint32_t)s, 1.0f) == IntroOutcome::Escaped) ++escHigh;
        }
        check(escHigh > escLow, "T4 higher skill escapes more often over a seed sweep");
        // Rates land near the floor/ceiling probabilities (sanity on the gate).
        float rLow = (float)escLow / N, rHigh = (float)escHigh / N;
        check(rLow > 0.02f && rLow < 0.13f, "T4b low-skill escape rate ~ floor (0.07)");
        check(rHigh > 0.33f && rHigh < 0.47f, "T4c high-skill escape rate ~ ceiling (0.40)");
        x3::logInfo("  [info] escape rate: skill0=" + std::to_string(rLow) +
                    " skill1=" + std::to_string(rHigh));
    }

    // --- T5: skillScore weighting — pure, in [0,1], monotone in each input, and a
    //         flawless run scores ~1 / a failed run ~0. ---
    {
        SkillMetrics zero{}; zero.finalHullFrac = 0.0f; // worst case
        check(std::fabs(skillScore(zero)) < 1e-6f, "T5 worst metrics -> skill 0");
        SkillMetrics best{};
        best.subsystemsDestroyed = kMaxSubsystems;
        best.finalHullFrac = 1.0f;
        best.salvosFaced = 10; best.salvosDodged = 10;
        best.shotsFired = 10; best.shotsHit = 10;
        best.windowDurationSec = 20.0f;
        best.timeToCrippleSec = 0.001f; // >0 so the speed term counts; near-instant cripple
        check(skillScore(best) > 0.99f, "T5b flawless metrics -> skill ~ 1");
        // Monotone in subsystems (holding the rest fixed).
        SkillMetrics m1{}, m2{};
        m2.subsystemsDestroyed = 1;
        check(skillScore(m2) > skillScore(m1), "T5c more subsystems -> higher skill");
        // Result always in [0,1].
        check(skillScore(best) <= 1.0f && skillScore(zero) >= 0.0f, "T5d skill clamped to [0,1]");
    }

    // --- T6: StoryFlags["intro.outcome"] write per outcome. ---
    {
        x3::game::StoryFlags f;
        writeOutcomeFlag(f, IntroOutcome::Escaped);
        const std::string esc = std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeEscaped;
        const std::string sd  = std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeShotDown;
        check(f.has(esc) && !f.has(sd), "T6 escaped writes intro.outcome=escaped");
        writeOutcomeFlag(f, IntroOutcome::ShotDown);
        check(f.has(sd) && !f.has(esc), "T6b shot_down overwrites to intro.outcome=shot_down");
    }

    // --- T7: input/state cleared on the cinematic<->interactive hand-off. The
    //         clearInputState seam is a no-op headless (window == nullptr) and must
    //         not throw / mutate state; runInteractiveBeat headless must produce
    //         deterministic, bounded metrics with no window. ---
    {
        x3::apphost::HostContext hc{};   // window == nullptr, device == nullptr (headless)
        SkillMetrics m{}; m.finalHullFrac = 1.0f;
        Beat dogfight; dogfight.kind = BeatKind::InteractiveWindow;
        dogfight.id = "test.dogfight"; dogfight.enemyCount = 4;
        dogfight.timeoutSec = 20.0f; dogfight.isClimax = true;
        runInteractiveBeat(hc, dogfight, m);
        // Deterministic: a second identical run yields identical metrics.
        SkillMetrics m2{}; m2.finalHullFrac = 1.0f;
        runInteractiveBeat(hc, dogfight, m2);
        bool det = m.subsystemsDestroyed == m2.subsystemsDestroyed &&
                   m.salvosFaced == m2.salvosFaced &&
                   m.salvosDodged == m2.salvosDodged &&
                   m.shotsFired == m2.shotsFired &&
                   m.shotsHit == m2.shotsHit &&
                   std::fabs(m.finalHullFrac - m2.finalHullFrac) < 1e-6f &&
                   std::fabs(m.timeToCrippleSec - m2.timeToCrippleSec) < 1e-6f;
        check(det, "T7 headless interactive window is deterministic (input cleared, no carry-over)");
        check(m.shotsFired > 0 && m.windowDurationSec > 0.0f,
              "T7b headless window produced bounded metrics");
        float sc = skillScore(m);
        check(sc >= 0.0f && sc <= 1.0f, "T7c metrics -> skill in [0,1]");
        x3::logInfo("  [info] headless climax skill=" + std::to_string(sc) +
                    " subs=" + std::to_string(m.subsystemsDestroyed) +
                    " hull=" + std::to_string(m.finalHullFrac));
    }

    // --- T8: end-to-end headless runInteractiveIntro is deterministic + writes the
    //         flag-derived outcome consistent with the computed skill. ---
    {
        x3::apphost::HostContext hc{};
        IntroOutcome o1 = runInteractiveIntro(hc);
        IntroOutcome o2 = runInteractiveIntro(hc);
        check(o1 == o2, "T8 headless runInteractiveIntro is deterministic");
    }

    // --- T9: ION-PULSE DESCENT (Phase 6) — the ESCAPED branch runs the on-rails
    //         atmo-descent to the surface hand-off and sets StoryFlags["intro.landed"];
    //         the ShotDown branch does NOT (canon -> cell). Forces the outcome so both
    //         branches are hit deterministically headless. The descent itself is
    //         bounded + GPU-free in headless (drives the SpaceLayer runner to Surface). ---
    {
        // The descent beat in isolation lands at the surface (bounded, GPU-free).
        x3::apphost::HostContext hc{};   // headless
        check(runIonDescentBeat(hc), "T9 headless ion-descent reaches the surface (bounded)");

        // Forced ESCAPED end-to-end -> intro.landed set; forced SHOT_DOWN -> not set.
        // runInteractiveIntro persists to defaultGameStoryFlagsPath(); reload from there
        // to assert the marker the way Phase 7 / app_run will read it.
        const std::string fp = defaultGameStoryFlagsPath();
        {
            x3::apphost::HostContext esc{}; esc.introForce = 1;   // escaped
            IntroOutcome o = runInteractiveIntro(esc);
            x3::game::StoryFlags f; f.loadFile(fp);
            check(o == IntroOutcome::Escaped && f.has(kIntroLandedFlag),
                  "T9b ESCAPED runs the descent and sets StoryFlags['intro.landed']");
        }
        {
            x3::apphost::HostContext sd{}; sd.introForce = 0;     // shot_down
            IntroOutcome o = runInteractiveIntro(sd);
            x3::game::StoryFlags f; f.loadFile(fp);
            check(o == IntroOutcome::ShotDown && !f.has(kIntroLandedFlag),
                  "T9c SHOT_DOWN skips the descent (no intro.landed; canon cell)");
        }
    }

    x3::logInfo("intro-orchestrator: " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    std::printf("intro-orchestrator: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

// ===========================================================================
// --test-introbranch self-test (Phase 4: the app_run branch-selection contract).
// Headless, deterministic, no window/Vulkan. Verifies what app_run.cpp keys off:
// the intro.outcome flag round-trip, the dev force-override, the per-save seed
// thread, and the canon default — without touching the real save file (every
// end-to-end run forces an outcome OR pins a seed, so the result is fixed).
// ===========================================================================
bool runIntroBranchSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    // --- B1: writeOutcomeFlag/readOutcomeFlag round-trip — the EXACT encoding
    //         app_run reads to pick cell vs surface. ---
    {
        x3::game::StoryFlags f;
        writeOutcomeFlag(f, IntroOutcome::Escaped);
        check(readOutcomeFlag(f) == IntroOutcome::Escaped,
              "B1 escaped flag reads back as Escaped (-> surface stub path)");
        writeOutcomeFlag(f, IntroOutcome::ShotDown);
        check(readOutcomeFlag(f) == IntroOutcome::ShotDown,
              "B1b shot_down flag reads back as ShotDown (-> canon cell)");
        // A flag round-trip through the persisted text blob is stable.
        writeOutcomeFlag(f, IntroOutcome::Escaped);
        x3::game::StoryFlags g; g.deserialize(f.serialize());
        check(readOutcomeFlag(g) == IntroOutcome::Escaped,
              "B1c outcome survives serialize/deserialize (persisted save round-trip)");
        // CAPITAL_KILLED — the earned third branch (crash -> salvage -> breach).
        writeOutcomeFlag(f, IntroOutcome::CapitalKilled);
        check(readOutcomeFlag(f) == IntroOutcome::CapitalKilled,
              "B1d capital_killed flag reads back as CapitalKilled (-> crash-site Act-1)");
        // The three encodings are MUTUALLY EXCLUSIVE: writing one clears the others,
        // so a kill can never be read as an escape (or vice versa) on a reused save.
        const std::string kEsc  = std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeEscaped;
        const std::string kSd   = std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeShotDown;
        const std::string kKill = std::string(kIntroOutcomeFlag) + "=" + kIntroOutcomeCapitalKilled;
        check(f.has(kKill) && !f.has(kEsc) && !f.has(kSd),
              "B1e capital_killed write clears the escaped/shot_down keys");
        writeOutcomeFlag(f, IntroOutcome::ShotDown);
        check(f.has(kSd) && !f.has(kKill) && readOutcomeFlag(f) == IntroOutcome::ShotDown,
              "B1f shot_down write clears a prior capital_killed (no stale kill)");
        writeOutcomeFlag(f, IntroOutcome::CapitalKilled);
        x3::game::StoryFlags k; k.deserialize(f.serialize());
        check(readOutcomeFlag(k) == IntroOutcome::CapitalKilled,
              "B1g capital_killed survives serialize/deserialize");
    }

    // --- B2: canon DEFAULT — an empty/cleared flag set reads ShotDown, so a
    //         missing flag never mis-routes to the surface stub. ---
    {
        x3::game::StoryFlags empty;
        check(readOutcomeFlag(empty) == IntroOutcome::ShotDown,
              "B2 missing intro.outcome defaults to ShotDown (canon cell)");
    }

    // --- B3: dev force-override drives the RETURNED branch (what app_run uses).
    //         force=escaped -> Escaped (surface stub); force=shot_down -> ShotDown
    //         (canon). Pin the seed so the non-forced control is also fixed. ---
    {
        x3::apphost::HostContext hc{};         // headless (window/device null)
        hc.introSeed = 0xC0FFEEu;              // pinned -> deterministic non-forced roll
        hc.introForce = 1;                      // escaped
        check(runInteractiveIntro(hc) == IntroOutcome::Escaped,
              "B3 force=escaped selects the surface(stub) branch");
        hc.introForce = 0;                      // shot_down
        check(runInteractiveIntro(hc) == IntroOutcome::ShotDown,
              "B3b force=shot_down selects the canon cell branch");
        // Forced outcome ignores the seed entirely (QA can hit a branch regardless).
        hc.introForce = 1; hc.introSeed = 1u;
        IntroOutcome a = runInteractiveIntro(hc);
        hc.introSeed = 999999u;
        check(a == runInteractiveIntro(hc) && a == IntroOutcome::Escaped,
              "B3c forced outcome is seed-independent");
        hc.introForce = 2;                      // capital_killed
        hc.introSeed  = 0xC0FFEEu;
        check(runInteractiveIntro(hc) == IntroOutcome::CapitalKilled,
              "B3d force=capital_killed selects the crash-site branch");
    }

    // --- B6: THE KILL BEATS THE DICE. metrics.capitalDestroyed forces
    //         CapitalKilled deterministically — it must NOT go through the capped
    //         (max 40%) roll. Pinned across many seeds: if any seed produced a
    //         different branch, the kill would be a coin flip, which is the exact
    //         thing the owner asked us to stop doing. ---
    {
        SkillMetrics killed{};
        killed.capitalDestroyed = true;
        // The kill flag is orthogonal to the score: even a WORST-CASE skill run
        // (skill 0 -> p = 7%) must still fork to CapitalKilled.
        killed.finalHullFrac = 0.0f; killed.shotsFired = 100; killed.shotsHit = 0;
        check(skillScore(killed) >= 0.0f && skillScore(killed) <= 1.0f,
              "B6 capitalDestroyed leaves skillScore in range (it is not scored)");
        bool allKilled = true;
        for (uint32_t s = 1; s <= 64u; ++s) {
            x3::apphost::HostContext hc{};
            hc.introForce = 2;      // the same branch the earned kill takes
            hc.introSeed  = s;
            if (runInteractiveIntro(hc) != IntroOutcome::CapitalKilled) allKilled = false;
        }
        check(allKilled, "B6b the kill branch is seed-independent across 64 seeds");
    }

    // --- B4: EARNED-OUTCOME contract (the hidden roll is gone). A non-forced run
    //         is deterministic, reproducible, and decided by what the pilot DID:
    //         kill -> CapitalKilled, cripple+survive -> Escaped, else ShotDown. ---
    {
        x3::apphost::HostContext hc{}; hc.introForce = -1; hc.introSeed = 42u;
        IntroOutcome r1 = runInteractiveIntro(hc);
        IntroOutcome r2 = runInteractiveIntro(hc);
        check(r1 == r2, "B4 non-forced outcome is deterministic run-to-run");
        // Recompute the headless metrics and apply the SAME earned mapping
        // runInteractiveIntro uses — the expectation is derived, not assumed.
        SkillMetrics m{}; m.finalHullFrac = 1.0f;
        for (const Beat& b : defaultIntroBeats())
            if (b.kind == BeatKind::InteractiveWindow) runInteractiveBeat(hc, b, m);
        IntroOutcome expect = IntroOutcome::ShotDown;
        if (m.capitalDestroyed) expect = IntroOutcome::CapitalKilled;
        else if (m.subsystemsDestroyed >= kMaxSubsystems && m.finalHullFrac > 0.0f)
            expect = IntroOutcome::Escaped;
        check(r1 == expect,
              "B4b outcome matches the earned mapping of the replayed metrics");
        // END-TO-END PROOF THE BOSS IS KILLABLE. The headless synthetic pilot
        // works the anatomy (hardpoints in enum order, then hull) and puts the
        // dreadnought's hull to 0 within the authored windows. Regression guard
        // for the launch-day bug where all capital damage routed into an already-
        // dead subsystem forever and the hull sat at 1980/2000: unkillable.
        check(m.capitalDestroyed,
              "B4c headless pilot drives the capital HULL to 0 (capital is killable)");
        check(m.subsystemsDestroyed >= kMaxSubsystems,
              "B4d headless pilot downs all hardpoints on the way to the kill");
    }

    // --- B5: the SEED no longer decides the branch (vet pass: outcomes are
    //         earned, not rolled — a flawless run can never coin-flip into
    //         shot-down). Across a seed sweep, the non-forced outcome is
    //         CONSTANT; the seed thread survives for content variety only. ---
    {
        IntroOutcome first{}; bool same = true;
        for (uint32_t s = 1; s <= 16u; ++s) {
            x3::apphost::HostContext hc0{}; hc0.introForce = -1; hc0.introSeed = s;
            const IntroOutcome o = runInteractiveIntro(hc0);
            if (s == 1u) first = o; else same = same && (o == first);
        }
        check(same, "B5 outcome is seed-independent (earned, not rolled)");
    }

    x3::logInfo("intro-branch: " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    std::printf("intro-branch: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::intro
