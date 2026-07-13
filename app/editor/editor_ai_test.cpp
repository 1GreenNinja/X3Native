// --test-editor-ai — the AI Architect's self-test. Deterministic: no model, no GPU,
// no network, no filesystem.
//
// The point of this file is NOT to prove the happy path works — it always does. It is
// to prove the BAD paths are refused. A language model will, given enough prompts,
// emit NaN, 1e30, negative extents, 200-brush runaways, indices that don't exist, and
// its JSON wrapped in an apology. This validator is the only thing standing between a
// hallucinated token and the user's level, so every case below is a failure a real
// model actually produces.
//
// Reporting goes through x3::logInfo, like every other self-test in the repo — one
// stream, one format, and it lands in whatever sink the host has configured.
// (For the record, since a wrong explanation is worse than none: printf WOULD also
// work here. app/CMakeLists.txt:1 says add_executable(... WIN32 ...), but :176-178
// then overrides it with /SUBSYSTEM:CONSOLE precisely so dev builds keep a console.
// The exe is x64/PE32+ — CMake's WIN32 keyword picks the SUBSYSTEM, never the
// architecture. Don't re-derive that; it's been checked.)
#include "editor_ai.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::editor {

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        x3::logInfo(std::string("[editor-ai]   FAIL: ") + what);
    }
}

} // namespace

bool runEditorAiSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("[editor-ai] AI Architect self-test (parse / validate / transact)");

    // ---- 1. The happy path. ----
    {
        AiPlan p; std::string err;
        const bool ok = parseAiPlan(
            "{\"summary\":\"floor + wall\",\"ops\":["
            "{\"op\":\"add_brush\",\"brush\":\"box\",\"name\":\"floor\",\"pos\":[0,-0.1,0],"
            "\"size\":[8,0.2,6],\"material\":\"concrete\"},"
            "{\"op\":\"add_brush\",\"brush\":\"box\",\"pos\":[0,1.5,3],\"size\":[8,3,0.2]}"
            "]}", p, err);
        check(ok, "well-formed plan parses");
        check(p.ops.size() == 2, "two ops survive");
        check(p.summary == "floor + wall", "summary is read");
        check(!p.ops.empty() && p.ops[0].kind == AiOpKind::AddBrush, "op kind is AddBrush");
        check(!p.ops.empty() && std::fabs(p.ops[0].size[0] - 8.0f) < 1e-4f, "size round-trips");
        check(!p.ops.empty() && p.ops[0].material == "concrete", "material round-trips");
    }

    // ---- 2. PROSE + ``` fences around the JSON. Models do this constantly. ----
    {
        AiPlan p; std::string err;
        const bool ok = parseAiPlan(
            "Sure! Here is the plan you asked for:\n```json\n"
            "{\"summary\":\"a pillar\",\"ops\":[{\"op\":\"add_brush\",\"pos\":[2,1,2],"
            "\"size\":[1,2,1]}]}\n```\nHope that helps!",
            p, err);
        check(ok, "JSON is extracted from prose + code fences");
        check(p.ops.size() == 1, "fenced plan yields its op");
    }

    // ---- 3. The hostile inputs. Each has a real path to wrecking a level. ----
    {
        AiPlan p; std::string err;
        check(!parseAiPlan("I can't do that.", p, err),
              "pure prose is refused");
        check(!parseAiPlan("{\"summary\":\"x\"}", p, err),
              "a plan with no ops[] is refused");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"add_brush\",\"pos\":[0,0,0],"
                           "\"size\":[0,2,2]}]}", p, err),
              "a ZERO-extent brush is refused (degenerate mesh)");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"add_brush\",\"pos\":[0,0,0],"
                           "\"size\":[-5,2,2]}]}", p, err),
              "a NEGATIVE-extent brush is refused");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"add_brush\",\"pos\":[0,0,0],"
                           "\"size\":[1000000,2,2]}]}", p, err),
              "a 1e6-metre brush is refused (swallows the viewport)");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"add_brush\",\"pos\":[9999,0,0],"
                           "\"size\":[2,2,2]}]}", p, err),
              "a pos outside the world box is refused");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"teleport_moon\",\"pos\":[0,0,0]}]}", p, err),
              "an unknown/hallucinated op is refused");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"set_material\",\"index\":-3,"
                           "\"material\":\"m\"}]}", p, err),
              "a negative index is refused");
        check(!parseAiPlan("{\"summary\":\"trunc\",\"ops\":[{\"op\":\"add_brush\"",
                           p, err),
              "a TRUNCATED generation is refused (unbalanced braces)");
        check(!parseAiPlan("{\"ops\":[{\"op\":\"add_brush\",\"pos\":[0,0,0],"
                           "\"size\":[2,2]}]}", p, err),
              "a 2-component size is refused (not silently zero-filled)");
    }

    // ---- 3b. REGRESSION: a THIN slab must be ACCEPTED. ----
    // The first cut of AiLimits set minSize = 0.25 m while aiSystemPrompt() was busy
    // telling the model to build "a 0.2 m-thick floor". Every wall and every floor the
    // model produced was silently rejected, and it looked like the AI was stupid — when
    // in fact it was obeying us and we were refusing it. The prompt and the validator
    // are ONE contract; this test is the thing that holds them together.
    {
        AiPlan p; std::string err;
        const bool ok = parseAiPlan(
            "{\"ops\":[{\"op\":\"add_brush\",\"pos\":[0,-0.1,0],\"size\":[8,0.2,6]}]}",
            p, err);
        check(ok, "a 0.2 m-thick FLOOR is accepted (the prompt asks for exactly this)");
        check(p.ops.size() == 1 && std::fabs(p.ops[0].size[1] - 0.2f) < 1e-4f,
              "...and its thickness survives validation unclamped");
    }

    // ---- 4. A RUNAWAY plan is refused WHOLESALE, never truncated. ----
    {
        std::string big = "{\"ops\":[";
        for (int i = 0; i < 200; ++i) {
            if (i) big += ",";
            big += "{\"op\":\"add_brush\",\"pos\":[0,0,0],\"size\":[1,1,1]}";
        }
        big += "]}";
        AiPlan p; std::string err;
        check(!parseAiPlan(big, p, err),
              "a 200-op plan is refused WHOLE (half a city is worse than no city)");
        check(p.ops.empty(), "no ops leak out of a refused runaway plan");
    }

    // ---- 5. A MIXED plan: good ops survive; bad ops are dropped and REPORTED. ----
    {
        AiPlan p; std::string err;
        const bool ok = parseAiPlan(
            "{\"ops\":["
            "{\"op\":\"add_brush\",\"pos\":[0,0,0],\"size\":[2,2,2]},"
            "{\"op\":\"add_brush\",\"pos\":[0,0,0],\"size\":[-5,2,2]},"
            "{\"op\":\"add_brush\",\"pos\":[4,0,0],\"size\":[2,2,2]}]}", p, err);
        check(ok, "a mixed plan still applies its valid ops");
        check(p.ops.size() == 2, "the invalid op is dropped");
        check(!err.empty(), "the rejection is REPORTED, never silent");
    }

    // ---- 6. THE TRANSACTION — the whole reason this feature is safe to ship. ----
    {
        LevelDoc doc;
        EditorState st(doc);
        AiPlan p; std::string err;
        parseAiPlan(
            "{\"summary\":\"a 4-wall room\",\"ops\":["
            "{\"op\":\"add_brush\",\"pos\":[0,-0.1,0],\"size\":[8,0.2,6]},"
            "{\"op\":\"add_brush\",\"pos\":[0,1.5,3],\"size\":[8,3,0.2]},"
            "{\"op\":\"add_brush\",\"pos\":[0,1.5,-3],\"size\":[8,3,0.2]},"
            "{\"op\":\"add_brush\",\"pos\":[4,1.5,0],\"size\":[0.2,3,6]},"
            "{\"op\":\"add_brush\",\"pos\":[-4,1.5,0],\"size\":[0.2,3,6]}]}", p, err);
        const int applied = applyAiPlan(st, p);
        check(applied == 5, "all 5 room brushes applied");
        check(doc.brushes.size() == 5, "the doc holds 5 brushes");
        check(doc.brushes.size() == 5 && std::fabs(doc.brushes[0].size[0] - 8.0f) < 1e-4f,
              "the AI's SIZE landed (not the default brush size)");

        const HistoryEffect e = st.undo();
        check(doc.brushes.empty(), "ONE undo removes the ENTIRE 5-brush room");
        check(e.op == HistoryEffect::Op::RespawnAll,
              "a grouped undo reports RespawnAll so the host rebuilds the blockout");
        check(!st.canUndo(), "the stack is empty after that single undo");

        const HistoryEffect r = st.redo();
        check(doc.brushes.size() == 5, "ONE redo restores the entire room");
        check(r.op == HistoryEffect::Op::RespawnAll, "grouped redo reports RespawnAll");
        check(doc.brushes.size() == 5 && std::fabs(doc.brushes[0].size[0] - 8.0f) < 1e-4f,
              "the redone room kept the AI's sizes");
    }

    // ---- 7. Grouping must NOT leak: hand edits stay independent undo steps. ----
    {
        LevelDoc doc;
        EditorState st(doc);
        const float a[3] = { 0, 0, 0 };
        const float b[3] = { 4, 0, 0 };
        st.addBrushCmd(0, a);
        st.addBrushCmd(0, b);
        check(doc.brushes.size() == 2, "two hand-placed brushes");
        st.undo();
        check(doc.brushes.size() == 1,
              "a hand edit is still ONE undo step (the AI group did not swallow it)");
        st.undo();
        check(doc.brushes.empty(), "the second undo removes the first hand brush");
    }

    // ---- 8. Deletes apply DESCENDING, so the model's indices stay valid. ----
    {
        LevelDoc doc;
        EditorState st(doc);
        const float o[3] = { 0, 0, 0 };
        for (int i = 0; i < 4; ++i) st.addBrushCmd(0, o);
        doc.brushes[0].name = "keep0";
        doc.brushes[3].name = "keep3";
        AiPlan p; std::string err;
        parseAiPlan("{\"ops\":[{\"op\":\"delete_brush\",\"index\":1},"
                    "{\"op\":\"delete_brush\",\"index\":2}]}", p, err);
        applyAiPlan(st, p);
        check(doc.brushes.size() == 2, "two brushes deleted");
        check(doc.brushes.size() == 2 &&
              doc.brushes[0].name == "keep0" && doc.brushes[1].name == "keep3",
              "descending delete removed the RIGHT brushes (indices did not shift under us)");
        st.undo();
        check(doc.brushes.size() == 4, "ONE undo restores BOTH deleted brushes");
    }

    // ---- 9. An all-rejected plan leaves NO phantom undo step. ----
    {
        LevelDoc doc;
        EditorState st(doc);
        const float o[3] = { 0, 0, 0 };
        st.addBrushCmd(0, o);
        AiPlan empty;                        // nothing survived validation
        const int applied = applyAiPlan(st, empty);
        check(applied == 0, "an empty plan applies nothing");
        st.undo();
        check(doc.brushes.empty(),
              "undo still removes the HAND brush (no phantom group step ate it)");
    }

    // ---- 10. The model is TOLD the indices it must cite. ----
    {
        LevelDoc doc;
        EditorState st(doc);
        const float o[3] = { 1, 2, 3 };
        st.addBrushCmd(0, o);
        doc.brushes[0].name = "pillar";
        const std::string d = describeLevel(doc, 0);
        check(d.find("[0]") != std::string::npos,
              "the level description carries brush INDICES");
        check(d.find("pillar") != std::string::npos, "...and names");
        check(d.find("SELECTED") != std::string::npos, "...and marks the selection");
        check(aiSystemPrompt().find("add_brush") != std::string::npos,
              "the system prompt documents the op schema");
    }

    x3::logInfo(std::string("[editor-ai] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::editor
