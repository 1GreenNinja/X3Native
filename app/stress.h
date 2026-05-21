#pragma once
// Stress-test scene injection (perf instrumentation layer). Adds N procedural
// cubes at random positions in a volume around the player so the renderer can be
// load-tested: crank N from 100 -> 100k to find the frame-budget ceiling.
//
// Game/slice code only — engine/ stays pure. Built on the public Scene +
// IRenderDevice + mesh_prims interfaces. DEFAULT OFF: nothing here runs unless
// the host explicitly requests it (--stress N CLI flag or the `spawn N` console
// command), so Level 1 is unaffected.
//
// Design: ALL N cubes share ONE GPU mesh + ONE texture (createMesh/createTexture
// are called once). Each cube is a separate visible Entity with its own model
// transform, so the renderer issues N drawMesh() calls — that is the realistic
// many-object draw load we want to measure (draw-call + per-draw descriptor cost),
// not a memory test. The cubes are purely visual (no physics body) so the stress
// is on the render path; they do not affect gameplay/collision.

#include "scene.h"
#include "mesh_prims.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/core/x3_log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// Owns the shared stress mesh/texture + the ids of the spawned cubes so a repeated
// `spawn N` replaces (re-randomizes) rather than endlessly piling on. Reusable for
// both the CLI flag and the console command.
class StressSpawner {
public:
    // Add `count` cubes randomly placed in a box of half-extent `spread` (meters)
    // centered at (cx,cy,cz). Reuses a single shared cube mesh + checker texture
    // created on first use. Cubes are visible, untagged, body-less entities.
    // Returns the number actually spawned. Safe to call repeatedly (accumulates).
    uint32_t spawn(Scene& scene, x3::rhi::IRenderDevice& device,
                   uint32_t count, float cx, float cy, float cz, float spread = 40.0f) {
        if (count == 0) return 0;
        ensureShared(device);
        if (!m_mesh.valid()) return 0; // mesh creation failed (headless / OOM)

        const uint32_t before = (uint32_t)m_spawned.size();
        m_spawned.reserve(before + count);
        for (uint32_t i = 0; i < count; ++i) {
            Entity e;
            e.mesh = m_mesh;
            e.tex  = m_tex;
            // Random position in the spread volume around the center; keep cubes a
            // little above the floor so they don't all z-fight the ground plane.
            const float rx = (frand() * 2.0f - 1.0f) * spread;
            const float ry = (frand())              * spread * 0.5f + 0.5f;
            const float rz = (frand() * 2.0f - 1.0f) * spread;
            // Per-cube uniform scale in [0.4, 1.2] for visual variety.
            const float s = 0.4f + frand() * 0.8f;
            e.transform[0]  = s;   e.transform[5]  = s;   e.transform[10] = s;
            e.transform[12] = cx + rx;
            e.transform[13] = cy + ry;
            e.transform[14] = cz + rz;
            e.transform[15] = 1.0f;
            // Random-ish tint so the cube field reads as a crowd, not a blob.
            e.baseColor[0] = 0.4f + frand() * 0.6f;
            e.baseColor[1] = 0.4f + frand() * 0.6f;
            e.baseColor[2] = 0.4f + frand() * 0.6f;
            e.baseColor[3] = 1.0f;
            e.visible = true;
            e.tag = (uint32_t)Tag::None;
            m_spawned.push_back(scene.add(e));
        }
        m_count += count;
        x3::logInfo("stress: spawned " + std::to_string(count) + " cubes (total " +
                    std::to_string(m_count) + ")");
        return count;
    }

    // Hide every spawned cube (sets Entity::visible=false). Cheap way to "clear"
    // the stress field without rebuilding the Scene's flat array; the shared
    // mesh/texture stay resident for the next spawn. Returns the count hidden.
    uint32_t clear(Scene& scene) {
        uint32_t n = 0;
        for (uint32_t id : m_spawned) {
            if (id < scene.size()) { scene.get(id).visible = false; ++n; }
        }
        m_spawned.clear();
        m_count = 0;
        if (n) x3::logInfo("stress: cleared " + std::to_string(n) + " cubes");
        return n;
    }

    uint32_t count() const { return m_count; }

private:
    // Deterministic-ish LCG so runs are repeatable and we don't pull in <random>'s
    // weight; spread quality is irrelevant for a load test.
    float frand() {
        m_rng = m_rng * 1664525u + 1013904223u;
        return (float)(m_rng >> 8) / (float)(1u << 24); // [0,1)
    }

    void ensureShared(x3::rhi::IRenderDevice& device) {
        if (m_mesh.valid()) return;
        std::vector<x3::rhi::MeshVertex> verts;
        std::vector<uint32_t> idx;
        x3::prims::makeCube(0.5f, verts, idx); // unit cube, 12 triangles
        m_mesh = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                   idx.data(), (uint32_t)idx.size());
        // A small checker so lit faces read; reuses the prims checker generator.
        auto px = x3::prims::makeCheckerRGBA(8, 4, 220, 220, 230, 60, 70, 110);
        m_tex = device.createTexture(px.data(), 8, 8, true);
    }

    x3::rhi::MeshHandle    m_mesh{};
    x3::rhi::TextureHandle m_tex{};
    std::vector<uint32_t>  m_spawned;
    uint32_t               m_count = 0;
    uint32_t               m_rng = 0x1234567u;
};

} // namespace x3::game
