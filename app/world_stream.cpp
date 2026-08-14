// SEAMLESS WORLD STREAMING — region graph + residency manager. See world_stream.h.
// Clean-room: X3Native's own Scene / level_loader / content lanes + the engine
// interfaces. No game-engine source consulted.
#include "world_stream.h"

#include "city.h"
#include "crowd.h"            // self-test: the hooked region-owned crowd proof
#include "ocean_base.h"
#include "world_regions.h"
#include "story_ops.h"        // JValue / JParser — the minimal shared JSON reader
#include "headless_device.h"  // self-test device

#include "engine/core/x3_log.h"
#include "engine/asset/IModelLoader.h"   // isCacheManagedTexture — W5b ledger guard
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace x3::game {

// ===========================================================================
// Region graph loader (assets/world/regions.json, format x3.regions/1)
// ===========================================================================

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

float jnum(const JValue* v, float d) { return (v && v->isNum()) ? (float)v->num : d; }

} // namespace

bool WorldRegionGraph::load(const std::string& path, std::vector<std::string>& errors) {
    regions.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { errors.push_back(path + ": cannot open"); return false; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    JParser parser(src);
    JValue root = parser.parseValue();
    if (!parser.ok || !root.isObj()) { errors.push_back(path + ": JSON parse failed"); return false; }
    const std::string fmt = root.find("format") ? root.find("format")->asStr() : "";
    if (fmt != "x3.regions/1") {
        errors.push_back(path + ": format is `" + fmt + "` (want x3.regions/1)");
        return false;
    }
    const JValue* arr = root.find("regions");
    if (!arr || !arr->isArr()) { errors.push_back(path + ": missing `regions` array"); return false; }

    bool ok = true;
    for (const JValue& jr : *arr->arr) {
        if (!jr.isObj()) { errors.push_back(path + ": region is not an object"); ok = false; continue; }
        WorldRegionDesc d;
        d.id   = jr.find("id")   ? jr.find("id")->asStr()   : "";
        d.name = jr.find("name") ? jr.find("name")->asStr() : d.id;
        d.builder  = jr.find("builder")  ? jr.find("builder")->asStr()  : "";
        d.levelDoc = jr.find("leveldoc") ? jr.find("leveldoc")->asStr() : "";
        d.floor = (int)jnum(jr.find("floor"), 1.0f);
        if (const JValue* a = jr.find("anchor"); a && a->isArr() && a->arr->size() == 3)
            for (int i = 0; i < 3; ++i) d.anchor[i] = jnum(&(*a->arr)[i], 0.0f);
        d.radius       = jnum(jr.find("radius"),       0.0f);
        d.loadRadius   = jnum(jr.find("loadRadius"),   300.0f);
        d.unloadRadius = jnum(jr.find("unloadRadius"), d.loadRadius * 1.5f);
        if (const JValue* n = jr.find("neighbors"); n && n->isArr())
            for (const JValue& nv : *n->arr) d.neighbors.push_back(nv.asStr());
        if (d.id.empty()) { errors.push_back(path + ": region missing `id`"); ok = false; continue; }
        if (d.builder.empty() && d.levelDoc.empty()) {
            errors.push_back(path + ": region `" + d.id + "` has neither `builder` nor `leveldoc`");
            ok = false; continue;
        }
        if (d.unloadRadius <= d.loadRadius) {
            errors.push_back(path + ": region `" + d.id + "` unloadRadius must exceed loadRadius (hysteresis)");
            ok = false; continue;
        }
        regions.push_back(std::move(d));
    }
    // Neighbor ids must resolve (a typo here would silently kill the preload edge).
    for (const WorldRegionDesc& d : regions)
        for (const std::string& n : d.neighbors)
            if (indexOf(n) < 0) { errors.push_back(path + ": region `" + d.id + "` names unknown neighbor `" + n + "`"); ok = false; }
    if (!ok) regions.clear();
    return ok;
}

int WorldRegionGraph::indexOf(std::string_view id) const {
    for (size_t i = 0; i < regions.size(); ++i)
        if (regions[i].id == id) return (int)i;
    return -1;
}

std::string worldRegionsJsonPath() {
    // Mirror canonProjectJsonPath(): repo-relative first, then machine fallbacks.
    static const char* kCandidates[] = {
        "assets/world/regions.json",
        "../assets/world/regions.json",
        "../../assets/world/regions.json",
        R"(D:\GameDev\X3Native-frustumcull\assets\world\regions.json)",
        R"(C:\GameDev\X3Native-engine\assets\world\regions.json)",
    };
    for (const char* c : kCandidates)
        if (std::filesystem::exists(c)) return c;
    return kCandidates[0];
}

std::string worldRegionsCanonJsonPath() {
    // SEAM 3: the canon master-world graph (spire_f1 dropped — canonWorld builds
    // the real tower itself). Same repo-relative-first fallback chain.
    static const char* kCandidates[] = {
        "assets/world/regions.canon.json",
        "../assets/world/regions.canon.json",
        "../../assets/world/regions.canon.json",
        R"(D:\GameDev\X3Native-frustumcull\assets\world\regions.canon.json)",
        R"(C:\GameDev\X3Native-engine\assets\world\regions.canon.json)",
    };
    for (const char* c : kCandidates)
        if (std::filesystem::exists(c)) return c;
    return kCandidates[0];
}

// ===========================================================================
// WorldStreamer
// ===========================================================================

void WorldStreamer::init(const WorldRegionGraph& graph, x3::jobs::IJobSystem* jobs) {
    m_regions.clear();
    m_regions.reserve(graph.regions.size());
    for (const WorldRegionDesc& d : graph.regions) {
        Region r;
        r.desc = d;
        m_regions.push_back(std::move(r));
    }
    m_jobs = jobs;
    m_shutdown = false;
}

int WorldStreamer::indexOf(std::string_view id) const {
    for (size_t i = 0; i < m_regions.size(); ++i)
        if (m_regions[i].desc.id == id) return (int)i;
    return -1;
}

uint32_t WorldStreamer::residentCount() const {
    uint32_t n = 0;
    for (const Region& r : m_regions)
        if (r.state == RegionState::Resident) ++n;
    return n;
}

float WorldStreamer::distToFootprint(uint32_t i, float x, float z) const {
    const WorldRegionDesc& d = m_regions[i].desc;
    const float dx = x - d.anchor[0], dz = z - d.anchor[2];
    const float dist = std::sqrt(dx * dx + dz * dz) - d.radius;
    return dist > 0.0f ? dist : 0.0f;
}

// ---- async LevelDoc parse --------------------------------------------------

namespace {
// Heap payload handed to the I/O lane: holds a strong ref on the result so the
// region can be dropped while the parse is still in flight without a dangle.
struct ParseJob {
    std::string path;
    int         floor = 1;
    std::shared_ptr<WorldStreamer::ParseResult> result;
};
} // namespace

void WorldStreamer::parseThunk(void* user) {
    auto* job = (ParseJob*)user;
    job->result->floor = loadCanonFloor(job->path, job->floor);
    job->result->done.store(true, std::memory_order_release);
    delete job;
}

void WorldStreamer::kickParse(Region& r) {
    auto res = std::make_shared<ParseResult>();
    r.parse = res;
    r.state = RegionState::Parsing;

    // Resolve the LevelDoc path: as authored, else the canon fallback chain.
    std::string path = r.desc.levelDoc;
    if (!std::filesystem::exists(path)) path = canonProjectJsonPath();

    if (m_jobs) {
        auto* job = new ParseJob;
        job->path = path;
        job->floor = r.desc.floor;
        job->result = res;            // worker-side strong ref
        m_jobs->runIO(&WorldStreamer::parseThunk, job, nullptr);
    } else {
        // No job system (headless minimal mode): parse synchronously, still correct.
        res->floor = loadCanonFloor(path, r.desc.floor);
        res->done.store(true);
    }
}

// ---- ownership ledger -------------------------------------------------------

void WorldStreamer::captureLedger(Region& r, const Scene& scene) {
    std::unordered_set<uint32_t> meshIds, texIds, bodyIds;
    r.meshes.clear(); r.textures.clear(); r.bodies.clear();
    // W8-3: capture EVERY texture channel an entity references (tex + normalTex
    // + mrTex — normal/mr previously leaked on evict), EXCEPT textures owned by
    // the streamer's shared surface library (destroyed once at shutdown, never
    // per-region — other resident regions may reference the same set).
    auto captureTex = [&](const x3::rhi::TextureHandle& t) {
        if (!t.valid() || m_surflib.ownsTexture(t.id)) return;
        // SEAM 3: an external shared library's textures (the canon RoomDressing's
        // sets) belong to their owner — never in a region ledger, never torn down here.
        if (m_sharedSurf && m_sharedSurf->ownsTexture(t.id)) return;
        // W5b DOUBLE-FREE FIX: textures held by the process-wide refcounted GLB
        // cache are owned by the model loader, not by whichever region happened to
        // reference them first. The SAME converted kit piece is loaded by many
        // builders across Echotropolis and they all get the SAME handle, so a
        // per-region destroyTexture() frees a texture other resident regions (and
        // the boot-time cache's permanent ref) still point at. That is why teardown
        // measured MORE textures destroyed than created (76 created / 112 destroyed)
        // while meshes and bodies balanced exactly — meshes are never shared.
        // The loader releases these on the last Model unload(); see
        // engine/asset/IModelLoader.h isCacheManagedTexture() and env_art.cpp's
        // destroy() doctrine, which already solved this for the env-art path.
        if (x3::asset::isCacheManagedTexture(t.id)) return;
        if (texIds.insert(t.id).second) r.textures.push_back(t);
    };
    for (uint32_t id : r.entities) {
        const Entity& e = scene.get(id);
        if (e.mesh.valid() && meshIds.insert(e.mesh.id).second) r.meshes.push_back(e.mesh);
        captureTex(e.tex);
        captureTex(e.normalTex);
        captureTex(e.mrTex);
        if (e.body.valid() && bodyIds.insert(e.body.id).second) r.bodies.push_back(e.body);
    }
    m_meshesCreated   += r.meshes.size();
    m_texturesCreated += r.textures.size();
    m_bodiesCreated   += r.bodies.size();
}

double WorldStreamer::realize(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics) {
    const double t0 = nowMs();
    r.entities.clear();
    // BATCHED upload window (the fast-boot pattern, replicated minimally on this
    // lineage): every createMesh/createTexture this builder issues records into
    // ONE command buffer + ONE submit/fence instead of a blocking submit EACH —
    // on the real device this is the difference between ~26 ms/mesh and ~free.
    // No-op on headless devices.
    device.beginUploadBatch();
    scene.beginEntityCapture(&r.entities);
    if (!r.desc.levelDoc.empty()) {
        CanonFloor floor;
        if (r.parse && r.parse->done.load(std::memory_order_acquire)) {
            floor = std::move(r.parse->floor);
        } else {
            // Sync fallback (boot path / no parse kicked yet).
            std::string path = r.desc.levelDoc;
            if (!std::filesystem::exists(path)) path = canonProjectJsonPath();
            floor = loadCanonFloor(path, r.desc.floor);
        }
        if (floor.valid()) {
            CanonBuildOpts opts;   // graybox shells only; doors/gameplay are host layers
            buildCanonFloor(floor, scene, device, physics, opts);
        } else {
            x3::logError("[worldstream] region `" + r.desc.id + "`: LevelDoc invalid (" +
                         r.desc.levelDoc + " floor " + std::to_string(r.desc.floor) + ")");
        }
        r.parse.reset();

    } else if (r.desc.builder == "city") {
        // CONTENT WIRING: refill the glow list for THIS realize (the previous
        // set died with the last eviction) and let City emit into it.
        m_cityGlows.clear();
        City c; c.build(scene, device, physics, m_sharedSurf ? m_sharedSurf : &m_surflib,
                        m_emitCityGlows ? &m_cityGlows : nullptr);
    } else if (r.desc.builder == "oceanbase") {
        OceanBase ob; ob.build(scene, device, physics, m_sharedSurf ? m_sharedSurf : &m_surflib);
    } else if (r.desc.builder == "worldregions") {
        WorldRegions wr; wr.build(scene, device, physics);
    } else {
        x3::logError("[worldstream] region `" + r.desc.id + "`: unknown builder `" +
                     r.desc.builder + "`");
    }
    // LIVING NPCs: the host's region-owned content (city street crowds) builds
    // INSIDE the capture window — its entities/meshes join this region's
    // ownership ledger and are torn down with the region.
    if (m_onRegionBuild) m_onRegionBuild(r.desc, scene, device, physics);
    scene.endEntityCapture();
    device.endUploadBatch();
    captureLedger(r, scene);
    // SEAM 3: stamp the realized entities with the host's room tag (canon PVS
    // gate — see setRealizedRoomTag). Default kNoRoom = untouched (streamed host).
    if (m_roomTag != kNoRoom)
        for (uint32_t id : r.entities) scene.get(id).roomId = m_roomTag;
    r.state = RegionState::Resident;
    r.evicting = false;
    r.evictCursor = 0;
    ++r.builds;
    releaseProxy(r, physics);
    const double dt = nowMs() - t0;
    m_maxRealizeMs = std::max(m_maxRealizeMs, dt);
    x3::logInfo("[worldstream] + region `" + r.desc.id + "` resident: " +
                std::to_string(r.entities.size()) + " entities, " +
                std::to_string(r.meshes.size()) + " meshes, " +
                std::to_string(r.bodies.size()) + " bodies (" +
                std::to_string(dt) + " ms)");
    return dt;
}

void WorldStreamer::evictSlice(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (!r.evicting) {
        // LIVING NPCs: tell the host FIRST — before any slot is released — so
        // region-owned systems abandon their entity ids (region safety).
        if (m_onRegionTeardown) m_onRegionTeardown(r.desc);
        r.evicting = true;
        r.evictCursor = 0;
    }
    const size_t end = std::min(r.entities.size(), r.evictCursor + m_evictChunk);
    for (size_t i = r.evictCursor; i < end; ++i)
        scene.releaseSlot(r.entities[i]);
    r.evictCursor = end;
    if (r.evictCursor < r.entities.size()) return;   // more slices next frame

    // All entities released — destroy the region's unique GPU/physics resources.
    // (Resource destruction is the cheap half — destroyMesh/removeBody enqueue;
    // the entity release above is what touched the scene.)
    for (const x3::rhi::MeshHandle& m : r.meshes)      device.destroyMesh(m);
    for (const x3::rhi::TextureHandle& t : r.textures) device.destroyTexture(t);
    for (const x3::phys::BodyId& b : r.bodies)         physics.removeBody(b);
    m_meshesDestroyed   += r.meshes.size();
    m_texturesDestroyed += r.textures.size();
    m_bodiesDestroyed   += r.bodies.size();
    x3::logInfo("[worldstream] - region `" + r.desc.id + "` unloaded (" +
                std::to_string(r.entities.size()) + " entities, " +
                std::to_string(r.meshes.size()) + " meshes released)");
    r.entities.clear(); r.meshes.clear(); r.textures.clear(); r.bodies.clear();
    r.evicting = false;
    r.evictCursor = 0;
    r.state = RegionState::Unloaded;
}

void WorldStreamer::evictAll(Region& r, Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics) {
    while (r.state == RegionState::Resident)
        evictSlice(r, scene, device, physics);
}

// ---- soft fallback proxy ------------------------------------------------------

void WorldStreamer::engageProxy(Region& r, x3::phys::IPhysicsWorld& physics) {
    if (r.proxyActive) return;
    // An invisible collision floor spanning the footprint at the region's anchor
    // height: the player who outran streaming stands on SOMETHING (never falls
    // into nothing) while the real content lands. Render is simply skipped (the
    // region isn't resident), which is the documented soft fallback — no loading
    // screen.
    const float cx = r.desc.anchor[0], cy = r.desc.anchor[1], cz = r.desc.anchor[2];
    const float h = std::max(r.desc.radius, 50.0f);
    const float v[12] = { cx - h, cy, cz - h,   cx + h, cy, cz - h,
                          cx + h, cy, cz + h,   cx - h, cy, cz + h };
    const uint32_t idx[6] = { 0, 2, 1, 0, 3, 2 };
    r.proxyBody = physics.addStaticMesh(v, 4, idx, 6);
    r.proxyActive = true;
    ++m_proxyEngages;
    ++m_bodiesCreated;
    x3::logInfo("[worldstream] ! region `" + r.desc.id +
                "`: player outran streaming — PROXY collision floor engaged (soft fallback)");
}

void WorldStreamer::releaseProxy(Region& r, x3::phys::IPhysicsWorld& physics) {
    if (!r.proxyActive) return;
    if (r.proxyBody.valid()) physics.removeBody(r.proxyBody);
    r.proxyBody = {};
    r.proxyActive = false;
    ++m_bodiesDestroyed;
}

// ---- boot + per-frame -----------------------------------------------------------

void WorldStreamer::buildStartRegions(Scene& scene, x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& physics,
                                      float x, float y, float z) {
    (void)y;
    for (uint32_t i = 0; i < (uint32_t)m_regions.size(); ++i) {
        Region& r = m_regions[i];
        if (r.state != RegionState::Unloaded) continue;
        if (distToFootprint(i, x, z) <= 0.0f) {
            r.wanted = true;
            const double ms = realize(r, scene, device, physics);
            x3::logInfo("[worldstream] boot region `" + r.desc.id + "` built in " +
                        std::to_string(ms) + " ms");
        }
    }
}

double WorldStreamer::update(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics,
                             float px, float py, float pz,
                             float vx, float vy, float vz,
                             double budgetMs, double alreadySpentMs) {
    (void)py; (void)vy;
    const double t0 = nowMs();
    auto spent = [&] { return (nowMs() - t0) + alreadySpentMs; };

    // ---- 1) wants: hysteresis band from pos + velocity lookahead -------------
    const float ex = px + vx * m_lookaheadS;
    const float ez = pz + vz * m_lookaheadS;

    // Neighbor preload: the regions the player is INSIDE extend a warmer load
    // radius (x1.5) to their graph neighbors, so the next region over is never a
    // surprise even when its plain distance band hasn't tripped yet.
    std::unordered_set<size_t> neighborWarm;
    for (size_t i = 0; i < m_regions.size(); ++i) {
        if (m_regions[i].state == RegionState::Resident &&
            distToFootprint((uint32_t)i, px, pz) <= 0.0f) {
            for (const std::string& n : m_regions[i].desc.neighbors) {
                const int ni = indexOf(n);
                if (ni >= 0) neighborWarm.insert((size_t)ni);
            }
        }
    }

    for (size_t i = 0; i < m_regions.size(); ++i) {
        Region& r = m_regions[i];
        const float dNow   = distToFootprint((uint32_t)i, px, pz);
        const float dAhead = distToFootprint((uint32_t)i, ex, ez);
        const float d = std::min(dNow, dAhead);
        const float loadR = r.desc.loadRadius * (neighborWarm.count(i) ? 1.5f : 1.0f);
        if (d < loadR)                       r.wanted = true;
        else if (d > r.desc.unloadRadius)    r.wanted = false;
        // else: inside the hysteresis band — keep the previous want (no thrash).
    }

    // ---- 2) cheap state transitions ------------------------------------------
    for (Region& r : m_regions) {
        if (r.wanted) {
            if (r.state == RegionState::Unloaded && !r.evicting) {
                if (!r.desc.levelDoc.empty() && m_jobs) kickParse(r);
                else r.state = RegionState::ReadyToBuild;
            } else if (r.state == RegionState::Parsing &&
                       r.parse && r.parse->done.load(std::memory_order_acquire)) {
                r.state = RegionState::ReadyToBuild;
            }
        } else {
            if (r.state == RegionState::ReadyToBuild) {
                r.parse.reset();
                r.state = RegionState::Unloaded;
            } else if (r.state == RegionState::Parsing &&
                       r.parse && r.parse->done.load(std::memory_order_acquire)) {
                r.parse.reset();
                r.state = RegionState::Unloaded;
            }
            // Parsing-but-not-done: let the I/O job land, dropped next tick.
        }
    }

    // ---- 3) proxy maintenance (engage BEFORE the build work so even a
    //         same-frame build records the engagement honestly) ---------------
    for (size_t i = 0; i < m_regions.size(); ++i) {
        Region& r = m_regions[i];
        const bool inside = distToFootprint((uint32_t)i, px, pz) <= 0.0f;
        if (inside && r.state != RegionState::Resident && r.wanted) engageProxy(r, physics);
        else if (r.proxyActive && (!inside || r.state == RegionState::Resident))
            releaseProxy(r, physics);
    }

    // ---- 4) budgeted work: evictions (chunked) + at most ONE realize ----------
    // Nearest-first realize order so the region the player is about to enter wins.
    bool builtOne = false;
    std::vector<size_t> order(m_regions.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return distToFootprint((uint32_t)a, px, pz) < distToFootprint((uint32_t)b, px, pz);
    });
    for (size_t oi : order) {
        if (spent() >= budgetMs) break;
        Region& r = m_regions[oi];
        if (!r.wanted && r.state == RegionState::Resident) {
            evictSlice(r, scene, device, physics);
        } else if (!builtOne && r.wanted && r.state == RegionState::ReadyToBuild) {
            const double ms = realize(r, scene, device, physics);
            if (ms > budgetMs)
                x3::logInfo("[worldstream] region `" + r.desc.id + "` realize " +
                            std::to_string(ms) + " ms overshot the " +
                            std::to_string(budgetMs) + " ms budget (monolithic builder — " +
                            "the documented atomicity floor)");
            builtOne = true;
        }
    }

    return nowMs() - t0;
}

void WorldStreamer::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics) {
    if (m_shutdown) return;
    m_shutdown = true;
    for (Region& r : m_regions) {
        releaseProxy(r, physics);
        evictAll(r, scene, device, physics);
        r.parse.reset();

        r.state = RegionState::Unloaded;
        r.wanted = false;
    }
    // W8-3: release the shared surface library's textures (decoded once per
    // process; excluded from every per-region ledger above).
    m_surflib.destroyAll(device);
}

WorldStreamer::~WorldStreamer() = default;

// ===========================================================================
// Headless self-test (--test-worldstream)
// ===========================================================================

namespace {

int gw_pass = 0, gw_fail = 0;
void checkW(bool ok, const char* what) {
    if (ok) { ++gw_pass; x3::logInfo (std::string("[worldstream-test] PASS ") + what); }
    else    { ++gw_fail; x3::logError(std::string("[worldstream-test] FAIL ") + what); }
}

// Counting headless device (mirrors terrain.cpp's leak-check double).
class CountingDevice : public HeadlessRenderDevice {
public:
    uint64_t meshesCreated = 0, meshesDestroyed = 0;
    uint64_t texturesCreated = 0, texturesDestroyed = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshesCreated;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
    void destroyMesh(x3::rhi::MeshHandle m) override {
        ++meshesDestroyed;
        HeadlessRenderDevice::destroyMesh(m);
    }
    x3::rhi::TextureHandle createTexture(const void* p, uint32_t w, uint32_t h, bool srgb) override {
        ++texturesCreated;
        return HeadlessRenderDevice::createTexture(p, w, h, srgb);
    }
    void destroyTexture(x3::rhi::TextureHandle t) override {
        ++texturesDestroyed;
        HeadlessRenderDevice::destroyTexture(t);
    }
};

} // namespace

bool runWorldStreamSelfTest() {
    gw_pass = gw_fail = 0;

    std::unique_ptr<x3::jobs::IJobSystem> jobs(x3::jobs::createJobSystem());
    jobs->init(0);
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    CountingDevice device;
    Scene scene;

    // ---- W0: the region graph loads from the real data file ------------------
    WorldRegionGraph graph;
    std::vector<std::string> errors;
    const bool loaded = graph.load(worldRegionsJsonPath(), errors);
    for (const std::string& e : errors) x3::logError("[worldstream-test] " + e);
    checkW(loaded && graph.regions.size() >= 4,
           "W0 region graph parses (>=4 regions, neighbors resolve, hysteresis sane)");
    if (!loaded) {
        jobs->shutdown(); physics->shutdown();
        return false;
    }

    WorldStreamer ws;
    ws.init(graph, jobs.get());

    const int iSpire = ws.indexOf("spire_f1");
    const int iCity  = ws.indexOf("city");
    const int iOcean = ws.indexOf("ocean_base");
    const int iSurf  = ws.indexOf("surface_landmarks");
    checkW(iSpire >= 0 && iCity >= 0 && iOcean >= 0 && iSurf >= 0,
           "W0b canonical region ids present (spire_f1/city/ocean_base/surface_landmarks)");

    // Boot at the Spire: the regions CONTAINING the spawn build synchronously
    // (the boot cost); everything else streams.
    const float startX = ws.desc(iSpire).anchor[0], startZ = ws.desc(iSpire).anchor[2];
    ws.buildStartRegions(scene, device, *physics, startX, 0.0f, startZ);
    checkW(ws.state(iSpire) == RegionState::Resident &&
           ws.state(iSurf)  == RegionState::Resident &&
           ws.state(iOcean) == RegionState::Unloaded,
           "W0c boot: start regions resident, far regions NOT built at boot");

    // ---- W7 (W8-3): the CITY region actually loads + carries a real city ------
    // The Spire spawn sits inside the city footprint (anchor -200,425 r750), so
    // the city builds at boot. Assert it is resident AND that the builder placed
    // Babylon-scale content (blockout-plus: buildings + bands + streets => the
    // ledger owns hundreds of entities, not the old 27-prop graybox).
    {
        const bool cityResident = (ws.state(iCity) == RegionState::Resident);
        const uint32_t cityEnts = ws.ownedEntityCount(iCity);
        x3::logInfo("[worldstream-test] city region at boot: resident=" +
                    std::to_string((int)cityResident) + " ownedEntities=" +
                    std::to_string(cityEnts));
        checkW(cityResident && cityEnts >= 200,
               "W7 city region loads with blockout-plus content (>=200 owned entities)");
    }

    // ---- the tour: Spire -> ocean base -> Spire, twice (lap2 = stability) -----
    const float walkSpeed = 7.0f;            // m/s (brisk walk/jog)
    const float dt = 1.0f / 60.0f;
    // The test budget cvar. Release gate = 33 ms (contention-robust, mirrors the
    // terrain S5 gate). Debug codegen makes the MONOLITHIC LevelDoc realize alone
    // ~100 ms (the documented atomicity floor; same class as the pre-existing S5
    // Debug-only overrun in --test-streaming) — the budget MECHANISM is exercised
    // identically, so Debug runs the same correctness assertions under a wider
    // gate instead of pinning a codegen-speed number. Release is the perf bar.
#ifdef NDEBUG
    const double budgetMs = 33.0;
#else
    const double budgetMs = 200.0;
#endif
    const float oceanX = ws.desc(iOcean).anchor[0], oceanZ = ws.desc(iOcean).anchor[2];

    float fx = startX, fz = startZ;
    double maxFrameMs = 0.0, sumSteadyMs = 0.0;
    int steadyFrames = 0;
    float oceanResidentAtDist = -1.0f;       // load-ahead margin (walk)
    float spireResidentAtDist = -1.0f;       // load-ahead margin on the return leg
    bool oceanResidentOnArrival = false, spireResidentOnReturn = false;
    bool cityUnloadedFarAway = false, spireUnloadedFarAway = false;
    uint32_t proxyAtWalk = 0;
    uint32_t sceneSizeLap1 = 0, sceneSizeLap2 = 0;

    auto stepToward = [&](float tx, float tz) {
        const float dx = tx - fx, dz = tz - fz;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float step = walkSpeed * dt;
        float vx = 0.0f, vz = 0.0f;
        if (d > 1e-3f) { vx = dx / d * walkSpeed; vz = dz / d * walkSpeed; }
        if (d <= step) { fx = tx; fz = tz; }
        else           { fx += vx * dt; fz += vz * dt; }
        const uint32_t residBefore = ws.residentCount();
        const double ms = ws.update(scene, device, *physics, fx, 0.0f, fz, vx, 0.0f, vz, budgetMs);
        maxFrameMs = std::max(maxFrameMs, ms);
        if (ws.residentCount() == residBefore) { sumSteadyMs += ms; ++steadyFrames; }
        return d <= step;   // arrived
    };

    for (int lap = 0; lap < 2; ++lap) {
        // Leg 1: Spire -> ocean base.
        for (int f = 0; f < 60000; ++f) {
            const bool arrived = stepToward(oceanX, oceanZ);
            if (lap == 0) {
                if (oceanResidentAtDist < 0.0f && ws.state(iOcean) == RegionState::Resident)
                    oceanResidentAtDist = ws.distToFootprint(iOcean, fx, fz);
                if (ws.distToFootprint(iOcean, fx, fz) <= 0.0f && !oceanResidentOnArrival)
                    oceanResidentOnArrival = (ws.state(iOcean) == RegionState::Resident);
            }
            if (arrived) break;
        }
        // At the ocean base: the regions behind must be gone (ledger drained).
        if (lap == 0) {
            cityUnloadedFarAway  = (ws.state(iCity)  == RegionState::Unloaded) &&
                                   (ws.ownedEntityCount(iCity) == 0);
            spireUnloadedFarAway = (ws.state(iSpire) == RegionState::Unloaded) &&
                                   (ws.ownedEntityCount(iSpire) == 0);
            proxyAtWalk = ws.proxyEngageCount();
        }
        // Leg 2: ocean base -> Spire.
        for (int f = 0; f < 60000; ++f) {
            const bool arrived = stepToward(startX, startZ);
            if (lap == 0) {
                if (spireResidentAtDist < 0.0f && ws.state(iSpire) == RegionState::Resident)
                    spireResidentAtDist = ws.distToFootprint(iSpire, fx, fz);
                if (ws.distToFootprint(iSpire, fx, fz) <= 0.0f && !spireResidentOnReturn)
                    spireResidentOnReturn = (ws.state(iSpire) == RegionState::Resident);
            }
            if (arrived) break;
        }
        // Settle a few frames so chunked evictions/draws drain fully.
        for (int f = 0; f < 240; ++f)
            ws.update(scene, device, *physics, fx, 0.0f, fz, 0, 0, 0, budgetMs);
        if (lap == 0) sceneSizeLap1 = scene.size();
        else          sceneSizeLap2 = scene.size();
    }

    // ---- W1: regions load BEFORE arrival at walk speed (no proxy) -------------
    {
        char b[240];
        std::snprintf(b, sizeof(b),
            "[worldstream-test] load-ahead margins: ocean resident %.0f m out, "
            "spire resident %.0f m out (footprint-edge distance); proxy engages at walk = %u",
            oceanResidentAtDist, spireResidentAtDist, proxyAtWalk);
        x3::logInfo(b);
        checkW(oceanResidentOnArrival && spireResidentOnReturn && proxyAtWalk == 0 &&
               oceanResidentAtDist > 50.0f && spireResidentAtDist > 50.0f,
               "W1 regions resident BEFORE arrival (no proxy fallback at walk speed)");
    }

    // ---- W2: regions unload BEHIND the player (ledgers drained) ---------------
    checkW(cityUnloadedFarAway && spireUnloadedFarAway,
           "W2 regions unload behind (city+spire gone at the ocean, ledgers empty)");

    // ---- W3: hysteresis — boundary oscillation must not double-build ----------
    {
        // Park exactly on the ocean load boundary and wiggle across it.
        const float dx = oceanX - fx, dz = oceanZ - fz;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float ux = dx / d, uz = dz / d;
        // Point where footprint distance == loadRadius:
        const float bdist = ws.desc(iOcean).radius + ws.desc(iOcean).loadRadius;
        const float bx = oceanX - ux * bdist, bz = oceanZ - uz * bdist;
        const uint32_t buildsBefore = ws.buildCount(iOcean);
        for (int f = 0; f < 900; ++f) {
            const float wob = 30.0f * std::sin((float)f * 0.05f);   // +-30 m across the line
            const float ox = bx + ux * wob, oz = bz + uz * wob;
            ws.update(scene, device, *physics, ox, 0.0f, oz, 0, 0, 0, budgetMs);
        }
        const uint32_t buildsAfter = ws.buildCount(iOcean);
        x3::logInfo("[worldstream-test] hysteresis: ocean builds " +
                    std::to_string(buildsBefore) + " -> " + std::to_string(buildsAfter) +
                    " across 900 boundary-wobble frames");
        checkW(buildsAfter <= buildsBefore + 1,
               "W3 hysteresis: boundary oscillation does not thrash (<=1 build)");
        // Walk back out cleanly past the unload radius so W5's teardown is honest.
        float cx = bx, cz = bz;
        for (int f = 0; f < 20000; ++f) {
            const float gx = startX, gz = startZ;
            const float ddx = gx - cx, ddz = gz - cz;
            const float dd = std::sqrt(ddx * ddx + ddz * ddz);
            if (dd < 1.0f) break;
            cx += ddx / dd * walkSpeed * dt; cz += ddz / dd * walkSpeed * dt;
            ws.update(scene, device, *physics, cx, 0.0f, cz,
                      ddx / dd * walkSpeed, 0.0f, ddz / dd * walkSpeed, budgetMs);
        }
        fx = startX; fz = startZ;
        for (int f = 0; f < 240; ++f)
            ws.update(scene, device, *physics, fx, 0.0f, fz, 0, 0, 0, budgetMs);
    }

    // ---- W4: per-frame budget respected ---------------------------------------
    {
        const double steadyAvg = steadyFrames ? sumSteadyMs / steadyFrames : 0.0;
        char b[240];
        std::snprintf(b, sizeof(b),
            "[worldstream-test] stream work: max frame %.3f ms (budget %.0f ms), "
            "steady-state avg %.4f ms over %d frames, worst single realize %.3f ms",
            maxFrameMs, budgetMs, steadyAvg, steadyFrames, ws.maxRealizeMs());
        x3::logInfo(b);
        checkW(maxFrameMs < budgetMs && steadyAvg < 1.0,
               "W4 budget respected (max frame < budget cvar, steady avg < 1 ms)");
    }

    // ---- W6: teleport soft fallback (sprint outruns streaming) ----------------
    {
        const uint32_t engagesBefore = ws.proxyEngageCount();
        // Teleport INTO the (currently unloaded) ocean footprint.
        fx = oceanX; fz = oceanZ;
        bool becameResident = false;
        for (int f = 0; f < 900; ++f) {
            ws.update(scene, device, *physics, fx, 0.0f, fz, 0, 0, 0, budgetMs);
            if (ws.state(iOcean) == RegionState::Resident) { becameResident = true; break; }
        }
        const bool proxied = ws.proxyEngageCount() > engagesBefore;
        checkW(proxied && becameResident && !ws.proxyActive(iOcean),
               "W6 teleport fallback: proxy floor engages, region lands, proxy releases");
        // Walk home so the final teardown mirrors a real quit-at-spawn.
        for (int f = 0; f < 60000; ++f) {
            const float ddx = startX - fx, ddz = startZ - fz;
            const float dd = std::sqrt(ddx * ddx + ddz * ddz);
            if (dd < 1.0f) break;
            fx += ddx / dd * walkSpeed * dt; fz += ddz / dd * walkSpeed * dt;
            ws.update(scene, device, *physics, fx, 0.0f, fz,
                      ddx / dd * walkSpeed, 0.0f, ddz / dd * walkSpeed, budgetMs);
        }
        for (int f = 0; f < 240; ++f)
            ws.update(scene, device, *physics, fx, 0.0f, fz, 0, 0, 0, budgetMs);
    }

    // ---- W5: no leak / allocation stability across the full tour --------------
    {
        x3::logInfo("[worldstream-test] scene size lap1=" + std::to_string(sceneSizeLap1) +
                    " lap2=" + std::to_string(sceneSizeLap2) +
                    " freeSlots=" + std::to_string(scene.freeSlotCount()));
        checkW(sceneSizeLap1 > 0 && sceneSizeLap1 == sceneSizeLap2,
               "W5a Scene::size() stable across a full tour loop (slot reuse)");
        ws.shutdown(scene, device, *physics);
        const bool meshBalance = (device.meshesCreated == device.meshesDestroyed);
        const bool texBalance  = (device.texturesCreated == device.texturesDestroyed);
        const bool ledgerBalance = (ws.meshesCreated() == ws.meshesDestroyed()) &&
                                   (ws.texturesCreated() == ws.texturesDestroyed()) &&
                                   (ws.bodiesCreated() == ws.bodiesDestroyed());
        x3::logInfo("[worldstream-test] device meshes c/d=" +
                    std::to_string(device.meshesCreated) + "/" + std::to_string(device.meshesDestroyed) +
                    " textures c/d=" + std::to_string(device.texturesCreated) + "/" +
                    std::to_string(device.texturesDestroyed) +
                    " | ledger bodies c/d=" + std::to_string(ws.bodiesCreated()) + "/" +
                    std::to_string(ws.bodiesDestroyed()));
        checkW(meshBalance && texBalance && ledgerBalance,
               "W5b no leak: every region-created mesh/texture/body destroyed at teardown");
    }

    // =======================================================================
    // SEAM 3 — the CANON master-world graph (regions.canon.json). canonWorld
    // builds the real tower itself, so the canon graph drops spire_f1 and the
    // canon host streams the remaining lanes AROUND the building. These checks
    // pin the canon wiring contract at the streamer level (the host-level
    // proof is the canon screenshots + smoketest).
    // =======================================================================
    {
        WorldRegionGraph cgraph;
        std::vector<std::string> cerrs;
        const bool cloaded = cgraph.load(worldRegionsCanonJsonPath(), cerrs);
        for (const std::string& e : cerrs) x3::logError("[worldstream-test] " + e);
        const bool noSpire = cloaded && cgraph.indexOf("spire_f1") < 0;
        checkW(cloaded && noSpire && cgraph.indexOf("city") >= 0 &&
               cgraph.indexOf("ocean_base") >= 0 && cgraph.indexOf("surface_landmarks") >= 0,
               "C0 canon graph parses: city/ocean_base/surface_landmarks, NO spire_f1");
        if (cloaded) {
            CountingDevice cdev;
            Scene cscene;
            WorldStreamer cws;
            cws.init(cgraph, jobs.get());
            cws.setRealizedRoomTag(kStreamedExteriorRoom);   // the canon host contract
            // C5 (LIVING NPCs): a host-hooked city crowd must be LEDGER-OWNED —
            // built inside the realize capture (so W5-style teardown owns it) and
            // abandon()ed by the teardown hook before any slot release. This is
            // the exact wiring the canon host uses for the street crowds.
            CrowdSystem hookCrowd;
            uint32_t hookBuilds = 0, hookTeardowns = 0;
            cws.setRegionHooks(
                [&](const WorldRegionDesc& rd, Scene& s, x3::rhi::IRenderDevice& d,
                    x3::phys::IPhysicsWorld&) {
                    if (rd.id != "city") return;
                    ++hookBuilds;
                    CrowdConfig hcfg;
                    hcfg.count = 6;
                    hcfg.centerX = rd.anchor[0]; hcfg.centerZ = rd.anchor[2];
                    hcfg.radius = 30.0f;
                    hcfg.roomId = kStreamedExteriorRoom;
                    CrowdWorkPoint wp; wp.kind = CrowdWorkPoint::Kind::Carry;
                    wp.ax = rd.anchor[0] - 6.0f; wp.az = rd.anchor[2];
                    wp.bx = rd.anchor[0] + 6.0f; wp.bz = rd.anchor[2];
                    hcfg.work = { wp };
                    CrowdPlaySpot psp; psp.cx = rd.anchor[0]; psp.cz = rd.anchor[2] + 10.0f;
                    psp.players = 3; psp.ball = true;
                    hcfg.play = { psp };
                    hookCrowd.build(hcfg, s, d);
                },
                [&](const WorldRegionDesc& rd) {
                    if (rd.id != "city") return;
                    ++hookTeardowns;
                    hookCrowd.abandon();
                });
            // Boot at the canon tower center (REAL facade footprint x[-3..47],
            // z[-34.5..55.5] at base Y=-2 — from the SEAM 2 exterior build log).
            const float towerCx = 22.0f, towerCz = 10.5f;
            cws.buildStartRegions(cscene, cdev, *physics, towerCx, 0.0f, towerCz);
            const int ciCity = cws.indexOf("city");
            const int ciSurf = cws.indexOf("surface_landmarks");
            const int ciOcn  = cws.indexOf("ocean_base");
            // The tower center sits INSIDE the city residency footprint (anchor
            // (-200,425) r750 => ~481 m out), so the city legitimately boots
            // resident alongside surface_landmarks — the canon host pays that
            // cost on the loading screen, never as a first-frame hitch.
            checkW(ciCity >= 0 && ciSurf >= 0 && ciOcn >= 0 &&
                   cws.state(ciCity) == RegionState::Resident &&
                   cws.state(ciSurf) == RegionState::Resident &&
                   cws.state(ciOcn)  == RegionState::Unloaded,
                   "C1 canon boot at the tower: city+surface_landmarks resident, ocean NOT");
            // C2: every realized entity carries the exterior room tag (the PVS gate).
            {
                bool tagged = ciCity >= 0 && cws.ownedEntityCount(ciCity) > 0;
                for (int ri : { ciCity, ciSurf })
                    if (ri >= 0)
                        for (uint32_t id : cws.ownedEntities((uint32_t)ri))
                            if (cscene.get(id).roomId != kStreamedExteriorRoom) tagged = false;
                checkW(tagged, "C2 realized entities stamped kStreamedExteriorRoom (canon PVS gate)");
            }
            // C3: NO streamed entity AABB inside the canon tower interior. The
            // Crash Site was relocated off the origin for exactly this (risk 1)
            // and the city's Spire-approach road now ends at the apron edge
            // (z=80), outside the facade. Interior test box = the REAL facade
            // footprint x[-3..47] z[-34.5..55.5], shrunk 1 m, from below the
            // base (-2) up through the tower.
            {
                bool clear = true;
                std::string offender;
                const float bx0 = -2.0f, bx1 = 46.0f, bz0 = -33.5f, bz1 = 54.5f;
                const float by0 = -3.5f, by1 = 100.0f;
                for (int ri : { ciCity, ciSurf }) {
                    if (ri < 0) continue;
                    for (uint32_t id : cws.ownedEntities((uint32_t)ri)) {
                        const Entity& e = cscene.get(id);
                        float mn[3], mx[3];
                        if (!e.mesh.valid() || !cdev.meshBounds(e.mesh, mn, mx)) continue;
                        // Builder props bake world positions into the verts and keep
                        // an identity transform; apply the translation anyway.
                        const float tx = e.transform[12], ty = e.transform[13], tz = e.transform[14];
                        const float ex0 = mn[0] + tx, ex1 = mx[0] + tx;
                        const float ey0 = mn[1] + ty, ey1 = mx[1] + ty;
                        const float ez0 = mn[2] + tz, ez1 = mx[2] + tz;
                        if (ex1 > bx0 && ex0 < bx1 && ez1 > bz0 && ez0 < bz1 &&
                            ey1 > by0 && ey0 < by1) {
                            clear = false;
                            char ob[128];
                            std::snprintf(ob, sizeof(ob), "region %d entity %u AABB (%.0f..%.0f, %.0f..%.0f, %.0f..%.0f)",
                                          ri, id, ex0, ex1, ey0, ey1, ez0, ez1);
                            offender = ob;
                        }
                    }
                }
                if (!clear) x3::logError("[worldstream-test] interior intrusion: " + offender);
                checkW(clear, "C3 no streamed entity AABB inside the canon tower interior");
            }
            // C4: the boot-resident regions own ZERO physics bodies (the canon
            // interior's collision/queries can never hit streamed content).
            checkW(cws.bodiesCreated() == 0,
                   "C4 streamed regions own zero physics bodies at boot");
            // C5 (LIVING NPCs): the hooked crowd built inside the city realize;
            // every one of its entities is in the CITY ledger (captured + tagged).
            {
                bool inLedger = hookBuilds == 1 && hookCrowd.built() &&
                                hookCrowd.agentCount() == 6 && ciCity >= 0;
                if (inLedger) {
                    const std::vector<uint32_t>& owned = cws.ownedEntities((uint32_t)ciCity);
                    auto ledgered = [&](uint32_t id) {
                        return std::find(owned.begin(), owned.end(), id) != owned.end();
                    };
                    for (uint32_t i = 0; i < hookCrowd.agentCount(); ++i)
                        if (!ledgered(hookCrowd.agent(i).entity)) inLedger = false;
                    for (uint32_t i = 0; i < hookCrowd.crateCount(); ++i)
                        if (!ledgered(hookCrowd.crate(i).entity)) inLedger = false;
                    for (uint32_t i = 0; i < hookCrowd.ballCount(); ++i)
                        if (hookCrowd.ball(i).entity != kNoLink &&
                            !ledgered(hookCrowd.ball(i).entity)) inLedger = false;
                }
                checkW(inLedger,
                       "C5 hooked city crowd is ledger-owned (agents+crate+ball captured)");
            }
            cws.shutdown(cscene, cdev, *physics);
            // C5b: the teardown hook fired before slot release (the crowd
            // abandoned its ids) and the crowd's meshes died with the region —
            // the device's create/destroy counts balance (zero leak).
            checkW(hookTeardowns == 1 && !hookCrowd.built() &&
                   hookCrowd.agentCount() == 0 &&
                   cdev.meshesCreated > 0 && cdev.meshesCreated == cdev.meshesDestroyed,
                   "C5b region teardown abandons the crowd + leaks no meshes");
        }
    }

    physics->shutdown();
    jobs->shutdown();

    x3::logInfo("[worldstream-test] " + std::to_string(gw_pass) + " passed, " +
                std::to_string(gw_fail) + " failed");
    return gw_fail == 0;
}

} // namespace x3::game
