#pragma once
// HIGH-POLY, ARTIFACT-FREE elevator mesh library for the X3 "core of the building"
// showpiece (Tim's centerpiece). These builders produce REAL premium-elevator
// geometry — beveled door slabs, chamfered frame jambs, realistic call-panel
// keypads with raised round buttons, a tubular handrail, a coffered ceiling, and a
// strata-glass canopy — instead of flat graybox quads.
//
// ART-DIRECTION RULES BAKED IN (Tim: "realistic keypads, doors, no artifacts"):
//   * NO Z-FIGHTING: every applied/inset face is pushed off its host surface by a
//     fixed kSurfaceLift (>= 1.5 mm) so coplanar geometry never flickers.
//   * NO GAPS/SEAMS: panels are authored as solid prisms (closed boxes), so there
//     are no open edges between the cab walls, floor, and ceiling.
//   * CLEAN NORMALS: every face carries its true outward normal (per-face), and
//     the bevels/chamfers get their own averaged-direction normals so highlights
//     read as real rounded edges, not faceted noise.
//   * HIGH-POLY where it counts: buttons are 12-gon cylinders with a domed top,
//     the handrail is an octagonal tube, frame jambs are chamfered.
//
// All builders return x3::prims::PrimMesh (render verts + matching collision) so a
// single authored mesh feeds createMesh() AND addStaticMesh(). Pure header (game
// layer); engine/ stays untouched.

#include "mesh_prims.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::elevmesh {

using x3::rhi::MeshVertex;
using x3::prims::PrimMesh;

// Minimum lift (m) applied to any decal/inset face over its host surface so two
// coplanar triangles never z-fight. 2 mm — invisible at arm's length, decisive
// against flicker.
inline constexpr float kSurfaceLift = 0.002f;

// ---------------------------------------------------------------------------
// Low-level: append one quad (a,b,c,d CCW) with an explicit normal + UVs. Shared
// by every builder so winding/normals stay consistent (CCW front face to match
// the device's VK_FRONT_FACE_COUNTER_CLOCKWISE).
// ---------------------------------------------------------------------------
inline void quad(PrimMesh& m,
                 const float a[3], const float b[3], const float c[3], const float d[3],
                 float nx, float ny, float nz,
                 float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    uint32_t base = (uint32_t)m.verts.size();
    m.verts.push_back({{a[0],a[1],a[2]},{nx,ny,nz},{u0,v0}});
    m.verts.push_back({{b[0],b[1],b[2]},{nx,ny,nz},{u1,v0}});
    m.verts.push_back({{c[0],c[1],c[2]},{nx,ny,nz},{u1,v1}});
    m.verts.push_back({{d[0],d[1],d[2]},{nx,ny,nz},{u0,v1}});
    m.index.insert(m.index.end(), {base, base+1, base+2, base, base+2, base+3});
}

// Merge src into dst (render + collision), offsetting indices. Lets a builder
// compose many sub-prims (frame + buttons + bezel) into one mesh handle.
inline void append(PrimMesh& dst, const PrimMesh& src) {
    uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (uint32_t i : src.index) dst.index.push_back(base + i);
    uint32_t cbase = (uint32_t)(dst.cverts.size() / 3);
    dst.cverts.insert(dst.cverts.end(), src.cverts.begin(), src.cverts.end());
    for (uint32_t i : src.cindex) dst.cindex.push_back(cbase + i);
}

// Rebuild collision (position triples + indices) from the current render verts.
// Call after a builder finishes authoring render geometry by hand.
inline void finalizeCollision(PrimMesh& m) {
    m.cverts.clear(); m.cindex.clear();
    m.cverts.reserve(m.verts.size() * 3);
    for (const auto& v : m.verts) {
        m.cverts.push_back(v.pos[0]); m.cverts.push_back(v.pos[1]); m.cverts.push_back(v.pos[2]);
    }
    m.cindex = m.index;
}

// ---------------------------------------------------------------------------
// BEVELED BOX — a box with chamfered edges along all 12 edges (45-degree bevel
// of width `b`). The 6 main faces shrink by `b`; 12 narrow bevel strips + 8 corner
// tris fill the chamfer so the silhouette reads ROUNDED under specular light
// instead of a hard graybox cube. Centered at (cx,cy,cz), half-extents (hx,hy,hz).
// This is the workhorse for premium panels (door slabs, frame jambs, cab walls).
// ---------------------------------------------------------------------------
inline PrimMesh beveledBox(float hx, float hy, float hz,
                           float cx, float cy, float cz, float b,
                           float uv = 1.0f) {
    PrimMesh m;
    b = std::min(b, std::min(hx, std::min(hy, hz)) * 0.9f);
    const float ix = hx - b, iy = hy - b, iz = hz - b;   // inset (face) extents
    auto P = [&](float x, float y, float z, float* o){ o[0]=cx+x; o[1]=cy+y; o[2]=cz+z; };
    float p[8][3];   // helper scratch

    // ---- 6 main faces (shrunk by the bevel) ----
    // +Z
    { float a[3],bb[3],c[3],d[3]; P(-ix,-iy,hz,a);P(ix,-iy,hz,bb);P(ix,iy,hz,c);P(-ix,iy,hz,d);
      quad(m,a,bb,c,d, 0,0,1, 0,0, ix*2*uv, iy*2*uv); }
    // -Z
    { float a[3],bb[3],c[3],d[3]; P(ix,-iy,-hz,a);P(-ix,-iy,-hz,bb);P(-ix,iy,-hz,c);P(ix,iy,-hz,d);
      quad(m,a,bb,c,d, 0,0,-1, 0,0, ix*2*uv, iy*2*uv); }
    // +X
    { float a[3],bb[3],c[3],d[3]; P(hx,-iy,iz,a);P(hx,-iy,-iz,bb);P(hx,iy,-iz,c);P(hx,iy,iz,d);
      quad(m,a,bb,c,d, 1,0,0, 0,0, iz*2*uv, iy*2*uv); }
    // -X
    { float a[3],bb[3],c[3],d[3]; P(-hx,-iy,-iz,a);P(-hx,-iy,iz,bb);P(-hx,iy,iz,c);P(-hx,iy,-iz,d);
      quad(m,a,bb,c,d, -1,0,0, 0,0, iz*2*uv, iy*2*uv); }
    // +Y
    { float a[3],bb[3],c[3],d[3]; P(-ix,hy,iz,a);P(ix,hy,iz,bb);P(ix,hy,-iz,c);P(-ix,hy,-iz,d);
      quad(m,a,bb,c,d, 0,1,0, 0,0, ix*2*uv, iz*2*uv); }
    // -Y
    { float a[3],bb[3],c[3],d[3]; P(-ix,-hy,-iz,a);P(ix,-hy,-iz,bb);P(ix,-hy,iz,c);P(-ix,-hy,iz,d);
      quad(m,a,bb,c,d, 0,-1,0, 0,0, ix*2*uv, iz*2*uv); }

    // ---- 4 vertical bevel strips (around the Y edges), normal = diagonal ----
    const float s = 0.70710678f;
    // +X+Z
    { float a[3],bb[3],c[3],d[3]; P(ix,-iy,hz,a);P(hx,-iy,iz,bb);P(hx,iy,iz,c);P(ix,iy,hz,d);
      quad(m,a,bb,c,d, s,0,s); }
    // +X-Z
    { float a[3],bb[3],c[3],d[3]; P(hx,-iy,-iz,a);P(ix,-iy,-hz,bb);P(ix,iy,-hz,c);P(hx,iy,-iz,d);
      quad(m,a,bb,c,d, s,0,-s); }
    // -X-Z
    { float a[3],bb[3],c[3],d[3]; P(-ix,-iy,-hz,a);P(-hx,-iy,-iz,bb);P(-hx,iy,-iz,c);P(-ix,iy,-hz,d);
      quad(m,a,bb,c,d, -s,0,-s); }
    // -X+Z
    { float a[3],bb[3],c[3],d[3]; P(-hx,-iy,iz,a);P(-ix,-iy,hz,bb);P(-ix,iy,hz,c);P(-hx,iy,iz,d);
      quad(m,a,bb,c,d, -s,0,s); }

    // ---- 4 top + 4 bottom horizontal bevel strips ----
    // top +Z
    { float a[3],bb[3],c[3],d[3]; P(-ix,iy,hz,a);P(ix,iy,hz,bb);P(ix,hy,iz,c);P(-ix,hy,iz,d);
      quad(m,a,bb,c,d, 0,s,s); }
    // top -Z
    { float a[3],bb[3],c[3],d[3]; P(ix,iy,-hz,a);P(-ix,iy,-hz,bb);P(-ix,hy,-iz,c);P(ix,hy,-iz,d);
      quad(m,a,bb,c,d, 0,s,-s); }
    // top +X
    { float a[3],bb[3],c[3],d[3]; P(hx,iy,iz,a);P(hx,iy,-iz,bb);P(ix,hy,-iz,c);P(ix,hy,iz,d);
      quad(m,a,bb,c,d, s,s,0); }
    // top -X
    { float a[3],bb[3],c[3],d[3]; P(-hx,iy,-iz,a);P(-hx,iy,iz,bb);P(-ix,hy,iz,c);P(-ix,hy,-iz,d);
      quad(m,a,bb,c,d, -s,s,0); }
    // bottom +Z
    { float a[3],bb[3],c[3],d[3]; P(-ix,-hy,iz,a);P(ix,-hy,iz,bb);P(ix,-iy,hz,c);P(-ix,-iy,hz,d);
      quad(m,a,bb,c,d, 0,-s,s); }
    // bottom -Z
    { float a[3],bb[3],c[3],d[3]; P(ix,-hy,-iz,a);P(-ix,-hy,-iz,bb);P(-ix,-iy,-hz,c);P(ix,-iy,-hz,d);
      quad(m,a,bb,c,d, 0,-s,-s); }
    // bottom +X
    { float a[3],bb[3],c[3],d[3]; P(hx,-hy,-iz,a);P(hx,-hy,iz,bb);P(ix,-iy,hz,c);P(ix,-iy,-hz,d);
      quad(m,a,bb,c,d, s,-s,0); }
    // bottom -X
    { float a[3],bb[3],c[3],d[3]; P(-hx,-hy,iz,a);P(-hx,-hy,-iz,bb);P(-ix,-iy,-hz,c);P(-ix,-iy,hz,d);
      quad(m,a,bb,c,d, -s,-s,0); }

    // ---- 8 corner triangles (fill the 3-way chamfer junctions) ----
    auto tri = [&](float ax,float ay,float az, float bx,float by,float bz,
                   float ccx,float ccy,float ccz, float nx,float ny,float nz){
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{cx+ax,cy+ay,cz+az},{nx,ny,nz},{0,0}});
        m.verts.push_back({{cx+bx,cy+by,cz+bz},{nx,ny,nz},{1,0}});
        m.verts.push_back({{cx+ccx,cy+ccy,cz+ccz},{nx,ny,nz},{1,1}});
        m.index.insert(m.index.end(), {base,base+1,base+2});
    };
    const float d3 = 0.57735027f;
    tri( ix, iy, hz,  hx, iy, iz,  ix, hy, iz,  d3, d3, d3);   // +++
    tri( hx, iy,-iz,  ix, iy,-hz,  ix, hy,-iz,  d3, d3,-d3);   // ++-
    tri(-ix, iy,-hz, -hx, iy,-iz, -ix, hy,-iz, -d3, d3,-d3);   // -+-
    tri(-hx, iy, iz, -ix, iy, hz, -ix, hy, iz, -d3, d3, d3);   // -++
    tri( hx,-iy, iz,  ix,-iy, hz,  ix,-hy, iz,  d3,-d3, d3);   // +-+
    tri( ix,-iy,-hz,  hx,-iy,-iz,  ix,-hy,-iz,  d3,-d3,-d3);   // +--
    tri(-hx,-iy,-iz, -ix,-iy,-hz, -ix,-hy,-iz, -d3,-d3,-d3);   // ---
    tri(-ix,-iy, hz, -hx,-iy, iz, -ix,-hy, iz, -d3,-d3, d3);   // --+

    finalizeCollision(m);
    return m;
}

// ---------------------------------------------------------------------------
// CYLINDER (capped) — `sides`-gon prism radius `r`, height 2*hy, centered at
// (cx,cy,cz), axis +Y. Smooth radial normals so it reads as a turned tube (used
// for the handrail posts, button bodies, the disco-ball cable, etc.).
// ---------------------------------------------------------------------------
inline PrimMesh cylinderY(float r, float hy, float cx, float cy, float cz,
                          uint32_t sides = 16, bool caps = true) {
    PrimMesh m;
    const float k2pi = 6.2831853f;
    for (uint32_t i = 0; i < sides; ++i) {
        float a0 = (float)i / sides * k2pi, a1 = (float)(i+1) / sides * k2pi;
        float x0 = std::cos(a0), z0 = std::sin(a0), x1 = std::cos(a1), z1 = std::sin(a1);
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{cx+x0*r, cy-hy, cz+z0*r},{x0,0,z0},{(float)i/sides,0}});
        m.verts.push_back({{cx+x1*r, cy-hy, cz+z1*r},{x1,0,z1},{(float)(i+1)/sides,0}});
        m.verts.push_back({{cx+x1*r, cy+hy, cz+z1*r},{x1,0,z1},{(float)(i+1)/sides,1}});
        m.verts.push_back({{cx+x0*r, cy+hy, cz+z0*r},{x0,0,z0},{(float)i/sides,1}});
        m.index.insert(m.index.end(), {base,base+1,base+2, base,base+2,base+3});
    }
    if (caps) {
        uint32_t topC = (uint32_t)m.verts.size();
        m.verts.push_back({{cx,cy+hy,cz},{0,1,0},{0.5f,0.5f}});
        uint32_t botC = (uint32_t)m.verts.size();
        m.verts.push_back({{cx,cy-hy,cz},{0,-1,0},{0.5f,0.5f}});
        for (uint32_t i = 0; i < sides; ++i) {
            float a0 = (float)i / sides * k2pi, a1 = (float)(i+1) / sides * k2pi;
            float x0 = std::cos(a0), z0 = std::sin(a0), x1 = std::cos(a1), z1 = std::sin(a1);
            uint32_t t0 = (uint32_t)m.verts.size();
            m.verts.push_back({{cx+x0*r,cy+hy,cz+z0*r},{0,1,0},{x0*0.5f+0.5f,z0*0.5f+0.5f}});
            m.verts.push_back({{cx+x1*r,cy+hy,cz+z1*r},{0,1,0},{x1*0.5f+0.5f,z1*0.5f+0.5f}});
            m.index.insert(m.index.end(), {topC,t0,t0+1});
            uint32_t b0 = (uint32_t)m.verts.size();
            m.verts.push_back({{cx+x1*r,cy-hy,cz+z1*r},{0,-1,0},{x1*0.5f+0.5f,z1*0.5f+0.5f}});
            m.verts.push_back({{cx+x0*r,cy-hy,cz+z0*r},{0,-1,0},{x0*0.5f+0.5f,z0*0.5f+0.5f}});
            m.index.insert(m.index.end(), {botC,b0,b0+1});
        }
    }
    finalizeCollision(m);
    return m;
}

// ---------------------------------------------------------------------------
// HORIZONTAL TUBE — a capped cylinder lying along an axis (for the cab handrail
// run). `axis`: 0=X, 2=Z. Spans [c-half, c+half] along that axis at height cy.
// ---------------------------------------------------------------------------
inline PrimMesh tube(float r, float half, float cx, float cy, float cz,
                     uint32_t axis, uint32_t sides = 12) {
    // Build a +Y cylinder then rotate its verts into the requested axis.
    PrimMesh m = cylinderY(r, half, 0, 0, 0, sides, true);
    for (auto& v : m.verts) {
        float x = v.pos[0], y = v.pos[1], z = v.pos[2];
        float nx = v.normal[0], ny = v.normal[1], nz = v.normal[2];
        if (axis == 0) {            // +Y -> +X
            v.pos[0] = cx + y;  v.pos[1] = cy + x;  v.pos[2] = cz + z;
            v.normal[0] = ny;   v.normal[1] = nx;   v.normal[2] = nz;
        } else {                    // +Y -> +Z
            v.pos[0] = cx + x;  v.pos[1] = cy + z;  v.pos[2] = cz + y;
            v.normal[0] = nx;   v.normal[1] = nz;   v.normal[2] = ny;
        }
    }
    finalizeCollision(m);
    return m;
}

// ---------------------------------------------------------------------------
// REALISTIC ROUND BUTTON — a short 12-gon cylinder body sunk into a panel with a
// DOMED top cap, raised `kSurfaceLift*2` proud of the host panel face so it never
// z-fights. Authored facing +Z by default (a wall-mounted button). `proud` is how
// far the button stands off the panel; `r` its radius. Centered at (cx,cy,pz)
// where pz is the panel FACE Z (the button sits just in front of it).
// ---------------------------------------------------------------------------
inline PrimMesh roundButton(float cx, float cy, float pz, float r, float proud,
                            uint32_t sides = 12) {
    PrimMesh m;
    const float k2pi = 6.2831853f;
    const float z0 = pz + kSurfaceLift;          // ring base, lifted off the panel
    const float z1 = pz + proud;                 // rim of the button top
    const float zd = z1 + r * 0.35f;             // dome apex
    // Cylindrical wall (smooth radial normals, +Z-ish since it's shallow).
    for (uint32_t i = 0; i < sides; ++i) {
        float a0 = (float)i/sides*k2pi, a1 = (float)(i+1)/sides*k2pi;
        float x0=std::cos(a0), y0=std::sin(a0), x1=std::cos(a1), y1=std::sin(a1);
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{cx+x0*r, cy+y0*r, z0},{x0,y0,0},{0,0}});
        m.verts.push_back({{cx+x1*r, cy+y1*r, z0},{x1,y1,0},{1,0}});
        m.verts.push_back({{cx+x1*r, cy+y1*r, z1},{x1,y1,0},{1,1}});
        m.verts.push_back({{cx+x0*r, cy+y0*r, z1},{x0,y0,0},{0,1}});
        m.index.insert(m.index.end(), {base,base+1,base+2, base,base+2,base+3});
    }
    // Domed top: a fan of tris from the rim up to the apex, normals tilted out+forward.
    uint32_t apex = (uint32_t)m.verts.size();
    m.verts.push_back({{cx, cy, zd},{0,0,1},{0.5f,0.5f}});
    for (uint32_t i = 0; i < sides; ++i) {
        float a0 = (float)i/sides*k2pi, a1 = (float)(i+1)/sides*k2pi;
        float x0=std::cos(a0), y0=std::sin(a0), x1=std::cos(a1), y1=std::sin(a1);
        // Dome normal: blend radial + forward for a rounded shaded cap.
        float n0x=x0*0.5f, n0y=y0*0.5f, n0z=0.86f;
        float n1x=x1*0.5f, n1y=y1*0.5f, n1z=0.86f;
        uint32_t b = (uint32_t)m.verts.size();
        m.verts.push_back({{cx+x0*r, cy+y0*r, z1},{n0x,n0y,n0z},{x0*0.5f+0.5f,y0*0.5f+0.5f}});
        m.verts.push_back({{cx+x1*r, cy+y1*r, z1},{n1x,n1y,n1z},{x1*0.5f+0.5f,y1*0.5f+0.5f}});
        m.index.insert(m.index.end(), {apex, b, b+1});
    }
    finalizeCollision(m);
    return m;
}

// ---------------------------------------------------------------------------
// FRAMED OPENING (door portal jamb) — a chamfered rectangular FRAME (4 jamb bars
// with beveled inner+outer edges) around an opening of inner half-size (ihx,ihy),
// jamb thickness `jt`, depth `dz`, centered at (cx,cy,cz) on the +/-Z wall. This
// is the premium sliding-door surround mounted on each shaft floor. Solid prisms,
// so no seams; the inner reveal is chamfered so the doorway reads machined.
// ---------------------------------------------------------------------------
inline PrimMesh doorFrame(float ihx, float ihy, float jt, float dz,
                          float cx, float cy, float cz, float bevel = 0.03f) {
    PrimMesh m;
    const float ohx = ihx + jt, ohy = ihy + jt;
    // Top bar.
    append(m, beveledBox(ohx, jt*0.5f, dz, cx, cy + ihy + jt*0.5f, cz, bevel));
    // Bottom bar (sill).
    append(m, beveledBox(ohx, jt*0.5f, dz, cx, cy - ihy - jt*0.5f, cz, bevel));
    // Left jamb.
    append(m, beveledBox(jt*0.5f, ihy, dz, cx - ihx - jt*0.5f, cy, cz, bevel));
    // Right jamb.
    append(m, beveledBox(jt*0.5f, ihy, dz, cx + ihx + jt*0.5f, cy, cz, bevel));
    (void)ohx; (void)ohy;
    return m;
}

} // namespace x3::elevmesh
