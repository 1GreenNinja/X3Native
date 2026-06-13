#pragma once
// MISSION RUNNER — the x3.mission/1 data format live in-engine.
// Spec: docs/design/MISSION_FORMAT.md.
//
// THE GTA MISSION LOOP AS DATA: a mission ships as a JSON doc (missions/
// *.mission.json today; the pak later) — an ordered/branching list of STAGES.
// Each stage carries the HUD objective text, fx to fire on entry/completion,
// and an `advance_when` condition list evaluated against the REAL game systems.
// Conditions and effects are the SAME ops vocabulary as x3.chattree/1
// (story_ops.h: ChatCond/ChatFx + evalChatConds/applyChatFx) — there is ONE
// condition system; missions and dialog share it. The 100-level arc becomes
// 100 docs authorable in parallel.
//
// HOW STAGES ADVANCE — the flag bridge: gameplay events (kills, trigger-zone
// entries, terminal codes, rescues, pickups, door/boss beats) are bridged into
// StoryFlags by small adapters (MissionEventBridge + the Level-1 poll adapter
// below); `advance_when` is then ordinary condition evaluation over flags/
// items/axes/Lua. Nothing here duplicates a sink: effects route through
// applyChatFx (TimelineState::adjust*, StoryFlags, IScriptSystem::fire).
//
// PERSISTENCE: the runner records its position IN StoryFlags
// ("mission.<id>.at.<stage>" / "mission.<id>.done" / "mission.<id>.failed"),
// so mission progress rides the existing flags save lane (alongside the binary
// checkpoint) and resume() lands back on the same stage after a load.
//
// Game/slice code only — engine/ stays pure. Headless-testable like the
// chat-tree runner: --test-mission drives docs + the flag bridge + the REAL
// Level1Game with no window/Vulkan.

#include "story_ops.h"                      // ChatCond/ChatFx/ChatContext/StoryFlags + eval/apply

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace x3::game {

class Level1Game;   // level1_game.h — the Level-1 poll adapter reads it

// ===========================================================================
// The parsed x3.mission/1 document model.
// ===========================================================================

// A branch taken INSTEAD of `next` when present: if `if` passes -> `then`,
// otherwise -> `else` (both stage ids or "end").
struct MissionBranch {
    std::vector<ChatCond> conds;          // `if` (AND; empty passes -> then)
    std::string thenTo;                   // stage id or "end"
    std::string elseTo;                   // stage id or "end"
};

struct MissionStage {
    std::string id;
    std::string objectiveText;            // HUD objective line ("" = keep previous)
    std::vector<ChatFx>   onEnter;        // applied when the stage is entered
    std::vector<ChatCond> advanceWhen;    // AND; empty = advance immediately
    std::vector<ChatCond> failWhen;       // optional; empty = never fails
    std::vector<ChatFx>   onComplete;     // applied when advanceWhen passes
    std::vector<ChatFx>   onFail;         // applied when failWhen passes
    std::string next;                     // stage id or "end" ("" + no branch => "end")
    bool          hasBranch = false;
    MissionBranch branch;                 // used instead of `next` when present
    std::string failTo;                   // stage to jump to on fail ("" => mission FAILED)
};

struct MissionDoc {
    std::string id;                       // stable id ("level1") — flag namespace
    std::string title;                    // display title (ASCII-folded at load)
    std::string start;                    // entry stage id (default: first stage)
    std::vector<MissionStage> stages;

    const MissionStage* stageById(std::string_view id) const;
};

// ===========================================================================
// Loader + validation.
// ===========================================================================

// Parse one x3.mission/1 JSON text into `out`. On parse/shape errors appends
// messages to `errors` and returns false. Unrecognized condition/effect KINDS
// are loader errors (the runtime contract is closed — shared with chattree).
bool loadMissionFromJson(std::string_view jsonText, std::string_view srcName,
                         MissionDoc& out, std::vector<std::string>& errors);

// Read + parse one file. Same contract as loadMissionFromJson.
bool loadMissionFile(const std::string& path, MissionDoc& out,
                     std::vector<std::string>& errors);

// Resolve missions/<name> from the usual candidates (repo root relative /
// exe-relative — mirrors ChatTreeSystem::loadDefault). "" when absent.
std::string findMissionFile(const std::string& name);

// Validate a parsed doc: stage ids unique + non-empty, start resolves, every
// next/branch/failTo target is a real stage or "end", and every stage is
// reachable from start. Appends problems to `errors`; returns true if sound.
bool validateMission(const MissionDoc& doc, std::vector<std::string>& errors);

// ===========================================================================
// MissionEventBridge — gameplay events -> StoryFlags (the condition substrate).
// ===========================================================================
// Small adapters setting the flags `advance_when` reads. The host forwards the
// SAME x3.fire stream it already feeds Lua (trigger_enter / terminal_code /
// dialog_end / ...), plus kill/rescue notifications. Flag scheme (documented in
// MISSION_FORMAT.md §4):
//   * any fired event:        "ev.<name>"  (latched: the event happened once)
//   * trigger_enter {zone}:   "trigger.<zone>"
//   * terminal_code {code}:   "code.<code>.entered"
//   * kills (onKill(type)):   "kill.<type>.<n>" for n = 1..count (monotonic
//                             milestones, so {"flag":"kill.guard.3"} = 3+ kills)
//   * rescues (onRescue(who)): "<who>.rescued" (the chattree glossary key)
class MissionEventBridge {
public:
    void bind(StoryFlags* flags) { m_flags = flags; }

    // Forward one fired script event (the host's scripts->fire mirror).
    void onEvent(std::string_view name, const x3::script::EventArgs& args);

    // A monster of `type` was killed: bump the counter + set milestone flags.
    void onKill(std::string_view type);
    // Set the kill counter ABSOLUTELY (poll-style adapters report totals; only
    // raises — milestones are emitted for every newly reached n).
    void setKills(std::string_view type, int total);
    int  kills(std::string_view type) const;

    // A captive was rescued.
    void onRescue(std::string_view who);

private:
    StoryFlags* m_flags = nullptr;
    std::unordered_map<std::string, int> m_kills;
};

// Level-1 poll adapter: derive the Level-1 mission flags from the LIVE
// Level1Game each tick (edge-latched — flags only ever get set). Sets:
//   "l1.doorA.open"        Door A reached Open       (beat 3 edge)
//   "l1.armed"             the player armed           (beat 6/7)
//   "l1.checkpoint.clear"  armed + checkpoint enemies all dead (beat 8)
//   "l1.martinez.dead"     Martinez spawned + dead    (beat 10)
//   "l1.complete"          the WIN latch              (beat 11)
//   "trigger.<id>"         every trigger fired this tick (via `bridge`)
//   kill counters          "kill.guard.<n>" over corridor+checkpoint dead,
//                          "kill.martinez.1" (via `bridge`)
// Pure reads of Level1Game public queries; no game state is touched.
void pollLevel1MissionFlags(const Level1Game& game, MissionEventBridge& bridge,
                            StoryFlags& flags);

// ===========================================================================
// MissionRunner — the live runner (one active mission).
// ===========================================================================
class MissionRunner {
public:
    // The evaluation context (same systems as the chat-tree runner; the host
    // points flags/timeline/scripts at the REAL ones).
    ChatContext&       ctx()       { return m_ctx; }
    const ChatContext& ctx() const { return m_ctx; }

    // The objective sink: called with the stage's objective text on every stage
    // entry (and "" when the mission ends). The host wires this to the REAL
    // ObjectiveSystem free-text lane (objectives().setText) so the doc drives
    // the HUD objective line.
    void setObjectiveSink(std::function<void(const std::string&)> sink) {
        m_objectiveSink = std::move(sink);
    }

    // Start `doc` from its start stage (fires onEnter fx; cascades through
    // already-satisfied stages). The doc must outlive the runner's run.
    // Returns false if the doc has no resolvable start stage.
    bool start(const MissionDoc& doc);

    // Resume `doc` from the stage recorded in ctx().flags
    // ("mission.<id>.at.<stage>"): re-enters that stage WITHOUT re-firing its
    // onEnter fx (effects are not idempotent; the save already carries their
    // results), re-emits the objective text, then cascades. "mission.<id>.done"
    // resumes straight to Complete. No marker => plain start().
    bool resume(const MissionDoc& doc);

    // Evaluate the current stage's failWhen then advanceWhen against ctx();
    // advance/cascade as far as conditions allow (bounded). Cheap: a handful of
    // flag lookups when nothing changed. Call once per frame (or after any
    // event/flag write in headless tests).
    void tick();

    // ---- State ----
    enum class State : uint32_t { Idle, Running, Complete, Failed };
    State state() const { return m_state; }
    bool  active() const { return m_state == State::Running; }
    const MissionDoc* doc() const { return m_doc; }
    const std::string& currentStageId() const { return m_stageId; }
    // The current stage's objective text ("" when not running).
    const std::string& currentObjective() const;

private:
    void emitObjective(const std::string& text);
    // Enter stage `id` ("end" => Complete). fireFx=false on resume.
    bool enterStage(const std::string& id, bool fireFx);
    void completeMission();
    void failMission();
    void setAtFlag(const std::string& stageId);   // move the persistence marker
    std::string condKey(const std::string& stageId) const;  // chance-hash key

    const MissionDoc* m_doc = nullptr;
    ChatContext       m_ctx;
    State             m_state = State::Idle;
    std::string       m_stageId;
    const MissionStage* m_stage = nullptr;   // borrowed view into m_doc
    std::function<void(const std::string&)> m_objectiveSink;
};

// ===========================================================================
// Headless self-test (--test-mission). No window / Vulkan. Asserts:
//   * doc parse/validate: a tiny inline doc parses; bad refs / unknown ops /
//     duplicate ids / unreachable stages are loader/validator errors;
//   * missions/level1.mission.json parses + validates from disk;
//   * stage advance on flag set / trigger_enter / terminal_code / the
//     kill-counter bridge (milestone flags);
//   * onEnter/onComplete fx fire through the real sinks (flags set + x3.fire
//     observed by a real loaded Lua script);
//   * branch (if/then/else) + failWhen -> onFail + failTo retry + Failed;
//   * save mid-mission -> flags round-trip -> resume() lands the same stage
//     (onEnter NOT re-fired) and the mission still completes;
//   * THE LEVEL-1 EQUIVALENCE WALK: build the REAL Level1Game headless, drive
//     the actual milestones (Door A button, sidearm pickup, corridor/checkpoint
//     fights, arena trigger, Martinez, elevator) with the doc-driven runner
//     polling the flag bridge, and assert the runner's objective sequence is
//     EXACTLY the hardcoded ObjectiveSystem beat list the game walks today.
// Prints "mission: X/Y passed"; returns true iff all pass.
// ===========================================================================
bool runMissionSelfTest();

} // namespace x3::game
