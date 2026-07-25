#pragma once
// ============================================================================
// THE CLONE — Act-1 finale boss (F7 Executive Laboratory). A 3-phase, HP-gated
// boss fight built ON TOP of the EXISTING MonsterSystem boss phase machine
// (monster.h: BossPhase + phase2Frac/phase3Frac + phase{2,3}{Speed,Damage,Scale}Mul
// + phase3SummonCount), the SAME machine Chief Martinez / the Saurian Warlord run.
// This module adds the FIGHT choreography around that machine + the neural-collar
// minigame, and exposes the two integration events the wider F7 finale needs:
//   * "Sarah freed"  — fired when the player destroys Sarah's neural collar (P2).
//   * "Clone dead"    — fired when the Clone falls (the descent-gate input).
//
// CLEAN-ROOM, original work. Composes ONLY X3Native's own systems (monster.* +
// engine interfaces). No id Tech / RBDOOM / Doom / Quake source consulted.
// CONTENT/SLICE code only — no renderer or core-engine changes.
//
// The three phases (the canon beat, MASTER_GAME_PLAN L7):
//   P1 SEPARATION   — the Clone (Jake's tinted duplicate) fights with its escort
//                     (host-owned adds); Sarah is present but RESTRAINED/collared
//                     (a non-combat placeholder — the companion-combat lane wakes
//                     her on the "Sarah freed" event). Runs until the Clone's HP
//                     falls to kSeparationEndFrac.
//   P2 NEURAL COLLAR— the Clone STAGGERS (stunned) and is SHIELDED (it cannot be
//                     killed by fire here — the collar is the gate). Sarah's neural
//                     collar becomes an ACTIVE interactable; destroying it (a short
//                     3-strike / hold-E sequence) fires "Sarah freed" and mutates
//                     the Clone into its final form.
//   P3 MUTATED HYBRID—the Clone mutates (the internal phase machine's Phase3 scale/
//                     tint push + emissive), buffed, and makes its last stand. On
//                     death it fires "Clone dead" (the F7 desk / descent gate reads
//                     this, wired by the integrator).
//
// Headless-testable: --test-clone (runCloneBossSelfTest) drives the whole machine
// on a HeadlessDevice + Jolt world with NO window/Vulkan.
// ============================================================================

#include "scene.h"
#include "monster.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace x3::game {

// The Clone fight's own 3-phase state (distinct from the MonsterSystem-internal
// BossPhase, which drives the visual enrage/mutation scale+tint underneath).
enum class ClonePhase : uint32_t {
    Separation    = 0,  // P1: fight the Clone (+ host escort); Sarah restrained
    NeuralCollar  = 1,  // P2: Clone staggers/shielded; destroy Sarah's collar
    MutatedHybrid = 2,  // P3: mutated final stand
    Dead          = 3,  // the Clone has fallen
};
const char* clonePhaseName(ClonePhase p);

// Sarah's NEURAL COLLAR — the P2 destroy-sequence interactable. Active ONLY during
// the NeuralCollar phase. Destroyed by kStrikes interact-strikes (E) OR a hold-E
// fill (kHoldSeconds); either completion frees Sarah + mutates the Clone.
struct NeuralCollar {
    static constexpr int   kStrikes     = 3;      // 3-hit destroy (E chain)
    static constexpr float kHoldSeconds = 2.5f;   // alt hold-E fill
    static constexpr float kReach       = 3.5f;   // interact proximity (m)

    x3::phys::Vec3 pos{};
    bool  active       = false;                   // gated to Phase 2
    bool  destroyed    = false;
    int   strikesLeft  = kStrikes;
    float holdProgress = 0.0f;                    // 0..1 (hold-E path)

    // 0..1 remaining integrity (for a HUD ring); 1 = pristine, 0 = broken.
    float integrityFrac() const {
        const float byStrike = (float)strikesLeft / (float)kStrikes;
        const float byHold   = 1.0f - holdProgress;
        return byStrike < byHold ? byStrike : byHold;
    }
};

// The F7 Clone boss fight. Owns the Clone MonsterManager (one Boss) + the collar +
// the 3-phase machine + the two integration events. Build once, tick each frame.
class CloneBossFight {
public:
    using EventFn = std::function<void()>;

    // ---- Tunables (thresholds are FRACTIONS of the Clone's maxHp) -------------
    // Separation -> NeuralCollar when the Clone's HP falls to/below this.
    static constexpr float kSeparationEndFrac = 0.66f;
    // While in NeuralCollar the Clone is SHIELDED: its HP is clamped at/above this
    // floor and incoming damage is scaled ~to nothing, so it cannot die before the
    // collar is destroyed (the collar is the only way forward).
    static constexpr float kCollarShieldFrac  = 0.45f;
    static constexpr float kCollarShieldMul   = 0.02f;   // incoming-damage scale in P2
    // On collar-destroy the Clone is dropped to this HP fraction so the internal
    // boss machine crosses its phase3Frac (0.33) and MUTATES (Phase3 scale/tint).
    static constexpr float kMutateDropFrac    = 0.30f;
    static constexpr float kMutatedDamageTakenMul = 1.20f; // exposed after mutation

    // Build the Clone at `pos` (Jake's rig, sickly clone tint, phase machine armed)
    // and place the neural collar at `collarPos` (on the restrained Sarah). `modelDir`
    // is the rigged-GLB dir (same one SpireTopFloors::build receives). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               const x3::phys::Vec3& pos, const x3::phys::Vec3& collarPos);
    bool built() const { return m_built; }

    // Advance one frame: the Clone's AI + internal phase machine, THIS fight's
    // phase machine (Separation->Collar->Mutated->Dead), and the collar shield.
    // `player` may be null (headless geometry/logic tick). Fires the "Clone dead"
    // event the frame the Clone falls.
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
              IDamageSink* player, const AttackFxFn& attackFx);

    // Fire one shot at the Clone (host folds this into its onFire path). In the
    // NeuralCollar phase the shield absorbs it (the shot still registers a hit for
    // FX, but the Clone will not drop below the shield floor).
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot,
                      x3::DamageType type = x3::DamageType::Kinetic);

    // Draw the Clone (host calls in its draw block).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const { m_boss.drawAll(device, frame, scene); }

    // Release in-flight death ragdolls before physics shutdown (idempotent).
    void shutdown() { m_boss.shutdown(); }

    // ---- Neural-collar interaction (host E-chain) ----------------------------
    bool collarActive() const { return m_collar.active && !m_collar.destroyed; }
    const NeuralCollar& collar() const { return m_collar; }
    // True iff `playerPos` is within reach of the ACTIVE collar.
    bool nearCollar(const x3::phys::Vec3& playerPos,
                    float range = NeuralCollar::kReach) const;
    // Land ONE destroy strike (E press). Returns true iff consumed (collar active +
    // in range). Breaking the collar fires "Sarah freed" + mutates the Clone.
    bool strikeCollar(const x3::phys::Vec3& playerPos);
    // Alt hold-E path: accrue dt of hold; completing it has the same effect as the
    // last strike. Returns true iff progress was made this frame.
    bool holdCollar(float dt, const x3::phys::Vec3& playerPos);

    // "[E] ..." collar prompt for the HUD ("" when not near/active).
    std::string collarPrompt(const x3::phys::Vec3& playerPos) const;

    // ---- Integration events (the two seams the finale wires) -----------------
    // SARAH FREED — latched true the frame the collar is destroyed (P2 success).
    bool sarahFreed() const { return m_sarahFreed; }
    void setOnSarahFreed(EventFn fn) { m_onSarahFreed = std::move(fn); }
    // CLONE DEAD — latched true the frame the Clone falls (the descent-gate input).
    bool cloneDead() const { return m_cloneDead; }
    void setOnCloneDead(EventFn fn) { m_onCloneDead = std::move(fn); }

    // ---- Queries (HUD + self-test) -------------------------------------------
    ClonePhase phase() const { return m_phase; }
    MonsterManager&       boss()       { return m_boss; }
    const MonsterManager& boss() const { return m_boss; }
    bool  bossAlive() const { return m_boss.aliveCount() > 0; }
    int   bossHp() const    { return m_boss.count() ? m_boss.at(0).hp() : 0; }
    int   bossMaxHp() const { return m_boss.count() ? m_boss.at(0).maxHp() : 0; }
    float bossHpFrac() const;
    // The internal boss visual phase (Phase3 == mutated look on screen).
    BossPhase bossVisualPhase() const {
        return m_boss.count() ? m_boss.at(0).phase() : BossPhase::Phase1;
    }
    // One-line HUD label, e.g. "THE CLONE  [SEPARATION]  HP 72%".
    std::string hudLabel() const;

private:
    void enterCollarPhase();     // Separation -> NeuralCollar (stagger + shield)
    void breakCollar();          // collar destroyed: fire Sarah-freed + mutate
    void enterMutatedPhase();    // NeuralCollar -> MutatedHybrid (unshield + Phase3)

    MonsterManager m_boss;               // the Clone (one Boss-type MonsterSystem)
    NeuralCollar   m_collar;
    ClonePhase     m_phase   = ClonePhase::Separation;
    bool  m_built            = false;
    bool  m_sarahFreed       = false;
    bool  m_cloneDead        = false;
    EventFn m_onSarahFreed;
    EventFn m_onCloneDead;
    std::string m_modelDir;
};

// The Clone's spawn Tuning: Jake's own rig, TINTED sickly/pale with an emissive
// "veined" push (it IS Jake's clone), a Boss running the HP-gated phase machine
// (Phase2 @ 66%, Phase3 @ 33% = the mutation), and a desperate P3 summon. Shared
// with SpireTopFloors so the F7 boss + the self-test agree on the Clone identity.
MonsterSystem::Tuning cloneBossTuning(std::string_view modelDir);

// Headless self-test (--test-clone). Builds the Clone on a HeadlessDevice + Jolt
// world and asserts the full machine:
//   (C0) spawns a live Boss in SEPARATION; collar inert; no events fired.
//   (C1) dropping HP to <= kSeparationEndFrac transitions to NEURAL COLLAR and the
//        collar goes ACTIVE (P1 -> P2, HP threshold).
//   (C2) the Clone is SHIELDED in P2: heavy damage cannot kill it or advance the
//        phase; it stays a captive gate until the collar is destroyed.
//   (C3) destroying the collar (3 strikes) fires "Sarah freed" exactly once and
//        advances P2 -> P3 (MUTATED HYBRID) — the internal boss phase reaches
//        Phase3 (the mutation) and the Clone is exposed again.
//   (C4) killing the mutated Clone fires "Clone dead" exactly once, sets the
//        descent flag, and lands the fight in DEAD.
//   (C5) events are idempotent (extra ticks/strikes never re-fire).
//   (C6) the hold-E collar path reaches the same completion.
// Prints "clone: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runCloneBossSelfTest();

} // namespace x3::game
