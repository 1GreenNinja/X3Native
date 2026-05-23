#pragma once
// ABOVE-GROUND SALVARI CLIFFS FINALE (Act 1 §2d) — `--world cliffs`.
// Game/slice code only — engine/ stays pure.
//
// The snowy mountain exterior past the F7 rooftop (docs/MASTER_GAME_PLAN.md Act 1
// §2d "above-ground exterior — cliffs + the Salvari landing"; specs/
// EFLZ_SPIRE_7FLOOR.spec.md §2d). It is the payoff after the ascent: cliffs, sky,
// snow, the alien ship — the climax exterior arena.
//
// Built ENTIRELY from the SHIPPED engine + app APIs (no engine/ changes):
//   * TERRAIN  — the streamed heightfield (TerrainStreamer over worldTerrainConfig),
//                so the ground is the real procedural landscape. The terrain's own
//                height-blended material already paints SNOW on the high/steep
//                cliffs, which is exactly the snowy-mountain look we want.
//   * PAD      — a flat landing pad planted on the terrain via placeOnTerrain() at a
//                high-ground vantage (well above the sea level), with static
//                collision so it reads as solid ground.
//   * SHIP     — SpaceShip.glb (rigged_glb) set down ON the pad — the Salvari ship,
//                landed, the level set-piece.
//   * FORCES   — SalvariPrincess.glb (K'thara) + a couple of EnemyOccupationTrooper
//                GLBs, each anchored to the terrain surface via placeOnTerrain().
//   * OCEAN    — setWaterParams() drives water at a sea level well UNDER the pad, so
//                there is sea at the cliff base and the landing reads as a cliff-top.
//   * MOOD     — the engine's analytic sky + sun (cool, snowy) plus warm point-light
//                fills on the ship + the Salvari so the characters read against the
//                bright snow.
//
// Coords per docs/CONVENTIONS.md: +X right, +Y up, -Z forward; face a target with
// the verified facing law (yaw = atan2(-dirX,-dirZ)), shared with monster/rescue.
//
// Self-contained (CliffsArea owns its terrain ring, GLBs, pad + textures), low
// conflict with Level 1 and the other worlds — its own translation unit, reached
// only through the new `--world cliffs` dispatch in app/main.cpp.

#include "scene.h"
#include "terrain.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// One placed GLB actor (the ship or a Salvari): a loaded model kept alive for the
// area's lifetime, drawn over its own world transform (translate * yaw * scale).
// Mirrors the multi-primitive draw RescueVictim/MonsterSystem use (entity render
// mesh left invalid; the actor owns the draw of every primitive).
struct CliffsActor {
    std::unique_ptr<x3::asset::IAssetSource>  assets;
    std::unique_ptr<x3::asset::IModelLoader>  loader;
    x3::asset::Model                          model;
    std::vector<x3::asset::ModelDrawable>     drawables;
    bool   usingReal = false;          // false => a procedural fallback box
    x3::rhi::MeshHandle    fallbackMesh; // valid only when !usingReal
    x3::rhi::TextureHandle fallbackTex;
    float  pos[3]   = {0,0,0};         // world position (surface-anchored)
    float  yaw      = 0.0f;            // facing (radians)
    float  scale    = 1.0f;            // uniform model scale
    float  tint[4]  = {1,1,1,1};       // base color multiplier
    float  emissive[4] = {0,0,0,0};    // optional HDR glow {r,g,b,strength}

    // World transform = T(pos) * Ry(yaw) * S(scale), column-major.
    void worldMatrix(float out[16]) const;
    // Draw every primitive at worldMatrix * nodeTransform (or the fallback box).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;
    void destroy(x3::rhi::IRenderDevice& device);
};

// The cliffs finale area. Owns the streamed terrain, the landing pad, and the
// placed Salvari actors. Pure app-layer; talks only to the public engine API.
class CliffsArea {
public:
    // Bring up the area: streamer ring around the chosen vantage, the sky + ocean,
    // the pad on the terrain, and the Salvari ship + forces. `jobs` may be null
    // (then the streamer generates synchronously — used headless). `device` should
    // be a REAL device so the GLBs upload (a headless device mints fake handles and
    // the actors fall back to boxes, which is still valid for --smoketest).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, x3::jobs::IJobSystem* jobs);

    // Per-frame tick: advance the wave clock, stream terrain around the focus, and
    // push the ocean params. Call once per frame between physics->step() and render.
    // `focusX/focusZ` is the camera/player XZ used to drive terrain streaming.
    void update(Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics, float dt,
                float focusX, float focusZ);

    // Draw the streamed terrain (Scene) + the pad + every Salvari actor. Call
    // between beginFrame()/endFrame(). The streamer's tiles are Scene entities, so
    // scene.render() draws the ground; this then overlays the pad + the GLB actors.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                const Scene& scene) const;

    // Tear down GPU + physics + streamer resources. Safe once.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // ---- Vantage / framing helpers (the host poses the screenshot camera) -----
    // The pad center (world) the ship sits on — the natural look-at target.
    const float* padCenter() const { return m_padCenter; }
    // Sea level the ocean is driven at (pad sits well above this).
    float seaLevel() const { return m_seaLevel; }
    // A good screenshot eye + look-at: fills the eye[] + look-at via the pad.
    void suggestCamera(float eye[3], float& yaw, float& pitch) const;

    // Diagnostics (logging / the self-test).
    uint32_t residentTiles() const { return m_streamer.residentCount(); }
    uint32_t actorCount() const { return (uint32_t)m_actors.size(); }
    bool shipReal() const { return !m_actors.empty() && m_actors[0].usingReal; }

private:
    TerrainStreamer m_streamer;
    std::vector<CliffsActor> m_actors;   // [0] = ship, then K'thara + troopers

    // The landing pad (a flat disc/box planted on the terrain).
    x3::rhi::MeshHandle    m_padMesh;
    x3::rhi::TextureHandle m_padTex;
    x3::phys::BodyId       m_padBody;
    float m_padCenter[3] = {0,0,0};
    float m_padTopY      = 0.0f;         // walkable top surface of the pad
    // Unit XZ direction from the pad toward the nearby cliff DROP (the descent to
    // the sea) — the camera looks out along this so the ocean falls in frame.
    float m_dropDir[2]   = {-0.707f, -0.707f};

    float m_seaLevel = 0.0f;
    float m_waveTime = 0.0f;
    x3::rhi::IRenderDevice::WaterParams m_water{};

    // Load one GLB actor from rigged_glb (or fall back to a tinted box).
    CliffsActor loadActor(x3::rhi::IRenderDevice& device, std::string_view file,
                          const float pos[3], float yaw, float scale,
                          const float tint[4]);
};

// Headless self-test (--test-cliffs / used by --smoketest coverage). Builds the
// area on the shared headless device + a real Jolt world (no window/Vulkan) and
// asserts: the pad sits ABOVE the sea level, K'thara + the troopers are anchored
// ON the terrain surface (placeOnTerrain agrees with the height field), and the
// terrain ring is resident. Logs PASS/FAIL C#, returns true iff all pass.
bool runCliffsSelfTest();

} // namespace x3::game
