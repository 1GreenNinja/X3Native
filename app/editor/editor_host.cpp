// Level Architect — EDITOR HOST. See editor_host.h.
//
// Submits the ImGui panels (the device wraps NewFrame/Render around draw()), runs
// the edit-mode fly-cam, and bridges the headless brushes[] list into the live
// Scene + Jolt so the greybox is walkable in Play mode.
#include "editor_host.h"

#include "../mesh_prims.h"
#include "../scene.h"
#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::editor {

namespace {
// Grid-snap presets the Blockout panel cycles. Index by EditorHost::m_gridSel.
const float kGridSteps[] = { 1.0f, 0.5f, 0.25f };
const char* kGridLabels[] = { "1 m", "0.5 m", "0.25 m" };

// Build a column-major model matrix from an origin-centered brush's pos + yaw.
void brushMatrix(const BlockoutBrush& b, float m[16]) {
    const float c = std::cos(b.yaw), s = std::sin(b.yaw);
    m[0]=c;    m[1]=0; m[2]=-s;   m[3]=0;
    m[4]=0;    m[5]=1; m[6]=0;    m[7]=0;
    m[8]=s;    m[9]=0; m[10]=c;   m[11]=0;
    m[12]=b.pos[0]; m[13]=b.pos[1]; m[14]=b.pos[2]; m[15]=1;
}
} // namespace

// ---------------------------------------------------------------------------
void EditorHost::init(x3::rhi::IRenderDevice& device, x3::game::Scene& /*scene*/,
                      x3::phys::IPhysicsWorld& /*physics*/, GLFWwindow* window) {
    if (m_inited) return;
    m_window = window;
    // Create the cached CLEAN grid material once (sRGB color, one texture per session
    // shared by every brush). Tasteful low-contrast 1 m / 5 m lines.
    {
        const uint32_t n = 1000;   // 100 px per metre, 10 m span -> seamless 5 m majors
        std::vector<uint8_t> px = x3::prims::makeBlockoutGridRGBA(n, /*meters*/10);
        x3::rhi::TextureHandle h = device.createTexture(px.data(), n, n, /*srgb*/true);
        m_gridTex = h.id;
    }
    m_state.setSnap(true, kGridSteps[m_gridSel]);
    m_inited = true;
    x3::logInfo("[editor-host] session init (grid material cached, blockout ready)");
}

// ---------------------------------------------------------------------------
bool EditorHost::tick(float dt, bool wantMouse, bool wantKbd,
                      x3::rhi::IRenderDevice& device) {
    if (!m_window) return false;

    // F8 toggles Edit<->Play (polled even when a panel has keyboard focus so it's
    // always reachable; it's not a typing key).
    {
        const bool f8 = glfwGetKey(m_window, GLFW_KEY_F8) == GLFW_PRESS;
        if (f8 && !m_f8Prev) {
            m_mode = (m_mode == HostMode::Edit) ? HostMode::Play : HostMode::Edit;
            x3::logInfo(m_mode == HostMode::Edit
                ? "[editor-host] EDIT mode (fly-cam + panels)"
                : "[editor-host] PLAY mode (game input; panels hidden)");
        }
        m_f8Prev = f8;
    }

    // In Play mode the game owns the camera + input; the host does nothing.
    if (m_mode == HostMode::Play) { m_rmbPrev = false; return false; }

    // ---- EDIT-mode fly-cam: WASD move + mouse-look while RMB held. ALL camera
    // input is gated on the device's editorWantsInput so hovering/clicking a panel
    // never moves the camera. ----
    double mx = 0, my = 0;
    glfwGetCursorPos(m_window, &mx, &my);
    const bool rmb = !wantMouse &&
                     glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rmb && m_rmbPrev) {
        const float sens = 0.0030f;
        m_camYaw   += (float)(mx - m_lastMouseX) * sens;
        m_camPitch -= (float)(my - m_lastMouseY) * sens;
        if (m_camPitch >  1.55f) m_camPitch =  1.55f;
        if (m_camPitch < -1.55f) m_camPitch = -1.55f;
    }
    m_lastMouseX = mx; m_lastMouseY = my;
    m_rmbPrev = rmb;

    // WASD/QE move along the look basis (only when not typing in a panel).
    const float fx = std::cos(m_camPitch) * std::cos(m_camYaw);
    const float fy = std::sin(m_camPitch);
    const float fz = std::cos(m_camPitch) * std::sin(m_camYaw);
    float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
    const float rx = -fz / rl, rz = fx / rl;
    if (!wantKbd) {
        float spd = 7.0f * dt;
        if (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) spd *= 3.0f;
        auto down = [&](int k){ return glfwGetKey(m_window, k) == GLFW_PRESS; };
        if (down(GLFW_KEY_W)) { m_camX += fx*spd; m_camY += fy*spd; m_camZ += fz*spd; }
        if (down(GLFW_KEY_S)) { m_camX -= fx*spd; m_camY -= fy*spd; m_camZ -= fz*spd; }
        if (down(GLFW_KEY_D)) { m_camX += rx*spd; m_camZ += rz*spd; }
        if (down(GLFW_KEY_A)) { m_camX -= rx*spd; m_camZ -= rz*spd; }
        if (down(GLFW_KEY_E) || down(GLFW_KEY_SPACE)) m_camY += spd;
        if (down(GLFW_KEY_Q) || down(GLFW_KEY_LEFT_CONTROL)) m_camY -= spd;
    }
    device.setCamera(m_camX, m_camY, m_camZ, m_camYaw, m_camPitch, 65.0f);
    return true;   // the host drove the camera this frame
}

// ---------------------------------------------------------------------------
void EditorHost::spawnBrush(int idx, x3::rhi::IRenderDevice& device,
                            x3::game::Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (idx < 0 || idx >= (int)m_doc.brushes.size()) return;
    BlockoutBrush& b = m_doc.brushes[idx];
    const auto type = (b.type == 1u) ? x3::prims::BrushType::Ramp : x3::prims::BrushType::Box;
    x3::prims::PrimMesh pm = x3::prims::buildBrushMesh(type, b.size);

    x3::rhi::MeshHandle mesh = device.createMesh(
        pm.verts.data(), (uint32_t)pm.verts.size(),
        pm.index.data(), (uint32_t)pm.index.size());

    x3::game::Entity e;
    e.mesh = mesh;
    e.tex  = x3::rhi::TextureHandle{ m_gridTex };
    e.baseColor[0] = b.tint[0]; e.baseColor[1] = b.tint[1];
    e.baseColor[2] = b.tint[2]; e.baseColor[3] = 1.0f;
    e.tag = (uint32_t)x3::game::Tag::Static;
    brushMatrix(b, e.transform);
    b.sceneEntity = scene.add(e);

    // Collision: a Box uses an AABB box body (cheap). A Ramp uses a static MESH (an
    // AABB would make the slope un-walkable — you'd only reach the bbox top).
    b.body = 0;
    if (b.collide) {
        x3::phys::Vec3 pos{ b.pos[0], b.pos[1], b.pos[2] };
        if (type == x3::prims::BrushType::Box) {
            x3::phys::Vec3 he{ b.size[0]*0.5f, b.size[1]*0.5f, b.size[2]*0.5f };
            x3::phys::BodyId bid = physics.addBox(he, pos, 0.0f, x3::phys::Layer::Static);
            b.body = bid.id;
        } else {
            // The Ramp collision verts are origin-centered (local); offset them to the
            // brush world position so the static mesh sits under the rendered wedge.
            // (Yaw on a ramp is a P3 refinement; axis-aligned ramps are the P2 core.)
            std::vector<float> cv = pm.cverts;
            for (size_t i = 0; i + 2 < cv.size(); i += 3) {
                cv[i+0] += b.pos[0]; cv[i+1] += b.pos[1]; cv[i+2] += b.pos[2];
            }
            x3::phys::BodyId bid = physics.addStaticMesh(
                cv.data(), (uint32_t)(cv.size()/3),
                pm.cindex.data(), (uint32_t)pm.cindex.size());
            b.body = bid.id;
        }
    }
}

void EditorHost::respawnBrush(int idx, x3::rhi::IRenderDevice& device,
                              x3::game::Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Resize path: destroy the mesh + remove the body, then rebuild both (safer than
    // updateMesh, whose changing-vertex-count realloc semantics are unverified).
    if (idx < 0 || idx >= (int)m_doc.brushes.size()) return;
    BlockoutBrush& b = m_doc.brushes[idx];
    if (b.sceneEntity != 0xFFFFFFFFu && b.sceneEntity < scene.size()) {
        x3::game::Entity& e = scene.get(b.sceneEntity);
        if (e.mesh.valid()) device.destroyMesh(e.mesh);
        e.mesh = x3::rhi::MeshHandle{};
        e.visible = false;                 // hide the dead slot; a fresh entity replaces it
    }
    if (b.body) { physics.removeBody(x3::phys::BodyId{ b.body }); b.body = 0; }
    spawnBrush(idx, device, scene, physics);
}

void EditorHost::syncBrushTransform(int idx, x3::game::Scene& scene,
                                    x3::phys::IPhysicsWorld& physics) {
    if (idx < 0 || idx >= (int)m_doc.brushes.size()) return;
    BlockoutBrush& b = m_doc.brushes[idx];
    if (b.sceneEntity != 0xFFFFFFFFu && b.sceneEntity < scene.size())
        brushMatrix(b, scene.get(b.sceneEntity).transform);
    if (b.body) physics.setBodyPosition(x3::phys::BodyId{ b.body },
                                        x3::phys::Vec3{ b.pos[0], b.pos[1], b.pos[2] });
}

void EditorHost::teardownLinks(uint32_t sceneEntity, uint32_t body,
                               x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                               x3::phys::IPhysicsWorld& physics) {
    if (sceneEntity != 0xFFFFFFFFu && sceneEntity < scene.size()) {
        x3::game::Entity& e = scene.get(sceneEntity);
        if (e.mesh.valid()) device.destroyMesh(e.mesh);
        e.mesh = x3::rhi::MeshHandle{}; e.visible = false;
    }
    if (body) physics.removeBody(x3::phys::BodyId{ body });
}

void EditorHost::applyEffect(const HistoryEffect& eff, x3::rhi::IRenderDevice& device,
                             x3::game::Scene& scene, x3::phys::IPhysicsWorld& physics) {
    using Op = HistoryEffect::Op;
    if (eff.op == Op::None) return;
    if (eff.removed) {
        // The brush record is gone (undo-of-add / redo-of-delete): tear down its links.
        teardownLinks(eff.deadSceneEntity, eff.deadBody, device, scene, physics);
        return;
    }
    if (eff.op == Op::Respawn) {
        // The brush exists with cleared links (restored) OR with stale ones (size
        // change). respawnBrush handles both: it destroys any live mesh/body first.
        respawnBrush(eff.index, device, scene, physics);
    } else if (eff.op == Op::SyncXform) {
        syncBrushTransform(eff.index, scene, physics);
    }
}

int EditorHost::placeBrush(uint32_t type, const float pos[3], const float size[3],
                           x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                           x3::phys::IPhysicsWorld& physics) {
    int idx = m_state.addBrush(type, pos);
    if (idx < 0) return idx;
    if (size) for (int a = 0; a < 3; ++a)
        m_doc.brushes[idx].size[a] = std::max(0.25f, m_state.snapValue(size[a]));
    spawnBrush(idx, device, scene, physics);
    return idx;
}

// ---------------------------------------------------------------------------
void EditorHost::draw(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                      x3::phys::IPhysicsWorld& physics, float /*dt*/) {
    // PLAY mode: hide the panels, leave only a thin status hint.
    if (m_mode == HostMode::Play) {
        ImGui::SetNextWindowPos(ImVec2(12, 12));
        ImGui::Begin("##playhint", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(ImVec4(0.4f, 0.86f, 1.0f, 1.0f), "PLAYING  (F8: back to Edit)");
        ImGui::End();
        return;
    }

    // ---- Editor keyboard shortcuts (Edit mode). Ctrl+Z/Y undo/redo, Q/W tool. Only
    // when ImGui is NOT capturing the keyboard (so typing in a field never fires). ----
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool kbdFree = !io.WantCaptureKeyboard && m_window;
        auto down = [&](int k){ return m_window && glfwGetKey(m_window, k) == GLFW_PRESS; };
        const bool ctrl = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
        if (kbdFree && ctrl) {
            const bool z = down(GLFW_KEY_Z), y = down(GLFW_KEY_Y);
            if (z && !m_ctrlZPrev) applyEffect(m_state.undo(), device, scene, physics);
            if (y && !m_ctrlYPrev) applyEffect(m_state.redo(), device, scene, physics);
            m_ctrlZPrev = z; m_ctrlYPrev = y;
        } else { m_ctrlZPrev = m_ctrlYPrev = false; }
        if (kbdFree && !ctrl) {
            if (down(GLFW_KEY_Q)) m_tool = Tool::Select;
            if (down(GLFW_KEY_W)) m_tool = Tool::Move;
        }
    }

    // ---- Menu bar (File / Mode). ----
    bool doNew = false, doSave = false, doLoad = false;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Level"))  doNew = true;
            if (ImGui::MenuItem("Save", "Ctrl+S")) doSave = true;
            if (ImGui::MenuItem("Load"))       doLoad = true;
            ImGui::EndMenu();
        }
        bool doUndo = false, doRedo = false;
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_state.canUndo())) doUndo = true;
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_state.canRedo())) doRedo = true;
            ImGui::EndMenu();
        }
        if (doUndo) applyEffect(m_state.undo(), device, scene, physics);
        if (doRedo) applyEffect(m_state.redo(), device, scene, physics);
        if (ImGui::BeginMenu("Mode")) {
            if (ImGui::MenuItem("Edit", "F8", m_mode == HostMode::Edit)) m_mode = HostMode::Edit;
            if (ImGui::MenuItem("Play", "F8", m_mode == HostMode::Play)) m_mode = HostMode::Play;
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("   Level Architect  |  RMB+WASD fly  |  F8 Edit/Play  |  Q/W tool  |  LMB drag axis");
        ImGui::EndMainMenuBar();
    }

    const char* kLevelPath = "build/proof/architect_level.json";
    if (doNew)  { m_doc = LevelDoc{}; m_doc.name = "untitled"; }
    if (doSave) {
        if (m_doc.saveJson(kLevelPath)) x3::logInfo(std::string("[editor-host] saved ") + kLevelPath);
    }
    if (doLoad) {
        LevelDoc tmp;
        if (tmp.loadJson(kLevelPath)) {
            m_doc = tmp;
            // Respawn the loaded brushes into the live scene + Jolt.
            for (int i = 0; i < (int)m_doc.brushes.size(); ++i) {
                m_doc.brushes[i].sceneEntity = 0xFFFFFFFFu; m_doc.brushes[i].body = 0;
                spawnBrush(i, device, scene, physics);
            }
            x3::logInfo(std::string("[editor-host] loaded ") + kLevelPath);
        }
    }

    // ---- Outliner (scene entities + blockout brushes; click to select). ----
    ImGui::Begin("Outliner");
    ImGui::TextDisabled("Brushes (%d)", (int)m_doc.brushes.size());
    for (int i = 0; i < (int)m_doc.brushes.size(); ++i) {
        const BlockoutBrush& b = m_doc.brushes[i];
        const bool sel = (m_state.selKind() == SelKind::Brush && m_state.selIndex() == i);
        char label[96];
        std::snprintf(label, sizeof(label), "%s  [%s]##b%d",
                      b.name.empty() ? "(brush)" : b.name.c_str(),
                      b.type == 1u ? "Ramp" : "Box", i);
        if (ImGui::Selectable(label, sel)) m_state.selectBrush(i);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Entities (%d)", (int)m_doc.entities.size());
    for (int i = 0; i < (int)m_doc.entities.size(); ++i) {
        const EditorEntity& e = m_doc.entities[i];
        const bool sel = (m_state.selKind() == SelKind::Entity && m_state.selIndex() == i);
        char label[96];
        std::snprintf(label, sizeof(label), "%s  [%s]##e%d",
                      e.name.empty() ? "(entity)" : e.name.c_str(), e.type.c_str(), i);
        if (ImGui::Selectable(label, sel)) m_state.select(i);
    }
    ImGui::End();

    // ---- Blockout panel (tools + selected-brush numeric edit). ----
    ImGui::Begin("Blockout");

    // Grid snap dropdown.
    if (ImGui::BeginCombo("Grid snap", kGridLabels[m_gridSel])) {
        for (int i = 0; i < (int)(sizeof(kGridSteps)/sizeof(kGridSteps[0])); ++i)
            if (ImGui::Selectable(kGridLabels[i], i == m_gridSel)) {
                m_gridSel = i; m_state.setSnap(true, kGridSteps[i]);
            }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Add a brush at the camera focus:");
    // Spawn point ~6 m in front of the fly-cam, snapped.
    float focus[3] = {
        m_state.snapValue(m_camX + std::cos(m_camPitch)*std::cos(m_camYaw) * 6.0f),
        m_state.snapValue(m_camY + std::sin(m_camPitch) * 6.0f),
        m_state.snapValue(m_camZ + std::cos(m_camPitch)*std::sin(m_camYaw) * 6.0f),
    };
    if (ImGui::Button("Add Box")) {
        int idx = m_state.addBrushCmd(0u, focus);     // undoable
        if (idx >= 0) spawnBrush(idx, device, scene, physics);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Ramp")) {
        int idx = m_state.addBrushCmd(1u, focus);     // undoable
        if (idx >= 0) {
            m_doc.brushes[idx].size[0] = std::max(0.25f, m_state.snapValue(3.0f));
            m_doc.brushes[idx].size[1] = std::max(0.25f, m_state.snapValue(2.0f));
            m_doc.brushes[idx].size[2] = std::max(0.25f, m_state.snapValue(4.0f));
            spawnBrush(idx, device, scene, physics);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Add buttons are undoable (Ctrl+Z / Ctrl+Y).");
    ImGui::End();

    // ---- Details panel (P3): two-way-synced pos/yaw/size for the selection, each
    // edit grouped into ONE undo step via begin/commitBrushEdit. Shares its selection
    // and transform with the viewport gizmo. ----
    ImGui::Begin("Details");
    if (m_state.hasBrushSelection()) {
        int si = m_state.selIndex();
        BlockoutBrush& b = m_doc.brushes[si];
        ImGui::Text("Selected: %s  [%s]", b.name.c_str(), b.type == 1u ? "Ramp" : "Box");
        const float step = kGridSteps[m_gridSel];
        bool moved = false, resized = false;

        // Tool readout (drives the viewport gizmo). Q select / W move.
        ImGui::TextDisabled("Tool: %s  (Q select, W move, Ctrl+Z/Y undo/redo)",
                            m_tool == Tool::Move ? "MOVE" : m_tool == Tool::Rotate ? "ROTATE"
                            : m_tool == Tool::Scale ? "SCALE" : "SELECT");

        // Position — DragFloat3, snapped. IsItemActivated/Deactivated bracket the drag
        // into one undo command (works for a click-drag on the slider too).
        float pos[3] = { b.pos[0], b.pos[1], b.pos[2] };
        if (ImGui::DragFloat3("Position (m)", pos, step)) {
            for (int a = 0; a < 3; ++a) b.pos[a] = m_state.snapValue(pos[a]);
            moved = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        // Rotation (yaw degrees in UI, radians in doc).
        float yawDeg = b.yaw * 57.29578f;
        if (ImGui::DragFloat("Rotation Y (deg)", &yawDeg, 1.0f)) {
            b.yaw = yawDeg * 0.0174533f; moved = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        // Scale = full extents (m), snapped + clamped. A change rebuilds the mesh.
        float size[3] = { b.size[0], b.size[1], b.size[2] };
        if (ImGui::DragFloat3("Scale / Size (m)", size, step, 0.25f, 200.0f)) {
            for (int a = 0; a < 3; ++a) b.size[a] = std::max(0.25f, m_state.snapValue(size[a]));
            resized = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        if (ImGui::Checkbox("Collide", &b.collide)) resized = true;  // toggling re-adds the body

        if (ImGui::Button("Delete brush")) {
            const uint32_t se = b.sceneEntity, bo = b.body;
            if (m_state.deleteSelectedBrushCmd())                 // undoable
                teardownLinks(se, bo, device, scene, physics);
        } else {
            if (resized) respawnBrush(si, device, scene, physics);   // rebuild mesh+body
            else if (moved) syncBrushTransform(si, scene, physics);  // transform-only
        }
    } else {
        ImGui::TextDisabled("(no brush selected — pick one in the viewport or Outliner)");
    }
    ImGui::Text("Undo: %s   Redo: %s",
                m_state.canUndo() ? "available" : "-", m_state.canRedo() ? "available" : "-");
    ImGui::End();

    // ---- Status / viewport readout. ----
    ImGui::Begin("Status");
    ImGui::Text("Mode: %s", m_mode == HostMode::Edit ? "EDIT" : "PLAY");
    ImGui::Text("Cam: %.1f, %.1f, %.1f", m_camX, m_camY, m_camZ);
    ImGui::Text("Brushes: %d   Entities: %d",
                (int)m_doc.brushes.size(), (int)m_doc.entities.size());
    ImGui::Text("Grid snap: %s", kGridLabels[m_gridSel]);
    x3::rhi::RenderStats st = device.stats();
    ImGui::Text("Draw calls: %u   Tris: %u", st.drawCalls, st.triangles);
    ImGui::End();

    // ---- Viewport gizmo + click-pick (P3). Drawn last so it overlays the scene. ----
    {
        ImGuiIO& io = ImGui::GetIO();
        gizmoAndPick(device, scene, physics, io.WantCaptureMouse);
    }
}

// ---------------------------------------------------------------------------
// P3 — viewport MOVE gizmo + click-pick. Uses device->worldToScreen to project the
// brush origin + axis tips to pixels, draws the 3 axis handles on the ImGui
// foreground draw list, and drags the grabbed axis by mapping cursor motion onto the
// screen-projected axis direction. A plain click in empty space ray-picks a brush.
// Talks to the engine ONLY through IRenderDevice (worldToScreen) — engine stays pure.
// ---------------------------------------------------------------------------
void EditorHost::gizmoAndPick(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                              x3::phys::IPhysicsWorld& physics, bool wantMouse) {
    const float kAxisLen = 1.6f;             // world-metres of each gizmo arm
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool lmb = !wantMouse && (glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    const bool lmbDown = lmb && !m_lmbPrev;   // rising edge

    // Project a world point; returns false if off-screen / behind.
    auto project = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
        float sx = 0, sy = 0;
        if (!device.worldToScreen(wx, wy, wz, sx, sy)) return false;
        out = ImVec2(sx, sy); return true;
    };

    // Only the MOVE tool draws an interactive gizmo; Select still allows pick.
    const bool haveSel = m_state.hasBrushSelection();
    int si = haveSel ? m_state.selIndex() : -1;

    // ---- Active drag: move the selected brush along m_dragAxis by cursor delta. ----
    if (m_dragging) {
        if (!lmb) {
            // Release: commit the grouped edit + final physics/scene sync.
            m_state.commitBrushEdit();
            if (si >= 0) syncBrushTransform(si, scene, physics);
            m_dragging = false; m_dragAxis = Axis::None;
        } else if (si >= 0 && m_tool == Tool::Move) {
            BlockoutBrush& b = m_doc.brushes[si];
            // Re-project the axis to get its current screen direction, map the cursor's
            // movement onto it, scale by the world/screen ratio of the axis.
            ImVec2 o, tip;
            const int a = (m_dragAxis == Axis::X) ? 0 : (m_dragAxis == Axis::Y) ? 1 : 2;
            float tipW[3] = { b.pos[0], b.pos[1], b.pos[2] }; tipW[a] += kAxisLen;
            if (project(b.pos[0], b.pos[1], b.pos[2], o) &&
                project(tipW[0], tipW[1], tipW[2], tip)) {
                const float dxS = tip.x - o.x, dyS = tip.y - o.y;
                const float len2 = dxS*dxS + dyS*dyS;
                if (len2 > 1e-3f) {
                    // Signed screen param of the cursor along the axis (0=origin,1=tip).
                    const float s = ((mouse.x - o.x)*dxS + (mouse.y - o.y)*dyS) / len2;
                    const float worldDelta = (s - m_dragStartS) * kAxisLen;
                    float target = m_dragBaseM + worldDelta;
                    target = m_state.snapValue(target);
                    if (std::fabs(target - b.pos[a]) > 1e-5f) {
                        b.pos[a] = target;
                        syncBrushTransform(si, scene, physics);   // live preview
                    }
                }
            }
        }
    }

    // ---- Draw the gizmo + (when idle) hit-test axis grab on LMB-down. ----
    if (haveSel && m_tool == Tool::Move) {
        BlockoutBrush& b = m_doc.brushes[si];
        ImVec2 o;
        if (project(b.pos[0], b.pos[1], b.pos[2], o)) {
            const ImU32 colX = IM_COL32(237, 69, 76, 255);   // X red
            const ImU32 colY = IM_COL32(102, 219, 102, 255); // Y green
            const ImU32 colZ = IM_COL32(76, 140, 242, 255);  // Z blue
            const ImU32 cols[3] = { colX, colY, colZ };
            const Axis  axes[3] = { Axis::X, Axis::Y, Axis::Z };
            float grabBest = 14.0f; Axis grab = Axis::None; float grabS = 0.0f;
            for (int a = 0; a < 3; ++a) {
                float tipW[3] = { b.pos[0], b.pos[1], b.pos[2] }; tipW[a] += kAxisLen;
                ImVec2 tip;
                if (!project(tipW[0], tipW[1], tipW[2], tip)) continue;
                dl->AddLine(o, tip, cols[a], 3.0f);
                dl->AddCircleFilled(tip, 4.5f, cols[a]);
                // Distance from cursor to this axis segment (for grab pick).
                if (!m_dragging) {
                    const float dxS = tip.x - o.x, dyS = tip.y - o.y;
                    const float len2 = dxS*dxS + dyS*dyS;
                    float s = len2 > 1e-3f ? ((mouse.x-o.x)*dxS + (mouse.y-o.y)*dyS)/len2 : 0;
                    s = s < 0 ? 0 : (s > 1 ? 1 : s);
                    const float px = o.x + dxS*s, py = o.y + dyS*s;
                    const float d = std::sqrt((mouse.x-px)*(mouse.x-px) + (mouse.y-py)*(mouse.y-py));
                    if (d < grabBest) {
                        grabBest = d; grab = axes[a];
                        // Store the UNCLAMPED param so the drag tracks past the tip.
                        grabS = len2 > 1e-3f ? ((mouse.x-o.x)*dxS + (mouse.y-o.y)*dyS)/len2 : 0;
                    }
                }
            }
            // Origin handle.
            dl->AddCircleFilled(o, 5.0f, IM_COL32(255, 209, 46, 255));
            // Begin a drag if LMB pressed on an axis.
            if (lmbDown && grab != Axis::None) {
                m_dragging = true; m_dragAxis = grab; m_dragStartS = grabS;
                const int a = (grab == Axis::X) ? 0 : (grab == Axis::Y) ? 1 : 2;
                m_dragBaseM = b.pos[a];
                m_state.beginBrushEdit(si);     // group the drag into one undo step
            }
        }
    }

    // ---- Plain click in empty space (no axis grabbed) = ray-pick a brush. ----
    if (lmbDown && !m_dragging) {
        // Build the camera ray from the host's fly-cam pose (same basis as tick()).
        const float fx = std::cos(m_camPitch) * std::cos(m_camYaw);
        const float fy = std::sin(m_camPitch);
        const float fz = std::cos(m_camPitch) * std::sin(m_camYaw);
        // Pick the brush whose center is nearest the ray (mirror of EditorState::pickRay,
        // but over brushes[] + radius from the brush's own size).
        const float o3[3] = { m_camX, m_camY, m_camZ };
        const float dl3 = std::sqrt(fx*fx+fy*fy+fz*fz);
        const float dx = fx/dl3, dy = fy/dl3, dz = fz/dl3;
        int best = -1; float bestT = 1e9f;
        for (int i = 0; i < (int)m_doc.brushes.size(); ++i) {
            const BlockoutBrush& bb = m_doc.brushes[i];
            const float ox = bb.pos[0]-o3[0], oy = bb.pos[1]-o3[1], oz = bb.pos[2]-o3[2];
            const float t = ox*dx + oy*dy + oz*dz;
            if (t < 0.0f) continue;
            const float px = o3[0]+dx*t, py = o3[1]+dy*t, pz = o3[2]+dz*t;
            const float perp = std::sqrt((bb.pos[0]-px)*(bb.pos[0]-px) +
                                         (bb.pos[1]-py)*(bb.pos[1]-py) +
                                         (bb.pos[2]-pz)*(bb.pos[2]-pz));
            // Hit radius ~ the brush's largest half-extent + a little slack.
            const float r = 0.5f * std::max(bb.size[0], std::max(bb.size[1], bb.size[2])) + 0.5f;
            if (perp <= r && t < bestT) { bestT = t; best = i; }
        }
        if (best >= 0) m_state.selectBrush(best);
    }

    m_lmbPrev = lmb;
}

} // namespace x3::editor
