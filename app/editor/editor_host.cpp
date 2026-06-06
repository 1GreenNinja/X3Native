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

    // ---- Menu bar (File / Mode). ----
    bool doNew = false, doSave = false, doLoad = false;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Level"))  doNew = true;
            if (ImGui::MenuItem("Save", "Ctrl+S")) doSave = true;
            if (ImGui::MenuItem("Load"))       doLoad = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mode")) {
            if (ImGui::MenuItem("Edit", "F8", m_mode == HostMode::Edit)) m_mode = HostMode::Edit;
            if (ImGui::MenuItem("Play", "F8", m_mode == HostMode::Play)) m_mode = HostMode::Play;
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("   Level Architect  |  RMB+WASD fly  |  F8 Edit/Play");
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
        float sz[3] = { 2,2,2 };
        placeBrush(0u, focus, sz, device, scene, physics);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Ramp")) {
        float sz[3] = { 3, 2, 4 };
        placeBrush(1u, focus, sz, device, scene, physics);
    }

    ImGui::Separator();
    if (m_state.hasBrushSelection()) {
        int si = m_state.selIndex();
        BlockoutBrush& b = m_doc.brushes[si];
        ImGui::Text("Selected: %s", b.name.c_str());
        const float step = kGridSteps[m_gridSel];
        bool moved = false, resized = false;

        // Position (snapped DragFloat3).
        float pos[3] = { b.pos[0], b.pos[1], b.pos[2] };
        if (ImGui::DragFloat3("Pos (m)", pos, step)) {
            for (int a = 0; a < 3; ++a) b.pos[a] = m_state.snapValue(pos[a]);
            moved = true;
        }
        // Size (snapped, clamped to 0.25 m min).
        float size[3] = { b.size[0], b.size[1], b.size[2] };
        if (ImGui::DragFloat3("Size (m)", size, step, 0.25f, 200.0f)) {
            for (int a = 0; a < 3; ++a) b.size[a] = std::max(0.25f, m_state.snapValue(size[a]));
            resized = true;
        }
        // Yaw (degrees in the UI, radians in the doc).
        float yawDeg = b.yaw * 57.29578f;
        if (ImGui::DragFloat("Yaw (deg)", &yawDeg, 5.0f)) { b.yaw = yawDeg * 0.0174533f; moved = true; }
        if (ImGui::Checkbox("Collide", &b.collide)) resized = true;  // toggling re-adds the body

        // Quick face-grow buttons (snapped), mirroring resizeSelectedBrush.
        ImGui::TextDisabled("Grow face (+%.2g m):", step);
        if (ImGui::Button("X+")) { m_state.resizeSelectedBrush(Axis::X, step); resized = true; } ImGui::SameLine();
        if (ImGui::Button("Y+")) { m_state.resizeSelectedBrush(Axis::Y, step); resized = true; } ImGui::SameLine();
        if (ImGui::Button("Z+")) { m_state.resizeSelectedBrush(Axis::Z, step); resized = true; }

        if (ImGui::Button("Delete brush")) {
            // Tear down the live mesh/body then remove from the doc.
            if (b.sceneEntity != 0xFFFFFFFFu && b.sceneEntity < scene.size()) {
                x3::game::Entity& e = scene.get(b.sceneEntity);
                if (e.mesh.valid()) device.destroyMesh(e.mesh);
                e.mesh = x3::rhi::MeshHandle{}; e.visible = false;
            }
            if (b.body) physics.removeBody(x3::phys::BodyId{ b.body });
            m_state.deleteSelectedBrush();
        } else {
            if (resized) respawnBrush(si, device, scene, physics);   // rebuild mesh+body
            else if (moved) syncBrushTransform(si, scene, physics);  // transform-only
        }
    } else {
        ImGui::TextDisabled("(no brush selected — pick one in the Outliner)");
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
}

} // namespace x3::editor
