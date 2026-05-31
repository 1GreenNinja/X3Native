#pragma once
// Space environment (S1, Lane C) — deep-space backdrop + proxy planets + sun.
//
// SpaceEnv is the visual foundation of the Act-3 space engine. It owns three
// things, all rendered through the existing IRenderDevice mesh API (no new RHI
// surface — engine/rhi/* is untouched, exactly like app/sky_stars.*):
//
//   1. A STAR / NEBULA DOME — an inside-out sphere carrying a baked
//      equirectangular texture: a dark nebula colour gradient with a procedural
//      starfield sprinkled on top. Same dome+baked-texture trick the shipped
//      SkyStars uses (drawMeshEmissive on a far, camera-anchored sphere). This
//      is the "sky" of deep space.
//
//   2. PROXY PLANETS — addPlanet() builds one low-poly UV sphere (~a few
//      thousand tris) at a world position/radius with an albedo tint. A default
//      procedural checker/banded material gives the planet visible surface
//      detail now; real planet-texture packs drop in later via assetRoot()
//      without touching this interface. render() draws every registered planet.
//
//   3. A SUN — setSun() records a directional light direction + colour +
//      intensity and render() draws an additive, bright emissive sprite (a
//      camera-facing billboard quad) in that sky direction so the scene has a
//      recognizable star to orbit. The directional term is exposed so an
//      integrator can feed it to the device's lighting; the showcase relies on
//      the emissive planets + sprite for the screenshot gate.
//
// CROSS-LANE NOTE (Wave 1): S1 conceptually consumes S0's Proxy/Context types,
// but SpaceEnv is deliberately STANDALONE for Wave 1 — it owns its own planet
// list and takes NO dependency on app/space/space_layer.h (Lane A, in parallel).
// The integrator wires SpaceEnv's planets into S0's proxy registry at the
// Wave-2 merge. Keeping this standalone lets all four lanes build independently.
//
// Clean-room: no idTech / Doom / Quake source consulted; the dome + procedural
// starfield math derives from app/sky_stars.* + shaders/starfield.* (this repo).

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::space {

class SpaceEnv {
public:
    // Build the star/nebula dome mesh + bake its texture, and create the shared
    // unit-sphere planet mesh + default planet material, through the device.
    // Idempotent: a second init() WITHOUT a shutdown() in between is a no-op.
    void init(rhi::IRenderDevice&);

    // Draw the backdrop + the sun sprite + each registered planet for this
    // frame. `viewProj16` is the column-major view*proj (accepted for parity /
    // future shader path; the dome is camera-anchored implicitly). `timeSec`
    // drives slow per-frame animation (twinkle / sun pulse). VUID-safe with
    // zero planets and before any setSun() call.
    void render(rhi::IRenderDevice&, const rhi::FrameContext&,
                const float* viewProj16, float timeSec);

    // Register a proxy planet: a low-poly UV sphere at world `pos`, `radius`
    // metres, tinted by linear-RGB `albedo`. Returns a stable id (the planet's
    // index). Safe to call before OR after init() (the draw uses the shared
    // sphere mesh built in init()).
    uint32_t addPlanet(const float pos[3], float radius, const float albedo[3]);

    // Set the sun: `dir` is the direction TOWARD the sun (normalized
    // internally), `color` is linear RGB, `intensity` scales the sprite +
    // directional term. The sun sprite is drawn at a far distance along `dir`.
    void setSun(const float dir[3], const float color[3], float intensity);

    // Update the camera position so the dome + sun sprite stay anchored on the
    // eye (call BEFORE render() each frame). If never called the dome sits at
    // the origin — fine for the origin-orbit showcase.
    void setCamera(float ex, float ey, float ez);

    // Destroy all GPU resources. Idempotent (no-op without a prior init()).
    void shutdown(rhi::IRenderDevice&);

    // ---- Introspection (used by --test-spaceenv) --------------------------
    bool     initialized() const { return m_initialized; }
    uint32_t planetCount() const { return (uint32_t)m_planets.size(); }
    bool     sunSet()      const { return m_sunSet; }
    rhi::MeshHandle    domeMesh()    const { return m_domeMesh; }
    rhi::MeshHandle    sphereMesh()  const { return m_sphereMesh; }
    rhi::MeshHandle    spriteMesh()  const { return m_spriteMesh; }

private:
    struct Planet {
        float pos[3]    = { 0, 0, 0 };
        float radius    = 1.0f;
        float albedo[3] = { 1, 1, 1 };
    };

    rhi::MeshHandle    m_domeMesh{};       // inside-out nebula/star dome
    rhi::TextureHandle m_domeTex{};        // baked equirect nebula + stars
    rhi::MeshHandle    m_sphereMesh{};     // shared unit UV sphere (all planets)
    rhi::TextureHandle m_planetTex{};      // default procedural planet material
    rhi::MeshHandle    m_spriteMesh{};     // camera-facing sun billboard quad
    rhi::TextureHandle m_sunTex{};         // radial sun-glow sprite texture

    std::vector<Planet> m_planets;

    bool  m_initialized = false;
    bool  m_sunSet      = false;
    float m_sunDir[3]   = { 0.0f, 0.4f, 1.0f };   // toward the sun (normalized)
    float m_sunColor[3] = { 1.0f, 0.95f, 0.85f };
    float m_sunIntensity = 1.0f;

    float m_camX = 0.0f, m_camY = 0.0f, m_camZ = 0.0f;
};

} // namespace x3::space
