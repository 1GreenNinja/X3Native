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
#include "engine/audio/IAudioSystem.h"   // hybrid-escalation heartbeat (Tim's ruling)
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
//   * Extracted— (W4-1) a Companion escorted to the extraction point (the F2
//                elevator lobby): goodbye bark, leaves the level. Freed AND safe —
//                distinct from Expired so story flags can tell the outcomes apart.
enum class VictimState : uint32_t { Captive = 0, Companion = 1, Expired = 2, Extracted = 3 };

// W5-2: how the interrupt-rescue resolved for a victim (RESCUE_SETPIECE_DESIGN.md §2).
//   None    — no assault window has resolved (pre-arm, or window still running).
//   Clean   — attackers killed inside the clean threshold: she is shaken but unhurt.
//   Wounded — killed late: `<girl>.interrupted` dialog tier; blood tint + no running.
//   Lost    — the hard cutoff passed with attackers alive: the existing expiry path
//             fires immediately (boss transform) — same flow as the 5-min timer.
enum class RescueTier : uint32_t { None = 0, Clean = 1, Wounded = 2, Lost = 3 };

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
constexpr float kCompanionRadius  = 0.40f;  // planar body radius for the anti-overlap pass (matches enemy hitbox)

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
    // — the timers run from when the player reaches the F2 rescue hub), and run the
    // W5-2 INTERRUPT WINDOW (see configureAssault) — `aliveAttackers` is the count of
    // living attacker enemies near this victim, computed by RescueSystem from the
    // scene (dead monsters drop their entity body link, so liveness is scene truth).
    // While Companion: follow `playerPos` (light follow AI). Returns true the FRAME
    // the victim expires (5-min timer OR the interrupt hard cutoff) — the host then
    // spawns the boss via the boss-tuning getter. No-op once Expired.
    bool tick(float dt, bool hubReached, Scene& scene,
              x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& playerPos,
              uint32_t aliveAttackers = 0);   // defaulted: non-assault hosts (showroom
                                              // gallery) tick 5-arg exactly as before

    // ---- W5-2: the interrupt-rescue window (RESCUE_SETPIECE_DESIGN.md §1) ------
    // The assault ARMS when the player first comes within `armRadius` (the door-tell
    // beat: muffled audio + threshold light — the host reads tellActive() for the
    // audio cue; the visual strip is room dressing). It goes ACTIVE (window clock
    // running) when the player closes within `activeRadius` (the burst-in). The
    // window resolves the first frame `aliveAttackers` hits zero: elapsed <= cleanS
    // -> Clean; <= woundedS -> Wounded (blood tint + run suppressed + the host sets
    // `<girl>.interrupted`); past woundedS with attackers alive -> Lost = immediate
    // expiry (boss transform, same code path as the 5-min timer).
    void configureAssault(float cleanS, float woundedS,
                          float armRadius = 12.0f, float activeRadius = 6.5f) {
        m_cleanS = cleanS; m_woundedS = woundedS;
        m_armR2 = armRadius * armRadius; m_activeR2 = activeRadius * activeRadius;
        m_assaultConfigured = true;
    }
    bool assaultArmed()  const { return m_assaultArmed; }
    bool assaultActive() const { return m_assaultActive; }
    // True while the door-tell should play (armed, window not yet running).
    bool tellActive()    const { return m_assaultArmed && !m_assaultActive && captive(); }
    RescueTier tier()    const { return m_tier; }
    float windowElapsed() const { return m_windowT; }
    // 0..1 fraction of the window REMAINING (1 = just opened, 0 = hard cutoff).
    float windowFrac() const {
        if (!m_assaultActive || m_woundedS <= 0.0f) return 0.0f;
        const float f = 1.0f - m_windowT / m_woundedS;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

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
    bool  extracted() const { return m_state == VictimState::Extracted; }

    // W4-1: a Companion reaching the extraction point leaves the level — hide the
    // entity + drop the body (the Expired vanish pattern) but keep the model owned
    // (app-lifetime, like everything else). No-op unless Companion. Idempotent.
    void extract(Scene& scene, x3::phys::IPhysicsWorld& physics);
    float timeLeft()  const { return m_timeLeft; }   // seconds remaining (Captive)
    float timerMax()  const { return m_timerMax; }
    x3::phys::Vec3 pos() const { return m_pos; }

    // ---- MUTUAL EXCLUSION + aim-ray pass-through (2026-07-26) --------------
    // The captive's Enemy-layer collision body (invalid once she ragdolls/expires/
    // extracts). The host's character-separation pass treats a captive as an
    // ANCHOR (monsters get pushed off her), and the weapon aim-ray SKIPS this body
    // so a captive can never eat a shot meant for an enemy behind her.
    x3::phys::BodyId body() const { return m_body; }
    // Planar collision radius (matches the standing box half-width kVictimHalf.x).
    float collisionRadius() const { return 0.4f; }
    // ---- Anti-overlap (companion never shares a cell) ---------------------
    // Planar collision radius, matched to the enemy body half-width (~0.4m) so a
    // companion and an enemy are held the same 0.8m apart as two enemies are.
    float bodyRadiusXZ() const { return kCompanionRadius; }
    // Apply a planar positional correction + sync the body (like the monster
    // de-overlap). Only a live COMPANION is pushed — a captive stays put in her
    // cell. Y untouched. No-op on a bodyless / non-companion victim.
    void nudgePlanar(float dx, float dz, x3::phys::IPhysicsWorld& physics) {
        if (m_state != VictimState::Companion || !m_body.valid()) return;
        m_pos.x += dx; m_pos.z += dz;
        physics.setBodyPosition(m_body, m_pos);
    }
    // If this is a live companion, push it out of every immovable point it overlaps
    // (each `pts[k]` has planar radius `ptR[k]`). The companion takes the full
    // correction, clamped per-frame. No-op unless a live companion.
    void deOverlapFromPoints(const x3::phys::Vec3* pts, const float* ptR, uint32_t n,
                             x3::phys::IPhysicsWorld& physics);

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
    // W5-2: shared expiry (5-min timer AND the interrupt hard cutoff): hide the
    // entity, drop the body, flip to Expired. Caller returns true to the host so it
    // spawns the boss this frame.
    void expire(Scene& scene, x3::phys::IPhysicsWorld& physics);
    // W5-2: resolve the window at tier (Clean/Wounded): stop the clock; Wounded gets
    // the blood tint + run suppression the design's aftermath table specifies.
    void resolveTier(RescueTier t);

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

    // ---- W5-2 interrupt-rescue window (see configureAssault) -------------------
    bool  m_assaultConfigured = false;   // no config -> legacy behavior (tests R0-R9)
    bool  m_assaultArmed      = false;   // player crossed armRadius: the tell plays
    bool  m_assaultActive     = false;   // player crossed activeRadius: clock running
    float m_windowT   = 0.0f;            // seconds since the window opened
    float m_cleanS    = 18.0f;           // clean-save threshold
    float m_woundedS  = 35.0f;           // hard cutoff (past this + attackers alive = Lost)
    float m_armR2     = 144.0f;          // armRadius^2
    float m_activeR2  = 42.25f;          // activeRadius^2
    RescueTier m_tier = RescueTier::None;
    bool  m_runCap    = false;           // Wounded: suppress the run clip + slow follow

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

    // ---- W4-1: extraction point (the safe hand-off) ------------------------
    // When set, any Companion whose XZ distance to `pos` drops under `radius`
    // EXTRACTS during tick(): she leaves the level (host reads extractedThisFrame
    // right after tick() for the goodbye bark + story flag). Unset (default) =
    // companions follow forever (legacy behavior, --test-rescue unchanged).
    void setExtractionPoint(const x3::phys::Vec3& pos, float radius) {
        m_extractPos = pos; m_extractR2 = radius * radius; m_extractSet = true;
    }
    // Victim index extracted during the LAST tick (girls arrive one at a time), or
    // UINT32_MAX when none.
    uint32_t extractedThisFrame() const { return m_extractedThisFrame; }

    // ---- W5-2: interrupt-tier resolution (mirrors extractedThisFrame) ----------
    // The victim index whose window RESOLVED during the last tick (Clean or Wounded;
    // Lost reports via the boss spawn as before), or UINT32_MAX. The host reads this
    // to set the `<girl>.interrupted` story flag on Wounded and to bark/SFX.
    uint32_t tierResolvedThisFrame() const { return m_tierResolvedThisFrame; }
    // Per-victim assault window config (call after build; victim order A/K/E = 0/1/2).
    void configureAssault(uint32_t victimIdx, float cleanS, float woundedS,
                          float armR = 12.0f, float activeR = 6.5f) {
        if (victimIdx < m_victims.size())
            m_victims[victimIdx]->configureAssault(cleanS, woundedS, armR, activeR);
    }
    // HYBRID ESCALATION (Tim's ruling 2026-07-08): the countdown stays hidden +
    // diegetic normally; when an active window drops under a third remaining the
    // pip ring PULSES blood-red and a low HEARTBEAT loop fades in, pitch rising
    // as the window closes. Wire once after boot audio exists; graceful when
    // unset (pure-visual pulse still runs off the internal clock).
    void setEscalationAudio(x3::audio::IAudioSystem* audio, x3::audio::SoundHandle heartbeat) {
        m_escAudio = audio; m_heartbeat = heartbeat;
    }
    // Advance the pulse clock + heartbeat loop. The host calls this every frame
    // (alongside tick(); separate so headless geometry tests skip audio churn).
    void escalationTick(float dt);

    // The diegetic countdown ring drawn around an active tableau (default ON; the
    // host may gate it behind a `rescue_ring` cvar — see the W5-2 paste-block).
    void setRingEnabled(bool on) { m_ringEnabled = on; }
    bool ringEnabled() const { return m_ringEnabled; }
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

    // ---- Anti-overlap (companion never shares a cell) ---------------------
    // Push every live COMPANION out of any overlap with (a) the given hostile body-
    // centers and (b) the other companions. The hostiles/peers are treated as
    // immovable (they de-overlap within their own manager), so a companion takes the
    // full correction — clamped per-frame. Call AFTER tick() + the enemy managers'
    // update() so it corrects this frame's final positions. No-op with no companions.
    void deOverlapCompanions(const x3::phys::Vec3* hostiles, const float* hostileR,
                             uint32_t nHostiles, x3::phys::IPhysicsWorld& physics);

    // Draw all victims (live captives + companions). Bosses are drawn by the boss
    // MonsterManager (call drawBosses too / it is folded into draw()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // ---- HUD timer data (host renders the rows) ---------------------------
    uint32_t victimCount() const { return (uint32_t)m_victims.size(); }
    const RescueVictim& victim(uint32_t i) const { return *m_victims[i]; }
    // Mutable overload: the CONTENT module that builds the scene needs to POSE a
    // victim after build (RescueVictim::setFacing — aim her at the attacker who
    // is scripted to be assaulting her). Read-only callers keep the const one.
    RescueVictim& victim(uint32_t i) { return *m_victims[i]; }
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
    // ---- W4-1 extraction point (see setExtractionPoint) --------------------
    x3::phys::Vec3 m_extractPos{};
    float          m_extractR2 = 0.0f;
    bool           m_extractSet = false;
    uint32_t       m_extractedThisFrame = UINT32_MAX;

    // ---- W5-2 -----------------------------------------------------------------
    uint32_t m_tierResolvedThisFrame = UINT32_MAX;
    bool     m_ringEnabled = true;
    // Hybrid-escalation state: the pulse clock + the live heartbeat voice.
    float                    m_ringClock = 0.0f;
    x3::audio::IAudioSystem* m_escAudio  = nullptr;
    x3::audio::SoundHandle   m_heartbeat{};
    x3::audio::LoopHandle    m_heartLoop{};

    // Pip quad for the diegetic countdown ring (built lazily on first active window).
    x3::rhi::MeshHandle m_ringPip{};
    // Count living attackers near `pos`: Tag::Enemy entities whose body link is
    // still valid (monster death drops the body — scene truth, no host wiring).
    static uint32_t aliveAttackersNear(const Scene& scene, const x3::phys::Vec3& pos,
                                       float radius);
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
