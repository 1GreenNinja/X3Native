// LEVEL 4.5 — THE NEXUS CHAMBER (W5-1). The facility's hidden horror: a 25 m cavern
// void between F4 and F5 whose thin tier platforms the data authors (Entry Platform,
// Whisper Gallery, Memory Maze, Resonance Ring, Chorus Antechamber, Apex Arena — "The
// Chorus", 3,472 merged consciousnesses). VIGIL denies this level exists; the elevator
// listing skips it; the blank expanse IS the tell (Tim's spec).
//
// This module owns everything the loader doesn't: the rock cavern shell sealing the
// void above the open-ceiling Nexus Chamber Access room, the scaffold climb (tower +
// L-stairs + catwalk decks — doctrine-legal risers, no ramps steeper than law), the
// two-accent horror dressing (biolume green veins + blood-red apex accents — the
// ART_BIBLE's ONE sanctioned two-accent zone), sparse creature spawns, the Whisper
// Gallery's proximity dread audio (the "name-calling" beat VIGIL warned about), and
// the dormant apex stand-in that wakes when the player nears the Apex Arena.
#pragma once

#include "level_loader.h"
#include "monster.h"
#include "surface_library.h"
#include "engine/audio/IAudioSystem.h"

#include <cstdint>
#include <vector>

namespace x3::game {

class Canon45 {
public:
    // W5-1b (fix/spire-hollow-core, owner canon 2026-07-25): the hidden level's
    // ARRIVAL MOUTH — a doorway-sized opening joining the elevator spine shaft to the
    // 4.5 arrival tunnel. The host feeds these to CanonBuildOpts (spineMouth*) so the
    // tube wall is cut where the tunnel Canon45 builds seals onto it.
    static constexpr float kMouthH    = 3.0f;   // opening height above the cavern floor
    static constexpr float kMouthHalf = 1.2f;   // opening half-width (X)

    // The 4.5 cavern FLOOR PLANE (top-of-slab Y), derived purely from the loaded
    // tower data: just above the tallest normal-floor roof inside the cavern
    // envelope (the F4 boss arena), so the slab seals every downward sightline and
    // interpenetrates nothing. Returns a large negative value when the tower
    // carries no Nexus platforms (callers treat that as "no hidden level").
    static float floorPlaneY(const CanonFloor& floor);

    // The 4.5 cavern ENVELOPE box, data-derived (same derivation build() uses):
    // out = { x0, x1, z0, z1, yFloor, yCeil }. False when the tower has no Nexus.
    // Consumed by the level lint's hidden-floor seal gate.
    static bool envelope(const CanonFloor& floor, float out[6]);

    // Build the cavern (shell/climb/dressing/spawns) from the loaded tower. Appends its
    // motivated lights into `canonLights` (same per-room-gated feed the tower uses).
    // No-op (built()==false) if the tower carries no Nexus platforms.
    void build(CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view riggedModelDir,
               const std::string& surfaceLibRoot, std::vector<CanonLight>& canonLights);

    bool built() const { return m_built; }
    // The cavern floor plane the build actually used (== floorPlaneY of its floor).
    float cavernFloorY() const { return m_y0; }

    // Combat sinks (same fan-out contract as CanonPlay's managers).
    void setCueSink(const GameCueFn& sink)      { m_creatures.setCueSink(sink); }
    void setDeathFxSink(const DeathFxFn& sink)  { m_creatures.setDeathFxSink(sink); }

    // Per-frame: whisper/name-call proximity audio, apex wake check, creature AI.
    // `audio` may be null (headless); whisper handles may be invalid (silent skip).
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos, IDamageSink* playerSink,
                const AttackFxFn& fx,
                x3::audio::IAudioSystem* audio,
                x3::audio::SoundHandle whisperQuiet, x3::audio::SoundHandle whisperCall);

    // Draw the cavern's creatures (shell/dressing are scene entities — the world
    // pass draws them). Mirrors CanonPlay's manager draw.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              Scene& scene)
    { m_creatures.drawAll(device, frame, scene); }

    MonsterManager&       creatures()       { return m_creatures; }
    const MonsterManager& creatures() const { return m_creatures; }

    // Shutdown hook (ragdoll bodies) — call before physics teardown, like CanonPlay.
    void shutdown() { m_creatures.shutdown(); }

private:
    bool m_built = false;
    MonsterManager m_creatures;
    SurfaceLibrary m_lib;

    // Cavern envelope (world m) captured at build for the update() proximity logic.
    float m_x0 = 0, m_x1 = 0, m_y0 = 0, m_y1 = 0, m_z0 = 0, m_z1 = 0;
    float m_whisperX = 0, m_whisperY = 0, m_whisperZ = 0;   // Whisper Gallery center
    float m_apexX = 0, m_apexY = 0, m_apexZ = 0;            // Apex Arena center
    int   m_apexIdx = -1;          // index of the dormant apex creature in m_creatures
    bool  m_apexAwake = false;
    float m_whisperT = 0, m_callT = 0;   // countdown timers (jittered on fire)
    uint32_t m_rng = 0x45c4a11u;         // tiny deterministic LCG for jitter/positions
};

} // namespace x3::game
