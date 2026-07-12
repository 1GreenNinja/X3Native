#pragma once
// STREET LIGHT — real street lamps for the one world (Tim: "Street lighting..
// with light cones.. REAL looking...").
//
// Every lamp is a POST + ARM + LUMINAIRE HEAD (emissive), a fake-volumetric
// ADDITIVE LIGHT CONE under the head (the glass pass's additive glow mode —
// GlassMaterial::additive — with the axial falloff baked into a gradient
// texture and the soft silhouette from the view-angle rim fade), an emissive
// GROUND-POOL disc on the asphalt (so the pool reads even when the pooled
// light loses the frame budget), and a real pooled PointLight at the head.
//
// COLOR STORY: warm sodium in the old districts (Scrapyard / Industrial),
// cool LED in the New District / on the Spire approach, warm-white on the
// facility apron, and a bright metal-halide WORK LIGHT rig at the city dock
// crate zone. Deterministic per-lamp variance (LCG on the lamp index): ~8%
// dead, ~5% flickering (irregular 8-13 Hz bursts, dt-scaled), and a slight
// per-lamp intensity/warmth spread so no two lamps are clones.
//
// OWNERSHIP (region safety): city lamps build INSIDE the `city` region
// realize via the host's region-build hook — every entity/mesh/texture joins
// the region ledger (shared cone/disc meshes are safe: the ledger dedups
// handles, so eviction destroys each exactly once — the world_cars doctrine).
// onCityTeardown() abandons the city lamp records BEFORE any slot release.
// Apron + Spire-approach lamps are HOST-owned (built once, kNoRoom).
//
// BUDGET: the host merges selectLights() — the nearest-K lit lamps — into the
// frame's pooled-light feed only when the streamed exterior is in frame,
// AFTER the facility/strata/club obligations (see app_run.cpp's merge block).

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

class StreetLights {
public:
    enum class Zone : uint32_t {
        Scrapyard = 0,   // warm sodium
        NewDistrict,     // cool LED
        Industrial,      // warm sodium
        Approach,        // Spire approach road (cool LED)
        Apron,           // facility apron by the breach (warm white)
        Dock,            // the dock crate-zone WORK LIGHT (metal halide)
        Count
    };
    enum class State : uint8_t { Lit = 0, Dead, Flicker };

    struct Lamp {
        float head[3]  = { 0, 0, 0 };     // luminaire world position
        float color[3] = { 1, 1, 1 };     // per-lamp tint (post variance)
        float range     = 16.0f;          // pooled-light falloff radius (m)
        float intensity = 4.5f;           // pooled-light intensity (color premul)
        Zone  zone      = Zone::Scrapyard;
        State state     = State::Lit;
        bool  workLight = false;          // the dock rig (never dead/flickering)
        bool  cityOwned = false;          // true = region-ledger lifetime
        float level     = 1.0f;           // live flicker level (1 = steady)
        // Flicker state machine (deterministic xorshift stream, dt-scaled).
        float    t = 0.0f, next = 0.0f, period = 0.1f, phase = 0.0f;
        bool     burst = false, on = true;
        uint32_t rng = 1;
        // Scene entities the flicker animates (generation-checked: a region
        // evict recycles slots, so stale handles must die, not scribble).
        SceneHandle coneEnt, headEnt, discEnt;
    };

    // Region-owned CITY lamps: the district street grids + the dock work
    // light. MUST be called inside the `city` region-build hook (the scene's
    // entity-capture window) so everything joins the region ledger.
    void buildCityLamps(Scene& scene, x3::rhi::IRenderDevice& device);

    // Region teardown: abandon the city lamp records (entities die with the
    // ledger; the shared meshes/textures are the ledger's to destroy).
    void onCityTeardown();

    // HOST-owned lamps: 3 on the facility apron near the breach (apronY =
    // apron walk level, breachX/apronZ = just outside the breach face) + the
    // Spire approach road rows. Built once at canon boot, kNoRoom.
    void buildHostLamps(Scene& scene, x3::rhi::IRenderDevice& device,
                        float apronY, float breachX, float apronZ);

    // Per-frame: advance the flicker machines (dt-scaled, irregular 8-13 Hz
    // bursts) and write cone/head/disc emissive levels into the scene.
    void update(float dt, Scene& scene);

    // Append the nearest-K LIT lamps (dead excluded, flicker scaled by its
    // live level) to `out` as pooled PointLights. Returns how many appended.
    uint32_t selectLights(float ex, float ey, float ez,
                          std::vector<x3::rhi::PointLight>& out, uint32_t k) const;

    // ---- Queries (host HUD + --test-city) ----
    bool     cityBuilt() const { return m_cityBuilt; }
    bool     hostBuilt() const { return m_hostBuilt; }
    uint32_t lampCount() const { return (uint32_t)m_lamps.size(); }
    uint32_t lampCount(Zone z) const;
    uint32_t deadCount() const;
    uint32_t flickerCount() const;
    bool     hasDockWorkLight() const;
    const std::vector<Lamp>& lamps() const { return m_lamps; }

private:
    // Shared render kit per OWNER group (region ledger vs host lifetime) —
    // a city evict must never destroy a mesh the host lamps still draw.
    struct Kit {
        x3::rhi::MeshHandle    unitCube;   // post/arm/head boxes (scaled)
        // Light cones are WORLD-BAKED per zone (exact normals; a non-uniform
        // scale through the plain-mat3 normal transform skews normals +Y and
        // kills the rim fade). Lazily created for the zones actually placed,
        // so no unreferenced mesh ever escapes the region ledger.
        x3::rhi::MeshHandle    cone[(uint32_t)Zone::Count];
        x3::rhi::MeshHandle    disc;       // unit ground-pool disc (+Y, scaled in XZ)
        x3::rhi::TextureHandle coneGrad;   // axial falloff bake
        x3::rhi::TextureHandle discGrad;   // radial falloff bake
    };
    Kit  makeKit(x3::rhi::IRenderDevice& device) const;
    void addLamp(Scene& scene, Kit& kit, x3::rhi::IRenderDevice& device,
                 float x, float z, float groundY,
                 float dirX, float dirZ, Zone zone, bool cityOwned, bool workLight);
    // Lamps along a street segment at ~`spacing`, alternating road sides
    // (`sideOffset` off the centerline, terrain-grounded), heads facing the road.
    void lampRow(Scene& scene, Kit& kit, x3::rhi::IRenderDevice& device,
                 float x0, float z0, float x1, float z1,
                 float spacing, float sideOffset, Zone zone, bool cityOwned);
    void logBuild(const char* who, uint32_t first) const;

    std::vector<Lamp> m_lamps;
    bool m_cityBuilt = false;
    bool m_hostBuilt = false;
};

} // namespace x3::game
