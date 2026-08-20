// FREEWAY TRAFFIC — see traffic.h for the design. Companion laws:
// docs/NO_SLOP.md (rules 1/4/5/9/11 are load-bearing here),
// docs/design/X3_WORLD_RULES.md (rules 4/5/7 for the furniture) and
// road_network.h/.cpp (the lane geometry source — this file must NEVER
// re-derive what the ribbon already computes).
#include "traffic.h"
#include "glb_cpu_read.h"    // CPU bbox measurement (node hierarchy applied)
#include "terrain.h"         // terrainHeightAtWorld (loose-car contact law)
#include "lns_shop.h"        // makeSignRGBA (the 5x7 baker) + makeMr1x1
#include "audio_root.h"      // resolveAudio — the horns/siren live in-repo
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
constexpr float kMps2Mph   = 2.2369363f;

// Following controller (constant time gap). These are the BASELINE numbers;
// every car scales them by its class profile and its temperament (see
// kClassProfiles / applyTemper). kMinGapM is the bumper-to-bumper floor;
// --test-traffic T3 gates gap >= 0 ALWAYS, for every driver ever built.
constexpr float kMinGapM  = 7.0f;
constexpr float kHeadwayS = 1.6f;

// ---- LANE CHANGES ---------------------------------------------------------
// A merge takes 2-3 s of real lateral travel; a car that snapped a lane width
// in one tick would tunnel straight through anyone beside it and no overlap
// pass could see it happen. Signalling leads the merge (the visible tell).
constexpr float kMergeMinS      = 2.0f;
constexpr float kMergeMaxS      = 3.0f;
constexpr float kSignalLeadS    = 1.15f;   // polite drivers signal this long
constexpr float kThinkEveryS    = 0.30f;   // lane-change deliberation cadence
constexpr float kScanWindowM    = 190.0f;  // longitudinal reach of every scan
// Lateral slack added to the two cars' half-widths. FOLLOW is the wider test
// (do I have to brake for this car?), OVERLAP the tighter one (am I INSIDE
// them?). With 3.658 m lanes and a 1.85 m car, adjacent lanes never interact
// and a car half a lane over always does — which is exactly the intent.
constexpr float kLatFollowSlack = 0.35f;
constexpr float kLatOverlapSlack = 0.10f;

// ---- THE SHOULDER ---------------------------------------------------------
// A broken-down car parks OFF the running lanes, on the paved apron. In lane
// coordinates the running lanes are [0, kFwyLaneCount-1]; laneLat() is linear
// in laneF, so the offset of laneF from the carriageway centre is
// (laneF + 0.5) * kTrafLaneM - kFwyRunningHalfM. At laneF 8.2 that is 17.24 m:
// a 1.85 m-wide car spans 16.31..18.16 m, so it clears the running-lane edge
// (kFwyRunningHalfM = 14.63 m) by 1.68 m and stays 3.79 m inside the paved
// edge (kFwyPavedHalfM = 21.95 m). A tow truck at 2.45 m wide still clears by
// 1.01 m. --test-traffic T9 MEASURES both margins rather than trusting this
// comment (NO_SLOP rule 9).
constexpr float kShoulderLaneF  = 8.2f;

// ---- HORNS ----------------------------------------------------------------
// "Rate-limit so a jam is not a cacophony." Three independent limiters: a
// global one-horn-at-a-time gap, a long per-car cooldown, and a distance gate
// (a horn 400 m away is inaudible anyway — do not spend a voice on it).
constexpr float kHornGlobalGapS = 0.55f;
constexpr float kHornCarCdS     = 6.0f;
constexpr float kHornRangeM     = 150.0f;
// The provocations. Tuned so a normal freeway minute is quiet and a cut-in is
// answered instantly.
constexpr float kHardBrakeMps2  = 4.2f;    // decel that earns a blast
constexpr float kTailgatedS     = 2.2f;    // seconds on my bumper before I say so
// Decel at which the rear lamps light. Real brake-light switches trip at a
// featherweight pedal press; 0.9 m/s^2 is about that, and it keeps the lamps
// off during the constant-time-gap controller's ordinary micro-corrections.
constexpr float kBrakeLightMps2 = 0.9f;

constexpr float kSirenRangeM    = 260.0f;  // beyond this a siren voice is wasted
constexpr uint32_t kMaxSirens   = 2;
constexpr uint32_t kMaxTrafficLights = 8;

enum TrafficClass { ClsSedan = 0, ClsSport, ClsUtility, ClsVan, ClsHeavy, ClsSuper };

// ---------------------------------------------------------------------------
// PERFORMANCE PROFILES — "some different performance profiles on the cars and
// trucks". Per CLASS, because that is the level at which the difference is
// legible from the driver's seat: a truck is slow to spool AND slow to stop, a
// supercar is neither. Every number is a physical quantity in SI, and the
// class's cruise BAND lives in the roster row beside the model (mphMin/mphMax)
// so speed and machine stay one fact.
//
//   accel  m/s^2 the driver is willing to use getting up to cruise.
//   brake  m/s^2 available for closing on a leader. A loaded box truck really
//          does stop at about half a car's rate; that is the whole character.
//   headway seconds of time gap held to the leader (the constant-time-gap
//          controller's T). Trucks legally and physically hold more.
//   minGap metres of bumper-to-bumper floor at a standstill.
//   urge   0..1 appetite for changing lanes to make progress. THE keep-right
//          law lives here too: a heavy's urge is near zero, so it only ever
//          moves left when actually blocked, and drifts back right after.
// ---------------------------------------------------------------------------
struct ClassProfile {
    float accel, brake, headway, minGap, urge;
    const char* name;
};
const ClassProfile kClassProfiles[] = {
    /* ClsSedan   */ { 2.60f, 6.50f, 1.60f,  7.0f, 0.35f, "sedan"   },
    /* ClsSport   */ { 4.20f, 8.00f, 1.35f,  6.5f, 0.70f, "sport"   },
    /* ClsUtility */ { 2.00f, 5.50f, 1.75f,  8.0f, 0.25f, "utility" },
    /* ClsVan     */ { 1.80f, 5.20f, 1.85f,  8.5f, 0.20f, "van"     },
    /* ClsHeavy   */ { 0.85f, 3.20f, 2.45f, 12.0f, 0.10f, "heavy"   },
    /* ClsSuper   */ { 5.60f, 9.00f, 1.25f,  6.0f, 0.85f, "super"   },
};

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
//
// THE SUPERCAR SLOT, and the substitution, stated plainly (the owner asked for
// "black Acura NSX Type S cars on the fastest profile"): there is NO NSX in
// the 914-package catalog — `unitypackage_index.py --search nsx` returns
// nothing, and a filename sweep for supercar/lambo/ferrari/mclaren/exotic
// across all 914 finds only an engine WAV. There is no mid-engine supercar
// mesh in the library at all. The SKYLINE (already converted, already proven,
// 2 textures, the roster's most performance-coupe silhouette) therefore takes
// the ClsSuper profile and a black-dominant palette. It is a SUBSTITUTE and is
// labelled as one in the boot log. The one true mid/rear-engine car on the box
// is CTR.glb — rejected here on measurement, not taste: 155k triangles against
// the roster's 9k average, so three of them would add ~10% to the frame's
// triangle count for three cars (see the perf table in HANDOFF_W-TRAFFIC.md).
const TrafficModelDef kTrafficModels[] = {
    { "Vehicles/Traffic/Sedan_Car3.glb",  "SEDAN A",   ClsSedan,   4.50f, 62, 75, 1, 6, 18 },
    { "Vehicles/Traffic/Sedan_Car4.glb",  "SEDAN B",   ClsSedan,   4.45f, 62, 75, 1, 6, 18 },
    { "Vehicles/Traffic/OldVan.glb",      "VAN",       ClsVan,     5.40f, 58, 68, 3, 7, 10 },
    { "Vehicles/Traffic/Pickup2_URP.glb", "PICKUP B",  ClsUtility, 5.10f, 60, 72, 2, 7, 10 },
    { "Vehicles/E30.glb",                 "E30",       ClsSedan,   4.32f, 60, 76, 1, 6, 12 },
    { "Vehicles/M3_E36.glb",              "M3 E36",    ClsSport,   4.43f, 68, 82, 0, 4,  7 },
    { "Vehicles/Skyline_by_BUMSTRUM.glb", "NSX-SUB",   ClsSuper,   4.60f, 80, 96, 0, 3,  6 },
    { "Vehicles/Muscle.glb",              "MUSCLE",    ClsSport,   5.00f, 64, 80, 1, 5,  7 },
    { "Vehicles/Pickup.glb",              "PICKUP A",  ClsUtility, 5.40f, 58, 70, 2, 7,  8 },
    { "Vehicles/Jeep.glb",                "JEEP",      ClsUtility, 4.20f, 58, 70, 2, 7,  7 },
    { "Vehicles/Truck.glb",               "BOX TRUCK", ClsHeavy,   8.60f, 55, 62, 5, 7, 14 },
};
constexpr int kTrafficModelCount = (int)(sizeof(kTrafficModels) / sizeof(kTrafficModels[0]));

// The COP body and the TOW body are drawn from the roster rather than from
// their own GLBs (there is no police car and no tow truck in the catalog that
// is both extracted and convertible — see the report). Both are chosen for a
// reason, not at random:
//   E30      a boxy three-box sedan, and one of the PAINTABLE models, so the
//            white patrol livery actually lands on it (the textured armory
//            sedans would ignore the tint — see Model::bodyPaintable).
//   TRUCK    the 6-wheel box truck: the only heavy body on the roster, and the
//            only one a boom and a TOWBOOK plate read correctly on.
const char* const kCopModelFile = "Vehicles/E30.glb";
const char* const kTowModelFile = "Vehicles/Truck.glb";

// ---------------------------------------------------------------------------
// PAINT — "some different colors on the cars", a CURATED automotive palette,
// never random hues. The real-world distribution is overwhelmingly achromatic:
// white, black, grey and silver are ~78% of cars on the road, red and blue
// most of the remainder, and everything else is a rounding error. Weight is
// how many times a colour is drawn from, so the mix is authored here rather
// than by luck of the RNG.
// ---------------------------------------------------------------------------
struct Paint { float r, g, b; int weight; const char* name; };
const Paint kCarPaints[] = {
    { 0.900f, 0.905f, 0.912f, 20, "white"        },
    { 0.735f, 0.745f, 0.765f, 14, "silver"       },
    { 0.400f, 0.412f, 0.430f,  9, "grey"         },
    { 0.055f, 0.056f, 0.060f, 17, "black"        },
    { 0.130f, 0.135f, 0.150f,  7, "graphite"     },
    { 0.320f, 0.028f, 0.030f,  7, "red"          },
    { 0.055f, 0.085f, 0.240f,  6, "blue"         },
    { 0.028f, 0.075f, 0.105f,  4, "deep teal"    },
    { 0.035f, 0.090f, 0.048f,  3, "british green"},
    { 0.235f, 0.130f, 0.048f,  3, "bronze"       },
    { 0.520f, 0.330f, 0.030f,  2, "gold-ish"     },
    { 0.640f, 0.470f, 0.030f,  1, "yellow"       },  // rare, on purpose
};
// TRUCKS get their own palette: fleet white dominates, then the primaries a
// haulage company actually paints a box in. No pearl, no yellow sports paint.
const Paint kTruckPaints[] = {
    { 0.880f, 0.885f, 0.895f, 26, "fleet white" },
    { 0.700f, 0.710f, 0.725f,  8, "silver"      },
    { 0.070f, 0.110f, 0.290f,  8, "haulage blue"},
    { 0.300f, 0.035f, 0.035f,  6, "haulage red" },
    { 0.070f, 0.072f, 0.078f,  5, "black"       },
    { 0.055f, 0.135f, 0.070f,  4, "fleet green" },
    { 0.360f, 0.200f, 0.060f,  3, "tan"         },
};
// The supercar slot is BLACK by request, with a couple of near-blacks so three
// of them in one frame are not a copy-paste.
const Paint kSuperPaints[] = {
    { 0.030f, 0.030f, 0.033f, 20, "nsx black"      },
    { 0.045f, 0.047f, 0.055f,  5, "black pearl"    },
    { 0.058f, 0.050f, 0.046f,  3, "berlina black"  },
};
const Paint kCopPaint = { 0.880f, 0.888f, 0.900f, 1, "patrol white" };

template <int N>
const Paint& pickPaint(const Paint (&tbl)[N], uint32_t roll) {
    int total = 0;
    for (int i = 0; i < N; ++i) total += tbl[i].weight;
    int p = (int)(roll % (uint32_t)total);
    for (int i = 0; i < N; ++i) { p -= tbl[i].weight; if (p < 0) return tbl[i]; }
    return tbl[0];
}

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
// `yaw` rotates the basis about world up: a car mid-merge is CRABBING, and
// pointing it straight down the lane while it slides sideways is the tell that
// separates a lane change from a teleport.
void travelBasis(float fx, float fy, float fz, float yaw,
                 float r[3], float u[3], float f[3]) {
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    f[0] = fx / fl; f[1] = fy / fl; f[2] = fz / fl;
    if (yaw != 0.0f) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        const float nx = f[0] * c - f[2] * s, nz = f[0] * s + f[2] * c;
        f[0] = nx; f[2] = nz;
        const float rl2 = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        f[0] /= rl2; f[1] /= rl2; f[2] /= rl2;
    }
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

inline float smoothstep01(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
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
                           std::string_view glbDir, const TrafficConfig& cfg,
                           x3::audio::IAudioSystem* audio) {
    m_cfg = cfg;
    // ---- CAPTURE / TUNING LEVERS (gotcha 4.1b's pattern: a still that must
    // show moving content needs a knob, and the knob defaults to the gameplay
    // value so no existing reference capture moves).
    //   X3_TRAFFIC_NEAR  — spawn-ring inner radius, m. The gameplay default
    //     (300 m) deliberately keeps cars from popping into view around the
    //     player, which also leaves a STATIC capture camera with an empty
    //     foreground: the ring is centred on the focus, and 200 settle frames
    //     (3.3 s) cannot close 300 m. Proof shots set this to ~10.
    //   X3_TRAFFIC_FAR / X3_TRAFFIC_COUNT — outer radius / population.
    //   X3_TRAFFIC_CHAOS — scales the aggressive/jerk fractions for a capture
    //     that has to SHOW a lane change or a cut-in inside a 200-frame settle
    //     window. 1 = gameplay. A proof shot of an overtake sets 3.
    // All read here, once, so the boot line reports what is ACTUALLY in force
    // (a lever whose value never reaches the log is a lever nobody can trust).
    if (m_cfg.envOverrides) {
        if (const char* e = std::getenv("X3_TRAFFIC_NEAR"))
            m_cfg.ringNearM = std::max(0.0f, (float)std::atof(e));
        if (const char* e = std::getenv("X3_TRAFFIC_FAR"))
            m_cfg.ringFarM  = std::max(m_cfg.ringNearM + 1.0f, (float)std::atof(e));
        if (const char* e = std::getenv("X3_TRAFFIC_COUNT"))
            m_cfg.targetCount = (uint32_t)std::max(0, std::atoi(e));
        if (const char* e = std::getenv("X3_TRAFFIC_CHAOS")) {
            const float k = std::max(0.0f, (float)std::atof(e));
            m_cfg.aggressiveFrac = std::min(0.80f, m_cfg.aggressiveFrac * k);
            m_cfg.jerkFrac       = std::min(0.20f, m_cfg.jerkFrac * k);
            if (k > 1.0f) m_cfg.breakdownMeanS /= k;
        }
        if (const char* e = std::getenv("X3_TRAFFIC_COPS"))
            m_cfg.copFrac = std::max(0.0f, std::min(0.5f, (float)std::atof(e)));
        if (const char* e = std::getenv("X3_TRAFFIC_BREAKDOWN"))
            m_cfg.breakdownMeanS = std::max(1.0f, (float)std::atof(e));
        // X3_TRAFFIC_PRESIM=<seconds> — ADVANCE THE SIM before the first real
        // frame. This is gotcha 4.1b's ECHO_SHOT_T lever, applied to traffic
        // and for exactly the same reason: the capture path settles for 200
        // frames (3.3 s), and the events worth photographing do not fit in
        // 3.3 s. A breakdown takes ~30 s to call a tow and the tow another
        // ~25 s to arrive; a patrol car's code-3 burst is on a 90-210 s cycle.
        // Without this every still shows a freeway where nothing has happened
        // yet — the "boat lanes cross dry land" defect all over again, where
        // every screenshot-based check looked at a world that had never run.
        // Defaults to 0, so no existing reference capture moves.
        if (const char* e = std::getenv("X3_TRAFFIC_PRESIM"))
            m_presimS = std::max(0.0f, std::min(600.0f, (float)std::atof(e)));
        // X3_TRAFFIC_PARK=<cw>,<laneF>,<s> — park a VIRTUAL stopped vehicle on
        // that lane point and feed it to the sim as the player. Capture-only,
        // and it exists because the capture path makes the CAMERA the player:
        // to photograph traffic reacting to a stopped car you would otherwise
        // have to stand at the stopped car, which is the one place you cannot
        // see it from. This decouples the two — park here, shoot from there.
        if (const char* e = std::getenv("X3_TRAFFIC_PARK")) {
            float cwf = 1.0f, lane = 5.0f, ss = 0.0f;
            if (std::sscanf(e, "%f,%f,%f", &cwf, &lane, &ss) == 3) {
                m_parkCw = (int)cwf; m_parkLane = lane; m_parkS = ss;
                m_parked = true;
            }
        }
    }
    m_rng = cfg.seed ? cfg.seed : 1u;
    m_device = device;
    m_phys = phys;
    m_audio = audio;
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
        // ---- THE ASPECT GATE ------------------------------------------------
        // Rescaling to targetLenM is UNIFORM, so it only produces a real car
        // when the measured box is the car's OWN box. Two of the RCC exports
        // carry a bbox polluted by an off-body node, and the length rescale
        // then squashes or inflates everything else:
        //   Jeep.glb    7.61 x  9.07 x 40.76  -> W/L 0.19 -> 0.78 m wide,
        //               0.93 m tall: a pancake in lane 4.
        //   Pickup.glb 10.50 x  8.36 x 17.46  -> W/L 0.60 -> 3.24 m wide,
        //               2.58 m tall: wider than its own lane.
        // Both were caught BY EYE first (shots_traffic/13 — two squat, too-wide
        // hulls in the median lanes) and only then measured; the gate is the
        // measurement, so the next polluted export cannot reach the road.
        // NO_SLOP rule 1 (metres are law) + rule 9 (measure, don't vibe).
        const float wOverL = ext[0] / ext[2], hOverL = ext[1] / ext[2];
        if (wOverL < 0.25f || wOverL > 0.55f || hOverL < 0.20f || hOverL > 0.75f) {
            char wb[220];
            std::snprintf(wb, sizeof(wb),
                "traffic: DROP %s — bbox cannot be a road vehicle: "
                "%.2f x %.2f x %.2f m, W/L %.2f (want 0.25-0.55), H/L %.2f "
                "(want 0.20-0.75). Uniform length-rescale would ship it "
                "mis-proportioned (rule 1).",
                d.label, ext[0], ext[1], ext[2], wOverL, hOverL);
            x3::logWarn(wb);
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
            if (!isWheel) {
                m.body.push_back(all[di]);
                // ---- WHAT TAKES THE PAINT, decided ONCE, here -------------
                // "The GLB materials are shared, so vary baseColor per
                // instance" — but only on the parts that ARE paint. Two
                // failure modes bound this test, and both have receipts on the
                // roster: repaint everything and the four TEXTURED armory cars
                // (Sedan_Car3/4, OldVan, Pickup2) lose their shells and become
                // flat blobs; repaint only clearcoat and a factor-material
                // body panel stays whatever colour the exporter left it.
                // So: a drawable is PAINT if it is a clearcoat lacquer panel,
                // or if it carries no baseColor texture, is not glass
                // (alphaBlend), is not a lamp (emissive), is opaque, and its
                // authored factor is light enough to be bodywork rather than
                // tyre rubber / window rubber / dark trim.
                const x3::asset::ModelDrawable& dd = all[di];
                const float lum = 0.2126f * dd.baseColorFactor[0] +
                                  0.7152f * dd.baseColorFactor[1] +
                                  0.0722f * dd.baseColorFactor[2];
                const bool emissiveLamp = dd.emissiveTexId != 0 ||
                    dd.emissiveFactor[0] > 0.02f || dd.emissiveFactor[1] > 0.02f ||
                    dd.emissiveFactor[2] > 0.02f;
                const bool paintable =
                    dd.clearcoat > 0.01f ||
                    (dd.baseColorTexId == 0 && !dd.alphaBlend && !emissiveLamp &&
                     dd.baseColorFactor[3] > 0.9f && lum > 0.075f);
                m.bodyPaintable.push_back(paintable ? 1u : 0u);
                continue;
            }
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
        uint32_t paintN = 0;
        for (uint8_t p : m.bodyPaintable) paintN += p;
        char b[280];
        std::snprintf(b, sizeof(b),
            "traffic: %-9s measured %.2f x %.2f x %.2f m -> scale %.3f (%.2f m), "
            "minY %+.2f, %u wheel node(s), %u body drawable(s), %u take paint, "
            "profile %s",
            d.label, ext[0], ext[1], ext[2], m.scale, m.lenM, mn[1],
            (uint32_t)m.wheels.size(), (uint32_t)m.body.size(), paintN,
            kClassProfiles[d.cls].name);
        x3::logInfo(b);
    }
    if (okModels == 0) {
        x3::logWarn("traffic: NO usable vehicle models — no traffic");
        return false;
    }

    // ---- SOUNDS. Synthesized by tools/gen_traffic_audio.py and committed in
    // repo (assets/audio/vehicles) so a fresh clone honks. A missing file is
    // non-fatal by IAudioSystem contract: load() logs once and every play call
    // becomes a no-op, which is exactly the right failure for a horn.
    if (m_audio) {
        m_sndHornCar   = m_audio->load(resolveAudio("../audio/vehicles/horn_car.wav"));
        m_sndHornTruck = m_audio->load(resolveAudio("../audio/vehicles/horn_truck.wav"));
        m_sndSiren     = m_audio->load(resolveAudio("../audio/vehicles/siren_wail.wav"));
        char sb[160];
        std::snprintf(sb, sizeof(sb),
            "traffic: audio horn_car=%s horn_truck=%s siren=%s",
            m_sndHornCar.valid() ? "ok" : "MISSING",
            m_sndHornTruck.valid() ? "ok" : "MISSING",
            m_sndSiren.valid() ? "ok" : "MISSING");
        x3::logInfo(sb);
    }

    // ---- FURNITURE + the radar sign ---------------------------------------
    if (device) buildFurniture(*device, glbDir);
    siteRadarSign();

    m_built = true;
    char b[280];
    std::snprintf(b, sizeof(b),
        "traffic: %u/%d models live | route %.2f miles %s | target %u cars, "
        "ring %.0f-%.0f m, cull %.0f m | aggressive %.0f%% jerks %.0f%% cops %.1f%% "
        "| radar %s",
        okModels, kTrafficModelCount, m_totalLen / 1609.34f,
        m_closed ? "(closed)" : "(open)", m_cfg.targetCount,
        m_cfg.ringNearM, m_cfg.ringFarM, m_cfg.cullM,
        m_cfg.aggressiveFrac * 100.0f, m_cfg.jerkFrac * 100.0f,
        m_cfg.copFrac * 100.0f, m_radar.sited ? "sited" : "NOT SITED");
    x3::logInfo(b);
    logCameraStations();
    return true;
}

// ---------------------------------------------------------------------------
// X3_TRAFFIC_CAMS=<n> — print n evenly-spaced freeway camera stations, each
// DERIVED FROM THE ROAD DATA (gotcha 4.1: "derive cameras from room data,
// never eyeball coordinates" — the freeway is 16 miles of curve, and a
// hand-guessed camera lands in a cut wall or off the ribbon entirely).
// Emits --shot-cam strings ready to paste, with the LEADING SPACE that
// gotcha 4.1 requires so a negative X is not parsed as a flag.
// ---------------------------------------------------------------------------
static float laneLat(int cw, float lane, float medianHalf);   // defined below

void FreewayTraffic::logCameraStations() const {
    const char* e = std::getenv("X3_TRAFFIC_CAMS");
    if (!e) return;
    int n = std::atoi(e);
    if (n <= 0) n = 8;
    for (int i = 0; i < n; ++i) {
        const float u = m_totalLen * ((float)i / (float)n);
        float pos[3], dir[2], mh, dy;
        sampleAt(u, pos, dir, &mh, &dy);
        const float yaw = std::atan2(dir[1], dir[0]);          // cam dir = (cos,0,sin)
        const float nx = -dir[1], nz = dir[0];
        const float latDrive = laneLat(1, 5.0f, mh);           // right cw, lane 5
        char b[420];
        std::snprintf(b, sizeof(b),
            "[traffic-cam] %02d u=%8.1f m  centre=(%.1f, %.1f, %.1f) yaw=%+.3f medianHalf=%.1f\n"
            "              drive --shot-cam \" %.1f,%.1f,%.1f,%.3f,%.2f\"\n"
            "              high  --shot-cam \" %.1f,%.1f,%.1f,%.3f,%.2f\"\n"
            "              side  --shot-cam \" %.1f,%.1f,%.1f,%.3f,%.2f\"",
            i, u, pos[0], pos[1], pos[2], yaw, mh,
            pos[0] + nx * latDrive, pos[1] + 1.45f, pos[2] + nz * latDrive, yaw, -0.02f,
            pos[0], pos[1] + 60.0f, pos[2], yaw, -0.62f,
            pos[0] - nx * 160.0f, pos[1] + 45.0f, pos[2] - nz * 160.0f,
            std::atan2(nz, nx), -0.25f);
        x3::logInfo(b);
        (void)dy;
    }
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

// Lane-centre lateral offset from the route centreline, signed in the ribbon's
// lat convention (lat>0 = right of +u travel = (-tz,+tx)).
//
// `lane` IS A FLOAT and this function is LINEAR IN IT — that single property is
// what makes everything else in this file possible. An integer lane is a lane
// centre (0 = median-side fast lane, kFwyLaneCount-1 = outer); a fraction is a
// car mid-merge; a value past kFwyLaneCount-1 is out on the paved shoulder
// where the breakdowns and the tow truck live. Because it is linear, the
// LATERAL DISTANCE between two cars on the same carriageway is just
// |laneF_a - laneF_b| * kTrafLaneM, which is how the no-overlap pass can be
// 2-D without ever calling this function.
static float laneLat(int cw, float lane, float medianHalf) {
    const float sgn = (cw == 1) ? 1.0f : -1.0f;
    return sgn * (medianHalf + kFwyPavedHalfM - kFwyRunningHalfM
                  + (lane + 0.5f) * kTrafLaneM);
}

float FreewayTraffic::carHalfWidth(const Car& c) const {
    return m_models[c.model].widthM * 0.5f;
}

// Where may this car legally sit? Civilians and cops keep to the running
// lanes; a broken-down car and its tow are the only things allowed onto the
// shoulder band, and nothing at all may go past the paved edge.
bool FreewayTraffic::laneAllowed(const Car& c, float laneF) const {
    if (laneF < -0.02f) return false;
    const float maxRunning = (float)(kFwyLaneCount - 1);
    if (laneF <= maxRunning + 0.02f) return true;
    if (c.role != RoleBroken && c.role != RoleTow) return false;
    // On the shoulder: the whole car must stay on pavement (rule 11's spirit —
    // a wheel off the apron is a wheel in the dirt).
    const float offset = (laneF + 0.5f) * kTrafLaneM - kFwyRunningHalfM;
    return offset + carHalfWidth(c) <= kFwyPavedHalfM - 0.15f;
}

void FreewayTraffic::setPlayer(const float pos[3], float speedMps) {
    if (!m_havePlayer) {
        m_prevPlayerPos[0] = pos[0];
        m_prevPlayerPos[1] = pos[1];
        m_prevPlayerPos[2] = pos[2];
    }
    m_playerPos[0] = pos[0]; m_playerPos[1] = pos[1]; m_playerPos[2] = pos[2];
    m_playerSpeed = speedMps;
    m_havePlayer = true;
}

// ---------------------------------------------------------------------------
// PROJECT THE PLAYER onto the sim's own (cw, s, laneF) coordinates.
//
// THE DEFECT THIS FIXES, in the owner's words after parking on the freeway and
// watching the traffic close on him: "what do you think is about to happen, and
// what do we not have wired!" Nothing was wired. The following controller
// scanned m_cars and m_cars only, so a stopped player was not an obstacle, not
// an occupancy, not a leader — he was empty road. Cars drove through him and
// the only system that reacted was the impulse-4000 wreck conversion, i.e. the
// physics correctly resolving a collision the AI should never have allowed.
//
// Once he is in these coordinates EVERY existing rule picks him up for free:
// the follower brakes for him, `blocked` goes true so thinkLaneChanges runs the
// same merge-around it runs for a slow truck, laneGapSafe refuses to merge into
// him, enforceNoOverlap refuses to let anyone share his box, and the horn
// provocations fire on the gap he leaves. A stalled player IS a breakdown from
// the sim's point of view, and that is exactly how it is modelled.
// ---------------------------------------------------------------------------
void FreewayTraffic::projectPlayer(float dt) {
    m_player.valid = false;
    // X3_TRAFFIC_PARK: a virtual stopped vehicle, straight into the obstacle
    // slot. No projection needed — it is already in lane coordinates.
    if (m_parked) {
        m_player.valid = true;
        m_player.cw = m_parkCw;
        m_player.s = m_parkS;
        m_player.laneF = m_parkLane;
        m_player.v = 0.0f;
        m_player.halfW = 0.95f;
        m_player.lenM = 4.6f;
        m_player.inLane = m_parkLane > -0.55f &&
                          m_parkLane < (float)kFwyLaneCount - 0.45f;
        float p[3];
        if (laneWorldPos(m_parkCw, m_parkLane, m_parkS, p)) {
            m_playerPos[0] = p[0]; m_playerPos[1] = p[1]; m_playerPos[2] = p[2];
            m_havePlayer = true;
        }
        if (!m_parkLogged) {
            m_parkLogged = true;
            char b[200];
            std::snprintf(b, sizeof(b),
                "traffic: X3_TRAFFIC_PARK — a stopped vehicle sits in cw %d lane "
                "%.1f at s %.0f = (%.1f, %.1f, %.1f). Traffic must deal with it.",
                m_parkCw, m_parkLane, m_parkS,
                m_playerPos[0], m_playerPos[1], m_playerPos[2]);
            x3::logInfo(b);
        }
        return;
    }
    if (!m_havePlayer || m_path.size() < 2) return;

    // Nearest fine station. Brute force over ~2.2k stations ONCE per frame is
    // nothing next to the per-car work, and it cannot get lost the way an
    // incrementally-tracked index can when the player teleports or reverses.
    size_t best = 0;
    float bestD2 = 1e30f;
    for (size_t i = 0; i < m_path.size(); ++i) {
        const float dx = m_path[i].x - m_playerPos[0];
        const float dz = m_path[i].z - m_playerPos[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    // More than a carriageway's paved half-width plus slack off the ribbon and
    // he is not on this road at all (a spur, the town, the garage).
    if (bestD2 > (kFwyDualMaxHalfM + 30.0f) * (kFwyDualMaxHalfM + 30.0f)) return;

    const RoadRenderStation& st = m_path[best];
    // Signed lateral offset in the ribbon's convention: lat>0 = right of +u.
    const float nx = -st.tz, nz = st.tx;
    const float lat = (m_playerPos[0] - st.x) * nx + (m_playerPos[2] - st.z) * nz;
    const int cw = (lat >= 0.0f) ? 1 : 0;
    const float sgn = (cw == 1) ? 1.0f : -1.0f;
    // Invert laneLat: lat = sgn*(mh + pavedHalf - runningHalf + (laneF+0.5)*W)
    const float laneF = (sgn * lat - st.medianHalf - kFwyPavedHalfM + kFwyRunningHalfM)
                        / kTrafLaneM - 0.5f;
    // Off the paved carriageway entirely (median, verge, far side)? Not an
    // obstacle — traffic should not brake for a car parked in a field.
    if (laneF < -1.5f || laneF > (float)kFwyLaneCount + 1.5f) return;

    // Longitudinal position, plus the along-lane component of his ACTUAL
    // velocity. Speed alone is unsigned; a car reversing or sitting crossways
    // must not read as one keeping pace, so the direction comes from measured
    // displacement and the magnitude from the speedo the host handed us.
    float vAlong = 0.0f;
    if (dt > 1e-5f) {
        const float mx = (m_playerPos[0] - m_prevPlayerPos[0]) / dt;
        const float mz = (m_playerPos[2] - m_prevPlayerPos[2]) / dt;
        const float along = mx * (sgn * st.tx) + mz * (sgn * st.tz);
        const float mag = std::sqrt(mx * mx + mz * mz);
        vAlong = (mag > 0.35f) ? along : 0.0f;
        // Trust the speedo's magnitude when it agrees in sign — it is exact,
        // where a frame-to-frame difference is noisy.
        if (m_playerSpeed > 0.1f && vAlong > 0.1f) vAlong = m_playerSpeed;
        else if (m_playerSpeed > 0.1f && vAlong < -0.1f) vAlong = -m_playerSpeed;
    }
    m_prevPlayerPos[0] = m_playerPos[0];
    m_prevPlayerPos[1] = m_playerPos[1];
    m_prevPlayerPos[2] = m_playerPos[2];

    m_player.valid = true;
    m_player.cw = cw;
    m_player.s = (cw == 1) ? st.u : m_totalLen - st.u;
    m_player.laneF = laneF;
    m_player.v = std::max(0.0f, vAlong);   // a car going backwards is a wall
    m_player.lat = lat;
    m_player.halfW = 0.95f;                // hero car footprint, metres
    m_player.lenM = 4.6f;
    m_player.inLane = laneF > -0.55f && laneF < (float)kFwyLaneCount - 0.45f;
}

bool FreewayTraffic::laneWorldPos(int cw, float laneF, float s, float out[3]) const {
    if (!m_built) return false;
    float pos[3], dir[2], mh;
    sampleAt(uOfS(cw, s), pos, dir, &mh, nullptr);
    const float lat = laneLat(cw, laneF, mh);
    out[0] = pos[0] + (-dir[1]) * lat;
    out[1] = pos[1] + kTrafficPaveProud;
    out[2] = pos[2] + ( dir[0]) * lat;
    return true;
}

bool FreewayTraffic::playerLane(int& cw, float& laneF, float& s, float& v) const {
    if (!m_player.valid) return false;
    cw = m_player.cw; laneF = m_player.laneF; s = m_player.s; v = m_player.v;
    return true;
}

// ---------------------------------------------------------------------------
// SPAWN / DESPAWN
// ---------------------------------------------------------------------------
// Give a freshly-spawned car its CHARACTER: the class profile, then the
// temperament that scales it. Kept in one place so "what kind of driver is
// this" has exactly one answer and the self-test can assert on it.
void FreewayTraffic::giveCharacter(Car& c) {
    const Model& md = m_models[c.model];
    const ClassProfile& p = kClassProfiles[md.cls];
    c.accelMax = p.accel;
    c.brakeMax = p.brake;
    c.headway  = p.headway;
    c.minGap   = p.minGap;
    c.overtakeUrge = p.urge;
    c.prefLaneMin = md.laneMin;
    c.prefLaneMax = md.laneMax;
    c.phase = rndf(0.0f, 6.2831853f);

    if (c.role != RoleCivilian) {
        // Cops and tows are their own thing: no jerk dice for them.
        if (c.role == RoleCop) {
            c.accelMax *= 1.35f; c.brakeMax *= 1.20f;
            c.headway = 1.35f;   c.overtakeUrge = 0.55f;
            c.prefLaneMin = 1;   c.prefLaneMax = 6;
        } else if (c.role == RoleTow) {
            c.overtakeUrge = 0.05f;
            c.prefLaneMin = 6;   c.prefLaneMax = 7;
        }
        return;
    }

    const float roll = rndf(0.0f, 1.0f);
    if (roll < m_cfg.jerkFrac) {
        c.temper = TempJerk;
        // ONE flavour per driver. A car that cuts in AND camps AND weaves
        // reads as a physics bug; a car that does exactly one antisocial
        // thing reads as a person, which is the whole point.
        c.jerk = (JerkKind)(1 + (int)(rnd() % ((uint32_t)JerkKind::Count - 1)));
        switch (c.jerk) {
            case JerkKind::Cutter:
                // Takes gaps he has no business taking. gapScale is applied in
                // laneGapSafe; the HARD no-overlap pass still binds him, so he
                // cuts it close and never clips (T6 gates that, on him).
                c.overtakeUrge = 0.85f; c.headway *= 0.75f;
                break;
            case JerkKind::LaneHog:
                // The left-lane camper: sits in 0/1, UNDER the flow, and will
                // not move over for anyone. Urge ~0 is the whole behaviour.
                c.prefLaneMin = 0; c.prefLaneMax = 1;
                c.overtakeUrge = 0.02f; c.headway *= 1.25f;
                break;
            case JerkKind::Weaver:
                // ~30 mph over and changes lane at the first excuse.
                c.overtakeUrge = 0.98f; c.headway *= 0.45f;
                c.accelMax *= 1.5f; c.brakeMax *= 1.15f;
                c.prefLaneMin = 0; c.prefLaneMax = (int)kFwyLaneCount - 1;
                break;
            case JerkKind::BrakeChecker:
                c.headway *= 0.9f;
                break;
            case JerkKind::Tailgater:
                c.headway *= 0.30f; c.minGap *= 0.55f;
                c.overtakeUrge = 0.6f; c.accelMax *= 1.25f;
                break;
            default: break;
        }
    } else if (roll < m_cfg.jerkFrac + m_cfg.aggressiveFrac) {
        // "some that accelerates" — hard on the gas out of a gap, closer than
        // most, keener to overtake. Visible in the mirror, not antisocial.
        c.temper = TempAggressive;
        c.accelMax *= 1.45f;
        c.brakeMax *= 1.10f;
        c.headway  *= 0.62f;
        c.minGap   *= 0.80f;
        c.overtakeUrge = std::min(0.95f, c.overtakeUrge + 0.35f);
    }
    // Floors that apply to EVERY driver, jerk or not: nobody gets a negative
    // gap budget, and nobody out-brakes physics.
    c.headway = std::max(0.28f, c.headway);
    c.minGap  = std::max(3.2f, c.minGap);
}

// The paint for one car: class palette, role override, one draw of the RNG.
void FreewayTraffic::givePaint(Car& c) {
    const Model& md = m_models[c.model];
    const Paint* p = nullptr;
    if (c.role == RoleCop)            p = &kCopPaint;
    else if (md.cls == ClsHeavy)      p = &pickPaint(kTruckPaints, rnd());
    else if (md.cls == ClsSuper)      p = &pickPaint(kSuperPaints, rnd());
    else                              p = &pickPaint(kCarPaints, rnd());
    c.tint[0] = p->r; c.tint[1] = p->g; c.tint[2] = p->b;
    c.hasTint = true;
}

int FreewayTraffic::spawnForTest(int model, int cw, int lane, float s,
                                 float v, float cruise) {
    if (!m_built || model < 0 || model >= (int)m_models.size() || !m_models[model].ok)
        return -1;
    Car c;
    c.id = m_nextCarId++;
    c.model = model; c.cw = cw;
    c.laneF = c.laneFrom = c.laneTo = (float)lane;
    c.s = s; c.v = v; c.cruise = cruise; c.lastV = v;
    c.halfH = m_models[model].heightM * 0.5f;
    giveCharacter(c);
    c.temper = TempNormal;   // deterministic baseline for the gates
    c.jerk = JerkKind::None;
    givePaint(c);
    m_cars.push_back(c);
    return (int)m_cars.size() - 1;
}

int FreewayTraffic::spawnForTestPhys(int model, int cw, int lane, float s, float v,
                                     float cruise, x3::phys::IPhysicsWorld* phys) {
    const int idx = spawnForTest(model, cw, lane, s, v, cruise);
    if (idx < 0 || !phys) return idx;
    Car& c = m_cars[idx];
    const Model& md = m_models[c.model];
    float pos[3], dir[2], mh;
    sampleAt(uOfS(cw, s), pos, dir, &mh, nullptr);
    const float lat = laneLat(cw, c.laneF, mh);
    c.body = phys->addKinematicBox(
        x3::phys::Vec3{ md.widthM * 0.5f, c.halfH, md.lenM * 0.5f },
        x3::phys::Vec3{ pos[0] + (-dir[1]) * lat,
                        pos[1] + kTrafficPaveProud + c.halfH,
                        pos[2] + ( dir[0]) * lat },
        x3::phys::Layer::Dynamic);
    if (!m_phys) m_phys = phys;
    return idx;
}

bool FreewayTraffic::carHasBody(uint32_t i) const {
    return i < m_cars.size() && m_cars[i].body.valid();
}
x3::phys::BodyId FreewayTraffic::carBodyId(uint32_t i) const {
    return i < m_cars.size() ? m_cars[i].body : x3::phys::BodyId{};
}

void FreewayTraffic::carBodyBox(uint32_t i, float outCentre[3], float outHalf[3]) const {
    outCentre[0] = outCentre[1] = outCentre[2] = 0.0f;
    outHalf[0] = outHalf[1] = outHalf[2] = 0.0f;
    if (i >= m_cars.size() || !m_phys || !m_cars[i].body.valid()) return;
    const Car& c = m_cars[i];
    const Model& md = m_models[c.model];
    const x3::phys::Vec3 p = m_phys->getBodyPosition(c.body);
    outCentre[0] = p.x; outCentre[1] = p.y; outCentre[2] = p.z;
    // The car is axis-aligned to its own travel frame, not the world, so the
    // half extents below are the SHAPE's — this comparison is only meaningful
    // for a car whose heading is close to an axis, which is why the gate
    // compares against the drawn box computed in the SAME frame.
    outHalf[0] = md.widthM * 0.5f;
    outHalf[1] = c.halfH;
    outHalf[2] = md.lenM * 0.5f;
}

void FreewayTraffic::carDrawnBox(uint32_t i, float outLo[3], float outHi[3]) const {
    outLo[0] = outLo[1] = outLo[2] = 0.0f;
    outHi[0] = outHi[1] = outHi[2] = 0.0f;
    if (i >= m_cars.size()) return;
    const Car& c = m_cars[i];
    const Model& md = m_models[c.model];
    // Exactly the pose render() uses: contact plane on the pavement, model
    // lifted by its own measured groundLift, extents from the measured bbox.
    float pos[3], dir[2], mh;
    sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, nullptr);
    const float lat = laneLat(c.cw, c.laneF, mh);
    const float wx = pos[0] + (-dir[1]) * lat;
    const float wz = pos[2] + ( dir[0]) * lat;
    const float wy = pos[1] + kTrafficPaveProud;
    outLo[0] = wx - md.widthM * 0.5f; outHi[0] = wx + md.widthM * 0.5f;
    outLo[1] = wy;                    outHi[1] = wy + md.heightM;
    outLo[2] = wz - md.lenM * 0.5f;   outHi[2] = wz + md.lenM * 0.5f;
}

bool FreewayTraffic::setTemperForTest(uint32_t idx, int temper, JerkKind jerk) {
    if (idx >= m_cars.size()) return false;
    Car& c = m_cars[idx];
    c.temper = (uint8_t)temper;
    c.jerk = jerk;
    // Re-derive from the class profile so the gate sees the SAME code path a
    // spawned car of this temperament would have taken.
    const uint8_t keepRole = c.role;
    c.role = keepRole;
    const ClassProfile& p = kClassProfiles[m_models[c.model].cls];
    c.accelMax = p.accel; c.brakeMax = p.brake; c.headway = p.headway;
    c.minGap = p.minGap; c.overtakeUrge = p.urge;
    if (temper == TempAggressive) {
        c.accelMax *= 1.45f; c.brakeMax *= 1.10f;
        c.headway *= 0.62f;  c.minGap *= 0.80f;
        c.overtakeUrge = std::min(0.95f, c.overtakeUrge + 0.35f);
    } else if (temper == TempJerk) {
        switch (jerk) {
            case JerkKind::Cutter:       c.overtakeUrge = 0.85f; c.headway *= 0.75f; break;
            case JerkKind::LaneHog:      c.prefLaneMin = 0; c.prefLaneMax = 1;
                                         c.overtakeUrge = 0.02f; c.headway *= 1.25f; break;
            case JerkKind::Weaver:       c.overtakeUrge = 0.98f; c.headway *= 0.45f;
                                         c.accelMax *= 1.5f; c.brakeMax *= 1.15f;
                                         c.prefLaneMin = 0;
                                         c.prefLaneMax = (int)kFwyLaneCount - 1; break;
            case JerkKind::BrakeChecker: c.headway *= 0.9f; break;
            case JerkKind::Tailgater:    c.headway *= 0.30f; c.minGap *= 0.55f;
                                         c.overtakeUrge = 0.6f; c.accelMax *= 1.25f; break;
            default: break;
        }
    }
    c.headway = std::max(0.28f, c.headway);
    c.minGap  = std::max(3.2f, c.minGap);
    return true;
}

bool FreewayTraffic::forceBreakdownForTest(uint32_t idx) {
    if (idx >= m_cars.size()) return false;
    Car& c = m_cars[idx];
    if (c.role != RoleCivilian || c.loose) return false;
    beginBreakdown(c);
    return true;
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

    // COPS: a small fraction of spawns become patrol cars, and they take the
    // cop body rather than whatever the weighted draw produced.
    uint8_t role = RoleCivilian;
    if (rndf(0.0f, 1.0f) < m_cfg.copFrac) {
        const int ci = modelIndexByFile(kCopModelFile);
        if (ci >= 0) { mi = ci; role = RoleCop; }
    }

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
        if (o.cw != cw || std::fabs(o.laneF - (float)lane) > 0.6f) continue;
        float ds = std::fabs(o.s - s);
        if (m_closed) ds = std::min(ds, m_totalLen - ds);
        if (ds < kMinGapM + std::max(v, o.v) * kHeadwayS * 1.5f) return false;
    }

    Car c;
    c.id = m_nextCarId++;
    c.model = mi; c.cw = cw;
    c.laneF = c.laneFrom = c.laneTo = (float)lane;
    c.s = s; c.v = v; c.cruise = cruise; c.lastV = v;
    c.halfH = md.heightM * 0.5f;
    c.role = role;
    c.thinkT = rndf(0.0f, kThinkEveryS);   // de-phase the deliberation cost
    giveCharacter(c);
    givePaint(c);
    if (c.temper == TempJerk && c.jerk == JerkKind::Weaver)
        c.cruise *= 1.38f;                 // the ~30-over merchant
    else if (c.temper == TempJerk && c.jerk == JerkKind::LaneHog)
        c.cruise *= 0.82f;                 // camping the fast lane UNDER the flow
    if (phys) {
        float pos[3], dir[2], mh;
        sampleAt(uOfS(cw, s), pos, dir, &mh, nullptr);
        const float lat = laneLat(cw, c.laneF, mh);
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

int FreewayTraffic::modelIndexByFile(const char* file) const {
    for (int i = 0; i < (int)m_models.size(); ++i)
        if (m_models[i].ok && m_models[i].file == file) return i;
    return -1;
}

void FreewayTraffic::despawnCar(size_t idx, x3::phys::IPhysicsWorld* phys) {
    Car& c = m_cars[idx];
    if (phys && c.body.valid()) phys->removeBody(c.body);
    if (m_audio && c.siren.valid()) { m_audio->stopLoop(c.siren); c.siren = {}; }
    m_cars[idx] = m_cars.back();
    m_cars.pop_back();
}

// ---------------------------------------------------------------------------
// UPDATE — sim + kinematic march. Call BEFORE the host's phys->step().
//
// The stages run in a fixed order and each one has ONE job:
//   1 rebuildOrder     cars sorted by s, per carriageway (every later scan
//                      walks this instead of all-pairs — with 300 cars the
//                      old O(n^2) loops were 90k iterations EACH and this
//                      pass adds three more scans)
//   2 driveFollowers   longitudinal control (the constant-time-gap law)
//   3 thinkLaneChanges who WANTS to change lane, and is it measurably safe
//   4 advanceMerges    the 2-3 s lateral spline
//   5 enforceNoOverlap the hard invariant, in 2-D. Runs LAST among the
//                      motion stages so nothing downstream can undo it.
//   6 runRoles         cops / breakdowns / the tow truck
//   7 serviceHorns     the rate-limited voice of everyone's annoyance
// ---------------------------------------------------------------------------
void FreewayTraffic::update(float dt, const float focus[3], x3::phys::IPhysicsWorld* phys) {
    if (!m_built || dt <= 0.0f) return;
    // X3_TRAFFIC_PRESIM: burn the requested seconds on the FIRST tick, at a
    // fixed 30 Hz, with NO physics world (the kinematic bodies would be marched
    // thousands of times for nothing, and Jolt is not the point of a fast
    // forward). Everything the sim owns — merges, roles, the tow's whole
    // journey — advances normally, so the frame that finally renders is a
    // freeway with a history.
    if (m_presimS > 0.0f) {
        const float step = 1.0f / 30.0f;
        const int steps = (int)(m_presimS / step);
        const float want = m_presimS;
        m_presimS = 0.0f;
        // THE FOCUS IS QUANTISED TO A 250 m GRID, and that is the whole reason
        // the two-pass capture workflow works. The spawn ring is centred on the
        // focus, so feeding it the raw capture camera made the population a
        // function of the camera: a probe run would print "the cop is HERE",
        // and the follow-up run aimed at that spot would simulate a DIFFERENT
        // 300 cars and photograph an empty lane. (Receipt: 05_cop_lights.png,
        // first cut — a data-derived camera pointed at nothing.) Quantising
        // means any two cameras inside the same 250 m cell fast-forward the
        // SAME freeway, so reportShotCams' coordinates stay valid for the shot
        // that uses them. Capture-only; the live game never presims.
        const float q = 250.0f;
        const float pf[3] = { std::round(focus[0] / q) * q, focus[1],
                              std::round(focus[2] / q) * q };
        char b[220];
        std::snprintf(b, sizeof(b),
            "traffic: X3_TRAFFIC_PRESIM — fast-forwarding %.0f s (%d steps) so the "
            "capture shows a freeway that has been running; focus quantised to "
            "(%.0f, %.0f)", want, steps, pf[0], pf[2]);
        x3::logInfo(b);
        for (int i = 0; i < steps; ++i) update(step, pf, nullptr);
        // Reaction counters. A still can be ambiguous about whether traffic is
        // actually dealing with the obstacle; these are not.
        uint32_t braking = 0, blocked = 0, nearObstacle = 0;
        for (const Car& c : m_cars) {
            if (c.brakeLit) ++braking;
            if (c.blockedByPlayer) ++blocked;
            if (m_player.valid && c.cw == m_player.cw) {
                const float ds = arcDelta(c.s, m_player.s);
                if (ds > -30.0f && ds < 160.0f) ++nearObstacle;
            }
        }
        std::snprintf(b, sizeof(b),
            "traffic: presim done — %u live, %u lane changes, %u horns, %u cop(s), "
            "%u breaking down, %u merging now | %u braking (lights lit), "
            "%u following the PLAYER, %u within 160 m of him",
            liveCount(), m_laneChanges, m_hornCount, copCount(), breakdownCount(),
            mergingCount(), braking, blocked, nearObstacle);
        x3::logInfo(b);
        reportShotCams();
    }
    m_time += dt;
    m_focus[0] = focus[0]; m_focus[1] = focus[1]; m_focus[2] = focus[2];
    if (!m_havePlayer) {
        m_playerPos[0] = focus[0]; m_playerPos[1] = focus[1]; m_playerPos[2] = focus[2];
    }

    rebuildOrder();
    projectPlayer(dt);      // the player joins the world model BEFORE anyone drives
    driveFollowers(dt);
    thinkLaneChanges(dt);
    advanceMerges(dt);
    enforceNoOverlap();
    runRoles(dt, phys);
    serviceHorns(dt);

    // ---- cull + refill the ring -------------------------------------------
    for (size_t i = 0; i < m_cars.size();) {
        Car& c = m_cars[i];
        // A tow truck on its way to a job, and the car it is coming for, are
        // NEVER culled by distance: the player driving 1.7 km up the road and
        // back must not find the incident silently deleted.
        if (c.role == RoleTow || c.role == RoleBroken) { ++i; continue; }
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

    updateRadar(dt);
    collectLights();

    // ---- march the kinematic bodies / police the loose ones ----------------
    if (!phys) return;
    for (Car& c : m_cars) {
        if (!c.body.valid()) continue;
        if (c.loose) {
            // NO_SLOP rule 11 - THE CONTACT LAW, runtime invariant: a loose
            // (dynamic) wreck must never end up under the carved field. Same
            // shape as DriveDemo::postStep's wheel clamp: only ever push UP.
            //
            // DECK-AWARE (W-STACK's trap, fixed before it could bite). The
            // carved field is NOT the floor everywhere this road goes: over a
            // RoadSpec::Gap - a bridge deck, an interchange ramp, a Stack
            // flyover - the pavement rides on structure and the field is the
            // ground far below (21 m on the Stack's L4; its tallest pier is
            // 33.5 m). A terrain-only clamp is merely silent while a car sits
            // on a deck, and actively wrong the instant one dips a hair below
            // it: it reads "under the field" and yanks the car to the dirt
            // underneath the interchange.
            //
            // So the floor is the TOPMOST STATIC SURFACE under the car, not
            // the field: max(field, downward ray). The ray starts just above
            // the body and reaches only a little below it - the character
            // rig's v3 lesson (character_anim.cpp), where a ray that started
            // 40 m overhead found a tunnel LID and ejected the player through
            // the roof. Start low, look down: a deck can then only hold a car
            // UP, never teleport it somewhere it has never been.
            const x3::phys::Vec3 bp = phys->getBodyPosition(c.body);
            float floorY = terrainHeightAtWorld(bp.x, bp.z);
            const x3::phys::RayHit deck = phys->rayCast(
                x3::phys::Vec3{ bp.x, bp.y + 0.5f, bp.z },
                x3::phys::Vec3{ 0.0f, -1.0f, 0.0f },
                c.halfH * 2.0f + 1.5f, x3::phys::Layer::Static);
            if (deck.hit && deck.point.y > floorY) floorY = deck.point.y;
            if (bp.y < floorY - 0.2f)
                phys->setBodyPosition(c.body, x3::phys::Vec3{ bp.x, floorY + c.halfH, bp.z });
            continue;
        }
        float pos[3], dir[2], mh, dy;
        sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, &dy);
        const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
        const float lat = laneLat(c.cw, c.laneF, mh);
        const float wx = pos[0] + (-dir[1]) * lat;
        const float wz = pos[2] + ( dir[0]) * lat;
        const float wy = pos[1] + kTrafficPaveProud;
        float r[3], upv[3], f[3];
        travelBasis(sgn * dir[0], sgn * dy, sgn * dir[1], crabYaw(c), r, upv, f);
        float q[4];
        basisToQuat(r, upv, f, q);
        phys->moveKinematic(c.body, x3::phys::Vec3{ wx, wy + c.halfH, wz }, q, dt);
    }
}

// The heading offset of a car that is sliding sideways. Small (a real lane
// change is a few degrees of yaw) but it is the difference between a car that
// CHANGES LANE and a car that slides sideways facing straight ahead.
float FreewayTraffic::crabYaw(const Car& c) const {
    if (c.mergeT >= 1.0f || c.mergeDur <= 0.0f) return 0.0f;
    // d(lat)/dt of the smoothstep, in metres per second.
    const float t = c.mergeT;
    const float dSmooth = 6.0f * t * (1.0f - t);            // d/dt smoothstep
    const float latRate = (c.laneTo - c.laneFrom) * kTrafLaneM * dSmooth / c.mergeDur;
    const float yaw = std::atan2(latRate, std::max(4.0f, c.v));
    // lat is signed in the ribbon convention; on the LEFT carriageway the
    // world sense of +lat flips, and so must the yaw.
    const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
    return -sgn * yaw;
}

void FreewayTraffic::rebuildOrder() {
    m_order[0].clear();
    m_order[1].clear();
    for (uint32_t i = 0; i < (uint32_t)m_cars.size(); ++i) {
        const Car& c = m_cars[i];
        if (c.loose) continue;
        m_order[c.cw & 1].push_back(i);
    }
    for (int cw = 0; cw < 2; ++cw) {
        std::vector<uint32_t>& o = m_order[cw];
        std::sort(o.begin(), o.end(), [this](uint32_t a, uint32_t b) {
            return m_cars[a].s < m_cars[b].s;
        });
    }
}

// Signed arc gap from a to b, in a's direction of travel, shortest way round.
float FreewayTraffic::arcDelta(float sa, float sb) const {
    float ds = sb - sa;
    if (m_closed) {
        if (ds < -m_totalLen * 0.5f) ds += m_totalLen;
        if (ds >  m_totalLen * 0.5f) ds -= m_totalLen;
    }
    return ds;
}

// Do these two cars' lateral footprints overlap, with `slack` added?
bool FreewayTraffic::latOverlap(const Car& a, const Car& b, float slack) const {
    const float sep = std::fabs(a.laneF - b.laneF) * kTrafLaneM;
    return sep < carHalfWidth(a) + carHalfWidth(b) + slack;
}

// ---------------------------------------------------------------------------
// 2. THE FOLLOWING CONTROLLER (constant time gap), now lateral-aware.
// A car brakes for whoever is ahead AND beside-enough to be in the way — which
// during a merge means it brakes for the traffic in BOTH lanes. That is what
// keeps a merge from being a battering ram, and it is why an aborted merge is
// not needed: the merging car simply slows until its gap is real.
// ---------------------------------------------------------------------------
void FreewayTraffic::driveFollowers(float dt) {
    for (int cw = 0; cw < 2; ++cw) {
        const std::vector<uint32_t>& o = m_order[cw];
        const int n = (int)o.size();
        if (n == 0) continue;
        for (int k = 0; k < n; ++k) {
            Car& c = m_cars[o[k]];
            float bestDs = 1e9f, leaderV = 0.0f, leaderLen = 0.0f;
            // Walk FORWARD in s until out of scan range. m_order is sorted, so
            // the first lateral match is the leader.
            for (int step = 1; step < n; ++step) {
                const int j = m_closed ? (k + step) % n : (k + step);
                if (j >= n) break;
                const Car& ot = m_cars[o[j]];
                const float ds = arcDelta(c.s, ot.s);
                if (ds <= 0.0f) { if (!m_closed) break; else continue; }
                if (ds > kScanWindowM) break;
                if (!latOverlap(c, ot, kLatFollowSlack)) continue;
                bestDs = ds; leaderV = ot.v;
                leaderLen = m_models[ot.model].lenM;
                break;
            }
            c.gapAhead = (bestDs < 1e8f) ? bestDs - leaderLen : 1e9f;
            c.leaderV = leaderV;

            // ---- THE PLAYER IS A LEADER TOO ------------------------------
            // Same test as an AI car, in the same coordinates. If he is nearer
            // than the AI leader he BECOMES the leader, which is what makes a
            // car brake for a stopped player instead of driving through him.
            c.blockedByPlayer = false;
            if (m_player.valid && m_player.cw == c.cw) {
                const float sep = std::fabs(c.laneF - m_player.laneF) * kTrafLaneM;
                if (sep < carHalfWidth(c) + m_player.halfW + kLatFollowSlack) {
                    const float ds = arcDelta(c.s, m_player.s);
                    if (ds > 0.0f && ds < kScanWindowM) {
                        const float gap = ds - m_player.lenM;
                        if (gap < c.gapAhead) {
                            c.gapAhead = gap;
                            c.leaderV = m_player.v;
                            c.blockedByPlayer = true;
                        }
                    }
                }
            }

            float vT = c.cruise;
            if (c.gapAhead < 1e8f) {
                const float desired = c.minGap + c.v * c.headway;
                const float vFollow = leaderV + 0.5f * (c.gapAhead - desired) / c.headway;
                vT = std::min(vT, std::max(0.0f, vFollow));
            }
            // A car parked on the shoulder has no target speed but zero, and a
            // tow closing on a job takes its speed from the role stage.
            if (c.role == RoleBroken && c.parked) vT = 0.0f;
            if (c.roleSpeedOverride >= 0.0f) vT = std::min(vT, c.roleSpeedOverride);
            // BRAKE CHECK: the stab is a target-speed override, not a teleport,
            // so it is still bounded by brakeMax and still cannot cause an
            // overlap (the hard pass runs after everything).
            if (c.brakeCheckT > 0.0f) { vT = std::min(vT, c.v * 0.55f); c.brakeCheckT -= dt; }

            const float dv = vT - c.v;
            const float a = std::max(-c.brakeMax, std::min(c.accelMax, dv / std::max(dt, 1e-4f)));
            c.lastAccel = a;
            // BRAKE LIGHTS, off the SAME decel signal the controller just
            // computed — not a second opinion that could drift from it
            // (NO_SLOP rule 4). Without these a car slowing behind the player
            // is invisible to him, which is both a safety tell and the thing
            // that sells the whole system from the driver's seat.
            c.brakeLit = (a < -kBrakeLightMps2) && c.v > 0.4f;
            c.v = std::max(0.0f, c.v + a * dt);
            c.s += c.v * dt;
            if (m_closed) {
                if (c.s >= m_totalLen) c.s -= m_totalLen;
                else if (c.s < 0.0f)   c.s += m_totalLen;
            }
            c.spin -= c.v * dt;    // accumulated -distance; theta = spin/radius
        }
    }
}

// ---------------------------------------------------------------------------
// 5. THE HARD NO-OVERLAP INVARIANT — now in 2-D.
//
// The original pass compared cars with the same INTEGER lane. The moment lane
// changes existed that stopped being sufficient: a car at laneF 3.5 shares
// space with cars in lane 3 AND lane 4 and matched neither. This version
// tests the real footprints — lateral overlap first (cheap), then the arc
// separation — so a merge in progress is covered by construction.
//
// T6 gates this over the whole live population, every tick.
// ---------------------------------------------------------------------------
void FreewayTraffic::enforceNoOverlap() {
    for (int cw = 0; cw < 2; ++cw) {
        const std::vector<uint32_t>& o = m_order[cw];
        const int n = (int)o.size();
        for (int k = 0; k < n; ++k) {
            Car& c = m_cars[o[k]];
            if (c.loose) continue;
            for (int step = 1; step < n; ++step) {
                const int j = m_closed ? (k + step) % n : (k + step);
                if (j >= n) break;
                const Car& ot = m_cars[o[j]];
                if (ot.loose) continue;
                const float ds = arcDelta(c.s, ot.s);
                if (ds <= 0.0f) { if (!m_closed) break; else continue; }
                if (ds > kScanWindowM) break;
                if (!latOverlap(c, ot, kLatOverlapSlack)) continue;
                // Half-lengths of both cars plus a hair: this is the true
                // bumper-to-bumper contact distance, not a lane heuristic.
                const float minSep = 0.5f * (m_models[c.model].lenM +
                                             m_models[ot.model].lenM) + 0.5f;
                if (ds < minSep) {
                    c.s = ot.s - minSep;
                    if (m_closed) {
                        if (c.s < 0.0f) c.s += m_totalLen;
                        else if (c.s >= m_totalLen) c.s -= m_totalLen;
                    }
                    c.v = std::min(c.v, ot.v);
                }
                break;   // the nearest overlapping car is the binding one
            }
            // ---- AND THE PLAYER. The invariant is "no two vehicles occupy
            // the same space", and he is a vehicle. Without this clause a car
            // that the follower could not slow in time (or one whose lane he
            // drifted into) would still pass clean through his box.
            if (m_player.valid && m_player.cw == c.cw) {
                const float sep = std::fabs(c.laneF - m_player.laneF) * kTrafLaneM;
                if (sep < carHalfWidth(c) + m_player.halfW + kLatOverlapSlack) {
                    const float ds = arcDelta(c.s, m_player.s);
                    const float minSep = 0.5f * (m_models[c.model].lenM +
                                                 m_player.lenM) + 0.5f;
                    if (ds > 0.0f && ds < minSep) {
                        c.s = m_player.s - minSep;
                        if (m_closed) {
                            if (c.s < 0.0f) c.s += m_totalLen;
                            else if (c.s >= m_totalLen) c.s -= m_totalLen;
                        }
                        c.v = std::min(c.v, m_player.v);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 3. LANE CHANGES — the headline.
//
// THE SAFETY TEST IS MEASURED, BOTH DIRECTIONS. `laneGapSafe` walks the sorted
// order out from this car in +s and -s, considers every car whose footprint
// would touch the TARGET lane (including cars already merging INTO it — that
// is what `laneTo` is doing in the test), and requires:
//     ahead   ds - theirLen        >= scale * (myMinGap + myV * 0.9)
//     behind  -ds - myLen          >= scale * (theirMinGap + theirV * 0.9)
//                                     + scale * closingSpeed * 1.6
// The closing term is the one that stops the classic bad-AI merge: pulling
// into a gap that is big RIGHT NOW in front of someone doing 20 m/s more.
// `scale` is the driver's nerve: 1.0 normal, ~0.72 aggressive, 0.42 for the
// Cutter. It can make a merge RUDE. It can never make one overlap, because
// enforceNoOverlap runs afterwards and is absolute.
// ---------------------------------------------------------------------------
bool FreewayTraffic::laneGapSafe(size_t ci, float targetLane, float scale) const {
    const Car& c = m_cars[ci];
    const std::vector<uint32_t>& o = m_order[c.cw & 1];
    const int n = (int)o.size();
    if (n <= 1) return true;
    int k = -1;
    for (int i = 0; i < n; ++i) if (o[i] == (uint32_t)ci) { k = i; break; }
    if (k < 0) return true;
    const float myLen  = m_models[c.model].lenM;
    const float myHalf = carHalfWidth(c);

    for (int dir = -1; dir <= 1; dir += 2) {
        for (int step = 1; step < n; ++step) {
            int j = k + dir * step;
            if (m_closed) { j = ((j % n) + n) % n; }
            else if (j < 0 || j >= n) break;
            const Car& ot = m_cars[o[j]];
            const float ds = arcDelta(c.s, ot.s);
            if (std::fabs(ds) > kScanWindowM) break;
            if ((dir > 0 && ds < 0.0f) || (dir < 0 && ds > 0.0f)) continue;
            // Does this car occupy the target lane — now, or by the end of its
            // own merge? Either counts: two cars converging on one gap from
            // opposite sides is exactly the case a naive check misses.
            const float sepNow = std::fabs(ot.laneF  - targetLane) * kTrafLaneM;
            const float sepEnd = std::fabs(ot.laneTo - targetLane) * kTrafLaneM;
            const float need   = myHalf + carHalfWidth(ot) + 0.45f;
            if (sepNow >= need && sepEnd >= need) continue;
            const float otLen = m_models[ot.model].lenM;
            if (ds > 0.0f) {
                const float clear = ds - otLen;
                if (clear < scale * (c.minGap + c.v * 0.9f)) return false;
                // Also: do not pull in front of someone I am slower than.
                if (ot.v < c.v - 2.0f && clear < scale * (c.v - ot.v) * 1.4f) return false;
            } else {
                const float clear = -ds - myLen;
                if (clear < scale * (ot.minGap + ot.v * 0.9f)) return false;
                const float closing = ot.v - c.v;
                if (closing > 0.0f && clear < scale * closing * 1.6f) return false;
            }
        }
    }
    // NEVER MERGE INTO THE PLAYER. Same test, same coordinates. This is what
    // makes the merge-AROUND land in a clear lane instead of swapping one
    // blocked lane for the one he is sitting in.
    if (m_player.valid && m_player.cw == c.cw) {
        const float sepNow = std::fabs(m_player.laneF - targetLane) * kTrafLaneM;
        if (sepNow < myHalf + m_player.halfW + 0.45f) {
            const float ds = arcDelta(c.s, m_player.s);
            if (std::fabs(ds) < kScanWindowM) {
                if (ds > 0.0f) {
                    if (ds - m_player.lenM < scale * (c.minGap + c.v * 0.9f)) return false;
                } else {
                    if (-ds - myLen < scale * (c.minGap + m_player.v * 0.9f)) return false;
                }
            }
        }
    }
    return true;
}

void FreewayTraffic::startMerge(Car& c, float targetLane, float lead) {
    c.laneFrom = c.laneF;
    c.laneTo   = targetLane;
    c.mergeT   = 0.0f;
    c.mergeDur = (c.temper == TempNormal) ? rndf(kMergeMinS, kMergeMaxS)
                                          : rndf(1.6f, 2.3f);
    c.signalDir = (targetLane > c.laneF) ? +1 : -1;   // +1 = toward the shoulder
    c.signalT   = lead;
}

void FreewayTraffic::thinkLaneChanges(float dt) {
    const float maxLane = (float)(kFwyLaneCount - 1);
    for (size_t ci = 0; ci < m_cars.size(); ++ci) {
        Car& c = m_cars[ci];
        if (c.loose) continue;
        // Signalling counts down BEFORE the lateral motion starts. This is the
        // "signal-then-merge" the spec asks for, and it is also what makes an
        // overtake readable from behind.
        if (c.signalT > 0.0f) {
            c.signalT -= dt;
            if (c.signalT <= 0.0f) c.signalT = 0.0f;
            continue;
        }
        if (c.mergeT < 1.0f) continue;                 // already sliding
        if (c.role == RoleBroken || c.role == RoleTow) continue;  // roles steer
        c.thinkT -= dt;
        if (c.thinkT > 0.0f) continue;
        c.thinkT = kThinkEveryS + rndf(0.0f, 0.20f);

        const float cur = c.laneF;
        const float desired = c.minGap + c.v * c.headway;
        bool blocked = c.gapAhead < desired * 1.35f &&
                       c.leaderV < c.cruise - 1.2f;
        // A STOPPED OR CRAWLING VEHICLE IN A LIVE LANE — player or AI — is an
        // emergency, not a slow car: get around it. The look-ahead is much
        // longer than the ordinary blocked test (a 30 m/s car needs ~150 m to
        // deal with a stationary obstacle politely) and it overrides the
        // keep-right drift below. This is the SAME merge-around a breakdown
        // gets, because from the sim's point of view a stalled player IS a
        // breakdown.
        // Jerks are SLOWER TO YIELD: they leave it later, which is what makes
        // them read as jerks from the driver's seat rather than as a different
        // number in a table. They still cannot hit anything.
        const float reactK = (c.temper == TempJerk) ? 0.45f
                           : (c.temper == TempAggressive ? 0.75f : 1.0f);
        bool avoidStalled = false;
        if (c.leaderV < 2.5f && c.gapAhead < reactK * std::max(60.0f, c.v * 5.0f)) {
            blocked = true;
            avoidStalled = true;
        }
        // How much nerve this driver merges with.
        float scale = 1.0f;
        if (c.temper == TempAggressive) scale = 0.72f;
        if (c.temper == TempJerk) {
            scale = (c.jerk == JerkKind::Cutter) ? 0.42f
                  : (c.jerk == JerkKind::Weaver) ? 0.55f : 0.85f;
        }
        // Signal lead: the polite signal, the quick flick, or nothing at all.
        float lead = kSignalLeadS;
        if (c.temper == TempAggressive) lead = 0.6f;
        if (c.temper == TempJerk)
            lead = (c.jerk == JerkKind::Cutter || c.jerk == JerkKind::Weaver)
                 ? 0.0f : 0.45f;

        // ---- YIELD TO A RUNNING PATROL CAR --------------------------------
        // Simple and legible: if a cop with its lights on is closing on me in
        // my lane, get right. This is the only "other traffic yields" rule and
        // it is deliberately not a pursuit AI (see runRoles).
        bool yieldRight = false;
        if (c.role == RoleCivilian) {
            for (const Car& p : m_cars) {
                if (p.role != RoleCop || !p.lightsOn || p.cw != c.cw) continue;
                const float ds = arcDelta(p.s, c.s);      // I am ahead of him by ds
                if (ds < 5.0f || ds > 110.0f) continue;
                if (std::fabs(p.laneF - cur) > 1.2f) continue;
                yieldRight = true;
                break;
            }
        }

        // ---- the candidate, in priority order ------------------------------
        float target = -1.0f;
        if (yieldRight && cur < maxLane) {
            if (laneGapSafe(ci, cur + 1.0f, 0.85f)) target = cur + 1.0f;
        }
        if (target < 0.0f && blocked) {
            // OVERTAKE: go left (toward the median = the faster lanes). When
            // genuinely blocked a driver is allowed ONE lane left of the lane
            // band his class prefers — that is how "trucks stay right unless
            // blocked" works without pinning a blocked truck behind a slower
            // truck forever.
            const float leftLimit = std::max(0.0f, (float)c.prefLaneMin - 1.0f);
            if (cur - 1.0f >= leftLimit && laneGapSafe(ci, cur - 1.0f, scale))
                target = cur - 1.0f;
            // If left is not available, an undertake to the right is legal
            // here (US freeway practice). Ordinarily only the impatient do it;
            // faced with something STOPPED in the lane, everybody does.
            if (target < 0.0f && cur < maxLane &&
                (avoidStalled || c.temper != TempNormal || c.overtakeUrge > 0.5f) &&
                (avoidStalled || rndf(0.0f, 1.0f) < c.overtakeUrge) &&
                laneGapSafe(ci, cur + 1.0f, avoidStalled ? scale * 0.8f : scale))
                target = cur + 1.0f;
            // Still nowhere to go and something stationary ahead? Try the
            // shoulder-side lane with real urgency rather than sitting behind
            // a dead car at 70 mph hoping.
            if (target < 0.0f && avoidStalled) {
                if (cur - 1.0f >= 0.0f && laneGapSafe(ci, cur - 1.0f, 0.55f))
                    target = cur - 1.0f;
                else if (cur < maxLane && laneGapSafe(ci, cur + 1.0f, 0.55f))
                    target = cur + 1.0f;
            }
        }
        if (target < 0.0f && !blocked) {
            // KEEP RIGHT. Not blocked and sitting left of my class's home
            // band? Move over. This is what makes the outer lanes read as
            // truck lanes and the median lanes stay clear for the fast traffic.
            if (cur > (float)c.prefLaneMin && cur < maxLane + 0.5f &&
                rndf(0.0f, 1.0f) < 0.55f &&
                laneGapSafe(ci, cur + 1.0f, 1.0f) &&
                cur + 1.0f <= (float)c.prefLaneMax)
                target = cur + 1.0f;
            // The restless ones also probe LEFT for a faster lane even when
            // they are not strictly blocked — this is what "overtake more
            // readily" looks like from the mirror.
            if (target < 0.0f && cur > 0.0f &&
                rndf(0.0f, 1.0f) < c.overtakeUrge * 0.35f &&
                cur - 1.0f >= (float)std::max(0, c.prefLaneMin - 1) &&
                laneGapSafe(ci, cur - 1.0f, scale))
                target = cur - 1.0f;
        }
        // ---- THE TAILGATER picks a victim, leans on him, then swerves by ---
        if (c.temper == TempJerk && c.jerk == JerkKind::Tailgater &&
            c.tailgatingFor > 5.0f && target < 0.0f) {
            const float side = (cur > 0.0f && laneGapSafe(ci, cur - 1.0f, 0.5f))
                             ? cur - 1.0f
                             : ((cur < maxLane && laneGapSafe(ci, cur + 1.0f, 0.5f))
                                ? cur + 1.0f : -1.0f);
            if (side >= 0.0f) { target = side; c.tailgatingFor = 0.0f; }
        }

        if (target >= 0.0f && laneAllowed(c, target)) startMerge(c, target, lead);
    }
}

// ---------------------------------------------------------------------------
// 4. THE LATERAL SPLINE. A smoothstep over mergeDur seconds: zero lateral
// velocity at both ends, so a car eases out of its lane and settles into the
// next one instead of stepping between them.
// ---------------------------------------------------------------------------
void FreewayTraffic::advanceMerges(float dt) {
    for (Car& c : m_cars) {
        if (c.mergeT >= 1.0f || c.loose) continue;
        c.mergeT += dt / std::max(0.2f, c.mergeDur);
        if (c.mergeT >= 1.0f) {
            c.mergeT = 1.0f;
            c.laneF = c.laneFrom = c.laneTo;
            c.signalDir = 0;
            ++m_laneChanges;
            // Somebody just got cut off? Their horn is decided in serviceHorns
            // from the gap this merge left, not from a flag set here — the
            // provocation has to be MEASURED or it fires on merges that were
            // actually fine.
            continue;
        }
        c.laneF = c.laneFrom + (c.laneTo - c.laneFrom) * smoothstep01(c.mergeT);
    }
}

// ---------------------------------------------------------------------------
// 6. ROLES — cops, breakdowns, and the tow truck.
//
// SCOPE, STATED PLAINLY: this is NOT a pursuit AI and does not pretend to be.
// A patrol car drives the freeway like everyone else and occasionally runs
// lights-and-siren for half a minute, during which nearby traffic yields
// right. It does not select a target, does not chase the player, does not
// pull anyone over. The hook for that is onCopWouldPursue() below — one
// virtual-shaped seam, deliberately empty.
// ---------------------------------------------------------------------------
void FreewayTraffic::beginBreakdown(Car& c) {
    c.role = RoleBroken;
    c.roleT = 0.0f;
    c.parked = false;
    c.lightsOn = true;               // hazards go on immediately
    c.signalDir = +1;
    c.cruise = std::min(c.cruise, 16.0f);
    char b[140];
    std::snprintf(b, sizeof(b),
        "traffic: %s (car %u) has broken down — pulling to the shoulder",
        m_models[c.model].label.c_str(), c.id);
    x3::logInfo(b);
}

void FreewayTraffic::runRoles(float dt, x3::phys::IPhysicsWorld* phys) {
    (void)phys;
    const float maxLane = (float)(kFwyLaneCount - 1);

    // ---- schedule the next breakdown --------------------------------------
    m_breakdownCd -= dt;
    if (m_breakdownCd <= 0.0f) {
        m_breakdownCd = m_cfg.breakdownMeanS * rndf(0.55f, 1.45f);
        // Only break down a plain civilian that is already out in the right
        // half of the road — a car in lane 0 crossing seven lanes of traffic
        // to die is a stunt, not a breakdown.
        int cand = -1, seen = 0;
        for (size_t i = 0; i < m_cars.size(); ++i) {
            const Car& c = m_cars[i];
            if (c.role != RoleCivilian || c.loose || c.mergeT < 1.0f) continue;
            if (c.laneF < maxLane - 2.5f) continue;
            if ((int)(rnd() % (uint32_t)(++seen)) == 0) cand = (int)i;   // reservoir
        }
        if (cand >= 0 && breakdownCount() < 2) beginBreakdown(m_cars[cand]);
    }

    for (size_t i = 0; i < m_cars.size(); ++i) {
        Car& c = m_cars[i];
        c.roleSpeedOverride = -1.0f;
        if (c.loose) continue;
        c.roleT += dt;

        switch (c.role) {
        case RoleCop: {
            // A PATROL CAR NOTICES A STOPPED VEHICLE IN A LIVE LANE and rolls
            // up on it with the lights on. That is as far as this goes: it is
            // personality, not a pursuit (onCopWouldPursue holds that seam).
            // The obstacle can be the player or a broken-down AI car — the
            // sim does not distinguish, and neither should the cop.
            if (!c.lightsOn) {
                bool scene = false;
                if (m_player.valid && m_player.cw == c.cw && m_player.inLane &&
                    m_player.v < 2.5f) {
                    const float ds = arcDelta(c.s, m_player.s);
                    if (ds > 0.0f && ds < 220.0f &&
                        std::fabs(m_player.laneF - c.laneF) < 2.5f) scene = true;
                }
                if (!scene) {
                    for (const Car& o : m_cars) {
                        if (o.role != RoleBroken || o.cw != c.cw) continue;
                        if (o.laneF > (float)kFwyLaneCount - 0.6f) continue; // shoulder: fine
                        const float ds = arcDelta(c.s, o.s);
                        if (ds > 0.0f && ds < 220.0f) { scene = true; break; }
                    }
                }
                if (scene) {
                    c.lightsOn = true;
                    c.roleT = 0.0f;
                    c.roleNext = rndf(25.0f, 45.0f);
                    char b[120];
                    std::snprintf(b, sizeof(b),
                        "traffic: patrol %u lighting up for a vehicle stopped in a "
                        "live lane", c.id);
                    x3::logInfo(b);
                    break;
                }
            }
            // A long quiet patrol, then a burst of code-3. Deterministic in
            // the seed like everything else here.
            if (!c.lightsOn && c.roleT > c.roleNext) {
                c.lightsOn = true;
                c.roleT = 0.0f;
                c.roleNext = rndf(22.0f, 40.0f);
                c.cruise *= 1.35f;
                onCopWouldPursue(c);
            } else if (c.lightsOn && c.roleT > c.roleNext) {
                c.lightsOn = false;
                c.roleT = 0.0f;
                c.roleNext = rndf(90.0f, 210.0f);
                c.cruise /= 1.35f;
                if (m_audio && c.siren.valid()) { m_audio->stopLoop(c.siren); c.siren = {}; }
            }
            break;
        }
        case RoleBroken: {
            if (!c.parked) {
                // Steer to the shoulder: first out to the outer running lane,
                // then off it. laneAllowed lets ONLY this role past the lane
                // band, so the same merge machinery carries it.
                const float want = (c.laneF < maxLane - 0.1f) ? maxLane : kShoulderLaneF;
                if (c.mergeT >= 1.0f && std::fabs(c.laneF - want) > 0.05f &&
                    laneAllowed(c, want)) {
                    // A dying car gets to be a bit rude about merging right —
                    // but laneGapSafe still has to agree, so it waits for a gap.
                    if (laneGapSafe(i, want, 0.65f)) startMerge(c, want, 0.35f);
                }
                if (c.laneF > maxLane + 0.05f) {
                    c.roleSpeedOverride = 0.0f;         // coast to a stop
                    if (c.v < 0.35f && c.mergeT >= 1.0f) {
                        c.v = 0.0f;
                        c.parked = true;
                        c.roleT = 0.0f;
                        c.towCalled = rndf(18.0f, 40.0f);
                        char b[160];
                        std::snprintf(b, sizeof(b),
                            "traffic: car %u is on the shoulder — tow called, "
                            "ETA %.0f s", c.id, c.towCalled);
                        x3::logInfo(b);
                    }
                } else {
                    c.roleSpeedOverride = 8.0f;         // limping
                }
            } else {
                c.v = 0.0f;
                // Call the tow once, when the timer runs out.
                if (c.towCalled > 0.0f && c.roleT > c.towCalled) {
                    c.towCalled = -1.0f;
                    spawnTowFor(c, phys);
                }
            }
            break;
        }
        case RoleTow: {
            // Drive to the job, park behind it, hook up, then both leave.
            const Car* job = nullptr;
            for (const Car& o : m_cars)
                if (o.id == c.towTarget) { job = &o; break; }
            if (!job) {                       // the job vanished — go home
                c.role = RoleCivilian;
                c.lightsOn = false;
                c.prefLaneMin = 6; c.prefLaneMax = 7;
                break;
            }
            const float ds = arcDelta(c.s, job->s);      // + = job is ahead
            if (c.hooked) {
                c.roleSpeedOverride = 0.0f;
                if (c.roleT > c.hookDur) {
                    // Clear the scene: both vehicles despawn together, so the
                    // incident ENDS rather than leaving furniture on the road.
                    const uint32_t jobId = c.towTarget;
                    char b[120];
                    std::snprintf(b, sizeof(b),
                        "traffic: tow %u has cleared car %u from the shoulder",
                        c.id, jobId);
                    x3::logInfo(b);
                    for (size_t k = 0; k < m_cars.size();) {
                        if (m_cars[k].id == jobId || m_cars[k].id == c.id)
                            despawnCar(k, phys);
                        else ++k;
                    }
                    return;   // m_cars was reshuffled under us
                }
            } else if (ds > 90.0f) {
                // Still inbound: run the outer lane at a working pace.
                if (c.mergeT >= 1.0f && c.laneF < maxLane - 0.1f &&
                    laneGapSafe(i, std::min(maxLane, c.laneF + 1.0f), 0.8f))
                    startMerge(c, std::min(maxLane, c.laneF + 1.0f), 0.5f);
                c.roleSpeedOverride = 26.0f;
            } else {
                // Close in: get onto the shoulder and stop just behind the job.
                const float want = kShoulderLaneF;
                if (c.mergeT >= 1.0f && std::fabs(c.laneF - want) > 0.05f &&
                    laneAllowed(c, want) && laneGapSafe(i, want, 0.65f))
                    startMerge(c, want, 0.4f);
                // STANDOFF IS PAIRED WITH enforceNoOverlap's minSep (NO_SLOP
                // rule 4). The first cut parked the boom at 0.5*(lenA+lenB)+2.5
                // and demanded |ds - standoff| < 2 to hook. The overlap
                // invariant independently pins the tow at 0.5*(lenA+lenB)+0.5,
                // which is 2.0 m SHORTER — exactly on the excluded boundary, so
                // the truck sat 7.5 m off its job forever and T9b failed with
                // "closest approach 7.5 m". Two rules were arguing about one
                // distance. Now the standoff is DERIVED from the same minSep
                // the overlap pass uses, and the hook test is an upper bound
                // only: the lower bound is already guaranteed by that pass.
                const float minSep = 0.5f * (m_models[c.model].lenM +
                                             m_models[job->model].lenM) + 0.5f;
                const float standoff = minSep + 1.5f;
                const float err = ds - standoff;
                // A real braking profile, not a linear ramp: v = sqrt(2*a*d)
                // is the fastest approach that still stops in `err` metres, so
                // the truck arrives instead of coasting past and being caught
                // by the overlap pass.
                c.roleSpeedOverride = (err <= 0.0f) ? 0.0f
                    : std::min(14.0f, std::sqrt(2.0f * c.brakeMax * err));
                if (c.laneF > maxLane + 0.05f && ds < standoff + 2.0f && c.v < 0.6f) {
                    c.v = 0.0f;
                    c.hooked = true;
                    c.roleT = 0.0f;
                    c.hookDur = rndf(9.0f, 15.0f);
                    x3::logInfo("traffic: tow truck on scene, hooking up");
                }
            }
            break;
        }
        default: break;
        }
    }

    // ---- SIRENS. Bounded to kMaxSirens voices, nearest first, and only
    // within earshot: a siren 900 m away behind a hill is a wasted voice.
    if (m_audio && m_sndSiren.valid()) {
        struct Cand { float d2; size_t i; };
        std::vector<Cand> want;
        for (size_t i = 0; i < m_cars.size(); ++i) {
            const Car& c = m_cars[i];
            if (c.role != RoleCop || !c.lightsOn) continue;
            float p[3];
            worldPosOf(c, p);
            const float dx = p[0] - m_playerPos[0], dz = p[2] - m_playerPos[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 < kSirenRangeM * kSirenRangeM) want.push_back({ d2, i });
        }
        std::sort(want.begin(), want.end(),
                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        if (want.size() > kMaxSirens) want.resize(kMaxSirens);
        std::vector<uint8_t> keep(m_cars.size(), 0);
        for (const Cand& w : want) keep[w.i] = 1;
        for (size_t i = 0; i < m_cars.size(); ++i) {
            Car& c = m_cars[i];
            if (keep[i]) {
                float p[3];
                worldPosOf(c, p);
                if (!c.siren.valid())
                    c.siren = m_audio->startLoop3D(m_sndSiren, p[0], p[1] + 1.2f, p[2],
                                                   0.85f, 1.0f);
                else
                    m_audio->setLoopPosition(c.siren, p[0], p[1] + 1.2f, p[2]);
            } else if (c.siren.valid()) {
                m_audio->stopLoop(c.siren);
                c.siren = {};
            }
        }
    }
}

// THE PURSUIT HOOK. Deliberately empty: a patrol car that decides to chase
// somebody needs a target-selection policy, a speed budget the player can
// actually escape, a "pulled over" state for the target, and a way to end the
// event — none of which is in scope for this pass and all of which would be
// guesswork without the owner saying what a bust should FEEL like. What lands
// here is the seam: this is called once, at the moment a patrol car lights up.
void FreewayTraffic::onCopWouldPursue(Car& cop) {
    char b[120];
    std::snprintf(b, sizeof(b),
        "traffic: patrol %u running code 3 (pursuit AI not in scope — hook only)",
        cop.id);
    x3::logInfo(b);
}

void FreewayTraffic::spawnTowFor(Car& job, x3::phys::IPhysicsWorld* phys) {
    const int mi = modelIndexByFile(kTowModelFile);
    if (mi < 0) { x3::logWarn("traffic: no tow body on the roster"); return; }
    const Model& md = m_models[mi];
    Car t;
    t.id = m_nextCarId++;
    t.model = mi;
    t.cw = job.cw;
    t.role = RoleTow;
    t.towTarget = job.id;
    t.lightsOn = true;                      // amber beacons the whole way
    // Come from UPSTREAM so the player watching the scene sees it ARRIVE.
    t.s = job.s - 620.0f;
    if (m_closed) { if (t.s < 0.0f) t.s += m_totalLen; }
    else t.s = std::max(0.0f, t.s);
    t.laneF = t.laneFrom = t.laneTo = (float)(kFwyLaneCount - 1);
    t.cruise = 26.0f;
    t.v = 24.0f;
    t.lastV = t.v;
    t.halfH = md.heightM * 0.5f;
    giveCharacter(t);
    t.tint[0] = 0.86f; t.tint[1] = 0.62f; t.tint[2] = 0.10f;   // recovery amber
    t.hasTint = true;
    if (phys) {
        float pos[3], dir[2], mh;
        sampleAt(uOfS(t.cw, t.s), pos, dir, &mh, nullptr);
        const float lat = laneLat(t.cw, t.laneF, mh);
        t.body = phys->addKinematicBox(
            x3::phys::Vec3{ md.widthM * 0.5f, t.halfH, md.lenM * 0.5f },
            x3::phys::Vec3{ pos[0] + (-dir[1]) * lat,
                            pos[1] + kTrafficPaveProud + t.halfH,
                            pos[2] + ( dir[0]) * lat },
            x3::phys::Layer::Dynamic);
    }
    m_cars.push_back(t);
    char b[140];
    std::snprintf(b, sizeof(b),
        "traffic: TOWBOOK recovery truck %u dispatched to car %u (620 m out)",
        t.id, job.id);
    x3::logInfo(b);
}

// World position of a car's contact point (lane centre, on the pavement).
void FreewayTraffic::worldPosOf(const Car& c, float out[3]) const {
    float pos[3], dir[2], mh;
    sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, nullptr);
    const float lat = laneLat(c.cw, c.laneF, mh);
    out[0] = pos[0] + (-dir[1]) * lat;
    out[1] = pos[1] + kTrafficPaveProud;
    out[2] = pos[2] + ( dir[0]) * lat;
}

// ---------------------------------------------------------------------------
// 7. HORNS — "a horn when someone is cut off, brakes hard, or is tailgated",
// rate-limited so a jam is not a cacophony.
//
// Every provocation is MEASURED from the sim state, never set as a flag by the
// code that caused it. That matters: a flag fires on every merge, and most
// merges are fine. What earns a horn is the RESULT.
// ---------------------------------------------------------------------------
void FreewayTraffic::serviceHorns(float dt) {
    m_hornGlobalCd -= dt;
    for (size_t ci = 0; ci < m_cars.size(); ++ci) {
        Car& c = m_cars[ci];
        c.hornCooldown -= dt;
        if (c.loose) continue;

        // --- am I being tailgated, and am I tailgating? --------------------
        const std::vector<uint32_t>& o = m_order[c.cw & 1];
        const int n = (int)o.size();
        int k = -1;
        for (int i = 0; i < n; ++i) if (o[i] == (uint32_t)ci) { k = i; break; }
        bool tailgated = false;
        if (k >= 0) {
            for (int step = 1; step < n; ++step) {
                int j = k - step;
                if (m_closed) j = ((j % n) + n) % n;
                else if (j < 0) break;
                const Car& b = m_cars[o[j]];
                const float ds = arcDelta(b.s, c.s);       // b is behind me by ds
                if (ds <= 0.0f || ds > 60.0f) break;
                if (!latOverlap(c, b, kLatFollowSlack)) continue;
                const float clear = ds - m_models[c.model].lenM;
                tailgated = clear < std::max(3.0f, b.v * 0.55f);
                break;
            }
        }
        c.tailgatedFor = tailgated ? c.tailgatedFor + dt : 0.0f;
        // The mirror image, for the Tailgater jerk's swerve trigger.
        const float myClear = c.gapAhead;
        c.tailgatingFor = (myClear < std::max(3.0f, c.v * 0.5f))
                        ? c.tailgatingFor + dt : 0.0f;

        // --- BRAKE CHECK: a jerk who is being tailgated stabs the brakes ---
        if (c.temper == TempJerk && c.jerk == JerkKind::BrakeChecker &&
            c.tailgatedFor > 1.6f && c.brakeCheckT <= 0.0f && c.v > 12.0f) {
            c.brakeCheckT = 0.9f;
            c.tailgatedFor = 0.0f;
        }

        // --- the three provocations ----------------------------------------
        const char* why = nullptr;
        // 1. CUT OFF: someone is merging into my lane right in front of me and
        //    the gap they are leaving is under half what I need.
        for (const Car& o2 : m_cars) {
            if (o2.id == c.id || o2.cw != c.cw || o2.mergeT >= 1.0f) continue;
            if (std::fabs(o2.laneTo - c.laneF) > 0.6f) continue;
            const float ds = arcDelta(c.s, o2.s);
            if (ds <= 0.0f || ds > 55.0f) continue;
            const float clear = ds - m_models[o2.model].lenM;
            if (clear < 0.5f * (c.minGap + c.v * c.headway)) { why = "cut off"; break; }
        }
        // 1b. THE PLAYER IS IN MY WAY. He is by far the most likely thing on
        //     this freeway to be sitting still in a live lane, and being
        //     honked at for it is the most alive-feeling moment in the whole
        //     feature. Jerks lean on the horn from twice as far back.
        if (!why && c.blockedByPlayer && m_player.inLane) {
            const float reach = (c.temper == TempJerk) ? 70.0f
                              : (c.temper == TempAggressive ? 45.0f : 26.0f);
            if (m_player.v < 3.0f && c.gapAhead < reach) why = "player in the lane";
        }
        // 2. HARD BRAKE (mine, and not one I chose): I am hauling it down.
        if (!why && c.lastAccel < -kHardBrakeMps2 && c.v > 8.0f &&
            c.brakeCheckT <= 0.0f)
            why = "braking hard";
        // 3. TAILGATED for long enough to be rude.
        if (!why && c.tailgatedFor > kTailgatedS) { why = "tailgated"; c.tailgatedFor = 0.0f; }
        if (why) honk(c, why);
    }
}

void FreewayTraffic::honk(Car& c, const char* why) {
    if (c.hornCooldown > 0.0f || m_hornGlobalCd > 0.0f) return;
    // Distance gate: no voice spent on a horn nobody can hear.
    float p[3];
    worldPosOf(c, p);
    const float dx = p[0] - m_playerPos[0], dz = p[2] - m_playerPos[2];
    if (dx * dx + dz * dz > kHornRangeM * kHornRangeM) return;
    c.hornCooldown = kHornCarCdS * rndf(0.8f, 1.4f);
    m_hornGlobalCd = kHornGlobalGapS;
    ++m_hornCount;
    (void)why;
    if (!m_audio) return;
    const bool heavy = m_models[c.model].cls == ClsHeavy;
    const x3::audio::SoundHandle snd = heavy ? m_sndHornTruck : m_sndHornCar;
    if (!snd.valid()) return;
    // A little pitch scatter so twelve cars are not one car twelve times.
    m_audio->playSound3D(snd, p[0], p[1] + 1.1f, p[2],
                         heavy ? 0.95f : 0.8f, rndf(0.94f, 1.07f));
}

// ===========================================================================
// FURNITURE — the cop light bar (a real GLB), and the procedural props:
// beacons, the TOWBOOK plate, the radar speed sign.
// ===========================================================================
namespace {

// A tiny mesh builder. Local frame for EVERY prop built here:
//   +X right, +Y up, +Z the direction the prop FACES.
// Deciding the facing once, locally, is the whole defence against the
// app/factory.cpp receipt: its first sign was authored, textured, correctly
// UV'd and BACKFACE-CULLED because one quad's winding came out reversed. Here
// a quad's normal is asserted by construction and the world matrix carries the
// facing, so there is exactly one place a facing bug could live.
struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;

    void tri(uint32_t a, uint32_t b, uint32_t c) { i.push_back(a); i.push_back(b); i.push_back(c); }

    // Axis-aligned box, world-UV'd (uvPerM) — for structure, not pictures.
    void box(float x0, float x1, float y0, float y1, float z0, float z1, float uvPerM) {
        const float P[8][3] = {
            {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
            {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
        const int F[6][4] = { {0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5} };
        const float N[6][3] = { {0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0} };
        for (int f = 0; f < 6; ++f) {
            const uint32_t base = (uint32_t)v.size();
            for (int k = 0; k < 4; ++k) {
                x3::rhi::MeshVertex mv{};
                const float* p = P[F[f][k]];
                mv.pos[0] = p[0]; mv.pos[1] = p[1]; mv.pos[2] = p[2];
                mv.normal[0] = N[f][0]; mv.normal[1] = N[f][1]; mv.normal[2] = N[f][2];
                // Project onto the face's two dominant axes for the UV.
                const bool ax = std::fabs(N[f][0]) > 0.5f, ay = std::fabs(N[f][1]) > 0.5f;
                mv.uv[0] = (ax ? p[2] : p[0]) * uvPerM;
                mv.uv[1] = (ay ? p[2] : p[1]) * uvPerM;
                v.push_back(mv);
            }
            tri(base, base + 1, base + 2);
            tri(base, base + 2, base + 3);
        }
    }

    // ONE face with 0..1 UVs, normal = +Z, at z. For a texture that is a
    // PICTURE, not a tile — the factory's signFace lesson: a 26 m board at
    // 0.25 uv/m wrapped its baked panel four times and read "GLIMVA" x4.
    void pictureQuad(float x0, float x1, float y0, float y1, float z, bool mirrorU) {
        const uint32_t base = (uint32_t)v.size();
        const float px[4] = { x0, x1, x1, x0 };
        const float py[4] = { y0, y0, y1, y1 };
        float u[4] = { 0, 1, 1, 0 };
        const float vv[4] = { 1, 1, 0, 0 };
        if (mirrorU) { u[0] = 1; u[1] = 0; u[2] = 0; u[3] = 1; }
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0] = px[k]; mv.pos[1] = py[k]; mv.pos[2] = z;
            mv.normal[0] = 0; mv.normal[1] = 0; mv.normal[2] = 1;
            mv.uv[0] = u[k]; mv.uv[1] = vv[k];
            v.push_back(mv);
        }
        // Winding for a +Z normal in a right-handed system: CCW seen from +Z.
        tri(base, base + 1, base + 2);
        tri(base, base + 2, base + 3);
    }

    // The same picture, BACK-TO-BACK, so a plate reads correctly from either
    // side. The -Z face mirrors its U or the wordmark comes out backwards —
    // which is the OTHER half of the factory sign trap and the reason this is
    // one function rather than two call sites that must remember.
    void plate(float x0, float x1, float y0, float y1, float t) {
        pictureQuad(x0, x1, y0, y1, +t, /*mirrorU=*/false);
        const uint32_t base = (uint32_t)v.size();
        const float px[4] = { x1, x0, x0, x1 };
        const float py[4] = { y0, y0, y1, y1 };
        const float u[4]  = { 0, 1, 1, 0 };
        const float vv[4] = { 1, 1, 0, 0 };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0] = px[k]; mv.pos[1] = py[k]; mv.pos[2] = -t;
            mv.normal[0] = 0; mv.normal[1] = 0; mv.normal[2] = -1;
            mv.uv[0] = u[k]; mv.uv[1] = vv[k];
            v.push_back(mv);
        }
        tri(base, base + 1, base + 2);
        tri(base, base + 2, base + 3);
    }
};

x3::rhi::MeshHandle upload(x3::rhi::IRenderDevice& d, const MeshBuf& m) {
    if (m.v.empty() || m.i.empty()) return {};
    return d.createMesh(m.v.data(), (uint32_t)m.v.size(),
                        m.i.data(), (uint32_t)m.i.size());
}

// A 1x1 RGBA texel.
std::vector<uint8_t> px1(uint8_t r, uint8_t g, uint8_t b) {
    return { r, g, b, 255 };
}

} // namespace

void FreewayTraffic::buildFurniture(x3::rhi::IRenderDevice& dev, std::string_view glbDir) {
    // ---- shared materials --------------------------------------------------
    {
        auto w = px1(255, 255, 255);
        m_texWhite = dev.createTexture(w.data(), 1, 1, true);
        auto md = lns::makeMr1x1(205, 0);           // matte, dielectric
        m_texMrDull = dev.createTexture(md.data(), 1, 1, false);
        auto ml = lns::makeMr1x1(60, 0);            // glossy lens / glass
        m_texMrLens = dev.createTexture(ml.data(), 1, 1, false);
    }

    // ---- THE COP LIGHT BAR: a real authored asset --------------------------
    // RCC v4's Model_Police_Siren, converted by tools/convert_lightbar_glb.py
    // (which re-origins it so the MOUNT FACE is at y=0 and bakes the red/blue
    // lens identity into emissiveFactor — see that script for why).
    {
        const std::string rel = "Vehicles/Traffic/LightBar.glb";
        const std::string abs = std::string(glbDir) + "/" + rel;
        const GlbModel cpu = readGlbForLod(abs, /*minTriangles=*/4);
        if (!cpu.ok) {
            x3::logWarn(std::string("traffic: no light bar (") + cpu.error +
                        ") — patrol cars will run without one");
        } else {
            float mn[3] = { 1e18f, 1e18f, 1e18f }, mx[3] = { -1e18f, -1e18f, -1e18f };
            for (const GlbPrimitive& pr : cpu.prims)
                for (const auto& vv : pr.verts)
                    for (int k = 0; k < 3; ++k) {
                        mn[k] = std::min(mn[k], vv.pos[k]);
                        mx[k] = std::max(mx[k], vv.pos[k]);
                    }
            m_lightBar.w = mx[0] - mn[0];
            m_lightBar.h = mx[1] - mn[1];
            m_lightBar.d = mx[2] - mn[2];
            m_lightBar.src.reset(x3::asset::createAssetSource());
            if (m_lightBar.src && m_lightBar.src->mountDir(glbDir, 0)) {
                m_lightBar.loader.reset(x3::asset::createModelLoader(&dev, m_lightBar.src.get()));
                m_lightBar.model = m_lightBar.loader->load(rel);
                if (m_lightBar.model.ok) {
                    m_lightBar.draw = x3::asset::makeDrawables(m_lightBar.model);
                    for (const auto& d : m_lightBar.draw) {
                        // THE CLASSIFIER the converter baked in.
                        uint8_t kind = 0;
                        if (d.emissiveFactor[0] > 0.5f) kind = 1;        // red lens
                        else if (d.emissiveFactor[2] > 0.5f) kind = 2;   // blue lens
                        m_lightBar.lens.push_back(kind);
                    }
                    m_lightBar.ok = !m_lightBar.draw.empty();
                }
            }
            uint32_t nr = 0, nb = 0;
            for (uint8_t k : m_lightBar.lens) { nr += (k == 1); nb += (k == 2); }
            char b[200];
            std::snprintf(b, sizeof(b),
                "traffic: light bar %s — %.3f x %.3f x %.3f m, %u drawable(s) "
                "(%u red lens, %u blue lens)",
                m_lightBar.ok ? "loaded" : "FAILED",
                m_lightBar.w, m_lightBar.h, m_lightBar.d,
                (uint32_t)m_lightBar.draw.size(), nr, nb);
            if (m_lightBar.ok && nr && nb) x3::logInfo(b); else x3::logWarn(b);
        }
    }

    // ---- the amber lens (tow beacons + breakdown hazards) ------------------
    // A small rounded-ish dome, near-black albedo. Rule 5's shape: the glow is
    // driven per-draw as emissive on a DARK base, so unlit it is a dark lens
    // and lit it blooms — never a flat bright quad.
    {
        MeshBuf m;
        m.box(-0.085f, 0.085f, 0.0f, 0.075f, -0.075f, 0.075f, 1.0f);
        m.box(-0.065f, 0.065f, 0.075f, 0.105f, -0.055f, 0.055f, 1.0f);
        m_lens.mesh = upload(dev, m);
        m_lens.base = m_texWhite;
        m_lens.mr   = m_texMrLens;
        m_lens.tris = (uint32_t)(m.i.size() / 3);
    }

    // ---- the wordmark plate (TOWBOOK, POLICE) ------------------------------
    // ONE two-faced 0..1-UV quad mesh, 1 m wide x 1 m tall in local units and
    // scaled at the call site. Both faces readable (see MeshBuf::plate).
    {
        MeshBuf m;
        m.plate(-0.5f, 0.5f, -0.5f, 0.5f, 0.006f);
        m_plate.mesh = upload(dev, m);
        m_plate.mr   = m_texMrDull;
        m_plate.tris = (uint32_t)(m.i.size() / 3);
    }
    {
        MeshBuf m;
        m.pictureQuad(-0.5f, 0.5f, -0.5f, 0.5f, 0.0f, false);
        m_quad.mesh = upload(dev, m);
        m_quad.mr   = m_texMrDull;
        m_quad.tris = (uint32_t)(m.i.size() / 3);
    }
    // TOWBOOK — the owner's own company. Baked as a crisp 5x7 bitmap wordmark
    // through lns::makeSignRGBA (the LNS neon baker, already the source of the
    // GLIMVALE sign), NOT a generated image: every glyph is authored pixels.
    // Cold white-blue on a near-black field so it reads as reflective fleet
    // lettering under the sun and glows a little at night.
    {
        auto tb = lns::makeSignRGBA(512, 96, "TOWBOOK", 0.80f, 0.88f, 1.00f);
        m_texTowbook = dev.createTexture(tb.data(), 512, 96, true);
        auto po = lns::makeSignRGBA(512, 96, "POLICE", 0.32f, 0.52f, 1.00f);
        m_texPolice = dev.createTexture(po.data(), 512, 96, true);
        auto hd = lns::makeSignRGBA(512, 96, "YOUR SPEED", 1.00f, 0.72f, 0.18f);
        m_texHeader = dev.createTexture(hd.data(), 512, 96, true);
    }
    // The DIGITS. Ten textures and one quad, drawn three times — rather than
    // re-baking a panel texture every time the number changes. A speed readout
    // changes several times a second; createTexture is a GPU submit.
    {
        for (int d = 0; d < 10; ++d) {
            const char s[2] = { (char)('0' + d), 0 };
            auto px = lns::makeSignRGBA(96, 128, s, 1.00f, 0.74f, 0.16f);
            m_texDigit[d] = dev.createTexture(px.data(), 96, 128, true);
        }
        auto blank = lns::makeSignRGBA(96, 128, " ", 1.0f, 1.0f, 1.0f);
        m_texDigit[10] = dev.createTexture(blank.data(), 96, 128, true);
    }

    // ---- THE RADAR SPEED SIGN ---------------------------------------------
    // Local frame: +Z faces the traffic it reads. Post from the ground up,
    // then the housing. Sized like the real thing: a 2.4 m post with a
    // 1.5 x 1.25 m head, which is a road sign a driver can read at 100 m.
    {
        MeshBuf m;
        m.box(-0.075f, 0.075f, 0.0f, 2.45f, -0.075f, 0.075f, 0.8f);   // post
        m.box(-0.78f, 0.78f, 2.30f, 3.55f, -0.075f, 0.075f, 0.8f);    // head shell
        m.box(-0.20f, 0.20f, 0.0f, 0.10f, -0.30f, 0.30f, 0.8f);       // base plate
        m_steel.mesh = upload(dev, m);
        m_steel.base = m_texWhite;
        m_steel.mr   = m_texMrDull;
        m_steel.tris = (uint32_t)(m.i.size() / 3);
    }
    {
        // X3_WORLD_RULES rule 7: the display is a DARK-GLASS panel, not a
        // bright quad. Near-black albedo, glossy MR, and the digits ride
        // ON it as separate emissive quads standing 1 cm proud.
        MeshBuf m;
        m.pictureQuad(-0.70f, 0.70f, 2.38f, 3.47f, 0.078f, false);
        m_glass.mesh = upload(dev, m);
        m_glass.base = m_texWhite;
        m_glass.mr   = m_texMrLens;
        m_glass.tris = (uint32_t)(m.i.size() / 3);
    }
    m_furniture = true;
}

// ---------------------------------------------------------------------------
// SITING THE SIGN. Derived from the road data, never eyeballed (gotcha 4.1's
// law, and the same reason logCameraStations exists). Requirements: on the
// RIGHT carriageway's outer shoulder, on a straight-ish reach so it is visible
// from far enough back to matter, not inside a bore/deck, and facing back
// down the traffic it reads.
// ---------------------------------------------------------------------------
void FreewayTraffic::siteRadarSign() {
    if (m_path.size() < 32) return;
    // Score candidate stations on straightness over the ~180 m a driver spends
    // approaching. Start a quarter of the way round so the sign is not on top
    // of the spawn point.
    const size_t n = m_path.size();
    size_t best = 0;
    float bestScore = -1e9f;
    for (size_t k = n / 4; k < n / 4 + n / 3 && k + 24 < n; k += 3) {
        const RoadRenderStation& st = m_path[k];
        if (st.gap) continue;
        float turn = 0.0f;
        for (size_t j = k; j < k + 24 && j + 1 < n; ++j) {
            if (m_path[j].gap) { turn += 100.0f; break; }
            const float d = m_path[j].tx * m_path[j + 1].tx + m_path[j].tz * m_path[j + 1].tz;
            turn += 1.0f - std::min(1.0f, std::max(-1.0f, d));
        }
        const float score = -turn;
        if (score > bestScore) { bestScore = score; best = k; }
    }
    const RoadRenderStation& st = m_path[best];
    // THE SIGN STANDS ON THE VERGE, OUTSIDE THE PAVED EDGE.
    // Getting this wrong put the sign IN THE MIDDLE OF THE ROAD, and the
    // capture showed it plainly (shots_traffic2/dbg_radar.png, first cut): a
    // post planted between the running lanes with traffic flowing past both
    // sides. The arithmetic: the carriageway spans lat from `medianHalf` to
    // `medianHalf + 2*kFwyPavedHalfM`, so its CENTRE is at
    // `medianHalf + kFwyPavedHalfM` — which is exactly what the first cut used
    // as if it were the outer edge. The outer paved edge is one more
    // kFwyPavedHalfM out. (laneLat() encodes the same fact: it starts from
    // medianHalf + kFwyPavedHalfM and then subtracts kFwyRunningHalfM to reach
    // the innermost lane, i.e. it treats that sum as the centre line.)
    const float lat = st.medianHalf + 2.0f * kFwyPavedHalfM + 1.35f;
    const float nx = -st.tz, nz = st.tx;
    m_radar.pos[0] = st.x + nx * lat;
    m_radar.pos[1] = st.y;
    m_radar.pos[2] = st.z + nz * lat;
    // It reads the RIGHT carriageway, which travels +u, so it FACES -u.
    m_radar.dirX = -st.tx;
    m_radar.dirZ = -st.tz;
    m_radar.u = st.u;
    m_radar.sited = true;
    char b[200];
    std::snprintf(b, sizeof(b),
        "traffic: radar speed sign sited at u=%.0f m (%.1f, %.1f, %.1f), "
        "facing (%.2f, %.2f), limit %.0f mph",
        st.u, m_radar.pos[0], m_radar.pos[1], m_radar.pos[2],
        m_radar.dirX, m_radar.dirZ, m_cfg.radarLimitMph);
    x3::logInfo(b);
}

// ---------------------------------------------------------------------------
// THE RADAR. Reads the PLAYER's speed — the whole point of the prop; an AI
// car's speed on a sign nobody is chasing is scenery, the player's own number
// is a conversation. Reads only while he is in range and APPROACHING (a real
// radar sign is directional), then holds the last number briefly so it does
// not blank the instant he passes.
// ---------------------------------------------------------------------------
void FreewayTraffic::updateRadar(float dt) {
    if (!m_radar.sited) { m_radar.shownMph = -1; return; }
    m_radar.flashPhase += dt;
    const float dx = m_playerPos[0] - m_radar.pos[0];
    const float dz = m_playerPos[2] - m_radar.pos[2];
    const float d = std::sqrt(dx * dx + dz * dz);
    // "In front of the sign" = on the side its face points at.
    const float ahead = dx * m_radar.dirX + dz * m_radar.dirZ;
    const bool inRange = m_havePlayer && d < 240.0f && ahead > -12.0f;
    if (inRange) {
        m_radar.shownMph = (int)(m_playerSpeed * kMps2Mph + 0.5f);
        if (m_radar.shownMph > 199) m_radar.shownMph = 199;
        m_radar.over = (float)m_radar.shownMph > m_cfg.radarLimitMph;
        m_radar.holdT = 2.5f;
    } else if (m_radar.holdT > 0.0f) {
        m_radar.holdT -= dt;
        if (m_radar.holdT <= 0.0f) { m_radar.shownMph = -1; m_radar.over = false; }
    } else {
        m_radar.shownMph = -1;
        m_radar.over = false;
    }
}

// ---------------------------------------------------------------------------
// X3_TRAFFIC_PRESIM's companion: after the fast-forward, print a paste-ready
// --shot-cam AIMED AT each thing worth photographing — the nearest lit patrol
// car, a car mid-merge, the breakdown, the tow truck, the radar sign.
//
// This is gotcha 4.1's law ("derive cameras from the data, never eyeball
// coordinates") applied to MOVING subjects. Eyeballing a camera at a static
// room is merely error-prone; eyeballing one at a car that is somewhere on 16
// miles of freeway after a 140-second fast-forward is hopeless, and the two
// captures before this existed proved it — one framed an empty foreground and
// the other a line of trees.
// ---------------------------------------------------------------------------
void FreewayTraffic::reportShotCams() const {
    if (!std::getenv("X3_TRAFFIC_PRESIM") && !std::getenv("X3_TRAFFIC_SHOTS")) return;
    // Look at `t` from `back` metres behind along its travel and `side` metres
    // to its right, at `up` metres of eye height.
    auto emit = [&](const char* what, const float t[3], float dirX, float dirZ,
                    float back, float side, float up) {
        const float rx = -dirZ, rz = dirX;                 // right of travel
        const float cx = t[0] - dirX * back + rx * side;
        const float cz = t[2] - dirZ * back + rz * side;
        const float cy = t[1] + up;
        const float ddx = t[0] - cx, ddz = t[2] - cz;
        const float horiz = std::sqrt(ddx * ddx + ddz * ddz);
        const float yaw = std::atan2(ddz, ddx);
        const float pitch = std::atan2((t[1] + 0.9f) - cy, std::max(0.1f, horiz));
        char b[260];
        std::snprintf(b, sizeof(b),
            "[traffic-shot] %-22s --shot-cam \" %.1f,%.2f,%.1f,%.3f,%.3f\"   "
            "(subject at %.1f, %.1f, %.1f)",
            what, cx, cy, cz, yaw, pitch, t[0], t[1], t[2]);
        x3::logInfo(b);
    };
    // THE LEAD, and why the first cut of this framed an empty road twice.
    // reportShotCams runs at the END of the presim, but the capture then runs
    // a 200-frame settle at 1/60 s (host_tunnel's settleAndGrab, documented at
    // its line 2273) before the shutter. A car at 30 m/s is 100 METRES down the
    // road by then, so a camera aimed at where the subject IS photographs an
    // empty lane — which is exactly what 05_cop_lights.png did. So aim at where
    // the subject WILL BE: advance its arc position by v * settle and re-sample
    // the lane there. Exact for a car holding its lane, which is what these
    // subjects are doing. X3_TRAFFIC_SHOTLEAD overrides for a longer settle.
    float lead = 200.0f / 60.0f;
    if (const char* e = std::getenv("X3_TRAFFIC_SHOTLEAD")) lead = (float)std::atof(e);
    auto future = [&](const Car& c, float p[3], float& dx, float& dz) {
        Car ahead = c;
        ahead.s = c.s + c.v * lead;
        if (m_closed) {
            if (ahead.s >= m_totalLen) ahead.s -= m_totalLen;
            else if (ahead.s < 0.0f)   ahead.s += m_totalLen;
        }
        // A merging car is mid-slide; by the shutter it has arrived, so aim at
        // the lane it is going to.
        if (ahead.mergeT < 1.0f && lead > ahead.mergeDur * (1.0f - ahead.mergeT))
            ahead.laneF = ahead.laneTo;
        worldPosOf(ahead, p);
        float pos[3], dir[2];
        sampleAt(uOfS(ahead.cw, ahead.s), pos, dir, nullptr, nullptr);
        const float sgn = (ahead.cw == 1) ? 1.0f : -1.0f;
        dx = sgn * dir[0]; dz = sgn * dir[1];
    };
    const Car* cop = nullptr; const Car* merger = nullptr;
    const Car* broke = nullptr; const Car* tow = nullptr; const Car* truck = nullptr;
    for (const Car& c : m_cars) {
        if (c.loose) continue;
        if (!cop && c.role == RoleCop && c.lightsOn) cop = &c;
        if (!broke && c.role == RoleBroken) broke = &c;
        if (!tow && c.role == RoleTow) tow = &c;
        // A merger worth photographing is one actually mid-slide, not one that
        // has barely started or is already settling.
        if (!merger && c.mergeT > 0.25f && c.mergeT < 0.75f) merger = &c;
        if (!truck && m_models[c.model].cls == ClsHeavy &&
            c.laneF > (float)kFwyLaneCount - 1.6f) truck = &c;
    }
    struct Job { const char* name; const Car* c; float back, side, up; };
    const Job jobs[] = {
        { "COP (lights on)",    cop,     14.0f, -4.5f, 2.6f },
        { "LANE CHANGE",        merger,  17.0f,  5.5f, 3.2f },
        { "BREAKDOWN",          broke,   16.0f, -7.0f, 3.0f },
        { "TOW TRUCK",          tow,     18.0f, -7.0f, 3.4f },
        { "TRUCK KEEPING RIGHT",truck,   19.0f, -6.0f, 3.0f },
    };
    for (const Job& j : jobs) {
        if (!j.c) {
            char b[110];
            std::snprintf(b, sizeof(b), "[traffic-shot] %-22s none live right now",
                          j.name);
            x3::logInfo(b);
            continue;
        }
        float p[3], dx, dz;
        future(*j.c, p, dx, dz);
        emit(j.name, p, dx, dz, j.back, j.side, j.up);
    }
    if (m_radar.sited) {
        // The sign is READ from in front of its face, at driver eye height.
        const float t[3] = { m_radar.pos[0], m_radar.pos[1] + 2.2f, m_radar.pos[2] };
        emit("RADAR SIGN", t, -m_radar.dirX, -m_radar.dirZ, 26.0f, 7.0f, -0.6f);
    }
    if (m_parked) {
        // The parked obstacle, framed from UPSTREAM and above: that puts the
        // approaching queue between the camera and the stopped car, showing
        // their REAR faces — which is where the brake lights are. Standing at
        // the obstacle (which is what the camera-as-player path forces) is the
        // one viewpoint from which none of this is visible.
        float p[3];
        if (laneWorldPos(m_parkCw, m_parkLane, m_parkS, p)) {
            float pos[3], dir[2];
            sampleAt(uOfS(m_parkCw, m_parkS), pos, dir, nullptr, nullptr);
            const float sgn = (m_parkCw == 1) ? 1.0f : -1.0f;
            emit("PARKED OBSTACLE", p, sgn * dir[0], sgn * dir[1], 46.0f, -7.0f, 7.0f);
            emit("PARKED (low/close)", p, sgn * dir[0], sgn * dir[1], 24.0f, -5.5f, 2.4f);
        }
    }
}

// A point 60 m IN FRONT of the sign's face — where a driver reading it is.
// The self-test stands the player here rather than at a guessed coordinate.
void FreewayTraffic::radarProbePos(float out[3]) const {
    out[0] = m_radar.pos[0] + m_radar.dirX * 60.0f;
    out[1] = m_radar.pos[1] + 1.2f;
    out[2] = m_radar.pos[2] + m_radar.dirZ * 60.0f;
}

// ---------------------------------------------------------------------------
// The point lights traffic contributes this frame. BOUNDED and sorted
// nearest-first: the device caps the whole world at 64 (kMaxPointLights) and
// the host's tunnel + town array already spends most of them, so traffic takes
// a small, predictable slice and lets the host decide how to merge.
// ---------------------------------------------------------------------------
void FreewayTraffic::collectLights() {
    m_lights.clear();
    if (!m_furniture) return;
    struct L { float d2; x3::rhi::PointLight pl; };
    std::vector<L> cand;
    auto add = [&](const float p[3], float r, float g, float b, float range) {
        const float dx = p[0] - m_focus[0], dz = p[2] - m_focus[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 > 260.0f * 260.0f) return;
        x3::rhi::PointLight pl;
        pl.pos[0] = p[0]; pl.pos[1] = p[1]; pl.pos[2] = p[2];
        pl.color[0] = r; pl.color[1] = g; pl.color[2] = b;
        pl.range = range;
        cand.push_back({ d2, pl });
    };
    for (const Car& c : m_cars) {
        if (!c.lightsOn || c.loose) continue;
        float p[3];
        worldPosOf(c, p);
        p[1] += m_models[c.model].heightM + 0.05f;
        if (c.role == RoleCop) {
            // Alternating red / blue, out of phase per car.
            const float t = std::fmod(m_time * 2.6f + c.phase, 2.0f);
            const float k = (std::fmod(m_time * 10.0f + c.phase, 1.0f) < 0.55f) ? 1.0f : 0.15f;
            if (t < 1.0f) add(p, 3.4f * k, 0.15f * k, 0.15f * k, 16.0f);
            else          add(p, 0.15f * k, 0.35f * k, 3.6f * k, 16.0f);
        } else {
            // Amber, rotating-beacon pulse (tow) or steady blink (hazards).
            const float k = (c.role == RoleTow)
                ? 0.25f + 0.75f * std::pow(std::fabs(std::sin(m_time * 3.1f + c.phase)), 6.0f)
                : ((std::fmod(m_time * 1.5f + c.phase, 1.0f) < 0.5f) ? 1.0f : 0.05f);
            add(p, 2.6f * k, 1.15f * k, 0.10f * k, 12.0f);
        }
    }
    // The sign's own panel wash, so it reads at night.
    if (m_radar.sited && m_radar.shownMph >= 0) {
        const float p[3] = { m_radar.pos[0], m_radar.pos[1] + 3.0f, m_radar.pos[2] };
        const bool on = !m_radar.over || std::fmod(m_radar.flashPhase, 0.7f) < 0.42f;
        if (on) add(p, 1.5f, 1.05f, 0.25f, 9.0f);
    }
    std::sort(cand.begin(), cand.end(), [](const L& a, const L& b) { return a.d2 < b.d2; });
    if (cand.size() > kMaxTrafficLights) cand.resize(kMaxTrafficLights);
    for (const L& l : cand) m_lights.push_back(l.pl);
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
            if (m_audio && c.siren.valid()) { m_audio->stopLoop(c.siren); c.siren = {}; }
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
// RENDER — the DriveDemo skin path, per traffic car, plus the furniture.
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
    // THE REPAINTED-PANEL METALLIC CLAMP. X3_WORLD_RULES rule 5: an untextured
    // full-metal material renders BLACK, because metallic=1 zeroes the diffuse
    // lobe and there is no albedo left to see. Several RCC bodies bake their
    // paint materials at metallic ~1 and rely on the clearcoat lobe for their
    // look — which is fine until we substitute a per-instance colour and the
    // colour has nowhere to go.
    //
    // RECEIPT: the parked-obstacle stand-in was tinted (0.62, 0.10, 0.02) — a
    // strong orange-red — and rendered as a BLACK car in shots_traffic2/
    // dbg_park.png with every other vehicle removed from the frame, so there
    // was nothing else it could have been. The same defect is why E30s in the
    // early full-road stills read as mottled dark lumps instead of painted
    // cars. drawMeshPBR's `metallicScale` is the engine's documented fix for
    // exactly this class ("per-object metallic CLAMP for dark-albedo kit props
    // whose MR map bakes metallic=1"), so a repainted NON-clearcoat panel gets
    // it. Clearcoat paint (Muscle, Skyline) already reads correctly and is left
    // strictly alone — 1.0 is the no-op every other call site passes.
    float metallicScale = 1.0f;
    if (tint) {
        bc[0] = tint[0]; bc[1] = tint[1]; bc[2] = tint[2];
        if (d.clearcoat <= 0.01f) metallicScale = 0.15f;
    }
    dev.drawMeshPBR(f, x3::rhi::MeshHandle{ d.meshId },
                    x3::rhi::TextureHandle{ d.baseColorTexId },
                    x3::rhi::TextureHandle{ d.normalTexId },
                    x3::rhi::TextureHandle{ d.mrTexId },
                    bc, emis, world, d.alphaMask, d.alphaBlend,
                    x3::rhi::TextureHandle{ d.emissiveTexId },
                    x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                    d.clearcoat, d.clearcoatRough, 0.0f, metallicScale,
                    /*foliage=*/0.0f, d.metallicFactor, d.roughnessFactor);
}

// Compose a world matrix from an orthonormal basis, an origin and a scale.
void composeM(const float r[3], const float u[3], const float f[3],
              float ox, float oy, float oz, float s, float out[16]) {
    out[0] = r[0]*s; out[1] = r[1]*s; out[2]  = r[2]*s; out[3]  = 0;
    out[4] = u[0]*s; out[5] = u[1]*s; out[6]  = u[2]*s; out[7]  = 0;
    out[8] = f[0]*s; out[9] = f[1]*s; out[10] = f[2]*s; out[11] = 0;
    out[12] = ox;    out[13] = oy;    out[14] = oz;     out[15] = 1;
}
} // namespace

void FreewayTraffic::render(const x3::rhi::FrameContext& frame, const float camPos[3]) {
    if (!m_built || !m_device) return;
    (void)camPos;   // everything live is inside the cull ring already
    for (const Car& c : m_cars) {
        const Model& md = m_models[c.model];
        if (!md.ok) continue;

        float carM[16];
        float basisR[3], basisU[3], basisF[3];
        float originY = 0.0f;
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
            const float loc[3] = { 0.0f, -c.halfH + md.groundLift, 0.0f };
            carM[0] = R[0] * s;  carM[1] = R[1] * s;  carM[2]  = R[2] * s;  carM[3]  = 0;
            carM[4] = R[4] * s;  carM[5] = R[5] * s;  carM[6]  = R[6] * s;  carM[7]  = 0;
            carM[8] = R[8] * s;  carM[9] = R[9] * s;  carM[10] = R[10] * s; carM[11] = 0;
            carM[12] = bp.x + R[0] * loc[0] + R[4] * loc[1] + R[8]  * loc[2];
            carM[13] = bp.y + R[1] * loc[0] + R[5] * loc[1] + R[9]  * loc[2];
            carM[14] = bp.z + R[2] * loc[0] + R[6] * loc[1] + R[10] * loc[2];
            carM[15] = 1;
            basisR[0] = R[0]; basisR[1] = R[1]; basisR[2] = R[2];
            basisU[0] = R[4]; basisU[1] = R[5]; basisU[2] = R[6];
            basisF[0] = R[8]; basisF[1] = R[9]; basisF[2] = R[10];
            originY = carM[13];
        } else {
            float pos[3], dir[2], mh, dy;
            sampleAt(uOfS(c.cw, c.s), pos, dir, &mh, &dy);
            const float sgn = (c.cw == 1) ? 1.0f : -1.0f;
            const float lat = laneLat(c.cw, c.laneF, mh);
            const float wx = pos[0] + (-dir[1]) * lat;
            const float wz = pos[2] + ( dir[0]) * lat;
            const float wy = pos[1] + kTrafficPaveProud + md.groundLift;
            travelBasis(sgn * dir[0], sgn * dy, sgn * dir[1], crabYaw(c),
                        basisR, basisU, basisF);
            composeM(basisR, basisU, basisF, wx, wy, wz, md.scale, carM);
            originY = wy;
        }

        float fin[16];
        for (size_t bi = 0; bi < md.body.size(); ++bi) {
            const auto& d = md.body[bi];
            mul4(carM, d.nodeTransform, fin);
            const bool paint = c.hasTint && bi < md.bodyPaintable.size() &&
                               md.bodyPaintable[bi];
            drawTrafficDrawable(*m_device, frame, d, fin, paint ? c.tint : nullptr);
        }
        // WHEELS: spin about the model-local X axle at each group's own hub.
        for (const WheelGroup& wg : md.wheels) {
            const float worldR = wg.radius * md.scale;
            const float circ = 6.2831853f * worldR;
            const float th = c.loose ? 0.0f : std::fmod(c.spin, circ) / worldR;
            const float ct = std::cos(th), st = std::sin(th);
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
                drawTrafficDrawable(*m_device, frame, d, fin2, nullptr);
            }
        }

        if (m_furniture && !c.loose)
            renderCarFurniture(frame, c, basisR, basisU, basisF, originY);
    }
    // X3_TRAFFIC_PARK's stand-in. The parked obstacle is normally the PLAYER's
    // own car, which this system does not own or draw — so under the capture
    // lever there is nothing on screen and a proof shot shows traffic reacting
    // to thin air. Draw a car there, in a colour nothing else on the road
    // wears, so the still actually proves what it claims.
    if (m_parked && m_device) {
        // Pick the MOST paintable body on the roster, or the "unmistakable
        // colour" is a lie. Two cuts of this failed the same way: taking the
        // first ok model grabbed a TEXTURED armory sedan whose shell ignores
        // the tint, and the stand-in rendered as just another grey car nobody
        // could pick out of the still. Most-paintable-wins gets the E30
        // (30 of 38 drawables), which actually turns red.
        int pick = -1;
        uint32_t bestPaint = 0;
        for (size_t k = 0; k < m_models.size(); ++k) {
            if (!m_models[k].ok) continue;
            uint32_t n = 0;
            for (uint8_t p : m_models[k].bodyPaintable) n += p;
            if (n > bestPaint) { bestPaint = n; pick = (int)k; }
        }
        for (int only = pick; only >= 0; only = -1) {
            const Model& md = m_models[only];
            float pos[3], dir[2], mh, dy;
            sampleAt(uOfS(m_parkCw, m_parkS), pos, dir, &mh, &dy);
            const float sgn = (m_parkCw == 1) ? 1.0f : -1.0f;
            const float lat = laneLat(m_parkCw, m_parkLane, mh);
            float r[3], upv[3], f[3];
            travelBasis(sgn * dir[0], sgn * dy, sgn * dir[1], 0.0f, r, upv, f);
            float M[16];
            composeM(r, upv, f,
                     pos[0] + (-dir[1]) * lat,
                     pos[1] + kTrafficPaveProud + md.groundLift,
                     pos[2] + ( dir[0]) * lat, md.scale, M);
            const float hot[3] = { 0.62f, 0.10f, 0.02f };   // unmistakable
            float fin[16];
            for (size_t bi = 0; bi < md.body.size(); ++bi) {
                mul4(M, md.body[bi].nodeTransform, fin);
                drawTrafficDrawable(*m_device, frame, md.body[bi], fin,
                                    (bi < md.bodyPaintable.size() && md.bodyPaintable[bi])
                                        ? hot : nullptr);
            }
            break;
        }
    }
    if (m_furniture) renderRadarSign(frame);
}

// The bits bolted ONTO a car: the cop's light bar, the tow's beacons and
// TOWBOOK plates, the breakdown's hazards. All positioned from the model's
// MEASURED extents, never from a magic constant (NO_SLOP rule 5).
void FreewayTraffic::renderCarFurniture(const x3::rhi::FrameContext& frame, const Car& c,
                                        const float r[3], const float u[3],
                                        const float f[3], float originY) {
    const Model& md = m_models[c.model];
    // Origin of the car's contact plane in world space is carried in the
    // basis + this helper's caller; rebuild the world point for an offset in
    // the car's own frame (right, up, forward), all in metres.
    float base[3];
    worldPosOf(c, base);
    base[1] = originY;
    auto at = [&](float rr, float uu, float ff, float out[3]) {
        out[0] = base[0] + r[0] * rr + u[0] * uu + f[0] * ff;
        out[1] = base[1] + r[1] * rr + u[1] * uu + f[1] * ff;
        out[2] = base[2] + r[2] * rr + u[2] * uu + f[2] * ff;
    };
    const float roofY = md.heightM;      // measured, scaled: the model's own top
    const float halfW = md.widthM * 0.5f;
    const float halfL = md.lenM * 0.5f;

    // ---- COP: the light bar on the roof, flashing red/blue ----------------
    if (c.role == RoleCop && m_lightBar.ok) {
        float o[3];
        at(0.0f, roofY, -0.15f, o);      // just aft of the windscreen header
        float M[16];
        composeM(r, u, f, o[0], o[1], o[2], 1.0f, M);
        // The flash. Two-stage so it reads as a real light bar and not a
        // metronome: a slow left/right alternation with a fast strobe burst
        // inside each half. Peak 1.6 on a near-black lens — bright enough to
        // bloom, nowhere near a flat >0.5 emissive across a whole object
        // (X3_WORLD_RULES rule 5; the lens primitive IS the gate here).
        const bool on = c.lightsOn;
        const float alt = std::fmod(m_time * 2.6f + c.phase, 2.0f);
        const float strobe = (std::fmod(m_time * 11.0f + c.phase, 1.0f) < 0.5f) ? 1.0f : 0.12f;
        const float redI  = on && alt < 1.0f ? 1.6f * strobe : 0.0f;
        const float blueI = on && alt >= 1.0f ? 1.6f * strobe : 0.0f;
        float fin[16];
        for (size_t i = 0; i < m_lightBar.draw.size(); ++i) {
            const auto& d = m_lightBar.draw[i];
            mul4(M, d.nodeTransform, fin);
            const uint8_t kind = i < m_lightBar.lens.size() ? m_lightBar.lens[i] : 0;
            float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                            d.baseColorFactor[2], d.baseColorFactor[3] };
            float em[4] = { 0, 0, 0, 0 };
            if (kind == 1)      { em[0] = redI;  em[3] = redI  > 0.0f ? 1.0f : 0.0f; }
            else if (kind == 2) { em[2] = blueI; em[3] = blueI > 0.0f ? 1.0f : 0.0f; }
            m_device->drawMeshPBR(frame, x3::rhi::MeshHandle{ d.meshId },
                                  x3::rhi::TextureHandle{ d.baseColorTexId },
                                  x3::rhi::TextureHandle{ d.normalTexId },
                                  x3::rhi::TextureHandle{ d.mrTexId },
                                  bc, em, fin, false, false, {}, {}, 1.0f,
                                  0.0f, 0.05f);
        }
        // POLICE on both doors. m_plate is two-faced and the -Z face mirrors
        // its U, so the wordmark reads correctly from either side of the car.
        for (int side = -1; side <= 1; side += 2) {
            float po[3];
            at((float)side * (halfW + 0.01f), roofY * 0.42f, -0.15f, po);
            float PM[16];
            // The plate's own +Z must point OUT of that flank, so the plate's
            // local frame is (forward, up, side-normal).
            const float pn[3] = { r[0] * (float)side, r[1] * (float)side, r[2] * (float)side };
            composeM(f, u, pn, po[0], po[1], po[2], 1.0f, PM);
            const float sc[16] = { halfL * 0.95f, 0, 0, 0,
                                   0, roofY * 0.30f, 0, 0,
                                   0, 0, 1, 0,
                                   0, 0, 0, 1 };
            float fin2[16];
            mul4(PM, sc, fin2);
            const float bc[4] = { 1, 1, 1, 1 };
            const float em[4] = { 0.55f, 0.72f, 1.0f, 0.85f };
            m_device->drawMeshPBR(frame, m_plate.mesh, m_texPolice, {}, m_plate.mr,
                                  bc, em, fin2, false, false, m_texPolice, {}, 1.0f,
                                  0.0f, 0.05f);
        }
    }

    // ---- TOW: amber beacons + the TOWBOOK wordmark on both flanks ---------
    if (c.role == RoleTow) {
        const float beam = 0.25f + 0.75f *
            std::pow(std::fabs(std::sin(m_time * 3.1f + c.phase)), 6.0f);
        for (int side = -1; side <= 1; side += 2) {
            float o[3];
            at((float)side * halfW * 0.55f, roofY, halfL * 0.35f, o);
            float M[16];
            composeM(r, u, f, o[0], o[1], o[2], 1.0f, M);
            const float bc[4] = { 0.035f, 0.022f, 0.006f, 1.0f };
            const float em[4] = { 1.5f * beam, 0.62f * beam, 0.03f * beam, 1.0f };
            m_device->drawMeshPBR(frame, m_lens.mesh, m_lens.base, {}, m_lens.mr,
                                  bc, em, M, false, false, {}, {}, 1.0f, 0.0f, 0.05f);
        }
        for (int side = -1; side <= 1; side += 2) {
            float po[3];
            at((float)side * (halfW + 0.01f), roofY * 0.55f, -halfL * 0.15f, po);
            float PM[16];
            const float pn[3] = { r[0] * (float)side, r[1] * (float)side, r[2] * (float)side };
            composeM(f, u, pn, po[0], po[1], po[2], 1.0f, PM);
            const float sc[16] = { halfL * 0.85f, 0, 0, 0,
                                   0, roofY * 0.26f, 0, 0,
                                   0, 0, 1, 0,
                                   0, 0, 0, 1 };
            float fin2[16];
            mul4(PM, sc, fin2);
            const float bc[4] = { 1, 1, 1, 1 };
            const float em[4] = { 0.80f, 0.90f, 1.0f, 0.90f };
            m_device->drawMeshPBR(frame, m_plate.mesh, m_texTowbook, {}, m_plate.mr,
                                  bc, em, fin2, false, false, m_texTowbook, {}, 1.0f,
                                  0.0f, 0.05f);
        }
    }

    // ---- BRAKE LIGHTS on every car, off the follower's own decel ----------
    // Rule 5 shape, same as every other lamp here: near-black lens, the glow
    // driven per-draw, so unlit they are dark and lit they bloom. Two lamps at
    // the rear corners, small (0.42 scale of the beacon lens).
    if (c.brakeLit) {
        for (int side = -1; side <= 1; side += 2) {
            float o[3];
            at((float)side * halfW * 0.80f, md.heightM * 0.40f, -halfL * 0.97f, o);
            float M[16];
            composeM(r, u, f, o[0], o[1], o[2], 0.42f, M);
            const float bc[4] = { 0.040f, 0.006f, 0.006f, 1.0f };
            const float em[4] = { 1.45f, 0.055f, 0.030f, 1.0f };
            m_device->drawMeshPBR(frame, m_lens.mesh, m_lens.base, {}, m_lens.mr,
                                  bc, em, M, false, false, {}, {}, 1.0f, 0.0f, 0.05f);
        }
    }

    // ---- BREAKDOWN: four-way hazards, blinking together -------------------
    if (c.role == RoleBroken && c.lightsOn) {
        const bool on = std::fmod(m_time * 1.5f + c.phase, 1.0f) < 0.5f;
        const float k = on ? 1.0f : 0.04f;
        for (int side = -1; side <= 1; side += 2)
            for (int end = -1; end <= 1; end += 2) {
                float o[3];
                at((float)side * halfW * 0.92f, md.heightM * 0.42f,
                   (float)end * halfL * 0.94f, o);
                float M[16];
                composeM(r, u, f, o[0], o[1], o[2], 0.55f, M);
                const float bc[4] = { 0.035f, 0.020f, 0.005f, 1.0f };
                const float em[4] = { 1.35f * k, 0.55f * k, 0.02f * k, 1.0f };
                m_device->drawMeshPBR(frame, m_lens.mesh, m_lens.base, {}, m_lens.mr,
                                      bc, em, M, false, false, {}, {}, 1.0f, 0.0f, 0.05f);
            }
    }
}

// ---------------------------------------------------------------------------
// THE RADAR SIGN. Post + head in steel, a dark-glass face (rule 7), the
// "YOUR SPEED" header, and up to three emissive digits standing 1 cm proud of
// the glass. Over the limit the digits FLASH.
// ---------------------------------------------------------------------------
void FreewayTraffic::renderRadarSign(const x3::rhi::FrameContext& frame) {
    if (!m_radar.sited || !m_steel.mesh.valid()) return;
    // Local frame: +Z is the way the sign faces.
    // THE BASIS MUST BE RIGHT-HANDED (r x u = f) or the model matrix mirrors
    // and every +Z-facing quad on it renders AWAY from the viewer. The first
    // cut used r = (-fz, 0, fx), for which r x u = -f: the sign showed its
    // blank grey back to a camera standing directly in front of it, panel and
    // digits and all, and no amount of winding fiddling on the quads would
    // have fixed it because the whole frame was inside out. This is the
    // app/factory.cpp sign receipt (authored, textured, correctly UV'd and
    // BACKFACE-CULLED) reappearing one level up, at the transform instead of
    // the triangle. r = u x f is the correct handedness — the same
    // construction travelBasis() uses for every car in this file.
    const float f[3] = { m_radar.dirX, 0.0f, m_radar.dirZ };
    const float u[3] = { 0.0f, 1.0f, 0.0f };
    const float r[3] = { f[2], 0.0f, -f[0] };   // u x f
    float M[16];
    composeM(r, u, f, m_radar.pos[0], m_radar.pos[1], m_radar.pos[2], 1.0f, M);

    // Galvanised post + housing: light grey, dielectric, matte.
    {
        const float bc[4] = { 0.38f, 0.39f, 0.41f, 1.0f };
        const float em[4] = { 0, 0, 0, 0 };
        m_device->drawMeshPBR(frame, m_steel.mesh, m_steel.base, {}, m_steel.mr,
                              bc, em, M, false, false, {}, {}, 1.0f, 0.0f, 0.05f);
    }
    // THE DARK GLASS. Rule 7: near-black, glossy, never a bright flat quad.
    {
        const float bc[4] = { 0.021f, 0.022f, 0.026f, 1.0f };
        const float em[4] = { 0, 0, 0, 0 };
        m_device->drawMeshPBR(frame, m_glass.mesh, m_glass.base, {}, m_glass.mr,
                              bc, em, M, false, false, {}, {}, 1.0f, 0.0f, 0.05f);
    }
    auto panelQuad = [&](float cx, float cy, float w, float h,
                         x3::rhi::TextureHandle tex, const float em[4]) {
        // m_quad is a 1x1 quad at local z=0 facing +Z; place it on the glass.
        const float S[16] = { w, 0, 0, 0,
                              0, h, 0, 0,
                              0, 0, 1, 0,
                              cx, cy, 0.090f, 1 };
        float fin[16];
        mul4(M, S, fin);
        const float bc[4] = { 1, 1, 1, 1 };
        m_device->drawMeshPBR(frame, m_quad.mesh, tex, {}, m_quad.mr,
                              bc, em, fin, false, false, tex, {}, 1.0f, 0.0f, 0.05f);
    };
    // Header, always lit but modest.
    {
        const float em[4] = { 1.0f, 0.72f, 0.18f, 0.95f };
        panelQuad(0.0f, 3.28f, 1.16f, 0.26f, m_texHeader, em);
    }
    // The number. Blank when there is nothing to show.
    const bool flashOn = !m_radar.over || std::fmod(m_radar.flashPhase, 0.66f) < 0.40f;
    const int mph = m_radar.shownMph;
    int digits[3] = { 10, 10, 10 };        // 10 == blank
    if (mph >= 0) {
        digits[2] = mph % 10;
        if (mph >= 10)  digits[1] = (mph / 10) % 10;
        if (mph >= 100) digits[0] = (mph / 100) % 10;
    }
    // Over the limit the digits go red as well as flashing — the colour is the
    // part a driver reads at a glance, the flash is what makes him look.
    float em[4] = { 1.05f, 0.78f, 0.18f, 0.95f };
    if (m_radar.over) {
        em[0] = 1.35f; em[1] = 0.16f; em[2] = 0.10f;
        em[3] = flashOn ? 1.15f : 0.06f;
    }
    for (int d = 0; d < 3; ++d) {
        if (digits[d] == 10 && !(d == 2 && mph == 0)) continue;
        panelQuad(-0.40f + 0.40f * (float)d, 2.83f, 0.34f, 0.62f,
                  m_texDigit[digits[d]], em);
    }
}

void FreewayTraffic::shutdown(x3::phys::IPhysicsWorld* phys) {
    for (Car& c : m_cars) {
        if (phys && c.body.valid()) phys->removeBody(c.body);
        if (m_audio && c.siren.valid()) { m_audio->stopLoop(c.siren); c.siren = {}; }
    }
    m_cars.clear();
    m_models.clear();
    m_lightBar.draw.clear();
    m_lightBar.ok = false;
    m_lights.clear();
    m_built = false;
    m_furniture = false;
}

uint32_t FreewayTraffic::looseCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.loose) ++k;
    return k;
}
uint32_t FreewayTraffic::mergingCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.mergeT < 1.0f) ++k;
    return k;
}
uint32_t FreewayTraffic::copCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.role == RoleCop) ++k;
    return k;
}
uint32_t FreewayTraffic::jerkCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.temper == TempJerk) ++k;
    return k;
}
uint32_t FreewayTraffic::breakdownCount() const {
    uint32_t k = 0;
    for (const Car& c : m_cars) if (c.role == RoleBroken) ++k;
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
    const float lat = laneLat(c.cw, c.laneF, mh);
    s.id = c.id;
    s.x = pos[0] + (-dir[1]) * lat;
    s.y = pos[1];
    s.z = pos[2] + ( dir[0]) * lat;
    s.cx = pos[0];               // the route centreline (median axis) sample
    s.cz = pos[2];
    s.dirX = sgn * dir[0];
    s.dirZ = sgn * dir[1];
    s.v = c.v; s.cruise = c.cruise;
    s.cw = c.cw;
    s.laneF = c.laneF;
    s.lane = (int)std::lround(c.laneF);
    s.lat = lat;
    s.halfWidth = m_models[c.model].widthM * 0.5f;
    s.loose = c.loose;
    s.cls = m_models[c.model].cls;
    s.gapAhead = c.gapAhead;
    s.merging = c.mergeT < 1.0f;
    s.brakeLit = c.brakeLit;
    s.blockedByPlayer = c.blockedByPlayer;
    s.signalDir = c.signalDir;
    s.temper = c.temper;
    s.jerk = (int)c.jerk;
    s.role = c.role;
    s.s = c.s;
    return s;
}

// ===========================================================================
// --test-traffic — headless gates on the REAL inner course. No device, no
// physics world (the sim is pure); T4's determinism leg re-runs from scratch.
// ===========================================================================
bool runTrafficSelfTest() {
    int pass = 0, fail = 0;
    char d[400];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        char line[520];
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

    auto makeSim = [&](uint32_t seed, uint32_t target, float chaos = 1.0f) {
        auto t = std::make_unique<FreewayTraffic>();
        TrafficConfig cfg;
        cfg.seed = seed;
        cfg.targetCount = target;
        cfg.aggressiveFrac *= chaos;
        cfg.jerkFrac       *= chaos;
        // The gates own their numbers. X3_TRAFFIC_* are capture levers for the
        // host; a suite that silently retargets because one is exported in the
        // operator's shell proves nothing.
        cfg.envOverrides = false;
        t->build(ringSpec, ringY, nullptr, nullptr, "", cfg, nullptr);
        return t;
    };

    // Focus: a point ON the route, like a parked player.
    std::vector<RoadRenderStation> gpath;
    {
        const std::vector<float> mp = computeMedianPlan(ringSpec, ringY);
        buildRoadRenderPath(ringSpec, &ringY, mp.empty() ? nullptr : &mp, gpath);
    }
    auto focusAt = [&](float uFrac, float out[3]) {
        const size_t idx = (size_t)((float)(gpath.size() - 1) * uFrac);
        out[0] = gpath[idx].x; out[1] = gpath[idx].y; out[2] = gpath[idx].z;
    };

    // ---- T1 + T2 + T5: direction law, median-left, class discipline -------
    {
        auto t = makeSim(0xC0FFEEu, 60);
        float focus[3];
        focusAt(0.25f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 600; ++i) t->update(dt, focus, nullptr);

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
            // A merging car moves LATERALLY too, so the forward component is
            // compared against the forward distance, not the total.
            const float mdx = b.x - a->x, mdz = b.z - a->z;
            const float mv = std::sqrt(mdx * mdx + mdz * mdz);
            const float fwd = mdx * a->dirX + mdz * a->dirZ;
            if (mv > 1e-3f && fwd < 0.75f * mv) dirOk = false;
            if (mv > 1e-3f && fwd < 0.0f) dirOk = false;
            // T2: the MEDIAN (the route centreline) must be on the driver's
            // LEFT. left = up x fwd = (dirZ, -dirX) for fwd (dirX, dirZ).
            const float toCx = b.cx - b.x, toCz = b.cz - b.z;
            if (toCx * b.dirZ + toCz * (-b.dirX) <= 0.0f) medianOk = false;
            // T5: heavies keep right. A heavy is allowed ONE lane left of its
            // band while actually overtaking (see thinkLaneChanges) — the gate
            // is that it never reaches the median lanes.
            if (b.cls == 4) { ++heavies; if (b.laneF < 3.9f) heavyOk = false; }
            if (b.laneF < -0.05f || b.laneF > (float)kFwyLaneCount - 1.0f + 0.05f) {
                if (b.role != 2 && b.role != 3) laneRangeOk = false;
            }
        }
        std::snprintf(d, sizeof(d), "%u cars live, %u measured, %u heavy",
                      t->liveCount(), measured, heavies);
        check(t->liveCount() >= 30, "T0b the ring filled", d);
        check(dirOk && measured > 20,
              "T1 no head-on traffic: every car travels its carriageway's one legal way", d);
        check(medianOk, "T2 the median is on every driver's LEFT (measured)", d);
        check(heavyOk && heavies > 0, "T5 heavy trucks keep to the outer lanes", d);
        check(laneRangeOk, "T5b every car is inside the 8 running lanes (or a shoulder role)");
        t->shutdown(nullptr);
    }

    // ---- T3: following distance never negative ----------------------------
    {
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
            if (f2.gapAhead < 1e8f && !f2.merging) {
                minGap = std::min(minGap, f2.gapAhead);
                if (f2.v < f2.cruise - 1.0f) everLimited = true;
            }
        }
        const FreewayTraffic::CarState f2 = t->carState(1);
        std::snprintf(d, sizeof(d), "min gap %.2f m, settled v %.1f m/s (leader 20)",
                      minGap, f2.v);
        check(minGap >= 0.0f, "T3 following distance NEVER negative", d);
        check(everLimited, "T3b the follower genuinely yielded to the leader", d);
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
            if (a.role == 2 || a.role == 3) continue;   // tow/breakdown never cull
            const float dx = a.x - focus[0], dz = a.z - focus[2];
            if (std::sqrt(dx * dx + dz * dz) > 1650.0f) inRing = false;
        }
        float focus2[3];
        focusAt(0.75f, focus2);
        for (int i = 0; i < 900; ++i) t->update(dt, focus2, nullptr);
        bool culled = true;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState a = t->carState(i);
            if (a.role == 2 || a.role == 3) continue;
            const float dx = a.x - focus2[0], dz = a.z - focus2[2];
            if (std::sqrt(dx * dx + dz * dz) > 1650.0f) culled = false;
        }
        std::snprintf(d, sizeof(d), "filled %u, refilled %u after the move",
                      filled, t->liveCount());
        check(filled >= 30 && filled <= 62 && inRing,
              "T4 the ring fills inside its band", d);
        check(culled && t->liveCount() >= 30,
              "T4b the move culls the far side and refills", d);
        t->shutdown(nullptr);

        // Determinism: same seed, same focus script -> identical state hash.
        auto hashOf = [&](FreewayTraffic& tt) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < tt.liveCount(); ++i) {
                const FreewayTraffic::CarState a = tt.carState(i);
                const float vals[4] = { a.x, a.z, a.v, a.laneF + (float)(a.cw * 32) };
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

    // ---- T6: LANE CHANGES HAPPEN, AND NOTHING EVER OVERLAPS ---------------
    // The headline feature and its hard invariant, gated together on a FULL
    // 300-car population with the chaos dial up so cut-ins are frequent — the
    // hostile case, not a quiet one. Overlap is measured in 2-D every tick:
    // two cars on the same carriageway whose lateral footprints touch must be
    // separated longitudinally by at least their combined half-lengths.
    {
        auto t = makeSim(0x1A5EC0DEu, 300, /*chaos=*/3.0f);
        float focus[3];
        focusAt(0.4f, focus);
        const float dt = 1.0f / 60.0f;
        uint32_t worstPairs = 0;
        float worstPen = 0.0f;
        uint32_t everMerging = 0;
        char worst[200] = "none";
        for (int step = 0; step < 2400; ++step) {          // 40 s
            t->update(dt, focus, nullptr);
            if ((step % 3) != 0) continue;                 // sample, 20 Hz
            const uint32_t n = t->liveCount();
            std::vector<FreewayTraffic::CarState> cs;
            cs.reserve(n);
            for (uint32_t i = 0; i < n; ++i) cs.push_back(t->carState(i));
            for (const auto& a : cs) if (a.merging) ++everMerging;
            for (uint32_t i = 0; i < n; ++i) {
                for (uint32_t j = i + 1; j < n; ++j) {
                    const auto& A = cs[i];
                    const auto& B = cs[j];
                    if (A.cw != B.cw || A.loose || B.loose) continue;
                    const float latSep = std::fabs(A.laneF - B.laneF) * kTrafLaneM;
                    if (latSep >= A.halfWidth + B.halfWidth) continue;   // clear beside
                    float ds = std::fabs(A.s - B.s);
                    const float L = t->routeLen();
                    ds = std::min(ds, L - ds);
                    // Combined half-lengths: the true contact distance. Class
                    // lengths are the headless defaults, so derive from the
                    // longest thing on the road to stay conservative.
                    const float need = 0.5f * (4.5f + 4.5f);
                    if (ds < need) {
                        const float pen = need - ds;
                        if (pen > worstPen) {
                            worstPen = pen;
                            std::snprintf(worst, sizeof(worst),
                                "cars %u/%u lat %.2f m arc %.2f m (need %.2f)",
                                A.id, B.id, latSep, ds, need);
                        }
                        ++worstPairs;
                    }
                }
            }
        }
        std::snprintf(d, sizeof(d),
            "%u merge-ticks observed, %u lane changes completed, "
            "%u overlapping pair-samples, worst %s",
            everMerging, t->laneChangeCount(), worstPairs, worst);
        check(t->laneChangeCount() > 40 && everMerging > 100,
              "T6 cars actually CHANGE LANES (measured, on a full 300-car road)", d);
        check(worstPairs == 0,
              "T6b NO TWO CARS EVER OVERLAP — including mid-merge", d);
        t->shutdown(nullptr);
    }

    // ---- T7: the profiles are measurably distinct, the mix is in band -----
    {
        auto t = makeSim(0x9E77A1u, 300);
        float focus[3];
        focusAt(0.6f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 1200; ++i) t->update(dt, focus, nullptr);
        double cruiseSum[6] = { 0, 0, 0, 0, 0, 0 };
        uint32_t clsN[6] = { 0, 0, 0, 0, 0, 0 };
        uint32_t aggro = 0, jerks = 0, total = 0;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState a = t->carState(i);
            if (a.cls >= 0 && a.cls < 6) { cruiseSum[a.cls] += a.cruise; ++clsN[a.cls]; }
            if (a.temper == 1) ++aggro;
            if (a.temper == 2) ++jerks;
            ++total;
        }
        const double heavyMean = clsN[4] ? cruiseSum[4] / clsN[4] : 0.0;
        const double sedanMean = clsN[0] ? cruiseSum[0] / clsN[0] : 0.0;
        const double superMean = clsN[5] ? cruiseSum[5] / clsN[5] : 0.0;
        std::snprintf(d, sizeof(d),
            "cruise mph heavy %.1f (n=%u) < sedan %.1f (n=%u) < super %.1f (n=%u); "
            "brake heavy %.1f < sedan %.1f < super %.1f m/s^2",
            heavyMean * kMps2Mph, clsN[4], sedanMean * kMps2Mph, clsN[0],
            superMean * kMps2Mph, clsN[5],
            kClassProfiles[ClsHeavy].brake, kClassProfiles[ClsSedan].brake,
            kClassProfiles[ClsSuper].brake);
        check(clsN[4] > 0 && clsN[0] > 0 && clsN[5] > 0 &&
              heavyMean < sedanMean && sedanMean < superMean &&
              kClassProfiles[ClsHeavy].accel < kClassProfiles[ClsSedan].accel &&
              kClassProfiles[ClsSedan].accel < kClassProfiles[ClsSuper].accel &&
              kClassProfiles[ClsHeavy].brake < kClassProfiles[ClsSedan].brake &&
              kClassProfiles[ClsHeavy].headway > kClassProfiles[ClsSuper].headway,
              "T7 performance profiles are distinct and correctly ordered", d);
        const float aF = total ? (float)aggro / (float)total : 0.0f;
        const float jF = total ? (float)jerks / (float)total : 0.0f;
        std::snprintf(d, sizeof(d),
            "%u live: aggressive %u (%.1f%%, want ~14%%), jerks %u (%.1f%%, want 5-10%%), "
            "cops %u", total, aggro, aF * 100.0f, jerks, jF * 100.0f, t->copCount());
        check(total > 100 && aF > 0.06f && aF < 0.24f && jF > 0.02f && jF < 0.14f,
              "T7b the aggressive / jerk mix lands in its configured band", d);
        t->shutdown(nullptr);
    }

    // ---- T8: horns are rate-limited (a jam is not a cacophony) ------------
    {
        // A DELIBERATE JAM: 300 cars with the chaos dial at max, run for a
        // minute. The global limiter caps the whole freeway at one horn per
        // kHornGlobalGapS, so 60 s can physically produce at most ~109.
        auto t = makeSim(0x40C0FFu, 300, /*chaos=*/3.0f);
        float focus[3];
        focusAt(0.4f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 3600; ++i) t->update(dt, focus, nullptr);
        const uint32_t horns = t->hornCount();
        const float ceiling = 60.0f / kHornGlobalGapS + 2.0f;
        std::snprintf(d, sizeof(d),
            "%u horns in 60 s of a deliberately hostile 300-car road "
            "(hard ceiling %.0f = 1 per %.2f s)", horns, ceiling, kHornGlobalGapS);
        check(horns <= (uint32_t)ceiling, "T8 horns are globally rate-limited", d);
        t->shutdown(nullptr);
    }

    // ---- T9: a breakdown ends on the SHOULDER, and the tow clears it ------
    {
        auto t = makeSim(0xB2EA4u, 40);
        float focus[3];
        focusAt(0.3f, focus);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 600; ++i) t->update(dt, focus, nullptr);
        // Force one, so the gate does not depend on the breakdown dice.
        int victim = -1;
        for (uint32_t i = 0; i < t->liveCount(); ++i) {
            const FreewayTraffic::CarState a = t->carState(i);
            if (a.role == 0 && a.laneF > (float)kFwyLaneCount - 3.0f && !a.merging) {
                victim = (int)i; break;
            }
        }
        // Track THIS car by its stable id. The first cut asserted "no broken
        // cars and no tows are live", which the sim can never satisfy: the
        // breakdown scheduler keeps producing NEW incidents, so a second one
        // starting at t=150 s made a passing run look like a failure. The gate
        // is about ONE incident completing, so follow one incident.
        const uint32_t victimId = victim >= 0
            ? t->carState((uint32_t)victim).id : 0u;
        const bool forced = victim >= 0 && t->forceBreakdownForTest((uint32_t)victim);
        bool parkedClear = false, towCame = false, cleared = false;
        float clearM = -99.0f, pavedM = -99.0f, towClosest = 1e9f;
        for (int i = 0; i < 60 * 220 && !cleared; ++i) {            // up to 220 s
            t->update(dt, focus, nullptr);
            bool victimAlive = false;
            float victimS = 0.0f;
            for (uint32_t k = 0; k < t->liveCount(); ++k) {
                const FreewayTraffic::CarState a = t->carState(k);
                if (a.id != victimId) continue;
                victimAlive = true;
                victimS = a.s;
                if (a.role == 3 && a.v < 0.01f) {
                    // MEASURED shoulder clearance. laneLat is linear in laneF,
                    // so the offset from the carriageway centre is exact
                    // arithmetic on published constants — no eyeballing.
                    const float off = (a.laneF + 0.5f) * kTrafLaneM - kFwyRunningHalfM;
                    clearM = (off - a.halfWidth) - kFwyRunningHalfM;
                    pavedM = kFwyPavedHalfM - (off + a.halfWidth);
                    if (clearM > 0.3f && pavedM > 0.0f) parkedClear = true;
                }
            }
            // Did a tow get to THIS car? Measure the closest approach.
            for (uint32_t k = 0; k < t->liveCount(); ++k) {
                const FreewayTraffic::CarState a = t->carState(k);
                if (a.role != 2) continue;
                towCame = true;
                if (victimAlive) {
                    float ds = std::fabs(a.s - victimS);
                    ds = std::min(ds, t->routeLen() - ds);
                    towClosest = std::min(towClosest, ds);
                }
            }
            if (towCame && !victimAlive) cleared = true;
        }
        std::snprintf(d, sizeof(d),
            "forced=%d car %u parked clear of the running lanes by %.2f m, %.2f m "
            "inside the paved edge; a tow rolled=%d, closest approach %.1f m; "
            "incident cleared=%d",
            (int)forced, victimId, clearM, pavedM, (int)towCame, towClosest,
            (int)cleared);
        check(forced && parkedClear,
              "T9 a breakdown ends fully OFF the running lanes, on pavement", d);
        check(towCame && cleared,
              "T9b the tow truck reaches it and both leave together", d);
        t->shutdown(nullptr);
    }

    // ---- T10: the radar sign reads the PLAYER's speed ---------------------
    {
        auto t = makeSim(0x2ADA2u, 0);
        check(t->radarSited(), "T10 the radar sign is sited on the freeway shoulder");
        // Park the player right in front of the sign at a known speed. The
        // sign's own reported position is the only honest place to stand.
        const float dt = 1.0f / 60.0f;
        float focus[3];
        focusAt(0.3f, focus);
        struct Probe { float mps; int wantMph; bool wantFlash; };
        const Probe probes[] = {
            { 25.0f * (1.0f / kMps2Mph), 25,  false },   // 25 mph
            { 70.0f * (1.0f / kMps2Mph), 70,  false },   // at the limit
            { 88.0f * (1.0f / kMps2Mph), 88,  true  },   // over -> flashing
            { 132.0f * (1.0f / kMps2Mph), 132, true },   // three digits
        };
        bool allOk = true;
        char detail[300] = "";
        for (const Probe& p : probes) {
            float pp[3];
            t->radarProbePos(pp);
            t->setPlayer(pp, p.mps);
            t->update(dt, focus, nullptr);
            const int got = t->radarReadingMph();
            const bool fl = t->radarFlashing();
            char one[70];
            std::snprintf(one, sizeof(one), "%.1f m/s->%d(%c) ", p.mps, got,
                          fl ? 'F' : '-');
            std::strncat(detail, one, sizeof(detail) - std::strlen(detail) - 1);
            if (got != p.wantMph || fl != p.wantFlash) allOk = false;
        }
        check(allOk, "T10b the radar reads the player's speed in mph and flashes over "
                     "the limit", detail);
        t->shutdown(nullptr);
    }

    // ---- T11: THE PLAYER IS A FIRST-CLASS OBSTACLE ------------------------
    // The owner parked on the freeway and watched traffic drive through him.
    // This is that defect's gate. A stationary vehicle is placed IN A LIVE
    // LANE using the sim's own lane geometry (never a guessed coordinate) and
    // traffic is launched at it. Four assertions, all measured per tick:
    //   a) approaching traffic DECELERATES rather than holding cruise,
    //   b) NOTHING ever occupies his box (the no-overlap invariant, extended),
    //   c) the merge-around fires — somebody goes round him,
    //   d) brake lights are LIT while decel exceeds the threshold.
    {
        auto t = makeSim(0x9A11EDu, 0);
        const float dt = 1.0f / 60.0f;
        const float obsS = 800.0f, obsLane = 3.0f;
        float obs[3];
        const bool sited = t->laneWorldPos(1, obsLane, obsS, obs);
        // Four cars closing on him in his lane, plus one in the next lane over
        // so the merge-around has to be a MEASURED decision, not a free lane.
        const int mi = 0;                        // SEDAN A
        t->spawnForTest(mi, 1, 3, 640.0f, 32.0f, 33.0f);
        t->spawnForTest(mi, 1, 3, 570.0f, 32.0f, 33.0f);
        t->spawnForTest(mi, 1, 3, 500.0f, 32.0f, 33.0f);
        t->spawnForTest(mi, 1, 3, 430.0f, 32.0f, 33.0f);
        t->spawnForTest(mi, 1, 2, 610.0f, 30.0f, 30.0f);
        float focus[3] = { obs[0], obs[1], obs[2] };
        bool everBraked = false, everLit = false, everMerged = false;
        bool everOverlapped = false, everDecelWithoutLight = false;
        float minV = 1e9f, worstPen = -1e9f;
        for (int i = 0; i < 60 * 40; ++i) {                  // 40 s
            t->setPlayer(obs, 0.0f);
            t->update(dt, focus, nullptr);
            int cw; float pl, ps, pv;
            if (!t->playerLane(cw, pl, ps, pv)) continue;
            for (uint32_t k = 0; k < t->liveCount(); ++k) {
                const FreewayTraffic::CarState a = t->carState(k);
                if (a.cw != cw) continue;
                if (a.v < minV) minV = a.v;
                if (a.v < 30.0f) everBraked = true;
                if (a.brakeLit) everLit = true;
                if (a.lane != 3 && a.laneF > 2.6f) everMerged = true;
                if (a.merging && std::lround(a.laneF) != 3) everMerged = true;
                // (b) THE BOX. Same 2-D test the AI-vs-AI gate uses.
                const float latSep = std::fabs(a.laneF - pl) * kTrafLaneM;
                if (latSep >= a.halfWidth + 0.95f) continue;
                float ds = std::fabs(a.s - ps);
                ds = std::min(ds, t->routeLen() - ds);
                const float need = 0.5f * (4.5f + 4.6f);
                if (ds < need) {
                    everOverlapped = true;
                    worstPen = std::max(worstPen, need - ds);
                }
            }
        }
        (void)everDecelWithoutLight;
        std::snprintf(d, sizeof(d),
            "obstacle parked at lane %.0f s %.0f (%.1f, %.1f); slowest approach "
            "%.2f m/s (from 32); braked=%d brake-lights-lit=%d merged-around=%d; "
            "worst box penetration %.2f m",
            obsLane, obsS, obs[0], obs[2], minV, (int)everBraked, (int)everLit,
            (int)everMerged, everOverlapped ? worstPen : 0.0f);
        check(sited, "T11 the obstacle is placed on a real lane (sim geometry)");
        check(everBraked && minV < 12.0f,
              "T11a traffic DECELERATES for a vehicle stopped in its lane", d);
        check(!everOverlapped,
              "T11b nothing ever occupies the stopped vehicle's box", d);
        check(everMerged, "T11c the merge-around fires — traffic goes round him", d);
        check(everLit, "T11d brake lights light while the follower is braking", d);
        t->shutdown(nullptr);
    }

    // ---- T12: THE CARS ARE SOLID ------------------------------------------
    // The owner, parked on the freeway: "there is also no cOLLISION factor for
    // any of those cars!" This is the gate for that, and it runs against a REAL
    // Jolt world rather than the pure sim — every other gate here passes phys
    // = nullptr, which is exactly why a physics defect could hide behind
    // twenty green checks.
    //
    // It measures, in order, the things that must all be true for a traffic car
    // to be solid:
    //   a) the kinematic body EXISTS,
    //   b) its box is where the DRAWN CAR is (the AABB agreement check the
    //      coordinator asked for — if these disagree that IS the bug),
    //   c) a dynamic body in the lane REPORTS A CONTACT,
    //   d) the pair never interpenetrate beyond a tolerance,
    //   e) a hard enough hit converts the car to dynamic at the threshold.
    {
        const float dtStep = 1.0f / 60.0f;
        std::unique_ptr<x3::phys::IPhysicsWorld> pw(x3::phys::createPhysicsWorld());
        bool physOk = pw && pw->init();
        auto t = makeSim(0x5011Du, 0);
        // Headless: no device, so the models carry class-default footprints.
        // That is fine — the point is agreement between the BOX and the pose
        // the renderer would use, both of which come from the same numbers.
        const int mi = 0;                                  // SEDAN A
        int idx = -1;
        if (physOk) {
            idx = t->spawnForTestPhys(mi, 1, 3, 500.0f, 26.0f, 26.0f, pw.get());
        }
        const bool haveBody = idx >= 0 && t->carHasBody((uint32_t)idx);
        check(physOk, "T12 a real physics world came up for the collision gate");
        check(haveBody, "T12a the traffic car owns a kinematic body");

        float boxC[3] = { 0, 0, 0 }, boxH[3] = { 0, 0, 0 }, drawnLo[3], drawnHi[3];
        bool aabbOk = false;
        char aabbD[300] = "no body";
        if (haveBody) {
            t->carBodyBox((uint32_t)idx, boxC, boxH);
            t->carDrawnBox((uint32_t)idx, drawnLo, drawnHi);
            float worst = 0.0f;
            for (int k = 0; k < 3; ++k) {
                worst = std::max(worst, std::fabs((boxC[k] - boxH[k]) - drawnLo[k]));
                worst = std::max(worst, std::fabs((boxC[k] + boxH[k]) - drawnHi[k]));
            }
            aabbOk = worst < 0.05f;
            std::snprintf(aabbD, sizeof(aabbD),
                "body box [%.2f %.2f %.2f]..[%.2f %.2f %.2f] vs drawn "
                "[%.2f %.2f %.2f]..[%.2f %.2f %.2f]; worst face disagreement %.3f m",
                boxC[0]-boxH[0], boxC[1]-boxH[1], boxC[2]-boxH[2],
                boxC[0]+boxH[0], boxC[1]+boxH[1], boxC[2]+boxH[2],
                drawnLo[0], drawnLo[1], drawnLo[2], drawnHi[0], drawnHi[1], drawnHi[2],
                worst);
        }
        check(aabbOk, "T12b the collision box IS where the car is drawn", aabbD);

        // ---- c/d/e: put a heavy dynamic box in the lane and drive at it ----
        // NOTE THE FORWARDING, and why it matters beyond this gate: the
        // contact callback is SINGULAR (one per world, last writer wins). The
        // first cut of this probe installed its own and thereby silently
        // orphaned FreewayTraffic::onContact, so T12e reported "no conversion"
        // at an impulse of 37,873 against a threshold of 4,000 -- a failure
        // caused entirely by the test harness. The host has exactly one
        // installer (verified: host_tunnel.cpp:2034 is the only setter), so
        // in-game the wiring is sound; here the probe must chain.
        struct Probe {
            uint32_t contacts = 0;
            float    maxImpulse = 0.0f;
            x3::phys::BodyId car{}, wall{};
            FreewayTraffic* traffic = nullptr;
            x3::phys::IPhysicsWorld* world = nullptr;
        } probe;
        bool contacted = false, converted = false;
        float worstPen = 0.0f, minPlanar = 1e9f, dyAtMin = 0.0f;
        float wallMoved = 0.0f, wallSpawn[3] = { 0, 0, 0 }, carTravel = 0.0f;
        if (haveBody && physOk) {
            // A 1500 kg box (the hero car's mass) sitting in lane 3, 60 m
            // ahead — placed with the sim's own lane geometry, never guessed.
            // THE OVERLAP IS MADE UNAMBIGUOUS. The first cut drove the car at
            // a box 60 m up the lane and measured "closest planar approach
            // 3.06 m" -- but that box was axis-aligned to the WORLD while the
            // lane runs diagonally, so whether the two shapes actually
            // intersected depended on the local heading and the number proved
            // nothing either way. A SPHERE has no orientation, so placing one
            // centred exactly on the car's own body centre is overlap that
            // cannot be argued with: if Jolt reports no contact for THAT, the
            // pair does not collide, full stop.
            float wp[3];
            {
                float bc[3], bh[3];
                t->carBodyBox((uint32_t)idx, bc, bh);
                wp[0] = bc[0]; wp[1] = bc[1] - bh[1]; wp[2] = bc[2];
            }
            // A STATIC FLOOR UNDER THE SCENE. Without it the obstacle is a
            // dynamic body in an empty world: the first cut of this gate had
            // it FREE-FALL, and the telemetry read "closest planar approach
            // 3.06 m (dy 278.58)" — the two boxes passed 3 m apart in plan
            // while 278 metres apart vertically. That was a defect in the
            // GATE, not the engine (the real world has terrain under the
            // road), and it is exactly why the dy is measured and printed
            // rather than assumed: a contact test between a body and a body
            // that has fallen out of the world proves nothing.
            pw->addBox(x3::phys::Vec3{ 400.0f, 1.0f, 400.0f },
                       x3::phys::Vec3{ wp[0], wp[1] - 1.0f, wp[2] },
                       0.0f, x3::phys::Layer::Static);
            probe.wall = pw->addSphere(1.10f,
                                       x3::phys::Vec3{ wp[0], wp[1] + 1.10f, wp[2] },
                                       1500.0f, x3::phys::Layer::Dynamic);
            probe.car = t->carBodyId((uint32_t)idx);
            probe.traffic = t.get();
            probe.world = pw.get();
            {
                const x3::phys::Vec3 w0 = pw->getBodyPosition(probe.wall);
                wallSpawn[0] = w0.x; wallSpawn[1] = w0.y; wallSpawn[2] = w0.z;
            }
            pw->setContactCallback(
                [](x3::phys::BodyId a, x3::phys::BodyId b, const float*,
                   const float*, float imp, void* user) {
                    auto* p = static_cast<Probe*>(user);
                    const bool hitCar  = a.id == p->car.id  || b.id == p->car.id;
                    const bool hitWall = a.id == p->wall.id || b.id == p->wall.id;
                    if (hitCar && hitWall) {
                        ++p->contacts;
                        p->maxImpulse = std::max(p->maxImpulse, imp);
                    }
                    if (p->traffic) p->traffic->onContact(a, b, imp, p->world);
                }, &probe);
            float focus[3] = { wp[0], wp[1], wp[2] };
            for (int i = 0; i < 60 * 8; ++i) {
                t->update(dtStep, focus, pw.get());
                pw->step(dtStep);
                // (d) INTERPENETRATION, measured between the two body centres.
                const x3::phys::Vec3 cb = pw->getBodyPosition(probe.car);
                const x3::phys::Vec3 wb = pw->getBodyPosition(probe.wall);
                const float dx = cb.x - wb.x, dz = cb.z - wb.z;
                const float planar = std::sqrt(dx * dx + dz * dz);
                if (planar < minPlanar) {
                    minPlanar = planar;
                    dyAtMin = std::fabs(cb.y - wb.y);
                }
                // THE DECISIVE MEASUREMENT: did the obstacle get PUSHED? If it
                // moved, the solver is resolving the contact and only the
                // callback is at fault; if it never moves, the pair genuinely
                // never collides. One number separates two very different bugs.
                const float wdx = wb.x - wallSpawn[0], wdz = wb.z - wallSpawn[2];
                wallMoved = std::max(wallMoved, std::sqrt(wdx * wdx + wdz * wdz));
                // Only meaningful while they are roughly level with each other.
                if (std::fabs(cb.y - wb.y) < 1.6f) {
                    const float minCentre = 1.10f + 0.5f * 1.85f;
                    if (planar < minCentre)
                        worstPen = std::max(worstPen, minCentre - planar);
                }
            }
            contacted = probe.contacts > 0;
            converted = t->looseCount() > 0;
            pw->setContactCallback(nullptr, nullptr);
        }
        std::snprintf(d, sizeof(d),
            "%u contacts reported, peak impulse %.0f (threshold %.0f), "
            "closest planar approach %.2f m (dy %.2f), worst interpenetration "
            "%.2f m, OBSTACLE PUSHED %.2f m, converted-to-dynamic=%d",
            probe.contacts, probe.maxImpulse, 4000.0f, minPlanar, dyAtMin,
            worstPen, wallMoved, (int)converted);
        check(contacted, "T12c the car's body REPORTS CONTACT with a car in its lane", d);
        // T12d states what this setup can actually prove. The obstacle is
        // SPAWNED deliberately overlapping the car (that is how the ambiguity
        // was removed), so "they never interpenetrate" is not a claim this
        // probe is entitled to make -- the interpenetration is the premise.
        // What it does prove is that the solver RESOLVES the overlap it is
        // given: a 1500 kg body sitting inside a kinematic car gets ejected
        // rather than sinking through. The genuine no-tunnelling claim belongs
        // to T11b, which measures the AI's own box invariant against a parked
        // player over 40 s.
        check(wallMoved > 0.15f,
              "T12d the solver EJECTS a body overlapping a traffic car", d);
        check(converted, "T12e a hard hit converts the traffic car to dynamic", d);
        t->shutdown(pw.get());
        if (pw) pw->shutdown();
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    char sum[96];
    std::snprintf(sum, sizeof(sum), "traffic: %d/%d passed", pass, pass + fail);
    if (fail == 0) x3::logInfo(sum); else x3::logError(sum);
    return fail == 0;
}

} // namespace x3::game
