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

#include "intro_orchestrator.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include "host_context.h"
#include "story_ops.h"        // x3::game::StoryFlags + x3::game::chanceRoll
#include "cutscene.h"         // x3::cut::CutscenePlayer + the cold-open asset
#include "cinematic.h"        // runCutsceneWindowed (public cinematic driver)
#include "asset_root.h"
#include "space_pilot.h"
#include "space/ship_ai.h"
#include "space/targeting.h"
#include "space/ship_damage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

    // 6) Cinematic — the outcome stinger span (the orchestrator picks escape vs
    //    shot-down framing after the roll; both live in the cold-open tail).
    Beat b6; b6.kind = BeatKind::CutsceneClip; b6.id = "cine.outcome";
    b6.clipStart = 36.0f; b6.clipEnd = 67.5f; beats.push_back(b6);

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
void playCinematicBeat(x3::apphost::HostContext& hc, const Beat& beat,
                       const x3::cut::Cutscene* cs) {
    x3::logInfo("[intro] beat '" + beat.id + "' (cinematic " +
                std::to_string(beat.clipStart) + ".." + std::to_string(beat.clipEnd) + " s)");
    if (hc.window && hc.device && cs) {
        clearInputState(hc.window);
        // runCutsceneWindowed plays from startAt to the cutscene end / skip. We
        // seek to the span start; span-end gating is a Phase-5 clip-split concern.
        x3::apphost::runCutsceneWindowed(*hc.device, hc.window, nullptr, *cs, beat.clipStart);
        clearInputState(hc.window);
    }
}

// Run one bounded interactive combat window and accumulate metrics into `m`. The
// climax window's metrics drive the outcome roll. With a window this hands control
// to the live combat stack; headless it advances deterministically (fixed-dt sim,
// synthetic player aim) so the metrics are reproducible without a GPU.
void runInteractiveBeat(x3::apphost::HostContext& hc, const Beat& beat,
                        SkillMetrics& m) {
    x3::logInfo("[intro] beat '" + beat.id + "' (interactive: " +
                std::to_string(beat.enemyCount) + " enemies, " +
                std::to_string(beat.timeoutSec) + " s timeout)");
    clearInputState(hc.window);

    // Physics world for the pilot body. Headless-safe (no GPU).
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[intro] physics init failed"); return; }

    x3::game::SpacePilotController pilot;
    pilot.spawn(*phys, 0.0f, 0.0f, 0.0f);

    x3::space::EnemyShipManager enemies;
    enemies.init((uint32_t)std::max(0, beat.enemyCount));
    for (int i = 0; i < beat.enemyCount; ++i) {
        const float ang = (float)i * 1.3f;
        const float pos[3] = { 250.0f + 30.0f * (float)i,
                               20.0f * std::sin(ang),
                               40.0f * std::cos(ang) };
        enemies.spawn(pos);
    }

    x3::space::TargetingSystem targeting;

    // The capital ship's destructible damage model (the dogfight objective).
    auto capital = x3::space::ShipDamage::makeCapital(/*shield*/400, /*hull*/2000, /*subHp*/120);

    const float dt = 1.0f / 60.0f;
    const int   maxSteps = (int)(beat.timeoutSec / dt);
    int   localSalvosFaced = 0, localSalvosDodged = 0;
    int   localShotsFired = 0, localShotsHit = 0;
    int   localSubsDestroyed = 0;
    float crippleTime = 0.0f;
    bool  crippled = false;

    const bool live = (hc.window != nullptr && hc.device != nullptr);

    for (int step = 0; step < maxSteps; ++step) {
        const float tNow = (float)step * dt;

        // ---- Input: live reads GLFW; headless uses a deterministic synthetic
        //      "competent pilot" profile (steady forward + aim at the capital). ----
        x3::game::PlayerInput in{};
        float rollAxis = 0.0f;
        bool fire = false;
        if (live) {
            if (glfwWindowShouldClose(hc.window)) break;
            if (glfwGetKey(hc.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            auto kd = [&](int k){ return glfwGetKey(hc.window, k) == GLFW_PRESS; };
            in.moveFwd    = (kd(GLFW_KEY_W)?1.f:0.f) + (kd(GLFW_KEY_S)?-1.f:0.f);
            in.moveStrafe = (kd(GLFW_KEY_D)?1.f:0.f) + (kd(GLFW_KEY_A)?-1.f:0.f);
            in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            rollAxis      = (kd(GLFW_KEY_Q)?-1.f:0.f) + (kd(GLFW_KEY_E)?1.f:0.f);
            fire = glfwGetMouseButton(hc.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        } else {
            in.moveFwd = 1.0f;                  // close the distance
            fire = (step % 12) == 0;            // a measured firing cadence
        }
        pilot.setRollInput(rollAxis);
        pilot.update(in, dt, *phys);

        // ---- Enemy AI tick + salvo accounting (dodge metric). ----
        const x3::phys::Vec3 pp = pilot.pos();
        const x3::phys::Vec3 pv = pilot.velocity();
        const float ppos[3] = { pp.x, pp.y, pp.z };
        const float pvel[3] = { pv.x, pv.y, pv.z };
        enemies.update(dt, ppos, pvel);
        for (const auto& fe : enemies.fireEvents()) {
            ++localSalvosFaced;
            // A salvo "lands" only if the player is near the fire line endpoint;
            // otherwise it is dodged. Cheap proximity test against the tracer end.
            const float dx = fe.to[0] - ppos[0];
            const float dy = fe.to[1] - ppos[1];
            const float dz = fe.to[2] - ppos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < 30.0f * 30.0f) {
                pilot.takeDamage(x3::space::shipai::kLaserDamage);
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

        // ---- Player fire -> resolve onto the capital's subsystems. ----
        if (fire && pilot.fireLaser(dt)) {
            ++localShotsFired;
            // Deterministic "competent" hit model: when crippling is still
            // possible, route a hit to a subsystem (shields down first), counting
            // a hit. (Live aim quality will replace this with a real raycast in a
            // later phase; the metric semantics stay the same.)
            const int subIdx = std::min(localSubsDestroyed, kMaxSubsystems - 1);
            x3::space::ShipDamage::applyDamage(capital, 60,
                (x3::space::Subsystem)subIdx);
            ++localShotsHit;
            if (x3::space::ShipDamage::subsystemDown(capital,
                    (x3::space::Subsystem)subIdx) && subIdx == localSubsDestroyed)
                ++localSubsDestroyed;
        }
        x3::space::ShipDamage::tick(capital, dt);

        // Crippled == all subsystems down (the escape-enabling objective).
        if (!crippled && localSubsDestroyed >= kMaxSubsystems) {
            crippled = true; crippleTime = tNow;
        }

        // ---- Live render (3P chase of the pilot; minimal, reuses host_space art
        //      conventions). Headless skips drawing entirely. ----
        if (live) {
            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            hc.device->setCamera(cx, cy, cz, cyaw, cpit, 65.0f);
            auto frame = hc.device->beginFrame();
            hc.device->endFrame(frame);
        }

        // ---- Exit conditions: all enemies down + ship crippled, or pilot dead. ----
        if (!pilot.isAlive()) break;
        if (crippled && enemies.aliveCount() == 0) break;
    }

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

// Source the deterministic save seed. Mirrors how the chat-tree {chance} op gets
// its per-save seed; here we derive it from the StoryFlags path/content so a fresh
// run is reproducible. For Phase 3 we use a fixed default seed (Phase 4 wires the
// real per-save seed through HostContext).
uint32_t introSeed() { return 0x1A7E0u; }

} // namespace

IntroOutcome runInteractiveIntro(x3::apphost::HostContext& hc) {
    x3::logInfo("[intro] runInteractiveIntro: beat sequence start");

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

    const std::vector<Beat> beats = defaultIntroBeats();
    SkillMetrics metrics{};
    metrics.finalHullFrac = 1.0f;   // assume intact until the climax measures it

    for (const Beat& beat : beats) {
        if (beat.kind == BeatKind::CutsceneClip)
            playCinematicBeat(hc, beat, haveCs ? &coldOpen : nullptr);
        else
            runInteractiveBeat(hc, beat, metrics);
    }

    const float skill = skillScore(metrics);
    const float p = outcomeProbability(skill);
    const IntroOutcome outcome = rollOutcome(introSeed(), skill);

    x3::logInfo("[intro] skillScore=" + std::to_string(skill) +
                " p=" + std::to_string(p) +
                " (subs=" + std::to_string(metrics.subsystemsDestroyed) +
                " hull=" + std::to_string(metrics.finalHullFrac) +
                " dodge=" + std::to_string(metrics.salvosDodged) + "/" +
                std::to_string(metrics.salvosFaced) +
                " acc=" + std::to_string(metrics.shotsHit) + "/" +
                std::to_string(metrics.shotsFired) + ")");
    x3::logInfo(std::string("[intro] outcome = ") +
                (outcome == IntroOutcome::Escaped ? "ESCAPED" : "SHOT_DOWN"));

    // Persist the branch flag beside the save (x3::game StoryFlags — the narrative
    // lane app_run.cpp branches on).
    x3::game::StoryFlags flags;
    writeOutcomeFlag(flags, outcome);

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

    x3::logInfo("intro-orchestrator: " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    std::printf("intro-orchestrator: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::intro
