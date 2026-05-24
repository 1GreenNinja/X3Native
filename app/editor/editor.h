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
#include <cstdint>
#include <string>
#include <vector>

namespace x3::editor {

// One placed object in a level. A pragmatic superset of what the editor needs +
// what x3-level-builder consumes (type maps to its areas[]->entities kinds).
struct EditorEntity {
    std::string name;
    std::string type = "prop";          // prop | enemy | item | npc | light | static
    float pos[3]   = { 0, 0, 0 };
    float yaw      = 0.0f;               // radians about +Y
    float scale    = 1.0f;
    float tint[3]  = { 0.8f, 0.8f, 0.85f };
    // Live link to the Scene entity id while editing (not serialized). kNoLink-ish
    // sentinel = not spawned in the live scene.
    uint32_t sceneEntity = 0xFFFFFFFFu;
};

// A whole level document.
struct LevelDoc {
    std::string name  = "untitled";
    std::string biome = "facility";     // x3-level-builder BIOME_PRESETS key
    float playerStart[3] = { 0, 0, 0 };
    std::vector<EditorEntity> entities;

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

// Move-gizmo axes.
enum class Axis : uint8_t { None = 0, X, Y, Z };

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

private:
    LevelDoc& m_doc;
    int   m_selected = -1;
    bool  m_snap = true;
    float m_grid = 0.5f;
};

// Headless self-test (--test-editor): JSON save/load round-trip equality, ray pick
// selects the nearest entity (and misses cleanly), move+snap updates the position,
// add/delete mutate the doc. Asserts E0-E5. No window / Vulkan.
bool runEditorSelfTest();

} // namespace x3::editor
