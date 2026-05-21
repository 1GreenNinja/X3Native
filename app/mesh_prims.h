#pragma once
// Procedural MeshVertex primitive builders for the graybox test level (S2).
// Factored out of app/main.cpp (S1). Engine stays pure: this lives under app/.
//
// Two flavors of geometry are produced from the same authoring call:
//   - RENDER geometry  : x3::rhi::MeshVertex {pos, normal, uv}  -> createMesh
//   - COLLISION geometry: position-only float triples + indices -> addStaticMesh
// A single box builder fills BOTH so the renderer and Jolt see identical shapes.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
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

} // namespace x3::prims
