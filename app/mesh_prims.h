#pragma once
// Procedural MeshVertex primitive builders for the graybox test level (S2).
// Factored out of app/main.cpp (S1). Engine stays pure: this lives under app/.
//
// Two flavors of geometry are produced from the same authoring call:
//   - RENDER geometry  : x3::rhi::MeshVertex {pos, normal, uv}  -> createMesh
//   - COLLISION geometry: position-only float triples + indices -> addStaticMesh
// A single box builder fills BOTH so the renderer and Jolt see identical shapes.

#include "engine/rhi/IRenderDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace x3::prims {

using x3::rhi::MeshVertex;

// Render + collision geometry produced together. `cverts` is tightly-packed
// xyz triples (3 floats per vertex) suitable for IPhysicsWorld::addStaticMesh;
// `cindex` matches it. The render side carries normals + UVs.
struct PrimMesh {
    std::vector<MeshVertex> verts;    // render vertices (pos/normal/uv)
    std::vector<uint32_t>   index;    // render indices
    std::vector<float>      cverts;   // collision positions (xyz triples)
    std::vector<uint32_t>   cindex;   // collision indices (== render index here)
};

// A flat ground quad on the XZ plane, centered at origin, `half` units to a
// side, UVs tiled `tiles` times so a checker reads as repeated cells.
inline void makeGroundQuad(float half, float tiles,
                           std::vector<MeshVertex>& verts,
                           std::vector<uint32_t>& idx) {
    verts = {
        {{-half, 0, -half}, {0, 1, 0}, {0,     0    }},
        {{ half, 0, -half}, {0, 1, 0}, {tiles, 0    }},
        {{ half, 0,  half}, {0, 1, 0}, {tiles, tiles}},
        {{-half, 0,  half}, {0, 1, 0}, {0,     tiles}},
    };
    // CCW when viewed from above (+Y), matching VK_FRONT_FACE_COUNTER_CLOCKWISE.
    idx = { 0, 2, 1, 0, 3, 2 };
}

// A unit cube (24 verts, per-face normals + UVs), `h` = half-extent.
inline void makeCube(float h,
                     std::vector<MeshVertex>& verts,
                     std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    auto face = [&](float ax,float ay,float az, float bx,float by,float bz,
                    float cx,float cy,float cz, float dx,float dy,float dz,
                    float nx,float ny,float nz) {
        uint32_t base = (uint32_t)verts.size();
        verts.push_back({{ax,ay,az},{nx,ny,nz},{0,0}});
        verts.push_back({{bx,by,bz},{nx,ny,nz},{1,0}});
        verts.push_back({{cx,cy,cz},{nx,ny,nz},{1,1}});
        verts.push_back({{dx,dy,dz},{nx,ny,nz},{0,1}});
        idx.insert(idx.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    face(-h,-h, h,  h,-h, h,  h, h, h, -h, h, h,  0,0, 1); // +Z
    face( h,-h,-h, -h,-h,-h, -h, h,-h,  h, h,-h,  0,0,-1); // -Z
    face( h,-h, h,  h,-h,-h,  h, h,-h,  h, h, h,  1,0, 0); // +X
    face(-h,-h,-h, -h,-h, h, -h, h, h, -h, h,-h, -1,0, 0); // -X
    face(-h, h, h,  h, h, h,  h, h,-h, -h, h,-h,  0,1, 0); // +Y
    face(-h,-h,-h,  h,-h,-h,  h,-h, h, -h,-h, h,  0,-1,0); // -Y
}

// An axis-aligned box with arbitrary per-axis half extents (hx,hy,hz), centered
// at (cx,cy,cz) in WORLD space. Produces BOTH render and collision geometry so
// the same authored box feeds createMesh AND addStaticMesh.
//
// `uvScale` controls how many UV tiles span one world meter (so larger surfaces
// get more checker cells). Per-face normals + UVs; CCW winding for the device's
// counter-clockwise front face.
inline PrimMesh makeBox(float hx, float hy, float hz,
                        float cx, float cy, float cz,
                        float uvScale = 1.0f) {
    PrimMesh m;
    auto face = [&](float ax,float ay,float az, float bx,float by,float bz,
                    float cxx,float cyy,float czz, float dx,float dy,float dz,
                    float nx,float ny,float nz, float u, float v) {
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{cx+ax,cy+ay,cz+az},{nx,ny,nz},{0, 0}});
        m.verts.push_back({{cx+bx,cy+by,cz+bz},{nx,ny,nz},{u, 0}});
        m.verts.push_back({{cx+cxx,cy+cyy,cz+czz},{nx,ny,nz},{u, v}});
        m.verts.push_back({{cx+dx,cy+dy,cz+dz},{nx,ny,nz},{0, v}});
        m.index.insert(m.index.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    const float su = uvScale, sv = uvScale;
    // u/v tiling derived from the in-plane extents of each face.
    face(-hx,-hy, hz,  hx,-hy, hz,  hx, hy, hz, -hx, hy, hz,  0,0, 1, hx*2*su, hy*2*sv); // +Z
    face( hx,-hy,-hz, -hx,-hy,-hz, -hx, hy,-hz,  hx, hy,-hz,  0,0,-1, hx*2*su, hy*2*sv); // -Z
    face( hx,-hy, hz,  hx,-hy,-hz,  hx, hy,-hz,  hx, hy, hz,  1,0, 0, hz*2*su, hy*2*sv); // +X
    face(-hx,-hy,-hz, -hx,-hy, hz, -hx, hy, hz, -hx, hy,-hz, -1,0, 0, hz*2*su, hy*2*sv); // -X
    face(-hx, hy, hz,  hx, hy, hz,  hx, hy,-hz, -hx, hy,-hz,  0,1, 0, hx*2*su, hz*2*sv); // +Y
    face(-hx,-hy,-hz,  hx,-hy,-hz,  hx,-hy, hz, -hx,-hy, hz,  0,-1,0, hx*2*su, hz*2*sv); // -Y

    // Collision: reuse the render vertex positions + indices (same triangles).
    m.cverts.reserve(m.verts.size() * 3);
    for (const auto& vtx : m.verts) {
        m.cverts.push_back(vtx.pos[0]);
        m.cverts.push_back(vtx.pos[1]);
        m.cverts.push_back(vtx.pos[2]);
    }
    m.cindex = m.index;
    return m;
}

// Procedural NxN checker texture (RGBA8). Two contrasting colors per cell:
// a light tint and a darker base. Defaults match the S1 blue-grey scheme.
inline std::vector<uint8_t> makeCheckerRGBA(uint32_t n, uint32_t cell,
                                            uint8_t lr = 230, uint8_t lg = 230, uint8_t lb = 235,
                                            uint8_t dr = 40,  uint8_t dg = 55,  uint8_t db = 90) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            bool on = ((x / cell) ^ (y / cell)) & 1u;
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            if (on) { p[0]=lr; p[1]=lg; p[2]=lb; }
            else    { p[0]=dr; p[1]=dg; p[2]=db; }
            p[3] = 255;
        }
    return px;
}

// Procedural solid-color RGBA8 texture (n x n). Useful as a flat graybox surface
// or a default white tile.
inline std::vector<uint8_t> makeSolidRGBA(uint32_t n, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (size_t i = 0; i < (size_t)n * n; ++i) {
        px[i*4+0] = r; px[i*4+1] = g; px[i*4+2] = b; px[i*4+3] = 255;
    }
    return px;
}

// ============================================================================
// RICHER PROCEDURAL SCI-FI TEXTURES (S2 art uplift) — tileable RGBA8 generators
// that replace the flat blue/grey CHECKER look with industrial detention-facility
// surfaces: inset metal panels w/ seam grooves + corner bolts (walls), a grated /
// tiled deck with hazard trim (floors), and a recessed-panel light coffer
// (ceilings). All are SEAMLESS at the edges (every coordinate test wraps mod the
// texture size or mod the panel pitch that divides it), so they tile cleanly when
// the surface UVs repeat across big plates. Output is RGBA8, n*n*4 bytes, suitable
// for IRenderDevice::createTexture(px, n, n, /*srgb*/true) with mips.
//
// FUTURE (real PBR): to swap in Stable-Diffusion-generated tiling albedo later,
// generate sci-fi wall/floor/ceiling PNGs with the diffusers script (model at
// C:\GameDev\SD_Models\sd35), save under assets/textures/, then load via stbi_load
// (already linked) and feed the decoded RGBA8 straight into createTexture in place
// of these calls. These procedural maps are the no-GPU-contention fallback.
// ============================================================================

namespace detail {
// Cheap deterministic value-noise hash in [0,1) from integer pixel coords. Wraps
// on `n` so the noise itself is tileable (left edge == right edge sample).
inline float hash01(uint32_t x, uint32_t y, uint32_t n, uint32_t salt) {
    uint32_t h = (x % n) * 374761393u + (y % n) * 668265263u + salt * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;   // [0,1)
}
inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
// Tint a base byte by a 0..1 multiplier (the per-floor color), then add grime.
inline void putTinted(uint8_t* p, int r, int g, int b, const float tint3[3], int grime) {
    p[0] = clamp8((int)(r * tint3[0]) + grime);
    p[1] = clamp8((int)(g * tint3[1]) + grime);
    p[2] = clamp8((int)(b * tint3[2]) + grime);
    p[3] = 255;
}
const float kNoTint[3] = { 1.0f, 1.0f, 1.0f };
} // namespace detail

// WALLS — dark gunmetal base divided into a `panels`x`panels` grid of inset metal
// PANELS. Each panel has a darker recessed border (seam groove) + a faint highlight
// just inside it (bevel), small bolts in the panel corners, and per-pixel grime.
// `accent{r,g,b}` with accentH>0 paints a thin emissive accent strip a fraction
// `accentH` of the way up the tile (a lit conduit line); set accentH=0 for none.
// `tint3` multiplies the base palette (per-floor color) — pass detail::kNoTint to
// keep the raw gunmetal.
inline std::vector<uint8_t> makeSciFiPanelRGBA(uint32_t n, uint32_t panels,
                                               const float tint3[3] = detail::kNoTint,
                                               uint8_t accentR = 60, uint8_t accentG = 170, uint8_t accentB = 200,
                                               float accentH = 0.0f) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t pitch = std::max(1u, n / std::max(1u, panels)); // panel size (px)
    const uint32_t seam  = std::max(2u, pitch / 16);               // groove half-width
    const uint32_t bolt  = std::max(2u, pitch / 24);               // bolt radius
    const uint32_t boltInset = seam + bolt + 1;                    // bolt center offset from seam
    const int accentRow = accentH > 0.0f ? (int)((1.0f - accentH) * (float)n) : -1;
    const int accentBand = std::max(2, (int)(pitch / 12));
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t lx = x % pitch, ly = y % pitch;   // position within panel
            // Distance from the nearest seam (panel edge) along each axis.
            const uint32_t dx = std::min(lx, pitch - 1 - lx);
            const uint32_t dy = std::min(ly, pitch - 1 - ly);
            const uint32_t edge = std::min(dx, dy);
            // Gunmetal base + subtle per-pixel grime/noise.
            const int grime = (int)((hash01(x, y, n, 7u) - 0.5f) * 18.0f);
            int r = 70, g = 76, b = 86;       // gunmetal panel face
            if (edge < seam) {                 // recessed seam groove (darker)
                r = 34; g = 38; b = 46;
            } else if (edge < seam * 2) {      // bevel highlight just inside the groove
                r = 96; g = 104; b = 116;
            }
            // Corner bolts: a small bright dot near each panel corner.
            const uint32_t bx = std::min(lx, pitch - 1 - lx);
            const uint32_t by = std::min(ly, pitch - 1 - ly);
            const int ddx = (int)bx - (int)boltInset;
            const int ddy = (int)by - (int)boltInset;
            if ((uint32_t)(ddx*ddx + ddy*ddy) <= bolt * bolt) {
                r = 150; g = 156; b = 168;     // rivet head (bright metal)
                if (ddx*ddx + ddy*ddy >= (int)((bolt-1)*(bolt-1))) { r-=60; g-=60; b-=60; } // bolt shadow rim
            }
            putTinted(p, r, g, b, tint3, grime);
            // Emissive accent strip (drawn last, ignores tint so it stays a bright
            // conduit line). A thin horizontal band — tileable since it's a fixed row.
            if (accentRow >= 0) {
                const int d = (int)y - accentRow;
                if (d >= 0 && d < accentBand) {
                    const float k = 1.0f - (float)d / (float)accentBand;  // brightest at the top
                    p[0] = clamp8((int)(accentR * (0.4f + 0.6f * k)) + p[0] / 4);
                    p[1] = clamp8((int)(accentG * (0.4f + 0.6f * k)) + p[1] / 4);
                    p[2] = clamp8((int)(accentB * (0.4f + 0.6f * k)) + p[2] / 4);
                }
            }
        }
    }
    return px;
}

// FLOORS — a darker industrial DECK: a `tiles`x`tiles` grid of metal plates with
// recessed seams (grout/expansion gaps), a fine diagonal anti-slip tread pattern on
// the plate faces, grime pooling, and (when hazard=true) a yellow/black hazard
// stripe trim band around the OUTER edge of the tile (reads as caution trim where
// the floor texture meets walls). Tileable. `tint3` multiplies the base.
inline std::vector<uint8_t> makeFloorGrateRGBA(uint32_t n, uint32_t tiles,
                                               const float tint3[3] = detail::kNoTint,
                                               bool hazard = false) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t pitch = std::max(1u, n / std::max(1u, tiles));
    const uint32_t seam  = std::max(2u, pitch / 20);
    const uint32_t hazW  = std::max(3u, n / 28);   // hazard trim band width (texture edge)
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t lx = x % pitch, ly = y % pitch;
            const uint32_t dx = std::min(lx, pitch - 1 - lx);
            const uint32_t dy = std::min(ly, pitch - 1 - ly);
            const uint32_t edge = std::min(dx, dy);
            const int grime = (int)((hash01(x, y, n, 13u) - 0.5f) * 22.0f);
            int r = 44, g = 47, b = 54;        // dark deck plate
            // Diagonal tread ridges on the plate face (anti-slip), every 6 px.
            if (edge >= seam && (((x + y) / 6u) & 1u)) { r += 10; g += 10; b += 11; }
            if (edge < seam) { r = 24; g = 26; b = 30; }   // recessed seam (drainage gap)
            putTinted(p, r, g, b, tint3, grime);
            // Outer-edge hazard trim: yellow/black diagonal caution stripes in the
            // band within hazW of ANY texture border. Tileable (band on all 4 sides).
            if (hazard) {
                const uint32_t bd = std::min(std::min(x, n - 1 - x), std::min(y, n - 1 - y));
                if (bd < hazW) {
                    const bool stripe = (((x + y) / 8u) & 1u);
                    if (stripe) { p[0] = clamp8(190 + grime); p[1] = clamp8(160 + grime); p[2] = clamp8(20 + grime); }
                    else        { p[0] = clamp8(24 + grime);  p[1] = clamp8(24 + grime);  p[2] = clamp8(26 + grime); }
                    p[3] = 255;
                }
            }
        }
    }
    return px;
}

// CEILINGS — flatter + darker than the walls: a `coffers`x`coffers` grid of recessed
// panel coffers (groove lines), each with a faint central LIGHT-FIXTURE motif (a
// soft bright rectangle in the panel center, like a recessed luminaire) and light
// grime. No bolts (reads as overhead acoustic/utility panels). Tileable. `tint3`
// multiplies; `lit` toggles the fixture glow.
inline std::vector<uint8_t> makeCeilingPanelRGBA(uint32_t n, uint32_t coffers,
                                                 const float tint3[3] = detail::kNoTint,
                                                 bool lit = true) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t pitch = std::max(1u, n / std::max(1u, coffers));
    const uint32_t seam  = std::max(2u, pitch / 14);
    const uint32_t fixHalf = std::max(2u, pitch / 5);   // half-size of the central fixture
    const uint32_t half = pitch / 2;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t lx = x % pitch, ly = y % pitch;
            const uint32_t dx = std::min(lx, pitch - 1 - lx);
            const uint32_t dy = std::min(ly, pitch - 1 - ly);
            const uint32_t edge = std::min(dx, dy);
            const int grime = (int)((hash01(x, y, n, 23u) - 0.5f) * 12.0f);
            int r = 50, g = 53, b = 60;        // dark overhead panel
            if (edge < seam) { r = 30; g = 32; b = 38; }   // recessed coffer groove
            putTinted(p, r, g, b, tint3, grime);
            // Central recessed luminaire: a soft bright rectangle in the panel middle.
            if (lit) {
                const int cdx = (int)lx - (int)half;
                const int cdy = (int)ly - (int)half;
                if ((uint32_t)std::abs(cdx) < fixHalf && (uint32_t)std::abs(cdy) < fixHalf) {
                    // Soft falloff toward the fixture edge so it doesn't read as a hard box.
                    const float fx = 1.0f - (float)std::abs(cdx) / (float)fixHalf;
                    const float fy = 1.0f - (float)std::abs(cdy) / (float)fixHalf;
                    const float k = std::min(fx, fy);
                    p[0] = clamp8(p[0] + (int)(150.0f * k));
                    p[1] = clamp8(p[1] + (int)(155.0f * k));
                    p[2] = clamp8(p[2] + (int)(135.0f * k));
                }
            }
        }
    }
    return px;
}

} // namespace x3::prims
