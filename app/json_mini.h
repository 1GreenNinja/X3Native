#pragma once
// Tiny shared JSON DOM (living-world pass). A header-only copy of the proven
// vehparts.cpp recursive-descent parser (subset: obj/arr/str/num/bool, tolerant
// of whitespace; unknown keys are simply never queried) so the new data-driven
// config loaders (assets/world/ecology.json, assets/world/alert.json) don't each
// re-embed their own. Game/slice code only — engine/ stays pure. No exceptions,
// no external dependency; a malformed document just reads as "ok == false" and
// the caller falls back to its built-in defaults.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace x3::game::jmini {

struct JVal {
    enum T { Null, Num, Str, Bool, Arr, Obj } t = Null;
    double num = 0.0;
    bool   b   = false;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    float fnum(const char* k, float def) const {
        const JVal* v = get(k); return (v && v->t == Num) ? (float)v->num : def;
    }
    int inum(const char* k, int def) const {
        const JVal* v = get(k); return (v && v->t == Num) ? (int)v->num : def;
    }
    bool bval(const char* k, bool def) const {
        const JVal* v = get(k); return (v && v->t == Bool) ? v->b : def;
    }
    std::string sval(const char* k, const std::string& def = "") const {
        const JVal* v = get(k); return (v && v->t == Str) ? v->str : def;
    }
};

struct JReader {
    const char* p; const char* e; bool ok = true;
    explicit JReader(const std::string& s) : p(s.c_str()), e(s.c_str() + s.size()) {}
    void ws() { while (p < e && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    bool eat(char c) { ws(); if (p < e && *p == c) { ++p; return true; } return false; }

    JVal parse() {
        JVal v; ws();
        if (p >= e) { ok = false; return v; }
        const char c = *p;
        if (c == '{') {
            ++p; v.t = JVal::Obj;
            ws();
            if (eat('}')) return v;
            while (ok) {
                ws();
                if (p >= e || *p != '"') { ok = false; break; }
                std::string key = parseStr();
                if (!eat(':')) { ok = false; break; }
                v.obj.emplace_back(std::move(key), parse());
                ws();
                if (eat(',')) continue;
                if (eat('}')) break;
                ok = false; break;
            }
        } else if (c == '[') {
            ++p; v.t = JVal::Arr;
            ws();
            if (eat(']')) return v;
            while (ok) {
                v.arr.push_back(parse());
                ws();
                if (eat(',')) continue;
                if (eat(']')) break;
                ok = false; break;
            }
        } else if (c == '"') {
            v.t = JVal::Str; v.str = parseStr();
        } else if (c == 't' || c == 'f') {
            v.t = JVal::Bool;
            if (e - p >= 4 && std::strncmp(p, "true", 4) == 0)  { v.b = true;  p += 4; }
            else if (e - p >= 5 && std::strncmp(p, "false", 5) == 0) { v.b = false; p += 5; }
            else ok = false;
        } else if (c == 'n') {
            if (e - p >= 4 && std::strncmp(p, "null", 4) == 0) p += 4; else ok = false;
        } else {
            v.t = JVal::Num;
            const char* s = p;
            while (p < e && (*p=='-'||*p=='+'||*p=='.'||*p=='e'||*p=='E'||(*p>='0'&&*p<='9'))) ++p;
            if (p == s) { ok = false; return v; }
            v.num = std::atof(std::string(s, p).c_str());
        }
        return v;
    }
    std::string parseStr() {
        std::string s;
        if (p >= e || *p != '"') { ok = false; return s; }
        ++p;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) { ++p; s += *p; }
            else s += *p;
            ++p;
        }
        if (p < e) ++p; else ok = false;
        return s;
    }
};

// Whole-file read ("" on a missing/unreadable file).
inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

} // namespace x3::game::jmini
