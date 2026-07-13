#pragma once
// ---------------------------------------------------------------------------
// THE AI ARCHITECT — natural-language authoring for the Level Architect.
//
//   "add a 8x4 room north of the selected brush, with a doorway back to it"
//        -> a validated AiPlan -> ONE undoable transaction on the LevelDoc.
//
// DESIGN LAWS (each one is here because the obvious shortcut is a trap):
//
//  1. THE MODEL NEVER TOUCHES THE DOC. It emits a PLAN (JSON) describing ops. We
//     parse it, VALIDATE it against a hard safety envelope, and only then apply it
//     through the editor's ordinary command API. An LLM that can call addBrush()
//     directly is an LLM that can corrupt your level; an LLM that emits a plan we
//     refuse to run is just an LLM that wasted a second.
//
//  2. VALIDATION IS NOT OPTIONAL. Models emit NaN, 1e30, negative sizes, 900
//     brushes, indices that don't exist, and prose wrapped around their JSON. Every
//     one of those is a real failure mode, not a hypothetical, and every one is
//     rejected here (see AiLimits + parseAiPlan). A plan is all-or-nothing at the
//     OP level: bad ops are dropped and reported, they never half-apply.
//
//  3. ONE SENTENCE = ONE CTRL+Z. The whole plan applies inside EditorState's undo
//     TRANSACTION (beginGroup/endGroup). A 12-brush room the AI built must vanish on
//     ONE undo. Twelve undos to take back one instruction is not undo, it's penance.
//
//  4. IT MUST WORK WITH NO MODEL. The editor is not allowed to become unusable
//     because a .gguf is missing. parse/validate/apply are pure logic with zero LLM
//     dependency, and the self-test drives them through the MOCK backend — so
//     --test-editor-ai is deterministic and needs no model file, no GPU, no network.
//
// Pure logic — no ImGui, no Vulkan, no llama. The host (editor_host.cpp) owns the
// panel and the async submit/poll; this header owns the CONTRACT.
// ---------------------------------------------------------------------------
#include "editor.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::editor {

// The ops the model is allowed to ask for. Deliberately SMALL: every op here maps
// onto an existing, already-tested EditorState command. Growing this enum is how the
// AI gains powers — and each new power gets a validator and a self-test case.
enum class AiOpKind : uint8_t {
    None = 0,
    AddBrush,        // create a blockout solid (Box/Ramp) at pos with size/yaw/material
    MoveBrush,       // move an existing brush by index
    SetMaterial,     // re-skin an existing brush by index
    DeleteBrush,     // remove an existing brush by index
    SetPlayerStart,  // move the player spawn
};

struct AiOp {
    AiOpKind    kind      = AiOpKind::None;
    uint32_t    brushType = 0;                    // 0 = Box, 1 = Ramp (prims::BrushType)
    int         index     = -1;                   // target brush (Move/SetMaterial/Delete)
    float       pos[3]    = { 0, 0, 0 };
    float       size[3]   = { 2, 2, 2 };          // full extents (m)
    float       yaw       = 0.0f;                 // radians about +Y
    std::string name;
    std::string material;
};

struct AiPlan {
    std::string      summary;                     // the model's one-line description
    std::vector<AiOp> ops;
};

// The SAFETY ENVELOPE. These are not style preferences — each bound corresponds to a
// way a model has a plausible path to wrecking a level or hanging the editor.
struct AiLimits {
    size_t maxOps      = 64;        // "build me a city" must not emit 10,000 brushes
    float  maxAbsCoord = 250.0f;    // keep geometry inside a sane world box
    // 0.05 m, NOT 0.25: a wall/floor slab is legitimately THIN (a 0.2 m floor is the
    // most ordinary thing an architect can ask for), and the first version of this
    // limit was 0.25 — which silently rejected every wall and floor the model built
    // while the prompt was busy TELLING it to make them 0.2 m thick. The schema and
    // the prompt must agree or the model looks stupid for obeying us. Keep this in
    // lockstep with the range quoted in aiSystemPrompt().
    float  minSize     = 0.05f;     // a zero/negative-extent brush is a degenerate mesh
    float  maxSize     = 80.0f;     // and a 1e6-metre brush is a swallowed viewport
};

// The system prompt handed to the model: the op schema + the rules. Kept next to the
// parser ON PURPOSE — if the schema and the prompt drift apart, the model emits
// plans we reject, and the failure looks like "the AI is dumb" instead of "we lied
// to it". One source of truth.
std::string aiSystemPrompt();

// A COMPACT description of the level for the model's context (brush list w/ indices,
// sizes, the selection, the player start). Indices here are the same indices the
// model must use in MoveBrush/SetMaterial/DeleteBrush — that is the whole contract.
std::string describeLevel(const LevelDoc& doc, int selectedBrush);

// Parse a model reply into a plan.
//   * Tolerant of PROSE and ``` fences around the JSON (models do this constantly) —
//     we extract the outermost {...} span rather than demanding a clean document.
//   * Every op is validated against `lim`. Invalid ops are DROPPED, never clamped
//     silently into something the user didn't ask for, and each rejection is appended
//     to `err` so the panel can show exactly what the model got wrong.
//   * Returns false only when NOTHING usable survived (no JSON, or zero valid ops).
bool parseAiPlan(const std::string& modelText, AiPlan& out, std::string& err,
                 const AiLimits& lim = AiLimits{});

// Apply a plan as ONE undo transaction. Returns the number of ops applied.
// Deletes are applied LAST and in DESCENDING index order, so that the indices the
// model was given (which refer to the doc as it was described to it) stay valid for
// the whole batch — an add shifts nothing, and a descending delete shifts nothing
// that a later delete still needs.
int applyAiPlan(EditorState& st, const AiPlan& plan);

// --test-editor-ai. Deterministic; needs no model, no GPU, no network.
bool runEditorAiSelfTest();

} // namespace x3::editor
