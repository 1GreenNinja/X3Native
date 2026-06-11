// RT ACOUSTICS brain (see RtAcoustics.h). CLEAN-ROOM original work: standard
// ray-fan occlusion + mean-free-path room sizing (acoustics textbook math),
// no id Tech / RBDOOM / Steam Audio source consulted.

#include "engine/audio/RtAcoustics.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace x3::audio {
namespace {

constexpr float kEmitterMatchRadius = 1.0f;   // slot reuse radius (m)
constexpr float kEmitterExpire      = 3.0f;   // idle seconds before a slot frees
constexpr float kOccSmoothTau       = 0.20f;  // ~200 ms occlusion smoothing
constexpr float kRoomSmoothTau      = 0.70f;  // reverb target sweep
constexpr float kRoomProbePeriod    = 0.5f;   // 2 Hz room sphere
constexpr float kRoomRayLen         = 80.0f;  // probe tMax (m); miss = outdoors-ish
constexpr float kFanRadius          = 0.40f;  // jitter fan radius at the emitter (m)
constexpr float kEndBias            = 0.25f;  // stop short of the emitter/listener

struct V3 { float x, y, z; };
V3 sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 norm(const V3& a) { float l = len(a); return (l > 1e-6f) ? V3{ a.x / l, a.y / l, a.z / l } : V3{ 0, 1, 0 }; }
V3 cross(const V3& a, const V3& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// Deterministic 64-direction sphere (golden-spiral / Fibonacci lattice).
// Computed once; identical across runs and machines (pure float math).
const std::vector<AcousticRay>& roomDirTemplate() {
    static const std::vector<AcousticRay> dirs = [] {
        std::vector<AcousticRay> v(RtAcoustics::kRoomRays);
        const float ga = 2.39996323f;   // golden angle (rad)
        for (int i = 0; i < RtAcoustics::kRoomRays; ++i) {
            const float yf = 1.0f - 2.0f * ((float)i + 0.5f) / (float)RtAcoustics::kRoomRays;
            const float r  = std::sqrt(std::max(0.0f, 1.0f - yf * yf));
            const float th = ga * (float)i;
            v[i].dx = r * std::cos(th);
            v[i].dy = yf;
            v[i].dz = r * std::sin(th);
            v[i].tMax = kRoomRayLen;
        }
        return v;
    }();
    return dirs;
}

float smoothStep(float cur, float target, float dt, float tau) {
    const float a = 1.0f - std::exp(-dt / tau);
    return cur + (target - cur) * a;
}

// Build the (1 + kFanRays) ray fan listener -> emitter into `out`.
// Returns the number of rays appended (0 when the emitter is on top of the
// listener — trivially unoccluded).
int buildFan(float lx, float ly, float lz, float ex, float ey, float ez,
             AcousticRay* out) {
    const V3 L{ lx, ly, lz }, E{ ex, ey, ez };
    const V3 d = sub(E, L);
    const float dist = len(d);
    if (dist < 0.5f) return 0;   // point-blank: clear by definition
    const V3 dir = norm(d);

    // Stable perpendicular basis for the jitter fan.
    const V3 up = (std::fabs(dir.y) < 0.9f) ? V3{ 0, 1, 0 } : V3{ 1, 0, 0 };
    const V3 u = norm(cross(up, dir));
    const V3 v = cross(dir, u);

    int n = 0;
    auto put = [&](const V3& target) {
        const V3 td = sub(target, L);
        const float tl = len(td);
        const V3 tdir = norm(td);
        AcousticRay r;
        r.ox = L.x; r.oy = L.y; r.oz = L.z;
        r.dx = tdir.x; r.dy = tdir.y; r.dz = tdir.z;
        r.tMax = std::max(0.05f, tl - kEndBias);
        out[n++] = r;
    };
    put(E);   // primary
    for (int k = 0; k < RtAcoustics::kFanRays; ++k) {
        const float a = (float)k * (6.2831853f / (float)RtAcoustics::kFanRays);
        const float cu = std::cos(a) * kFanRadius, cv = std::sin(a) * kFanRadius;
        put(V3{ E.x + u.x * cu + v.x * cv,
                E.y + u.y * cu + v.y * cv,
                E.z + u.z * cu + v.z * cv });
    }
    return n;
}

} // namespace

void RtAcoustics::setTracer(AcousticSubmitFn submit, AcousticHarvestFn harvest, void* user) {
    m_submit = submit;
    m_harvest = harvest;
    m_traceUser = (submit && harvest) ? user : nullptr;
    if (!submit || !harvest) { m_submit = nullptr; m_harvest = nullptr; }
    m_pending.active = false;
}

void RtAcoustics::setListener(float x, float y, float z) {
    m_lx = x; m_ly = y; m_lz = z;
}

int RtAcoustics::activeEmitters() const {
    int n = 0;
    for (const Emitter& e : m_emitters) n += e.used ? 1 : 0;
    return n;
}

float RtAcoustics::occlusionAt(float x, float y, float z) {
    if (!m_enabled || !m_submit) return 0.0f;

    // Match an existing slot (same emitter spot re-firing / live voice query).
    int freeSlot = -1, idlest = -1; float idlestT = -1.0f;
    for (int i = 0; i < kMaxEmitters; ++i) {
        Emitter& e = m_emitters[i];
        if (!e.used) { if (freeSlot < 0) freeSlot = i; continue; }
        const float dx = e.x - x, dy = e.y - y, dz = e.z - z;
        if (dx * dx + dy * dy + dz * dz <= kEmitterMatchRadius * kEmitterMatchRadius) {
            e.idle = 0.0f;
            e.x = x; e.y = y; e.z = z;   // track small drift (moving enemy)
            return e.occ;
        }
        if (e.idle > idlestT) { idlestT = e.idle; idlest = i; }
    }

    // New emitter: take a free slot (or steal the idlest). It starts at 0 and
    // SNAPS to its first traced value when the next batch lands (one update of
    // latency); the mixer re-queries live voices per update, so the snap is
    // applied to the already-playing voice within ~16-33 ms.
    const int slot = (freeSlot >= 0) ? freeSlot : idlest;
    if (slot < 0) return 0.0f;
    Emitter& e = m_emitters[slot];
    e.used = true; e.newborn = true; e.idle = 0.0f;
    e.x = x; e.y = y; e.z = z;
    e.occ = 0.0f; e.occTarget = 0.0f;
    ++e.gen;
    return e.occ;
}

void RtAcoustics::update(float dt) {
    if (!m_enabled || !m_submit) return;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;

    // ---- 1) HARVEST the in-flight batch (if any) and apply via the saved
    // metadata. Generation guards make a reused/expired slot ignore results
    // that were traced for its previous occupant. -----------------------------
    if (m_pending.active) {
        float hit[kBatchCap];
        const int got = m_harvest(m_traceUser, hit, kBatchCap);
        if (got > 0) {
            m_pending.active = false;
            for (int i = 0; i < kMaxEmitters; ++i) {
                if (m_pending.fanCount[i] <= 0) continue;
                Emitter& e = m_emitters[i];
                if (!e.used || e.gen != m_pending.gen[i]) continue;
                int blocked = 0;
                for (int k = 0; k < m_pending.fanCount[i]; ++k)
                    if (hit[m_pending.fanStart[i] + k] >= 0.0f) ++blocked;
                e.occTarget = (float)blocked / (float)m_pending.fanCount[i];
                if (e.newborn) { e.occ = e.occTarget; e.newborn = false; }  // snap
            }
            if (m_pending.roomStart >= 0) probeApply(hit, m_pending.roomStart);
        } else if (got < 0) {
            m_pending.active = false;   // batch lost (device reset/raced) — drop
        }
        // got == 0: still on the GPU — keep waiting, no new submit this update.
    }

    // ---- 2) Expire idle emitters + advance the ~200 ms smoothing ------------
    for (int i = 0; i < kMaxEmitters; ++i) {
        Emitter& e = m_emitters[i];
        if (!e.used) continue;
        e.idle += dt;
        if (e.idle > kEmitterExpire) { e.used = false; ++e.gen; continue; }
        e.occ = smoothStep(e.occ, e.occTarget, dt, kOccSmoothTau);
    }
    m_room.t60 = smoothStep(m_room.t60, m_roomT60Target, dt, kRoomSmoothTau);
    m_room.wet = smoothStep(m_room.wet, m_roomWetTarget, dt, kRoomSmoothTau);

    // ---- 3) SUBMIT the next batch (emitter fans + the 2 Hz room sphere) -----
    if (m_pending.active) return;       // previous batch still pending
    m_roomTimer += dt;

    AcousticRay batch[kBatchCap];
    Pending p;
    int n = 0;
    for (int i = 0; i < kMaxEmitters; ++i) {
        Emitter& e = m_emitters[i];
        p.fanStart[i] = n; p.fanCount[i] = 0; p.gen[i] = e.gen;
        if (!e.used) continue;
        p.fanCount[i] = buildFan(m_lx, m_ly, m_lz, e.x, e.y, e.z, batch + n);
        if (p.fanCount[i] == 0 && e.newborn) { e.occ = 0.0f; e.occTarget = 0.0f; e.newborn = false; }
        n += p.fanCount[i];
    }
    if (m_roomTimer >= kRoomProbePeriod) {
        p.roomStart = n;
        const auto& dirs = roomDirTemplate();
        for (int i = 0; i < kRoomRays; ++i) {
            AcousticRay r = dirs[i];
            r.ox = m_lx; r.oy = m_ly; r.oz = m_lz;
            batch[n++] = r;
        }
    }
    if (n == 0) return;                  // nothing to trace
    p.count = n;
    if (m_submit(m_traceUser, batch, n)) {
        if (p.roomStart >= 0) m_roomTimer = 0.0f;   // probe actually went out
        p.active = true;
        m_pending = p;
    }
    // submit false (busy / TLAS pending): try again next update; values hold.
}

void RtAcoustics::probeApply(const float* hitT, int roomStart) {
    float sum = 0.0f; int misses = 0;
    for (int i = 0; i < kRoomRays; ++i) {
        const float t = hitT[roomStart + i];
        if (t >= 0.0f) sum += t;
        else { sum += kRoomRayLen; ++misses; }
    }
    m_room.meanFreePath = sum / (float)kRoomRays;
    m_room.missFrac = (float)misses / (float)kRoomRays;
    classify(m_room.meanFreePath, m_room.missFrac,
             m_room.cls, m_roomT60Target, m_roomWetTarget);
}

void RtAcoustics::classify(float mfp, float missFrac,
                           RoomClass& cls, float& t60, float& wet) {
    if (missFrac > 0.45f)  { cls = RoomClass::Outdoor; t60 = 0.25f; wet = 0.05f; }
    else if (mfp < 4.5f)   { cls = RoomClass::Small;   t60 = 0.35f; wet = 0.14f; }
    else if (mfp < 12.0f)  { cls = RoomClass::Medium;  t60 = 0.80f; wet = 0.18f; }
    else                   { cls = RoomClass::Large;   t60 = 1.80f; wet = 0.26f; }
}

std::string RtAcoustics::debugString() const {
    static const char* kCls[] = { "small", "medium", "large", "outdoor" };
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[rta] room=%s mfp=%.1fm miss=%.0f%% t60=%.2fs wet=%.2f | emitters:",
                  kCls[(int)m_room.cls], m_room.meanFreePath,
                  m_room.missFrac * 100.0f, m_room.t60, m_room.wet);
    std::string s = buf;
    bool any = false;
    for (const Emitter& e : m_emitters) {
        if (!e.used) continue;
        any = true;
        std::snprintf(buf, sizeof(buf), " (%.0f,%.0f,%.0f occ=%.2f)", e.x, e.y, e.z, e.occ);
        s += buf;
    }
    if (!any) s += " none";
    return s;
}

// ===========================================================================
// Headless self-test (--test-acoustics): deterministic CPU box-room tracer
// behind the SAME async submit/harvest contract (one-update latency).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[acoustics-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[acoustics-test] FAIL ") + name); }
}

struct Aabb { float minx, miny, minz, maxx, maxy, maxz; };

// Test world: a 10 x 4 x 10 room (x,z in [-5,5], y in [0,4], 0.2 m shells) with
// a dividing wall at x ~ 0 that has a DOOR opening at z in [0.8, 2.0]. The
// emitter sits on the +x side aligned with the door; the listener starts on
// the -x side BEHIND the solid wall and "walks" toward the doorway sightline.
struct TestWorld {
    std::vector<Aabb> boxes;
    bool wall = true;     // dividing wall present?
    bool shell = true;    // outer room shell present?

    // Async adapter state (mimics the GPU: submit stores, harvest resolves).
    std::vector<AcousticRay> pendingRays;
    bool inFlight = false;

    void rebuild() {
        boxes.clear();
        if (shell) {
            boxes.push_back({ -5.2f, -0.2f, -5.2f,  5.2f,  0.0f,  5.2f });  // floor
            boxes.push_back({ -5.2f,  4.0f, -5.2f,  5.2f,  4.2f,  5.2f });  // ceiling
            boxes.push_back({ -5.2f,  0.0f, -5.2f, -5.0f,  4.0f,  5.2f });  // -x wall
            boxes.push_back({  5.0f,  0.0f, -5.2f,  5.2f,  4.0f,  5.2f });  // +x wall
            boxes.push_back({ -5.2f,  0.0f, -5.2f,  5.2f,  4.0f, -5.0f });  // -z wall
            boxes.push_back({ -5.2f,  0.0f,  5.0f,  5.2f,  4.0f,  5.2f });  // +z wall
        }
        if (wall) {
            // Divider x in [-0.1, 0.1] with a door hole z in [0.8, 2.0].
            boxes.push_back({ -0.1f, 0.0f, -5.0f, 0.1f, 4.0f, 0.8f });   // solid below door
            boxes.push_back({ -0.1f, 0.0f,  2.0f, 0.1f, 4.0f, 5.0f });   // solid above door
            boxes.push_back({ -0.1f, 3.0f,  0.8f, 0.1f, 4.0f, 2.0f });   // header over door
        }
    }
};

// Slab-method ray vs AABB: nearest positive t within tMax, or -1.
float rayAabb(const AcousticRay& r, const Aabb& b) {
    float t0 = 0.0f, t1 = r.tMax;
    const float o[3] = { r.ox, r.oy, r.oz };
    const float d[3] = { r.dx, r.dy, r.dz };
    const float mn[3] = { b.minx, b.miny, b.minz };
    const float mx[3] = { b.maxx, b.maxy, b.maxz };
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-8f) {
            if (o[a] < mn[a] || o[a] > mx[a]) return -1.0f;
            continue;
        }
        float ta = (mn[a] - o[a]) / d[a];
        float tb = (mx[a] - o[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1.0f;
    }
    return (t0 > 0.0f) ? t0 : -1.0f;   // origin inside a box: ignore (open space)
}

void cpuTraceNow(const TestWorld& w, const AcousticRay* rays, int count, float* outHitT) {
    for (int i = 0; i < count; ++i) {
        float best = -1.0f;
        for (const Aabb& b : w.boxes) {
            const float t = rayAabb(rays[i], b);
            if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
        }
        outHitT[i] = best;
    }
}

// Async adapter: submit stores the batch; harvest (next update) traces it
// against the CURRENT geometry — exactly one update of latency, like the GPU.
bool cpuSubmit(void* user, const AcousticRay* rays, int count) {
    TestWorld* w = static_cast<TestWorld*>(user);
    if (w->inFlight) return false;
    w->pendingRays.assign(rays, rays + count);
    w->inFlight = true;
    return true;
}
int cpuHarvest(void* user, float* outHitT, int capacity) {
    TestWorld* w = static_cast<TestWorld*>(user);
    if (!w->inFlight) return -1;
    const int n = (int)w->pendingRays.size();
    if (capacity < n) { w->inFlight = false; return -1; }
    cpuTraceNow(*w, w->pendingRays.data(), n, outHitT);
    w->inFlight = false;
    return n;
}

// Settle helper: run update() at a fixed step until t seconds have elapsed.
void settle(RtAcoustics& rta, float seconds) {
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) rta.update(dt);
}

} // namespace

bool runAcousticsSelfTest() {
    g_pass = g_fail = 0;

    TestWorld world;
    world.rebuild();

    // Emitter on the +x side, lined up with the door hole (z = 1.4).
    const float ex = 2.0f, ey = 1.6f, ez = 1.4f;

    // T1: tracer sanity — a ray straight at the divider hits at the expected
    // distance (listener side, solid section).
    {
        AcousticRay r;
        r.ox = -3.0f; r.oy = 1.6f; r.oz = -2.0f;
        r.dx = 1.0f; r.dy = 0.0f; r.dz = 0.0f; r.tMax = 10.0f;
        float t = -2.0f;
        cpuTraceNow(world, &r, 1, &t);
        check(t > 2.85f && t < 2.95f, "T1 CPU tracer hits the divider at ~2.9 m");
    }

    // T2: LINE OF SIGHT — listener in the doorway sightline: occlusion ~0 once
    // the first (async, one-update-latency) batch lands.
    {
        world.inFlight = false;   // reset adapter (prior instance may have left a batch)
        RtAcoustics rta;
        rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
        rta.setListener(-3.0f, 1.6f, 1.4f);          // straight through the door
        rta.occlusionAt(ex, ey, ez);                  // register the emitter
        settle(rta, 0.5f);
        const float occ = rta.occlusionAt(ex, ey, ez);
        check(occ <= 0.15f, "T2 line-of-sight occlusion ~0 (through the door)");
    }

    // T3: BEHIND THE WALL — listener behind the solid section: occlusion high
    // (the newborn slot SNAPS to its first traced value — no slow ramp-in).
    {
        world.inFlight = false;
        RtAcoustics rta;
        rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
        rta.setListener(-3.0f, 1.6f, -3.0f);         // behind solid divider
        rta.occlusionAt(ex, ey, ez);
        // Two updates: submit, then harvest+snap. The very next mixer query is
        // already fully muffled (~33 ms after the first shot).
        rta.update(1.0f / 60.0f);
        rta.update(1.0f / 60.0f);
        const float occ = rta.occlusionAt(ex, ey, ez);
        check(occ >= 0.85f, "T3 behind-the-wall occlusion high within 2 updates (newborn snap)");
    }

    // T4: ~200 ms temporal smoothing — remove the wall mid-flight: the smoothed
    // occlusion must FALL but not snap (a real intermediate value), then settle
    // near 0 after ~1 s. Proves no zipper on door-open transitions.
    {
        world.inFlight = false;
        RtAcoustics rta;
        rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
        rta.setListener(-3.0f, 1.6f, -3.0f);
        rta.occlusionAt(ex, ey, ez);
        settle(rta, 0.4f);                                 // converged on target
        const float occ0 = rta.occlusionAt(ex, ey, ez);    // wall up: ~1
        world.wall = false; world.rebuild();               // wall vanishes
        rta.occlusionAt(ex, ey, ez);                       // keep the slot warm
        settle(rta, 0.1f);                                 // a few 16 ms steps
        const float occMid = rta.occlusionAt(ex, ey, ez);
        settle(rta, 1.2f);
        const float occEnd = rta.occlusionAt(ex, ey, ez);
        world.wall = true; world.rebuild();
        check(occ0 >= 0.85f && occMid < occ0 && occMid > 0.20f && occEnd <= 0.10f,
              "T4 smoothing: high -> intermediate (~100 ms) -> ~0 (settled)");
    }

    // T5: ROOM ESTIMATE — inside the 10x4x10 shell = SMALL room; with the shell
    // removed (no geometry at all) = OUTDOOR. The classes must differ and match.
    {
        world.inFlight = false;
        RtAcoustics rta;
        rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
        rta.setListener(-3.0f, 1.6f, -3.0f);
        settle(rta, 1.5f);                                 // >= 2 room probes
        const RoomEstimate inRoom = rta.room();

        TestWorld open;
        open.shell = false; open.wall = false; open.rebuild();
        RtAcoustics rta2;
        rta2.setTracer(&cpuSubmit, &cpuHarvest, &open);
        rta2.setListener(-3.0f, 1.6f, -3.0f);
        settle(rta2, 1.5f);
        const RoomEstimate outdoor = rta2.room();

        check(inRoom.cls == RoomClass::Small && inRoom.meanFreePath < 4.5f,
              "T5a inside the box: SMALL room (mfp < 4.5 m)");
        check(outdoor.cls == RoomClass::Outdoor && outdoor.missFrac > 0.9f,
              "T5b shell removed: OUTDOOR (rays escape)");
        check(inRoom.wet > outdoor.wet && inRoom.t60 > outdoor.t60,
              "T5c small room drives wetter/longer reverb than outdoor");
    }

    // T6: DETERMINISM — two fresh instances over the same sequence produce
    // bit-identical occlusion + room numbers (fixed ray sets, no RNG).
    {
        auto run = [&](RoomEstimate& rm) {
            world.inFlight = false;   // reset adapter between instances
            RtAcoustics rta;
            rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
            rta.setListener(-3.0f, 1.6f, -3.0f);
            rta.occlusionAt(ex, ey, ez);
            settle(rta, 0.7f);
            rm = rta.room();
            return rta.occlusionAt(ex, ey, ez);
        };
        RoomEstimate ra, rb;
        const float oa = run(ra), ob = run(rb);
        check(oa == ob && ra.meanFreePath == rb.meanFreePath &&
              ra.t60 == rb.t60 && ra.wet == rb.wet,
              "T6 deterministic ray sets: identical results across instances");
    }

    // T7: THE WALL-WALK TRANSCRIPT (the audible contract, headless): the
    // listener walks from behind the solid wall to the doorway sightline while
    // the emitter keeps "firing". Occlusion must start high, END ~0, and never
    // rise along the way. The printed lines are the snd_rta_debug story.
    {
        world.inFlight = false;
        RtAcoustics rta;
        rta.setTracer(&cpuSubmit, &cpuHarvest, &world);
        const float walkZ[] = { -3.0f, -2.0f, -1.0f, -0.3f, 0.4f, 1.0f, 1.4f };
        float first = -1.0f, last = -1.0f, prev = 2.0f;
        bool monotone = true;
        x3::logInfo("[acoustics-test] T7 wall-walk transcript (listener walks to the doorway):");
        for (float z : walkZ) {
            rta.setListener(-3.0f, 1.6f, z);
            rta.occlusionAt(ex, ey, ez);     // emitter keeps firing
            settle(rta, 0.4f);               // let the 200 ms smooth track
            const float occ = rta.occlusionAt(ex, ey, ez);
            if (first < 0.0f) first = occ;
            last = occ;
            if (occ > prev + 0.05f) monotone = false;
            prev = occ;
            char line[160];
            std::snprintf(line, sizeof(line),
                          "[acoustics-test]   listener z=%+.1f  ->  occlusion %.2f   %s",
                          z, occ,
                          occ > 0.7f ? "(muffled behind wall)" :
                          occ > 0.2f ? "(edge of the doorway)" : "(clear through the door)");
            x3::logInfo(line);
            x3::logInfo(std::string("[acoustics-test]   ") + rta.debugString());
        }
        check(first >= 0.85f && last <= 0.15f && monotone,
              "T7 wall-walk: high behind wall -> ~0 in the doorway, never rising");
    }

    x3::logInfo(std::string("[acoustics-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::audio
