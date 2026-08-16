// LATE NIGHT SPEED shop material kit — see lns_shop.h for provenance.
// These bodies are MOVED VERBATIM from app/club1127.cpp (LNS GARAGE pass,
// Tim 2026-07-18); club1127.cpp now calls them from here. Behaviour change: none
// — same hash, same recipes, same pixels (club captures are the regression).
#include "lns_shop.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::game::lns {

uint32_t lnsHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

std::vector<uint8_t> makeCmuBlockRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t courseH = n / 8;          // 8 courses
    const uint32_t blockW  = n / 4;          // 4 blocks per row
    const uint32_t mortar  = std::max(2u, n / 128);   // joint thickness (px)
    for (uint32_t y = 0; y < n; ++y) {
        const uint32_t course = y / courseH;
        const uint32_t offset = (course & 1) ? blockW / 2 : 0;   // running bond
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t bx = (x + offset) / blockW;
            // Per-block painted value (hashed) — painted CMU is fairly uniform pale grey.
            float v = 0.58f + 0.10f * ((lnsHash(bx * 71 + course * 263) % 1000) / 1000.0f - 0.5f);
            // Mortar joints: horizontal (between courses) + vertical (between blocks).
            const bool hJoint = (y % courseH) < mortar;
            const bool vJoint = ((x + offset) % blockW) < mortar;
            if (hJoint || vJoint) v *= 0.52f;                    // recessed dark joint
            // Fine paint grit / porosity speckle.
            v += 0.03f * ((lnsHash(x * 911 + y * 379) % 100) / 100.0f - 0.5f);
            // Slight cool tint (painted white-grey reads a touch blue under UV).
            const float r = v * 0.98f, g = v * 0.99f, b = v * 1.03f;
            p[0] = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255);
            p[1] = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255);
            p[2] = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255);
            p[3] = 255;
        }
    }
    return px;
}

std::vector<uint8_t> makeCmuNormalRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t courseH = n / 8, blockW = n / 4, mortar = std::max(2u, n / 128);
    for (uint32_t y = 0; y < n; ++y) {
        const uint32_t course = y / courseH;
        const uint32_t offset = (course & 1) ? blockW / 2 : 0;
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            float nx = 0.0f, ny = 0.0f;
            const uint32_t yin = y % courseH, xin = (x + offset) % blockW;
            if (yin < mortar)            ny = -0.6f; else if (yin < mortar * 2) ny = 0.6f;
            if (xin < mortar)            nx = -0.6f; else if (xin < mortar * 2) nx = 0.6f;
            const float nz = 1.0f;
            const float l = std::sqrt(nx*nx + ny*ny + nz*nz);
            p[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255);
            p[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255);
            p[2] = (uint8_t)((nz / l * 0.5f + 0.5f) * 255);
            p[3] = 255;
        }
    }
    return px;
}

// NEW here (not a move): the club builds its checkerboard as ~1 m tile BOXES
// with per-box baseColor; a swept-quad floor wants the same split as a TEXTURE.
// Both read kCheckerBright/kCheckerDark from the header, so there is exactly
// one place the shop-floor albedo lives. LINEAR values -> create srgb=false.
std::vector<uint8_t> makeCheckerFloorRGBA(uint32_t n, uint32_t tiles) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t tile = std::max(1u, n / std::max(1u, tiles));
    // The club leaves a 2 cm gap between 1 m tiles: 2 % of a tile edge, at
    // least 1 px, showing the near-black underfloor between squares.
    const uint32_t seam = std::max(1u, tile / 50u);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t gx = x / tile, gy = y / tile;
            const bool onSeam = (x % tile) < seam || (y % tile) < seam;
            const float* c = ((gx + gy) & 1u) ? kCheckerDark : kCheckerBright;
            if (onSeam) { p[0] = p[1] = p[2] = 3; }   // the gap between tiles
            else {
                p[0] = (uint8_t)(c[0] * 255.0f + 0.5f);
                p[1] = (uint8_t)(c[1] * 255.0f + 0.5f);
                p[2] = (uint8_t)(c[2] * 255.0f + 0.5f);
            }
            p[3] = 255;
        }
    return px;
}

std::vector<uint8_t> makeSignRGBA(uint32_t w, uint32_t h, const char* line,
                                  float rr, float rg, float rb) {
    // 5x7 glyphs (7 rows, each a 5-bit mask, MSB = left).
    auto glyph = [](char c, uint8_t rows[7]) -> bool {
        auto set = [&](uint8_t a,uint8_t b,uint8_t d,uint8_t e,uint8_t f,uint8_t g,uint8_t i){
            rows[0]=a;rows[1]=b;rows[2]=d;rows[3]=e;rows[4]=f;rows[5]=g;rows[6]=i; };
        switch (c) {
            case 'L': set(0x10,0x10,0x10,0x10,0x10,0x10,0x1F); return true;
            case 'A': set(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11); return true;
            case 'T': set(0x1F,0x04,0x04,0x04,0x04,0x04,0x04); return true;
            case 'E': set(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F); return true;
            case 'N': set(0x11,0x19,0x15,0x13,0x11,0x11,0x11); return true;
            case 'I': set(0x1F,0x04,0x04,0x04,0x04,0x04,0x1F); return true;
            case 'G': set(0x0E,0x11,0x10,0x17,0x11,0x11,0x0E); return true;
            case 'H': set(0x11,0x11,0x11,0x1F,0x11,0x11,0x11); return true;
            case 'S': set(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E); return true;
            case 'P': set(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10); return true;
            case 'D': set(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E); return true;
            case 'C': set(0x0E,0x11,0x10,0x10,0x10,0x11,0x0E); return true;
            case 'U': set(0x11,0x11,0x11,0x11,0x11,0x11,0x0E); return true;
            case 'B': set(0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E); return true;
            case '1': set(0x04,0x0C,0x04,0x04,0x04,0x04,0x0E); return true;
            case '2': set(0x0E,0x11,0x01,0x02,0x04,0x08,0x1F); return true;
            case '7': set(0x1F,0x01,0x02,0x04,0x04,0x04,0x04); return true;
            default:  set(0,0,0,0,0,0,0); return false;   // space / unknown
        }
    };
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {   // dark panel bg
        px[i*4+0] = 6; px[i*4+1] = 5; px[i*4+2] = 9; px[i*4+3] = 255;
    }
    const uint32_t n = (uint32_t)std::strlen(line);
    if (n == 0) return px;
    const uint32_t cellW = 6, cellH = 7;                    // 5+1 spacing
    const uint32_t textWcells = n * cellW;
    // Scale each font-cell to fill ~80% of the panel width.
    const uint32_t scale = std::max(1u, (uint32_t)((w * 0.86f) / textWcells));
    const uint32_t textPxW = textWcells * scale, textPxH = cellH * scale;
    const uint32_t ox = (w > textPxW) ? (w - textPxW) / 2 : 0;
    const uint32_t oy2 = (h > textPxH) ? (h - textPxH) / 2 : 0;
    // Render lit cells + a soft halo.
    auto lit = [&](int cx, int cy) -> bool {
        if (cx < 0 || cy < 0) return false;
        const uint32_t ci = (uint32_t)cx / cellW;
        if (ci >= n) return false;
        const uint32_t gx = (uint32_t)cx % cellW, gy = (uint32_t)cy;
        if (gx >= 5 || gy >= 7) return false;
        uint8_t rows[7]; glyph(line[ci], rows);
        return (rows[gy] >> (4 - gx)) & 1;
    };
    for (uint32_t y = 0; y < textPxH; ++y)
        for (uint32_t x = 0; x < textPxW; ++x) {
            const int cx = (int)(x / scale), cy = (int)(y / scale);
            float glow = 0.0f;
            if (lit(cx, cy)) glow = 1.0f;
            else if (lit(cx-1,cy)||lit(cx+1,cy)||lit(cx,cy-1)||lit(cx,cy+1)) glow = 0.35f; // halo
            if (glow <= 0.0f) continue;
            uint8_t* p = &px[((size_t)(oy2 + y) * w + (ox + x)) * 4];
            const float br = glow;
            p[0] = (uint8_t)(std::min(1.0f, rr * br + (glow>0.9f?0.35f:0.0f)) * 255);
            p[1] = (uint8_t)(std::min(1.0f, rg * br + (glow>0.9f?0.30f:0.0f)) * 255);
            p[2] = (uint8_t)(std::min(1.0f, rb * br + (glow>0.9f?0.30f:0.0f)) * 255);
        }
    return px;
}

std::vector<uint8_t> makeMr1x1(uint8_t rough, uint8_t metal) {
    return { 255, rough, metal, 255 };
}

} // namespace x3::game::lns
