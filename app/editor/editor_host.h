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

#include <cstdint>
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

    LevelDoc    m_doc;
    EditorState m_state;
    HostMode    m_mode = HostMode::Edit;

    bool          m_inited   = false;
    GLFWwindow*   m_window   = nullptr;
    // Cached clean grid material (one texture shared by every brush). Created in init.
    uint32_t      m_gridTex  = 0;   // x3::rhi::TextureHandle.id (0 == none)

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
};

} // namespace x3::editor
