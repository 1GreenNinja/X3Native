#pragma once
// F2 RESCUE SYSTEM (EFLZ Spire spec §5). Game/slice code only — engine/ stays pure.
//
// Floor 2 is the rescue hub: three captive victims are held in the medical wards,
// each on a 5-minute countdown. The player rescues a victim by walking up to it
// and pressing E while it is still alive (RescueSystem::tryRescue) — the victim
// becomes a friendly COMPANION that follows the player (light follow AI). If a
// victim's timer expires before it is reached, the victim TRANSFORMS into a boss
// (Aria -> The Siren, Keisha -> Breeder Queen, Emily -> Oracle), spawned via the
// MonsterManager (the existing enemy/boss framework), and the captive vanishes.
//
// This mirrors monster.* deliberately:
//   * Each victim owns a loaded GLB (kept alive for the app's lifetime) drawn over
//     a Tag::Prop entity, exactly like MonsterSystem draws its multi-primitive
//     model at the entity transform (entity render mesh left invalid).
//   * Movement reuses the monster chase pattern (move the body with
//     setBodyPosition + bake a yaw-toward-target into the render 3x3), so a
//     companion follows the player just like a monster chases it.
//   * Transform-on-expire reuses MonsterManager::spawn with a Boss-type Tuning, so
//     the bosses run the SAME phase machine / combat as Martinez.
//
// Coords per docs/CONVENTIONS.md: +X right, +Y up, -Z forward; face a target with
// the verified headingToFace law (yaw = atan2(-dirX,-dirZ)) shared with monster.*.
//
// MP-friendly style: this system OWNS the victim state (alive/companion/expired +
// timers) and is driven purely by data fed in from the host each frame (dt, the
// player position). The host reads it for the HUD and pokes it via tryRescue() on
// an E-interact edge — gameplay reads input, owns state.

#include "scene.h"
#include "monster.h"   // MonsterManager + Tuning (the boss the victim becomes)
#include "anim.h"      // J1/T1: skeletal-animation Skinner (so girls breathe/idle)
#include "ragdoll.h"   // RagdollSkin (drive the skin from physics parts) — mirrors monster.*

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Ragdoll.h"   // phys::IRagdoll + makeHumanoidRagdollBones (skinned death/collapse ragdoll)
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Per-victim lifecycle state.
//   * Captive  — alive in the ward, timer running; can be rescued (E in range).
//   * Companion— rescued; follows the player (friendly), timer stopped/cleared.
//   * Expired  — timer ran out; the captive is gone, its boss has been spawned.
enum class VictimState : uint32_t { Captive = 0, Companion = 1, Expired = 2 };

// The three F2 victims (spec §5). Index doubles as the HUD timer slot order.
enum class VictimId : uint32_t { Aria = 0, Keisha = 1, Emily = 2 };

// Default rescue countdown: 5 minutes (spec §5). Tunable per-victim via build().
constexpr float kRescueTimer = 300.0f;

// Interact reach (meters): the player must be within this of a captive to rescue.
constexpr float kRescueReach = 3.0f;

// Companion follow tuning (mirrors the monster chase constants).
constexpr float kCompanionSpeed   = 4.0f;   // m/s toward the player (a touch faster than walk-pace enemies)
constexpr float kCompanionStop    = 2.2f;   // hold this far behind the player (don't crowd)
constexpr float kCompanionTeleport= 25.0f;  // if left this far behind (e.g. an elevator), snap to the player

// One rescue victim: a loaded character GLB + a Tag::Prop entity + a collision
// box, plus the lifecycle state + countdown. Self-contained like MonsterSystem.
class RescueVictim {
public:
    // Build the victim: load `liveModel` from `modelDir`, upload its drawables,
    // add a static-by-mass collision box, and register a Tag::Prop entity at `pos`.
    // `bossTuning` is applied when (and only if) the timer expires and the victim
    // transforms (it carries the BOSS model + stats). On model load failure a
    // procedural box stands in (the level never breaks).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               std::string_view modelDir, const x3::phys::Vec3& pos,
               VictimId id, std::string_view name, std::string_view liveModel,
               float timer, const MonsterSystem::Tuning& bossTuning);

    // Advance one frame. While Captive: count the timer down (only once `hubReached`
    // — the timers run from when the player reaches the F2 rescue hub). While
    // Companion: follow `playerPos` (light follow AI). Returns true the FRAME the
    // timer crosses zero (the host then spawns the boss via the boss-tuning getter).
    // No-op once Expired.
    bool tick(float dt, bool hubReached, Scene& scene,
              x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& playerPos);

    // Try to rescue: if Captive, alive, and the player is within `reach`, flip to
    // Companion (stop the timer, retag friendly) and return true. Else false.
    bool tryRescue(const x3::phys::Vec3& playerPos, float reach = kRescueReach);

    // ---- PHYSICS RAGDOLL (collapse) — mirrors MonsterSystem's death ragdoll -----
    // Collapse Aria into a physics ragdoll: snapshot the Skinner's CURRENT animated
    // bone globals, build a Jolt humanoid ragdoll (engine makeHumanoidRagdollBones +
    // createRagdoll) placed/yawed/scaled to match her, seed it from those globals (so
    // the flop starts seamlessly from where the animation left off), and latch
    // m_ragdolled. From then on tick() reads the ragdoll bone transforms back out of
    // the SHARED physics world (which the host steps each frame) and drives the skin
    // via Skinner::applyExternalGlobals — the MESH follows the bodies. Idempotent: a
    // second call (or a non-skinnable model) is a no-op. `physics` MUST be the same
    // world the host steps each frame and must outlive the victim. `scene` is needed to
    // FREEZE her entity (drop its body link) so Scene::update() can't move the frozen
    // collapse transform once the standing collision body is removed.
    void ragdoll(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // True once ragdoll() built a live physics ragdoll driving the skin.
    bool ragdolled() const { return m_ragdolled; }

    // Override the draw tint (debug / headless proof framing only — e.g. make the
    // collapsed ragdoll read against a dark floor). Multiplies the per-drawable base
    // color exactly like the default friendly tint.
    void setTint(float r, float g, float b, float a) { m_tint[0]=r; m_tint[1]=g; m_tint[2]=b; m_tint[3]=a; }

    // Set the STATIC facing yaw (radians, headingToFace convention: yaw =
    // atan2(-dirX,-dirZ) to point the model's local -Z along (dirX,dirZ)). Used by
    // non-gameplay placements (e.g. the showroom ANALYST GALLERY) to face a Captive
    // figure toward a fixed target (its terminal). A Captive's tick() re-bakes this
    // yaw each frame (it never self-rotates while Captive), so the figure holds the
    // facing. No effect on the rescue/companion follow logic. The 180deg VISUAL flip
    // for the +Z-authored rigged GLBs is applied in bakeTransform(), so pass the
    // logical heading (toward the target).
    void setFacing(float yaw) { m_yaw = yaw; }

    // Draw the victim's model at its transform (the entity render mesh is invalid;
    // this is the single source of truth for the multi-primitive draw — like
    // MonsterSystem::drawMonster). No-op once Expired (the model is gone).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // ---- Queries (HUD + host wiring) --------------------------------------
    VictimId    id() const { return m_id; }
    const std::string& name() const { return m_name; }
    VictimState state() const { return m_state; }
    bool  captive()   const { return m_state == VictimState::Captive; }
    bool  companion() const { return m_state == VictimState::Companion; }
    bool  expired()   const { return m_state == VictimState::Expired; }
    float timeLeft()  const { return m_timeLeft; }   // seconds remaining (Captive)
    float timerMax()  const { return m_timerMax; }
    x3::phys::Vec3 pos() const { return m_pos; }

    // ---- Animation queries (BUG #48 + --test-rescue) ----------------------
    // True once bind() found a skinnable model + a usable clip, so tick() drives
    // the Skinner each frame (the girl breathes/idles instead of freezing).
    bool animActive() const { return m_animActive; }
    // Read-only access to the Skinner so the self-test can assert the joint palette
    // actually changed between two times (i.e. she's animating, not frozen).
    const x3::anim::Skinner& skinner() const { return m_skinner; }
    const x3::asset::Model&  model()   const { return m_model; }

    // The boss tuning this victim transforms into (read by the host on expiry so it
    // can MonsterManager::spawn the boss with the right model/stats).
    const MonsterSystem::Tuning& bossTuning() const { return m_bossTuning; }

    // ---- Serialize / deserialize (plain methods; no save system present yet) ---
    // The host's save layer can call these to persist/restore the per-victim
    // lifecycle + timer. Packed POD so a future G.serializeRescueState hook can
    // blit it. (See RescueSystem::serialize for the per-system aggregate.)
    struct SaveState { uint32_t state; float timeLeft; };
    SaveState save() const { return { (uint32_t)m_state, m_timeLeft }; }
    void load(const SaveState& s) { m_state = (VictimState)s.state; m_timeLeft = s.timeLeft; }

private:
    void drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                const float model[16]) const;
    void bakeTransform(Scene& scene);   // write yaw + scale + pos into the entity
    // BUG #48: drive the Skinner one frame from a planar speed (m/s). Locomotion
    // blend when a walk/run set exists; else the single Idle clip (pumped faster
    // while moving). No-op if the model isn't skinnable. Re-uploads via m_device.
    void driveAnim(float dt, float planarSpeed);

    // RAGDOLL: read the live ragdoll bone WORLD transforms, map them into skin space,
    // run the rigid bone->skin attach (RagdollSkin), and feed the result to the
    // Skinner's external-pose path so the GPU-skinned MESH flops with the bodies.
    // Mirrors MonsterSystem::driveSkinFromRagdoll. No-op unless ragdolled + device.
    void driveSkinFromRagdoll();

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    x3::asset::Model                         m_model;
    std::vector<x3::asset::ModelDrawable>    m_drawables;
    bool                                     m_usingReal = false;

    // ---- Skeletal animation (BUG #48): each victim drives her OWN Skinner every
    // frame so she breathes/idles (and a companion walks) instead of freezing at
    // bind/T-pose. Mirrors MonsterSystem exactly: bind() the loaded model, resolve
    // idle/walk/run clips by fuzzy name, setLocomotionClips(0.2,2.0), then call
    // applyLocomotion() (locomotion blend) or apply() (idle-only) in tick(). Each
    // girl owns her own animTime + phase offset so they don't animate in lockstep.
    // The device is captured at build() so tick() (no device param) can re-upload
    // the CPU-skinned verts. Unskinnable models leave m_skinner invalid -> static. -
    x3::anim::Skinner        m_skinner;
    x3::rhi::IRenderDevice*  m_device     = nullptr;
    int   m_idleClip   = -1;
    int   m_walkClip   = -1;
    int   m_runClip    = -1;
    bool  m_useLocoBlend = false;   // a real idle(+walk/+run) set drives the blend
    bool  m_animActive   = false;   // a usable clip was found (skinner valid)
    float m_animTime     = 0.0f;    // single-clip playback time (looped in the runtime)
    float m_phaseOffset  = 0.0f;    // per-girl start phase so they aren't identical

    // ---- PHYSICS RAGDOLL (collapse) — SAME machinery as MonsterSystem's death
    // ragdoll. On ragdoll() we build a Jolt humanoid ragdoll from the canonical rig,
    // placed/yawed/scaled to match Aria + seeded from her CURRENT animated bone
    // globals, and add it to the shared world. Each frame while m_ragdolled we read the
    // bone WORLD transforms out, map them onto the skin nodes (RagdollSkin, rigid
    // attach), and feed them to applyExternalGlobals so the MESH flops physically. If
    // the model has no usable skeleton, m_deathRagdoll stays null (ragdoll() no-ops). --
    std::unique_ptr<x3::phys::IRagdoll>      m_deathRagdoll;   // null => no skinned ragdoll
    RagdollSkin                              m_ragdollSkin;    // rigid bone->skin driver
    std::vector<x3::phys::RagdollBoneDesc>   m_ragdollBones;   // the rig the ragdoll was built from
    std::vector<float>                       m_ragPartInit;    // per-bone INIT skin-local 4x4 (boneCount*16)
    std::vector<float>                       m_ragWorldScratch;// per-bone CURRENT world 4x4 scratch
    std::vector<float>                       m_ragPartCur;     // per-bone CURRENT skin-local 4x4 scratch
    std::vector<float>                       m_ragNodeGlobals; // RagdollSkin output (nodeCount*16)
    float                                    m_deathModelInv[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // inverse of the frozen draw matrix at collapse
    bool                                     m_ragdolled = false; // a physics ragdoll is driving the skin (collapse latched)

    VictimId        m_id        = VictimId::Aria;
    std::string     m_name;
    VictimState     m_state     = VictimState::Captive;
    float           m_timeLeft  = kRescueTimer;
    float           m_timerMax  = kRescueTimer;

    x3::phys::Vec3   m_pos{};
    uint32_t         m_entity = kNoLink;
    x3::phys::BodyId m_body;
    float            m_modelScale = 1.0f;
    float            m_yaw = 0.0f;
    float            m_modelFixup[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float            m_tint[4] = { 1, 1, 1, 1 };

    MonsterSystem::Tuning m_bossTuning;   // applied on transform-on-expire
};

// HUD timer row (one per still-active captive). The host renders these stacked.
struct RescueTimerHud {
    std::string name;     // "ARIA"
    float       seconds;  // remaining time
    bool        urgent;   // < 60 s -> draw red/flashing
};

// The F2 rescue system: owns the three victims + the boss MonsterManager the
// transformed victims spawn into. Mirrors Level1Game's other sub-systems: build()
// once, tick() each frame, an interact hook, HUD accessors, serialize/deserialize.
class RescueSystem {
public:
    // Build all three victims (Aria/Keisha/Emily) in the F2 wards. `wardA/B/C` are
    // the ward floor positions; pass distinct spots (the host derives them from the
    // level layout). `modelDir` is the rigged-GLB dir. `timer` defaults to 5 min.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               const x3::phys::Vec3& wardA, const x3::phys::Vec3& wardB,
               const x3::phys::Vec3& wardC, float timer = kRescueTimer);

    // ---- Timer gating (playtest-fix) --------------------------------------
    // The rescue countdowns are GATED on this flag, which defaults to FALSE: until
    // the player reaches the F2 ward hub the victims stay captive with NO countdown
    // (so the 5-min timers can't expire at load and spawn all three bosses on the
    // first frame — the playtest bug). The host wires the F2-hub trigger volume to
    // activate() (see level1_game.cpp). After activate(), the timers run + expire ->
    // boss exactly as before.
    //
    // activate() is the canonical "the hub was reached, start the clocks" call;
    // setHubReached(bool) is the explicit setter kept for the existing API + the
    // save/restore path (and tests that drive the flag directly). Both are idempotent.
    void activate() { m_hubReached = true; }
    void setHubReached(bool reached) { m_hubReached = reached; }
    bool hubReached() const { return m_hubReached; }
    // True iff the rescue clocks are running (the hub was reached / activate()d).
    bool active() const { return m_hubReached; }

    // Advance one frame: tick every victim (timers / companion follow) and, for any
    // whose timer expired THIS frame, spawn its boss via the MonsterManager. The
    // spawned bosses are also moved/animated here (their own update). `device` is
    // needed to spawn the boss mesh on expiry (cached from build() too).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& playerPos);

    // Interact hook (E in range). Rescue the nearest captive within `reach` of the
    // player. Returns true iff a victim was rescued this call (the host logs / SFX).
    bool tryRescue(const x3::phys::Vec3& playerPos, float reach = kRescueReach);

    // Draw all victims (live captives + companions). Bosses are drawn by the boss
    // MonsterManager (call drawBosses too / it is folded into draw()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // ---- HUD timer data (host renders the rows) ---------------------------
    uint32_t victimCount() const { return (uint32_t)m_victims.size(); }
    const RescueVictim& victim(uint32_t i) const { return *m_victims[i]; }
    // The active timer rows (one per Captive whose timer is running). At most 3.
    std::vector<RescueTimerHud> hudTimers() const;
    uint32_t rescuedCount() const;   // companions secured
    uint32_t expiredCount() const;   // victims lost (now bosses)

    // The bosses spawned from expired victims (read for combat/objective gating).
    MonsterManager&       bosses()       { return m_bosses; }
    const MonsterManager& bosses() const { return m_bosses; }

    bool built() const { return m_built; }

    // ---- Serialize / deserialize ------------------------------------------
    // No save system is wired in the app yet (no G.serializeRescueState hook
    // exists), so these are plain methods the future save layer can call. They
    // persist each victim's lifecycle + remaining time + the hub flag. NOTE: a
    // restored Expired victim does NOT re-spawn its boss (the boss is enemy state,
    // owned by the MonsterManager / save layer separately) — load only restores the
    // captive/companion/timer bookkeeping.
    struct SaveState {
        bool                          hubReached = false;
        std::vector<RescueVictim::SaveState> victims;
    };
    SaveState serialize() const;
    void deserialize(const SaveState& s);

private:
    std::vector<std::unique_ptr<RescueVictim>> m_victims;
    MonsterManager m_bosses;        // transformed-victim bosses (Siren/Queen/Oracle)
    std::string    m_modelDir;
    x3::rhi::IRenderDevice* m_device = nullptr;
    bool m_hubReached = false;
    bool m_built = false;
};

// Headless self-test (--test-rescue). Builds three victims on a HeadlessDevice +
// Jolt world and asserts: (R1) rescuing a captive in range yields a companion;
// (R2) a companion follows the player; (R3) a timer that expires transforms the
// victim into a boss (a new MonsterManager entry appears, Boss type); (R4) you
// cannot rescue an out-of-range / already-expired victim; (R5) serialize/
// deserialize round-trips the lifecycle + timers. Logs PASS/FAIL R#, returns true
// iff all pass. No window/Vulkan. Mirrors runCombatSelfTest / runLevel1SelfTest.
bool runRescueSelfTest();

} // namespace x3::game
