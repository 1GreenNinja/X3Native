#pragma once
// app/space/sun_bake.h — the LIVING SUN SURFACE bake (granulation + faculae +
// sunspots), extracted for reuse from world_hosts/host_space.cpp (the
// CommanderIntegrator flyable-star work) so the interactive intro can hang the
// SAME star in the dogfight sky. The host keeps its own file-local copy (its
// body is marked VERBATIM re-homed and the codebase's stated pattern for this
// bake is copy-per-TU, ship_windows.cpp -> host_space.cpp -> here); if the
// surface look is retuned there, mirror it here.
//
// Bake an n x n RGBA sun-surface texture: 5-octave tileable value-noise
// granulation (deep-orange cells, white-gold cores), a sparse bright
// faculae/vein layer, and 3-7 hash-placed near-black sunspots with soft
// penumbrae, wrapping the U (longitude) seam. Bind as BOTH baseColor and
// emissiveTex so the detail modulates the bloom, not just unlit albedo.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::space::sunbake {

inline uint32_t hashU(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline float hashF(uint32_t x) { return (hashU(x) & 0xFFFFFFu) / 16777216.0f; }
inline float smoothstepL(float e0, float e1, float x) {
    float d = e1 - e0; if (std::fabs(d) < 1e-6f) d = (d < 0.0f) ? -1e-6f : 1e-6f;
    float t = (x - e0) / d; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
inline float sunHash2(uint32_t x, uint32_t y, uint32_t cell, uint32_t salt) {
    uint32_t h = (x % cell) * 374761393u + (y % cell) * 668265263u + salt * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}
inline float sunValueNoise(float u, float v, uint32_t cell, uint32_t salt) {
    const float fx = u * (float)cell, fy = v * (float)cell;
    const uint32_t x0 = (uint32_t)std::floor(fx), y0 = (uint32_t)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto sm = [](float t){ return t * t * (3.0f - 2.0f * t); };
    const float sx = sm(tx), sy = sm(ty);
    const float a = sunHash2(x0,     y0,     cell, salt);
    const float b = sunHash2(x0 + 1, y0,     cell, salt);
    const float c = sunHash2(x0,     y0 + 1, cell, salt);
    const float d = sunHash2(x0 + 1, y0 + 1, cell, salt);
    const float ab = a + (b - a) * sx;
    const float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}
inline std::vector<uint8_t> bakeSunRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    struct Spot { float u, v, r; };
    const int nSpots = 3 + (int)(sunHash2(1, 1, n, 777u) * 5.0f);   // 3-7
    Spot spots[7];
    for (int i = 0; i < nSpots; ++i) {
        spots[i].u = hashF((uint32_t)(i * 7 + 1000));
        spots[i].v = 0.18f + 0.64f * hashF((uint32_t)(i * 7 + 2000));   // keep off the poles
        spots[i].r = 0.050f + 0.075f * hashF((uint32_t)(i * 7 + 3000));
    }
    for (uint32_t y = 0; y < n; ++y) {
        const float v = (y + 0.5f) / (float)n;
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / (float)n;
            // Granulation: 5 octaves of tileable value noise, contrast-remapped
            // so wide dark inter-granular lanes separate sparse bright cells.
            float gran = 0.0f, amp = 0.5f; uint32_t cell = 6;
            for (int o = 0; o < 5; ++o) {
                gran += amp * sunValueNoise(u, v, cell, 5000u + (uint32_t)o * 37u);
                amp *= 0.55f; cell *= 2u;
            }
            gran = std::clamp((gran - 0.40f) * 2.9f, 0.0f, 1.0f);
            gran = gran * gran * (0.55f + 0.45f * gran);
            const float vein = sunValueNoise(u, v, 40, 6600u);
            const float veinBright = std::max(0.0f, vein - 0.74f) / 0.26f;
            // Deep orange-red lanes dominant; blue near-zero until the hottest
            // cores so peaks go white-gold while the disc reads rich ORANGE.
            float r = 0.52f + 0.48f * gran;
            float g = 0.13f + 0.62f * gran;
            float b = 0.02f + 0.26f * gran * gran * gran;
            r += veinBright * 0.26f; g += veinBright * 0.22f; b += veinBright * 0.10f;
            for (int i = 0; i < nSpots; ++i) {
                float du = u - spots[i].u; du -= std::round(du);
                const float dv = v - spots[i].v;
                const float d = std::sqrt(du * du + dv * dv);
                const float k = 1.0f - smoothstepL(spots[i].r * 0.5f, spots[i].r, d);
                r *= (1.0f - 0.94f * k); g *= (1.0f - 0.96f * k); b *= (1.0f - 0.98f * k);
            }
            auto u8 = [](float c){ c = std::clamp(c, 0.0f, 1.0f); return (uint8_t)std::lround(c * 255.0f); };
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            p[0] = u8(r); p[1] = u8(g); p[2] = u8(b); p[3] = 255;
        }
    }
    return px;
}

} // namespace x3::space::sunbake
