#pragma once
// SEAMLESS WORLD STREAMING — the region graph + residency manager that lets the
// player traverse Spire -> surface -> city -> ocean with NO loading screens:
// regions flow in ahead of the player and out behind, under a per-frame
// millisecond budget. Game/slice code only — engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN systems (Scene,
// level_loader, city/ocean_base/world_regions content lanes, terrain.*) + the
// engine interfaces (IJobSystem for async parse, IRenderDevice/IPhysicsWorld for
// the main-thread realize) + public open-world streaming talks/papers. No
// game-engine source consulted.
//
// DESIGN (extends, does not duplicate, the existing streaming):
//   * TerrainStreamer (terrain.h, B3) already streams the GROUND as a residency
//     ring of procedural tiles. This module streams the AUTHORED CONTENT that
//     sits on/under that ground — whole regions (the canonical Spire floor, the
//     city, the undersea base, the surface landmarks) — using the same
//     lifecycle vocabulary: distance-based wants, async heavy work, budgeted
//     main-thread realize, counted teardown, hysteresis.
//   * REGION GRAPH AS DATA (assets/world/regions.json, format x3.regions/1):
//     each region = id + anchor/footprint radius + a CONTENT REF (a LevelDoc
//     project-JSON path + floor, or a builder id naming an existing code-built
//     world) + neighbors + load/unload residency radii (hysteresis pair).
//     The regions map the EXISTING authored worlds at their REAL coordinates.
//   * OWNERSHIP LEDGER teardown: a region build is bracketed with
//     Scene::beginEntityCapture, so the ledger records exactly the entities the
//     builder created. Unload destroys the ledger's unique meshes/textures/
//     bodies and releases the slots back to the Scene free-list (Scene::
//     releaseSlot), so Scene::size() and the resource counts return to baseline
//     — the LevelDoc/terrain "created == destroyed" discipline, region-shaped.
//   * BUDGET: update() does stream work only while its frame budget (ms cvar)
//     has time left. LevelDoc JSON parse runs OFF-thread (jobs->runIO); the
//     main-thread realize (mesh/body creation) runs at most ONE region build
//     per frame; evictions are CHUNKED (a few entities per frame). A monolithic
//     builder call is the atomicity floor: the budget gate is checked before
//     each item, so one item can overshoot — measured + logged when it does.
//   * SEAM POLICY: loadRadius is measured from the footprint EDGE and tuned to
//     vehicle speeds (generous lead time); unloadRadius > loadRadius so
//     boundary-hovering never thrashes. If the player OUTRUNS streaming
//     (teleport/noclip) a SOFT FALLBACK engages: an invisible collision floor
//     at the region anchor (no falling into nothing) + a log line — never a
//     loading screen. The proxy releases the moment the region lands.

#include "scene.h"
#include "level_loader.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Region graph (data model of assets/world/regions.json).
// ---------------------------------------------------------------------------
struct WorldRegionDesc {
    std::string id;            // unique region id ("spire_f1", "city", ...)
    std::string name;          // display name
    std::string builder;       // code-built content ref: "city" | "oceanbase" | "worldregions" ("" => leveldoc)
    std::string levelDoc;      // LevelDoc content ref: project-JSON path ("" => builder)
    int         floor = 1;     // LevelDoc floor number
    float       anchor[3] = {0, 0, 0};  // world anchor (m)
    float       radius = 0.0f;          // authored content footprint radius (XZ, m)
    float       loadRadius   = 300.0f;  // want-in when distToFootprint < this (m)
    float       unloadRadius = 450.0f;  // want-out only when farther than this (m)
    std::vector<std::string> neighbors; // adjacent region ids (graph edges)
};

struct WorldRegionGraph {
    std::vector<WorldRegionDesc> regions;

    // Parse the x3.regions/1 JSON at `path`. Returns false (with errors
    // appended) on parse/shape failure; the graph is left empty then.
    bool load(const std::string& path, std::vector<std::string>& errors);

    int indexOf(std::string_view id) const;    // -1 if absent
    bool empty() const { return regions.empty(); }
};

// The canonical regions.json path with machine fallbacks (mirrors
// canonProjectJsonPath()).
std::string worldRegionsJsonPath();

// ---------------------------------------------------------------------------
// Residency manager.
// ---------------------------------------------------------------------------
enum class RegionState : uint8_t {
    Unloaded = 0,   // nothing resident
    Parsing,        // LevelDoc parse in flight on the I/O lane
    ReadyToBuild,   // parse landed (or builder region wanted); awaiting realize
    Resident,       // content live in the scene
};

class WorldStreamer {
public:
    // Install the graph + wiring. `jobs` may be null (then LevelDoc parses run
    // synchronously on the main thread at realize time — used by the headless
    // self-test without a pool; still correct).
    void init(const WorldRegionGraph& graph, x3::jobs::IJobSystem* jobs);

    // Synchronously build every region whose FOOTPRINT CONTAINS the spawn point
    // (the boot cost = the region you start in; neighbors arrive streamed,
    // post-interactive). Call once after init().
    void buildStartRegions(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           float x, float y, float z);

    // Per-frame residency tick from the player position + velocity:
    //   1) compute wants from pos + vel*lookahead (velocity lookahead) against
    //      each region's load/unload radii (hysteresis);
    //   2) kick async LevelDoc parses for newly-wanted regions;
    //   3) within the remaining frame budget (budgetMs - alreadySpentMs): realize
    //      at most one ready region; advance chunked evictions;
    //   4) proxy maintenance (engage when the player is INSIDE a non-resident
    //      footprint; release when the region lands).
    // Returns the milliseconds of main-thread stream work this call consumed.
    double update(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics,
                  float px, float py, float pz,
                  float vx, float vy, float vz,
                  double budgetMs, double alreadySpentMs = 0.0);

    // Tear down: synchronously evict every resident region (full ledger
    // teardown) + drop any in-flight parse results. Safe to call once.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // ---- Tuning ------------------------------------------------------------
    void setLookahead(float seconds) { m_lookaheadS = seconds; }
    void setEvictChunk(uint32_t entitiesPerSlice) { m_evictChunk = entitiesPerSlice; }

    // ---- Queries (host HUD + self-test) -------------------------------------
    uint32_t   regionCount() const { return (uint32_t)m_regions.size(); }
    RegionState state(uint32_t i) const { return m_regions[i].state; }
    const WorldRegionDesc& desc(uint32_t i) const { return m_regions[i].desc; }
    int  indexOf(std::string_view id) const;
    uint32_t residentCount() const;
    // XZ distance from (x,z) to region i's footprint edge (0 == inside).
    float distToFootprint(uint32_t i, float x, float z) const;
    // Lifetime number of times region i was BUILT (hysteresis check: boundary
    // oscillation must not increment this twice).
    uint32_t buildCount(uint32_t i) const { return m_regions[i].builds; }
    // Lifetime proxy-fallback engagements (0 at sane traversal speeds).
    uint32_t proxyEngageCount() const { return m_proxyEngages; }
    bool     proxyActive(uint32_t i) const { return m_regions[i].proxyActive; }
    // Ledger sizes (entities currently owned by region i; 0 when Unloaded).
    uint32_t ownedEntityCount(uint32_t i) const { return (uint32_t)m_regions[i].entities.size(); }
    // The region's ownership ledger (scene slots) — read-only. The world map's
    // tile bake rasterizes these entities' AABB footprints (world_map.*).
    const std::vector<uint32_t>& ownedEntities(uint32_t i) const { return m_regions[i].entities; }
    // Lifetime resource counters across all regions (leak check: at shutdown
    // created == destroyed).
    uint64_t meshesCreated()   const { return m_meshesCreated; }
    uint64_t meshesDestroyed() const { return m_meshesDestroyed; }
    uint64_t texturesCreated()   const { return m_texturesCreated; }
    uint64_t texturesDestroyed() const { return m_texturesDestroyed; }
    uint64_t bodiesCreated()   const { return m_bodiesCreated; }
    uint64_t bodiesDestroyed() const { return m_bodiesDestroyed; }
    // Worst single realize (region build) observed, ms — the atomicity floor.
    double maxRealizeMs() const { return m_maxRealizeMs; }

    ~WorldStreamer();

    // Async LevelDoc parse result (filled on the I/O lane, consumed on main).
    // Shared-owned by BOTH the region and the in-flight job, so dropping the
    // region mid-parse never dangles the worker's write target. Public only so
    // the .cpp's job payload can name it.
    struct ParseResult {
        CanonFloor        floor;
        std::atomic<bool> done{false};
    };

private:
    struct Region {
        WorldRegionDesc desc;
        RegionState     state = RegionState::Unloaded;
        bool            wanted = false;
        // Ownership ledger (valid while Resident / Evicting).
        std::vector<uint32_t>               entities;   // scene slots
        std::vector<x3::rhi::MeshHandle>    meshes;     // unique meshes created
        std::vector<x3::rhi::TextureHandle> textures;   // unique textures created
        std::vector<x3::phys::BodyId>       bodies;     // unique bodies created
        // Chunked eviction cursor (entity index released so far).
        bool   evicting = false;
        size_t evictCursor = 0;
        // Async parse plumbing (LevelDoc regions).
        std::shared_ptr<ParseResult> parse;
        // Soft-fallback proxy (collision floor while inside a non-resident footprint).
        bool             proxyActive = false;
        x3::phys::BodyId proxyBody{};
        uint32_t builds = 0;     // lifetime build count (hysteresis check)
    };

    void kickParse(Region& r);
    // Realize a region's content into the scene NOW (main thread). Captures the
    // ownership ledger. Returns the milliseconds it took.
    double realize(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics);
    // Advance a chunked eviction; finishes (state -> Unloaded) when the ledger
    // is drained. Releases `m_evictChunk` entities per call.
    void evictSlice(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics);
    void evictAll(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);
    void engageProxy(Region& r, x3::phys::IPhysicsWorld& physics);
    void releaseProxy(Region& r, x3::phys::IPhysicsWorld& physics);
    // Derive the unique mesh/texture/body sets of the captured entities into
    // the region ledger (dedup by handle id).
    void captureLedger(Region& r, const Scene& scene);

    static void parseThunk(void* user);

    std::vector<Region>   m_regions;
    x3::jobs::IJobSystem* m_jobs = nullptr;
    float    m_lookaheadS = 2.5f;
    uint32_t m_evictChunk = 48;

    // W8-3: streamer-lifetime SHARED surface library. Builder regions (city /
    // oceanbase) draw their PBR sets from here, so a region REBUILD costs zero
    // PNG decode (the city was a 2 s realize hitch when it streamed back in) and
    // the sets are decoded once per process. captureLedger EXCLUDES these
    // textures from per-region teardown; shutdown() destroys them once.
    SurfaceLibrary m_surflib;
    uint32_t m_proxyEngages = 0;
    uint64_t m_meshesCreated = 0, m_meshesDestroyed = 0;
    uint64_t m_texturesCreated = 0, m_texturesDestroyed = 0;
    uint64_t m_bodiesCreated = 0, m_bodiesDestroyed = 0;
    double   m_maxRealizeMs = 0.0;
    bool     m_shutdown = false;
};

// Headless self-test (--test-worldstream). Loads the real regions.json, walks a
// scripted tour across 3+ region boundaries on a HeadlessDevice + Jolt world +
// the real job system, and asserts: W1 regions are Resident BEFORE arrival (no
// proxy at walk speed, with the load-ahead margin reported), W2 regions unload
// behind (ledgers drain to zero), W3 hysteresis (boundary oscillation does not
// double-build), W4 the per-frame budget is respected, W5 no leak across a full
// tour loop (created == destroyed at teardown; Scene::size() stable across the
// second lap), W6 the teleport soft-fallback engages + resolves. Logs PASS/FAIL
// W#, returns true iff all pass. No window/Vulkan.
bool runWorldStreamSelfTest();

} // namespace x3::game
