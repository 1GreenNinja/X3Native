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

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

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
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // ---- Queries (host HUD + self-test) ----
    bool built() const { return m_built; }
    const CityZonePlan&      zone(CityZone z) const { return m_zones[(uint32_t)z]; }
    const FreewayTunnelPlan& tunnel(uint32_t i) const { return m_tunnels[i]; }
    uint32_t tunnelCount() const { return kFreewayTunnelCount; }
    uint32_t roadSegmentCount() const { return m_roadSegments; }
    uint32_t propCount() const { return (uint32_t)m_props.size(); }  // total graybox props

private:
    bool m_built = false;
    CityZonePlan      m_zones[kCityZoneCount];
    FreewayTunnelPlan m_tunnels[kFreewayTunnelCount];
    uint32_t          m_roadSegments = 0;
    std::vector<uint32_t> m_props;   // Scene entity ids
};

// Headless self-test (--test-city). Builds the city on a HeadlessDevice + Jolt world
// and asserts: 3 districts present (named, with buildings on the surface); a road grid
// (roadSegmentCount > 0); exactly 4 freeway tunnels heading outward toward the ranges
// (each with a nonzero length + a unit heading); total props > 0. Prints
// "city: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runCitySelfTest();

} // namespace x3::game
