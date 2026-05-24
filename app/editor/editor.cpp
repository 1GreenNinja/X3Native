// Native Level Editor E1 — see app/editor/editor.h.
//
// Clean-room: a minimal JSON writer + a focused recursive-descent parser for the
// subset we emit, plus pure selection/gizmo logic. No third-party JSON lib.
#include "editor.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace x3::editor {

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

    x3::logInfo(std::string("[editor-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::editor
