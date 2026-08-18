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
#include <functional>
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
        // ---- CONTENT WIRING (r_citylights 1): GLOW-ONLY sources ------------
        // No post/arm/head/cone/disc geometry — city.cpp already draws the
        // emissive window bands and neon strips. These are the POOLED LIGHTS
        // those emissive surfaces never had: a night city where the windows
        // and the signage actually throw light on the street. They ride the
        // same nearest-K selection and the same region lifetime as the lamps.
        Window,          // warm interior spill out of a lit window band
        Sign,            // neon shop signage wash
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
        bool  glowOnly  = false;          // window/sign wash: light only, no geometry
        float level     = 1.0f;           // live flicker level (1 = steady)
        // WD2 GRID CUT (junction-box hack): a blackout timer. While > 0 the
        // lamp is forced Dead; expiry strikes it back to its pre-cut state.
        float deadUntil = 0.0f;
        State preState  = State::Lit;
        // Ground-pool emissive multiplier (district lamps ride it: the island
        // TERRAIN ignores pooled point lights — A/B proven at 8x intensity,
        // black ground either way — so the disc must carry the night read).
        float discMul   = 1.0f;
        // Per-lamp emissive scale = this lamp's pooled intensity over its zone's
        // nominal. Applied to the cone/head/pool/source glow so a lamp's VISUAL
        // brightness tracks the light it actually casts (a mismatch between the
        // two is half of what read as "fake" in the parking-lot shots).
        float emisMul   = 1.0f;
        // Flicker state machine (deterministic xorshift stream, dt-scaled).
        float    t = 0.0f, next = 0.0f, period = 0.1f, phase = 0.0f;
        bool     burst = false, on = true;
        uint32_t rng = 1;
        // Scene entities the flicker animates (generation-checked: a region
        // evict recycles slots, so stale handles must die, not scribble).
        SceneHandle coneEnt, headEnt, discEnt, glowEnt;
    };

    // Region-owned CITY lamps: the district street grids + the dock work
    // light. MUST be called inside the `city` region-build hook (the scene's
    // entity-capture window) so everything joins the region ledger.
    //
    // CONTENT WIRING (lane inspx/content-wiring) — `dense`:
    //   false (r_citylights 0, the default): the ORIGINAL nine rows at the
    //     original 30-34 m spacing, ~56 lamps. Byte-for-byte the pre-lane city.
    //   true  (r_citylights 1): every street city.cpp actually authors gets a
    //     lamp row at realistic urban spacing (16 m staggered => ~32 m per
    //     kerbside), plus the connector freeway and the coast spur. ~240 lamps.
    //     The BL predecessor's city was light-starved because Babylon could not
    //     afford the lights; with r_clusterlights 1 that constraint is gone.
    void buildCityLamps(Scene& scene, x3::rhi::IRenderDevice& device, bool dense = false);

    // GLOW-ONLY city lights (r_citylights 1): the pooled lights behind the
    // emissive window bands + neon signs city.cpp already draws. Fed in as
    // authored world positions so the roster stays in city.cpp (one source of
    // truth) instead of being duplicated here. Call inside the same region
    // capture window; they are cityOwned and die with onCityTeardown().
    struct Glow {
        float pos[3]   = { 0, 0, 0 };
        float color[3] = { 1, 1, 1 };
        float range     = 9.0f;
        float intensity = 2.0f;
        bool  sign      = false;   // Zone::Sign vs Zone::Window
    };
    void adoptCityGlows(const std::vector<Glow>& glows);

    // Region teardown: abandon the city lamp records (entities die with the
    // ledger; the shared meshes/textures are the ledger's to destroy).
    void onCityTeardown();

    // HOST-owned lamps: 3 on the facility apron near the breach (apronY =
    // apron walk level, breachX/apronZ = just outside the breach face) + the
    // Spire approach road rows. Built once at canon boot, kNoRoom.
    void buildHostLamps(Scene& scene, x3::rhi::IRenderDevice& device,
                        float apronY, float breachX, float apronZ);

    // DISTRICT lamps (echotropolis metropolis): arbitrary host-supplied rows —
    // rows[i] = {x0, z0, x1, z1, y, spacing}. Warm pools so the pack districts
    // read at night (their HDRP kits carry only sparse baked neon).
    void buildDistrictLamps(Scene& scene, x3::rhi::IRenderDevice& device,
                            const float (*rows)[6], uint32_t nRows);

    // Optional terrain query: when set, each lamp's GROUND-POOL disc seats on
    // the LOCAL terrain instead of the row's flat y — on undulating ground
    // (the crown drag) a row-seated disc is buried and the pool never reads.
    // Set BEFORE the build calls.
    using GroundFn = std::function<float(float x, float z)>;
    void setGroundQuery(GroundFn fn) { m_ground = std::move(fn); }

    // Per-frame: advance the flicker machines (dt-scaled, irregular 8-13 Hz
    // bursts) and write cone/head/disc emissive levels into the scene.
    void update(float dt, Scene& scene);

    // WD2 GRID CUT: force every lamp within `radius` of (x,z) Dead for
    // `seconds` (emissives zeroed now; update() strikes them back after the
    // timer). The dock work light is exempt. Returns lamps cut.
    uint32_t killNear(Scene& scene, float x, float z, float radius, float seconds);

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
        // SOURCE GLOW: a small unit sphere hung at the luminaire aperture and
        // driven above the bloom threshold, so the fixture BLOOMS like a real
        // light in a dusk frame instead of being a flat emissive housing box.
        x3::rhi::MeshHandle    glowSphere;
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
    GroundFn m_ground;              // optional local-terrain seat for pools
    bool m_cityBuilt = false;
    bool m_hostBuilt = false;
};

} // namespace x3::game
