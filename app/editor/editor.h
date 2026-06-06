#pragma once
// Native Level Editor — E1 MVP (docs/design/WORLD_AND_EDITOR_PLAN.md §D).
//
// The editor's BRAIN, decoupled from rendering so it is fully headless-testable:
//   * LevelDoc  — the on-disk level format (a subset of the x3-level-builder JSON
//                 schema: name/biome/playerStart + entities[]). save/load round-trip.
//   * EditorState — selection, a 3-axis MOVE gizmo (with grid snap), and pick.
//
// The in-app layer (app/editor wiring in main.cpp, behind --editor / F9) renders
// this over the live viewport using the engine's existing HUD primitives
// (drawHudText / drawHudQuad / worldToScreen) + Jolt rayCast for click-select +
// the fly camera — no new render backend. UE-style ImGui panels are a later phase.
//
// Game/slice code only; engine/ stays pure.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::editor {

// One placed object in a level. A pragmatic superset of what the editor needs +
// what x3-level-builder consumes (type maps to its areas[]->entities kinds).
struct EditorEntity {
    std::string name;
    std::string type = "prop";          // prop | enemy | item | npc | light | static | model
    float pos[3]   = { 0, 0, 0 };
    float yaw      = 0.0f;               // radians about +Y
    float scale    = 1.0f;
    float tint[3]  = { 0.8f, 0.8f, 0.85f };
    // Feature 3 (content/model browser): a GLB relative path under the editor's
    // mounted converted_glb dir. When non-empty the editor renders that model's
    // drawables at this entity's transform (instead of a graybox). Round-trips in JSON.
    std::string model;
    // Live link to the Scene entity id while editing (not serialized). kNoLink-ish
    // sentinel = not spawned in the live scene.
    uint32_t sceneEntity = 0xFFFFFFFFu;
};

// A BLOCKOUT BRUSH (Level Architect P2 greybox). A parametric prototyping solid —
// a Box or Ramp — drawn with the clean grid material and given STATIC Jolt
// collision so the greybox is immediately walkable in Play mode. ORIGIN-CENTERED:
// pos/yaw live here (in the transform), size is full extents in metres; the mesh
// is built origin-centered so move/resize is a transform/body update, not a mesh
// regen for move. `type` matches x3::prims::BrushType (0=Box, 1=Ramp).
struct BlockoutBrush {
    std::string name;
    uint32_t type   = 0;                 // 0 = Box, 1 = Ramp (== prims::BrushType)
    float pos[3]    = { 0, 0, 0 };       // world center
    float size[3]   = { 2, 2, 2 };       // full extents (m)
    float yaw       = 0.0f;              // radians about +Y
    float tint[3]   = { 0.85f, 0.85f, 0.88f };
    // Surface MATERIAL id (Feature 1, click-a-wall texturing). Names a built-in
    // material in editorMaterials() — the host resolves it to a cached GPU texture +
    // tint when (re)spawning the brush. Empty == the default clean grid material.
    // Round-trips through the brushes[] JSON so a textured blockout reloads as-authored.
    std::string material;
    bool  collide   = true;              // add a static Jolt body
    // Live links (NOT serialized): the Scene entity id + Jolt body id while editing.
    uint32_t sceneEntity = 0xFFFFFFFFu;
    uint32_t body        = 0;            // x3::phys::BodyId.id (0 == none)
};

// A whole level document.
struct LevelDoc {
    std::string name  = "untitled";
    std::string biome = "facility";     // x3-level-builder BIOME_PRESETS key
    float playerStart[3] = { 0, 0, 0 };
    std::vector<EditorEntity> entities;
    // Level Architect P2 BLOCKOUT brushes. A top-level brushes[] array in the JSON,
    // mirroring entities[]. Save/Load round-trips it (type/pos/size/yaw/collide).
    std::vector<BlockoutBrush> brushes;

    // Serialize to / parse from the level JSON. saveJson writes a pretty doc;
    // loadJson parses what saveJson emits (a focused subset parser, tolerant of
    // whitespace). Returns false on file IO / parse failure.
    bool saveJson(const std::string& path) const;
    bool loadJson(const std::string& path);

    // Serialize to / from a JSON STRING (the file fns wrap these). Exposed for the
    // round-trip self-test without touching the filesystem.
    std::string toJson() const;
    bool        fromJson(const std::string& json);
};

// Transform-gizmo axes.
enum class Axis : uint8_t { None = 0, X, Y, Z };

// ---------------------------------------------------------------------------
// Undo/redo command stack (P0.6). The history is BRUSH-FOCUSED: every brush
// add / delete / transform routes through it so Ctrl+Z/Y are universal in the
// editor. Commands are SNAPSHOT-based (store the full BlockoutBrush value), which
// is robust and trivially correct for the small brush count a blockout has.
//
// applyUndo/applyRedo MUTATE only the LevelDoc's brushes[] (pure logic, headless-
// testable). They return a HistoryEffect so the live host knows how to re-sync the
// Scene + Jolt (a moved brush needs only a transform sync; an add/delete/resize
// needs a full respawn). The host owns the GPU/physics side via the returned hint.
// ---------------------------------------------------------------------------
enum class CmdKind : uint8_t { None = 0, Add, Delete, Transform };

// What the host must do to the live scene after an undo/redo applied to brushes[].
//   None       — nothing changed (empty stack).
//   Respawn    — brush at `index` must be (re)built (add/undo-delete/resize): rebuild
//                mesh + body. For an undo-of-add / redo-of-delete the brush is GONE;
//                `removed` is true and the host tears down the prior live links.
//   SyncXform  — only the transform (pos/yaw) changed: cheap transform/body update.
struct HistoryEffect {
    enum class Op : uint8_t { None = 0, Respawn, SyncXform } op = Op::None;
    int  index   = -1;       // brush index the host must act on (post-apply doc index)
    bool removed = false;    // the brush at `index` no longer exists (tear down links)
    // For a teardown the host needs the dead brush's live links; carried here so the
    // host can destroy the mesh/body without the brush record still existing.
    uint32_t deadSceneEntity = 0xFFFFFFFFu;
    uint32_t deadBody        = 0;
};

// What the current selection refers to (tagged selection — entities[] vs brushes[]).
// P3 will extend this to a multi-select vector; P2 needs only the single tagged kind.
enum class SelKind : uint8_t { None = 0, Entity, Brush };

// UE-style tool modes, bound to Q/W/E/R (Task9D used W/E/R for the gizmos).
//   Q Select · W Move · E Rotate · R Scale.
enum class Tool : uint8_t { Select = 0, Move, Rotate, Scale };

// ---------------------------------------------------------------------------
// Theme — the Lab Architect / Task9D palette (dark navy + cyan accents + the
// per-tool gizmo colors W=green/E=blue/R=pink), as DATA so the in-app HUD layer
// just reads it. RGBA floats 0..1.
// ---------------------------------------------------------------------------
struct EditorTheme {
    float bg[4]      = { 0.024f, 0.024f, 0.059f, 0.95f };  // #06060f panels
    float panel[4]   = { 0.039f, 0.055f, 0.094f, 0.95f };  // rgba(10,14,24)
    float border[4]  = { 0.102f, 0.165f, 0.227f, 1.0f };   // #1a2a3a steel
    float accent[4]  = { 0.267f, 0.667f, 1.0f, 1.0f };     // #44aaff cyan
    float text[4]    = { 0.80f, 0.86f, 0.95f, 1.0f };
    float textDim[4] = { 0.50f, 0.56f, 0.66f, 1.0f };
    float warn[4]    = { 1.0f, 0.53f, 0.30f, 1.0f };        // pathway/exit orange-red
    // Per-tool gizmo accents (match Task9D btnGiz colors).
    float gizMove[4]   = { 0.53f, 1.0f, 0.53f, 1.0f };      // #8f8 green  (W)
    float gizRotate[4] = { 0.53f, 0.67f, 1.0f, 1.0f };      // #8af blue   (E)
    float gizScale[4]  = { 1.0f, 0.53f, 0.67f, 1.0f };      // #f8a pink   (R)
    float selected[4]  = { 1.0f, 0.82f, 0.18f, 1.0f };      // selection highlight (amber)
    // The three world axes (X red / Y green / Z blue — UE/industry standard).
    float axisX[4] = { 0.93f, 0.27f, 0.30f, 1.0f };
    float axisY[4] = { 0.40f, 0.86f, 0.40f, 1.0f };
    float axisZ[4] = { 0.30f, 0.55f, 0.95f, 1.0f };
};

// A legend / palette row (Task9D's right-hand legend): a room/entity TYPE name +
// its color. Doubles as the drag-and-drop content palette (drag a row into the
// viewport to place that type). Returned by editorPalette().
struct PaletteItem { const char* type; const char* label; float color[4]; };

// The Lab-Architect-derived content palette (room/entity types + colors). Stable
// array; count via editorPaletteCount().
const PaletteItem* editorPalette();
uint32_t           editorPaletteCount();

// ---------------------------------------------------------------------------
// Feature 1 — CLICK-A-WALL surface MATERIALS. A curated built-in set surfaced as a
// clickable palette panel: pick a brush, click a material, the brush re-skins (its
// render tint + a procedural texture) and the choice persists in the LevelDoc JSON
// (BlockoutBrush::material). `id` is the stable serialized name; `tex` selects which
// procedural generator the host bakes once + caches; `tint` multiplies the texel.
// id "" (the implicit default) == the clean blockout grid material.
// ---------------------------------------------------------------------------
enum class MatTex : uint8_t {
    Grid = 0,   // the clean blockout grid (the default)
    Panel,      // sci-fi gunmetal wall panel
    CleanPanel, // smooth near-white architectural panel
    Floor,      // walkable deck plate
    Ceiling,    // overhead coffer panel
    Solid,      // flat solid color (tint only)
};
struct BlockoutMaterial { const char* id; const char* label; MatTex tex; float tint[3]; };
// The curated built-in material set. Stable array; count via editorMaterialCount().
// editorMaterialFind() returns the index of `id` (or -1), so the host + tests resolve
// a serialized name back to a material deterministically.
const BlockoutMaterial* editorMaterials();
uint32_t                editorMaterialCount();
int                     editorMaterialFind(const std::string& id);

// ---------------------------------------------------------------------------
// Feature 3 — content/MODEL browser. A curated handful of GLB props from the asset
// catalog (converted_glb): click an entry to place a model entity (EditorEntity with
// type "model" + EditorEntity::model = relPath) at the fly-cam focus. `relPath` is the
// path under the editor's mounted converted_glb dir; `label` is the browser display.
// ---------------------------------------------------------------------------
struct ModelCatalogItem { const char* relPath; const char* label; };
const ModelCatalogItem* editorModelCatalog();
uint32_t                editorModelCatalogCount();

// A menu / toolbar command (data-driven so the HUD renders it + the test checks
// it). `shortcut` is a display hint ("W", "Ctrl+S"); `id` drives dispatch.
enum class Cmd : uint16_t {
    None = 0,
    ToolSelect, ToolMove, ToolRotate, ToolScale,
    Duplicate, Delete, Focus, ToggleSnap, ToggleSpace,
    Undo, Redo, Save, Load, NewLevel,
    CamOrbit, CamFly, CamFpsWalk, ToggleWireframe,
};
struct MenuItem { const char* label; const char* tooltip; const char* shortcut; Cmd id; };
struct Menu     { const char* title; const MenuItem* items; uint32_t count; };
// The top menu bar (File / Edit / Tools / View), data-driven. menuBar() returns
// the array, menuBarCount() its length.
const Menu* editorMenuBar();
uint32_t    editorMenuBarCount();

// Editor interaction state over a LevelDoc. Pure logic — no rendering.
class EditorState {
public:
    explicit EditorState(LevelDoc& doc) : m_doc(doc) {}

    LevelDoc&       doc()       { return m_doc; }
    const LevelDoc& doc() const { return m_doc; }

    int  selected() const { return m_selected; }                 // -1 = none
    void select(int index);                                      // clamps; -1 clears
    bool hasSelection() const { return m_selected >= 0 && m_selected < (int)m_doc.entities.size(); }

    // Pick the entity whose center is nearest the ray (origin + t*dir), within
    // `hitRadius` of the ray and `maxDist` along it. Returns the index or -1. This
    // mirrors what a Jolt rayCast→entity pick resolves to, but works on the doc so
    // it is testable headlessly (the live editor can use either).
    int pickRay(const float origin[3], const float dir[3],
                float maxDist = 1000.0f, float hitRadius = 0.6f) const;

    // Move the selection along `axis` by `delta` metres (snapped to the grid if
    // snapping is on). No-op without a selection. Returns true if it moved.
    bool moveSelected(Axis axis, float delta);

    // Grid snap (applied by moveSelected + snapSelected).
    void  setSnap(bool on, float grid = 0.5f) { m_snap = on; m_grid = (grid > 1e-4f ? grid : 0.5f); }
    bool  snapEnabled() const { return m_snap; }
    float grid() const { return m_grid; }
    // Snap the selection's position to the grid now. Returns true if it moved.
    bool  snapSelected();

    // Add a fresh entity (duplicating the selection's type/tint at `pos`) and select
    // it. Returns the new index.
    int   addEntity(const char* type, const float pos[3]);
    // Delete the selection. Returns true if one was removed.
    bool  deleteSelected();

    // ---- Tagged selection (entities[] vs brushes[]) -------------------------
    // P2 selection spans both lists. selectBrush()/selectEntity() set the kind +
    // index; selKind()/selIndex() report it. The legacy select()/selected() above
    // still drive entities[] (kept for the existing self-tests + entity tools).
    SelKind selKind() const { return m_selKind; }
    int     selIndex() const { return m_selIndex; }
    void    selectBrush(int index);          // clamps to brushes[]; -1 clears
    bool    hasBrushSelection() const {
        return m_selKind == SelKind::Brush && m_selIndex >= 0 &&
               m_selIndex < (int)m_doc.brushes.size();
    }

    // Pick the BRUSH whose oriented box (size extents, yaw about +Y, at pos) the ray
    // (origin + t*dir) enters NEAREST the origin. A true ray-vs-OBB slab test (tighter
    // than a center-distance pick on long thin brushes — Feature 2). Returns the brush
    // index or -1 (miss). Pure logic over brushes[], so the live viewport + a headless
    // test share it. `pad` adds a small slack to the half-extents for easier grabbing.
    int pickBrushRay(const float origin[3], const float dir[3], float pad = 0.0f) const;

    // ---- Blockout brush ops (Level Architect P2; headless-testable) ----------
    // Add a brush of `type` (0=Box, 1=Ramp) at `pos` (snapped to the grid), default
    // 2 m cube extents (snapped), selects it. Returns the new brush index.
    int   addBrush(uint32_t type, const float pos[3]);
    // Resize the selected brush along `axis` by `delta` metres (face grow; the
    // resulting extent is snapped to the grid + clamped to a 0.25 m minimum). No-op
    // without a brush selection. Returns true if it changed.
    bool  resizeSelectedBrush(Axis axis, float delta);
    // Move the selected brush along `axis` by `delta` metres (snapped to the grid).
    bool  moveSelectedBrush(Axis axis, float delta);
    // Delete the selected brush. Returns true if one was removed.
    bool  deleteSelectedBrush();

    // Snap a scalar to the current grid (exposed so the live host snaps too).
    float snapValue(float v) const { return m_snap ? std::round(v / m_grid) * m_grid : v; }
    void  setGrid(float g) { if (g > 1e-4f) m_grid = g; }

    // ---- Undo / redo (P0.6) -------------------------------------------------
    // Add a brush AND record it as an undoable command. Same as addBrush() but the
    // op goes on the history stack. Returns the new brush index.
    int   addBrushCmd(uint32_t type, const float pos[3]);
    // Delete the selected brush AND record it. The live links (sceneEntity/body) are
    // captured so a teardown is possible and a redo-restore round-trips them. Returns
    // true if one was removed.
    bool  deleteSelectedBrushCmd();

    // Transform-edit commands group a continuous drag into ONE undo step. Call
    // beginBrushEdit() at the start of a gizmo/DragFloat interaction (captures the
    // before-snapshot), mutate brushes[idx] freely, then commitBrushEdit() once the
    // interaction ends (captures the after-snapshot + pushes a Transform command iff
    // the value actually changed). A begin with no net change is dropped on commit.
    void  beginBrushEdit(int index);
    void  commitBrushEdit();
    bool  editing() const { return m_editing; }

    bool  canUndo() const { return m_undoPos > 0; }
    bool  canRedo() const { return m_undoPos < (int)m_history.size(); }
    // Apply one undo / redo to brushes[]. Returns the host re-sync hint (see
    // HistoryEffect). No-op (op==None) when the stack end is reached.
    HistoryEffect undo();
    HistoryEffect redo();

private:
    // One undoable brush operation (snapshot pair). For Add: `after` is the created
    // brush, `before` unused. For Delete: `before` is the removed brush, `after`
    // unused. For Transform: both are the brush value pre/post the edit.
    struct BrushCmd {
        CmdKind kind = CmdKind::None;
        int     index = -1;          // brush index the op targets
        BlockoutBrush before;        // pre-state (Delete / Transform)
        BlockoutBrush after;         // post-state (Add / Transform)
    };
    void pushCmd(const BrushCmd& c);  // truncates redo tail, appends, advances pos

    LevelDoc& m_doc;
    int   m_selected = -1;
    bool  m_snap = true;
    float m_grid = 0.5f;
    // Tagged selection (P2). m_selIndex indexes brushes[] when kind==Brush, else the
    // legacy entities[] selection above is the source of truth.
    SelKind m_selKind  = SelKind::None;
    int     m_selIndex = -1;

    // Undo/redo stack (P0.6). m_history holds commands oldest->newest; m_undoPos is
    // the count of APPLIED commands (everything < m_undoPos is "done", >= is "undone").
    // A new push truncates the redo tail (anything >= m_undoPos).
    std::vector<BrushCmd> m_history;
    int                   m_undoPos = 0;
    // Transform-edit grouping (beginBrushEdit/commitBrushEdit).
    bool          m_editing      = false;
    int           m_editIndex    = -1;
    BlockoutBrush m_editBefore;
};

// Headless self-test (--test-editor): JSON save/load round-trip equality, ray pick
// selects the nearest entity (and misses cleanly), move+snap updates the position,
// add/delete mutate the doc. Asserts E0-E5. No window / Vulkan.
bool runEditorSelfTest();

// Headless self-test (--test-blockout): a BlockoutBrush round-trips through the
// brushes[] JSON (type/pos/size/yaw/collide preserved), addBrush snaps to the grid,
// resizeSelectedBrush grows a face with snap, and the brush mesh builder produces
// non-degenerate render + collision geometry for Box and Ramp. Asserts B0-B4. No
// window / Vulkan. Returns true iff all pass.
bool runBlockoutSelfTest();

} // namespace x3::editor
