// app/space/space_layer.h
//
// S0 SpaceLayer — the spine of the Act-3 space engine.
//
// Owns the current rendering/gameplay Context, drives the staged
// transitions (wormhole / atmospheric descent / ascent) via runner
// callbacks registered by the transition subsystems (S3/S4), integrates
// the moving-environment transform (the ship is static at origin and the
// environment carries the motion — design decision 2.4), and holds the
// scaled-Proxy registry that S1 (env) populates and S2/S6 consume.
//
// This interface is FROZEN per the space-engine design spec §3 — every
// other lane compiles against it, so the signatures below must not drift.
//
// Headless logic only: the spine itself needs no GPU.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace x3::space {

// The five mutually-exclusive states the space engine can be in. The active
// Context decides which ship representation (moving exterior vs. static
// interior), which world, and which subsystems are live.
enum class Context {
    DeepSpace,       // free flight in a star system; combat happens here (real moving ship)
    WormholeTransit, // autopilot; player walks the static ship interior
    EVA,             // ship at rest/drifting; player free-floats outside on the hull (repairs)
    AtmoDescent,     // on-rails orbit->ground cinematic
    Surface,         // handed off to a --world (open-world hub or arena)
};

// Scaled-proxy descriptor for anything visible through windows / at range.
// A real object, but LOD'd hard and placed in scaled scene-space — NOT at
// true planetary scale. This is the only thing S1/S6/S2 exchange about
// "stuff outside."
struct Proxy {
    enum class Kind { Planet, Ship, Station, Asteroid, Wormhole } kind;
    float pos[3];      // scene-space position (scaled, not true distance)
    float radius;      // proxy size
    uint32_t lodAsset; // handle into the LOD system (S2)
    float tint[4];     // RGBA tint applied to the proxy
};

class SpaceLayer {
public:
    // Bring the layer up: DeepSpace, identity environment transform, empty
    // proxy registry, no pending transition. No GPU needed for the spine.
    void init();

    Context context() const;

    // ---- Transition requests --------------------------------------------
    // S0 drives the state machine; the transition subsystems (S3/S4) register
    // runner callbacks and run the actual cinematic sequence. A request arms a
    // pending transition: each update(dt) ticks the registered runner until it
    // returns true (complete), then the layer lands in the destination
    // Context. If no runner is registered, the transition completes on the
    // very next update.
    void requestWormhole(uint32_t destSystemId); // -> WormholeTransit -> DeepSpace(dest)
    void requestDescent(uint32_t planetId);      // -> AtmoDescent -> Surface
    void requestAscent();                        // Surface -> DeepSpace

    // ---- Moving-environment model (decision 2.4) ------------------------
    // The ship is static at origin; S0 owns the environment transform that
    // everything-outside is expressed relative to. Window rendering (S6) and
    // the space env (S1) read this. Velocity is in environment-frame units
    // per second; angular velocity is radians per second.
    void setEnvironmentVelocity(const float vel[3], const float angVel[3]);
    void environmentTransform(float out16[16]) const; // column-major 4x4

    // ---- Proxy registry --------------------------------------------------
    // Free-list-backed; ids are stable across the lifetime of a proxy (a
    // removed id is not reused until a fresh add reclaims its slot, and the
    // returned id encodes the slot so accessors stay O(1)). S1 populates;
    // S6 (windows) + S2 (LOD) consume.
    uint32_t addProxy(const Proxy&);
    void     updateProxy(uint32_t id, const Proxy&);
    void     removeProxy(uint32_t id);
    uint32_t proxyCount() const;             // number of LIVE proxies
    const Proxy& proxy(uint32_t i) const;    // i in [0, proxyCount()) — dense live view

    // ---- Per-frame -------------------------------------------------------
    // Advances the active transition (ticks its runner) and integrates the
    // environment transform from the current linear+angular velocity.
    void update(float dt);

    // ---- Transition runner registration ---------------------------------
    // Transition subsystems register their sequence runners here. A runner
    // returns true when its sequence is complete.
    using TransitionFn = std::function<bool(float dt)>;
    void registerWormholeRunner(TransitionFn);
    void registerDescentRunner(TransitionFn);

private:
    // What kind of staged transition is currently in flight (if any).
    enum class Pending { None, Wormhole, Descent, Ascent };

    // Free-list slot for the proxy registry. `live` distinguishes an
    // occupied slot from a hole left by removeProxy().
    struct Slot {
        Proxy proxy{};
        bool  live = false;
    };

    Context context_ = Context::DeepSpace;
    Pending pending_ = Pending::None;

    // Column-major 4x4 environment transform, integrated each update().
    float env_[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float linVel_[3] = { 0, 0, 0 };
    float angVel_[3] = { 0, 0, 0 };

    TransitionFn wormholeRunner_;
    TransitionFn descentRunner_;

    // Free-list proxy storage. `slots_` is append-only (ids index into it);
    // `freeList_` recycles holes; `liveCount_` tracks live proxies; `live_`
    // is the dense list of live slot indices that backs proxy(i)/proxyCount().
    std::vector<Slot>     slots_;
    std::vector<uint32_t> freeList_;
    std::vector<uint32_t> live_;
    uint32_t              liveCount_ = 0;

    // Step the pending transition's runner; land in the destination Context
    // when it completes.
    void tickTransition(float dt);
    // Rebuild the dense `live_` index list after add/remove.
    void rebuildLive();
};

} // namespace x3::space
