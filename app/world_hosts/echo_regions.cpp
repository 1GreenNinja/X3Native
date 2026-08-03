// ECHOTROPOLIS REGION CORE (TIER 2 streaming, WP-1) — see echo_regions.h for
// the full design rationale and docs/plans/TIER2_STREAMING_PLAN.md §3/§4 for
// the architecture this implements. Game/slice code only — engine/ stays pure.
#include "echo_regions.h"
#include "../world_stream.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace x3::game {

// ===========================================================================
// EchoRegion
// ===========================================================================

void EchoRegion::addArt(std::unique_ptr<EnvArtSystem> e) {
    if (e) m_art.push_back(std::move(e));
}

void EchoRegion::setUpdate(std::function<void(float dt, float t)> fn) {
    m_updateFn = std::move(fn);
}

void EchoRegion::setScene(Scene* s) {
    m_scene = s;  // not owned — caller (the builder / EchoRegionCtx owner) keeps it alive
}

void EchoRegion::addLights(std::vector<x3::rhi::PointLight> l) {
    // APPEND, not replace — see header doc (a region may harvest light slices
    // from more than one sub-builder call, mirroring the host's per-district
    // districtLights.push_back loop, host_echotropolis.cpp:1813).
    m_lights.insert(m_lights.end(),
                    std::make_move_iterator(l.begin()),
                    std::make_move_iterator(l.end()));
}

void EchoRegion::build(EchoRegionCtx& ctx,
                       const std::function<void(EchoRegion&, EchoRegionCtx&)>& builder) {
    // IDEMPOTENT content creation: the builder runs exactly once per lifetime
    // (or once again after a true destroy() resets m_built). A region that
    // streams back in after a deactivate()-only eviction (EnvArt's GPU memory
    // was NEVER freed — see env_art.h / plan risk #1) must NOT re-run the
    // builder: that would silently double the region's EnvArtSystems/lights/
    // update hooks every re-entry. Re-entry is therefore just a residency flip.
    if (!m_built) {
        if (builder) builder(*this, ctx);
        m_built = true;
    }
    m_resident = true;
}

void EchoRegion::deactivate() {
    // M-A..C contract (plan §0 / risk #1): EnvArtSystem has no GPU teardown
    // path today, so "evict" means stop draw()/update() and keep the VRAM —
    // that already wins the frame time (records are the cost, not memory).
    m_resident = false;
}

namespace detail {

// ---------------------------------------------------------------------------
// Compile-time "does EnvArtSystem have a destroy(device) method yet" probe
// (WP-4 lands it in app/env_art.h/.cpp; this package must stay compilable
// BEFORE and AFTER that lands, per the WP-1 brief). A plain
// `art.destroy(device)` call in a NON-template function would fail to compile
// today (env_art.h — read in full for this task — declares no such member),
// regardless of any `if constexpr` wrapped around it: `if constexpr` only
// discards a branch's TEMPLATE-DEPENDENT statements from instantiation, so the
// probed call must live inside a function template for the discard to apply.
// destroyOrDeactivateEnvArt<EnvArtT> below is that template; HasDestroyMethod
// is the SFINAE/void_t detector. Net effect: this file compiles clean against
// the CURRENT env_art.h (false branch, documented no-op) and will start
// calling EnvArtSystem::destroy() automatically the moment WP-4 adds it — zero
// edits required in this package when that lands.
template <typename T, typename = void>
struct HasDestroyMethod : std::false_type {};
template <typename T>
struct HasDestroyMethod<T, std::void_t<decltype(std::declval<T&>().destroy(
                               std::declval<x3::rhi::IRenderDevice&>()))>>
    : std::true_type {};

template <typename EnvArtT>
void destroyOrDeactivateEnvArt(EnvArtT& art, x3::rhi::IRenderDevice& device) {
    if constexpr (HasDestroyMethod<EnvArtT>::value) {
        art.destroy(device);   // WP-4 landed: true GPU free (M-D).
    } else {
        (void)art; (void)device;
        // WP-4 not landed against the env_art.h this file was built with: no
        // GPU release path exists (verified by reading env_art.h — no
        // destructor releases meshes/textures, no destroy() member). Nothing
        // to call; the EnvArtSystem's GPU state is abandoned in VRAM exactly
        // as deactivate() already leaves it (M-C's accepted contract). The
        // caller (EchoRegion::destroy) still clears its OWN bookkeeping
        // (m_art/m_lights/m_updateFn/m_scene/m_built) so the region can be
        // rebuilt from scratch by a later build() call.
    }
}

} // namespace detail

void EchoRegion::destroy(x3::rhi::IRenderDevice& device) {
    m_resident = false;
    for (auto& a : m_art) {
        if (a) detail::destroyOrDeactivateEnvArt(*a, device);
    }
    m_art.clear();
    m_updateFn = nullptr;
    m_scene    = nullptr;   // not owned — just forget the pointer
    m_lights.clear();
    m_built = false;        // a later build() call re-runs the builder from scratch (M-D re-realize)
}

void EchoRegion::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const Scene& gate) {
    (void)gate;  // accepted per plan §3's verbatim signature; see header doc — no
                 // established semantics found for what it should gate.
    if (!m_resident) return;
    for (auto& a : m_art) {
        if (a) a->draw(device, frame);
    }
    if (m_scene) m_scene->render(device, frame);
}

void EchoRegion::update(float dt, float t) {
    if (!m_resident) return;
    if (m_updateFn) m_updateFn(dt, t);
}

// ===========================================================================
// EchoRegionSet
// ===========================================================================

void EchoRegionSet::registerBuilder(const std::string& id, RegionBuilderFn fn) {
    m_builders[id] = std::move(fn);
}

void EchoRegionSet::setTeardownDestroys(bool destroys) {
    m_teardownDestroys = destroys;
}

bool EchoRegionSet::regionResident(const std::string& id) const {
    auto it = m_regions.find(id);
    return it != m_regions.end() && it->second.resident();
}

void EchoRegionSet::init(EchoRegionCtx& ctx, WorldStreamer& streamer, const WorldRegionGraph& graph) {
    m_ctx = &ctx;

    m_regions.clear();
    m_orderedIds.clear();
    m_orderedIds.reserve(graph.regions.size());
    for (const WorldRegionDesc& d : graph.regions) {
        m_orderedIds.push_back(d.id);
        m_regions.emplace(d.id, EchoRegion{});
    }

    // WorldStreamer hook bridge — see the init() doc comment in echo_regions.h
    // for the exact world_stream.cpp call sites this relies on. Both lambdas
    // capture `this` (not `&ctx`/`&streamer`/`&graph`): EchoRegionSet outlives
    // any single init() call's stack frame, and the bridge must keep working
    // for the WorldStreamer's entire remaining lifetime.
    streamer.setRegionHooks(
        [this](const WorldRegionDesc& desc, Scene& /*streamerScene*/,
               x3::rhi::IRenderDevice& /*streamerDevice*/,
               x3::phys::IPhysicsWorld& /*streamerPhysics*/) {
            // Deliberately ignore the streamer's own Scene&/IRenderDevice&/
            // IPhysicsWorld& — they are the SAME walkScene/device/physics WP-0
            // wired into EchoRegionCtx at init() time (single source of truth
            // per plan §2 Lane B: Echotropolis content is built through ctx,
            // never through the streamer's Scene-ledger capture window).
            this->onStreamerBuild(desc);
        },
        [this](const WorldRegionDesc& desc) {
            this->onStreamerTeardown(desc);
        });
}

void EchoRegionSet::onStreamerBuild(const WorldRegionDesc& desc) {
    if (!m_ctx) {
        x3::logError("[echo_regions] onStreamerBuild(`" + desc.id + "`) fired before init() — ignored.");
        return;
    }
    auto rit = m_regions.find(desc.id);
    if (rit == m_regions.end()) {
        x3::logError("[echo_regions] streamer wants unknown region `" + desc.id +
                     "` (not present in the EchoRegion table built at init() time) — nothing built.");
        return;
    }
    auto bit = m_builders.find(desc.id);
    if (bit == m_builders.end()) {
        x3::logError("[echo_regions] region `" + desc.id +
                     "` has no registered builder (registerBuilder() was never called for this "
                     "id) — the container stays empty.");
        return;
    }
    rit->second.build(*m_ctx, bit->second);
}

void EchoRegionSet::onStreamerTeardown(const WorldRegionDesc& desc) {
    // Vista override (plan §5 decision 1): while vista mode is on, the bridge
    // suppresses teardown outright — every region that streamed in stays in,
    // so setVistaMode(true)'s force-materialize pass (below) is never undone
    // behind its own back by a normal distance-based eviction.
    if (m_vistaMode) return;
    auto rit = m_regions.find(desc.id);
    if (rit == m_regions.end()) return;  // unknown id — onStreamerBuild already logged this region
    if (m_teardownDestroys && m_ctx) {
        rit->second.destroy(m_ctx->device);   // M-D: true GPU free (WP-4's EnvArtSystem::destroy)
    } else {
        rit->second.deactivate();             // M-A..C: stop draw/update, keep VRAM
    }
}

void EchoRegionSet::forceAllResident(EchoRegionCtx& ctx) {
    for (const std::string& id : m_orderedIds) {
        auto rit = m_regions.find(id);
        if (rit == m_regions.end()) continue;
        auto bit = m_builders.find(id);
        if (bit == m_builders.end()) {
            x3::logError("[echo_regions] forceAllResident: region `" + id +
                         "` has no registered builder — skipped.");
            continue;
        }
        rit->second.build(ctx, bit->second);  // idempotent per-region (see EchoRegion::build)
    }
}

void EchoRegionSet::drawAll(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) {
    if (!m_ctx) return;  // init() never called — nothing to draw
    for (const std::string& id : m_orderedIds) {
        auto rit = m_regions.find(id);
        if (rit == m_regions.end()) continue;
        // M-B DRAW GATE: outside vista, only regions the STREAMER holds Resident
        // submit draw records (see bindStreamerForDrawGate's header doc). The
        // container's own residency (content/VRAM) is untouched.
        if (!m_vistaMode && m_gateStreamer) {
            const int si = m_gateStreamer->indexOf(id);
            if (si >= 0 && m_gateStreamer->state((uint32_t)si) != RegionState::Resident)
                continue;
        }
        // `gate` = ctx.walkScene, cached from init() — see EchoRegion::draw's
        // header doc for why (drawAll carries no Scene/ctx parameter of its
        // own per plan §3's verbatim signature, so this is the only
        // long-lived Scene reference this package has to offer).
        rit->second.draw(device, frame, m_ctx->walkScene);
    }
}

void EchoRegionSet::updateAll(float dt, float t) {
    for (const std::string& id : m_orderedIds) {
        auto rit = m_regions.find(id);
        if (rit == m_regions.end()) continue;
        if (!m_vistaMode && m_gateStreamer) {   // M-B: skip pose work for gated-out regions
            const int si = m_gateStreamer->indexOf(id);
            if (si >= 0 && m_gateStreamer->state((uint32_t)si) != RegionState::Resident)
                continue;
        }
        rit->second.update(dt, t);
    }
}

void EchoRegionSet::appendNearLights(float ex, float ez, std::vector<x3::rhi::PointLight>& out,
                                     uint32_t budget) {
    // Replaces host_echotropolis.cpp:1848-1860's appendDistrictLights: same
    // nearest-first partial_sort over squared XZ distance, same budget cap, but
    // the candidate pool is the union of RESIDENT regions' own light slices
    // instead of one global districtLights vector (plan §2 risk #4: slices
    // must not leak across evictions — a deactivated region's lights are
    // simply not in this pool at all).
    if (out.size() >= budget) return;
    std::vector<std::pair<float, const x3::rhi::PointLight*>> ord;
    for (const std::string& id : m_orderedIds) {
        auto rit = m_regions.find(id);
        if (rit == m_regions.end() || !rit->second.resident()) continue;
        for (const x3::rhi::PointLight& pl : rit->second.lights()) {
            const float dx = pl.pos[0] - ex, dz = pl.pos[2] - ez;
            ord.emplace_back(dx * dx + dz * dz, &pl);
        }
    }
    if (ord.empty()) return;
    const uint32_t want = std::min<uint32_t>(budget - (uint32_t)out.size(), (uint32_t)ord.size());
    std::partial_sort(ord.begin(), ord.begin() + want, ord.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    for (uint32_t i = 0; i < want; ++i) out.push_back(*ord[i].second);
}

void EchoRegionSet::setVistaMode(bool on) {
    if (on == m_vistaMode) return;
    m_vistaMode = on;
    if (on && m_ctx) {
        // "all regions treated resident for draw" (plan §3/§5 decision 1),
        // made LITERALLY true rather than special-cased inside draw(): force
        // every never-built region to build NOW (idempotent for already-built
        // ones — see EchoRegion::build), so each EchoRegion's own residency
        // gate naturally passes for the whole table. Needed beyond M-A..C
        // (where forceAllResident() already ran once at boot) for M-D's
        // spawn-only boot, where a region the player never streamed into could
        // still be un-built the moment they fly up to vista height.
        // onStreamerTeardown() (above) keeps everything resident for as long
        // as vista mode stays on, so this materialize pass is never undone
        // behind it while active.
        forceAllResident(*m_ctx);
    }
    // NOTE: the plan's 5s leaving-vista hysteresis (§6 decision 1) is the
    // CALLER's job (host_echotropolis.cpp / WP-0) — this setter is a dumb,
    // immediate toggle by design (no content knowledge, no timers here);
    // debounce the on->off edge before calling setVistaMode(false).
}

} // namespace x3::game
