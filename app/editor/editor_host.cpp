// Level Architect — EDITOR HOST. See editor_host.h.
//
// Submits the ImGui panels (the device wraps NewFrame/Render around draw()), runs
// the edit-mode fly-cam, and bridges the headless brushes[] list into the live
// Scene + Jolt so the greybox is walkable in Play mode.
#include "editor_host.h"
#include "editor_armory.h"

#include "../asset_root.h"
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

// ---------------------------------------------------------------------------
// DEFAULT PANEL LAYOUT.
//
// Every ImGui window defaults to the same top-left corner, so the editor opened with
// SEVEN panels stacked on top of one another and Keybinds burying the rest — you had
// to drag five windows apart before you could see the level. A tool that needs to be
// tidied up before it can be used is a tool people stop using.
//
// ImGuiCond_FirstUseEver means this is a STARTING layout, not a cage: it seeds the
// first run, and after that imgui.ini remembers wherever the user dragged things.
// Positions are derived from the live viewport, so it lays out correctly at any
// resolution instead of hard-coding 1280x720.
// ---------------------------------------------------------------------------
namespace {
// The BOTTOM-RIGHT quadrant is deliberately left free: the Visual-Mode nudge
// cheat-sheet (##nudgecheat) is an overlay that lives there and cannot be dragged, so
// any dockable panel seeded into it is permanently half-buried.
void panelRect(float nx, float ny, float nw, float nh) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 o = vp->WorkPos;
    const ImVec2 s = vp->WorkSize;
    ImGui::SetNextWindowPos (ImVec2(o.x + s.x * nx, o.y + s.y * ny), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(s.x * nw,       s.y * nh),       ImGuiCond_FirstUseEver);
}
} // namespace

// ---------------------------------------------------------------------------
// THE AI ARCHITECT — panel + async generation.
//
// The editor must stay at frame rate while a 3B model thinks, so generation is
// ILlmSystem::submit() (async) + poll() (non-blocking, drained once per frame). The
// panel NEVER mutates the level directly: it shows the validated plan and waits for an
// explicit Apply. And it must degrade gracefully with no .gguf on disk — an editor
// that becomes unusable because a model file is missing is a broken editor.
// ---------------------------------------------------------------------------
void EditorHost::aiEnsureModel() {
    if (m_aiTried) return;               // one attempt per session, not per frame
    m_aiTried = true;
    m_llm = x3::llm::createLlmSystem();
    if (!m_llm) { m_aiStatus = "no LLM backend compiled in"; return; }
    const std::string gguf = x3::game::assetRoot() + "/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf";
    x3::llm::ModelOpts opts;
    opts.contextTokens   = 4096;   // the level description + the schema is not small
    opts.maxOutputTokens = 768;    // a room is ~6 ops; give it room to finish the JSON
    opts.temperature     = 0.2f;   // this is a STRUCTURED-OUTPUT task, not a poem
    opts.seed            = 1;      // reproducible plans for the same sentence
    if (!m_llm->loadModel(gguf, opts)) {
        m_llm.reset();
        m_aiStatus = "model not found: assets/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf";
        return;
    }
    m_aiChat = m_llm->startChat(aiSystemPrompt());
    m_aiStatus = (m_aiChat != x3::llm::kInvalidChat) ? "ready" : "could not open a chat";
}

void EditorHost::aiSubmit() {
    aiEnsureModel();
    if (!m_llm || m_aiChat == x3::llm::kInvalidChat) return;
    if (m_aiBusy) return;
    // Hand the model the CURRENT level, because every index it cites is an index into
    // this description. Context and contract are the same object.
    const std::string user =
        describeLevel(m_doc, m_state.hasBrushSelection() ? m_state.selIndex() : -1) +
        "\nREQUEST: " + m_aiPrompt + "\nReply with the JSON plan only.";
    m_aiRaw.clear();
    m_aiErr.clear();
    m_aiHavePlan = false;
    m_aiPlan = AiPlan{};
    if (m_llm->submit(m_aiChat, user)) {
        m_aiBusy = true;
        m_aiStatus = "thinking...";
    } else {
        m_aiStatus = "the model is busy";
    }
}

void EditorHost::aiPoll() {
    if (!m_aiBusy || !m_llm) return;
    const x3::llm::PollResult r = m_llm->poll(m_aiChat);
    m_aiRaw += r.newTokens;
    if (!r.done) return;
    m_aiBusy = false;
    if (r.failed) { m_aiStatus = "generation failed"; return; }
    // The reply is complete: parse + VALIDATE. Nothing touches the level yet.
    m_aiHavePlan = parseAiPlan(m_aiRaw, m_aiPlan, m_aiErr);
    if (m_aiHavePlan) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "plan ready: %d op(s)%s", (int)m_aiPlan.ops.size(),
                      m_aiErr.empty() ? "" : "  (some ops rejected)");
        m_aiStatus = buf;
    } else {
        m_aiStatus = "the model did not produce a usable plan";
    }
}

// ---------------------------------------------------------------------------
// THE ARMORY PANEL — every mesh in the library, searchable, one click to place.
//
// 11,000+ rows means two rules:
//   * NEVER build 11,000 ImGui widgets. ImGuiListClipper draws only the visible rows.
//   * The filter caps its own result, so a single-letter search does not try to render
//     the entire library while you are still typing.
// ---------------------------------------------------------------------------
void EditorHost::drawArmoryPanel(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                                 x3::phys::IPhysicsWorld& physics) {
    (void)scene; (void)physics;
    panelRect(0.190f, 0.145f, 0.300f, 0.495f);   // BELOW Status (0.045..0.135), ABOVE the AI panel (0.655)
    ImGui::Begin("Armory");

    ensureModelLoader(device);      // also loads + mounts the index (first use)

    if (!m_armory.ok) {
        ImGui::TextWrapped("No asset library index.");
        ImGui::TextDisabled("%s", m_armory.error.c_str());
        ImGui::TextDisabled("Set X3_ARMORY_ROOT, or run the Armory indexer.");
        ImGui::End();
        return;
    }

    ImGui::Text("%d meshes  /  %d packs",
                (int)m_armory.items.size(), (int)m_armory.packs.size());
    if (!m_armoryMounted)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "root NOT mounted - cannot place");

    // ---- pack filter ----
    {
        std::string preview = (m_armoryPackSel == 0 || m_armoryPackSel > (int)m_armory.packs.size())
                            ? std::string("All packs")
                            : m_armory.packs[m_armoryPackSel - 1];
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##armpack", preview.c_str())) {
            if (ImGui::Selectable("All packs", m_armoryPackSel == 0)) m_armoryPackSel = 0;
            for (int i = 0; i < (int)m_armory.packs.size(); ++i) {
                const bool sel = (m_armoryPackSel == i + 1);
                if (ImGui::Selectable(m_armory.packs[i].c_str(), sel)) m_armoryPackSel = i + 1;
            }
            ImGui::EndCombo();
        }
    }

    // ---- search ----
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##armsearch", "search  (e.g. console, sewer, crate, neon)",
                             m_armorySearch, sizeof m_armorySearch);

    const std::string pack = (m_armoryPackSel > 0 && m_armoryPackSel <= (int)m_armory.packs.size())
                           ? m_armory.packs[m_armoryPackSel - 1] : std::string();
    // The cap is the UI's, not the library's: you cannot read 11,000 rows, and building
    // them costs a frame. Narrow the search instead — that is what the search is FOR.
    const std::vector<uint32_t> hits = filterArmory(m_armory, m_armorySearch, pack, 600);
    ImGui::TextDisabled("%d shown%s", (int)hits.size(),
                        hits.size() >= 600 ? "  (capped - narrow the search)" : "");

    ImGui::Separator();
    if (ImGui::BeginChild("##armlist", ImVec2(0, 0), false)) {
        ImGuiListClipper clip;
        clip.Begin((int)hits.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const ArmoryItem& it = m_armory.items[hits[(size_t)r]];
                char lbl[192];
                std::snprintf(lbl, sizeof lbl, "%s##arm%d", it.name.c_str(), r);
                if (ImGui::Selectable(lbl, false)) {
                    const int idx = placeModel(it.relPath, device);
                    if (idx >= 0) m_state.select(idx);
                    else x3::logWarn("[armory] failed to place: " + it.relPath);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(it.pack.c_str());
                    ImGui::TextDisabled("%s", it.relPath.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        clip.End();
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorHost::drawAiPanel(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                             x3::phys::IPhysicsWorld& physics) {
    aiPoll();      // drain tokens once per frame; never blocks

    panelRect(0.190f, 0.655f, 0.420f, 0.335f);
    ImGui::Begin("AI Architect");
    ImGui::TextWrapped("Describe a change. The model proposes a PLAN; nothing is applied "
                       "until you press Apply, and Apply is ONE undo step.");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    const bool entered = ImGui::InputTextWithHint(
        "##aiprompt", "e.g. add a 8x6 room north of the selection, 3m ceiling",
        m_aiInput, sizeof m_aiInput, ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::BeginDisabled(m_aiBusy);
    const bool go = ImGui::Button("Generate") || entered;
    ImGui::EndDisabled();
    if (go && m_aiInput[0] != '\0') {
        m_aiPrompt = m_aiInput;
        aiSubmit();
    }
    ImGui::SameLine();
    if (m_aiBusy && ImGui::Button("Cancel")) {
        if (m_llm) m_llm->cancel(m_aiChat);
        m_aiStatus = "cancelled";
    }

    if (!m_aiStatus.empty()) {
        ImGui::TextDisabled("%s", m_aiStatus.c_str());
    }

    // ---- the PROPOSED plan (read-only until Apply) ----
    if (m_aiHavePlan && !m_aiPlan.ops.empty()) {
        ImGui::Separator();
        if (!m_aiPlan.summary.empty())
            ImGui::TextWrapped("%s", m_aiPlan.summary.c_str());
        if (ImGui::BeginChild("##plan", ImVec2(0, 120), true)) {
            for (size_t i = 0; i < m_aiPlan.ops.size(); ++i) {
                const AiOp& o = m_aiPlan.ops[i];
                switch (o.kind) {
                    case AiOpKind::AddBrush:
                        ImGui::Text("%2d  add %s %-10s  pos[%.1f %.1f %.1f]  size[%.1f %.1f %.1f]",
                                    (int)i, o.brushType == 1 ? "ramp" : "box ",
                                    o.name.empty() ? "-" : o.name.c_str(),
                                    o.pos[0], o.pos[1], o.pos[2],
                                    o.size[0], o.size[1], o.size[2]);
                        break;
                    case AiOpKind::MoveBrush:
                        ImGui::Text("%2d  move  [%d] -> [%.1f %.1f %.1f]", (int)i, o.index,
                                    o.pos[0], o.pos[1], o.pos[2]);
                        break;
                    case AiOpKind::SetMaterial:
                        ImGui::Text("%2d  skin  [%d] -> %s", (int)i, o.index, o.material.c_str());
                        break;
                    case AiOpKind::DeleteBrush:
                        ImGui::Text("%2d  DELETE [%d]", (int)i, o.index);
                        break;
                    case AiOpKind::SetPlayerStart:
                        ImGui::Text("%2d  player start -> [%.1f %.1f %.1f]", (int)i,
                                    o.pos[0], o.pos[1], o.pos[2]);
                        break;
                    default: break;
                }
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Apply (one undo step)")) {
            const int n = applyAiPlan(m_state, m_aiPlan);
            // The plan touched brushes[] only; the live Scene + Jolt must catch up.
            // Same path a grouped UNDO takes, so there is exactly one rebuild rule.
            HistoryEffect eff;
            eff.op = HistoryEffect::Op::RespawnAll;
            for (int i = 0; i < (int)m_doc.brushes.size(); ++i)
                respawnBrush(i, device, scene, physics);
            (void)eff;
            char buf[96];
            std::snprintf(buf, sizeof buf, "applied %d op(s) — Ctrl+Z undoes ALL of it", n);
            m_aiStatus = buf;
            m_aiHavePlan = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            m_aiHavePlan = false;
            m_aiPlan = AiPlan{};
            m_aiStatus = "discarded";
        }
    }

    // ---- what the model got WRONG, verbatim. Never hide this: a silently dropped op
    // is indistinguishable from a model that ignored the request. ----
    if (!m_aiErr.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("rejected by the validator:");
        ImGui::TextWrapped("%s", m_aiErr.c_str());
    }
    ImGui::End();
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
    if (eff.op == Op::RespawnAll) {
        // A grouped TRANSACTION was undone/redone (an AI plan, typically): many brushes
        // changed at once and per-brush hints don't survive a batch.
        //
        // ORDER MATTERS. First destroy the links of every brush the group REMOVED —
        // those records are already gone from the doc, so nothing else will ever free
        // them, and skipping this leaks a mesh + a Jolt body per brush, every single
        // time the user undoes an AI room. EditorState collected them for exactly this.
        for (const HistoryEffect& e : m_state.groupEffects())
            if (e.removed)
                teardownLinks(e.deadSceneEntity, e.deadBody, device, scene, physics);
        // Then rebuild every brush that still exists. respawnBrush() destroys any live
        // mesh/body first, so survivors (whose links are still valid) are simply
        // rebuilt rather than double-spawned. A blockout is small; correctness beats
        // cleverness here.
        for (int i = 0; i < (int)m_doc.brushes.size(); ++i)
            respawnBrush(i, device, scene, physics);
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
// Feature 3 — content/MODEL browser: load + place GLB props as model entities.
// ---------------------------------------------------------------------------
void EditorHost::ensureModelLoader(x3::rhi::IRenderDevice& device) {
    if (m_modelLoader) return;
    m_modelAssets.reset(x3::asset::createAssetSource());
    const std::string dir = x3::game::convertedGlbRoot();
    m_modelDirMounted = m_modelAssets->mountDir(dir, 0);
    if (!m_modelDirMounted)
        x3::logWarn("[editor-host] model browser: mountDir failed: " + dir);

    // ---- THE ARMORY: mount the whole library as a SECOND source -------------------
    // The curated Models list is NINE props. The library on disk is ELEVEN THOUSAND
    // converted GLBs (Sci-Fi Kit, Cyberpunk City, Command Center, Abandoned Factory,
    // Modular Sewers, Space Station interiors...). mountDir() takes a PRIORITY, so the
    // armory rides alongside the repo's converted_glb rather than replacing it: repo
    // assets keep priority 0 and always win a name collision.
    m_armory = loadArmoryIndex();
    if (m_armory.ok)
        m_armoryMounted = m_modelAssets->mountDir(m_armory.root, 1);
    if (m_armory.ok && !m_armoryMounted)
        x3::logWarn("[editor-host] armory: mountDir failed: " + m_armory.root);

    m_modelLoader.reset(x3::asset::createModelLoader(&device, m_modelAssets.get()));
}

const EditorHost::LoadedModel* EditorHost::loadModelCached(const std::string& relPath,
                                                           x3::rhi::IRenderDevice& device) {
    auto it = m_modelCache.find(relPath);
    if (it != m_modelCache.end()) return &it->second;
    ensureModelLoader(device);
    LoadedModel lm;
    if (m_modelLoader && m_modelDirMounted) {
        lm.model = m_modelLoader->load(relPath);
        if (lm.model.ok) { lm.drawables = x3::asset::makeDrawables(lm.model); lm.ok = !lm.drawables.empty(); }
    }
    if (lm.ok) x3::logInfo("[editor-host] model loaded: " + relPath + " (" +
                           std::to_string(lm.drawables.size()) + " drawables)");
    else       x3::logWarn("[editor-host] model FAILED to load: " + relPath);
    auto res = m_modelCache.emplace(relPath, std::move(lm));
    return &res.first->second;
}

int EditorHost::placeModel(const std::string& relPath, x3::rhi::IRenderDevice& device) {
    loadModelCached(relPath, device);     // warm the cache (ok if it fails — placed anyway)
    // Spawn point ~6 m in front of the fly-cam, snapped (matches the brush spawn focus).
    float focus[3] = {
        m_state.snapValue(m_camX + std::cos(m_camPitch)*std::cos(m_camYaw) * 6.0f),
        m_state.snapValue(m_camY + std::sin(m_camPitch) * 6.0f),
        m_state.snapValue(m_camZ + std::cos(m_camPitch)*std::sin(m_camYaw) * 6.0f),
    };
    int idx = m_state.addEntity("model", focus);
    if (idx < 0) return idx;
    m_doc.entities[idx].model = relPath;
    // Name it after the file stem for the Outliner.
    size_t slash = relPath.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? relPath : relPath.substr(slash + 1);
    m_doc.entities[idx].name = stem;
    return idx;
}

void EditorHost::renderModels(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) {
    if (m_doc.entities.empty()) return;
    const float white[4] = { 1, 1, 1, 1 };
    for (const auto& e : m_doc.entities) {
        if (e.model.empty()) continue;
        const LoadedModel* lm = loadModelCached(e.model, device);
        if (!lm || !lm->ok) continue;
        // Object transform: yaw about +Y, uniform scale, translate to pos (column-major).
        const float c = std::cos(e.yaw), s = std::sin(e.yaw), k = e.scale;
        float obj[16] = {
            c*k, 0, -s*k, 0,
            0,   k, 0,    0,
            s*k, 0, c*k,  0,
            e.pos[0], e.pos[1], e.pos[2], 1
        };
        for (const auto& d : lm->drawables) {
            if (!d.meshId) continue;
            float m[16];
            x3::asset::mulMat4(obj, d.nodeTransform, m);   // object * baked node TRS
            device.drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                            x3::rhi::TextureHandle{ d.baseColorTexId }, white, m);
        }
    }
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
    panelRect(0.005f, 0.045f, 0.175f, 0.340f);
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
    panelRect(0.005f, 0.395f, 0.175f, 0.220f);
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
    panelRect(0.778f, 0.045f, 0.217f, 0.400f);
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

        // Reserve room for the LABEL. ImGui draws a DragFloat's label to the RIGHT of
        // the widget, so at any sane panel width the default item width pushed
        // "Position (m)" / "Scale / Size (m)" straight off the edge of the panel and the
        // user could not read what they were dragging. A negative item width means
        // "fill, minus this much" — that much being the label column.
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 6.0f);

        // Position — DragFloat3, snapped. IsItemActivated/Deactivated bracket the drag
        // into one undo command (works for a click-drag on the slider too).
        float pos[3] = { b.pos[0], b.pos[1], b.pos[2] };
        if (ImGui::DragFloat3("Pos", pos, step)) {
            for (int a = 0; a < 3; ++a) b.pos[a] = m_state.snapValue(pos[a]);
            moved = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        // Rotation (yaw degrees in UI, radians in doc).
        float yawDeg = b.yaw * 57.29578f;
        if (ImGui::DragFloat("Yaw", &yawDeg, 1.0f)) {
            b.yaw = yawDeg * 0.0174533f; moved = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        // Scale = full extents (m), snapped + clamped. A change rebuilds the mesh.
        float size[3] = { b.size[0], b.size[1], b.size[2] };
        if (ImGui::DragFloat3("Size", size, step, 0.25f, 200.0f)) {
            for (int a = 0; a < 3; ++a) b.size[a] = std::max(0.25f, m_state.snapValue(size[a]));
            resized = true;
        }
        if (ImGui::IsItemActivated())   m_state.beginBrushEdit(si);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_state.commitBrushEdit();

        ImGui::PopItemWidth();   // MUST balance the PushItemWidth above: an unbalanced
                                 // push leaks into every panel drawn after this one.
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
    drawArmoryPanel(device, scene, physics);
    drawAiPanel(device, scene, physics);

    panelRect(0.778f, 0.455f, 0.217f, 0.280f);
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

    // ---- Model Browser (Feature 3): click a prop to place it at the fly-cam focus. ----
    panelRect(0.005f, 0.625f, 0.175f, 0.360f);
    ImGui::Begin("Models");
    ImGui::TextDisabled("Place a GLB prop at the camera focus:");
    const x3::editor::ModelCatalogItem* cat = x3::editor::editorModelCatalog();
    const uint32_t nCat = x3::editor::editorModelCatalogCount();
    for (uint32_t i = 0; i < nCat; ++i) {
        char lbl[96]; std::snprintf(lbl, sizeof(lbl), "%s##mdl%u", cat[i].label, i);
        if (ImGui::Selectable(lbl, false)) {
            int idx = placeModel(cat[i].relPath, device);
            if (idx >= 0) m_state.select(idx);   // select the new entity (Outliner highlight)
        }
    }
    ImGui::Separator();
    if (m_state.hasSelection() && m_state.selKind() != SelKind::Brush) {
        int ei = m_state.selected();
        if (ei >= 0 && ei < (int)m_doc.entities.size() && !m_doc.entities[ei].model.empty()) {
            EditorEntity& e = m_doc.entities[ei];
            ImGui::Text("Selected model: %s", e.name.c_str());
            ImGui::DragFloat3("Pos##mdl", e.pos, kGridSteps[m_gridSel]);
            float yawDeg = e.yaw * 57.29578f;
            if (ImGui::DragFloat("Yaw##mdl", &yawDeg, 1.0f)) e.yaw = yawDeg * 0.0174533f;
            ImGui::DragFloat("Scale##mdl", &e.scale, 0.05f, 0.05f, 50.0f);
            if (ImGui::Button("Delete model")) m_state.deleteSelected();
        }
    } else {
        ImGui::TextDisabled("(placed models are listed in the Outliner)");
    }
    ImGui::End();

    // ---- Status / viewport readout. ----
    panelRect(0.190f, 0.045f, 0.260f, 0.090f);
    ImGui::Begin("Status");
    ImGui::Text("Mode: %s", m_mode == HostMode::Edit ? "EDIT" : "PLAY");
    ImGui::Text("Cam: %.1f, %.1f, %.1f", m_camX, m_camY, m_camZ);
    ImGui::Text("Brushes: %d   Entities: %d",
                (int)m_doc.brushes.size(), (int)m_doc.entities.size());
    ImGui::Text("Grid snap: %s", kGridLabels[m_gridSel]);
    x3::rhi::RenderStats st = device.stats();
    ImGui::Text("Draw calls: %u   Tris: %u", st.drawCalls, st.triangles);
    ImGui::End();

    // ---- Phase 5: Doom-Builder Visual Mode KEYBOARD nudge editing -----------
    // The Keybinds rebind panel (data-driven; shares m_keybinds with the overlay + poll).
    drawRebindPanel();
    {
        ImGuiIO& io = ImGui::GetIO();
        // Crosshair-raycast the looked-at brush + apply keyboard nudges (gated on
        // !WantCaptureKeyboard so typing in a field never nudges).
        m_lastNudge = visualNudge(device, scene, physics, io.WantCaptureKeyboard);
    }
    // The unobtrusive floating cheat-sheet (corner, faint) — H toggles it.
    drawKeybindOverlay();

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

// ---------------------------------------------------------------------------
// Phase 5 — resolve the faced AXIS: which world axis the crosshair ray crosses the
// looked-at brush on. Transform the camera ray into the brush's local frame, run the
// slab test, and report the axis of the entry face. Falls back to the camera-forward
// dominant axis if the ray misses (so a nudge still has a sensible axis).
// ---------------------------------------------------------------------------
Axis EditorHost::facedAxis(int brushIdx) const {
    const float fx = std::cos(m_camPitch) * std::cos(m_camYaw);
    const float fy = std::sin(m_camPitch);
    const float fz = std::cos(m_camPitch) * std::sin(m_camYaw);
    if (brushIdx >= 0 && brushIdx < (int)m_doc.brushes.size()) {
        const BlockoutBrush& b = m_doc.brushes[brushIdx];
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        // world->local (transpose of R(yaw)); translate to the brush center.
        const float ox = m_camX-b.pos[0], oy = m_camY-b.pos[1], oz = m_camZ-b.pos[2];
        const float lox = c*ox - s*oz, loy = oy, loz = s*ox + c*oz;
        const float ldx = c*fx - s*fz, ldy = fy, ldz = s*fx + c*fz;
        const float ro[3] = { lox, loy, loz }, rd[3] = { ldx, ldy, ldz };
        const float h[3] = { b.size[0]*0.5f, b.size[1]*0.5f, b.size[2]*0.5f };
        float tmin = 0.0f, tmax = 1e30f; int entryAxis = -1; bool hit = true;
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(rd[a]) < 1e-8f) {
                if (ro[a] < -h[a] || ro[a] > h[a]) { hit = false; break; }
            } else {
                float t1 = (-h[a]-ro[a])/rd[a], t2 = (h[a]-ro[a])/rd[a];
                if (t1 > t2) { float t=t1; t1=t2; t2=t; }
                if (t1 > tmin) { tmin = t1; entryAxis = a; }   // the last-entered slab = face
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) { hit = false; break; }
            }
        }
        if (hit && entryAxis >= 0)
            return entryAxis == 0 ? Axis::X : entryAxis == 1 ? Axis::Y : Axis::Z;
    }
    // Fallback: dominant camera-forward axis.
    const float ax = std::fabs(fx), ay = std::fabs(fy), az = std::fabs(fz);
    if (ax >= ay && ax >= az) return Axis::X;
    if (az >= ax && az >= ay) return Axis::Z;
    return Axis::Y;
}

// ---------------------------------------------------------------------------
// Phase 5 — KEYBOARD nudge editing. The crosshair raycasts the looked-at brush each
// frame (auto-selecting it so the user just LOOKS to edit), then any bound nudge key /
// wheel notch fires one grid-snapped, undoable nudge, live-synced to Scene + Jolt.
// ---------------------------------------------------------------------------
NudgeAction EditorHost::visualNudge(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                                    x3::phys::IPhysicsWorld& physics, bool wantKbd) {
    if (!m_window) return NudgeAction::Count;

    // The crosshair ray from the fly-cam pose (reuse pickBrushRay). Auto-select the
    // looked-at brush so Visual Mode = look + press (no click needed). Only when not
    // mid-gizmo-drag (so a mouse drag doesn't fight the crosshair) and not over a panel.
    if (!m_dragging) {
        const float fx = std::cos(m_camPitch) * std::cos(m_camYaw);
        const float fy = std::sin(m_camPitch);
        const float fz = std::cos(m_camPitch) * std::sin(m_camYaw);
        const float o3[3] = { m_camX, m_camY, m_camZ };
        const float d3[3] = { fx, fy, fz };
        int looked = m_state.pickBrushRay(o3, d3, /*pad*/0.1f);
        if (looked >= 0 &&
            !(m_state.selKind() == SelKind::Brush && m_state.selIndex() == looked)) {
            m_state.selectBrush(looked);
        }
    }

    if (wantKbd) { for (auto& p : m_nudgePrev) p = false; return NudgeAction::Count; }

    // Step size: base grid step, scaled by modifiers (Shift = 4x bigger, Ctrl = quarter).
    auto down = [&](int k){ return glfwGetKey(m_window, k) == GLFW_PRESS; };
    const bool shift = down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT);
    const bool ctrl  = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
    float step = m_state.grid();
    if (shift) step *= 4.0f;
    else if (ctrl) step *= 0.25f;
    if (step < 1e-4f) step = 0.0625f;

    // Translate this frame's mouse-wheel into a synthetic wheel key (Doom's classic
    // raise/lower-with-wheel). One notch = one nudge.
    const float wheel = ImGui::GetIO().MouseWheel;
    const int wheelKey = wheel > 0.5f ? kKeyMouseWheelUp
                       : wheel < -0.5f ? kKeyMouseWheelDown : 0;

    const Axis face = facedAxis(m_state.hasBrushSelection() ? m_state.selIndex() : -1);
    m_lastFaceAxis = (face == Axis::X) ? 0 : (face == Axis::Y) ? 1 : 2;

    NudgeAction fired = NudgeAction::Count;
    // Walk every action; fire on the rising edge of its bound key (or a matching wheel
    // notch). Keyboard binds repeat once per press; the wheel fires per notch.
    for (int i = 0; i < (int)NudgeAction::Count; ++i) {
        const NudgeAction act = (NudgeAction)i;
        const int key = m_keybinds.keyFor(act);
        if (key == 0) { m_nudgePrev[i] = false; continue; }

        bool pressed = false;
        if (key == kKeyMouseWheelUp || key == kKeyMouseWheelDown) {
            pressed = (wheelKey == key);          // per-notch, no edge tracking needed
        } else {
            const bool nowDown = down(key);
            pressed = nowDown && !m_nudgePrev[i]; // rising edge
            m_nudgePrev[i] = nowDown;
        }
        if (!pressed) continue;

        if (act == NudgeAction::ToggleTooltip) {
            m_tooltipVisible = !m_tooltipVisible;
            fired = act;
            continue;
        }
        // A geometry nudge — needs a selected brush. One undo step + live sync.
        if (m_state.hasBrushSelection()) {
            HistoryEffect eff = m_state.nudgeBrush(act, face, step);
            applyEffect(eff, device, scene, physics);
            if (eff.op != HistoryEffect::Op::None) fired = act;
        }
    }
    return fired;
}

// ---------------------------------------------------------------------------
// Phase 5 — the UNOBTRUSIVE floating keybind cheat-sheet. A small, low-alpha overlay
// pinned to the bottom-right corner, listing the ACTIVE nudge binds (read straight from
// m_keybinds so it can never drift from the input poll). H toggles it. Edit mode only
// (draw() already returns early in Play mode before this runs).
// ---------------------------------------------------------------------------
void EditorHost::drawKeybindOverlay() {
    if (!m_tooltipVisible) {
        // A single faint hint so the user knows how to bring it back.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 10,
                                       vp->WorkPos.y + vp->WorkSize.y - 10),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.20f);
        ImGui::Begin("##nudgehint", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.8f, 0.6f), "[%s] keybinds",
                           KeybindTable::keyName(m_keybinds.keyFor(NudgeAction::ToggleTooltip)));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
        return;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 10,
                                   vp->WorkPos.y + vp->WorkSize.y - 10),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.28f);   // faint — a cheat-sheet, not a panel
    ImGui::Begin("##nudgecheat", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(0.85f);
    const char* axis = m_lastFaceAxis == 0 ? "X" : m_lastFaceAxis == 1 ? "Y" : "Z";
    const bool sel = m_state.hasBrushSelection();
    ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 0.85f),
                       "VISUAL MODE  (look at a brush)  axis:%s%s", axis,
                       sel ? "" : "  [aim at a brush]");
    ImGui::Separator();
    // Read every bind from the same table the poll reads — guaranteed in-sync.
    for (uint32_t i = 0; i < m_keybinds.count(); ++i) {
        const Keybind& kb = m_keybinds.at(i);
        ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 0.80f), "%-9s",
                           KeybindTable::keyName(kb.key));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.66f, 0.74f, 0.7f), "%s",
                           KeybindTable::actionLabel(kb.action));
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.6f, 0.68f, 0.6f),
                       "Shift=x4 step   Ctrl=fine   (rebind: Keybinds panel)");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Phase 5 — the REBIND panel. Click an action's button, then press a key to bind it
// (or scroll the wheel to bind a wheel notch). Writes m_keybinds, which the overlay +
// poll both read — single source of truth. A Reset restores the classic defaults.
// ---------------------------------------------------------------------------
void EditorHost::drawRebindPanel() {
    panelRect(0.545f, 0.045f, 0.220f, 0.400f);
    ImGui::Begin("Keybinds");
    ImGui::TextDisabled("Doom-Builder Visual Mode nudge binds.");
    ImGui::TextDisabled("Click an action, then press a key (or scroll) to rebind.");
    ImGui::Separator();

    // Capture mode: the next key/wheel the user presses rebinds m_rebindAction.
    if (m_rebinding && m_window) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.5f)       { m_keybinds.rebind(m_rebindAction, kKeyMouseWheelUp);   m_rebinding = false; }
        else if (wheel < -0.5f) { m_keybinds.rebind(m_rebindAction, kKeyMouseWheelDown); m_rebinding = false; }
        else {
            // Scan the key range for the first pressed key (skip Escape = cancel).
            for (int k = 32; k <= GLFW_KEY_LAST; ++k) {
                if (glfwGetKey(m_window, k) == GLFW_PRESS) {
                    if (k == GLFW_KEY_ESCAPE) { m_rebinding = false; break; }
                    m_keybinds.rebind(m_rebindAction, k);
                    m_rebinding = false;
                    break;
                }
            }
        }
    }

    for (uint32_t i = 0; i < m_keybinds.count(); ++i) {
        const Keybind& kb = m_keybinds.at(i);
        const bool capturing = m_rebinding && m_rebindAction == kb.action;
        ImGui::Text("%-18s", KeybindTable::actionLabel(kb.action));
        ImGui::SameLine(190);
        char btn[48];
        std::snprintf(btn, sizeof(btn), "%s##rb%u",
                      capturing ? "press a key..." : KeybindTable::keyName(kb.key), i);
        if (ImGui::Button(btn)) { m_rebinding = true; m_rebindAction = kb.action; }
    }
    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) { m_keybinds.resetDefaults(); m_rebinding = false; }
    ImGui::SameLine();
    ImGui::Text("Cheat-sheet: %s", m_tooltipVisible ? "shown (H)" : "hidden (H)");
    ImGui::End();
}

} // namespace x3::editor
