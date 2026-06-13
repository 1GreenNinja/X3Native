#include "story_ops.h"
// STORY OPS — shared condition/effect vocabulary implementation. Moved verbatim
// from chat_tree.cpp (the x3.chattree/1 runner) so the mission runner shares it.

#include "timeline.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace x3::game {

// ===========================================================================
// JSON reader
// ===========================================================================

std::string JValue::scalarStr() const {
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

JValue JParser::parseValue() {
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

std::string JParser::parseString() {
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
                        // Encode the BMP code point as UTF-8 (authored docs carry
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

JValue JParser::parseNumber() {
    const char* s = p;
    while (p < end) { char c = *p; if ((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E') { ++p; continue; } break; }
    if (p == s) { ok = false; ++p; return {}; }
    JValue v; v.t = JValue::T::Num; v.num = std::strtod(std::string(s, p).c_str(), nullptr); return v;
}

JValue JParser::parseBool() {
    JValue v; v.t = JValue::T::Bool;
    if (*p=='t') { v.b=true; p+=4; } else { v.b=false; p+=5; }
    return v;
}

JValue JParser::parseArray() {
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

JValue JParser::parseObject() {
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

// ===========================================================================
// Text helpers
// ===========================================================================

std::string asciiFold(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out += (char)c; ++i; continue; }
        // Decode the UTF-8 sequence length.
        size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
        if (i + len > in.size()) break;
        const std::string seq = in.substr(i, len);
        if      (seq == "\xE2\x80\x94" || seq == "\xE2\x80\x93") out += " - "; // em/en dash
        else if (seq == "\xE2\x80\x99" || seq == "\xE2\x80\x98") out += '\'';  // curly single
        else if (seq == "\xE2\x80\x9C" || seq == "\xE2\x80\x9D") out += '"';   // curly double
        else if (seq == "\xE2\x80\xA6") out += "...";                          // ellipsis
        else if (seq == "\xC3\xA9") out += 'e';                                // é
        // anything else multi-byte: dropped (better absent than "???")
        i += len;
    }
    return out;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

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
// Cond / fx parsing from JValues (the closed spec-§3 vocabulary).
// ===========================================================================

namespace {

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

// Effect-kind axis names (fx {"karma": +n} etc.).
bool isAxisFxKey(const std::string& key) {
    return key == "karma" || key == "humanity" || key == "trust" ||
           key == "mercy" || key == "love"     || key == "redemption";
}

} // namespace

bool parseStoryCondList(const JValue* jv, std::vector<ChatCond>& out,
                        std::vector<std::string>& errors, const std::string& where) {
    if (!jv) return true;
    if (!jv->isArr()) { errors.push_back(where + ": `if` is not an array"); return false; }
    bool ok = true;
    for (const JValue& c : *jv->arr) {
        ChatCond cc;
        if (parseStoryCond(c, cc, errors, where)) out.push_back(std::move(cc));
        else ok = false;
    }
    return ok;
}

bool parseStoryCond(const JValue& jv, ChatCond& out, std::vector<std::string>& errors,
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
        return parseStoryCondList(&val, out.sub, errors, where + ".any");
    }
    if (key == "not") {
        out.kind = ChatCondKind::Not;
        ChatCond inner;
        if (!parseStoryCond(val, inner, errors, where + ".not")) return false;
        out.sub.push_back(std::move(inner));
        return true;
    }
    errors.push_back(where + ": unrecognized condition kind `" + key + "`");
    return false;
}

bool parseStoryFx(const JValue& jv, ChatFx& out, std::vector<std::string>& errors,
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

bool parseStoryFxList(const JValue* jv, std::vector<ChatFx>& out,
                      std::vector<std::string>& errors, const std::string& where) {
    if (!jv) return true;
    if (!jv->isArr()) { errors.push_back(where + ": `fx` is not an array"); return false; }
    bool ok = true;
    for (const JValue& f : *jv->arr) {
        ChatFx fx;
        if (parseStoryFx(f, fx, errors, where)) out.push_back(std::move(fx));
        else ok = false;
    }
    return ok;
}

// ===========================================================================
// Condition evaluation
// ===========================================================================

namespace {

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
    const std::string g = lowerAscii(girl);
    if (g == "aria")   { out = Woman::Aria;   return true; }
    if (g == "keisha") { out = Woman::Keisha; return true; }
    if (g == "emily")  { out = Woman::Emily;  return true; }
    return false;
}

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
                if (lowerAscii(t) == lowerAscii(cur)) return true;
            return false;
        }
        case ChatCondKind::GirlSaved: {
            Woman w;
            if (ctx.timeline && womanByName(c.s, w)) {
                const CaptiveFate f = ctx.timeline->fate(w);
                if (f == CaptiveFate::Saved || f == CaptiveFate::Cured) return true;
            }
            // Non-F2 girls (lena/...) — the glossary `<girl>.rescued` flag.
            return ctx.flags && ctx.flags->has(lowerAscii(c.s) + ".rescued");
        }
        case ChatCondKind::GirlLost: {
            Woman w;
            if (ctx.timeline && womanByName(c.s, w))
                return ctx.timeline->fate(w) == CaptiveFate::Lost;
            return ctx.flags && ctx.flags->has(lowerAscii(c.s) + ".lost");
        }
        case ChatCondKind::Item:   return ctx.flags && ctx.flags->hasItem(c.s);
        case ChatCondKind::RelGte: return ctx.flags && ctx.flags->rel(lowerAscii(c.s)) >= c.n;
        case ChatCondKind::Chance: return chanceRoll(ctx.chanceSeed, nodeKey) < c.f;
        case ChatCondKind::Lua:
            if (!ctx.luaCond) {
                x3::logWarn("[storyops] {\"lua\": \"" + c.s + "\"} with no luaCond hook -> false");
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
                        x3::logWarn("[storyops] follow effect: host rescue refused (" +
                                    std::string(npcId) + ")");
                } else {
                    x3::logWarn("[storyops] follow effect with no host hook (" +
                                std::string(npcId) + ")");
                }
                break;
            case ChatFxKind::Rel:  if (ctx.flags) ctx.flags->raiseRel(lowerAscii(f.s), f.n); break;
            case ChatFxKind::Ally: if (ctx.timeline) ctx.timeline->onAllyJoined(); break;
            case ChatFxKind::End:
                if (ctx.scripts)
                    ctx.scripts->fire("dialog_end", {{"npc", std::string(npcId)},
                                                     {"verb", f.s}});
                break;
        }
    }
}

} // namespace x3::game
