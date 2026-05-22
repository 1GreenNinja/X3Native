#pragma once
// Structural collapse (support graph) — Subsystem K, tier T3 (clean-room).
// Spec: specs/K-gpu-destruction.spec.md (§8 structural connectivity / progressive
// collapse, §12 acceptance test T10). Built ONLY from the K spec + the engine's own
// IPhysicsWorld interface + the IRenderDevice GPU-debris API + public refs (Red
// Faction "Geo-Mod" / Teardown structural-integrity talks, connected-components /
// BFS). NO id Tech / RBDOOM source. No JPH:: / Vulkan types here.
//
// WHAT THIS IS (T3): a destructible *structure* is a GRAPH of connected pieces with
// support relationships. Ground-anchored pieces are stable; every other piece is
// supported (transitively) through neighbor links down to an anchor. When pieces are
// destroyed (by the existing K-T0/T1 hit / impact / explosion path, or directly),
// we recompute support REACHABILITY from the anchors; any piece no longer
// transitively connected to an anchor has lost support and COLLAPSES — it is
// converted from a static (load-bearing) Jolt body into a DYNAMIC Jolt rigid body
// that falls/topples under gravity. This is the Red Faction Geo-Mod / Teardown-class
// "buildings sag and collapse rather than float" behavior.
//
// HOW IT REUSES EXISTING WORK (do NOT reinvent):
//  - Each intact piece is a STATIC box body in IPhysicsWorld (mass 0, Layer::Static)
//    — load-bearing, doesn't move. On collapse the static body is removed and a
//    DYNAMIC convex body is spawned at the same pose with inherited gravity + a small
//    settling kick (the SAME chunk-body path the K-T1 fracture uses: addConvexHull ->
//    addBodyFromShape Layer::Dynamic -> set velocities).
//  - Secondary fine debris from a collapse feeds the K-T2 GPU debris pool via the
//    existing IRenderDevice::gpuDebrisSpawnBurst — large collapses stay cheap (the
//    big falling pieces are the few Jolt bodies; the dust/rubble is GPU compute).
//  - Render: collapsing pieces are reported through the same POD ChunkView callback /
//    forEachActivePiece() iteration the destruction demo already draws, so NO new
//    renderer code is needed (the host draws a scaled cube per piece).
//
// DETERMINISM + BOUNDS: the support pass is a plain BFS over a fixed adjacency graph
// (deterministic order); it runs ONLY on a destroy/break event (amortized, throttled
// to one pass per update), and the number of pieces is bounded by the structure size.
// Collapsed pieces age + recycle exactly like K-T1 debris (no unbounded growth, no
// per-frame heap alloc beyond the one-time graph build). Runs on a GTX 1080 Ti with
// plain Jolt + the K-T2 compute path (no hardware ray tracing).

#include "IPhysicsWorld.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace x3::phys {

// Opaque id for a piece within a Structure (1-based; 0 == invalid).
using PieceId = uint32_t;
constexpr PieceId kInvalidPiece = 0;

// One piece of a structure: an axis-aligned box at a world position, with a mass it
// gets once it becomes a dynamic falling body. `anchored` marks a piece that is
// fixed to the ground/world (a foundation) — it is ALWAYS stable and is the root of
// support reachability. Local rotation is identity (boxes); the collapse path still
// carries full rotation so toppling pieces tumble.
struct PieceDesc {
    float center[3]     = { 0, 0, 0 };          // world-space piece center
    float halfExtents[3]= { 0.5f, 0.5f, 0.5f }; // box half-size
    float mass          = 1.0f;                 // dynamic mass once it falls
    bool  anchored      = false;                // true => ground/foundation (always stable)
};

// Connectivity / support edge: pieces a and b touch (load can transfer between
// them). Undirected. Indices are 0-based into the PieceDesc array passed to build().
struct PieceLink { uint32_t a = 0; uint32_t b = 0; };

// Tunables for the collapse behavior. Defaults follow the K spec §15 scale.
struct CollapseTuning {
    float    collapseKick      = 1.2f;   // small outward/lateral nudge on a freed piece (m/s)
    float    collapseSpin      = 0.6f;   // small random-ish spin so pieces topple (rad/s)
    float    settleSpeed       = 0.04f;  // a piece slower than this is eligible for early reap
    float    pieceDespawnTime  = 14.0f;  // a collapsed piece despawns after this many s
    uint32_t maxActivePieces   = 512;    // cap on live dynamic (collapsed) bodies; recycle oldest
    // K-T2 GPU debris coupling: per collapsed piece, spawn this many fine fragments.
    uint32_t debrisPerPiece    = 24;     // 0 disables the GPU-debris coupling
    float    debrisSpeed       = 2.5f;   // fragment outward speed
    float    debrisLifetime    = 4.0f;   // fragment life (s)
    float    debrisHalfExtent  = 0.06f;  // fragment size
};

// A live piece reported to the host for rendering (POD only). Mirrors the K-T1
// ChunkView so the same host draw path works. `xform` is column-major translate*rot;
// `stable` is true while the piece is still load-bearing (static), false once it has
// collapsed into a falling dynamic body.
struct PieceView {
    BodyId  body;
    PieceId piece = kInvalidPiece;
    float   xform[16];
    float   halfExtents[3];
    bool    stable = true;
    bool    anchored = false;
};

// Host callbacks (POD only). spawn: a piece body now exists / changed body (host may
// (re)create a render handle keyed by bodyId). despawn: a piece body is gone.
using PieceSpawnFn   = std::function<void(const PieceView&)>;
using PieceDespawnFn = std::function<void(BodyId)>;

// GPU-debris burst hook (POD). Lets the structure feed the K-T2 pool WITHOUT this
// engine module depending on IRenderDevice (keeps engine/ render-free). The host
// wires it to IRenderDevice::gpuDebrisSpawnBurst. Args: world pos, count, speed,
// lifetime, fragment half-extent, seed. Null => no GPU debris (headless box test
// asserts the call count instead).
using DebrisBurstFn = std::function<void(const float pos[3], uint32_t count, float speed,
                                         float lifetime, float halfExtent, uint32_t seed)>;

// ---------------------------------------------------------------------------
// Structure — a graph of connected pieces with support relationships, driving
// progressive structural collapse. Built on IPhysicsWorld only (engine-pure).
//
// Lifecycle:
//   init(world, tuning); build(pieces, links);     // creates static load-bearing bodies
//   ... destroyPiece(id) / on a K-T1 break / explosion ...
//   step(dt);   // recomputes support, collapses unsupported pieces, ages debris
//   forEachActivePiece(fn);   // host draws
//   shutdown();
// ---------------------------------------------------------------------------
class Structure {
public:
    Structure() = default;
    ~Structure();

    // Bind to a physics world. The world must outlive the structure.
    void init(IPhysicsWorld* world, const CollapseTuning& tuning = {});
    void shutdown();

    void setTuning(const CollapseTuning& t) { m_tuning = t; }
    const CollapseTuning& tuning() const { return m_tuning; }

    // Render + debris hooks (pass null to ignore; headless tests do for render).
    void setPieceCallbacks(PieceSpawnFn onSpawn, PieceDespawnFn onDespawn);
    void setDebrisBurstFn(DebrisBurstFn fn) { m_debrisBurst = std::move(fn); }

    // Build the structure: create one STATIC load-bearing box body per piece and
    // record the adjacency graph. Returns the piece count (0 on failure). Call once
    // after init(). `links` are undirected touch/support edges (0-based indices).
    uint32_t build(const std::vector<PieceDesc>& pieces,
                   const std::vector<PieceLink>& links);

    // Destroy a piece NOW (e.g. shot out a support). The piece body is removed; its
    // graph edges are cut. A support recompute is requested for the next step().
    // SAFE any time. Returns true iff the piece existed + was removed.
    bool destroyPiece(PieceId id);

    // Destroy every intact piece whose center is within `radius` of `center`
    // (explosion). Requests a support recompute. SAFE any time. Returns the count.
    uint32_t destroyInRadius(const float center[3], float radius);

    // Per-frame advance. Call AFTER IPhysicsWorld::step(). If a destroy happened
    // since the last call, recomputes support reachability from anchors (one BFS)
    // and collapses every piece no longer connected to an anchor (static -> dynamic
    // falling body + a GPU-debris burst). Always ages + reaps collapsed debris.
    void step(float dt);

    // Live piece iteration for rendering (stable static pieces AND falling collapsed
    // pieces), reading live transforms from the physics world. No allocation.
    void forEachActivePiece(const std::function<void(const PieceView&)>& fn) const;

    // --- Queries (HUD / tests) ---
    uint32_t pieceCount() const { return (uint32_t)m_pieces.size(); }
    uint32_t stableCount() const;       // pieces still load-bearing (static)
    uint32_t collapsedCount() const;    // pieces converted to falling dynamic bodies
    uint32_t destroyedCount() const;    // pieces fully removed (shot out / reaped)
    bool     isStable(PieceId id) const;
    bool     isCollapsed(PieceId id) const;
    // The live body of a piece (invalid if destroyed/unknown).
    BodyId   bodyOf(PieceId id) const;
    // Number of GPU-debris bursts emitted so far (test probe; counts even with a
    // null DebrisBurstFn so the headless test can assert the coupling fired).
    uint32_t debrisBurstCount() const { return m_debrisBurstCount; }
    uint32_t debrisFragmentsRequested() const { return m_debrisFragmentsRequested; }

private:
    enum class State : uint8_t { Stable, Collapsed, Destroyed };

    struct Piece {
        BodyId  body;                          // static (Stable) or dynamic (Collapsed)
        PieceDesc desc;
        State   state = State::Stable;
        float   age   = 0.0f;                  // seconds since collapsed (despawn timer)
    };

    // Convert a Stable piece into a falling Dynamic body (the K-T1 chunk-body path).
    void collapsePiece(uint32_t idx);
    // Recompute support reachability from anchors; collapse any unreachable Stable
    // piece. The ONLY structural-mutation path (post-step, mutation-safe).
    void recomputeSupport();
    // Age collapsed pieces + reap old ones; enforce the active cap (recycle oldest).
    void ageReapAndCap(float dt);
    // Remove a piece's body + notify host (used by destroy + reap). Marks Destroyed.
    void removePieceBody(uint32_t idx);
    // Emit the K-T2 GPU-debris burst for a collapsed piece (counts even if no fn).
    void emitDebris(const float pos[3]);

    IPhysicsWorld* m_world = nullptr;
    CollapseTuning m_tuning;

    std::vector<Piece>                 m_pieces;     // [id-1]
    std::vector<std::vector<uint32_t>> m_adj;        // adjacency list (0-based, parallel to m_pieces)

    PieceSpawnFn   m_onSpawn;
    PieceDespawnFn m_onDespawn;
    DebrisBurstFn  m_debrisBurst;

    bool     m_supportDirty = false;     // a destroy happened -> recompute next step
    uint32_t m_debrisBurstCount = 0;
    uint32_t m_debrisFragmentsRequested = 0;
    uint32_t m_seed = 0x5C0117A1u;       // deterministic per-burst seed advance

    // Scratch reused by the BFS so recompute does no per-call heap alloc after warmup.
    mutable std::vector<uint8_t>  m_reach;   // 1 == reachable from an anchor
    mutable std::vector<uint32_t> m_bfs;     // BFS frontier queue

    static constexpr uint64_t kStructureMarker = 0x57'52'07'C7'1B'1E'00'02ull;
};

// Headless self-test (--test-collapse). Builds small structures (a column/stack and a
// beam on two supports), destroys a base/support piece, steps the sim, and asserts:
//  - the dependent (now-unsupported) pieces collapse to dynamic + FALL (Y drops /
//    they leave their start pose),
//  - pieces still connected to an anchor remain STABLE,
//  - the rubble settles (no NaNs, bounded),
//  - GPU-debris was emitted on collapse,
//  - everything is leak-clean (all bodies removed on shutdown).
// Prints "collapse: X/Y passed". Returns true iff all pass. No window / Vulkan.
bool runCollapseSelfTest();

} // namespace x3::phys
