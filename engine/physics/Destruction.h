#pragma once
// Destruction system — Subsystem K, tiers T0 (foundation) + T1 (Jolt CPU
// destruction). Spec: specs/K-gpu-destruction.spec.md (§2, §4, §15).
//
// CLEAN-ROOM: built only from the K spec + the engine's own IPhysicsWorld
// interface + public Jolt docs. NO id Tech / RBDOOM source. No JPH:: types here.
//
// WHAT THIS IS (T1): destructible objects that break into convex chunks on impact
// / weapon-hit / explosion, with split linear + angular velocities. The intact
// object is ONE dynamic compound body (the chunks as a StaticCompoundShape); on
// break the parent is removed and each chunk is re-spawned as its own dynamic
// convex body inheriting the parent motion plus an impact/radial kick + spin.
//
// CRITICAL SAFETY RULE (spec §4b): physics is NEVER mutated inside the contact
// callback (bodies are locked there). Break requests are QUEUED and processed once
// per step, AFTER IPhysicsWorld::step() has returned + drained its contacts. This
// manager registers IPhysicsWorld::setContactCallback and only enqueues; the real
// fracture happens in update().
//
// RENDER DECOUPLING: this engine module owns NO render types. The host (app) gets
// told about chunk lifetimes through POD callbacks (ChunkSpawnFn / ChunkDespawnFn)
// and reads live chunk transforms each frame via forEachActiveChunk(), so it can
// draw a small box/convex mesh per chunk through the existing mesh path. The
// intact, un-broken object is reported the same way (it is just chunk slot(s)
// belonging to a not-yet-broken destructible).
//
// DEFERRED (later tiers, noted not built): T2 = GPU-compute debris world (SSBOs +
// indirect draw + CPU->GPU handoff of small/sleeping pieces); T3 = structural
// connectivity / progressive collapse + nested fracture + 1M+ GPU debris.

#include "IPhysicsWorld.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace x3::phys {

// Opaque ids (spec §2).
using DestructibleId  = uint32_t;
using FractureAssetId = uint32_t;
constexpr uint32_t kInvalidId = 0;

// One convex chunk of a fracture asset. A chunk is EITHER an explicit convex hull
// (hullPoints != null, pointCount >= 4) OR an axis-aligned box (halfExtents) when
// hullPoints is null. localOffset/localRot place it inside the intact object;
// `mass` is its dynamic mass once it becomes its own body. localRot is (x,y,z,w).
struct FractureChunkDesc {
    const float* hullPoints = nullptr;   // n*3 local xyz; null => use box halfExtents
    uint32_t     pointCount = 0;
    float        halfExtents[3] = { 0.25f, 0.25f, 0.25f }; // used iff hullPoints == null
    float        localOffset[3] = { 0, 0, 0 };
    float        localRot[4]    = { 0, 0, 0, 1 };          // (x,y,z,w)
    float        mass           = 1.0f;
};

// A fracture asset = a set of convex chunks + break thresholds (spec §2 / §15).
struct FractureAssetDesc {
    const FractureChunkDesc* chunks = nullptr;
    uint32_t                 chunkCount = 0;
    float breakImpulse = 15.0f;   // contact-impulse magnitude that triggers a break
    float breakRelVel  = 8.0f;    // relative approach speed that triggers a break
};

// Emitted (queued, drained via drainBreakEvents) when an object breaks.
struct BreakEvent {
    DestructibleId id = kInvalidId;
    float worldPos[3] = { 0, 0, 0 };
    float impulse     = 0.0f;
    uint32_t childCount = 0;
};

// Tunables (spec §15). Defaults are the spec start values.
struct DestructionTuning {
    float    childImpactFactor = 0.4f;  // child linear += impactVel * this
    float    angularScale      = 8.0f;  // child spin scale from the impact lever arm
    float    radialFactor      = 1.0f;  // explosion: impulse/mass * this -> outward vel
    uint32_t maxActiveChunks   = 256;   // global cap on live chunk bodies (recycle oldest)
    float    chunkDespawnTime   = 12.0f; // a free (broken-off) chunk despawns after this many s
    float    sleepDespawnSpeed  = 0.05f; // a chunk slower than this is eligible for early recycle
};

// ---------------------------------------------------------------------------
// Procedural fracture authoring (T1): split a box into an Nx*Ny*Nz grid of convex
// box chunks (no external Voronoi tool needed). Real Blender Cell-Fracture assets
// can feed loadFractureAsset() through the same FractureAssetDesc path later.
// `half*` are the intact box half-extents; `n*` are the cell counts per axis
// (each >= 1). `chunkMass` is the per-chunk mass. Fills `outChunks` (cleared
// first). Returns the chunk count. Coplanar boxes are fine — chunks stay boxes.
// ---------------------------------------------------------------------------
uint32_t makeGridFractureChunks(float halfX, float halfY, float halfZ,
                                uint32_t nx, uint32_t ny, uint32_t nz,
                                float chunkMass,
                                std::vector<FractureChunkDesc>& outChunks);

// A live chunk reported to the host for rendering. `bodyId` lets the host correlate
// frame-to-frame; `xform` is the column-major 4x4 world transform of THIS chunk;
// `halfExtents` is the chunk's box size (for a box mesh); `intact` is true while
// the chunk still belongs to an un-broken object (drawn as part of the whole).
struct ChunkView {
    BodyId         body;
    DestructibleId owner = kInvalidId;
    float          xform[16];
    float          halfExtents[3];
    bool           intact = false;
};

// Host callbacks (POD only). spawn: a new chunk body now exists (host may create a
// render mesh keyed by bodyId). despawn: a chunk body is gone (host frees its mesh).
using ChunkSpawnFn   = std::function<void(const ChunkView&)>;
using ChunkDespawnFn = std::function<void(BodyId)>;

// ---------------------------------------------------------------------------
// DestructibleManager (spec §4a). Owns the destructibles + their chunks, drives
// break detection (queued) + fracture spawning (post-step), and enforces the
// active-chunk cap. Built on IPhysicsWorld only — engine-pure.
// ---------------------------------------------------------------------------
class DestructibleManager {
public:
    DestructibleManager() = default;
    ~DestructibleManager();

    // Bind to a physics world + install the queued contact callback. Must be called
    // once before spawn/update. The world must outlive the manager.
    void init(IPhysicsWorld* world, const DestructionTuning& tuning = {});
    void shutdown();

    void setTuning(const DestructionTuning& t) { m_tuning = t; }
    const DestructionTuning& tuning() const { return m_tuning; }

    // Optional render hooks. Pass null to ignore (headless tests do).
    void setChunkCallbacks(ChunkSpawnFn onSpawn, ChunkDespawnFn onDespawn);

    // Register a fracture asset (offline-authored chunk set). Returns an id.
    FractureAssetId loadFractureAsset(const FractureAssetDesc&);

    // Spawn a destructible at a column-major 4x4 world transform. The intact object
    // is one dynamic compound body. Returns its id (kInvalidId on failure).
    DestructibleId spawnDestructible(FractureAssetId asset, const float xform[16]);

    // Remove a destructible + all its chunks (intact or broken).
    void despawn(DestructibleId);

    // --- Explicit (non-contact) break triggers (spec §4b) ---
    // Weapon ray/shape hit: break the nearest destructible whose intact body the ray
    // (from `point` along `dir`) hits within reach, kicked by `strength`. Returns
    // true iff something broke. SAFE to call any time (queues the break).
    bool applyHit(const float point[3], const float dir[3], float strength);
    // Explosion: break every intact destructible within `radius` of `center`,
    // kicked outward scaled by `strength`. SAFE any time (queues breaks).
    void applyRadialImpulse(const float center[3], float radius, float strength);

    // Advance one step. Call AFTER IPhysicsWorld::step(). Processes queued break
    // requests (the only place fracture mutates physics), ages free chunks, and
    // enforces the active-chunk cap. `dt` ages despawn timers.
    void update(float dt);

    // Drain queued break events (for HUD/FX/audio/log). Returns the number written.
    uint32_t drainBreakEvents(BreakEvent* out, uint32_t maxOut);

    // Live chunk iteration for rendering. Calls `fn(ChunkView)` for every active
    // chunk body (intact-object chunks AND free debris chunks), reading the live
    // transform from the physics world. No allocation.
    void forEachActiveChunk(const std::function<void(const ChunkView&)>& fn) const;

    // --- Queries (HUD / tests) ---
    uint32_t activeChunkCount() const { return (uint32_t)m_chunks.size(); }
    uint32_t destructibleCount() const;
    bool     isBroken(DestructibleId) const;
    // The intact compound body of a destructible (invalid once broken / unknown).
    BodyId   intactBodyOf(DestructibleId) const;

    // Contact-callback safety probe (test (c)): true iff the manager EVER attempted
    // to mutate physics while inside the locked contact callback. MUST stay false.
    bool sawMutationInCallback() const { return m_mutationInCallback; }

private:
    struct FractureAsset {
        std::vector<FractureChunkDesc> chunks;     // owns copies of the descs
        std::vector<std::vector<float>> hullData;  // owns hull point arrays (descs point in)
        float breakImpulse = 15.0f;
        float breakRelVel  = 8.0f;
    };

    // A live chunk (either part of an intact compound, or free debris).
    struct Chunk {
        BodyId         body;
        DestructibleId owner = kInvalidId;
        float          halfExtents[3] = { 0.25f, 0.25f, 0.25f };
        float          mass = 1.0f;
        bool           free = false;     // true once broken off (own body, debris)
        float          age  = 0.0f;      // seconds since freed (despawn timer)
    };

    struct Destructible {
        FractureAssetId asset = kInvalidId;
        BodyId          intactBody;       // the compound parent (valid until broken)
        bool            broken = false;
        float           xform[16];        // last known intact transform (for spawn)
        std::vector<uint32_t> chunkSlots; // indices into m_chunks for this object
    };

    struct BreakRequest {
        DestructibleId id = kInvalidId;
        float impactPoint[3] = { 0, 0, 0 };
        float impactVel[3]   = { 0, 0, 0 };   // velocity to impart (impact dir * strength)
        float impulse        = 0.0f;
    };

    // The C-style contact callback trampolines here (post-step, mutation-safe path),
    // but we ONLY enqueue from it to keep the break-processing single-pathed.
    static void contactTrampoline(BodyId a, BodyId b, const float point[3],
                                  const float normal[3], float impulse, void* user);
    void onContact(BodyId a, BodyId b, const float point[3],
                   const float normal[3], float impulse);

    void enqueueBreak(const BreakRequest&);
    void processBreaks();                 // the ONLY place that fractures (post-step)
    void breakObject(Destructible&, const BreakRequest&);
    void enforceChunkCap();
    void ageAndReap(float dt);
    void freeChunkSlot(uint32_t slot);    // remove a chunk body + notify host

    DestructibleId destructibleOfBody(BodyId) const;

    IPhysicsWorld*    m_world = nullptr;
    DestructionTuning m_tuning;

    std::vector<FractureAsset>  m_assets;        // [id-1]
    std::vector<Destructible>   m_objects;        // [id-1]
    std::vector<Chunk>          m_chunks;         // active chunk bodies (compact)

    // body.id (intact parent) -> destructible id, for the contact callback's fast
    // "is this a destructible?" check (mirrors the body user-data marker).
    std::vector<std::pair<uint32_t, DestructibleId>> m_bodyToObject;

    std::vector<BreakRequest> m_pendingBreaks;   // queued in callback; drained post-step
    std::vector<BreakEvent>   m_events;          // outgoing break events

    ChunkSpawnFn   m_onSpawn;
    ChunkDespawnFn m_onDespawn;

    // Recycle order for the cap (oldest-first ring of free-chunk slots).
    uint32_t m_recycleCursor = 0;

    // Safety: set true if a mutation is ever attempted during the locked callback.
    bool m_inLockedCallback   = false;
    bool m_mutationInCallback  = false;

    // User-data marker stamped on intact destructible bodies (spec §4a).
    static constexpr uint64_t kDestructibleMarker = 0xD3'57'02'C7'1B'1E'00'01ull;
};

// Headless self-test (--test-destruction). Drives the four required cases:
//  (a) impact/hit above threshold -> parent removed + N chunks with split lin+ang vel
//  (b) below threshold -> no break
//  (c) contact-callback safety: breaks are QUEUED + applied post-step, ZERO mutation
//      inside the locked callback (asserted)
//  (d) chunk cap respected + chunks despawn/recycle (bounded, no leak)
// Returns true iff all pass. No window / Vulkan. Mirrors runPhysicsSelfTest().
bool runDestructionSelfTest();

} // namespace x3::phys
