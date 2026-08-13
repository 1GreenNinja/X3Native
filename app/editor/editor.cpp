// Native Level Editor E1 — see app/editor/editor.h.
//
// Clean-room: a minimal JSON writer + a focused recursive-descent parser for the
// subset we emit, plus pure selection/gizmo logic. No third-party JSON lib.
#include "editor.h"

#include "../mesh_prims.h"               // buildBrushMesh (B4 geometry assertion)
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace x3::editor {

// ---------------------------------------------------------------------------
// Level Architect 11.0 — feature gates + shared rotation math (see editor.h).
// ---------------------------------------------------------------------------
bool rot3Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("X3_EDITOR_ROT3");
        return !(v && v[0] == '0');          // default ON; "0" restores yaw-only
    }();
    return on;
}

bool portalsEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("X3_EDITOR_PORTAL");
        return !(v && v[0] == '0');          // default ON; "0" restores markers
    }();
    return on;
}

// R = Ry(yaw) * Rx(pitch) * Rz(roll), column-major (out[col*3 + row]). Built by
// an explicit 3x3 multiply of the three primitive rotations rather than a hand-
// expanded closed form — harder to transpose-typo, and Ry alone is bit-identical
// to the 10.x yaw matrix (c,0,-s / 0,1,0 / s,0,c in columns).
void rotYPR(float yaw, float pitch, float roll, float R[9]) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll),  sr = std::sin(roll);
    const float Ry[9] = { cy,0,-sy,  0,1,0,   sy,0,cy };
    const float Rx[9] = { 1,0,0,     0,cp,sp, 0,-sp,cp };
    const float Rz[9] = { cr,sr,0,  -sr,cr,0, 0,0,1 };
    auto mul = [](const float A[9], const float B[9], float C[9]) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                C[c*3+r] = A[0*3+r]*B[c*3+0] + A[1*3+r]*B[c*3+1] + A[2*3+r]*B[c*3+2];
    };
    float T[9];
    mul(Ry, Rx, T);
    mul(T, Rz, R);
}

// Same composite as a quaternion: q = qy * qx * qz (xyzw, Hamilton product) —
// matches rotYPR because quaternion composition mirrors matrix composition.
void yprToQuat(float yaw, float pitch, float roll, float q[4]) {
    auto qmul = [](const float a[4], const float b[4], float o[4]) {
        o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
        o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
        o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
        o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    };
    const float qy[4] = { 0, std::sin(yaw*0.5f), 0, std::cos(yaw*0.5f) };
    const float qx[4] = { std::sin(pitch*0.5f), 0, 0, std::cos(pitch*0.5f) };
    const float qz[4] = { 0, 0, std::sin(roll*0.5f), std::cos(roll*0.5f) };
    float t[4];
    qmul(qy, qx, t);
    qmul(t, qz, q);
}

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
        // 11.0: hand-placed portal plane (the tunnel-lane payoff). See the
        // LEVELDOC PORTAL CONTRACT on EditorEntity::type.
        { "portal",    "Portal",          { 0.30f, 0.90f, 1.0f,  1.0f } },
    };
    return k;
}
uint32_t editorPaletteCount() { return 16; }

// ---------------------------------------------------------------------------
// Feature 1 — curated built-in surface MATERIALS (click-a-wall texturing). The
// first entry (id "grid") is the clean blockout default; the rest are named sci-fi
// surfaces the host bakes to a cached procedural texture + tint. The serialized
// BlockoutBrush::material stores `id`; an empty id also resolves to the grid default.
// ---------------------------------------------------------------------------
const BlockoutMaterial* editorMaterials() {
    static const BlockoutMaterial k[] = {
        { "grid",       "Blockout Grid",  MatTex::Grid,       { 1.00f, 1.00f, 1.00f } },
        { "wall",       "Sci-Fi Wall",    MatTex::Panel,      { 1.00f, 1.00f, 1.00f } },
        { "wall_rust",  "Rusted Wall",    MatTex::Panel,      { 1.10f, 0.78f, 0.62f } },
        { "wall_blue",  "Blue Bulkhead",  MatTex::Panel,      { 0.70f, 0.85f, 1.10f } },
        { "clean",      "Clean Panel",    MatTex::CleanPanel, { 1.00f, 1.00f, 1.00f } },
        { "floor",      "Deck Floor",     MatTex::Floor,      { 1.00f, 1.00f, 1.00f } },
        { "ceiling",    "Ceiling Panel",  MatTex::Ceiling,    { 1.00f, 1.00f, 1.00f } },
        { "concrete",   "Concrete",       MatTex::Solid,      { 0.62f, 0.61f, 0.58f } },
        { "hazard",     "Hazard Red",     MatTex::Solid,      { 0.82f, 0.20f, 0.18f } },
        { "emerald",    "Emerald",        MatTex::Solid,      { 0.18f, 0.65f, 0.42f } },
    };
    return k;
}
uint32_t editorMaterialCount() { return 10; }
int editorMaterialFind(const std::string& id) {
    if (id.empty()) return 0;                       // empty == the grid default
    const BlockoutMaterial* m = editorMaterials();
    for (uint32_t i = 0; i < editorMaterialCount(); ++i)
        if (id == m[i].id) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// Feature 3 — curated MODEL browser catalog (converted_glb props that ship in the
// repo's assets dir). relPath is resolved under the editor's mounted converted_glb
// dir; a GLB that fails to load just places nothing (graybox-safe).
// ---------------------------------------------------------------------------
const ModelCatalogItem* editorModelCatalog() {
    static const ModelCatalogItem k[] = {
        { "SciFi_Warehouse_Kit/Barrel.glb",      "Barrel" },
        { "SciFi_Warehouse_Kit/Crate Short.glb", "Crate (short)" },
        { "SciFi_Warehouse_Kit/Crate Long.glb",  "Crate (long)" },
        { "SciFi_Warehouse_Kit/Pallet.glb",      "Pallet" },
        { "SciFi_Warehouse_Kit/Fusebox 01.glb",  "Fusebox" },
        { "ModularSciFi_Interior/SM_Console.glb", "Console" },
        { "ModularSciFi_Interior/SM_Light_A.glb", "Light fixture" },
        { "ModularSciFi_Interior/SM_Pipes_A.glb", "Pipes" },
    };
    return k;
}
uint32_t editorModelCatalogCount() { return 8; }

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
    // The VIEW menu is the CAMERA menu: these three rows are live (EditorHost::dispatch
    // drives m_camMode off them). "Wireframe" used to sit here and did nothing — there is
    // no wireframe path in IRenderDevice — so it is GONE rather than shipped dead. The Cmd
    // enum keeps the id for when a wireframe pass exists.
    static const MenuItem view[] = {
        { "Orbit",       "Orbit the selection (RMB drag, wheel dolly, MMB pan)", "1", Cmd::CamOrbit },
        { "Fly",         "Free-fly camera (RMB + WASD)",         "2",      Cmd::CamFly },
        { "FPS Walk",    "Walk the level at eye height",         "3",      Cmd::CamFpsWalk },
    };
    static const Menu bar[] = {
        { "File",  file,  3 },
        { "Edit",  edit,  4 },
        { "Tools", tools, 7 },
        { "View",  view,  3 },
    };
    return bar;
}
uint32_t editorMenuBarCount() { return 4; }

// ---------------------------------------------------------------------------
// JSON emit
// ---------------------------------------------------------------------------
namespace {
std::string num(float f) {
    // %.9g: 9 significant digits round-trip ANY float bit-exactly (FLT_DECIMAL_DIG).
    // The previous %.4g was a real defect: a brush at x = 123.4567 saved as "123.5"
    // — every save/reload cycle nudged far-from-origin geometry by centimetres, and
    // no rotation angle could survive a round trip. Values already exact in 4
    // digits ("2", "0.5", "3.5") print identically, so most files do not change.
    // Fallback (house rule): X3_LEVELDOC_NUM4=1 restores the old %.4g exactly.
    static const bool legacy = [] {
        const char* v = std::getenv("X3_LEVELDOC_NUM4");
        return v && v[0] == '1';
    }();
    char buf[32];
    std::snprintf(buf, sizeof(buf), legacy ? "%.4g" : "%.9g", (double)f);
    return buf;
}
std::string vec3(const float v[3]) {
    return "[" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + "]";
}
std::string vec4(const float v[4]) {
    return "[" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + ", " + num(v[3]) + "]";
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
          << ", \"yaw\": " << num(e.yaw);
        // 11.0 third rotation axis — emitted only when set (and rot3 on), so every
        // existing yaw-only file round-trips byte-identical.
        if (rot3Enabled() && e.pitch != 0.0f) o << ", \"pitch\": " << num(e.pitch);
        if (rot3Enabled() && e.roll  != 0.0f) o << ", \"roll\": "  << num(e.roll);
        o << ", \"scale\": " << num(e.scale)
          << ", \"tint\": " << vec3(e.tint)
          << ", \"size\": " << vec3(e.size)
          << ", \"model\": \"" << esc(e.model) << "\""
          << ", \"script\": \"" << esc(e.script) << "\"";
        // Self-lit emissive (canon glow) — emitted only when set, so every
        // existing level file round-trips byte-identical.
        if (e.emissive[3] > 0.0f) o << ", \"emissive\": " << vec4(e.emissive);
        o << " }";
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
          << ", \"yaw\": " << num(b.yaw);
        if (rot3Enabled() && b.pitch != 0.0f) o << ", \"pitch\": " << num(b.pitch);
        if (rot3Enabled() && b.roll  != 0.0f) o << ", \"roll\": "  << num(b.roll);
        o << ", \"tint\": " << vec3(b.tint)
          << ", \"material\": \"" << esc(b.material) << "\""
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
    void vec4(float out[4]) {
        if (!eat('[')) { ok = false; return; }
        out[0] = number(); out[1] = number(); out[2] = number(); out[3] = number();
        eat(']');
    }
    // Read a key string followed by ':'.
    std::string key() { std::string k = str(); eat(':'); return k; }

    // Skip ONE value of any kind (string / number / array / object / literal).
    // The old skip called str() unconditionally, which set ok=false on a numeric
    // value — meaning any FUTURE numeric key would hard-fail an older parser.
    // Fixed so unknown keys are genuinely tolerated (depth-bounded recursion).
    void skipValue(int depth = 0) {
        if (depth > 16) { ok = false; return; }
        const char c = peek();
        if (c == '"') { str(); return; }
        if (c == '[') {
            eat('[');
            while (ok && peek() && peek() != ']') skipValue(depth + 1);
            eat(']');
            return;
        }
        if (c == '{') {
            eat('{');
            while (ok && peek() && peek() != '}') { key(); skipValue(depth + 1); }
            eat('}');
            return;
        }
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') { number(); return; }
        // Bare literal (true/false/null): consume the identifier run.
        ws();
        const char* s = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p;
        if (p == s) ok = false;   // nothing recognizable — a real parse error
    }
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
                    // 11.0 third axis. ALWAYS consumed (the doc may carry them);
                    // applied only when rot3 is enabled (fallback = yaw-only).
                    else if (ek == "pitch") { float v = j.number(); if (rot3Enabled()) e.pitch = v; }
                    else if (ek == "roll")  { float v = j.number(); if (rot3Enabled()) e.roll  = v; }
                    else if (ek == "scale") e.scale = j.number();
                    else if (ek == "tint")  j.vec3(e.tint);
                    else if (ek == "size")  j.vec3(e.size);
                    else if (ek == "model") e.model = j.str();
                    else if (ek == "script") e.script = j.str();
                    else if (ek == "emissive") j.vec4(e.emissive);
                    else { j.skipValue(); }   // unknown key: skip any value kind
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
                    else if (bk == "pitch")  { float v = j.number(); if (rot3Enabled()) b.pitch = v; }
                    else if (bk == "roll")   { float v = j.number(); if (rot3Enabled()) b.roll  = v; }
                    else if (bk == "tint")   j.vec3(b.tint);
                    else if (bk == "material") b.material = j.str();
                    else if (bk == "collide") b.collide = (j.number() != 0.0f);
                    else { j.skipValue(); }   // unknown key: skip any value kind
                }
                j.eat('}');
                brushes.push_back(b);
            }
            j.eat(']');
        } else {
            j.skipValue();   // unknown top-level key: skip any value kind
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

int EditorState::pickBrushRay(const float origin[3], const float dir[3], float pad) const {
    float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dl < 1e-6f) return -1;
    const float dx = dir[0]/dl, dy = dir[1]/dl, dz = dir[2]/dl;
    int best = -1; float bestT = 1e30f;
    for (size_t i = 0; i < m_doc.brushes.size(); ++i) {
        const BlockoutBrush& b = m_doc.brushes[i];
        // Transform the ray into the brush's LOCAL frame (full inverse rotation,
        // translate to origin) so the OBB becomes an axis-aligned box [-h, +h] and
        // we run a slab test. R maps local->world; the inverse is the transpose:
        // (R^T v)[i] = dot(column i of R, v). Yaw-only brushes take the exact same
        // path as before (rotYPR with pitch=roll=0 IS the old yaw matrix).
        float R[9];
        rotYPR(b.yaw, b.pitch, b.roll, R);
        const float rx0 = origin[0]-b.pos[0], ry0 = origin[1]-b.pos[1], rz0 = origin[2]-b.pos[2];
        const float ro[3] = {
            R[0]*rx0 + R[1]*ry0 + R[2]*rz0,
            R[3]*rx0 + R[4]*ry0 + R[5]*rz0,
            R[6]*rx0 + R[7]*ry0 + R[8]*rz0,
        };
        const float rd[3] = {
            R[0]*dx + R[1]*dy + R[2]*dz,
            R[3]*dx + R[4]*dy + R[5]*dz,
            R[6]*dx + R[7]*dy + R[8]*dz,
        };
        const float h[3] = { b.size[0]*0.5f + pad, b.size[1]*0.5f + pad, b.size[2]*0.5f + pad };
        float tmin = 0.0f, tmax = 1e30f; bool hit = true;
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(rd[a]) < 1e-8f) {
                if (ro[a] < -h[a] || ro[a] > h[a]) { hit = false; break; }
            } else {
                float t1 = (-h[a] - ro[a]) / rd[a];
                float t2 = ( h[a] - ro[a]) / rd[a];
                if (t1 > t2) { float tmp=t1; t1=t2; t2=tmp; }
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) { hit = false; break; }
            }
        }
        if (hit && tmax >= 0.0f && tmin < bestT) { bestT = tmin; best = (int)i; }
    }
    return best;
}

const char* EditorState::brushTypeName(uint32_t type) {
    switch (type) {
        case 1u: return "Ramp";
        case 2u: return "Cylinder";
        case 3u: return "Stairs";
        default: return "Box";
    }
}

int EditorState::addBrush(uint32_t type, const float pos[3]) {
    BlockoutBrush b;
    b.type = (type <= 3u) ? type : 0u;   // Box(0) / Ramp(1) / Cylinder(2) / Stairs(3)
    const char* stem = (b.type == 1u) ? "ramp_" : (b.type == 2u) ? "cyl_"
                     : (b.type == 3u) ? "stairs_" : "box_";
    b.name = stem + std::to_string(m_doc.brushes.size());
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

bool EditorState::rotateSelectedBrush(Axis axis, float delta) {
    if (!hasBrushSelection() || axis == Axis::None || delta == 0.0f) return false;
    // 11.0: X = pitch, Y = yaw (the 10.9 axis), Z = roll. With rot3 disabled only
    // yaw operates — the prior behaviour, exactly.
    if (!rot3Enabled() && axis != Axis::Y) return false;
    BlockoutBrush& b = m_doc.brushes[m_selIndex];
    float& a = (axis == Axis::X) ? b.pitch : (axis == Axis::Y) ? b.yaw : b.roll;
    const float before = a;
    a = snapAngle(a + delta);
    return std::fabs(a - before) > 1e-7f;
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
namespace {
// True iff transitioning between two brush states needs a full mesh+texture rebuild
// (vs a cheap transform-only sync). Size changes the mesh; material/tint changes the
// bound texture + baseColor, both applied at spawn — so either forces a Respawn.
bool brushNeedsRespawn(const BlockoutBrush& a, const BlockoutBrush& b) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a.size[i] - b.size[i]) > 1e-5f) return true;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a.tint[i] - b.tint[i]) > 1e-5f) return true;
    // 11.0: a ROTATION change on a mesh-collision brush (anything but a Box)
    // must rebuild the Jolt static mesh — its triangles are baked in world space,
    // so setBodyRotation cannot fix it up. A rotated Box stays a cheap SyncXform
    // (the box body takes a quat), preserving the yaw-drag feel Tim already has.
    if (rot3Enabled() && a.type != 0u && (a.collide || b.collide)) {
        if (std::fabs(a.yaw   - b.yaw)   > 1e-5f ||
            std::fabs(a.pitch - b.pitch) > 1e-5f ||
            std::fabs(a.roll  - b.roll)  > 1e-5f) return true;
    }
    return a.material != b.material;
}
} // namespace

void EditorState::pushCmd(const BrushCmd& c) {
    // Truncate the redo tail (any command at/after the current undo position) then
    // append + advance — the classic linear-history behaviour.
    if (m_undoPos < (int)m_history.size())
        m_history.erase(m_history.begin() + m_undoPos, m_history.end());
    BrushCmd cc = c;
    cc.group = m_group;              // 0 unless we are inside a transaction
    m_history.push_back(cc);
    m_undoPos = (int)m_history.size();
}

// ---- Undo TRANSACTIONS (see editor.h) --------------------------------------
void EditorState::beginGroup() {
    if (m_group != 0) return;        // groups do not nest; a stray begin is ignored
    // Opening a transaction TRUNCATES the redo tail up-front. Without this, an empty
    // group (one that pushes nothing) would leave the tail alive while the user
    // believes an edit happened, and the next redo would replay a stale future.
    if (m_undoPos < (int)m_history.size())
        m_history.erase(m_history.begin() + m_undoPos, m_history.end());
    m_group      = m_nextGroup++;
    m_groupStart = m_history.size();
}

void EditorState::endGroup() {
    if (m_group == 0) return;
    m_group = 0;
    // A group that pushed NOTHING (e.g. an AI plan whose every op was rejected)
    // leaves no trace: no phantom undo step that appears to do nothing.
    if (m_history.size() == m_groupStart) m_undoPos = (int)m_history.size();
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
                           std::fabs(m_editBefore.yaw - now.yaw) < 1e-5f &&
                           std::fabs(m_editBefore.pitch - now.pitch) < 1e-5f &&
                           std::fabs(m_editBefore.roll - now.roll) < 1e-5f &&
                           v3eq(m_editBefore.tint, now.tint) &&
                           m_editBefore.material == now.material;
    if (unchanged) return;
    BrushCmd c; c.kind = CmdKind::Transform; c.index = idx;
    c.before = m_editBefore; c.after = now;
    pushCmd(c);
}

HistoryEffect EditorState::undoOne() {
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
                // A pure move/yaw can sync cheaply; a size OR material/tint change must
                // respawn the mesh (the texture/baseColor is bound at spawn time).
                eff.op = brushNeedsRespawn(c.before, c.after)
                       ? HistoryEffect::Op::Respawn : HistoryEffect::Op::SyncXform;
                eff.index = c.index;
                m_selKind = SelKind::Brush; m_selIndex = c.index;
            }
            break;
        }
        default: break;
    }
    return eff;
}

HistoryEffect EditorState::redoOne() {
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
                eff.op = brushNeedsRespawn(c.before, c.after)
                       ? HistoryEffect::Op::Respawn : HistoryEffect::Op::SyncXform;
                eff.index = c.index;
                m_selKind = SelKind::Brush; m_selIndex = c.index;
            }
            break;
        }
        default: break;
    }
    return eff;
}

// Public undo/redo: unwind a whole TRANSACTION as ONE step when the command at the
// stack cursor belongs to a group (see beginGroup). undoOne() walks backwards on its
// own, so the loop naturally applies a group in reverse order — which is the only
// correct order for a batch of index-shifting inserts/erases.
HistoryEffect EditorState::undo() {
    m_groupEffects.clear();
    if (!canUndo()) return HistoryEffect{};
    const uint32_t g = m_history[m_undoPos - 1].group;
    if (g == 0) return undoOne();                       // plain single-command undo
    // Collect EVERY per-command effect: the ones with `removed` carry the dead brush's
    // live links, and the host must destroy them or the undo leaks a mesh + a body.
    while (canUndo() && m_history[m_undoPos - 1].group == g)
        m_groupEffects.push_back(undoOne());
    HistoryEffect eff; eff.op = HistoryEffect::Op::RespawnAll;
    m_selKind = SelKind::None; m_selIndex = -1;
    return eff;
}

HistoryEffect EditorState::redo() {
    m_groupEffects.clear();
    if (!canRedo()) return HistoryEffect{};
    const uint32_t g = m_history[m_undoPos].group;
    if (g == 0) return redoOne();
    while (canRedo() && m_history[m_undoPos].group == g)
        m_groupEffects.push_back(redoOne());
    HistoryEffect eff; eff.op = HistoryEffect::Op::RespawnAll;
    m_selKind = SelKind::None; m_selIndex = -1;
    return eff;
}

// ---------------------------------------------------------------------------
// Phase 5 — keyboard NUDGE (Doom-Builder Visual Mode). Each call is ONE undo step
// (begin/commit brackets the mutation) + grid-snapped. Pure doc mutation; the host
// applies the returned HistoryEffect to the live Scene + Jolt.
// ---------------------------------------------------------------------------
HistoryEffect EditorState::nudgeBrush(NudgeAction action, Axis faceAxis, float step) {
    HistoryEffect eff;
    if (!hasBrushSelection()) return eff;
    if (step <= 0.0f) step = m_grid;
    const int idx = m_selIndex;
    BlockoutBrush& b = m_doc.brushes[idx];

    // Resolve the moved axis. Height actions are always +Y (Doom floor/ceiling); the
    // rest act on the crosshair's faced axis (fall back to Z = "forward" if unknown).
    int a = (faceAxis == Axis::X) ? 0 : (faceAxis == Axis::Y) ? 1 : 2;

    bool sizeChanged = false;
    const int undoBefore = m_undoPos;
    beginBrushEdit(idx);
    switch (action) {
        case NudgeAction::MoveOut:
            b.pos[a] = snapValue(b.pos[a] + step);
            break;
        case NudgeAction::MoveIn:
            b.pos[a] = snapValue(b.pos[a] - step);
            break;
        case NudgeAction::StretchGrow:
            b.size[a] = std::max(0.25f, snapValue(b.size[a] + step));
            sizeChanged = true;
            break;
        case NudgeAction::StretchShrink:
            b.size[a] = std::max(0.25f, snapValue(b.size[a] - step));
            sizeChanged = true;
            break;
        // Height: move the TOP face. Grow/shrink Y by `step` AND shift the center by
        // half a step so the BOTTOM stays put (the top rises/falls — Doom ceiling).
        case NudgeAction::RaiseHeight: {
            float ns = std::max(0.25f, snapValue(b.size[1] + step));
            b.pos[1] = snapValue(b.pos[1] + (ns - b.size[1]) * 0.5f);
            b.size[1] = ns; sizeChanged = true; break;
        }
        case NudgeAction::LowerHeight: {
            float ns = std::max(0.25f, snapValue(b.size[1] - step));
            b.pos[1] = snapValue(b.pos[1] + (ns - b.size[1]) * 0.5f);
            b.size[1] = ns; sizeChanged = true; break;
        }
        // Floor: move the BOTTOM face. Raising the floor SHRINKS Y and lifts the center
        // by half (top stays put); lowering the floor grows Y + drops the center.
        case NudgeAction::RaiseFloor: {
            float ns = std::max(0.25f, snapValue(b.size[1] - step));
            b.pos[1] = snapValue(b.pos[1] - (ns - b.size[1]) * 0.5f);
            b.size[1] = ns; sizeChanged = true; break;
        }
        case NudgeAction::LowerFloor: {
            float ns = std::max(0.25f, snapValue(b.size[1] + step));
            b.pos[1] = snapValue(b.pos[1] - (ns - b.size[1]) * 0.5f);
            b.size[1] = ns; sizeChanged = true; break;
        }
        default:
            // Non-geometry actions (ToggleTooltip) don't touch the brush.
            m_editing = false; m_editIndex = -1; return eff;
    }
    commitBrushEdit();   // pushes one Transform command IFF something actually changed

    // If commit dropped the command (a snap that landed on the same value -> no net
    // change), there's nothing for the host to do — report op==None.
    if (m_undoPos == undoBefore) return eff;
    eff.op = sizeChanged ? HistoryEffect::Op::Respawn : HistoryEffect::Op::SyncXform;
    eff.index = idx;
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
bool feq(float a, float b, float e = 1e-3f) { return std::fabs(a-b) < e; }
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
                    feq(rt.playerStart[0], 1.5f) && feq(rt.playerStart[2], -2.0f) &&
                    rt.entities[0].name == "crate" && rt.entities[0].type == "prop" &&
                    feq(rt.entities[0].pos[0], 3.0f) && feq(rt.entities[0].yaw, 0.5f) &&
                    feq(rt.entities[0].scale, 1.2f) &&
                    rt.entities[1].type == "enemy" && feq(rt.entities[1].pos[0], 8.0f) &&
                    rt.entities[2].type == "light" && feq(rt.entities[2].pos[1], 3.0f);
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
        bool moved = feq(doc.entities[1].pos[0], 8.5f);
        ed.moveSelected(Axis::Z, -1.2f);      // 4 - 1.2 = 2.8 -> snap 3.0... actually 2.8 -> 3.0? round(2.8/0.5)=round(5.6)=6 ->3.0
        bool movedZ = feq(doc.entities[1].pos[2], 3.0f);
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
                     d2.entities[idx].type == "npc" && feq(d2.entities[idx].pos[0], 5.0f);
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
        bool pal = editorPaletteCount() == 16 && editorPalette()[0].type != nullptr &&
                   std::string(editorPalette()[15].type) == "portal";   // 11.0
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
                    feq(rt.brushes[0].size[0], 8.0f) && feq(rt.brushes[0].pos[2], -3.0f) &&
                    rt.brushes[0].collide == true &&
                    rt.brushes[1].type == 1u && feq(rt.brushes[1].yaw, 1.5708f) &&
                    feq(rt.brushes[1].size[1], 2.0f) && rt.brushes[1].collide == false;
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
                      feq(d.brushes[0].pos[2], 2.0f) && ed.canUndo() && !ed.canRedo();
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
                        feq(d.brushes[0].pos[0], 4.0f) && u.op == HistoryEffect::Op::Respawn &&
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
        bool back = feq(d.brushes[0].pos[0], 0.0f) && u.op == HistoryEffect::Op::SyncXform;
        HistoryEffect r = ed.redo();                  // forward to x=7
        bool fwd = feq(d.brushes[0].pos[0], 7.0f) && r.op == HistoryEffect::Op::SyncXform;
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
        check(u.op == HistoryEffect::Op::Respawn && feq(d.brushes[0].size[0], 2.0f),
              "E11 size edit undo asks the host to RESPAWN (rebuild mesh)");
    }

    // ---- E12 (Feature 1): material assign is undoable, RESPAWNs, + round-trips JSON. ----
    {
        // The material table + lookup is well-formed (grid is index 0; empty resolves to it).
        bool tableOk = editorMaterialCount() == 10 &&
                       std::string(editorMaterials()[0].id) == "grid" &&
                       editorMaterialFind("") == 0 && editorMaterialFind("grid") == 0 &&
                       editorMaterialFind("hazard") > 0 && editorMaterialFind("nope") == -1;

        LevelDoc d; EditorState ed(d);
        ed.setSnap(false);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);                         // brush 0, default (empty) material
        ed.selectBrush(0);
        // Assign a material as a grouped edit (mirrors the Materials panel click).
        ed.beginBrushEdit(0);
        d.brushes[0].material = "hazard";
        ed.commitBrushEdit();
        bool assigned = d.brushes[0].material == "hazard" && ed.canUndo();
        // Undo restores the empty material AND asks the host to RESPAWN (re-skin), not
        // a cheap transform-only sync (the texture is bound at spawn).
        HistoryEffect u = ed.undo();
        bool reverted = d.brushes[0].material.empty() && u.op == HistoryEffect::Op::Respawn;
        HistoryEffect r = ed.redo();
        bool redone = d.brushes[0].material == "hazard" && r.op == HistoryEffect::Op::Respawn;
        // The material survives a JSON save->load round-trip.
        LevelDoc rt; bool parsed = rt.fromJson(d.toJson());
        bool roundtrip = parsed && rt.brushes.size() == 1 && rt.brushes[0].material == "hazard";
        check(tableOk && assigned && reverted && redone && roundtrip,
              "E12 material assign: undoable (RESPAWN) + JSON round-trips the brush surface");
    }

    // ---- E13 (Feature 2): true OBB raycast pick is TIGHT on a long thin brush. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(false);
        // A long thin wall along Z at the origin (0.5 wide x 3 tall x 12 long): the OBB
        // hugs x in [-0.25,0.25], z in [-6,6]. brush index 0.
        { BlockoutBrush b; b.pos[0]=0; b.pos[1]=1.5f; b.pos[2]=0;
          b.size[0]=0.5f; b.size[1]=3.0f; b.size[2]=12.0f; d.brushes.push_back(b); }
        // Ray straight DOWN onto the wall's far end (x=0, z=5): inside the thin slab -> HIT.
        float oHit[3] = { 0.0f, 20.0f, 5.0f }, dn[3] = { 0, -1, 0 };
        int hit = ed.pickBrushRay(oHit, dn);
        // Ray straight DOWN at x=2 (well OUTSIDE the 0.5 m wall width) over z=5: a tight
        // OBB test MISSES. (A loose center-distance pick using the 12 m extent as a fat
        // radius would falsely grab the wall here — the regression Feature 2 fixes.)
        float oMiss[3] = { 2.0f, 20.0f, 5.0f };
        int miss = ed.pickBrushRay(oMiss, dn);
        check(hit == 0 && miss == -1,
              "E13 OBB raycast pick is tight (inside thin slab hits; 2 m off-axis misses)");
    }

    // ---- E14 (Feature 2): rotate + scale gizmo deltas group into one undo step each. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(false);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);                          // brush 0 (2 m cube, yaw 0)
        // ROTATE: a yaw drag (mirrors the gizmo writing b.yaw across frames) = 1 undo step.
        ed.beginBrushEdit(0);
        d.brushes[0].yaw = 0.3f;
        d.brushes[0].yaw = 0.7854f;                     // 45 deg
        ed.commitBrushEdit();
        HistoryEffect ru = ed.undo();                   // back to yaw 0 (cheap SyncXform)
        bool rotOk = feq(d.brushes[0].yaw, 0.0f) && ru.op == HistoryEffect::Op::SyncXform;
        ed.redo();                                      // forward to 45 deg
        bool rotFwd = feq(d.brushes[0].yaw, 0.7854f);
        // SCALE: a size drag = 1 undo step, and undo asks the host to RESPAWN (mesh rebuild).
        ed.beginBrushEdit(0);
        d.brushes[0].size[0] = 5.0f;                    // grow X
        ed.commitBrushEdit();
        HistoryEffect su = ed.undo();
        bool scaleOk = feq(d.brushes[0].size[0], 2.0f) && su.op == HistoryEffect::Op::Respawn;
        check(rotOk && rotFwd && scaleOk,
              "E14 rotate(yaw)=SyncXform + scale(size)=Respawn, each one undo step");
    }

    // ---- E15 (Feature 3): a placed MODEL entity round-trips through the JSON. ----
    {
        bool catOk = editorModelCatalogCount() == 8 &&
                     editorModelCatalog()[0].relPath != nullptr &&
                     std::string(editorModelCatalog()[0].relPath).find(".glb") != std::string::npos;

        LevelDoc d; EditorState ed(d);
        float p[3] = { 2, 0, -3 };
        int idx = ed.addEntity("model", p);
        d.entities[idx].model = "SciFi_Warehouse_Kit/Barrel.glb";
        d.entities[idx].yaw = 0.5f; d.entities[idx].scale = 1.5f;
        // Save -> load: the model relpath + transform survive.
        LevelDoc rt; bool parsed = rt.fromJson(d.toJson());
        bool roundtrip = parsed && rt.entities.size() == 1 &&
                         rt.entities[0].type == "model" &&
                         rt.entities[0].model == "SciFi_Warehouse_Kit/Barrel.glb" &&
                         feq(rt.entities[0].pos[0], 2.0f) && feq(rt.entities[0].pos[2], -3.0f) &&
                         feq(rt.entities[0].yaw, 0.5f) && feq(rt.entities[0].scale, 1.5f);
        check(catOk && roundtrip,
              "E15 model browser: placed GLB entity round-trips through the LevelDoc JSON");
    }

    // ---- E16 (Phase 5): keyboard MOVE nudge = snapped pos + ONE undo step. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(true, 0.5f);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);                          // brush 0 at origin (1 undo step)
        ed.selectBrush(0);
        int undoBefore = 0; (void)undoBefore;
        // Move OUT along Z by a 0.5 step -> z = 0.5, reported as a cheap SyncXform.
        HistoryEffect e1 = ed.nudgeBrush(NudgeAction::MoveOut, Axis::Z, 0.5f);
        bool moved = feq(d.brushes[0].pos[2], 0.5f) && e1.op == HistoryEffect::Op::SyncXform;
        // The nudge is ONE undo step: undo restores z=0.
        HistoryEffect u = ed.undo();
        bool oneStep = feq(d.brushes[0].pos[2], 0.0f) && u.op == HistoryEffect::Op::SyncXform;
        // Move IN along X -> x = -0.5.
        ed.redo();                                      // back to z=0.5
        HistoryEffect e2 = ed.nudgeBrush(NudgeAction::MoveIn, Axis::X, 0.5f);
        bool movedIn = feq(d.brushes[0].pos[0], -0.5f) && e2.op == HistoryEffect::Op::SyncXform;
        check(moved && oneStep && movedIn,
              "E16 keyboard MOVE nudge: snapped pos, one undo step, SyncXform sync");
    }

    // ---- E17 (Phase 5): STRETCH + HEIGHT nudges resize (Respawn), top/bottom logic. ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(true, 0.5f);
        float p[3] = { 0, 1, 0 };
        ed.addBrushCmd(0u, p);                          // 2 m cube centered at y=1
        ed.selectBrush(0);
        // STRETCH grow on X by 1 step -> size.x 2 -> 2.5, host must RESPAWN (mesh rebuild).
        HistoryEffect s = ed.nudgeBrush(NudgeAction::StretchGrow, Axis::X, 0.5f);
        bool grew = feq(d.brushes[0].size[0], 2.5f) && s.op == HistoryEffect::Op::Respawn;
        // RAISE CEILING: top was y=2 (center 1 + half 1). Raise by 1 -> size.y 2->3,
        // center shifts up by 0.5 (to 1.5) so the BOTTOM stays at y=0, top -> 3.
        HistoryEffect h = ed.nudgeBrush(NudgeAction::RaiseHeight, Axis::Y, 1.0f);
        const float top = d.brushes[0].pos[1] + d.brushes[0].size[1]*0.5f;
        const float bot = d.brushes[0].pos[1] - d.brushes[0].size[1]*0.5f;
        bool raised = feq(d.brushes[0].size[1], 3.0f) && feq(top, 3.0f) && feq(bot, 0.0f) &&
                      h.op == HistoryEffect::Op::Respawn;
        // RAISE FLOOR: bottom was 0. Raise floor by 1 -> size.y 3->2, center up by 0.5
        // (to 2.0) so the TOP stays at 3, bottom -> 1.
        HistoryEffect f = ed.nudgeBrush(NudgeAction::RaiseFloor, Axis::Y, 1.0f);
        const float top2 = d.brushes[0].pos[1] + d.brushes[0].size[1]*0.5f;
        const float bot2 = d.brushes[0].pos[1] - d.brushes[0].size[1]*0.5f;
        bool floor = feq(d.brushes[0].size[1], 2.0f) && feq(top2, 3.0f) && feq(bot2, 1.0f) &&
                     f.op == HistoryEffect::Op::Respawn;
        check(grew && raised && floor,
              "E17 keyboard STRETCH=Respawn + RAISE ceiling/floor move the right face");
    }

    // ---- E18 (Phase 5): the keybind TABLE maps actions<->keys + rebind updates it. ----
    {
        KeybindTable kb;
        // Defaults: the classic Doom-Builder map (wheel = ceiling, arrows = move, brackets
        // = stretch). actionForKey + keyFor are inverse over the table.
        bool dflt = kb.keyFor(NudgeAction::RaiseHeight) == kKeyMouseWheelUp &&
                    kb.keyFor(NudgeAction::LowerHeight) == kKeyMouseWheelDown &&
                    kb.actionForKey(kKeyMouseWheelUp) == NudgeAction::RaiseHeight &&
                    kb.actionForKey(0) == NudgeAction::Count &&        // 0 = unbound
                    kb.count() == (uint32_t)NudgeAction::Count;
        // Rebind MoveOut to an arbitrary key code; keyFor + actionForKey both reflect it.
        const int kFoo = 70000;
        bool rebound = kb.rebind(NudgeAction::MoveOut, kFoo) &&
                       kb.keyFor(NudgeAction::MoveOut) == kFoo &&
                       kb.actionForKey(kFoo) == NudgeAction::MoveOut;
        // Rebinding a key already held by another action STEALS it (no two share a key).
        int moveInKey = kb.keyFor(NudgeAction::MoveIn);
        bool steal = kb.rebind(NudgeAction::MoveOut, moveInKey) &&
                     kb.keyFor(NudgeAction::MoveOut) == moveInKey &&
                     kb.keyFor(NudgeAction::MoveIn) == 0 &&            // stolen -> unbound
                     kb.actionForKey(moveInKey) == NudgeAction::MoveOut;
        // Reset restores the defaults.
        kb.resetDefaults();
        bool reset = kb.keyFor(NudgeAction::RaiseHeight) == kKeyMouseWheelUp &&
                     kb.actionForKey(kFoo) == NudgeAction::Count;
        check(dflt && rebound && steal && reset,
              "E18 keybind table: defaults + actionForKey<->keyFor + rebind steals + reset");
    }

    // ---- E19 (11.0): rotateSelectedBrush — three axes, 5-degree angle snap,
    // NEGATIVE-CONTROLLED (the same input with snap off gives a DIFFERENT angle,
    // so a broken snap cannot pass both arms). ----
    {
        LevelDoc d; EditorState ed(d);
        ed.setSnap(true, 0.5f);
        float p[3] = { 0, 0, 0 };
        ed.addBrushCmd(0u, p);
        ed.selectBrush(0);
        // X (pitch): +0.10 rad snaps to one 5-deg step (0.0872665).
        bool px = ed.rotateSelectedBrush(Axis::X, 0.10f) &&
                  feq(d.brushes[0].pitch, kAngleSnapRad, 1e-5f);
        // Y (yaw, Tim's axis): -0.16 rad snaps to two steps down (-0.1745329).
        bool py = ed.rotateSelectedBrush(Axis::Y, -0.16f) &&
                  feq(d.brushes[0].yaw, -2.0f * kAngleSnapRad, 1e-5f);
        // Z (roll): +0.04 rad snaps BACK to zero -> no change, returns false.
        bool pzSnap = !ed.rotateSelectedBrush(Axis::Z, 0.04f) &&
                      d.brushes[0].roll == 0.0f;
        // NEGATIVE CONTROL: snap OFF, the identical nudge lands raw at 0.04.
        ed.setSnap(false);
        bool pzRaw = ed.rotateSelectedBrush(Axis::Z, 0.04f) &&
                     feq(d.brushes[0].roll, 0.04f, 1e-6f) &&
                     d.brushes[0].roll != 0.0f;
        check(px && py && pzSnap && pzRaw,
              "E19 rotateSelectedBrush: X/Y/Z + 5-deg snap (negative control: raw when off)");
    }

    // ---- E20 (11.0): pickBrushRay honors the THIRD axis. A flat slab is hit from
    // above; ROLLED 90 deg it becomes a thin vertical wall and the same ray MUST
    // miss (this check fails outright if pitch/roll are ignored by the pick). ----
    {
        LevelDoc d; EditorState ed(d);
        { BlockoutBrush b; b.pos[0]=0; b.pos[1]=0; b.pos[2]=0;
          b.size[0]=4.0f; b.size[1]=0.5f; b.size[2]=4.0f; d.brushes.push_back(b); }
        const float dn[3] = { 0, -1, 0 };
        const float oA[3] = { 1.5f, 10.0f, 0.0f };
        int hitFlat = ed.pickBrushRay(oA, dn);          // x=1.5 inside the 4 m slab
        d.brushes[0].roll = 1.5707963f;                 // roll 90 deg about +Z
        int missRolled = ed.pickBrushRay(oA, dn);       // slab now 0.5 m wide in X
        const float oB[3] = { 0.1f, 10.0f, 0.0f };
        int hitRolled = ed.pickBrushRay(oB, dn);        // x=0.1 still inside
        check(hitFlat == 0 && missRolled == -1 && hitRolled == 0,
              "E20 OBB pick honors roll (flat slab hits; rolled wall misses off-axis)");
    }

    // ---- E21 (11.0): pitch/roll round-trip the JSON BIT-EXACTLY, and a zero
    // rotation emits NO pitch/roll keys (existing files stay byte-identical —
    // the presence probe is the negative control: it fails if we start always-
    // emitting). ----
    {
        LevelDoc d;
        { BlockoutBrush b; b.name = "tiltwall";
          b.pos[0]=123.4567f; b.pos[1]=-0.125f; b.pos[2]=42.42f;
          b.yaw = 0.78539816f; b.pitch = 0.12345679f; b.roll = -1.1071487f;
          d.brushes.push_back(b); }
        { EditorEntity e; e.name = "gate"; e.type = "portal";
          e.pos[0]=5; e.pos[1]=2; e.pos[2]=1;
          e.yaw = 0.9f; e.pitch = 0.25f; e.roll = -0.1f;
          e.size[0]=3; e.size[1]=3; e.size[2]=0.4f; e.script = "tunnel_a_west";
          d.entities.push_back(e); }
        LevelDoc rt; bool parsed = rt.fromJson(d.toJson());
        // BIT-exact: float == float, no epsilon. %.9g + strtod guarantees this.
        bool brushExact = parsed && rt.brushes.size() == 1 &&
                          rt.brushes[0].yaw   == d.brushes[0].yaw &&
                          rt.brushes[0].pitch == d.brushes[0].pitch &&
                          rt.brushes[0].roll  == d.brushes[0].roll &&
                          rt.brushes[0].pos[0] == d.brushes[0].pos[0];
        bool portalExact = parsed && rt.entities.size() == 1 &&
                           rt.entities[0].type == "portal" &&
                           rt.entities[0].yaw   == d.entities[0].yaw &&
                           rt.entities[0].pitch == d.entities[0].pitch &&
                           rt.entities[0].roll  == d.entities[0].roll &&
                           rt.entities[0].size[0] == 3.0f &&
                           rt.entities[0].script == "tunnel_a_west";
        // Zero-rotation docs must not grow the new keys.
        LevelDoc plain; { BlockoutBrush b; b.name="box"; plain.brushes.push_back(b); }
        const std::string js = plain.toJson();
        bool clean = js.find("\"pitch\"") == std::string::npos &&
                     js.find("\"roll\"")  == std::string::npos;
        check(brushExact && portalExact && clean,
              "E21 third-axis rotation + portal round-trip bit-exact; zero rot emits no keys");
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
                    feq(rt.brushes[0].pos[0], 3.5f) && feq(rt.brushes[0].pos[1], 1.25f) &&
                    feq(rt.brushes[0].pos[2], -7.0f) &&
                    feq(rt.brushes[0].size[0], 4.0f) && feq(rt.brushes[0].size[1], 2.5f) &&
                    feq(rt.brushes[0].size[2], 6.0f) && feq(rt.brushes[0].yaw, 0.7854f) &&
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
                  feq(doc.brushes[0].pos[0], 2.5f) &&   // 2.3 -> snap 2.5
                  feq(doc.brushes[0].pos[2], -4.5f);    // -4.7 -> snap -4.5
        check(ok, "B1 addBrush snaps pos to grid + selects the brush");
    }

    // ---- B2: resizeSelectedBrush grows a face with grid snap + a min clamp. ----
    {
        LevelDoc doc; EditorState ed(doc);
        ed.setSnap(true, 0.5f);
        float p[3] = { 0, 0, 0 };
        ed.addBrush(0u, p);                       // 2 m cube
        ed.resizeSelectedBrush(Axis::X, 1.3f);    // 2 + 1.3 = 3.3 -> snap 3.5
        bool grew = feq(doc.brushes[0].size[0], 3.5f);
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
        bool centered = feq(mn[0],-2.0f) && feq(mx[0],2.0f) &&
                        feq(mn[1],-1.0f) && feq(mx[1],1.0f) &&
                        feq(mn[2],-3.0f) && feq(mx[2],3.0f);
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

    // ---- B5: Cylinder + Stairs build valid, BOUNDING-BOX-EXACT, origin-centered geo.
    // The bbox must equal `size` exactly: the OBB pick, the gizmo and the AABB collision
    // body all assume size[] IS the brush's extent, so a primitive that under/overfills
    // its box would be un-clickable or would collide with air. ----
    {
        float size[3] = { 3.0f, 2.0f, 5.0f };
        auto bboxOk = [&](const x3::prims::PrimMesh& m) {
            if (m.verts.empty() || m.index.size() % 3 != 0) return false;
            if (m.cverts.size() != m.verts.size() * 3 || m.cindex.size() != m.index.size()) return false;
            float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
            for (const auto& v : m.verts)
                for (int a = 0; a < 3; ++a) { mn[a]=std::min(mn[a],v.pos[a]); mx[a]=std::max(mx[a],v.pos[a]); }
            for (int a = 0; a < 3; ++a)
                if (!feq(mn[a], -size[a]*0.5f, 1e-3f) || !feq(mx[a], size[a]*0.5f, 1e-3f)) return false;
            return true;
        };
        x3::prims::PrimMesh cyl = x3::prims::buildBrushMesh(x3::prims::BrushType::Cylinder, size);
        x3::prims::PrimMesh st  = x3::prims::buildBrushMesh(x3::prims::BrushType::Stairs,   size);
        // Stairs must be actual STEPS: 2 m rise / 0.25 m risers = 8 treads = 8 boxes.
        const bool steps = st.verts.size() == 8u * 24u;
        check(bboxOk(cyl) && bboxOk(st) && steps,
              "B5 Cylinder + Stairs: origin-centered, bbox == size, stairs are stepped solids");
    }

    // ---- B6: a Cylinder + a Stairs brush round-trip their `type` through the JSON. ----
    {
        LevelDoc doc;
        { BlockoutBrush b; b.name="col"; b.type=2; b.size[0]=1; b.size[1]=4; b.size[2]=1; doc.brushes.push_back(b); }
        { BlockoutBrush b; b.name="flight"; b.type=3; b.size[0]=2; b.size[1]=3; b.size[2]=6; doc.brushes.push_back(b); }
        LevelDoc rt; bool ok = rt.fromJson(doc.toJson());
        check(ok && rt.brushes.size() == 2 &&
              rt.brushes[0].type == 2u && rt.brushes[1].type == 3u &&
              feq(rt.brushes[1].size[2], 6.0f),
              "B6 Cylinder(2) + Stairs(3) round-trip through the brushes[] JSON");
    }

    x3::logInfo(std::string("[blockout-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::editor
