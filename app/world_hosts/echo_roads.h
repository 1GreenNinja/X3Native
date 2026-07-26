#pragma once
// ECHO ROADS — the procedural road network for Echotropolis (Tim's order:
// "Curving freeway aerial structures... interchanges... streets that flow
// around the harbor in nicely angled grid sections"). Target bar: GTA5-class
// freeway GEOMETRY (curved banked deck, barriers, lane paint, pillar rows,
// trumpet interchanges) and CP2077-class street bones (angled harbor grids
// with curbs + sidewalks + lamp light slices). Game/slice code only.
//
// REPLACES (at integration time): the host's FREEWAY NETWORK kRoute block
// (straight 40 m GLB deck segments), its hand-mirrored kFreeway copy in the
// woodlands keep-out, and the glowing kRoute ribbon — the curved ring below
// deliberately shadows the SAME ten waypoints so the Recife/Urban gates, the
// crown crossing, and the woodlands corridor keep-out all still line up.
//
// ============================= INTEGRATION =============================
// (for WP-0 / the host integrator — this module is complete but UNWIRED)
//  1. CMake: add world_hosts/echo_roads.cpp to app/CMakeLists.txt.
//  2. Build once at boot (after hf.load, before first frame):
//         x3::game::EchoRoads roads;
//         roads.build(*device, hf);          // ~6-9 meshes, one-time
//  3. Draw every frame (both live + screenshot paths), persistent lane for
//     now — regionization can slice by edge class later:
//         roads.draw(*device, frame);
//  4. Lights: merge into the host's per-frame light selection with the SAME
//     day/night gate the street lamps use (tod.sample().cityLightsOn) — the
//     module NEVER bakes emissive into materials (Tim's "neon never sleeps"
//     bug class). appendNearLights-style: roads.lights() is a static slice;
//     nearest-K select from it alongside lamp/district lights.
//  5. RETIRE from the host once this draws: the FREEWAY NETWORK block
//     (placeDeckP/placePillar loop over kRoute), the kRoute glow ribbon, and
//     switch the woodlands keep-out corridor + car AI to graph() centerlines
//     (RoadGraph is built for exactly those consumers — see below).
//  6. Do NOT wire before milestone A's byte-compare is banked: this module
//     intentionally CHANGES pixels (it replaces the old freeway).
//  7. FIRST-RUN CHECK (v2 harbor): the boulevard/blocks find the shore by
//     probing the heightfield at build() — grep the boot log for "[roads]"
//     lines: "nudged inland" is normal near coves; "skipped"/"no waterline"
//     means a shore seed or the land threshold needs a second look before
//     shipping the capture set.
// =======================================================================
//
// Deterministic by construction: no rand — the same hash discipline as the
// woodlands scatter. All tunables are named constants in the .cpp.

#include "echo_heightfield.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Road graph — the data shape is designed for the two roadmapped consumers:
//   * CAR AI (pillar: drivable cars / traffic): follow RoadEdge::center
//     samples; laneCenterOffset() turns a lane index into a lateral offset so
//     a vehicle can hold lane 0/1 in either direction. Ramps/streets connect
//     POSITIONALLY (node xz == a point on/near another edge) — consumers snap
//     by proximity, no cross-edge index bookkeeping to go stale.
//   * HOUSE/STREET ALIGNMENT (pillar: real placement, not hash-yaw rings):
//     walk an Avenue/HarborStreet edge's samples; a lot faces the road when
//     its yaw = atan2(tangent) ± 90°, its frontage sits at
//     sample ± perp * (width/2 + setback).
// ---------------------------------------------------------------------------
enum class RoadClass : uint8_t {
    Freeway = 0,     // elevated 2+2 deck, barriers, banked curves, pillars
    Ramp,            // interchange link, grades deck <-> ground
    Avenue,          // ground trunk road, curbs + sidewalks
    HarborStreet,    // harbor-grid street, curbs + sidewalks, lamp-lit
};

struct RoadSample {
    float x = 0, y = 0, z = 0;   // centerline point (world; y = finished deck/tarmac)
    float tx = 0, tz = 1;        // unit tangent (XZ)
    float bank = 0;              // radians; + = right edge dips (superelevation)
};

struct RoadNode { float x = 0, z = 0; };   // graph endpoints (positional joins)

struct RoadEdge {
    RoadClass cls = RoadClass::Avenue;
    float width = 9.0f;          // full paved width (m)
    int   lanesF = 1, lanesB = 1;// forward / backward lane counts
    uint32_t a = 0, b = 0;       // RoadNode indices (a==b for the closed ring)
    std::vector<RoadSample> center;   // arc-length-even samples (~4 m; 2 m ramps)
    float length = 0.0f;         // meters along center
};

struct RoadGraph {
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;

    // Lateral offset (meters, along right-perp (tz,-tx) of the tangent) of the
    // CENTER of lane `lane` (0 = innermost). Right-hand traffic: forward lanes
    // sit on the right of the centerline, backward lanes mirror negative.
    // kLaneWidth lives in the .cpp (3.4 m) and is baked in here.
    static float laneCenterOffset(const RoadEdge& e, int lane, bool forward);
};

// ---------------------------------------------------------------------------
// EchoRoads — build once, draw every frame. One mesh per material bucket
// (asphalt / paint / concrete / sidewalk), identity transform, flat PBR colors
// (textures are a v2 swap — UVs are already road-metric: u across, v = meters
// along / 10, so a tiling asphalt/paint atlas drops in without regeometry).
// ---------------------------------------------------------------------------
class EchoRoads {
public:
    // Generates graph + geometry. Requires a loaded heightfield (returns false
    // and builds nothing if !hf.ok() — the host should log and keep the old
    // freeway in that case). Safe to call exactly once per instance.
    bool build(x3::rhi::IRenderDevice& device, const Heightfield& hf);

    // Draw all buckets (no-op before build). Identity model; one drawMeshPBR
    // per bucket, alphaMask/Blend off, zero emissive (lighting is lights()).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Static lamp slice (freeway poles + harbor street lamps). Host gates by
    // cityLightsOn and merges into its nearest-K per-frame selection.
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

    const RoadGraph& graph() const { return m_graph; }

    // Diagnostics (log/HUD): total paved meters + mesh vertex count.
    float    pavedMeters()   const { return m_pavedMeters; }
    uint32_t vertexCount()   const { return m_vertexCount; }

private:
    struct Bucket {                      // one material = one mesh = one draw
        std::vector<x3::rhi::MeshVertex> v;
        std::vector<uint32_t>            i;
        x3::rhi::MeshHandle              mesh;
        float                            color[4] = { 1, 1, 1, 1 };
    };
    enum { kBucketAsphalt = 0, kBucketPaint, kBucketConcrete, kBucketSidewalk,
           kBucketCount };

    Bucket m_buckets[kBucketCount];
    RoadGraph m_graph;
    std::vector<x3::rhi::PointLight> m_lights;
    float    m_pavedMeters = 0.0f;
    uint32_t m_vertexCount = 0;
    bool     m_built = false;
};

} // namespace x3::game
