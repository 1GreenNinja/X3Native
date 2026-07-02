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

// A faceted CRYSTAL / energy cell: a 6-sided prism band in y=[-midH,+midH] capped
// by pointed apexes at +/-(midH+tipH) — a hex bipyramid with a straight midsection,
// the pointed-crystal language of Tim's Lab2 reference. Per-face FLAT normals so it
// reads as sharp facets under emissive/glass shading. Render-only (no collision).
inline PrimMesh makeCrystal(float r, float midH, float tipH) {
    PrimMesh m;
    const int N = 6;
    const float kPiL = 3.14159265f;
    auto pushTri = [&](float ax,float ay,float az, float bx,float by,float bz,
                       float cx,float cy,float cz) {
        // Flat normal, auto-oriented OUTWARD (away from the crystal's Y axis center).
        float ux=bx-ax, uy=by-ay, uz=bz-az, vx=cx-ax, vy=cy-ay, vz=cz-az;
        float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
        float cxm=(ax+bx+cx)/3.0f, cym=(ay+by+cy)/3.0f, czm=(az+bz+cz)/3.0f;
        // Outward reference: from the axis point at the tri's height to the centroid.
        if (nx*cxm + nz*czm + ny*(cym) < 0.0f) { // pointing inward -> swap b,c
            std::swap(bx,cx); std::swap(by,cy); std::swap(bz,cz);
            nx=-nx; ny=-ny; nz=-nz;
        }
        float l=std::sqrt(nx*nx+ny*ny+nz*nz); if(l<1e-6f)l=1.0f; nx/=l;ny/=l;nz/=l;
        uint32_t base=(uint32_t)m.verts.size();
        m.verts.push_back({{ax,ay,az},{nx,ny,nz},{0,0}});
        m.verts.push_back({{bx,by,bz},{nx,ny,nz},{1,0}});
        m.verts.push_back({{cx,cy,cz},{nx,ny,nz},{0.5f,1}});
        m.index.insert(m.index.end(), {base, base+1, base+2});
    };
    float rx[6], rz[6];
    for (int i=0;i<N;++i){ float a=(float)i*(kPiL/3.0f); rx[i]=std::cos(a)*r; rz[i]=std::sin(a)*r; }
    const float topY=midH+tipH, botY=-(midH+tipH);
    for (int i=0;i<N;++i){
        int j=(i+1)%N;
        // side quad (top ring i,j ; bottom ring j,i)
        pushTri(rx[i],midH,rz[i],  rx[j],midH,rz[j],  rx[j],-midH,rz[j]);
        pushTri(rx[i],midH,rz[i],  rx[j],-midH,rz[j], rx[i],-midH,rz[i]);
        // top facet to apex, bottom facet to apex
        pushTri(0,topY,0, rx[i],midH,rz[i],  rx[j],midH,rz[j]);
        pushTri(0,botY,0, rx[i],-midH,rz[i], rx[j],-midH,rz[j]);
    }
    return m;
}

// A faceted CRYSTAL shard: an N-sided prism rooted at y=0 that rises to a
// SHOULDER ring, then tapers to a single sharp APEX — a pointed gem (used by the
// surface-landing world's glowing crystal cluster). Per-facet FLAT normals so the
// faces catch light crisply. Radius `r`, total `height`, `sides` facets. Render
// geometry only (visual prop; no collision). Built centered on the Y axis at the
// origin — position/lean it via the entity transform.
inline PrimMesh makeCrystal(float r, float height, int sides = 6) {
    PrimMesh m;
    if (sides < 3) sides = 3;
    const float shoulderY = height * 0.68f;   // shaft top / where the point begins
    const float baseY      = 0.0f;
    const float baseR      = r * 0.82f;        // slightly narrower at the root
    // Emit one flat-shaded triangle with an outward normal (away from the Y axis).
    auto tri = [&](float ax,float ay,float az, float bx,float by,float bz,
                   float cx,float cy,float cz) {
        float ux=bx-ax, uy=by-ay, uz=bz-az;
        float vx=cx-ax, vy=cy-ay, vz=cz-az;
        float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
        float len=std::sqrt(nx*nx+ny*ny+nz*nz); if(len<1e-6f) len=1.0f;
        nx/=len; ny/=len; nz/=len;
        float mx=(ax+bx+cx)/3.0f, mz=(az+bz+cz)/3.0f;   // facet center (xz = outward ref)
        if (nx*mx + nz*mz < 0.0f) { nx=-nx; ny=-ny; nz=-nz;
            std::swap(bx,cx); std::swap(by,cy); std::swap(bz,cz); }
        uint32_t base=(uint32_t)m.verts.size();
        m.verts.push_back({{ax,ay,az},{nx,ny,nz},{0,0}});
        m.verts.push_back({{bx,by,bz},{nx,ny,nz},{1,0}});
        m.verts.push_back({{cx,cy,cz},{nx,ny,nz},{0.5f,1}});
        m.index.insert(m.index.end(), {base, base+1, base+2});
    };
    const float TWO_PI = 6.2831853f;
    for (int i = 0; i < sides; ++i) {
        float a0 = TWO_PI * i / sides, a1 = TWO_PI * (i + 1) / sides;
        float c0=std::cos(a0), s0=std::sin(a0), c1=std::cos(a1), s1=std::sin(a1);
        // Shaft quad (base ring -> shoulder ring) as two triangles.
        float bx0=baseR*c0, bz0=baseR*s0, bx1=baseR*c1, bz1=baseR*s1;
        float sx0=r*c0,     sz0=r*s0,     sx1=r*c1,     sz1=r*s1;
        tri(bx0,baseY,bz0,  bx1,baseY,bz1,  sx1,shoulderY,sz1);
        tri(bx0,baseY,bz0,  sx1,shoulderY,sz1,  sx0,shoulderY,sz0);
        // Point facet (shoulder ring -> apex).
        tri(sx0,shoulderY,sz0,  sx1,shoulderY,sz1,  0.0f,height,0.0f);
    }
    return m;
}

// A walkable RAMP wedge: a sloped top surface rising from y=0 at the LOW edge to
// y=`rise` at the HIGH edge, over a horizontal run `run`, `halfW` wide. Built in
// LOCAL space centered on the run axis at (cx,cy,cz) where cy is the LOW floor:
//   - `axis`==1 (run along +Z): low edge at z=cz, high edge at z=cz+run (slope up
//     toward +Z). Width spans x in [cx-halfW, cx+halfW].
//   - `axis`==0 (run along +X): low edge at x=cx, high edge at x=cx+run.
// `dir` flips the climb direction (+1 climbs toward +axis, -1 toward -axis). The
// solid fills from the floor (cy) up to the sloped top, so a character walks UP it.
// Produces render (with up-facing normal on the ramp top) + collision geometry.
inline PrimMesh makeRamp(float cx, float cy, float cz,
                         float halfW, float run, float rise,
                         uint32_t axis, float dir,
                         float uvScale = 1.0f) {
    PrimMesh m;
    auto quad = [&](float ax,float ay,float az, float bx,float by,float bz,
                    float ccx,float ccy,float ccz, float dx,float dy,float dz,
                    float nx,float ny,float nz, float u, float v) {
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{ax,ay,az},{nx,ny,nz},{0,0}});
        m.verts.push_back({{bx,by,bz},{nx,ny,nz},{u,0}});
        m.verts.push_back({{ccx,ccy,ccz},{nx,ny,nz},{u,v}});
        m.verts.push_back({{dx,dy,dz},{nx,ny,nz},{0,v}});
        m.index.insert(m.index.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    // 6 corner points of the wedge. Run axis local coordinate `s` in [0,run]*dir.
    const float s0 = 0.0f, s1 = run * dir;
    auto P = [&](float w, float s, float yy, float& ox, float& oy, float& oz) {
        oy = cy + yy;
        if (axis == 1) { ox = cx + w; oz = cz + s; }
        else           { ox = cx + s; oz = cz + w; }
    };
    // Low edge (s0): floor only. High edge (s1): floor up to rise. Top is sloped.
    float LL[3], LR[3], HLb[3], HRb[3], HLt[3], HRt[3];
    P(-halfW, s0, 0,    LL[0],LL[1],LL[2]);
    P( halfW, s0, 0,    LR[0],LR[1],LR[2]);
    P(-halfW, s1, 0,    HLb[0],HLb[1],HLb[2]);
    P( halfW, s1, 0,    HRb[0],HRb[1],HRb[2]);
    P(-halfW, s1, rise, HLt[0],HLt[1],HLt[2]);
    P( halfW, s1, rise, HRt[0],HRt[1],HRt[2]);
    const float su = run * uvScale, sv = halfW * 2 * uvScale;
    // Sloped TOP: low edge (LL,LR) up to high-top (HLt,HRt). Up-ish normal.
    quad(LL[0],LL[1],LL[2], LR[0],LR[1],LR[2], HRt[0],HRt[1],HRt[2], HLt[0],HLt[1],HLt[2], 0,1,0, su, sv);
    // HIGH vertical face (riser at the top end). Normal along +run.
    {
        const float nz = (axis==1) ? dir : 0.0f, nx = (axis==1) ? 0.0f : dir;
        quad(HLb[0],HLb[1],HLb[2], HRb[0],HRb[1],HRb[2], HRt[0],HRt[1],HRt[2], HLt[0],HLt[1],HLt[2], nx,0,nz, sv, rise*uvScale);
    }
    // Two side triangles (walls of the wedge). Emit as degenerate quads (tri).
    {
        // Left side (w=-halfW): LL, HLb, HLt.
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{LL[0],LL[1],LL[2]},{-1,0,0},{0,0}});
        m.verts.push_back({{HLb[0],HLb[1],HLb[2]},{-1,0,0},{1,0}});
        m.verts.push_back({{HLt[0],HLt[1],HLt[2]},{-1,0,0},{1,1}});
        m.index.insert(m.index.end(), {base, base+1, base+2});
        // Right side (w=+halfW): LR, HRt, HRb.
        base = (uint32_t)m.verts.size();
        m.verts.push_back({{LR[0],LR[1],LR[2]},{1,0,0},{0,0}});
        m.verts.push_back({{HRt[0],HRt[1],HRt[2]},{1,0,0},{1,1}});
        m.verts.push_back({{HRb[0],HRb[1],HRb[2]},{1,0,0},{1,0}});
        m.index.insert(m.index.end(), {base, base+1, base+2});
    }
    // Collision: reuse render positions + indices.
    m.cverts.reserve(m.verts.size() * 3);
    for (const auto& vtx : m.verts) {
        m.cverts.push_back(vtx.pos[0]); m.cverts.push_back(vtx.pos[1]); m.cverts.push_back(vtx.pos[2]);
    }
    m.cindex = m.index;
    return m;
}

// A CANTED "/" STRUT BLADE: a sheared rectangular prism whose cross-section is
// constant (halfW along the TANGENTIAL axis, halfT along the RADIAL axis) but
// whose center SLIDES from (baseX,baseY,baseZ) to (topX,topY,topZ) as it rises —
// giving the inward-leaning "/" cant of the showroom's tripod legs. The blade is
// oriented by the RADIAL-OUT unit vector (rox,roz): thickness (halfT) runs along
// it, width (halfW) along the perpendicular (tangential) axis (-roz,rox). Produces
// render (per-face normals/UVs) + collision geometry. Eight corners: a bottom quad
// at baseY around (baseX,baseZ) and a top quad at topY around (topX,topZ); the
// four slanted side faces connect them, so the whole prism leans.
//
// If `hollow` is true, the +radial (outward) face is OMITTED from BOTH render and
// collision (so a doorway can be set into that face and the player can step into
// the hollow interior) — used for the stair-bearing strut.
inline PrimMesh makeCantedStrut(float baseX, float baseY, float baseZ,
                                float topX,  float topY,  float topZ,
                                float halfW, float halfT,
                                float rox, float roz,
                                float uvScale = 0.5f, bool hollow = false) {
    PrimMesh m;
    // Tangential unit (perpendicular to radial-out, in XZ): rotate radial 90 deg.
    const float tx = -roz, tz = rox;
    // Eight corners. b* = bottom (baseY), t* = top (topY). Index by (radial sign,
    // tangential sign): name as [r][w] with r in {-1=in,+1=out}, w in {-1,+1}.
    auto corner = [&](float ccx, float ccz, float yy, float rs, float ws,
                      float& ox, float& oy, float& oz) {
        ox = ccx + rox * halfT * rs + tx * halfW * ws;
        oz = ccz + roz * halfT * rs + tz * halfW * ws;
        oy = yy;
    };
    float Bmm[3], Bmp[3], Bpm[3], Bpp[3];   // bottom: [r-][w-],[r-][w+],[r+][w-],[r+][w+]
    float Tmm[3], Tmp[3], Tpm[3], Tpp[3];   // top
    corner(baseX, baseZ, baseY, -1, -1, Bmm[0], Bmm[1], Bmm[2]);
    corner(baseX, baseZ, baseY, -1, +1, Bmp[0], Bmp[1], Bmp[2]);
    corner(baseX, baseZ, baseY, +1, -1, Bpm[0], Bpm[1], Bpm[2]);
    corner(baseX, baseZ, baseY, +1, +1, Bpp[0], Bpp[1], Bpp[2]);
    corner(topX,  topZ,  topY,  -1, -1, Tmm[0], Tmm[1], Tmm[2]);
    corner(topX,  topZ,  topY,  -1, +1, Tmp[0], Tmp[1], Tmp[2]);
    corner(topX,  topZ,  topY,  +1, -1, Tpm[0], Tpm[1], Tpm[2]);
    corner(topX,  topZ,  topY,  +1, +1, Tpp[0], Tpp[1], Tpp[2]);
    auto quad = [&](const float* a, const float* b, const float* c, const float* d,
                    float nx, float ny, float nz, float u, float v) {
        uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{a[0],a[1],a[2]},{nx,ny,nz},{0,0}});
        m.verts.push_back({{b[0],b[1],b[2]},{nx,ny,nz},{u,0}});
        m.verts.push_back({{c[0],c[1],c[2]},{nx,ny,nz},{u,v}});
        m.verts.push_back({{d[0],d[1],d[2]},{nx,ny,nz},{0,v}});
        m.index.insert(m.index.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    const float hgt = topY - baseY;
    const float su = halfW*2*uvScale, sv = hgt*uvScale, swT = halfT*2*uvScale;
    // OUTWARD (+radial) face: Bpm,Bpp,Tpp,Tpm. Omitted when hollow.
    if (!hollow) quad(Bpm, Bpp, Tpp, Tpm,  rox,0,roz,  su, sv);
    // INWARD (-radial) face: Bmp,Bmm,Tmm,Tmp.
    quad(Bmp, Bmm, Tmm, Tmp,  -rox,0,-roz,  su, sv);
    // +tangential face: Bpp,Bmp,Tmp,Tpp.
    quad(Bpp, Bmp, Tmp, Tpp,  tx,0,tz,  swT, sv);
    // -tangential face: Bmm,Bpm,Tpm,Tmm.
    quad(Bmm, Bpm, Tpm, Tmm,  -tx,0,-tz,  swT, sv);
    // TOP cap (+Y): Tmm,Tpm,Tpp,Tmp.
    quad(Tmm, Tpm, Tpp, Tmp,  0,1,0,  swT, su);
    // BOTTOM cap (-Y): Bmm,Bmp,Bpp,Bpm.
    quad(Bmm, Bmp, Bpp, Bpm,  0,-1,0,  swT, su);
    // Collision: reuse render positions + indices (a closed-enough hull; the hollow
    // case drops only the outward wall so the doorway/interior is reachable).
    m.cverts.reserve(m.verts.size() * 3);
    for (const auto& vtx : m.verts) {
        m.cverts.push_back(vtx.pos[0]); m.cverts.push_back(vtx.pos[1]); m.cverts.push_back(vtx.pos[2]);
    }
    m.cindex = m.index;
    return m;
}

// A UNIT UV-SPHERE (radius 1, centered at origin) for procedural planet bodies.
// Authored in OBJECT space so pos == normal == the object-space direction the
// planet triplanar samplers need; UV is a standard lat-long parameterisation
// (u = longitude [0,1], v = latitude pole->pole [0,1]). Winding is CCW front-face
// to match the device's VK_FRONT_FACE_COUNTER_CLOCKWISE. Render geometry only
// (a sky-hung planet needs no collision); cverts/cindex are left empty.
inline PrimMesh makeUVSphere(uint32_t stacks = 64, uint32_t slices = 128) {
    PrimMesh m;
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v   = (float)i / (float)stacks;        // 0..1 pole->pole
        float phi = v * 3.14159265f;                 // latitude
        for (uint32_t j = 0; j <= slices; ++j) {
            float u  = (float)j / (float)slices;     // 0..1 longitude
            float th = u * 6.2831853f;
            float x = sinf(phi) * cosf(th);
            float y = cosf(phi);
            float z = sinf(phi) * sinf(th);
            m.verts.push_back({{x, y, z}, {x, y, z}, {u, v}}); // unit sphere: pos==normal
        }
    }
    const uint32_t cols = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t a = i * cols + j, b = a + cols;
            // OUTWARD-facing winding for VK_FRONT_FACE_COUNTER_CLOCKWISE: with this
            // lat-long vertex order (a = top-left, a+1 = top-right, b = bottom-left,
            // b+1 = bottom-right) the CCW-from-outside order is a, a+1, b / a+1, b+1, b.
            m.index.insert(m.index.end(), { a, a + 1, b, a + 1, b + 1, b });
        }
    return m;
}

// A flat ANNULUS (planetary ring disc) on the XZ plane, authored in OBJECT space
// centered at the origin with normal +Y. `innerR`/`outerR` are OBJECT-space radii;
// `segments` controls the angular tessellation (one quad ring of `segments` cells).
//
// IMPORTANT — matches planet_ring.frag's object-space math: that frag derives the
// radial strip lookup from `length(vObjPos)` against HARDCODED radii (inner = 1.3,
// outer = 2.5) and treats the planet surface radius as 1.0. planet.vert passes the
// object-space position straight through as vObjPos, so this mesh must be authored
// with object-space radii in that SAME range (pass innerR ~1.3, outerR ~2.5). The
// model matrix then translates/tilts/scales the whole ring uniformly in the world
// (a uniform scale keeps length(vObjPos) proportional, so the strip still maps).
// UVs: u = radial t (0 at inner edge, 1 at outer), v = angle [0,1] (cosmetic — the
// ring frag uses object-space radius, not UV). Double-sided is handled by the
// pipeline (cull NONE); winding here is CCW from +Y. Render geometry only.
inline PrimMesh makeRing(float innerR, float outerR, uint32_t segments = 128) {
    PrimMesh m;
    segments = std::max(8u, segments);
    const float kTwoPi = 6.2831853f;
    for (uint32_t j = 0; j <= segments; ++j) {
        float u  = (float)j / (float)segments;     // angle param [0,1]
        float th = u * kTwoPi;
        float cx = cosf(th), sz = sinf(th);
        // Inner ring vertex (radial t = 0), then outer (radial t = 1).
        m.verts.push_back({{ innerR * cx, 0.0f, innerR * sz }, { 0.0f, 1.0f, 0.0f }, { 0.0f, u }});
        m.verts.push_back({{ outerR * cx, 0.0f, outerR * sz }, { 0.0f, 1.0f, 0.0f }, { 1.0f, u }});
    }
    for (uint32_t j = 0; j < segments; ++j) {
        uint32_t a = j * 2;          // inner @ j
        uint32_t b = a + 1;          // outer @ j
        uint32_t c = a + 2;          // inner @ j+1
        uint32_t d = a + 3;          // outer @ j+1
        // CCW from +Y: (inner_j, inner_j1, outer_j) / (inner_j1, outer_j1, outer_j).
        m.index.insert(m.index.end(), { a, c, b, c, d, b });
    }
    return m;
}

// A ROUNDED-RECTANGLE TUBE: a closed pipe with a CIRCULAR cross-section of radius
// `tubeR` swept along a rounded-rectangle PATH in the XY plane (z = 0 centerline),
// so it reads as a real round-section trim/frame around a panel — NOT a flat ribbon
// with a square cross-section. The path runs CCW around a rounded rect whose corner
// centers sit at (+/-(hw-corner), +/-(hh-corner)); the swept circle gives the round
// profile. `pathSeg` = arc segments per rounded corner of the path; `tubeSeg` = ring
// segments around the circular cross-section. Authored in OBJECT space centered at
// origin, normals point radially outward from the path centerline (so it lights like
// a real polished pipe). Winding is CCW front-face for VK. Render geometry only.
inline PrimMesh makeRoundedRectTube(float hw, float hh, float corner, float tubeR,
                                    uint32_t pathSeg = 6, uint32_t tubeSeg = 12) {
    PrimMesh m;
    pathSeg = std::max(1u, pathSeg);
    tubeSeg = std::max(3u, tubeSeg);
    const float kPi = 3.14159265f, kHalfPi = kPi * 0.5f, kTwoPi = 6.2831853f;
    const float r = std::min(corner, std::min(hw, hh) * 0.95f);

    // ---- 1) Build the CENTERLINE path (CCW) + its in-plane tangent at each point.
    // Four straight edges joined by four quarter-arc corners. Each path point stores
    // its position (px,py) and the unit tangent (tx,ty) so we can frame a circle
    // perpendicular to the path there.
    struct PP { float px, py, tx, ty; };
    std::vector<PP> path;
    auto addArc = [&](float cx, float cy, float a0) {
        // Quarter arc CCW from a0 to a0+pi/2 around (cx,cy) at radius r. Skip the
        // first sample on all but the first corner so we don't duplicate the joint.
        for (uint32_t s = 0; s <= pathSeg; ++s) {
            const float a = a0 + kHalfPi * ((float)s / (float)pathSeg);
            const float ca = std::cos(a), sa = std::sin(a);
            // tangent of a CCW circle = (-sin, cos)
            path.push_back({ cx + ca * r, cy + sa * r, -sa, ca });
        }
    };
    // Corner centers + arc start angles, ordered so the path is continuous CCW:
    // bottom-right corner (arc -90deg->0), top-right (0->90), top-left (90->180),
    // bottom-left (180->270). Straight edges fall naturally between consecutive arcs.
    addArc(  hw - r, -(hh - r), -kHalfPi );  // bottom-right
    addArc(  hw - r,   hh - r,   0.0f    );  // top-right
    addArc(-(hw - r),  hh - r,   kHalfPi );  // top-left
    addArc(-(hw - r), -(hh - r), kPi     );  // bottom-left

    const uint32_t P = (uint32_t)path.size();   // closed loop: vertex P wraps to 0

    // ---- 2) Sweep a circle of radius tubeR around each path point. The circle lies
    // in the plane spanned by the path NORMAL (in-XY, perpendicular to tangent) and
    // the Z axis (out of the panel), so the tube bulges both sideways and in depth.
    for (uint32_t i = 0; i < P; ++i) {
        const PP& p = path[i];
        // In-plane normal (perpendicular to tangent, pointing outward from the rect).
        const float nx = p.ty, ny = -p.tx;       // rotate tangent -90deg -> outward
        for (uint32_t j = 0; j <= tubeSeg; ++j) {
            const float a = kTwoPi * ((float)j / (float)tubeSeg);
            const float ca = std::cos(a), sa = std::sin(a);
            // Offset = cos along the in-plane outward normal, sin along world Z.
            const float ox = nx * (ca * tubeR);
            const float oy = ny * (ca * tubeR);
            const float oz = sa * tubeR;
            const float vx = p.px + ox, vy = p.py + oy, vz = oz;
            // Normal = radial direction from the centerline to the surface point.
            const float len = std::sqrt(ox*ox + oy*oy + oz*oz);
            const float inv = (len > 1e-6f) ? 1.0f / len : 0.0f;
            const float u = (float)i / (float)P;
            const float v = (float)j / (float)tubeSeg;
            m.verts.push_back({ {vx, vy, vz}, {ox*inv, oy*inv, oz*inv}, {u, v} });
        }
    }
    // ---- 3) Index the quad strips between consecutive cross-sections (wrapping the
    // last ring back to the first so the tube is a closed loop).
    const uint32_t ring = tubeSeg + 1;            // verts per cross-section
    for (uint32_t i = 0; i < P; ++i) {
        const uint32_t i0 = i * ring;
        const uint32_t i1 = ((i + 1) % P) * ring;
        for (uint32_t j = 0; j < tubeSeg; ++j) {
            const uint32_t a = i0 + j, b = i0 + j + 1;
            const uint32_t c = i1 + j, d = i1 + j + 1;
            // CCW from outside: (a, c, b) / (b, c, d).
            m.index.insert(m.index.end(), { a, c, b, b, c, d });
        }
    }
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

// CLEAN WHITE PANEL — a SMOOTH, near-WHITE / light-grey architectural surface that
// matches the sleek Unity ShowRoom interior (smooth powder-coated panels, NOT a
// busy sci-fi grid). Deliberately FLAT: a uniform light face with at most a
// WHISPER-FINE, low-contrast seam line at the panel pitch (a 1-px hairline only a
// hair darker than the face — no heavy grout, no bevels, no bolts, no grid read),
// plus a barely-there low-frequency shading so it isn't a dead flat fill. Use this
// for additive architectural cladding (floors/ring/struts/stairs/atrium/parapet)
// that must sit flush with the imported white GLB walls. `tint3` multiplies the
// base (pass detail::kNoTint to keep the default off-white). `panels` sets how many
// panel divisions span the tile (the seam pitch); the seam is intentionally faint.
inline std::vector<uint8_t> makeCleanPanelRGBA(uint32_t n, uint32_t panels = 4,
                                               const float tint3[3] = detail::kNoTint,
                                               bool seams = true) {
    using namespace detail;
    std::vector<uint8_t> px((size_t)n * n * 4);
    panels = std::max(1u, panels);
    const uint32_t pitch = std::max(1u, n / panels);          // panel pitch (px)
    // Smooth near-white powder-coat face (light grey-white, faint cool cast).
    const int faceR = 226, faceG = 228, faceB = 233;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            // Barely-there low-frequency shading (very large-scale, tiny amplitude) so
            // the panel reads as a real surface, not a dead flat fill. +-3 levels only.
            const float nlo = (hash01(x / 48, y / 48, std::max(1u, n / 48), 11u) - 0.5f);
            const int shade = (int)(nlo * 6.0f);
            int r = faceR, g = faceG, b = faceB;
            if (seams) {
                // Whisper-fine seam: a single 1-px hairline at each panel edge, only a
                // hair (~12 levels) darker than the face -> low contrast, no grout read.
                const uint32_t lx = x % pitch, ly = y % pitch;
                const bool seamX = (lx == 0) || (lx == pitch - 1);
                const bool seamY = (ly == 0) || (ly == pitch - 1);
                if (seamX || seamY) { r -= 12; g -= 12; b -= 13; }
            }
            putTinted(p, r, g, b, tint3, shade);
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

// ============================================================================
// LEVEL ARCHITECT — blockout (greybox) grid material + origin-centered brushes.
// ============================================================================

// BLOCKOUT GRID — a CLEAN, TASTEFUL UE5-style prototyping grid (NOT the garish
// high-contrast grout grid that was removed from the showroom). A light warm-grey
// base with VERY faint 1 m MINOR lines and a slightly stronger (but still subtle)
// 5 m MAJOR line, so the surface reads as a quiet scale reference rather than a
// busy checker. Authored so ONE texel-cell == ONE world metre when the brush UVs
// use uvScale = 1.0 (makeBox emits hx*2 u-tiles per face, i.e. 1 tile == 1 m).
//
// SEAMLESS: the texture spans EXACTLY `meters` world metres (default 10) at a whole
// pixel pitch (n / meters) and the major lines sit at metre 0 and metre 5 — both
// INSIDE the 10 m span — so when the UVs wrap every 10 m the major lines tile with
// no seam (the metre-0 line is shared by the wrap). Pick n a multiple of `meters`
// (1000 / 10 = 100 px per metre) so every metre boundary lands on an exact pixel.
//
// Created ONCE per editor session via createTexture(px, n, n, /*srgb*/true) and
// shared by every brush. Low-contrast on purpose: lines are only a few levels off
// the base so the greybox never fights the geometry for attention.
inline std::vector<uint8_t> makeBlockoutGridRGBA(uint32_t n = 1000, uint32_t meters = 10) {
    meters = std::max(1u, meters);
    const uint32_t pitch = std::max(1u, n / meters);   // px per world metre
    // Light warm-grey base (a hair warm so it doesn't read clinical blue).
    const int baseR = 176, baseG = 174, baseB = 170;
    // Line widths in px (thin; scaled off the metre pitch so big tiles stay crisp).
    const uint32_t minorW = std::max(1u, pitch / 64);
    const uint32_t majorW = std::max(2u, pitch / 28);
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            int r = baseR, g = baseG, b = baseB;
            // Distance (in px) to the NEAREST metre boundary along each axis, wrapping
            // at the metre pitch so the test is seamless across the tile edge.
            const uint32_t lx = x % pitch, ly = y % pitch;
            const uint32_t dx = std::min(lx, pitch - lx);
            const uint32_t dy = std::min(ly, pitch - ly);
            const uint32_t edge = std::min(dx, dy);
            // Which metre index this pixel sits in (0..meters-1) on each axis — used to
            // promote the line to a MAJOR line at every 5th metre boundary.
            const uint32_t mxIdx = (x / pitch) % meters;
            const uint32_t myIdx = (y / pitch) % meters;
            const bool majorX = (dx <= majorW) && (mxIdx % 5u == 0u);
            const bool majorY = (dy <= majorW) && (myIdx % 5u == 0u);
            if (majorX || majorY) {                 // stronger 5 m line (still subtle)
                r -= 38; g -= 38; b -= 36;
            } else if (edge <= minorW) {             // faint 1 m line
                r -= 16; g -= 16; b -= 15;
            }
            p[0] = (uint8_t)std::max(0, r);
            p[1] = (uint8_t)std::max(0, g);
            p[2] = (uint8_t)std::max(0, b);
            p[3] = 255;
        }
    }
    return px;
}

// Blockout brush primitive types (P2 CORE = Box + Ramp; Cylinder/Stairs DEFERRED).
enum class BrushType : uint32_t { Box = 0, Ramp = 1 };

// Build an ORIGIN-CENTERED brush mesh of `type` with full extents `size` (x,y,z
// in metres). Origin-centered (NOT world-baked like makeBox's cx/cy/cz) so the
// brush's position + yaw live in its TRANSFORM — move/resize is a transform/body
// update, never a per-move mesh regen. Produces render + collision geometry.
//   Box  : a box centered at origin, half-extents = size*0.5. UVs at uvScale 1.0
//          so one grid texel-cell == one world metre.
//   Ramp : a wedge whose footprint is size.x (width) by size.z (run) and whose top
//          rises by size.y over that run, RE-CENTERED to the origin (makeRamp builds
//          from a low corner, so we shift it back by half the run / half the rise so
//          the brush pivot is its centroid-ish origin, matching Box).
inline PrimMesh buildBrushMesh(BrushType type, const float size[3]) {
    const float hx = std::max(0.05f, size[0]) * 0.5f;
    const float hy = std::max(0.05f, size[1]) * 0.5f;
    const float hz = std::max(0.05f, size[2]) * 0.5f;
    if (type == BrushType::Ramp) {
        // makeRamp builds a wedge from a LOW corner (floor at cy, low edge at cz,
        // climbing toward +Z). Recenter so the wedge's bounding box is centered on
        // the origin: shift -run/2 in Z and -rise/2 in Y, width already spans +-halfW.
        const float run  = std::max(0.05f, size[2]);
        const float rise = std::max(0.05f, size[1]);
        const float halfW = hx;
        PrimMesh m = makeRamp(/*cx*/0.0f, /*cy*/ -rise * 0.5f, /*cz*/ -run * 0.5f,
                              halfW, run, rise, /*axis*/1u, /*dir*/ +1.0f, /*uvScale*/1.0f);
        return m;
    }
    // Box (default).
    return makeBox(hx, hy, hz, 0.0f, 0.0f, 0.0f, /*uvScale*/1.0f);
}

} // namespace x3::prims
