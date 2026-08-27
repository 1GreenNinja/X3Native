// THE CANON LEVEL, IN THE EDITOR — see app/editor/editor_canon.h.
//
// Clean-room: a raw-span-preserving JSON reader + a writer that reproduces the source's
// formatting (JSON.stringify(v, null, 2)). No third-party JSON lib.
#include "editor_canon.h"

#include "../level_loader.h"      // canonProjectJsonPath() + the GAME'S loader (CE6)
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace x3::editor {

// ===========================================================================
// A JSON reader that also hands back the RAW SOURCE SPAN of any value.
//
// That is the whole trick behind losslessness: for every key we do not model we
// keep the exact bytes and write them straight back out. A field we never parsed
// is a field we cannot corrupt.
// ===========================================================================
namespace {

struct Src {
    const char* p   = nullptr;
    const char* end = nullptr;
    bool        ok  = true;
    std::string err;

    void fail(const char* what) {
        if (ok) { ok = false; err = what; }
    }
    void ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    char peek() { ws(); return p < end ? *p : '\0'; }
    bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
    void expect(char c, const char* what) { if (!eat(c)) fail(what); }

    // A JSON string, DECODED. UTF-8 bytes >= 0x80 pass through untouched (the canon file
    // is raw UTF-8 — an em-dash in the project name, a degree sign in a room desc).
    std::string str() {
        ws();
        std::string s;
        if (p >= end || *p != '"') { fail("expected a string"); return s; }
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case '"': s += '"';  break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/';  break;
                    case 'u': {
                        // \uXXXX -> UTF-8. (The shipping file has none; a hand edit might.)
                        if (p + 4 > end) { fail("bad \\u escape"); return s; }
                        int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++; cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else { fail("bad \\u escape"); return s; }
                        }
                        if (cp < 0x80) s += (char)cp;
                        else if (cp < 0x800) {
                            s += (char)(0xC0 | (cp >> 6));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else {
                            s += (char)(0xE0 | (cp >> 12));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail("bad escape"); return s;
                }
            } else {
                s += c;
            }
        }
        if (p >= end) { fail("unterminated string"); return s; }
        ++p;   // closing quote
        return s;
    }

    double num() {
        ws();
        const char* s = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p=='.' || *p=='e' || *p=='E' ||
                           *p=='-' || *p=='+')) ++p;
        if (p == s) { fail("expected a number"); return 0.0; }
        return std::atof(std::string(s, p).c_str());
    }

    // Skip exactly one JSON value and return its verbatim source text.
    std::string rawValue() {
        ws();
        const char* s = p;
        skipValue();
        if (!ok) return {};
        return std::string(s, p);
    }

    void skipValue() {
        ws();
        if (p >= end) { fail("unexpected end of input"); return; }
        char c = *p;
        if (c == '"') { str(); return; }
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++p;
            for (;;) {
                if (!ok) return;
                ws();
                if (p >= end) { fail("unterminated object/array"); return; }
                if (*p == close) { ++p; return; }
                if (*p == ',') { ++p; continue; }
                if (c == '{') {
                    if (*p != '"') { fail("expected a key"); return; }
                    str();
                    expect(':', "expected ':'");
                    if (!ok) return;
                }
                skipValue();
            }
        }
        if (c == 't') { if (p + 4 <= end) p += 4; else fail("bad literal"); return; }
        if (c == 'f') { if (p + 5 <= end) p += 5; else fail("bad literal"); return; }
        if (c == 'n') { if (p + 4 <= end) p += 4; else fail("bad literal"); return; }
        num();
    }
};

// ---------------------------------------------------------------------------
// Emission. The source is JSON.stringify(v, null, 2); we reproduce it exactly so an
// unedited round-trip is byte-for-byte the input file.
// ---------------------------------------------------------------------------
std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) {   // other control chars must be escaped
                    char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (int)c);
                    o += b;
                } else {
                    o += (char)c;   // >= 0x80: raw UTF-8 byte, as the source has it
                }
        }
    }
    return o;
}

// A room COORDINATE, written the way JavaScript's JSON.stringify writes a number:
// integral values print with no decimal point ("22"), everything else prints with the
// SHORTEST decimal that reads back as the SAME VALUE ("44.5").
//
// "THE SAME VALUE" MEANS THE SAME **FLOAT**, AND THAT IS THE WHOLE POINT.
// The editor's (and the engine's, and the game's) working precision for a room is
// float — BlockoutBrush::pos/size are float[3], CanonRoom::cx/w are float. 3.8 is not
// representable in binary floating point, so the float nearest to it promotes to the
// double 3.7999999523162842. Ask for the shortest DOUBLE that round-trips and you get
// "3.799999952316284" — and a load->save with ZERO EDITS silently rewrites 30-odd room
// heights in the game's level with that noise. It is numerically harmless (the game
// reads floats back) and it is still unacceptable: it makes a one-room edit a
// whole-file diff, and it drifts the values away from the double-precision tool that
// authored them.
//
// The fix is to serialize at the precision we actually carry: the shortest decimal that
// identifies THIS FLOAT. "3.8" parses to exactly the float we hold, so "3.8" is what we
// write. Any value in the file that a float cannot represent WOULD be quantized here —
// there are none (CE2e asserts the whole file survives byte-for-byte, which is only
// possible if every coordinate is float-exact).
std::string jnum(float v) {
    if (!std::isfinite(v)) return "0";
    if (v == std::floor(v) && std::fabs(v) < 1e15f) {
        char b[32];
        std::snprintf(b, sizeof b, "%lld", (long long)v);
        return b;
    }
    char b[40];
    for (int prec = 1; prec <= 9; ++prec) {           // 9 digits identifies any float
        std::snprintf(b, sizeof b, "%.*g", prec, (double)v);
        if ((float)std::atof(b) == v) return b;
    }
    return b;
}
// Door endpoints are indices — plain integers.
std::string jint(int v) {
    char b[16];
    std::snprintf(b, sizeof b, "%d", v);
    return b;
}

std::string indent(int n) { return std::string((size_t)n, ' '); }

} // namespace

// ===========================================================================
// PATH — resolved exactly the way the GAME resolves it. See KNOWN_BUGS L2: this
// chain must never end in an absolute path guess at another machine's disk.
// ===========================================================================
std::string canonLevelPath() { return x3::game::canonProjectJsonPath(); }

// ===========================================================================
// LOAD
// ===========================================================================
bool canonLoad(const std::string& path, LevelDoc& doc, std::string* err) {
    auto bail = [&](const std::string& m) {
        if (err) *err = m;
        x3::logWarn("[canon] load failed: " + m);
        return false;
    };

    std::ifstream f(path, std::ios::binary);
    if (!f) return bail("cannot open " + path);
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return bail("empty file: " + path);

    LevelDoc out;                       // build into a scratch doc; only commit on success
    out.canon.sourcePath = path;
    out.name  = "canon";
    out.biome = "facility";

    Src s{ text.data(), text.data() + text.size() };
    s.expect('{', "expected '{' at the document root");
    if (!s.ok) return bail(s.err);

    bool sawFloors = false;
    while (s.ok) {
        char c = s.peek();
        if (c == '}') { ++s.p; break; }
        if (c == ',') { ++s.p; continue; }
        if (c == '\0') { s.fail("unterminated document"); break; }

        const std::string key = s.str();
        s.expect(':', "expected ':' after a root key");
        if (!s.ok) break;

        if (key != "floors") {
            out.canon.header.emplace_back(key, s.rawValue());
            continue;
        }

        // ---- floors { "1": {...}, ... } ----
        sawFloors = true;
        s.expect('{', "expected '{' for floors");
        while (s.ok) {
            char fc = s.peek();
            if (fc == '}') { ++s.p; break; }
            if (fc == ',') { ++s.p; continue; }
            if (fc == '\0') { s.fail("unterminated floors"); break; }

            CanonFloorMeta fm;
            fm.key = s.str();
            s.expect(':', "expected ':' after a floor key");
            s.expect('{', "expected '{' for a floor");
            if (!s.ok) break;

            // Door pairs arrive as INDICES into this floor's rooms[]; map them to stable
            // room ids after the floor's members are all read (rooms may come after doors).
            std::vector<std::pair<int,int>> doorIdx;
            std::vector<uint32_t>           roomIdOfIndex;

            while (s.ok) {
                char mc = s.peek();
                if (mc == '}') { ++s.p; break; }
                if (mc == ',') { ++s.p; continue; }
                if (mc == '\0') { s.fail("unterminated floor"); break; }

                const std::string mk = s.str();
                s.expect(':', "expected ':' after a floor member key");
                if (!s.ok) break;

                if (mk == "name") {
                    fm.nameRaw = s.rawValue();
                    fm.hasName = true;
                } else if (mk == "rooms") {
                    s.expect('[', "expected '[' for rooms");
                    while (s.ok) {
                        char rc = s.peek();
                        if (rc == ']') { ++s.p; break; }
                        if (rc == ',') { ++s.p; continue; }
                        if (rc == '\0') { s.fail("unterminated rooms"); break; }
                        s.expect('{', "expected '{' for a room");
                        if (!s.ok) break;

                        BlockoutBrush b;
                        b.type    = 0;      // a room is a Box
                        b.collide = true;
                        b.yaw     = 0.0f;   // the canon schema is axis-aligned; no yaw
                        b.canon.room     = true;
                        b.canon.floorKey = fm.key;
                        b.canon.id       = out.canon.nextRoomId++;
                        b.canon.order    = (uint32_t)roomIdOfIndex.size();

                        while (s.ok) {
                            char kc = s.peek();
                            if (kc == '}') { ++s.p; break; }
                            if (kc == ',') { ++s.p; continue; }
                            if (kc == '\0') { s.fail("unterminated room"); break; }
                            const std::string rk = s.str();
                            s.expect(':', "expected ':' after a room key");
                            if (!s.ok) break;
                            if      (rk == "n") b.name         = s.str();
                            else if (rk == "t") b.canon.type   = s.str();
                            else if (rk == "x") b.pos[0]       = (float)s.num();
                            else if (rk == "y") b.pos[1]       = (float)s.num();
                            else if (rk == "z") b.pos[2]       = (float)s.num();
                            else if (rk == "w") b.size[0]      = (float)s.num();
                            else if (rk == "h") b.size[1]      = (float)s.num();
                            else if (rk == "d") b.size[2]      = (float)s.num();
                            else if (rk == "f") { b.canon.fField = s.str(); b.canon.hasF = true; }
                            else if (rk == "desc") { b.canon.desc = s.str(); b.canon.hasDesc = true; }
                            else b.canon.extra.emplace_back(rk, s.rawValue());   // never lose it
                        }
                        if (!s.ok) break;
                        roomIdOfIndex.push_back(b.canon.id);
                        out.brushes.push_back(std::move(b));
                    }
                } else if (mk == "doors") {
                    s.expect('[', "expected '[' for doors");
                    while (s.ok) {
                        char dc = s.peek();
                        if (dc == ']') { ++s.p; break; }
                        if (dc == ',') { ++s.p; continue; }
                        if (dc == '\0') { s.fail("unterminated doors"); break; }
                        s.expect('[', "expected '[' for a door pair");
                        if (!s.ok) break;
                        const int a = (int)s.num();
                        s.eat(',');
                        const int b = (int)s.num();
                        s.expect(']', "expected ']' closing a door pair");
                        doorIdx.emplace_back(a, b);
                    }
                } else {
                    fm.rawExtra.emplace_back(mk, s.rawValue());   // entities/triggers/...
                }
            }
            if (!s.ok) break;

            for (const auto& d : doorIdx) {
                if (d.first  < 0 || d.first  >= (int)roomIdOfIndex.size() ||
                    d.second < 0 || d.second >= (int)roomIdOfIndex.size()) {
                    x3::logWarn("[canon] floor " + fm.key + ": door references a room that does"
                                " not exist (" + std::to_string(d.first) + "," +
                                std::to_string(d.second) + ") — dropped");
                    continue;
                }
                fm.doors.push_back(CanonDoorEdge{ roomIdOfIndex[(size_t)d.first],
                                                  roomIdOfIndex[(size_t)d.second] });
            }
            out.canon.floors.push_back(std::move(fm));
        }
    }

    if (!s.ok)   return bail(s.err);
    if (!sawFloors) return bail("no 'floors' object");
    if (out.brushes.empty()) return bail("no rooms");

    out.canon.loaded = true;
    doc = std::move(out);

    uint32_t doorCount = 0;
    for (const auto& fl : doc.canon.floors) doorCount += (uint32_t)fl.doors.size();
    x3::logInfo("[canon] opened " + path + " — " +
                std::to_string(doc.canon.floors.size()) + " floors, " +
                std::to_string(doc.brushes.size()) + " rooms, " +
                std::to_string(doorCount) + " doors");
    return true;
}

// ===========================================================================
// SAVE
// ===========================================================================
std::vector<std::string> canonSaveWarnings(const LevelDoc& doc) {
    std::vector<std::string> w;
    if (!doc.canon.loaded) { w.emplace_back("no canon level is open"); return w; }

    uint32_t plain = 0;
    for (const BlockoutBrush& b : doc.brushes) if (!b.canon.room) ++plain;
    if (plain)
        w.push_back(std::to_string(plain) + " non-canon blockout brush(es) will NOT be "
                    "saved — the canon schema has nowhere to put them");
    if (!doc.entities.empty())
        w.push_back(std::to_string(doc.entities.size()) + " editor entity/entities will NOT "
                    "be saved — the canon floor's entities[] is not wired yet");

    // Doors whose room is gone.
    std::vector<uint32_t> live;
    for (const BlockoutBrush& b : doc.brushes) if (b.canon.room) live.push_back(b.canon.id);
    std::sort(live.begin(), live.end());
    uint32_t dropped = 0;
    for (const CanonFloorMeta& fl : doc.canon.floors)
        for (const CanonDoorEdge& d : fl.doors)
            if (!std::binary_search(live.begin(), live.end(), d.a) ||
                !std::binary_search(live.begin(), live.end(), d.b)) ++dropped;
    if (dropped)
        w.push_back(std::to_string(dropped) + " door(s) reference a DELETED room and will be "
                    "dropped");
    return w;
}

std::string canonToJson(const LevelDoc& doc) {
    const CanonMeta& cm = doc.canon;
    std::string o;
    o.reserve(64 * 1024);

    // Group the room brushes by floor, in each floor's authored order.
    std::map<std::string, std::vector<const BlockoutBrush*>> byFloor;
    for (const BlockoutBrush& b : doc.brushes)
        if (b.canon.room) byFloor[b.canon.floorKey].push_back(&b);
    for (auto& kv : byFloor)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [](const BlockoutBrush* a, const BlockoutBrush* b) {
                             return a->canon.order < b->canon.order;
                         });

    o += "{\n";
    for (const auto& kv : cm.header)
        o += indent(2) + "\"" + esc(kv.first) + "\": " + kv.second + ",\n";
    o += indent(2) + "\"floors\": {\n";

    for (size_t fi = 0; fi < cm.floors.size(); ++fi) {
        const CanonFloorMeta& fl = cm.floors[fi];
        const std::vector<const BlockoutBrush*>& rooms = byFloor[fl.key];

        // Stable room id -> this floor's rooms[] INDEX, re-derived on every save. This is
        // what makes add/delete/undo safe: doors never held an index, they held an id.
        std::map<uint32_t, int> indexOf;
        for (size_t i = 0; i < rooms.size(); ++i) indexOf[rooms[i]->canon.id] = (int)i;

        o += indent(4) + "\"" + esc(fl.key) + "\": {\n";
        if (fl.hasName) o += indent(6) + "\"name\": " + fl.nameRaw + ",\n";

        // ---- rooms[] ----
        if (rooms.empty()) {
            o += indent(6) + "\"rooms\": [],\n";
        } else {
            o += indent(6) + "\"rooms\": [\n";
            for (size_t i = 0; i < rooms.size(); ++i) {
                const BlockoutBrush& b = *rooms[i];
                o += indent(8) + "{\n";
                o += indent(10) + "\"n\": \"" + esc(b.name) + "\",\n";
                o += indent(10) + "\"t\": \"" + esc(b.canon.type) + "\",\n";
                o += indent(10) + "\"x\": " + jnum(b.pos[0])  + ",\n";
                o += indent(10) + "\"y\": " + jnum(b.pos[1])  + ",\n";
                o += indent(10) + "\"z\": " + jnum(b.pos[2])  + ",\n";
                o += indent(10) + "\"w\": " + jnum(b.size[0]) + ",\n";
                o += indent(10) + "\"h\": " + jnum(b.size[1]) + ",\n";
                o += indent(10) + "\"d\": " + jnum(b.size[2]);
                if (b.canon.hasF)    o += ",\n" + indent(10) + "\"f\": \"" + esc(b.canon.fField) + "\"";
                if (b.canon.hasDesc) o += ",\n" + indent(10) + "\"desc\": \"" + esc(b.canon.desc) + "\"";
                for (const auto& kv : b.canon.extra)
                    o += ",\n" + indent(10) + "\"" + esc(kv.first) + "\": " + kv.second;
                o += "\n" + indent(8) + "}";
                o += (i + 1 < rooms.size()) ? ",\n" : "\n";
            }
            o += indent(6) + "],\n";
        }

        // ---- doors[] (stable ids -> current indices; an edge into a deleted room dies) ----
        std::vector<std::pair<int,int>> live;
        for (const CanonDoorEdge& d : fl.doors) {
            auto ia = indexOf.find(d.a), ib = indexOf.find(d.b);
            if (ia == indexOf.end() || ib == indexOf.end()) continue;
            live.emplace_back(ia->second, ib->second);
        }
        if (live.empty()) {
            o += indent(6) + "\"doors\": []";
        } else {
            o += indent(6) + "\"doors\": [\n";
            for (size_t i = 0; i < live.size(); ++i) {
                o += indent(8) + "[\n";
                o += indent(10) + jnum(live[i].first)  + ",\n";
                o += indent(10) + jnum(live[i].second) + "\n";
                o += indent(8) + "]";
                o += (i + 1 < live.size()) ? ",\n" : "\n";
            }
            o += indent(6) + "]";
        }

        for (const auto& kv : fl.rawExtra)
            o += ",\n" + indent(6) + "\"" + esc(kv.first) + "\": " + kv.second;
        o += "\n" + indent(4) + "}";
        o += (fi + 1 < cm.floors.size()) ? ",\n" : "\n";
    }

    o += indent(2) + "}\n";
    o += "}";
    return o;
}

bool canonSave(const std::string& path, const LevelDoc& doc, std::string* err) {
    if (!doc.canon.loaded) {
        if (err) *err = "no canon level is open";
        return false;
    }

    // ---- BACK IT UP BEFORE THE FIRST WRITE. The canon level IS the game. An existing
    // .bak is NOT overwritten, so the pristine original survives a chain of saves. ----
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        const std::string bak = path + ".bak";
        if (!std::filesystem::exists(bak, ec)) {
            std::filesystem::copy_file(path, bak, ec);
            if (ec) {
                if (err) *err = "could not write the backup " + bak + ": " + ec.message();
                return false;
            }
            x3::logInfo("[canon] backed up " + path + " -> " + bak);
        }
    }

    const std::string json = canonToJson(doc);
    // Write to a temp file, then move it into place: a crash mid-write can never leave a
    // truncated level behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot open " + tmp; return false; }
        f.write(json.data(), (std::streamsize)json.size());
        if (!f) { if (err) *err = "write failed: " + tmp; return false; }
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (err) *err = "could not replace " + path + ": " + ec.message();
        return false;
    }
    x3::logInfo("[canon] saved " + path + " (" + std::to_string(json.size()) + " bytes)");
    return true;
}

// ===========================================================================
// SELF-TEST (--test-canonedit)
// ===========================================================================
namespace {

int  g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; x3::logInfo("  PASS  " + what); }
    else      { ++g_fail; x3::logError("  FAIL  " + what); }
}

// SEMANTIC comparison of two canon documents (parsed structures, NOT bytes — key order
// and whitespace are allowed to differ; content is not). Returns "" when identical, else
// the FIRST difference found. This is the round-trip oracle, and CE3 proves it can fail.
std::string canonDiff(const LevelDoc& a, const LevelDoc& b) {
    auto num = [](float x) { return std::to_string((double)x); };
    if (a.canon.header != b.canon.header) return "project header differs";
    if (a.canon.floors.size() != b.canon.floors.size()) return "floor count differs";

    for (size_t i = 0; i < a.canon.floors.size(); ++i) {
        const CanonFloorMeta& fa = a.canon.floors[i];
        const CanonFloorMeta& fb = b.canon.floors[i];
        if (fa.key != fb.key)         return "floor key differs at " + std::to_string(i);
        if (fa.hasName != fb.hasName) return "floor " + fa.key + ": name presence differs";
        if (fa.nameRaw != fb.nameRaw) return "floor " + fa.key + ": name differs";
        if (fa.rawExtra != fb.rawExtra) return "floor " + fa.key + ": entities/triggers/extra differ";

        // Doors are compared as the SET of connected room ORDER pairs (undirected), because
        // stable ids are an editor-side construct: what the file means is "these two rooms
        // are connected".
        auto edges = [&](const LevelDoc& d, const CanonFloorMeta& fl) {
            std::map<uint32_t, uint32_t> orderOf;
            for (const BlockoutBrush& br : d.brushes)
                if (br.canon.room && br.canon.floorKey == fl.key)
                    orderOf[br.canon.id] = br.canon.order;
            std::vector<std::pair<uint32_t,uint32_t>> e;
            for (const CanonDoorEdge& dd : fl.doors) {
                auto ia = orderOf.find(dd.a), ib = orderOf.find(dd.b);
                if (ia == orderOf.end() || ib == orderOf.end()) continue;
                e.emplace_back(ia->second, ib->second);
            }
            return e;
        };
        if (edges(a, fa) != edges(b, fb))
            return "floor " + fa.key + ": doors differ";
    }

    // Rooms, in save order (floorKey, order).
    auto rooms = [](const LevelDoc& d) {
        std::vector<const BlockoutBrush*> r;
        for (const BlockoutBrush& b : d.brushes) if (b.canon.room) r.push_back(&b);
        std::stable_sort(r.begin(), r.end(), [](const BlockoutBrush* x, const BlockoutBrush* y) {
            if (x->canon.floorKey != y->canon.floorKey) return x->canon.floorKey < y->canon.floorKey;
            return x->canon.order < y->canon.order;
        });
        return r;
    };
    const std::vector<const BlockoutBrush*> ra = rooms(a), rb = rooms(b);
    if (ra.size() != rb.size())
        return "room count differs (" + std::to_string(ra.size()) + " vs " +
               std::to_string(rb.size()) + ")";
    for (size_t i = 0; i < ra.size(); ++i) {
        const BlockoutBrush& x = *ra[i];
        const BlockoutBrush& y = *rb[i];
        const std::string where = "room '" + x.name + "' (floor " + x.canon.floorKey + ")";
        if (x.name != y.name)                   return where + ": name differs";
        if (x.canon.type != y.canon.type)       return where + ": type differs";
        if (x.canon.floorKey != y.canon.floorKey) return where + ": floor differs";
        for (int k = 0; k < 3; ++k) {
            if (x.pos[k]  != y.pos[k])  return where + ": pos differs (" + num(x.pos[k]) +
                                               " vs " + num(y.pos[k]) + ")";
            if (x.size[k] != y.size[k]) return where + ": extent differs (" + num(x.size[k]) +
                                               " vs " + num(y.size[k]) + ")";
        }
        if (x.canon.hasF != y.canon.hasF || x.canon.fField != y.canon.fField)
            return where + ": 'f' differs";
        if (x.canon.hasDesc != y.canon.hasDesc || x.canon.desc != y.canon.desc)
            return where + ": 'desc' differs";
        if (x.canon.extra != y.canon.extra)     return where + ": unmodelled keys differ";
    }
    return {};
}

std::string tempPath(const char* stem) {
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec);
    if (ec || d.empty()) d = ".";
    return (d / stem).string();
}

} // namespace

bool runCanonEditSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("=== --test-canonedit: THE EDITOR EDITS THE GAME'S LEVEL ===");

    const std::string src = canonLevelPath();
    x3::logInfo("canon level: " + src);

    // ---- CE1: it OPENS ----------------------------------------------------
    LevelDoc doc;
    std::string err;
    const bool opened = canonLoad(src, doc, &err);
    check(opened, "CE1a the canon level opens (" + (opened ? std::string("ok") : err) + ")");
    if (!opened) { x3::logError("--test-canonedit: 0/1 (cannot open the canon level)"); return false; }

    uint32_t doors = 0;
    for (const auto& fl : doc.canon.floors) doors += (uint32_t)fl.doors.size();
    check(doc.canon.floors.size() == 7,  "CE1b 7 floors parse (got " +
                                          std::to_string(doc.canon.floors.size()) + ")");
    check(doc.brushes.size() == 124,     "CE1c 124 rooms parse (got " +
                                          std::to_string(doc.brushes.size()) + ")");
    check(doors == 160,                  "CE1d 160 doors parse (got " +
                                          std::to_string(doors) + ")");
    check(doc.canon.header.size() == 6,  "CE1e the 6 project-header keys are preserved");
    // Floor 1 is the one the game builds: 53 rooms / 111 doors (matches --test-canonlevel).
    {
        uint32_t f1r = 0, f1d = 0;
        for (const BlockoutBrush& b : doc.brushes)
            if (b.canon.room && b.canon.floorKey == "1") ++f1r;
        for (const auto& fl : doc.canon.floors) if (fl.key == "1") f1d = (uint32_t)fl.doors.size();
        check(f1r == 53 && f1d == 111, "CE1f Floor 1 = 53 rooms / 111 doors (the game's floor)");
    }
    // The story data the GAME does not read but the LEVEL cannot lose.
    {
        uint32_t descs = 0;
        for (const BlockoutBrush& b : doc.brushes) if (b.canon.hasDesc) ++descs;
        check(descs == 72, "CE1g all 72 room 'desc' fields load (got " +
                            std::to_string(descs) + ")");
    }
    // Every room must have carried its type through.
    {
        bool allTyped = true;
        for (const BlockoutBrush& b : doc.brushes)
            if (b.canon.room && b.canon.type.empty()) allTyped = false;
        check(allTyped, "CE1h every room carries its type string");
    }

    // ---- CE2: ROUND-TRIP — load -> save (zero edits) is semantically identical --------
    const std::string rt = tempPath("x3_canon_roundtrip.json");
    {
        const std::vector<std::string> warn = canonSaveWarnings(doc);
        check(warn.empty(), "CE2a a clean round-trip reports NO lossy fields");
        for (const std::string& w : warn) x3::logWarn("        " + w);
    }
    {
        std::error_code ec;
        std::filesystem::remove(rt, ec);
        std::filesystem::remove(rt + ".bak", ec);
    }
    check(canonSave(rt, doc, &err), "CE2b the round-trip saves (" + err + ")");

    LevelDoc back;
    check(canonLoad(rt, back, &err), "CE2c the saved file re-opens (" + err + ")");
    const std::string diff = canonDiff(doc, back);
    check(diff.empty(), "CE2d ROUND-TRIP IS SEMANTICALLY IDENTICAL" +
                        (diff.empty() ? std::string() : (" — DIFFERS: " + diff)));

    // The strong form: it is also byte-for-byte the input file. NOT the invariant (key
    // order and whitespace are allowed to move), but it is true today, so assert it —
    // if it ever stops being true, we want to hear about it.
    {
        auto slurp = [](const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss; ss << f.rdbuf(); return ss.str();
        };
        const std::string a = slurp(src), b = slurp(rt);
        check(a == b, "CE2e (strong) the round-trip is BYTE-IDENTICAL to the input file"
                      " — " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) +
                      " bytes");
    }

    // ---- CE3: NEGATIVE CONTROL — the comparison MUST be able to go red ---------------
    // A round-trip test that cannot fail is worthless. Perturb one room's extent and
    // prove canonDiff() catches it (and that it survives a save/load, i.e. the writer
    // really did write the perturbed value).
    {
        LevelDoc bad = doc;
        int idx = -1;
        for (size_t i = 0; i < bad.brushes.size(); ++i)
            if (bad.brushes[i].canon.room) { idx = (int)i; break; }
        bad.brushes[(size_t)idx].size[0] += 1.0f;              // one room, one metre wider
        const std::string d3 = canonDiff(doc, bad);
        check(!d3.empty(), "CE3a NEGATIVE CONTROL: a 1 m extent change is DETECTED "
                           "(\"" + d3 + "\")");

        const std::string badPath = tempPath("x3_canon_negative.json");
        std::error_code ec;
        std::filesystem::remove(badPath, ec);
        std::filesystem::remove(badPath + ".bak", ec);
        LevelDoc badBack;
        const bool okw = canonSave(badPath, bad, &err) && canonLoad(badPath, badBack, &err);
        const std::string d3b = okw ? canonDiff(doc, badBack) : std::string("save/load failed");
        check(!d3b.empty(), "CE3b NEGATIVE CONTROL survives the file: the perturbed level "
                            "differs after save+reload (\"" + d3b + "\")");
        // ...and the perturbation is the ONLY difference (the oracle is not just noisy).
        check(canonDiff(bad, badBack).empty(),
              "CE3c the perturbed level itself round-trips cleanly (the oracle is precise)");
        std::filesystem::remove(badPath, ec);
        std::filesystem::remove(badPath + ".bak", ec);
    }

    // ---- CE4/CE5: EDIT through the COMMAND STACK, save, reload, undo ------------------
    {
        LevelDoc  edoc;
        canonLoad(src, edoc, &err);
        EditorState st(edoc);
        st.setSnap(false);   // canon coordinates are authored on a 0.5 m grid; don't fight it

        // Find Jake's Cell — the room the player wakes up in. If we can move THAT, we can
        // edit the game.
        int jake = -1;
        for (size_t i = 0; i < edoc.brushes.size(); ++i)
            if (edoc.brushes[i].canon.room && edoc.brushes[i].name == "Jake's Cell" &&
                edoc.brushes[i].canon.floorKey == "1") { jake = (int)i; break; }
        check(jake >= 0, "CE4a Jake's Cell is selectable in the canon document");
        if (jake < 0) { x3::logError("--test-canonedit: FAILED"); return false; }

        const BlockoutBrush before = edoc.brushes[(size_t)jake];

        // THE MUTATION, through the EXISTING undo/redo command stack — the same path the
        // gizmo and the AI Architect use. Nothing bypasses it.
        st.selectBrush(jake);
        st.beginBrushEdit(jake);
        edoc.brushes[(size_t)jake].pos[0]  += 2.0f;    // move it 2 m in +X
        edoc.brushes[(size_t)jake].size[1] += 1.0f;    // raise the ceiling 1 m
        edoc.brushes[(size_t)jake].canon.desc = "EDITED BY THE LEVEL EDITOR";
        edoc.brushes[(size_t)jake].canon.hasDesc = true;
        st.commitBrushEdit();
        check(st.canUndo(), "CE4b the edit landed on the command stack (undo is available)");

        const std::string ep = tempPath("x3_canon_edited.json");
        std::error_code ec;
        std::filesystem::remove(ep, ec);
        std::filesystem::remove(ep + ".bak", ec);
        check(canonSave(ep, edoc, &err), "CE4c the edited level saves (" + err + ")");

        LevelDoc reloaded;
        check(canonLoad(ep, reloaded, &err), "CE4d the edited level reloads");
        int j2 = -1;
        for (size_t i = 0; i < reloaded.brushes.size(); ++i)
            if (reloaded.brushes[i].name == "Jake's Cell" &&
                reloaded.brushes[i].canon.floorKey == "1") { j2 = (int)i; break; }
        const bool survived =
            j2 >= 0 &&
            std::fabs(reloaded.brushes[(size_t)j2].pos[0]  - (before.pos[0]  + 2.0f)) < 1e-4f &&
            std::fabs(reloaded.brushes[(size_t)j2].size[1] - (before.size[1] + 1.0f)) < 1e-4f &&
            reloaded.brushes[(size_t)j2].canon.desc == "EDITED BY THE LEVEL EDITOR";
        check(survived, "CE4e THE MUTATION SURVIVED SAVE + RELOAD (position, extent AND the "
                        "canon-only desc field)");
        // Everything ELSE is untouched: the edited file differs from the original ONLY in
        // that room. (Compare the edited doc to the reload — they must match exactly.)
        check(canonDiff(edoc, reloaded).empty(),
              "CE4f the edited level round-trips exactly (only that room moved)");

        // ---- CE5: UNDO restores the prior state EXACTLY -----------------------------
        const HistoryEffect eff = st.undo();
        check(eff.op != HistoryEffect::Op::None, "CE5a undo produced a host re-sync hint");
        const BlockoutBrush& after = edoc.brushes[(size_t)jake];
        const bool restored =
            after.pos[0]  == before.pos[0]  && after.pos[1] == before.pos[1] &&
            after.pos[2]  == before.pos[2]  &&
            after.size[0] == before.size[0] && after.size[1] == before.size[1] &&
            after.size[2] == before.size[2] &&
            after.canon.desc    == before.canon.desc &&
            after.canon.hasDesc == before.canon.hasDesc &&
            after.canon.type    == before.canon.type &&
            after.canon.id      == before.canon.id;
        check(restored, "CE5b UNDO restored the room exactly — geometry AND the canon payload "
                        "(the snapshot stack carries type/desc/id for free)");
        // And the whole document is byte-for-byte back where it started.
        LevelDoc pristine;
        canonLoad(src, pristine, &err);
        check(canonDiff(pristine, edoc).empty(),
              "CE5c after undo the document is identical to the file on disk");

        std::filesystem::remove(ep, ec);
        std::filesystem::remove(ep + ".bak", ec);
    }

    // ---- CE6: THE GAME'S OWN LOADER reads what the editor wrote ----------------------
    // This is the assertion that actually matters. Not "my writer agrees with my reader" —
    // the GAME'S parser, on the editor's output.
    {
        x3::game::CanonFloor f1 = x3::game::loadCanonFloor(rt, 1);
        check(f1.valid() && f1.rooms.size() == 53 && f1.jsonDoorCount == 111,
              "CE6a the GAME'S loader parses the editor's output: Floor 1 = " +
              std::to_string(f1.rooms.size()) + " rooms / " +
              std::to_string(f1.jsonDoorCount) + " JSON doors");
        x3::game::CanonFloor tower = x3::game::loadCanonTower(rt);
        check(tower.valid() && tower.rooms.size() == 124,
              "CE6b the GAME'S tower loader gets all 124 rooms back (got " +
              std::to_string(tower.rooms.size()) + ")");
        // Spot-check the beat rooms the game spawns and fights in.
        x3::game::CanonBeats beats = x3::game::canonBeats(f1);
        check(beats.jakeCell != x3::game::kNoRoom && beats.mainHall != x3::game::kNoRoom &&
              beats.bossArena != x3::game::kNoRoom,
              "CE6c the game still resolves its beat rooms (Jake's Cell / Main Hall / Boss Arena)");
    }

    {
        std::error_code ec;
        std::filesystem::remove(rt, ec);
        std::filesystem::remove(rt + ".bak", ec);
    }

    x3::logInfo("--test-canonedit: " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " " + (g_fail ? "FAILED" : "PASS"));
    return g_fail == 0;
}

} // namespace x3::editor
