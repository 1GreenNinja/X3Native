#pragma once
// ECHOTROPOLIS HEIGHTFIELD (TIER 2 streaming, WP-0) — the island terrain
// sampler, HOISTED VERBATIM out of host_echotropolis.cpp's anonymous namespace
// so the region packages (echo_regions.h EchoRegionCtx::hf, echo_woodlands'
// scatter, echo_region_builders' seat math) and the host all see the SAME
// x3::game::Heightfield type. See docs/plans/TIER2_STREAMING_PLAN.md and the
// "OPEN INTEGRATION ITEM" note in echo_regions.h — both WP-1 and WP-3 hit the
// TU-local definition independently and coded against this header existing.
//
// The struct body is byte-for-byte the host's (constants, bilinear sampling,
// STBI_NO_STDIO memory-decode path) — only the namespace and a local clamp
// (the host's clampf is also TU-local) differ. PERSISTENT-LANE DOCTRINE
// (plan §2 Lane C): the heightfield and the terrain collision mesh built from
// it are NEVER streamed/regionized — every region builder samples this, so it
// must outlive them all.

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <stb_image.h>   // stbi_load_16_from_memory (impl compiled in engine ModelLoader.cpp)

namespace x3::game {

struct Heightfield {
    int w = 0, h = 0;
    std::vector<uint16_t> px;                     // 16-bit grayscale, row-major
    static constexpr float kMeters  = 4096.0f;    // world extent (island frame)
    static constexpr float kScale   = 320.0f;     // HEIGHT_SCALE
    static constexpr float kSeaNorm = 0.20f;      // normalized sea level

    // ---- MESH-MATCHED SAMPLING (2026-08-12, road-surface lane) --------------
    // THE DESYNC THIS CLOSES. tools/echo_terrain_gen.py writes the heightmap at
    // N_PNG (1025, or 2048 in the older out-of-repo bake) but MESHES the land at
    // N_MESH = 513 — `sub = px[::step, ::step]`. So the surface the player SEES
    // is a 513^2 TRIANGLE mesh over every step'th pixel, while this sampler was
    // reading a BILINEAR patch of the full-resolution PNG. Those are different
    // surfaces: they agree only at the shared vertices and diverge everywhere
    // between them — measured on the loaded bake, up to +18 m, with 10.4% of all
    // road samples ending up BELOW the rendered ground. Anything seated with a
    // 15 cm margin (kGroundLift) or a 2 cm one (the junction patch) is simply
    // buried in the grass; that is the "ground streets have no road surface" and
    // "intersections just cross" report, and the same mechanism is what put
    // houses in the bay and towers off the crown rim.
    //
    // meshN = the land grid the GLB was built with (0 = sample the raw PNG, the
    // pre-fix behaviour). When set, heightAt evaluates the DECIMATED grid using
    // the generator's own triangle split — `(v0, v0+N, v0+1)` / `(v0+1, v0+N,
    // v0+N+1)`, i.e. the diagonal from (c+1,r) to (c,r+1) — so heightAt returns
    // exactly the y the rasterizer puts on screen and every seat in the world is
    // authored against the terrain that is actually drawn.
    // Deliberately bake-AGNOSTIC: a mesh vertex is defined by its WORLD
    // position (linspace over the 4096 m frame), and its height is the raw
    // sampler read there. The committed 1025^2 bake lands each vertex exactly on
    // a pixel, so this is bit-exact; the older out-of-repo 2048^2 bake lands
    // between pixels and lands within ~1 cm of the GLB's own vertices —
    // negligible against the 15 cm kGroundLift, and two orders of magnitude
    // better than the metres of error a full-res read between vertices had.
    int meshN = 0;
    void setMeshGrid(int n) { meshN = (n >= 2 && ok()) ? n : 0; }

    bool load(const std::string& path) {
        // The engine's stb build is STBI_NO_STDIO (memory loaders only), so slurp
        // the file ourselves and decode from memory.
        std::ifstream f(path, std::ios::binary);
        if (!f) { w = h = 0; return false; }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
        if (bytes.empty()) { w = h = 0; return false; }
        int comp = 0;
        uint16_t* d = stbi_load_16_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 1);
        if (!d || w < 2 || h < 2) { if (d) stbi_image_free(d); w = h = 0; return false; }
        px.assign(d, d + (size_t)w * h);
        stbi_image_free(d);
        return true;
    }
    bool ok() const { return w >= 2 && h >= 2; }

    // Bilinear world height (metres) at world (x,z). Off-grid clamps to the edge.
    float heightAt(float x, float z) const {
        if (!ok()) return 0.0f;
        if (meshN >= 2) {
            auto cl = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
            return meshHeightAt(x, z, cl);
        }
        return rawHeightAt(x, z);
    }

    // The PNG as a bilinear field — the pre-fix heightAt, kept because the mesh
    // sampler is defined in terms of it (and ECHO_RAW_HF still selects it).
    float rawHeightAt(float x, float z) const {
        if (!ok()) return 0.0f;
        auto cl = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
        float u = cl((x / kMeters + 0.5f) * (float)(w - 1), 0.0f, (float)(w - 1));
        float v = cl((z / kMeters + 0.5f) * (float)(h - 1), 0.0f, (float)(h - 1));
        const int x0 = (int)u, z0 = (int)v;
        const int x1 = x0 < w - 1 ? x0 + 1 : x0;
        const int z1 = z0 < h - 1 ? z0 + 1 : z0;
        const float fx = u - (float)x0, fz = v - (float)z0;
        auto S = [&](int cx, int cz) { return (float)px[(size_t)cz * w + cx] / 65535.0f; };
        const float a = S(x0, z0), b = S(x1, z0), c = S(x0, z1), d2 = S(x1, z1);
        const float hn = a + (b - a) * fx + (c - a) * fz + (a - b - c + d2) * fx * fz;
        return (hn - kSeaNorm) * kScale;
    }

    // The RENDERED land surface: linear interpolation across the two triangles
    // of the (meshN x meshN) quad, exactly as tools/echo_terrain_gen.py emits
    // them. No bilinear twist term — a triangle is planar, and that planarity is
    // the whole point: this is what the depth buffer contains.
    template <class Clamp>
    float meshHeightAt(float x, float z, Clamp cl) const {
        const float cell = kMeters / (float)(meshN - 1);
        const float u = cl((x / kMeters + 0.5f) * (float)(meshN - 1), 0.0f, (float)(meshN - 1));
        const float v = cl((z / kMeters + 0.5f) * (float)(meshN - 1), 0.0f, (float)(meshN - 1));
        int c = (int)u, r = (int)v;
        if (c > meshN - 2) c = meshN - 2;
        if (r > meshN - 2) r = meshN - 2;
        const float fx = u - (float)c, fz = v - (float)r;
        const float x0 = -kMeters * 0.5f + (float)c * cell;
        const float z0 = -kMeters * 0.5f + (float)r * cell;
        const float h00 = rawHeightAt(x0,        z0);
        const float h10 = rawHeightAt(x0 + cell, z0);
        const float h01 = rawHeightAt(x0,        z0 + cell);
        const float h11 = rawHeightAt(x0 + cell, z0 + cell);
        return (fx + fz <= 1.0f)
            ? h00 + (h10 - h00) * fx + (h01 - h00) * fz
            : h11 + (h01 - h11) * (1.0f - fx) + (h10 - h11) * (1.0f - fz);
    }
};

} // namespace x3::game
