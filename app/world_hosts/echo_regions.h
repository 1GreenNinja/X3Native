#pragma once
// ECHOTROPOLIS REGION CORE (TIER 2 streaming, WP-1) — see
// docs/plans/TIER2_STREAMING_PLAN.md §3/§4 for the full architecture this file
// implements exactly. Game/slice code only — engine/ stays pure.
//
// WHY THIS EXISTS (plan §0): Echotropolis draws everything every frame today —
// host-owned EnvArtSystem containers fanned out by hand in host_echotropolis.cpp,
// NOT Scene entities, so app/world_stream.h's Scene-ledger residency engine
// cannot own the content directly. The adoption shape (plan §0): WorldStreamer
// stays the RESIDENCY ENGINE (wants / budget / hysteresis / proxy floor) via its
// `setRegionHooks(onBuild, onTeardown)` contract (world_stream.h ~L164-183);
// this file is the BRIDGE + CONTAINER on the other end of those hooks. Every
// region's content lives in an EchoRegion (EnvArtSystem containers + optional
// per-region Scene + per-region light slice), built/deactivated through the
// bridge below. The Scene-ledger machinery the streamer also offers runs EMPTY
// for every Echotropolis region (Lane A is unused here) — harmless by design.
//
// FILE OWNERSHIP (plan §4, WP-1): this header + echo_regions.cpp are owned
// EXCLUSIVELY by WP-1. No content knowledge lives here — WP-2
// (echo_region_builders.*) supplies the actual `void(EchoRegion&, EchoRegionCtx&)`
// builder functions per region id via EchoRegionSet::registerBuilder(); WP-0 (the
// integrator) is the only writer of host_echotropolis.cpp/CMakeLists.txt and
// wires ctx/streamer/graph/builders together after WP-1..4 land.
//
// *** OPEN INTEGRATION ITEM (read before wiring this up) ***
// EchoRegionCtx::hf below is declared as `Heightfield&` per plan §3 verbatim.
// The ONLY `Heightfield` definition in the repo today (grep-verified) is
// `x3::apphost::{anonymous namespace}::Heightfield` inside host_echotropolis.cpp
// (TU-local, internal linkage — never meant to be shared across translation
// units), and WP-1 is forbidden from editing that file. Per cross-package
// coordination (WP-3 found the same conflict independently): this header
// #includes "echo_heightfield.h", a header WP-1 does NOT own and does NOT
// create — the INTEGRATOR (WP-0) is expected to create it by hoisting the
// host's Heightfield struct verbatim (it is a pure PNG-sampler, no RHI deps)
// out of its anonymous namespace into that shared file, so host_echotropolis.cpp,
// this package, and WP-2/3's builder code all see the SAME x3::game::Heightfield
// type — no duplicate/ODR-colliding definition invented here. Until WP-0 adds
// echo_heightfield.h, this package does not compile standalone on that one
// include — everything else in echo_regions.h/.cpp is independent of it.
// This package never dereferences `hf` itself (no content knowledge); the only
// members any call site in the codebase uses are `hf.ok()` and
// `hf.heightAt(x, z)` (per the coordination note), left to WP-2/3's builders.
// NOTE on include paths: engine/CMakeLists.txt's target_include_directories
// only adds ${CMAKE_SOURCE_DIR} (repo root) PUBLICly, not app/ itself — that's
// why app/world_hosts/host_echotropolis.cpp bare-includes "engine/..." headers
// (repo-root-relative) but needs "../scene.h" / "../env_art.h" (app/-relative)
// for app-level headers. This file lives in app/world_hosts/ too, so it
// follows the same convention. "echo_heightfield.h" is a SIBLING file (also
// under app/world_hosts/, per WP-0), so it stays a bare quoted include.
#include "echo_heightfield.h"
#include "../scene.h"
#include "../env_art.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x3::game {

// Forward declarations only — this header stays independent of world_stream.h's
// full definitions (kept to what the .cpp needs; "no engine/ includes beyond
// what world_stream.h itself uses" per the WP-1 brief, so no IPhysicsWorld.h /
// IJobSystem.h are pulled in here — echo_regions.cpp includes world_stream.h
// directly for the complete types its bridge implementation needs).
class WorldStreamer;
struct WorldRegionGraph;
struct WorldRegionDesc;

// Heightfield itself comes from "echo_heightfield.h" (included above) — see the
// "OPEN INTEGRATION ITEM" header comment. Not dereferenced anywhere in this
// package.

// ---------------------------------------------------------------------------
// EchoRegionCtx — everything a region BUILDER needs, resolved ONCE by the
// integrator and threaded through by reference (plan §3 verbatim). Reference
// members mean this is built in place by WP-0 and never copied/reassigned;
// EchoRegionSet caches a pointer to the instance it's init()'d with (see
// EchoRegionSet::m_ctx) so later calls that don't carry a ctx parameter of
// their own (setVistaMode, drawAll's `gate` argument) still have one to use.
// ---------------------------------------------------------------------------
struct EchoRegionCtx {
    x3::rhi::IRenderDevice& device;
    Heightfield&            hf;         // opaque here — see header note above
    Scene&                  walkScene;  // for systems that need it (persistent Lane C scene)
    // Asset roots (island/models/districts dirs, assetRoot()) resolved once by
    // the integrator — plain data, no behavior. WP-1 does not interpret these.
    std::string modelsDir, districtsTxt, vegDir, houseForgeDir, cityDir;
    // #34a CORRIDOR AUDIT: the road network, when the integrator built it
    // before the regions (echotropolis does since V7.2). Builders consult
    // EchoRoads::corridorHits so placements never spawn inside a road/deck
    // corridor. Null => no audit (legacy order / roads failed) — placements
    // behave exactly as before.
    const class EchoRoads*  roads = nullptr;
};

// ---------------------------------------------------------------------------
// EchoRegion — one region's hook-owned content (plan §2 Lane B): a set of
// EnvArtSystem containers, an optional per-frame pose update (boats, drones,
// ...), an optional whole draw-gated Scene (lampScene/mineGlowScene-style), and
// a per-region point-light slice (feeds EchoRegionSet::appendNearLights).
//
// RESIDENCY CONTRACT: draw()/update() are no-ops whenever !resident() — the
// "gating on residency" the WP-1 brief calls for. build() is the ONLY way to
// flip resident() true; it is IDEMPOTENT for content creation (see .cpp): the
// registered builder callback runs exactly ONCE per EchoRegion's lifetime (or
// once again after a true destroy()), so a region that streams back in after a
// deactivate()-only eviction (plan M-A..C: "VRAM retained, records gone — the
// actual perf win") does NOT rebuild/duplicate its EnvArtSystems — it just
// resumes drawing the ones it already has.
// ---------------------------------------------------------------------------
class EchoRegion {
public:
    // Register content + per-frame hooks. Call these from inside a builder
    // (invoked by build(), below) to populate the region. draw() is skipped
    // when !resident().
    void addArt(std::unique_ptr<EnvArtSystem> e);
    void setUpdate(std::function<void(float dt, float t)> fn);   // poses (boats…)
    void setScene(Scene* s);            // optional draw-gated whole Scene (lampScene); NOT owned
    // Appends (does not replace) — plan §2 risk #4: districts/etc. harvest their
    // own light slice, and a region may accumulate lights from more than one
    // sub-builder call (mirrors the host's per-district districtLights.push_back,
    // host_echotropolis.cpp:1813, but scoped to this region instead of one
    // global vector).
    void addLights(std::vector<x3::rhi::PointLight> l);

    // Ensure this region is built + resident (see class doc: idempotent content
    // creation). `builder` is looked up by id and supplied by EchoRegionSet; a
    // null/empty builder is a no-op that still flips resident() true (nothing to
    // build, e.g. a misconfigured id — EchoRegionSet logs that case instead).
    void build(EchoRegionCtx& ctx, const std::function<void(EchoRegion&, EchoRegionCtx&)>& builder);
    // Stop draw()/update() (residency -> false). Content is NOT freed — EnvArt
    // has no GPU teardown path yet (see env_art.h; plan §0 + risk #1): this is
    // the M-A..C contract ("deactivate: stop drawing/updating, keep VRAM —
    // that already wins the frame time (records are the cost, not memory)").
    void deactivate();
    // True GPU free (M-D, needs WP-4's EnvArtSystem::destroy(device)). See the
    // .cpp for the compile-time SFINAE guard that makes this call site
    // compilable against BOTH the current env_art.h (no destroy() yet — falls
    // back to a documented no-op, matching deactivate()'s VRAM-retained
    // contract) and a future one where WP-4 has landed it (auto-detected, zero
    // edits needed here when that happens).
    void destroy(x3::rhi::IRenderDevice& device);
    // No-op when !resident(). `gate` is accepted per plan §3's verbatim
    // signature; WP-1's own draw() does not consult it (no established
    // semantics for what it should gate were found in the plan or codebase —
    // flagged as an open item for the architect/WP-0). EchoRegionSet::drawAll
    // feeds it ctx.walkScene (the persistent Lane-C scene), the only Scene this
    // package has a long-lived reference to, so a future consumer has
    // something concrete to look at without a signature change.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame, const Scene& gate);
    void update(float dt, float t);
    bool resident() const { return m_resident; }
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

private:
    std::vector<std::unique_ptr<EnvArtSystem>> m_art;
    std::function<void(float dt, float t)>     m_updateFn;
    Scene*                                     m_scene = nullptr;  // not owned
    std::vector<x3::rhi::PointLight>           m_lights;
    // m_built: has the registered builder ever run? Gates build()'s idempotent
    // content-creation step (see class doc). m_resident: is this region
    // currently supposed to draw/update? destroy() resets BOTH so a later
    // build() does a true from-scratch re-realize (M-D streaming).
    bool m_built    = false;
    bool m_resident = false;
};

// ---------------------------------------------------------------------------
// EchoRegionSet — the region table + the WorldStreamer bridge (plan §3/§4).
// Owns one EchoRegion per region id and wires WorldStreamer::setRegionHooks so
// the streamer's residency decisions (wants/hysteresis/budget — all streamer-
// owned, unchanged) build/deactivate the matching EchoRegion by desc.id. No
// content knowledge: which EnvArtSystems/updates/lights a region gets is
// entirely up to the builder function registered for its id.
// ---------------------------------------------------------------------------
class EchoRegionSet {
public:
    // A region builder: populates `EchoRegion&` via addArt/setUpdate/setScene/
    // addLights, using `EchoRegionCtx&` for the device/heightfield/scene/asset
    // roots. Matches EchoRegion::build()'s builder parameter type exactly.
    using RegionBuilderFn = std::function<void(EchoRegion&, EchoRegionCtx&)>;

    // ADDITIVE (not in plan §3's illustrative snippet, but required to connect
    // WP-1's content-agnostic core to WP-2's concrete builders without WP-1
    // knowing region ids/content, and without WP-0 needing to touch this
    // package's files — see the WP-1 final report for why this exists). Call
    // for every region id BEFORE init() (safe to call after too, as long as
    // it's before the streamer's first update()/buildStartRegions() call —
    // the bridge looks builders up by id at call time, not at registration
    // time).
    void registerBuilder(const std::string& id, RegionBuilderFn fn);
    // ADDITIVE diagnostic knob: flips the teardown bridge from
    // EchoRegion::deactivate() (M-A..C default, false) to
    // EchoRegion::destroy(ctx.device) (M-D, once WP-4 lands GPU teardown).
    // WorldStreamer's RegionTeardownFn contract (world_stream.h ~L179) carries
    // no device parameter, so this must be a stateful switch on the bridge
    // rather than a per-call argument — set it from host_echotropolis.cpp
    // (WP-0) when M-D wiring goes live. Default false = M-A..C behavior.
    void setTeardownDestroys(bool destroys);
    // ADDITIVE read-only diagnostic (HUD/log convenience) — false for an
    // unknown id, never throws.
    bool regionResident(const std::string& id) const;
    uint32_t regionCount() const { return (uint32_t)m_orderedIds.size(); }

    // Build the region table from `graph` (one EchoRegion per WorldRegionDesc,
    // all starting non-resident) and wire streamer.setRegionHooks to the bridge
    // below. Caches `&ctx` (see EchoRegionCtx doc) for calls that don't carry
    // their own ctx (setVistaMode's forced-materialize path, drawAll's `gate`).
    //
    // HOOK BRIDGE CONTRACT (world_stream.h ~L164-183, world_stream.cpp exact
    // call sites):
    //   * onBuild fires inside WorldStreamer::realize()'s entity-capture window
    //     (world_stream.cpp:279: `if (m_onRegionBuild) m_onRegionBuild(...)`),
    //     AFTER the streamer's own builder/levelDoc step, for EVERY region
    //     regardless of its desc.builder/desc.levelDoc fields. Echotropolis
    //     regions carry neither (plan §2 Lane B: "no LevelDoc and no
    //     Scene-ledger content") — the ledger this call's Scene& would capture
    //     into stays empty and harmless (plan §0).
    //   * onTeardown fires at the START of a region's first eviction slice
    //     (world_stream.cpp:307: `if (m_onRegionTeardown) m_onRegionTeardown(...)`),
    //     BEFORE any scene slot is released — safe to abandon region state here.
    void init(EchoRegionCtx& ctx, WorldStreamer& streamer, const WorldRegionGraph& graph);

    // Force every region in the table to build()+resident (milestone A / vista
    // mode). Idempotent per-region (see EchoRegion::build) — safe to call
    // repeatedly.
    void forceAllResident(EchoRegionCtx& ctx);
    void drawAll(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);
    void updateAll(float dt, float t);
    // Replaces the host's appendDistrictLights (host_echotropolis.cpp:1848-1860):
    // same nearest-first, budget-capped selection, but the candidate set is the
    // union of RESIDENT regions' light slices instead of one global vector
    // (plan §2 risk #4 — slices don't leak across evictions).
    void appendNearLights(float ex, float ez, std::vector<x3::rhi::PointLight>& out,
                          uint32_t budget);
    // M-B DRAW GATE (integrator, WP-0): when bound and vista is OFF, drawAll/
    // updateAll follow the STREAMER's residency view (state()==Resident) instead
    // of the containers' own flags — the containers keep their content (VRAM
    // retained, the M-A..C contract), the streamer decides what SUBMITS. This
    // resolves the two-owners mismatch: M-A boot-builds everything container-
    // side, which the streamer cannot see (its states start Unloaded), so
    // without this gate nothing would ever stop drawing at street level.
    void bindStreamerForDrawGate(const WorldStreamer* ws) { m_gateStreamer = ws; }

    // Vista rule (plan §5 decision 1): true => every region is force-built (if
    // not already) so draw()'s own residency gate naturally passes for all of
    // them, AND the teardown bridge suppresses eviction entirely for as long as
    // vista mode stays on (so nothing streamed-in for the vista gets undone
    // behind it). This setter does NOT debounce — the 5s leaving-vista
    // hysteresis the plan calls for is the CALLER's job (host_echotropolis.cpp);
    // call setVistaMode(false) only after that hysteresis has elapsed.
    void setVistaMode(bool on);

private:
    void onStreamerBuild(const WorldRegionDesc& desc);
    void onStreamerTeardown(const WorldRegionDesc& desc);

    std::unordered_map<std::string, EchoRegion>      m_regions;
    std::vector<std::string>                         m_orderedIds;  // graph order, for deterministic iteration
    std::unordered_map<std::string, RegionBuilderFn>  m_builders;
    EchoRegionCtx* m_ctx = nullptr;   // not owned; must outlive this set (see init())
    const WorldStreamer* m_gateStreamer = nullptr;   // M-B draw gate (see bindStreamerForDrawGate)
    bool m_vistaMode        = false;
    bool m_teardownDestroys = false;  // see setTeardownDestroys()
};

} // namespace x3::game
