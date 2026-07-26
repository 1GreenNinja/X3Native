// THE CLONE — Act-1 finale boss + the neural-collar minigame. See clone_boss.h.
//
// Clean-room: composes ONLY X3Native's own systems (monster.* phase machine,
// Scene/prims, the engine RHI + physics interfaces). No id Tech / RBDOOM / Doom /
// Quake — or any other game-engine — source consulted. CONTENT/SLICE code only.
#include "clone_boss.h"
#include "asset_root.h"
#include "headless_device.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

namespace x3::game {

// ===========================================================================
// Identity / tuning
// ===========================================================================

const char* clonePhaseName(ClonePhase p) {
    switch (p) {
        case ClonePhase::Separation:    return "SEPARATION";
        case ClonePhase::NeuralCollar:  return "NEURAL COLLAR";
        case ClonePhase::MutatedHybrid: return "MUTATED HYBRID";
        case ClonePhase::Dead:          return "DEAD";
    }
    return "?";
}

namespace {

// The Clone's rig: Jake's own 24-joint clone humanoid (standard bone names —
// Hips / LeftUpLeg / RightHand / Head, NOT the mixamorig* prefix). Preferred; if
// the LFS payload is absent we fall back to the older Jake rig, and past that to
// the MonsterSystem's tinted procedural box (the level never breaks).
const char* pickCloneModel(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path(dir) / "JakeClone_player.glb", ec)) return "JakeClone_player.glb";
    if (fs::exists(fs::path(dir) / "Jake_22_actions.glb", ec))  return "Jake_22_actions.glb";
    return "JakeClone_player.glb";   // named anyway; the loader falls back to a box
}

const char* pickSarahModel(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path(dir) / "Sarah.glb", ec))       return "Sarah.glb";
    if (fs::exists(fs::path(dir) / "AnnaTactical.glb", ec)) return "AnnaTactical.glb";
    if (fs::exists(fs::path(dir) / "AnnaCasual.glb", ec))   return "AnnaCasual.glb";
    return "Sarah.glb";
}

// Collar prop half-extents (m) — a small band at neck height.
constexpr float kCollarHX = 0.16f, kCollarHY = 0.07f, kCollarHZ = 0.16f;

} // namespace

MonsterSystem::Tuning cloneBossTuning(std::string_view modelDir) {
    MonsterSystem::Tuning bt;
    bt.type           = MonsterType::Boss;
    // Act-1 FINALE tier: above the F5 mid-boss (460) and Chief Martinez (340). The
    // 3-phase machine's thresholds are fractions of this, so the fight's pacing is
    // set by kSeparationEndFrac / phase3Frac, not by the raw number.
    bt.hp             = 620;
    bt.chaseSpeed     = 3.4f;
    bt.damage         = 14;           // inside the melee band; phase muls ramp it
    bt.attackRange    = 2.4f;
    bt.attackCooldown = 1.05f;
    bt.attackWindup   = 0.30f;
    bt.ranged         = false;
    // CLONE LOOK: sickly pale-cyan wash over Jake's own albedo (it reads as HIM,
    // drained) + an emissive push so the "veined" material glow carries. OWNER
    // EYEBALL ITEM — this is the dial for "how wrong does the clone look".
    bt.tint[0] = 0.62f; bt.tint[1] = 0.88f; bt.tint[2] = 0.95f; bt.tint[3] = 1.0f;
    bt.emissiveScale    = 1.6f;
    bt.modelDirOverride = std::string(modelDir);
    bt.modelFile        = pickCloneModel(bt.modelDirOverride);
    bt.standUpZtoY      = false;      // the clone rig is authored Y-up, feet at y=0
    bt.modelScale       = 1.10f;      // Jake-sized (it IS Jake) — not a giant
    // The HP-gated phase machine underneath: Phase2 at 66% (the separation beat's
    // end) and Phase3 at 33% (the MUTATION). kMutateDropFrac (0.30) is set BELOW
    // phase3Frac on purpose so breaking the collar guarantees the mutation fires.
    bt.phase2Frac        = 0.66f;
    bt.phase3Frac        = 0.33f;
    bt.phase2SpeedMul    = 1.30f;
    bt.phase2DamageMul   = 1.35f;
    bt.phase3SpeedMul    = 1.75f;     // mutated hybrid: fast + heavy
    bt.phase3DamageMul   = 1.85f;
    bt.phase2ScaleMul    = 1.10f;
    bt.phase3ScaleMul    = 1.40f;     // the mutation reads as a SIZE change
    bt.phase3SummonCount = 2;         // desperate: the bible's "summons" beat
    return bt;
}

MonsterSystem::Tuning restrainedSarahTuning(std::string_view modelDir) {
    MonsterSystem::Tuning t;
    t.type           = MonsterType::Guard;
    t.hp             = 200;
    t.chaseSpeed     = 0.0f;          // restrained: she does not move
    t.damage         = 0;             // non-combat placeholder
    t.attackRange    = 0.0f;
    t.ranged         = false;
    t.startAllied    = true;          // never harms / is never a hostile target
    t.tint[0] = 1.0f; t.tint[1] = 0.96f; t.tint[2] = 0.94f; t.tint[3] = 1.0f;
    t.modelDirOverride = std::string(modelDir);
    t.modelFile        = pickSarahModel(t.modelDirOverride);
    t.standUpZtoY      = false;
    t.modelScale       = 1.0f;
    return t;
}

void applyCloneClipOverrides(MonsterSystem& m) {
    using CS = MonsterSystem::ClipSlot;
    // The clone rig's clip vocabulary sits outside the generic fuzzy keys, so name
    // its combat clips explicitly. Each call is a no-op when the clip is absent
    // (older rig / procedural-box fallback), so this is safe on any model.
    m.overrideClip(CS::Idle,     "Idle_11");
    m.overrideClip(CS::Walk,     "Walking");
    m.overrideClip(CS::Run,      "Running");
    m.overrideClip(CS::Attack,   "Backflip_Sweep_Kick");   // spinning sweep kick
    m.overrideClip(CS::Attack2,  "Backflip_and_Hooks");    // hook combo
    m.overrideClip(CS::HitReact, "Gunshot_Reaction");
    m.overrideClip(CS::Death,    "Dead");
}

// ===========================================================================
// Build
// ===========================================================================

void CloneBossFight::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                           const x3::phys::Vec3& pos, const x3::phys::Vec3& collarPos) {
    if (m_built) return;
    m_modelDir = std::string(modelDir);

    // ---- The Clone ---------------------------------------------------------
    const uint32_t idx = m_boss.spawn(scene, device, physics, m_modelDir, pos,
                                      cloneBossTuning(m_modelDir));
    applyCloneClipOverrides(m_boss.at(idx));

    // ---- Sarah's NEURAL COLLAR (the P2 gate). A small emissive band prop at the
    // collar position; INERT (dark, no interaction) until Phase 2 arms it. Built as
    // a purely visual Scene entity — no physics body, so it never eats an Enemy-mask
    // shot ray and never blocks the player. The destroy sequence is state on this
    // object (strikes / hold), driven from the host's E-chain. ----
    m_collar = NeuralCollar{};
    m_collar.pos = collarPos;
    {
        x3::prims::PrimMesh geo = x3::prims::makeBox(kCollarHX, kCollarHY, kCollarHZ,
                                                     collarPos.x, collarPos.y, collarPos.z,
                                                     1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = 0.16f; e.baseColor[1] = 0.17f; e.baseColor[2] = 0.20f;
        e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Prop;
        m_collarEntity = scene.add(e);
    }
    refreshCollarLook(scene);

    m_phase = ClonePhase::Separation;
    m_built = true;
    x3::logInfo("[clone] THE CLONE built — Act-1 finale boss (" +
                cloneBossTuning(m_modelDir).modelFile + ", HP " +
                std::to_string(bossMaxHp()) + "), phase SEPARATION; neural collar staged");
}

void CloneBossFight::buildSarahPlaceholder(Scene& scene, x3::rhi::IRenderDevice& device,
                                           x3::phys::IPhysicsWorld& physics,
                                           std::string_view modelDir,
                                           const x3::phys::Vec3& pos) {
    if (m_sarah.count() > 0) return;    // idempotent
    const std::string dir = modelDir.empty() ? m_modelDir : std::string(modelDir);
    const uint32_t i = m_sarah.spawn(scene, device, physics, dir, pos,
                                     restrainedSarahTuning(dir));
    // RESTRAINED: docile => she never perceives, targets, moves or attacks. Combined
    // with startAllied (0 damage) she is a pure tableau body in Phase 1. The
    // companion-combat lane takes her over on the "Sarah freed" event.
    m_sarah.at(i).setDocile(true);
    m_sarah.at(i).setCalmLoop("struggle");   // no-op unless the rig has the clip
    m_sarahRestrained = true;
    x3::logInfo("[clone] Sarah placeholder spawned — RESTRAINED / collared, non-combat");
}

// ===========================================================================
// Phase machine
// ===========================================================================

float CloneBossFight::bossHpFrac() const {
    const int mx = bossMaxHp();
    if (mx <= 0) return 0.0f;
    const float f = (float)bossHp() / (float)mx;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

void CloneBossFight::raiseBanner(std::string b) {
    m_banner = std::move(b);
    m_bannerTimer = kBannerSecs;
}

void CloneBossFight::enterCollarPhase() {
    if (m_phase != ClonePhase::Separation) return;
    m_phase = ClonePhase::NeuralCollar;
    m_collar.active = !m_collar.destroyed;
    if (m_boss.count()) {
        MonsterSystem& c = m_boss.at(0);
        // STAGGER: the Clone reels and breaks off (the "separation" ends here).
        c.stun(kCollarStaggerSecs);
        // SHIELD: incoming damage is scaled to ~nothing AND the HP floor is clamped
        // in tick()/onFire(), so the Clone cannot be killed before the collar breaks.
        c.setDamageTakenMul(kCollarShieldMul);
    }
    raiseBanner("PHASE 2!  NEURAL COLLAR");
    x3::logInfo("[clone] PHASE 2 — NEURAL COLLAR: the Clone staggers + SHIELDS; "
                "destroy Sarah's collar to advance");
}

void CloneBossFight::breakCollar() {
    if (m_collar.destroyed) return;
    m_collar.destroyed   = true;
    m_collar.active      = false;
    m_collar.strikesLeft = 0;
    m_collar.holdProgress = 1.0f;

    // ---- "SARAH FREED" — the integration event the companion lane hooks. ----
    if (!m_sarahFreed) {
        m_sarahFreed      = true;
        m_sarahRestrained = false;
        if (m_sarah.count()) m_sarah.at(0).setDocile(false);   // she wakes
        x3::logInfo("[clone] EVENT: SARAH FREED — neural collar destroyed");
        if (m_onSarahFreed) m_onSarahFreed();
    }
    enterMutatedPhase();
}

void CloneBossFight::enterMutatedPhase() {
    if (m_phase == ClonePhase::MutatedHybrid || m_phase == ClonePhase::Dead) return;
    m_phase = ClonePhase::MutatedHybrid;
    if (m_boss.count()) {
        MonsterSystem& c = m_boss.at(0);
        // Un-shield and EXPOSE: the mutated form takes MORE damage than baseline.
        c.setDamageTakenMul(kMutatedDamageTakenMul);
        // Drop HP below the internal machine's phase3Frac so the next update()
        // crosses it and the boss MUTATES on screen (Phase3 up-scale + tint push).
        const int mx = c.maxHp();
        const int want = (int)((float)mx * kMutateDropFrac);
        if (c.hp() > want) c.setHp(want < 1 ? 1 : want);
    }
    raiseBanner("PHASE 3!  MUTATED HYBRID");
    x3::logInfo("[clone] PHASE 3 — MUTATED HYBRID: the Clone mutates + makes its last stand");
}

void CloneBossFight::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                          IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;
    if (m_bannerTimer > 0.0f) m_bannerTimer -= dt;

    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;

    // The Clone: AI + the INTERNAL boss phase machine (enrage/mutate visuals).
    BossPhaseFn onInner = [](BossPhase p) {
        x3::logInfo(std::string("[clone] internal boss phase -> ") +
                    (p == BossPhase::Phase3 ? "Phase3 (MUTATED look)"
                                            : (p == BossPhase::Phase2 ? "Phase2" : "Phase1")));
    };
    m_boss.update(dt, scene, physics, playerPos, atkTarget, attackFx, onInner);

    // The restrained Sarah placeholder: movement-only tick so she stays synced +
    // animating. She is docile while restrained, so this never produces behaviour.
    if (m_sarah.count()) m_sarah.update(dt, scene, physics, playerPos);

    // ---- THIS fight's 3-phase machine -------------------------------------
    switch (m_phase) {
        case ClonePhase::Separation:
            if (bossAlive() && bossHpFrac() <= kSeparationEndFrac) enterCollarPhase();
            break;
        case ClonePhase::NeuralCollar: {
            // SHIELD FLOOR: whatever leaked through the damage multiplier, hold the
            // Clone at/above the floor. The collar is the ONLY way out of Phase 2.
            if (m_boss.count() && bossAlive()) {
                MonsterSystem& c = m_boss.at(0);
                const int floorHp = (int)((float)c.maxHp() * kCollarShieldFrac);
                if (c.hp() < floorHp) c.setHp(floorHp);
            }
            break;
        }
        case ClonePhase::MutatedHybrid:
        case ClonePhase::Dead:
            break;
    }

    // ---- "CLONE DEAD" — the descent-gate input. Fires exactly once. --------
    if (!m_cloneDead && m_built && m_boss.count() && !bossAlive()) {
        m_cloneDead = true;
        m_phase     = ClonePhase::Dead;
        m_collar.active = false;
        raiseBanner("THE CLONE HAS FALLEN");
        x3::logInfo("[clone] EVENT: CLONE DEAD — the Act-1 finale boss has fallen "
                    "(descent gate input set)");
        if (m_onCloneDead) m_onCloneDead();
    }

    refreshCollarLook(scene);
}

FireResult CloneBossFight::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                  Scene& scene, x3::phys::IPhysicsWorld& physics,
                                  int damage, x3::DamageType type) {
    FireResult r = m_boss.fire(eye, dir, scene, physics, damage, type);
    // Re-assert the shield floor immediately (so a burst inside one frame cannot
    // punch the Clone through the Phase-2 gate before tick() runs).
    if (m_phase == ClonePhase::NeuralCollar && m_boss.count() && m_boss.at(0).alive()) {
        MonsterSystem& c = m_boss.at(0);
        const int floorHp = (int)((float)c.maxHp() * kCollarShieldFrac);
        if (c.hp() < floorHp) { c.setHp(floorHp); r.hpAfter = c.hp(); r.killed = false; }
    }
    return r;
}

// ===========================================================================
// Neural collar — the P2 minigame
// ===========================================================================

bool CloneBossFight::nearCollar(const x3::phys::Vec3& playerPos, float range) const {
    if (!collarActive()) return false;
    const float dx = playerPos.x - m_collar.pos.x;
    const float dy = playerPos.y - m_collar.pos.y;
    const float dz = playerPos.z - m_collar.pos.z;
    return (dx * dx + dy * dy + dz * dz) <= range * range;
}

bool CloneBossFight::strikeCollar(const x3::phys::Vec3& playerPos) {
    if (!collarActive() || !nearCollar(playerPos)) return false;
    if (m_collar.strikesLeft > 0) --m_collar.strikesLeft;
    x3::logInfo("[clone] collar STRIKE — " + std::to_string(m_collar.strikesLeft) +
                " left");
    if (m_collar.strikesLeft <= 0) breakCollar();
    return true;
}

bool CloneBossFight::holdCollar(float dt, const x3::phys::Vec3& playerPos) {
    if (!collarActive() || !nearCollar(playerPos) || dt <= 0.0f) return false;
    m_collar.holdProgress += dt / NeuralCollar::kHoldSeconds;
    if (m_collar.holdProgress >= 1.0f) {
        m_collar.holdProgress = 1.0f;
        breakCollar();
    }
    return true;
}

std::string CloneBossFight::collarPrompt(const x3::phys::Vec3& playerPos) const {
    if (!nearCollar(playerPos)) return std::string();
    return "[E] BREAK SARAH'S NEURAL COLLAR  (" +
           std::to_string(m_collar.strikesLeft) + ")";
}

void CloneBossFight::refreshCollarLook(Scene& scene) {
    if (m_collarEntity == kNoLink || m_collarEntity >= scene.size()) return;
    Entity& e = scene.get(m_collarEntity);
    if (m_collar.destroyed) { e.visible = false; return; }
    e.visible = true;
    if (m_collar.active) {
        // ARMED: hot red band, brighter as its integrity drops (the destroy tell).
        const float f = m_collar.integrityFrac();
        const float glow = 2.5f + (1.0f - f) * 5.5f;
        e.baseColor[0] = 0.85f; e.baseColor[1] = 0.16f; e.baseColor[2] = 0.16f;
        e.emissive[0] = 1.0f; e.emissive[1] = 0.16f; e.emissive[2] = 0.12f;
        e.emissive[3] = glow;
    } else {
        // INERT (Phase 1): a dull powered-down band.
        e.baseColor[0] = 0.18f; e.baseColor[1] = 0.19f; e.baseColor[2] = 0.22f;
        e.emissive[0] = 0.20f; e.emissive[1] = 0.05f; e.emissive[2] = 0.05f;
        e.emissive[3] = 0.35f;
    }
}

std::string CloneBossFight::hudLabel() const {
    const int pct = (int)(bossHpFrac() * 100.0f + 0.5f);
    std::string s = "THE CLONE  [";
    s += clonePhaseName(m_phase);
    s += "]  HP ";
    s += std::to_string(pct);
    s += "%";
    if (collarActive()) s += "   COLLAR " + std::to_string(m_collar.strikesLeft);
    return s;
}

// ===========================================================================
// Headless self-test (--test-clone)
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[clone-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[clone-test] FAIL ") + name); }
}

// Drive the fight forward `n` frames with no player (pure logic/geometry tick).
void step(CloneBossFight& f, Scene& scene, x3::phys::IPhysicsWorld& w, int n,
          float dt = 1.0f / 60.0f) {
    // NOTE: not named `far` — that is a legacy MSVC keyword macro.
    const x3::phys::Vec3 away{ 0.0f, 0.0f, 500.0f };   // out of aggro/attack range
    for (int i = 0; i < n; ++i) f.tick(dt, scene, w, away, away, nullptr, AttackFxFn{});
}

// Drop the Clone's HP directly (setHp does NOT run the death path) so the test can
// drive the HP-gated transitions deterministically without ray geometry.
void setBossFrac(CloneBossFight& f, float frac) {
    if (!f.boss().count()) return;
    MonsterSystem& c = f.boss().at(0);
    int hp = (int)((float)c.maxHp() * frac);
    if (hp < 1) hp = 1;
    c.setHp(hp);
}

} // namespace

bool runCloneBossSelfTest() {
    g_pass = g_fail = 0;
    x3::game::HeadlessRenderDevice device;
    const std::string modelDir = riggedGlbRoot();

    // ---- C7: identity — the Clone IS Jake's clone rig, Boss-typed, phases armed. --
    {
        namespace fs = std::filesystem;
        const MonsterSystem::Tuning t = cloneBossTuning(modelDir);
        std::error_code ec;
        const bool onDisk = fs::exists(fs::path(t.modelDirOverride) / t.modelFile, ec);
        const bool isJakeClone = t.modelFile == "JakeClone_player.glb";
        const bool armed = t.type == MonsterType::Boss && t.hp >= 400 &&
                           t.phase2Frac > t.phase3Frac && t.phase3Frac > 0.0f &&
                           t.phase3ScaleMul > t.phase2ScaleMul &&
                           t.phase3DamageMul > t.phase2DamageMul;
        // The mutate drop MUST land under phase3Frac or breaking the collar would
        // not guarantee the on-screen mutation.
        const bool mutateGuaranteed = CloneBossFight::kMutateDropFrac < t.phase3Frac;
        if (!onDisk) x3::logError("[clone-test] clone rig missing: " + t.modelFile);
        check(isJakeClone && onDisk && armed && mutateGuaranteed,
              "C7 the Clone = JakeClone_player.glb (on disk), Boss + escalating phase "
              "machine, mutate-drop below phase3Frac");
    }

    std::unique_ptr<x3::phys::IPhysicsWorld> world(x3::phys::createPhysicsWorld());
    world->init();
    Scene scene;
    CloneBossFight fight;
    const x3::phys::Vec3 clonePos { 0.0f, 0.0f, 0.0f };
    const x3::phys::Vec3 sarahPos { 4.0f, 0.0f, 0.0f };
    const x3::phys::Vec3 collarPos{ 4.0f, 1.45f, 0.0f };   // neck height on Sarah
    fight.build(scene, device, *world, modelDir, clonePos, collarPos);
    fight.buildSarahPlaceholder(scene, device, *world, modelDir, sarahPos);

    int sarahFreedFires = 0, cloneDeadFires = 0;
    fight.setOnSarahFreed([&] { ++sarahFreedFires; });
    fight.setOnCloneDead ([&] { ++cloneDeadFires; });

    // ---- C0: spawns live in SEPARATION; collar inert; no events; Sarah restrained. --
    {
        step(fight, scene, *world, 5);
        const bool boss = fight.built() && fight.bossAlive() &&
                          fight.boss().count() == 1 &&
                          fight.boss().at(0).type() == MonsterType::Boss &&
                          fight.phase() == ClonePhase::Separation;
        const bool collarInert = !fight.collarActive() && !fight.collar().destroyed &&
                                 fight.collar().strikesLeft == NeuralCollar::kStrikes;
        const bool quiet = !fight.sarahFreed() && !fight.cloneDead() &&
                           sarahFreedFires == 0 && cloneDeadFires == 0;
        const bool sarah = fight.sarahPresent() && fight.sarahRestrained() &&
                           fight.sarah().at(0).isAllied() &&
                           fight.sarah().at(0).docile() &&
                           fight.sarah().at(0).attackDamage() == 0;
        check(boss && collarInert && quiet && sarah,
              "C0 spawns a live Boss in SEPARATION; collar inert; no events; Sarah "
              "present + RESTRAINED (allied, docile, 0 damage)");
    }

    // ---- C8: the CLIP OVERRIDES landed. The generic fuzzy resolve mis-picks this
    // rig (walk -> "Crouch_Walk_Left_with_Gun_inplace", run -> "BackRight_Run", and
    // attack/attack2/death unresolved at -1), so the fight names them explicitly.
    // Skipped (auto-pass) only if the rig failed to load as a skinned model at all. --
    {
        using CS = MonsterSystem::ClipSlot;
        const MonsterSystem& c = fight.boss().at(0);
        const bool skinned = c.clipIndex(CS::Idle) >= 0;
        const int atk = c.clipIndex(CS::Attack), atk2 = c.clipIndex(CS::Attack2);
        const int die = c.clipIndex(CS::Death),  hit  = c.clipIndex(CS::HitReact);
        const int wlk = c.clipIndex(CS::Walk),   run  = c.clipIndex(CS::Run);
        const bool combatWired = atk >= 0 && atk2 >= 0 && die >= 0 && hit >= 0 &&
                                 atk != atk2;
        const bool locoFixed   = wlk >= 0 && run >= 0 && wlk != run &&
                                 wlk != c.clipIndex(CS::Idle);
        if (!skinned) x3::logError("[clone-test] clone rig did not load skinned — C8 vacuous");
        check(!skinned || (combatWired && locoFixed),
              "C8 the Clone's clip overrides resolved — attack/attack2/hit-react/death "
              "wired to its combat clips + walk/run corrected off the fuzzy mis-picks");
    }

    // ---- C1: HP <= kSeparationEndFrac -> NEURAL COLLAR + the collar goes ACTIVE. --
    {
        setBossFrac(fight, CloneBossFight::kSeparationEndFrac - 0.02f);
        step(fight, scene, *world, 3);
        check(fight.phase() == ClonePhase::NeuralCollar && fight.collarActive() &&
              fight.bossAlive() && !fight.sarahFreed(),
              "C1 HP threshold advances P1 -> P2 (NEURAL COLLAR) and ARMS the collar");
    }

    // ---- C2: SHIELDED in P2 — heavy damage cannot kill it or advance the phase. --
    {
        // Hammer it with melee well past its remaining HP.
        for (int i = 0; i < 40; ++i)
            fight.boss().at(0).takeMeleeDamage(200, scene, *world, x3::DamageType::Melee);
        step(fight, scene, *world, 3);
        const float frac = fight.bossHpFrac();
        check(fight.bossAlive() && fight.phase() == ClonePhase::NeuralCollar &&
              frac >= CloneBossFight::kCollarShieldFrac - 0.01f && !fight.cloneDead(),
              "C2 the Clone is SHIELDED in P2 — it cannot be killed or advanced by "
              "damage; only the collar opens the gate");
    }

    // ---- C3: 3 strikes destroy the collar -> "Sarah freed" once -> P3 MUTATED. --
    {
        const x3::phys::Vec3 outOfReach{ 40.0f, 0.0f, 0.0f };
        const bool rejectedFar = !fight.strikeCollar(outOfReach);
        bool consumed = true;
        for (int i = 0; i < NeuralCollar::kStrikes; ++i)
            consumed = consumed && fight.strikeCollar(collarPos);
        step(fight, scene, *world, 10);   // let the internal machine cross phase3Frac
        const bool freed  = fight.sarahFreed() && sarahFreedFires == 1;
        const bool woke   = fight.sarahPresent() && !fight.sarahRestrained() &&
                            !fight.sarah().at(0).docile();
        const bool mutated = fight.phase() == ClonePhase::MutatedHybrid &&
                             fight.bossVisualPhase() == BossPhase::Phase3 &&
                             fight.bossAlive() && !fight.collarActive() &&
                             fight.collar().destroyed;
        check(rejectedFar && consumed && freed && woke && mutated,
              "C3 collar destroy (3 strikes, reach-gated) fires SARAH FREED once and "
              "advances P2 -> P3 MUTATED HYBRID (internal Phase3 = the mutation)");
    }

    // ---- C4: killing the mutated Clone fires "Clone dead" once -> DEAD. -------
    {
        for (int i = 0; i < 30 && fight.bossAlive(); ++i)
            fight.boss().at(0).takeMeleeDamage(120, scene, *world, x3::DamageType::Melee);
        step(fight, scene, *world, 5);
        check(!fight.bossAlive() && fight.cloneDead() && cloneDeadFires == 1 &&
              fight.phase() == ClonePhase::Dead,
              "C4 killing the mutated Clone fires CLONE DEAD once + lands in DEAD "
              "(the descent-gate flag)");
    }

    // ---- C5: events are idempotent (extra ticks/strikes never re-fire). -------
    {
        step(fight, scene, *world, 60);
        const bool noRestrike = !fight.strikeCollar(collarPos) &&
                                !fight.holdCollar(1.0f, collarPos);
        check(sarahFreedFires == 1 && cloneDeadFires == 1 && noRestrike &&
              fight.phase() == ClonePhase::Dead,
              "C5 events are idempotent — extra ticks/strikes never re-fire");
    }
    fight.shutdown();
    world->shutdown();

    // ---- C6: the HOLD-E collar path reaches the same completion. --------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w2(x3::phys::createPhysicsWorld());
        w2->init();
        Scene s2;
        CloneBossFight f2;
        f2.build(s2, device, *w2, modelDir, clonePos, collarPos);
        int freed2 = 0;
        f2.setOnSarahFreed([&] { ++freed2; });
        setBossFrac(f2, CloneBossFight::kSeparationEndFrac - 0.02f);
        step(f2, s2, *w2, 3);
        const bool armed = f2.collarActive();
        // Hold in reach; also prove an out-of-reach hold makes NO progress.
        const x3::phys::Vec3 outOfReach{ 40.0f, 0.0f, 0.0f };
        const bool farNoop = !f2.holdCollar(0.5f, outOfReach);
        for (int i = 0; i < 400 && f2.collarActive(); ++i) f2.holdCollar(1.0f / 60.0f, collarPos);
        step(f2, s2, *w2, 10);
        check(armed && farNoop && f2.collar().destroyed && f2.sarahFreed() &&
              freed2 == 1 && f2.phase() == ClonePhase::MutatedHybrid,
              "C6 the hold-E collar path (reach-gated) completes identically -> "
              "SARAH FREED + P3");
        f2.shutdown();
        w2->shutdown();
    }

    x3::logInfo(std::string("clone: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
