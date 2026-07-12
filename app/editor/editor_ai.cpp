// The AI Architect — plan parsing, validation, and transactional apply.
// See editor_ai.h for the design laws. Pure logic: no ImGui, no Vulkan, no llama.
#include "editor_ai.h"
#include "../json_mini.h"

namespace jm = x3::game::jmini;

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::editor {

// ---------------------------------------------------------------------------
// The system prompt. Lives beside the parser so schema and prompt cannot drift.
// ---------------------------------------------------------------------------
std::string aiSystemPrompt() {
    return
"You are the AI Architect inside a 3D level editor. You turn a designer's request into a\n"
"STRICT JSON plan of edit operations. You NEVER write prose outside the JSON.\n"
"\n"
"Reply with EXACTLY one JSON object:\n"
"{\n"
"  \"summary\": \"<one short line describing what you did>\",\n"
"  \"ops\": [ ... ]\n"
"}\n"
"\n"
"Op forms (use only these):\n"
"  {\"op\":\"add_brush\",\"brush\":\"box\"|\"ramp\",\"name\":\"wall_n\",\n"
"   \"pos\":[x,y,z],\"size\":[sx,sy,sz],\"yaw\":0.0,\"material\":\"concrete\"}\n"
"  {\"op\":\"move_brush\",\"index\":3,\"pos\":[x,y,z]}\n"
"  {\"op\":\"set_material\",\"index\":3,\"material\":\"metal\"}\n"
"  {\"op\":\"delete_brush\",\"index\":3}\n"
"  {\"op\":\"set_player_start\",\"pos\":[x,y,z]}\n"
"\n"
"AXES — READ THIS TWICE. Getting these three lines wrong is the ONE mistake that is\n"
"actually made here, every time, and it silently produces nonsense that still parses:\n"
"      pos = [ X, Y, Z ]   and   size = [ X, Y, Z ]   -- SAME order, always.\n"
"      X = LEFT  <-> RIGHT   (width)\n"
"      Y = DOWN  <-> UP      (HEIGHT)   <-- Y IS HEIGHT. Never thickness. Never depth.\n"
"      Z = OUT   <-> IN      (depth, toward you / away from you)\n"
"  So size = [ WIDTH, HEIGHT, DEPTH ].\n"
"\n"
"  A WALL IS TALL. Its Y is the ROOM HEIGHT (e.g. 3.0). Its THIN 0.2 goes in whichever\n"
"  HORIZONTAL axis it faces along — X or Z, NEVER Y.\n"
"      a wall running LEFT-RIGHT  is thin in Z -> size [ roomW, height, 0.2   ]\n"
"      a wall running IN-OUT      is thin in X -> size [ 0.2,   height, roomD ]\n"
"      a floor or ceiling         is thin in Y -> size [ roomW, 0.2,    roomD ]\n"
"  If you ever write 0.2 as the SECOND number of a wall, you have built a floor floating\n"
"  in mid-air. That is the mistake. Do not make it.\n"
"\n"
"WORKED EXAMPLE — an 8 (wide) x 6 (deep) room, 3 m high, on a floor already at\n"
"[0,-0.1,0], with a 2 m doorway in the far wall (so that wall is split around the gap):\n"
"{\"summary\":\"8x6 room, 3m high, doorway in the far wall\",\"ops\":[\n"
" {\"op\":\"add_brush\",\"name\":\"wall_near\",\"pos\":[0,1.5,-3],\"size\":[8,3,0.2]},\n"
" {\"op\":\"add_brush\",\"name\":\"wall_right\",\"pos\":[4,1.5,0],\"size\":[0.2,3,6]},\n"
" {\"op\":\"add_brush\",\"name\":\"wall_left\",\"pos\":[-4,1.5,0],\"size\":[0.2,3,6]},\n"
" {\"op\":\"add_brush\",\"name\":\"wall_far_l\",\"pos\":[-2.5,1.5,3],\"size\":[3,3,0.2]},\n"
" {\"op\":\"add_brush\",\"name\":\"wall_far_r\",\"pos\":[2.5,1.5,3],\"size\":[3,3,0.2]},\n"
" {\"op\":\"add_brush\",\"name\":\"ceiling\",\"pos\":[0,3.1,0],\"size\":[8,0.2,6]}\n"
"]}\n"
"Every wall has Y = 3 (the HEIGHT) and sits at pos.y = 1.5 (half of it). The ceiling is\n"
"the only piece with 0.2 in Y, and it sits ABOVE at 3.1. The two far pieces are 3 m each,\n"
"leaving a 2 m gap between them. Copy this SHAPE of reasoning.\n"
"\n"
"RULES — a plan that breaks these is rejected:\n"
"  * Units are METRES. `pos` is the brush CENTER. `size` is FULL extents.\n"
"  * A room is built from SOLID brushes: a floor, a ceiling, and four walls. Leave a\n"
"    gap (or omit a wall segment) where a doorway belongs. Do NOT emit a single hollow\n"
"    box and call it a room — there is no CSG; a box is a solid block.\n"
"  * A wall's center must be offset by half the room's span, and a floor's center sits\n"
"    HALF ITS THICKNESS BELOW the floor plane (a 0.2m-thick floor at y=0 has pos.y=-0.1).\n"
"  * Coordinates must satisfy |x|,|y|,|z| <= 250. Every size component must be within\n"
"    [0.25, 80]. Never emit NaN, Infinity, or scientific notation.\n"
"  * At most 64 ops. Indices must refer to brushes that exist in the level given to you.\n"
"  * Keep it MINIMAL: emit only the brushes the request actually needs.\n";
}

// ---------------------------------------------------------------------------
// Compact level context. The indices here ARE the indices the model must cite.
// ---------------------------------------------------------------------------
std::string describeLevel(const LevelDoc& doc, int selectedBrush) {
    char buf[256];
    std::string s = "LEVEL \"" + doc.name + "\" (biome " + doc.biome + ")\n";
    std::snprintf(buf, sizeof buf, "player_start: [%.2f, %.2f, %.2f]\n",
                  doc.playerStart[0], doc.playerStart[1], doc.playerStart[2]);
    s += buf;
    std::snprintf(buf, sizeof buf, "brushes: %d\n", (int)doc.brushes.size());
    s += buf;
    for (size_t i = 0; i < doc.brushes.size(); ++i) {
        const BlockoutBrush& b = doc.brushes[i];
        std::snprintf(buf, sizeof buf,
                      "  [%d] %s %s pos[%.2f,%.2f,%.2f] size[%.2f,%.2f,%.2f] yaw %.2f%s\n",
                      (int)i, (b.type == 1 ? "ramp" : "box"),
                      b.name.empty() ? "-" : b.name.c_str(),
                      b.pos[0], b.pos[1], b.pos[2],
                      b.size[0], b.size[1], b.size[2], b.yaw,
                      ((int)i == selectedBrush) ? "  <-- SELECTED" : "");
        s += buf;
    }
    if (selectedBrush < 0) s += "(nothing selected)\n";
    return s;
}

// ---------------------------------------------------------------------------
// Parsing.
// ---------------------------------------------------------------------------
namespace {

bool finite3(const float v[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// Read a [x,y,z] array. Missing/short/non-numeric -> false (never a silent zero:
// a defaulted coordinate silently drops geometry at the origin, which reads as
// "the AI ignored me" and is impossible to debug from the viewport).
bool readVec3(const jm::JVal* v, float out[3]) {
    if (!v || v->t != jm::JVal::Arr || v->arr.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (v->arr[i].t != jm::JVal::Num) return false;
        out[i] = (float)v->arr[i].num;
    }
    return finite3(out);
}

// Models wrap JSON in prose and ``` fences constantly. Extract the outermost
// balanced {...} span rather than demanding a clean document.
bool extractJsonObject(const std::string& text, std::string& out) {
    const size_t start = text.find('{');
    if (start == std::string::npos) return false;
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) { out = text.substr(start, i - start + 1); return true; }
        }
    }
    return false;   // unbalanced (a truncated generation) -> not usable
}

void reject(std::string& err, int opIndex, const char* why) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "op[%d] rejected: %s\n", opIndex, why);
    err += buf;
}

} // namespace

bool parseAiPlan(const std::string& modelText, AiPlan& out, std::string& err,
                 const AiLimits& lim) {
    out = AiPlan{};
    err.clear();

    std::string js;
    if (!extractJsonObject(modelText, js)) {
        err = "no JSON object in the model reply (truncated or pure prose)";
        return false;
    }

    jm::JReader rd(js);
    jm::JVal root = rd.parse();
    if (!rd.ok || root.t != jm::JVal::Obj) {
        err = "malformed JSON in the model reply";
        return false;
    }

    out.summary = root.sval("summary", "");

    const jm::JVal* ops = root.get("ops");
    if (!ops || ops->t != jm::JVal::Arr) {
        err = "the plan has no \"ops\" array";
        return false;
    }
    if (ops->arr.size() > lim.maxOps) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "plan has %d ops, limit is %d — refusing the whole plan\n",
                      (int)ops->arr.size(), (int)lim.maxOps);
        err += buf;
        return false;      // a runaway plan is refused WHOLESALE, never truncated:
                           // half a city is worse than no city.
    }

    for (size_t i = 0; i < ops->arr.size(); ++i) {
        const jm::JVal& o = ops->arr[i];
        const int oi = (int)i;
        if (o.t != jm::JVal::Obj) { reject(err, oi, "not an object"); continue; }

        const std::string op = o.sval("op", "");
        AiOp a;

        auto checkPos = [&](void) -> bool {
            if (!readVec3(o.get("pos"), a.pos)) { reject(err, oi, "bad/missing pos"); return false; }
            for (int k = 0; k < 3; ++k)
                if (std::fabs(a.pos[k]) > lim.maxAbsCoord) {
                    reject(err, oi, "pos outside the world box"); return false;
                }
            return true;
        };
        auto checkIndex = [&](void) -> bool {
            const jm::JVal* iv = o.get("index");
            if (!iv || iv->t != jm::JVal::Num) { reject(err, oi, "bad/missing index"); return false; }
            a.index = (int)iv->num;
            if (a.index < 0) { reject(err, oi, "negative index"); return false; }
            return true;
        };

        if (op == "add_brush") {
            a.kind = AiOpKind::AddBrush;
            const std::string bt = o.sval("brush", "box");
            a.brushType = (bt == "ramp") ? 1u : 0u;
            if (!checkPos()) continue;
            if (!readVec3(o.get("size"), a.size)) { reject(err, oi, "bad/missing size"); continue; }
            bool badSize = false;
            for (int k = 0; k < 3; ++k)
                if (a.size[k] < lim.minSize || a.size[k] > lim.maxSize) badSize = true;
            if (badSize) { reject(err, oi, "size out of range [0.05, 80]"); continue; }
            const jm::JVal* yv = o.get("yaw");
            a.yaw = (yv && yv->t == jm::JVal::Num) ? (float)yv->num : 0.0f;
            if (!std::isfinite(a.yaw)) { reject(err, oi, "non-finite yaw"); continue; }
            a.name     = o.sval("name", "");
            a.material = o.sval("material", "");
        }
        else if (op == "move_brush") {
            a.kind = AiOpKind::MoveBrush;
            if (!checkIndex()) continue;
            if (!checkPos())   continue;
        }
        else if (op == "set_material") {
            a.kind = AiOpKind::SetMaterial;
            if (!checkIndex()) continue;
            a.material = o.sval("material", "");
            if (a.material.empty()) { reject(err, oi, "set_material with no material"); continue; }
        }
        else if (op == "delete_brush") {
            a.kind = AiOpKind::DeleteBrush;
            if (!checkIndex()) continue;
        }
        else if (op == "set_player_start") {
            a.kind = AiOpKind::SetPlayerStart;
            if (!checkPos()) continue;
        }
        else {
            reject(err, oi, "unknown op");
            continue;
        }
        out.ops.push_back(a);
    }

    if (out.ops.empty()) {
        if (err.empty()) err = "the plan contained no valid ops";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transactional apply.
// ---------------------------------------------------------------------------
int applyAiPlan(EditorState& st, const AiPlan& plan) {
    if (plan.ops.empty()) return 0;

    LevelDoc& doc = st.doc();
    int applied = 0;

    st.beginGroup();                     // ONE sentence = ONE Ctrl+Z (editor.h law 3)

    // Pass 1: everything that does not shift indices.
    for (const AiOp& o : plan.ops) {
        switch (o.kind) {
            case AiOpKind::AddBrush: {
                const int idx = st.addBrushCmd(o.brushType, o.pos);
                if (idx < 0) break;
                // addBrushCmd creates a default-sized brush; the size/yaw/skin land as
                // a second command in the SAME group, so it is still one undo step.
                st.beginBrushEdit(idx);
                BlockoutBrush& b = doc.brushes[idx];
                b.size[0] = o.size[0]; b.size[1] = o.size[1]; b.size[2] = o.size[2];
                b.yaw     = o.yaw;
                if (!o.name.empty())     b.name     = o.name;
                if (!o.material.empty()) b.material = o.material;
                st.commitBrushEdit();
                ++applied;
                break;
            }
            case AiOpKind::MoveBrush: {
                if (o.index < 0 || o.index >= (int)doc.brushes.size()) break;
                st.beginBrushEdit(o.index);
                BlockoutBrush& b = doc.brushes[o.index];
                b.pos[0] = o.pos[0]; b.pos[1] = o.pos[1]; b.pos[2] = o.pos[2];
                st.commitBrushEdit();
                ++applied;
                break;
            }
            case AiOpKind::SetMaterial: {
                if (o.index < 0 || o.index >= (int)doc.brushes.size()) break;
                st.beginBrushEdit(o.index);
                doc.brushes[o.index].material = o.material;
                st.commitBrushEdit();
                ++applied;
                break;
            }
            case AiOpKind::SetPlayerStart: {
                doc.playerStart[0] = o.pos[0];
                doc.playerStart[1] = o.pos[1];
                doc.playerStart[2] = o.pos[2];
                ++applied;      // not brush history; survives undo (by design: it is
                                // a doc property, and the blockout stack is brush-only)
                break;
            }
            default: break;
        }
    }

    // Pass 2: DELETES, descending. The model cited indices against the doc as it was
    // DESCRIBED to it. Adds only append, so those indices are still valid here; and
    // deleting from the back never invalidates a smaller index still to come.
    std::vector<int> dead;
    for (const AiOp& o : plan.ops)
        if (o.kind == AiOpKind::DeleteBrush) dead.push_back(o.index);
    std::sort(dead.begin(), dead.end(), std::greater<int>());
    dead.erase(std::unique(dead.begin(), dead.end()), dead.end());
    for (int idx : dead) {
        if (idx < 0 || idx >= (int)doc.brushes.size()) continue;
        st.selectBrush(idx);
        if (st.deleteSelectedBrushCmd()) ++applied;
    }

    st.endGroup();
    return applied;
}

} // namespace x3::editor
