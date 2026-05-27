// ECS -> GPU render feed — see app/ecs_render.h.
#include "ecs_render.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>
#include <vector>

namespace x3::game {

uint32_t renderEcs(x3::ecs::World& world, x3::rhi::IRenderDevice& device,
                   const x3::rhi::FrameContext& frame) {
    uint32_t n = 0;
    world.each<EcsTransform, EcsRenderable>(
        [&](x3::ecs::EntityId, EcsTransform& t, EcsRenderable& r) {
            device.drawMesh(frame, x3::rhi::MeshHandle{ r.meshId },
                            x3::rhi::TextureHandle{ r.texId }, r.color, t.world);
            ++n;
        });
    return n;   // device groups identical meshIds into one multidraw-indirect call
}

uint32_t integrateEcs(x3::ecs::World& world, float dt) {
    uint32_t n = 0;
    world.each<EcsTransform, EcsVelocity>(
        [&](x3::ecs::EntityId, EcsTransform& t, EcsVelocity& vel) {
            t.world[12] += vel.v[0] * dt;
            t.world[13] += vel.v[1] * dt;
            t.world[14] += vel.v[2] * dt;
            ++n;
        });
    return n;
}

// ===========================================================================
// Headless self-test (--test-ecsrender). R0-R3.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[ecsrender-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[ecsrender-test] FAIL ") + name); }
}
}

bool runEcsRenderSelfTest() {
    g_pass = g_fail = 0;
    x3::ecs::World w;
    HeadlessRenderDevice device;
    x3::rhi::FrameContext frame{};   // headless: drawMesh is a no-op but callable

    constexpr uint32_t N = 10000;
    std::vector<x3::ecs::EntityId> ents; ents.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        auto e = w.create();
        ents.push_back(e);
        EcsTransform t;
        t.world[12] = (float)i;                 // spread along X
        w.add<EcsTransform>(e, t);
        // Two shared meshes -> the device collapses to 2 multidraw-indirect draws.
        EcsRenderable r; r.meshId = (i % 2 == 0) ? 1u : 2u; r.texId = 0;
        w.add<EcsRenderable>(e, r);
        if (i % 2 == 0) w.add<EcsVelocity>(e, EcsVelocity{ { 1.0f, 0.0f, 0.0f } });
    }

    // ---- R0: the render feed visits every renderable (10k draws issued). ----
    uint32_t drawn = renderEcs(w, device, frame);
    check(drawn == N, "R0 render feed issues a draw per renderable (10k)");

    // ---- R1: the movement system integrates only the moving subset (5k). ----
    uint32_t moved = integrateEcs(w, 0.5f);
    bool integrated = std::fabs(w.get<EcsTransform>(ents[2]).world[12] - (2.0f + 0.5f)) < 1e-3f;
    bool stationary = std::fabs(w.get<EcsTransform>(ents[3]).world[12] - 3.0f) < 1e-3f;
    check(moved == N / 2 && integrated && stationary,
          "R1 movement system sweeps only the moving entities + integrates");

    // ---- R2: destroying entities shrinks the render feed. ----
    for (uint32_t i = 0; i < 2000; ++i) w.destroy(ents[i]);
    uint32_t drawn2 = renderEcs(w, device, frame);
    check(drawn2 == N - 2000, "R2 destroying entities shrinks the feed");

    // ---- R3: removing just the Renderable (entity lives) drops it from the feed
    // but NOT from a Transform-only sweep. ----
    uint32_t before = renderEcs(w, device, frame);
    w.remove<EcsRenderable>(ents[5000]);          // alive entity, no longer drawn
    uint32_t after = renderEcs(w, device, frame);
    size_t transforms = 0;
    w.each<EcsTransform>([&](x3::ecs::EntityId, EcsTransform&){ ++transforms; });
    check(after == before - 1 && transforms == N - 2000,
          "R3 removing Renderable drops it from the feed only");

    x3::logInfo(std::string("[ecsrender-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
