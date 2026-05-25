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

// Wall variants — pick a motif so adjacent corridor surfaces don't read as one
// repeating tile. PLAIN = clean panel, CONDUIT = vertical cable-tray duct on one
// panel column, VENT = louvered air-vent / access-hatch inset in a panel.
enum class WallVariant : uint32_t { Plain = 0, Conduit = 1, Vent = 2 };

// WALLS — a CALM, large-scale gunmetal panel wall. The tile is divided into a small
// `panels`x`panels` grid (default 2 → big plates, lots of negative space) of inset
// metal panels: a clean face, a crisp recessed seam groove between plates, a subtle
// bevel highlight, discreet corner bolts, and only faint low-frequency grime (NOT
// the busy per-pixel speckle of the first pass — that read like a bedspread). A thin
// emissive accent line is optional. `variant` overlays a distinguishing motif so
// corridors get visual variety:
//   Plain   — just the panel grid (the calm baseline).
//   Conduit — a recessed cable-tray / conduit duct running floor-to-ceiling down one
//             panel column, with evenly spaced clamp bands.
//   Vent    — a louvered air-return vent (horizontal slats) inset in the middle panel.
// `tint3` multiplies the base palette (per-floor color); pass detail::kNoTint to keep
// raw gunmetal. SEAMLESS: every test wraps mod n or mod the panel pitch.
inline std::vector<uint8_t> makeSciFiPanelRGBA(uint32_t n, uint32_t panels,
                                               const float tint3[3] = detail::kNoTint,
                                               uint8_t accentR = 60, uint8_t accentG = 170, uint8_t accentB = 200,
                                               float accentH = 0.0f,
                                               WallVariant variant = WallVariant::Plain) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    panels = std::max(1u, panels);
    const uint32_t pitch = std::max(1u, n / panels);          // panel size (px) — large
    const uint32_t seam  = std::max(2u, pitch / 22);          // groove half-width (thin/clean)
    const uint32_t bolt  = std::max(2u, pitch / 30);          // bolt radius (discreet)
    const uint32_t boltInset = seam + bolt + 3;               // bolt center offset from seam
    const int accentRow = accentH > 0.0f ? (int)((1.0f - accentH) * (float)n) : -1;
    const int accentBand = std::max(2, (int)(pitch / 14));

    // Conduit duct geometry: a vertical band centered on one panel column (column 0).
    const uint32_t ductCx   = pitch / 2;                      // duct center within a panel column
    const uint32_t ductHalf = std::max(4u, pitch / 7);        // duct half-width
    // Clamp-band spacing up the duct. Keyed on (y % clampPitch), so for the texture to
    // tile vertically clampPitch MUST divide n — derive it as n / bands (8 bands).
    const uint32_t clampPitch = std::max(8u, n / 8);
    // Vent geometry: a louvered rectangle inset in the center of the middle panel.
    const uint32_t ventCol  = panels / 2;                     // which panel column hosts the vent
    const uint32_t ventHalfX = std::max(6u, pitch / 3);
    const uint32_t ventHalfY = std::max(6u, (uint32_t)(pitch * 0.40f));
    const uint32_t louver   = std::max(3u, pitch / 16);       // louver slat pitch

    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t lx = x % pitch, ly = y % pitch;    // position within panel
            const uint32_t colX = (x / pitch) % panels;       // panel column index
            // Distance from the nearest seam (panel edge) along each axis.
            const uint32_t dx = std::min(lx, pitch - 1 - lx);
            const uint32_t dy = std::min(ly, pitch - 1 - ly);
            const uint32_t edge = std::min(dx, dy);
            // CALM base: faint LOW-FREQUENCY shading only (averaged 2x2 noise so it
            // reads as gentle large-scale mottling, not per-pixel quilt speckle).
            const float nlo = (hash01(x / 24, y / 24, std::max(1u, n / 24), 7u) - 0.5f);
            const int grime = (int)(nlo * 9.0f);
            int r = 78, g = 84, b = 94;        // clean gunmetal panel face
            if (edge < seam) {                 // recessed seam groove (darker, crisp)
                r = 40; g = 44; b = 52;
            } else if (edge < seam * 2) {      // bevel highlight just inside the groove
                r = 100; g = 108; b = 120;
            }
            // Corner bolts: a small bright dot near each panel corner (discreet).
            const int ddx = (int)dx - (int)boltInset;
            const int ddy = (int)dy - (int)boltInset;
            if ((uint32_t)(ddx*ddx + ddy*ddy) <= bolt * bolt) {
                r = 150; g = 156; b = 168;     // rivet head (bright metal)
                if (ddx*ddx + ddy*ddy >= (int)((bolt-1)*(bolt-1))) { r-=55; g-=55; b-=55; }
            }

            // ---- Variant motif overlays (drawn over the calm base) -------------
            if (variant == WallVariant::Conduit && colX == 0) {
                const int dd = (int)lx - (int)ductCx;
                const uint32_t ad = (uint32_t)std::abs(dd);
                if (ad <= ductHalf) {
                    // Recessed duct channel: darker trough, lighter raised rails at the lips.
                    if (ad > ductHalf - 3) { r = 110; g = 116; b = 128; }   // lip rail (raised)
                    else                   { r = 46;  g = 50;  b = 58;  }   // duct trough
                    // Evenly spaced clamp bands across the duct (tileable: keyed on y % clampPitch).
                    if ((y % clampPitch) < std::max(2u, clampPitch / 6)) { r = 120; g = 126; b = 138; }
                }
            } else if (variant == WallVariant::Vent && colX == ventCol) {
                const int vx = (int)lx - (int)(pitch / 2);
                const int vy = (int)ly - (int)(pitch / 2);
                const uint32_t avx = (uint32_t)std::abs(vx);
                const uint32_t avy = (uint32_t)std::abs(vy);
                if (avx <= ventHalfX && avy <= ventHalfY) {
                    if (avx > ventHalfX - 3 || avy > ventHalfY - 3) {
                        r = 116; g = 122; b = 134;                          // vent bezel (raised frame)
                    } else {
                        // Horizontal louver slats: dark slot then a thin highlight ridge.
                        const bool slot = ((ly / louver) & 1u);
                        if (slot) { r = 30; g = 33; b = 39; }               // shadowed slot
                        else      { r = 66; g = 71; b = 80; }               // slat face
                    }
                }
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

// FLOORS — an unmistakable top-down walkable DECK. Big square floor PLATES (a small
// `tiles`x`tiles` grid: default 2 → large plates, a clearly different/larger scale
// than the wall panels) separated by DEEP recessed seams (an obvious cross/grid of
// gaps you read as a tiled floor from above). Each plate carries a subtle raised
// DIAMOND-PLATE tread (the classic anti-slip checker-plate lozenges) plus four small
// countersunk bolts at its corners, and gentle low-frequency grime pooling. When
// `hazard` is set, a yellow/black caution stripe runs around the OUTER edge of the
// tile (reads as floor trim where the deck meets a wall). The motif (top-down plates
// + diamond tread + drainage seams) is deliberately UNLIKE the vertical wall panels,
// so the floor never reads as a wall. SEAMLESS. `tint3` multiplies the base.
inline std::vector<uint8_t> makeFloorGrateRGBA(uint32_t n, uint32_t tiles,
                                               const float tint3[3] = detail::kNoTint,
                                               bool hazard = false) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    tiles = std::max(1u, tiles);
    const uint32_t pitch = std::max(1u, n / tiles);    // BIG plate size (px)
    const uint32_t seam  = std::max(3u, pitch / 14);   // DEEP recessed seam (wide drainage gap)
    const uint32_t bolt  = std::max(2u, pitch / 26);   // countersunk corner bolt radius
    const uint32_t boltInset = seam + bolt + 4;        // bolt offset in from the seam
    const uint32_t hazW  = std::max(3u, n / 26);       // hazard trim band width (texture edge)
    // Diamond-plate tread cell (the raised lozenges). Sized off the plate so it scales.
    const uint32_t tread = std::max(8u, pitch / 8);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t lx = x % pitch, ly = y % pitch;
            const uint32_t dx = std::min(lx, pitch - 1 - lx);
            const uint32_t dy = std::min(ly, pitch - 1 - ly);
            const uint32_t edge = std::min(dx, dy);
            // Gentle low-frequency grime (large-scale pooling, not per-pixel speckle).
            const float nlo = (hash01(x / 20, y / 20, std::max(1u, n / 20), 13u) - 0.5f);
            const int grime = (int)(nlo * 16.0f);
            int r = 52, g = 55, b = 62;        // mid-grey deck plate face (lighter than walls)

            // Raised DIAMOND-PLATE tread on the plate face: a brick-offset lattice of
            // small lozenges. Rows offset every other tread row so it reads as the
            // classic checker-plate diamond, with a lit top-left lip + shadowed base.
            if (edge >= seam) {
                const uint32_t row = ly / tread;
                const uint32_t off = (row & 1u) ? tread / 2 : 0u;       // brick offset
                const uint32_t cxr = (lx + off) % tread;
                const uint32_t cyr = ly % tread;
                const int ldx = (int)cxr - (int)(tread / 2);
                const int ldy = (int)cyr - (int)(tread / 2);
                // Diamond (L1) distance: small raised lozenge in the cell center.
                const uint32_t dloz = (uint32_t)(std::abs(ldx) + std::abs(ldy));
                if (dloz < tread / 3) {
                    // Lit on the up-left side, shadowed on the down-right -> reads raised.
                    if (ldx + ldy < 0) { r += 16; g += 16; b += 17; }    // highlight
                    else               { r -= 8;  g -= 8;  b -= 9;  }    // shadow
                }
            }

            // Countersunk corner bolts on each plate corner (top-down screw heads).
            const int bdx = (int)dx - (int)boltInset;
            const int bdy = (int)dy - (int)boltInset;
            if ((uint32_t)(bdx*bdx + bdy*bdy) <= bolt * bolt) {
                r = 96; g = 100; b = 110;                                // bolt head
                if (bdx*bdx + bdy*bdy >= (int)((bolt-1)*(bolt-1))) { r-=40; g-=40; b-=40; }
            }

            // DEEP recessed seam (drainage gap between plates) — drawn last so it cuts
            // through the tread, giving the strong top-down grid that says "floor".
            if (edge < seam) {
                r = 22; g = 24; b = 28;
                if (edge < seam / 2) { r = 16; g = 17; b = 20; }         // darkest at gap center
            }

            putTinted(p, r, g, b, tint3, grime);
            // Outer-edge hazard trim: yellow/black diagonal caution stripes in the
            // band within hazW of ANY texture border. Tileable (band on all 4 sides).
            if (hazard) {
                const uint32_t bd = std::min(std::min(x, n - 1 - x), std::min(y, n - 1 - y));
                if (bd < hazW) {
                    const bool stripe = (((x + y) / 10u) & 1u);
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
