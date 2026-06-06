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
// Feature 1 — resolve a brush material id to a cached GPU texture + tint, baking the
// procedural texture on first use. The grid kind reuses the session grid texture; the
// rest reuse the mesh_prims sci-fi generators (same surfaces the env art uses), each
// baked ONCE and shared. Engine stays pure: only IRenderDevice::createTexture is used.
uint32_t EditorHost::resolveMaterial(const std::string& id, x3::rhi::IRenderDevice& device,
                                     float outTint[3]) {
    int mi = x3::editor::editorMaterialFind(id);
    if (mi < 0) mi = 0;                                  // unknown id -> grid default
    const BlockoutMaterial& mat = x3::editor::editorMaterials()[mi];
    if (outTint) { outTint[0]=mat.tint[0]; outTint[1]=mat.tint[1]; outTint[2]=mat.tint[2]; }
    const uint8_t k = (uint8_t)mat.tex;
    if (k == (uint8_t)MatTex::Grid) return m_gridTex;    // the cached session grid
    if (k < 8 && m_matTex[k] != 0) return m_matTex[k];   // already baked
    // Bake the procedural texture once for this kind.
    std::vector<uint8_t> px; uint32_t n = 256;
    switch (mat.tex) {
        case MatTex::Panel:      px = x3::prims::makeSciFiPanelRGBA(n, 2); break;
        case MatTex::CleanPanel: px = x3::prims::makeCleanPanelRGBA(n, 4); break;
        case MatTex::Floor:      px = x3::prims::makeFloorGrateRGBA(n, 2); break;
        case MatTex::Ceiling:    px = x3::prims::makeCeilingPanelRGBA(n, 3); break;
        case MatTex::Solid:      n = 4; px = x3::prims::makeSolidRGBA(n, 255, 255, 255); break;
        default:                 return m_gridTex;
    }
    x3::rhi::TextureHandle h = device.createTexture(px.data(), n, n, /*srgb*/true);
    if (k < 8) m_matTex[k] = h.id;
    return h.id;
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
        // Vertical fly: SPACE up / LEFT_CTRL down. (Q/E are reserved for the Q/W/E/R
        // tool hotkeys in draw(), so they no longer double as camera vertical.)
        if (down(GLFW_KEY_SPACE)) m_camY += spd;
        if (down(GLFW_KEY_LEFT_CONTROL)) m_camY -= spd;
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
    // Feature 1: resolve the brush's surface material -> cached texture + tint multiplier
    // (empty material falls back to the clean grid). The brush's own tint multiplies the
    // material tint so the Details color picker still tunes the surface.
    float matTint[3] = { 1, 1, 1 };
    uint32_t tex = resolveMaterial(b.material, device, matTint);
    e.tex  = x3::rhi::TextureHandle{ tex };
    e.baseColor[0] = b.tint[0]*matTint[0]; e.baseColor[1] = b.tint[1]*matTint[1];
    e.baseColor[2] = b.tint[2]*matTint[2]; e.baseColor[3] = 1.0f;
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
            if (down(GLFW_KEY_E)) m_tool = Tool::Rotate;
            if (down(GLFW_KEY_R)) m_tool = Tool::Scale;
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
        ImGui::TextDisabled("   Level Architect  |  RMB+WASD fly (SPACE/CTRL up-down)  |  F8 Edit/Play  |  Q/W/E/R tool  |  LMB drag gizmo");
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

    // ---- Materials palette (Feature 1: click-a-wall texturing). Click a swatch to
    // re-skin the selected brush; the change is ONE undo step + persists in the JSON. ----
    ImGui::Begin("Materials");
    if (m_state.hasBrushSelection()) {
        int si = m_state.selIndex();
        BlockoutBrush& b = m_doc.brushes[si];
        int cur = x3::editor::editorMaterialFind(b.material);
        ImGui::TextDisabled("Surface for the selected brush:");
        const BlockoutMaterial* mats = x3::editor::editorMaterials();
        const uint32_t nMat = x3::editor::editorMaterialCount();
        for (uint32_t i = 0; i < nMat; ++i) {
            const BlockoutMaterial& m = mats[i];
            // A color swatch (the material tint) + the label as a selectable row.
            ImU32 sw = IM_COL32((int)(m.tint[0]*200), (int)(m.tint[1]*200), (int)(m.tint[2]*200), 255);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x+16, p0.y+16), sw, 3.0f);
            ImGui::Dummy(ImVec2(20, 16)); ImGui::SameLine();
            char lbl[64]; std::snprintf(lbl, sizeof(lbl), "%s##mat%u", m.label, i);
            if (ImGui::Selectable(lbl, (int)i == cur)) {
                if ((int)i != cur) {
                    m_state.beginBrushEdit(si);     // group into one undo step
                    b.material = m.id;
                    m_state.commitBrushEdit();
                    respawnBrush(si, device, scene, physics);   // re-skin (texture+tint)
                }
            }
        }
        ImGui::Separator();
        // A tint multiplier so the same texture can be recolored per brush (also undoable).
        float tint[3] = { b.tint[0], b.tint[1], b.tint[2] };
        if (ImGui::ColorEdit3("Tint", tint, ImGuiColorEditFlags_NoInputs)) {
            for (int a = 0; a < 3; ++a) b.tint[a] = tint[a];
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_state.commitBrushEdit();
            respawnBrush(si, device, scene, physics);
        }
    } else {
        ImGui::TextDisabled("(select a brush to assign its surface material)");
    }
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
// Feature 2 — viewport transform gizmos (Move/Rotate/Scale) + AABB-raycast pick.
// Projects the brush origin + axis tips to pixels (device->worldToScreen), draws the
// active tool's handles on the ImGui foreground draw list, and drags the grabbed axis
// by mapping cursor motion onto the screen-projected axis. Handle LENGTH is camera-
// distance-scaled so it stays a roughly constant screen size. A plain click ray-picks
// the brush whose ORIENTED BOX the ray enters (EditorState::pickBrushRay — tight on
// long thin brushes). Engine stays pure: only worldToScreen is used.
// ---------------------------------------------------------------------------
void EditorHost::gizmoAndPick(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                              x3::phys::IPhysicsWorld& physics, bool wantMouse) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool lmb = !wantMouse && (glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    const bool lmbDown = lmb && !m_lmbPrev;   // rising edge

    auto project = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
        float sx = 0, sy = 0;
        if (!device.worldToScreen(wx, wy, wz, sx, sy)) return false;
        out = ImVec2(sx, sy); return true;
    };

    const bool haveSel = m_state.hasBrushSelection();
    int si = haveSel ? m_state.selIndex() : -1;
    // The Select tool draws no gizmo; the other three do (Move/Rotate/Scale).
    const bool gizmoTool = (m_tool == Tool::Move || m_tool == Tool::Rotate || m_tool == Tool::Scale);

    // Camera-distance-scaled handle arm length: keep the gizmo a roughly constant
    // SCREEN size by scaling its world length with the camera->brush distance. Without
    // this the handles shrink to nothing when you fly away from the brush.
    float kAxisLen = 1.6f;
    if (haveSel) {
        const BlockoutBrush& b = m_doc.brushes[si];
        const float ddx = b.pos[0]-m_camX, ddy = b.pos[1]-m_camY, ddz = b.pos[2]-m_camZ;
        const float dist = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
        kAxisLen = std::max(0.6f, dist * 0.16f);   // ~constant on-screen handle size
    }

    // ---- Active drag: apply the cursor delta to the grabbed axis per the tool. ----
    if (m_dragging) {
        if (!lmb || si < 0) {
            m_state.commitBrushEdit();
            if (si >= 0) syncBrushTransform(si, scene, physics);
            m_dragging = false; m_dragAxis = Axis::None;
        } else {
            BlockoutBrush& b = m_doc.brushes[si];
            const int a = (m_dragAxis == Axis::X) ? 0 : (m_dragAxis == Axis::Y) ? 1 : 2;
            // Re-project the axis arm to get its current screen direction; map the
            // cursor onto it (signed param, 0=origin .. 1=tip) -> a world delta.
            ImVec2 o, tip;
            float tipW[3] = { b.pos[0], b.pos[1], b.pos[2] }; tipW[a] += kAxisLen;
            if (project(b.pos[0], b.pos[1], b.pos[2], o) &&
                project(tipW[0], tipW[1], tipW[2], tip)) {
                const float dxS = tip.x - o.x, dyS = tip.y - o.y;
                const float len2 = dxS*dxS + dyS*dyS;
                if (len2 > 1e-3f) {
                    const float s = ((mouse.x - o.x)*dxS + (mouse.y - o.y)*dyS) / len2;
                    const float worldDelta = (s - m_dragStartS) * kAxisLen;
                    bool changed = false;
                    if (m_tool == Tool::Move) {
                        float target = m_state.snapValue(m_dragBaseM + worldDelta);
                        if (std::fabs(target - b.pos[a]) > 1e-5f) { b.pos[a] = target; changed = true; }
                    } else if (m_tool == Tool::Scale) {
                        // Grow the extent on this axis by the drag delta (snap + min clamp).
                        float target = std::max(0.25f, m_state.snapValue(m_dragBaseM + worldDelta));
                        if (std::fabs(target - b.size[a]) > 1e-5f) { b.size[a] = target; changed = true; }
                    } // Rotate is handled below via the angular ring mapping.
                    if (changed) {
                        if (m_tool == Tool::Scale) respawnBrush(si, device, scene, physics); // mesh rebuild
                        else                       syncBrushTransform(si, scene, physics);   // transform only
                    }
                }
            }
            // Rotate uses a SEPARATE mapping (angle from cursor about the screen origin),
            // handled here so it doesn't depend on a single axis arm projection.
            if (m_tool == Tool::Rotate) {
                ImVec2 oc;
                if (project(b.pos[0], b.pos[1], b.pos[2], oc)) {
                    const float ang = std::atan2(mouse.y - oc.y, mouse.x - oc.x);
                    float dyaw = ang - m_dragStartAng;
                    float newYaw = m_dragBaseYaw + dyaw;
                    // Snap yaw to 5-degree increments when grid-snap is on.
                    if (m_state.snapEnabled()) {
                        const float step = 5.0f * 0.0174533f;
                        newYaw = std::round(newYaw / step) * step;
                    }
                    if (std::fabs(newYaw - b.yaw) > 1e-5f) {
                        b.yaw = newYaw;
                        syncBrushTransform(si, scene, physics);
                    }
                }
            }
        }
    }

    // ---- Draw the active tool's gizmo + hit-test the axis grab on LMB-down. ----
    if (haveSel && gizmoTool) {
        BlockoutBrush& b = m_doc.brushes[si];
        ImVec2 o;
        if (project(b.pos[0], b.pos[1], b.pos[2], o)) {
            const ImU32 cols[3] = { IM_COL32(237,69,76,255), IM_COL32(102,219,102,255), IM_COL32(76,140,242,255) };
            const Axis  axes[3] = { Axis::X, Axis::Y, Axis::Z };
            float grabBest = 14.0f; Axis grab = Axis::None; float grabS = 0.0f;

            if (m_tool == Tool::Rotate) {
                // A single yaw ring (about +Y) in the brush's tool-accent pink.
                dl->AddCircle(o, std::max(24.0f, kAxisLen * 22.0f), IM_COL32(255,136,170,255), 48, 2.5f);
                // Grab anywhere near the ring band.
                if (!m_dragging) {
                    const float rad = std::max(24.0f, kAxisLen * 22.0f);
                    const float dd = std::sqrt((mouse.x-o.x)*(mouse.x-o.x) + (mouse.y-o.y)*(mouse.y-o.y));
                    if (std::fabs(dd - rad) < 12.0f) grab = Axis::Y;
                }
            } else {
                // Move / Scale: three axis arms. Scale draws a box at the tip, Move a dot.
                for (int a = 0; a < 3; ++a) {
                    float tipW[3] = { b.pos[0], b.pos[1], b.pos[2] }; tipW[a] += kAxisLen;
                    ImVec2 tip;
                    if (!project(tipW[0], tipW[1], tipW[2], tip)) continue;
                    dl->AddLine(o, tip, cols[a], 3.0f);
                    if (m_tool == Tool::Scale)
                        dl->AddRectFilled(ImVec2(tip.x-5,tip.y-5), ImVec2(tip.x+5,tip.y+5), cols[a]);
                    else
                        dl->AddCircleFilled(tip, 4.5f, cols[a]);
                    if (!m_dragging) {
                        const float dxS = tip.x - o.x, dyS = tip.y - o.y;
                        const float len2 = dxS*dxS + dyS*dyS;
                        float s = len2 > 1e-3f ? ((mouse.x-o.x)*dxS + (mouse.y-o.y)*dyS)/len2 : 0;
                        s = s < 0 ? 0 : (s > 1 ? 1 : s);
                        const float px = o.x + dxS*s, py = o.y + dyS*s;
                        const float d = std::sqrt((mouse.x-px)*(mouse.x-px) + (mouse.y-py)*(mouse.y-py));
                        if (d < grabBest) {
                            grabBest = d; grab = axes[a];
                            grabS = len2 > 1e-3f ? ((mouse.x-o.x)*dxS + (mouse.y-o.y)*dyS)/len2 : 0;
                        }
                    }
                }
            }
            dl->AddCircleFilled(o, 5.0f, IM_COL32(255, 209, 46, 255));   // origin handle

            if (lmbDown && grab != Axis::None) {
                m_dragging = true; m_dragAxis = grab; m_dragStartS = grabS;
                const int a = (grab == Axis::X) ? 0 : (grab == Axis::Y) ? 1 : 2;
                m_dragBaseM   = (m_tool == Tool::Scale) ? b.size[a] : b.pos[a];
                m_dragBaseYaw = b.yaw;
                m_dragStartAng = std::atan2(mouse.y - o.y, mouse.x - o.x);
                m_state.beginBrushEdit(si);     // group the drag into one undo step
            }
        }
    }

    // ---- Plain click (no handle grabbed) = ray-pick the brush under the cursor. ----
    if (lmbDown && !m_dragging) {
        const float fx = std::cos(m_camPitch) * std::cos(m_camYaw);
        const float fy = std::sin(m_camPitch);
        const float fz = std::cos(m_camPitch) * std::sin(m_camYaw);
        const float o3[3] = { m_camX, m_camY, m_camZ };
        const float d3[3] = { fx, fy, fz };
        int best = m_state.pickBrushRay(o3, d3, /*pad*/0.1f);   // true OBB raycast
        if (best >= 0) m_state.selectBrush(best);
    }

    m_lmbPrev = lmb;
}

} // namespace x3::editor
