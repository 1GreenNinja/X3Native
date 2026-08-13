// vehcosmetics — the cosmetic layer. See vehcosmetics.h.
//                                                  [LANE: inspx/veh-cosmetics]
// The tiny JSON DOM below mirrors app/vehparts.cpp's (this repo's own code):
// both files parse the same parts.json and stay independently buildable.
// CLEAN-ROOM, original work; no other game or engine source consulted.

#include "vehcosmetics.h"

#include "engine/core/x3_log.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace x3::game::vehcosmetics {

// ===========================================================================
// Tiny JSON DOM (subset: obj/arr/str/num/bool) — see vehparts.cpp.
// ===========================================================================
namespace {

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

bool parseRGB(const JVal* v, float out[3]) {
    if (!v || v->t != JVal::Arr || v->arr.size() < 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (v->arr[i].t != JVal::Num) return false;
        out[i] = (float)v->arr[i].num;
    }
    return true;
}

} // namespace

// ===========================================================================
// CosmeticCatalog
// ===========================================================================
bool CosmeticCatalog::loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { x3::logError("[vehcos] catalog open failed: " + path); return false; }
    std::stringstream ss; ss << f.rdbuf();
    return loadJson(ss.str());
}

bool CosmeticCatalog::loadJson(const std::string& json) {
    m_ok = false;
    m_categories.clear();
    m_parts.clear();

    JReader r(json);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) {
        x3::logError("[vehcos] parts.json parse failed");
        return false;
    }
    if (root.sval("format") != "x3.vehparts/1") {
        x3::logError("[vehcos] unexpected format tag");
        return false;
    }
    const JVal* cos = root.get("cosmetic");
    if (!cos || cos->t != JVal::Arr) {
        // Older data without the block: valid, empty. Everything stays stock.
        m_ok = true;
        return true;
    }
    for (const JVal& c : cos->arr) {
        if (c.t != JVal::Obj) continue;
        CosCategory cat;
        cat.id = c.sval("id"); cat.label = c.sval("label"); cat.kind = c.sval("kind");
        if (cat.id.empty()) continue;
        m_categories.push_back(cat);
        const JVal* parts = c.get("parts");
        if (!parts || parts->t != JVal::Arr) continue;
        for (const JVal& pj : parts->arr) {
            if (pj.t != JVal::Obj) continue;
            CosPart p;
            p.id = pj.sval("id");
            p.category = cat.id;
            p.name  = pj.sval("name");
            p.tier  = pj.inum("tier", 1);
            p.price = pj.inum("price", 0);
            p.paintType      = pj.sval("paintType");
            p.clearcoat      = pj.fnum("clearcoat", 0.0f);
            p.clearcoatRough = pj.fnum("clearcoatRough", 0.05f);
            p.metallicScale  = pj.fnum("metallicScale", 1.0f);
            p.chrome         = pj.inum("chrome", 0) != 0;
            p.tintDark       = pj.fnum("tintDark", 0.0f);
            p.glowIntensity  = pj.fnum("glowIntensity", 0.0f);
            p.glowMode       = pj.sval("glowMode");
            p.hasRimColor    = parseRGB(pj.get("rimColor"), p.rimColor);
            p.rimMatchPaint  = pj.inum("rimMatchPaint", 0) != 0;
            p.rimMetallic    = pj.fnum("rimMetallic", 0.0f);
            if (!p.id.empty()) m_parts.push_back(std::move(p));
        }
    }
    m_ok = true;
    return true;
}

const CosPart* CosmeticCatalog::find(const std::string& id) const {
    for (const CosPart& p : m_parts) if (p.id == id) return &p;
    return nullptr;
}

std::vector<const CosPart*> CosmeticCatalog::inCategory(const std::string& cat) const {
    std::vector<const CosPart*> out;
    for (const CosPart& p : m_parts) if (p.category == cat) out.push_back(&p);
    return out;
}

// ===========================================================================
// CosmeticBuild
// ===========================================================================
const std::string* CosmeticBuild::installedIn(const std::string& category) const {
    for (const auto& kv : installed)
        if (kv.first == category && !kv.second.empty()) return &kv.second;
    return nullptr;
}
void CosmeticBuild::install(const std::string& category, const std::string& partId) {
    for (auto& kv : installed)
        if (kv.first == category) { kv.second = partId; return; }
    installed.emplace_back(category, partId);
}
void CosmeticBuild::removeFrom(const std::string& category) {
    for (auto& kv : installed)
        if (kv.first == category) { kv.second.clear(); return; }
}

std::string CosmeticBuild::toJson() const {
    std::ostringstream o;
    o << "{ \"format\": \"x3.vehlook/1\",\n  \"installed\": {";
    bool first = true;
    for (const auto& kv : installed) {
        if (kv.second.empty()) continue;
        if (!first) o << ",";
        o << "\n    \"" << kv.first << "\": \"" << kv.second << "\"";
        first = false;
    }
    o << "\n  },\n";
    o << "  \"paintRGB\": [" << paintRGB[0] << ", " << paintRGB[1] << ", " << paintRGB[2] << "],\n";
    o << "  \"underglowRGB\": [" << underglowRGB[0] << ", " << underglowRGB[1] << ", "
      << underglowRGB[2] << "]\n}\n";
    return o.str();
}

bool CosmeticBuild::fromJson(const std::string& json) {
    JReader r(json);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) return false;
    if (root.sval("format") != "x3.vehlook/1") return false;
    CosmeticBuild nb;   // parse into a fresh build; commit only on success
    const JVal* inst = root.get("installed");
    if (inst && inst->t == JVal::Obj)
        for (const auto& kv : inst->obj)
            if (kv.second.t == JVal::Str) nb.install(kv.first, kv.second.str);
    parseRGB(root.get("paintRGB"), nb.paintRGB);
    parseRGB(root.get("underglowRGB"), nb.underglowRGB);
    *this = std::move(nb);
    return true;
}

bool CosmeticBuild::saveFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string j = toJson();
    f.write(j.data(), (std::streamsize)j.size());
    return (bool)f;
}

bool CosmeticBuild::loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    return fromJson(ss.str());
}

std::string defaultLookSavePath() { return "vehlook.json"; }

// ===========================================================================
// composeVisual
// ===========================================================================
VehicleAppearance composeVisual(const CosmeticCatalog& cat, const CosmeticBuild& build) {
    VehicleAppearance a;
    auto part = [&](const char* category) -> const CosPart* {
        const std::string* id = build.installedIn(category);
        return id ? cat.find(*id) : nullptr;
    };
    if (const CosPart* p = part("paint")) {
        a.paintOn = true;
        if (p->chrome) {
            // Chrome ignores the picked color: bright metal base.
            a.paintRGB[0] = a.paintRGB[1] = 0.93f; a.paintRGB[2] = 0.95f;
        } else {
            a.paintRGB[0] = build.paintRGB[0];
            a.paintRGB[1] = build.paintRGB[1];
            a.paintRGB[2] = build.paintRGB[2];
        }
        a.clearcoat      = p->clearcoat;
        a.clearcoatRough = p->clearcoatRough;
        a.metallicScale  = p->metallicScale;
    }
    if (const CosPart* p = part("tint")) {
        a.tintOn   = p->tintDark > 0.0f;
        a.tintDark = p->tintDark;
    }
    if (const CosPart* p = part("lighting")) {
        if (p->glowIntensity > 0.0f) {
            a.glowOn = true;
            a.glowRGB[0] = build.underglowRGB[0];
            a.glowRGB[1] = build.underglowRGB[1];
            a.glowRGB[2] = build.underglowRGB[2];
            a.glowIntensity = p->glowIntensity;
            a.glowPulse = p->glowMode == "pulse";
        }
    }
    if (const CosPart* p = part("wheels")) {
        a.rimOn = true;
        if (p->rimMatchPaint) {
            a.rimRGB[0] = a.paintOn ? a.paintRGB[0] : build.paintRGB[0];
            a.rimRGB[1] = a.paintOn ? a.paintRGB[1] : build.paintRGB[1];
            a.rimRGB[2] = a.paintOn ? a.paintRGB[2] : build.paintRGB[2];
        } else if (p->hasRimColor) {
            a.rimRGB[0] = p->rimColor[0]; a.rimRGB[1] = p->rimColor[1]; a.rimRGB[2] = p->rimColor[2];
        } else {
            a.rimOn = false;
        }
        a.rimMetallic = p->rimMetallic;
    }
    return a;
}

// ===========================================================================
// Self-test (--test-vehcosmetics)
// ===========================================================================
bool runVehCosmeticsSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[vehcos-test] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[vehcos-test] FAIL ") + name); }
    };

    // C1 — the shipped catalog parses and routes fields per category.
    CosmeticCatalog cat;
    check(cat.loadFile("assets/vehicles/parts.json") && cat.ok(), "C1 catalog loads");
    check(cat.categories().size() == 4, "C1 four cosmetic categories (paint/tint/lighting/wheels)");
    check(cat.inCategory("paint").size() == 6, "C1 six paint tiers");
    {
        const CosPart* candy = cat.find("paint_candy");
        check(candy && candy->clearcoat >= 0.99f && candy->clearcoatRough <= 0.03f &&
              candy->tier == 4, "C1 candy paint: full deep clearcoat, tier 4");
        const CosPart* matte = cat.find("paint_matte");
        check(matte && matte->clearcoat == 0.0f && matte->metallicScale < 1.0f,
              "C1 matte: no clearcoat lobe");
        // Price ordering follows the spec: solid cheapest -> candy/pearl top.
        const CosPart* solid = cat.find("paint_solid");
        const CosPart* pearl = cat.find("paint_pearl");
        check(solid && pearl && candy && solid->price < pearl->price && pearl->price < candy->price,
              "C1 paint price ladder solid < pearl < candy");
        const CosPart* limo = cat.find("tint_limo");
        check(limo && limo->tintDark > 0.8f, "C1 limo tint darkest");
        const CosPart* glow = cat.find("glow_pulse");
        check(glow && glow->glowIntensity > 0.0f && glow->glowMode == "pulse",
              "C1 pulse underglow routed");
    }

    // C2 — composeVisual propagates paint + colors.
    {
        CosmeticBuild b;
        b.install("paint", "paint_candy");
        b.paintRGB[0] = 0.05f; b.paintRGB[1] = 0.30f; b.paintRGB[2] = 0.80f;
        b.install("tint", "tint_smoke");
        b.install("lighting", "glow_basic");
        b.underglowRGB[0] = 1.0f; b.underglowRGB[1] = 0.1f; b.underglowRGB[2] = 0.6f;
        b.install("wheels", "rim_match");
        VehicleAppearance a = composeVisual(cat, b);
        check(a.paintOn && a.clearcoat >= 0.99f && a.paintRGB[2] == 0.80f,
              "C2 candy paint + player RGB propagate");
        check(a.tintOn && a.tintDark > 0.6f && a.tintDark < 0.7f, "C2 smoke tint composes");
        check(a.glowOn && !a.glowPulse && a.glowRGB[0] == 1.0f, "C2 static glow + player color");
        check(a.rimOn && a.rimRGB[2] == 0.80f, "C2 color-match rims take the paint RGB");
        // Stock build: everything off — the authored look.
        VehicleAppearance s = composeVisual(cat, CosmeticBuild{});
        check(!s.paintOn && !s.tintOn && !s.glowOn && !s.rimOn,
              "C2 empty build composes to the authored (stock) look");
    }

    // C3 — JSON round-trip incl. player colors.
    {
        CosmeticBuild b;
        b.install("paint", "paint_metallic");
        b.install("tint", "tint_light");
        b.paintRGB[0] = 0.11f; b.paintRGB[1] = 0.22f; b.paintRGB[2] = 0.33f;
        b.underglowRGB[1] = 0.77f;
        const std::string j = b.toJson();
        CosmeticBuild b2;
        check(b2.fromJson(j), "C3 build JSON parses back");
        const std::string* paint = b2.installedIn("paint");
        check(paint && *paint == "paint_metallic" &&
              b2.paintRGB[2] == 0.33f && b2.underglowRGB[1] == 0.77f &&
              b2.installedIn("tint") != nullptr,
              "C3 round-trip preserves installed + colors");
    }

    // C4 — a parts.json WITHOUT the block: ok, empty (older data works).
    {
        CosmeticCatalog none;
        check(none.loadJson("{ \"format\": \"x3.vehparts/1\", \"categories\": [] }") &&
              none.ok() && none.parts().empty(),
              "C4 missing cosmetic block = valid empty catalog");
    }

    // C5 — NEGATIVE CONTROL: truncated JSON fails and leaves the build alone.
    {
        CosmeticBuild b;
        b.install("paint", "paint_solid");
        const bool loaded = b.fromJson("{ \"format\": \"x3.vehlook/1\", \"installed\": { \"paint\": ");
        const std::string* still = b.installedIn("paint");
        check(!loaded && still && *still == "paint_solid",
              "C5 negative control: truncated JSON rejected, build untouched");
        CosmeticCatalog badCat;
        check(!badCat.loadJson("{ \"format\": \"x3.vehparts/9\" }"),
              "C5 negative control: wrong format tag rejected");
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "[vehcos-test] %d/%d passed", passN, passN + failN);
    if (failN) x3::logError(buf); else x3::logInfo(buf);
    return failN == 0;
}

} // namespace x3::game::vehcosmetics
