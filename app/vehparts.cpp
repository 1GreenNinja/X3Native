// PERFORMANCE PARTS (x3.vehparts/1) — catalog parse, build composition, dyno
// knock model, persistence, and the --test-vehparts self-test. See vehparts.h +
// docs/design/VEHPARTS_FORMAT.md.
//
// Clean-room: C++ standard library + the engine's own public physics interfaces
// only. The JSON reader below is a focused little recursive-descent DOM for the
// subset the catalog/build files use (objects/arrays/strings/numbers/bools) —
// the same "focused subset parser" approach as editor.cpp's LevelDoc reader.
#include "vehparts.h"
#include "vehicle.h"        // DriveDemo (the physics-delta self-test harness)
#include "asset_root.h"     // assetRoot() -> assets/vehicles/parts.json
#include "mesh_prims.h"     // the self-test ground slab

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace x3::game::vehparts {

// ===========================================================================
// Tiny JSON DOM (subset: obj/arr/str/num/bool). Tolerant of whitespace; unknown
// keys are simply never queried (forward-compatible per the format doc).
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

std::vector<CurvePt> parseCurve(const JVal* v) {
    std::vector<CurvePt> out;
    if (!v || v->t != JVal::Arr) return out;
    for (const JVal& pt : v->arr) {
        if (pt.t == JVal::Arr && pt.arr.size() >= 2 &&
            pt.arr[0].t == JVal::Num && pt.arr[1].t == JVal::Num)
            out.push_back({ (float)pt.arr[0].num, (float)pt.arr[1].num });
    }
    return out;
}

// Piecewise-linear sample of a CurvePt list (ascending x; clamped ends).
float sampleCurve(const std::vector<CurvePt>& c, float x) {
    if (c.empty()) return 1.0f;
    if (x <= c.front().x) return c.front().y;
    if (x >= c.back().x)  return c.back().y;
    for (size_t i = 1; i < c.size(); ++i) {
        if (x <= c[i].x) {
            const float t = (x - c[i-1].x) / std::max(1e-6f, c[i].x - c[i-1].x);
            return c[i-1].y + t * (c[i].y - c[i-1].y);
        }
    }
    return c.back().y;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

constexpr const char* kFormatTag = "x3.vehparts/1";
constexpr float kDamagePowerScale = 0.85f;   // popped engine torque penalty

} // namespace

// ===========================================================================
// Catalog
// ===========================================================================
std::string defaultCatalogPath() { return assetRoot() + "/vehicles/parts.json"; }

bool Catalog::loadFile(const std::string& path) {
    const std::string js = readFile(path);
    if (js.empty()) {
        x3::logError("[vehparts] catalog file missing/unreadable: " + path);
        return false;
    }
    return loadJson(js);
}

bool Catalog::loadJson(const std::string& json) {
    m_ok = false;
    m_categories.clear(); m_parts.clear(); m_base = Baseline{};
    JReader r(json);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) { x3::logError("[vehparts] catalog JSON parse failed"); return false; }
    if (root.sval("format") != kFormatTag) {
        x3::logError("[vehparts] catalog format tag != " + std::string(kFormatTag));
        return false;
    }
    if (const JVal* b = root.get("baseline")) {
        m_base.torqueNm    = b->fnum("torqueNm", 700.0f);
        m_base.maxRpm      = b->fnum("maxRpm", 6500.0f);
        m_base.massKg      = b->fnum("massKg", 1300.0f);
        m_base.brakeTorque = b->fnum("brakeTorque", 2200.0f);
        m_base.suspFreq    = b->fnum("suspFreq", 2.2f);
        m_base.suspDamp    = b->fnum("suspDamp", 0.7f);
        m_base.curve       = parseCurve(b->get("curve"));
    }
    const JVal* cats = root.get("categories");
    if (!cats || cats->t != JVal::Arr) { x3::logError("[vehparts] catalog has no categories[]"); return false; }
    for (const JVal& c : cats->arr) {
        Category cat;
        cat.id    = c.sval("id");
        cat.label = c.sval("label", cat.id);
        if (cat.id.empty()) continue;
        m_categories.push_back(cat);
        const JVal* parts = c.get("parts");
        if (!parts || parts->t != JVal::Arr) continue;
        for (const JVal& pj : parts->arr) {
            Part p;
            p.id       = pj.sval("id");
            p.category = cat.id;
            p.name     = pj.sval("name", p.id);
            p.tier     = pj.inum("tier", 1);
            p.price    = pj.inum("price", 0);
            p.camCurve = parseCurve(pj.get("curve"));
            p.redlineBonus   = pj.fnum("redlineBonus", 0.0f);
            p.powerPct       = pj.fnum("powerPct", 0.0f);
            p.noteId         = pj.inum("noteId", 0);
            p.pitchOffset    = pj.fnum("pitchOffset", 0.0f);
            p.timbre         = pj.fnum("timbre", 0.0f);
            p.safeBoostBonus = pj.fnum("safeBoostBonus", 0.0f);
            p.fiType         = pj.sval("fiType");
            p.boostPowerPct  = pj.fnum("boostPowerPct", 0.0f);
            p.spoolLagS      = pj.fnum("spoolLagS", 0.0f);
            p.topEndBias     = pj.fnum("topEndBias", 0.0f);
            p.whine          = pj.inum("whine", 0) != 0;
            p.whistle        = pj.inum("whistle", 0) != 0;
            p.maxBoost       = pj.fnum("maxBoost", 0.0f);
            p.safeBoost      = pj.fnum("safeBoost", 0.0f);
            p.safeLean       = pj.fnum("safeLean", 0.0f);
            p.safeTiming     = pj.fnum("safeTiming", 0.0f);
            p.knockLimit     = pj.fnum("knockLimit", 0.0f);
            p.powerPerBoost  = pj.fnum("powerPerBoost", 0.0f);
            p.powerPerTiming = pj.fnum("powerPerTiming", 0.0f);
            p.leanPowerPct   = pj.fnum("leanPowerPct", 0.0f);
            p.repairCost     = pj.inum("repairCost", 0);
            p.gripScale      = pj.fnum("gripScale", 0.0f);
            p.compound       = pj.sval("compound");
            p.rideHeightDelta= pj.fnum("rideHeightDelta", 0.0f);
            p.suspFreq       = pj.fnum("suspFreq", 0.0f);
            p.suspDamp       = pj.fnum("suspDamp", 0.0f);
            p.brakeTorque    = pj.fnum("brakeTorque", 0.0f);
            p.massDelta      = pj.fnum("massDelta", 0.0f);
            p.nitrousMult    = pj.fnum("nitrousMult", 0.0f);
            p.tankSeconds    = pj.fnum("tankSeconds", 0.0f);
            p.refillCost     = pj.inum("refillCost", 0);
            if (!p.id.empty()) m_parts.push_back(std::move(p));
        }
    }
    m_ok = !m_categories.empty() && !m_parts.empty();
    if (m_ok)
        x3::logInfo("[vehparts] catalog: " + std::to_string(m_categories.size()) +
                    " categories, " + std::to_string(m_parts.size()) + " parts");
    return m_ok;
}

const Part* Catalog::find(const std::string& id) const {
    for (const Part& p : m_parts) if (p.id == id) return &p;
    return nullptr;
}

std::vector<const Part*> Catalog::inCategory(const std::string& cat) const {
    std::vector<const Part*> out;
    for (const Part& p : m_parts) if (p.category == cat) out.push_back(&p);
    std::sort(out.begin(), out.end(),
              [](const Part* a, const Part* b){ return a->tier < b->tier; });
    return out;
}

// ===========================================================================
// VehicleBuild
// ===========================================================================
std::string defaultBuildSavePath() { return "vehbuild.json"; }

const std::string* VehicleBuild::installedIn(const std::string& category) const {
    for (const auto& kv : installed)
        if (kv.first == category && !kv.second.empty()) return &kv.second;
    return nullptr;
}

void VehicleBuild::install(const std::string& category, const std::string& partId) {
    for (auto& kv : installed)
        if (kv.first == category) { kv.second = partId; return; }
    installed.emplace_back(category, partId);
}

void VehicleBuild::removeFrom(const std::string& category) {
    for (auto& kv : installed)
        if (kv.first == category) { kv.second.clear(); return; }
}

std::string VehicleBuild::toJson() const {
    std::ostringstream o;
    o << "{\n  \"format\": \"" << kFormatTag << "\",\n";
    o << "  \"credits\": " << credits << ",\n";
    o << "  \"damaged\": " << (engineDamaged ? 1 : 0) << ",\n";
    o << "  \"nitrous\": " << nitrousRemaining << ",\n";
    o << "  \"tune\": [" << tune.boost << ", " << tune.fuel << ", " << tune.timing << "],\n";
    o << "  \"installed\": [";
    bool first = true;
    for (const auto& kv : installed) {
        if (kv.second.empty()) continue;
        if (!first) o << ",";
        o << "\n    [\"" << kv.first << "\", \"" << kv.second << "\"]";
        first = false;
    }
    o << (first ? "]" : "\n  ]") << "\n}\n";
    return o.str();
}

bool VehicleBuild::fromJson(const std::string& json) {
    JReader r(json);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj || root.sval("format") != kFormatTag) return false;
    VehicleBuild nb;
    nb.credits          = root.inum("credits", 12000);
    nb.engineDamaged    = root.inum("damaged", 0) != 0;
    nb.nitrousRemaining = root.fnum("nitrous", 0.0f);
    if (const JVal* t = root.get("tune")) {
        if (t->t == JVal::Arr && t->arr.size() >= 3) {
            nb.tune.boost  = (float)t->arr[0].num;
            nb.tune.fuel   = (float)t->arr[1].num;
            nb.tune.timing = (float)t->arr[2].num;
        }
    }
    if (const JVal* inst = root.get("installed")) {
        if (inst->t == JVal::Arr) {
            for (const JVal& kv : inst->arr)
                if (kv.t == JVal::Arr && kv.arr.size() >= 2 &&
                    kv.arr[0].t == JVal::Str && kv.arr[1].t == JVal::Str)
                    nb.install(kv.arr[0].str, kv.arr[1].str);
        }
    }
    *this = std::move(nb);
    return true;
}

bool VehicleBuild::saveFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << toJson();
    return (bool)f;
}

bool VehicleBuild::loadFile(const std::string& path) {
    const std::string js = readFile(path);
    if (js.empty()) return false;
    return fromJson(js);
}

// ===========================================================================
// Composition
// ===========================================================================
float knockIndexFor(const Catalog& cat, const VehicleBuild& build, const EcuTune& tune) {
    const std::string* ecuId = build.installedIn("ecu");
    const Part* ecu = ecuId ? cat.find(*ecuId) : nullptr;
    if (!ecu) return 0.0f;                              // no ECU = sliders locked
    float icBonus = 0.0f;
    if (const std::string* icId = build.installedIn("intercooler"))
        if (const Part* ic = cat.find(*icId)) icBonus = ic->safeBoostBonus;
    const float boost = std::clamp(tune.boost, 0.0f, ecu->maxBoost);
    float knock = 0.0f;
    knock += std::max(0.0f, boost - (ecu->safeBoost + icBonus)) * 2.5f;
    knock += std::max(0.0f, tune.fuel   - ecu->safeLean)   * 8.0f;
    knock += std::max(0.0f, tune.timing - ecu->safeTiming) * 5.0f;
    return knock;
}

ComposedBuild compose(const Catalog& cat, const VehicleBuild& build) {
    ComposedBuild out;
    const Baseline& base = cat.baseline();

    auto part = [&](const char* category) -> const Part* {
        const std::string* id = build.installedIn(category);
        return id ? cat.find(*id) : nullptr;
    };
    const Part* cam   = part("camshaft");
    const Part* exh   = part("exhaust");
    const Part* intk  = part("intake");
    const Part* ic    = part("intercooler");
    const Part* fi    = part("forced_induction");
    const Part* ecu   = part("ecu");
    const Part* tires = part("tires");
    const Part* susp  = part("suspension");
    const Part* brk   = part("brakes");
    const Part* wt    = part("weight");
    const Part* nos   = part("nitrous");

    // ---- Engine: bolt-on power % (exhaust + intake + intercooler). ----
    float powerMult = 1.0f
        + ((exh ? exh->powerPct : 0.0f) +
           (intk ? intk->powerPct : 0.0f) +
           (ic ? ic->powerPct : 0.0f)) / 100.0f;

    // ---- Forced induction: gain scales with the ECU boost slider (no ECU = a
    // fixed 40% wastegate-spring boost fraction). Turbo pushes its gain to the
    // top half of the curve (topEndBias); SC is flat. ----
    float fiFrac = 0.0f, fiGain = 0.0f;
    if (fi) {
        fiFrac = ecu ? std::clamp(build.tune.boost / std::max(0.05f, ecu->maxBoost), 0.0f, 1.0f)
                     : 0.4f;
        fiGain = fi->boostPowerPct / 100.0f * fiFrac;
    }

    // ---- ECU tune factor (boost/fuel/timing) + knock/pop data. ----
    float ecuFactor = 1.0f;
    if (ecu) {
        const float boostBar = std::clamp(build.tune.boost, 0.0f, ecu->maxBoost);
        const float boostEff = fi ? boostBar : boostBar * 0.25f;  // NA: token gain
        ecuFactor += ecu->powerPerBoost  / 100.0f * boostEff;
        ecuFactor += ecu->powerPerTiming / 100.0f * std::clamp(build.tune.timing, 0.0f, 1.0f);
        // Lean edge bonus / rich penalty.
        if (build.tune.fuel > 1.0f)
            ecuFactor += ecu->leanPowerPct / 100.0f *
                         std::clamp((build.tune.fuel - 1.0f) / 0.15f, 0.0f, 1.0f);
        else if (build.tune.fuel < 0.95f)
            ecuFactor -= (0.95f - build.tune.fuel) * 0.5f;
        out.ecuMaxBoost = ecu->maxBoost;
        out.knockLimit  = ecu->knockLimit;
        out.repairCost  = ecu->repairCost;
    }
    out.knockIndex = knockIndexFor(cat, build, build.tune);
    out.willPop    = ecu && out.knockIndex >= out.knockLimit;

    if (build.engineDamaged) powerMult *= kDamagePowerScale;

    // ---- Final curve: cam profile (or stock), FI top-end shaping, normalized. ----
    std::vector<CurvePt> curve = (cam && !cam->camCurve.empty()) ? cam->camCurve : base.curve;
    if (curve.empty()) curve = { {0.0f, 0.8f}, {0.5f, 1.0f}, {1.0f, 0.8f} };
    const bool isTurbo = fi && fi->fiType == "turbo";
    float curveMax = 0.0f;
    for (CurvePt& p : curve) {
        float w = 1.0f;
        if (isTurbo) {
            const float bias = std::clamp(fi->topEndBias, 0.0f, 1.0f);
            w = (1.0f - bias) + bias * std::clamp((p.x - 0.25f) / 0.5f, 0.0f, 1.0f);
        }
        p.y *= 1.0f + fiGain * w;
        curveMax = std::max(curveMax, p.y);
    }
    if (curveMax > 1e-4f) for (CurvePt& p : curve) p.y /= curveMax;   // re-normalize

    // ---- Compose the WheeledTuning. ----
    out.maxRpm = base.maxRpm + (cam ? cam->redlineBonus : 0.0f);
    const float peakNm = base.torqueNm * powerMult * ecuFactor * curveMax;
    out.tuning.maxEngineTorque = peakNm;
    out.tuning.maxEngineRPM    = out.maxRpm;
    out.tuning.curvePoints = (uint32_t)std::min<size_t>(curve.size(), 8);
    for (uint32_t i = 0; i < out.tuning.curvePoints; ++i) {
        out.tuning.curve[i].rpmFrac    = curve[i].x;
        out.tuning.curve[i].torqueFrac = curve[i].y;
    }
    out.massKg = base.massKg + (wt ? wt->massDelta : 0.0f);
    out.tuning.massKg = out.massKg;
    out.tuning.gripScale       = tires ? tires->gripScale : 1.0f;
    out.tuning.suspensionFreq  = susp && susp->suspFreq > 0.0f ? susp->suspFreq : base.suspFreq;
    out.tuning.suspensionDamp  = susp && susp->suspDamp > 0.0f ? susp->suspDamp : base.suspDamp;
    out.tuning.rideHeightDelta = susp ? susp->rideHeightDelta : 0.0f;
    out.tuning.brakeTorque     = brk ? brk->brakeTorque : base.brakeTorque;
    out.finalCurve = std::move(curve);

    // ---- Peaks (the dyno headline numbers). ----
    out.peakTorque = 0.0f; out.peakPowerKw = 0.0f;
    for (int i = 0; i <= 64; ++i) {
        const float x = (float)i / 64.0f;
        const float tq = peakNm * sampleCurve(out.finalCurve, x);
        const float rpm = x * out.maxRpm;
        const float kw = tq * rpm * 2.0f * 3.14159265f / 60.0f / 1000.0f;
        if (tq > out.peakTorque) { out.peakTorque = tq; out.peakTorqueRpm = rpm; }
        if (kw > out.peakPowerKw) { out.peakPowerKw = kw; out.peakPowerRpm = rpm; }
    }

    // ---- Audio profile. ----
    if (exh) {
        out.exhaustNote        = exh->noteId;
        out.exhaustPitchOffset = exh->pitchOffset;
        out.exhaustTimbre      = exh->timbre;
    }
    if (fi) {
        out.scWhine      = fi->fiType == "supercharger" && fi->whine;
        out.turboWhistle = isTurbo && fi->whistle;
        out.turboSpoolS  = isTurbo ? fi->spoolLagS : 0.0f;
    }
    if (nos) {
        out.nitrousMult       = nos->nitrousMult;
        out.nitrousTankS      = nos->tankSeconds;
        out.nitrousRefillCost = nos->refillCost;
    }
    return out;
}

float ComposedBuild::torqueAtRpmFrac(float rpmFrac) const {
    return tuning.maxEngineTorque * sampleCurve(finalCurve, std::clamp(rpmFrac, 0.0f, 1.0f));
}
float ComposedBuild::powerKwAtRpmFrac(float rpmFrac) const {
    const float rpm = std::clamp(rpmFrac, 0.0f, 1.0f) * maxRpm;
    return torqueAtRpmFrac(rpmFrac) * rpm * 2.0f * 3.14159265f / 60.0f / 1000.0f;
}

// ===========================================================================
// --test-vehparts — composition math + REAL physics deltas + dyno pop.
// ===========================================================================
namespace {

// One headless drive-world measurement run. Builds a fresh Jolt world + slab +
// DriveDemo, applies `tuning`, then runs `fn(car, phys)` and returns its result.
template <typename Fn>
float measureRun(const x3::phys::WheeledTuning& tuning, float boostMult, Fn&& fn) {
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) return -1.0f;
    x3::prims::PrimMesh g = x3::prims::makeBox(600.0f, 0.5f, 600.0f, 0.0f, -0.5f, 0.0f, 0.02f);
    phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                        g.cindex.data(), (uint32_t)g.cindex.size());
    DriveDemo car;
    if (!car.buildPhysics(*phys, 0.0f, 1.2f, 0.0f)) { phys->shutdown(); return -1.0f; }
    phys->optimizeBroadphase();
    car.applyTuning(tuning);
    if (boostMult > 1.0f) car.setTorqueBoost(boostMult);
    const float dt = 1.0f / 60.0f;
    // Settle on the suspension first.
    for (int i = 0; i < 90; ++i) {
        x3::phys::VehicleInput in{};
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
    }
    const float result = fn(car, *phys, dt);
    car.shutdown();
    phys->shutdown();
    return result;
}

// Full-throttle ticks to reach `targetSpeed` m/s (cap 3000 = "never").
float ticksToSpeed(const x3::phys::WheeledTuning& t, float targetSpeed, float boostMult = 1.0f) {
    return measureRun(t, boostMult, [&](DriveDemo& car, x3::phys::IPhysicsWorld& phys, float dt) {
        for (int i = 1; i <= 3000; ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setInput(in); car.preStep(dt); phys.step(dt); car.postStep(dt);
            if (car.forwardSpeed() >= targetSpeed) return (float)i;
        }
        return 3000.0f;
    });
}

// Accelerate to ~20 m/s, then full brake: distance from brake application to <0.5 m/s.
float brakingDistance(const x3::phys::WheeledTuning& t) {
    return measureRun(t, 1.0f, [&](DriveDemo& car, x3::phys::IPhysicsWorld& phys, float dt) {
        for (int i = 0; i < 3000 && car.forwardSpeed() < 20.0f; ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setInput(in); car.preStep(dt); phys.step(dt); car.postStep(dt);
        }
        float p0[3]; car.chassisPos(p0);
        for (int i = 0; i < 3000 && car.forwardSpeed() > 0.5f; ++i) {
            x3::phys::VehicleInput in{}; in.brake = 1.0f;
            car.setInput(in); car.preStep(dt); phys.step(dt); car.postStep(dt);
        }
        float p1[3]; car.chassisPos(p1);
        const float dx = p1[0]-p0[0], dz = p1[2]-p0[2];
        return std::sqrt(dx*dx + dz*dz);
    });
}

// Get to ~14 m/s, then hold FULL STEER for 1.5 s: total heading change (rad) of
// the velocity vector. Grippier tires corner harder -> bigger heading change
// before the front washes out (lateral slip onset).
float steerHeadingChange(const x3::phys::WheeledTuning& t) {
    return measureRun(t, 1.0f, [&](DriveDemo& car, x3::phys::IPhysicsWorld& phys, float dt) {
        for (int i = 0; i < 3000 && car.forwardSpeed() < 14.0f; ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setInput(in); car.preStep(dt); phys.step(dt); car.postStep(dt);
        }
        float prev[3]; car.chassisPos(prev);
        float heading = 0.0f, lastYaw = 0.0f; bool haveYaw = false;
        for (int i = 0; i < 90; ++i) {                 // 1.5 s of full-lock corner
            x3::phys::VehicleInput in{}; in.throttle = 0.35f; in.steer = 1.0f;
            car.setInput(in); car.preStep(dt); phys.step(dt); car.postStep(dt);
            float cur[3]; car.chassisPos(cur);
            const float vx = cur[0]-prev[0], vz = cur[2]-prev[2];
            if (vx*vx + vz*vz > 1e-6f) {
                const float yaw = std::atan2(vx, -vz);  // -Z forward convention
                if (haveYaw) {
                    float d = yaw - lastYaw;
                    while (d >  3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    heading += d;
                }
                lastYaw = yaw; haveYaw = true;
            }
            prev[0]=cur[0]; prev[1]=cur[1]; prev[2]=cur[2];
        }
        return std::fabs(heading);
    });
}

} // namespace

bool runVehPartsSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const std::string& name) {
        if (ok) { ++passN; x3::logInfo("[vehparts-test] PASS " + name); }
        else    { ++failN; x3::logError("[vehparts-test] FAIL " + name); }
    };
    char buf[256];

    // ---- P1: catalog parses; categories/tiers/prices sane. ----
    Catalog cat;
    const std::string path = defaultCatalogPath();
    check(cat.loadFile(path), "P1 catalog loads from " + path);
    if (!cat.ok()) return false;
    check(cat.categories().size() >= 10, "P1 >= 10 categories (" +
          std::to_string(cat.categories().size()) + ")");
    bool tiersOk = true, pricesOk = true;
    for (const Category& c : cat.categories()) {
        auto parts = cat.inCategory(c.id);
        if (parts.size() < 3) tiersOk = false;
        for (size_t i = 1; i < parts.size(); ++i)
            if (parts[i]->price <= parts[i-1]->price) pricesOk = false;
    }
    check(tiersOk,  "P1 every category has >= 3 tiers");
    check(pricesOk, "P1 prices ascend with tier in every category");

    // ---- The three reference builds. ----
    VehicleBuild stock;                                   // bone stock
    VehicleBuild street;                                  // tier-1 bolt-ons
    street.install("exhaust", "exh_catback");
    street.install("intake", "int_panel");
    street.install("tires", "tire_touring");
    street.install("ecu", "ecu_piggy");
    street.tune = { 0.5f, 1.0f, 0.5f };                   // mild safe tune
    VehicleBuild race;                                    // the full build
    race.install("camshaft", "cam_race");
    race.install("exhaust", "exh_sidepipe");
    race.install("intake", "int_itb");
    race.install("intercooler", "ic_race");
    race.install("forced_induction", "fi_turbo_big");
    race.install("ecu", "ecu_standalone");
    race.install("tires", "tire_slick");
    race.install("suspension", "susp_coilover_r");
    race.install("brakes", "brk_carbon");
    race.install("weight", "wt_full");
    race.install("nitrous", "nos_200");
    race.tune = { 1.5f, 1.05f, 0.8f };                    // at the safe edge

    ComposedBuild cStock  = compose(cat, stock);
    ComposedBuild cStreet = compose(cat, street);
    ComposedBuild cRace   = compose(cat, race);

    // ---- P2: composition math ordering. ----
    std::snprintf(buf, sizeof(buf), "P2 peak torque ordered: stock %.0f < street %.0f < race %.0f Nm",
                  cStock.peakTorque, cStreet.peakTorque, cRace.peakTorque);
    check(cStock.peakTorque < cStreet.peakTorque && cStreet.peakTorque < cRace.peakTorque, buf);
    std::snprintf(buf, sizeof(buf), "P2 race build is lighter: %.0f < %.0f kg",
                  cRace.massKg, cStock.massKg);
    check(cRace.massKg < cStock.massKg, buf);
    check(cRace.tuning.gripScale > cStock.tuning.gripScale, "P2 race grip > stock grip");
    check(cRace.tuning.brakeTorque > cStock.tuning.brakeTorque, "P2 race brakes > stock");
    check(cRace.maxRpm > cStock.maxRpm, "P2 race cam raises the redline");
    // Race cam trades low end for top end vs stock curve.
    const float stockLow = sampleCurve(cStock.finalCurve, 0.1f);
    const float raceLow  = sampleCurve(cRace.finalCurve, 0.1f);
    const float stockTopFrac = sampleCurve(cStock.finalCurve, 0.95f);
    const float raceTopFrac  = sampleCurve(cRace.finalCurve, 0.95f);
    check(raceLow < stockLow && raceTopFrac > stockTopFrac,
          "P2 cam+turbo shift the CURVE: leaner low-end fraction, fatter top-end");

    // ---- P3: FELT physics — 0 -> 25 m/s tick counts strictly ordered. ----
    const float tStock  = ticksToSpeed(cStock.tuning, 25.0f);
    const float tStreet = ticksToSpeed(cStreet.tuning, 25.0f);
    const float tRace   = ticksToSpeed(cRace.tuning, 25.0f);
    std::snprintf(buf, sizeof(buf), "P3 0->25 m/s ticks strictly ordered: stock %.0f > street %.0f > race %.0f",
                  tStock, tStreet, tRace);
    check(tStock > tStreet && tStreet > tRace && tRace > 0.0f, buf);

    // ---- P4: mass + brakes change the braking distance. ----
    const float dStock = brakingDistance(cStock.tuning);
    const float dRace  = brakingDistance(cRace.tuning);
    std::snprintf(buf, sizeof(buf), "P4 braking 20->0 m/s: race %.1f m < stock %.1f m", dRace, dStock);
    check(dRace > 0.0f && dStock > 0.0f && dRace < dStock, buf);

    // ---- P5: tires change the lateral slip onset (corner heading change). ----
    x3::phys::WheeledTuning gripOnly = cStock.tuning;     // SAME power/mass, only grip
    gripOnly.gripScale = cRace.tuning.gripScale;          // slicks
    const float hStock = steerHeadingChange(cStock.tuning);
    const float hSlick = steerHeadingChange(gripOnly);
    std::snprintf(buf, sizeof(buf), "P5 1.5 s full-lock heading: slicks %.2f rad > stock %.2f rad",
                  hSlick, hStock);
    check(hSlick > hStock * 1.05f, buf);

    // ---- P6: nitrous accelerates harder. Measured on the STOCK tuning (the race
    // build is rear-slick TRACTION-limited off the line, where extra torque only
    // spins the wheels faster — physically correct, but it masks the delta; the
    // stock car has torque headroom to convert). ----
    const float tNos = ticksToSpeed(cStock.tuning, 25.0f, cRace.nitrousMult);
    std::snprintf(buf, sizeof(buf), "P6 nitrous (x%.2f) 0->25 on stock: %.0f < %.0f ticks",
                  cRace.nitrousMult, tNos, tStock);
    check(cRace.nitrousMult > 1.0f && tNos < tStock, buf);

    // ---- P7: dyno knock / LIMIT POP thresholds (data from the ECU part). ----
    check(knockIndexFor(cat, race, { 1.5f, 1.05f, 0.8f }) < cRace.knockLimit,
          "P7 safe tune (at thresholds) does NOT pop");
    const float knockAbuse = knockIndexFor(cat, race, { 2.0f, 1.2f, 1.0f });
    std::snprintf(buf, sizeof(buf), "P7 abusive tune knocks %.2f >= limit %.2f -> POP",
                  knockAbuse, cRace.knockLimit);
    check(knockAbuse >= cRace.knockLimit, buf);
    // The intercooler EARNS its price: same abusive tune without the race core
    // knocks harder (its safeBoostBonus is the headroom).
    VehicleBuild noIc = race; noIc.removeFrom("intercooler");
    check(knockIndexFor(cat, noIc, { 2.0f, 1.2f, 1.0f }) > knockAbuse,
          "P7 removing the intercooler raises knock at the same boost");
    check(knockIndexFor(cat, stock, { 2.0f, 1.3f, 1.0f }) == 0.0f,
          "P7 no ECU installed -> sliders locked, knock 0");
    // Damage penalty is REAL power loss; repair restores it.
    VehicleBuild popped = race; popped.engineDamaged = true;
    ComposedBuild cPopped = compose(cat, popped);
    std::snprintf(buf, sizeof(buf), "P7 popped engine loses power: %.0f < %.0f Nm",
                  cPopped.peakTorque, cRace.peakTorque);
    check(cPopped.peakTorque < cRace.peakTorque * 0.9f, buf);
    popped.engineDamaged = false;
    check(std::fabs(compose(cat, popped).peakTorque - cRace.peakTorque) < 0.5f,
          "P7 repair restores full power");
    check(cRace.repairCost > 0, "P7 repair cost comes from the ECU part data");

    // ---- P8: VehicleBuild JSON round-trip. ----
    race.credits = 4242; race.nitrousRemaining = 3.5f; race.engineDamaged = true;
    VehicleBuild rt;
    check(rt.fromJson(race.toJson()), "P8 build JSON parses back");
    bool same = rt.credits == race.credits && rt.engineDamaged == race.engineDamaged &&
                std::fabs(rt.nitrousRemaining - race.nitrousRemaining) < 1e-3f &&
                std::fabs(rt.tune.boost - race.tune.boost) < 1e-3f &&
                std::fabs(rt.tune.fuel - race.tune.fuel) < 1e-3f &&
                std::fabs(rt.tune.timing - race.tune.timing) < 1e-3f;
    for (const Category& c : cat.categories()) {
        const std::string* a = race.installedIn(c.id);
        const std::string* b = rt.installedIn(c.id);
        if ((a == nullptr) != (b == nullptr)) same = false;
        else if (a && b && *a != *b) same = false;
    }
    check(same, "P8 build JSON round-trip preserves installed/tune/credits/damage");

    x3::logInfo("[vehparts-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game::vehparts
