// Native Level Editor E1 — see app/editor/editor.h.
//
// Clean-room: a minimal JSON writer + a focused recursive-descent parser for the
// subset we emit, plus pure selection/gizmo logic. No third-party JSON lib.
#include "editor.h"

#include "../mesh_prims.h"               // buildBrushMesh (B4 geometry assertion)
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace x3::editor {

// ---------------------------------------------------------------------------
// Lab Architect content palette (room/entity types + legend colors, transcribed
// from the Task9D legend) — doubles as the drag-and-drop content list.
// ---------------------------------------------------------------------------
const PaletteItem* editorPalette() {
    static const PaletteItem k[] = {
        { "cell",      "Cell",            { 0.33f, 0.53f, 0.67f, 1.0f } }, // #5588aa
        { "hall",      "Main Hall",       { 0.67f, 0.53f, 0.27f, 1.0f } }, // #aa8844
        { "security",  "Security Stn",    { 0.23f, 0.54f, 0.41f, 1.0f } }, // #3a8a6a
        { "lab",       "Research Lab",    { 0.23f, 0.42f, 0.54f, 1.0f } }, // #3a6a8a
        { "medical",   "Medical Bay",     { 0.41f, 0.23f, 0.54f, 1.0f } }, // #6a3a8a
        { "nexus",     "Nexus Chamber",   { 0.80f, 0.27f, 1.0f,  1.0f } }, // #cc44ff magenta
        { "armory",    "Armory",          { 0.54f, 0.41f, 0.23f, 1.0f } }, // #8a6a3a
        { "boss",      "Boss Arena",      { 0.67f, 0.27f, 0.27f, 1.0f } }, // #aa4444 red
        { "elevator",  "Elevator Shaft",  { 0.40f, 0.47f, 0.53f, 1.0f } }, // #667788
        { "exterior",  "Exterior",        { 0.53f, 0.40f, 0.27f, 1.0f } }, // #886644
        { "enemy",     "Enemy",           { 0.85f, 0.35f, 0.30f, 1.0f } },
        { "npc",       "NPC",             { 0.40f, 0.80f, 0.55f, 1.0f } },
        { "item",      "Item / Pickup",   { 1.0f,  0.82f, 0.30f, 1.0f } },
        { "light",     "Light",           { 1.0f,  0.95f, 0.70f, 1.0f } },
        { "prop",      "Prop",            { 0.80f, 0.55f, 0.20f, 1.0f } },
    };
    return k;
}
uint32_t editorPaletteCount() { return 15; }

// ---------------------------------------------------------------------------
// Top menu bar (File / Edit / Tools / View) — data the HUD renders + dispatches.
// ---------------------------------------------------------------------------
const Menu* editorMenuBar() {
    static const MenuItem file[] = {
        { "New Level",   "Start an empty level",                 "Ctrl+N", Cmd::NewLevel },
        { "Load...",     "Load a level JSON",                    "Ctrl+O", Cmd::Load },
        { "Save",        "Save the level JSON",                  "Ctrl+S", Cmd::Save },
    };
    static const MenuItem edit[] = {
        { "Undo",        "Undo the last action",                 "Ctrl+Z", Cmd::Undo },
        { "Redo",        "Redo",                                 "Ctrl+Y", Cmd::Redo },
        { "Duplicate",   "Duplicate the selection",              "Ctrl+D", Cmd::Duplicate },
        { "Delete",      "Delete the selection",                 "Del",    Cmd::Delete },
    };
    static const MenuItem tools[] = {
        { "Select",      "Selection tool (no transform)",        "Q",      Cmd::ToolSelect },
        { "Move",        "Translate gizmo",                      "W",      Cmd::ToolMove },
        { "Rotate",      "Rotate gizmo",                         "E",      Cmd::ToolRotate },
        { "Scale",       "Scale gizmo",                          "R",      Cmd::ToolScale },
        { "Snap",        "Toggle grid / angle snap",             "X",      Cmd::ToggleSnap },
        { "World/Local", "Toggle gizmo space",                   "Tab",    Cmd::ToggleSpace },
        { "Focus",       "Frame the selection",                  "F",      Cmd::Focus },
    };
    static const MenuItem view[] = {
        { "Orbit",       "Orbit camera (drag)",                  "1",      Cmd::CamOrbit },
        { "Fly",         "Free-fly camera",                      "2",      Cmd::CamFly },
        { "FPS Walk",    "Walk the level",                       "3",      Cmd::CamFpsWalk },
        { "Wireframe",   "Toggle solid / wireframe",             "Z",      Cmd::ToggleWireframe },
    };
    static const Menu bar[] = {
        { "File",  file,  3 },
        { "Edit",  edit,  4 },
        { "Tools", tools, 7 },
        { "View",  view,  4 },
    };
    return bar;
}
uint32_t editorMenuBarCount() { return 4; }

// ---------------------------------------------------------------------------
// JSON emit
// ---------------------------------------------------------------------------
namespace {
std::string num(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", (double)f);
    return buf;
}
std::string vec3(const float v[3]) {
    return "[" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + "]";
}
std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 2);
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}
} // namespace

std::string LevelDoc::toJson() const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"name\": \"" << esc(name) << "\",\n";
    o << "  \"biome\": \"" << esc(biome) << "\",\n";
    o << "  \"playerStart\": " << vec3(playerStart) << ",\n";
    o << "  \"entities\": [\n";
    for (size_t i = 0; i < entities.size(); ++i) {
        const EditorEntity& e = entities[i];
        o << "    { \"name\": \"" << esc(e.name) << "\""
          << ", \"type\": \"" << esc(e.type) << "\""
          << ", \"pos\": " << vec3(e.pos)
          << ", \"yaw\": " << num(e.yaw)
          << ", \"scale\": " << num(e.scale)
          << ", \"tint\": " << vec3(e.tint) << " }";
        if (i + 1 < entities.size()) o << ",";
        o << "\n";
    }
    o << "  ],\n";
    // Level Architect P2 BLOCKOUT brushes (mirrors entities[]: same num()/vec3()/esc()
    // helpers). type 0 = Box, 1 = Ramp. collide -> a static Jolt body on load.
    o << "  \"brushes\": [\n";
    for (size_t i = 0; i < brushes.size(); ++i) {
        const BlockoutBrush& b = brushes[i];
        o << "    { \"name\": \"" << esc(b.name) << "\""
          << ", \"type\": " << (int)b.type
          << ", \"pos\": " << vec3(b.pos)
          << ", \"size\": " << vec3(b.size)
          << ", \"yaw\": " << num(b.yaw)
          << ", \"tint\": " << vec3(b.tint)
          << ", \"collide\": " << (b.collide ? "1" : "0") << " }";
        if (i + 1 < brushes.size()) o << ",";
        o << "\n";
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// JSON parse (focused subset: objects, arrays, strings, numbers — what we emit)
// ---------------------------------------------------------------------------
namespace {
struct JParse {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p==',')) ++p; }
    bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
    char peek() { ws(); return p < end ? *p : '\0'; }

    std::string str() {
        ws();
        std::string s;
        if (p >= end || *p != '"') { ok = false; return s; }
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) { ++p; s += *p; }
            else s += *p;
            ++p;
        }
        if (p < end) ++p;   // closing quote
        return s;
    }
    float number() {
        ws();
        const char* s = p;
        while (p < end && (*p=='-'||*p=='+'||*p=='.'||*p=='e'||*p=='E'||(*p>='0'&&*p<='9'))) ++p;
        if (p == s) { ok = false; return 0.0f; }
        return (float)std::atof(std::string(s, p).c_str());
    }
    void vec3(float out[3]) {
        if (!eat('[')) { ok = false; return; }
        out[0] = number(); out[1] = number(); out[2] = number();
        eat(']');
    }
    // Read a key string followed by ':'.
    std::string key() { std::string k = str(); eat(':'); return k; }
};
} // namespace

bool LevelDoc::fromJson(const std::string& json) {
    JParse j{ json.c_str(), json.c_str() + json.size() };
    entities.clear();
    brushes.clear();
    if (!j.eat('{')) return false;
    while (j.ok && j.peek() && j.peek() != '}') {
        std::string k = j.key();
        if (k == "name")        name = j.str();
        else if (k == "biome")  biome = j.str();
        else if (k == "playerStart") j.vec3(playerStart);
        else if (k == "entities") {
            if (!j.eat('[')) { return false; }
            while (j.ok && j.peek() && j.peek() != ']') {
                if (!j.eat('{')) break;
                EditorEntity e;
                while (j.ok && j.peek() && j.peek() != '}') {
                    std::string ek = j.key();
                    if (ek == "name")       e.name = j.str();
                    else if (ek == "type")  e.type = j.str();
                    else if (ek == "pos")   j.vec3(e.pos);
                    else if (ek == "yaw")   e.yaw = j.number();
                    else if (ek == "scale") e.scale = j.number();
                    else if (ek == "tint")  j.vec3(e.tint);
                    else { /* skip unknown scalar */ j.str(); }
                }
                j.eat('}');
                entities.push_back(e);
            }
            j.eat(']');
        } else if (k == "brushes") {
            if (!j.eat('[')) { return false; }
            while (j.ok && j.peek() && j.peek() != ']') {
                if (!j.eat('{')) break;
                BlockoutBrush b;
                while (j.ok && j.peek() && j.peek() != '}') {
                    std::string bk = j.key();
                    if (bk == "name")        b.name = j.str();
                    else if (bk == "type")   b.type = (uint32_t)j.number();
                    else if (bk == "pos")    j.vec3(b.pos);
                    else if (bk == "size")   j.vec3(b.size);
                    else if (bk == "yaw")    b.yaw = j.number();
                    else if (bk == "tint")   j.vec3(b.tint);
                    else if (bk == "collide") b.collide = (j.number() != 0.0f);
                    else { /* skip unknown scalar */ j.str(); }
                }
                j.eat('}');
                brushes.push_back(b);
            }
            j.eat(']');
        } else {
            j.str();   // skip unknown string value
        }
    }
    j.eat('}');
    return j.ok;
}

bool LevelDoc::saveJson(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) { x3::logWarn("[editor] saveJson: cannot open " + path); return false; }
    f << toJson();
    return (bool)f;
}

bool LevelDoc::loadJson(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { x3::logWarn("[editor] loadJson: cannot open " + path); return false; }
    std::ostringstream ss; ss << f.rdbuf();
    return fromJson(ss.str());
}

// ---------------------------------------------------------------------------
// EditorState
// ---------------------------------------------------------------------------
void EditorState::select(int index) {
    if (index < 0) { m_selected = -1; return; }
    if (index < (int)m_doc.entities.size()) m_selected = index;
}

int EditorState::pickRay(const float origin[3], const float dir[3],
                         float maxDist, float hitRadius) const {
    // Normalize dir.
    float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dl < 1e-6f) return -1;
    const float dx = dir[0]/dl, dy = dir[1]/dl, dz = dir[2]/dl;
    int best = -1; float bestT = maxDist;
    for (size_t i = 0; i < m_doc.entities.size(); ++i) {
        const float* c = m_doc.entities[i].pos;
        // Project center onto the ray; reject behind / beyond.
        const float ox = c[0]-origin[0], oy = c[1]-origin[1], oz = c[2]-origin[2];
        const float t = ox*dx + oy*dy + oz*dz;
        if (t < 0.0f || t > maxDist) continue;
        // Perpendicular distance from the center to the ray.
        const float px = origin[0]+dx*t, py = origin[1]+dy*t, pz = origin[2]+dz*t;
        const float perp = std::sqrt((c[0]-px)*(c[0]-px) + (c[1]-py)*(c[1]-py) + (c[2]-pz)*(c[2]-pz));
        if (perp <= hitRadius && t < bestT) { bestT = t; best = (int)i; }
    }
    return best;
}

bool EditorState::moveSelected(Axis axis, float delta) {
    if (!hasSelection() || axis == Axis::None || delta == 0.0f) return false;
    float* p = m_doc.entities[m_selected].pos;
    int a = (axis == Axis::X) ? 0 : (axis == Axis::Y) ? 1 : 2;
    p[a] += delta;
    if (m_snap) p[a] = std::round(p[a] / m_grid) * m_grid;
    return true;
}

bool EditorState::snapSelected() {
    if (!hasSelection()) return false;
    float* p = m_doc.entities[m_selected].pos;
    for (int a = 0; a < 3; ++a) p[a] = std::round(p[a] / m_grid) * m_grid;
    return true;
}

int EditorState::addEntity(const char* type, const float pos[3]) {
    EditorEntity e;
    e.type = type ? type : "prop";
    e.name = e.type + std::string("_") + std::to_string(m_doc.entities.size());
    if (pos) { e.pos[0]=pos[0]; e.pos[1]=pos[1]; e.pos[2]=pos[2]; }
    if (hasSelection()) {   // inherit the selection's look
        const EditorEntity& s = m_doc.entities[m_selected];
        e.tint[0]=s.tint[0]; e.tint[1]=s.tint[1]; e.tint[2]=s.tint[2];
        e.scale = s.scale;
    }
    m_doc.entities.push_back(e);
    m_selected = (int)m_doc.entities.size() - 1;
    return m_selected;
}

bool EditorState::deleteSelected() {
    if (!hasSelection()) return false;
    m_doc.entities.erase(m_doc.entities.begin() + m_selected);
    if (m_selected >= (int)m_doc.entities.size()) m_selected = (int)m_doc.entities.size() - 1;
    return true;
}

// ---------------------------------------------------------------------------
// Blockout brush ops (Level Architect P2). Pure doc mutation — the live host
// (EditorHost) mirrors each change into the Scene + Jolt. Headless-testable.
// ---------------------------------------------------------------------------
void EditorState::selectBrush(int index) {
    if (index < 0) { m_selKind = SelKind::None; m_selIndex = -1; return; }
    if (index < (int)m_doc.brushes.size()) { m_selKind = SelKind::Brush; m_selIndex = index; }
}

int EditorState::addBrush(uint32_t type, const float pos[3]) {
    BlockoutBrush b;
    b.type = (type <= 1u) ? type : 0u;   // P2 core = Box(0) / Ramp(1)
    b.name = std::string(b.type == 1u ? "ramp_" : "box_") + std::to_string(m_doc.brushes.size());
    if (pos) for (int a = 0; a < 3; ++a) b.pos[a] = snapValue(pos[a]);
    // Default 2 m cube, snapped (so a freshly placed brush already sits on the grid).
    for (int a = 0; a < 3; ++a) b.size[a] = std::max(0.25f, snapValue(b.size[a]));
    m_doc.brushes.push_back(b);
    m_selKind = SelKind::Brush; m_selIndex = (int)m_doc.brushes.size() - 1;
    return m_selIndex;
}

bool EditorState::resizeSelectedBrush(Axis axis, float delta) {
    if (!hasBrushSelection() || axis == Axis::None || delta == 0.0f) return false;
    int a = (axis == Axis::X) ? 0 : (axis == Axis::Y) ? 1 : 2;
    float& s = m_doc.brushes[m_selIndex].size[a];
    s = std::max(0.25f, snapValue(s + delta));
    return true;
}

bool EditorState::moveSelectedBrush(Axis axis, float delta) {
    if (!hasBrushSelection() || axis == Axis::None || delta == 0.0f) return false;
    int a = (axis == Axis::X) ? 0 : (axis == Axis::Y) ? 1 : 2;
    float& p = m_doc.brushes[m_selIndex].pos[a];
    p = snapValue(p + delta);
    return true;
}

bool EditorState::deleteSelectedBrush() {
    if (!hasBrushSelection()) return false;
    m_doc.brushes.erase(m_doc.brushes.begin() + m_selIndex);
    m_selKind = SelKind::None; m_selIndex = -1;
    return true;
}

// ---------------------------------------------------------------------------
// Undo / redo (P0.6). Snapshot-based brush command stack. Pure doc mutation —
// the host re-syncs the Scene + Jolt via the returned HistoryEffect hint.
// ---------------------------------------------------------------------------
void EditorState::pushCmd(const BrushCmd& c) {
    // Truncate the redo tail (any command at/after the current undo position) then
    // append + advance — the classic linear-history behaviour.
    if (m_undoPos < (int)m_history.size())
        m_history.erase(m_history.begin() + m_undoPos, m_history.end());
    m_history.push_back(c);
    m_undoPos = (int)m_history.size();
}

int EditorState::addBrushCmd(uint32_t type, const float pos[3]) {
    int idx = addBrush(type, pos);          // does the mutation + selects the brush
    if (idx < 0) return idx;
    BrushCmd c; c.kind = CmdKind::Add; c.index = idx; c.after = m_doc.brushes[idx];
    pushCmd(c);
    return idx;
}

bool EditorState::deleteSelectedBrushCmd() {
    if (!hasBrushSelection()) return false;
    BrushCmd c; c.kind = CmdKind::Delete; c.index = m_selIndex;
    c.before = m_doc.brushes[m_selIndex];   // capture value + live links for restore
    bool ok = deleteSelectedBrush();
    if (ok) pushCmd(c);
    return ok;
}

void EditorState::beginBrushEdit(int index) {
    if (m_editing) return;                  // already in a drag; ignore nested begins
    if (index < 0 || index >= (int)m_doc.brushes.size()) return;
    m_editing = true; m_editIndex = index; m_editBefore = m_doc.brushes[index];
}

void EditorState::commitBrushEdit() {
    if (!m_editing) return;
    m_editing = false;
    const int idx = m_editIndex; m_editIndex = -1;
    if (idx < 0 || idx >= (int)m_doc.brushes.size()) return;
    const BlockoutBrush& now = m_doc.brushes[idx];
    // Drop a no-op edit (e.g. a click that didn't drag) — only push if something moved.
    auto v3eq = [](const float a[3], const float b[3]) {
        return std::fabs(a[0]-b[0]) < 1e-5f && std::fabs(a[1]-b[1]) < 1e-5f &&
               std::fabs(a[2]-b[2]) < 1e-5f;
    };
    const bool unchanged = v3eq(m_editBefore.pos, now.pos) &&
                           v3eq(m_editBefore.size, now.size) &&
                           std::fabs(m_editBefore.yaw - now.yaw) < 1e-5f;
    if (unchanged) return;
    BrushCmd c; c.kind = CmdKind::Transform; c.index = idx;
    c.before = m_editBefore; c.after = now;
    pushCmd(c);
}

HistoryEffect EditorState::undo() {
    HistoryEffect eff;
    if (!canUndo()) return eff;
    const BrushCmd& c = m_history[--m_undoPos];
    switch (c.kind) {
        case CmdKind::Add: {
            // Undo an Add = remove the brush. Hand the host the dead links to tear down.
            if (c.index >= 0 && c.index < (int)m_doc.brushes.size()) {
                const BlockoutBrush& b = m_doc.brushes[c.index];
                eff.deadSceneEntity = b.sceneEntity; eff.deadBody = b.body;
                m_doc.brushes.erase(m_doc.brushes.begin() + c.index);
            }
            eff.op = HistoryEffect::Op::Respawn; eff.index = c.index; eff.removed = true;
            m_selKind = SelKind::None; m_selIndex = -1;
            break;
        }
        case CmdKind::Delete: {
            // Undo a Delete = re-insert the captured brush (links cleared so the host
            // rebuilds a fresh mesh/body).
            BlockoutBrush b = c.before; b.sceneEntity = 0xFFFFFFFFu; b.body = 0;
            int at = c.index; if (at > (int)m_doc.brushes.size()) at = (int)m_doc.brushes.size();
            m_doc.brushes.insert(m_doc.brushes.begin() + at, b);
            eff.op = HistoryEffect::Op::Respawn; eff.index = at;
            m_selKind = SelKind::Brush; m_selIndex = at;
            break;
        }
        case CmdKind::Transform: {
            if (c.index >= 0 && c.index < (int)m_doc.brushes.size()) {
                BlockoutBrush& b = m_doc.brushes[c.index];
                const uint32_t se = b.sceneEntity, bo = b.body;  // keep live links
                b = c.before; b.sceneEntity = se; b.body = bo;
                // A pure move/yaw can sync cheaply; a size change must respawn the mesh.
                const bool sizeChanged =
                    std::fabs(c.before.size[0]-c.after.size[0]) > 1e-5f ||
                    std::fabs(c.before.size[1]-c.after.size[1]) > 1e-5f ||
                    std::fabs(c.before.size[2]-c.after.size[2]) > 1e-5f;
                eff.op = sizeChanged ? HistoryEffect::Op::Respawn : HistoryEffect::Op::SyncXform;
                eff.index = c.index;
                m_selKind = SelKind::Brush; m_selIndex = c.index;
            }
            break;
        }
        default: break;
    }
    return eff;
}

HistoryEffect EditorState::redo() {
    HistoryEffect eff;
    if (!canRedo()) return eff;
    const BrushCmd& c = m_history[m_undoPos++];
    switch (c.kind) {
        case CmdKind::Add: {
            // Redo an Add = re-insert the created brush (links cleared -> host rebuilds).
            BlockoutBrush b = c.after; b.sceneEntity = 0xFFFFFFFFu; b.body = 0;
            int at = c.index; if (at > (int)m_doc.brushes.size()) at = (int)m_doc.brushes.size();
            m_doc.brushes.insert(m_doc.brushes.begin() + at, b);
            eff.op = HistoryEffect::Op::Respawn; eff.index = at;
            m_selKind = SelKind::Brush; m_selIndex = at;
            break;
        }
        case CmdKind::Delete: {
            // Redo a Delete = remove the brush again (tear down its live links).
            if (c.index >= 0 && c.index < (int)m_doc.brushes.size()) {
                const BlockoutBrush& b = m_doc.brushes[c.index];
                eff.deadSceneEntity = b.sceneEntity; eff.deadBody = b.body;
                m_doc.brushes.erase(m_doc.brushes.begin() + c.index);
            }
            eff.op = HistoryEffect::Op::Respawn; eff.index = c.index; eff.removed = true;
            m_selKind = SelKind::None; m_selIndex = -1;
            break;
        }
        case CmdKind::Transform: {
            if (c.index >= 0 && c.index < (int)m_doc.brushes.size()) {
                BlockoutBrush& b = m_doc.brushes[c.index];
                const uint32_t se = b.sceneEntity, bo = b.body;
                b = c.after; b.sceneEntity = se; b.body = bo;
                const bool sizeChanged =
                    std::fabs(c.before.size[0]-c.after.size[0]) > 1e-5f ||
                    std::fabs(c.before.size[1]-c.after.size[1]) > 1e-5f ||
                    std::fabs(c.before.size[2]-c.after.size[2]) > 1e-5f;
                eff.op = sizeChanged ? HistoryEffect::Op::Respawn : HistoryEffect::Op::SyncXform;
                eff.index = c.index;
                m_selKind = SelKind::Brush; m_selIndex = c.index;
            }
            break;
        }
        default: break;
    }
    return eff;
}

// ===========================================================================
// Headless self-test (--test-editor). E0-E5.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[editor-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[editor-test] FAIL ") + name); }
}
bool near(float a, float b, float e = 1e-3f) { return std::fabs(a-b) < e; }
}

bool runEditorSelfTest() {
    g_pass = g_fail = 0;

    // Build a small doc.
    LevelDoc doc;
    doc.name = "test_level"; doc.biome = "cave";
    doc.playerStart[0] = 1.5f; doc.playerStart[1] = 0.05f; doc.playerStart[2] = -2.0f;
    { EditorEntity e; e.name="crate"; e.type="prop"; e.pos[0]=3; e.pos[1]=0.5f; e.pos[2]=0; e.yaw=0.5f; e.scale=1.2f; e.tint[0]=0.8f; doc.entities.push_back(e); }
    { EditorEntity e; e.name="grunt"; e.type="enemy"; e.pos[0]=8; e.pos[1]=0; e.pos[2]=4; doc.entities.push_back(e); }
    { EditorEntity e; e.name="lamp";  e.type="light"; e.pos[0]=-2; e.pos[1]=3; e.pos[2]=1; doc.entities.push_back(e); }

    // ---- E0: JSON round-trip preserves the doc. ----
    {
        std::string js = doc.toJson();
        LevelDoc rt; bool parsed = rt.fromJson(js);
        bool same = parsed && rt.name == doc.name && rt.biome == doc.biome &&
                    rt.entities.size() == 3 &&
                    near(rt.playerStart[0], 1.5f) && near(rt.playerStart[2], -2.0f) &&
                    rt.entities[0].name == "crate" && rt.entities[0].type == "prop" &&
                    near(rt.entities[0].pos[0], 3.0f) && near(rt.entities[0].yaw, 0.5f) &&
                    near(rt.entities[0].scale, 1.2f) &&
                    rt.entities[1].type == "enemy" && near(rt.entities[1].pos[0], 8.0f) &&
                    rt.entities[2].type == "light" && near(rt.entities[2].pos[1], 3.0f);
        check(same, "E0 JSON save->load round-trip preserves the level");
    }

    // ---- E1: file save/load round-trip. ----
    {
        std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/x3_editor_test.json";
        bool saved = doc.saveJson(path);
        LevelDoc rd; bool loaded = rd.loadJson(path);
        check(saved && loaded && rd.entities.size() == 3 && rd.entities[1].name == "grunt",
              "E1 file saveJson/loadJson round-trip");
    }

    // ---- E2: ray pick selects the entity the ray points at, misses cleanly. ----
    {
        EditorState ed(doc);
        // Aim from x=0 toward the crate at (3,0.5,0): origin (0,0.5,0) dir +X.
        float o[3] = { 0, 0.5f, 0 }, d[3] = { 1, 0, 0 };
        int hit = ed.pickRay(o, d);
        // Aim +Y from below origin at nothing near the ray.
        float dUp[3] = { 0, 1, 0 };
        int miss = ed.pickRay(o, dUp);
        check(hit == 0 && miss == -1, "E2 ray pick hits the aimed entity, misses empty space");
    }

    // ---- E3: select + move along an axis (with snap) updates the position. ----
    {
        EditorState ed(doc);
        ed.setSnap(true, 0.5f);
        ed.select(1);                         // grunt at (8,0,4)
        ed.moveSelected(Axis::X, 0.4f);       // 8.4 -> snap to 8.5
        bool moved = near(doc.entities[1].pos[0], 8.5f);
        ed.moveSelected(Axis::Z, -1.2f);      // 4 - 1.2 = 2.8 -> snap 3.0... actually 2.8 -> 3.0? round(2.8/0.5)=round(5.6)=6 ->3.0
        bool movedZ = near(doc.entities[1].pos[2], 3.0f);
        check(moved && movedZ, "E3 move gizmo + grid snap updates the selection");
    }

    // ---- E4: add then delete mutate the doc + selection. ----
    {
        LevelDoc d2 = doc;                    // copy
        EditorState ed(d2);
        size_t before = d2.entities.size();
        float p[3] = { 5, 0, 5 };
        int idx = ed.addEntity("npc", p);
        bool added = d2.entities.size() == before + 1 && idx == (int)before &&
                     d2.entities[idx].type == "npc" && near(d2.entities[idx].pos[0], 5.0f);
        bool del = ed.deleteSelected();
        check(added && del && d2.entities.size() == before,
              "E4 add + delete entity mutate the doc");
    }

    // ---- E5: an empty doc round-trips (no entities). ----
    {
        LevelDoc empty; empty.name = "blank";
        LevelDoc rt; bool ok = rt.fromJson(empty.toJson());
        check(ok && rt.name == "blank" && rt.entities.empty(), "E5 empty level round-trips");
    }

    // ---- E6: theme/palette/menu data are present + well-formed. ----
    {
        bool pal = editorPaletteCount() == 15 && editorPalette()[0].type != nullptr;
        bool men = editorMenuBarCount() == 4 &&
                   std::string(editorMenuBar()[2].title) == "Tools" &&
                   editorMenuBar()[2].items[1].id == Cmd::ToolMove &&      // W = Move
                   std::string(editorMenuBar()[2].items[1].shortcut) == "W";
        check(pal && men, "E6 Lab-Architect palette + UE-style menu data present");
    }

    // ---- E7: blockout brushes[] JSON round-trip preserves the brushes. ----
    {
        LevelDoc bd; bd.name = "blockout";
        { BlockoutBrush b; b.name="floor"; b.type=0; b.pos[0]=2; b.pos[1]=0; b.pos[2]=-3;
          b.size[0]=8; b.size[1]=0.5f; b.size[2]=8; b.yaw=0.0f; b.collide=true; bd.brushes.push_back(b); }
        { BlockoutBrush b; b.name="slope"; b.type=1; b.pos[0]=5; b.pos[1]=1; b.pos[2]=2;
          b.size[0]=3; b.size[1]=2; b.size[2]=4; b.yaw=1.5708f; b.collide=false; bd.brushes.push_back(b); }
        LevelDoc rt; bool parsed = rt.fromJson(bd.toJson());
        bool same = parsed && rt.brushes.size() == 2 &&
                    rt.brushes[0].name == "floor" && rt.brushes[0].type == 0u &&
                    near(rt.brushes[0].size[0], 8.0f) && near(rt.brushes[0].pos[2], -3.0f) &&
                    rt.brushes[0].collide == true &&
                    rt.brushes[1].type == 1u && near(rt.brushes[1].yaw, 1.5708f) &&
                    near(rt.brushes[1].size[1], 2.0f) && rt.brushes[1].collide == false;
        check(same, "E7 blockout brushes[] JSON round-trip preserves type/pos/size/yaw/collide");
    }

    // ---- E8: undo/redo of add — addBrushCmd then undo removes it, redo restores. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(true, 0.5f);
        float p[3] = { 1, 0, 2 };
        int idx = ed.addBrushCmd(0u, p);
        bool added = idx == 0 && d.brushes.size() == 1 && ed.canUndo() && !ed.canRedo();
        HistoryEffect u = ed.undo();
        bool undone = d.brushes.empty() && u.removed && u.op == HistoryEffect::Op::Respawn &&
                      !ed.canUndo() && ed.canRedo();
        HistoryEffect r = ed.redo();
        bool redone = d.brushes.size() == 1 && r.op == HistoryEffect::Op::Respawn &&
                      near(d.brushes[0].pos[2], 2.0f) && ed.canUndo() && !ed.canRedo();
        check(added && undone && redone, "E8 undo/redo of brush ADD round-trips the doc");
    }

    // ---- E9: undo/redo of delete — captured brush is restored faithfully. ----
    {
        LevelDoc d; EditorState ed(d);
        float p[3] = { 4, 1, -3 };
        ed.addBrushCmd(1u, p);                       // a Ramp
        ed.selectBrush(0);
        bool del = ed.deleteSelectedBrushCmd() && d.brushes.empty();
        HistoryEffect u = ed.undo();                 // undo the delete -> restore
        bool restored = d.brushes.size() == 1 && d.brushes[0].type == 1u &&
                        near(d.brushes[0].pos[0], 4.0f) && u.op == HistoryEffect::Op::Respawn &&
                        u.index == 0;
        HistoryEffect r = ed.redo();                 // redo the delete -> gone again
        bool gone = d.brushes.empty() && r.removed;
        check(del && restored && gone, "E9 undo/redo of brush DELETE restores the captured brush");
    }

    // ---- E10: transform edit groups a drag into ONE undo step (move). ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(false);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);                        // brush 0 at origin (1 undo step)
        ed.beginBrushEdit(0);
        d.brushes[0].pos[0] = 5.0f;                   // simulate a multi-frame drag
        d.brushes[0].pos[0] = 7.0f;
        ed.commitBrushEdit();                         // ONE Transform command
        bool oneStep = ed.canUndo();
        HistoryEffect u = ed.undo();                  // back to x=0
        bool back = near(d.brushes[0].pos[0], 0.0f) && u.op == HistoryEffect::Op::SyncXform;
        HistoryEffect r = ed.redo();                  // forward to x=7
        bool fwd = near(d.brushes[0].pos[0], 7.0f) && r.op == HistoryEffect::Op::SyncXform;
        // A commit with no net change must NOT push a command.
        ed.beginBrushEdit(0); ed.commitBrushEdit();
        bool noEmpty = !ed.canRedo();                 // still nothing to redo
        check(oneStep && back && fwd && noEmpty, "E10 transform drag = one undo step; empty edit dropped");
    }

    // ---- E11: a size edit reports Respawn (mesh rebuild) on undo, not SyncXform. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(false);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);
        ed.beginBrushEdit(0);
        d.brushes[0].size[0] = 6.0f;                  // grow X face
        ed.commitBrushEdit();
        HistoryEffect u = ed.undo();
        check(u.op == HistoryEffect::Op::Respawn && near(d.brushes[0].size[0], 2.0f),
              "E11 size edit undo asks the host to RESPAWN (rebuild mesh)");
    }

    x3::logInfo(std::string("[editor-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Blockout self-test (--test-blockout). B0-B4. No window / Vulkan.
// ===========================================================================
bool runBlockoutSelfTest() {
    g_pass = g_fail = 0;

    // ---- B0: a brush round-trips through the brushes[] JSON (full fidelity). ----
    {
        LevelDoc doc; doc.name = "bt";
        BlockoutBrush b; b.name="ramp_test"; b.type=1;
        b.pos[0]=3.5f; b.pos[1]=1.25f; b.pos[2]=-7.0f;
        b.size[0]=4.0f; b.size[1]=2.5f; b.size[2]=6.0f; b.yaw=0.7854f; b.collide=true;
        doc.brushes.push_back(b);
        LevelDoc rt; bool ok = rt.fromJson(doc.toJson());
        bool same = ok && rt.brushes.size() == 1 &&
                    rt.brushes[0].name == "ramp_test" && rt.brushes[0].type == 1u &&
                    near(rt.brushes[0].pos[0], 3.5f) && near(rt.brushes[0].pos[1], 1.25f) &&
                    near(rt.brushes[0].pos[2], -7.0f) &&
                    near(rt.brushes[0].size[0], 4.0f) && near(rt.brushes[0].size[1], 2.5f) &&
                    near(rt.brushes[0].size[2], 6.0f) && near(rt.brushes[0].yaw, 0.7854f) &&
                    rt.brushes[0].collide == true;
        check(same, "B0 brush round-trips (type/pos/size/yaw/collide preserved)");
    }

    // ---- B1: addBrush snaps the position to the grid + selects the new brush. ----
    {
        LevelDoc doc; EditorState ed(doc);
        ed.setSnap(true, 0.5f);
        float p[3] = { 2.3f, 0.1f, -4.7f };
        int idx = ed.addBrush(/*Box*/0u, p);
        bool ok = idx == 0 && doc.brushes.size() == 1 &&
                  ed.selKind() == SelKind::Brush && ed.selIndex() == 0 &&
                  near(doc.brushes[0].pos[0], 2.5f) &&   // 2.3 -> snap 2.5
                  near(doc.brushes[0].pos[2], -4.5f);    // -4.7 -> snap -4.5
        check(ok, "B1 addBrush snaps pos to grid + selects the brush");
    }

    // ---- B2: resizeSelectedBrush grows a face with grid snap + a min clamp. ----
    {
        LevelDoc doc; EditorState ed(doc);
        ed.setSnap(true, 0.5f);
        float p[3] = { 0, 0, 0 };
        ed.addBrush(0u, p);                       // 2 m cube
        ed.resizeSelectedBrush(Axis::X, 1.3f);    // 2 + 1.3 = 3.3 -> snap 3.5
        bool grew = near(doc.brushes[0].size[0], 3.5f);
        ed.resizeSelectedBrush(Axis::Y, -5.0f);   // 2 - 5 -> clamp 0.25
        bool clamped = doc.brushes[0].size[1] >= 0.25f - 1e-3f && doc.brushes[0].size[1] < 0.5f;
        check(grew && clamped, "B2 resizeSelectedBrush face-grows with snap + min clamp");
    }

    // ---- B3: a Box brush mesh is origin-CENTERED (bbox symmetric about 0). ----
    {
        float size[3] = { 4.0f, 2.0f, 6.0f };
        x3::prims::PrimMesh m = x3::prims::buildBrushMesh(x3::prims::BrushType::Box, size);
        float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
        for (const auto& v : m.verts) for (int a=0;a<3;++a){ mn[a]=std::min(mn[a],v.pos[a]); mx[a]=std::max(mx[a],v.pos[a]); }
        bool centered = near(mn[0],-2.0f) && near(mx[0],2.0f) &&
                        near(mn[1],-1.0f) && near(mx[1],1.0f) &&
                        near(mn[2],-3.0f) && near(mx[2],3.0f);
        check(centered && !m.verts.empty(), "B3 Box brush mesh is origin-centered (transform carries pos)");
    }

    // ---- B4: Box + Ramp builders produce non-degenerate render + collision geo. ----
    {
        float size[3] = { 3.0f, 2.0f, 5.0f };
        x3::prims::PrimMesh box  = x3::prims::buildBrushMesh(x3::prims::BrushType::Box,  size);
        x3::prims::PrimMesh ramp = x3::prims::buildBrushMesh(x3::prims::BrushType::Ramp, size);
        bool boxOk  = box.verts.size()  >= 8 && box.index.size()  % 3 == 0 &&
                      box.cverts.size()  == box.verts.size()  * 3 && box.cindex.size()  == box.index.size();
        bool rampOk = ramp.verts.size() >= 6 && ramp.index.size() % 3 == 0 &&
                      ramp.cverts.size() == ramp.verts.size() * 3 && ramp.cindex.size() == ramp.index.size();
        check(boxOk && rampOk, "B4 Box + Ramp meshes: valid render + matching collision geometry");
    }

    x3::logInfo(std::string("[blockout-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::editor
