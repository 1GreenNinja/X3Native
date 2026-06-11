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
// Minimal JSON parser — the same lean, file-local JParser shape canon_play.cpp
// and level_loader.cpp carry (full value set; UTF-8 bytes pass through).
// ---------------------------------------------------------------------------
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
    std::string scalarStr() const {
        switch (t) {
            case T::Str:  return str;
            case T::Bool: return b ? "true" : "false";
            case T::Num: {
                // Integral numbers print without a trailing ".000000".
                if (num == (double)(long long)num) return std::to_string((long long)num);
                std::ostringstream os; os << num; return os.str();
            }
            default: return "";
        }
    }
};

struct JParser {
    const char* p; const char* end; bool ok = true;
    explicit JParser(std::string_view s) : p(s.data()), end(s.data() + s.size()) {}
    void skipWs() { while (p < end) { char c = *p; if (c==' '||c=='\t'||c=='\n'||c=='\r') { ++p; continue; } break; } }
    JValue parseValue() {
        skipWs();
        if (p >= end) { ok = false; return {}; }
        char c = *p;
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { JValue v; v.t = JValue::T::Str; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { p += 4; JValue v; v.t = JValue::T::Null; return v; }
        return parseNumber();
    }
    std::string parseString() {
        std::string out; ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': out += '\n'; break;  case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;  case '"': out += '"'; break;
                    case '\\': out += '\\'; break; case '/': out += '/'; break;
                    case 'u': {
                        if (p + 4 <= end) {
                            int code = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = *p++; code <<= 4;
                                if (h >= '0' && h <= '9') code |= (h - '0');
                                else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            }
                            // Encode the BMP code point as UTF-8 (the trees carry
                            // em-dashes etc. as literal UTF-8, but be correct here too).
                            if (code < 0x80) out += (char)code;
                            else if (code < 0x800) {
                                out += (char)(0xC0 | (code >> 6));
                                out += (char)(0x80 | (code & 0x3F));
                            } else {
                                out += (char)(0xE0 | (code >> 12));
                                out += (char)(0x80 | ((code >> 6) & 0x3F));
                                out += (char)(0x80 | (code & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else out += c;
        }
        if (p < end) ++p;
        return out;
    }
    JValue parseNumber() {
        const char* s = p;
        while (p < end) { char c = *p; if ((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E') { ++p; continue; } break; }
        if (p == s) { ok = false; ++p; return {}; }
        JValue v; v.t = JValue::T::Num; v.num = std::strtod(std::string(s, p).c_str(), nullptr); return v;
    }
    JValue parseBool() { JValue v; v.t = JValue::T::Bool; if (*p=='t') { v.b=true; p+=4; } else { v.b=false; p+=5; } return v; }
    JValue parseArray() {
        JValue v; v.t = JValue::T::Arr; v.arr = std::make_shared<JArray>(); ++p; skipWs();
        if (p < end && *p == ']') { ++p; return v; }
        while (p < end) {
            v.arr->push_back(parseValue()); skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; break; }
            if (p >= end) { ok = false; break; }
            ++p;
        }
        return v;
    }
    JValue parseObject() {
        JValue v; v.t = JValue::T::Obj; v.obj = std::make_shared<JObject>(); ++p; skipWs();
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end) {
            skipWs();
            if (p >= end || *p != '"') { ok = false; break; }
            std::string key = parseString(); skipWs();
            if (p < end && *p == ':') ++p; else { ok = false; break; }
            JValue val = parseValue();
            v.obj->emplace_back(std::move(key), std::move(val));
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; break; }
            if (p >= end) { ok = false; break; }
            ++p;
        }
        return v;
    }
};

// ---------------------------------------------------------------------------
// Cond / fx parsing from JValues (the closed spec-§3 vocabulary).
// ---------------------------------------------------------------------------

bool parseCond(const JValue& jv, ChatCond& out, std::vector<std::string>& errors,
               const std::string& where);

bool parseCondList(const JValue* jv, std::vector<ChatCond>& out,
                   std::vector<std::string>& errors, const std::string& where) {
    if (!jv) return true;
    if (!jv->isArr()) { errors.push_back(where + ": `if` is not an array"); return false; }
    bool ok = true;
    for (const JValue& c : *jv->arr) {
        ChatCond cc;
        if (parseCond(c, cc, errors, where)) out.push_back(std::move(cc));
        else ok = false;
    }
    return ok;
}

// Axis _gte condition kinds by key.
bool axisCondKind(const std::string& key, ChatCondKind& out) {
    if (key == "karma_gte")      { out = ChatCondKind::KarmaGte;      return true; }
    if (key == "karma_lte")      { out = ChatCondKind::KarmaLte;      return true; }
    if (key == "humanity_gte")   { out = ChatCondKind::HumanityGte;   return true; }
    if (key == "trust_gte")      { out = ChatCondKind::TrustGte;      return true; }
    if (key == "mercy_gte")      { out = ChatCondKind::MercyGte;      return true; }
    if (key == "love_gte")       { out = ChatCondKind::LoveGte;       return true; }
    if (key == "redemption_gte") { out = ChatCondKind::RedemptionGte; return true; }
    return false;
}

bool parseCond(const JValue& jv, ChatCond& out, std::vector<std::string>& errors,
               const std::string& where) {
    if (!jv.isObj() || jv.obj->empty()) {
        errors.push_back(where + ": condition is not an object");
        return false;
    }
    // A condition object carries exactly one operative key.
    const std::string& key = jv.obj->front().first;
    const JValue&      val = jv.obj->front().second;

    ChatCondKind axis;
    if (key == "flag")      { out.kind = ChatCondKind::Flag; out.s = val.asStr(); return !out.s.empty(); }
    if (axisCondKind(key, axis)) { out.kind = axis; out.n = (int)val.num; return val.isNum(); }
    if (key == "timeline") {
        out.kind = ChatCondKind::Timeline;
        if (!val.isArr()) { errors.push_back(where + ": `timeline` wants an array"); return false; }
        for (const JValue& t : *val.arr) out.names.push_back(t.asStr());
        return true;
    }
    if (key == "girl_saved") { out.kind = ChatCondKind::GirlSaved; out.s = val.asStr(); return !out.s.empty(); }
    if (key == "girl_lost")  { out.kind = ChatCondKind::GirlLost;  out.s = val.asStr(); return !out.s.empty(); }
    if (key == "item")       { out.kind = ChatCondKind::Item;      out.s = val.asStr(); return !out.s.empty(); }
    if (key == "rel_gte") {
        out.kind = ChatCondKind::RelGte;
        if (!val.isArr() || val.arr->size() != 2 || !(*val.arr)[0].isStr() || !(*val.arr)[1].isNum()) {
            errors.push_back(where + ": `rel_gte` wants [\"npc\", n]");
            return false;
        }
        out.s = (*val.arr)[0].str;
        out.n = (int)(*val.arr)[1].num;
        return true;
    }
    if (key == "chance") { out.kind = ChatCondKind::Chance; out.f = (float)val.num; return val.isNum(); }
    if (key == "lua")    { out.kind = ChatCondKind::Lua;    out.s = val.asStr(); return !out.s.empty(); }
    if (key == "any") {
        out.kind = ChatCondKind::Any;
        return parseCondList(&val, out.sub, errors, where + ".any");
    }
    if (key == "not") {
        out.kind = ChatCondKind::Not;
        ChatCond inner;
        if (!parseCond(val, inner, errors, where + ".not")) return false;
        out.sub.push_back(std::move(inner));
        return true;
    }
    errors.push_back(where + ": unrecognized condition kind `" + key + "`");
    return false;
}

// Effect-kind axis names (fx {"karma": +n} etc.).
bool isAxisFxKey(const std::string& key) {
    return key == "karma" || key == "humanity" || key == "trust" ||
           key == "mercy" || key == "love"     || key == "redemption";
}

bool parseFx(const JValue& jv, ChatFx& out, std::vector<std::string>& errors,
             const std::string& where) {
    if (!jv.isObj() || jv.obj->empty()) {
        errors.push_back(where + ": effect is not an object");
        return false;
    }
    const std::string& key = jv.obj->front().first;
    const JValue&      val = jv.obj->front().second;

    if (isAxisFxKey(key)) { out.kind = ChatFxKind::Axis; out.s = key; out.n = (int)val.num; return val.isNum(); }
    if (key == "set")   { out.kind = ChatFxKind::SetFlag;   out.s = val.asStr(); return !out.s.empty(); }
    if (key == "clear") { out.kind = ChatFxKind::ClearFlag; out.s = val.asStr(); return !out.s.empty(); }
    if (key == "fire") {
        out.kind = ChatFxKind::Fire;
        out.s = val.asStr();
        if (out.s.empty()) { errors.push_back(where + ": `fire` wants an event name"); return false; }
        // Optional sibling `args` object — stringified key/value pairs.
        if (const JValue* a = jv.find("args")) {
            if (!a->isObj()) { errors.push_back(where + ": `args` is not an object"); return false; }
            for (const auto& kv : *a->obj)
                out.args.emplace_back(kv.first, kv.second.scalarStr());
        }
        return true;
    }
    if (key == "give")   { out.kind = ChatFxKind::Give; out.s = val.asStr(); return !out.s.empty(); }
    if (key == "take")   { out.kind = ChatFxKind::Take; out.s = val.asStr(); return !out.s.empty(); }
    if (key == "follow") { out.kind = ChatFxKind::Follow; return true; }
    if (key == "rel") {
        out.kind = ChatFxKind::Rel;
        if (!val.isArr() || val.arr->size() != 2 || !(*val.arr)[0].isStr() || !(*val.arr)[1].isNum()) {
            errors.push_back(where + ": `rel` wants [\"npc\", stage]");
            return false;
        }
        out.s = (*val.arr)[0].str;
        out.n = (int)(*val.arr)[1].num;
        return true;
    }
    if (key == "ally") { out.kind = ChatFxKind::Ally; return true; }
    if (key == "end")  { out.kind = ChatFxKind::End;  out.s = val.asStr(); return true; }
    if (key == "args") {
        // `args` leading an fx object means the authoring put args before fire —
        // accept by looking up the `fire` sibling.
        if (const JValue* f = jv.find("fire")) {
            out.kind = ChatFxKind::Fire; out.s = f->asStr();
            if (jv.find("args")->isObj())
                for (const auto& kv : *jv.find("args")->obj)
                    out.args.emplace_back(kv.first, kv.second.scalarStr());
            return !out.s.empty();
        }
        errors.push_back(where + ": dangling `args` with no `fire`");
        return false;
    }
    errors.push_back(where + ": unrecognized effect kind `" + key + "`");
    return false;
}

bool parseFxList(const JValue* jv, std::vector<ChatFx>& out,
                 std::vector<std::string>& errors, const std::string& where) {
    if (!jv) return true;
    if (!jv->isArr()) { errors.push_back(where + ": `fx` is not an array"); return false; }
    bool ok = true;
    for (const JValue& f : *jv->arr) {
        ChatFx fx;
        if (parseFx(f, fx, errors, where)) out.push_back(std::move(fx));
        else ok = false;
    }
    return ok;
}

bool parseNode(const JValue& jv, ChatNode& out, std::vector<std::string>& errors,
               const std::string& where) {
    if (!jv.isObj()) { errors.push_back(where + ": node is not an object"); return false; }
    bool ok = true;
    out.id = jv.find("id") ? jv.find("id")->asStr() : "";
    if (out.id.empty()) { errors.push_back(where + ": node missing `id`"); ok = false; }
    const std::string nw = where + "." + out.id;
    out.line     = jv.find("line")    ? jv.find("line")->asStr()    : "";
    out.speaker  = jv.find("speaker") ? jv.find("speaker")->asStr() : "";
    out.next     = jv.find("next")    ? jv.find("next")->asStr()    : "";
    out.elseNode = jv.find("else")    ? jv.find("else")->asStr()    : "";
    out.hostTriggered = jv.find("_trigger") != nullptr;
    if (out.line.empty()) { errors.push_back(nw + ": node missing `line`"); ok = false; }
    ok &= parseCondList(jv.find("if"), out.conds, errors, nw);
    ok &= parseFxList(jv.find("fx"), out.fx, errors, nw);
    if (const JValue* ch = jv.find("choices")) {
        if (!ch->isArr()) { errors.push_back(nw + ": `choices` is not an array"); return false; }
        uint32_t ci = 0;
        for (const JValue& c : *ch->arr) {
            ChatChoice cc;
            const std::string cw = nw + ".c" + std::to_string(ci++);
            if (!c.isObj()) { errors.push_back(cw + ": choice is not an object"); ok = false; continue; }
            cc.text = c.find("text") ? c.find("text")->asStr() : "";
            cc.next = c.find("next") ? c.find("next")->asStr() : "";
            if (cc.text.empty()) { errors.push_back(cw + ": choice missing `text`"); ok = false; }
            if (cc.next.empty()) { errors.push_back(cw + ": choice missing `next`"); ok = false; }
            ok &= parseCondList(c.find("if"), cc.conds, errors, cw);
            ok &= parseFxList(c.find("fx"), cc.fx, errors, cw);
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
            bl.line = b.find("line") ? b.find("line")->asStr() : "";
            if (bl.line.empty()) { errors.push_back(bw + ": banter missing `line`"); ok = false; }
            if (const JValue* w = b.find("weight")) bl.weight = std::max(1, (int)w->num);
            if (const JValue* o = b.find("once"))   bl.once = o->b;
            ok &= parseCondList(b.find("if"), bl.conds, errors, bw);
            // `fx_on_play` — lena.json's banter-effect extension (format deviation;
            // accepted: effects applied when the bark plays).
            ok &= parseFxList(b.find("fx_on_play"), bl.fx, errors, bw);
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

// Lowercase ASCII helper (ids/names are ASCII by construction).
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

// Deterministic per-(save,node) hash -> [0,1) for {"chance"} (spec §3).
float chanceRoll(uint32_t seed, std::string_view nodeKey) {
    uint32_t h = 2166136261u ^ seed;            // FNV-1a over seed + key
    for (char c : nodeKey) { h ^= (uint8_t)c; h *= 16777619u; }
    return (float)(h % 100000u) / 100000.0f;
}

// girl name -> TimelineState Woman (the F2 three). Others (lena/sarah/...) fall
// back to the `<girl>.rescued` story flag (the spec's own glossary key) — the
// trees use girl_saved for non-F2 girls too (format deviation, handled).
bool womanByName(std::string_view girl, Woman& out) {
    const std::string g = lower(girl);
    if (g == "aria")   { out = Woman::Aria;   return true; }
    if (g == "keisha") { out = Woman::Keisha; return true; }
    if (g == "emily")  { out = Woman::Emily;  return true; }
    return false;
}

} // namespace

// ===========================================================================
// StoryFlags
// ===========================================================================

int StoryFlags::rel(std::string_view npc) const {
    auto it = m_rel.find(std::string(npc));
    return it == m_rel.end() ? 0 : it->second;
}

void StoryFlags::raiseRel(std::string_view npc, int stage) {
    int& cur = m_rel[std::string(npc)];
    if (stage > cur) cur = stage;
}

std::string StoryFlags::serialize() const {
    // Line-based text blob: "flag <name>" / "rel <npc> <n>" / "item <id>" /
    // "seen <key>". Sorted for deterministic round-trip diffs.
    std::vector<std::string> lines;
    for (const auto& f : m_flags) lines.push_back("flag " + f);
    for (const auto& kv : m_rel)  lines.push_back("rel " + kv.first + " " + std::to_string(kv.second));
    for (const auto& i : m_items) lines.push_back("item " + i);
    for (const auto& s : m_seen)  lines.push_back("seen " + s);
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto& l : lines) { out += l; out += '\n'; }
    return out;
}

bool StoryFlags::deserialize(std::string_view text) {
    clearAll();
    std::istringstream in{ std::string(text) };
    std::string kind, a;
    while (in >> kind >> a) {
        if      (kind == "flag") m_flags.insert(a);
        else if (kind == "item") m_items.insert(a);
        else if (kind == "seen") m_seen.insert(a);
        else if (kind == "rel")  { int n = 0; if (in >> n) m_rel[a] = n; }
        // Unknown kinds are skipped (forward-compatible).
    }
    return true;
}

bool StoryFlags::saveFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string blob = serialize();
    f.write(blob.data(), (std::streamsize)blob.size());
    return (bool)f;
}

bool StoryFlags::loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return deserialize(blob);
}

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
// Condition evaluation
// ===========================================================================

namespace {
bool evalOne(const ChatCond& c, const ChatContext& ctx, std::string_view nodeKey) {
    const MoralityAxes* ax = ctx.timeline ? &ctx.timeline->axes() : nullptr;
    switch (c.kind) {
        case ChatCondKind::Flag:        return ctx.flags && ctx.flags->has(c.s);
        case ChatCondKind::KarmaGte:    return ax && ax->karma >= c.n;
        case ChatCondKind::KarmaLte:    return ax && ax->karma <= c.n;
        case ChatCondKind::HumanityGte: return ax && ax->humanity >= c.n;
        case ChatCondKind::TrustGte:    return ax && ax->trust >= c.n;
        case ChatCondKind::MercyGte:    return ax && ax->mercy >= c.n;
        case ChatCondKind::LoveGte:     return ax && ax->love >= c.n;
        case ChatCondKind::RedemptionGte: return ax && ax->redemption >= c.n;
        case ChatCondKind::Timeline: {
            if (!ctx.timeline) return false;
            const char* cur = timelineName(ctx.timeline->timeline());
            for (const std::string& t : c.names)
                if (lower(t) == lower(cur)) return true;
            return false;
        }
        case ChatCondKind::GirlSaved: {
            Woman w;
            if (ctx.timeline && womanByName(c.s, w)) {
                const CaptiveFate f = ctx.timeline->fate(w);
                if (f == CaptiveFate::Saved || f == CaptiveFate::Cured) return true;
            }
            // Non-F2 girls (lena/...) — the glossary `<girl>.rescued` flag.
            return ctx.flags && ctx.flags->has(lower(c.s) + ".rescued");
        }
        case ChatCondKind::GirlLost: {
            Woman w;
            if (ctx.timeline && womanByName(c.s, w))
                return ctx.timeline->fate(w) == CaptiveFate::Lost;
            return ctx.flags && ctx.flags->has(lower(c.s) + ".lost");
        }
        case ChatCondKind::Item:   return ctx.flags && ctx.flags->hasItem(c.s);
        case ChatCondKind::RelGte: return ctx.flags && ctx.flags->rel(lower(c.s)) >= c.n;
        case ChatCondKind::Chance: return chanceRoll(ctx.chanceSeed, nodeKey) < c.f;
        case ChatCondKind::Lua:
            if (!ctx.luaCond) {
                x3::logWarn("[chattree] {\"lua\": \"" + c.s + "\"} with no luaCond hook -> false");
                return false;
            }
            return ctx.luaCond(c.s);
        case ChatCondKind::Any:
            for (const ChatCond& s : c.sub)
                if (evalOne(s, ctx, nodeKey)) return true;
            return false;
        case ChatCondKind::Not:
            return c.sub.empty() ? false : !evalOne(c.sub[0], ctx, nodeKey);
    }
    return false;
}
} // namespace

bool evalChatConds(const std::vector<ChatCond>& conds, const ChatContext& ctx,
                   std::string_view nodeKey) {
    for (const ChatCond& c : conds)
        if (!evalOne(c, ctx, nodeKey)) return false;
    return true;   // AND semantics; empty list passes
}

// ===========================================================================
// Effect application — routed through the EXISTING sinks (no duplicates):
// TimelineState::adjust*, StoryFlags, IScriptSystem::fire, the host follow fn.
// ===========================================================================

void applyChatFx(const std::vector<ChatFx>& fx, const ChatContext& ctx,
                 std::string_view npcId) {
    for (const ChatFx& f : fx) {
        switch (f.kind) {
            case ChatFxKind::Axis:
                if (ctx.timeline) {
                    if      (f.s == "karma")      ctx.timeline->adjustKarma(f.n);
                    else if (f.s == "humanity")   ctx.timeline->adjustHumanity(f.n);
                    else if (f.s == "trust")      ctx.timeline->adjustTrust(f.n);
                    else if (f.s == "mercy")      ctx.timeline->adjustMercy(f.n);
                    else if (f.s == "love")       ctx.timeline->adjustLove(f.n);
                    else if (f.s == "redemption") ctx.timeline->adjustRedemption(f.n);
                }
                break;
            case ChatFxKind::SetFlag:   if (ctx.flags) ctx.flags->set(f.s); break;
            case ChatFxKind::ClearFlag: if (ctx.flags) ctx.flags->clear(f.s); break;
            case ChatFxKind::Fire:
                if (ctx.scripts) {
                    x3::script::EventArgs args = f.args;
                    args.emplace_back("npc", std::string(npcId));
                    ctx.scripts->fire(f.s, args);
                }
                break;
            case ChatFxKind::Give: if (ctx.flags) ctx.flags->give(f.s); break;
            case ChatFxKind::Take: if (ctx.flags) ctx.flags->take(f.s); break;
            case ChatFxKind::Follow:
                if (ctx.follow) {
                    if (!ctx.follow())
                        x3::logWarn("[chattree] follow effect: host rescue refused (" +
                                    std::string(npcId) + ")");
                } else {
                    x3::logWarn("[chattree] follow effect with no host hook (" +
                                std::string(npcId) + ")");
                }
                break;
            case ChatFxKind::Rel:  if (ctx.flags) ctx.flags->raiseRel(lower(f.s), f.n); break;
            case ChatFxKind::Ally: if (ctx.timeline) ctx.timeline->onAllyJoined(); break;
            case ChatFxKind::End:
                if (ctx.scripts)
                    ctx.scripts->fire("dialog_end", {{"npc", std::string(npcId)},
                                                     {"verb", f.s}});
                break;
        }
    }
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
    const std::vector<std::string> rows = wrapText(sys.currentLine(), 88, 5);

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
            const std::vector<std::string> crow = wrapText(choices[i].text, 84, 1);
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
