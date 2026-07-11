// descent_slide.cpp — see header. The track is authored as a TURTLE SCRIPT of
// parametric pieces (pitched straights / pitch ramps / helices) sampled every
// ~2.5 m; a MAKEUP SPIRAL before the SL approach absorbs authoring drift so the
// ride always lands the SL window bands and the −178 m bowl exactly (the gate
// asserts it, arithmetic never has to).
#include "descent_slide.h"
#include "asset_root.h"
#include "mesh_prims.h"
#include "engine/core/x3_log.h"

// stb_image: file-local static copy (the cinematic.cpp recipe — the engine's
// implementation is file-local in ModelLoader.cpp; each app TU that decodes
// PNGs instantiates its own).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

// Surface sets (ONE-LINE SWAPPABLE: sl_chute_steel / cv_rock_flume land from
// the Wave-2A forge; until then the concrete sets carry the read).
constexpr const char* kChuteSet = "sr_concrete_01";   // upper chute (facility)
constexpr const char* kRockSet  = "sr_concrete_a";    // lower chute + cavern (rock)

// Rider tuning (shared by the live host + the headless gate sim).
constexpr float kGAlong    = 13.0f;   // effective along-track gravity (arcade)
constexpr float kDrag      = 0.11f;   // speed drag (1/s)
constexpr float kCrawl     = 2.2f;    // lift-crest crawl speed (m/s)
constexpr float kMaxSpeed  = 26.0f;   // coaster-grade cap
constexpr float kBrakeDecl = 9.0f;    // brake-run decel (m/s^2)
constexpr float kSteerMax  = 1.35f;   // lateral clamp (m)
constexpr float kSampleStep= 2.5f;    // spline sampling pitch (m)

// SL glimpse-window depth bands (canon: spire_sublevels kSubBaseY; SL3's window
// sits just above the bowl so the flash lands before the cavern burst).
constexpr float kWinSL1 = -170.0f, kWinSL2 = -174.0f, kWinSL3 = -177.0f;
constexpr float kBowlY  = -178.0f;   // canon cave horizon (spire_sublevels kCaveBaseY)

struct RawSample {
    x3::phys::Vec3 pos; float bank; TrackSegType type;
};

// ---- Turtle-script track generator ----------------------------------------
struct Turtle {
    std::vector<RawSample> out;
    x3::phys::Vec3 p{ 0, 0, 0 };
    float heading = 0.0f;              // yaw (rad); 0 = +X per engine convention
    void emit(float bank, TrackSegType t) { out.push_back({ p, bank, t }); }

    // Straight run at a fixed pitch (deg, negative = down), heading unchanged.
    void straightPitched(float len, float pitchDeg, float bank, TrackSegType t) {
        const float steps = std::max(1.0f, std::round(len / kSampleStep));
        const float ds = len / steps;
        const float cp = std::cos(pitchDeg * kDeg), sp = std::sin(pitchDeg * kDeg);
        for (int i = 0; i < (int)steps; ++i) {
            p.x += std::cos(heading) * cp * ds;
            p.z += std::sin(heading) * cp * ds;
            p.y += sp * ds;
            emit(bank, t);
        }
    }
    // Straight run whose pitch ramps p0 -> p1 (the drop entries/pullouts).
    void pitchRamp(float len, float p0, float p1, float bank, TrackSegType t) {
        const float steps = std::max(1.0f, std::round(len / kSampleStep));
        const float ds = len / steps;
        for (int i = 0; i < (int)steps; ++i) {
            const float f = (i + 0.5f) / steps;
            const float pd = p0 + (p1 - p0) * f;
            p.x += std::cos(heading) * std::cos(pd * kDeg) * ds;
            p.z += std::sin(heading) * std::cos(pd * kDeg) * ds;
            p.y += std::sin(pd * kDeg) * ds;
            emit(bank, t);
        }
    }
    // Helical arc: revs revolutions of radius r, descending `drop` meters total,
    // banked `bank` deg (sign = handedness), ccw = counter-clockwise (yaw +).
    void helix(float revs, float r, float drop, float bank, bool ccw, TrackSegType t) {
        const float arc = 2.0f * kPi * r * revs;                       // horizontal arc length
        const float steps = std::max(2.0f, std::round(arc / kSampleStep));
        const float dAng = (2.0f * kPi * revs / steps) * (ccw ? 1.0f : -1.0f);
        const float dy = drop / steps;
        // Turn center sits 90° off the heading toward the turn side.
        for (int i = 0; i < (int)steps; ++i) {
            heading += dAng * 0.5f;
            const float ds = arc / steps;
            p.x += std::cos(heading) * ds;
            p.z += std::sin(heading) * ds;
            p.y -= dy;
            heading += dAng * 0.5f;
            emit(ccw ? bank : -bank, t);
        }
    }
};

x3::phys::Vec3 vnorm(x3::phys::Vec3 v) {
    const float l = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-6f) return { 1, 0, 0 };
    return { v.x/l, v.y/l, v.z/l };
}
x3::phys::Vec3 vcross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
// Rodrigues rotation of v around unit axis k by angle a.
x3::phys::Vec3 vrot(const x3::phys::Vec3& v, const x3::phys::Vec3& k, float a) {
    const float c = std::cos(a), s = std::sin(a);
    const x3::phys::Vec3 kv = vcross(k, v);
    const float kd = k.x*v.x + k.y*v.y + k.z*v.z;
    return { v.x*c + kv.x*s + k.x*kd*(1-c),
             v.y*c + kv.y*s + k.y*kd*(1-c),
             v.z*c + kv.z*s + k.z*kd*(1-c) };
}

} // namespace

// ---------------------------------------------------------------------------
// Track spec (pure data — the gate's entry point)
// ---------------------------------------------------------------------------
TrackSpec DescentSlide::buildTrackSpec() {
    Turtle t;

    // ---- ACT 1: the lift-crest + FIRST DROP (Millennium-Force grammar) ----
    t.straightPitched(10.0f, -3.0f, 0.0f, TrackSegType::Crest);      // slow crawl, depth yawns
    t.pitchRamp( 6.0f,  -3.0f, -78.0f, 0.0f, TrackSegType::Drop);    // commit
    t.straightPitched(30.0f, -78.0f, 0.0f, TrackSegType::Drop);      // the plunge (~29 m down)
    t.pitchRamp(10.0f, -78.0f, -12.0f, 8.0f, TrackSegType::Drop);    // pullout
    // ---- Overbanked turn #1 (>90°: geometry+physics banked; camera roll is a
    //      flagged renderer follow-up — setCamera has no view-up axis). ----
    t.helix(0.55f, 11.0f, 8.0f, 96.0f, true, TrackSegType::Curve);
    // ---- Airtime hill #1 (floor falls away; rider unweights) ----
    t.pitchRamp(5.0f, -12.0f, -42.0f, 0.0f, TrackSegType::Airtime);
    t.pitchRamp(7.0f, -42.0f,  -8.0f, 0.0f, TrackSegType::Airtime);
    // ---- Tight BORE (roof + headchopper beams) then BURST into the geode void ----
    t.helix(0.40f, 13.0f, 6.0f, 30.0f, false, TrackSegType::Bore);
    t.straightPitched(12.0f, -10.0f, 0.0f, TrackSegType::Bore);
    t.straightPitched(18.0f, -14.0f, 0.0f, TrackSegType::Burst);     // G1 geode stamped here
    // ---- ACT 2: helix down, second drop, airtime #2, the amber throat ----
    t.helix(1.20f, 10.0f, 22.0f, 55.0f, false, TrackSegType::Curve);
    t.pitchRamp(5.0f, -12.0f, -70.0f, 0.0f, TrackSegType::Drop);
    t.straightPitched(14.0f, -70.0f, 0.0f, TrackSegType::Drop);
    t.pitchRamp(8.0f, -70.0f, -10.0f, 6.0f, TrackSegType::Drop);
    t.pitchRamp(5.0f, -10.0f, -38.0f, 0.0f, TrackSegType::Airtime);
    t.pitchRamp(7.0f, -38.0f,  -6.0f, 0.0f, TrackSegType::Airtime);
    t.straightPitched(10.0f, -8.0f, 0.0f, TrackSegType::Bore);       // narrowing throat, G2 amber
    // ---- Overbank #2 (85°) + the long spiral toward the SL band ----
    t.helix(0.50f, 12.0f, 6.0f, 85.0f, true, TrackSegType::Curve);
    t.helix(1.60f, 14.0f, 30.0f, 45.0f, false, TrackSegType::Curve);

    // ---- MAKEUP SPIRAL: absorb authoring drift so the SL approach starts at
    //      exactly −165 m (radius sized to the remaining drop; grade ≤ 35%). ----
    {
        const float need = -165.0f - t.p.y;                  // negative = must descend
        if (need < -1.0f) {
            const float drop = -need;
            const float revs = std::max(0.5f, drop / 12.0f); // ~12 m per rev
            t.helix(revs, 13.0f, drop, 40.0f, true, TrackSegType::Curve);
        }
    }
    // ---- ACT 3: the SL triple-flash bore (−165 → −177.5) + cavern burst finale ----
    t.straightPitched(36.0f, -20.0f, 0.0f, TrackSegType::Bore);      // SL1/SL2/SL3 windows stamp here
    t.helix(0.60f, 13.0f, 0.6f, 25.0f, true, TrackSegType::Burst);   // island turnaround in the dark
    t.straightPitched(14.0f, -1.0f, 0.0f, TrackSegType::Brake);      // brake run into the bowl

    // ---- Frames: tangents + banked lateral frames + arc lengths -------------
    TrackSpec spec;
    const auto& raw = t.out;
    spec.frames.resize(raw.size());
    float cum = 0.0f;
    for (size_t i = 0; i < raw.size(); ++i) {
        TrackFrame& f = spec.frames[i];
        f.pos = raw[i].pos; f.bankDeg = raw[i].bank; f.type = raw[i].type;
        const size_t a = (i == 0) ? 0 : i - 1;
        const size_t b = (i + 1 < raw.size()) ? i + 1 : i;
        f.tan = vnorm({ raw[b].pos.x - raw[a].pos.x,
                        raw[b].pos.y - raw[a].pos.y,
                        raw[b].pos.z - raw[a].pos.z });
        // Flat lateral frame, then bank-rotate around the tangent. Near-vertical
        // tangents fall back to the heading-perpendicular so right never NaNs.
        x3::phys::Vec3 right = vcross(f.tan, { 0, 1, 0 });
        if (right.x*right.x + right.y*right.y + right.z*right.z < 1e-4f)
            right = vcross(f.tan, { 1, 0, 0 });
        right = vnorm(right);
        x3::phys::Vec3 up = vnorm(vcross(right, f.tan));
        if (up.y < 0.0f) { up = { -up.x, -up.y, -up.z }; right = { -right.x, -right.y, -right.z }; }
        const float bankRad = f.bankDeg * kDeg;
        f.right = vrot(right, f.tan, bankRad);
        f.up    = vrot(up,    f.tan, bankRad);
        if (i > 0) {
            const float dx = raw[i].pos.x - raw[i-1].pos.x;
            const float dy = raw[i].pos.y - raw[i-1].pos.y;
            const float dz = raw[i].pos.z - raw[i-1].pos.z;
            cum += std::sqrt(dx*dx + dy*dy + dz*dz);
        }
        f.cumLen = cum;
        if (std::fabs(f.bankDeg) > 90.0f) ++spec.overbankSamples;
    }
    spec.totalLen  = cum;
    spec.totalDrop = raw.front().pos.y - raw.back().pos.y;

    // ---- Feature stamps: airtime zones, headchoppers, glimpse-windows -------
    TrackSegType prev = TrackSegType::Crest;
    float boreRun = 0.0f;
    bool g1 = false, g2 = false, sl1 = false, sl2 = false, sl3 = false;
    for (size_t i = 0; i < spec.frames.size(); ++i) {
        const TrackFrame& f = spec.frames[i];
        if (f.type == TrackSegType::Airtime && prev != TrackSegType::Airtime) ++spec.airtimeZones;
        if (f.type == TrackSegType::Bore) {
            boreRun += kSampleStep;
            if (boreRun >= 6.0f) { boreRun = 0.0f; ++spec.chopperCount; }   // one beam / 6 m
        } else boreRun = 0.0f;
        prev = f.type;

        auto stamp = [&](bool& flag, float r, float g, float b) {
            if (flag) return;
            flag = true;
            TrackSpec::Window w;
            w.pos = { f.pos.x + f.right.x * 2.4f, f.pos.y + f.up.y * 0.8f,
                      f.pos.z + f.right.z * 2.4f };
            w.hue[0] = r; w.hue[1] = g; w.hue[2] = b;
            w.depthY = f.pos.y;
            spec.windows.push_back(w);
        };
        if (f.type == TrackSegType::Burst && !g1 && f.pos.y < -50.0f && f.pos.y > -90.0f)
            stamp(g1, 0.25f, 0.95f, 0.85f);                        // geode teal
        if (f.type == TrackSegType::Bore && !g2 && f.pos.y < -110.0f && f.pos.y > -150.0f)
            stamp(g2, 1.0f, 0.62f, 0.18f);                         // mining amber
        if (!sl1 && f.pos.y <= kWinSL1 + 0.6f) stamp(sl1, 0.95f, 0.95f, 0.90f);  // SL1 steam-white
        if (!sl2 && f.pos.y <= kWinSL2 + 0.6f) stamp(sl2, 0.30f, 0.55f, 1.0f);   // SL2 cryo-blue
        if (!sl3 && f.pos.y <= kWinSL3 + 0.6f) stamp(sl3, 1.0f, 0.22f, 0.15f);   // SL3 red
    }
    return spec;
}

// ---------------------------------------------------------------------------
// Rider
// ---------------------------------------------------------------------------
void TrackRider::tick(const TrackSpec& spec, float dt, float steer,
                      float camOut[3], float& yawOut, float& pitchOut, float& fovOut) {
    if (spec.frames.size() < 2) { done = true; return; }
    // Frame lookup by arc length (frames are near-uniform; index walk is O(1)).
    size_t i = std::min((size_t)(arcLen / kSampleStep), spec.frames.size() - 2);
    while (i + 2 < spec.frames.size() && spec.frames[i + 1].cumLen < arcLen) ++i;
    while (i > 0 && spec.frames[i].cumLen > arcLen) --i;
    const TrackFrame& f0 = spec.frames[i];
    const TrackFrame& f1 = spec.frames[i + 1];
    const float span = std::max(0.01f, f1.cumLen - f0.cumLen);
    const float fr = std::clamp((arcLen - f0.cumLen) / span, 0.0f, 1.0f);
    auto lerp3 = [&](const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
        return x3::phys::Vec3{ a.x + (b.x-a.x)*fr, a.y + (b.y-a.y)*fr, a.z + (b.z-a.z)*fr };
    };
    const x3::phys::Vec3 pos = lerp3(f0.pos, f1.pos);
    const x3::phys::Vec3 tan = vnorm(lerp3(f0.tan, f1.tan));
    const x3::phys::Vec3 rgt = vnorm(lerp3(f0.right, f1.right));
    const x3::phys::Vec3 up  = vnorm(lerp3(f0.up, f1.up));

    // Speed integration per segment behavior.
    if (f0.type == TrackSegType::Crest) {
        speed = std::min(speed + 1.5f * dt, kCrawl);          // lift-crawl tension
    } else {
        float accel = kGAlong * (-tan.y) - kDrag * speed;
        if (f0.type == TrackSegType::Brake) accel -= kBrakeDecl;
        speed = std::clamp(speed + accel * dt, (f0.type == TrackSegType::Brake) ? 0.0f : 2.0f, kMaxSpeed);
    }
    // Steer (recenters when released).
    lateral += steer * 3.5f * dt;
    lateral -= lateral * std::min(1.0f, 0.9f * dt);
    lateral = std::clamp(lateral, -kSteerMax, kSteerMax);
    // Unweight spring (Airtime zones float the rider off the channel).
    const float floatTarget = (f0.type == TrackSegType::Airtime) ? 0.5f : 0.0f;
    floatUp += (floatTarget - floatUp) * std::min(1.0f, 5.0f * dt);

    arcLen += speed * dt;
    if (arcLen >= spec.totalLen - 0.5f || (f0.type == TrackSegType::Brake && speed <= 0.05f))
        done = true;

    const float eye = 1.05f + floatUp;
    camOut[0] = pos.x + up.x * eye + rgt.x * lateral;
    camOut[1] = pos.y + up.y * eye + rgt.y * lateral;
    camOut[2] = pos.z + up.z * eye + rgt.z * lateral;
    yawOut   = std::atan2(tan.z, tan.x);
    pitchOut = std::clamp(std::asin(std::clamp(tan.y, -1.0f, 1.0f)), -1.2f, 1.2f);
    fovOut   = 70.0f + 17.0f * std::clamp(speed / 24.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Geometry build
// ---------------------------------------------------------------------------
namespace {

// Column-major basis transform from a track frame + world offset.
void frameToTransform(const TrackFrame& f, const x3::phys::Vec3& at, float out[16]) {
    out[0]=f.right.x; out[1]=f.right.y; out[2]=f.right.z;  out[3]=0;   // local X = lateral
    out[4]=f.up.x;    out[5]=f.up.y;    out[6]=f.up.z;     out[7]=0;   // local Y = channel up
    out[8]=f.tan.x;   out[9]=f.tan.y;   out[10]=f.tan.z;   out[11]=0;  // local Z = travel
    out[12]=at.x;     out[13]=at.y;     out[14]=at.z;      out[15]=1;
}

} // namespace

bool DescentSlide::build(x3::rhi::IRenderDevice& device, Scene& scene,
                         x3::phys::IPhysicsWorld& physics) {
    m_device = &device;
    m_spec = buildTrackSpec();
    if (m_spec.frames.size() < 8) return false;

    // ---- Textures: surface-library sets (albedo/normal/mr) + 1x1 utilities ----
    auto loadPng = [&](const std::string& path, bool srgb) -> x3::rhi::TextureHandle {
        int w = 0, h = 0, c = 0;
        stbi_uc* px = stbi_load(path.c_str(), &w, &h, &c, 4);
        if (!px) return {};
        x3::rhi::TextureHandle t = device.createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
        stbi_image_free(px);
        if (t.valid()) m_textures.push_back(t);
        return t;
    };
    struct Set { x3::rhi::TextureHandle alb, nrm, mr; };
    auto loadSet = [&](const char* name) -> Set {
        const std::string root = x3::game::assetRoot() + "/surface_library/" + name + "/";
        return { loadPng(root + "albedo.png", true),
                 loadPng(root + "normal.png", false),
                 loadPng(root + "mr.png", false) };
    };
    const Set chute = loadSet(kChuteSet);
    const Set rock  = loadSet(kRockSet);
    auto solid1 = [&](uint8_t r, uint8_t g, uint8_t b, bool srgb) {
        const uint8_t px[4] = { r, g, b, 255 };
        x3::rhi::TextureHandle t = device.createTexture(px, 1, 1, srgb);
        if (t.valid()) m_textures.push_back(t);
        return t;
    };
    const x3::rhi::TextureHandle whiteEmis = solid1(255, 255, 255, true);   // emissiveTex; rgb tint carries hue
    const x3::rhi::TextureHandle satinMR   = solid1(0, 150, 40, false);     // AO/rough/metal — forces PBR route
    const x3::rhi::TextureHandle wetMR     = solid1(0, 80, 40, false);      // cavern wet floor (low rough)

    // Place one oriented box: geometry entity (+ optional emissive) and optional
    // CPU-transformed collision.
    auto place = [&](const TrackFrame& f, const x3::phys::Vec3& at,
                     float hx, float hy, float hz,
                     const Set& set, bool collide,
                     const float* emisTint = nullptr, float emisStrength = 0.0f,
                     float uvScale = 1.0f) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, uvScale);
        x3::rhi::MeshHandle mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                                     m.index.data(), (uint32_t)m.index.size());
        m_meshes.push_back(mesh);
        Entity e;
        e.mesh = mesh;
        e.tex = set.alb; e.normalTex = set.nrm;
        e.mrTex = set.mr.valid() ? set.mr : satinMR;
        if (emisTint) {
            e.emissiveTex = whiteEmis;
            e.emissive[0] = emisTint[0]; e.emissive[1] = emisTint[1];
            e.emissive[2] = emisTint[2]; e.emissive[3] = emisStrength;
        }
        frameToTransform(f, at, e.transform);
        e.tag = (uint32_t)Tag::Static;
        scene.add(e);
        ++m_entities;
        if (collide) {
            // Transform the collision verts by the same basis (CPU-side — the
            // physics API has no oriented-box add; static meshes take world tris).
            std::vector<float> cv = m.cverts;
            for (size_t vi = 0; vi + 2 < cv.size(); vi += 3) {
                const float x = cv[vi], y = cv[vi+1], z = cv[vi+2];
                cv[vi]   = f.right.x*x + f.up.x*y + f.tan.x*z + at.x;
                cv[vi+1] = f.right.y*x + f.up.y*y + f.tan.y*z + at.y;
                cv[vi+2] = f.right.z*x + f.up.z*y + f.tan.z*z + at.z;
            }
            m_bodies.push_back(physics.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                                                     m.cindex.data(), (uint32_t)m.cindex.size()));
        }
    };

    // ---- Channel sweep: one U-section per pair of samples --------------------
    const float halfW = 1.6f;               // 3.2 m bore
    float ribbonAccum = 0.0f, beamAccum = 0.0f, trestleAccum = 0.0f;
    uint32_t shoulderLights = 0;
    for (size_t i = 0; i + 1 < m_spec.frames.size(); i += 2) {
        const TrackFrame& f = m_spec.frames[i];
        const size_t j = std::min(i + 2, m_spec.frames.size() - 1);
        const float segLen = std::max(1.0f, m_spec.frames[j].cumLen - f.cumLen);
        const float hz = segLen * 0.5f + 0.15f;                     // slight overlap: no gaps
        const x3::phys::Vec3 mid = {
            (f.pos.x + m_spec.frames[j].pos.x) * 0.5f,
            (f.pos.y + m_spec.frames[j].pos.y) * 0.5f,
            (f.pos.z + m_spec.frames[j].pos.z) * 0.5f };
        const bool lower = (mid.y < -90.0f);
        const Set& set = lower ? rock : chute;
        const bool burst = (f.type == TrackSegType::Burst);
        const bool bore  = (f.type == TrackSegType::Bore);
        const bool brake = (f.type == TrackSegType::Brake);
        const float w = brake ? halfW * 1.8f : halfW;               // bowl widens

        // Floor plate (always; collision).
        place(f, { mid.x - f.up.x*0.10f, mid.y - f.up.y*0.10f, mid.z - f.up.z*0.10f },
              w, 0.10f, hz, set, true);
        if (!burst) {
            // Side walls (collision).
            for (int s = -1; s <= 1; s += 2) {
                const x3::phys::Vec3 at = { mid.x + f.right.x * w * (float)s,
                                            mid.y + f.right.y * w * (float)s + f.up.y * 0.8f,
                                            mid.z + f.right.z * w * (float)s };
                place(f, at, 0.12f, 0.9f, hz, set, true);
            }
        } else {
            // BURST: open void — trestle columns instead of walls (render-only).
            trestleAccum += segLen;
            if (trestleAccum >= 7.0f) {
                trestleAccum = 0.0f;
                place(f, { mid.x, mid.y - 2.2f, mid.z }, 0.14f, 2.1f, 0.14f, set, false);
            }
        }
        if (bore) {
            // Roof (render-only) + HEADCHOPPER beams every ~6 m at 1.55 m clearance.
            place(f, { mid.x + f.up.x*1.9f, mid.y + f.up.y*1.9f, mid.z + f.up.z*1.9f },
                  w + 0.15f, 0.10f, hz, set, false);
            beamAccum += segLen;
            if (beamAccum >= 6.0f) {
                beamAccum = 0.0f;
                place(f, { mid.x + f.up.x*1.55f, mid.y + f.up.y*1.55f, mid.z + f.up.z*1.55f },
                      w + 0.10f, 0.09f, 0.12f, set, false);
            }
        }
        // Emissive guide ribbons on both shoulders (cyan -> teal with depth).
        const float depthFrac = std::clamp(-mid.y / 178.0f, 0.0f, 1.0f);
        const float rib[3] = { 0.20f + 0.05f * depthFrac,
                               0.75f - 0.10f * depthFrac,
                               1.00f - 0.25f * depthFrac };
        for (int s = -1; s <= 1; s += 2) {
            const x3::phys::Vec3 at = { mid.x + f.right.x * (w - 0.10f) * (float)s,
                                        mid.y + f.right.y * (w - 0.10f) * (float)s + f.up.y * 0.16f,
                                        mid.z + f.right.z * (w - 0.10f) * (float)s };
            place(f, at, 0.06f, 0.045f, hz, { {}, {}, satinMR }, false, rib, 1.1f);
        }
        // Spaced shoulder point lights (budget: ~1 per 30 m).
        ribbonAccum += segLen;
        if (ribbonAccum >= 30.0f && shoulderLights < 18) {
            ribbonAccum = 0.0f; ++shoulderLights;
            x3::rhi::PointLight pl{};
            pl.pos[0] = mid.x + f.up.x*1.2f; pl.pos[1] = mid.y + f.up.y*1.2f; pl.pos[2] = mid.z + f.up.z*1.2f;
            pl.range = 11.0f;
            pl.color[0] = rib[0]*2.4f; pl.color[1] = rib[1]*2.4f; pl.color[2] = rib[2]*2.4f;
            m_lights.push_back(pl);
        }
    }

    // ---- Glimpse-window alcoves (frame + hued emissive panel + light) --------
    for (const auto& wdw : m_spec.windows) {
        // Nearest frame for orientation.
        size_t best = 0; float bd = 1e12f;
        for (size_t i = 0; i < m_spec.frames.size(); i += 4) {
            const auto& fp = m_spec.frames[i].pos;
            const float d = (fp.x-wdw.pos.x)*(fp.x-wdw.pos.x) + (fp.y-wdw.pos.y)*(fp.y-wdw.pos.y)
                          + (fp.z-wdw.pos.z)*(fp.z-wdw.pos.z);
            if (d < bd) { bd = d; best = i; }
        }
        const TrackFrame& f = m_spec.frames[best];
        place(f, wdw.pos, 1.3f, 1.0f, 0.35f, rock, false);                         // alcove shell
        const x3::phys::Vec3 pn = { wdw.pos.x - f.right.x*0.30f, wdw.pos.y, wdw.pos.z - f.right.z*0.30f };
        place(f, pn, 1.0f, 0.75f, 0.06f, { {}, {}, satinMR }, false, wdw.hue, 1.25f);  // the flash panel
        x3::rhi::PointLight pl{};
        pl.pos[0] = wdw.pos.x - f.right.x; pl.pos[1] = wdw.pos.y + 0.4f; pl.pos[2] = wdw.pos.z - f.right.z;
        pl.range = 9.0f;
        pl.color[0] = wdw.hue[0]*4.5f; pl.color[1] = wdw.hue[1]*4.5f; pl.color[2] = wdw.hue[2]*4.5f;
        m_lights.push_back(pl);
    }

    // ---- Mouth platform + the crystal cavern + bowl ---------------------------
    const TrackFrame& fm = m_spec.frames.front();
    const TrackFrame& fe = m_spec.frames.back();
    place(fm, { fm.pos.x - fm.tan.x*3.0f, fm.pos.y - 0.15f, fm.pos.z - fm.tan.z*3.0f },
          3.2f, 0.12f, 3.2f, chute, true);                                          // mouth platform
    { x3::rhi::PointLight pl{}; pl.pos[0]=fm.pos.x; pl.pos[1]=fm.pos.y+2.4f; pl.pos[2]=fm.pos.z;
      pl.range = 10.0f; pl.color[0]=3.4f; pl.color[1]=3.0f; pl.color[2]=2.4f; m_lights.push_back(pl); }

    // Cavern: floor slab + partial rock walls + 7 singing crystals (canon count),
    // wet low-roughness floor so crystal light streaks. Identity frame (flat).
    TrackFrame flat{};
    flat.tan = { 1,0,0 }; flat.right = { 0,0,1 }; flat.up = { 0,1,0 };
    const x3::phys::Vec3 cc = { fe.pos.x + fe.tan.x*10.0f, kBowlY - 0.12f, fe.pos.z + fe.tan.z*10.0f };
    {
        Set wetRock = rock; wetRock.mr = wetMR;
        // Cave-scale UV (0.22: one tile ~= 4.5 m) so the cavern reads as carved
        // rock mass, not paneled room — the chute keeps its 1.0 panel tiling.
        place(flat, cc, 17.0f, 0.12f, 15.0f, wetRock, true, nullptr, 0.0f, 0.22f);   // cavern floor
        place(flat, { cc.x, cc.y + 13.5f, cc.z }, 17.0f, 0.12f, 15.0f, rock, false, nullptr, 0.0f, 0.22f);
        for (int s = -1; s <= 1; s += 2) {
            place(flat, { cc.x, cc.y + 6.5f, cc.z + 15.0f*(float)s }, 17.0f, 6.5f, 0.4f, rock, true, nullptr, 0.0f, 0.22f);
            place(flat, { cc.x + 17.0f*(float)s, cc.y + 6.5f, cc.z }, 0.4f, 6.5f, 15.0f, rock, true, nullptr, 0.0f, 0.22f);
        }
        const float crys[3] = { 0.30f, 0.95f, 0.85f };
        for (int i = 0; i < 7; ++i) {                                              // kSalvariCrystalCount
            const float a = (float)i * 0.9f + 0.4f;
            const float r = 5.5f + 3.5f * (float)((i * 37) % 5) / 4.0f;
            TrackFrame lean = flat;
            lean.up    = vnorm({ 0.12f * std::cos(a), 1.0f, 0.12f * std::sin(a) });
            lean.right = vnorm(vcross(lean.up, lean.tan));
            const float h = 2.2f + 1.4f * (float)((i * 53) % 4) / 3.0f;   // 2.2-3.6 m: they OWN the room
            place(lean, { cc.x + std::cos(a)*r, cc.y + h, cc.z + std::sin(a)*r },
                  0.38f, h, 0.38f, { {}, {}, satinMR }, false, crys, 1.15f);
            if (i % 2 == 0) {
                x3::rhi::PointLight pl{};
                pl.pos[0] = cc.x + std::cos(a)*r; pl.pos[1] = cc.y + h + 0.6f; pl.pos[2] = cc.z + std::sin(a)*r;
                pl.range = 9.0f; pl.color[0] = 0.7f; pl.color[1] = 2.6f; pl.color[2] = 2.3f;
                m_lights.push_back(pl);
            }
        }
        // CLUB BASS-BLEED HOOK: Club 1127 sits BELOW this floor (canon 2026-07-11).
        // A quiet low-passed 128 BPM loop positioned at (cc.x, kBowlY-2, cc.z) via
        // IAudioSystem::playMusic can't be positional (music is the single global
        // bed) — the correct wiring is a looping playSound3D once the audio API
        // grows loopable 3D one-shots. Marked hook; host logs it so the beat is
        // never silently forgotten.
    }

    x3::logInfo("[descentslide] track " + std::to_string((int)m_spec.totalLen) + " m, drop " +
                std::to_string((int)m_spec.totalDrop) + " m, " +
                std::to_string(m_entities) + " entities, " +
                std::to_string((int)m_spec.windows.size()) + " windows, " +
                std::to_string(m_spec.chopperCount) + " choppers, " +
                std::to_string((int)m_lights.size()) + " lights");
    return true;
}

x3::phys::Vec3 DescentSlide::mouth() const {
    if (m_spec.frames.empty()) return {};
    const TrackFrame& f = m_spec.frames.front();
    return { f.pos.x - f.tan.x * 3.0f, f.pos.y + 0.1f, f.pos.z - f.tan.z * 3.0f };
}
x3::phys::Vec3 DescentSlide::bowl() const {
    if (m_spec.frames.empty()) return {};
    const TrackFrame& f = m_spec.frames.back();
    return { f.pos.x + f.tan.x * 6.0f, f.pos.y + 0.4f, f.pos.z + f.tan.z * 6.0f };
}

void DescentSlide::shutdown(x3::phys::IPhysicsWorld& physics) {
    for (auto b : m_bodies) physics.removeBody(b);
    m_bodies.clear();
    if (m_device) {
        for (auto m : m_meshes) if (m.valid()) m_device->destroyMesh(m);
        for (auto t : m_textures) if (t.valid()) m_device->destroyTexture(t);
    }
    m_meshes.clear(); m_textures.clear(); m_lights.clear();
    m_entities = 0;
}

// ---------------------------------------------------------------------------
// Self-test (--test-descentslide, headless — pure spec + rider sim)
// ---------------------------------------------------------------------------
bool runDescentSlideSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        std::printf("  %s %s\n", c ? "PASS" : "FAIL", name);
        if (c) ++pass;
    };

    const TrackSpec spec = DescentSlide::buildTrackSpec();

    // T1: scale — a real ride, a real drop.
    check(spec.frames.size() >= 100 && spec.totalDrop >= 170.0f && spec.totalLen >= 300.0f,
          "T1 track scale (>=100 frames, >=170 m drop, >=300 m length)");

    // T2: monotonic descent until the brake run (the bowl may flatten, never climb >0.2).
    bool mono = true;
    for (size_t i = 1; i < spec.frames.size(); ++i) {
        if (spec.frames[i].type == TrackSegType::Brake) break;
        if (spec.frames[i].pos.y > spec.frames[i-1].pos.y + 0.2f) { mono = false; break; }
    }
    check(mono, "T2 monotonic descent to the brake run");

    // T3: winding — the heading rate changes sign (direction reversals >= 2).
    int flips = 0; float prevRate = 0.0f;
    for (size_t i = 2; i < spec.frames.size(); ++i) {
        const float y1 = std::atan2(spec.frames[i-1].tan.z, spec.frames[i-1].tan.x);
        const float y2 = std::atan2(spec.frames[i].tan.z, spec.frames[i].tan.x);
        float d = y2 - y1;
        while (d >  kPi) d -= 2*kPi;
        while (d < -kPi) d += 2*kPi;
        if (std::fabs(d) > 0.01f) {
            if (prevRate != 0.0f && (d > 0) != (prevRate > 0)) ++flips;
            prevRate = d;
        }
    }
    check(flips >= 2, "T3 winding (>=2 turn-direction reversals)");

    // T4: coaster grammar — first drop steepness, overbank, airtime, choppers.
    float steepest = 0.0f;
    for (const auto& f : spec.frames)
        if (f.type == TrackSegType::Drop) steepest = std::min(steepest, f.tan.y);
    check(steepest <= std::sin(-70.0f * kDeg), "T4a first drop reaches <= -70 deg");
    check(spec.overbankSamples > 0, "T4b >=1 overbanked samples (>90 deg bank)");
    check(spec.airtimeZones >= 2, "T4c >=2 airtime zones");
    check(spec.chopperCount >= 2, "T4d >=2 headchopper beams");

    // T5: windows — 5 total, the SL trio in canon bands.
    int inBand = 0;
    for (const auto& w : spec.windows)
        if (w.depthY <= -168.0f && w.depthY >= -178.5f) ++inBand;
    check(spec.windows.size() == 5 && inBand == 3,
          "T5 five glimpse-windows, SL trio in the -170/-174/-177 bands");

    // T6: anchors — mouth at B1 (y=0) and end at the cave horizon.
    check(std::fabs(spec.frames.front().pos.y) <= 1.0f &&
          std::fabs(spec.frames.back().pos.y - (-178.0f)) <= 1.5f,
          "T6 mouth at B1 y=0, bowl at the -178 m horizon");

    // T7: full rider sim reaches the bowl in a coaster-plausible time with real
    // unweight observed; the crest actually crawls (tension exists).
    {
        TrackRider r;
        float cam[3], yaw, pitch, fov;
        float t = 0.0f, maxFloat = 0.0f, crestTime = 0.0f;
        bool ok = false;
        const float dt = 1.0f / 60.0f;
        for (int step = 0; step < 60 * 120; ++step) {
            const size_t idx = std::min((size_t)(r.arcLen / kSampleStep), spec.frames.size() - 1);
            if (spec.frames[idx].type == TrackSegType::Crest) crestTime += dt;
            r.tick(spec, dt, (step % 240 < 120) ? 0.6f : -0.6f, cam, yaw, pitch, fov);
            maxFloat = std::max(maxFloat, r.floatUp);
            t += dt;
            if (r.done) { ok = true; break; }
        }
        check(ok && t >= 15.0f && t <= 90.0f, "T7a ride completes in 15-90 s");
        check(maxFloat >= 0.25f, "T7b airtime unweight observed (float >= 0.25 m)");
        check(crestTime >= 2.0f, "T7c lift-crest tension (>= 2 s crawl)");
        std::printf("  [info] ride duration %.1f s, peak float %.2f m\n", t, maxFloat);
    }

    // T8: steer clamps.
    {
        TrackRider r; r.arcLen = spec.totalLen * 0.4f; r.speed = 18.0f;
        float cam[3], yaw, pitch, fov;
        for (int i = 0; i < 600; ++i) r.tick(spec, 1.0f/60.0f, 1.0f, cam, yaw, pitch, fov);
        check(r.lateral <= kSteerMax + 0.01f, "T8 lateral steer clamps");
    }

    std::printf("descent-slide: %d/%d passed\n", pass, total);
    return pass == total;
}

} // namespace x3::game
