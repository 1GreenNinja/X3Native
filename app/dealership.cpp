// DEALERSHIP — buy new cars; cars spinning slowly under great lighting.
// See dealership.h for the design. Clean-room; built only through the public
// engine interfaces (IRenderDevice / IPhysicsWorld / IModelLoader).

#include "dealership.h"
#include "mesh_prims.h"
#include "vehicle.h"        // makeUnitCylinderY (graybox wheels)
#include "showroom_tod.h"   // applyShowroomTimeOfDay (the screenshot rig)
#include "terrain.h"        // terrainHeightAtWorld / worldUnderRiverContains (self-test siting)
#include "asset_root.h"     // convertedGlbRoot (screenshots)

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

// Four trims of the ONE hero GLB the assets carry (Vehicles/CTR.glb). Distinct
// names / prices / paints / spin rates; the GLB is shared and repainted.
const DealershipCarDef kTrims[kDealershipDiscs] = {
    { "ctr_street",  "CTR STREET",      9500, { 0.92f, 0.93f, 0.95f }, 5.0f, "Vehicles/CTR.glb" },
    { "ctr_sport",   "CTR SPORT",      14500, { 0.78f, 0.06f, 0.05f }, 6.0f, "Vehicles/CTR.glb" },
    { "ctr_gt",      "CTR GT",         21000, { 0.05f, 0.22f, 0.70f }, 7.0f, "Vehicles/CTR.glb" },
    { "ctr_clubsport","CTR CLUBSPORT", 32000, { 0.10f, 0.10f, 0.11f }, 8.0f, "Vehicles/CTR.glb" },
};

// Turntable centers (LOCAL): a row along the hall, nudged 1 m toward the back
// wall so the walkway along the glass is ~5.5 m.
constexpr float kDiscX = 1.0f;
constexpr float kDiscZ[kDealershipDiscs] = { -11.25f, -3.75f, 3.75f, 11.25f };
// Forecourt delivery slots (LOCAL): a row between the glass and the road.
constexpr float kSlotX = -17.4f;
constexpr float kSlotZ[kDealershipForecourtSlots] = { -12.0f, -4.0f, 4.0f, 12.0f };
// Doorway half-width (LOCAL Z) + height.
constexpr float kDoorHalfW = 1.25f, kDoorH = 3.0f;
// Floor top lip above the ground (the terrain mesh must not z-fight the slab).
constexpr float kFloorLip = 0.10f;

// Column-major T(pos) * RotY(yaw). RotY maps +Z -> (sin yaw, 0, cos yaw).
void composeYaw(float x, float y, float z, float yaw, float out[16]) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = c;  out[1] = 0; out[2] = -s; out[3] = 0;
    out[4] = 0;  out[5] = 1; out[6] = 0;  out[7] = 0;
    out[8] = s;  out[9] = 0; out[10] = c; out[11] = 0;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1;
}

x3::rhi::PointLight light(float x, float y, float z, float r, float cr, float cg, float cb) {
    x3::rhi::PointLight pl{};
    pl.pos[0] = x; pl.pos[1] = y; pl.pos[2] = z; pl.range = r;
    pl.color[0] = cr; pl.color[1] = cg; pl.color[2] = cb;
    return pl;
}

constexpr float kNoEmis[4] = { 0, 0, 0, 0 };
constexpr float kWhite[4]  = { 0.96f, 0.96f, 0.97f, 1.0f };
constexpr float kFloorCol[4] = { 0.035f, 0.035f, 0.04f, 1.0f };     // near-black polished
constexpr float kCharcoal[4] = { 0.09f, 0.09f, 0.10f, 1.0f };
constexpr float kConcrete[4] = { 0.23f, 0.23f, 0.24f, 1.0f };
constexpr float kDiscCol[4]  = { 0.14f, 0.14f, 0.16f, 1.0f };
constexpr float kStripEmis[4] = { 1.0f, 0.99f, 0.95f, 6.0f };       // cool-white HDR strips
constexpr float kRimEmis[4]   = { 0.85f, 0.92f, 1.0f, 2.2f };       // disc skirt glow
constexpr float kSignEmis[4]  = { 0.35f, 0.65f, 1.0f, 3.0f };       // cyan signage band
constexpr float kLampEmis[4]  = { 1.0f, 0.97f, 0.9f, 4.0f };

} // namespace

// ===========================================================================
// Frame helpers
// ===========================================================================
void Dealership::toWorld(float lx, float lz, float& wx, float& wz) const {
    wx = m_site.cx + m_cosY * lx + m_sinY * lz;
    wz = m_site.cz - m_sinY * lx + m_cosY * lz;
}

uint32_t Dealership::carCount() const { return kDealershipDiscs; }
const DealershipCarDef& Dealership::trim(uint32_t i) const { return kTrims[i]; }

void Dealership::discCenter(uint32_t i, float out[3]) const {
    toWorld(kDiscX, kDiscZ[i], out[0], out[2]);
    out[1] = m_floorY + kDealershipDiscHeight;
}

void Dealership::forecourtSlot(uint32_t i, float& x, float& z, float& yawDeg) const {
    toWorld(kSlotX, kSlotZ[i], x, z);
    yawDeg = m_site.yawDeg - 90.0f;   // nose toward local -X (the road)
}

int Dealership::nearestDisc(float x, float z) const {
    int best = -1; float bestD = 1e9f;
    for (uint32_t i = 0; i < kDealershipDiscs; ++i) {
        float c[3]; discCenter(i, c);
        const float d = std::sqrt((x - c[0]) * (x - c[0]) + (z - c[2]) * (z - c[2]));
        if (d < kDealershipDiscRadius + kDealershipReach && d < bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

// ===========================================================================
// Build
// ===========================================================================
bool Dealership::build(x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld& physics,
                       std::string_view glbDir, const DealershipSite& site) {
    if (m_built || !m_ground) return false;
    m_site = site;
    m_device = device;
    m_physics = &physics;
    m_cosY = std::cos(site.yawDeg * kDeg);
    m_sinY = std::sin(site.yawDeg * kDeg);
    m_floorY = m_ground(site.cx, site.cz) + kFloorLip;
    for (uint32_t i = 0; i < kDealershipDiscs; ++i) m_discYaw[i] = 0.0f;

    if (m_device) {
        auto mesh = [&](const x3::prims::PrimMesh& pm) {
            x3::rhi::MeshHandle h = m_device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                                         pm.index.data(), (uint32_t)pm.index.size());
            m_meshes.push_back(h);
            return h;
        };
        auto tex1 = [&](uint8_t r, uint8_t g, uint8_t b, bool srgb) {
            const uint8_t px[4] = { r, g, b, 255 };
            x3::rhi::TextureHandle h = m_device->createTexture(px, 1, 1, srgb);
            m_textures.push_back(h);
            return h;
        };
        // Textures: clean white panels (4 panels/repeat, seams) + MR dials
        // (glTF packing: roughness G, metallic B).
        {
            const float tint[3] = { 0.97f, 0.97f, 0.98f };
            auto px = x3::prims::makeCleanPanelRGBA(256, 4, tint, true);
            m_panelTex = m_device->createTexture(px.data(), 256, 256, true);
            m_textures.push_back(m_panelTex);
        }
        m_polishedMr = tex1(0, 20, 128, false);   // rough .08 metal .5 (the showcase slab)
        m_matteMr    = tex1(0, 200, 0, false);    // rough .78 dielectric (panels/concrete)
        m_metalMr    = tex1(0, 90, 220, false);   // brushed dark metal (fascia/mullions)
        m_concreteMr = tex1(0, 235, 0, false);

        const float hd = site.halfDepth, hw = site.halfWidth, ch = site.ceilingH;
        using x3::prims::makeBox;
        m_floor     = mesh(makeBox(hd + 0.3f, 0.06f, hw + 0.3f, 0.0f, -0.06f, 0.0f, 0.25f));
        m_backWall  = mesh(makeBox(0.15f, ch * 0.5f, hw + 0.3f, hd + 0.15f, ch * 0.5f, 0.0f, 0.25f));
        m_sideWallA = mesh(makeBox(hd + 0.3f, ch * 0.5f, 0.15f, 0.0f, ch * 0.5f, -(hw + 0.15f), 0.25f));
        m_sideWallB = mesh(makeBox(hd + 0.3f, ch * 0.5f, 0.15f, 0.0f, ch * 0.5f,  (hw + 0.15f), 0.25f));
        m_ceiling   = mesh(makeBox(hd + 0.3f, 0.15f, hw + 0.3f, 0.0f, ch + 0.15f, 0.0f, 0.25f));
        m_fascia    = mesh(makeBox(hd + 0.45f, 0.35f, hw + 0.45f, 0.0f, ch + 0.45f, 0.0f, 1.0f));
        m_sign      = mesh(makeBox(0.06f, 0.22f, 6.0f, -(hd + 0.5f), ch + 0.45f, 0.0f, 1.0f));
        m_strip     = mesh(makeBox(0.25f, 0.03f, hw - 1.5f, 0.0f, ch - 0.03f, 0.0f, 1.0f));
        m_frontSill = mesh(makeBox(0.15f, 0.08f, hw + 0.3f, -(hd + 0.15f), 0.08f, 0.0f, 1.0f));
        // Glass: two full-height panes flanking the doorway + the header pane.
        const float paneHz = (hw - kDoorHalfW) * 0.5f, paneCz = kDoorHalfW + paneHz;
        m_glassA    = mesh(makeBox(0.02f, ch * 0.5f, paneHz, -(hd + 0.15f), ch * 0.5f, -paneCz, 1.0f));
        m_glassB    = mesh(makeBox(0.02f, ch * 0.5f, paneHz, -(hd + 0.15f), ch * 0.5f,  paneCz, 1.0f));
        m_glassHead = mesh(makeBox(0.02f, (ch - kDoorH) * 0.5f, kDoorHalfW, -(hd + 0.15f),
                                   kDoorH + (ch - kDoorH) * 0.5f, 0.0f, 1.0f));
        m_mullion   = mesh(makeBox(0.06f, ch * 0.5f, 0.05f, -(hd + 0.15f), ch * 0.5f, 0.0f, 1.0f));
        m_disc      = mesh(x3::prims::makeCylinder(kDealershipDiscRadius, kDealershipDiscRadius,
                                                   kDealershipDiscHeight * 0.5f, 48, 0.5f));
        m_discRim   = mesh(x3::prims::makeCylinder(kDealershipDiscRadius + 0.06f,
                                                   kDealershipDiscRadius + 0.06f, 0.02f, 48, 1.0f));
        m_desk      = mesh(makeBox(0.45f, 0.5f, 1.6f, hd - 1.4f, 0.5f, 0.0f, 0.5f));
        m_deskTop   = mesh(makeBox(0.55f, 0.03f, 1.7f, hd - 1.4f, 1.03f, 0.0f, 1.0f));
        {
            const float x0 = -(hd + 0.3f), x1 = -(hd + site.forecourtDepth);
            m_forecourt = mesh(makeBox((x0 - x1) * 0.5f, 0.05f, hw + 1.5f,
                                       (x0 + x1) * 0.5f, -0.09f, 0.0f, 0.25f));
        }
        m_lampPost  = mesh(makeBox(0.08f, 2.4f, 0.08f, 0.0f, 2.4f, 0.0f, 1.0f));
        m_lampHead  = mesh(makeBox(0.35f, 0.06f, 0.35f, 0.0f, 4.9f, 0.0f, 1.0f));

        // The display car: the one hero GLB, or the graybox fallback.
        m_glbSrc.reset(x3::asset::createAssetSource());
        if (m_glbSrc && m_glbSrc->mountDir(std::string(glbDir), 0)) {
            m_glbLoader.reset(x3::asset::createModelLoader(m_device, m_glbSrc.get()));
            m_glbModel = m_glbLoader->load(kTrims[0].glb);
            if (m_glbModel.ok) {
                m_glbDraw = x3::asset::makeDrawables(m_glbModel);
                m_skinned = !m_glbDraw.empty();
            }
        }
        if (!m_skinned) {
            std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
            x3::prims::makeCube(0.5f, cv, ci);
            m_boxMesh = m_device->createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
            m_meshes.push_back(m_boxMesh);
            std::vector<x3::rhi::MeshVertex> wv; std::vector<uint32_t> wi;
            makeUnitCylinderY(14, wv, wi);
            m_wheelMesh = m_device->createMesh(wv.data(), (uint32_t)wv.size(), wi.data(), (uint32_t)wi.size());
            m_meshes.push_back(m_wheelMesh);
            m_whiteTex = tex1(255, 255, 255, true);
        }
    }

    m_built = true;
    x3::logInfo("dealership: built at (" + std::to_string(site.cx) + "," + std::to_string(site.cz) +
                ") floor y " + std::to_string(m_floorY) + ", " + std::to_string(kDealershipDiscs) +
                " turntables, skin " + (m_skinned ? "CTR GLB" : (m_device ? "graybox" : "headless")) +
                ", bodies pending region `" + site.region + "`");
    return true;
}

// ===========================================================================
// Region lifecycle — OUR static bodies only (see the doctrine in the header)
// ===========================================================================
void Dealership::addStaticBox(const Box& b, x3::phys::IPhysicsWorld& physics) {
    float wx, wz; toWorld(b.cx, b.cz, wx, wz);
    x3::phys::BodyId id = physics.addBox(x3::phys::Vec3{ b.hx, b.hy, b.hz },
                                         x3::phys::Vec3{ wx, m_floorY + b.cy, wz },
                                         0.0f, x3::phys::Layer::Static);
    if (!id.valid()) return;
    const float yaw = m_site.yawDeg * kDeg;
    const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    physics.setBodyRotation(id, q);
    m_bodies.push_back(id);
}

void Dealership::onRegionBuild(std::string_view regionId, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || regionId != m_site.region || m_resident) return;
    const float hd = m_site.halfDepth, hw = m_site.halfWidth, ch = m_site.ceilingH;
    // Hall shell.
    addStaticBox({ 0.0f, -0.06f, 0.0f, hd + 0.3f, 0.06f, hw + 0.3f }, physics);                // floor
    addStaticBox({ hd + 0.15f, ch * 0.5f, 0.0f, 0.15f, ch * 0.5f, hw + 0.3f }, physics);       // back wall
    addStaticBox({ 0.0f, ch * 0.5f, -(hw + 0.15f), hd + 0.3f, ch * 0.5f, 0.15f }, physics);    // side A
    addStaticBox({ 0.0f, ch * 0.5f,  (hw + 0.15f), hd + 0.3f, ch * 0.5f, 0.15f }, physics);    // side B
    addStaticBox({ 0.0f, ch + 0.15f, 0.0f, hd + 0.3f, 0.15f, hw + 0.3f }, physics);            // roof
    // Glass front (solid to the player; the doorway between the panes is OPEN).
    const float paneHz = (hw - kDoorHalfW) * 0.5f, paneCz = kDoorHalfW + paneHz;
    addStaticBox({ -(hd + 0.15f), ch * 0.5f, -paneCz, 0.06f, ch * 0.5f, paneHz }, physics);
    addStaticBox({ -(hd + 0.15f), ch * 0.5f,  paneCz, 0.06f, ch * 0.5f, paneHz }, physics);
    addStaticBox({ -(hd + 0.15f), kDoorH + (ch - kDoorH) * 0.5f, 0.0f, 0.06f, (ch - kDoorH) * 0.5f, kDoorHalfW }, physics);
    // Desk + forecourt slab + lamp posts.
    addStaticBox({ hd - 1.4f, 0.5f, 0.0f, 0.55f, 0.53f, 1.7f }, physics);
    {
        const float x0 = -(hd + 0.3f), x1 = -(hd + m_site.forecourtDepth);
        addStaticBox({ (x0 + x1) * 0.5f, -0.09f, 0.0f, (x0 - x1) * 0.5f, 0.05f, hw + 1.5f }, physics);
    }
    addStaticBox({ -18.0f, 2.4f, -9.0f, 0.08f, 2.4f, 0.08f }, physics);
    addStaticBox({ -18.0f, 2.4f,  9.0f, 0.08f, 2.4f, 0.08f }, physics);
    // Turntables: exact cylinders (static meshes in world space).
    for (uint32_t i = 0; i < kDealershipDiscs; ++i) {
        x3::prims::PrimMesh cyl = x3::prims::makeCylinder(kDealershipDiscRadius, kDealershipDiscRadius,
                                                          kDealershipDiscHeight * 0.5f, 24, 1.0f);
        float wx, wz; toWorld(kDiscX, kDiscZ[i], wx, wz);
        const float wy = m_floorY + kDealershipDiscHeight * 0.5f;
        for (size_t v = 0; v + 2 < cyl.cverts.size(); v += 3) {
            cyl.cverts[v] += wx; cyl.cverts[v + 1] += wy; cyl.cverts[v + 2] += wz;
        }
        x3::phys::BodyId id = physics.addStaticMesh(cyl.cverts.data(), (uint32_t)(cyl.cverts.size() / 3),
                                                    cyl.cindex.data(), (uint32_t)cyl.cindex.size());
        if (id.valid()) m_bodies.push_back(id);
    }
    m_resident = true;
    x3::logInfo("dealership: " + std::to_string(m_bodies.size()) + " static bodies placed with region `" +
                std::string(regionId) + "`");
}

void Dealership::onRegionTeardown(std::string_view regionId, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || regionId != m_site.region || !m_resident) return;
    for (x3::phys::BodyId id : m_bodies) physics.removeBody(id);
    m_bodies.clear();
    m_resident = false;
    m_line.clear();
    m_nearIdx = -1;
}

// ===========================================================================
// Per-frame: turntables, prompt, buy
// ===========================================================================
void Dealership::update(float dt) {
    if (!m_built) return;
    dt = std::max(0.0f, dt);
    for (uint32_t i = 0; i < kDealershipDiscs; ++i)
        m_discYaw[i] += kTrims[i].rateDegS * kDeg * dt;   // yaw = rate x elapsed
    if (m_statusT > 0.0f) {
        m_statusT -= dt;
        if (m_statusT <= 0.0f) { m_statusT = 0.0f; m_status.clear(); }
    }
}

bool Dealership::interact(const x3::phys::Vec3& feet, bool eEdge) {
    if (!m_built || !m_resident) { m_line.clear(); return false; }
    m_nearIdx = nearestDisc(feet.x, feet.z);
    bool consumed = false;
    if (m_nearIdx >= 0 && eEdge) {
        const DealershipCarDef& t = kTrims[m_nearIdx];
        char sb[160];
        if (!m_wallet || m_wallet->credits < t.price) {
            m_status = "NO CREDITS";
        } else if (m_sold.size() >= kDealershipForecourtSlots) {
            m_status = "FORECOURT FULL";
        } else {
            WorldCarDef def;
            const uint32_t slot = (uint32_t)m_sold.size();
            def.id = std::string("dealer_") + t.id + "_" + std::to_string(slot);
            forecourtSlot(slot, def.x, def.z, def.yawDeg);
            def.locked = false;
            def.tint[0] = t.tint[0]; def.tint[1] = t.tint[1]; def.tint[2] = t.tint[2];
            def.region = m_site.region;
            if (m_deliver && m_deliver(def)) {
                m_wallet->credits -= t.price;   // exactly the price, once
                m_sold.push_back(m_nearIdx);
                if (m_save) m_save();
                std::snprintf(sb, sizeof(sb), "SOLD - %s delivered to the forecourt", t.name);
                m_status = sb;
                consumed = true;
                x3::logInfo(std::string("dealership: sold ") + t.name + " for " + std::to_string(t.price) +
                            " cr, balance " + std::to_string(m_wallet->credits));
            } else {
                m_status = "DELIVERY UNAVAILABLE";
            }
        }
        m_statusT = kDealershipStatusS;
    }
    if (m_statusT > 0.0f) {
        m_line = m_status;
    } else if (m_nearIdx >= 0) {
        const DealershipCarDef& t = kTrims[m_nearIdx];
        char sb[160];
        if (m_wallet) std::snprintf(sb, sizeof(sb), "[E] BUY %s - %d cr   (you have %d cr)", t.name, t.price, m_wallet->credits);
        else          std::snprintf(sb, sizeof(sb), "[E] BUY %s - %d cr", t.name, t.price);
        m_line = sb;
    } else {
        m_line.clear();
    }
    return consumed;
}

uint32_t Dealership::selectLights(float ex, float ey, float ez,
                                  std::vector<x3::rhi::PointLight>& out) const {
    if (!m_built || !m_resident) return 0;
    const float dx = ex - m_site.cx, dz = ez - m_site.cz;
    (void)ey;
    if (dx * dx + dz * dz > kDealershipLightRange * kDealershipLightRange) return 0;
    std::vector<x3::rhi::PointLight> L;
    L.reserve(kDealershipDiscs * 2 + 2);
    float wx, wz;
    for (uint32_t i = 0; i < kDealershipDiscs; ++i) {
        toWorld(kDiscX, kDiscZ[i], wx, wz);                     // warm overhead key
        L.push_back(light(wx, m_floorY + 4.9f, wz, 12.0f, 6.0f, 5.7f, 5.2f));
        toWorld(kDiscX - 4.2f, kDiscZ[i] + 1.4f, wx, wz);       // cool low fill (glass side)
        L.push_back(light(wx, m_floorY + 1.7f, wz, 9.0f, 1.2f, 2.2f, 3.4f));
    }
    toWorld(-18.0f, -9.0f, wx, wz); L.push_back(light(wx, m_floorY + 4.7f, wz, 18.0f, 3.2f, 3.4f, 4.0f));
    toWorld(-18.0f,  9.0f, wx, wz); L.push_back(light(wx, m_floorY + 4.7f, wz, 18.0f, 3.2f, 3.4f, 4.0f));
    out.insert(out.begin(), L.begin(), L.end());
    return (uint32_t)L.size();
}

// ===========================================================================
// Draw (direct-draw; nothing in the Scene)
// ===========================================================================
void Dealership::drawBox(const x3::rhi::FrameContext& frame, x3::rhi::MeshHandle mesh,
                         x3::rhi::TextureHandle bc, x3::rhi::TextureHandle mr,
                         const float color[4], const float emis[4]) const {
    float m[16]; composeYaw(m_site.cx, m_floorY, m_site.cz, m_site.yawDeg * kDeg, m);
    m_device->drawMeshPBR(frame, mesh, bc, x3::rhi::TextureHandle{}, mr, color, emis, m);
}

void Dealership::drawCar(const x3::rhi::FrameContext& frame, const DealershipCarDef& t,
                         float x, float y, float z, float yaw) const {
    float carM[16]; composeYaw(x, y, z, yaw, carM);
    if (m_skinned) {
        float fin[16];
        for (const auto& d : m_glbDraw) {
            x3::asset::mulMat4(carM, d.nodeTransform, fin);
            const bool matEmis = d.emissiveTexId != 0 ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4] = { d.emissiveFactor[0], d.emissiveFactor[1], d.emissiveFactor[2], matEmis ? 1.0f : 0.0f };
            float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1], d.baseColorFactor[2], d.baseColorFactor[3] };
            if (d.clearcoat > 0.01f) { bc[0] = t.tint[0]; bc[1] = t.tint[1]; bc[2] = t.tint[2]; }  // the paint
            m_device->drawMeshPBR(frame, x3::rhi::MeshHandle{ d.meshId },
                                  x3::rhi::TextureHandle{ d.baseColorTexId },
                                  x3::rhi::TextureHandle{ d.normalTexId },
                                  x3::rhi::TextureHandle{ d.mrTexId },
                                  bc, emis, fin, d.alphaMask, d.alphaBlend,
                                  x3::rhi::TextureHandle{ d.emissiveTexId },
                                  x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                                  d.clearcoat, d.clearcoatRough, 0.0f, 1.0f, 0.0f,
                                  d.metallicFactor, d.roughnessFactor);
        }
        return;
    }
    if (!m_boxMesh.valid()) return;
    const float bodyCol[4]  = { t.tint[0], t.tint[1], t.tint[2], 1.0f };
    const float wheelCol[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
    float local[16], fin[16];
    std::memset(local, 0, sizeof(local));
    local[0] = 1.68f; local[5] = 1.0f; local[10] = 3.9f; local[13] = 0.76f; local[15] = 1.0f;
    x3::asset::mulMat4(carM, local, fin);
    m_device->drawMesh(frame, m_boxMesh, m_whiteTex, bodyCol, fin);
    const float st[4][2] = { { -0.677f, -1.186f }, { 0.677f, -1.186f }, { -0.723f, 1.088f }, { 0.723f, 1.088f } };
    for (int i = 0; i < 4; ++i) {
        const float r = 0.33f, w = 0.24f;
        std::memset(local, 0, sizeof(local));
        local[1] = r; local[4] = -w; local[10] = r;
        local[12] = st[i][0]; local[13] = r; local[14] = st[i][1]; local[15] = 1.0f;
        x3::asset::mulMat4(carM, local, fin);
        m_device->drawMesh(frame, m_wheelMesh, m_whiteTex, wheelCol, fin);
    }
}

void Dealership::draw(const x3::rhi::FrameContext& frame) const {
    if (!m_built || !m_resident || !m_device) return;
    const x3::rhi::TextureHandle none{};
    // Shell.
    drawBox(frame, m_floor,     none,       m_polishedMr, kFloorCol, kNoEmis);
    drawBox(frame, m_backWall,  m_panelTex, m_matteMr,    kWhite,    kNoEmis);
    drawBox(frame, m_sideWallA, m_panelTex, m_matteMr,    kWhite,    kNoEmis);
    drawBox(frame, m_sideWallB, m_panelTex, m_matteMr,    kWhite,    kNoEmis);
    drawBox(frame, m_ceiling,   m_panelTex, m_matteMr,    kWhite,    kNoEmis);
    drawBox(frame, m_fascia,    none,       m_metalMr,    kCharcoal, kNoEmis);
    drawBox(frame, m_frontSill, none,       m_metalMr,    kCharcoal, kNoEmis);
    drawBox(frame, m_desk,      m_panelTex, m_matteMr,    kWhite,    kNoEmis);
    drawBox(frame, m_deskTop,   none,       m_polishedMr, kCharcoal, kNoEmis);
    drawBox(frame, m_forecourt, none,       m_concreteMr, kConcrete, kNoEmis);
    // Emissive: the signage band + three recessed ceiling strips + lamp heads.
    {
        float m[16];
        const float yaw = m_site.yawDeg * kDeg;
        composeYaw(m_site.cx, m_floorY, m_site.cz, yaw, m);
        m_device->drawMeshEmissive(frame, m_sign, none, kWhite, kSignEmis, m);
        for (int i = -1; i <= 1; ++i) {
            float wx, wz; toWorld((float)i * 5.0f, 0.0f, wx, wz);
            composeYaw(wx, m_floorY, wz, yaw, m);
            m_device->drawMeshEmissive(frame, m_strip, none, kWhite, kStripEmis, m);
        }
        for (int s = -1; s <= 1; s += 2) {
            float wx, wz; toWorld(-18.0f, 9.0f * (float)s, wx, wz);
            composeYaw(wx, m_floorY, wz, yaw, m);
            m_device->drawMeshPBR(frame, m_lampPost, none, none, m_metalMr, kCharcoal, kNoEmis, m);
            m_device->drawMeshEmissive(frame, m_lampHead, none, kWhite, kLampEmis, m);
        }
        // Mullions along the glass line (either side of the doorway).
        const float mz[10] = { -12.0f, -9.0f, -6.0f, -3.0f, -kDoorHalfW, kDoorHalfW, 3.0f, 6.0f, 9.0f, 12.0f };
        for (float z : mz) {
            float wx, wz; toWorld(0.0f, z, wx, wz);
            composeYaw(wx, m_floorY, wz, yaw, m);
            m_device->drawMeshPBR(frame, m_mullion, none, none, m_metalMr, kCharcoal, kNoEmis, m);
        }
        // Turntables + display cars.
        for (uint32_t i = 0; i < kDealershipDiscs; ++i) {
            float c[3]; discCenter(i, c);
            composeYaw(c[0], m_floorY + kDealershipDiscHeight * 0.5f, c[2], yaw + m_discYaw[i], m);
            m_device->drawMeshPBR(frame, m_disc, none, none, m_polishedMr, kDiscCol, kNoEmis, m);
            composeYaw(c[0], m_floorY + 0.05f, c[2], yaw, m);
            m_device->drawMeshEmissive(frame, m_discRim, none, kWhite, kRimEmis, m);
            drawCar(frame, kTrims[i], c[0], c[1], c[2], yaw + m_discYaw[i] + (float)i * 0.9f);
        }
        // Glass LAST (blend partition): tinted, mostly clear, no emissive.
        x3::rhi::IRenderDevice::GlassMaterial g{};
        g.opacity = 0.30f; g.refraction = 0.015f; g.roughness = 0.0f; g.specular = 0.7f;
        g.tint[0] = 0.72f; g.tint[1] = 0.82f; g.tint[2] = 0.92f;
        const float glassBc[4] = { 0.72f, 0.82f, 0.92f, 0.30f };
        composeYaw(m_site.cx, m_floorY, m_site.cz, yaw, m);
        m_device->drawMeshGlass(frame, m_glassA,    none, glassBc, kNoEmis, g, m, true);
        m_device->drawMeshGlass(frame, m_glassB,    none, glassBc, kNoEmis, g, m, true);
        m_device->drawMeshGlass(frame, m_glassHead, none, glassBc, kNoEmis, g, m, true);
    }
}

// ===========================================================================
// Shutdown
// ===========================================================================
void Dealership::shutdown(x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    for (x3::phys::BodyId id : m_bodies) physics.removeBody(id);
    m_bodies.clear();
    m_resident = false;
    if (m_device) {
        if (m_glbLoader && m_glbModel.ok) m_glbLoader->unload(m_glbModel);
        m_glbDraw.clear();
        for (x3::rhi::MeshHandle h : m_meshes) if (h.valid()) m_device->destroyMesh(h);
        for (x3::rhi::TextureHandle h : m_textures) if (h.valid()) m_device->destroyTexture(h);
    }
    m_meshes.clear(); m_textures.clear();
    m_glbLoader.reset(); m_glbSrc.reset();
    m_skinned = false;
    m_built = false;
    m_device = nullptr;
}

// ===========================================================================
// Headless self-test (--test-dealership)
// ===========================================================================
bool runDealershipSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[dealership] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[dealership] FAIL ") + name); }
    };

    const DealershipSite& S = kDealershipSite;
    Dealership probe;   // frame math only (toWorld) for the siting checks
    probe.setGroundQuery([](float, float) { return 0.0f; });

    // ---- S: siting against the REAL canon terrain field (pure queries) ----
    {
        // Footprint corners: the hall (+0.5 m) and the far forecourt edge.
        const float lx[6] = { S.halfDepth + 0.5f, S.halfDepth + 0.5f, -(S.halfDepth + 0.5f), -(S.halfDepth + 0.5f),
                              -(S.halfDepth + S.forecourtDepth), -(S.halfDepth + S.forecourtDepth) };
        const float lz[6] = { -(S.halfWidth + 1.5f), S.halfWidth + 1.5f, -(S.halfWidth + 1.5f), S.halfWidth + 1.5f,
                              -(S.halfWidth + 1.5f), S.halfWidth + 1.5f };
        float hmin = 1e9f, hmax = -1e9f; bool river = false;
        float bMinX = 1e9f, bMaxX = -1e9f, bMinZ = 1e9f, bMaxZ = -1e9f;   // hall AABB (world)
        float fMinX = 1e9f;                                                  // forecourt far edge
        for (int i = 0; i < 6; ++i) {
            float wx, wz; probe.toWorld(lx[i], lz[i], wx, wz);
            const float h = terrainHeightAtWorld(wx, wz);
            hmin = std::min(hmin, h); hmax = std::max(hmax, h);
            river = river || worldUnderRiverContains(wx, wz, 2.0f);
            if (i < 4) { bMinX = std::min(bMinX, wx); bMaxX = std::max(bMaxX, wx);
                         bMinZ = std::min(bMinZ, wz); bMaxZ = std::max(bMaxZ, wz); }
            else fMinX = std::min(fMinX, wx);
        }
        x3::logInfo("[dealership] site terrain y " + std::to_string(hmin) + ".." + std::to_string(hmax) +
                    ", hall x " + std::to_string(bMinX) + ".." + std::to_string(bMaxX) +
                    " z " + std::to_string(bMinZ) + ".." + std::to_string(bMaxZ));
        check(hmax - hmin < 0.05f, "S1 site: footprint + forecourt on a FLAT pad (<5 cm relief)");
        check(!river, "S2 site: footprint clear of the river");
        // The fronted road (mirrored from city.cpp's District->Spire connector).
        const float roadEdge = S.roadX + S.roadHalfW;
        const float gap = fMinX - roadEdge;
        check(gap >= 0.0f && gap <= 1.5f, "S3 road: forecourt meets the connector verge (0..1.5 m off the asphalt)");
        check(bMinZ > S.roadZ0 && bMaxZ < S.roadZ1, "S3 road: hall lies along the connector's run");
        check(bMinX > roadEdge + 10.0f, "S4 road: hall does not sit on the asphalt");
        // District built edge: New District's Industrial Blvd is z=410 hw 7 with
        // the warehouses behind it (city.cpp) => nothing built south of z=403.
        check(bMaxZ < 403.0f - 3.0f, "S5 district: hall clear of the New District's built edge (z<400)");
        auto clearOf = [&](float cx, float cz, float r) {
            const float nx = std::clamp(cx, bMinX, bMaxX), nz = std::clamp(cz, bMinZ, bMaxZ);
            return std::hypot(nx - cx, nz - cz) > r;
        };
        check(clearOf(-200.0f, 350.0f, 150.0f) && clearOf(-600.0f, 500.0f, 250.0f),
              "S6 district: hall outside the Industrial Zone + Scrapyard footprints");
    }

    // ---- The headless world: flat slab at y=0 + the dealership ----
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[dealership] physics init failed"); return false; }
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(400.0f, 0.5f, 400.0f, S.cx, -0.5f, S.cz, 0.02f);
        phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                            g.cindex.data(), (uint32_t)g.cindex.size());
    }
    Dealership d;
    d.setGroundQuery([](float, float) { return 0.0f; });
    vehparts::VehicleBuild wallet;              // default 12000 cr
    int saves = 0;
    d.setWallet(&wallet, [&] { ++saves; });
    std::vector<WorldCarDef> delivered;
    d.setDeliverHook([&](const WorldCarDef& def) { delivered.push_back(def); return true; });
    check(d.build(nullptr, *phys, ""), "B1 build: headless build (null device)");
    check(d.turntableCount() == kDealershipDiscs && d.carCount() == kDealershipDiscs,
          "B2 build: 4 turntables + 4 display cars");
    {
        bool distinct = true;
        for (uint32_t i = 0; i < d.carCount(); ++i)
            for (uint32_t j = i + 1; j < d.carCount(); ++j)
                if (d.trim(i).price == d.trim(j).price || std::string(d.trim(i).name) == d.trim(j).name)
                    distinct = false;
        check(distinct, "B3 build: trims carry distinct names + prices");
    }
    check(d.bodyCount() == 0 && !d.resident(), "B4 build: no bodies before the region realizes");

    // ---- T: turntables — dt-scaled and frame-rate independent ----
    {
        Dealership a, b;
        a.setGroundQuery([](float, float) { return 0.0f; });
        b.setGroundQuery([](float, float) { return 0.0f; });
        a.build(nullptr, *phys, ""); b.build(nullptr, *phys, "");
        for (int i = 0; i < 600; ++i) a.update(1.0f / 60.0f);   // 10 s @ 60 Hz
        for (int i = 0; i < 300; ++i) b.update(1.0f / 30.0f);   // 10 s @ 30 Hz
        bool same = true, scaled = true, moving = true, distinct = true;
        for (uint32_t i = 0; i < kDealershipDiscs; ++i) {
            const float expect = a.trim(i).rateDegS * kDeg * 10.0f;
            same    = same    && std::fabs(a.discYaw(i) - b.discYaw(i)) < 1e-3f;
            scaled  = scaled  && std::fabs(a.discYaw(i) - expect) < 1e-3f;
            moving  = moving  && a.discYaw(i) > 0.5f;
            const float r = a.trim(i).rateDegS;
            distinct = distinct && r >= 5.0f && r <= 8.0f;
            for (uint32_t j = i + 1; j < kDealershipDiscs; ++j)
                distinct = distinct && a.trim(j).rateDegS != r;
        }
        check(scaled,  "T1 turntable: yaw == rate x elapsed after 10 s (dt-scaled)");
        check(same,    "T2 turntable: 60 Hz and 30 Hz land on the same yaw (frame-rate independent)");
        check(moving && distinct, "T3 turntable: every disc turns, 5..8 deg/s, rates distinct");
        a.shutdown(*phys); b.shutdown(*phys);
    }

    // ---- R: region realize ----
    d.onRegionBuild("city", *phys);
    const uint32_t bodiesA = d.bodyCount();
    check(d.resident() && bodiesA >= 12, "R1 region: realize places the site's static bodies");
    phys->optimizeBroadphase();
    float c0[3]; d.discCenter(0, c0);
    auto probeDown = [&](float x, float z) {
        return phys->rayCast(x3::phys::Vec3{ x, c0[1] + 2.0f, z }, x3::phys::Vec3{ 0, -1, 0 }, 10.0f,
                             x3::phys::Layer::Static);
    };
    {
        const auto h = probeDown(c0[0], c0[2]);
        check(h.hit && std::fabs(h.distance - 2.0f) < 0.05f, "R2 region: ray down onto turntable 0 hits its top (0.6 m)");
    }

    // ---- P: buy ----
    x3::phys::Vec3 feet{ c0[0], 0.0f, c0[2] };
    {
        float wx, wz; d.toWorld(kDiscX - 3.6f, kDiscZ[0], wx, wz);   // standing at the disc rim, glass side
        feet.x = wx; feet.z = wz;
    }
    check(d.nearestDisc(feet.x, feet.z) == 0, "P0 buy: player at the rim is in reach of disc 0");
    d.interact(feet, false);
    check(d.prompt().find("[E] BUY") == 0 && d.prompt().find(d.trim(0).name) != std::string::npos &&
          d.prompt().find(std::to_string(d.trim(0).price)) != std::string::npos,
          "P1 buy: prompt reads `[E] BUY <NAME> - <price> cr` + balance");
    const int price0 = d.trim(0).price;
    const int before = wallet.credits;
    const bool bought = d.interact(feet, true);
    check(bought && wallet.credits == before - price0, "P2 buy: E with credits deducts EXACTLY the price");
    check(delivered.size() == 1 && d.soldCount() == 1, "P3 buy: one forecourt car delivered");
    check(saves == 1, "P4 buy: wallet persisted once");
    if (!delivered.empty()) {
        const WorldCarDef& w = delivered[0];
        float sx, sz, syaw; d.forecourtSlot(0, sx, sz, syaw);
        check(!w.locked && w.region == "city" && std::fabs(w.x - sx) < 1e-3f && std::fabs(w.z - sz) < 1e-3f &&
              std::fabs(w.tint[0] - d.trim(0).tint[0]) < 1e-4f,
              "P5 buy: delivered car is UNLOCKED, region `city`, on forecourt slot 0, in the trim paint");
        // The slot is on the forecourt slab: between the glass and the road.
        check(w.x > S.roadX + S.roadHalfW && w.x < S.cx - S.halfDepth,
              "P6 buy: forecourt slot lies between the road and the glass");
    }
    check(d.status().rfind("SOLD", 0) == 0 && d.status().find("forecourt") != std::string::npos,
          "P7 buy: status `SOLD - ... delivered to the forecourt`");
    // Insufficient credits: refuse, change nothing.
    d.update(kDealershipStatusS + 0.1f);      // let the SOLD line expire
    wallet.credits = price0 - 1;
    const size_t nDel = delivered.size(); const int nSaves = saves;
    const bool refused = !d.interact(feet, true);
    check(refused && wallet.credits == price0 - 1 && delivered.size() == nDel && saves == nSaves &&
          d.soldCount() == 1, "P8 buy: insufficient credits refuses and changes nothing");
    check(d.status() == "NO CREDITS" && d.prompt() == "NO CREDITS", "P9 buy: status `NO CREDITS`");
    // Fill the forecourt, then a fifth refuses.
    d.update(kDealershipStatusS + 0.1f);
    wallet.credits = 1000000;
    for (int i = 0; i < 3; ++i) { d.interact(feet, true); d.update(kDealershipStatusS + 0.1f); }
    check(d.soldCount() == kDealershipForecourtSlots && delivered.size() == kDealershipForecourtSlots,
          "P10 buy: forecourt fills to 4 slots");
    const int full = wallet.credits;
    check(!d.interact(feet, true) && wallet.credits == full && d.status() == "FORECOURT FULL",
          "P11 buy: fifth purchase refused `FORECOURT FULL`, money untouched");
    d.update(kDealershipStatusS + 0.1f);

    // ---- R: teardown / rebuild — no leaks, sold list kept ----
    d.onRegionTeardown("city", *phys);
    check(d.bodyCount() == 0 && !d.resident(), "R3 region: teardown removes every body");
    check(d.prompt().empty() && !d.interact(feet, true), "R4 region: no prompt / no sales while evicted");
    {
        const auto h = probeDown(c0[0], c0[2]);
        check(h.hit && h.distance > 2.5f, "R5 region: turntable collision GONE after teardown (ray reaches the ground)");
    }
    d.onRegionBuild("city", *phys);
    phys->optimizeBroadphase();
    check(d.bodyCount() == bodiesA && d.resident(), "R6 region: rebuild places the same body count (no leak, no loss)");
    {
        const auto h = probeDown(c0[0], c0[2]);
        check(h.hit && std::fabs(h.distance - 2.0f) < 0.05f, "R7 region: turntable collision back after rebuild");
    }
    check(d.soldCount() == kDealershipForecourtSlots, "R8 region: sold list survives the stream cycle");
    d.onRegionTeardown("city", *phys);
    d.onRegionBuild("city", *phys);
    check(d.bodyCount() == bodiesA, "R9 region: second cycle still leak-free");

    d.shutdown(*phys);
    check(d.bodyCount() == 0 && !d.built(), "X1 shutdown: releases bodies");
    phys->shutdown();

    x3::logInfo("dealership: " + std::to_string(passN) + "/" + std::to_string(passN + failN) + " passed");
    return failN == 0;
}

// ===========================================================================
// --screenshot-dealership: standalone night set at the canon site
// ===========================================================================
int runDealershipScreenshots(x3::rhi::IRenderDevice* device, const std::string& outDir) {
    namespace fs = std::filesystem;
    std::error_code mkec; fs::create_directories(outDir, mkec);
    x3::logInfo("--screenshot-dealership: writing night set to " + outDir);

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("--screenshot-dealership: physics init failed"); return 1; }
    const DealershipSite& S = kDealershipSite;

    Dealership d;
    d.setGroundQuery([](float, float) { return 0.0f; });
    if (!d.build(device, *phys, convertedGlbRoot())) {
        x3::logError("--screenshot-dealership: build failed"); return 1;
    }
    d.onRegionBuild(S.region, *phys);

    // Ground: a wide dark pad + the connector road strip (asphalt) at x = roadX.
    auto makeMesh = [&](const x3::prims::PrimMesh& pm) {
        return device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                  pm.index.data(), (uint32_t)pm.index.size());
    };
    x3::rhi::MeshHandle pad  = makeMesh(x3::prims::makeBox(160.0f, 0.02f, 160.0f, S.cx, -0.02f, S.cz, 0.05f));
    x3::rhi::MeshHandle road = makeMesh(x3::prims::makeBox(S.roadHalfW, 0.03f, (S.roadZ1 - S.roadZ0) * 0.5f,
                                                           S.roadX, 0.0f, (S.roadZ0 + S.roadZ1) * 0.5f, 0.1f));
    x3::rhi::TextureHandle groundMr{}, asphaltMr{};
    { const uint8_t mr[4] = { 0, 240, 0, 255 }; groundMr  = device->createTexture(mr, 1, 1, false); }
    { const uint8_t mr[4] = { 0, 56, 26, 255 };  asphaltMr = device->createTexture(mr, 1, 1, false); }  // wet asphalt
    const float kGround[4]  = { 0.07f, 0.075f, 0.06f, 1.0f };
    const float kAsphalt[4] = { 0.045f, 0.045f, 0.05f, 1.0f };
    const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
    { x3::rhi::IRenderDevice::GiParams g{};   g.enabled = false; device->setGiParams(g); }
    { x3::rhi::IRenderDevice::ReflectionParams rf{}; rf.ssr = true; rf.rtFallback = true; rf.fullRes = true;
      rf.intensity = 1.0f; device->setReflectionParams(rf); }
    device->setShadowBounds(S.cx, 3.0f, S.cz, 60.0f);

    int fails = 0;
    auto still = [&](const char* name, float cx, float cy, float cz, float tx, float ty, float tz, float fov) {
        std::vector<x3::rhi::PointLight> L;
        d.selectLights(cx, cy, cz, L);
        x3::apphost::applyShowroomTimeOfDay(device, /*day=*/false, &L);
        device->setSkyTime(10.0f);
        const float lx = tx - cx, ly = ty - cy, lz = tz - cz;
        const float len = std::max(std::sqrt(lx * lx + ly * ly + lz * lz), 1e-3f);
        device->setCamera(cx, cy, cz, std::atan2(lz, lx), std::asin(ly / len), fov);
        const std::string path = outDir + "/" + name + ".png";
        const int kSettle = 90;   // TAA history + SSR + auto-exposure + IBL probe
        for (int i = 0; i < kSettle; ++i) {
            d.update(1.0f / 60.0f);
            if (i == kSettle - 1) device->armCapture(path.c_str());
            auto fr = device->beginFrame();
            if (fr.valid) {
                device->drawMeshPBR(fr, pad,  x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{}, groundMr,  kGround,  kNoEmis, ident);
                device->drawMeshPBR(fr, road, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{}, asphaltMr, kAsphalt, kNoEmis, ident);
                d.draw(fr);
            }
            device->endFrame(fr);
        }
        if (device->captureFrame(path.c_str())) x3::logInfo("--screenshot-dealership: wrote " + path);
        else { x3::logError("--screenshot-dealership: capture FAILED " + path); ++fails; }
    };

    // 1. Exterior from the road at night: the glass front glowing, forecourt in front.
    {
        float ex, ez, tx, tz;
        d.toWorld(-31.0f, -24.0f, ex, ez);      // on the road, south of the hall
        d.toWorld(-2.0f, 2.0f, tx, tz);          // into the hall
        still("dealership_ext_night", ex, 1.8f, ez, tx, 2.6f, tz, 58.0f);
    }
    // 2. Interior wide: from the front-left corner down the row of turntables.
    {
        float ex, ez, tx, tz;
        d.toWorld(-6.5f, -12.5f, ex, ez);
        d.toWorld(3.0f, 6.0f, tx, tz);
        still("dealership_int_wide", ex, 2.1f, ez, tx, 1.0f, tz, 64.0f);
    }
    // 3. Interior close on one car (turntable 1, the red SPORT), 3/4 front.
    {
        float ex, ez; float c[3]; d.discCenter(1, c);
        d.toWorld(kDiscX - 4.2f, kDiscZ[1] - 3.6f, ex, ez);
        still("dealership_int_car", ex, 1.35f, ez, c[0], c[1] + 0.65f, c[2], 46.0f);
    }

    device->destroyMesh(pad); device->destroyMesh(road);
    device->destroyTexture(groundMr); device->destroyTexture(asphaltMr);
    d.shutdown(*phys);
    phys->shutdown();
    return fails == 0 ? 0 : 1;
}

} // namespace x3::game
