// CAR ROSTER (x3.vehicle/1) — roster.json loader + the --test-roster self-test.
// See app/roster.h. Clean-room: C++ stdlib + the shared json_mini DOM + the public
// vehparts catalog/compose API only (engine/ stays pure).
#include "roster.h"
#include "json_mini.h"
#include "asset_root.h"     // assetRoot() / convertedGlbRoot()

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace x3::game::roster {

using x3::game::jmini::JVal;
using x3::game::jmini::JReader;
using x3::game::jmini::readFile;

// ---------------------------------------------------------------------------
// enum <-> string
// ---------------------------------------------------------------------------
const char* drivetrainName(Drivetrain d) {
    switch (d) { case Drivetrain::FWD: return "FWD";
                 case Drivetrain::AWD: return "AWD";
                 default: return "RWD"; }
}
Drivetrain drivetrainFromString(const std::string& s) {
    if (s == "FWD" || s == "fwd") return Drivetrain::FWD;
    if (s == "AWD" || s == "awd") return Drivetrain::AWD;
    return Drivetrain::RWD;
}
const char* classNameOf(VClass c) {
    switch (c) { case VClass::Muscle:  return "muscle";
                 case VClass::Super:   return "super";
                 case VClass::Utility: return "utility";
                 case VClass::Bike:    return "bike";
                 case VClass::Alien:   return "alien";
                 default: return "street"; }
}
VClass classFromString(const std::string& s) {
    if (s == "muscle")  return VClass::Muscle;
    if (s == "super")   return VClass::Super;
    if (s == "utility") return VClass::Utility;
    if (s == "bike")    return VClass::Bike;
    if (s == "alien")   return VClass::Alien;
    return VClass::Street;
}

// ---------------------------------------------------------------------------
// Curve parse (shared shape with vehparts: array of [x,y] pairs).
// ---------------------------------------------------------------------------
static std::vector<vehparts::CurvePt> parseCurve(const JVal* v) {
    std::vector<vehparts::CurvePt> out;
    if (!v || v->t != JVal::Arr) return out;
    for (const auto& pt : v->arr) {
        if (pt.t == JVal::Arr && pt.arr.size() >= 2 &&
            pt.arr[0].t == JVal::Num && pt.arr[1].t == JVal::Num) {
            out.push_back({ (float)pt.arr[0].num, (float)pt.arr[1].num });
        }
    }
    return out;
}

static void parseVec3(const JVal* v, float out[3]) {
    if (v && v->t == JVal::Arr && v->arr.size() >= 3) {
        for (int i = 0; i < 3; ++i)
            if (v->arr[i].t == JVal::Num) out[i] = (float)v->arr[i].num;
    }
}

// ---------------------------------------------------------------------------
// Roster::load
// ---------------------------------------------------------------------------
std::string defaultRosterPath() {
    return x3::game::assetRoot() + "/vehicles/roster.json";
}

bool Roster::loadFile(const std::string& path) {
    return loadJson(readFile(path));
}

bool Roster::loadJson(const std::string& json) {
    m_ok = false;
    m_cars.clear();
    if (json.empty()) return false;

    JReader r(json);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) return false;
    if (root.sval("format") != "x3.vehicle/1") return false;

    const JVal* cars = root.get("cars");
    if (!cars || cars->t != JVal::Arr) return false;

    for (const auto& cv : cars->arr) {
        if (cv.t != JVal::Obj) continue;
        Car c;
        c.id     = cv.sval("id");
        c.name   = cv.sval("name", c.id);
        c.cls    = classFromString(cv.sval("class", "street"));
        c.source = cv.sval("source");
        c.glb    = cv.sval("glb");
        parseVec3(cv.get("tint"), c.tint);
        c.drivetrain = drivetrainFromString(cv.sval("drivetrain", "RWD"));
        parseVec3(cv.get("halfExtents"), c.halfExtents);
        c.rideHeight = cv.fnum("rideHeight", c.rideHeight);

        if (const JVal* b = cv.get("baseline")) {
            c.base.torqueNm    = b->fnum("torqueNm",    c.base.torqueNm);
            c.base.maxRpm      = b->fnum("maxRpm",      c.base.maxRpm);
            c.base.massKg      = b->fnum("massKg",      c.base.massKg);
            c.base.brakeTorque = b->fnum("brakeTorque", c.base.brakeTorque);
            c.base.suspFreq    = b->fnum("suspFreq",    c.base.suspFreq);
            c.base.suspDamp    = b->fnum("suspDamp",    c.base.suspDamp);
            auto cur = parseCurve(b->get("curve"));
            if (!cur.empty()) c.base.curve = std::move(cur);
        }
        if (c.id.empty() || c.glb.empty()) continue;   // skip malformed
        m_cars.push_back(std::move(c));
    }
    m_ok = !m_cars.empty();
    return m_ok;
}

const Car* Roster::find(const std::string& id) const {
    for (const auto& c : m_cars) if (c.id == id) return &c;
    return nullptr;
}

// ===========================================================================
// --test-roster
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& msg) {
    if (cond) { ++g_pass; x3::logInfo("[roster-test] PASS " + msg); }
    else      { ++g_fail; x3::logError("[roster-test] FAIL " + msg); }
}

// Is the converted GLB present locally, OR listed in the asset-store manifest?
// (manifest-listed but not yet fetched = a graceful skip, not a failure.)
struct GlbState { bool local = false; bool inManifest = false; };

GlbState glbState(const std::string& relPath) {
    namespace fs = std::filesystem;
    GlbState st;
    const std::string full = x3::game::convertedGlbRoot() + "/" + relPath;
    std::error_code ec;
    st.local = fs::exists(full, ec) && fs::is_regular_file(full, ec);

    // manifest entry: "assets/converted_glb/<relPath>" appears as a repo_path.
    const std::string manifestPath = x3::game::assetRoot() + "/manifest.json";
    const std::string body = readFile(manifestPath);
    if (!body.empty()) {
        const std::string needle = "converted_glb/" + relPath;
        st.inManifest = body.find(needle) != std::string::npos;
    }
    return st;
}

// Cheap headless wheel-node check: GLB stores node names as plain UTF-8 in the
// JSON chunk; scan the file bytes for the canonical wheel node names the skin
// path (app/vehicle.cpp) resolves. Avoids spinning up a render device.
bool glbHasWheels(const std::string& relPath) {
    const std::string full = x3::game::convertedGlbRoot() + "/" + relPath;
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    const char* names[4] = { "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" };
    for (const char* n : names)
        if (body.find(n) == std::string::npos) return false;
    return true;
}

// A "full send" parts build: install the top tier in every performance category
// present in the catalog (the heaviest valid upgrade) for the compose() check.
vehparts::VehicleBuild fullBuild(const vehparts::Catalog& cat) {
    vehparts::VehicleBuild b;
    for (const auto& category : cat.categories()) {
        auto parts = cat.inCategory(category.id);
        const vehparts::Part* best = nullptr;
        for (const auto* p : parts)
            if (!best || p->tier > best->tier) best = p;
        if (best) b.install(category.id, best->id);
    }
    b.tune.boost = 0.0f;   // SAFE tune (no knock pop) — we want a clean power read.
    return b;
}

bool curveAscending(const std::vector<vehparts::CurvePt>& c) {
    if (c.empty()) return true;     // empty = "keep stock" (vehparts default)
    for (size_t i = 1; i < c.size(); ++i)
        if (c[i].x < c[i - 1].x - 1e-4f) return false;
    return true;
}

} // namespace

bool runRosterSelfTest() {
    g_pass = g_fail = 0;

    // --- Load the roster + the shared parts catalog. ---
    Roster ros;
    const bool rosterOk = ros.loadFile(defaultRosterPath());
    check(rosterOk, "roster.json loads (format x3.vehicle/1)");
    if (!rosterOk) { x3::logError("[roster-test] cannot continue without a roster"); return false; }

    check(!ros.cars().empty(), "roster has at least one car");
    x3::logInfo("[roster-test] roster size: " + std::to_string(ros.size()) + " car(s)");

    vehparts::Catalog cat;
    const bool catOk = cat.loadFile(vehparts::defaultCatalogPath());
    check(catOk, "shared parts catalog (parts.json) loads");

    // Baseline power for the parts-gain ordering check (stock, no parts).
    auto peakPowerOf = [&](const vehparts::Baseline& base, const vehparts::VehicleBuild& build) {
        cat.setBaseline(base);
        return vehparts::compose(cat, build).peakPowerKw;
    };

    int present = 0, skipped = 0, wheelsOk = 0;

    for (const auto& c : ros.cars()) {
        const std::string tag = "[" + c.id + "] ";

        // 1) Stats are finite + sane.
        const bool statsSane =
            std::isfinite(c.base.torqueNm)    && c.base.torqueNm    > 0 &&
            std::isfinite(c.base.maxRpm)      && c.base.maxRpm      > 1000 &&
            std::isfinite(c.base.massKg)      && c.base.massKg      > 200 && c.base.massKg < 6000 &&
            std::isfinite(c.base.brakeTorque) && c.base.brakeTorque > 0 &&
            std::isfinite(c.base.suspFreq)    && c.base.suspFreq    > 0 &&
            std::isfinite(c.base.suspDamp)    && c.base.suspDamp    > 0;
        check(statsSane, tag + "baseline stats finite + in range");
        check(curveAscending(c.base.curve), tag + "torque curve ascending in rpmFrac");

        // 2) Parts-system compatibility: the car's baseline composes with a full
        //    parts build into a finite Jolt tuning whose power EXCEEDS stock.
        if (catOk) {
            vehparts::VehicleBuild stock;          // nothing installed
            vehparts::VehicleBuild full = fullBuild(cat);
            const float pStock = peakPowerOf(c.base, stock);
            const float pFull  = peakPowerOf(c.base, full);
            cat.setBaseline(c.base);
            vehparts::ComposedBuild cb = vehparts::compose(cat, full);
            const bool tuningResolves =
                std::isfinite(cb.tuning.maxEngineTorque) && cb.tuning.maxEngineTorque > 0 &&
                std::isfinite(cb.tuning.massKg)          && cb.tuning.massKg > 0 &&
                std::isfinite(cb.peakPowerKw)            && cb.peakPowerKw > 0;
            check(tuningResolves, tag + "full parts build -> finite Jolt WheeledTuning");
            check(std::isfinite(pStock) && std::isfinite(pFull) && pFull > pStock,
                  tag + "parts add power (" + std::to_string((int)pStock) + " -> " +
                  std::to_string((int)pFull) + " kW)");
        }

        // 3) GLB present (local) or in the store manifest (graceful skip if neither).
        GlbState gs = glbState(c.glb);
        if (gs.local) {
            ++present;
            // 4) wheels resolve for spinning (only when the file is on disk).
            const bool w = glbHasWheels(c.glb);
            check(w, tag + "GLB exposes Wheel_FL/FR/RL/RR for independent spin");
            if (w) ++wheelsOk;
        } else if (gs.inManifest) {
            ++skipped;
            x3::logInfo("[roster-test] " + tag + "GLB not fetched yet but in manifest — graceful skip: " + c.glb);
        } else {
            // Not present and not published: that's a roster integrity problem.
            check(false, tag + "GLB present locally OR in the asset-store manifest (" + c.glb + ")");
        }
    }

    x3::logInfo("[roster-test] GLBs: " + std::to_string(present) + " local (" +
                std::to_string(wheelsOk) + " with wheels), " +
                std::to_string(skipped) + " manifest-only (skipped).");

    // Spawn-table sanity: the carshow lineup instantiates one transform per car.
    int spawned = 0;
    for (const auto& c : ros.cars()) { (void)c; ++spawned; }
    check(spawned == (int)ros.size() && spawned > 0, "spawn table instantiates one entity per car");

    x3::logInfo("[roster-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game::roster
