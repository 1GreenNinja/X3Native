#pragma once
// Companion Slice C seam: CompanionController -- ONE clean object that wraps the
// previously-scattered companion triple:
//   * a Player        (physics capsule + camera + facing),
//   * a CompanionBrain (the Slice-A deterministic reflex utility AI), and
//   * a per-companion Identity (name / sex -> Provider routing / model / voice).
//
// Design choice (documented per task): the controller OWNS its own Player +
// CompanionBrain rather than being a view over a CompanionSquad slot. Rationale:
//   * The upcoming LLM cognitive layer (Slice C) binds ONE cognitive brain to ONE
//     companion and needs a stable, self-contained handle -- independent of the
//     squad's cross-slot downed/revive bookkeeping.
//   * CompanionSquad's revive state machine couples slots together (a slot revives
//     ANOTHER slot); a single CompanionController is a standalone agent whose
//     downed/revive lifecycle is self-contained and testable in isolation.
//   * The reflex internals (CompanionBrain + CompanionContext + CompanionCommand)
//     and CompanionSquad are reused UNCHANGED -- this is a facade, not a rewrite.
//
// The optional visual body reuses the same inert-MonsterSystem proxy pattern that
// `--world companion` already uses (a rigged GLB drawn at the capsule pose). It is
// only built when spawn() is given a live render device + scene, so the headless
// self-test runs with no GPU.
//
// Clean-room: X3Native systems only. No engine source consulted.
// Namespace: x3::game

#include "companion.h"
#include "player.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <memory>
#include <string>

namespace x3::game {

class MonsterSystem;   // forward decl: threats + optional visual body proxy
class Scene;

class CompanionController {
public:
    // Cognitive-layer provider routing (per EFLZ design): female companions are
    // voiced by Grok, male companions by Claude.
    enum class Provider : uint8_t { Grok = 0, Claude = 1 };

    // Per-companion identity. `female` drives Provider routing; the model/voice
    // keys are carried for the visual + (future) bark systems.
    struct Identity {
        std::string name;                 // e.g. "Anna", "Sarah", "Marcus"
        bool        female = true;        // female -> Grok, male -> Claude
        std::string voiceLineSet;         // key into a future voice/bark table
        std::string modelFile;            // rigged GLB for the visible body
    };

    // ---- Lifecycle --------------------------------------------------------
    // Spawn the owned Player capsule at `pos` (feet) facing `yaw`. If `dev`/`scene`
    // are non-null, also build an inert visual MonsterSystem from id.modelFile so
    // the body is drawable; pass nullptr for both in headless tests. Call once.
    void spawn(x3::phys::IPhysicsWorld& phys,
               x3::rhi::IRenderDevice* dev, Scene* scene,
               const x3::phys::Vec3& pos, float yaw, const Identity& id);

    // Per-tick: build a CompanionContext from the live world, run the reflex brain
    // (biased by any pending suggest()), map the resulting CompanionCommand onto the
    // owned Player (facing via setLook + movement via PlayerInput), drive
    // Player::update, and fire at threats when the brain commands it. Also advances
    // the self-contained downed lifecycle. `threats` is the live enemy array; its
    // pointers must remain valid for this call.
    void tick(float dt, x3::phys::IPhysicsWorld& phys,
              const x3::phys::Vec3& playerPos, float playerHpFrac, bool playerDowned,
              MonsterSystem* const* threats, uint32_t threatCount);

    // ---- Slice-C cognitive seam -------------------------------------------
    // The cognitive layer pushes a desired behavior bias here; the NEXT tick()'s
    // brain run consumes it (a SOFT nudge via the existing CompanionSuggestion
    // mechanism -- it cannot override a strong reflex decision). The suggestion is
    // sticky until replaced or cleared.
    void suggest(const CompanionSuggestion& s) { m_suggestion = s; }
    void clearSuggestion() { m_suggestion = CompanionSuggestion{}; }
    const CompanionSuggestion& suggestion() const { return m_suggestion; }

    // Queue a spoken/bark line (buffered for now; a future voice system drains it).
    void say(const std::string& line) { m_pendingSpeech = line; }
    const std::string& pendingSpeech() const { return m_pendingSpeech; }
    void clearSpeech() { m_pendingSpeech.clear(); }

    // ---- Identity / provider ----------------------------------------------
    Provider provider() const { return m_id.female ? Provider::Grok : Provider::Claude; }
    const Identity& identity() const { return m_id; }

    // The last behavior the brain chose (for debug / cognitive feedback).
    CompanionBehavior lastBehavior() const { return m_lastBehavior; }

    // ---- Pass-throughs to the owned Player / lifecycle --------------------
    x3::phys::Vec3 feet() const { return m_player.feet(); }
    float yaw()   const { return m_player.yaw(); }
    int   hp()    const { return m_player.hp(); }
    int   maxHp() const { return m_player.maxHp(); }
    // Alive == not dead AND not downed (downed companions are a distinct state).
    bool  isAlive() const { return m_player.isAlive() && !m_downed; }
    bool  isDowned() const { return m_downed; }

    // Apply damage to the owned Player; if HP hits 0 the controller enters Downed.
    void takeDamage(int amount);

    // Advance this companion's revive while a teammate is reviving it. Accumulates
    // `dt`; once the configured hold time elapses, restores HP and clears Downed.
    // Returns true on the frame the revive COMPLETES (companion stands back up).
    bool reviveProgress(float dt);

    // Draw the visual body at the current capsule pose. No-op if no visual was
    // built (headless). Bakes feet position + yaw into the proxy entity transform.
    void drawBody(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& fr,
                  Scene& scene);

    // Remove the owned Player's physics body (call before phys.shutdown()).
    void shutdown(x3::phys::IPhysicsWorld& phys);

    // Direct access to the owned Player (for the host that owns the render loop).
    Player&       player()       { return m_player; }
    const Player& player() const { return m_player; }

private:
    Player          m_player;
    CompanionBrain  m_brain;
    Identity        m_id;
    CompanionSuggestion m_suggestion{};        // sticky cognitive bias for next tick
    std::string     m_pendingSpeech;           // buffered bark line
    CompanionBehavior m_lastBehavior = CompanionBehavior::Follow;

    bool   m_spawned     = false;
    bool   m_downed      = false;
    float  m_reviveTimer = 0.0f;               // >0 while being revived
    int    m_ammoInMag   = 30;                 // tracked host-side (deferred reload)

    // Optional visual body proxy (inert MonsterSystem rendering a rigged GLB).
    std::unique_ptr<MonsterSystem> m_viz;
    // Live scene bound at spawn (used by the fire raycast); null in pure-headless.
    Scene*                         m_scene = nullptr;
};

// Headless self-test (--test-companion-controller). Builds a flat-arena physics
// world and asserts: spawn/identity, Provider routing, engage-with-threat,
// suggestion bias, say/pendingSpeech round-trip, downed->revive->alive lifecycle,
// and clean shutdown. No window/Vulkan. Returns true iff all pass.
bool runCompanionControllerSelfTest();

} // namespace x3::game
