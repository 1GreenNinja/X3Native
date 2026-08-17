// FREEWAY TRAFFIC — see traffic.h for the design. Companion laws:
// docs/NO_SLOP.md (rules 1/4/5/11 are load-bearing here) and
// road_network.h/.cpp (the lane geometry source — this file must NEVER
// re-derive what the ribbon already computes).
#include "traffic.h"
#include "glb_cpu_read.h"    // CPU bbox measurement (node hierarchy applied)
#include "terrain.h"         // terrainHeightAtWorld (loose-car contact law)
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace x3::game {

namespace {

// PAIRED with road_network.cpp's file-local kPaveProud (:2531) — the pavement
// slab rides the datum + this. A change to one IS a change to both (NO_SLOP
// rule 4), or every traffic car floats/sinks a slab thickness off the lanes.
constexpr float kTrafficPaveProud = 0.02f;

constexpr float kTrafLaneM = kLaneFt * kFtToM;    // 3.6576 m
constexpr float kMph2Mps   = 0.44704f;

// Following controller (constant time gap). kMinGapM is the bumper-to-bumper
// floor; --test-traffic T3 gates gap >= 0 ALWAYS.
constexpr float kMinGapM  = 7.0f;
constexpr float kHeadwayS = 1.6f;
constexpr float kAccelMax = 2.5f;    // m/s^2
constexpr float kBrakeMax = 6.5f;    // m/s^2

enum TrafficClass { ClsSedan = 0, ClsSport, ClsUtility, ClsVan, ClsHeavy };

struct TrafficModelDef {
    const char* file;
    const char* label;
    int   cls;
    float targetLenM;    // the REAL car's length — rule 1's metre law; a model
                         // measuring off it is uniformly rescaled to it
    float mphMin, mphMax;
    int   laneMin, laneMax;   // 0 = median-side fast lane .. 7 = outer truck lane
    int   weight;
};

// THE ROSTER. RCC fleet (proven in-engine: the tunnel garage + the hero cars)
// + the armory finds (draco-decoded by tools/traffic_decode_batch.py, audited
// by tools/traffic_roster_audit.py — OldVan / Sedan_Car3 / Sedan_Car4 /
// Pickup2 shipped real textures; the flat-grey-default-material armory trucks
// were REJECTED under NO_SLOP rule 3). Exclusions, each with its receipt:
// Coupe (exports at 13 cm — toy scale, broken), E46_New (the black-panel
// full-metal materials — the seven [gltf] L5 clamp warnings), F1 (an
// open-wheeler is not commuter traffic), CTR (the hero car, and 155k tris).
const TrafficModelDef kTrafficModels[] = {
    { "Vehicles/Traffic/Sedan_Car3.glb",  "SEDAN A",   ClsSedan,   4.50f, 62, 75, 1, 6, 18 },
    { "Vehicles/Traffic/Sedan_Car4.glb",  "SEDAN B",   ClsSedan,   4.45f, 62, 75, 1, 6, 18 },
    { "Vehicles/Traffic/OldVan.glb",      "VAN",       ClsVan,     5.40f, 58, 68, 3, 7, 10 },
    { "Vehicles/Traffic/Pickup2_URP.glb", "PICKUP B",  ClsUtility, 5.10f, 60, 72, 2, 7, 10 },
    { "Vehicles/E30.glb",                 "E30",       ClsSedan,   4.32f, 60, 76, 1, 6, 10 },
    { "Vehicles/M3_E36.glb",              "M3 E36",    ClsSport,   4.43f, 68, 82, 0, 4,  7 },
    { "Vehicles/Skyline_by_BUMSTRUM.glb", "SKYLINE",   ClsSport,   4.60f, 70, 85, 0, 3,  7 },
    { "Vehicles/Muscle.glb",              "MUSCLE",    ClsSport,   5.00f, 64, 80, 1, 5,  7 },
    { "Vehicles/Pickup.glb",              "PICKUP A",  ClsUtility, 5.40f, 58, 70, 2, 7,  8 },
    { "Vehicles/Jeep.glb",                "JEEP",      ClsUtility, 4.20f, 58, 70, 2, 7,  7 },
    { "Vehicles/Truck.glb",               "BOX TRUCK", ClsHeavy,   8.60f, 55, 62, 5, 7, 14 },
};
constexpr int kTrafficModelCount = (int)(sizeof(kTrafficModels) / sizeof(kTrafficModels[0]));

// Realistic paint distribution for the clearcoat (RCC) models — white/silver/
// black dominate real traffic. The armory models carry their own textures and
// ignore the tint (it only repaints clearcoat>0 drawables — DriveDemo's rule).
const float kPaintPalette[][3] = {
    { 0.92f, 0.92f, 0.93f },   // white
    { 0.72f, 0.73f, 0.76f },   // silver
    { 0.72f, 0.73f, 0.76f },   // silver (weighted twice)
    { 0.13f, 0.13f, 0.14f },   // black
    { 0.35f, 0.36f, 0.38f },   // grey
    { 0.55f, 0.08f, 0.08f },   // red
    { 0.09f, 0.14f, 0.34f },   // dark blue
    { 0.10f, 0.22f, 0.13f },   // dark green
};
constexpr int kPaintCount = (int)(sizeof(kPaintPalette) / sizeof(kPaintPalette[0]));

// Column-major orthonormal basis -> quaternion (x,y,z,w). Standard Shepperd.
void basisToQuat(const float c0[3], const float c1[3], const float c2[3], float q[4]) {
    const float m00 = c0[0], m10 = c0[1], m20 = c0[2];
    const float m01 = c1[0], m11 = c1[1], m21 = c1[2];
    const float m02 = c2[0], m12 = c2[1], m22 = c2[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
}

// Quaternion (x,y,z,w) -> column-major rotation matrix (upper 3x3 of out).
void quatToMat(const float q[4], float out[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float M[16] = {
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),     0,
        2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),     0,
        2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y), 0,
        0, 0, 0, 1 };
    std::memcpy(out, M, sizeof(M));
}

// Travel-direction basis at a lane point: forward (grade-aware), right, up.
// right = up x fwd — the same construction road_network's P() lateral implies.
void travelBasis(float fx, float fy, float fz, float r[3], float u[3], float f[3]) {
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    f[0] = fx / fl; f[1] = fy / fl; f[2] = fz / fl;
    r[0] = -f[2]; r[1] = 0.0f; r[2] = f[0];           // worldUp x fwd (XZ part)
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl; r[2] /= rl;
    u[0] = r[1] * f[2] - r[2] * f[1];                 // up = right x fwd
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
}

inline void mul4(const float a[16], const float b[16], float o[16]) {
    x3::asset::mulMat4(a, b, o);
}

} // namespace

uint32_t FreewayTraffic::rnd() {
    // xorshift32 — deterministic in (seed, call order); never libc rand.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}
float FreewayTraffic::rndf(float a, float b) {
    return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777215.0f;
}

// ---------------------------------------------------------------------------
// BUILD
// ---------------------------------------------------------------------------
bool FreewayTraffic::build(const RoadSpec& spec, const std::vector<float>& roadY,
                           x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld* phys,
                           std::string_view glbDir, const TrafficConfig& cfg) {
    m_cfg = cfg;
    m_rng = cfg.seed ? cfg.seed : 1u;
    m_device = device;
    m_phys = phys;
    if (!spec.dualCarriageway || spec.x.size() < 3 || roadY.size() != spec.x.size()) {
        x3::logWarn("traffic: spec is not a dual carriageway (or datum missing) — no traffic");
        return false;
    }
    // THE LANE GEOMETRY SOURCE: the exact fine path + median plan the ribbon
    // rode. Never re-derived, so lanes and pavement agree by construction.
    const std::vector<float> medianPlan = computeMedianPlan(spec, roadY);
    buildRoadRenderPath(spec, &roadY, medianPlan.empty() ? nullptr : &medianPlan, m_path);
    if (m_path.size() < 8) {
        x3::logWarn("traffic: render path degenerate — no traffic");
        return false;
    }
    m_totalLen = m_path.back().u;
    m_closed = std::fabs(m_path.front().x - m_path.back().x) < 0.05f &&
               std::fabs(m_path.front().z - m_path.back().z) < 0.05f;

    // ---- load + MEASURE the roster ----------------------------------------
    m_models.clear();
    m_models.resize(kTrafficModelCount);
    uint32_t okModels = 0;
    for (int i = 0; i < kTrafficModelCount; ++i) {
        const TrafficModelDef& d = kTrafficModels[i];
        Model& m = m_models[i];
        m.file = d.file; m.label = d.label; m.cls = d.cls;
        m.mphMin = d.mphMin; m.mphMax = d.mphMax;
        m.laneMin = d.laneMin; m.laneMax = d.laneMax; m.weight = d.weight;
        m.lenM = d.targetLenM;
        if (!device) {
            // Headless self-test: sim-only cars with class-default footprints.
            m.widthM  = (d.cls == ClsHeavy) ? 2.45f : 1.85f;
            m.heightM = (d.cls == ClsHeavy) ? 3.4f : (d.cls == ClsVan ? 2.2f : 1.45f);
            m.ok = true;
            ++okModels;
            continue;
        }
        // MEASURE, never assume (rule 0/1): CPU-read the GLB with the full
        // node hierarchy applied — the same numbers the GPU loader will draw.
        const std::string absPath = std::string(glbDir) + "/" + d.file;
        const GlbModel cpu = readGlbForLod(absPath, /*minTriangles=*/16);
        if (!cpu.ok) {
            x3::logWarn(std::string("traffic: DROP ") + d.label + " — cpu read: " + cpu.error);
            continue;
        }
        float mn[3] = { 1e18f, 1e18f, 1e18f }, mx[3] = { -1e18f, -1e18f, -1e18f };
        for (const GlbPrimitive& pr : cpu.prims)
            for (const auto& v : pr.verts)
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], v.pos[k]);
                    mx[k] = std::max(mx[k], v.pos[k]);
                }
        const float ext[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
        if (ext[2] < 0.01f || ext[0] < 0.01f) {
            x3::logWarn(std::string("traffic: DROP ") + d.label + " — degenerate bounds");
            continue;
        }
        if (ext[0] > ext[2] * 1.15f) {
            // Length must run the glTF Z axis (nose = +Z, the fleet's
            // documented convention). A sideways model would drive broadside.
            x3::logWarn(std::string("traffic: DROP ") + d.label +
                        " — authored along X, not Z (rule 3: facing broken)");
            continue;
        }
        m.scale = d.targetLenM / ext[2];
        if (std::fabs(m.scale - 1.0f) < 0.08f) m.scale = 1.0f;   // authored right
        m.widthM     = ext[0] * m.scale;
        m.heightM    = ext[1] * m.scale;
        m.groundLift = -mn[1] * m.scale;   // contact plane -> lane surface (rule 11)

        m.src.reset(x3::asset::createAssetSource());
        if (!m.src || !m.src->mountDir(glbDir, 0)) {
            x3::logWarn(std::string("traffic: DROP ") + d.label + " — mount failed");
            continue;
        }
        m.loader.reset(x3::asset::createModelLoader(device, m.src.get()));
        m.model = m.loader->load(d.file);
        if (!m.model.ok) {
            x3::logWarn(std::string("traffic: DROP ") + d.label + " — GLB load failed");
            continue;
        }
        std::vector<std::string> names;
        std::vector<x3::asset::ModelDrawable> all = x3::asset::makeDrawablesNamed(m.model, names);
        // Partition wheels by NODE name (Wheel_* / wheel_* / *Tire*; never a
        // steering wheel). One group per distinct node: each spins on its own
        // hub. Models with no identifiable wheel nodes simply don't spin.
        std::vector<std::string> groupNames;
        for (size_t di = 0; di < all.size(); ++di) {
            const std::string nm = di < names.size() ? names[di] : std::string();
            const bool isWheel =
                (nm.find("Wheel") != std::string::npos || nm.find("wheel") != std::string::npos ||
                 nm.find("Tire")  != std::string::npos || nm.find("tire")  != std::string::npos) &&
                nm.find("Steer") == std::string::npos && nm.find("steer") == std::string::npos;
            if (!isWheel) { m.body.push_back(all[di]); continue; }
            size_t g = 0;
            for (; g < groupNames.size(); ++g) if (groupNames[g] == nm) break;
            if (g == groupNames.size()) {
                groupNames.push_back(nm);
                WheelGroup wg;
                wg.hub[0] = all[di].nodeTransform[12];
                wg.hub[1] = all[di].nodeTransform[13];
                wg.hub[2] = all[di].nodeTransform[14];
                // Radius = hub height over the model's OWN contact plane — the
                // asset's measurement, never a magic constant (rule 5's
                // "measured offsets" law).
                wg.radius = std::max(0.12f, wg.hub[1] - mn[1]);
                m.wheels.push_back(wg);
            }
            m.wheels[g].draw.push_back(all[di]);
        }
        m.ok = !m.body.empty();
        if (!m.ok) {
            x3::logWarn(std::string("traffic: DROP ") + d.label + " — no body drawables");
            continue;
        }
        ++okModels;
        char b[240];
        std::snprintf(b, sizeof(b),
            "traffic: %-9s measured %.2f x %.2f x %.2f m -> scale %.3f (%.2f m), "
            "minY %+.2f, %u wheel node(s), %u body drawable(s)",
            d.label, ext[0], ext[1], ext[2], m.scale, m.lenM, mn[1],
            (uint32_t)m.wheels.size(), (uint32_t)m.body.size());
        x3::logInfo(b);
    }
    if (okModels == 0) {
        x3::logWarn("traffic: NO usable vehicle models — no traffic");
        return false;
    }
    m_built = true;
    char b[200];
    std::snprintf(b, sizeof(b),
        "traffic: %u/%d models live | route %.2f miles %s | target %u cars, "
        "ring %.0f-%.0f m, cull %.0f m",
        okModels, kTrafficModelCount, m_totalLen / 1609.34f,
        m_closed ? "(closed)" : "(open)", m_cfg.targetCount,
        m_cfg.ringNearM, m_cfg.ringFarM, m_cfg.cullM);
    x3::logInfo(b);
    return true;
}

// ---------------------------------------------------------------------------
// LANE SAMPLING
// ---------------------------------------------------------------------------
void FreewayTraffic::sampleAt(float u, float out[3], float dir[2],
                              float* medianHalf, float* dydu) const {
    if (m_closed) {
        u = std::fmod(u, m_totalLen);
        if (u < 0.0f) u += m_totalLen;
    } else {
        u = std::max(0.0f, std::min(u, m_totalLen));
    }
    size_t lo = 0, hi = m_path.size() - 1;
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (m_path[mid].u <= u) lo = mid; else hi = mid;
    }
    const RoadRenderStation& A = m_path[lo];
    const RoadRenderStation& B = m_path[hi];
    const float span = std::max(1e-4f, B.u - A.u);
    const float t = std::max(0.0f, std::min(1.0f, (u - A.u) / span));
    out[0] = A.x + (B.x - A.x) * t;
    out[1] = A.y + (B.y - A.y) * t;
    out[2] = A.z + (B.z - A.z) * t;
    float tx = A.tx + (B.tx - A.tx) * t;
    float tz = A.tz + (B.tz - A.tz) * t;
    const float tl = std::sqrt(tx * tx + tz * tz);
    if (tl > 1e-5f) { tx /= tl; tz /= tl; }
    dir[0] = tx; dir[1] = tz;
    if (medianHalf) *medianHalf = A.medianHalf + (B.medianHalf - A.medianHalf) * t;
    if (dydu) *dydu = (B.y - A.y) / span;
}

float FreewayTraffic::uOfS(int cw, float s) const {
    // Travel coordinate s increases in the DIRECTION OF TRAVEL. The right
    // carriageway (cw 1, centre at +lat) travels +u — that puts the median on
    // the driver's LEFT (the header's direction law); the left carriageway
    // travels -u by the mirrored argument.
    return (cw == 1) ? s : m_totalLen - s;
}

// Lane-centre lateral offset from the route centreline, signed in the
// ribbon's lat convention (lat>0 = right of +u travel = (-tz,+tx)).
// lane 0 = the median-side (fast) lane, kFwyLaneCount-1 = the outer lane.
static float laneLat(int cw, int lane, float medianHalf) {
    const float sgn = (cw == 1) ? 1.0f : -1.0f;
    return sgn * (medianHalf + kFwyPavedHalfM - kFwyRunningHalfM
                  + ((float)lane + 0.5f) * kTrafLaneM);
}

// ---------------------------------------------------------------------------
// SPAWN / DESPAWN
// ---------------------------------------------------------------------------
int FreewayTraffic::spawnForTest(int model, int cw, int lane, float s,
                                 float v, float cruise) {
    if (!m_built || model < 0 || model >= (int)m_models.size() || !m_models[model].ok)
        return -1;
    Car c;
    c.id = m_nextCarId++;
    c.model = model; c.cw = cw; c.lane = lane;
    c.s = s; c.v = v; c.cruise = cruise;
    c.halfH = m_models[model].heightM * 0.5f;
    m_cars.push_back(c);
    return (int)m_cars.size() - 1;
}

bool FreewayTraffic::trySpawn(const float focus[3], x3::phys::IPhysicsWorld* phys) {
    int totalW = 0;
    for (const Model& m : m_models) if (m.ok) totalW += m.weight;
    if (totalW == 0) return false;
    int pick = (int)(rnd() % (uint32_t)totalW);
    int mi = -1;
    for (int i = 0; i < (int)m_models.size(); ++i) {
        if (!m_models[i].ok) continue;
        pick -= m_models[i].weight;
        if (pick < 0) { mi = i; break; }
    }
    if (mi < 0) return false;
    const Model& md = m_models[mi];
    const int cw = (int)(rnd() & 1u);
    const int lane = md.laneMin + (int)(rnd() % (uint32_t)(md.laneMax - md.laneMin + 1));

    // Random station in the ring band around the focus. Uniform station pick
    // + distance test: stations are ~6-20 m apart, so the band coverage is
    // dense; misses just return false (the caller bounds attempts).
    const size_t si = (size_t)(rnd() % (uint32_t)m_path.size());
    const RoadRenderStation& st = m_path[si];
    if (st.gap) return false;   // a bore/deck owns that reach — never spawn in it
    const float ddx = st.x - focus[0], ddz = st.z - focus[2];
    const float d = std::sqrt(ddx * ddx + ddz * ddz);
    if (d < m_cfg.ringNearM || d > m_cfg.ringFarM) return false;

    const float u = st.u + rndf(0.0f, 12.0f);
    float s = (cw == 1) ? u : m_totalLen - u;
    if (m_closed) {
        s = std::fmod(s, m_totalLen);
        if (s < 0.0f) s += m_totalLen;
    }

    const float cruise = rndf(md.mphMin, md.mphMax) * kMph2Mps;
    const float v = cruise * rndf(0.85f, 1.0f);

    // Same-lane spacing: never spawn inside another car's following envelope.
    for (const Car& o : m_cars) {
        if (o.cw != cw || o.lane != lane) continue;
        float ds = std::fabs(o.s - s);
        if (m_closed) ds = std::min(ds, m_totalLen - ds);
        if (ds < kMinGapM + std::max(v, o.v) * kHeadwayS * 1.5f) return false;
    }

    Car c;
    c.id = m_nextCarId++;
    c.model = mi; c.cw = cw; c.lane = lane;
    c.s = s; c.v = v; c.cruise = cruise;
    c.halfH = md.heightM * 0.5f;
    const float* p = kPaintPalette[rnd() % (uint32_t)kPaintCount];
    c.tint[0] = p[0]; c.tint[1] = p[1]; c.tint[2] = p[2];
    c.hasTint = true;
    if (phys) {
        float pos[3], dir[2], mh;
        sampleAt(uOfS(cw, s), pos, dir, &mh, nullptr);
        const float lat = laneLat(cw, lane, mh);
        c.body = phys->addKinematicBox(
            x3::phys::Vec3{ md.widthM * 0.5f, c.halfH, md.lenM * 0.5f },
            x3::phys::Vec3{ pos[0] + (-dir[1]) * lat,
                            pos[1] + kTrafficPaveProud + c.halfH,
                            pos[2] + ( dir[0]) * lat },
            x3::phys::Layer::Dynamic);
    }
    m_cars.push_back(c);
    return true;
}

void FreewayTraffic::despawnCar(size_t idx, x3::phys::IPhysicsWorld* phys) {
    if (phys && m_cars[idx].body.valid()) phys->removeBody(m_cars[idx].body);
    m_cars[idx] = m_cars.back();
    m_cars.pop_back();
}

// ---------------------------------------------------------------------------
// UPDATE — sim + kinematic march. Call BEFORE the host's phys->step().
// ---------------------------------------------------------------------------
void FreewayTraffic::update(float dt, const float focus[3], x3::phys::IPhysicsWorld* phys) {
    if (!m_built || dt <= 0.0f) return;

    // ---- 1. following controller (constant time gap), per car -------------
    const size_t n = m_cars.size();
    for (size_t i = 0; i < n; ++i) {
        Car& c = m_cars[i];
        if (c.loose) continue;
        float bestDs = 1e9f, leaderV = 0.0f, leaderLen = 0.0f;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const Car& o = m_cars[j];
            if (o.cw != c.cw || o.lane != c.lane || o.loose) continue;
            float ds = o.s - c.s;
            if (m_closed) { if (ds <= 0.0f) ds += m_totalLen; }
            else if (ds <= 0.0f) continue;
            if (ds < bestDs) {
                bestDs = ds;
                leaderV = o.v;
                leaderLen = m_models[o.model].lenM;
            }
        }
        c.gapAhead = (bestDs < 1e8f) ? bestDs - leaderLen : 1e9f;
        float vT = c.cruise;
        if (c.gapAhead < 1e8f) {
            const float desired = kMinGapM + c.v * kHeadwayS;
            const float vFollow = leaderV + 0.5f * (c.gapAhead - desired) / kHeadwayS;
            vT = std::min(vT, std::max(0.0f, vFollow));
        }
        const float dv = vT - c.v;
        const float a = std::max(-kBrakeMax, std::min(kAccelMax, dv / std::max(dt, 1e-4f)));
        c.v = std::max(0.0f, c.v + a * dt);
        c.s += c.v * dt;
        if (m_closed && c.s >= m_totalLen) c.s -= m_totalLen;
        c.spin -= c.v * dt;    // accumulated -distance; per-wheel theta = spin/radius
    }

    // ---- 2. hard no-overlap invariant (the gap is NEVER negative) ---------
    for (size_t i = 0; i < n && i < m_cars.size(); ++i) {
        Car& c = m_cars[i];
        if (c.loose) continue;
        for (size_t j = 0; j < m_cars.size(); ++j) {
            if (j == i) continue;
            const Car& o = m_cars[j];
            if (o.cw != c.cw || o.lane != c.lane || o.loose) continue;
            float ds = o.s - c.s;
            if (m_closed) {
                if (ds < -m_totalLen * 0.5f) ds += m_totalLen;
                if (ds >  m_totalLen * 0.5f) ds -= m_totalLen;
            }
            if (ds <= 0.0f) continue;
            const float minSep = m_models[o.model].lenM + 0.5f;
            if (ds < minSep) {
                c.s = o.s - minSep;
                if (m_closed && c.s < 0.0f) c.s += m_totalLen;
                c.v = std::min(c.v, o.v);
            }
        }
    }

    // ---- 3. cull + refill the ring ----------------------------------------
    for (size_t i = 0; i < m_cars.size();) {
        Car& c = m_cars[i];
        float px, pz;
        if (c.loose && phys && c.body.valid()) {
            const x3::phys::Vec3 bp = phys->getBodyPosition(c.body);
            px = bp.x; pz = bp.z;
        } else {
            float pos[3], dir[2];
            sampleAt(uOfS(c.cw, c.s), pos, dir, nullptr, nullptr);
            px = pos[0]; pz = pos[2];
        }
        const float dx = px - focus[0], dz = pz - focus[2];
        const bool offEnd = !m_closed && !c.loose &&
                            (c.s <= 0.0f || c.s >= m_totalLen - 1.0f);
        if (dx * dx + dz * dz > m_cfg.cullM * m_cfg.cullM || offEnd) {
            despawnCar(i, phys);
            continue;
        }
        ++i;
    }
    int attempts = (m_cars.size() + 8 < m_cfg.targetCount) ? 64 : 8;
    while (m_cars.size() < m_cfg.targetCount && attempts-- > 0)
        trySpawn(focus, phys);

    // ---- 4. march the kinematic bodies / police the loose ones ------------
    if (!phys) return;
    for (Car& c : m_cars) {
        if (!c.body.valid()) continue;
        if (c.loose) {
            // NO_SLOP rule 11 — THE CONTACT LAW, runtime invariant: a loose
            // (dynamic) wreck must never end up under the carved field. Same
            // shape as DriveDemo::postStep's wheel clamp: only ever push UP.
            const x3::phys::Vec3 bp = phys->getBodyPosition(c.body);
            const float ground = terrainHeightAtWorld(bp.x, bp.z);
            if (bp.y < ground - 0.2f)
                phys->setBodyPosition(c.body, x3::phys::Vec3{ bp.x, ground + c.halfH, bp.z });
            continue;
        }
        float pos[3], dir[2], mh, dy;
        sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, &dy);
        const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
        const float lat = laneLat(c.cw, c.lane, mh);
        const float wx = pos[0] + (-dir[1]) * lat;
        const float wz = pos[2] + ( dir[0]) * lat;
        const float wy = pos[1] + kTrafficPaveProud;
        float r[3], upv[3], f[3];
        travelBasis(sgn * dir[0], sgn * dy, sgn * dir[1], r, upv, f);
        float q[4];
        basisToQuat(r, upv, f, q);
        phys->moveKinematic(c.body, x3::phys::Vec3{ wx, wy + c.halfH, wz }, q, dt);
    }
}

// ---------------------------------------------------------------------------
// CONTACT — a hard hit converts kinematic -> dynamic (the drum pattern).
// ---------------------------------------------------------------------------
void FreewayTraffic::onContact(x3::phys::BodyId a, x3::phys::BodyId b,
                               float impulse, x3::phys::IPhysicsWorld* phys) {
    if (!m_built || !phys || impulse < m_cfg.looseImpulse) return;
    for (Car& c : m_cars) {
        if (c.loose || !c.body.valid()) continue;
        if (c.body.id != a.id && c.body.id != b.id) continue;
        if (phys->makeBodyDynamic(c.body, 1400.0f)) {
            // Carry the lane momentum into the wreck so it slides on, not
            // stops dead. (moveKinematic left real velocity on the body; set
            // it explicitly so the conversion is deterministic regardless of
            // where in the frame the queued contact drained.)
            float pos[3], dir[2];
            sampleAt(uOfS(c.cw, c.s), pos, dir, nullptr, nullptr);
            const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
            const float vel[3] = { sgn * dir[0] * c.v, 0.0f, sgn * dir[1] * c.v };
            phys->setBodyLinearVelocity(c.body, vel);
            c.loose = true;
            char bmsg[96];
            std::snprintf(bmsg, sizeof(bmsg),
                "traffic: %s hit hard (impulse %.0f) — gone dynamic",
                m_models[c.model].label.c_str(), impulse);
            x3::logInfo(bmsg);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// RENDER — the DriveDemo skin path, per traffic car.
// ---------------------------------------------------------------------------
namespace {
void drawTrafficDrawable(x3::rhi::IRenderDevice& dev, const x3::rhi::FrameContext& f,
                         const x3::asset::ModelDrawable& d, const float world[16],
                         const float* tint) {
    const bool matEmis = d.emissiveTexId != 0 ||
        d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f ||
        d.emissiveFactor[2] > 0.001f;
    float emis[4] = { d.emissiveFactor[0], d.emissiveFactor[1], d.emissiveFactor[2],
                      matEmis ? 1.0f : 0.0f };
    float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                    d.baseColorFactor[2], d.baseColorFactor[3] };
    // Clearcoat-only repaint — DriveDemo::drawDrawable's rule: paint panels
    // recolor; glass/tires/trim/textured parts keep their authored look.
    if (tint && d.clearcoat > 0.01f) { bc[0] = tint[0]; bc[1] = tint[1]; bc[2] = tint[2]; }
    dev.drawMeshPBR(f, x3::rhi::MeshHandle{ d.meshId },
                    x3::rhi::TextureHandle{ d.baseColorTexId },
                    x3::rhi::TextureHandle{ d.normalTexId },
                    x3::rhi::TextureHandle{ d.mrTexId },
                    bc, emis, world, d.alphaMask, d.alphaBlend,
                    x3::rhi::TextureHandle{ d.emissiveTexId },
                    x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                    d.clearcoat, d.clearcoatRough);
}
} // namespace

void FreewayTraffic::render(const x3::rhi::FrameContext& frame, const float camPos[3]) {
    if (!m_built || !m_device) return;
    (void)camPos;   // everything live is inside the cull ring already
    for (const Car& c : m_cars) {
        const Model& md = m_models[c.model];
        if (!md.ok) continue;

        float carM[16];
        if (c.loose) {
            // Wreck: the physics body owns the pose (RiverLife's hull-attitude
            // precedent). Body centre sits halfH above the contact plane, so
            // the model (origin at its measured contact plane) drops by halfH.
            if (!m_phys || !c.body.valid()) continue;
            const x3::phys::Vec3 bp = m_phys->getBodyPosition(c.body);
            float bq[4];
            m_phys->getBodyRotation(c.body, bq);
            float R[16];
            quatToMat(bq, R);
            const float s = md.scale;
            // local drop (0,-halfH+groundLift... the box bottom == contact
            // plane): model origin sits at box-local (0, -halfH + groundLift)?
            // The model's contact plane is its minY; groundLift already maps
            // origin->contact. Box local offset of the model origin:
            const float loc[3] = { 0.0f, -c.halfH + md.groundLift, 0.0f };
            carM[0] = R[0] * s;  carM[1] = R[1] * s;  carM[2]  = R[2] * s;  carM[3]  = 0;
            carM[4] = R[4] * s;  carM[5] = R[5] * s;  carM[6]  = R[6] * s;  carM[7]  = 0;
            carM[8] = R[8] * s;  carM[9] = R[9] * s;  carM[10] = R[10] * s; carM[11] = 0;
            carM[12] = bp.x + R[0] * loc[0] + R[4] * loc[1] + R[8]  * loc[2];
            carM[13] = bp.y + R[1] * loc[0] + R[5] * loc[1] + R[9]  * loc[2];
            carM[14] = bp.z + R[2] * loc[0] + R[6] * loc[1] + R[10] * loc[2];
            carM[15] = 1;
        } else {
            float pos[3], dir[2], mh, dy;
            sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, &dy);
            const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
            const float lat = laneLat(c.cw, c.lane, mh);
            const float wx = pos[0] + (-dir[1]) * lat;
            const float wz = pos[2] + ( dir[0]) * lat;
            const float wy = pos[1] + kTrafficPaveProud + md.groundLift;
            float r[3], upv[3], f[3];
            travelBasis(sgn * dir[0], sgn * dy, sgn * dir[1], r, upv, f);
            const float s = md.scale;
            const float M[16] = { r[0] * s,   r[1] * s,   r[2] * s,   0,
                                  upv[0] * s, upv[1] * s, upv[2] * s, 0,
                                  f[0] * s,   f[1] * s,   f[2] * s,   0,
                                  wx, wy, wz, 1 };
            std::memcpy(carM, M, sizeof(M));
        }

        float fin[16];
        for (const auto& d : md.body) {
            mul4(carM, d.nodeTransform, fin);
            drawTrafficDrawable(*m_device, frame, d, fin,
                                c.hasTint ? c.tint : nullptr);
        }
        // WHEELS: spin about the model-local X axle at each group's own hub.
        // Model nose = +Z, contact at -Y: forward roll is omega = -v/r about
        // +X (v = omega x r_contact). c.spin accumulates -distance, so
        // theta = spin/radius already carries the sign.
        for (const WheelGroup& wg : md.wheels) {
            // World rolling radius = model-space hub height x model scale.
            // fmod against the circumference first: c.spin is metres of
            // travel and grows without bound; reducing per wheel keeps the
            // phase continuous AND the sin/cos argument small.
            const float worldR = wg.radius * md.scale;
            const float circ = 6.2831853f * worldR;
            const float th = c.loose ? 0.0f
                           : std::fmod(c.spin, circ) / worldR;
            const float ct = std::cos(th), st = std::sin(th);
            // spinM = T(hub) * RotX(th) * T(-hub), column-major:
            // p' = hub + R*(p - hub); R about X leaves x untouched.
            const float spinM[16] = {
                1, 0, 0, 0,
                0, ct, st, 0,
                0, -st, ct, 0,
                0,
                wg.hub[1] - ct * wg.hub[1] + st * wg.hub[2],
                wg.hub[2] - st * wg.hub[1] - ct * wg.hub[2],
                1 };
            float tmp[16], fin2[16];
            for (const auto& d : wg.draw) {
                mul4(spinM, d.nodeTransform, tmp);
                mul4(carM, tmp, fin2);
                drawTrafficDrawable(*m_device, frame, d, fin2,
                                    c.hasTint ? c.tint : nullptr);
            }
        }
    }
}

void FreewayTraffic::shutdown(x3::phys::IPhysicsWorld* phys) {
    if (phys)
        for (Car& c : m_cars)
            if (c.body.valid()) phys->removeBody(c.body);
    m_cars.clear();
    m_models.clear();
    m_built = false;
}

uint32_t FreewayTraffic::looseCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.loose) ++k;
    return k;
}

const char* FreewayTraffic::modelLabel(uint32_t i) const {
    return i < m_models.size() ? m_models[i].label.c_str() : "";
}

FreewayTraffic::CarState FreewayTraffic::carState(uint32_t i) const {
    CarState s{};
    if (i >= m_cars.size()) return s;
    const Car& c = m_cars[i];
    float pos[3], dir[2], mh;
    sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, nullptr);
    const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
    const float lat = laneLat(c.cw, c.lane, mh);
    s.id = c.id;
    s.x = pos[0] + (-dir[1]) * lat;
    s.y = pos[1];
    s.z = pos[2] + ( dir[0]) * lat;
    s.cx = pos[0];               // the route centreline (median axis) sample
    s.cz = pos[2];
    s.dirX = sgn * dir[0];
    s.dirZ = sgn * dir[1];
    s.v = c.v; s.cruise = c.cruise;
    s.cw = c.cw; s.lane = c.lane; s.loose = c.loose;
    s.cls = m_models[c.model].cls;
    s.gapAhead = c.gapAhead;
    return s;
}

// ===========================================================================
// --test-traffic — headless gates on the REAL inner course. No device, no
// physics world (the sim is pure); T4's determinism leg re-runs from scratch.
// ===========================================================================
bool runTrafficSelfTest() {
    int pass = 0, fail = 0;
    char d[300];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        char line[420];
        std::snprintf(line, sizeof(line), "%s %s%s%s", ok ? "PASS" : "FAIL", name,
                      detail ? " — " : "", detail ? detail : "");
        if (ok) { ++pass; x3::logInfo(line); }
        else    { ++fail; x3::logError(line); }
    };

    // The same recipe the host boots: the freeway course, registered fresh.
    clearTerrainCorridors();
    clearRoadJunctions();
    RoadSpec ringSpec = makeInnerCourse();
    std::vector<float> ringY;
    const RoadBuildResult rr = registerRoad(ringSpec, &ringY);
    check(rr.ok && ringSpec.dualCarriageway, "T0 the freeway course registers (dual)");
    if (!rr.ok) return fail == 0;

    auto makeSim = [&](uint32_t seed, uint32_t target) {
        auto t = std::make_unique<FreewayTraffic>();
        TrafficConfig cfg;
        cfg.seed = seed;
        cfg.targetCount = target;
        t->build(ringSpec, ringY, nullptr, nullptr, "", cfg);
        return t;
    };

    // Focus: a point ON the route, like a parked player.
    auto focusAt = [&](float uFrac, float out[3]) {
        std::vector<RoadRenderStation> path;
        const std::vector<float> mp = computeMedianPlan(ringSpec, ringY);
        buildRoadRenderPath(ringSpec, &ringY, mp.empty() ? nullptr : &mp, path);
        const size_t idx = (size_t)((float)(path.size() - 1) * uFrac);
        out[0] = path[idx].x; out[1] = path[idx].y; out[2] = path[idx].z;
    };

    // ---- T1 + T2 + T5: direction law, median-left, class discipline -------
    {
        auto t = makeSim(0xC0FFEEu, 60);
        float focus[3];
        focusAt(0.25f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 600; ++i) t->update(dt, focus, nullptr);

        // Snapshot -> one tick -> snapshot; compare by STABLE id (indices
        // reorder when a car culls mid-measurement).
        std::vector<FreewayTraffic::CarState> s0;
        for (uint32_t i = 0; i < t->liveCount(); ++i) s0.push_back(t->carState(i));
        t->update(dt, focus, nullptr);
        bool dirOk = true, medianOk = true, heavyOk = true, laneRangeOk = true;
        uint32_t heavies = 0, measured = 0;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState b = t->carState(i);
            const FreewayTraffic::CarState* a = nullptr;
            for (const auto& sc : s0) if (sc.id == b.id) { a = &sc; break; }
            if (!a) continue;
            ++measured;
            // T1: measured displacement vs the car's own claimed direction —
            // and, through the cw law, vs its carriageway's ONLY legal way.
            const float mdx = b.x - a->x, mdz = b.z - a->z;
            const float mv = std::sqrt(mdx * mdx + mdz * mdz);
            if (mv > 1e-3f && mdx * a->dirX + mdz * a->dirZ < 0.9f * mv) dirOk = false;
            // T2: the MEDIAN (the route centreline) must be on the driver's
            // LEFT. left = up x fwd = (dirZ, -dirX) for fwd (dirX, dirZ).
            const float toCx = b.cx - b.x, toCz = b.cz - b.z;
            if (toCx * b.dirZ + toCz * (-b.dirX) <= 0.0f) medianOk = false;
            if (b.cls == 4) { ++heavies; if (b.lane < 5) heavyOk = false; }
            if (b.lane < 0 || b.lane >= kFwyLaneCount) laneRangeOk = false;
        }
        std::snprintf(d, sizeof(d), "%u cars live, %u measured, %u heavy",
                      t->liveCount(), measured, heavies);
        check(t->liveCount() >= 30, "T0b the ring filled", d);
        check(dirOk && measured > 20,
              "T1 no head-on traffic: every car travels its carriageway's one legal way", d);
        check(medianOk, "T2 the median is on every driver's LEFT (measured)", d);
        check(heavyOk && heavies > 0, "T5 heavy trucks keep to the outer lanes", d);
        check(laneRangeOk, "T5b every car is inside the 8 running lanes");
        t->shutdown(nullptr);
    }

    // ---- T3: following distance never negative ----------------------------
    {
        // targetCount 0: the ring is OFF — only the seeded convoy exists, so
        // the gate isolates the following controller.
        auto t = makeSim(0xBEEF01u, 0);
        const int mi = 0;   // SEDAN A (headless models are always ok)
        t->spawnForTest(mi, 1, 3, 500.0f, 20.0f, 20.0f);          // leader at 20 m/s
        t->spawnForTest(mi, 1, 3, 380.0f, 33.0f, 36.0f);          // follower, closing
        float focus[3];
        focusAt(0.0f, focus);
        float minGap = 1e9f;
        bool everLimited = false;
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 1800; ++i) {                          // 30 s
            t->update(dt, focus, nullptr);
            const FreewayTraffic::CarState f2 = t->carState(1);
            if (f2.gapAhead < 1e8f) {
                minGap = std::min(minGap, f2.gapAhead);
                if (f2.v < f2.cruise - 1.0f) everLimited = true;
            }
        }
        const FreewayTraffic::CarState f2 = t->carState(1);
        std::snprintf(d, sizeof(d), "min gap %.2f m, settled v %.1f m/s (leader 20)",
                      minGap, f2.v);
        check(minGap >= 0.0f, "T3 following distance NEVER negative", d);
        check(everLimited && f2.v < 24.0f,
              "T3b the follower genuinely yielded to the leader", d);
        t->shutdown(nullptr);
    }

    // ---- T4: the ring + determinism ---------------------------------------
    {
        auto t = makeSim(0xD00Du, 60);
        float focus[3];
        focusAt(0.5f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 900; ++i) t->update(dt, focus, nullptr);
        const uint32_t filled = t->liveCount();
        bool inRing = true;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState a = t->carState(i);
            const float dx = a.x - focus[0], dz = a.z - focus[2];
            if (std::sqrt(dx * dx + dz * dz) > 1650.0f) inRing = false;
        }
        // Move the focus a quarter route away; the old population must cull.
        float focus2[3];
        focusAt(0.75f, focus2);
        for (int i = 0; i < 900; ++i) t->update(dt, focus2, nullptr);
        bool culled = true;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState a = t->carState(i);
            const float dx = a.x - focus2[0], dz = a.z - focus2[2];
            if (std::sqrt(dx * dx + dz * dz) > 1650.0f) culled = false;
        }
        std::snprintf(d, sizeof(d), "filled %u, refilled %u after the move",
                      filled, t->liveCount());
        check(filled >= 30 && filled <= 60 && inRing,
              "T4 the ring fills inside its band", d);
        check(culled && t->liveCount() >= 30,
              "T4b the move culls the far side and refills", d);
        t->shutdown(nullptr);

        // Determinism: same seed, same focus script -> identical state hash.
        auto hashOf = [&](FreewayTraffic& tt) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < tt.liveCount(); ++i) {
                const FreewayTraffic::CarState a = tt.carState(i);
                const float vals[4] = { a.x, a.z, a.v, (float)(a.cw * 8 + a.lane) };
                for (float v : vals) {
                    uint32_t bits;
                    std::memcpy(&bits, &v, sizeof(bits));
                    h = (h ^ bits) * 1099511628211ull;
                }
            }
            return h;
        };
        auto t1 = makeSim(0x5EED5EEDu, 60);
        auto t2 = makeSim(0x5EED5EEDu, 60);
        float f3[3];
        focusAt(0.1f, f3);
        for (int i = 0; i < 600; ++i) { t1->update(dt, f3, nullptr); t2->update(dt, f3, nullptr); }
        const uint64_t h1 = hashOf(*t1), h2 = hashOf(*t2);
        std::snprintf(d, sizeof(d), "hash %016llx", (unsigned long long)h1);
        check(h1 == h2 && t1->liveCount() > 0,
              "T4c the sim is deterministic (same seed, same story)", d);
        t1->shutdown(nullptr);
        t2->shutdown(nullptr);
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    char sum[96];
    std::snprintf(sum, sizeof(sum), "traffic: %d/%d passed", pass, pass + fail);
    if (fail == 0) x3::logInfo(sum); else x3::logError(sum);
    return fail == 0;
}

} // namespace x3::game
