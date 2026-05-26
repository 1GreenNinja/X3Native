// EFLZ Act 1 "The Spire" — FLOOR 4.5: the Nexus Chamber / The Chorus. See spire_nexus.h.
//
// Clean-room: built ONLY from the existing Scene / monster (MultiPodBoss +
// chorusConfig) / trigger / player systems + the engine interfaces. No purchased
// C# / id Tech / RBDOOM source consulted. CONTENT/LEVEL-SCRIPT ONLY — no renderer
// or core-engine changes; this STAGES the Wave-1 multi-pod boss machine in a
// discrete off-elevator arena and dispatches through the host's shared trigger
// system. Mirrors spire_mid.cpp's authoring/host style exactly.
#include "spire_nexus.h"
#include "asset_root.h"   // riggedGlbRoot() (the loose rigged-GLB dir for the self-test)

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Pod body-center Y above the arena floor (the rigged GLBs sit ~0.4 m up, same as
// SpireMidFloors' kEnemyYOff) — added to the arena base Y so a pod lands on the
// chamber floor, not the ground.
constexpr float kPodYOff = 0.4f;

// The Nexus arena is placed OFF the elevator spine. The numbered plates span
// x in [0,24], z in [-8,+8] with the elevator shaft at x=21,z=0. We place the
// chamber clear of that footprint in -Z (a separate volume hung off the F4->F5
// connector path), so it is unambiguously NOT on the spine and not overlapping a
// numbered floor plate. The exact XZ is content tuning; the off-spine PROPERTY is
// what the self-test asserts. kArenaCx is centered on the plate X so the chamber
// reads as a mezzanine hung off the wing; kArenaCz pushes it well clear in -Z.
constexpr float kArenaCx = 12.0f;    // arena center X (centered on the plate, off the +X spine)
constexpr float kArenaCz = -22.0f;   // arena center Z (well clear of the plates' z in [-8,8])

} // namespace

void SpireNexus::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
                       TriggerSystem& triggers, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);
    m_triggers = &triggers;

    // The "Floor 4.5" half-step Y: midway between the F4 and F5 plate base heights.
    // (F4 y0=20, F5 y0=25 in the canonical floor table -> Nexus at ~22.5.) This is a
    // WORLD-SPACE half-step the elevator never advertises as a numbered stop.
    const float f4y = layout.floorBaseY[(uint32_t)L1Floor::F4];
    const float f5y = layout.floorBaseY[(uint32_t)L1Floor::F5];
    const float baseY = (f4y + f5y) * 0.5f;

    const x3::phys::Vec3 arenaCenter{ kArenaCx, baseY, kArenaCz };
    const x3::phys::Vec3 origin{ kArenaCx, baseY + kPodYOff, kArenaCz };  // pods spawn at body-center Y

    // Stage THE CHORUS verbatim: 5 fused minds in 5 pods, fall when all 5 are downed,
    // save up to 4 (Subject Zero/Maya is the un-sparable core). chorusConfig() is the
    // canon instance; we use it as-is. Pods fall back to the tinted procedural box if
    // the rigged GLBs are absent (clean-checkout safe).
    const MultiPodBoss::Config cfg = chorusConfig();
    m_chorus.build(cfg, scene, device, physics, m_modelDir, origin);

    // The encounter is NOT armed at load: register the F4->F5 connector trigger
    // DISABLED. It "discovers" the Nexus when the host enables it later (found later
    // on the F4->F5 path) and the player walks into it; on its fired id the host
    // dispatches onTrigger() which arms the Chorus. The volume is a box hung on the
    // -Z connector mouth at the half-step Y (placement is content; the GATING is the
    // contract — it is added enabled=false so it cannot fire at load).
    triggers.add(x3::phys::Vec3{ kArenaCx - 6.0f, baseY - 1.0f, kArenaCz + 9.0f },
                 x3::phys::Vec3{ kArenaCx + 6.0f, baseY + 4.0f, kArenaCz + 13.0f },
                 (uint32_t)NexusTrigger::Connector, /*enabled*/false);

    // Fill the authored plan (read by the host HUD + the self-test).
    m_plan.arena          = arenaCenter;
    m_plan.baseY          = baseY;
    m_plan.f4BaseY        = f4y;
    m_plan.f5BaseY        = f5y;
    m_plan.podCount       = m_chorus.podCount();
    m_plan.maxSaved       = cfg.maxSaved;
    m_plan.fallThresh     = m_chorus.fallThreshold();
    m_plan.isElevatorStop = false;        // off-elevator half-step (never a numbered stop)

    m_armed = false;
    m_built = true;
    x3::logInfo("SpireNexus::build complete — Floor 4.5 'Nexus Chamber' staged OFF the "
                "elevator spine at y=" + std::to_string(baseY) + " (between F4 y=" +
                std::to_string(f4y) + " and F5 y=" + std::to_string(f5y) + "); The Chorus = " +
                std::to_string(m_chorus.podCount()) + " pods (save up to " +
                std::to_string(cfg.maxSaved) + "), fallThreshold=" +
                std::to_string(m_chorus.fallThreshold()) +
                "; connector trigger DISABLED (not armed at load — found later on the F4->F5 path)");
}

void SpireNexus::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                      const x3::phys::Vec3& playerPos, IDamageSink* player,
                      const AttackFxFn& attackFx) {
    if (!m_built) return;
    // GATED: the Chorus does not act until the Nexus is discovered (armed). Before
    // then the pods are placed but inert — no chase/attack/phase advance.
    if (!m_armed) return;

    // The Chorus attacks only while the player is alive (matches SpireMidFloors::tick).
    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;
    m_chorus.update(dt, scene, physics, playerPos, atkTarget, attackFx, BossPhaseFn{});
}

void SpireNexus::onTrigger(uint32_t triggerId) {
    switch ((NexusTrigger)triggerId) {
        case NexusTrigger::Connector:
            // The player found the F4->F5 connector — DISCOVER the Nexus and arm the
            // Chorus. Idempotent (the trigger latches once anyway).
            if (!m_armed) {
                m_armed = true;
                x3::logInfo("SpireNexus: F4->F5 connector reached — Floor 4.5 NEXUS discovered; "
                            "The Chorus awakens (5 voices in unison)");
            }
            break;
    }
}

bool SpireNexus::onInteract(const x3::phys::Vec3& playerPos, Scene& scene,
                            x3::phys::IPhysicsWorld& physics, float range) {
    if (!m_built || !m_armed) return false;       // no sparing before discovery
    // SPARE the nearest LIVE pod within range — the "save up to 4" morality path.
    // sparePod() enforces the maxSaved cap (4) and refuses a dead/out-of-range pod,
    // so the core (Subject Zero/Maya) cannot be saved once the budget is spent.
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < m_chorus.podCount(); ++i) {
        const MonsterSystem& p = m_chorus.pod(i);
        if (!p.alive()) continue;                 // already down (killed or saved)
        const x3::phys::Vec3 pp = p.pos();
        const float dx = playerPos.x - pp.x;
        const float dy = playerPos.y - pp.y;
        const float dz = playerPos.z - pp.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;                   // no live pod in reach
    return m_chorus.sparePod((uint32_t)best, scene, physics);
}

FireResult SpireNexus::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                              Scene& scene, x3::phys::IPhysicsWorld& physics,
                              int damage) {
    if (!m_built || !m_armed) return FireResult{};   // no Chorus to hit before discovery
    return m_chorus.fire(eye, dir, scene, physics, damage);
}

void SpireNexus::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const Scene& scene) const {
    if (!m_built) return;
    m_chorus.drawAll(device, frame, scene);
}

bool SpireNexus::offElevatorSpine(const x3::phys::Vec3& shaftCenter, float shaftHalfXZ) const {
    if (!m_built) return false;
    // The arena origin is OFF the spine iff it is clear of the shaft footprint in XZ
    // (outside the shaft half-extents on either axis).
    const float dx = std::fabs(m_plan.arena.x - shaftCenter.x);
    const float dz = std::fabs(m_plan.arena.z - shaftCenter.z);
    return dx > shaftHalfXZ || dz > shaftHalfXZ;
}

} // namespace x3::game

// ===========================================================================
// Headless self-test (--test-nexus). Builds the Spire (buildLevel1) + the Nexus on a
// HeadlessDevice + Jolt world and asserts the Floor 4.5 / Chorus contract. No
// window / Vulkan. Mirrors runSpireMidSelfTest().
// ===========================================================================
#include "headless_device.h"

namespace x3::game {
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[nexus-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[nexus-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

using HeadlessDevice = x3::game::HeadlessRenderDevice;

// The elevator spine XZ from level1.cpp (kShaftCx=21, kShaftCz=0, half=1.5). Mirrored
// here for the off-spine assertion (the level table is the single source of truth;
// these are the published shaft constants the host builds the cab at).
constexpr float kShaftCx = 21.0f, kShaftCz = 0.0f, kShaftHx = 1.5f;

// A trivial damage sink for the gated-tick assertions (a live target the Chorus
// would attack IF it were armed). Mirrors the other tests' stubs.
class NexusTargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 12.0f, 1.6f, -22.0f };
    int hits = 0;
    bool takeDamage(int) override { ++hits; return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

} // namespace

bool runNexusSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    // Scope every body-owning system (Scene + SpireNexus, whose MultiPodBoss pods own
    // Jolt bodies) in an INNER block so they are destroyed — releasing those bodies —
    // BEFORE physics->shutdown() frees the Jolt world. Without this, the function-end
    // destructor order (physics->shutdown() first, then ~nexus/~scene) tore the world
    // down under the body owners -> a process-exit access violation (the test logged
    // 11/11 in Release, then crashed 0xC0000005 on teardown; Debug tripped a Jolt assert).
    {
    Scene scene;

    // Build the Spire geometry (gives floorBaseY[]), then stage the Nexus on top,
    // sharing a TriggerSystem like the host does.
    Level1Layout layout = buildLevel1(scene, device, *physics);
    TriggerSystem triggers;
    SpireNexus nexus;
    const std::string rigged = riggedGlbRoot();
    nexus.build(scene, device, *physics, layout, triggers, rigged);

    check(nexus.built(), "N0 Nexus staged");

    // ---- N1: the 5-pod Chorus builds (chorusConfig wired through). ----
    {
        check(nexus.podCount() == 5 && nexus.plan().podCount == 5,
              "N1 The Chorus builds 5 pods");
    }

    // ---- N2: NOT armed at load — gated on the connector trigger. The connector is
    // registered DISABLED so it cannot fire at load. ----
    {
        const bool notArmed = !nexus.armed();
        const TriggerVolume* conn = triggers.findById((uint32_t)NexusTrigger::Connector);
        const bool connDisabled = conn && !conn->enabled && !conn->fired;
        check(notArmed && connDisabled,
              "N2 NOT armed at load (connector trigger present + DISABLED)");
    }

    // ---- N3: while UNARMED the Chorus is inert — ticking many frames with a live
    // target produces NO attacks on it (no chase/attack/phase before discovery). ----
    {
        NexusTargetStub tgt;
        for (int i = 0; i < 240; ++i) {
            // Walk the triggers like the host does; the connector is disabled so this
            // never arms the Nexus.
            for (uint32_t tid : triggers.update(tgt.eye)) nexus.onTrigger(tid);
            nexus.tick(kFixedDt, scene, *physics, tgt.eye, &tgt, AttackFxFn{});
            physics->step(kFixedDt);
            scene.update(*physics);
        }
        check(!nexus.armed() && tgt.hits == 0,
              "N3 inert while unarmed (no attacks dealt before discovery)");
    }

    // ---- N4: OFF-ELEVATOR — the Nexus is NOT a numbered elevator stop, and its arena
    // sits OFF the elevator spine, at the half-step Y BETWEEN the F4 and F5 plates. ----
    {
        const bool notAStop = !nexus.isElevatorStop() && !nexus.plan().isElevatorStop;
        const bool offSpine = nexus.offElevatorSpine(
            x3::phys::Vec3{ kShaftCx, 0.0f, kShaftCz }, kShaftHx);
        const float f4y = layout.floorBaseY[(uint32_t)L1Floor::F4];
        const float f5y = layout.floorBaseY[(uint32_t)L1Floor::F5];
        const bool halfStep = nexus.plan().baseY > f4y && nexus.plan().baseY < f5y &&
                              std::fabs(nexus.plan().baseY - (f4y + f5y) * 0.5f) < 1e-3f;
        x3::logInfo(std::string("[nexus-test] baseY=") + std::to_string(nexus.plan().baseY) +
                    " (F4=" + std::to_string(f4y) + " F5=" + std::to_string(f5y) + ")");
        check(notAStop && offSpine && halfStep,
              "N4 off-elevator half-step (not a stop; off-spine; Y between F4 and F5)");
    }

    // ---- N5: reaching the connector ARMS the encounter (the host enables it once the
    // F4->F5 path is open; entering it discovers the Nexus). ----
    {
        triggers.setEnabled((uint32_t)NexusTrigger::Connector, true);   // F4->F5 path opens
        const x3::phys::Vec3 connPt = nexus.plan().arena;               // step into the chamber
        x3::phys::Vec3 p{ connPt.x, nexus.plan().baseY + 1.0f, connPt.z + 11.0f };  // inside the connector box
        for (uint32_t tid : triggers.update(p)) nexus.onTrigger(tid);
        check(nexus.armed(), "N5 connector trigger discovers + arms the Nexus");
    }

    // ---- N6: the boss does NOT FALL until the pod fall-threshold is met. Down 4 of 5
    // pods by lethal damage; the 5th drop crosses the threshold. ----
    {
        const uint32_t n = nexus.podCount();
        const uint32_t thresh = nexus.fallThreshold();
        bool fellEarly = false;
        for (uint32_t i = 0; i < n - 1; ++i) {
            MonsterSystem& p = nexus.chorus().pod(i);
            p.takeMeleeDamage(p.hp() + 1, scene, *physics);   // kill pod i
            if (nexus.hasFallen()) fellEarly = true;          // must NOT fall before all down
        }
        const bool notFallenBelow = !fellEarly && !nexus.hasFallen();
        MonsterSystem& last = nexus.chorus().pod(n - 1);
        last.takeMeleeDamage(last.hp() + 1, scene, *physics);  // down the last -> falls
        const bool fallenAtThreshold = nexus.hasFallen();
        check(thresh == 5 && notFallenBelow && fallenAtThreshold,
              "N6 boss falls ONLY when the pod fall-threshold (5) is met");
        check(nexus.killedCount() == 5 && nexus.savedCount() == 0,
              "N6 killing all pods => killedCount=5, savedCount=0");
    }

    // ---- N7: the SAVE-UP-TO-4 morality path. On a FRESH Nexus, spare pods via
    // onInteract (E in range): each spare increments savedCount (NOT killedCount), and
    // the maxSaved cap (4) refuses the 5th — the core (Subject Zero/Maya) remains. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        // Same teardown discipline as the outer scope: scope the body owners so they die
        // BEFORE w->shutdown() (below) frees the Jolt world.
        {
        Scene s2;
        Level1Layout L2 = buildLevel1(s2, device, *w);
        TriggerSystem t2;
        SpireNexus nx2;
        nx2.build(s2, device, *w, L2, t2, rigged);
        // Discover/arm it (sparing is gated on discovery too).
        t2.setEnabled((uint32_t)NexusTrigger::Connector, true);
        nx2.onTrigger((uint32_t)NexusTrigger::Connector);
        const bool armedOk = nx2.armed();

        // Spare 4 pods: stand AT each live pod and interact. Each must increment saved.
        uint32_t spared = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            // Find a live pod and step onto it (well within kRescueReach).
            int target = -1;
            for (uint32_t i = 0; i < nx2.podCount(); ++i)
                if (nx2.chorus().pod(i).alive()) { target = (int)i; break; }
            if (target < 0) break;
            const x3::phys::Vec3 at = nx2.chorus().pod((uint32_t)target).pos();
            if (nx2.onInteract(at, s2, *w, kRescueReach)) ++spared;
        }
        const uint32_t savedAfter = nx2.savedCount();
        const uint32_t killedAfter = nx2.killedCount();
        // The 5th spare must be REFUSED (cap reached) — there IS a live pod (the core)
        // but the morality budget is spent.
        int liveCore = -1;
        for (uint32_t i = 0; i < nx2.podCount(); ++i)
            if (nx2.chorus().pod(i).alive()) { liveCore = (int)i; break; }
        bool fifthRefused = false;
        if (liveCore >= 0) {
            const x3::phys::Vec3 at = nx2.chorus().pod((uint32_t)liveCore).pos();
            fifthRefused = !nx2.onInteract(at, s2, *w, kRescueReach);   // refused by the cap
        }
        x3::logInfo(std::string("[nexus-test] armed=") + (armedOk ? "1" : "0") +
                    " spared=" + std::to_string(spared) +
                    " saved=" + std::to_string(savedAfter) +
                    " killed=" + std::to_string(killedAfter) +
                    " coreStillLive=" + (liveCore >= 0 ? "1" : "0") +
                    " fifthRefused=" + (fifthRefused ? "1" : "0"));
        check(armedOk && spared == 4 && savedAfter == 4 && killedAfter == 0,
              "N7 SAVE path: sparing increments saved (not killed) up to 4");
        check(liveCore >= 0 && fifthRefused,
              "N7 maxSaved cap holds: the 5th spare is refused (Subject Zero remains)");

        // The boss has NOT fallen with 4 saved + 1 alive (only 4 downed of 5).
        check(!nx2.hasFallen() && nx2.downedCount() == 4 && nx2.aliveCount() == 1,
              "N7 4 saved + core alive => 4 downed, boss not yet fallen");
        }   // s2/t2/nx2 (+ pods' bodies) destruct here, before w->shutdown() below
        w->shutdown();
    }

    }   // end inner block: scene/triggers/nexus (+ its MultiPodBoss pods' bodies) destruct
        // HERE, before physics->shutdown() below — so the Jolt world outlives its bodies.
    physics->shutdown();
    x3::logInfo(std::string("nexus: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
