#pragma once
// Level Architect — EDITOR HOST (Phase 1 shell + Phase 2 blockout).
//
// The live-mode orchestrator. The DEVICE owns the ImGui frame lifecycle
// (beginEditorUI = NewFrame + the dockspace root; endEditorUI = Render + stash
// draw data); the HOST submits the actual panels between them via draw(). It also
// owns:
//   * the editor LevelDoc + EditorState (selection / brush ops)
//   * a fly-cam for edit mode (WASD + RMB mouse-look), gated on !editorWantsInput
//   * the F8 Edit<->Play toggle (Play = normal game input + panels hidden)
//   * the BLOCKOUT subsystem: the clean grid material (one cached texture), and the
//     live spawn/move/resize/collision bridge from brushes[] into the Scene + Jolt.
//
// ADDITIVE + EDITOR-ONLY: nothing here runs without --editor; the game path is
// untouched. Game/slice code only — engine/ stays pure (the host talks to the
// engine through IRenderDevice / IPhysicsWorld / Scene, never Vulkan/Jolt types).
//
// ImGui is used DIRECTLY here (the app links imgui::imgui for the core widget API;
// the GLFW/Vulkan backends live in the device). The device's begin/endEditorUI
// wrap the NewFrame/Render so the host only issues Begin/End/widget calls.

#include "editor.h"

#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace x3::rhi { class IRenderDevice; struct FrameContext; }
namespace x3::phys { class IPhysicsWorld; }

namespace x3::game { class Scene; }

namespace x3::editor {

// Host run mode. Edit = fly-cam + panels + brush tools. Play = the normal game
// input/camera path runs, the editor panels are hidden (a thin PLAYING hint stays).
enum class HostMode : uint8_t { Edit = 0, Play = 1 };

class EditorHost {
public:
    EditorHost() : m_state(m_doc) {}

    // One-time session init (called once after the device + editor UI are up, with a
    // valid window + scene + physics). Creates the cached grid material on the GPU.
    // Safe to call once; a second call is a no-op.
    void init(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
              x3::phys::IPhysicsWorld& physics, GLFWwindow* window);

    // Per-frame tick BEFORE beginFrame()/beginEditorUI(): poll editor hotkeys (F8),
    // run the fly-cam (Edit mode) and push it to device->setCamera. `wantMouse`/
    // `wantKbd` are the device's editorWantsInput() flags so panel interaction never
    // moves the camera. Returns true if the host drove the camera this frame (so the
    // caller skips the game camera in Edit mode). No-op-ish in Play mode (returns
    // false; the game owns the camera).
    bool tick(float dt, bool wantMouse, bool wantKbd, x3::rhi::IRenderDevice& device);

    // Submit the editor panels (call between device->beginEditorUI() and
    // device->endEditorUI()). The dockspace root is already open (device side); this
    // adds the menu bar + Outliner + Blockout + status panels. Hidden in Play mode
    // except a small PLAYING overlay.
    void draw(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
              x3::phys::IPhysicsWorld& physics, float dt);

    // Programmatic blockout placement (used by the headless --screenshot-editor proof
    // and any scripted setup): add a brush + spawn its live mesh/collision. Returns
    // the brush index. `device`/`scene`/`physics` must be the same instances passed to
    // init(). pos is the world center; size is full extents (m).
    int placeBrush(uint32_t type, const float pos[3], const float size[3],
                   x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                   x3::phys::IPhysicsWorld& physics);

    // Feature 3: place a GLB model entity (type "model", model=relPath) at the fly-cam
    // focus, loading + caching the GLB on first use. Returns the entity index (or -1 if
    // the LevelDoc is full). The model is drawn by renderModels() each frame.
    int placeModel(const std::string& relPath, x3::rhi::IRenderDevice& device);

    // Feature 3: draw all placed model entities' GLB drawables at their transforms.
    // Call from the main loop ALONGSIDE scene.render() (it needs a live FrameContext,
    // so it can't run inside the ImGui-only draw()). No-op if no models are placed or
    // the converted_glb dir didn't mount. Editor-only.
    void renderModels(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);

    HostMode mode() const { return m_mode; }
    LevelDoc&       doc()       { return m_doc; }
    EditorState&    state()     { return m_state; }

private:
    // Build/destroy the live Scene entity + Jolt body for a single brush (by index).
    void spawnBrush(int idx, x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                    x3::phys::IPhysicsWorld& physics);
    void respawnBrush(int idx, x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                      x3::phys::IPhysicsWorld& physics);   // resize: rebuild mesh+body
    void syncBrushTransform(int idx, x3::game::Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Apply an undo/redo HistoryEffect to the LIVE scene (rebuild / sync / tear down a
    // brush per the hint the EditorState returned). Centralizes the GPU+Jolt side so
    // Ctrl+Z/Y, gizmo commits, and the Details panel all share one re-sync path.
    void applyEffect(const HistoryEffect& eff, x3::rhi::IRenderDevice& device,
                     x3::game::Scene& scene, x3::phys::IPhysicsWorld& physics);
    // Tear down a brush's live mesh + body by explicit links (used when the brush
    // record is already gone, e.g. undo-of-add).
    void teardownLinks(uint32_t sceneEntity, uint32_t body,
                       x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                       x3::phys::IPhysicsWorld& physics);

    // ---- P3 transform gizmo (move) + viewport pick --------------------------
    // Draw the move gizmo over the selected brush (ImGui foreground draw list, via
    // device->worldToScreen) and handle LMB axis drag -> moveSelectedBrush, grouped
    // into one undo step (begin/commitBrushEdit). Also handles a plain LMB click in
    // the empty viewport = ray-pick a brush to select. Called from draw() in Edit mode.
    void gizmoAndPick(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                      x3::phys::IPhysicsWorld& physics, bool wantMouse);

    // ---- Phase 5: Doom-Builder "Visual Mode" KEYBOARD nudge editing ---------
    // Crosshair-raycast the brush the fly-cam LOOKS at, then shape it with the keyboard
    // (move in/out, stretch, raise/lower floor & ceiling) — all grid-snapped, each nudge
    // one undo step, live-synced to Scene + Jolt. Reads the rebindable m_keybinds table
    // (Shift = larger step, Ctrl = finer). Called from draw() in Edit mode after the
    // gizmo. wantKbd gates it so typing in a panel never nudges. Returns the action that
    // fired this frame (or Count) for the status readout.
    NudgeAction visualNudge(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                            x3::phys::IPhysicsWorld& physics, bool wantKbd);
    // Resolve the world AXIS the looked-at brush's faced surface normal points along, by
    // testing which face of the brush OBB the crosshair ray crosses. Falls back to the
    // camera-forward dominant axis. Used to pick the move/stretch axis for visualNudge.
    Axis facedAxis(int brushIdx) const;

    // Draw the unobtrusive floating keybind cheat-sheet (corner, low alpha, small) and
    // the rebind panel. Reads/writes m_keybinds so the table + tooltip never drift.
    void drawKeybindOverlay();
    void drawRebindPanel();

    LevelDoc    m_doc;
    EditorState m_state;
    HostMode    m_mode = HostMode::Edit;

    bool          m_inited   = false;
    GLFWwindow*   m_window   = nullptr;
    // Cached clean grid material (one texture shared by every brush). Created in init.
    uint32_t      m_gridTex  = 0;   // x3::rhi::TextureHandle.id (0 == none)

    // ---- Feature 1: surface MATERIAL texture cache --------------------------
    // One GPU texture per MatTex kind, baked lazily on first use + shared by every
    // brush that picks that material (mirrors the grid-material session caching). The
    // grid kind aliases m_gridTex. Index by (uint8_t)MatTex. 0 == not yet created.
    uint32_t      m_matTex[8] = { 0,0,0,0,0,0,0,0 };
    // Resolve a brush's material id -> { GPU texture id, tint[3] } for spawnBrush, baking
    // (and caching) the texture on first use. Empty/unknown id falls back to the grid.
    uint32_t resolveMaterial(const std::string& id, x3::rhi::IRenderDevice& device, float outTint[3]);

    // ---- Feature 3: model browser (GLB props) -------------------------------
    // One loaded GLB: its drawables (handle-resolved, node-TRS baked). Cached by
    // relpath so repeated placements of the same prop share one GPU upload.
    struct LoadedModel {
        x3::asset::Model                      model;
        std::vector<x3::asset::ModelDrawable> drawables;
        bool ok = false;
    };
    std::unique_ptr<x3::asset::IAssetSource> m_modelAssets;  // mounts converted_glb
    std::unique_ptr<x3::asset::IModelLoader> m_modelLoader;  // bound to the device
    std::unordered_map<std::string, LoadedModel> m_modelCache;
    bool m_modelDirMounted = false;
    // Lazily mount the converted_glb dir + create the loader (first model placement).
    void ensureModelLoader(x3::rhi::IRenderDevice& device);
    // Load + cache a GLB by relpath; returns the cached entry (ok=false on failure).
    const LoadedModel* loadModelCached(const std::string& relPath, x3::rhi::IRenderDevice& device);

    // Fly-cam state (Edit mode). pitch clamped +-1.55. Seeded near the world origin.
    float m_camX = 6.0f, m_camY = 4.0f, m_camZ = 10.0f;
    float m_camYaw = -2.2f, m_camPitch = -0.25f;
    bool  m_rmbPrev = false;             // RMB held last frame (mouse-look gate)
    double m_lastMouseX = 0.0, m_lastMouseY = 0.0;
    bool  m_f8Prev = false;              // F8 rising-edge

    // Grid snap presets (1 / 0.5 / 0.25 m). The Blockout panel picks one.
    int   m_gridSel = 1;                 // index into kGridSteps
    // Which brush type the "Add" buttons spawn next (0=Box, 1=Ramp).
    uint32_t m_addType = 0;

    // ---- P3 gizmo drag state ------------------------------------------------
    // The current tool (Q/W/E/R). P3 ships the MOVE gizmo as the interactive one;
    // Rotate/Scale are reachable via the Details panel fields (also undoable).
    Tool  m_tool = Tool::Move;
    // Active axis drag: which axis is grabbed, and the cursor's signed distance along
    // the projected axis at grab time (so we move by the delta, not absolute).
    Axis  m_dragAxis  = Axis::None;
    bool  m_dragging  = false;
    float m_dragStartS = 0.0f;           // screen-space param at grab
    float m_dragBaseM  = 0.0f;           // brush coord (pos OR size) on the axis at grab
    // Feature 2 ROTATE drag: the brush yaw + the cursor's angle about the gizmo origin
    // at grab time, so a Rotate drag adds (current angle - start angle) to the base yaw.
    float m_dragBaseYaw  = 0.0f;
    float m_dragStartAng = 0.0f;
    bool  m_lmbPrev    = false;          // LMB held last frame (rising/falling edge)
    // Last gizmo HistoryEffect produced by a commit, applied next frame by draw().
    bool  m_ctrlZPrev = false, m_ctrlYPrev = false;  // Ctrl+Z / Ctrl+Y rising edge

    // ---- Phase 5: keyboard nudge / cheat-sheet / rebind ---------------------
    KeybindTable m_keybinds;                 // the rebindable {action -> key} table
    bool   m_tooltipVisible = true;          // floating cheat-sheet shown (H toggles)
    // Rising-edge tracking for each bound key (so a held key fires once per press, while
    // the wheel — inherently per-tick — fires per notch). Indexed by NudgeAction.
    bool   m_nudgePrev[(int)NudgeAction::Count] = {};
    NudgeAction m_lastNudge = NudgeAction::Count;   // for the status readout
    int    m_lastFaceAxis = 2;               // last resolved faced axis (for the readout)
    // Rebind capture: when set, the next key the user presses rebinds this action.
    bool        m_rebinding = false;
    NudgeAction m_rebindAction = NudgeAction::Count;
};

} // namespace x3::editor
