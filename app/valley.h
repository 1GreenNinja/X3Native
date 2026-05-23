#pragma once
// Crystal Valleys — Act 2, Level 15 (--world valley).
//
// Game/slice code only — engine/ stays pure. The FIRST open surface biome of
// Act 2 (canon: docs/MASTER_GAME_PLAN.md "L15-20 Crystal Valleys — first surface
// biome; Dominion patrols; the Salvari ship crash -> meet K'thara"). It comes
// AFTER the cliffs finale of Act 1.
//
// LOW-CONFLICT, additive: like Club1127World (app/club1127.*), this is a
// self-contained world reached via the `--world valley` CLI flag. It does NOT
// touch level1.cpp / the Spire. It is CONTENT placed onto the engine's existing
// STREAMED terrain (the farm's TerrainStreamer): the host brings up the streamer
// + sky exactly as `--world terrain` does, and ValleyWorld anchors everything to
// that surface through the terrain placement API (app/terrain.h):
//   * placeOnTerrain(x,z,out)          — drop a thing ON the surface.
//   * terrainHeightAtWorld(x,z)        — surface Y for a query.
//   * terrainNormalAtWorld(x,z,n)      — slope normal (tilt the crashed ship to it).
//
// What it places (canon content, modeled on the club's character/prop pattern):
//   * DOMINION PATROLS — a few hostile Dominion enemies (rigged-GLB MonsterSystem
//     guards/drones; the MonsterSystem auto-binds the locomotion blend + nav),
//     each anchored ON the terrain via placeOnTerrain, patrolling the valley.
//   * SALVARI SHIP CRASH SITE — the ship GLB dropped onto the terrain and TILTED
//     to the local surface normal as if it crashed (orient to terrainNormalAtWorld).
//   * K'THARA — the Salvari commander, a FRIENDLY ally NPC near the crash. Spawned
//     like an enemy but marked non-hostile (damage 0, never attacks the player).
//   * WATER — a lake at a low spot of the terrain (host applies setWaterParams at
//     the sea level this world exposes).
//   * CRYSTAL FORMATIONS — a scatter of emissive crystal/rock props (emissive
//     boxes with their own point lights, LevelArchitect-style — same construction
//     the caves use), giving the biome its glow.
//
// Construction mirrors app/club1127.cpp:
//   * Emissive prop geometry: x3::prims::makeBox -> device->createMesh +
//     physics->addStaticMesh, registered as Scene entities (Tag::Static).
//   * Characters: MonsterSystem (hostile Dominion = damage>0 chase>0; friendly
//     K'thara = damage 0 chase 0). A failed GLB load falls back to a box so the
//     level never breaks.
//   * Lights: the world hands the host a vector<PointLight> (crystal glow) to push
//     via setPointLights, plus the water sea level for setWaterParams.

#include "scene.h"
#include "monster.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Build result / handle for the Crystal Valleys area. Owns the character systems
// (hostile Dominion + friendly K'thara) so their loaded GLB GPU handles stay alive
// for the app lifetime, and exposes the spawn point, the crystal point-light set,
// and the water sea level the host applies.
class ValleyWorld {
public:
    // Build the valley CONTENT onto the already-streamed terrain in `scene` /
    // `physics`, uploading prop meshes through `device`. `modelDir` is the rigged-
    // GLB root (assets/rigged_glb) used for the Dominion patrols + K'thara + the
    // Salvari ship. Everything is anchored to the surface via the terrain placement
    // API. Call once. Idempotent: a second call is a no-op.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir);

    // Advance the characters one frame. Hostile Dominion enemies chase/attack the
    // player (pass a live target); the friendly K'thara just idles in place. The
    // crystals/ship/terrain are static, so this only ticks the NPCs.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerPos, IDamageSink* target);

    // Draw the loaded GLB characters (Dominion patrols, K'thara) and the ship.
    // Call alongside scene.render() each frame, like Club1127World::drawCharacters.
    void drawCharacters(x3::rhi::IRenderDevice& device,
                        const x3::rhi::FrameContext& frame, const Scene& scene) const;

    // Player spawn (feet position) — on the surface near the crash site, on dry
    // ground above the lake. The host poses the camera/character here.
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // A good fixed showcase camera pose for the headless screenshot: an elevated
    // 3/4 vantage that frames the crashed ship, K'thara, a Dominion patrol, the
    // crystals, and the lake in one frame. Fills 5 floats (x,y,z,yaw,pitch).
    void showcaseCamera(float out[5]) const;

    // The crystal point lights the host applies with setPointLights once after
    // build (static; re-uploaded each frame). color[] is linear RGB * intensity.
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // The lake sea level (world Y) the host feeds to setWaterParams.
    float waterSeaLevel() const { return m_seaLevel; }

    // ---- Placement queries (host + self-test) ----------------------------
    // World position the crashed Salvari ship was placed at (its surface anchor).
    x3::phys::Vec3 shipPos() const { return m_shipPos; }
    // World position K'thara (the ally) was placed at (her surface anchor).
    x3::phys::Vec3 ktharaPos() const { return m_ktharaPos; }
    // Number of hostile Dominion enemies spawned.
    uint32_t dominionCount() const { return m_dominionCount; }
    // True iff K'thara is the friendly/ally NPC (never attacks the player).
    bool ktharaIsAlly() const { return m_ktharaAlly; }

    bool built() const { return m_built; }

private:
    // An emissive crystal prop (render mesh + Jolt collision + Scene entity), with
    // its own point light. Center + half extents in world meters.
    uint32_t addCrystal(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        float cx, float cy, float cz, float hx, float hy, float hz,
                        const float color[4], const float emissive[4],
                        float lr, float lg, float lb, float lrange, bool collide);

    // A character (Dominion enemy or friendly ally) loaded via MonsterSystem.
    // hostile => damage>0 / chase>0 (a real enemy); !hostile => inert ally (K'thara).
    // Returns the index into m_chars.
    uint32_t addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                          const std::string& modelFile, const x3::phys::Vec3& pos,
                          float scale, bool standUpZtoY, const float tint[4],
                          bool hostile, MonsterType type);

    bool                                          m_built = false;
    x3::phys::Vec3                                m_spawn{};
    float                                         m_seaLevel = 0.0f;
    std::vector<x3::rhi::PointLight>              m_lights;
    std::vector<std::unique_ptr<MonsterSystem>>   m_chars;
    // Parallel to m_chars: true for hostile Dominion enemies, false for the ally.
    std::vector<bool>                             m_hostile;

    x3::phys::Vec3   m_shipPos{};
    x3::phys::Vec3   m_ktharaPos{};
    uint32_t         m_dominionCount = 0;
    bool             m_ktharaAlly    = false;
};

// Headless self-test (--test-valley). Builds the valley content onto a synchronous
// (no-job) terrain streamer via a HeadlessRenderDevice + physics, then asserts:
//   * the terrain placement API returns finite/valid heights over the area;
//   * the Salvari ship + K'thara sit ON the surface (their Y == terrainHeightAtWorld
//     within tolerance);
//   * the expected number of Dominion enemies spawned (and K'thara is an ally);
//   * water params were set (a positive sea level is exposed).
// Logs "[valley-test] N passed, 0 failed" and returns true iff all pass. No
// window / Vulkan. Lives in valley.cpp.
bool runValleySelfTest();

} // namespace x3::game
