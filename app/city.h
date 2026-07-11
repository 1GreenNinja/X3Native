#pragma once
// EFLZ Act-2 open world — the CITY / INDUSTRIAL metropolis + road grid + freeway
// tunnels (design L16-18: Ruined Metropolis / Downtown). Game/slice content only.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN Scene / terrain /
// mesh_prims systems + the engine interfaces + the EFLZ design (Tim's own Q3Engine
// x3-city-roads.js / x3-freeway-tunnels.js as the content reference). NO RBDOOM /
// id Tech / Doom / Quake engine source consulted.
//
// SCOPE (open-world lane): the city districts + the road network + the 4 freeway
// tunnels that bore from the city toward the surrounding mountain ranges — graybox
// on the terrain surface (PURE placeOnTerrain). Disjoint from world_regions.* (the
// mountains/outposts) and the act2 level lanes. Mirrors act2_world.*'s authoring
// pattern (build / queries / a headless --test-city). Coordinates from the blueprint
// gazetteer §1: Scrapyard City (-600,500), New District (200,500), E-W freeway Z=500.

#include "scene.h"
#include "terrain.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

class HackableRegistry;   // hackables.h — the WD2 hacking layer (optional, populated by the district)
class CrowdSystem;        // crowd.h — civilian crowd on the main drag (optional)
class EnvArtSystem;       // env_art.h — persistent GLB-instancing host for real building facades (optional)

// ===========================================================================
// THE NEON DISTRICT (feat/city-aaa Milestone 1) — the art-directed, walkable/drivable
// CP2077-vibe city block at the canon Scrapyard City center (-600, 500). A richer build
// than the legacy graybox City below: a wet reflective street grid + sidewalks, varied
// building massing with emissive window grids / lit shopfronts / neon signage, street
// lamps that pool light, parked vehicles, and the LNG tank landmark at (-500, 525). When
// a HackableRegistry is passed it also SCATTERS the Watch-Dogs-2 hackable objects
// (cameras, junction boxes, ATMs, vehicles, traffic signals) + their holo MARKER
// entities; when a CrowdSystem is passed it configures + builds a civilian crowd on the
// drag. Additive to City (below) — the streamed/city host chooses which to build.
struct NeonDistrictStats {
    uint32_t buildings    = 0;
    uint32_t streetlights = 0;
    uint32_t vehicles     = 0;
    uint32_t signs        = 0;
    uint32_t hackables    = 0;   // objects registered into the HackableRegistry (if any)
    uint32_t propClutter  = 0;   // scattered street-clutter prop instances
    float    centerX = -600.0f, centerZ = 500.0f;
    float    groundY = 0.0f;     // terrain height at the district center (player spawn feet)
};

// Build the neon district onto `scene` at (cx, cz). Static geometry (no collision this
// pass beyond the terrain it stands on) + optional hackables/crowd. Deterministic layout.
// `facades` (optional): a persistent EnvArtSystem, already mounted on the converted-
// GLB dir, that supplies REAL cyberpunk building facade meshes. When present + loaded,
// each building's procedural box BODY is replaced by an instanced GLB facade (the neon
// signage, lit shopfronts, lamps, hackables, crowd + LNG tank are unchanged). When null
// / unmounted / a GLB fails, the procedural box body is kept (headless tests, no-assets).
NeonDistrictStats buildNeonDistrict(Scene& scene, x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& physics,
                                    HackableRegistry* hax, CrowdSystem* crowd,
                                    float cx = -600.0f, float cz = 500.0f,
                                    EnvArtSystem* facades = nullptr);

// City districts (the metropolis).
enum class CityZone : uint32_t {
    ScrapyardCity = 0,   // salvage/scrap district (ramshackle)
    NewDistrict,         // newer urban grid
    IndustrialZone,      // factories / refinery
    Count
};
constexpr uint32_t kCityZoneCount = (uint32_t)CityZone::Count;

// The 4 freeway tunnels boring from the city toward the 4 mountain ranges.
constexpr uint32_t kFreewayTunnelCount = 4;

struct CityZonePlan {
    CityZone    zone   = CityZone::ScrapyardCity;
    const char* name   = "";
    float       cx     = 0.0f, cz = 0.0f;   // district center
    float       radius = 0.0f;              // footprint radius (m)
    uint32_t    buildingCount = 0;          // graybox buildings placed
};

struct FreewayTunnelPlan {
    const char* name = "";
    float mouthX = 0.0f, mouthZ = 0.0f;     // tunnel mouth (city side)
    float dirX   = 0.0f, dirZ = 0.0f;       // unit heading toward the range
    float length = 0.0f;                    // bore length (m, graybox segment)
};

// City host. Build once after the terrain world exists; graybox props are plain
// Scene entities (drawn by the host's scene.render()).
class City {
public:
    // `sharedSurf` (optional): a caller-lifetime SurfaceLibrary (the WorldStreamer's
    // shared cache) so repeat realizes don't re-decode the PBR sets; when null the
    // builder uses a local library (self-tests / one-shot hosts).
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               SurfaceLibrary* sharedSurf = nullptr);

    // ---- Queries (host HUD + self-test) ----
    bool built() const { return m_built; }
    const CityZonePlan&      zone(CityZone z) const { return m_zones[(uint32_t)z]; }
    const FreewayTunnelPlan& tunnel(uint32_t i) const { return m_tunnels[i]; }
    uint32_t tunnelCount() const { return kFreewayTunnelCount; }
    uint32_t roadSegmentCount() const { return m_roadSegments; }
    uint32_t propCount() const { return (uint32_t)m_props.size(); }  // total props
    // W8-3 blockout-plus queries:
    uint32_t buildingCount() const { return m_buildings; }       // all districts
    uint32_t trafficLightCount() const { return m_trafficLights; }
    uint32_t neonSignCount() const { return m_neonSigns; }
    uint32_t windowBandCount() const { return m_windowBands; }

private:
    bool m_built = false;
    CityZonePlan      m_zones[kCityZoneCount];
    FreewayTunnelPlan m_tunnels[kFreewayTunnelCount];
    uint32_t          m_roadSegments = 0;
    uint32_t          m_buildings = 0;      // W8-3: total buildings (all districts)
    uint32_t          m_trafficLights = 0;  // W8-3: signalized intersections
    uint32_t          m_neonSigns = 0;      // W8-3: emissive shop signage strips
    uint32_t          m_windowBands = 0;    // W8-3: dark-glass window strips
    std::vector<uint32_t> m_props;   // Scene entity ids
};

// Headless self-test (--test-city). Builds the city on a HeadlessDevice + Jolt world
// and asserts: 3 districts present (named, with buildings on the surface); a road grid
// (roadSegmentCount > 0); exactly 4 freeway tunnels heading outward toward the ranges
// (each with a nonzero length + a unit heading); total props > 0. Prints
// "city: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runCitySelfTest();

} // namespace x3::game
