// CHAT-TREE DIALOG RUNNER — implementation. See chat_tree.h.
#include "chat_tree.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// JSON parsing + the cond/fx vocabulary now come from story_ops.{h,cpp} (the
// shared x3 narrative ops — the mission runner reuses the exact same set).
// ---------------------------------------------------------------------------


// Cond/fx JSON parsing is SHARED (story_ops.h): parseStoryCondList / parseStoryFxList.

bool parseNode(const JValue& jv, ChatNode& out, std::vector<std::string>& errors,
               const std::string& where) {
    if (!jv.isObj()) { errors.push_back(where + ": node is not an object"); return false; }
    bool ok = true;
    out.id = jv.find("id") ? jv.find("id")->asStr() : "";
    if (out.id.empty()) { errors.push_back(where + ": node missing `id`"); ok = false; }
    const std::string nw = where + "." + out.id;
    out.line     = jv.find("line")    ? asciiFold(jv.find("line")->asStr()) : "";
    out.speaker  = jv.find("speaker") ? jv.find("speaker")->asStr() : "";
    out.next     = jv.find("next")    ? jv.find("next")->asStr()    : "";
    out.elseNode = jv.find("else")    ? jv.find("else")->asStr()    : "";
    out.hostTriggered = jv.find("_trigger") != nullptr;
    if (out.line.empty()) { errors.push_back(nw + ": node missing `line`"); ok = false; }
    ok &= parseStoryCondList(jv.find("if"), out.conds, errors, nw);
    ok &= parseStoryFxList(jv.find("fx"), out.fx, errors, nw);
    if (const JValue* ch = jv.find("choices")) {
        if (!ch->isArr()) { errors.push_back(nw + ": `choices` is not an array"); return false; }
        uint32_t ci = 0;
        for (const JValue& c : *ch->arr) {
            ChatChoice cc;
            const std::string cw = nw + ".c" + std::to_string(ci++);
            if (!c.isObj()) { errors.push_back(cw + ": choice is not an object"); ok = false; continue; }
            cc.text = c.find("text") ? asciiFold(c.find("text")->asStr()) : "";
            cc.next = c.find("next") ? c.find("next")->asStr() : "";
            if (cc.text.empty()) { errors.push_back(cw + ": choice missing `text`"); ok = false; }
            if (cc.next.empty()) { errors.push_back(cw + ": choice missing `next`"); ok = false; }
            ok &= parseStoryCondList(c.find("if"), cc.conds, errors, cw);
            ok &= parseStoryFxList(c.find("fx"), cc.fx, errors, cw);
            out.choices.push_back(std::move(cc));
        }
    }
    return ok;
}

bool parseTree(const std::string& name, const JValue& jv, ChatTree& out,
               std::vector<std::string>& errors, const std::string& where) {
    out.name = name;
    if (!jv.isObj()) { errors.push_back(where + ": tree is not an object"); return false; }
    bool ok = true;
    out.start = jv.find("start") ? jv.find("start")->asStr() : "";
    if (const JValue* nodes = jv.find("nodes")) {
        if (!nodes->isArr()) { errors.push_back(where + ": `nodes` is not an array"); return false; }
        for (const JValue& n : *nodes->arr) {
            ChatNode node;
            if (parseNode(n, node, errors, where)) out.nodes.push_back(std::move(node));
            else ok = false;
        }
    }
    if (const JValue* pool = jv.find("pool")) {
        if (!pool->isArr()) { errors.push_back(where + ": `pool` is not an array"); return false; }
        uint32_t bi = 0;
        for (const JValue& b : *pool->arr) {
            ChatBanter bl;
            const std::string bw = where + ".pool" + std::to_string(bi++);
            if (!b.isObj()) { errors.push_back(bw + ": banter entry is not an object"); ok = false; continue; }
            bl.line = b.find("line") ? asciiFold(b.find("line")->asStr()) : "";
            if (bl.line.empty()) { errors.push_back(bw + ": banter missing `line`"); ok = false; }
            if (const JValue* w = b.find("weight")) bl.weight = std::max(1, (int)w->num);
            if (const JValue* o = b.find("once"))   bl.once = o->b;
            ok &= parseStoryCondList(b.find("if"), bl.conds, errors, bw);
            // `fx_on_play` — lena.json's banter-effect extension (format deviation;
            // accepted: effects applied when the bark plays).
            ok &= parseStoryFxList(b.find("fx_on_play"), bl.fx, errors, bw);
            out.pool.push_back(std::move(bl));
        }
    }
    if (out.nodes.empty() && out.pool.empty()) {
        errors.push_back(where + ": tree has neither `nodes` nor `pool`");
        ok = false;
    }
    if (!out.nodes.empty() && out.start.empty()) {
        errors.push_back(where + ": node tree missing `start`");
        ok = false;
    }
    return ok;
}

bool parseNpcBody(const std::string& id, const JValue& body, ChatNpc& out,
                  std::vector<std::string>& errors, const std::string& where) {
    out.id = id;
    out.display = body.find("display") ? body.find("display")->asStr() : id;
    out.voice   = body.find("voice")   ? body.find("voice")->asStr()   : "";
    const JValue* trees = body.find("trees");
    if (!trees || !trees->isObj()) {
        errors.push_back(where + ": missing `trees` object");
        return false;
    }
    bool ok = true;
    for (const auto& kv : *trees->obj) {
        ChatTree t;
        if (parseTree(kv.first, kv.second, t, errors, where + "." + kv.first))
            out.trees.push_back(std::move(t));
        else ok = false;
    }
    return ok;
}

// Lowercase ASCII helper -- shared impl (story_ops.h).
std::string lower(std::string_view s) { return lowerAscii(s); }

} // namespace

// ===========================================================================
// Model lookups
// ===========================================================================

const ChatNode* ChatTree::nodeById(std::string_view id) const {
    for (const ChatNode& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const ChatTree* ChatNpc::tree(std::string_view name) const {
    for (const ChatTree& t : trees) if (t.name == name) return &t;
    return nullptr;
}

// ===========================================================================
// Loader
// ===========================================================================

bool loadChatTreesFromJson(std::string_view jsonText, std::string_view srcName,
                           std::vector<ChatNpc>& out,
                           std::vector<std::string>& errors) {
    const std::string where0 = std::string(srcName);
    JParser parser(jsonText);
    JValue root = parser.parseValue();
    if (!parser.ok || !root.isObj()) {
        errors.push_back(where0 + ": JSON parse failed");
        return false;
    }
    const std::string fmt = root.find("format") ? root.find("format")->asStr() : "";
    if (fmt != "x3.chattree/1") {
        errors.push_back(where0 + ": format is `" + fmt + "` (want x3.chattree/1)");
        return false;
    }
    bool ok = true;
    if (const JValue* npcs = root.find("npcs")) {
        // Multi-NPC shape (club1127_patrons.json — format deviation, accepted):
        // "npcs": { "<id>": { display/voice/trees } , ... }.
        if (!npcs->isObj()) { errors.push_back(where0 + ": `npcs` is not an object"); return false; }
        for (const auto& kv : *npcs->obj) {
            ChatNpc npc;
            if (parseNpcBody(kv.first, kv.second, npc, errors, where0 + ":" + kv.first))
                out.push_back(std::move(npc));
            else ok = false;
        }
        return ok;
    }
    const std::string id = root.find("npc") ? root.find("npc")->asStr() : "";
    if (id.empty()) { errors.push_back(where0 + ": missing `npc` id"); return false; }
    ChatNpc npc;
    if (!parseNpcBody(id, root, npc, errors, where0 + ":" + id)) return false;
    out.push_back(std::move(npc));
    return ok;
}

bool loadChatTreeFile(const std::string& path, std::vector<ChatNpc>& out,
                      std::vector<std::string>& errors) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { errors.push_back(path + ": cannot open"); return false; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string name = std::filesystem::path(path).filename().string();
    return loadChatTreesFromJson(src, name, out, errors);
}

bool validateChatNpc(const ChatNpc& npc, bool fullReachability,
                     std::vector<std::string>& errors) {
    bool ok = true;
    for (const ChatTree& t : npc.trees) {
        const std::string where = npc.id + "." + t.name;
        if (t.nodes.empty()) continue;   // pool-only banter tree — nothing to chase
        // Unique ids.
        for (size_t i = 0; i < t.nodes.size(); ++i)
            for (size_t j = i + 1; j < t.nodes.size(); ++j)
                if (t.nodes[i].id == t.nodes[j].id) {
                    errors.push_back(where + ": duplicate node id `" + t.nodes[i].id + "`");
                    ok = false;
                }
        // Start resolves.
        if (!t.nodeById(t.start)) {
            errors.push_back(where + ": start `" + t.start + "` is not a node");
            ok = false;
        }
        // Every next/else/choice target resolves to a node or "end".
        auto checkRef = [&](const std::string& ref, const std::string& at) {
            if (ref.empty() || ref == "end") return;
            if (!t.nodeById(ref)) {
                errors.push_back(where + "." + at + ": dangling node ref `" + ref + "`");
                ok = false;
            }
        };
        for (const ChatNode& n : t.nodes) {
            checkRef(n.next, n.id + ".next");
            checkRef(n.elseNode, n.id + ".else");
            for (size_t c = 0; c < n.choices.size(); ++c)
                checkRef(n.choices[c].next, n.id + ".c" + std::to_string(c));
        }
        if (fullReachability) {
            // BFS from {start} ∪ {host-`_trigger` entry nodes} over next/else/choices.
            std::unordered_set<std::string> seen;
            std::vector<const ChatNode*> q;
            auto push = [&](const std::string& id) {
                if (id.empty() || id == "end" || seen.count(id)) return;
                if (const ChatNode* n = t.nodeById(id)) { seen.insert(id); q.push_back(n); }
            };
            push(t.start);
            for (const ChatNode& n : t.nodes) if (n.hostTriggered) push(n.id);
            while (!q.empty()) {
                const ChatNode* n = q.back(); q.pop_back();
                push(n->next); push(n->elseNode);
                for (const ChatChoice& c : n->choices) push(c.next);
            }
            for (const ChatNode& n : t.nodes)
                if (!seen.count(n.id)) {
                    errors.push_back(where + ": node `" + n.id +
                                     "` unreachable from start/host-trigger entries");
                    ok = false;
                }
        }
    }
    return ok;
}

// ===========================================================================
// ChatTreeSystem
// ===========================================================================

int ChatTreeSystem::loadDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int loaded = 0;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".json") continue;
        std::vector<ChatNpc> got;
        std::vector<std::string> errors;
        if (loadChatTreeFile(ent.path().string(), got, errors)) {
            for (ChatNpc& n : got) {
                std::vector<std::string> verr;
                if (!validateChatNpc(n, /*fullReachability*/false, verr)) {
                    for (const auto& e : verr) x3::logWarn("[chattree] " + e);
                    continue;   // a structurally bad npc is skipped whole
                }
                m_npcs.push_back(std::move(n));
                ++loaded;
            }
        } else {
            for (const auto& e : errors) x3::logWarn("[chattree] " + e);
        }
    }
    if (loaded)
        x3::logInfo("[chattree] loaded " + std::to_string(loaded) + " NPC(s) from " + dir);
    // Default context targets (the host overrides timeline/scripts/hooks).
    if (!m_ctx.flags) m_ctx.flags = &m_flags;
    return loaded;
}

int ChatTreeSystem::loadDefault() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe;
#ifdef _WIN32
    {
        char buf[1024];
        DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
        exe = (n && n < sizeof(buf)) ? fs::path(std::string(buf, n)).parent_path() : fs::path(".");
    }
#else
    exe = fs::current_path();
#endif
    const fs::path rel = fs::path("docs") / "design" / "narrative" / "chat_trees";
    const fs::path cands[] = {
        exe / ".." / ".." / ".." / rel,    // build/bin/<Config> -> repo root
        exe / rel,
        fs::path(".") / rel,
        rel,
    };
    for (const fs::path& c : cands)
        if (fs::is_directory(c, ec)) return loadDir(c.string());
    return 0;
}

const ChatNpc* ChatTreeSystem::npc(std::string_view id) const {
    const std::string want = lower(id);
    for (const ChatNpc& n : m_npcs) if (lower(n.id) == want) return &n;
    return nullptr;
}

const std::string& ChatTreeSystem::currentLine() const {
    static const std::string kEmpty;
    return (m_active && m_curNode) ? m_curNode->line : kEmpty;
}

const std::string& ChatTreeSystem::currentSpeaker() const {
    static const std::string kEmpty;
    if (!m_active || !m_curNode || !m_curNpc) return kEmpty;
    return m_curNode->speaker.empty() ? m_curNpc->display : m_curNode->speaker;
}

bool ChatTreeSystem::start(std::string_view npcId, std::string_view treeName) {
    const ChatNpc* n = npc(npcId);
    if (!n) return false;
    const ChatTree* t = n->tree(treeName);
    if (!t || t->nodes.empty()) return false;
    return startAt(npcId, treeName, t->start);
}

bool ChatTreeSystem::startAt(std::string_view npcId, std::string_view treeName,
                             std::string_view nodeId) {
    const ChatNpc* n = npc(npcId);
    if (!n) return false;
    const ChatTree* t = n->tree(treeName);
    if (!t) return false;
    m_curNpc  = n;
    m_curTree = t;
    m_npcId   = n->id;
    m_treeName = std::string(treeName);
    m_followFired = false;
    m_active = deliver(*n, *t, nodeId);
    if (!m_active) m_choices.clear();
    return m_active;
}

bool ChatTreeSystem::deliver(const ChatNpc& npcRef, const ChatTree& tree,
                             std::string_view nodeId) {
    std::string cur(nodeId);
    for (int hops = 0; hops < 16; ++hops) {
        if (cur.empty() || cur == "end") return false;
        const ChatNode* node = tree.nodeById(cur);
        if (!node) {
            x3::logWarn("[chattree] " + npcRef.id + "." + tree.name +
                        ": missing node `" + cur + "` — ending");
            return false;
        }
        // Node-level `if`: fail -> the `else` redirect (or end when absent —
        // spec leaves missing-else undefined; ending is the safe host behavior).
        const std::string key = npcRef.id + "." + node->id;
        if (!node->conds.empty() && !evalChatConds(node->conds, m_ctx, key)) {
            if (node->elseNode.empty()) return false;
            cur = node->elseNode;
            continue;
        }
        // Deliver: fx fire once, choices filter now.
        m_curNode = node;
        m_nodeId  = node->id;
        for (const ChatFx& f : node->fx)
            if (f.kind == ChatFxKind::Follow) m_followFired = true;
        applyChatFx(node->fx, m_ctx, npcRef.id);
        refreshChoices();
        return true;
    }
    x3::logWarn("[chattree] " + npcRef.id + "." + tree.name + ": else-chain loop guard hit");
    return false;
}

void ChatTreeSystem::refreshChoices() {
    m_choices.clear();
    if (!m_curNode || !m_curNpc) return;
    uint32_t ci = 0;
    for (const ChatChoice& c : m_curNode->choices) {
        const std::string key = m_npcId + "." + m_nodeId + ".c" + std::to_string(ci++);
        if (evalChatConds(c.conds, m_ctx, key)) m_choices.push_back(c);
    }
}

bool ChatTreeSystem::advance() {
    if (!m_active || !m_curNode || !m_curTree || !m_curNpc) return false;
    if (!m_choices.empty()) return true;      // a choice is required; E is a no-op
    const std::string next = m_curNode->next; // copy: deliver() may invalidate
    if (next.empty() || next == "end") { m_active = false; m_choices.clear(); return false; }
    m_active = deliver(*m_curNpc, *m_curTree, next);
    if (!m_active) m_choices.clear();
    return m_active;
}

bool ChatTreeSystem::choose(uint32_t index) {
    if (!m_active || index >= m_choices.size() || !m_curTree || !m_curNpc) return m_active;
    const ChatChoice c = m_choices[index];     // copy: deliver() replaces the node
    for (const ChatFx& f : c.fx)
        if (f.kind == ChatFxKind::Follow) m_followFired = true;
    applyChatFx(c.fx, m_ctx, m_npcId);
    if (c.next.empty() || c.next == "end") { m_active = false; m_choices.clear(); return false; }
    m_active = deliver(*m_curNpc, *m_curTree, c.next);
    if (!m_active) m_choices.clear();
    return m_active;
}

std::string ChatTreeSystem::pickBanter(std::string_view npcId, float roll01) {
    const ChatNpc* n = npc(npcId);
    if (!n) return "";
    const ChatTree* t = n->tree("banter");
    if (!t || t->pool.empty()) return "";
    // Eligible entries: conditions pass + once-respect.
    struct Cand { size_t idx; int weight; };
    std::vector<Cand> elig;
    for (size_t i = 0; i < t->pool.size(); ++i) {
        const ChatBanter& b = t->pool[i];
        const std::string onceKey = "banter." + n->id + "." + std::to_string(i);
        if (b.once && m_flags.seen(onceKey)) continue;
        if (!evalChatConds(b.conds, m_ctx, n->id + ".banter" + std::to_string(i))) continue;
        elig.push_back({ i, b.weight });
    }
    if (elig.empty()) return "";
    // Rotation: while more than one is eligible, never repeat the last pick.
    auto lastIt = m_lastBanter.find(n->id);
    if (elig.size() > 1 && lastIt != m_lastBanter.end()) {
        for (size_t i = 0; i < elig.size(); ++i)
            if (elig[i].idx == lastIt->second) { elig.erase(elig.begin() + i); break; }
    }
    int total = 0;
    for (const Cand& c : elig) total += c.weight;
    if (roll01 < 0.0f) roll01 = 0.0f;
    if (roll01 >= 1.0f) roll01 = 0.999999f;
    int pickAt = (int)(roll01 * (float)total);
    size_t picked = elig.back().idx;
    for (const Cand& c : elig) {
        pickAt -= c.weight;
        if (pickAt < 0) { picked = c.idx; break; }
    }
    const ChatBanter& b = t->pool[picked];
    m_lastBanter[n->id] = picked;
    if (b.once) m_flags.markSeen("banter." + n->id + "." + std::to_string(picked));
    applyChatFx(b.fx, m_ctx, n->id);
    return b.line;
}

// ===========================================================================
// HUD presentation — the npc_dialog box grown player-choice rows.
// ===========================================================================

namespace {
// Greedy word-wrap on a budget of ~`maxChars` per row (the Menu font is close
// enough to monospaced at subtitle size for a char budget; avoids per-word
// textAdvance calls). Returns up to `maxRows` rows; the rest is elided.
std::vector<std::string> wrapText(const std::string& text, size_t maxChars, size_t maxRows) {
    std::vector<std::string> rows;
    size_t pos = 0;
    while (pos < text.size() && rows.size() < maxRows) {
        size_t take = std::min(maxChars, text.size() - pos);
        if (pos + take < text.size()) {
            size_t brk = text.rfind(' ', pos + take);
            if (brk != std::string::npos && brk > pos) take = brk - pos;
        }
        rows.push_back(text.substr(pos, take));
        pos += take;
        while (pos < text.size() && text[pos] == ' ') ++pos;
    }
    if (pos < text.size() && !rows.empty()) rows.back() += " ...";
    return rows;
}
} // namespace

void drawChatTreeUi(x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame,
                    const ChatTreeSystem& sys) {
    if (!sys.active()) return;
    uint32_t hudW = 0, hudH = 0;
    device.hudSize(hudW, hudH);
    const float cx   = (hudW > 0) ? hudW * 0.5f : 640.0f;
    const float boxW = (hudW > 0) ? hudW * 0.72f : 920.0f;

    const std::string& speaker = sys.currentSpeaker();
    const std::vector<ChatChoice>& choices = sys.choices();
    // Char budgets measured against the REAL font: average advance of a sample
    // string at the line size vs the panel's usable width — wraps never overflow.
    const float avgChar = device.textAdvance(x3::rhi::FontRole::Menu,
                              "the quick brown fox jumps over it", 24.0f) / 33.0f;
    const size_t lineBudget   = (size_t)std::max(20.0f, (boxW - 56.0f)  / std::max(avgChar, 1.0f));
    const size_t choiceBudget = (size_t)std::max(20.0f, (boxW - 110.0f) / (std::max(avgChar, 1.0f) * (22.0f / 24.0f)));
    const std::vector<std::string> rows = wrapText(sys.currentLine(), lineBudget, 6);

    // Panel height: header + line rows + choice rows + hint.
    const float lineH = 30.0f, choiceH = 28.0f;
    const float boxH  = 64.0f + lineH * (float)rows.size()
                      + (choices.empty() ? 0.0f : 12.0f + choiceH * (float)choices.size())
                      + 30.0f;
    const float boxX = cx - boxW * 0.5f;
    const float boxY = (hudH > 0) ? (float)hudH - boxH - 64.0f : 540.0f;

    // The npc_dialog panel look (translucent navy + cyan rim).
    const float panel[4]  = { 0.05f, 0.07f, 0.12f, 0.82f };
    const float border[4] = { 0.40f, 0.78f, 1.0f, 0.85f };
    device.drawHudQuad(frame, boxX - 3.0f, boxY - 3.0f, boxW + 6.0f, boxH + 6.0f, border);
    device.drawHudQuad(frame, boxX, boxY, boxW, boxH, panel);

    // Speaker label (warm rose for her, cool cyan for "YOU").
    const bool isYou = (speaker == "YOU");
    const float namePx = 24.0f;
    const float herCol[4]  = { 1.0f, 0.62f, 0.78f, 1.0f };
    const float youCol[4]  = { 0.66f, 0.92f, 1.0f, 1.0f };
    const float shadow[4]  = { 0.0f, 0.0f, 0.0f, 0.75f };
    const float nameX = boxX + 24.0f, nameY = boxY + 16.0f;
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                        nameX + 1.5f, nameY + 1.5f, namePx, shadow);
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                        nameX, nameY, namePx, isYou ? youCol : herCol);

    // The spoken line (wrapped), white, GTA-subtitle weight.
    const float linePx = 24.0f;
    const float lineCol[4] = { 0.96f, 0.97f, 1.0f, 1.0f };
    float ty = boxY + 52.0f;
    for (const std::string& row : rows) {
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, row.c_str(),
                            boxX + 24.0f + 1.5f, ty + 1.5f, linePx, shadow);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, row.c_str(),
                            boxX + 24.0f, ty, linePx, lineCol);
        ty += lineH;
    }

    // Player choices: up to 4 numbered rows, cyan numerals + white text.
    if (!choices.empty()) {
        ty += 12.0f;
        const float numCol[4]    = { 0.40f, 0.86f, 1.0f, 1.0f };
        const float choiceCol[4] = { 0.85f, 0.90f, 1.0f, 1.0f };
        const uint32_t shown = std::min<uint32_t>((uint32_t)choices.size(), 4u);
        for (uint32_t i = 0; i < shown; ++i) {
            const std::string num = std::to_string(i + 1) + ".";
            const std::vector<std::string> crow = wrapText(choices[i].text, choiceBudget, 1);
            const std::string& ctext = crow.empty() ? choices[i].text : crow[0];
            device.drawHudTextF(frame, x3::rhi::FontRole::Menu, num.c_str(),
                                boxX + 32.0f + 1.5f, ty + 1.5f, 22.0f, shadow);
            device.drawHudTextF(frame, x3::rhi::FontRole::Menu, num.c_str(),
                                boxX + 32.0f, ty, 22.0f, numCol);
            device.drawHudTextF(frame, x3::rhi::FontRole::Menu, ctext.c_str(),
                                boxX + 70.0f + 1.5f, ty + 1.5f, 22.0f, shadow);
            device.drawHudTextF(frame, x3::rhi::FontRole::Menu, ctext.c_str(),
                                boxX + 70.0f, ty, 22.0f, choiceCol);
            ty += choiceH;
        }
    }

    // Hint, right-aligned at the bottom of the panel.
    const char* hint = choices.empty() ? "[E] Continue" : "[1-4] Choose";
    const float hintPx = 17.0f;
    const float hw = device.textAdvance(x3::rhi::FontRole::Menu, hint, hintPx);
    const float hintCol[4] = { 0.75f, 0.85f, 0.95f, 0.85f };
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, hint,
                        boxX + boxW - hw - 22.0f, boxY + boxH - 26.0f, hintPx, hintCol);
}

// ===========================================================================
// Headless self-test (--test-chattree).
// ===========================================================================

bool runChatTreeSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* name) {
        ++total;
        if (ok) { ++pass; x3::logInfo(std::string("  PASS CT") + std::to_string(total) + " " + name); }
        else    {         x3::logWarn(std::string("  FAIL CT") + std::to_string(total) + " " + name); }
        return ok;
    };

    // ---- CT: ALL chat_trees/*.json parse + structurally validate. -----------
    // 8 files -> 9 NPCs (club1127_patrons carries hollow_pete + static).
    ChatTreeSystem sys;
    const int loaded = sys.loadDefault();
    check(loaded >= 9, "parse: all 8 chat_trees files load (>= 9 NPCs incl. the patrons pair)");
    const char* kAll[] = { "aria", "keisha", "emily", "lena", "martinez",
                           "dr_reyes", "vesper", "hollow_pete", "static" };
    {
        int found = 0;
        for (const char* id : kAll) if (sys.hasNpc(id)) ++found;
        // dr_reyes/vesper ids come from the files' own `npc` fields — resolve loosely.
        check(found >= 7 && sys.hasNpc("lena") && sys.hasNpc("aria"),
              "roster: expected NPC ids resolve (lena + aria present)");
    }

    // ---- CT: lena + aria FULL validation (reachability incl. _trigger entries).
    {
        std::vector<std::string> errs;
        const bool lenaOk = sys.npc("lena") && validateChatNpc(*sys.npc("lena"), true, errs);
        const bool ariaOk = sys.npc("aria") && validateChatNpc(*sys.npc("aria"), true, errs);
        for (const auto& e : errs) x3::logWarn("[chattree-test] " + e);
        check(lenaOk, "validate: lena.json fully sound (every node reachable, no dangling refs)");
        check(ariaOk, "validate: aria.json fully sound");
    }

    // ---- Context: local TimelineState + a REAL Lua script system. -----------
    TimelineState tl;
    std::unique_ptr<x3::script::IScriptSystem> scripts(
        x3::script::createLuaScriptSystem(nullptr));
    std::vector<std::string> marks;     // events observed BY the script
    std::string hintCode;               // dialog_hint code observed BY the script
    scripts->registerFunction("mark",
        [&](const std::vector<x3::script::ScriptValue>& a) -> x3::script::ScriptValue {
            if (!a.empty()) marks.push_back(a[0].asString());
            return {};
        });
    scripts->registerFunction("hint",
        [&](const std::vector<x3::script::ScriptValue>& a) -> x3::script::ScriptValue {
            if (!a.empty()) hintCode = a[0].asString();
            return {};
        });
    const char* kTestLua =
        "function onEvent(name, args)\n"
        "  if name == 'rumor' or name == 'companion_joined' then x3.mark(name) end\n"
        "  if name == 'dialog_hint' then x3.hint(args.code) end\n"
        "end\n"
        "function condYes() return true end\n"
        "function condNo() return false end\n";
    const x3::script::ScriptId sid = scripts->load("chattree_test.lua", kTestLua);
    check(sid != x3::script::kInvalidScript && !scripts->status(sid).failed,
          "lua: test observer script loads");

    bool followCalled = false;
    sys.ctx().timeline = &tl;
    sys.ctx().scripts  = scripts.get();
    sys.ctx().follow   = [&]() { followCalled = true; return true; };
    sys.ctx().luaCond  = [&](const std::string& fn) {
        return scripts->eval(sid, fn + "()") == "true";
    };

    // ---- CT: entry if/else redirect (lena.interrupted). ---------------------
    check(sys.start("lena", "first_meeting") && sys.currentNodeId() == "fm0_alt",
          "entry: no lena.interrupted -> else-redirect lands fm0_alt");
    check(sys.flags().has("lena.met"), "fx-on-delivery: fm0_alt set lena.met");
    sys.cancel();
    sys.flags().set("lena.interrupted");
    check(sys.start("lena", "first_meeting") && sys.currentNodeId() == "fm0",
          "entry: lena.interrupted set -> fm0 (condition-gated node now shown)");

    // ---- CT: the lena walk — choices, karma fx, fire observed, follow. ------
    {
        const int karma0 = tl.axes().karma;
        check(sys.choices().size() == 3, "fm0: 3 choices offered");
        sys.choose(0);                                     // "(cut her loose)" karma+1 -> fm1
        check(tl.axes().karma == karma0 + 1, "choice fx: karma +1 applied via TimelineState");
        check(sys.currentNodeId() == "fm1" && sys.choices().empty(), "fm1 delivered (no choices)");
        sys.advance();                                     // -> fm2
        check(sys.currentNodeId() == "fm2" && sys.choices().size() == 3, "fm2: the free-question test");
        const int love0 = tl.axes().love;
        sys.choose(2);                                     // "Are you okay?" karma+2 love+3 -> fm3c
        check(tl.axes().karma == karma0 + 3 && tl.axes().love == love0 + 3,
              "choice fx: karma+2 / love+3 applied");
        check(sys.currentNodeId() == "fm3c" && sys.flags().has("lena.asked_her"),
              "fm3c delivered + lena.asked_her set");
        sys.advance();                                     // -> fm4 (follow + rel + fire)
        check(sys.currentNodeId() == "fm4", "fm4 (the spine tail) reached");
        check(followCalled && sys.followFired(), "follow effect: host rescue callback fired");
        check(sys.flags().rel("lena") == 1, "rel effect: lena -> 1 (Rescued)");
        const bool companionSeen =
            std::find(marks.begin(), marks.end(), "companion_joined") != marks.end();
        check(companionSeen, "x3.fire: companion_joined observed by the loaded Lua script");
        const bool stillActive = sys.advance();            // next: "end"
        check(!stillActive && !sys.active(), "spine: conversation ends cleanly");
    }

    // ---- CT: condition-gated choice hidden then shown (flag gate). ----------
    {
        const char* kGated =
            "{ \"format\": \"x3.chattree/1\", \"npc\": \"testy\", \"display\": \"TESTY\","
            "  \"trees\": { \"t\": { \"start\": \"a\", \"nodes\": ["
            "    { \"id\": \"a\", \"line\": \"pick.\", \"choices\": ["
            "      { \"text\": \"always\", \"next\": \"end\" },"
            "      { \"text\": \"gated\",  \"next\": \"end\", \"if\": [ {\"flag\": \"test.gate\"} ] },"
            "      { \"text\": \"lua\",    \"next\": \"end\", \"if\": [ {\"lua\": \"condNo\"} ] } ] } ] },"
            "    \"banter\": { \"pool\": ["
            "      { \"line\": \"only-once\", \"once\": true } ] } } }";
        std::vector<ChatNpc> got; std::vector<std::string> errs;
        check(loadChatTreesFromJson(kGated, "inline", got, errs) && got.size() == 1,
              "inline gated tree parses");
        // No public injector by design (data comes from files); run it through a
        // temp dir round-trip so the REAL file lane is exercised too.
        ChatTreeSystem mini;
        namespace fs = std::filesystem;
        const fs::path tdir = fs::temp_directory_path() / "x3_chattree_test";
        {
            std::error_code ec; fs::create_directories(tdir, ec);
            std::ofstream f((tdir / "testy.json").string(), std::ios::binary | std::ios::trunc);
            f << kGated; f.close();
            check(mini.loadDir(tdir.string()) == 1, "temp-dir load: inline tree via file lane");
        }
        mini.ctx().timeline = &tl;
        mini.ctx().scripts  = scripts.get();
        mini.ctx().luaCond  = sys.ctx().luaCond;
        check(mini.start("testy", "t") && mini.choices().size() == 1,
              "choice filtering: flag-gated + lua-false choices HIDDEN");
        mini.cancel();
        mini.flags().set("test.gate");
        check(mini.start("testy", "t") && mini.choices().size() == 2,
              "choice filtering: flag set -> gated choice SHOWN");
        mini.cancel();
        // lua condition flips via the script system (condYes vs condNo).
        mini.ctx().luaCond = [&](const std::string&) {
            return scripts->eval(sid, "condYes()") == "true";
        };
        check(mini.start("testy", "t") && mini.choices().size() == 3,
              "lua condition: condYes via IScriptSystem::eval -> choice SHOWN");
        mini.cancel();
        // once-banter: first pick plays, second is exhausted.
        const std::string b1 = mini.pickBanter("testy", 0.0f);
        const std::string b2 = mini.pickBanter("testy", 0.0f);
        check(b1 == "only-once" && b2.empty(), "banter `once`: plays exactly once");
        std::error_code ec; fs::remove_all(tdir, ec);
    }

    // ---- CT: the trust path — the 1278 teach beat. ---------------------------
    {
        check(!sys.start("lena", "trust"), "trust: gated OFF at rel 1 / mercy 50 (no else -> won't start)");
        sys.flags().raiseRel("lena", 2);
        tl.adjustMercy(10);                                // 50 -> 60 (>= 55)
        check(sys.start("lena", "trust") && sys.currentNodeId() == "t0",
              "trust: rel 2 + mercy 60 -> t0 (the 1278 teach node) reachable");
        check(sys.flags().has("code.1278.known"), "t0 fx: code.1278.known set");
        check(hintCode == "1278", "x3.fire: dialog_hint{code=1278} observed by the Lua script");
        check(sys.choices().size() == 3, "t0: 3 replies");
        sys.choose(2);                                     // "(just memorize it)" -> t1c, rel 3
        check(sys.currentNodeId() == "t1c" && sys.flags().rel("lena") == 3,
              "t1c: rel raised to 3 (Bond)");
        sys.advance();
        check(!sys.active(), "trust scene ends");
    }

    // ---- CT: banter pool — gating + rotation. --------------------------------
    {
        const std::string b1 = sys.pickBanter("lena", 0.0f);
        const std::string b2 = sys.pickBanter("lena", 0.0f);
        check(!b1.empty() && !b2.empty() && b1 != b2,
              "banter: rotation never repeats the last line while others are eligible");
        // The carrier line is gated on lena.carrier — never eligible here.
        bool carrierLeaked = false;
        for (int i = 0; i < 16; ++i) {
            const std::string b = sys.pickBanter("lena", (float)i / 16.0f);
            if (b.find("drift wrong") != std::string::npos) carrierLeaked = true;
        }
        check(!carrierLeaked, "banter: lena.carrier-gated line stays hidden");
    }

    // ---- CT: deterministic chance. -------------------------------------------
    {
        ChatContext c1; c1.chanceSeed = 7;
        const float r1 = 0.0f; (void)r1;
        std::vector<ChatCond> cc(1);
        cc[0].kind = ChatCondKind::Chance; cc[0].f = 0.5f;
        const bool a = evalChatConds(cc, c1, "npc.node1");
        const bool b = evalChatConds(cc, c1, "npc.node1");
        check(a == b, "chance: deterministic per (seed, node)");
    }

    // ---- CT: StoryFlags + rel save/load round-trip (file lane). --------------
    {
        namespace fs = std::filesystem;
        const std::string path =
            (fs::temp_directory_path() / "x3_chattree_flags.txt").string();
        check(sys.flags().saveFile(path), "flags: saveFile writes");
        StoryFlags back;
        check(back.loadFile(path), "flags: loadFile reads");
        check(back.has("lena.met") && back.has("lena.interrupted") &&
              back.has("code.1278.known") && back.rel("lena") == 3,
              "flags: flags + rel survive the round-trip exactly");
        check(back.serialize() == sys.flags().serialize(),
              "flags: serialized blobs identical");
        std::error_code ec; fs::remove(path, ec);
    }

    x3::logInfo("chattree: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
