#pragma once
// CHAT-TREE DIALOG RUNNER — the x3.chattree/1 data format live in-engine.
// Spec: docs/design/narrative/NPC_CHAT_TREE_FORMAT.md (+ IMPLEMENTATION_NOTES.md).
//
// NPC conversations as DATA: trees ship as JSON (docs/design/narrative/chat_trees/
// *.json today; the pak later), the runner here evaluates `if` conditions against
// the REAL game systems (TimelineState axes/timeline/fates, StoryFlags, items,
// per-NPC rel ints, Lua escape hatch) and applies `fx` effects through the SAME
// sinks the rest of the game uses (TimelineState::adjust*, IScriptSystem::fire,
// the host's RescueSystem follow callback). Dialog stays data; story side-effects
// route through the D14 Lua layer (x3.fire) — nothing here duplicates a sink.
//
// Game/slice code only — engine/ stays pure. Headless-testable like NpcDialog:
// the runner owns conversation state and is driven purely by data; the host feeds
// it E/choice edges and reads it back to draw (--test-chattree, no window/Vulkan).
//
// WHAT LIVES HERE
//   * StoryFlags          — persistent string-keyed flags + per-NPC rel ints +
//                           string-id inventory + seen-set (once-banter), with a
//                           tiny text serialize/save-file lane (alongside the
//                           binary checkpoint; see saveFile/loadFile).
//   * ChatCond / ChatFx   — the parsed condition/effect vocabulary (spec §3).
//   * ChatNode/Tree/Npc   — the parsed x3.chattree/1 document model.
//   * loadChatTree*       — JSON -> model (handles the single-NPC `trees` shape
//                           AND the multi-NPC `npcs` shape club1127_patrons uses).
//   * validateChatNpc     — refs resolve, kinds recognized, ids unique; full mode
//                           also demands reachability (start ∪ host-`_trigger`
//                           entry nodes must cover every node).
//   * ChatTreeSystem      — the live runner: one active conversation, condition-
//                           filtered choices, fx on delivery, banter-pool picks
//                           with weight/rotation/once.
//   * drawChatTreeUi      — the HUD presentation (GTA-subtitle styling, the
//                           npc_dialog box grown choice rows).

#include "timeline.h"                       // TimelineState (axes / timeline / fates)
#include "engine/rhi/IRenderDevice.h"       // HUD draw (same dependency as objective.h)
#include "engine/script/IScriptSystem.h"    // fire effects + EventArgs

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x3::game {

// ===========================================================================
// StoryFlags — the persistent narrative key/value lane (spec §3 "StoryFlags").
// ===========================================================================
// String-keyed story flags ("lena.met", "code.1278.known"), per-NPC relationship
// stages (rel 0..4, only ever raised by fx per spec §5), a string-id inventory
// ("lena_tunnel_map"), and a seen-set for once-banter. Serializes to a tiny
// line-based text blob saved ALONGSIDE the binary checkpoint (the checkpoint
// format stays untouched / version-stable; the flags file is additive).
class StoryFlags {
public:
    // ---- flags ----
    void set(std::string_view flag)        { m_flags.insert(std::string(flag)); }
    void clear(std::string_view flag)      { m_flags.erase(std::string(flag)); }
    bool has(std::string_view flag) const  { return m_flags.count(std::string(flag)) != 0; }

    // ---- per-NPC relationship stage (0 Stranger .. 4 Romance; only raises) ----
    int  rel(std::string_view npc) const;
    void raiseRel(std::string_view npc, int stage);   // no-op if stage <= current

    // ---- string-id inventory ----
    void give(std::string_view item)       { m_items.insert(std::string(item)); }
    void take(std::string_view item)       { m_items.erase(std::string(item)); }
    bool hasItem(std::string_view item) const { return m_items.count(std::string(item)) != 0; }

    // ---- once-banter / seen keys ----
    void markSeen(std::string_view key)    { m_seen.insert(std::string(key)); }
    bool seen(std::string_view key) const  { return m_seen.count(std::string(key)) != 0; }

    void clearAll() { m_flags.clear(); m_rel.clear(); m_items.clear(); m_seen.clear(); }

    // ---- persistence (text blob; line-based, order-independent) ----
    std::string serialize() const;
    bool deserialize(std::string_view text);          // replaces current state
    bool saveFile(const std::string& path) const;
    bool loadFile(const std::string& path);           // false + untouched on absence

private:
    std::unordered_set<std::string>      m_flags;
    std::unordered_map<std::string, int> m_rel;
    std::unordered_set<std::string>      m_items;
    std::unordered_set<std::string>      m_seen;
};

// ===========================================================================
// The parsed condition / effect vocabulary (spec §3).
// ===========================================================================

enum class ChatCondKind : uint32_t {
    Flag, KarmaGte, KarmaLte, HumanityGte, TrustGte, MercyGte, LoveGte,
    RedemptionGte, Timeline, GirlSaved, GirlLost, Item, RelGte, Chance, Lua,
    Any, Not
};

struct ChatCond {
    ChatCondKind kind = ChatCondKind::Flag;
    std::string  s;                       // flag / item / girl / lua-fn / rel-npc
    int          n = 0;                   // gte/lte threshold, rel stage
    float        f = 0.0f;                // chance probability
    std::vector<std::string> names;       // timeline set
    std::vector<ChatCond>    sub;         // any (OR) / not (1 entry)
};

enum class ChatFxKind : uint32_t {
    Axis,        // s = karma|humanity|trust|mercy|love|redemption, n = delta
    SetFlag, ClearFlag,
    Fire,        // s = event, args = key/value pairs
    Give, Take,  // s = item
    Follow,      // she becomes a Companion (host callback == NpcDialog onComplete)
    Rel,         // s = npc, n = stage (only raises)
    Ally,        // TimelineState::onAllyJoined
    End          // s = host verb ("fight"/"flee"/...) -> x3.fire("dialog_end")
};

struct ChatFx {
    ChatFxKind  kind = ChatFxKind::SetFlag;
    std::string s;
    int         n = 0;
    x3::script::EventArgs args;           // fire args (stringified)
};

// ===========================================================================
// The parsed x3.chattree/1 document model.
// ===========================================================================

struct ChatChoice {
    std::string text;
    std::string next;                     // node id or "end"
    std::vector<ChatCond> conds;          // hide unless ALL pass
    std::vector<ChatFx>   fx;             // applied when picked
};

struct ChatNode {
    std::string id;
    std::string speaker;                  // "" => npc display; "YOU" = Jake
    std::string line;
    std::string next;                     // auto-advance target ("" => terminal)
    std::string elseNode;                 // redirect when `if` fails
    std::vector<ChatCond>   conds;        // node-level `if`
    std::vector<ChatFx>     fx;           // applied when the line is delivered
    std::vector<ChatChoice> choices;
    bool hostTriggered = false;           // authored `_trigger` (host-fired entry)
};

struct ChatBanter {
    std::string line;
    std::vector<ChatCond> conds;
    std::vector<ChatFx>   fx;             // `fx_on_play` (lena.json extension)
    int  weight = 1;
    bool once   = false;
};

struct ChatTree {
    std::string name;                     // "first_meeting" / "banter" / ...
    std::string start;                    // entry node id ("" for pool trees)
    std::vector<ChatNode>   nodes;
    std::vector<ChatBanter> pool;         // banter trees

    const ChatNode* nodeById(std::string_view id) const;
};

struct ChatNpc {
    std::string id;                       // stable id ("lena") — flag namespace
    std::string display;                  // HUD speaker label ("LENA")
    std::string voice;                    // TTS voice name (unused this slice)
    std::vector<ChatTree> trees;

    const ChatTree* tree(std::string_view name) const;
};

// ===========================================================================
// Loader + validation.
// ===========================================================================

// Parse one x3.chattree/1 JSON text into >=1 ChatNpc (the multi-NPC `npcs` shape
// yields one per entry). Appends to `out`. On parse/shape errors appends messages
// to `errors` and returns false. Unrecognized condition/effect KINDS are loader
// errors (the runtime contract is closed — spec §3).
bool loadChatTreesFromJson(std::string_view jsonText, std::string_view srcName,
                           std::vector<ChatNpc>& out,
                           std::vector<std::string>& errors);

// Read + parse one file. Same contract as loadChatTreesFromJson.
bool loadChatTreeFile(const std::string& path, std::vector<ChatNpc>& out,
                      std::vector<std::string>& errors);

// Validate a parsed NPC: per tree — start resolves, node ids unique, every
// next/else/choice target is a real node or "end". With `fullReachability` every
// node must be reachable from {start} ∪ {host-`_trigger` nodes} (lena/aria gate).
// Appends human-readable problems to `errors`; returns true if sound.
bool validateChatNpc(const ChatNpc& npc, bool fullReachability,
                     std::vector<std::string>& errors);

// ===========================================================================
// The evaluation context — the REAL systems conditions read / effects write.
// ===========================================================================
struct ChatContext {
    TimelineState*  timeline = nullptr;   // karma/axes/timeline/fates (REQUIRED)
    StoryFlags*     flags    = nullptr;   // flags/rel/items/seen      (REQUIRED)
    x3::script::IScriptSystem* scripts = nullptr;   // fire effects (optional)
    // {"lua": "fn"} escape hatch: host evaluates `fn` via the script system and
    // returns its truthiness. Unset => the condition fails (and logs once).
    std::function<bool(const std::string& fn)> luaCond;
    // {"follow": true}: the host's rescue action (RescueSystem::tryRescue — the
    // exact NpcDialog::onComplete sink). Unset => effect ignored (logged).
    std::function<bool()> follow;
    uint32_t chanceSeed = 0;              // per-save seed for deterministic {"chance"}
};

// Evaluate one condition list (AND semantics) against the context. `nodeKey`
// feeds the deterministic chance hash (per-(save,node) — spec §3).
bool evalChatConds(const std::vector<ChatCond>& conds, const ChatContext& ctx,
                   std::string_view nodeKey);

// Apply one effect list in order. `npcId` namespaces rel/follow/dialog_end.
void applyChatFx(const std::vector<ChatFx>& fx, const ChatContext& ctx,
                 std::string_view npcId);

// ===========================================================================
// ChatTreeSystem — the live runner (one active conversation, like NpcDialog).
// ===========================================================================
class ChatTreeSystem {
public:
    // Load every *.json under `dir`. Returns NPCs loaded (multi-NPC files count
    // each). Parse/validate problems are logged; a bad file is skipped whole.
    int loadDir(const std::string& dir);
    // Resolve the chat_trees dir from the usual candidates (repo root relative /
    // exe-relative — mirrors GirlsDialog::defaultPath) and loadDir it.
    int loadDefault();

    uint32_t npcCount() const { return (uint32_t)m_npcs.size(); }
    const ChatNpc* npc(std::string_view id) const;     // nullptr if absent
    bool hasNpc(std::string_view id) const { return npc(id) != nullptr; }

    ChatContext&       ctx()       { return m_ctx; }
    const ChatContext& ctx() const { return m_ctx; }
    StoryFlags&        flags()     { return m_flags; }  // default ctx.flags target

    // ---- Conversation ----
    // Start `treeName` on `npcId` from its start node (node-level if/else entry
    // redirects honored; the entry node's fx fire on delivery). Returns false if
    // the npc/tree/start is missing or the entry condition chain dead-ends.
    bool start(std::string_view npcId, std::string_view treeName);
    // Host-fired entry (Lua x3.fire("dialog_open") / quest beats): start at an
    // explicit node (e.g. lena sidequest "sq_done").
    bool startAt(std::string_view npcId, std::string_view treeName,
                 std::string_view nodeId);

    bool active() const { return m_active; }
    const std::string& activeNpc()  const { return m_npcId; }
    const std::string& activeTree() const { return m_treeName; }
    const std::string& currentNodeId() const { return m_nodeId; }
    // The current line + speaker label (speaker resolves "" -> npc display).
    const std::string& currentLine() const;
    const std::string& currentSpeaker() const;
    // Condition-FILTERED choices at the current node (hide, never grey — spec §2).
    const std::vector<ChatChoice>& choices() const { return m_choices; }

    // E on a no-choice node: deliver `next` (or end when terminal/"end").
    // Returns true if the conversation is still active after the advance.
    bool advance();
    // Pick filtered choice `index`: apply its fx, jump to its target. Returns
    // true if the conversation is still active after the move.
    bool choose(uint32_t index);
    // Cancel without effects (player walked away — matches NpcDialog::cancel()).
    void cancel() { m_active = false; m_choices.clear(); }

    // Did the conversation that just ran/ended execute a {"follow": true}? (the
    // host plays the companion bark + SFX on this — NpcDialog::interact parity).
    bool followFired() const { return m_followFired; }

    // ---- Banter pool (companion re-talk) ----
    // Weighted pick among `if`-passing pool entries of `npcId`'s "banter" tree.
    // Applies the entry's fx_on_play, rotates (no immediate repeat while >1
    // eligible), and honors `once` via the seen-set. Empty string when nothing
    // is eligible. `roll01` is a [0,1) sample (caller supplies — deterministic
    // in tests, rng in the host).
    std::string pickBanter(std::string_view npcId, float roll01);

private:
    bool deliver(const ChatNpc& npc, const ChatTree& tree, std::string_view nodeId);
    void refreshChoices();

    std::vector<ChatNpc> m_npcs;
    StoryFlags           m_flags;
    ChatContext          m_ctx;

    bool        m_active = false;
    bool        m_followFired = false;
    std::string m_npcId, m_treeName, m_nodeId;
    const ChatNpc*  m_curNpc  = nullptr;   // borrowed views into m_npcs (stable:
    const ChatTree* m_curTree = nullptr;   // m_npcs never mutates mid-conversation)
    const ChatNode* m_curNode = nullptr;
    std::vector<ChatChoice> m_choices;     // filtered copies for the HUD
    std::unordered_map<std::string, size_t> m_lastBanter;   // npc -> last pool idx
};

// ===========================================================================
// HUD presentation — the npc_dialog box grown player-choice rows.
// ===========================================================================
// GTA-subtitle styling consistent with the existing fonts/roles: the NPC line in
// a translucent bottom panel (Menu font role, warm speaker label / white body —
// the exact npcDialog box look), then up to 4 numbered player choices below it,
// "[1-4] Choose" / "[E] Continue" hint right-aligned. Pure draw — reads the
// system, renders via the HUD text/quad API, owns no state.
void drawChatTreeUi(x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame,
                    const ChatTreeSystem& sys);

// ===========================================================================
// Headless self-test (--test-chattree). No window / Vulkan. Asserts:
//   * ALL 8 chat_trees/*.json parse + validate (refs/kinds/ids), lena + aria
//     with FULL reachability;
//   * the lena walk: entry if/else redirect (lena.interrupted), a condition-
//     gated choice hidden then shown after a flag/axis change, fx on delivery
//     (karma delta + flag set + x3.fire observed by a real loaded Lua script),
//     the spine landing on follow (host callback fired, rel raised to 1);
//   * the trust path: rel 2 + mercy >= 55 reaches t0 (the 1278 teach beat) and
//     fires dialog_hint{code=1278} + sets code.1278.known;
//   * banter-pool rotation/weights/once + rel gating;
//   * StoryFlags + rel save/load round-trip (file lane);
//   * deterministic {"chance"} + the {"lua"} escape hatch via the script system.
// Prints "chattree: X/Y passed"; returns true iff all pass.
// ===========================================================================
bool runChatTreeSelfTest();

} // namespace x3::game
