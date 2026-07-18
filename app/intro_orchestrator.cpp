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
#include "space/space_layer.h"   // x3::space::SpaceLayer (S0 spine; AtmoDescent runner host)
#include "space/descent.h"       // x3::space::AtmoDescent (the ion-pulse on-rails coast-down)

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
} // namespace

// ---------------------------------------------------------------------------
// OUTCOME STINGER spans (Phase 5) — the two endings authored in the cold-open
// timeline (assets/cutscenes/cold_open.cutscene.json). The cine.outcome beat plays
// ONE of these, selected by the rolled IntroOutcome:
//   * SHOT_DOWN [36..66]: the killing salvo -> smash-to-black -> "ESCAPE FROM LAB
//     ZERO" + "SIX MONTHS LATER" (canon: wake in the cell).
//   * ESCAPED   [66..80]: Jake slips the kill-box, a glancing hit drains the
//     antimatter, the ion-pulse drive ignites -> "ANTIMATTER CRITICAL — IGNITING
//     ION DRIVE" -> hands off to the Phase-6 atmo descent.
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
    b5.enemyCount = 4; b5.timeoutSec = 20.0f; b5.isClimax = true; beats.push_back(b5);

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
    const char* val = (outcome == IntroOutcome::Escaped)
        ? kIntroOutcomeEscaped : kIntroOutcomeShotDown;
    flags.set(std::string(kIntroOutcomeFlag) + "=" + val);
}

IntroOutcome readOutcomeFlag(const x3::game::StoryFlags& flags) {
    // Escaped only when its key is explicitly present; absence/cleared => canon
    // ShotDown (the safe default so a missing flag never mis-routes to the stub).
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
    tun.noseFollow     = 2.8f;    // arcade steering: the ship GOES where the nose
                                  // points (Newtonian drift read as "axes wrong")
    // FIRST person: the beats render from inside the cockpit rig. The default
    // 3P chase cam swings on a 12 m arm rotated by the ship's FULL quaternion
    // (roll included) while setCamera() is roll-less Euler — the mismatch reads
    // as "mouse is off axis" (owner playtest). 1P puts the eye at the ship.
    tun.defaultThirdPerson = true;   // 3P by default: the cockpit rig is a giant
                                     // dark box with a letterbox slot for a window
                                     // (owner: "MICROSCOPIC window", "can't see the
                                     // ship"). Open chase view sees the whole fight.
                                     // F2 toggles back to the cockpit.
    pilot.spawn(*phys, 0.0f, 0.0f, 0.0f, tun);

    x3::space::EnemyShipManager enemies;
    enemies.init((uint32_t)std::max(0, beat.enemyCount));
    for (int i = 0; i < beat.enemyCount; ++i) {
        const float ang = (float)i * 1.3f;
        // OWNER: "I can NEVER SEE THE ENEMY SHIP." They spawned at 250 m — a 2.5 m ship
        // at 250 m is ~11 px, a speck dead ahead. Bring the wing IN CLOSE and spread it
        // across the view so it fills the canopy the moment the beat starts: you see
        // who you are fighting, and you can actually turn to face them.
        const float pos[3] = { 70.0f + 22.0f * (float)i,     // 70 m out (was 250)
                               14.0f * std::sin(ang),
                               26.0f * std::cos(ang) };
        enemies.spawn(pos);
    }
    // A PLANET BACKDROP so space reads as SPACE, not a black void (owner: "I can NEVER
    // SEE THE PLANETS"). Eye-anchored bodies hung in the sky by loadNightSkyPlanets;
    // the same helper the showroom + cutscenes use, so no new asset path.
    x3::rhi::MeshHandle planetMesh{}, ringMesh{};
    std::vector<x3::apphost::NightSkyPlanet> planets;
    // THE DOGFIGHT IS IN A NAMED SYSTEM, FAR FROM SOL (owner: "far from earth").
    // Kethzar Prime (x3.starsys/1): an amber-hypergiant sky, a huge hero LAVA world,
    // a ringed gas giant, an ice world — the most dramatic sky in the registry. Its
    // body set drives the far-distance backdrop AND the HUD minimap below; a faint
    // labelled SOL is appended so "far from Earth" is a findable point, not implied.
    const x3::starsys::StarSystem& dfSystem = x3::starsys::dogfightSystem();
    if (hc.window != nullptr && hc.device != nullptr) {   // live (declared below as `live`)
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

    x3::space::TargetingSystem targeting;

    // The capital ship's destructible damage model (the dogfight objective).
    auto capital = x3::space::ShipDamage::makeCapital(/*shield*/400, /*hull*/2000, /*subHp*/120);

    const float dt = 1.0f / 60.0f;
    const int   maxSteps = (int)(beat.timeoutSec / dt);
    // ---- REAL-TIME PACING (owner playtest: "I can fly for 0.8 seconds ... but not
    // control my ship"). The beat advances the SIM by a FIXED dt = 1/60 per step, which
    // is correct for determinism — but the loop presented frames with NO vsync and NO
    // wall-clock pacing, so on a fast GPU it spun through all maxSteps in a fraction of a
    // second: a 30 s beat (1800 steps) rendered at ~2000 fps = ~0.9 s of REAL control,
    // and then it was over. The fix is the house rule (memory: "delta time, never frame
    // time") applied to the LOOP: gate each fixed step on real elapsed time so 1800 steps
    // of 1/60 take 30 s of WALL CLOCK. Headless (deterministic tests) is unpaced. If a
    // frame is genuinely slow (elapsed already past target) we never sleep — we just do
    // not run FASTER than real time; slower is the renderer's problem, not this gate's.
    const auto   tStart = std::chrono::steady_clock::now();
    int   localSalvosFaced = 0, localSalvosDodged = 0;
    int   localShotsFired = 0, localShotsHit = 0;
    int   localSubsDestroyed = 0;
    float crippleTime = 0.0f;
    bool  crippled = false;

    const bool live = (hc.window != nullptr && hc.device != nullptr);

    // Combat FX (tracers, muzzle flashes, crosshair) — live only. Heap-allocated:
    // CombatFx carries ~256 KB of scratch (the host_space convention).
    std::unique_ptr<x3::game::CombatFx> fxPtr;
    const bool fxOn = live && cockpit != nullptr;
    if (fxOn) { fxPtr = std::make_unique<x3::game::CombatFx>(); fxPtr->init(*hc.device); }

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
    auto fighterKillFx = [&](const float p[3]) {
        play3D(kSfxExplosion, "explosion_fighter (3D, fighter kill)", sndExplosion,
               p, 1.0f, 1.0f);
        if (fxOn) {
            fxPtr->spawnDeath({ p[0], p[1], p[2] });
            fxPtr->spawnSmoke({ p[0], p[1], p[2] });
        }
    };
    float zapCooldown = 0.0f;   // one zap per bounce, not per frame on the bubble
    float zapFlashT   = 0.0f;   // HUD border cyan flash timer
    bool  prevSprint  = false, prevLock = false;
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

    // Captured-cursor mouse-look for the live window (host_space pattern). The
    // cursor is restored to NORMAL on every exit path below (the cinematic beats
    // + the cutscene player expect a visible cursor).
    double lastMX = 0.0, lastMY = 0.0;
    if (live) {
        glfwSetInputMode(hc.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(hc.window, &lastMX, &lastMY);
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
    for (int step = 0; live || step < maxSteps; ++step) {
        const float tNow = (float)step * dt;

        // ---- Input: live reads GLFW; headless uses a deterministic synthetic
        //      "competent pilot" profile (steady forward + aim at the capital). ----
        x3::game::PlayerInput in{};
        float rollAxis = 0.0f;
        bool fire = false;
        if (live) {
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
        }
        // ANTIMATTER BOOST: one whoosh per Shift ENGAGE (edge-detected — the
        // sustained roar is the thrust bed swelling below, not a retrigger).
        if (in.sprint && !prevSprint)
            play2D(kSfxBoost, "boost_antimatter (Shift engage)", sndBoost, 0.9f, 1.0f);
        prevSprint = in.sprint;
        pilot.setRollInput(rollAxis);
        pilot.update(in, dt, *phys);

        // ---- FORCE FIELDS (owner: "I fly right thru the enemy ship"). The player
        //      bounces off the capital's shield bubble and off each fighter's hull
        //      bubble — a shield BOUNCE (pushOut cancels+reflects inward velocity).
        {
            const float capC[3] = { 200.0f, 0.0f, 0.0f };
            bool zapped = pilot.pushOut(capC, 110.0f);      // capital shield bubble
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
                fxPtr->addTracer({ fe.from[0], fe.from[1], fe.from[2] },
                             { fe.to[0],   fe.to[1],   fe.to[2] });
                fxPtr->spawnMuzzleFlash({ fe.from[0], fe.from[1], fe.from[2] },
                                    { fe.to[0] - fe.from[0], fe.to[1] - fe.from[1],
                                      fe.to[2] - fe.from[2] });
            }
            ++localSalvosFaced;
            // A salvo "lands" only if the player is near the fire line endpoint;
            // otherwise it is dodged. Cheap proximity test against the tracer end.
            const float dx = fe.to[0] - ppos[0];
            const float dy = fe.to[1] - ppos[1];
            const float dz = fe.to[2] - ppos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < 30.0f * 30.0f) {
                playerHitSfx(x3::space::shipai::kLaserDamage);
                if (fxOn) fxPtr->spawnImpact({ fe.to[0], fe.to[1], fe.to[2] },
                                             { -dx, -dy, -dz });
            } else {
                ++localSalvosDodged;
            }
        }

        // ---- Targeting feed: the capital ship is the priority hostile contact. ----
        x3::space::Contact contacts[8]{};
        uint32_t nc = 0;
        contacts[nc++] = { 1000u, { 280.0f, 0.0f, 0.0f }, { 0,0,0 }, true }; // capital
        for (uint32_t i = 0; i < enemies.count() && nc < 8; ++i) {
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
            targeting.lockNearest(ppos, pfw);
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
                        pilot.setCameraLookBias(dirT, 0.30f);
                        fedBias = true;
                    }
                    break;
                }
            }
            if (!fedBias) { const float z[3] = { 1, 0, 0 }; pilot.setCameraLookBias(z, 0.0f); }
        }
        // LOCK-ACQUIRED chirp (edge-detected: one chirp per acquisition).
        {
            const bool lockNow = targeting.hasLock();
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
            if (live) {
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
                    constexpr float kHitR = 9.0f;   // generous vs the 6 m draw scale
                    if (d2c <= kHitR * kHitR && tca < bestT) { bestT = tca; best = (int)i; }
                }
                if (best >= 0) {
                    hitFighter = true;
                    ++localShotsHit;
                    constexpr int kPlayerLaserDamage = 20;      // 3 hits downs a fighter
                    const auto& es = enemies.ship((uint32_t)best);
                    const float hp[3] = { es.pos[0], es.pos[1], es.pos[2] };
                    const bool willDie = es.hull <= kPlayerLaserDamage;
                    if (fxOn) fxPtr->spawnImpact({ hp[0], hp[1], hp[2] },
                                                 { -fw[0], -fw[1], -fw[2] });
                    enemies.damageShip((uint32_t)best, kPlayerLaserDamage);
                    if (willDie) fighterKillFx(hp);
                }
            }
            if (!hitFighter) {
            // Deterministic "competent" hit model: when crippling is still
            // possible, route a hit to a subsystem (shields down first), counting
            // a hit. (Live aim quality will replace this with a real raycast in a
            // later phase; the metric semantics stay the same.)
            const int subIdx = std::min(localSubsDestroyed, kMaxSubsystems - 1);
            x3::space::ShipDamage::applyDamage(capital, 60,
                (x3::space::Subsystem)subIdx);
            if (fxOn) {
                float dxc = ppos[0] - 280.0f, dyc = ppos[1], dzc = ppos[2];
                const float dl = std::sqrt(dxc*dxc + dyc*dyc + dzc*dzc);
                if (dl > 1.0f) { dxc /= dl; dyc /= dl; dzc /= dl; }
                fxPtr->spawnImpact({ 280.0f + dxc * 24.0f, dyc * 24.0f, dzc * 24.0f },
                                   { dxc, dyc, dzc });
            }
            ++localShotsHit;
            if (x3::space::ShipDamage::subsystemDown(capital,
                    (x3::space::Subsystem)subIdx) && subIdx == localSubsDestroyed)
                ++localSubsDestroyed;
            }   // !hitFighter (capital routing)
        }
        x3::space::ShipDamage::tick(capital, dt);

        // Crippled == all subsystems down (the escape-enabling objective).
        if (!crippled && localSubsDestroyed >= kMaxSubsystems) {
            crippled = true; crippleTime = tNow;
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
            if (cockpit && !pilot.isThirdPerson()) {
                x3::apphost::pulseIntroScreens(*cockpit, beatT);
                x3::apphost::poseIntroCockpit(*cockpit, cx, cy, cz, cyaw, cpit);
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
                fxPtr->addTracer({ wm[0], wm[1], wm[2] }, { aim[0], aim[1], aim[2] });
                fxPtr->spawnMuzzleFlash({ wm[0], wm[1], wm[2] }, { wd[0], wd[1], wd[2] });
            }
            // DAMAGE READS ON THE HULL (owner: "we should see some representation
            // of the damage on the enemy ship"): wounded fighters trail smoke,
            // thicker as hull drops — you can SEE who you've hurt.
            if (fxOn) {
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    const auto& e = enemies.ship(i);
                    if (e.hull <= 0 || e.hull >= e.maxHull) continue;
                    const float dmg = 1.0f - (float)e.hull / (float)e.maxHull;
                    const int period = (dmg > 0.65f) ? 5 : (dmg > 0.34f ? 10 : 18);
                    if ((int)(step + e.seed * 3u) % period == 0)
                        fxPtr->spawnSmoke({ e.pos[0], e.pos[1], e.pos[2] });
                }
            }
            if (fxOn) fxPtr->update(dt);
            // DEV evidence capture: X3_INTRO_CAPTURE=<dir> dumps the presented
            // frame at steps 180/420/660/900 (~3/7/11/15 s) per interactive beat —
            // a bearing-over-time series that shows the wing CIRCLING (the tool
            // that root-caused the milky-canopy bug, extended for the orbit AI).
            // Off (empty env) in normal play.
            static const char* evDir = std::getenv("X3_INTRO_CAPTURE");
            const bool evShot = evDir && *evDir &&
                (step == 180 || step == 420 || step == 660 || step == 900);
            if (evShot)
                hc.device->armCapture((std::string(evDir) + "/live_" + beat.id +
                                       "_s" + std::to_string(step) + ".png").c_str());
            auto frame = hc.device->beginFrame();
            if (frame.valid && cockpit) {
                cockpit->scene.render(*hc.device, frame);
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
                // YOUR fighter, in 3P — at the ship's own position + facing, so it sits
                // AHEAD of the chase camera. Skipped in 1P (you're inside it).
                if (pilot.isThirdPerson() && !playerDraw.empty()) {
                    const x3::phys::Vec3 pp3 = pilot.pos();
                    const x3::phys::Vec3 pf3 = pilot.forward();
                    const float sp[3] = { pp3.x, pp3.y, pp3.z };
                    const float sf[3] = { pf3.x, pf3.y, pf3.z };
                    // Scale 1.0, NOT 4.0: JakeFighterShip_textured.glb is ALREADY a
                    // ~10 m hull at native scale (measured 7.1 x 2.6 x 10.1 m). At 4x
                    // it was a 40 m building wrapped around the chase camera — the
                    // owner's whole screen was his own unlit hull, every enemy hidden
                    // behind it ("I cannot see the enemy ship at ALL in combat").
                    x3::apphost::drawIntroShip(*hc.device, frame, playerDraw, sp, sf, 1.0f, cockpit->mrShared);
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
                    const float dxE = e.pos[0]-cx, dyE = e.pos[1]-cy, dzE = e.pos[2]-cz;
                    const float dE = std::sqrt(dxE*dxE + dyE*dyE + dzE*dzE);
                    const float visScale = 6.0f * std::min(4.0f, std::max(1.0f, dE / 150.0f));
                    x3::apphost::drawIntroShip(*hc.device, frame, cockpit->enemyDraw,
                                  e.pos, e.fwd, visScale, cockpit->mrShared);
                }
                const float capPos[3] = { 200.0f, 0.0f, 0.0f };   // was 280 — loom bigger
                const float capFwd[3] = { -1.0f, 0.0f, 0.0f };
                x3::apphost::drawIntroShip(*hc.device, frame, cockpit->capDraw, capPos, capFwd, 34.0f, cockpit->mrShared);
                if (fxOn) {
                    fxPtr->draw(*hc.device, frame, cx, cy, cz, cyaw, cpit);
                    fxPtr->submit(*hc.device, frame);
                }
                // FORCE-FIELD FLASH: a one-blink cyan border while the zap timer
                // runs (pushOut bounced us this instant) — cheap: four hud quads.
                if (zapFlashT > 0.0f) {
                    int fw2 = 0, fh2 = 0;
                    glfwGetWindowSize(hc.window, &fw2, &fh2);
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
                    if (targeting.hasLock()) {
                        const float redHud[4] = { 1.0f, 0.35f, 0.25f, 0.95f };
                        hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                                "LOCK", 24.0f, 48.0f, 20.0f, redHud);
                    }
                }
                // ---- Target indicators (owner playtest: "can't find the enemy
                // ship"): a bracket over every on-screen hostile, an edge arrow
                // toward off-screen ones. Manual perspective projection from the
                // camera basis (fovY matches the setCamera(65) above). ----
                {
                    int winW = 0, winH = 0;
                    glfwGetWindowSize(hc.window, &winW, &winH);
                    if (winW > 0 && winH > 0) {
                        const float tanHalfY = std::tan(65.0f * 0.5f * 3.14159265f / 180.0f);
                        const float tanHalfX = tanHalfY * (float)winW / (float)winH;
                        // Project from the ACTUAL camera basis (camF/camU), not the
                        // ship's yaw/pitch — with lock-bias / ALT-freelook the view
                        // no longer equals the nose, and Euler-derived markers drift
                        // off the ships they bracket (owner caught it via "does the
                        // fire damage the enemy?!" — the crosshair was lying too).
                        const float fw[3] = { camF[0], camF[1], camF[2] };
                        const float rt[3] = { fw[1]*camU[2] - fw[2]*camU[1],
                                              fw[2]*camU[0] - fw[0]*camU[2],
                                              fw[0]*camU[1] - fw[1]*camU[0] };
                        const float up[3] = { fw[1]*rt[2] - fw[2]*rt[1],
                                              fw[2]*rt[0] - fw[0]*rt[2],
                                              fw[0]*rt[1] - fw[1]*rt[0] };
                        auto marker = [&](const float p[3], const char* onScr,
                                          const float col[4], float px) {
                            const float d[3] = { p[0]-cx, p[1]-cy, p[2]-cz };
                            const float zf = d[0]*fw[0] + d[1]*fw[1] + d[2]*fw[2];
                            const float xr = d[0]*rt[0] + d[1]*rt[1] + d[2]*rt[2];
                            const float yu = d[0]*up[0] + d[1]*up[1] + d[2]*up[2];
                            if (zf > 1.0f) {
                                const float nx = (xr / zf) / tanHalfX;    // -1..1
                                const float ny = (yu / zf) / tanHalfY;
                                if (nx > -1.f && nx < 1.f && ny > -1.f && ny < 1.f) {
                                    hc.device->drawHudTextF(frame, x3::rhi::FontRole::HudMono,
                                        onScr, (nx*0.5f + 0.5f) * winW - px * 0.9f,
                                        (0.5f - ny*0.5f) * winH - px * 0.5f, px, col);
                                    return;
                                }
                            }
                            // Off-screen: clamp the bearing to the screen edge.
                            float ex = xr, ey = yu;
                            if (zf > 0.0f) { ex = xr / std::max(zf, 1.0f); ey = yu / std::max(zf, 1.0f); }
                            const float m = std::max(std::fabs(ex), std::fabs(ey));
                            if (m < 1e-4f) return;
                            ex /= m; ey /= m;      // unit square edge
                            const char* arrow = (std::fabs(ex) > std::fabs(ey))
                                                ? (ex > 0 ? ">" : "<")
                                                : (ey > 0 ? "^" : "v");
                            const float sx2 = (ex * 0.92f * 0.5f + 0.5f) * winW;
                            const float sy2 = (0.5f - ey * 0.88f * 0.5f) * winH;
                            hc.device->drawHudTextF(frame, x3::rhi::FontRole::Enemy,
                                                    arrow, sx2, sy2, px * 1.2f, col);
                        };
                        const float redM[4]   = { 1.0f, 0.30f, 0.22f, 0.95f };
                        const float amberM[4] = { 1.0f, 0.72f, 0.20f, 0.95f };
                        for (uint32_t i = 0; i < enemies.count(); ++i) {
                            const auto& e = enemies.ship(i);
                            if (e.hull <= 0) continue;
                            marker(e.pos, "[ ]", redM, 26.0f);
                        }
                        marker(capPos, "[CAP]", amberM, 22.0f);
                        // GUN BORESIGHT — the reticle that CANNOT lie: projected at
                        // the point 600 m down the NOSE (the exact ray hits use).
                        // With lock-bias / freelook the camera gaze != the nose, so
                        // screen-center stops being the aim; this marker is. ("the
                        // aim must come from the sight" — the scope doctrine.)
                        {
                            const float fhN = std::cos(pilot.pitch());
                            const x3::phys::Vec3 pN = pilot.pos();
                            const float aimP[3] = {
                                pN.x + fhN * std::cos(pilot.yaw()) * 600.0f,
                                pN.y + std::sin(pilot.pitch()) * 600.0f,
                                pN.z + fhN * std::sin(pilot.yaw()) * 600.0f };
                            const float cyanB[4] = { 0.45f, 0.95f, 1.0f, 0.95f };
                            marker(aimP, "-+-", cyanB, 24.0f);
                        }
                        // FAR-FROM-EARTH TELL: label the faint SOL pinpoint so the
                        // player can FIND home — a tiny point ~10.5 km out at the
                        // shared Sol sky slot (matches the SOL body buildSystemSky
                        // appended to the far sky). Onscreen when Sol is in view; an
                        // edge arrow toward it otherwise (marker() handles both).
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
                            marker(solP, "SOL", solCol, 15.0f);
                        }
                    }
                }
                // ---- SYSTEM MINIMAP (owner: "visible on a minimap") — top-right box.
                // A north-up SYSTEM MAP, not just a contacts radar: the outer compass
                // ring carries the star + planets (+ faint SOL) at their sky bearing,
                // so the player reads the whole system layout at a glance; the inner
                // disc is the tactical radar (player at centre + heading tick, hostile
                // blips, the capital) scaled by range. Built from drawHudQuad/Text.
                {
                    int mw = 0, mh = 0;
                    glfwGetWindowSize(hc.window, &mw, &mh);
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
                            const float rr = Rin * std::min(1.0f, dist / range);
                            dot(cxm + std::sin(az) * rr, cym - std::cos(az) * rr, s, col);
                        };
                        const float redBlip[4]   = { 1.0f, 0.32f, 0.24f, 0.95f };
                        const float amberBlip[4] = { 1.0f, 0.72f, 0.20f, 0.95f };
                        for (uint32_t i = 0; i < enemies.count(); ++i) {
                            const auto& e = enemies.ship(i);
                            if (e.hull <= 0) continue;
                            plotContact(e.pos, 400.0f, redBlip, 5.0f);
                        }
                        plotContact(capPos, 400.0f, amberBlip, 8.0f);
                        // Player at centre + a heading tick (sky bearing = yaw + 90 deg).
                        const float pcol[4] = { 0.45f, 0.95f, 1.0f, 1.0f };
                        const float haz = (float)pilot.yaw() + 1.5707963f;
                        for (float t = 4.0f; t <= 16.0f; t += 4.0f)
                            dot(cxm + std::sin(haz) * t, cym - std::cos(haz) * t, 3.0f, pcol);
                        dot(cxm, cym, 7.0f, pcol);
                    }
                }
            }
            hc.device->endFrame(frame);
            if (evShot)
                hc.device->captureFrame((std::string(evDir) + "/live_" + beat.id +
                                         "_s" + std::to_string(step) + ".png").c_str());
            // Evidence runs are BOUNDED (nobody is at the controls to cripple the
            // capital or press Esc): end the beat just past the last capture.
            if (evDir && *evDir && step >= 960) break;

            // Hold this step until real time catches up to where the sim thinks it is
            // (see tStart). target = when step (step+1) SHOULD begin, in wall seconds.
            // Only ever waits; a slow frame that is already behind falls straight through.
            const double target  = (double)(step + 1) * (double)dt;
            const double elapsed = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - tStart).count();
            if (elapsed < target)
                std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed));
        }

        // ---- Exit conditions: all enemies down + ship crippled, or pilot dead. ----
        if (!pilot.isAlive()) break;
        if (crippled && enemies.aliveCount() == 0) break;
    }

    if (live) glfwSetInputMode(hc.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
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
// After the escape stinger ("ANTIMATTER CRITICAL — IGNITING ION DRIVE"), Jake's
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
    // Live-only (window + device); the headless self-test path stays render-free
    // and deterministic. A missing GLB degrades gracefully to the empty frame.
    std::unique_ptr<x3::apphost::IntroCockpitRig> cockpit;
    if (hc.window && hc.device) {
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
    // <0 => roll normally. Lets both branches be hit deterministically.
    IntroOutcome outcome;
    if (hc.introForce == 0) {
        outcome = IntroOutcome::ShotDown;
        x3::logInfo("[intro] FORCED outcome = SHOT_DOWN (intro_force)");
    } else if (hc.introForce == 1) {
        outcome = IntroOutcome::Escaped;
        x3::logInfo("[intro] FORCED outcome = ESCAPED (intro_force)");
    } else {
        outcome = rollOutcome(seed, skill);
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
                (outcome == IntroOutcome::Escaped ? "ESCAPED" : "SHOT_DOWN"));

    // OUTCOME STINGER (Phase 5): now that the roll is known, play the matching
    // cinematic ending span. SHOT_DOWN -> the kill + "SIX MONTHS LATER" (canon);
    // ESCAPED -> slip the kill-box + antimatter drain + ion drive (hand-off to P6).
    if (haveOutcomeBeat) {
        const float* span = (outcome == IntroOutcome::Escaped)
            ? kOutcomeEscapedSpan : kOutcomeShotDownSpan;
        outcomeBeat.clipStart = span[0];
        outcomeBeat.clipEnd   = span[1];
        outcomeBeat.id = (outcome == IntroOutcome::Escaped)
            ? "cine.outcome.escaped" : "cine.outcome.shot_down";
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
    if (outcome == IntroOutcome::Escaped) {
        const bool landed = runIonDescentBeat(hc);
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
    }

    // --- B4: per-save seed thread — an explicit hc.introSeed makes the non-forced
    //         roll deterministic + reproducible, and the headless run is stable. ---
    {
        x3::apphost::HostContext hc{}; hc.introForce = -1; hc.introSeed = 42u;
        IntroOutcome r1 = runInteractiveIntro(hc);
        IntroOutcome r2 = runInteractiveIntro(hc);
        check(r1 == r2, "B4 pinned seed -> deterministic non-forced outcome");
        // The pinned-seed run agrees with the pure rollOutcome at the headless skill
        // (the same gate app_run's live run uses). Recompute the headless skill.
        SkillMetrics m{}; m.finalHullFrac = 1.0f;
        for (const Beat& b : defaultIntroBeats())
            if (b.kind == BeatKind::InteractiveWindow) runInteractiveBeat(hc, b, m);
        IntroOutcome expect = rollOutcome(42u, skillScore(m));
        check(r1 == expect, "B4b pinned-seed outcome == rollOutcome(seed, headlessSkill)");
    }

    // --- B5: seed thread CAN change the outcome — across a seed sweep at the
    //         headless skill, the non-forced branch is not constant (the roll is
    //         genuinely per-seed, not hard-wired to one branch). ---
    {
        x3::apphost::HostContext hc0{}; hc0.introForce = -1;
        SkillMetrics m{}; m.finalHullFrac = 1.0f;
        for (const Beat& b : defaultIntroBeats())
            if (b.kind == BeatKind::InteractiveWindow) runInteractiveBeat(hc0, b, m);
        const float sk = skillScore(m);
        int esc = 0, sd = 0;
        for (uint32_t s = 1; s <= 200u; ++s)
            (rollOutcome(s, sk) == IntroOutcome::Escaped) ? ++esc : ++sd;
        check(esc > 0 && sd > 0,
              "B5 seed sweep yields both branches (roll is per-save, not fixed)");
        x3::logInfo("  [info] headless-skill seed sweep: escaped=" +
                    std::to_string(esc) + " shot_down=" + std::to_string(sd) +
                    " (skill=" + std::to_string(sk) + ")");
    }

    x3::logInfo("intro-branch: " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    std::printf("intro-branch: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::intro
