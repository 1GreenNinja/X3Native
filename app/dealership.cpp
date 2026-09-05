// DEALERSHIP — buy new cars; cars spinning slowly under great lighting.
// See dealership.h for the design. Clean-room; built only through the public
// engine interfaces (IRenderDevice / IPhysicsWorld / IModelLoader).

#include "dealership.h"
#include "mesh_prims.h"
#include "vehicle.h"        // makeUnitCylinderY (graybox wheels)
#include "showroom_tod.h"   // applyShowroomTimeOfDay (the screenshot rig)
#include "terrain.h"        // terrainHeightAtWorld / worldUnderRiverContains (self-test siting)
#include "asset_root.h"     // convertedGlbRoot (screenshots) / riggedGlbRoot (NPC rigs)
#include "audio_root.h"     // resolveAudio (door WAVs)
#include "door.h"           // doorEase (the swing profile; DoorSystem itself is slide-only)
#include "env_art.h"        // EnvArtSystem (the road-trees GLB path)
#include "character_anim.h" // AnimatedCharacter (NPC rigs)
#include "player.h"         // Player (NPC capsules)
#include "town.h"           // townPedClipTable (the CityPerson clip names)

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

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
// Doorway half-width (LOCAL Z) + height. Each swing leaf is kDoorHalfW wide
// (hinged at the outer jamb, meeting at the center when closed).
constexpr float kDoorHalfW = 1.25f, kDoorH = 3.0f;
constexpr float kLeafW = kDoorHalfW;
constexpr float kLeafBodyHx = 0.05f;                 // leaf collider half-thickness
// Floor top lip above the ground (the terrain mesh must not z-fight the slab).
constexpr float kFloorLip = 0.10f;
// Forecourt slab top relative to the floor top (the slab box is cy -0.09, hy 0.05).
constexpr float kSlabTop = -0.04f;
// Trees: the road_trees measurements (tools/tree_bole.py) at scale 1 — the
// collider is inscribed in the bole, the crown radius gates placement tests.
constexpr float kOakTrunkHalfW = 1.143f, kPoplarTrunkHalfW = 0.399f;
constexpr float kOakCrownR = 18.0f,    kPoplarCrownR = 4.0f;
constexpr float kTreeBoleH = 5.0f;                   // collider height x scale
constexpr float kTreeSink = 0.15f;                   // base sunk into the ground (never floats)
constexpr const char* kOakGlb    = "nature/OakBigTree01.glb";
constexpr const char* kPoplarGlb = "nature/PoplarTree001.glb";
// Entrance planters (LOCAL): between the glass and the forecourt slots.
constexpr float kPlanterX = -11.6f, kPlanterZ = 4.2f, kPlanterHalf = 0.8f, kPlanterT = 0.10f;
// NPC rigs (riggedGlbRoot) + the showroom-civilian warm palette (host_showroom.cpp).
constexpr const char* kNpcRigs[1 + kDealershipCustomers] = {
    "CityPerson_ManJacket.glb",      // the salesperson
    "CityPerson_WomanCasual.glb", "CityPerson_ManCasual.glb",
    "CityPerson_WomanCoat.glb",   "CityPerson_Elder.glb",
};
constexpr float kNpcTints[1 + kDealershipCustomers][3] = {
    { 0.80f, 0.74f, 0.42f },   // gold (salesperson)
    { 0.86f, 0.52f, 0.46f },   // terracotta
    { 0.52f, 0.66f, 0.40f },   // olive
    { 0.74f, 0.50f, 0.70f },   // mauve
    { 0.58f, 0.62f, 0.84f },   // periwinkle
};
// Salesperson: behind the desk (desk x 7.05..8.15), facing the glass (-X).
constexpr float kSalesX = 8.55f, kSalesZ = 0.0f;
// Salesperson lines: %d slots take the trim prices (from kTrims — never typed twice).
constexpr const char* kTalkLines[3] = {
    "Salesperson: STREET at %d cr is the honest buy. SPORT adds the cams for %d.",
    "Salesperson: The GT - %d cr, the blue one - is what people come back for.",
    "Salesperson: CLUBSPORT is %d cr. Never had one come back to the forecourt.",
};

// Concatenate render geometry (the leaf frame is four rails in ONE mesh).
void appendPrim(x3::prims::PrimMesh& dst, const x3::prims::PrimMesh& src) {
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (uint32_t i : src.index) dst.index.push_back(base + i);
}

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
constexpr float kKerbCol[4]   = { 0.72f, 0.72f, 0.70f, 1.0f };        // clean light-grey kerb

} // namespace

Dealership::Dealership() = default;
Dealership::~Dealership() = default;   // Npc's unique_ptrs need the complete types (here)

// ===========================================================================
// Frame helpers
// ===========================================================================
void Dealership::toWorld(float lx, float lz, float& wx, float& wz) const {
    wx = m_site.cx + m_cosY * lx + m_sinY * lz;
    wz = m_site.cz - m_sinY * lx + m_cosY * lz;
}

void Dealership::toLocal(float wx, float wz, float& lx, float& lz) const {
    const float dx = wx - m_site.cx, dz = wz - m_site.cz;
    lx = m_cosY * dx - m_sinY * dz;
    lz = m_sinY * dx + m_cosY * dz;
}

void Dealership::doorwayCenter(float& wx, float& wz) const {
    toWorld(-(m_site.halfDepth + 0.15f), 0.0f, wx, wz);
}

float Dealership::doorAngleDeg() const {
    return kDealershipDoorSwingDeg * doorEase(m_doorU, 0.3f);
}

float Dealership::treeTrunkHalfW(const DealershipTree& t) {
    return (t.oak ? kOakTrunkHalfW : kPoplarTrunkHalfW) * t.scale;
}
float Dealership::treeCrownRadius(const DealershipTree& t) {
    return (t.oak ? kOakCrownR : kPoplarCrownR) * t.scale;
}

x3::phys::Vec3 Dealership::npcFeet(uint32_t i) const {
    if (i >= m_npcs.size() || !m_npcs[i].body) return x3::phys::Vec3{ 0, 0, 0 };
    return m_npcs[i].body->feet();
}
bool Dealership::npcWalking(uint32_t i) const { return i < m_npcs.size() && m_npcs[i].walking; }

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
        // Kerb prisms: ONE unit cube scaled per prism at draw (untextured, so
        // the UV stretch is invisible); the table is the truth for both draw
        // and collision.
        m_kerbUnit  = mesh(makeBox(0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f));
        // Swing-door leaf, LEAF-LOCAL: hinge on the Y axis at the origin, the
        // leaf extends +Z to kLeafW, 0..kDoorH tall. Slim charcoal frame (two
        // stiles + two rails + push bars, one mesh) around a tinted pane.
        {
            x3::prims::PrimMesh fr;
            const float st = 0.035f, rl = 0.045f;
            appendPrim(fr, makeBox(st, kDoorH * 0.5f, rl, 0.0f, kDoorH * 0.5f, rl));            // hinge stile
            appendPrim(fr, makeBox(st, kDoorH * 0.5f, rl, 0.0f, kDoorH * 0.5f, kLeafW - rl));   // meeting stile
            appendPrim(fr, makeBox(st, rl, kLeafW * 0.5f - 2.0f * rl, 0.0f, rl, kLeafW * 0.5f));           // bottom rail
            appendPrim(fr, makeBox(st, rl, kLeafW * 0.5f - 2.0f * rl, 0.0f, kDoorH - rl, kLeafW * 0.5f));  // top rail
            appendPrim(fr, makeBox(0.02f, 0.02f, 0.30f, 0.09f, 1.05f, kLeafW - 0.42f));                    // push bar (+X face)
            appendPrim(fr, makeBox(0.02f, 0.02f, 0.30f, -0.09f, 1.05f, kLeafW - 0.42f));                   // push bar (-X face)
            m_leafFrame = mesh(fr);
            m_leafGlass = mesh(makeBox(0.012f, kDoorH * 0.5f - 2.0f * rl, kLeafW * 0.5f - 2.0f * rl,
                                       0.0f, kDoorH * 0.5f, kLeafW * 0.5f, 1.0f));
        }

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

    // Site dressing tables (headless too: the colliders + tests read them).
    layoutSite();
    // Trees: the road_trees path verbatim. EnvArtSystem, billboard LOD node
    // skipped, foliage shading on, one instance per table entry.
    if (m_device) {
        m_treeArt = std::make_unique<EnvArtSystem>();
        m_treeArt->setNodeSkip({ "billboard" });
        m_treeArt->setFoliage(1.0f);
        uint32_t planted = 0;
        if (m_treeArt->beginFromDir(*m_device, glbDir)) {
            for (const DealershipTree& t : m_trees) {
                float wx, wz; toWorld(t.lx, t.lz, wx, wz);
                const float wy = treeBaseY(t);
                const float yaw = (m_site.yawDeg + t.yawDeg) * kDeg, c = std::cos(yaw), sn = std::sin(yaw), sc = t.scale;
                const float T[16] = { c * sc, 0, -sn * sc, 0,
                                      0,      sc, 0,       0,
                                      sn * sc, 0, c * sc,  0,
                                      wx, wy, wz, 1 };
                if (m_treeArt->addGlbInstance(t.oak ? kOakGlb : kPoplarGlb, T)) ++planted;
            }
        }
        if (planted == 0) { m_treeArt->destroy(*m_device); m_treeArt.reset(); }
        x3::logInfo("dealership: " + std::to_string(planted) + "/" + std::to_string(m_trees.size()) +
                    " trees planted (poplar/oak GLBs)");
    }
    // NPCs (capsules always; rigs with a device).
    spawnNpcs(physics);

    m_built = true;
    x3::logInfo("dealership: built at (" + std::to_string(site.cx) + "," + std::to_string(site.cz) +
                ") floor y " + std::to_string(m_floorY) + ", " + std::to_string(kDealershipDiscs) +
                " turntables, skin " + (m_skinned ? "CTR GLB" : (m_device ? "graybox" : "headless")) +
                ", " + std::to_string(m_kerbs.size()) + " kerb prisms, " + std::to_string(m_trees.size()) +
                " trees, " + std::to_string(m_npcs.size()) + " NPCs, bodies pending region `" + site.region + "`");
    return true;
}

// ---------------------------------------------------------------------------
// Site dressing tables (LOCAL). The kerb line hugs the slab's road edge and
// both side edges; the aprons drop it where cars cross; the planters ring the
// two entrance trees. Trees: entrance poplars, a poplar row down each side,
// an oak row behind the hall. Every number is in the site frame so the whole
// dressing re-points with the site.
// ---------------------------------------------------------------------------
void Dealership::layoutSite() {
    m_kerbs.clear(); m_trees.clear();
    const float hd = m_site.halfDepth, hw = m_site.halfWidth;
    const float x0 = -(hd + 0.3f), x1 = -(hd + m_site.forecourtDepth);   // slab x range [x1, x0]
    const float zw = hw + 1.5f;                                          // slab half width
    const float kw = kDealershipKerbW * 0.5f, kh = kDealershipKerbH * 0.5f, ah = kDealershipApronH * 0.5f;
    const float kerbX = x1 + kw;                                         // road-edge kerb center
    // Aprons: 6 m wide, centred between forecourt slots 0/1 and 2/3 (z = +-8).
    const float apronZ = (kSlotZ[0] + kSlotZ[1]) * 0.5f, apronHalf = 3.0f;   // -8
    auto kerbZ = [&](float za, float zb) {   // a road-edge kerb run from za..zb
        m_kerbs.push_back({ kerbX, kSlabTop + kh, (za + zb) * 0.5f, kw, kh, (zb - za) * 0.5f });
    };
    kerbZ(-zw, apronZ - apronHalf);
    kerbZ(apronZ + apronHalf, -apronZ - apronHalf);
    kerbZ(-apronZ + apronHalf, zw);
    m_kerbs.push_back({ kerbX, kSlabTop + ah,  apronZ, kw, ah, apronHalf });   // dropped kerb (in)
    m_kerbs.push_back({ kerbX, kSlabTop + ah, -apronZ, kw, ah, apronHalf });   // dropped kerb (out)
    // Side kerbs: from the road-edge kerb to the glass line.
    {
        const float xa = x1 + 2.0f * kw, xb = x0;
        m_kerbs.push_back({ (xa + xb) * 0.5f, kSlabTop + kh, -(zw - kw), (xb - xa) * 0.5f, kh, kw });
        m_kerbs.push_back({ (xa + xb) * 0.5f, kSlabTop + kh,  (zw - kw), (xb - xa) * 0.5f, kh, kw });
    }
    // Entrance planters: a square kerb ring around each flanking tree.
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float px = kPlanterX, pz = kPlanterZ * (float)sgn, h = kPlanterHalf, t = kPlanterT * 0.5f;
        m_kerbs.push_back({ px - h + t, kSlabTop + kh, pz, t, kh, h });
        m_kerbs.push_back({ px + h - t, kSlabTop + kh, pz, t, kh, h });
        m_kerbs.push_back({ px, kSlabTop + kh, pz - h + t, h - 2.0f * t, kh, t });
        m_kerbs.push_back({ px, kSlabTop + kh, pz + h - t, h - 2.0f * t, kh, t });
        m_trees.push_back({ px, pz, 0.42f, sgn < 0 ? 0.0f : 90.0f, false });   // ~9 m poplar
    }
    // Side rows: poplars 2 m outside the slab edge, from the road end to the back wall.
    {
        const float xs[8] = { -23.0f, -18.5f, -14.0f, -9.5f, -5.0f, -0.5f, 4.0f, 8.5f };
        for (int sgn = -1; sgn <= 1; sgn += 2)
            for (int i = 0; i < 8; ++i)
                m_trees.push_back({ xs[i], (zw + 2.0f) * (float)sgn, 0.50f, (float)(i * 47 % 360), false });   // ~10.8 m
    }
    // Back row: oaks behind the hall, crowns (6.1 m) clearing the back wall.
    for (int i = -2; i <= 2; ++i)
        m_trees.push_back({ hd + 6.5f, (float)i * 6.0f, 0.34f, (float)((i + 2) * 73 % 360), true });   // ~12 m
}

float Dealership::treeBaseY(const DealershipTree& t) const {
    const float hd = m_site.halfDepth;
    const float x0 = -(hd + 0.3f), x1 = -(hd + m_site.forecourtDepth), zw = m_site.halfWidth + 1.5f;
    const bool onSlab = t.lx >= x1 && t.lx <= x0 && std::fabs(t.lz) <= zw;
    if (onSlab) return m_floorY + kSlabTop - 0.05f;        // planter fill
    float wx, wz; toWorld(t.lx, t.lz, wx, wz);
    return (m_ground ? m_ground(wx, wz) : m_floorY - kFloorLip) - kTreeSink;
}

// ---------------------------------------------------------------------------
// NPCs: capsules at their first waypoint (the salesperson behind the desk),
// rigs only with a device. See the header (kinematic browse pace).
// ---------------------------------------------------------------------------
void Dealership::spawnNpcs(x3::phys::IPhysicsWorld& physics) {
    m_npcs.clear();
    const float d0 = kDiscZ[0], d1 = kDiscZ[1], d2 = kDiscZ[2], d3 = kDiscZ[3];
    // Waypoints (LOCAL): the glass-side walkway is x -8..-2 (disc rims at -1.5),
    // the back aisle x 4.5..6.5 (rims at 3.5, desk from 7.05). Every point is
    // >= 0.9 m off a rim, and every LEG stays > kDealershipDoorTrigger from the
    // doorway center (-9.3, 0) so browsing never cycles the doors (asserted);
    // faceDisc turns the browser toward that car on arrival.
    const std::vector<std::vector<Waypoint>> paths = {
        {},                                                                                   // salesperson
        { { -5.5f, -13.5f, 0.0f, -1 }, { -3.2f, d0, 5.0f, 0 }, { -5.5f, -7.5f, 0.0f, -1 }, { -3.2f, d1, 5.0f, 1 } },
        { { -5.5f,  13.5f, 0.0f, -1 }, { -3.2f, d3, 5.0f, 3 }, { -5.5f,  7.5f, 0.0f, -1 }, { -3.2f, d2, 5.0f, 2 } },
        { { -6.5f,  -2.0f, 0.0f, -1 }, { -6.5f, 2.0f, 3.0f, -1 }, { -6.5f, 9.5f, 0.0f, -1 }, { -3.2f, 7.5f, 4.0f, 2 } },
        { {  5.4f,  -7.5f, 6.0f,  0 }, {  5.4f, 0.0f, 4.0f,  1 }, {  5.4f, 7.5f, 6.0f,  2 } },
    };
    const CharacterClipTable table = townPedClipTable();
    const std::string rigDir = riggedGlbRoot();
    for (uint32_t i = 0; i < 1 + kDealershipCustomers; ++i) {
        Npc n;
        n.path = paths[i];
        n.tint[0] = kNpcTints[i][0]; n.tint[1] = kNpcTints[i][1]; n.tint[2] = kNpcTints[i][2];
        const float lx = n.path.empty() ? kSalesX : n.path[0].lx;
        const float lz = n.path.empty() ? kSalesZ : n.path[0].lz;
        float wx, wz; toWorld(lx, lz, wx, wz);
        n.body = std::make_unique<Player>();
        n.body->spawn(physics, wx, m_floorY + 0.05f, wz);
        n.faceYaw = localDirYaw(-1.0f, 0.0f);            // face the glass until told otherwise
        n.body->setLook(n.faceYaw, 0.0f);
        if (m_device) {
            n.rig = std::make_unique<AnimatedCharacter>();
            if (n.rig->load(*m_device, rigDir, kNpcRigs[i], table)) n.rig->setTint(n.tint[0], n.tint[1], n.tint[2]);
            else { x3::logWarn(std::string("dealership: NPC rig unavailable: ") + kNpcRigs[i]); n.rig.reset(); }
        }
        m_npcs.push_back(std::move(n));
    }
}

float Dealership::localDirYaw(float lx, float lz) const {
    // Rig/device convention: fwd = (cos yaw, ., sin yaw) in WORLD.
    const float wx = m_cosY * lx + m_sinY * lz, wz = -m_sinY * lx + m_cosY * lz;
    return std::atan2(wz, wx);
}

// ===========================================================================
// Region lifecycle — OUR static bodies only (see the doctrine in the header)
// ===========================================================================
x3::phys::BodyId Dealership::addStaticBox(const Box& b, x3::phys::IPhysicsWorld& physics) {
    float wx, wz; toWorld(b.cx, b.cz, wx, wz);
    x3::phys::BodyId id = physics.addBox(x3::phys::Vec3{ b.hx, b.hy, b.hz },
                                         x3::phys::Vec3{ wx, m_floorY + b.cy, wz },
                                         0.0f, x3::phys::Layer::Static);
    if (!id.valid()) return id;
    const float yaw = m_site.yawDeg * kDeg;
    const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
    physics.setBodyRotation(id, q);
    m_bodies.push_back(id);
    return id;
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
    // Door leaves: one static box each, seated to the CURRENT angle right away
    // (the doors may be mid-swing when the region comes back).
    for (uint32_t leaf = 0; leaf < 2; ++leaf)
        m_leafBody[leaf] = addStaticBox({ -(hd + 0.15f), kDoorH * 0.5f, 0.0f, kLeafBodyHx, kDoorH * 0.5f, kLeafW * 0.5f }, physics);
    m_physics = &physics;
    seatLeafBodies();
    // Kerb prisms (the table is the truth; the draw scales the unit cube by it).
    for (const DealershipKerb& k : m_kerbs)
        addStaticBox({ k.cx, k.cy, k.cz, k.hx, k.hy, k.hz }, physics);
    // Tree trunks: the road_trees bole collider (a box at the base, boleH tall).
    for (const DealershipTree& t : m_trees) {
        const float thw = treeTrunkHalfW(t), bole = kTreeBoleH * t.scale;
        addStaticBox({ t.lx, (treeBaseY(t) - m_floorY) + bole * 0.5f, t.lz, thw, bole * 0.5f, thw }, physics);
    }
    m_resident = true;
    x3::logInfo("dealership: " + std::to_string(m_bodies.size()) + " static bodies placed with region `" +
                std::string(regionId) + "`");
}

void Dealership::onRegionTeardown(std::string_view regionId, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || regionId != m_site.region || !m_resident) return;
    for (x3::phys::BodyId id : m_bodies) physics.removeBody(id);
    m_bodies.clear();
    m_leafBody[0] = m_leafBody[1] = x3::phys::BodyId{};
    m_physics = nullptr;
    m_resident = false;
    m_line.clear();
    m_nearIdx = -1;
    m_nearSales = false;
}

void Dealership::setAudio(x3::audio::IAudioSystem* audio) {
    m_audio = audio;
    if (!m_audio) return;
    m_sndOpen  = m_audio->load(resolveAudio("doors/door_open.wav"));
    m_sndClose = m_audio->load(resolveAudio("doors/door_close.wav"));
}

// ===========================================================================
// Per-frame: turntables, prompt, buy
// ===========================================================================
void Dealership::update(float dt, const x3::phys::Vec3* playerFeet) {
    if (!m_built) return;
    dt = std::max(0.0f, dt);
    for (uint32_t i = 0; i < kDealershipDiscs; ++i)
        m_discYaw[i] += kTrims[i].rateDegS * kDeg * dt;   // yaw = rate x elapsed
    if (m_statusT > 0.0f) {
        m_statusT -= dt;
        if (m_statusT <= 0.0f) { m_statusT = 0.0f; m_status.clear(); }
    }
    updateNpcs(dt);                  // NPCs first: they are door approachers too
    updateDoors(dt, playerFeet);
}

// Leaf pose in WORLD: hinge position + the leaf's world yaw. Leaf-local +Z is
// the leaf's run (hinge -> meeting stile). Closed: leaf 0 (hinge at -Z) runs
// +Z, leaf 1 (hinge at +Z) runs -Z (yaw pi). Swing rotates both about their
// hinges by the same angle in mirror, sign from m_doorDir (+1 into the hall).
void Dealership::leafPose(uint32_t leaf, float& wx, float& wz, float& worldYaw) const {
    const float hd = m_site.halfDepth;
    const float hz = leaf == 0 ? -kDoorHalfW : kDoorHalfW;
    toWorld(-(hd + 0.15f), hz, wx, wz);
    const float ang = doorAngleDeg() * kDeg * (float)m_doorDir;
    const float leafYaw = leaf == 0 ? ang : kPi - ang;
    worldYaw = m_site.yawDeg * kDeg + leafYaw;
}

void Dealership::seatLeafBodies() {
    if (!m_physics) return;
    for (uint32_t leaf = 0; leaf < 2; ++leaf) {
        if (!m_leafBody[leaf].valid()) continue;
        float hx, hz, yaw; leafPose(leaf, hx, hz, yaw);
        // Box center = hinge + half the run along the leaf's +Z (world: sin, cos).
        const x3::phys::Vec3 c{ hx + std::sin(yaw) * kLeafW * 0.5f, m_floorY + kDoorH * 0.5f, hz + std::cos(yaw) * kLeafW * 0.5f };
        const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        m_physics->setBodyPosition(m_leafBody[leaf], c);
        m_physics->setBodyRotation(m_leafBody[leaf], q);
    }
}

void Dealership::updateDoors(float dt, const x3::phys::Vec3* playerFeet) {
    if (!m_resident) return;
    // Who wants the door: any actor within the trigger radius of the doorway
    // center (XZ). The swing direction is chosen ONCE, from the nearest actor's
    // side when the doors start to open, and held until they are fully shut.
    const float hd = m_site.halfDepth, doorX = -(hd + 0.15f);
    float best = 1e9f, bestLx = 0.0f;
    auto consider = [&](const x3::phys::Vec3& f) {
        float lx, lz; toLocal(f.x, f.z, lx, lz);
        const float dx = lx - doorX, d = std::sqrt(dx * dx + lz * lz);
        if (d < best) { best = d; bestLx = lx; }
    };
    if (playerFeet) consider(*playerFeet);
    for (const Npc& n : m_npcs) consider(n.body->feet());
    const bool want = best < kDealershipDoorTrigger;
    const float before = m_doorU;
    float dwx, dwz; doorwayCenter(dwx, dwz);
    if (want) {
        if (m_doorU <= 0.0f) {
            m_doorDir = bestLx > doorX ? -1 : +1;   // swing AWAY from the approacher
            if (m_audio && m_sndOpen.valid()) m_audio->playSound3D(m_sndOpen, dwx, m_floorY + 1.5f, dwz, 0.8f);
        }
        m_doorHold = kDealershipDoorHoldS;
        m_doorU = std::min(1.0f, m_doorU + dt / kDealershipDoorOpenS);
    } else if (m_doorU > 0.0f) {
        m_doorHold -= dt;
        if (m_doorHold <= 0.0f) {
            m_doorU = std::max(0.0f, m_doorU - dt / kDealershipDoorCloseS);
            if (m_doorU <= 0.0f) {
                m_doorDir = 0;
                if (m_audio && m_sndClose.valid()) m_audio->playSound3D(m_sndClose, dwx, m_floorY + 1.5f, dwz, 0.8f);
            }
        }
    }
    if (m_doorU != before) seatLeafBodies();
}

// Customers loiter: dwell at a waypoint (facing its disc), then walk a
// straight line to the next at browse pace. The capsule is MOVED (teleport
// per frame, dt-scaled) and then given a zero-input update so gravity /
// contacts still run; the rig reads the feet delta and picks walk/idle.
void Dealership::updateNpcs(float dt) {
    if (!m_resident || !m_physics) return;
    for (Npc& n : m_npcs) {
        n.walking = false;
        if (!n.path.empty()) {
            const Waypoint& w = n.path[n.wp];
            if (n.dwell > 0.0f) {
                n.dwell -= dt;
            } else {
                const x3::phys::Vec3 f = n.body->feet();
                float tx, tz; toWorld(w.lx, w.lz, tx, tz);
                const float dx = tx - f.x, dz = tz - f.z, d = std::sqrt(dx * dx + dz * dz);
                if (d < 0.1f) {
                    n.dwell = w.dwell;
                    if (w.faceDisc >= 0) {
                        float c[3]; discCenter((uint32_t)w.faceDisc, c);
                        n.faceYaw = std::atan2(c[2] - f.z, c[0] - f.x);
                    }
                    n.wp = (n.wp + 1) % (uint32_t)n.path.size();
                } else {
                    const float step = std::min(d, kDealershipNpcWalk * dt);
                    n.body->setFeetPosition(*m_physics, { f.x + dx / d * step, f.y, f.z + dz / d * step });
                    n.faceYaw = std::atan2(dz, dx);
                    n.walking = true;
                }
            }
        }
        n.body->update(PlayerInput{}, dt, *m_physics);
        if (n.rig && m_device) {
            AnimatedCharacter::Intent in;
            in.moveFwd = n.walking ? 1.0f : 0.0f;
            n.rig->update(*n.body, in, n.faceYaw, dt, *m_physics, *m_device);
        }
    }
}

bool Dealership::interact(const x3::phys::Vec3& feet, bool eEdge) {
    if (!m_built || !m_resident) { m_line.clear(); m_nearSales = false; return false; }
    m_nearIdx = nearestDisc(feet.x, feet.z);
    {
        const x3::phys::Vec3 sf = m_npcs.empty() ? feet : m_npcs[0].body->feet();
        const float dx = feet.x - sf.x, dz = feet.z - sf.z;
        m_nearSales = !m_npcs.empty() && dx * dx + dz * dz < kDealershipTalkReach * kDealershipTalkReach;
    }
    bool consumed = false;
    if (m_nearIdx >= 0 && eEdge) {
        const DealershipCarDef& t = kTrims[m_nearIdx];
        char sb[160];
        if (!m_wallet || m_wallet->credits < t.price) {
            m_status = "NO CREDITS";
        } else if (m_sold.size() >= kDealershipForecourtSlots) {
            m_status = "FORECOURT FULL";
        } else if (deliverSold(m_nearIdx, t.tint)) {
            m_wallet->credits -= t.price;   // exactly the price, once
            if (m_save) m_save();
            if (!m_soldPath.empty()) saveSoldFile(m_soldPath);
            std::snprintf(sb, sizeof(sb), "SOLD - %s delivered to the forecourt", t.name);
            m_status = sb;
            consumed = true;
            x3::logInfo(std::string("dealership: sold ") + t.name + " for " + std::to_string(t.price) +
                        " cr, balance " + std::to_string(m_wallet->credits));
        } else {
            m_status = "DELIVERY UNAVAILABLE";
        }
        m_statusT = kDealershipStatusS;
    } else if (m_nearSales && eEdge) {
        // Salesperson: three short lines cycling; prices come from the table.
        char sb[200];
        const int i = m_talkLine % 3;
        if (i == 0)      std::snprintf(sb, sizeof(sb), kTalkLines[0], kTrims[0].price, kTrims[1].price);
        else if (i == 1) std::snprintf(sb, sizeof(sb), kTalkLines[1], kTrims[2].price);
        else             std::snprintf(sb, sizeof(sb), kTalkLines[2], kTrims[3].price);
        m_status = sb;
        ++m_talkLine;
        m_statusT = kDealershipStatusS;
        consumed = true;
    }
    if (m_statusT > 0.0f) {
        m_line = m_status;
    } else if (m_nearIdx >= 0) {
        const DealershipCarDef& t = kTrims[m_nearIdx];
        char sb[160];
        if (m_wallet) std::snprintf(sb, sizeof(sb), "[E] BUY %s - %d cr   (you have %d cr)", t.name, t.price, m_wallet->credits);
        else          std::snprintf(sb, sizeof(sb), "[E] BUY %s - %d cr", t.name, t.price);
        m_line = sb;
    } else if (m_nearSales) {
        m_line = "[E] Talk";
    } else {
        m_line.clear();
    }
    return consumed;
}

// ===========================================================================
// Sold list: deliver + persist (sidecar JSON; see the header)
// ===========================================================================
bool Dealership::deliverSold(int trimIdx, const float tint[3], WorldCarDef* out) {
    if (trimIdx < 0 || trimIdx >= (int)kDealershipDiscs) return false;
    if (m_sold.size() >= kDealershipForecourtSlots) return false;
    const DealershipCarDef& t = kTrims[trimIdx];
    WorldCarDef def;
    const uint32_t slot = (uint32_t)m_sold.size();
    def.id = std::string("dealer_") + t.id + "_" + std::to_string(slot);
    forecourtSlot(slot, def.x, def.z, def.yawDeg);
    def.locked = false;
    def.tint[0] = tint[0]; def.tint[1] = tint[1]; def.tint[2] = tint[2];
    def.region = m_site.region;
    if (!m_deliver || !m_deliver(def)) return false;
    Sold s; s.trim = trimIdx; s.tint[0] = tint[0]; s.tint[1] = tint[1]; s.tint[2] = tint[2];
    m_sold.push_back(s);
    if (out) *out = def;
    return true;
}

std::string Dealership::soldJson() const {
    std::ostringstream o;
    o << "{\n  \"format\": \"x3-dealership-1\",\n  \"sold\": [";
    for (size_t i = 0; i < m_sold.size(); ++i) {
        const Sold& s = m_sold[i];
        char b[200];
        std::snprintf(b, sizeof(b), "%s\n    { \"slot\": %u, \"trim\": \"%s\", \"tint\": [%.4f, %.4f, %.4f] }",
                      i ? "," : "", (unsigned)i, kTrims[s.trim].id, s.tint[0], s.tint[1], s.tint[2]);
        o << b;
    }
    o << (m_sold.empty() ? "" : "\n  ") << "]\n}\n";
    return o.str();
}

bool Dealership::restoreSold(const std::string& json) {
    // Hand parser, same posture as vehparts: find "sold", then each {...}
    // object's slot / trim / tint. Unknown trims are skipped; the list is
    // re-sorted by slot so slot == index holds regardless of file order.
    if (json.find("\"x3-dealership") == std::string::npos) return false;
    const size_t soldAt = json.find("\"sold\"");
    if (soldAt == std::string::npos) return false;
    struct Entry { int slot, trim; float tint[3]; };
    std::vector<Entry> entries;
    size_t pos = json.find('[', soldAt);
    if (pos == std::string::npos) return false;
    // The array's own close bracket (the tint arrays inside nest one deeper).
    size_t end = std::string::npos;
    for (size_t i = pos, depth = 0; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']' && --depth == 0) { end = i; break; }
    }
    if (end == std::string::npos) return false;
    while (pos < end) {
        const size_t ob = json.find('{', pos);
        if (ob == std::string::npos || ob > end) break;
        const size_t cb = json.find('}', ob);
        if (cb == std::string::npos) return false;
        const std::string obj = json.substr(ob, cb - ob + 1);
        Entry e{ -1, -1, { 1, 1, 1 } };
        size_t k = obj.find("\"slot\":");
        if (k != std::string::npos) e.slot = std::atoi(obj.c_str() + k + 7);
        k = obj.find("\"trim\":");
        if (k != std::string::npos) {
            const size_t q0 = obj.find('"', k + 7);
            const size_t q1 = q0 == std::string::npos ? q0 : obj.find('"', q0 + 1);
            if (q1 != std::string::npos) {
                const std::string id = obj.substr(q0 + 1, q1 - q0 - 1);
                for (uint32_t i = 0; i < kDealershipDiscs; ++i) if (id == kTrims[i].id) e.trim = (int)i;
            }
        }
        k = obj.find("\"tint\":");
        if (k != std::string::npos) {
            const size_t br = obj.find('[', k);
            if (br != std::string::npos) {
                char* cur = const_cast<char*>(obj.c_str()) + br + 1;
                for (int c = 0; c < 3; ++c) {
                    e.tint[c] = std::strtof(cur, &cur);
                    while (*cur == ' ' || *cur == ',') ++cur;
                }
            }
        }
        if (e.slot >= 0 && e.trim >= 0) entries.push_back(e);
        else x3::logWarn("dealership: sold entry skipped (unknown trim or slot): " + obj);
        pos = cb + 1;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.slot < b.slot; });
    m_sold.clear();
    for (const Entry& e : entries) {
        if (m_sold.size() >= kDealershipForecourtSlots) break;
        if (!deliverSold(e.trim, e.tint)) x3::logWarn("dealership: restore could not deliver slot " + std::to_string(e.slot));
    }
    x3::logInfo("dealership: restored " + std::to_string(m_sold.size()) + " sold car(s)");
    return true;
}

bool Dealership::saveSoldFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { x3::logWarn("dealership: cannot write " + path); return false; }
    f << soldJson();
    return (bool)f;
}

bool Dealership::loadSoldFile(const std::string& path) {
    m_soldPath = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return true;                  // no file yet: nothing sold, not an error
    std::stringstream ss; ss << f.rdbuf();
    return restoreSold(ss.str());
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
        // Kerb line: the unit cube scaled per prism (site frame).
        {
            float siteM[16], L[16], fin[16];
            composeYaw(m_site.cx, m_floorY, m_site.cz, yaw, siteM);
            for (const DealershipKerb& k : m_kerbs) {
                std::memset(L, 0, sizeof(L));
                L[0] = 2.0f * k.hx; L[5] = 2.0f * k.hy; L[10] = 2.0f * k.hz;
                L[12] = k.cx; L[13] = k.cy; L[14] = k.cz; L[15] = 1.0f;
                x3::asset::mulMat4(siteM, L, fin);
                m_device->drawMeshPBR(frame, m_kerbUnit, none, none, m_matteMr, kKerbCol, kNoEmis, fin);
            }
        }
        // Trees (EnvArtSystem draws its own instances).
        if (m_treeArt) m_treeArt->draw(*m_device, frame);
        // NPCs at their capsules' feet.
        for (const Npc& n : m_npcs)
            if (n.rig) n.rig->draw(frame, *m_device, *n.body, 0.0f, 0.0f, true);
        // Door leaf frames (opaque; the panes go with the glass below).
        for (uint32_t leaf = 0; leaf < 2; ++leaf) {
            float hx, hz, ly; leafPose(leaf, hx, hz, ly);
            composeYaw(hx, m_floorY, hz, ly, m);
            m_device->drawMeshPBR(frame, m_leafFrame, none, none, m_metalMr, kCharcoal, kNoEmis, m);
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
        for (uint32_t leaf = 0; leaf < 2; ++leaf) {
            float hx, hz, ly; leafPose(leaf, hx, hz, ly);
            composeYaw(hx, m_floorY, hz, ly, m);
            m_device->drawMeshGlass(frame, m_leafGlass, none, glassBc, kNoEmis, g, m, true);
        }
    }
}

// ===========================================================================
// Shutdown
// ===========================================================================
void Dealership::shutdown(x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    for (x3::phys::BodyId id : m_bodies) physics.removeBody(id);
    m_bodies.clear();
    m_leafBody[0] = m_leafBody[1] = x3::phys::BodyId{};
    m_physics = nullptr;
    m_resident = false;
    // NPC capsules are characters (not removable through removeBody; the
    // physics world owns them until its own shutdown). Rig GPU resources are
    // process-lifetime like Town's.
    m_npcs.clear();
    if (m_device) {
        if (m_treeArt) { m_treeArt->destroy(*m_device); m_treeArt.reset(); }
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

    // ---- K: kerb line + trees (tables are LOCAL; the colliders follow them) ----
    {
        const float hd = S.halfDepth, hw = S.halfWidth;
        const float hallX0 = -(hd + 0.3f), hallX1 = hd + 0.3f, hallZ = hw + 0.3f;
        const float slabX1 = -(hd + S.forecourtDepth);
        const float eps = 1e-3f;
        auto boxHitsHall = [&](float cx, float cz, float hx, float hz) {
            return cx + hx > hallX0 + eps && cx - hx < hallX1 - eps && std::fabs(cz) - hz < hallZ - eps;
        };
        auto boxHitsSlot = [&](float cx, float cz, float hx, float hz) {
            for (uint32_t i = 0; i < kDealershipForecourtSlots; ++i)
                if (std::fabs(cx - kSlotX) - hx < 2.6f - eps && std::fabs(cz - kSlotZ[i]) - hz < 1.3f - eps) return true;
            return false;
        };
        int aprons = 0; bool kerbHeight = true, kerbClear = true, kerbFloat = true;
        for (uint32_t i = 0; i < d.kerbCount(); ++i) {
            const DealershipKerb& k = d.kerb(i);
            const bool apron = k.hy * 2.0f < kDealershipKerbH * 0.5f;
            if (apron) { if (std::fabs(std::fabs(k.cz) - 8.0f) < 0.01f && k.hz * 2.0f >= 6.0f - eps) ++aprons; }
            else kerbHeight = kerbHeight && std::fabs(k.hy * 2.0f - kDealershipKerbH) < 1e-4f;
            kerbClear = kerbClear && !boxHitsHall(k.cx, k.cz, k.hx, k.hz) && !boxHitsSlot(k.cx, k.cz, k.hx, k.hz) &&
                        k.cx - k.hx >= slabX1 - eps;                       // never onto the asphalt
            kerbFloat = kerbFloat && std::fabs((k.cy - k.hy) - kSlabTop) < 1e-4f;   // sits ON the slab
        }
        check(d.kerbCount() >= 7 && kerbHeight, "K1 kerb: kerb line placed, prisms 0.15 m high");
        check(aprons == 2, "K2 kerb: two 6 m dropped-kerb aprons at z = +-8 (the driveways)");
        check(kerbClear, "K3 kerb: no prism overlaps the hall, a forecourt slot, or the asphalt");
        check(kerbFloat, "K4 kerb: every prism's underside is the slab top (nothing floats)");
        bool trunksClear = true, crownsClear = true, anyOak = false, anyPoplar = false;
        for (uint32_t i = 0; i < d.treeCount(); ++i) {
            const DealershipTree& t = d.tree(i);
            const float thw = Dealership::treeTrunkHalfW(t), cr = Dealership::treeCrownRadius(t);
            anyOak = anyOak || t.oak; anyPoplar = anyPoplar || !t.oak;
            trunksClear = trunksClear && !boxHitsHall(t.lx, t.lz, thw, thw) && !boxHitsSlot(t.lx, t.lz, thw, thw) &&
                          t.lx - thw >= slabX1 - eps;
            crownsClear = crownsClear && !boxHitsSlot(t.lx, t.lz, cr, cr) && t.lx - cr >= slabX1 - 0.5f - eps;
        }
        check(d.treeCount() >= 12 && anyOak && anyPoplar, "K5 trees: a mixed row planted (poplars + oaks)");
        check(trunksClear && crownsClear, "K6 trees: no trunk in the hall / a slot / the road; crowns clear of slots + road");
        check(d.bodyCount() == 16 + 2 + d.kerbCount() + d.treeCount(),
              "K7 region: body count == shell + 2 leaves + kerbs + trunks");
        {
            float wx, wz; d.toWorld(slabX1 + 0.15f, 0.0f, wx, wz);
            const auto h = probeDown(wx, wz);
            check(h.hit && std::fabs(h.distance - (kDealershipDiscHeight + 2.0f - 0.11f)) < 0.02f,   // disc top -> slab + kerb
                  "K8 kerb: ray down onto the road-edge kerb lands on its top (slab + 0.15 m)");
        }
    }

    // ---- D: swing doors ----
    {
        const float doorX = -(S.halfDepth + 0.15f);
        auto rayIn = [&](float lz, float maxD) {   // +X ray through the doorway at chest height
            float ox, oz; d.toWorld(doorX - 2.7f, lz, ox, oz);
            float tx, tz; d.toWorld(doorX + 1.0f, lz, tx, tz);
            const float dx = tx - ox, dz = tz - oz, len = std::sqrt(dx * dx + dz * dz);
            return phys->rayCast(x3::phys::Vec3{ ox, 1.5f, oz }, x3::phys::Vec3{ dx / len, 0.0f, dz / len }, maxD,
                                 x3::phys::Layer::Static);
        };
        {
            const auto h = rayIn(0.6f, 4.0f);
            check(d.doorsClosed() && d.doorSwingDir() == 0 && h.hit && std::fabs(h.distance - 2.65f) < 0.08f,
                  "D1 doors: closed at rest; a ray through the doorway stops on a leaf");
        }
        x3::phys::Vec3 walker; { float wx, wz; d.toWorld(doorX - 1.3f, 0.3f, wx, wz); walker = { wx, 0.0f, wz }; }
        for (int i = 0; i < 6; ++i) d.update(1.0f / 60.0f, &walker);
        check(!d.doorsClosed() && d.doorSwingDir() == +1 && d.doorAngleDeg() > 0.0f && d.doorAngleDeg() < 45.0f,
              "D2 doors: approach from the forecourt starts an INWARD swing (away from the walker)");
        for (int i = 0; i < 80; ++i) d.update(1.0f / 60.0f, &walker);   // 1.43 s > kOpenS
        check(d.doorsOpen() && std::fabs(d.doorAngleDeg() - kDealershipDoorSwingDeg) < 1e-3f,
              "D3 doors: fully open (90 deg) after kDealershipDoorOpenS");
        phys->optimizeBroadphase();
        {
            const auto h = rayIn(0.6f, 6.0f);
            check(!h.hit, "D4 doors: the open pair lets a 6 m ray pass through the doorway");
        }
        d.update(1.0f / 60.0f, nullptr);
        check(d.doorsOpen(), "D5 doors: still open right after clearance (hold)");
        for (int i = 0; i < 60; ++i) d.update(1.0f / 60.0f, nullptr);    // 1.0 s: hold 0.8 + 0.2 closing
        check(!d.doorsOpen() && !d.doorsClosed(), "D6 doors: closing after the hold expires");
        for (int i = 0; i < 100; ++i) d.update(1.0f / 60.0f, nullptr);   // > kCloseS
        check(d.doorsClosed() && d.doorSwingDir() == 0, "D7 doors: closed again, swing direction released");
        {
            const auto h = rayIn(0.6f, 4.0f);
            check(h.hit && std::fabs(h.distance - 2.65f) < 0.08f, "D8 doors: the leaf collider is back on the closed line");
        }
        // Approach from INSIDE: the pair swings onto the forecourt.
        { float wx, wz; d.toWorld(doorX + 1.3f, -0.3f, wx, wz); walker = { wx, 0.0f, wz }; }
        for (int i = 0; i < 6; ++i) d.update(1.0f / 60.0f, &walker);
        check(!d.doorsClosed() && d.doorSwingDir() == -1, "D9 doors: approach from inside swings OUTWARD");
        for (int i = 0; i < 200; ++i) d.update(1.0f / 60.0f, nullptr);   // settle shut again
        // Frame-rate independence: two fresh instances, same approach, 0.6 s at 60 vs 30 Hz.
        {
            Dealership a, b;
            a.setGroundQuery([](float, float) { return 0.0f; });
            b.setGroundQuery([](float, float) { return 0.0f; });
            a.build(nullptr, *phys, ""); b.build(nullptr, *phys, "");
            a.onRegionBuild("city", *phys); b.onRegionBuild("city", *phys);
            float wx, wz; a.toWorld(doorX - 1.3f, 0.3f, wx, wz);
            const x3::phys::Vec3 w{ wx, 0.0f, wz };
            for (int i = 0; i < 36; ++i) a.update(1.0f / 60.0f, &w);
            for (int i = 0; i < 18; ++i) b.update(1.0f / 30.0f, &w);
            check(std::fabs(a.doorAngleDeg() - b.doorAngleDeg()) < 1e-3f && a.doorAngleDeg() > 5.0f && a.doorAngleDeg() < 85.0f,
                  "D10 doors: 60 Hz and 30 Hz reach the same mid-swing angle (dt-scaled, eased)");
            a.shutdown(*phys); b.shutdown(*phys);
        }
    }

    // ---- N: NPCs ----
    {
        check(d.npcCount() == 1 + kDealershipCustomers, "N1 npc: a salesperson + 4 customers");
        {
            const x3::phys::Vec3 f = d.npcFeet(0); float lx, lz; d.toLocal(f.x, f.z, lx, lz);
            check(lx > S.halfDepth - 1.4f + 0.55f && lx < S.halfDepth && std::fabs(lz) < 1.7f,
                  "N2 npc: the salesperson stands behind the desk");
        }
        bool inside = true, offDiscs = true, paced = true, walkedSome = false, doorsShut = true;
        std::vector<x3::phys::Vec3> start; for (uint32_t i = 0; i < d.npcCount(); ++i) start.push_back(d.npcFeet(i));
        float maxMove = 0.0f;
        for (int f = 0; f < 3600; ++f) {           // 60 s @ 60 Hz
            std::vector<x3::phys::Vec3> prev; for (uint32_t i = 0; i < d.npcCount(); ++i) prev.push_back(d.npcFeet(i));
            d.update(1.0f / 60.0f, nullptr);
            doorsShut = doorsShut && d.doorsClosed();
            for (uint32_t i = 0; i < d.npcCount(); ++i) {
                const x3::phys::Vec3 p = d.npcFeet(i); float lx, lz; d.toLocal(p.x, p.z, lx, lz);
                inside = inside && lx > -S.halfDepth && lx < S.halfDepth && std::fabs(lz) < S.halfWidth;
                for (uint32_t k = 0; k < kDealershipDiscs; ++k) {
                    float c[3]; d.discCenter(k, c);
                    offDiscs = offDiscs && std::hypot(p.x - c[0], p.z - c[2]) >= kDealershipDiscRadius + 0.4f;
                }
                const float step = std::hypot(p.x - prev[i].x, p.z - prev[i].z);
                paced = paced && step <= 1.0f / 60.0f + 1e-4f;
                walkedSome = walkedSome || d.npcWalking(i);
                maxMove = std::max(maxMove, std::hypot(p.x - start[i].x, p.z - start[i].z));
            }
        }
        check(inside, "N3 npc: 60 s of loitering never leaves the hall footprint");
        check(doorsShut, "N3b npc: browsing never trips the doors (paths stay outside the trigger)");
        check(offDiscs, "N4 npc: nobody walks onto a turntable");
        check(walkedSome && maxMove > 2.0f && paced, "N5 npc: customers browse (walk between points) at <= 1 m/s");
        // Talk: in front of the desk, no disc in reach.
        x3::phys::Vec3 tf; { float wx, wz; d.toWorld(S.halfDepth - 2.5f, 0.0f, wx, wz); tf = { wx, 0.0f, wz }; }
        d.update(kDealershipStatusS + 0.1f, nullptr);
        d.interact(tf, false);
        check(d.nearestDisc(tf.x, tf.z) < 0 && d.prompt() == "[E] Talk", "N6 talk: `[E] Talk` prompt at the desk");
        const bool t1 = d.interact(tf, true); const std::string l1 = d.status();
        const bool t2 = d.interact(tf, true); const std::string l2 = d.status();
        const bool t3 = d.interact(tf, true); const std::string l3 = d.status();
        d.interact(tf, true); const std::string l4 = d.status();
        check(t1 && t2 && t3 && l1.rfind("Salesperson:", 0) == 0 && l1 != l2 && l2 != l3 && l1 == l4 &&
              l1.find(std::to_string(d.trim(0).price)) != std::string::npos,
              "N7 talk: E consumes, three lines cycle, prices from the trim table");
        check(d.soldCount() == kDealershipForecourtSlots, "N8 talk: talking never sells");
        d.update(kDealershipStatusS + 0.1f, nullptr);
    }

    // ---- V: sold list persisted ----
    {
        const std::string j = d.soldJson();
        check(j.find("\"x3-dealership-1\"") != std::string::npos && j.find("\"slot\": 3") != std::string::npos &&
              j.find(d.trim(0).id) != std::string::npos, "V1 save: JSON carries the format tag + every slot");
        Dealership e; e.setGroundQuery([](float, float) { return 0.0f; });
        std::vector<WorldCarDef> got;
        e.setDeliverHook([&](const WorldCarDef& def) { got.push_back(def); return true; });
        e.build(nullptr, *phys, "");
        bool same = e.restoreSold(j) && e.soldCount() == d.soldCount() && got.size() == d.soldCount();
        for (uint32_t i = 0; same && i < e.soldCount(); ++i) {
            float sx, sz, sy; e.forecourtSlot(i, sx, sz, sy);
            same = e.soldTrim(i) == d.soldTrim(i) && std::fabs(got[i].x - sx) < 1e-3f && std::fabs(got[i].z - sz) < 1e-3f &&
                   std::fabs(got[i].tint[0] - d.trim(d.soldTrim(i)).tint[0]) < 1e-3f;
        }
        check(same, "V2 save: restoreSold re-delivers the same trims to the same slots in the same paint");
        e.shutdown(*phys);
        // File round trip with a partial list, then a purchase after the load APPENDS.
        namespace fs = std::filesystem;
        const std::string path = (fs::temp_directory_path() / "x3_dealership_selftest.json").string();
        {
            Dealership g; g.setGroundQuery([](float, float) { return 0.0f; });
            g.setDeliverHook([&](const WorldCarDef&) { return true; });
            g.build(nullptr, *phys, "");
            const std::string two = std::string("{ \"format\": \"x3-dealership-1\", \"sold\": ["
                                                "{ \"slot\": 1, \"trim\": \"") + d.trim(2).id + "\", \"tint\": [0.1, 0.2, 0.3] },"
                                                "{ \"slot\": 0, \"trim\": \"" + d.trim(3).id + "\", \"tint\": [0.4, 0.5, 0.6] } ] }";
            check(g.restoreSold(two) && g.soldCount() == 2 && g.soldTrim(0) == 3 && g.soldTrim(1) == 2,
                  "V3 save: out-of-order slots are restored in slot order");
            check(g.saveSoldFile(path), "V4 save: file written");
            g.shutdown(*phys);
        }
        {
            Dealership h; h.setGroundQuery([](float, float) { return 0.0f; });
            std::vector<WorldCarDef> got2;
            h.setDeliverHook([&](const WorldCarDef& def) { got2.push_back(def); return true; });
            vehparts::VehicleBuild w2; w2.credits = 1000000;
            h.setWallet(&w2);
            h.build(nullptr, *phys, "");
            check(h.loadSoldFile(path) && h.soldCount() == 2 && got2.size() == 2 && h.soldTrim(1) == 2 &&
                  std::fabs(got2[1].tint[2] - 0.3f) < 1e-3f, "V5 load: file round trip restores 2 cars + tints");
            h.onRegionBuild("city", *phys);
            const bool sold = h.interact(feet, true);           // disc 0, from the P tests
            float sx, sz, sy; h.forecourtSlot(2, sx, sz, sy);
            check(sold && h.soldCount() == 3 && got2.size() == 3 && std::fabs(got2[2].x - sx) < 1e-3f &&
                  std::fabs(got2[2].z - sz) < 1e-3f, "V6 load: buying after a load appends to the next free slot");
            Dealership k; k.setGroundQuery([](float, float) { return 0.0f; });
            k.setDeliverHook([&](const WorldCarDef&) { return true; });
            k.build(nullptr, *phys, "");
            check(k.loadSoldFile(path) && k.soldCount() == 3 && k.soldTrim(2) == 0,
                  "V7 load: the purchase was written straight back to the file");
            check(!k.restoreSold("{ nonsense") && k.soldCount() == 3, "V8 load: malformed JSON refused, list untouched");
            Dealership m; m.setGroundQuery([](float, float) { return 0.0f; });
            m.setDeliverHook([&](const WorldCarDef&) { return true; });
            m.build(nullptr, *phys, "");
            check(m.loadSoldFile(path + ".missing") && m.soldCount() == 0, "V9 load: missing file == nothing sold (not an error)");
            m.shutdown(*phys);
            k.shutdown(*phys);
            h.onRegionTeardown("city", *phys); h.shutdown(*phys);
        }
        std::error_code ec; fs::remove(path, ec);
    }

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
    auto still = [&](const char* name, float cx, float cy, float cz, float tx, float ty, float tz, float fov,
                     const x3::phys::Vec3* approacher = nullptr, int approachFrom = 0) {
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
            d.update(1.0f / 60.0f, (approacher && i >= approachFrom) ? approacher : nullptr);
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
    // 4. Doorway from the forecourt: an approacher trips the pair from frame 57
    //    (0.55 s of the 1.1 s swing => ~45 deg), customers visible through it.
    {
        float ex, ez, tx, tz, ax, az;
        d.toWorld(-15.5f, 2.8f, ex, ez);
        d.toWorld(-(S.halfDepth + 0.15f), 0.0f, tx, tz);
        d.toWorld(-(S.halfDepth + 0.15f) - 1.3f, 0.3f, ax, az);
        const x3::phys::Vec3 walker{ ax, 0.0f, az };
        still("dealership_doors_open", ex, 1.65f, ez, tx, 1.5f, tz, 52.0f, &walker, 90 - 33);
    }

    device->destroyMesh(pad); device->destroyMesh(road);
    device->destroyTexture(groundMr); device->destroyTexture(asphaltMr);
    d.shutdown(*phys);
    phys->shutdown();
    return fails == 0 ? 0 : 1;
}

} // namespace x3::game
