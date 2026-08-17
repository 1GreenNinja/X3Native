#pragma once
// ============================================================================
// LEVEL ARCHITECT — CUTAWAY VIEW  (the native port of the Babylon tool's X-ray)
//
// docs/design/LEVEL_ARCHITECT_CUTAWAY_REF{,2,3}.png is the spec: the WHOLE
// facility seen from outside as a stack of translucent floors, interiors
// readable through the glass, a hover card naming the room under the cursor,
// per-floor and per-structure visibility, one global OPACITY dial, a colour
// legend, a live X/Y/Z readout and a stats panel.
//
// THIS IS A VIEW, NOT AN EDITOR. It renders the CANONICAL data
// (assets/levels/EscapeLab48_AllFloors_v2.project.json — 7 floors, 124 rooms,
// 160 doors) through the loader that already parses it (`loadCanonTower`,
// app/level_loader.h). It builds no Jolt bodies, spawns no gameplay, and owns
// no schema of its own: change the project JSON and the cutaway changes.
//
// WHY IT IS ITS OWN MODULE AND NOT PART OF EditorHost. EditorHost is the live
// ImGui edit session over ONE floor's brushes (app/editor/editor_host.h). The
// cutaway is a whole-building READ of the canon at a scale no brush session
// wants to hold. Keeping it separate means it costs the editor nothing and can
// be captured headlessly (--screenshot-cutaway) without an ImGui context.
//
// RENDERING SHAPE (X3_WORLD_RULES rule 5 kept):
//   * every room is a translucent SHELL via IRenderDevice::drawMeshGlass, whose
//     GlassMaterial::opacity is the panel's opacity dial. (Rule 5's "authored
//     baseColor alpha in (0,0.07)" governs GLB-authored panes; this is the
//     explicit runtime dial the same rule points at for translucent shells.)
//   * FLOOR PLATES + EDGE CAGES + DOOR MARKERS are thin opaque prisms drawn
//     with a LOW flat emissive (<= 0.45, under the ACES clip the rule names) so
//     they read as glowing structure through the glass instead of a white slab.
//   * the scene is deliberately unlit and low-key: no sky, near-black ambient,
//     fixed exposure (auto-exposure OFF, or a black frame would be gained up
//     until the emissives blow out).
// ============================================================================

#include "level_loader.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// One room as the cutaway needs it: the canon geometry + the colour it draws in
// + which visibility groups own it. Index == the room's index in the merged
// CanonFloor (so `CutawayModel::canon.rooms[i]` is always this room).
struct CutRoom {
    uint32_t id       = 0;
    int      floorNum = 1;      // 1..7 (the canonical floor this room came from)
    uint8_t  band     = 0;      // index into CutawayModel::bands
    uint8_t  group    = 0;      // index into CutawayModel::groups (the STRUCTURES list)
    std::string name;
    std::string type;
    std::string desc;           // the hover card's second line
    float cx = 0, cy = 0, cz = 0;
    float w = 0, h = 0, d = 0;
    float color[3] = { 0.5f, 0.6f, 0.7f };   // legend colour (by room type)

    float x0() const { return cx - w * 0.5f; }
    float x1() const { return cx + w * 0.5f; }
    float y0() const { return cy - h * 0.5f; }
    float y1() const { return cy + h * 0.5f; }
    float z0() const { return cz - d * 0.5f; }
    float z1() const { return cz + d * 0.5f; }
};

// A toggleable horizontal band. The 7 canonical floors, plus one "Sub-Level /
// Caves" band for the Floor-1 rooms that live 178 m down (the JSON files them
// under floor 1, but they are a separate DECK to anyone looking at the stack —
// which is exactly what the reference's "Hidden + Cave" row is).
struct CutBand {
    std::string name;           // "F4 Cybernetics Wing"
    int   floorNum = 1;
    bool  visible  = true;
    bool  subLevel = false;     // the deep cave/sub-level band
    float y0 = 0, y1 = 0;       // world Y span of the rooms in this band
    uint32_t roomCount = 0;
};

// A toggleable STRUCTURE group — the reference's left-hand list (Main Hall,
// Cells, Service Corridors, Boss Area, Elevator Shaft, ...). Derived from the
// canonical room `type` + name, so it cannot drift from the data.
struct CutGroup {
    std::string name;
    bool  visible = true;
    uint32_t roomCount = 0;
    float color[3] = { 0.5f, 0.6f, 0.7f };
};

// Whole-facility numbers, all MEASURED from the loaded data (NO_SLOP rule 9 —
// the reference's stats panel quotes figures; ours computes them).
struct CutStats {
    uint32_t floors = 0, rooms = 0, doors = 0, groups = 0;
    float towerY0 = 0, towerY1 = 0;      // the above-ground stack's Y span
    float caveY0  = 0;                   // deepest room floor (the cave deck)
    float footprintX = 0, footprintZ = 0;
    uint32_t cells = 0;                  // detention cells (the "Cells (32)" row)
    uint32_t descsFromJson = 0;          // rooms whose desc came from the project
    uint32_t descsAuthored = 0;          // rooms filled from kF1Desc
};

// The parsed, coloured, grouped facility. Pure DATA — buildable with no window,
// no device and no Vulkan, which is what lets --test-cutaway assert against it.
struct CutawayModel {
    CanonFloor            canon;     // the merged 7-floor tower (loadCanonTower)
    std::vector<CutRoom>  rooms;
    std::vector<CutBand>  bands;
    std::vector<CutGroup> groups;
    CutStats              stats;
    std::string           sourcePath;
    bool valid() const { return !rooms.empty(); }

    // Facility center + radius (above-ground stack only, so the default framing
    // is the tower the reference shows, not a view zoomed out to include a cave
    // 178 m below it).
    void towerFraming(float outCenter[3], float& outRadius) const;

    // Is this room drawn right now? (its band AND its structure group are on)
    bool visible(const CutRoom& r) const {
        return bands[r.band].visible && groups[r.group].visible;
    }
};

// HOVER PICK — exact ray/AABB against every VISIBLE room, nearest hit wins.
// A FREE function over the model (not a CutawayView method) on purpose: the
// hover card is the feature most likely to be silently wrong, so --test-cutaway
// has to be able to fire real rays at the real facility with no GPU in the
// process. CutawayView::pickRoom is a one-line forward to this.
//
// `eye`/`yaw`/`pitch` are the engine camera convention (docs/CONVENTIONS.md:
// fwd = (cos p cos y, sin p, cos p sin y), up = +Y). Mouse coords are HUD pixels
// (top-left origin). Returns kNoRoom for a miss.
uint32_t cutawayPick(const CutawayModel& m,
                     const float eye[3], float yaw, float pitch, float fovDeg,
                     float mouseXPx, float mouseYPx, uint32_t viewW, uint32_t viewH);

// Parse the canonical project and build the cutaway model. Headless-safe.
// Returns a model with valid()==false if the JSON is missing/unparseable.
CutawayModel buildCutawayModel(std::string_view jsonPath);

// The one place a room type becomes a colour. Exposed so the legend, the plates
// and the self-test all read the SAME table (NO_SLOP rule 4).
void cutawayTypeColor(std::string_view roomType, float outRgb[3]);
// The one place a room becomes a STRUCTURE group name (the reference's list).
const char* cutawayGroupFor(const CutRoom& r);

// ---------------------------------------------------------------------------
// The live view: GPU meshes + orbit camera + HUD panels. One instance per host.
// ---------------------------------------------------------------------------
class CutawayView {
public:
    // Build the model, then the GPU meshes. `device` may be a headless device
    // (the mesh creates are then no-ops that still exercise the build path).
    bool build(x3::rhi::IRenderDevice& device, std::string_view jsonPath);
    void shutdown(x3::rhi::IRenderDevice& device);

    const CutawayModel& model() const { return m_model; }

    // ---- Camera (orbit: drag to swing, wheel to dolly, MMB/RMB to pan) ----
    void frameTower();                      // default framing (the reference's)
    void frameAll();                        // include the cave deck
    void orbitDrag(float dxPx, float dyPx);
    void dolly(float notches);
    void pan(float dxPx, float dyPx);
    void applyCamera(x3::rhi::IRenderDevice& device) const;
    void eye(float out[3]) const { out[0] = m_eye[0]; out[1] = m_eye[1]; out[2] = m_eye[2]; }
    float yaw()   const { return m_yaw; }
    float pitch() const { return m_pitch; }
    float fovDeg() const { return m_fov; }

    // ---- Visibility + look dials (the panel + the console cvars drive these) --
    void setOpacity(float o);
    float opacity() const { return m_opacity; }
    void toggleBand(uint32_t i);
    void soloBand(uint32_t i);
    void showAllBands();
    void toggleGroup(uint32_t i);
    bool bandVisible(uint32_t i) const;
    bool roomVisible(const CutRoom& r) const;

    bool plates = true;      // the per-room floor slabs
    bool cages  = true;      // the per-room edge wireframe cages
    bool doors  = true;      // the doorway markers
    bool planes = true;      // the reference's "Floor Planes": one deck plate per band,
                             // spanning the WHOLE footprint. Without these the stack
                             // reads as floating trays instead of a building, because
                             // the canon's decks really are 10-35 m apart.
    bool envelope = true;    // the reference's "Steel Frame + Exterior": the building skin
                             // + corner columns. (NOT named `frame` — every draw call in
                             // this class already has a FrameContext& frame.)
    bool solid  = false;     // "Solid View" — opacity forced to 1
    bool panel  = true;      // the left-hand tool panel
    bool legend = true;      // the right-hand colour legend

    // ---- Render ----
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);

    // ---- Hover: which room is under this pixel? kNoRoom if none. ----
    // Exact ray/AABB against every VISIBLE room, nearest hit wins; the ray is
    // built from this view's own camera pose, so it cannot drift from what is
    // on screen.
    uint32_t pickRoom(float mouseXPx, float mouseYPx,
                      uint32_t viewW, uint32_t viewH) const;

    // ---- HUD (drawHudQuad / drawHudText — ImGui is NOT used here) ----
    // Draws the tool panel, the legend, the stats panel, the POS/DEPTH readout
    // and, when `hovered` is a real room, the hover card at its screen position.
    void drawUi(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                uint32_t hovered) const;
    // Just the hover card (the headless capture uses it without the chrome).
    void drawCard(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  uint32_t room) const;

private:
    struct RoomGpu {
        x3::rhi::MeshHandle shell{};   // the translucent box
        x3::rhi::MeshHandle plate{};   // the floor slab
        x3::rhi::MeshHandle cage{};    // 12 edge beams, merged
    };

    CutawayModel          m_model;
    std::vector<RoomGpu>  m_gpu;
    // Doorway markers, merged PER BAND — not one mesh for all 160. Hiding F4-F7
    // has to hide their doors too; a single merged mesh left green pips floating
    // in the empty upper volume of the very capture meant to prove the cut.
    std::vector<x3::rhi::MeshHandle> m_doorMesh;
    x3::rhi::MeshHandle   m_envShell{};       // the whole-building glass envelope
    x3::rhi::MeshHandle   m_envFrame{};       // corner columns + per-deck rim beams
    std::vector<x3::rhi::MeshHandle> m_planeMesh;   // one full-footprint deck plate per band
    x3::rhi::TextureHandle m_white{};
    bool                  m_built = false;

    // Orbit camera state.
    float m_pivot[3] = { 0, 0, 0 };
    float m_dist     = 160.0f;
    float m_yaw      = -2.20f;
    float m_pitch    = -0.42f;
    float m_eye[3]   = { 0, 0, 0 };
    float m_fov      = 45.0f;
    float m_opacity  = 0.12f;

    float fitDistance(float radius) const;
    void  recomputeEye();
};

// --test-cutaway: headless self-test. Asserts the canonical project loads, the
// 7 floors / 124 rooms / 160 doors are all present in the built model, every
// band and group is populated, EVERY room carries a description, the colour
// table covers every room type, and the ray-pick agrees with the geometry.
// No window, no Vulkan. Logs PASS/FAIL per check; true iff all pass.
bool runCutawaySelfTest();

} // namespace x3::game
