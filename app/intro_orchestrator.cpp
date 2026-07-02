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
#include "fx.h"                   // x3::game::CombatFx — bolt/tracer/impact/explosion FX (live windows)
#include "mesh_prims.h"           // x3::prims — box/sphere fallback meshes for the live scene
#include "engine/asset/IModelLoader.h"   // x3::asset model load + makeDrawables + mulMat4 (live ship draw)
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

// ---------------------------------------------------------------------------
// LIVE COMBAT SCENE (Job B) — the drawable + FX side of an interactive window.
//
// The interactive windows used to run the sim but render a BLANK frame (begin/
// endFrame with nothing between) — so the player saw a black void and there was
// "no option to take control". This owns the GPU resources + draw primitives so
// the window actually renders: the menacing capital, the enemy fighters, Jake's
// fighter (3P), the bolts (CombatFx tracers), and the HUD. Headless windows never
// touch this (the deterministic sim path is unchanged).
// ---------------------------------------------------------------------------

// Build a ship model matrix from a pilot-style basis (forward/up/right) at `scale`.
inline void shipMatrix(const x3::phys::Vec3& f, const x3::phys::Vec3& u,
                       const x3::phys::Vec3& r, const x3::phys::Vec3& p,
                       float scale, float out[16]) {
    out[0]=f.x*scale; out[1]=f.y*scale; out[2]=f.z*scale; out[3]=0;
    out[4]=u.x*scale; out[5]=u.y*scale; out[6]=u.z*scale; out[7]=0;
    out[8]=r.x*scale; out[9]=r.y*scale; out[10]=r.z*scale; out[11]=0;
    out[12]=p.x; out[13]=p.y; out[14]=p.z; out[15]=1;
}
inline x3::phys::Vec3 vnorm(const x3::phys::Vec3& v) {
    const float l = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    return (l > 1e-5f) ? x3::phys::Vec3{ v.x/l, v.y/l, v.z/l } : x3::phys::Vec3{ 1, 0, 0 };
}
inline x3::phys::Vec3 vcross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

// The enemy capital ship placement + its destructible "engine block" weak points
// (world space; the capital faces -X toward the player, who spawns at origin and
// closes along +X). Four blocks mirror x3::space::Subsystem (Engines/Turrets/
// ShieldGen/Sensors). Shooting a lit block damages that subsystem; all down ==
// crippled == the escape-enabling objective.
struct CapitalRig {
    float pos[3]   = { 288.0f, 48.0f, 0.0f };   // hull world position (looms ahead + above)
    float scale    = 22.0f;                     // model-matrix scale for SpaceShip4.glb
    float turret[3]= { 258.0f, 72.0f, 0.0f };   // main-gun muzzle (telegraphed return fire)
    static constexpr int kWeak = kMaxSubsystems;
    float weak[kWeak][3] = {
        { 226.0f, 32.0f, -40.0f }, { 226.0f, 32.0f,  40.0f },
        { 234.0f, 60.0f, -24.0f }, { 234.0f, 60.0f,  24.0f },
    };
};

struct LiveCombatView {
    std::unique_ptr<x3::asset::IAssetSource> src;
    std::unique_ptr<x3::asset::IModelLoader> loader;
    x3::asset::Model playerModel, capitalModel;
    std::vector<x3::asset::ModelDrawable> playerDraw, capitalDraw;
    bool playerOk = false, capitalOk = false;
    x3::rhi::MeshHandle boxMesh{}, sphereMesh{};
    x3::rhi::TextureHandle boxTex{};
    std::unique_ptr<x3::game::CombatFx> fx;
    double lastMX = 0.0, lastMY = 0.0;
    bool   mouseBase = false;
    GLFWwindow* win = nullptr;

    bool init(x3::rhi::IRenderDevice& dev, GLFWwindow* window) {
        win = window;
        // Capture the cursor for relative mouse-look (banked flight aim).
        if (win) glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        src.reset(x3::asset::createAssetSource());
        src->mountDir(x3::game::assetRoot(), 0);
        loader.reset(x3::asset::createModelLoader(&dev, src.get()));
        playerModel = loader->load("rigged_glb/JakeFighterShip.glb");
        if (playerModel.ok) { playerDraw = x3::asset::makeDrawables(playerModel); playerOk = true; }
        capitalModel = loader->load("rigged_glb/SpaceShip4.glb");
        if (capitalModel.ok) { capitalDraw = x3::asset::makeDrawables(capitalModel); capitalOk = true; }

        x3::prims::PrimMesh bx = x3::prims::makeBox(1.6f, 0.5f, 1.0f, 0, 0, 0, 0.5f);
        boxMesh = dev.createMesh(bx.verts.data(), (uint32_t)bx.verts.size(),
                                 bx.index.data(), (uint32_t)bx.index.size());
        auto tex = x3::prims::makeCheckerRGBA(64, 8, 150, 160, 185, 50, 58, 74);
        boxTex = dev.createTexture(tex.data(), 64, 64, true);
        x3::prims::PrimMesh sp = x3::prims::makeUVSphere(18, 32);
        sphereMesh = dev.createMesh(sp.verts.data(), (uint32_t)sp.verts.size(),
                                    sp.index.data(), (uint32_t)sp.index.size());

        fx = std::make_unique<x3::game::CombatFx>();
        fx->init(dev);

        // Deep-space look with a RAKING key sun (same menace treatment as the cold-open
        // relight) so the hulls read as dark masses with a bright rim + stars behind.
        x3::rhi::IRenderDevice::SkyParams s{};
        s.enabled = true;
        // Key from the PLAYER's upper side (-X, +Y) so the capital's camera-facing hull
        // (it faces -X, toward the player) is lit + sculpted — a readable menacing mass,
        // not a black silhouette. (The shader sun is full-strength; sunIntensity only
        // affects the sky disc, kept off.)
        s.sunDir[0] = -0.42f; s.sunDir[1] = 0.54f; s.sunDir[2] = 0.48f;
        s.sunColor[0] = 1.0f; s.sunColor[1] = 0.95f; s.sunColor[2] = 0.86f;
        s.sunIntensity = 0.0f;
        s.haze = 0.0f; s.exposure = 1.0f;
        s.zenith[0]  = 0.004f; s.zenith[1]  = 0.004f; s.zenith[2]  = 0.010f;
        s.horizon[0] = 0.008f; s.horizon[1] = 0.010f; s.horizon[2] = 0.020f;
        dev.setSkyParams(s);
        dev.setAmbient(0.10f, 0.11f, 0.15f);
        dev.setBloom(0.34f);
        return true;
    }

    // Mouse-look + WASD/QE input for the pilot (banked flight). Baselines the cursor
    // on the first live frame so no look delta jumps across the hand-off.
    void input(x3::game::PlayerInput& in, float& rollAxis, bool& fire) {
        auto kd = [&](int k){ return glfwGetKey(win, k) == GLFW_PRESS; };
        in.moveFwd    = (kd(GLFW_KEY_W)?1.f:0.f) + (kd(GLFW_KEY_S)?-1.f:0.f);
        in.moveStrafe = (kd(GLFW_KEY_D)?1.f:0.f) + (kd(GLFW_KEY_A)?-1.f:0.f);
        in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
        rollAxis      = (kd(GLFW_KEY_Q)?-1.f:0.f) + (kd(GLFW_KEY_E)?1.f:0.f);
        double mx, my; glfwGetCursorPos(win, &mx, &my);
        if (!mouseBase) { lastMX = mx; lastMY = my; mouseBase = true; }
        in.lookDX = (float)(mx - lastMX);
        in.lookDY = (float)(my - lastMY);
        lastMX = mx; lastMY = my;
        fire = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    }

    // Key/rim/fill point lights around the capital (+ a fill that follows the player),
    // refreshed each frame (setPointLights replaces the whole array).
    void updateLights(x3::rhi::IRenderDevice& dev, const CapitalRig& cap, const x3::phys::Vec3& p) {
        x3::rhi::PointLight L[4]{};
        // Warm key above the capital (bright enough that the dark hull READS as a mass).
        L[0].pos[0]=cap.pos[0]+40; L[0].pos[1]=cap.pos[1]+90; L[0].pos[2]=cap.pos[2]-30;
        L[0].range=1100; L[0].color[0]=55; L[0].color[1]=44; L[0].color[2]=30;
        // Cool rim from behind/left.
        L[1].pos[0]=cap.pos[0]-30; L[1].pos[1]=cap.pos[1]+10; L[1].pos[2]=cap.pos[2]+120;
        L[1].range=1000; L[1].color[0]=14; L[1].color[1]=20; L[1].color[2]=34;
        // Red engine wash near the weak blocks.
        L[2].pos[0]=cap.weak[0][0]; L[2].pos[1]=cap.weak[0][1]-6; L[2].pos[2]=0;
        L[2].range=260; L[2].color[0]=26; L[2].color[1]=2; L[2].color[2]=1;
        // Player-follow fill so Jake's fighter reads.
        L[3].pos[0]=p.x-6; L[3].pos[1]=p.y+8; L[3].pos[2]=p.z;
        L[3].range=120; L[3].color[0]=5; L[3].color[1]=6; L[3].color[2]=9;
        dev.setPointLights(L, 4);
    }

    void drawModel(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fc,
                   const std::vector<x3::asset::ModelDrawable>& draws, bool ok,
                   const float model[16], const float tint[4]) {
        if (ok && !draws.empty()) {
            for (const auto& d : draws) {
                float fin[16]; x3::asset::mulMat4(model, d.nodeTransform, fin);
                const float bc[4] = { d.baseColorFactor[0]*tint[0], d.baseColorFactor[1]*tint[1],
                                      d.baseColorFactor[2]*tint[2], d.baseColorFactor[3] };
                dev.drawMesh(fc, x3::rhi::MeshHandle{ d.meshId },
                             x3::rhi::TextureHandle{ d.baseColorTexId }, bc, fin);
            }
        } else {
            dev.drawMesh(fc, boxMesh, boxTex, tint, model);
        }
    }

    void drawGlow(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fc,
                  const float p[3], float scale, const float color[4], const float emis[4]) {
        const float m[16] = { scale,0,0,0, 0,scale,0,0, 0,0,scale,0, p[0],p[1],p[2],1 };
        dev.drawMeshEmissive(fc, sphereMesh, {}, color, emis, m);
    }

    // ---- Rich weapon-impact FX (Job B): a flash CORE + a radiating SPARK BURST +
    //      a scorch, instead of a flat circle sprite. Reference feel: the arc/spark
    //      streak work on feat/weapons-overhaul (drawMeshEmissive primitives). Hits
    //      are logged with a birth time and aged/drawn each frame. ----
    struct Hit { float p[3]; float seed; float born; bool big; };
    std::vector<Hit> hits;
    float clock = 0.0f;   // view-local FX time (advanced in drawScene by dt)

    void addHit(const x3::phys::Vec3& p, bool big) {
        hits.push_back({ {p.x,p.y,p.z}, (float)(hits.size()%17)*0.61803f, clock, big });
        if (hits.size() > 64) hits.erase(hits.begin());
    }
    void drawHits(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fc) {
        for (const Hit& h : hits) {
            const float life = h.big ? 0.60f : 0.34f;
            const float age = clock - h.born;
            if (age < 0.0f || age > life) continue;
            const float k = age / life;              // 0..1
            const float fade = 1.0f - k;
            // Flash core — a brief white-hot pop that shrinks fast (kept tight so it
            // reads as a hit flash, not a lingering sun).
            {
                const float core = (h.big ? 1.3f : 0.8f) * (0.35f + 0.65f*fade);
                const float c[4] = { 1.0f, 0.9f, 0.62f, 1.0f };
                const float e[4] = { (h.big?10.f:6.5f)*fade, (h.big?6.f:3.5f)*fade,
                                     (h.big?2.0f:1.2f)*fade, (h.big?10.f:6.5f)*fade };
                drawGlow(dev, fc, h.p, core, c, e);
            }
            // Radiating SPARK BURST — emissive specks flung outward over the life (the
            // "streaks", vs a single circle). More + bigger for the big (capital) hits.
            const int n = h.big ? 14 : 8;
            for (int s = 0; s < n; ++s) {
                const float a = (float)s * 2.39996f + h.seed * 6.2831f;
                const float b = (float)s * 1.61f + h.seed * 3.1f;
                const x3::phys::Vec3 d = vnorm({ std::cos(a)*std::cos(b), std::sin(b), std::sin(a)*std::cos(b) });
                const float reach = (h.big ? 26.0f : 13.0f) * k;
                const float sp[3] = { h.p[0]+d.x*reach, h.p[1]+d.y*reach, h.p[2]+d.z*reach };
                const float sc = (h.big ? 1.1f : 0.7f) * fade;
                const float c[4] = { 1.0f, 0.6f, 0.2f, 1.0f };
                const float e[4] = { 9.0f*fade, 3.2f*fade, 0.6f*fade, 9.0f*fade };
                drawGlow(dev, fc, sp, sc, c, e);
            }
        }
    }

    // Draw the full interactive combat frame: menacing capital + running lights +
    // weak-point targets, enemy fighters, Jake's fighter (3P), the capital telegraph,
    // the FX (bolts/impacts), and the HUD. Shared by the live window loop AND the
    // --screenshot-introcombat proof host. `dt` advances the FX only.
    void drawScene(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& frame,
                   const CapitalRig& cap, x3::game::SpacePilotController& pilot,
                   x3::space::EnemyShipManager& enemies, const x3::space::ShipDamageModel& capital,
                   const Beat& beat, int subsDestroyed, float tNow,
                   bool capCharging, float capChargeStart, bool showTakeControl,
                   float dt, float cx, float cy, float cz, float cyaw, float cpit) {
        clock += dt;   // advance view-local FX time (impact bursts, ember trail)
        const x3::phys::Vec3 pp = pilot.pos();
        // Capital hull — dark tint => a menacing DARK MASS (faces -X toward player).
        {
            float m[16];
            shipMatrix({-1,0,0}, {0,1,0}, {0,0,-1},
                       x3::phys::Vec3{cap.pos[0],cap.pos[1],cap.pos[2]}, cap.scale, m);
            const float dark[4] = { 0.52f, 0.55f, 0.64f, 1.0f };
            drawModel(dev, frame, capitalDraw, capitalOk, m, dark);
        }
        // Capital red running lights + warm engine glow (HDR emissive => blooms). Kept
        // SMALL so they read as lights on the hull, not free-floating suns.
        {
            const float redc[4]={1.0f,0.10f,0.08f,1.0f}, rede[4]={3.0f,0.3f,0.2f,3.0f};
            const float b0[3]={cap.pos[0]-64,cap.pos[1]+4,cap.pos[2]-96};
            const float b1[3]={cap.pos[0]-64,cap.pos[1]+4,cap.pos[2]+96};
            drawGlow(dev,frame,b0,1.1f,redc,rede);
            drawGlow(dev,frame,b1,1.1f,redc,rede);
            const float engc[4]={1.0f,0.5f,0.2f,1.0f}, enge[4]={3.0f,1.2f,0.5f,3.2f};
            const float eg[3]={cap.pos[0]+60,cap.pos[1]-2,cap.pos[2]};
            drawGlow(dev,frame,eg,2.4f,engc,enge);
        }
        // Engine-block WEAK POINTS: a small pulsing orange target if alive, dim if down
        // (the HUD bracket does the heavy lifting; this just marks the block on the hull).
        const float pulse = 0.65f + 0.35f * std::sin(tNow * 6.0f);
        for (int i = 0; i < CapitalRig::kWeak; ++i) {
            const bool down = x3::space::ShipDamage::subsystemDown(capital,(x3::space::Subsystem)i);
            float c[4], e[4], sc;
            if (down) { c[0]=0.30f;c[1]=0.12f;c[2]=0.08f;c[3]=1;
                        e[0]=0.25f;e[1]=0.07f;e[2]=0.04f;e[3]=0.5f; sc=1.1f; }
            else      { c[0]=1.0f;c[1]=0.50f;c[2]=0.12f;c[3]=1;
                        e[0]=2.6f*pulse;e[1]=0.9f*pulse;e[2]=0.2f*pulse;e[3]=2.8f*pulse; sc=2.0f; }
            drawGlow(dev,frame,cap.weak[i],sc,c,e);
        }
        // Enemy fighters (red-tinted) + a small red running light.
        for (uint32_t i = 0; i < enemies.count(); ++i) {
            const auto& e = enemies.ship(i);
            const x3::phys::Vec3 ef = vnorm({e.fwd[0],e.fwd[1],e.fwd[2]});
            const x3::phys::Vec3 er = vnorm(vcross(ef, {0,1,0}));
            const x3::phys::Vec3 eu = vcross(er, ef);
            float m[16]; shipMatrix(ef, eu, er, x3::phys::Vec3{e.pos[0],e.pos[1],e.pos[2]}, 1.4f, m);
            const float rt[4] = { 1.2f, 0.5f, 0.45f, 1.0f };
            drawModel(dev, frame, playerDraw, playerOk, m, rt);
            const float ep[3]={e.pos[0],e.pos[1]+1.6f,e.pos[2]};
            const float ec[4]={1,0.15f,0.1f,1}, ee[4]={2.0f,0.25f,0.15f,2.0f};
            drawGlow(dev,frame,ep,0.45f,ec,ee);
        }
        // Player fighter (3P) — keep its character (cyan engines from its own GLB).
        {
            const x3::phys::Vec3 f=pilot.forward(), u=pilot.up(), r=pilot.right();
            float m[16]; shipMatrix(f, u, r, pp, 1.6f, m);
            const float pt[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            drawModel(dev, frame, playerDraw, playerOk, m, pt);
        }
        // ESCALATING DAMAGE STATE: as Jake's hull drops, a growing ember->smoke trail
        // streams off the ship (extends the spiral-down ember trail into a live tell).
        {
            const float hfrac = pilot.maxHull()>0 ? (float)pilot.hull()/(float)pilot.maxHull() : 0.f;
            if (hfrac < 0.72f) {
                const x3::phys::Vec3 bwd = pilot.forward();  // trail streams BEHIND (-forward)
                const int puffs = 2 + (int)((0.72f - hfrac) / 0.72f * 8.0f);
                for (int i = 0; i < puffs; ++i) {
                    const float t = (float)i;
                    const float d = 5.0f + t * 2.6f;   // start well behind the hull (no camera blob)
                    const float sp[3] = { pp.x - bwd.x*d + std::sin(clock*3.0f + t)*0.7f,
                                          pp.y - bwd.y*d + 0.5f,
                                          pp.z - bwd.z*d + std::cos(clock*2.3f + t)*0.7f };
                    const float heat = std::max(0.0f, 1.0f - t * 0.22f);   // hot near hull -> cool smoke
                    const float col[4] = { 0.13f, 0.12f, 0.11f, 1.0f };
                    const float e[4]   = { 1.8f*heat, 0.6f*heat, 0.12f*heat, 1.8f*heat };
                    drawGlow(dev, frame, sp, 0.35f + t*0.13f, col, e);
                }
            }
        }
        // Capital main-gun TELEGRAPH: a growing red charge glow at the turret (the tell).
        if (capCharging) {
            const float k = std::min(1.0f, (tNow - capChargeStart) / 0.7f);
            const float cc[4]={1.0f,0.25f,0.10f,1.0f}, ce[4]={5.0f*k,0.9f*k,0.3f*k,5.0f*k};
            drawGlow(dev, frame, cap.turret, 1.2f + 3.0f * k, cc, ce);
        }
        // Bolts / muzzle flashes (tracers via CombatFx) + the rich impact bursts.
        fx->update(dt);
        fx->draw(dev, frame, cx, cy, cz, cyaw, cpit);
        fx->submit(dev, frame);
        drawHits(dev, frame);

        // ---- HUD overlay ----
        uint32_t W=0,H=0; dev.hudSize(W,H);
        const float fw=(float)W, fh=(float)H;
        using FR = x3::rhi::FontRole;
        auto box = [&](float x,float y,float w,float h,float t,const float col[4]){
            dev.drawHudQuad(frame,x,y,w,t,col); dev.drawHudQuad(frame,x,y+h-t,w,t,col);
            dev.drawHudQuad(frame,x,y,t,h,col); dev.drawHudQuad(frame,x+w-t,y,t,h,col);
        };
        const float rc[4]={0.55f,0.9f,1.0f,0.9f};
        dev.drawHudQuad(frame, fw*0.5f-11, fh*0.5f-1.5f, 22, 3, rc);
        dev.drawHudQuad(frame, fw*0.5f-1.5f, fh*0.5f-11, 3, 22, rc);
        const float hf = pilot.maxHull()>0 ? (float)pilot.hull()/(float)pilot.maxHull() : 0.f;
        const float sf = pilot.maxShield()>0 ? (float)pilot.shield()/(float)pilot.maxShield() : 0.f;
        const float bg[4]={0.08f,0.09f,0.12f,0.72f};
        const float lbl[4]={0.75f,0.82f,0.92f,0.95f};
        dev.drawHudQuad(frame, 40, fh-72, 320, 16, bg);
        const float hullc[4]={ 1.0f-0.7f*hf, 0.25f+0.6f*hf, 0.20f, 0.95f };
        dev.drawHudQuad(frame, 40, fh-72, 320*std::max(0.f,hf), 16, hullc);
        dev.drawHudQuad(frame, 40, fh-50, 320, 9, bg);
        const float shc[4]={0.30f,0.62f,1.0f,0.92f};
        dev.drawHudQuad(frame, 40, fh-50, 320*std::max(0.f,sf), 9, shc);
        dev.drawHudTextF(frame, FR::Console, "HULL", 40, fh-96, 18, lbl);
        char obj[96];
        if (beat.isClimax) std::snprintf(obj,sizeof(obj),"DESTROY THE ENGINE BLOCKS    %d / %d",
                                         subsDestroyed, kMaxSubsystems);
        else               std::snprintf(obj,sizeof(obj),"EVADE THE CAPITAL SALVO");
        const float oc[4]={1.0f,0.84f,0.38f,0.95f};
        const float oadv=dev.textAdvance(FR::Menu,obj,26);
        dev.drawHudTextF(frame, FR::Menu, obj, fw*0.5f-oadv*0.5f, 40, 26, oc);
        if (beat.isClimax) {
            const float chf = x3::space::ShipDamage::hullFrac(capital);
            dev.drawHudQuad(frame, fw*0.5f-200, 80, 400, 8, bg);
            const float capc[4]={0.9f,0.22f,0.2f,0.9f};
            dev.drawHudQuad(frame, fw*0.5f-200, 80, 400*std::max(0.f,chf), 8, capc);
        }
        for (int i = 0; i < CapitalRig::kWeak; ++i) {
            if (x3::space::ShipDamage::subsystemDown(capital,(x3::space::Subsystem)i)) continue;
            float sx=0,sy=0;
            if (dev.worldToScreen(cap.weak[i][0],cap.weak[i][1],cap.weak[i][2],sx,sy)) {
                const float wc[4]={1.0f,0.55f,0.15f,0.9f};
                box(sx-18, sy-18, 36, 36, 3, wc);
            }
        }
        if (showTakeControl && tNow < 2.6f) {
            const float a = tNow < 2.0f ? 1.0f : std::max(0.f,(2.6f-tNow)/0.6f);
            const char* t1 = "TAKE CONTROL";
            const float t1adv = dev.textAdvance(FR::Title, t1, 64);
            const float tc[4]={1.0f,0.9f,0.5f,a};
            dev.drawHudTextF(frame, FR::Title, t1, fw*0.5f-t1adv*0.5f, fh*0.30f, 64, tc);
            const char* t2 = "W/S THROTTLE    MOUSE AIM    A/D STRAFE    Q/E ROLL    LMB FIRE";
            const float t2adv = dev.textAdvance(FR::Menu, t2, 22);
            const float tc2[4]={0.8f,0.9f,1.0f,a};
            dev.drawHudTextF(frame, FR::Menu, t2, fw*0.5f-t2adv*0.5f, fh*0.30f+82, 22, tc2);
        }
    }

    void shutdown(x3::rhi::IRenderDevice& dev) {
        if (win) glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (fx) { fx->shutdown(dev); fx.reset(); }
        if (boxMesh.valid())    dev.destroyMesh(boxMesh);
        if (sphereMesh.valid()) dev.destroyMesh(sphereMesh);
        if (boxTex.valid())     dev.destroyTexture(boxTex);
        if (playerOk)  loader->unload(playerModel);
        if (capitalOk) loader->unload(capitalModel);
        loader.reset(); src.reset();
        // Restore the engine defaults the follow-on scene expects (matches
        // CinematicScene::restoreLook so the hand-off scene is not left in space-look).
        x3::rhi::IRenderDevice::SkyParams off{};   // enabled = false
        dev.setSkyParams(off);
        dev.setAmbient(0.42f, 0.44f, 0.50f);
        dev.setBloom(0.06f);
        dev.setPointLights(nullptr, 0);
    }
};

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

    // ---- LIVE combat scene (Job B). Headless keeps the deterministic sim + a blank
    //      no-op render; live builds the drawable scene + reads real mouse/keys. ----
    CapitalRig cap;
    LiveCombatView view;
    if (live) view.init(*hc.device, hc.window);
    // Capital main-gun (telegraphed) return-fire state (live only).
    float capFireTimer = 0.0f, capChargeStart = 0.0f;
    bool  capCharging = false;
    x3::phys::Vec3 capLock{};
    float shakeT = 0.0f;   // decaying camera-shake on player hits (live feel)
    // "TAKE CONTROL" banner: shown at the top of the FIRST interactive window (dodge).
    const bool showTakeControl = (beat.id == "play.dodge");

    for (int step = 0; step < maxSteps; ++step) {
        const float tNow = (float)step * dt;

        // ---- Input: live reads GLFW; headless uses a deterministic synthetic
        //      "competent pilot" profile (steady forward + aim at the capital). ----
        x3::game::PlayerInput in{};
        float rollAxis = 0.0f;
        bool fire = false;
        if (live) {
            glfwPollEvents();
            if (glfwWindowShouldClose(hc.window)) break;
            if (glfwGetKey(hc.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            view.input(in, rollAxis, fire);   // WASD + Q/E roll + relative mouse-look aim
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
                if (live) { view.addHit({ppos[0]-2,ppos[1]+1,ppos[2]}, /*big*/false); shakeT = std::max(shakeT, 0.20f); }
            } else {
                ++localSalvosDodged;
            }
        }

        // ---- Capital main-gun TELEGRAPHED return fire (LIVE only; headless keeps the
        //      enemy-only salvo model so its metrics stay deterministic). The gun locks
        //      the player's position, CHARGES for 0.7 s (a growing red glow — the tell),
        //      then fires a heavy bolt at the LOCKED point. Move during the charge to
        //      dodge; standing still is a hit. Passive play (no dodge) eats enough bolts
        //      to be shot down, which is the intended default outcome. ----
        if (live) {
            constexpr float kCapInterval = 2.0f, kCapCharge = 0.7f;
            constexpr int   kCapBolt = 155;      // heavy: ~10 clean hits > shield+hull
            capFireTimer += dt;
            if (!capCharging && capFireTimer >= kCapInterval) {
                capCharging = true; capChargeStart = tNow; capLock = pp;
            }
            if (capCharging && (tNow - capChargeStart) >= kCapCharge) {
                x3::phys::Vec3 tp{ cap.turret[0], cap.turret[1], cap.turret[2] };
                view.fx->addTracer(tp, capLock, x3::game::WeaponFxKind::Plasma);
                view.fx->spawnExplosion({ capLock.x, capLock.y, capLock.z }, 6.0f);
                ++localSalvosFaced;
                const float dx = capLock.x - pp.x, dy = capLock.y - pp.y, dz = capLock.z - pp.z;
                if (dx*dx + dy*dy + dz*dz < 24.0f * 24.0f) {
                    pilot.takeDamage(kCapBolt);
                    view.addHit({pp.x-1.5f, pp.y+1.0f, pp.z}, /*big*/true);   // heavy hit: flash + big spark burst
                    shakeT = std::max(shakeT, 0.35f);
                } else ++localSalvosDodged;
                capCharging = false; capFireTimer = 0.0f;
            }
        }

        // ---- Targeting feed: the capital ship is the priority hostile contact. ----
        x3::space::Contact contacts[8]{};
        uint32_t nc = 0;
        contacts[nc++] = { 1000u, { cap.pos[0], cap.pos[1], cap.pos[2] }, { 0,0,0 }, true }; // capital
        for (uint32_t i = 0; i < enemies.count() && nc < 8; ++i) {
            const auto& e = enemies.ship(i);
            contacts[nc++] = { 1u + i, { e.pos[0], e.pos[1], e.pos[2] },
                                       { e.vel[0], e.vel[1], e.vel[2] }, true };
        }
        targeting.setContacts(contacts, nc);

        // ---- Player fire. LIVE: a real forward RAYCAST with generous aim assist
        //      against the lit engine blocks + enemy fighters (Plasma tracer + impact
        //      FX). HEADLESS: the original deterministic "competent pilot" hit routing
        //      (kept byte-for-byte so --test-* metrics are reproducible). ----
        if (fire && pilot.fireLaser(dt)) {
            ++localShotsFired;
            if (live) {
                const x3::phys::Vec3 o = pilot.pos();
                const x3::phys::Vec3 f = pilot.forward();
                // Closest-approach of the aim ray to a target center; only in FRONT (t>0).
                auto rayDist = [&](const float w[3], float& tOut)->float {
                    const float tox=w[0]-o.x, toy=w[1]-o.y, toz=w[2]-o.z;
                    const float t = tox*f.x + toy*f.y + toz*f.z; tOut=t;
                    if (t <= 0.0f) return 1e9f;
                    const float cx=o.x+f.x*t, cy=o.y+f.y*t, cz=o.z+f.z*t;
                    const float dx=w[0]-cx, dy=w[1]-cy, dz=w[2]-cz;
                    return std::sqrt(dx*dx+dy*dy+dz*dz);
                };
                int hitSub = -1, hitEnemy = -1; float bestT = 1e9f;
                for (int i = 0; i < CapitalRig::kWeak; ++i) {
                    if (x3::space::ShipDamage::subsystemDown(capital, (x3::space::Subsystem)i)) continue;
                    float t; if (rayDist(cap.weak[i], t) < 26.0f && t < bestT) { bestT=t; hitSub=i; hitEnemy=-1; }
                }
                for (uint32_t i = 0; i < enemies.count(); ++i) {
                    float t; if (rayDist(enemies.ship(i).pos, t) < 20.0f && t < bestT) { bestT=t; hitEnemy=(int)i; hitSub=-1; }
                }
                const x3::phys::Vec3 mz{ o.x+f.x*3.0f, o.y+f.y*3.0f, o.z+f.z*3.0f };
                if (hitSub >= 0) {
                    const x3::phys::Vec3 hp{ cap.weak[hitSub][0], cap.weak[hitSub][1], cap.weak[hitSub][2] };
                    x3::space::ShipDamage::applyDamage(capital, 75, (x3::space::Subsystem)hitSub);
                    view.fx->addTracer(mz, hp, x3::game::WeaponFxKind::Plasma);
                    const bool killed = x3::space::ShipDamage::subsystemDown(capital, (x3::space::Subsystem)hitSub);
                    view.addHit(hp, /*big*/killed);           // flash + spark burst (not a circle)
                    if (killed) view.fx->spawnExplosion({hp.x,hp.y,hp.z}, 16.0f);  // + debris/smoke
                    ++localShotsHit;
                } else if (hitEnemy >= 0) {
                    const auto& e = enemies.ship((uint32_t)hitEnemy);
                    const x3::phys::Vec3 hp{ e.pos[0], e.pos[1], e.pos[2] };
                    view.fx->addTracer(mz, hp, x3::game::WeaponFxKind::Plasma);
                    view.addHit(hp, /*big*/false);
                    enemies.damageShip((uint32_t)hitEnemy, 35);
                    ++localShotsHit;
                } else {
                    const x3::phys::Vec3 miss{ o.x+f.x*520.0f, o.y+f.y*520.0f, o.z+f.z*520.0f };
                    view.fx->addTracer(mz, miss, x3::game::WeaponFxKind::Plasma);
                }
                view.fx->spawnMuzzleFlash({mz.x,mz.y,mz.z}, {f.x,f.y,f.z});
                // Recount downed subsystems from the live damage model.
                int down = 0;
                for (int i = 0; i < kMaxSubsystems; ++i)
                    if (x3::space::ShipDamage::subsystemDown(capital, (x3::space::Subsystem)i)) ++down;
                localSubsDestroyed = down;
            } else {
                // Deterministic "competent" hit model (headless): route a hit to the
                // next live subsystem (shields down first), counting a hit.
                const int subIdx = std::min(localSubsDestroyed, kMaxSubsystems - 1);
                x3::space::ShipDamage::applyDamage(capital, 60, (x3::space::Subsystem)subIdx);
                ++localShotsHit;
                if (x3::space::ShipDamage::subsystemDown(capital,
                        (x3::space::Subsystem)subIdx) && subIdx == localSubsDestroyed)
                    ++localSubsDestroyed;
            }
        }
        x3::space::ShipDamage::tick(capital, dt);

        // Crippled == all subsystems down (the escape-enabling objective).
        if (!crippled && localSubsDestroyed >= kMaxSubsystems) {
            crippled = true; crippleTime = tNow;
        }

        // ---- Live render: the full interactive combat scene + HUD (3P chase). This is
        //      the fix for "no option to take control" — the window used to draw NOTHING.
        //      Headless skips drawing entirely (deterministic sim only). ----
        if (live) {
            view.updateLights(*hc.device, cap, pp);
            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            // Decaying camera shake on recent player hits (impact feel).
            if (shakeT > 0.0f) {
                const float mag = 0.9f * shakeT;
                const float j = tNow * 97.0f;
                cx += std::sin(j*1.7f) * mag; cy += std::sin(j*2.3f) * mag; cz += std::cos(j*1.9f) * mag;
                shakeT = std::max(0.0f, shakeT - dt);
            }
            hc.device->setCamera(cx, cy, cz, cyaw, cpit, 65.0f);
            auto frame = hc.device->beginFrame();
            if (frame.valid)
                view.drawScene(*hc.device, frame, cap, pilot, enemies, capital, beat,
                               localSubsDestroyed, tNow, capCharging, capChargeStart,
                               showTakeControl, dt, cx, cy, cz, cyaw, cpit);
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

    if (live) view.shutdown(*hc.device);
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

// ===========================================================================
// --test-introcombat self-test (Job B: the interactive combat segment).
// Headless, deterministic, no window/Vulkan. Verifies the combat -> skill ->
// outcome -> branch -> hand-off chain a WIN and a LOSS each drive.
// ===========================================================================
bool runIntroCombatSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    // --- C1: the headless interactive climax window runs bounded + deterministic and
    //         produces valid metrics (the sim the live window renders on top of). ---
    {
        x3::apphost::HostContext hc{};   // headless
        SkillMetrics a{}; a.finalHullFrac = 1.0f;
        Beat dog; dog.kind = BeatKind::InteractiveWindow; dog.id = "play.dogfight";
        dog.enemyCount = 4; dog.timeoutSec = 20.0f; dog.isClimax = true;
        runInteractiveBeat(hc, dog, a);
        SkillMetrics b{}; b.finalHullFrac = 1.0f;
        runInteractiveBeat(hc, dog, b);
        const bool det = a.shotsFired==b.shotsFired && a.subsystemsDestroyed==b.subsystemsDestroyed &&
                         a.salvosFaced==b.salvosFaced && a.salvosDodged==b.salvosDodged;
        check(det, "C1 headless combat window is deterministic (bounded, no leak/carry)");
        check(a.shotsFired > 0 && skillScore(a) >= 0.0f && skillScore(a) <= 1.0f,
              "C1b combat window yields bounded metrics + skill in [0,1]");
    }

    // --- C2: a WIN-quality run (cripple all blocks, full hull, evade + accurate) maps
    //         to the escape CEILING and a reachable Escaped outcome. ---
    {
        SkillMetrics win{};
        win.subsystemsDestroyed = kMaxSubsystems; win.finalHullFrac = 1.0f;
        win.salvosFaced = 8; win.salvosDodged = 8; win.shotsFired = 20; win.shotsHit = 18;
        win.windowDurationSec = 20.0f; win.timeToCrippleSec = 9.0f;
        const float sw = skillScore(win);
        check(sw > 0.85f, "C2 win-quality metrics -> high skill (>0.85)");
        check(outcomeProbability(sw) > 0.34f && outcomeProbability(sw) <= kCeilP,
              "C2b high skill -> escape probability near the ceiling (>0.34, <=0.40)");
        bool escReachable = false;
        for (uint32_t s = 1; s <= 500 && !escReachable; ++s)
            if (rollOutcome(s, sw) == IntroOutcome::Escaped) escReachable = true;
        check(escReachable, "C2c a WIN can ESCAPE (Escaped outcome reachable at win skill)");
    }

    // --- C3: a LOSS-quality run (no blocks down, hull gone) maps to the FLOOR and a
    //         reachable ShotDown outcome. ---
    {
        SkillMetrics lose{}; lose.finalHullFrac = 0.0f;   // took the beating, downed nothing
        const float sl = skillScore(lose);
        check(sl < 1e-4f, "C3 loss-quality metrics -> skill ~ 0");
        check(std::fabs(outcomeProbability(sl) - kFloorP) < 1e-6f,
              "C3b zero skill -> escape probability at the floor (0.07)");
        bool sdReachable = false;
        for (uint32_t s = 1; s <= 50 && !sdReachable; ++s)
            if (rollOutcome(s, sl) == IntroOutcome::ShotDown) sdReachable = true;
        check(sdReachable, "C3c a LOSS gets SHOT DOWN (ShotDown outcome reachable at loss skill)");
    }

    // --- C4: forced end-to-end WIN and LOSS route + hand off correctly, round-tripping
    //         through the persisted StoryFlags exactly as app_run reads them. ---
    {
        const std::string fp = defaultGameStoryFlagsPath();
        {
            x3::apphost::HostContext esc{}; esc.introForce = 1;   // WIN -> escaped
            const IntroOutcome o = runInteractiveIntro(esc);
            x3::game::StoryFlags f; f.loadFile(fp);
            check(o == IntroOutcome::Escaped && readOutcomeFlag(f) == IntroOutcome::Escaped &&
                  f.has(kIntroLandedFlag),
                  "C4 WIN -> intro.outcome=escaped + intro.landed set (surface start hand-off)");
        }
        {
            x3::apphost::HostContext sd{}; sd.introForce = 0;     // LOSS -> shot_down
            const IntroOutcome o = runInteractiveIntro(sd);
            x3::game::StoryFlags f; f.loadFile(fp);
            check(o == IntroOutcome::ShotDown && readOutcomeFlag(f) == IntroOutcome::ShotDown &&
                  !f.has(kIntroLandedFlag),
                  "C4b LOSS -> intro.outcome=shot_down + no intro.landed (canon cell hand-off)");
        }
    }

    x3::logInfo("intro-combat: " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    std::printf("intro-combat: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

// ===========================================================================
// --screenshot-introcombat proof host (Job B visual verification). Builds the live
// combat scene and captures two stills via the shared LiveCombatView::drawScene.
// ===========================================================================
bool runIntroCombatShots(x3::rhi::IRenderDevice& device, GLFWwindow* window,
                         const std::string& basePath) {
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) return false;

    LiveCombatView view; view.init(device, window);
    CapitalRig cap;

    // Pilot posed mid-approach so the capital LOOMS ahead + above and Jake's fighter
    // reads in 3P chase (the coordinator's hand-over composition).
    x3::game::SpacePilotController pilot;
    pilot.spawn(*phys, 150.0f, 10.0f, 26.0f);
    // Fly forward + pitch UP a touch so the 3P camera frames the capital looming above.
    for (int i = 0; i < 40; ++i) {
        x3::game::PlayerInput in{}; in.moveFwd = 1.0f;
        if (i < 16) in.lookDY = -1.4f;   // gentle nose-up toward the capital
        pilot.update(in, 1.0f/60.0f, *phys);
    }

    x3::space::EnemyShipManager enemies; enemies.init(4);
    for (int i = 0; i < 4; ++i) {
        const float ang = (float)i * 1.3f;
        const float p[3] = { 235.0f + 22.0f*(float)i, 12.0f + 14.0f*std::sin(ang), 22.0f*std::cos(ang) };
        enemies.spawn(p);
    }
    { const float pp0[3] = { pilot.pos().x, pilot.pos().y, pilot.pos().z };
      const float pv0[3] = { 0, 0, 0 };
      enemies.update(1.0f/60.0f, pp0, pv0); }

    bool okAll = true;
    auto capture = [&](const std::string& path, const Beat& beat, bool takeControl,
                       int destroy, bool telegraph, bool bolts, int hurt) -> bool {
        auto capital = x3::space::ShipDamage::makeCapital(400, 2000, 120);
        for (int d = 0; d < destroy && d < kMaxSubsystems; ++d)
            for (int g = 0; g < 8 && !x3::space::ShipDamage::subsystemDown(capital,(x3::space::Subsystem)d); ++g)
                x3::space::ShipDamage::applyDamage(capital, 120, (x3::space::Subsystem)d);
        if (hurt > 0) pilot.takeDamage(hurt);   // battered hull => the ember/smoke trail shows

        const float tNow = 1.0f;
        const bool capCharging = telegraph;
        const float chargeStart = telegraph ? (tNow - 0.5f) : 0.0f;
        float cx,cy,cz,cyaw,cpit; pilot.camera(cx,cy,cz,cyaw,cpit);
        device.setCamera(cx,cy,cz,cyaw,cpit,65.0f);

        const int settle = 8;
        for (int i = 0; i < settle; ++i) {
            glfwPollEvents();
            view.updateLights(device, cap, pilot.pos());
            if (bolts) {
                // Fresh bolts + impact bursts each frame so the CAPTURE frame shows them.
                const x3::phys::Vec3 o = pilot.pos(), f = pilot.forward();
                const x3::phys::Vec3 mz{ o.x+f.x*3.f, o.y+f.y*3.f, o.z+f.z*3.f };
                const x3::phys::Vec3 wp{ cap.weak[3][0], cap.weak[3][1], cap.weak[3][2] };
                view.fx->addTracer(mz, wp, x3::game::WeaponFxKind::Plasma);
                const x3::phys::Vec3 tp{ cap.turret[0], cap.turret[1], cap.turret[2] };
                view.fx->addTracer(tp, { o.x-14, o.y+8, o.z-10 }, x3::game::WeaponFxKind::Plasma);
                if (i == settle-3) { view.addHit(wp, /*big*/true);                        // player hit bursting on the weak block
                                     view.addHit({o.x+f.x*9, o.y+f.y*9+3, o.z+f.z*9}, /*big*/false); }  // capital bolt sparking off Jake's bow
            }
            if (i == settle-1) device.armCapture(path.c_str());
            auto fr = device.beginFrame();
            if (fr.valid)
                view.drawScene(device, fr, cap, pilot, enemies, capital, beat,
                               destroy, tNow, capCharging, chargeStart, takeControl,
                               1.0f/60.0f, cx,cy,cz,cyaw,cpit);
            device.endFrame(fr);
        }
        const bool wrote = device.captureFrame(path.c_str());
        if (wrote) x3::logInfo("--screenshot-introcombat: wrote " + path);
        else       x3::logError("--screenshot-introcombat: capture FAILED " + path);
        return wrote;
    };

    Beat dodge; dodge.kind = BeatKind::InteractiveWindow; dodge.id = "play.dodge"; dodge.isClimax = false;
    Beat dog;   dog.kind   = BeatKind::InteractiveWindow; dog.id   = "play.dogfight"; dog.isClimax = true;
    okAll &= capture(basePath + "_takecontrol.png", dodge, /*takeControl*/true,  /*destroy*/0, /*telegraph*/true,  /*bolts*/false, /*hurt*/0);
    okAll &= capture(basePath + "_fight.png",       dog,   /*takeControl*/false, /*destroy*/2, /*telegraph*/true,  /*bolts*/true,  /*hurt*/1050);

    view.shutdown(device);
    phys->shutdown();
    return okAll;
}

} // namespace x3::intro
