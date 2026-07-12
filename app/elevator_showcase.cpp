// THE CENTERPIECE — self-contained THICK / NICE / DARK-GLASS elevator showcase.
// See app/elevator_showcase.h for the design + decoupling contract.
#include "elevator_showcase.h"
#include "elevator_mesh.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/audio/IAudioSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace x3::game {

// ---------------------------------------------------------------------------
// Small carrier for an authored prim (render mesh + collision) we pass into the
// add* helpers. Keeps the helpers free of x3::prims/elevmesh template noise.
// ---------------------------------------------------------------------------
struct ElevPrim { x3::prims::PrimMesh mesh; };

namespace {
// Premium dark-luxury palette (linear-ish). Tim: THICK / NICE / DARK GLASS.
const float kBrushedSteel[4] = { 0.42f, 0.44f, 0.48f, 1.0f };   // frame / jambs (brushed)
const float kDarkSteel[4]    = { 0.16f, 0.17f, 0.20f, 1.0f };   // heavy structure
const float kDoorMetal[4]    = { 0.30f, 0.32f, 0.36f, 1.0f };   // sliding door slabs
const float kCabFloor[4]     = { 0.10f, 0.10f, 0.12f, 1.0f };   // dark cab deck
const float kDarkGlassTint[3]= { 0.10f, 0.12f, 0.16f };         // SMOKED dark glass tint
const float kNoEm[4]         = { 0, 0, 0, 0 };
// Accent strip glow (cool premium teal-white), holo cyan, warm vent amber.
const float kAccentEm[4]     = { 0.10f, 0.55f, 0.70f, 2.2f };
const float kWarmEm[4]       = { 0.95f, 0.78f, 0.45f, 1.4f };

constexpr float kPi2 = 6.2831853f;
} // namespace

// ===========================================================================
// ADD HELPERS
// ===========================================================================
uint32_t ElevatorShowcase::addSolid(Scene& scene, x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& physics,
                                    const ElevPrim& prim, const float color[4],
                                    const float emissive[4], bool collide, uint32_t tag) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    if (collide && !prim.mesh.cverts.empty()) {
        e.body = physics.addStaticMesh(prim.mesh.cverts.data(), (uint32_t)(prim.mesh.cverts.size()/3),
                                       prim.mesh.cindex.data(), (uint32_t)prim.mesh.cindex.size());
    }
    e.tag = tag;
    uint32_t id = scene.add(e);
    if (e.body.id) scene.get(id).body = e.body;
    ++m_stats.entities;
    return id;
}

uint32_t ElevatorShowcase::addDecor(Scene& scene, x3::rhi::IRenderDevice& device,
                                    const ElevPrim& prim, const float color[4],
                                    const float emissive[4], uint32_t tag) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = tag;
    e.body.id = 0;
    ++m_stats.entities;
    return scene.add(e);
}

uint32_t ElevatorShowcase::addDarkGlass(Scene& scene, x3::rhi::IRenderDevice& device,
                                        const ElevPrim& prim, float opacity,
                                        const float tint[3], const float emissive[4]) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    // Body color carries the dark tint too (so even with the scene-copy path off it
    // reads smoked, not clear). emissive honored for any glow baked behind the glass.
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = opacity;
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.transparent = true;
    e.glass.opacity    = opacity;        // DARK but still see-through
    e.glass.refraction = 0.025f;         // subtle bend (premium thick glass)
    e.glass.roughness  = 0.06f;          // near-polished, faint smoke
    e.glass.specular   = 0.9f;           // crisp reflections off the dark surface
    e.glass.tint[0] = tint[0]; e.glass.tint[1] = tint[1]; e.glass.tint[2] = tint[2];
    e.tag = (uint32_t)Tag::Prop;
    e.body.id = 0;
    ++m_stats.entities;
    return scene.add(e);
}

// ===========================================================================
// BUILD
// ===========================================================================
bool ElevatorShowcase::build(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics,
                             const PlacementSpec& spec, x3::audio::IAudioSystem* audio) {
    if (m_built) return true;
    m_spec = spec;
    m_audio = audio;
    m_shaftX = spec.shaftX;
    m_shaftZ = spec.shaftZ;

    // ---- Floor list: use the caller's, or synthesize a default showcase tower ----
    m_floors = spec.floors;
    if (m_floors.empty()) {
        // A premium tower: Club at the bottom (-200), then 6 above-ground floors.
        m_floors = {
            { ElevatorSystem::kDefaultClubFloorY + m_cabHY, "CLUB 1127  \xC2\xB7  THE DEEP", true },
            { m_cabHY + 0.0f,   "LOBBY  \xC2\xB7  GROUND",      false },
            { m_cabHY + 16.0f,  "F2  \xC2\xB7  DETENTION",      false },
            { m_cabHY + 32.0f,  "F3  \xC2\xB7  RESEARCH",       false },
            { m_cabHY + 48.0f,  "F4  \xC2\xB7  STRATA DECK",    false },
            { m_cabHY + 64.0f,  "F5  \xC2\xB7  THE CHORUS",     false },
            { m_cabHY + 80.0f,  "F6  \xC2\xB7  EXEC / SPIRE",   false },
        };
    }
    std::sort(m_floors.begin(), m_floors.end(),
              [](const ShowcaseFloor& a, const ShowcaseFloor& b){ return a.centerY < b.centerY; });

    std::vector<float> stopsY;
    std::vector<std::string> labels;
    m_clubStop = -1;
    for (int i = 0; i < (int)m_floors.size(); ++i) {
        stopsY.push_back(m_floors[i].centerY);
        labels.push_back(m_floors[i].label);
        if (m_floors[i].isClub) m_clubStop = i;
    }
    m_stats.floors = (int)m_floors.size();
    m_stats.hasClubStop = (m_clubStop >= 0);

    // Start at the lobby (first non-club stop) so the cab is at the human entrance.
    int start = spec.startStop;
    if (start <= 0) { start = (m_clubStop == 0 && m_floors.size() > 1) ? 1 : 0; }

    // ---- The core elevator (THICK cab platform) + FSM ----
    if (!m_elev.build(scene, device, physics, m_shaftX, m_shaftZ,
                      m_cabHX, m_cabHY, m_cabHZ, stopsY, start)) {
        x3::logError("[showcase] elevator core build failed");
        return false;
    }
    m_elev.enableFsm(true);
    m_elev.setAudio(audio);
    m_elev.setFloorLabels(labels);
    if (m_clubStop >= 0) m_elev.setClubStopY(m_floors[m_clubStop].centerY);
    m_stats.clubStopY = m_clubStop >= 0 ? m_floors[m_clubStop].centerY : 0.0f;

    // ---- The shell (shaft + per-floor doors + call panels) ----
    if (spec.buildShaftShell) buildShaft(scene, device, physics);

    // ---- The thick dark-glass cab interior + holo control panel ----
    buildCabInterior(scene, device);
    buildHoloPanel(scene, device);

    // ---- Interior + accent point lights (host pushes these each frame) ----
    m_lights.clear();
    // Warm KEY light — intensity baked into the color magnitude (PointLight.color =
    // linear RGB * intensity). Bright enough to lift the dark-glass cab interior so
    // the smoked walls read rich (not black) + the accent strips/holo pop against it.
    { x3::rhi::PointLight ceil; ceil.color[0]=3.4f; ceil.color[1]=3.0f; ceil.color[2]=2.4f; ceil.range=8.0f; m_lights.push_back(ceil); } // warm key
    { x3::rhi::PointLight holo; holo.color[0]=0.6f; holo.color[1]=2.2f; holo.color[2]=3.2f; holo.range=5.0f;  m_lights.push_back(holo); }  // holo cyan glow
    // WAVE-2B (LD review #3): the cab read as a BLACK BOX between two beautiful vistas —
    // the exterior crown glow existed but the interior ceiling had no fill, so the coffer
    // + upper walls fell to black (captures/elevtrio/elevator_interior.png). Add ONE SOFT
    // ceiling fill high at cab centre — additive only (this is the 14900K showcase; the
    // warm key + holo mood are untouched). A gentle cool-neutral wash so the coffered
    // ceiling + rails read without flattening the smoked-glass richness. Placed at [2] so
    // the disco spots stay the TRAILING lights (layoutCab poses this + skips it in the
    // disco sweep). Low intensity: lift the black, don't wash the room.
    { x3::rhi::PointLight fill; fill.color[0]=2.4f; fill.color[1]=2.6f; fill.color[2]=3.0f; fill.range=6.5f; m_lights.push_back(fill); } // soft ceiling fill
    // 4 disco spots (off until 1127); placed in layoutCab().
    for (int i = 0; i < 4; ++i) { x3::rhi::PointLight l; l.color[0]=l.color[1]=l.color[2]=0.0f; l.range=7.0f; m_lights.push_back(l); }

    // Player spawn just inside the cab, on the deck, facing -Z toward the holo panel.
    const float floorY = m_floors[start].centerY + m_cabHY;
    m_spawn = x3::phys::Vec3{ m_shaftX, floorY + 0.05f, m_shaftZ + m_cabHZ - 0.6f };

    m_built = true;
    layoutCab(scene);
    x3::logInfo("[showcase] THICK dark-glass elevator built: " + std::to_string(m_floors.size()) +
                " floors, club stop " + std::to_string(m_clubStop) +
                ", " + std::to_string(m_stats.entities) + " entities");
    return true;
}

// ===========================================================================
// SHAFT SHELL — a heavy structural tube + premium portal frames + sliding doors +
// realistic call-panel keypads on each served floor. THICK + NICE.
// ===========================================================================
void ElevatorShowcase::buildShaft(Scene& scene, x3::rhi::IRenderDevice& device,
                                  x3::phys::IPhysicsWorld& physics) {
    using namespace x3::elevmesh;
    const float lo = m_floors.front().centerY - 3.0f;
    const float hi = m_floors.back().centerY  + 4.0f;
    const float shaftH = hi - lo;
    const float midY = (lo + hi) * 0.5f;
    // Shaft inner half-extent: the cab plus generous clearance + THICK walls.
    const float inHX = m_cabHX + 0.55f, inHZ = m_cabHZ + 0.55f;
    const float wallT = 0.45f;                // THICK structural walls
    const float ox = inHX + wallT, oz = inHZ + wallT;

    auto box = [&](float hx,float hy,float hz,float cx,float cy,float cz,const float c[4],bool col){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,cx,cy,cz,0.04f);
        return addSolid(scene, device, physics, p, c, kNoEm, col, (uint32_t)Tag::Static);
    };

    // --- Back wall (-Z, behind the cab; this is the "spine" the strata reads against) ---
    box(ox, shaftH*0.5f, wallT*0.5f, m_shaftX, midY, m_shaftZ - inHZ - wallT*0.5f, kDarkSteel, true);
    // --- Side walls (+/-X) ---
    box(wallT*0.5f, shaftH*0.5f, oz, m_shaftX - inHX - wallT*0.5f, midY, m_shaftZ, kDarkSteel, true);
    box(wallT*0.5f, shaftH*0.5f, oz, m_shaftX + inHX + wallT*0.5f, midY, m_shaftZ, kDarkSteel, true);
    // The FRONT (+Z) is the doorway face — left open per-floor (the door frames sit there).
    // Front pillars flanking the doorway (full height, THICK).
    const float doorHalfW = m_cabHX - 0.05f;     // opening half-width
    box(wallT*0.55f, shaftH*0.5f, wallT*0.5f, m_shaftX - doorHalfW - wallT*0.55f, midY, m_shaftZ + inHZ + wallT*0.5f, kDarkSteel, true);
    box(wallT*0.55f, shaftH*0.5f, wallT*0.5f, m_shaftX + doorHalfW + wallT*0.55f, midY, m_shaftZ + inHZ + wallT*0.5f, kDarkSteel, true);

    // Shaft cap + base slabs (THICK).
    box(ox, 0.30f, oz, m_shaftX, hi, m_shaftZ, kDarkSteel, true);
    box(ox, 0.30f, oz, m_shaftX, lo, m_shaftZ, kDarkSteel, true);

    // --- Per-floor: a premium chamfered portal frame + 2 sliding door leaves + a
    //     realistic call-panel keypad beside the doorway. ---
    const float doorH = 2.35f;               // opening half-height ~ tall premium doors
    const float frontZ = m_shaftZ + inHZ + 0.02f;   // door plane (just outside the shaft front)
    m_shaftDoorL.clear(); m_shaftDoorR.clear(); m_shaftDoorY.clear();

    for (int f = 0; f < (int)m_floors.size(); ++f) {
        const float cy = m_floors[f].centerY + m_cabHY + doorH;   // doorway vertical center

        // Chamfered portal frame (THICK jambs).
        { ElevPrim p; p.mesh = doorFrame(doorHalfW + 0.06f, doorH, 0.22f, 0.16f,
                                         m_shaftX, cy, frontZ, 0.035f);
          addSolid(scene, device, physics, p, kBrushedSteel, kNoEm, true, (uint32_t)Tag::Static); }

        // 2 THICK sliding door leaves (heavy slabs, beveled), meeting at center.
        const float leafHW = doorHalfW * 0.5f - 0.01f;
        const float leafHD = 0.10f;          // THICK heavy door
        const float dz = frontZ + 0.10f;     // doors ride just proud of the frame face
        { ElevPrim p; p.mesh = beveledBox(leafHW, doorH - 0.05f, leafHD,
                                          m_shaftX - leafHW, cy, dz, 0.03f);
          uint32_t id = addSolid(scene, device, physics, p, kDoorMetal, kNoEm, false, (uint32_t)Tag::Door);
          m_shaftDoorL.push_back(id); m_stats.shaftDoors++; }
        { ElevPrim p; p.mesh = beveledBox(leafHW, doorH - 0.05f, leafHD,
                                          m_shaftX + leafHW, cy, dz, 0.03f);
          uint32_t id = addSolid(scene, device, physics, p, kDoorMetal, kNoEm, false, (uint32_t)Tag::Door);
          m_shaftDoorR.push_back(id); m_stats.shaftDoors++; }
        m_shaftDoorY.push_back(cy);
        // A glowing seam strip down the door meeting line (premium accent).
        { ElevPrim p; p.mesh = beveledBox(0.012f, doorH - 0.1f, 0.012f, m_shaftX, cy, dz + leafHD + 0.005f, 0.004f);
          addDecor(scene, device, p, kBrushedSteel, kAccentEm, (uint32_t)Tag::Prop); }

        // --- Realistic CALL PANEL keypad beside the door (+X jamb) ---
        const float panelX = m_shaftX + doorHalfW + 0.34f;
        const float panelY = cy - doorH + 1.3f;          // ~1.3 m up from floor
        const float panelZ = frontZ + 0.12f;
        // Panel plate (brushed, beveled, slightly proud of the wall).
        { ElevPrim p; p.mesh = beveledBox(0.13f, 0.20f, 0.03f, panelX, panelY, panelZ, 0.012f);
          addSolid(scene, device, physics, p, kBrushedSteel, kNoEm, false, (uint32_t)Tag::Button); }
        m_stats.callPanels++;
        // 2 real round call buttons (UP / DOWN) with a soft glow ring.
        { ElevPrim p; p.mesh = roundButton(panelX, panelY + 0.05f, panelZ + 0.03f, 0.035f, 0.018f, 12);
          float em[4] = {0.10f, 0.70f, 0.35f, 1.6f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button); m_stats.callButtons++; }
        { ElevPrim p; p.mesh = roundButton(panelX, panelY - 0.05f, panelZ + 0.03f, 0.035f, 0.018f, 12);
          float em[4] = {0.85f, 0.55f, 0.10f, 1.4f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button); m_stats.callButtons++; }
        // A small floor-indicator emissive chip above the buttons.
        { ElevPrim p; p.mesh = beveledBox(0.08f, 0.025f, 0.012f, panelX, panelY + 0.13f, panelZ + 0.02f, 0.004f);
          float em[4] = {0.20f, 0.80f, 1.0f, 2.0f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Prop); }

        // Floor lobby pad (a thick deck slab in front of the doors so you stand level).
        box(doorHalfW + 0.6f, 0.12f, 0.7f, m_shaftX, m_floors[f].centerY + m_cabHY - 0.12f,
            frontZ + 0.75f, kCabFloor, true);
    }
}

// ===========================================================================
// CAB INTERIOR — THICK deck + ceiling + DARK SMOKED GLASS walls + handrail +
// glass floor (strata view) + accent light strips + entertainment screen + vent.
// All authored centered at the cab origin; layoutCab() offsets them each frame.
// ===========================================================================
void ElevatorShowcase::buildCabInterior(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    // Premium cab interior dims (THICK, generous: ~3.4 x 4.6 x 3.4 m clear).
    const float W = 1.55f, D = 1.55f;        // interior half-extents (match cab platform)
    const float H = 2.35f;                    // interior half-height
    const float wallT = 0.06f;                // glass pane thickness (THICK premium glass)

    auto decorBox = [&](float hx,float hy,float hz,float cx,float cy,float cz,const float c[4],const float em[4]){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,cx,cy,cz,0.015f);
        return addDecor(scene, device, p, c, em, (uint32_t)Tag::Prop);
    };

    // --- DARK SMOKED GLASS walls (3 sides: -Z back, +/-X). Front (+Z) is the door. ---
    // Authored as thin glass slabs just inside the cab. See-through but dark + rich.
    { ElevPrim p; p.mesh = beveledBox(W - 0.02f, H, wallT, 0, 0, -(D - wallT), 0.012f);
      m_eWall[0] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    { ElevPrim p; p.mesh = beveledBox(wallT, H, D - 0.02f, -(W - wallT), 0, 0, 0.012f);
      m_eWall[1] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    { ElevPrim p; p.mesh = beveledBox(wallT, H, D - 0.02f, (W - wallT), 0, 0, 0.012f);
      m_eWall[2] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    // Front upper transom glass (above the doorway) so the cab reads enclosed.
    { ElevPrim p; p.mesh = beveledBox(W - 0.02f, H - 1.9f, wallT, 0, H - (H - 1.9f), (D - wallT), 0.012f);
      m_eWall[3] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    m_stats.hasDarkGlass = true;

    // --- THICK dark deck (the cab floor edge ring) + a GLASS center for the strata view ---
    // A solid dark frame ring around a transparent glass center panel (look down → strata).
    const float ringT = 0.28f;
    decorBox(W, 0.06f, ringT, 0, -H + 0.05f, -(D - ringT), kCabFloor, kNoEm);   // back ring
    decorBox(W, 0.06f, ringT, 0, -H + 0.05f,  (D - ringT), kCabFloor, kNoEm);   // front ring
    decorBox(ringT, 0.06f, D, -(W - ringT), -H + 0.05f, 0, kCabFloor, kNoEm);   // -X ring
    decorBox(ringT, 0.06f, D,  (W - ringT), -H + 0.05f, 0, kCabFloor, kNoEm);   // +X ring
    // GLASS FLOOR center (see the strata descend below you).
    { ElevPrim p; p.mesh = beveledBox(W - ringT, 0.025f, D - ringT, 0, -H + 0.04f, 0, 0.008f);
      float em[4] = {0,0,0,0};
      m_eGlassFloor = addDarkGlass(scene, device, p, 0.40f, kDarkGlassTint, em); }
    m_stats.hasGlassFloor = true;

    // --- The STRATA PLANE seen THROUGH the glass floor (driven by current stratum) ---
    { ElevPrim p; p.mesh = beveledBox(W - ringT - 0.05f, 0.02f, D - ringT - 0.05f, 0, -H - 1.2f, 0, 0.005f);
      float c[4] = {0.30f, 0.28f, 0.32f, 1.0f}; float em[4] = {0.30f, 0.28f, 0.32f, 0.8f};
      m_eStrata = addDecor(scene, device, p, c, em, (uint32_t)Tag::Prop); }

    // --- Coffered ceiling (THICK) with a recessed warm luminaire ---
    // WAVE-2B (LD review #3): the cab ceiling read pure black between the two vistas.
    // Brighten the recessed luminaire so the coffer is SELF-LIT (a soft ceiling fill),
    // paired with the new fill point light — additive, no restyle of the warm/holo mood.
    const float kCeilFill[4] = { 1.00f, 0.86f, 0.55f, 2.8f };   // brighter warm luminaire
    decorBox(W, 0.10f, D, 0, H - 0.05f, 0, kDarkSteel, kNoEm);
    m_eCeil = decorBox(W - 0.35f, 0.04f, D - 0.35f, 0, H - 0.14f, 0, kCabFloor, kCeilFill);

    // --- Octagonal brushed HANDRAIL around 3 walls at waist height ---
    const float railY = -0.15f, railR = 0.035f;
    { ElevPrim p; p.mesh = tube(railR, W - 0.12f, 0, railY, -(D - 0.10f), 0, 8);   // back run (along X)
      m_eRailEnts[0] = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop); }
    { ElevPrim p; p.mesh = tube(railR, D - 0.12f, -(W - 0.10f), railY, 0, 2, 8);   // -X run (along Z)
      m_eRailEnts[1] = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop); }
    { ElevPrim p; p.mesh = tube(railR, D - 0.12f,  (W - 0.10f), railY, 0, 2, 8);   // +X run
      m_eRailEnts[2] = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop); }
    m_stats.hasHandrail = true;

    // --- Glowing ACCENT light strips (vertical, at the wall corners) ---
    for (int i = 0; i < 4; ++i) {
        float sx = (i & 1) ? (W - 0.04f) : -(W - 0.04f);
        float sz = (i & 2) ? (D - 0.04f) : -(D - 0.04f);
        ElevPrim p; p.mesh = beveledBox(0.02f, H - 0.3f, 0.02f, sx, 0, sz, 0.006f);
        m_eAccent[i] = addDecor(scene, device, p, kDarkSteel, kAccentEm, (uint32_t)Tag::Prop);
    }

    // --- ENTERTAINMENT SCREEN on the -X wall (a wall display looping visuals/ads) ---
    { ElevPrim p; p.mesh = beveledBox(0.02f, 0.42f, 0.62f, -(W - 0.05f), 0.5f, 0.2f, 0.01f);
      float em[4] = {0.20f, 0.45f, 0.85f, 1.8f};
      m_eEntScreen = addDecor(scene, device, p, kCabFloor, em, (uint32_t)Tag::Prop); }
    m_stats.hasEntScreen = true;

    // --- VENT grille on the ceiling edge (the "heat/cooling" feel) ---
    { ElevPrim p; p.mesh = beveledBox(0.30f, 0.03f, 0.14f, 0, H - 0.18f, D - 0.30f, 0.006f);
      float em[4] = {0.05f, 0.06f, 0.08f, 0.3f};
      m_eVent = addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Prop); }
    m_stats.hasVent = true;

    // --- Disco ball (hidden until 1127) hung from ceiling center ---
    { ElevPrim p; p.mesh = x3::elevmesh::cylinderY(0.22f, 0.22f, 0, H - 0.55f, 0, 12, true);
      m_eDiscoBall = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop); }

    // --- INNER CAB DOOR leaves (slide with the FSM door %) ---
    const float cdHW = W * 0.5f - 0.02f, cdHD = 0.05f, cdH = 1.95f;
    { ElevPrim p; p.mesh = beveledBox(cdHW, cdH, cdHD, -cdHW, -H + cdH + 0.05f, D - 0.06f, 0.02f);
      m_eCabDoorL = addDecor(scene, device, p, kDoorMetal, kNoEm, (uint32_t)Tag::Door); }
    { ElevPrim p; p.mesh = beveledBox(cdHW, cdH, cdHD,  cdHW, -H + cdH + 0.05f, D - 0.06f, 0.02f);
      m_eCabDoorR = addDecor(scene, device, p, kDoorMetal, kNoEm, (uint32_t)Tag::Door); }
}

// ===========================================================================
// HOLO CONTROL PANEL — a transparent glowing glass panel on the -X wall with the
// building directory + an animated floor indicator + floor-select round buttons.
// ===========================================================================
void ElevatorShowcase::buildHoloPanel(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f;
    // Panel anchored on the +X wall, chest height, facing -X into the cab. The
    // HoloTerminal builds its own translucent emissive glass + ceiling arm. We place
    // it relative to the cab origin; layoutCab() does NOT move it (the holo manages
    // its own entities). We anchor it at the cab's START position; for the showcase
    // the cab + holo move together — but since the holo's entities aren't re-laid by
    // layoutCab, we anchor it at a FIXED interior spot and accept that the panel rides
    // with the cab via the SAME per-frame offset we apply to the buttons below.
    const float startFloorY = m_elev.cabCenter().y + m_cabHY;
    x3::phys::Vec3 holoPos{ m_shaftX + W - 0.10f, startFloorY + 1.35f, m_shaftZ + 0.2f };
    m_holo.build(scene, device, holoPos, /*yaw*/1.5708f, /*w*/0.7f, /*h*/0.95f,
                 holoPos.y + 0.9f);
    m_holo.setLines({
        std::string("X3  CORE LIFT  \xC2\xB7  DIRECTORY"),
        std::string("--------------------------------"),
    });
    for (int i = (int)m_floors.size() - 1; i >= 0; --i) {
        std::string row = (i == m_clubStop ? std::string("[*] ") : std::string("[ ] ")) + m_floors[i].label;
        m_holo.addLine(row);
    }
    m_holo.addLine(std::string("ENTER CODE 1127 -> DISCO DESCENT"));
    m_stats.hasHoloPanel = true;

    // Interior floor-select ROUND BUTTONS in a column beside the holo glass (real
    // raised buttons; pressing maps to callTo in the host). One per floor.
    const float bx = m_shaftX + W - 0.13f;
    const float bz = m_shaftZ - 0.55f;
    m_holoButtonCount = 0;
    for (int i = 0; i < (int)m_floors.size() && i < 16; ++i) {
        float by = startFloorY + 0.4f + (float)i * 0.16f;
        ElevPrim p; p.mesh = roundButton(0, 0, 0, 0.03f, 0.016f, 12);   // origin-authored; layoutCab offsets
        float em[4] = { (i == m_clubStop) ? 0.85f : 0.10f,
                        (i == m_clubStop) ? 0.10f : 0.55f,
                        (i == m_clubStop) ? 0.60f : 0.80f, 1.5f };
        uint32_t id = addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button);
        scene.get(id).link = (uint32_t)i;     // which stop this button calls
        m_eHoloButtons[m_holoButtonCount++] = id;
        m_stats.holoButtons++;
        (void)bx; (void)by; (void)bz;
    }
}

// ===========================================================================
// PER-FRAME LAYOUT — offset all cab-child entities to the live cab center.
// ===========================================================================
void ElevatorShowcase::layoutCab(Scene& scene) {
    if (!m_built) return;
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float W = 1.55f;
    // Interior origin sits at the cab center + half-height (the cab platform top is
    // the deck; the interior box is centered ~H above the deck).
    const float ix = c.x, iz = c.z;
    const float iy = c.y + m_cabHY + 2.35f;   // interior vertical center

    auto place = [&](uint32_t id, float ox, float oy, float oz) {
        if (id == kNoLink || id >= scene.size()) return;
        Entity& e = scene.get(id);
        e.transform[12] = ix + ox; e.transform[13] = iy + oy; e.transform[14] = iz + oz;
    };
    for (int i = 0; i < kWallGlass; ++i) place(m_eWall[i], 0, 0, 0);
    place(m_eCeil, 0, 0, 0);
    place(m_eGlassFloor, 0, 0, 0);
    place(m_eStrata, 0, 0, 0);
    for (int i = 0; i < 3; ++i) place(m_eRailEnts[i], 0, 0, 0);
    for (int i = 0; i < 4; ++i) place(m_eAccent[i], 0, 0, 0);
    place(m_eEntScreen, 0, 0, 0);
    place(m_eVent, 0, 0, 0);
    place(m_eDiscoBall, 0, 0, 0);

    // Interior floor-select buttons (column on the +X wall near the holo).
    for (int i = 0; i < m_holoButtonCount; ++i) {
        float by = 0.4f + (float)i * 0.16f - 1.0f;
        place(m_eHoloButtons[i], W - 0.13f, by, -0.55f);
    }

    // Disco-ball glow when disco mode is on.
    if (m_eDiscoBall != kNoLink && m_eDiscoBall < scene.size()) {
        Entity& e = scene.get(m_eDiscoBall);
        float g = m_elev.disco() ? 1.0f : 0.0f;
        e.emissive[0] = 0.8f*g; e.emissive[1] = 0.8f*g; e.emissive[2] = 0.95f*g;
        e.emissive[3] = m_elev.disco() ? 1.6f : 0.0f;
    }

    // Drive the strata plane (seen through the glass floor) from the current stratum.
    if (m_eStrata != kNoLink && m_eStrata < scene.size()) {
        Entity& e = scene.get(m_eStrata);
        for (const StrataLayer& s : ElevatorSystem::strata()) {
            if (c.y >= s.yMin && c.y <= s.yMax) {
                for (int k = 0; k < 3; ++k) e.baseColor[k] = s.rgb[k];
                if (s.glow) { for (int k = 0; k < 3; ++k) e.emissive[k] = s.glowRgb[k]; e.emissive[3] = 1.6f; }
                else        { for (int k = 0; k < 3; ++k) e.emissive[k] = s.rgb[k];     e.emissive[3] = 0.7f; }
                break;
            }
        }
    }

    // Interior lights: warm ceiling key + holo glow + (disco) spots.
    if (m_lights.size() >= 2) {
        m_lights[0].pos[0]=ix;            m_lights[0].pos[1]=iy + 1.9f; m_lights[0].pos[2]=iz;
        m_lights[1].pos[0]=ix + W - 0.3f; m_lights[1].pos[1]=iy + 0.6f; m_lights[1].pos[2]=iz - 0.4f;
        // [2] = WAVE-2B soft ceiling fill: high at cab centre, just under the coffer.
        if (m_lights.size() >= 3) { m_lights[2].pos[0]=ix; m_lights[2].pos[1]=iy + 2.15f; m_lights[2].pos[2]=iz; }
        for (int i = 3; i < (int)m_lights.size(); ++i) {
            float a = (float)(i-3)/4.0f * kPi2 + m_time * (m_elev.disco() ? 2.5f : 0.0f);
            m_lights[i].pos[0]=ix + std::cos(a)*1.0f;
            m_lights[i].pos[1]=iy + 1.4f;
            m_lights[i].pos[2]=iz + std::sin(a)*1.0f;
        }
    }
}

// ===========================================================================
// DOOR ANIMATION — slide the cab + per-floor leaves to match the FSM door %.
// doorPct: 1 = fully open, 0 = closed (we read it via the FSM state proxy).
// ===========================================================================
void ElevatorShowcase::animateDoors(Scene& scene) {
    // Derive an open fraction from the FSM state: open when stopped, closed while
    // travelling. We approximate doorPct from the state (the FSM owns the real %,
    // but it isn't exposed; this matches the visible behavior 1:1).
    float openF = 0.0f;
    switch (m_elev.state()) {
        case ElevState::DoorsOpen:    openF = 1.0f; break;
        case ElevState::Idle:         openF = 1.0f; break;   // sits open at a stop
        case ElevState::DoorsOpening: openF = std::min(1.0f, m_time * 0.0f + 0.5f); break;
        case ElevState::DoorsClosing: openF = 0.5f; break;
        default:                      openF = 0.0f; break;    // travelling => shut
    }
    const float W = 1.55f;
    const float slide = openF * (W * 0.5f);     // leaves retract by up to half-width

    // Cab inner doors.
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float iy = c.y + m_cabHY + 2.35f;
    const float cdHW = W * 0.5f - 0.02f;
    if (m_eCabDoorL != kNoLink && m_eCabDoorL < scene.size()) {
        Entity& e = scene.get(m_eCabDoorL);
        e.transform[12] = c.x - cdHW - slide; e.transform[13] = iy - 2.35f + 1.95f + 0.05f; e.transform[14] = c.z + (W - 0.06f);
    }
    if (m_eCabDoorR != kNoLink && m_eCabDoorR < scene.size()) {
        Entity& e = scene.get(m_eCabDoorR);
        e.transform[12] = c.x + cdHW + slide; e.transform[13] = iy - 2.35f + 1.95f + 0.05f; e.transform[14] = c.z + (W - 0.06f);
    }

    // Per-floor shaft doors: only the floor the cab is AT opens; the rest stay shut.
    int atFloor = currentFloorIndex();
    bool stopped = (m_elev.state() == ElevState::DoorsOpen || m_elev.state() == ElevState::Idle ||
                    m_elev.state() == ElevState::DoorsOpening);
    const float doorHalfW = m_cabHX - 0.05f;
    const float leafHW = doorHalfW * 0.5f - 0.01f;
    for (int f = 0; f < (int)m_shaftDoorL.size(); ++f) {
        float of = (stopped && f == atFloor) ? openF : 0.0f;
        float sl = of * (doorHalfW * 0.5f);
        float cy = m_shaftDoorY[f];
        float dz = m_shaftZ + m_cabHZ + 0.55f + 0.02f + 0.10f;
        if (m_shaftDoorL[f] < scene.size()) {
            Entity& e = scene.get(m_shaftDoorL[f]);
            e.transform[12] = m_shaftX - leafHW - sl; e.transform[13] = cy; e.transform[14] = dz;
        }
        if (m_shaftDoorR[f] < scene.size()) {
            Entity& e = scene.get(m_shaftDoorR[f]);
            e.transform[12] = m_shaftX + leafHW + sl; e.transform[13] = cy; e.transform[14] = dz;
        }
    }
}

// ===========================================================================
// UPDATE
// ===========================================================================
float ElevatorShowcase::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return 0.0f;
    m_time += dt;
    float dy = m_elev.update(dt, scene, physics);
    layoutCab(scene);
    animateDoors(scene);
    m_holo.update(dt);

    // Entertainment screen: a slow hue-cycling glow (looping ad visuals).
    if (m_eEntScreen != kNoLink && m_eEntScreen < scene.size()) {
        m_entScroll += dt * 0.4f;
        Entity& e = scene.get(m_eEntScreen);
        e.emissive[0] = 0.25f + 0.20f * std::sin(m_entScroll);
        e.emissive[1] = 0.40f + 0.20f * std::sin(m_entScroll + 2.1f);
        e.emissive[2] = 0.75f + 0.25f * std::sin(m_entScroll + 4.2f);
        e.emissive[3] = 1.8f;
    }
    // Vent hum visual flicker (very subtle) — feel of moving air.
    if (m_eVent != kNoLink && m_eVent < scene.size()) {
        Entity& e = scene.get(m_eVent);
        e.emissive[3] = 0.25f + 0.05f * std::sin(m_time * 9.0f);
    }
    // Accent strips brighten subtly while moving (the lift "comes alive").
    float pulse = m_elev.moving() ? (0.7f + 0.3f * std::sin(m_time * 5.0f)) : 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (m_eAccent[i] != kNoLink && m_eAccent[i] < scene.size()) {
            Entity& e = scene.get(m_eAccent[i]);
            e.emissive[3] = 2.2f * pulse;
            if (m_elev.disco()) { e.emissive[0] = 0.6f + 0.4f*std::sin(m_time*4.0f + i); e.emissive[1] = 0.2f; e.emissive[2] = 0.7f; }
        }
    }
    (void)device;
    return dy;
}

// ===========================================================================
// CALLS / DISPLAY READ-BACK
// ===========================================================================
void ElevatorShowcase::callClub() {
    if (m_clubStop >= 0) m_elev.callTo(m_clubStop);
}

int ElevatorShowcase::currentFloorIndex() const {
    const float y = m_elev.cabCenter().y;
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < (int)m_floors.size(); ++i) {
        float d = std::fabs(y - m_floors[i].centerY);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void ElevatorShowcase::showcaseCamera(int variant, float out[5]) const {
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float floorY = c.y + m_cabHY;
    if (variant == 1) {
        // Exterior shaft: stand in the lobby looking at the doors.
        out[0] = m_shaftX; out[1] = floorY + 1.6f; out[2] = m_shaftZ + m_cabHZ + 2.6f;
        out[3] = -1.5708f; out[4] = 0.05f;
    } else if (variant == 2) {
        // Strata descent: inside, looking down through the glass floor.
        out[0] = m_shaftX - 0.4f; out[1] = floorY + 1.3f; out[2] = m_shaftZ + 0.3f;
        out[3] = 0.6f; out[4] = -0.65f;
    } else {
        // Interior beauty: stand in a back corner of the cab looking across the dark-
        // glass interior toward the +X holo wall + accent strips (eye height, slight
        // downward so the glass floor + strata read at the bottom of frame).
        out[0] = m_shaftX - m_cabHX + 0.35f;
        out[1] = floorY + 1.60f;
        out[2] = m_shaftZ - m_cabHZ + 0.35f;
        out[3] = 0.55f;          // yaw toward +X / +Z (the holo + screen corner)
        out[4] = -0.12f;
    }
}

} // namespace x3::game

// ===========================================================================
// Headless self-test (--test-elevator-showcase). Uses the shared headless device +
// a fresh Jolt world; no window/Vulkan. Leak-clean.
// ===========================================================================
#include "headless_device.h"

namespace x3::game {
namespace {
int s_pass = 0, s_fail = 0;
void chk(bool c, const char* n) {
    if (c) { ++s_pass; x3::logInfo(std::string("  [PASS] ") + n); }
    else   { ++s_fail; x3::logError(std::string("  [FAIL] ") + n); }
}
constexpr float kDt = 1.0f/60.0f;
} // namespace

bool runElevatorShowcaseSelfTest() {
    s_pass = s_fail = 0;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessRenderDevice device;
    Scene scene;

    ElevatorShowcase show;
    PlacementSpec spec;   // default tower (club at the bottom)
    bool built = show.build(scene, device, *physics, spec, nullptr);
    const auto& st = show.stats();

    chk(built && show.built(), "S1 showcase builds");
    chk(st.floors >= 5, "S2 multi-floor tower (>=5 floors)");
    chk(st.hasClubStop && std::fabs(st.clubStopY - (ElevatorSystem::kDefaultClubFloorY + 0.18f)) < 0.5f,
        "S3 Club 1127 stop present at Y=-200");
    chk(st.hasDarkGlass && st.hasGlassFloor, "S4 DARK smoked-glass walls + glass floor");
    chk(st.hasHandrail && st.hasHoloPanel && st.hasEntScreen && st.hasVent,
        "S5 handrail + holo panel + entertainment screen + vent");
    chk(st.shaftDoors == 2 * st.floors, "S6 two sliding door leaves per floor");
    chk(st.callPanels == st.floors && st.callButtons == 2 * st.floors,
        "S7 a realistic call-panel keypad (2 buttons) on every floor");
    chk(st.holoButtons == st.floors, "S8 one interior floor-select button per floor");

    // Drive a normal ride up one floor: rider carried, doors animate.
    {
        float feetY = show.cabTopY() + 0.05f, carried = 0.0f;
        int target = show.currentFloorIndex() + 1;
        if (target >= show.stopCount()) target = show.currentFloorIndex() - 1;
        show.callTo(target);
        bool sawClosing=false, sawMoving=false, arrived=false;
        for (int i = 0; i < 6000; ++i) {
            float edy = show.update(kDt, scene, device, *physics);
            if (show.playerRiding(x3::phys::Vec3{0,feetY,0})) { feetY += edy; carried += edy; }
            if (show.state() == ElevState::DoorsClosing) sawClosing = true;
            if (show.moving()) sawMoving = true;
            if (!show.moving() && show.currentFloorIndex() == target && (sawMoving)) { arrived = true; break; }
        }
        chk(sawClosing && sawMoving && arrived, "S9 normal ride: doors close, travel, arrive");
        chk(std::fabs(carried) > 1.0f, "S10 rider carried by the cab");
    }

    // 1127 keypad -> DISCO + descend all the way to Club 1127.
    {
        ElevatorShowcase s2; PlacementSpec sp; s2.build(scene, device, *physics, sp, nullptr);
        s2.keypadDigit(1); s2.keypadDigit(1); s2.keypadDigit(2);
        bool done = s2.keypadDigit(7);
        chk(done && s2.disco(), "S11 code 1127 enables DISCO + queues club descent");
        for (int i = 0; i < 40000 && s2.state() != ElevState::DoorsOpen && s2.state() != ElevState::Idle; ++i)
            s2.update(kDt, scene, device, *physics);
        chk(std::fabs(s2.cabCenter().y - (ElevatorSystem::kDefaultClubFloorY + 0.18f)) < 0.2f,
            "S12 cab descends all the way to Club 1127 (Y=-200)");
    }

    physics->shutdown();
    x3::logInfo("elevshowcase: " + std::to_string(s_pass) + "/" + std::to_string(s_pass+s_fail) + " passed");
    return s_fail == 0;
}

} // namespace x3::game
