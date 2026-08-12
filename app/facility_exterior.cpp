// FACILITY EXTERIOR implementation. See app/facility_exterior.h.
//
// Clean-room: this is the surface-start facility skin (host_surface_start.cpp
// W3-3/W6-2/W8-2) factored into a footprint-parameterized builder. Every
// constant is the surface world's shipping value; the surface host passes its
// original numbers so --world surface renders the same facility it always
// did, and --world canonlevel passes the REAL tower's padded footprint.
#include "facility_exterior.h"
#include "mesh_prims.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace x3::game {

namespace {

constexpr float kPi     = 3.14159265f;
constexpr float kWallT  = 0.4f;   // backing-wall thickness (half extent, surface value)
constexpr float kStoreyH = 4.0f;  // one storey (the tower spec)
constexpr float kBandH   = 1.8f;  // concrete spandrel band height per storey
constexpr int   kMergeGroups = 6; // batched-pane material groups (mergePanes)

double monoMs() {
    using C = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(C::now().time_since_epoch()).count();
}

// Deterministic integer-hash micro-variation (the surface world's exact hash).
float h01(uint32_t s) {
    s ^= s >> 16; s *= 0x7feb352du; s ^= s >> 15; s *= 0x846ca68bu; s ^= s >> 16;
    return (float)(s & 0xffffffu) / 16777216.0f;
}

// One face of the axis-aligned footprint: outward normal, wall plane coord,
// the run axis (u) span, and the yaw constants the surface code used for it.
struct FaceDef {
    FacilityExterior::Face face;
    float plane;      // wall plane coordinate (x for X faces, z for Z faces)
    float outSign;    // outward direction along the plane axis (+1 / -1)
    bool  planeIsX;   // true: wall plane is X=const (run axis = Z)
    float uMid;       // face-run center (x-mid for Z faces, z-mid for X faces)
    float uLen;       // face-run FULL length (wall span, no overhang)
    float bandYaw;    // surface_library panel yaw (panels face -Z at yaw 0)
    float paneYaw;    // glass pane yaw (pane local +Z = outward)
};

} // namespace

void FacilityExterior::applyGoldenHourSky(x3::rhi::IRenderDevice& device) {
    // GOLDEN HOUR (ART_BIBLE §3 surface zone — the sky is the accent). The
    // surface world's exact parameters: low warm sun raking the tower face,
    // warm horizon band, cooling zenith (W6-2: whiteness lives in the SKY,
    // not baked into every lit surface).
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    sp.sunDir[0] = 0.55f; sp.sunDir[1] = 0.16f; sp.sunDir[2] = -0.35f;
    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.87f; sp.sunColor[2] = 0.72f;
    sp.sunIntensity = 1.25f; sp.haze = 0.45f; sp.exposure = 1.0f;
    sp.zenith[0]  = 0.10f; sp.zenith[1]  = 0.15f; sp.zenith[2]  = 0.26f;
    sp.horizon[0] = 0.55f; sp.horizon[1] = 0.34f; sp.horizon[2] = 0.20f;
    device.setSkyParams(sp);
}

x3::rhi::PointLight FacilityExterior::spillLight() const {
    // W6-2 breach light spill — the warm interior glow leaking out of the
    // entry (the surface world's exact values, seated at THIS breach).
    x3::rhi::PointLight pl{};
    const bool zFace = (m_desc.breachFace == Face::PlusZ || m_desc.breachFace == Face::MinusZ);
    const float outS = (m_desc.breachFace == Face::PlusZ || m_desc.breachFace == Face::PlusX) ? 1.0f : -1.0f;
    const float plane = (m_desc.breachFace == Face::PlusZ) ? m_desc.z1
                      : (m_desc.breachFace == Face::MinusZ) ? m_desc.z0
                      : (m_desc.breachFace == Face::PlusX) ? m_desc.x1 : m_desc.x0;
    if (zFace) { pl.pos[0] = m_desc.breachCenter; pl.pos[2] = plane + outS * 1.5f; }
    else       { pl.pos[2] = m_desc.breachCenter; pl.pos[0] = plane + outS * 1.5f; }
    pl.pos[1] = m_desc.baseY + 2.2f;
    pl.range = 14.0f;
    pl.color[0] = 3.2f; pl.color[1] = 2.2f; pl.color[2] = 1.2f;   // amber
    return pl;
}

void FacilityExterior::ensureOutdoorVis(const CanonFloor& floor, float x, float y, float z,
                                        std::vector<uint32_t>& visRooms) const {
    if (!m_built || m_desc.breachRoomHint == kNoRoom) return;
    if (floor.roomAt(x, y, z) != kNoRoom) return;   // indoors: the flood is authoritative
    for (uint32_t v : visRooms)
        if (v == m_desc.breachRoomHint) return;
    visRooms.push_back(m_desc.breachRoomHint);
}

void FacilityExterior::build(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics, const Desc& d,
                             SurfaceLibrary* sharedLib) {
    const double t0 = monoMs();
    m_desc = d;
    m_lib = sharedLib ? sharedLib : &m_surflib;
    if (!m_lib->mounted()) m_lib->mount(x3::game::assetRoot() + "/surface_library");
    m_sTower = &m_lib->get(device, d.towerSet);
    m_sApron = &m_lib->get(device, d.apronSet);

    const float cxMid = (d.x0 + d.x1) * 0.5f;
    const float czMid = (d.z0 + d.z1) * 0.5f;
    const float towerH = d.topY - d.baseY;
    const int   storeys = std::max(1, (int)std::lround(towerH / kStoreyH));

    // ---- Face table. Order = [breach, opposite, minor -side, minor +side] so
    // the band/pane emission order (and the pane jitter seed sequence) matches
    // the surface host exactly when the breach sits on the +Z face.
    auto makeFace = [&](Face f) -> FaceDef {
        FaceDef fd{};
        fd.face = f;
        switch (f) {
            case Face::PlusZ:  fd.plane = d.z1; fd.outSign = +1; fd.planeIsX = false;
                               fd.uMid = cxMid; fd.uLen = d.x1 - d.x0;
                               fd.bandYaw = kPi;        fd.paneYaw = 0.0f;       break;
            case Face::MinusZ: fd.plane = d.z0; fd.outSign = -1; fd.planeIsX = false;
                               fd.uMid = cxMid; fd.uLen = d.x1 - d.x0;
                               fd.bandYaw = 0.0f;       fd.paneYaw = kPi;        break;
            case Face::MinusX: fd.plane = d.x0; fd.outSign = -1; fd.planeIsX = true;
                               fd.uMid = czMid; fd.uLen = d.z1 - d.z0;
                               fd.bandYaw = kPi * 0.5f; fd.paneYaw = -kPi * 0.5f; break;
            default:           fd.plane = d.x1; fd.outSign = +1; fd.planeIsX = true;
                               fd.uMid = czMid; fd.uLen = d.z1 - d.z0;
                               fd.bandYaw = -kPi * 0.5f; fd.paneYaw = kPi * 0.5f; break;
        }
        return fd;
    };
    Face oppositeFace = Face::MinusZ, minorA = Face::MinusX, minorB = Face::PlusX;
    switch (d.breachFace) {
        case Face::PlusZ:  oppositeFace = Face::MinusZ; minorA = Face::MinusX; minorB = Face::PlusX; break;
        case Face::MinusZ: oppositeFace = Face::PlusZ;  minorA = Face::MinusX; minorB = Face::PlusX; break;
        case Face::MinusX: oppositeFace = Face::PlusX;  minorA = Face::MinusZ; minorB = Face::PlusZ; break;
        default:           oppositeFace = Face::MinusX; minorA = Face::MinusZ; minorB = Face::PlusZ; break;
    }
    const FaceDef fFront = makeFace(d.breachFace);
    const FaceDef fBack  = makeFace(oppositeFace);
    const FaceDef fMinA  = makeFace(minorA);
    const FaceDef fMinB  = makeFace(minorB);

    // Face-local (u, y, out) -> world position.
    auto facePos = [](const FaceDef& f, float u, float planeC, float& x, float& z) {
        if (f.planeIsX) { x = planeC; z = u; } else { x = u; z = planeC; }
    };

    // ---- Scene-entity helpers (the surface host's glassWall / opaqueSlab). --
    auto backingWall = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
        auto mh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                    m.index.data(), (uint32_t)m.index.size());
        // Near-black backing behind the glass curtain: reads as the mullion
        // grid in the pane gaps + the dark transmission behind each pane. It
        // carries the collision (W8-2, host_surface_start).
        auto td = x3::prims::makeSolidRGBA(8, 14, 16, 20);
        auto tx = device.createTexture(td.data(), 8, 8, true);
        Entity e{}; e.mesh = mh; e.tex = tx;
        e.transparent = false;
        e.baseColor[0] = 0.30f; e.baseColor[1] = 0.34f; e.baseColor[2] = 0.42f;
        e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Static;      // roomId stays kNoRoom => always drawn
        scene.add(e);
        physics.addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                       0.0f, x3::phys::Layer::Static);
    };
    auto opaqueSlab = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          uint8_t r, uint8_t g, uint8_t b) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 2.0f);
        auto mh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                    m.index.data(), (uint32_t)m.index.size());
        auto td = x3::prims::makeSolidRGBA(8, r, g, b);
        auto tx = device.createTexture(td.data(), 8, 8, true);
        Entity e{}; e.mesh = mh; e.tex = tx;
        e.tag = (uint32_t)Tag::Static;
        scene.add(e);
        physics.addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                       0.0f, x3::phys::Layer::Static);
    };

    // ---- Terrain plate (surface world: 300 m dark soil + collision floor). --
    if (d.terrainPlate) {
        x3::prims::PrimMesh g = x3::prims::makeBox(300.0f, 0.5f, 300.0f,
                                                   0.0f, d.baseY - 0.5f, 0.0f, 8.0f);
        auto gm = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                    g.index.data(), (uint32_t)g.index.size());
        auto gtD = x3::prims::makeCheckerRGBA(64, 16, 46, 42, 36, 38, 35, 30);
        auto gt = device.createTexture(gtD.data(), 64, 64, true);
        Entity e{}; e.mesh = gm; e.tex = gt;
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 1.0f;
        e.tag = (uint32_t)Tag::Static;
        scene.add(e);
        physics.addBox(x3::phys::Vec3{300.0f, 0.5f, 300.0f},
                       x3::phys::Vec3{0.0f, d.baseY - 0.5f, 0.0f},
                       0.0f, x3::phys::Layer::Static);
    }

    // ---- Apron. -------------------------------------------------------------
    if (d.apron == Apron::SurfacePanel) {
        // The surface world's single big panel, 2 cm proud of the soil.
        ApronDraw a{};
        a.mesh = m_lib->makePanel(device, /*floor*/1, d.apronPanelW, d.apronPanelD, 3.0f);
        const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                              d.apronAnchorX, d.baseY + d.apronAnchorY, d.apronAnchorZ, 1 };
        for (int i = 0; i < 16; ++i) a.xform[i] = m[i];
        m_aprons.push_back(a);
    } else if (d.apron == Apron::Ring) {
        // A concrete RING around the footprint (walkable; collision), then a
        // dark soil SKIRT out to soilOut so the near horizon isn't raw void.
        // Panels only OUTSIDE the footprint — nothing is placed under the
        // building (the canon strata/descent content lives below it).
        const float out = d.apronOut;
        const float yTop = d.baseY;
        auto ringPanel = [&](float ox, float oz, float w, float h) {
            // floor panel spans x in [-w/2, w/2], z in [0, h] from its origin
            ApronDraw a{};
            a.mesh = m_lib->makePanel(device, /*floor*/1, w, h, 3.0f);
            const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, ox, yTop, oz, 1 };
            for (int i = 0; i < 16; ++i) a.xform[i] = m[i];
            m_aprons.push_back(a);
            // Static collision slab, top flush with yTop.
            physics.addBox(x3::phys::Vec3{w * 0.5f, 0.25f, h * 0.5f},
                           x3::phys::Vec3{ox, yTop - 0.25f, oz + h * 0.5f},
                           0.0f, x3::phys::Layer::Static);
        };
        ringPanel(cxMid, d.z1,        (d.x1 - d.x0) + 2.0f * out, out);  // +Z side
        ringPanel(cxMid, d.z0 - out,  (d.x1 - d.x0) + 2.0f * out, out);  // -Z side
        ringPanel(d.x1 + out * 0.5f, d.z0, out, d.z1 - d.z0);            // +X side
        ringPanel(d.x0 - out * 0.5f, d.z0, out, d.z1 - d.z0);            // -X side
        // Soil skirt (scene entities; 2 cm below the apron so its edge reads).
        const float so = d.soilOut;
        auto soil = [&](float cx, float cz, float hx, float hz) {
            if (hx < 0.5f || hz < 0.5f) return;
            x3::prims::PrimMesh g = x3::prims::makeBox(hx, 0.4f, hz,
                                                       cx, yTop - 0.42f, cz, 8.0f);
            auto gm = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                        g.index.data(), (uint32_t)g.index.size());
            auto gtD = x3::prims::makeCheckerRGBA(64, 16, 46, 42, 36, 38, 35, 30);
            auto gt = device.createTexture(gtD.data(), 64, 64, true);
            Entity e{}; e.mesh = gm; e.tex = gt;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            physics.addBox(x3::phys::Vec3{hx, 0.4f, hz}, x3::phys::Vec3{cx, yTop - 0.42f, cz},
                           0.0f, x3::phys::Layer::Static);
        };
        const float ix0 = d.x0 - out, ix1 = d.x1 + out;   // apron outer edges
        const float iz0 = d.z0 - out, iz1 = d.z1 + out;
        soil(cxMid,               iz1 + (so - out) * 0.5f, (ix1 - ix0) * 0.5f + (so - out), (so - out) * 0.5f); // +Z
        soil(cxMid,               iz0 - (so - out) * 0.5f, (ix1 - ix0) * 0.5f + (so - out), (so - out) * 0.5f); // -Z
        soil(ix1 + (so - out) * 0.5f, czMid, (so - out) * 0.5f, (iz1 - iz0) * 0.5f);   // +X
        soil(ix0 - (so - out) * 0.5f, czMid, (so - out) * 0.5f, (iz1 - iz0) * 0.5f);   // -X
    }

    // ---- BACKING WALLS around the footprint, breach split on the front. ----
    const float yMidWall = d.baseY + towerH * 0.5f;
    const float hHalf    = towerH * 0.5f;
    {
        // Breach face: two segments flanking the gap + a lintel above it (the
        // breach stays a doorway-height opening, clear to baseY + 2*breachHalfW*?
        // — surface: clear to 3.2 m, lintel from there to the roof line).
        const float bC = d.breachCenter;
        const float bH = d.breachHalfW;
        const float clearH = 3.2f;                    // == 2 * (kFacilityHalfH+1.6 - kFacilityHalfH) legacy read
        const float uLo = fFront.uMid - fFront.uLen * 0.5f;
        const float uHi = fFront.uMid + fFront.uLen * 0.5f;
        auto frontSeg = [&](float sLo, float sHi) {
            if (sHi - sLo < 0.05f) return;
            float x, z; facePos(fFront, (sLo + sHi) * 0.5f, fFront.plane, x, z);
            if (fFront.planeIsX)
                backingWall(x, yMidWall, z, kWallT, hHalf, (sHi - sLo) * 0.5f);
            else
                backingWall(x, yMidWall, z, (sHi - sLo) * 0.5f, hHalf, kWallT);
        };
        frontSeg(uLo, bC - bH);                       // left of the breach
        frontSeg(bC + bH, uHi);                       // right of the breach
        // Lintel above the breach.
        {
            float x, z; facePos(fFront, bC, fFront.plane, x, z);
            const float lY0 = d.baseY + clearH;
            if (fFront.planeIsX)
                backingWall(x, (lY0 + d.topY) * 0.5f, z, kWallT, (d.topY - lY0) * 0.5f, bH);
            else
                backingWall(x, (lY0 + d.topY) * 0.5f, z, bH, (d.topY - lY0) * 0.5f, kWallT);
        }
        // Back + minor faces: one full wall each.
        auto fullWall = [&](const FaceDef& f) {
            float x, z; facePos(f, f.uMid, f.plane, x, z);
            if (f.planeIsX) backingWall(x, yMidWall, z, kWallT, hHalf, f.uLen * 0.5f);
            else            backingWall(x, yMidWall, z, f.uLen * 0.5f, hHalf, kWallT);
        };
        fullWall(fBack);
        fullWall(fMinA);
        fullWall(fMinB);
    }
    // Roof + (optional) interior floor slab.
    if (d.roofSlab)
        opaqueSlab(cxMid, d.topY + d.roofLift, czMid,
                   (d.x1 - d.x0) * 0.5f, kWallT, (d.z1 - d.z0) * 0.5f, 40, 44, 52);
    if (d.floorSlab)
        opaqueSlab(cxMid, d.baseY + 0.05f, czMid,
                   (d.x1 - d.x0) * 0.5f, 0.1f, (d.z1 - d.z0) * 0.5f, 30, 32, 38);

    // ---- BREACH MARKER: the glowing frame band over the entry. -------------
    {
        float x, z; facePos(fFront, d.breachCenter, fFront.plane, x, z);
        if (fFront.planeIsX)
            opaqueSlab(x, d.baseY + d.breachHalfW + 1.0f, z,
                       kWallT + 0.1f, 0.25f, d.breachHalfW + 0.3f, 90, 200, 255);
        else
            opaqueSlab(x, d.baseY + d.breachHalfW + 1.0f, z,
                       d.breachHalfW + 0.3f, 0.25f, kWallT + 0.1f, 90, 200, 255);
    }

    // ---- BREACH VESTIBULE (canon): floor + side walls across the interstice
    // between the room's cut wall and the facade plane, so the walk out is a
    // continuous deck (LAW 2: no void underfoot at the seam). -----------------
    if (d.vestibuleDepth > 0.05f) {
        const float vd = d.vestibuleDepth;
        const float vh = d.vestibuleHalfW;
        float bx, bz; facePos(fFront, d.breachCenter, fFront.plane, bx, bz);
        const float inS = -fFront.outSign;            // inward
        // Floor strip: from just inside the room wall out through the facade.
        const float fcU = fFront.plane + inS * (vd * 0.5f) + fFront.outSign * 0.25f;
        if (fFront.planeIsX)
            opaqueSlab(fcU, d.baseY - 0.05f, bz, vd * 0.5f + 0.45f, 0.05f, vh + 0.2f, 46, 48, 54);
        else
            opaqueSlab(bx, d.baseY - 0.05f, fcU, vh + 0.2f, 0.05f, vd * 0.5f + 0.45f, 46, 48, 54);
        // Side walls flanking the walk (up to the breach clear height).
        const float wallH = 3.2f;
        auto vWall = [&](float side) {
            const float u = d.breachCenter + side * vh;
            const float mid = fFront.plane + inS * (vd * 0.5f);
            if (fFront.planeIsX)
                backingWall(mid, d.baseY + wallH * 0.5f, u, vd * 0.5f, wallH * 0.5f, 0.1f);
            else
                backingWall(u, d.baseY + wallH * 0.5f, mid, 0.1f, wallH * 0.5f, vd * 0.5f);
        };
        vWall(-1.0f);
        vWall(+1.0f);
    }

    // ---- W3-3: CONCRETE SPANDREL BANDS over the black glass. ---------------
    const float spanA = fFront.uLen + 0.8f;           // breach-face pair span
    const float spanB = fMinA.uLen + 0.8f;            // minor pair span
    x3::rhi::MeshHandle bandA = m_lib->makePanel(device, 0, spanA, kBandH, 2.6f);
    x3::rhi::MeshHandle bandB = m_lib->makePanel(device, 0, spanB, kBandH, 2.6f);
    x3::rhi::MeshHandle jambSeg = m_lib->makePanel(device, 0, 1.0f, 4.2f, 2.6f);
    m_signMesh = m_lib->makePanel(device, 0, 2.0f * d.breachHalfW, 0.7f, 2.6f);

    auto pushBand = [&](x3::rhi::MeshHandle mesh, float yaw, float x, float y, float z) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        BandDraw b{}; b.mesh = mesh;
        float m[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, x,y,z,1 };
        for (int i = 0; i < 16; ++i) b.xform[i] = m[i];
        m_bands.push_back(b);
    };
    auto bandAt = [&](const FaceDef& f, x3::rhi::MeshHandle mesh, float u, float y, float proud) {
        float x, z; facePos(f, u, f.plane + f.outSign * proud, x, z);
        pushBand(mesh, f.bandYaw, x, y, z);
    };
    {
        const float proud = kWallT + 0.05f;           // bands 5 cm proud of the backing
        for (int f = 1; f < storeys; ++f) {           // storey lines
            const float y = d.baseY + f * kStoreyH - kBandH * 0.5f;
            bandAt(fFront, bandA, fFront.uMid, y, proud);
            bandAt(fBack,  bandA, fBack.uMid,  y, proud);
            bandAt(fMinA,  bandB, fMinA.uMid,  y, proud);
            bandAt(fMinB,  bandB, fMinB.uMid,  y, proud);
        }
        // Parapet crown + the W6-2 depth extension row above the roof line.
        for (int row = 0; row < 2; ++row) {
            const float y = d.baseY + towerH - 0.2f + row * (kBandH * 0.9f);
            bandAt(fFront, bandA, fFront.uMid, y, proud);
            bandAt(fBack,  bandA, fBack.uMid,  y, proud);
            bandAt(fMinA,  bandB, fMinA.uMid,  y, proud);
            bandAt(fMinB,  bandB, fMinB.uMid,  y, proud);
        }
        // W6-2 ENTRANCE PRESENCE: vertical jambs flanking the breach.
        bandAt(fFront, jambSeg, d.breachCenter - (d.breachHalfW + 0.55f), d.baseY + 2.1f, proud);
        bandAt(fFront, jambSeg, d.breachCenter + (d.breachHalfW + 0.55f), d.baseY + 2.1f, proud);
        // Ground base: full band on back/minors; split around the breach in front.
        bandAt(fBack, bandA, fBack.uMid, d.baseY, proud);
        bandAt(fMinA, bandB, fMinA.uMid, d.baseY, proud);
        bandAt(fMinB, bandB, fMinB.uMid, d.baseY, proud);
        // Front base segments (asymmetric-safe: from the face-panel edge +0.7
        // in to the jamb line ±0.3 — the surface world's exact margins).
        const float faceLo = fFront.uMid - spanA * 0.5f;
        const float faceHi = fFront.uMid + spanA * 0.5f;
        auto baseSeg = [&](float sLo, float sHi) {
            if (sHi - sLo < 0.4f) return;
            x3::rhi::MeshHandle seg = m_lib->makePanel(device, 0, sHi - sLo, 1.2f, 2.6f);
            bandAt(fFront, seg, (sLo + sHi) * 0.5f, d.baseY, proud);
        };
        baseSeg(faceLo + 0.7f, d.breachCenter - d.breachHalfW - 0.3f);
        baseSeg(d.breachCenter + d.breachHalfW + 0.3f, faceHi - 0.7f);
        // The amber entrance sign strip over the breach.
        {
            float x, z; facePos(fFront, d.breachCenter, fFront.plane + fFront.outSign * (kWallT + 0.08f), x, z);
            const float c = std::cos(fFront.bandYaw), s = std::sin(fFront.bandYaw);
            const float m[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, x, d.baseY + 4.7f, z, 1 };
            for (int i = 0; i < 16; ++i) m_signXform[i] = m[i];
            m_hasSign = true;
        }
    }

    // ---- W8-2: THE GLASS CURTAIN WALL — per-pane translucent glazing with a
    // hashed micro-tilt/tint jitter (the thing that makes the facade read as
    // real glass). Per-pane draws (surface) or merged batches (canon). -------
    // Glazing half-thickness. 6 mm half = 12 mm glass, a real curtain-wall pane;
    // it was 15 mm half (30 mm), which ate most of the clearance to the backing
    // wall and left no room for the micro-tilt below. See kPaneClear.
    constexpr float kPaneHalfT = 0.006f;
    // Minimum air gap the pane's INNER face keeps off the backing-wall face.
    constexpr float kPaneClear = 0.003f;
    x3::prims::PrimMesh gpm = x3::prims::makeBox(0.5f, 0.5f, kPaneHalfT, 0, 0, 0, 1.0f);
    m_glassPanelMesh = device.createMesh(gpm.verts.data(), (uint32_t)gpm.verts.size(),
                                         gpm.index.data(), (uint32_t)gpm.index.size());
    auto glassTexD = x3::prims::makeSolidRGBA(8, 255, 255, 255);
    m_glassPanelTex = device.createTexture(glassTexD.data(), 8, 8, true);

    // Merged-mode accumulation buffers (one mesh per material group).
    std::vector<std::vector<x3::rhi::MeshVertex>> mVerts(d.mergePanes ? kMergeGroups : 0);
    std::vector<std::vector<uint32_t>>            mIndex(d.mergePanes ? kMergeGroups : 0);

    uint32_t paneCount = 0;
    {
        // One pane: face-local center, outward yaw, w x h meters, seeded jitter.
        // M = T * RotY(yaw+dh) * RotX(dp) * S(w,h,1) — the surface world's matrix.
        auto pushPane = [&](float yaw, float px, float py, float pz,
                            float w, float h, uint32_t seed) {
            const float r0 = h01(seed * 3u + 11u), r1 = h01(seed * 3u + 12u),
                        r2 = h01(seed * 3u + 13u);
            const float dh = (r0 - 0.5f) * 0.016f;   // heading tilt, ~±0.46 deg
            const float dp = (r1 - 0.5f) * 0.016f;   // pitch tilt
            const float c = std::cos(yaw + dh), s = std::sin(yaw + dh);
            const float cp = std::cos(dp),      sp = std::sin(dp);
            // ---- THE SHARD FIX --------------------------------------------
            // The micro-tilt rotates the pane about its own centre, so it dips
            // the pane's corners TOWARD the backing wall by up to
            //     halfW*|sin dh| + halfH*|sin dp|
            // which for a 2.5 x 2.1 m pane at ±0.46° is 18.4 mm — far more than
            // the 5 mm of clearance the fixed `proud` offset used to leave. 85%
            // of panes therefore genuinely INTERSECTED the opaque wall, and the
            // wall clipped each one along the plane/plane intersection line — a
            // straight cut whose direction is random per pane. That is the
            // "jagged triangular shards", and being real geometry (not depth
            // precision) it looked identical at 12 m and at 70 m.
            // Push every pane out along its face normal by exactly its own
            // worst-corner dip, so the jitter survives but can never sink in.
            const float dip = 0.5f * w * std::fabs(std::sin(dh))
                            + 0.5f * h * std::fabs(std::sin(dp));
            px += std::sin(yaw) * dip;
            pz += std::cos(yaw) * dip;
            const float m[16] = {
                c * w,       0.0f,     -s * w,      0,
                sp * s * h,  cp * h,    sp * c * h, 0,
                cp * s,     -sp,        cp * c,     0,
                px,          py,        pz,         1 };
            ++paneCount;
            if (!d.mergePanes) {
                GlassPanelDraw p{};
                for (int i = 0; i < 16; ++i) p.xform[i] = m[i];
                p.base[0] = 0.055f; p.base[1] = 0.065f; p.base[2] = 0.080f; p.base[3] = 1.0f;
                p.mat.opacity    = 0.66f + (r1 - 0.5f) * 0.16f;
                p.mat.refraction = 0.004f;
                p.mat.roughness  = 0.035f + r0 * 0.10f;
                p.mat.specular   = 1.0f;
                p.mat.tint[0] = 0.60f + (r2 - 0.5f) * 0.10f;
                p.mat.tint[1] = 0.66f + (r0 - 0.5f) * 0.08f;
                p.mat.tint[2] = 0.70f + (r1 - 0.5f) * 0.10f;
                m_panes.push_back(p);
                return;
            }
            // MERGED: bake the pane's transformed unit box into its group mesh
            // (the tilt survives in the verts; tint/roughness become per-group).
            const int g = (int)(seed % (uint32_t)kMergeGroups);
            auto& vs = mVerts[g]; auto& is = mIndex[g];
            const uint32_t base = (uint32_t)vs.size();
            for (const x3::rhi::MeshVertex& v : gpm.verts) {
                x3::rhi::MeshVertex o = v;
                const float X = v.pos[0], Y = v.pos[1], Z = v.pos[2];
                o.pos[0] = m[0]*X + m[4]*Y + m[8]*Z  + m[12];
                o.pos[1] = m[1]*X + m[5]*Y + m[9]*Z  + m[13];
                o.pos[2] = m[2]*X + m[6]*Y + m[10]*Z + m[14];
                float nx = m[0]*v.normal[0] + m[4]*v.normal[1] + m[8]*v.normal[2];
                float ny = m[1]*v.normal[0] + m[5]*v.normal[1] + m[9]*v.normal[2];
                float nz = m[2]*v.normal[0] + m[6]*v.normal[1] + m[10]*v.normal[2];
                const float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
                o.normal[0] = nx; o.normal[1] = ny; o.normal[2] = nz;
                vs.push_back(o);
            }
            for (uint32_t idx : gpm.index) is.push_back(base + idx);
        };

        // Glass strip per storey: between the base band / storey bands / parapet.
        std::vector<float> yBot(storeys), yTop(storeys);
        for (int f = 0; f < storeys; ++f) {
            yBot[f] = d.baseY + ((f == 0) ? kBandH : f * kStoreyH + kBandH * 0.5f);
            yTop[f] = d.baseY + ((f + 1 < storeys) ? (f + 1) * kStoreyH - kBandH * 0.5f
                                                   : towerH - 0.2f);
        }
        const float kGap = 0.14f;                     // mullion gap
        const int colsA = std::max(1, (int)std::lround(spanA / 2.64f));   // 20 on the surface tower
        const int colsB = std::max(1, (int)std::lround(spanB / 2.64f));   // 12 on the surface tower
        // Pane CENTRE offset for an untilted pane: its inner face then sits
        // exactly kPaneClear off the backing wall. pushPane() adds each pane's
        // own tilt dip on top, so the worst corner still keeps that air gap.
        // Deepest possible pane front face = kWallT + 2*kPaneHalfT + kPaneClear
        // + maxDip = 0.433, still 17 mm behind the spandrel band quads at 0.45.
        const float proud = kWallT + kPaneHalfT + kPaneClear;
        uint32_t seed = 1u;
        for (int f = 0; f < storeys; ++f) {
            const float yc = (yBot[f] + yTop[f]) * 0.5f;
            const float ph = (yTop[f] - yBot[f]) - 0.10f;
            if (ph <= 0.2f) continue;
            const float pitchA = spanA / colsA, pwA = pitchA - kGap;
            for (int i = 0; i < colsA; ++i) {
                const float u = fFront.uMid - spanA * 0.5f + (i + 0.5f) * pitchA;
                // Breach face strip 0: leave the breach + jamb zone unglazed.
                const bool inBreachZone = (f == 0) &&
                    (std::fabs(u - d.breachCenter) - pwA * 0.5f) < (d.breachHalfW + 1.3f);
                if (!inBreachZone) {
                    float x, z; facePos(fFront, u, fFront.plane + fFront.outSign * proud, x, z);
                    pushPane(fFront.paneYaw, x, yc, z, pwA, ph, seed++);
                } else seed++;
                float bx2, bz2; facePos(fBack, u, fBack.plane + fBack.outSign * proud, bx2, bz2);
                pushPane(fBack.paneYaw, bx2, yc, bz2, pwA, ph, seed++);
            }
            const float pitchB = spanB / colsB, pwB = pitchB - kGap;
            for (int i = 0; i < colsB; ++i) {
                const float u = fMinA.uMid - spanB * 0.5f + (i + 0.5f) * pitchB;
                float ax, az; facePos(fMinA, u, fMinA.plane + fMinA.outSign * proud, ax, az);
                pushPane(fMinA.paneYaw, ax, yc, az, pwB, ph, seed++);
                float bx2, bz2; facePos(fMinB, u, fMinB.plane + fMinB.outSign * proud, bx2, bz2);
                pushPane(fMinB.paneYaw, bx2, yc, bz2, pwB, ph, seed++);
            }
        }
    }
    if (d.mergePanes) {
        for (int g = 0; g < kMergeGroups; ++g) {
            if (mVerts[g].empty()) continue;
            MergedGlass mg{};
            mg.mesh = device.createMesh(mVerts[g].data(), (uint32_t)mVerts[g].size(),
                                        mIndex[g].data(), (uint32_t)mIndex[g].size());
            const float r0 = h01((uint32_t)g * 3u + 11u), r1 = h01((uint32_t)g * 3u + 12u),
                        r2 = h01((uint32_t)g * 3u + 13u);
            mg.base[0] = 0.055f; mg.base[1] = 0.065f; mg.base[2] = 0.080f; mg.base[3] = 1.0f;
            mg.mat.opacity    = 0.66f + (r1 - 0.5f) * 0.16f;
            mg.mat.refraction = 0.004f;
            mg.mat.roughness  = 0.035f + r0 * 0.10f;
            mg.mat.specular   = 1.0f;
            mg.mat.tint[0] = 0.60f + (r2 - 0.5f) * 0.10f;
            mg.mat.tint[1] = 0.66f + (r0 - 0.5f) * 0.08f;
            mg.mat.tint[2] = 0.70f + (r1 - 0.5f) * 0.10f;
            m_merged.push_back(mg);
        }
    }
    m_paneCount = paneCount;
    m_built = true;

    const double cost = monoMs() - t0;
    char lb[256];
    std::snprintf(lb, sizeof(lb),
        "facility exterior: footprint x[%.1f..%.1f] z[%.1f..%.1f] y[%.1f..%.1f] — "
        "%d storeys, %u panes (%s), %u bands | build %.1f ms",
        d.x0, d.x1, d.z0, d.z1, d.baseY, d.topY, storeys, paneCount,
        d.mergePanes ? (std::to_string(m_merged.size()) + " merged draws").c_str()
                     : "per-pane draws",
        (uint32_t)m_bands.size(), cost);
    x3::logInfo(lb);
}

void FacilityExterior::draw(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame) const {
    if (!m_built) return;
    // Glass curtain wall (post-opaque glass pass; the near-black backing wall
    // behind supplies the dark transmission + the mullion grid in the gaps).
    for (const auto& p : m_panes)
        device.drawMeshGlass(frame, m_glassPanelMesh, m_glassPanelTex,
                             p.base, nullptr, p.mat, p.xform);
    static const float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    for (const auto& mg : m_merged)
        device.drawMeshGlass(frame, mg.mesh, m_glassPanelTex,
                             mg.base, nullptr, mg.mat, kIdent);
    // Textured skin: the concrete apron underfoot.
    if (m_sApron && m_sApron->ok)
        for (const auto& a : m_aprons)
            m_lib->drawPanel(device, frame, *m_sApron, a.mesh, a.xform);
    // Spandrel bands: cc_cement_white is genuinely white at the texture level,
    // so the factor stays near-neutral (whiteness from albedo, not the clamp).
    if (m_sTower && m_sTower->ok) {
        const float bcW[4]   = { 1.06f, 1.05f, 1.04f, 1.0f };
        const float emis0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (const auto& b : m_bands)
            device.drawMeshPBR(frame, b.mesh, m_sTower->albedo, m_sTower->normal, m_sTower->mr,
                               bcW, emis0, b.xform, false, false,
                               x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                               1.0f, 0.0f, 0.0f);
        // W6-2 ENTRANCE SIGN — the one emissive read on the facade (an amber-
        // lit strip over the breach; emissives are instruments: it marks the way in).
        if (m_hasSign && m_signMesh.valid()) {
            const float bcDark[4]  = { 0.22f, 0.20f, 0.18f, 1.0f };
            const float emAmber[4] = { 2.0f, 1.25f, 0.35f, 1.4f };
            device.drawMeshPBR(frame, m_signMesh, m_sTower->albedo, m_sTower->normal, m_sTower->mr,
                               bcDark, emAmber, m_signXform, false, false,
                               x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                               1.0f, 0.0f, 0.0f);
        }
    }
}

} // namespace x3::game
