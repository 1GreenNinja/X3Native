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
};

} // namespace x3::game
