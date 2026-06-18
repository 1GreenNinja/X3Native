#pragma once
// STORY OPS — the ONE shared condition/effect vocabulary for narrative data.
//
// Factored out of chat_tree.{h,cpp} so the x3.chattree/1 dialog runner AND the
// x3.mission/1 mission runner (mission.h) speak the SAME `if`/`fx` ops over the
// SAME sinks. There is deliberately no second condition system: a mission stage's
// advance_when and a chat node's `if` are the identical ChatCond list, evaluated
// by the identical evalChatConds(); a stage's on_enter/on_complete and a chat
// choice's `fx` are the identical ChatFx list, applied by the identical
// applyChatFx() (TimelineState::adjust*, StoryFlags, IScriptSystem::fire, the
// host follow callback).
//
// WHAT LIVES HERE (moved verbatim from chat_tree.{h,cpp}):
//   * StoryFlags            — persistent string-keyed flags + per-NPC rel ints +
//                             string-id inventory + seen-set, with the text
//                             serialize/save-file lane.
//   * ChatCond / ChatFx     — the parsed condition/effect vocabulary (chat-tree
//                             spec §3; closed contract).
//   * ChatContext           — the REAL systems conditions read / effects write.
//   * evalChatConds / applyChatFx — the shared evaluator + effect router.
//   * JValue / JParser      — the minimal shared JSON reader (the same lean
//                             shape canon_play.cpp / level_loader.cpp carry).
//   * parseStoryCond*/Fx*   — JSON -> ChatCond/ChatFx (shared by both loaders).
//   * asciiFold / lowerAscii — display-text + id helpers.
//
// Game/slice code only — engine/ stays pure.

#include "engine/script/IScriptSystem.h"    // fire effects + EventArgs

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace x3::game {

class TimelineState;   // timeline.h (axes / timeline / fates) — impl-only dep

// ===========================================================================
// StoryFlags — the persistent narrative key/value lane (chat-tree spec §3).
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
// The parsed condition / effect vocabulary (chat-tree spec §3).
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

// Deterministic per-(save,node) hash -> [0,1) for {"chance"} (chat-tree spec §3).
// FNV-1a over (seed, nodeKey). EXPOSED so other deterministic branch points (e.g.
// the interactive-intro outcome roll, app/intro_orchestrator.cpp) reuse the SAME
// save-seeded roll the dialog/mission {chance} op uses — no second RNG.
float chanceRoll(uint32_t seed, std::string_view nodeKey);

// Evaluate one condition list (AND semantics) against the context. `nodeKey`
// feeds the deterministic chance hash (per-(save,node) — spec §3).
bool evalChatConds(const std::vector<ChatCond>& conds, const ChatContext& ctx,
                   std::string_view nodeKey);

// Apply one effect list in order. `npcId` namespaces rel/follow/dialog_end (the
// mission runner passes its mission id here so fire args carry provenance).
void applyChatFx(const std::vector<ChatFx>& fx, const ChatContext& ctx,
                 std::string_view npcId);

// ===========================================================================
// Minimal shared JSON reader — the same lean shape canon_play.cpp and
// level_loader.cpp carry (full value set; UTF-8 bytes pass through).
// ===========================================================================
struct JValue;
using JObject = std::vector<std::pair<std::string, JValue>>;
using JArray  = std::vector<JValue>;

struct JValue {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::shared_ptr<JArray>  arr;
    std::shared_ptr<JObject> obj;
    bool isObj() const { return t == T::Obj && obj; }
    bool isArr() const { return t == T::Arr && arr; }
    bool isStr() const { return t == T::Str; }
    bool isNum() const { return t == T::Num; }
    const JValue* find(const std::string& key) const {
        if (!isObj()) return nullptr;
        for (const auto& kv : *obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string asStr(const char* d = "") const { return t == T::Str ? str : std::string(d); }
    // Stringify any scalar (fire-args values arrive as strings OR numbers/bools).
    std::string scalarStr() const;
};

struct JParser {
    const char* p; const char* end; bool ok = true;
    explicit JParser(std::string_view s) : p(s.data()), end(s.data() + s.size()) {}
    void skipWs() { while (p < end) { char c = *p; if (c==' '||c=='\t'||c=='\n'||c=='\r') { ++p; continue; } break; } }
    JValue parseValue();
    std::string parseString();
    JValue parseNumber();
    JValue parseBool();
    JValue parseArray();
    JValue parseObject();
};

// ===========================================================================
// Shared JSON -> cond/fx parsing (the closed spec-§3 vocabulary). Unrecognized
// kinds are loader errors in BOTH formats (the runtime contract is closed).
// ===========================================================================
bool parseStoryCond(const JValue& jv, ChatCond& out, std::vector<std::string>& errors,
                    const std::string& where);
// `jv` may be null (no `if` key) — passes with an empty list.
bool parseStoryCondList(const JValue* jv, std::vector<ChatCond>& out,
                        std::vector<std::string>& errors, const std::string& where);
bool parseStoryFx(const JValue& jv, ChatFx& out, std::vector<std::string>& errors,
                  const std::string& where);
bool parseStoryFxList(const JValue* jv, std::vector<ChatFx>& out,
                      std::vector<std::string>& errors, const std::string& where);

// ===========================================================================
// Text helpers.
// ===========================================================================
// ASCII-fold display text: the HUD font atlas is ASCII-only, but authored docs
// carry literal UTF-8 (em-dashes, curly quotes, ellipses). Fold the common
// punctuation to ASCII equivalents and drop anything else multi-byte. Applied
// at LOAD time to display strings only (ids/flags are ASCII by construction).
std::string asciiFold(const std::string& in);

// Lowercase ASCII helper (ids/names are ASCII by construction).
std::string lowerAscii(std::string_view s);

} // namespace x3::game
