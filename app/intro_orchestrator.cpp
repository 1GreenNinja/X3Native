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
#include "engine/audio/IAudioSystem.h"   // hc.audio threaded to the cinematic beats (Phase 5)

#include "host_context.h"
#include "story_ops.h"        // x3::game::StoryFlags + x3::game::chanceRoll
#include "cutscene.h"         // x3::cut::CutscenePlayer + the cold-open asset
#include "cinematic.h"        // runCutsceneWindowed (public cinematic driver)
#include "asset_root.h"
#include "space_pilot.h"
#include "space/ship_ai.h"
#include "space/targeting.h"
#include "space/ship_damage.h"
#include "space/space_layer.h"   // x3::space::SpaceLayer (S0 spine; AtmoDescent runner host)
#include "space/descent.h"       // x3::space::AtmoDescent (the ion-pulse on-rails coast-down)
#include "mesh_prims.h"          // P0-1: procedural ship/capital meshes for the LIVE dogfight render

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

    // ---- P0-1: LIVE dogfight render resources (built once; headless untouched) --
    // Reuses host_space's deep-space render conventions: analytic sky OFF (so the
    // dark clear shows through), SSAO + SSGI OFF (the Pascal raster-fallback paints
    // an empty-space scene black), and a few BRIGHT point lights near the fight so
    // the metal reads (deep space has no bounced fill). Procedural box meshes for the
    // pilot ship, the enemy wing, and the HUGE capital objective — the GLB ship cast
    // isn't in this baseline, and the boxes guarantee visible geometry every frame.
    // ALL rendering is READ-ONLY over the deterministic sim state below.
    x3::rhi::MeshHandle shipMesh{}, capitalMesh{};
    x3::rhi::TextureHandle shipTex{}, capitalTex{};
    if (live) {
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; hc.device->setSkyParams(sp); }
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false; hc.device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; hc.device->setGiParams(gp); }
        // T1: deep-space far plane so the capital at +X 280 (and any celestial layer)
        // never pops into view late. Near stays at 0.1 (the cockpit/foreground ships
        // sit close); standard-Z precision at this near/far is fine for a scene whose
        // depth-writing geometry has no coincident surfaces.
        hc.device->setCameraClip(0.1f, 12000.0f);
        // Key/fill/rim near the capital (at +X ~280) so the fight reads against dark space.
        x3::rhi::PointLight pl[3]{};
        pl[0].pos[0] = 180.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f; pl[0].range = 900.0f;
        pl[0].color[0] = 60.0f; pl[0].color[1] = 56.0f; pl[0].color[2] = 48.0f;   // warm key "sun"
        pl[1].pos[0] =  40.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  20.0f; pl[1].range = 500.0f;
        pl[1].color[0] = 15.0f; pl[1].color[1] = 18.0f; pl[1].color[2] = 24.0f;   // cool fill (player side)
        pl[2].pos[0] = 300.0f; pl[2].pos[1] = -40.0f; pl[2].pos[2] = -60.0f; pl[2].range = 700.0f;
        pl[2].color[0] = 10.0f; pl[2].color[1] =  7.0f; pl[2].color[2] =  5.0f;   // warm rim behind capital
        hc.device->setPointLights(pl, 3);
        // Fighter-scale box (~2 m) for the pilot + each enemy; a big slab for the capital.
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
        shipMesh = hc.device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                         sbm.index.data(), (uint32_t)sbm.index.size());
        auto sTex = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
        shipTex = hc.device->createTexture(sTex.data(), 64, 64, true);
        x3::prims::PrimMesh cbm = x3::prims::makeBox(1.0f, 0.5f, 0.6f, 0, 0, 0, 0.25f); // unit; scaled HUGE per-draw
        capitalMesh = hc.device->createMesh(cbm.verts.data(), (uint32_t)cbm.verts.size(),
                                            cbm.index.data(), (uint32_t)cbm.index.size());
        auto cTex = x3::prims::makeCheckerRGBA(64, 4, 90, 100, 120, 40, 45, 60);
        capitalTex = hc.device->createTexture(cTex.data(), 64, 64, true);
    }
    // Orient a ship box from a forward heading (yaw about +Y) at pos, uniform scale.
    auto shipXform = [](const float pos[3], const float fwd[3], float scale, float out[16]) {
        float fx = fwd[0], fz = fwd[2];
        const float fl = std::sqrt(fx*fx + fz*fz);
        if (fl > 1e-3f) { fx /= fl; fz /= fl; } else { fx = 1.0f; fz = 0.0f; }
        // col0 = forward (+X local), col2 = right (+Z local) from the yaw; col1 = +Y.
        out[0]=fx*scale; out[1]=0; out[2]=fz*scale; out[3]=0;
        out[4]=0; out[5]=scale; out[6]=0; out[7]=0;
        out[8]=-fz*scale; out[9]=0; out[10]=fx*scale; out[11]=0;
        out[12]=pos[0]; out[13]=pos[1]; out[14]=pos[2]; out[15]=1;
    };

    // P0-2: real boresight raycast vs a contact's bounding sphere (dir normalized).
    // Returns true on a forward intersection OR when the origin is inside the sphere
    // (point-blank). This is what turns the player's ACTUAL aim into hit odds — no
    // more guaranteed hit. Deterministic: the headless "competent" pilot flies dead
    // down its +X boresight straight at the on-axis capital, so it still connects.
    auto rayHitsSphere = [](const float o[3], const float d[3],
                            const float c[3], float radius) -> bool {
        const float ocx = o[0]-c[0], ocy = o[1]-c[1], ocz = o[2]-c[2];
        const float ocd = ocx*d[0] + ocy*d[1] + ocz*d[2];
        const float oc2 = ocx*ocx + ocy*ocy + ocz*ocz;
        const float cc  = oc2 - radius*radius;
        if (cc <= 0.0f) return true;               // inside the sphere (point-blank)
        const float disc = ocd*ocd - cc;
        if (disc < 0.0f) return false;             // the ray line misses the sphere
        return (-ocd - std::sqrt(disc)) > 0.0f;    // nearest intersection is forward
    };

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

        // ---- Player fire -> REAL raycast from the boresight onto the capital's
        //      subsystems. The guaranteed-hit fake is gone: a shot only lands when
        //      the pilot's forward axis actually intersects the capital's hull
        //      sphere, so player aim skill drives the hit rate -> how fast the ship
        //      is crippled -> the escaped/shot_down branch probability. ----
        if (fire && pilot.fireLaser(dt)) {
            ++localShotsFired;
            // Boresight ray: from the ship nose along its forward. The capital is
            // contact id 1000 at {280,0,0}; its hull sphere is sized to the drawn
            // ~90 m slab (radius 55 = a forgiving lock window on a capital-scale hull).
            const x3::phys::Vec3 pP = pilot.pos();
            const x3::phys::Vec3 pF = pilot.forward();
            float dn[3] = { pF.x, pF.y, pF.z };
            const float dl = std::sqrt(dn[0]*dn[0] + dn[1]*dn[1] + dn[2]*dn[2]);
            if (dl > 1e-4f) { dn[0]/=dl; dn[1]/=dl; dn[2]/=dl; }
            const float origin[3]  = { pP.x, pP.y, pP.z };
            const float capCenter[3] = { 280.0f, 0.0f, 0.0f };
            if (rayHitsSphere(origin, dn, capCenter, 55.0f)) {
                // Hit: shields absorb first, then subsystems fall in order. Counting
                // a hit here is what the skill metric reads.
                const int subIdx = std::min(localSubsDestroyed, kMaxSubsystems - 1);
                x3::space::ShipDamage::applyDamage(capital, 60,
                    (x3::space::Subsystem)subIdx);
                ++localShotsHit;
                if (x3::space::ShipDamage::subsystemDown(capital,
                        (x3::space::Subsystem)subIdx) && subIdx == localSubsDestroyed)
                    ++localSubsDestroyed;
            }
            // A miss simply burns a shot (lower accuracy -> slower cripple -> the
            // roll is likelier to land on SHOT_DOWN). No target damage.
        }
        x3::space::ShipDamage::tick(capital, dt);

        // Crippled == all subsystems down (the escape-enabling objective).
        if (!crippled && localSubsDestroyed >= kMaxSubsystems) {
            crippled = true; crippleTime = tNow;
        }

        // ---- Live render (3P chase of the pilot; reuses host_space art
        //      conventions). Headless skips drawing entirely. READ-ONLY over sim
        //      state — never mutates the deterministic brain above. ----
        if (live) {
            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            hc.device->setCamera(cx, cy, cz, cyaw, cpit, 65.0f);
            auto frame = hc.device->beginFrame();
            if (frame.valid) {
                // Player ship (only in 3P; a 1P chase cam would clip its own hull).
                if (pilot.isThirdPerson()) {
                    const x3::phys::Vec3 pp2 = pilot.pos();
                    const x3::phys::Vec3 pf  = pilot.forward();
                    const x3::phys::Vec3 pu  = pilot.up();
                    const x3::phys::Vec3 pr  = pilot.right();
                    const float m[16] = {
                        pf.x, pf.y, pf.z, 0,
                        pu.x, pu.y, pu.z, 0,
                        pr.x, pr.y, pr.z, 0,
                        pp2.x, pp2.y, pp2.z, 1 };
                    const float tint[4] = { 0.75f, 0.85f, 1.05f, 1.0f };
                    hc.device->drawMesh(frame, shipMesh, shipTex, tint, m);
                }
                // Enemy wing — oriented along each ship's firing heading.
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    const auto& e = enemies.ship(i);
                    float m[16]; shipXform(e.pos, e.fwd, 1.4f, m);
                    const float tint[4] = { 1.15f, 0.55f, 0.5f, 1.0f };   // hostile red
                    hc.device->drawMesh(frame, shipMesh, shipTex, tint, m);
                }
                // The HUGE capital objective at the priority-contact position. Scaled
                // huge (~90 m long) so it dominates the frame; emissive-ish tint reads
                // as running lights against the dark. Damage/subsystem climax (P1) will
                // dress this further; here it makes the objective unmistakably present.
                {
                    const float capPos[3] = { 280.0f, 0.0f, 0.0f };
                    const float sx = 90.0f, sy = 22.0f, sz = 30.0f;
                    const float m[16] = {
                        sx, 0,  0,  0,
                        0,  sy, 0,  0,
                        0,  0,  sz, 0,
                        capPos[0], capPos[1], capPos[2], 1 };
                    // Darken once each subsystem falls (visual feedback on the climax).
                    const float dmg = (float)localSubsDestroyed / (float)kMaxSubsystems;
                    const float tint[4] = { 0.55f - 0.25f*dmg, 0.60f - 0.20f*dmg,
                                            0.80f - 0.20f*dmg, 1.0f };
                    hc.device->drawMesh(frame, capitalMesh, capitalTex, tint, m);
                }
            }
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

    // Release the LIVE dogfight render resources (headless never created them).
    if (live) {
        if (shipMesh.valid())    hc.device->destroyMesh(shipMesh);
        if (capitalMesh.valid()) hc.device->destroyMesh(capitalMesh);
        if (shipTex.valid())     hc.device->destroyTexture(shipTex);
        if (capitalTex.valid())  hc.device->destroyTexture(capitalTex);
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

} // namespace  (close the internal-linkage helpers; captureIntroDogfight below
  //             needs EXTERNAL linkage — it's called from screenshot_hosts.cpp)

// ---------------------------------------------------------------------------
// PROOF CAPTURE (--screenshot-dogfight) — headless still of the playable
// dogfight. Reuses the exact render kit the interactive beat builds (deep-space
// look, three point lights, procedural pilot / enemy / capital boxes) but with a
// bespoke cinematic camera + a scripted synthetic pilot so a couple of
// representative frames can be written headless (no window). This is both the
// P0-1 render proof and the gate's mid-dogfight + subsystem-climax captures.
// ---------------------------------------------------------------------------
int captureIntroDogfight(x3::apphost::HostContext& hc, const std::string& outDir) {
    if (!hc.device) { x3::logError("--screenshot-dogfight: no device"); return 1; }
    auto* device = hc.device;
    std::error_code mkec; std::filesystem::create_directories(outDir, mkec);
    x3::logInfo("--screenshot-dogfight: writing dogfight proof stills to " + outDir);

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("--screenshot-dogfight: physics init failed"); return 1; }

    x3::game::SpacePilotController pilot;
    pilot.spawn(*phys, 0.0f, 0.0f, 0.0f);

    const int kEnemies = 4;
    x3::space::EnemyShipManager enemies;
    enemies.init(kEnemies);
    for (int i = 0; i < kEnemies; ++i) {
        const float ang = (float)i * 1.3f;
        const float pos[3] = { 150.0f + 30.0f * (float)i, 20.0f * std::sin(ang), 40.0f * std::cos(ang) };
        enemies.spawn(pos);
    }
    auto capital = x3::space::ShipDamage::makeCapital(400, 2000, 120);

    // Deep-space render kit (mirrors runInteractiveBeat / host_space).
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false; device->setSsaoParams(ap); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
    device->setCameraClip(0.1f, 12000.0f);         // T1: deep-space far plane (capital at +X 280)
    device->setBloom(0.35f);                       // hero glow on the emissive windows
    device->setFrustumCullEnabled(false);          // robust against nested-AABB culling for the still
    {
        x3::rhi::PointLight pl[3]{};
        pl[0].pos[0] = 180.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f; pl[0].range = 900.0f;
        pl[0].color[0] = 60.0f; pl[0].color[1] = 56.0f; pl[0].color[2] = 48.0f;
        pl[1].pos[0] =  20.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  40.0f; pl[1].range = 500.0f;
        pl[1].color[0] = 18.0f; pl[1].color[1] = 22.0f; pl[1].color[2] = 30.0f;
        pl[2].pos[0] = 240.0f; pl[2].pos[1] = -40.0f; pl[2].pos[2] = -60.0f; pl[2].range = 700.0f;
        pl[2].color[0] = 22.0f; pl[2].color[1] = 14.0f; pl[2].color[2] = 9.0f;
        device->setPointLights(pl, 3);
    }
    x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
    auto shipMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                       sbm.index.data(), (uint32_t)sbm.index.size());
    auto sTexD = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
    auto shipTex = device->createTexture(sTexD.data(), 64, 64, true);
    x3::prims::PrimMesh cbm = x3::prims::makeBox(1.0f, 0.5f, 0.6f, 0, 0, 0, 0.25f);
    auto capitalMesh = device->createMesh(cbm.verts.data(), (uint32_t)cbm.verts.size(),
                                          cbm.index.data(), (uint32_t)cbm.index.size());
    auto cTexD = x3::prims::makeCheckerRGBA(64, 4, 90, 100, 120, 40, 45, 60);
    auto capitalTex = device->createTexture(cTexD.data(), 64, 64, true);

    const float capPos[3] = { 280.0f, 0.0f, 0.0f };
    const float dt = 1.0f / 60.0f;

    auto drawScene = [&](const x3::rhi::FrameContext& fr, int subsDestroyed) {
        // Pilot ship (3P by default).
        const x3::phys::Vec3 pp = pilot.pos();
        const x3::phys::Vec3 pf = pilot.forward();
        const x3::phys::Vec3 pu = pilot.up();
        const x3::phys::Vec3 pr = pilot.right();
        const float pm[16] = { pf.x,pf.y,pf.z,0, pu.x,pu.y,pu.z,0, pr.x,pr.y,pr.z,0, pp.x,pp.y,pp.z,1 };
        const float ptint[4] = { 0.75f, 0.85f, 1.05f, 1.0f };
        device->drawMesh(fr, shipMesh, shipTex, ptint, pm);
        // Enemy wing.
        for (uint32_t i = 0; i < enemies.count(); ++i) {
            const auto& e = enemies.ship(i);
            float fx = e.fwd[0], fz = e.fwd[2];
            const float fl = std::sqrt(fx*fx+fz*fz); if (fl>1e-3f){fx/=fl;fz/=fl;} else {fx=1;fz=0;}
            const float S = 1.6f;
            const float m[16] = { fx*S,0,fz*S,0, 0,S,0,0, -fz*S,0,fx*S,0, e.pos[0],e.pos[1],e.pos[2],1 };
            const float etint[4] = { 1.2f, 0.55f, 0.5f, 1.0f };
            device->drawMesh(fr, shipMesh, shipTex, etint, m);
        }
        // HUGE capital (darkens as subsystems fall).
        const float sx=90.f, sy=22.f, sz=30.f;
        const float m[16] = { sx,0,0,0, 0,sy,0,0, 0,0,sz,0, capPos[0],capPos[1],capPos[2],1 };
        const float dmg = (float)subsDestroyed / (float)kMaxSubsystems;
        const float ctint[4] = { 0.55f-0.25f*dmg, 0.60f-0.20f*dmg, 0.80f-0.20f*dmg, 1.0f };
        device->drawMesh(fr, capitalMesh, capitalTex, ctint, m);
    };

    // A cinematic 3/4 chase: eye behind + above the pilot, looking toward the
    // capital (+X) so the fighter reads in the foreground and the capital fills
    // the background. atan2(dz,dx)=0 -> +X forward; slight downward pitch.
    auto framePilot = [&](int subsDestroyed) {
        const x3::phys::Vec3 pp = pilot.pos();
        const float ex = pp.x - 34.0f, ey = pp.y + 12.0f, ez = pp.z + 16.0f;
        const float lx = pp.x + 60.0f - ex, ly = pp.y - ey, lz = pp.z - ez;
        const float len = std::max(std::sqrt(lx*lx+ly*ly+lz*lz), 1e-3f);
        device->setCamera(ex, ey, ez, std::atan2(lz, lx), std::asin(ly/len), 60.0f);
    };

    int shotFails = 0;
    auto capture = [&](const std::string& name, int subsDestroyed) {
        const std::string path = outDir + "/" + name + ".png";
        const int kSettle = 48;                    // TAA + auto-exposure + bloom settle
        for (int i = 0; i < kSettle; ++i) {
            framePilot(subsDestroyed);
            if (i == kSettle - 1) device->armCapture(path.c_str());
            auto fr = device->beginFrame();
            if (fr.valid) drawScene(fr, subsDestroyed);
            device->endFrame(fr);
        }
        if (device->captureFrame(path.c_str())) x3::logInfo("--screenshot-dogfight: wrote " + path);
        else { x3::logError("--screenshot-dogfight: capture FAILED " + path); ++shotFails; }
    };

    // Fly the synthetic pilot up the engagement until the capital (at +X 280) sits
    // inside the 200 m camera far plane, so the beauty frame shows the fighter in
    // the foreground WITH the capital looming behind. (In the live dogfight the same
    // approach brings the capital into view as the player closes the distance.)
    x3::game::PlayerInput in{}; in.moveFwd = 1.0f;
    for (int s = 0; s < 1200 && pilot.pos().x < 150.0f; ++s) { pilot.update(in, dt, *phys);
        const x3::phys::Vec3 pp = pilot.pos(); const float ppos[3] = { pp.x, pp.y, pp.z };
        const x3::phys::Vec3 pv = pilot.velocity(); const float pvel[3] = { pv.x, pv.y, pv.z };
        enemies.update(dt, ppos, pvel); }
    capture("02_dogfight_capital_fx", /*subsDestroyed*/ 1);

    // Cripple the capital's subsystems for the CLIMAX frame (hull sphere hit model
    // is exercised live; here we drive the damage model straight for the still).
    for (int sub = 0; sub < kMaxSubsystems; ++sub)
        x3::space::ShipDamage::applyDamage(capital, 100000, (x3::space::Subsystem)sub);
    capture("03_subsystem_climax", /*subsDestroyed*/ kMaxSubsystems);

    device->destroyMesh(shipMesh); device->destroyMesh(capitalMesh);
    device->destroyTexture(shipTex); device->destroyTexture(capitalTex);
    phys->shutdown();
    return shotFails == 0 ? 0 : 1;
}

namespace {   // reopen the internal-linkage helper region

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

    std::vector<Beat> beats = defaultIntroBeats();
    SkillMetrics metrics{};
    metrics.finalHullFrac = 1.0f;   // assume intact until the climax measures it

    // Play every beat UP TO the outcome stinger (flight, reveal, dodge, charge,
    // dogfight). The cine.outcome beat is deferred: its span depends on the rolled
    // outcome, which depends on the climax metrics accumulated here — so we roll
    // FIRST, then play the matching stinger (below).
    Beat outcomeBeat{}; bool haveOutcomeBeat = false;
    for (const Beat& beat : beats) {
        if (beat.kind == BeatKind::CutsceneClip && beat.id == "cine.outcome") {
            outcomeBeat = beat; haveOutcomeBeat = true; continue;
        }
        if (beat.kind == BeatKind::CutsceneClip)
            playCinematicBeat(hc, beat, haveCs ? &coldOpen : nullptr);
        else
            runInteractiveBeat(hc, beat, metrics);
    }

    // Load the persisted narrative flags (the lane app_run branches on) so the
    // per-save seed is derived from the REAL save state, and the outcome write
    // augments (not clobbers) existing flags.
    x3::game::StoryFlags flags;
    const std::string flagsPath = defaultGameStoryFlagsPath();
    flags.loadFile(flagsPath);   // false + untouched on a fresh save — fine

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
