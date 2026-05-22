#pragma once
// Club 1127 + the flooded cave/tunnel network (EFLZ secret hub, lore code 1127).
//
// Game/slice code only — engine/ stays pure. A NEW self-contained area, kept
// LOW-CONFLICT: it does NOT touch level1.cpp / the Spire. The host reaches it via
// the `--world club` CLI flag (mirrors `--world terrain`); the in-game code-1127
// entry from the Spire would hook here later (see club1127.cpp header notes).
//
// What it builds (canon — docs/MASTER_GAME_PLAN.md "Secrets & hubs" + the
// EFLZ_SPIRE_7FLOOR spec §2b): a persistent NEON HUB — a multi-level club with a
// central dance floor, raised catwalks/balconies, a bar with a fixer NPC + a
// bouncer — then an ORGANIC flooded cave/tunnel network descending off the club:
// a sloped entry tunnel, an irregular cavern (power core), a flooded section with
// lurking sea creatures, a hidden boss arena, and lore-cache nooks.
//
// SHAPE/feel cribbed from the team's Babylon "LevelArchitect" walk-mode (cave +
// tunnel materials, descending arched tunnels, glowing cave crystals each with a
// small point light, emissive sconces). Box-engine approximation: many VARIED,
// jittered boxes (size/height/angle) so the caves read as organic rock, not a
// corridor; neon via the engine's forward point lights + emissive draws.
//
// Construction mirrors app/env_art.cpp + app/door.cpp:
//   * Geometry: x3::prims::makeBox -> device->createMesh (render) + physics->
//     addStaticMesh (collision), registered as Scene entities (Tag::Static). The
//     Scene draws/syncs them like any other static geometry.
//   * Characters (bartender / bouncer / sharks / sea creatures): loaded + drawn +
//     animated via MonsterSystem with damage 0 / chaseSpeed 0 (inert "props" that
//     still skin + play their idle clip). A failed GLB load falls back to a box,
//     so the area never breaks.
//   * Lights: the world hands the host a vector<PointLight> (neon for the club,
//     cool/teal for the caves, magenta accents) to push via setPointLights, plus
//     emissive fixture/crystal meshes for bloom.

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

// Build result / handle for the Club 1127 area. Owns the inert character systems
// (so their loaded GLB GPU handles stay alive for the app lifetime) and exposes
// the spawn point + the neon/cave point-light set the host must apply.
class Club1127World {
public:
    // Build the whole area (club room + cave/tunnel network) into `scene` /
    // `physics`, uploading meshes through `device`. `modelDir` is the rigged-GLB
    // root (G:/GameModels/rigged_glb) used for the bartender / bouncer / sea
    // creatures. Call once. Idempotent guard: a second call is a no-op.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir);

    // Advance the animated characters one frame (skeletal idle playback). The
    // club/cave geometry is fully static, so this only ticks the NPCs/creatures.
    void update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Draw the loaded GLB characters (bartender, bouncer, sea creatures, boss).
    // Call alongside scene.render() each frame, like Level1Game::drawWorldExtras.
    void drawCharacters(x3::rhi::IRenderDevice& device,
                        const x3::rhi::FrameContext& frame, const Scene& scene) const;

    // Player spawn (feet position) — at the club entrance landing, facing the
    // dance floor. The host poses the camera/character here.
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // A good fixed showcase camera pose for the headless screenshot: an elevated
    // 3/4 vantage over the dance floor that frames the catwalks + bar, with the
    // cave-mouth visible beyond. Fills the 5 floats (x,y,z,yaw,pitch).
    void showcaseCamera(float out[5]) const;

    // The neon (club) + cool/teal (cave) point lights the host should apply with
    // IRenderDevice::setPointLights once after build (static; re-uploaded each
    // frame by the device). color[] is linear RGB premultiplied by intensity.
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    bool built() const { return m_built; }

private:
    // --- Geometry helpers (defined in the .cpp) ---
    // A solid static box (render mesh + Jolt collision + Scene entity), tinted,
    // optionally emissive (for neon strips / crystals / fixtures). Center + half
    // extents in world meters. Returns the new entity id.
    uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics,
                    float cx, float cy, float cz, float hx, float hy, float hz,
                    const float color[4], const float emissive[4], bool collide,
                    float uvScale = 1.0f);

    // Inert character prop: a MonsterSystem with damage 0 / chaseSpeed 0 that
    // loads + draws + animates `modelFile`. Returns the system index in m_chars.
    void addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                      const std::string& modelFile, const x3::phys::Vec3& pos,
                      float scale, bool standUpZtoY, const float tint[4]);

    bool                                          m_built = false;
    x3::phys::Vec3                                m_spawn{};
    std::vector<x3::rhi::PointLight>              m_lights;
    // Cached unique meshes/textures are owned by the device; we keep no GPU
    // handles here beyond what the Scene entities + character systems hold.
    std::vector<std::unique_ptr<MonsterSystem>>   m_chars;
    // Per-character animation target position for update() (props track a far,
    // unreachable point so they face a fixed direction and just idle in place).
    std::vector<x3::phys::Vec3>                   m_charFace;
};

} // namespace x3::game
