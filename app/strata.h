#pragma once
// STRATA — "THE DESCENT". The geological vertical zone between the facility base
// (Y~=0, the canonical building / elevator shaft bottom) and Club 1127 ("THE
// DEEP") at Y=-200. Game/slice content code only — engine/ stays pure.
//
// CLEAN-ROOM, original work, built ONLY from X3Native's own Scene / mesh_prims /
// IRenderDevice / IPhysicsWorld / TriggerSystem seams (the same seams
// club1127.* / act2_caves.* / env_art.* use). The LAYER NAMES / COLORS / DEPTH
// ORDER are the 9 earth-strata bands ALREADY designed for the souped-up elevator
// (ElevatorSystem::strata(), ported from Tim's OWN Babylon module
// Q3Engine/src/features/x3-elevator.js — Tim's IP). This module makes those bands
// LITERAL, reachable geology (blueprint X3_WORLD_BLUEPRINT.md §2.6: "the elevator
// strata are LITERAL / reachable ... the deep layers — Crystal Veins -> Magma ->
// Alien Substrate — are real explorable zones").
//
// TWO LAYERS OF CONTENT, both in this one module (Tim wants BOTH):
//
//   PHASE 1 — SCENIC LAYERED DESCENT. A real, layered geological SHAFT the
//   glass-bottom elevator looks OUT at as it drops. Each of the strata bands is
//   built as a ring of rocky shaft-wall geometry with the band's distinct base
//   color + (for the bottom three) emissive Crystal-Vein / Magma / Alien-Substrate
//   glow, plus per-band point lights for mood. The descent reads as a real
//   geological journey: normal earth/rock at the top -> glowing crystal veins ->
//   magma heat-glow -> alien substrate near the club.
//
//   PHASE 2 — EXPLORABLE CAVE SYSTEM. Branching OFFSHOOT tunnels off the main
//   shaft at the layer boundaries (the "tumbling tunnel with offshoots"), walkable
//   cave passages (real geometry + Jolt collision) leading to side pockets (secret
//   rooms / content hooks / the rescue-storyline space). An ALTERNATE ON-FOOT route
//   down: a climbable/walkable spiral of cave ledges + ramps THROUGH the offshoots,
//   so a player can descend the strata on foot, not only by the elevator.
//
// CONNECTION: building base (top, Y~=0) -> strata shaft (the elevator runs through
// it; its glass sees the layers) -> offshoots branch at the layer boundaries ->
// the bottom opens into Club 1127 / The Deep at Y=-200. Seams kept clean (the top
// ring meets Y=0, the bottom ring meets the club ceiling at Y=-200).
//
// Reaching this area:
//   (a) STANDALONE: `--world strata` (app/main.cpp) — walk/fly the descent +
//       offshoots (WASD / mouse / Space / LeftShift / F noclip); `--world strata
//       --screenshot <path>` captures a showcase vantage of the layered bands.
//   (b) THE LIVE DESCENT (canon): the souped-up elevator drops through this shaft;
//       its glass-bottom / observation wall sees the real strata geometry built here.

#include "scene.h"
#include "trigger.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// The strata zone spans the facility base (top) down to the Club 1127 floor. The
// shaft is centered on the elevator shaft XZ (the elevator descends through it).
// Top of the modeled shaft (meets the facility base / Y=0 ground). The 9 elevator
// strata bands above this (Sky&Concrete +200 .. Limestone) are the building's own
// floors; THIS module models the descent from the base down.
constexpr float kStrataTopY    =    0.0f;   // shaft top (meets facility base)
constexpr float kStrataClubY   = -200.0f;   // shaft bottom (Club 1127 ceiling line)

// Offshoot tunnels branch at the layer boundaries; this is how many are authored.
// One per deep, atmospheric boundary (Granite, Basalt, Obsidian, Crystal, Magma).
constexpr uint32_t kStrataOffshootCount = 5;

// Fresh, non-colliding trigger id range for the strata descent (well above the
// Act-2 caves 100..108 range act2_caves.* owns, the Act-1 10..50 ranges, and the
// Act-2 host 80..82 range). Contiguous so the host can switch over them in one place.
enum class StrataTrigger : uint32_t {
    DescentTop        = 200,  // entered the shaft from the facility base
    Offshoot0Granite  = 201,  // crossed into the Granite-boundary offshoot
    Offshoot1Basalt   = 202,  // crossed into the Basalt-boundary offshoot
    Offshoot2Obsidian = 203,  // crossed into the Obsidian-boundary offshoot
    Offshoot3Crystal  = 204,  // crossed into the Crystal-Veins offshoot (glow)
    Offshoot4Magma    = 205,  // crossed into the Magma-Zone offshoot (heat-glow)
    OnFootRouteEntry  = 206,  // stepped onto the on-foot ledge route at the top
    ClubArrival       = 207,  // reached the bottom -> Club 1127 / The Deep
};
constexpr uint32_t kStrataTrigBase  = 200;
constexpr uint32_t kStrataTrigCount = 8;

// One built depth band (a portion of the descent with its own material + mood).
// Derived from ElevatorSystem::strata() but only the bands BELOW the facility base
// (Y<=0) are modeled here (the descent proper). Cached so the HUD + self-test never
// re-derive the band a given Y falls in.
struct StrataBand {
    float        yMin = 0.0f, yMax = 0.0f;     // world Y band (yMin < yMax)
    const char*  name = "";                    // band label (matches elevator)
    float        rgb[3]  = {0,0,0};            // base rock color (linear)
    bool         glow    = false;              // emissive vein band (Crystal/Magma/Alien)?
    float        glowRgb[3] = {0,0,0};         // glow color when glow==true
    int          ringEntities = 0;             // shaft-wall ring entities authored
    int          lightCount   = 0;             // mood point lights placed in this band
    bool         hasOffshoot  = false;         // an offshoot tunnel branches at this band
};

// One walkable offshoot tunnel (Phase 2). A branch off the main shaft at a layer
// boundary leading to a side pocket. Authored with real geometry + Jolt collision.
struct StrataOffshoot {
    x3::phys::Vec3 mouth{};         // where the tunnel leaves the shaft (entry)
    x3::phys::Vec3 pocket{};        // the side-room center (the payoff)
    const char*    bandName = "";   // which band it branches at
    uint32_t       trigger  = 0;    // the StrataTrigger fired when entered
    int            entities = 0;    // geometry pieces authored
    bool           glow     = false;// glowing (crystal/magma) tunnel?
};

// The descent / cave content system. Build once; tick() each frame to animate the
// crystal/magma glow pulse + the per-band mood lights. All queries are cached so
// the host HUD + the headless self-test never re-derive anything.
class StrataWorld {
public:
    // Build the whole strata descent into `scene` / `physics`, uploading meshes via
    // `device`, between Y=0 (top) and Y=-200 (Club 1127). `shaftX`/`shaftZ` is the
    // elevator shaft center XZ (the descent is built around it; the elevator runs
    // through it). `triggers` is the host's shared TriggerSystem (offshoot/arrival
    // reachability is registered into it). `radius` is the shaft inner radius (m).
    // Call once; idempotent (a second call is a no-op returning the cached state).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               float shaftX, float shaftZ, float radius = 14.0f);

    // Advance one frame: pulse the Crystal-Vein / Magma / Alien-Substrate emissive
    // glow + flicker the magma mood lights, and re-push the band mood light set to
    // the device (so the glow breathes). Call every frame before beginFrame() in the
    // live path. `eye` is the camera/player position (used to bias which bands' lights
    // are pushed when the set would exceed the device cap — nearest bands win).
    void update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                const x3::phys::Vec3& eye);

    // Dispatch a fired StrataTrigger id forwarded from the host TriggerSystem.
    // Latches the per-offshoot "reached" beats + the club-arrival beat. Idempotent.
    void onTrigger(uint32_t triggerId);

    // ---- Geometry / connection accessors (host + the elevator descent) --------
    // The shaft center XZ + inner radius (the elevator descends here; its glass
    // observation wall looks OUT at the bands built around this column).
    // W-RIFT: a KEEP-OUT VOLUME. The rift level's landing + approach corridor are
    // BORED THROUGH this shaft's rock at Y ~= -78, and rock authored inside them would
    // poke through the corridor's lining (and, worse, block the walk). Any band slab,
    // boulder or ledge whose center lands inside this world AABB is simply not built —
    // which is what "we bored a tunnel through it" means. Call BEFORE build(); unset by
    // default, so `--world strata` and --test-strata are untouched.
    void setKeepOut(const x3::phys::Vec3& mn, const x3::phys::Vec3& mx) {
        m_koMin = mn; m_koMax = mx; m_koOn = true;
    }
    bool keptOut(float x, float y, float z) const {
        return m_koOn && x >= m_koMin.x && x <= m_koMax.x && y >= m_koMin.y &&
               y <= m_koMax.y && z >= m_koMin.z && z <= m_koMax.z;
    }

    float shaftX() const { return m_shaftX; }
    float shaftZ() const { return m_shaftZ; }
    float radius() const { return m_radius; }

    // The band that contains world-Y `y` (the elevator's geo-survey OLED + the HUD
    // read this so the in-cab strata display matches the REAL geometry the glass
    // sees). Returns the nearest band if `y` is just outside the modeled range.
    const StrataBand& bandAtY(float y) const;
    const std::vector<StrataBand>& bands() const { return m_bands; }

    // The on-foot descent route: an ordered list of ledge/ramp waypoints from the
    // top (Y~=0) to the club arrival (Y=-200) through the offshoots. A player can
    // walk/climb this instead of riding the elevator (every step is collidable).
    const std::vector<x3::phys::Vec3>& onFootRoute() const { return m_route; }

    // The offshoot tunnels (Phase 2).
    const std::vector<StrataOffshoot>& offshoots() const { return m_offshoots; }

    // Player spawn for `--world strata` (feet) — at the TOP of the descent on the
    // on-foot route entry ledge, facing down into the shaft.
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // A good fixed showcase camera (x,y,z,yaw,pitch) framing the layered bands +
    // the glowing crystal/magma depths from an elevated vantage in the shaft.
    void showcaseCamera(float out[5]) const;

    // The per-band MOOD point lights the host applies via setPointLights (the deep
    // glowing bands carry crystal/magma/alien lights; update() breathes them).
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // ---- Story beats latched by onTrigger() (host HUD + the self-test) --------
    bool descentEntered() const { return m_descentEntered; }
    bool offshootReached(uint32_t i) const {
        return (i < m_offshootReached.size()) && m_offshootReached[i];
    }
    bool clubReached() const { return m_clubReached; }

    // Reachability (the self-test proof): true iff the link's trigger is registered
    // + enabled + spatially between the shaft and the destination so a player can
    // cross it. Mirrors act2_caves.transitionReachable().
    bool offshootReachable(const TriggerSystem& triggers, uint32_t i) const;
    bool allOffshootsReachable(const TriggerSystem& triggers) const;
    // True iff the on-foot route is CONTINUOUS (each waypoint within a max step of
    // the next, top reaches Y~=0, bottom reaches the club Y=-200) AND the club
    // arrival trigger sits at the bottom — i.e. a player can descend on foot + the
    // shaft connects to the club.
    bool onFootRouteContinuous() const;
    bool clubConnected(const TriggerSystem& triggers) const;

    // Census the self-test asserts against.
    struct Stats {
        int  entities      = 0;   // total Scene entities authored
        int  shaftRings    = 0;   // shaft-wall ring entities (the scenic descent)
        int  bandCount     = 0;   // modeled depth bands
        int  glowBands     = 0;   // emissive bands (Crystal/Magma/Alien) — target 3
        int  offshootCount = 0;   // walkable offshoot tunnels (Phase 2)
        int  routeWaypoints= 0;   // on-foot route ledge/ramp waypoints
        int  moodLights    = 0;   // per-band mood point lights
        float topY = 0.0f, bottomY = 0.0f;   // modeled extent (asserted ~0 .. -200)
    };
    const Stats& stats() const { return m_stats; }
    bool built() const { return m_built; }

private:
    // A solid static rock box (render mesh + Jolt collision + Scene entity), tinted,
    // optionally emissive (crystal/magma/alien glow). Center + half extents in world
    // meters. `collide=false` for thin decorative inlays. Returns the entity id.
    uint32_t addRock(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics,
                     float cx, float cy, float cz, float hx, float hy, float hz,
                     const float color[4], const float emissive[4], bool collide,
                     float uvScale = 0.25f);

    // Build the scenic shaft-wall ring for one band (Phase 1): a ring of canted rock
    // slabs around the shaft column between [yMin,yMax], tinted/glowing per the band.
    void buildBandRing(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, StrataBand& band);

    // Build one offshoot tunnel + its side pocket (Phase 2) at a band boundary.
    void buildOffshoot(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                       StrataOffshoot& off, uint32_t triggerId);

    bool   m_built = false;
    Stats  m_stats{};
    float  m_shaftX = 0.0f, m_shaftZ = 0.0f, m_radius = 14.0f;
    bool   m_koOn = false;                 // W-RIFT keep-out (the bored corridor)
    x3::phys::Vec3 m_koMin{}, m_koMax{};

    std::vector<StrataBand>     m_bands;        // modeled depth bands (top->bottom)
    std::vector<StrataOffshoot> m_offshoots;    // Phase-2 offshoot tunnels
    std::vector<x3::phys::Vec3> m_route;        // on-foot descent waypoints (top->bottom)
    std::vector<x3::rhi::PointLight> m_lights;  // per-band mood lights
    x3::phys::Vec3 m_spawn{};

    // Animated emissive entities (the glowing crystal/magma/alien rock the pulse
    // breathes each frame) + their authored base emissive strength (so update()
    // modulates around it).
    std::vector<uint32_t> m_glowEnts;
    std::vector<float>    m_glowBaseStrength;
    // Indices into m_lights of the magma mood lights (flickered each frame).
    std::vector<size_t>   m_magmaLightIdx;
    float m_time = 0.0f;

    // Story beat latches.
    bool m_descentEntered = false;
    bool m_clubReached    = false;
    std::vector<bool> m_offshootReached;
};

// Headless self-test (--test-strata). Builds the strata descent on a
// HeadlessRenderDevice + Jolt world and asserts:
//   * the descent spans Y~=0 (top, meets the facility base) down to Y=-200 (the
//     Club 1127 ceiling line) — the building<->strata<->club connection;
//   * the scenic bands are built with the elevator's LAYER NAMES + the 3 deep bands
//     (Crystal Veins / Magma / Alien Substrate) glow (emissive bands == 3);
//   * bandAtY() returns the correct band across the descent (so the elevator's
//     in-cab strata display matches the real geometry the glass sees);
//   * the Phase-2 offshoot tunnels are all REACHABLE via their triggers (each link
//     enabled + spatially between the shaft and the pocket);
//   * the on-foot route is CONTINUOUS top->bottom (a player can descend on foot);
//   * the club-arrival trigger sits at the bottom (the shaft opens into The Deep);
//   * a few ticks are leak-clean (the glow pulse + mood lights animate without leaks).
// Prints "strata: X/Y passed"; returns true iff all pass. No window / no Vulkan.
bool runStrataSelfTest();

} // namespace x3::game
