#pragma once
// RIFT DEPTHS — the way IN to the Rift Hub (W-RIFT, ONE WORLD).
//
// The hub used to be reachable only with `--world rifthub`. It is now a REGION of
// the canon world, buried under the detention level, and this module is the thing
// that makes it a PLACE you can walk to:
//
//   the elevator's RIFT stop (a real floor on the cab, code 4790)
//        -> THE LANDING      a bored station cut into the strata bore around the
//                            shaft: a steel-and-concrete deck with the cab WELL
//                            open through it (the 1127 club descent still drops
//                            straight through), lit by two failing overheads.
//        -> THE APPROACH     a long industrial corridor west out of the landing,
//                            through the rock, then a HARD LEFT. You hear the
//                            rifts before you see them — the hum bleeds up the
//                            corridor — and the blue glow spills around the bend
//                            before the space opens up.
//        -> THE HUB          the corridor seals into the doorway cut in the hub's
//                            -Z wall (Rifthub::Desc::doorway), and you step out
//                            between two gates.
//
// CLEAN-ROOM, original work: built only from X3Native's own Scene / mesh_prims /
// SurfaceLibrary / IRenderDevice / IPhysicsWorld seams (the same seams rifthub.cpp,
// strata.cpp and club1127.cpp use). No id Tech / RBDOOM / Quake source was
// consulted.
//
// SEAM LAW (docs/LEVEL_GEOMETRY.md + the x3-level-authoring doctrine): every span
// below is authored from explicit edge coordinates, walls butt on those edges (no
// coplanar overlap, no gap), the floor is continuous from the landing deck to the
// hub threshold, and every shell piece collides. selfCheck() re-derives the seams
// from the authored spans and is asserted by --test-rifthub.

#include "door.h"     // side-room slider doors + the card readers that watch them
#include "scene.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// PVS room tag for EVERYTHING in sub-level R1 (the hub + the landing + the approach).
// The canon per-room flood-fill cull keys off Entity::roomId; a hub 78 m under the
// facility must not be submitted while the player is walking the detention level, and
// vice versa. The host pushes this id into the visible set only while the eye is in
// the rift region — the same trick the streamed exterior uses (world_stream.h).
constexpr uint32_t kRiftRoom = 0xFFFFFFFDu;

class RiftDepths {
public:
    struct Desc {
        // The elevator shaft (XZ) and the rift level's FLOOR Y. The cab's rift stop
        // parks its top flush with this floor.
        x3::phys::Vec3 shaft{ 0.0f, -78.0f, 0.0f };
        float shaftWellHalf = 1.62f;   // the open cab well (cab half is 1.40)
        float landingHalf   = 6.0f;    // landing deck half-extent (square)
        float landingH      = 4.2f;    // landing ceiling height
        // The hub's doorway (world center of the opening in the hub's -Z wall,
        // at the hub floor Y) + its size. Rifthub::doorCenter() feeds this.
        x3::phys::Vec3 hubDoor{ 0.0f, -78.0f, 0.0f };
        float doorHalfW = 1.7f;
        float doorH     = 3.4f;
        // Corridor cross-section.
        float hallHalfW = 1.7f;    // must be >= doorHalfW so the mouth seals
        float hallH     = 3.4f;
        // SIDE ROOMS (owner 2026-08-30: "rooms off of it" + "card readers").
        // When a DoorSystem is provided, the approach's leg A grows two rooms
        // off its SOUTH wall — the OPS ANNEX (west) and the STORES BAY (east),
        // both behind keycard-locked slider doors with a CARD READER beside
        // each (red LED until the door unlocks; syncReaders flips them green).
        // nullptr = corridor only, byte-identical to the pre-room build.
        DoorSystem* doors = nullptr;
    };

    // A card reader serving one side-room door: the wall unit's LED entity +
    // the DoorSystem index of the door it watches. syncReaders() drives the
    // LED from the door's LIVE lock state, so the light can never lie.
    struct CardReader {
        uint32_t doorIdx = 0;      // DoorSystem::at() index
        uint32_t ledEnt  = 0;      // the LED strip entity (emissive swapped)
        x3::phys::Vec3 pos{};      // world position (HUD prompts / tests)
    };
    const std::vector<CardReader>& readers() const { return m_readers; }
    // Per-frame: LED red while its door is locked, green once unlocked.
    void syncReaders(Scene& scene, const DoorSystem& doors);

    // Author the landing + the L-shaped approach. The corridor leaves the landing
    // through its -X wall, runs west to the bend at the hub door's X, then turns and
    // runs along Z into the hub's -Z wall opening. Requires hubDoor to be WEST of the
    // shaft and offset along Z (the canon placement) — validated by selfCheck().
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Desc& desc);
    void shutdown(x3::rhi::IRenderDevice& device);

    bool built() const { return m_built; }
    const Desc& desc() const { return m_desc; }

    // Where the player stands when the elevator doors open at the RIFT stop (on the
    // landing deck, clear of the cab well, facing the corridor).
    x3::phys::Vec3 landingSpawn() const { return m_landingSpawn; }
    // The walk: landing deck -> corridor mouth -> the bend -> the hub threshold.
    // (The self-test walks these and asserts the shell is sealed around them.)
    const std::vector<x3::phys::Vec3>& route() const { return m_route; }
    // AABB of the whole depths region (landing + corridor), inflated a little. The
    // host uses it to know when the eye is IN the rift zone (atmosphere + the hub's
    // lights + the hub FX), so nothing about the facility above changes.
    void zoneAabb(x3::phys::Vec3& outMin, x3::phys::Vec3& outMax) const {
        outMin = m_zoneMin; outMax = m_zoneMax;
    }
    // The corridor's point lights (failing strip fixtures + the blue spill at the
    // bend). Appended by the host to the hub's own rig while in the zone.
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }
    // Per-frame: the bend's blue spill BREATHES with the hub's core hum (the tease —
    // you see it before you see the gates), and the two dying overheads stutter.
    void tick(float dt);

    uint32_t entityCount() const { return (uint32_t)m_ents.size(); }

    // Geometric self-check (the lint gate for a procedural region — the canon lint
    // works on CanonFloor room data, which this is not):
    //   * the corridor's mouth seals onto the hub's door opening (same X span, and
    //     the corridor is at least as wide as the door);
    //   * floor continuity: every waypoint on route() stands over an authored floor
    //     slab, and consecutive waypoints are joined by authored floor;
    //   * the shell is closed: landing walls + corridor walls + ceilings exist on
    //     every span, and the only holes are the two doorways + the cab well.
    // Returns "" when clean, else the first violation.
    std::string selfCheck() const;

private:
    struct Slab { float x0, x1, z0, z1, y0, y1; bool collide; };   // authored AABB

    void box(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
             float x0, float x1, float y0, float y1, float z0, float z1,
             const SurfaceSet* surf, const float tint[3], float uvScale,
             bool collide, float emissive = 0.0f);

    bool  m_built = false;
    Desc  m_desc{};
    x3::phys::Vec3 m_landingSpawn{};
    x3::phys::Vec3 m_zoneMin{}, m_zoneMax{};
    std::vector<x3::phys::Vec3> m_route;
    std::vector<uint32_t>       m_ents;
    std::vector<x3::rhi::MeshHandle> m_meshes;
    std::vector<x3::rhi::PointLight> m_lights;
    std::vector<float>          m_lightBase;   // authored intensities (the flicker restores them)
    std::vector<Slab>           m_floors;      // authored floor spans (selfCheck reads these)
    std::vector<CardReader>     m_readers;     // side-room card readers (LED + door idx)
    bool                        m_roomsBuilt = false;   // side rooms authored (selfCheck extends)
    SurfaceLibrary m_surf;
    float m_t = 0.0f;
};

} // namespace x3::game
