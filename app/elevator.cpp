// Souped-up strata / disco elevator (core). See app/elevator.h + the design
// summary docs/design/X3_WORLD_BLUEPRINT.md §2.2 + the motion contract
// specs/ELEVATOR.spec.md.
//
// Clean-room: ported from Tim's OWN Babylon module
// Q3Engine/src/features/x3-elevator.js (Tim's IP — NOT id Tech / RBDOOM /
// Quake), built ONLY against X3Native's IPhysicsWorld + Scene + IRenderDevice +
// IAudioSystem interfaces, mirroring DoorSystem's moved-static-body technique.
#include "elevator.h"
#include "mesh_prims.h"
#include "asset_root.h"                    // assetRoot() — the surface_library mount
#include <cstring>                         // strlen (the indicator caption)

#include "engine/core/x3_log.h"
#include "engine/rhi/font_robotomono.h"   // kRobotoMonoTTF — the OLED panel text
#include <stb_truetype.h>                  // declarations only (impl in the engine TU)
#include "engine/audio/IAudioSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ===========================================================================
// CONFIG — earth-strata layers (ported 1:1 from x3-elevator.js CFG.STRATA).
// Facility-relative Y bands; the bottom three glow (Crystal Veins / Magma /
// Alien Substrate). Atmospheric geology — see blueprint §2.2: the band runs to
// -400 m even though the real shaft bottoms higher.
// ===========================================================================
// Y bands STRETCHED (2026-07) so the geology reaches the relocated Club 1127 at
// Y=-800 (was -200): the DESCENT FALL SHAFT bores ~800 m through these bands and
// colors its rock walls per band as they rush up past the falling camera. Same 9
// named layers, same bottom-three glow (Crystal Veins / Magma / Alien Substrate) —
// only the depth ranges widen, so the elevator telemetry + --test-elevator F2 (9
// named layers, index-8 = "Alien Substrate", bottom-3 glow) are unchanged.
static const std::array<StrataLayer, 9> kStrata = {{
    {  80.0f,  200.0f, "Sky & Concrete",  {0.40f, 0.50f, 0.60f}, false, {0,0,0} },
    {  20.0f,   80.0f, "Foundation Stone",{0.35f, 0.30f, 0.25f}, false, {0,0,0} },
    { -40.0f,   20.0f, "Limestone",       {0.55f, 0.52f, 0.45f}, false, {0,0,0} },
    {-180.0f,  -40.0f, "Granite",         {0.30f, 0.28f, 0.32f}, false, {0,0,0} },
    {-340.0f, -180.0f, "Basalt",          {0.20f, 0.18f, 0.15f}, false, {0,0,0} },
    {-520.0f, -340.0f, "Obsidian",        {0.10f, 0.08f, 0.12f}, false, {0,0,0} },
    {-660.0f, -520.0f, "Crystal Veins",   {0.10f, 0.05f, 0.20f}, true,  {0.10f, 0.00f, 1.00f} },
    {-760.0f, -660.0f, "Magma Zone",      {0.25f, 0.06f, 0.02f}, true,  {0.80f, 0.20f, 0.05f} },
    {-860.0f, -760.0f, "Alien Substrate", {0.08f, 0.04f, 0.12f}, true,  {0.20f, 0.04f, 0.40f} },
}};

// ---- OLED text raster (the HoloTerminal stb_truetype move, panel-sized) ----
namespace {
struct OledFont {
    stbtt_fontinfo info{};
    bool ready = false;
    OledFont() {
        const unsigned char* ttf = x3::rhi::kRobotoMonoTTF;
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off >= 0 && stbtt_InitFont(&info, ttf, off)) ready = true;
    }
};
const OledFont& oledFont() { static OledFont f; return f; }

constexpr int kOledW = 256, kOledH = 168;

void oledText(std::vector<uint8_t>& px, const std::string& s, float penX, float topY,
              float ph, float r, float g, float b) {
    const OledFont& f = oledFont();
    if (!f.ready) return;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, ph);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
    const float baseline = topY + asc * scale;
    float pen = penX;
    for (unsigned char ch : s) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        if (ch != ' ') {
            int gw = 0, gh = 0, gxo = 0, gyo = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, scale, scale, ch,
                                                          &gw, &gh, &gxo, &gyo);
            if (bmp) {
                const float gx0 = pen + lsb * scale;
                for (int yy = 0; yy < gh; ++yy)
                    for (int xx = 0; xx < gw; ++xx) {
                        const float cov = bmp[yy * gw + xx] / 255.0f;
                        if (cov <= 0.003f) continue;
                        const int X = (int)std::lround(gx0 + gxo + xx);
                        const int Y = (int)std::lround(baseline + gyo + yy);
                        if (X < 0 || X >= kOledW || Y < 0 || Y >= kOledH) continue;
                        uint8_t* p = &px[((size_t)Y * kOledW + X) * 4];
                        p[0] = (uint8_t)std::min(255.0f, p[0] + r * 255.0f * cov);
                        p[1] = (uint8_t)std::min(255.0f, p[1] + g * 255.0f * cov);
                        p[2] = (uint8_t)std::min(255.0f, p[2] + b * 255.0f * cov);
                    }
                stbtt_FreeBitmap(bmp, nullptr);
            }
        }
        pen += adv * scale;
    }
}

struct OledLine { std::string text; float r, g, b; float px; };

x3::rhi::TextureHandle bakeOled(x3::rhi::IRenderDevice& device,
                                const std::vector<OledLine>& lines) {
    std::vector<uint8_t> px((size_t)kOledW * kOledH * 4);
    for (int y = 0; y < kOledH; ++y)
        for (int x = 0; x < kOledW; ++x) {
            uint8_t* p = &px[((size_t)y * kOledW + x) * 4];
            p[0] = 2; p[1] = 4; p[2] = 8; p[3] = 255;               // near-black glass
            if ((y % 3) == 0) { p[0] = 1; p[1] = 2; p[2] = 5; }     // scanlines
            const int edge = std::min(std::min(x, kOledW - 1 - x),
                                      std::min(y, kOledH - 1 - y));
            if (edge < 2) { p[0] = 10; p[1] = 30; p[2] = 45; }      // bezel line
        }
    float ty = 8.0f;
    for (const OledLine& L : lines) {
        oledText(px, L.text, 10.0f, ty, L.px, L.r, L.g, L.b);
        ty += L.px + 4.0f;
    }
    // X3_OLED_DUMP=<dir>: write each bake as a PPM (debug proof of the panel
    // content without hunting the cab with a camera).
    if (const char* dump = std::getenv("X3_OLED_DUMP")) {
        static int s_dumpN = 0;
        char pth[512];
        std::snprintf(pth, sizeof(pth), "%s/oled_%03d.ppm", dump, s_dumpN++);
        if (FILE* f = std::fopen(pth, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", kOledW, kOledH);
            for (size_t i = 0; i < (size_t)kOledW * kOledH; ++i)
                std::fwrite(&px[i * 4], 1, 3, f);
            std::fclose(f);
        }
    }
    return device.createTexture(px.data(), kOledW, kOledH, true);
}
} // namespace

const std::array<StrataLayer, 9>& ElevatorSystem::strata() { return kStrata; }

// The 1127 disco code (ported from CFG.DISCO_CODE).
static constexpr const char* kDiscoCode = "1127";
static constexpr float kDiscoSlow = 0.25f;   // 1/4-speed glide (CFG.DISCO_SLOW)

// Play a one-shot at the cab center (spatialized) if the handle + audio resolve.
void ElevatorSystem::playOneShot(x3::audio::SoundHandle s, float vol, float pitch) {
    if (!m_audio || !s.valid()) return;
    m_audio->playSound3D(s, m_pos.x, m_pos.y + m_halfY + 1.2f, m_pos.z, vol, pitch);
}

const char* ElevatorSystem::stateName(ElevState s) {
    switch (s) {
        case ElevState::Idle:          return "IDLE";
        case ElevState::Accelerating:  return "ACCELERATING";
        case ElevState::Cruising:      return "CRUISING";
        case ElevState::Decelerating:  return "DECELERATING";
        case ElevState::Arriving:      return "ARRIVING";
        case ElevState::DoorsOpening:  return "DOORS_OPENING";
        case ElevState::DoorsOpen:     return "DOORS_OPEN";
        case ElevState::DoorsClosing:  return "DOORS_CLOSING";
        case ElevState::EmergencyStop: return "EMERGENCY_STOP";
        case ElevState::Freefall:      return "FREEFALL";
        case ElevState::Burst:         return "BURST";
    }
    return "?";
}

bool ElevatorSystem::moving() const {
    // Anything that isn't sitting still with the doors settled counts as "moving"
    // for the legacy bool (preserves the core test's semantics: the cab is busy).
    return m_state != ElevState::Idle && m_state != ElevState::DoorsOpen;
}

const char* ElevatorSystem::currentStratum() const {
    const float y = m_pos.y;
    for (const StrataLayer& s : kStrata)
        if (y >= s.yMin && y <= s.yMax) return s.name;
    return "Unknown";
}

// ===========================================================================
// BUILD — the cab platform (unchanged core; identical to the prior behavior so
// --test-elevator E1-E6 stay green).
// ===========================================================================
bool ElevatorSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           float shaftX, float shaftZ,
                           float cabHalfX, float cabHalfY, float cabHalfZ,
                           const std::vector<float>& stopsCenterY, int startStop) {
    // Legacy vertical build — now a thin wrapper over the 3D graph build: the
    // stop list is sorted low -> high (unchanged), every stop sits at the shaft
    // XZ, and the rails form the full vertical chain. Behavior (and the build
    // log) is byte-identical to the pre-graph implementation.
    std::vector<float> ys = stopsCenterY;
    std::sort(ys.begin(), ys.end());                 // low -> high
    std::vector<Stop> stops;
    stops.reserve(ys.size());
    for (float y : ys)
        stops.push_back(Stop{ x3::phys::Vec3{ shaftX, y, shaftZ }, "", false });
    std::vector<std::pair<int, int>> rails;
    for (int i = 0; i + 1 < (int)stops.size(); ++i) rails.push_back({ i, i + 1 });
    const bool ok = buildEx(scene, device, physics, cabHalfX, cabHalfY, cabHalfZ,
                            stops, rails, startStop);
    m_chainGraph = ok;   // Y-sorted vertical chain: the club insert may rebuild it
    return ok;
}

bool ElevatorSystem::buildEx(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics,
                             float cabHalfX, float cabHalfY, float cabHalfZ,
                             const std::vector<Stop>& stops,
                             const std::vector<std::pair<int, int>>& rails, int startStop) {
    if (stops.empty()) {
        x3::logError("[elevator] build: no stops");
        return false;
    }
    m_halfX = cabHalfX; m_halfY = cabHalfY; m_halfZ = cabHalfZ;
    m_stops = stops;
    m_stopsY.resize(m_stops.size());                 // derived mirror (legacy reads)
    for (size_t i = 0; i < m_stops.size(); ++i) m_stopsY[i] = m_stops[i].center.y;
    const int n = (int)m_stops.size();
    m_adj.assign(m_stops.size(), {});
    for (const std::pair<int, int>& r : rails) {
        if (r.first < 0 || r.second < 0 || r.first >= n || r.second >= n ||
            r.first == r.second) continue;
        m_adj[r.first].push_back(r.second);
        m_adj[r.second].push_back(r.first);
    }
    m_chainGraph = false;                            // build() sets this after us
    m_target = std::clamp(startStop, 0, n - 1);
    m_curStop = m_target;
    m_pos = m_stops[m_target].center;
    m_state = ElevState::Idle;

    // Render mesh authored centered at the body origin (the Entity transform
    // translation drives world placement as the cab moves), like a door slab.
    x3::prims::PrimMesh geo = x3::prims::makeBox(m_halfX, m_halfY, m_halfZ, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Industrial cab platform: brushed metal.
    e.baseColor[0] = 0.40f; e.baseColor[1] = 0.42f; e.baseColor[2] = 0.46f; e.baseColor[3] = 1.0f;
    // Static body (mass 0): blocks/standable while still, repositioned via
    // setBodyPosition while moving. Half-extents == render extents.
    e.body = physics.addBox(x3::phys::Vec3{ m_halfX, m_halfY, m_halfZ }, m_pos, 0.0f,
                            x3::phys::Layer::Static);
    e.transform[12] = m_pos.x;
    e.transform[13] = m_pos.y;
    e.transform[14] = m_pos.z;
    m_entity = scene.add(e);
    m_body = scene.get(m_entity).body;
    m_built = true;

    // (Same log text as the pre-graph build(): the start stop's XZ IS the shaft
    // XZ on legacy vertical cabs, so --test-elevator output is byte-identical.)
    x3::logInfo("[elevator] built: " + std::to_string(m_stopsY.size()) + " stops at (" +
                std::to_string(m_pos.x) + ", " + std::to_string(m_pos.z) + "), start stop " +
                std::to_string(m_target));
    return true;
}

const x3::phys::Vec3& ElevatorSystem::stopCenter(int i) const {
    static const x3::phys::Vec3 kZero{};
    return (i >= 0 && i < (int)m_stops.size()) ? m_stops[i].center : kZero;
}

void ElevatorSystem::unlockHidden() {
    if (m_unlocked) return;
    m_unlocked = true;
    x3::logInfo("[elevator] hidden rail unlocked — the annex stops are on the panel now");
}

// Mirror-preserving stop insert (the disco club-descent used to raw-insert into
// m_stopsY; with m_stops primary the insert must keep stops/mirror/rails in
// sync). Legacy semantics preserved EXACTLY: the list stays Y-sorted, indices
// at/above the insert shift +1, and (as before) m_target/m_curStop/m_riftStop/
// m_secretStop are NOT re-mapped — the caller re-resolves what it needs.
// Returns the new stop's index.
int ElevatorSystem::insertStopY(float centerY) {
    const float sx = m_stops.empty() ? m_pos.x : m_stops[0].center.x;
    const float sz = m_stops.empty() ? m_pos.z : m_stops[0].center.z;
    int idx = 0;
    while (idx < (int)m_stops.size() && m_stops[idx].center.y < centerY) ++idx;
    m_stops.insert(m_stops.begin() + idx,
                   Stop{ x3::phys::Vec3{ sx, centerY, sz }, "", false });
    m_stopsY.resize(m_stops.size());
    for (size_t i = 0; i < m_stops.size(); ++i) m_stopsY[i] = m_stops[i].center.y;
    if (m_chainGraph) {
        // Legacy vertical cab: the rails are simply the sorted chain — rebuild it.
        m_adj.assign(m_stops.size(), {});
        for (int i = 0; i + 1 < (int)m_stops.size(); ++i) {
            m_adj[i].push_back(i + 1);
            m_adj[i + 1].push_back(i);
        }
    } else {
        // Graph cab: remap existing adjacency indices past the insert, then wire
        // the new stop to its nearest neighbour so it stays reachable.
        for (std::vector<int>& row : m_adj)
            for (int& j : row)
                if (j >= idx) ++j;
        m_adj.insert(m_adj.begin() + idx, {});
        int best = -1; float bd = 1e30f;
        for (int i = 0; i < (int)m_stops.size(); ++i) {
            if (i == idx) continue;
            const float dx = m_stops[i].center.x - sx;
            const float dy = m_stops[i].center.y - centerY;
            const float dz = m_stops[i].center.z - sz;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) { m_adj[idx].push_back(best); m_adj[best].push_back(idx); }
    }
    return idx;
}

// ===========================================================================
// T2 — STRAIGHT-SEGMENT 3D MOTION. A ride is a ROUTE (BFS over the rails) of
// straight legs; each leg runs the full trapezoid accel/cruise/decel profile
// on its arclength (the cab STOPS at corners — deliberate, no curve math).
// ===========================================================================
int ElevatorSystem::nearestStopTo3D(const x3::phys::Vec3& p) const {
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < (int)m_stops.size(); ++i) {
        const float dx = m_stops[i].center.x - p.x;
        const float dy = m_stops[i].center.y - p.y;
        const float dz = m_stops[i].center.z - p.z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void ElevatorSystem::beginLeg(int stopIndex) {
    m_segFrom = m_pos;
    m_segTo   = m_stops[stopIndex].center;
    const float dx = m_segTo.x - m_segFrom.x;
    const float dy = m_segTo.y - m_segFrom.y;
    const float dz = m_segTo.z - m_segFrom.z;
    m_segLen = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (m_segLen > 1e-6f)
        m_segDir = x3::phys::Vec3{ dx / m_segLen, dy / m_segLen, dz / m_segLen };
    else
        m_segDir = x3::phys::Vec3{};
    m_s = 0.0f;
}

void ElevatorSystem::buildRouteTo(int stopIndex) {
    m_route.clear();
    const int n = (int)m_stops.size();
    const int start = nearestStopTo3D(m_pos);
    if (n > 0 && start != stopIndex) {
        std::vector<int> prev(n, -1);
        std::vector<int> q; q.reserve(n);
        q.push_back(start);
        prev[start] = start;
        for (size_t h = 0; h < q.size(); ++h) {
            const int u = q[h];
            if (u == stopIndex) break;
            for (int v : m_adj[u])
                if (prev[v] < 0) { prev[v] = u; q.push_back(v); }
        }
        if (prev[stopIndex] >= 0) {
            for (int v = stopIndex; v != start; v = prev[v]) m_route.push_back(v);
            std::reverse(m_route.begin(), m_route.end());
        }
    }
    // Same stop, or a disconnected graph: one direct leg (legacy behavior — the
    // old scalar core drove straight at the target Y no matter what).
    if (m_route.empty()) m_route.push_back(stopIndex);
    // Collapse collinear runs so a straight chain rides as ONE leg. This is what
    // keeps legacy vertical cabs identical: a call 0->5 over the full chain is a
    // single segment — floors pass without stopping, exactly the old behavior.
    {
        x3::phys::Vec3 prevPt = m_pos;
        size_t i = 0;
        while (i + 1 < m_route.size()) {
            const x3::phys::Vec3& a = m_stops[m_route[i]].center;
            const x3::phys::Vec3& b = m_stops[m_route[i + 1]].center;
            const float d1x = a.x - prevPt.x, d1y = a.y - prevPt.y, d1z = a.z - prevPt.z;
            const float d2x = b.x - a.x,      d2y = b.y - a.y,      d2z = b.z - a.z;
            const float l1 = std::sqrt(d1x * d1x + d1y * d1y + d1z * d1z);
            const float l2 = std::sqrt(d2x * d2x + d2y * d2y + d2z * d2z);
            if (l1 < 1e-4f) { m_route.erase(m_route.begin() + i); continue; }   // already there
            const float dot = (l2 < 1e-4f) ? 1.0f
                            : (d1x * d2x + d1y * d2y + d1z * d2z) / (l1 * l2);
            if (dot > 0.9999f) { m_route.erase(m_route.begin() + i); continue; } // collinear
            prevPt = a; ++i;
        }
    }
    beginLeg(m_route.front());
}

// ===========================================================================
// CALL VERBS
// ===========================================================================
void ElevatorSystem::callTo(int stopIndex) {
    if (!m_built) return;
    // W-RIFT: the buried floor. A locked rift stop is not on the panel — a call to it
    // buzzes and goes nowhere until the access code (4790) is entered in the cabin.
    if (stopLocked(stopIndex)) {
        playOneShot(m_snd.buzz, 0.5f, 0.85f);
        x3::logInfo("[elevator] stop is LOCKED OUT (no floor button) — an access code is required");
        return;
    }
    if (m_fsm) {
        // FSM: ignore a fresh call while already busy travelling, but DO accept a
        // call while idle or with the doors open (closes the doors first).
        if (m_state != ElevState::Idle && m_state != ElevState::DoorsOpen) return;
        int t = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
        if (t == m_target && m_state == ElevState::Idle) return;
        startTravelTo(t);
        return;
    }
    // Legacy core.
    if (m_state != ElevState::Idle) return;
    int t = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
    if (t == m_target) return;
    m_target = t;
    m_state = ElevState::Accelerating;   // legacy "Moving"
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

void ElevatorSystem::callNext() {
    if (!m_built || m_stopsY.size() < 2) return;
    // W-RIFT: cycling the stops walks PAST a locked rift level (the cab behaves as if
    // the floor were not there at all — which, as far as the panel is concerned, it
    // is not). Bounded by the stop count so a fully-locked cab can never spin here.
    auto nextOpen = [&](int from) {
        int t = from;
        for (int guard = 0; guard < (int)m_stopsY.size(); ++guard) {
            t = (t + 1) % (int)m_stopsY.size();
            if (!stopLocked(t)) return t;
        }
        return from;
    };
    if (m_fsm) {
        if (m_state != ElevState::Idle && m_state != ElevState::DoorsOpen) return;
        startTravelTo(nextOpen(m_target));
        return;
    }
    if (m_state != ElevState::Idle) return;
    m_target = nextOpen(m_target);
    m_state = ElevState::Accelerating;   // legacy "Moving"
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

bool ElevatorSystem::playerRiding(const x3::phys::Vec3& feet) const {
    if (!m_built) return false;
    const float dx = feet.x - m_pos.x;
    const float dz = feet.z - m_pos.z;
    if (std::fabs(dx) > m_halfX + 0.35f) return false;
    if (std::fabs(dz) > m_halfZ + 0.35f) return false;
    // Y window: feet near/just above the cab top (generous for ride detection).
    const float top = m_pos.y + m_halfY;
    return feet.y >= top - 0.5f && feet.y <= top + 2.5f;
}

// ===========================================================================
// UPDATE — dispatch to the legacy linear move or the souped-up FSM.
// ===========================================================================
float ElevatorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || dt <= 0.0f) { m_carry = x3::phys::Vec3{}; return 0.0f; }
    return m_fsm ? fsmUpdate(dt, scene, physics) : legacyUpdate(dt, scene, physics);
}

float ElevatorSystem::legacyUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_state == ElevState::Idle) { m_carry = x3::phys::Vec3{}; return 0.0f; }

    const float target = m_stopsY[m_target];
    const float dy = target - m_pos.y;
    const float step = m_speed * dt;
    float moved;
    if (std::fabs(dy) <= step) {            // arrive: snap to the stop
        moved = dy;
        m_pos.y = target;
        m_state = ElevState::Idle;
        m_curStop = m_target;
        x3::logInfo("[elevator] arrived at stop " + std::to_string(m_target));
    } else {
        moved = (dy > 0.0f) ? step : -step;
        m_pos.y += moved;
    }
    syncBodyAndTransform(scene, physics);
    m_carry = x3::phys::Vec3{ 0.0f, moved, 0.0f };   // legacy core is vertical-only
    return moved;
}

void ElevatorSystem::syncBodyAndTransform(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    physics.setBodyPosition(m_body, x3::phys::Vec3{ m_pos.x + m_shakeX, m_pos.y, m_pos.z });
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& e = scene.get(m_entity);
        e.transform[12] = m_pos.x + m_shakeX;   // sync render transform to the body
        e.transform[13] = m_pos.y;
        e.transform[14] = m_pos.z;
    }
}

// ===========================================================================
// SOUPED-UP FSM
// ===========================================================================
void ElevatorSystem::enableFsm(bool on) {
    m_fsm = on;
    if (on) {
        // Make the FSM converge from a clean idle state.
        m_state = ElevState::Idle;
        m_fsmSpeed = 0.0f;
        m_stateTime = 0.0f;
        m_doorPct = 1.0f;
        if (m_clubStopY == kUninit) m_clubStopY = kDefaultClubFloorY + m_halfY;
    }
}

void ElevatorSystem::setClubStopY(float centerY) { m_clubStopY = centerY; }

void ElevatorSystem::unlockRift() {
    if (m_riftUnlocked || m_riftStop < 0) return;
    m_riftUnlocked = true;
    x3::logInfo("[elevator] sub-level R1 (RIFT) is now a selectable floor on this cab");
}

void ElevatorSystem::unlockSecret() {
    if (m_secretUnlocked || m_secretStop < 0) return;
    m_secretUnlocked = true;
    x3::logInfo("[elevator] level 4.5 is now a selectable floor on this cab");
}

std::string ElevatorSystem::floorLabel(int stopIndex) const {
    if (stopIndex >= 0 && stopIndex < (int)m_floorLabels.size() &&
        !m_floorLabels[stopIndex].empty())
        return m_floorLabels[stopIndex];
    return "S" + std::to_string(stopIndex);
}

void ElevatorSystem::startTravelTo(int stopIndex) {
    m_target = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
    // T2: plan the route (BFS over the rails; collapses to a single straight leg
    // on legacy vertical cabs) and arm the first segment.
    buildRouteTo(m_target);
    // Begin by closing the doors (DOORS_CLOSING -> ACCELERATING when shut). If the
    // doors are already shut, jump straight to acceleration.
    if (m_doorPct > 0.0f) {
        m_state = ElevState::DoorsClosing;
    } else {
        m_state = ElevState::Accelerating;
    }
    m_stateTime = 0.0f;
    // Disco slow-glide if descending in disco mode (ported from callElevator()).
    if (m_disco && m_stopsY[m_target] < m_pos.y) m_discoSlow = true;
    x3::logInfo(std::string("[elevator] FSM call to stop ") + std::to_string(m_target) +
                (m_discoSlow ? " (DISCO SLOW)" : ""));
}

void ElevatorSystem::emergencyStop() {
    if (!m_fsm) return;
    m_state = ElevState::EmergencyStop;
    m_stateTime = 0.0f;
    m_fsmSpeed = 0.0f;
    playOneShot(m_snd.buzz, 0.9f, 0.7f);   // alarm klaxon
    x3::logInfo("[elevator] EMERGENCY STOP");
}

void ElevatorSystem::freefall() {
    if (!m_fsm) return;
    m_state = ElevState::Freefall;
    m_stateTime = 0.0f;
    m_fsmSpeed = 0.0f;
    x3::logInfo("[elevator] FREEFALL");
}

// T4 — arm the scripted roof burst. Only from the burst dais, only while the
// cab is parked there (Idle / doors open); the doors seal first, then the cab
// punches up. A cab with no burst configured (m_burstStop = -1) never arms.
void ElevatorSystem::armBurst() {
    if (!m_fsm || m_burstStop < 0) return;
    if (m_curStop != m_burstStop) return;
    if (m_state != ElevState::Idle && m_state != ElevState::DoorsOpen) return;
    m_burstPhase = 0;
    m_fsmSpeed = 0.0f;
    m_stateTime = 0.0f;
    if (m_doorPct > 0.0f) {
        m_burstPending = true;               // seal the doors, then Burst
        m_state = ElevState::DoorsClosing;
    } else {
        m_burstPending = false;
        m_state = ElevState::Burst;
    }
    x3::logInfo("[elevator] BURST ARMED — the roof is not the limit");
}

float ElevatorSystem::fsmUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    m_stateTime += dt;
    const x3::phys::Vec3 prevPos = m_pos;

    // T2: travel is the active straight segment — the trapezoid profile advances
    // arclength m_s toward m_segLen (vertical graphs degenerate to the old
    // scalar math exactly: same profile on L = |dY|).
    const float dist = std::max(0.0f, m_segLen - m_s);   // remaining leg arclength
    // In disco slow mode the EFFECTIVE motion dt is scaled down while travelling
    // (ported from eDt in _updateElevator) — a dreamy 1/4-speed glide.
    const float eDt = (m_discoSlow && m_fsmSpeed > 0.5f) ? dt * kDiscoSlow : dt;
    // Advance the cab along the segment by a step of arclength.
    auto advance = [&](float step) {
        m_s = std::min(m_segLen, m_s + step);
        m_pos.x = m_segFrom.x + m_segDir.x * m_s;
        m_pos.y = m_segFrom.y + m_segDir.y * m_s;
        m_pos.z = m_segFrom.z + m_segDir.z * m_s;
        m_totalDist += step;
    };

    switch (m_state) {
        case ElevState::Idle:
            m_fsmSpeed = 0.0f;
            m_shakeX = 0.0f;
            break;

        case ElevState::DoorsClosing:
            if (!m_doorWasClosing) {              // state-entry edge: doors begin to seal
                playOneShot(m_snd.doorClose, 0.7f, 1.0f);
                // MUZAK: the trip's soundtrack starts as the doors seal (JS
                // startMuzak) — a soft 72 BPM pentatonic loop; stopped on arrival.
                if (m_audio && m_snd.muzak.valid() && !m_muzakLoop.valid())
                    m_muzakLoop = m_audio->startLoop(m_snd.muzak, 0.55f, 1.0f);
                m_doorWasClosing = true;
            }
            m_doorPct = std::max(0.0f, m_doorPct - dt / m_tune.doorSpeed);
            if (m_doorPct <= 0.0f) {
                m_doorPct = 0.0f;
                playOneShot(m_snd.doorThunk, 0.8f, 0.95f);   // panels SEAT (layered close)
                // T4: a sealed cab with the burst armed goes UP, not to a stop.
                if (m_burstPending) { m_burstPending = false; m_burstPhase = 0;
                                      m_state = ElevState::Burst; }
                else                { m_state = ElevState::Accelerating; }
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::Accelerating:
            m_fsmSpeed = std::min(m_fsmSpeed + m_tune.accel * eDt, m_tune.maxSpeed);
            advance(m_fsmSpeed * eDt);
            if (m_fsmSpeed >= m_tune.maxSpeed) { m_state = ElevState::Cruising; m_stateTime = 0.0f; }
            if (dist < m_tune.decelDist)       { m_state = ElevState::Decelerating; m_stateTime = 0.0f; }
            break;

        case ElevState::Cruising:
            m_fsmSpeed = m_tune.maxSpeed * (m_discoSlow ? kDiscoSlow : 1.0f);
            advance(m_fsmSpeed * eDt);
            // THE CABLE SLIPS (armed, once, never in disco): on a LONG descent,
            // past the shaft's halfway line, the cable lets go. Lights die, the
            // muzak cuts, the cab drops — the brakes catch it in the Freefall case.
            // (T2: "descending" is the leg direction now — a lateral leg never slips.)
            if (m_slipArmed && !m_slipDone && !m_disco && m_segDir.y < -0.5f &&
                dist > 18.0f && !m_stopsY.empty() &&
                m_pos.y < (m_stopsY.front() + m_stopsY.back()) * 0.5f) {
                m_slipDone = true;
                m_slipAlarmed = false;
                m_resumeStop = m_target;
                m_slipStartY = m_pos.y;
                if (m_muzakLoop.valid()) { m_audio->stopLoop(m_muzakLoop); m_muzakLoop = {}; }
                if (m_flickerT <= 0.0f && !m_lights.empty()) {   // lights DIE
                    m_lightSaveR = m_lights[0].color[0];
                    m_lightSaveG = m_lights[0].color[1];
                    m_lightSaveB = m_lights[0].color[2];
                    m_lights[0].color[0] *= 0.06f;
                    m_lights[0].color[1] *= 0.06f;
                    m_lights[0].color[2] *= 0.06f;
                    m_flickerT = 999.0f;                         // held dark until the catch
                }
                playOneShot(m_snd.creak, 1.0f, 0.65f);           // the cable LETS GO
                m_state = ElevState::Freefall;
                m_stateTime = 0.0f;
                x3::logInfo("[elevator] ...the CABLE SLIPS.");
                break;
            }
            if (dist < m_tune.decelDist) { m_state = ElevState::Decelerating; m_stateTime = 0.0f; }
            break;

        case ElevState::Decelerating:
            m_fsmSpeed = std::max(m_fsmSpeed - m_tune.decel * eDt, 0.5f);
            advance(m_fsmSpeed * eDt);
            if (dist < 0.08f) {
                m_pos = m_segTo;                 // snap to the waypoint (old: targetY)
                m_s = m_segLen;
                m_fsmSpeed = 0.0f;
                if (m_route.size() > 1) {
                    // Corner waypoint: the cab STOPS here, then runs the next
                    // leg's full accel/decel profile (deliberate, no curve math;
                    // doors stay sealed — this is a waypoint, not an arrival).
                    m_route.erase(m_route.begin());
                    beginLeg(m_route.front());
                    m_state = ElevState::Accelerating;
                } else {
                    m_state = ElevState::Arriving;
                }
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::Arriving:
            // Latch the arrival: update the current floor, clear disco-slow, ding,
            // then open the doors (ported from STATE.ARRIVING).
            m_fsmSpeed = 0.0f;
            m_curStop = m_target;
            m_discoSlow = false;
            playOneShot(m_snd.ding, 0.85f, 1.0f);   // arrival chime
            m_lastDingStop = m_curStop;
            // MUZAK off — the ride is over (JS stopMuzak on ARRIVING).
            if (m_muzakLoop.valid()) { m_audio->stopLoop(m_muzakLoop); m_muzakLoop = {}; }
            // HORROR EVENT (JS triggerHorror): 8 % on any arrival, ALWAYS on the
            // bottom stop (the deep is never friendly). Two moods: a light-flicker
            // + cable groan, or a full emergency stop (shake + klaxon). Skipped in
            // disco mode — nothing kills a party like a klaxon.
            if (!m_disco) {
                m_rng = m_rng * 1664525u + 1013904223u;
                const bool bottom = (m_curStop == 0);
                if (bottom || (m_rng >> 16) % 100 < 8) {
                    m_rng = m_rng * 1664525u + 1013904223u;
                    if (!bottom && (m_rng & 1u)) {
                        emergencyStop();                       // shake + klaxon (3 s)
                    } else if (m_flickerT <= 0.0f && !m_lights.empty()) {
                        m_flickerT = 0.45f;                    // interior dips dark
                        m_lightSaveR = m_lights[0].color[0];
                        m_lightSaveG = m_lights[0].color[1];
                        m_lightSaveB = m_lights[0].color[2];
                        m_lights[0].color[0] *= 0.12f;
                        m_lights[0].color[1] *= 0.12f;
                        m_lights[0].color[2] *= 0.12f;
                        playOneShot(m_snd.creak, 0.8f, 0.85f);
                        x3::logInfo("[elevator] ...the lights dip. Something shifts in the shaft.");
                    }
                }
            }
            m_state = ElevState::DoorsOpening;
            m_stateTime = 0.0f;
            x3::logInfo("[elevator] arrived at stop " + std::to_string(m_target) +
                        " (" + currentStratum() + ")");
            break;

        case ElevState::DoorsOpening:
            if (!m_doorWasOpening) {              // state-entry edge: doors begin to retract
                playOneShot(m_snd.doorOpen, 0.7f, 1.0f);
                m_doorWasOpening = true;
            }
            m_doorPct = std::min(1.0f, m_doorPct + dt / m_tune.doorSpeed);
            if (m_doorPct >= 1.0f) {
                m_doorPct = 1.0f;
                playOneShot(m_snd.doorThunk, 0.7f, 1.05f);   // panels park open (layered open)
                m_state = ElevState::DoorsOpen;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::DoorsOpen:
            // Hold open, then settle to idle (the host re-opens on a fresh call).
            if (m_stateTime > m_tune.doorHold) {
                m_state = ElevState::Idle;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::EmergencyStop: {
            // Halt + decaying lateral shake (ported from STATE.EMERGENCY_STOP).
            m_fsmSpeed = 0.0f;
            const float decay = std::max(0.0f, 1.0f - m_stateTime / 3.0f);
            m_shakeX = std::sin(m_stateTime * 30.0f) * 0.05f * decay;
            if (m_stateTime > 3.0f) {
                m_shakeX = 0.0f;
                m_state = ElevState::Idle;
                m_stateTime = 0.0f;
                if (m_pendingResume) {                            // the ride quietly resumes
                    m_pendingResume = false;
                    x3::logInfo("[elevator] resuming to the original stop...");
                    // startTravelTo, NOT callTo: the interrupted ride's target is
                    // still m_target, so callTo's already-called guard would refuse.
                    startTravelTo(std::clamp(m_resumeStop, 0, (int)m_stopsY.size() - 1));
                }
            }
            break;
        }

        case ElevState::Burst: {
            // T4 — THE ROOF BURST (scripted). Phase 0: punch UP at 2x accel,
            // uncapped to 1.6x cruise; the roof plane shatters (host callback,
            // once ever) on the way past; a decel window eases the cab into a
            // hover at the apex. Phase 1: hold 8 s over the world, then hand the
            // return to Freefall — the existing state does the drop theatrics.
            if (m_burstPhase == 0) {
                const float toApex = m_burstApexY - m_pos.y;
                if (toApex < m_tune.decelDist)
                    m_fsmSpeed = std::max(m_fsmSpeed - 2.0f * m_tune.decel * dt, 1.2f);
                else
                    m_fsmSpeed = std::min(m_fsmSpeed + 2.0f * m_tune.accel * dt,
                                          1.6f * m_tune.maxSpeed);
                m_pos.y += m_fsmSpeed * dt;
                m_totalDist += m_fsmSpeed * dt;
                if (!m_burstFired && m_pos.y >= m_burstRoofY) {
                    m_burstFired = true;               // a shattered roof stays shattered
                    if (onRoofShatter) onRoofShatter(m_pos);
                    x3::logInfo("[elevator] ...THROUGH THE ROOF.");
                }
                if (m_pos.y >= m_burstApexY) {
                    m_pos.y = m_burstApexY;
                    m_fsmSpeed = 0.0f;
                    m_burstPhase = 1;
                    m_holdT = 0.0f;
                    x3::logInfo("[elevator] hovering over the world");
                }
            } else {
                m_holdT += dt;
                if (m_holdT >= 8.0f) {
                    m_burstReturning = true;
                    m_state = ElevState::Freefall;
                    m_stateTime = 0.0f;
                    m_fsmSpeed = 0.0f;
                    x3::logInfo("[elevator] ...and the sky lets go.");
                }
            }
            break;
        }

        case ElevState::Freefall: {
            // Dramatic drop (ported from STATE.FREEFALL): accelerate down hard.
            m_fsmSpeed = std::min(m_fsmSpeed + 20.0f * dt, 40.0f);
            m_pos.y -= m_fsmSpeed * dt;
            m_totalDist += m_fsmSpeed * dt;
            // Mid-fall ALARM (once, after the stomach-drop beat of silence).
            if (!m_slipAlarmed && m_stateTime > 0.35f) {
                m_slipAlarmed = true;
                playOneShot(m_snd.buzz, 1.0f, 0.60f);
            }
            // T4 — the BURST RETURN catches below the roofline (roofY - 2), NOT
            // on the cable-slip's 14 m rule (the apex drop is far longer), then
            // resumes as a normal arrival back at the burst stop.
            if (m_burstReturning) {
                if (m_pos.y <= m_burstRoofY - 2.0f) {
                    m_burstReturning = false;
                    m_resumeStop = m_burstStop;
                    m_pendingResume = true;
                    emergencyStop();
                    x3::logInfo("[elevator] ...the emergency brakes CATCH below the roofline.");
                }
                break;
            }
            // THE BRAKES CATCH: after ~14 m of fall, or before the pit. Restore
            // the lights, hand off to EmergencyStop (shake + klaxon + 3 s recover),
            // and queue the resume to the original destination.
            const float floorY = m_stopsY.empty() ? (m_pos.y - 1.0f) : m_stopsY.front();
            if ((m_slipStartY - m_pos.y) > 14.0f || m_pos.y < floorY + 4.0f) {
                if (m_pos.y < floorY) m_pos.y = floorY;          // never through the pit
                if (m_flickerT > 100.0f && !m_lights.empty()) {  // lights back
                    m_lights[0].color[0] = m_lightSaveR;
                    m_lights[0].color[1] = m_lightSaveG;
                    m_lights[0].color[2] = m_lightSaveB;
                    m_flickerT = 0.0f;
                }
                m_pendingResume = (m_resumeStop >= 0);
                emergencyStop();
                x3::logInfo("[elevator] ...the emergency brakes CATCH.");
            }
            break;
        }
    }

    // Cable CREAKS while the cab travels (JS: every 3-5 s over 2 m/s) — the
    // shaft complains under load. Jittered by the tiny LCG so trips differ.
    if (m_fsmSpeed > 2.0f && m_snd.creak.valid()) {
        m_creakTimer -= dt;
        if (m_creakTimer <= 0.0f) {
            m_rng = m_rng * 1664525u + 1013904223u;
            playOneShot(m_snd.creak, 0.55f, 0.9f + 0.25f * (float)((m_rng >> 16) % 100) / 100.0f);
            m_creakTimer = 3.0f + 2.0f * (float)((m_rng >> 8) % 100) / 100.0f;
        }
    }
    // Horror light-flicker recovery: restore the interior fill when the dip ends.
    if (m_flickerT > 0.0f) {
        m_flickerT -= dt;
        if (m_flickerT <= 0.0f && !m_lights.empty()) {
            m_lights[0].color[0] = m_lightSaveR;
            m_lights[0].color[1] = m_lightSaveG;
            m_lights[0].color[2] = m_lightSaveB;
        }
    }

    // Floor-passing dings while moving (procedural-audio hook).
    if (m_fsmSpeed > 1.0f) {
        // (`nearest`, not `near`: asset_root.h reaches windows.h, whose ancient
        //  `near`/`far` segment-model macros would eat the identifier.)
        int nearest = -1; float nd = 1.0e30f;
        for (int i = 0; i < (int)m_stops.size(); ++i) {
            // T2: 3D distance (identical to the old |dY| on vertical cabs, where
            // every stop shares the shaft XZ; on a lateral leg the blip fires as
            // the cab actually PASSES a stop, not any stop at the same height).
            const float ddx = m_pos.x - m_stops[i].center.x;
            const float ddy = m_pos.y - m_stops[i].center.y;
            const float ddz = m_pos.z - m_stops[i].center.z;
            float d = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
            if (d < nd) { nd = d; nearest = i; }
        }
        if (nearest != m_lastDingStop && nd < 1.5f) {
            m_lastDingStop = nearest;
            // Quieter, higher-pitched blip as a floor slides past (vs the arrival ding).
            playOneShot(m_snd.ding, 0.25f, 1.5f);
        }
    }

    // Door SFX state-entry edges reset once we leave that door state, so the next
    // open/close fires its one-shot again.
    if (m_state != ElevState::DoorsClosing) m_doorWasClosing = false;
    if (m_state != ElevState::DoorsOpening) m_doorWasOpening = false;

    updateMotorAudio(dt);

    // ---- OLED TELEMETRY (Babylon twin-viewscreen parity): rebake both panel
    // textures ~3x/s — LEFT: the geological survey (live depth, stratum, speed,
    // ambient temp, lifetime odometer, deep-zone warning); RIGHT: the floor
    // directory with > current / * target markers. stb_truetype raster into a
    // fresh texture; the old one is destroyed (the HoloTerminal rebake move). ----
    m_oledTimer += dt;
    if (m_oledDevice && m_eOledL != kNoLink && m_eOledR != kNoLink && m_oledTimer >= 0.33f) {
        m_oledTimer = 0.0f;
        const float topY = m_stopsY.empty() ? m_pos.y : m_stopsY.back();
        const float depth = std::max(0.0f, topY - m_pos.y);
        char buf[64];
        std::vector<OledLine> L;
        L.push_back({ "GEOLOGICAL SURVEY", 0.25f, 0.55f, 0.85f, 13.0f });
        std::snprintf(buf, sizeof(buf), "DEPTH   %7.1f m", depth);
        L.push_back({ buf, 0.15f, 0.85f, 1.0f, 20.0f });
        // T3: on a lateral leg the geology readout is meaningless (the cab is not
        // descending through anything) — the survey line becomes the ANNEX
        // TRANSIT label band instead, matching the panorama on the strata plane.
        const bool lateralOled = moving() && m_segLen > 1e-3f &&
                                 std::fabs(m_segDir.y) < 0.3f && m_fsmSpeed > 0.05f;
        std::snprintf(buf, sizeof(buf), "STRATUM %s",
                      lateralOled ? ">> ANNEX TRANSIT <<" : currentStratum());
        if (lateralOled) L.push_back({ buf, 0.95f, 0.72f, 0.25f, 14.0f });
        else             L.push_back({ buf, 0.55f, 0.65f, 0.80f, 14.0f });
        std::snprintf(buf, sizeof(buf), "SPEED   %5.1f m/s", m_fsmSpeed);
        L.push_back({ buf, 0.30f, 0.70f, 0.55f, 14.0f });
        std::snprintf(buf, sizeof(buf), "AMBIENT %5.1f C", 18.0f + depth * 0.03f);
        L.push_back({ buf, depth > 120.0f ? 0.85f : 0.30f, depth > 120.0f ? 0.35f : 0.55f, 0.30f, 14.0f });
        std::snprintf(buf, sizeof(buf), "ODO     %6.0f m", m_totalDist);
        L.push_back({ buf, 0.25f, 0.35f, 0.45f, 12.0f });
        if (depth > 120.0f)
            L.push_back({ "! ALIEN SUBSTRATE !", 1.0f, 0.25f, 0.20f, 13.0f });
        x3::rhi::TextureHandle freshL = bakeOled(*m_oledDevice, L);

        std::vector<OledLine> R;
        R.push_back({ "FLOOR DIRECTORY", 0.55f, 0.35f, 0.80f, 13.0f });
        const int n = (int)m_stopsY.size();
        // 9+ stops (RIFT + F1..F7 + the hidden 4.5 row) overflow the 168 px canvas
        // at the 13/15 px row sizes (10 lines ~ 180 px): the BOTTOM row — RIFT —
        // baked half off the texture. Compact the rows when the list is long; the
        // 8-stop bake is pixel-identical to before.
        const float rowPx = (n >= 9) ? 11.0f : 13.0f;
        const float curPx = (n >= 9) ? 13.0f : 15.0f;
        for (int i = n - 1; i >= 0 && (int)R.size() < 11; --i) {
            const char mark = (i == m_curStop) ? '>' : (i == m_target && m_state != ElevState::Idle ? '*' : ' ');
            // W-RIFT: the buried floor reads as a DEAD ROW on the directory until the
            // access code opens it — the panel admits the level exists and nothing more.
            const bool lockedRow = stopLocked(i);
            if (lockedRow) std::snprintf(buf, sizeof(buf), "%c ---- [LOCKED]", mark);
            else           std::snprintf(buf, sizeof(buf), "%c %s", mark, floorLabel(i).c_str());
            const bool cur = (i == m_curStop);
            if (lockedRow) R.push_back({ buf, 0.32f, 0.30f, 0.34f, rowPx });
            else R.push_back({ buf, cur ? 0.95f : 0.40f, cur ? 0.95f : 0.42f, cur ? 1.0f : 0.55f,
                               cur ? curPx : rowPx });
        }
        if (m_disco)
            R.push_back({ "** DISCO MODE **", 1.0f, 0.20f, 0.90f, 13.0f });
        x3::rhi::TextureHandle freshR = bakeOled(*m_oledDevice, R);

        auto assign = [&](uint32_t ent, x3::rhi::TextureHandle& slot, x3::rhi::TextureHandle fresh) {
            if (ent >= scene.size()) return;
            Entity& e = scene.get(ent);
            e.tex = fresh; e.emissiveTex = fresh; e.mrTex = m_oledMr;
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.9f; e.baseColor[3] = 1.0f;
            e.emissive[0] = e.emissive[1] = e.emissive[2] = 1.0f; e.emissive[3] = 1.6f;
            if (slot.valid()) m_oledDevice->destroyTexture(slot);
            slot = fresh;
        };
        assign(m_eOledL, m_oledTexL, freshL);
        assign(m_eOledR, m_oledTexR, freshR);
    }

    // Disco light/strata/glass cue (animates the registered point lights + the
    // strata/disco-ball emissive). m_discoTime advances only in disco mode. When
    // disco is OFF the ceiling fill is re-asserted every frame (the cue dims it
    // purple / strobes it white, and nothing else would put it back) — unless a
    // horror flicker or the cable slip currently owns the light (m_flickerT).
    const float t = m_discoTime;
    if (m_disco) {
        m_discoTime += dt; applyDiscoCue(dt, t);
    } else if (!m_lights.empty() && m_flickerT <= 0.0f) {
        // (must match the buildVisuals value — this re-asserts the practical every frame
        //  that disco/flicker does not own it.)
        m_lights[0].color[0] = 3.10f; m_lights[0].color[1] = 3.35f; m_lights[0].color[2] = 3.80f;
    }

    syncBodyAndTransform(scene, physics);
    layoutVisuals(scene);
    // T2: publish the FULL cab delta; the legacy return is its Y (exact old
    // semantics — vertical rides return exactly what they always did).
    m_carry.x = m_pos.x - prevPos.x;
    m_carry.y = m_pos.y - prevPos.y;
    m_carry.z = m_pos.z - prevPos.z;
    return m_carry.y;
}

void ElevatorSystem::updateMotorAudio(float dt) {
    // Motor/cable hum: a SUSTAINED loop voice whose pitch + volume track cab speed.
    // The frequency sweep 40 -> 120 Hz (CFG.MOTOR_*) maps onto a playback-rate (and
    // hence pitch) ramp on the looped WAV, so the hum audibly winds up under load and
    // settles at rest. Started lazily the first time the cab moves; stopped when it
    // comes to rest so an idle car is quiet.
    const float ratio = std::clamp(m_fsmSpeed / std::max(0.001f, m_tune.maxSpeed), 0.0f, 1.0f);
    const float targetHz = m_tune.motorIdleHz + ratio * (m_tune.motorMoveHz - m_tune.motorIdleHz);
    m_motorHz += (targetHz - m_motorHz) * std::min(1.0f, dt * 5.0f);

    if (!m_audio) return;                   // no backend: nothing to drive (headless/tests)

    if (m_snd.motor.valid()) {              // motor hum needs its WAV; the club swell below does not
        const bool wantHum = m_fsmSpeed > 0.05f;   // only while the cab is actually moving
        // Pitch the hum from the tracked frequency (idle Hz -> rate 0.6, full -> 1.4).
        const float rate = 0.6f + (m_motorHz - m_tune.motorIdleHz) /
                           std::max(1.0f, m_tune.motorMoveHz - m_tune.motorIdleHz) * 0.8f;
        const float vol  = 0.18f + 0.42f * ratio;  // louder under load
        if (wantHum) {
            if (!m_motorLoop.valid())
                m_motorLoop = m_audio->startLoop(m_snd.motor, vol, std::max(0.25f, rate));
            else
                m_audio->setLoopParams(m_motorLoop, vol, std::max(0.25f, rate));
        } else if (m_motorLoop.valid()) {
            m_audio->stopLoop(m_motorLoop);
            m_motorLoop = x3::audio::LoopHandle{};
        }
    }

    // THE DESCENT CROSSFADE (goal #3, the signature): the club track does not just snap
    // on at the club — it SWELLS as the cab sinks toward Club 1127, rising from a distant
    // low bed near the surface to full at the floor, while the cabin muzak DUCKS under it.
    // So the ride from the 1127 code down to Y=-200 audibly becomes the club. Proximity is
    // the cab's depth between the surface (Y=0) and the club stop; graceful no-op if either
    // voice isn't live (music off / headless).
    if (m_clubStopY > kUninit) {
        const float depthMix = std::clamp((0.0f - m_pos.y) / std::max(1.0f, 0.0f - m_clubStopY),
                                          0.0f, 1.0f);
        if (m_clubLoop.valid())                      // club bed rises 0.20 -> 0.95 by depth
            m_audio->setLoopParams(m_clubLoop, 0.20f + 0.75f * depthMix, 1.0f);
        if (m_muzakLoop.valid())                     // muzak ducks out under the swelling club
            m_audio->setLoopParams(m_muzakLoop, 0.55f * (1.0f - 0.85f * depthMix), 1.0f);
    }
}

void ElevatorSystem::autoOpenFor(const x3::phys::Vec3& feet) {
    if (!m_fsm || m_state != ElevState::Idle || m_doorPct > 0.05f) return;
    const float dx = feet.x - m_pos.x, dz = feet.z - m_pos.z;
    const float dy = feet.y - (m_pos.y + m_halfY);
    // ~3.5 m approach ring, at (or a step below/above) the cab's floor level.
    if (dx * dx + dz * dz < 12.25f && dy > -1.5f && dy < 2.5f) {
        m_state = ElevState::DoorsOpening;   // the entry edge plays the door cue
        m_stateTime = 0.0f;
        x3::logInfo("[elevator] proximity: doors auto-open for the approaching rider");
    }
}

// ===========================================================================
// KEYPAD — terminal code entry. "1127" = DISCO toggle (+ descend to the club).
// ===========================================================================
bool ElevatorSystem::keypadDigit(int digit) {
    if (!m_fsm) return false;
    if (digit < 0 || digit > 9) return false;
    m_codeBuf += (char)('0' + digit);
    playOneShot(m_snd.keyClick, 0.5f, 1.0f + 0.04f * (float)digit);   // key-click blip

    if (m_codeBuf.size() >= 4) {
        const std::string tail = m_codeBuf.substr(m_codeBuf.size() - 4);
        // ---- W-RIFT: 4790 — THE RIFT STOP. The same machinery as 1127: a code on the
        // cabin keypad that reveals a floor the panel does not show, and rides you to
        // it. Unlike disco, it is a one-way UNLOCK — once the cab knows the floor
        // exists, the stop stays on the panel for the rest of the run.
        if (tail == kRiftAccessCode && m_riftStop >= 0) {
            m_codeBuf.clear();
            const bool first = !m_riftUnlocked;
            unlockRift();
            playOneShot(m_snd.ding, 1.0f, 1.25f);   // access granted
            x3::logInfo(std::string("[elevator] RIFT ACCESS — code 4790 accepted") +
                        (first ? "; sub-level R1 is on the panel now" : "") +
                        ". Descending to the RIFT stop at Y=" +
                        std::to_string(stopY(m_riftStop)));
            startTravelTo(m_riftStop);
            return true;
        }
        // ---- 4455 — LEVEL 4.5 (fix/spire-hollow-core, owner canon 2026-07-25). The
        // hidden floor between F4 and F5; the elevator is its ONLY access. Same
        // one-way unlock as the RIFT stop. Taught in-world (feat/secret-code-clues)
        // by the chief engineer's log on F4 — "double the four, double the five"
        // (Ch. Eng. Vasquez); the stairwell's unnumbered door sends you to it.
        // 7762 (kMasterBackupCode) is the owner's undocumented master key — it
        // opens this lock too, and is taught NOWHERE in-world by design.
        if ((tail == kNexusAccessCode || tail == kMasterBackupCode) &&
            m_secretStop >= 0) {
            m_codeBuf.clear();
            const bool first = !m_secretUnlocked;
            unlockSecret();
            playOneShot(m_snd.ding, 1.0f, 1.25f);   // access granted
            x3::logInfo(std::string("[elevator] LEVEL 4.5 ACCESS — code accepted") +
                        (first ? "; the hidden floor is on the panel now" : "") +
                        ". Travelling to Y=" + std::to_string(stopY(m_secretStop)));
            startTravelTo(m_secretStop);
            return true;
        }
        // ---- THE ANNEX RAIL (T3): 4790 on a cab that HAS hidden graph stops
        // (a rift cab consumed the code above — the gates are disjoint) reveals
        // them + lights the golden button. UNLOCK ONLY: the rider still presses
        // the button — the reveal is the moment, the ride is theirs to take.
        if (tail == kAnnexCode) {
            bool anyHidden = false;
            for (const Stop& s : m_stops) if (s.hidden) { anyHidden = true; break; }
            if (anyHidden) {
                m_codeBuf.clear();
                const bool first = !m_unlocked;
                unlockHidden();
                playOneShot(m_snd.ding, 1.0f, 1.25f);   // access granted
                x3::logInfo(std::string("[elevator] ANNEX ACCESS — code 4790 accepted") +
                            (first ? "; the golden button is lit" : ""));
                return true;
            }
        }
        // ---- T4: 9999 AT THE BURST DAIS — the roof is not the limit. Arms the
        // scripted roof-burst ride. On any cab with no burst configured (or away
        // from the dais) the code falls through to the wrong-code buzz, which is
        // exactly what the FSM self-test's 9999 negative control expects.
        if (tail == kBurstCode && m_burstStop >= 0 && m_curStop == m_burstStop &&
            (m_state == ElevState::Idle || m_state == ElevState::DoorsOpen)) {
            m_codeBuf.clear();
            playOneShot(m_snd.ding, 1.0f, 1.4f);   // access granted — going UP
            armBurst();
            return true;
        }
        const bool ok = (tail == kDiscoCode);
        m_codeBuf.clear();
        if (ok) {
            m_disco = !m_disco;
            if (m_disco) {
                m_discoTime = 0.0f;
                playOneShot(m_snd.ding, 1.0f, 1.25f);   // bright "access granted" chime
                // THE PARTY HAS A SOUNDTRACK (JS startDiscoMusic): the baked 128 BPM
                // kick/hat/stab loop takes over — the muzak yields for the duration.
                if (m_muzakLoop.valid()) { m_audio->stopLoop(m_muzakLoop); m_muzakLoop = {}; }
                if (m_audio && m_snd.clubTrack.valid() && !m_clubLoop.valid())
                    m_clubLoop = m_audio->startLoop(m_snd.clubTrack, 0.85f, 1.0f);
                x3::logInfo("[elevator] DISCO MODE activated — code 1127 accepted; "
                            "descending to Club 1127 at Y=" + std::to_string(m_clubStopY));
                // Descend to the Club 1127 stop. Make sure the club stop is among
                // the reachable stops (append it if the host didn't add it), then
                // drive the cab there. The Club 1127 lane builds the room at Y=-200.
                m_descendToClub = true;
                int clubIdx = -1;
                for (int i = 0; i < (int)m_stopsY.size(); ++i)
                    if (std::fabs(m_stopsY[i] - m_clubStopY) < 0.5f) { clubIdx = i; break; }
                if (clubIdx < 0) {
                    // Mirror-preserving insert (becomes the low stop on legacy
                    // cabs, exactly as the raw m_stopsY.insert+sort used to).
                    clubIdx = insertStopY(m_clubStopY);
                }
                startTravelTo(clubIdx);
            } else {
                m_disco = false;
                m_discoSlow = false;
                if (m_clubLoop.valid()) { m_audio->stopLoop(m_clubLoop); m_clubLoop = {}; }
                x3::logInfo("[elevator] DISCO MODE off");
            }
            return ok;
        } else {
            playOneShot(m_snd.buzz, 0.6f, 0.8f);   // wrong-code buzzer
            x3::logInfo("[elevator] keypad: wrong code");
        }
    }
    return false;
}

// ===========================================================================
// VISUALS — graybox in-car kit (glass / strata / OLEDs / mirror / terminal /
// ceiling light / disco ball) as child Scene entities offset around the cab.
// ===========================================================================
namespace {
// B6 — THE OLED/INDICATOR TEXT RENDERED UPSIDE-DOWN, AND IT IS A **V** FLIP, NOT A MIRROR.
// ROOT CAUSE: an image is uploaded TOP-DOWN (row 0 = the top of the raster, and V=0 samples
// row 0), but makeBox()'s V axis runs BOTTOM-UP — mesh_prims.h:85-88 put v=0 at -hy and
// v=vmax at +hy, on every one of the six faces. Nothing anywhere flips it. So the top row of
// the bake lands at the BOTTOM of the panel and the plate reads standing on its head.
// PROVED, not guessed (X3_OLED_DUMP): the baked PPM is upright and correct, while the panel
// in-cab shows the lines in reverse order with each glyph flipped vertically — and the glyph
// order LEFT-TO-RIGHT is intact, so U is fine on every face and this is a pure V flip.
// WHY IT SURVIVED: on a TILING wall texture (every addKitTex surface, every level prim) a V
// flip is invisible. It only bites a BAKED, NON-TILING image — i.e. text.
// WHY NOT FIX makeBox(): that UV convention is shared by every textured prim in the game,
// including their NORMAL maps (a V flip flips the derived bitangent, inverting the relief).
// Flipping it there is an engine-wide relight, not a bug fix. Flip V on the panels that
// actually carry a baked image, and nowhere else.
void flipMeshV(x3::prims::PrimMesh& m) {
    // makeBox emits one quad (4 verts) per face, ordered (0,0) (u,0) (u,v) (0,v).
    for (size_t i = 0; i + 3 < m.verts.size(); i += 4) {
        std::swap(m.verts[i + 0].uv[1], m.verts[i + 3].uv[1]);
        std::swap(m.verts[i + 1].uv[1], m.verts[i + 2].uv[1]);
    }
}

// B6-2 — THE BAKED PANELS SHOWED A CROP OF THE BAKE, NOT THE BAKE.
// ROOT CAUSE: makeBox UVs are TILING METERS (a face spans u = 2*extent*uvScale, v
// likewise — mesh_prims.h:91-98), and addKit passes uvScale = 1.0. On a wall texture
// that is the point; on a BAKED, NON-TILING image it is a crop rect. The 0.60 x 0.38 m
// OLED face sampled uv [0,0.60]x[0,0.38] — the top-left ~60% x 38% of the 256x168 bake
// (the survey read "DEPTH   90." with every lower row gone), and the 1.80 x 0.32 m
// indicator plate sampled [0,1.8]x[0,0.32] — "F1" bottom-clipped, off-center, tiled.
// PROVED, not guessed (before-shots in shots_cab/ + the X3_OLED_DUMP PPMs: the bakes
// are complete and correct; only the quads crop them). Same survival story as B6:
// meters-as-UV is invisible on tiling surfaces, and bites exactly the panels that
// carry a baked image. So: normalize each face quad's UVs to [0,1] and the full bake
// lands on the face. Applied ONLY to the baked-image panels (with flipV), never to
// tiling surfaces.
void fitMeshUV01(x3::prims::PrimMesh& m) {
    for (size_t i = 0; i + 3 < m.verts.size(); i += 4) {
        float um = 0.0f, vm = 0.0f;
        for (int k = 0; k < 4; ++k) {
            um = std::max(um, m.verts[i + k].uv[0]);
            vm = std::max(vm, m.verts[i + k].uv[1]);
        }
        if (um <= 0.0f || vm <= 0.0f) continue;   // degenerate face — leave it
        for (int k = 0; k < 4; ++k) {
            m.verts[i + k].uv[0] /= um;
            m.verts[i + k].uv[1] /= vm;
        }
    }
}

// Add a tinted (optionally emissive) box entity centered at the cab origin; the
// per-frame layout offsets it. Returns the entity id (kNoLink on a bad mesh).
// `flipV` (B6): set for panels that display a BAKED image (the OLEDs, the floor
// indicator) so the raster's top row lands at the top of the panel.
uint32_t addKit(Scene& scene, x3::rhi::IRenderDevice& device,
                float hx, float hy, float hz,
                const float color[4], const float emissive[4], bool flipV = false) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 1.0f);
    if (flipV) { flipMeshV(geo); fitMeshUV01(geo); }   // baked image: top-down V + full-bake UV
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Prop;
    e.body.id = 0;                  // purely visual — no physics body
    return scene.add(e);
}

// R11: the same box, but MADE OF SOMETHING. A SurfaceSet (albedo + normal + mr) on a
// box whose UVs tile every `uvPerM` metres. This is the difference between a grey box
// and brushed steel: the normal map is what catches the practical's light and turns a
// flat wall into a wall with SEAMS, RIVETS and WEAR on it (RIFTHUB_ART_TARGET #4).
uint32_t addKitTex(Scene& scene, x3::rhi::IRenderDevice& device,
                   float hx, float hy, float hz, const SurfaceSet& sf,
                   const float tint[3], float uvPerM = 0.5f) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, uvPerM);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    if (sf.ok) { e.tex = sf.albedo; e.normalTex = sf.normal; e.mrTex = sf.mr; }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.tag = (uint32_t)Tag::Prop;
    e.body.id = 0;
    return scene.add(e);
}

// The FLOOR-INDICATOR display: the floor label + the state, baked as TEXT into a
// texture, so the plate glows exactly WHERE THE GLYPHS ARE (per-texel emissive) rather
// than being a flat glowing bar. Same stb_truetype route as the OLEDs.
std::vector<uint8_t> renderIndicatorPixels(const std::string& label, const char* state,
                                           float r, float g, float b) {
    std::vector<uint8_t> px((size_t)kOledW * kOledH * 4);
    for (int y = 0; y < kOledH; ++y)
        for (int x = 0; x < kOledW; ++x) {
            uint8_t* p = &px[((size_t)y * kOledW + x) * 4];
            p[0] = 3; p[1] = 3; p[2] = 4; p[3] = 255;              // black glass substrate
            if ((y % 4) == 0) { p[0] = 1; p[1] = 1; p[2] = 2; }    // scanlines
        }
    // Big floor numeral, centred; a small state caption beneath it.
    const float glyphPx = 92.0f;
    const float w = 0.60f * glyphPx * (float)label.size();          // rough mono advance
    oledText(px, label, std::max(6.0f, (kOledW - w) * 0.5f), 12.0f, glyphPx, r, g, b);
    const float cw = 0.60f * 22.0f * (float)std::strlen(state);
    oledText(px, state, std::max(4.0f, (kOledW - cw) * 0.5f), 116.0f, 22.0f,
             r * 0.55f, g * 0.55f, b * 0.55f);
    return px;
}

x3::rhi::TextureHandle bakeIndicator(x3::rhi::IRenderDevice& device,
                                     const std::string& label, const char* state,
                                     float r, float g, float b) {
    std::vector<uint8_t> px = renderIndicatorPixels(label, state, r, g, b);
    return device.createTexture(px.data(), kOledW, kOledH, true);
}
} // namespace

// THE ANTI-REGRESSION PROBE (the rifthub's holoReadoutInkFraction, for the lift).
// "The plate exists" and "the plate SAYS SOMETHING" are not the same assertion — the
// rifthub consoles were declared fixed NINE times on the strength of the first one. So
// measure INK: the fraction of texels in the baked indicator that are actually lit. A
// blank/failed bake probes at ~0 and fails, and the test carries its own negative
// control (empty label + empty caption) proving the probe CAN fail.
float elevatorIndicatorInkFraction(const std::string& label, const char* state) {
    const std::vector<uint8_t> px = renderIndicatorPixels(label, state, 1.0f, 0.72f, 0.22f);
    size_t lit = 0;
    for (size_t i = 0; i < (size_t)kOledW * kOledH; ++i) {
        const uint8_t* p = &px[i * 4];
        // "Ink" = clearly above the black-glass substrate (3,3,4) + its scanlines.
        if ((int)p[0] + (int)p[1] + (int)p[2] > 60) ++lit;
    }
    return (float)lit / (float)(kOledW * kOledH);
}

void ElevatorSystem::buildVisuals(Scene& scene, x3::rhi::IRenderDevice& device) {
    if (!m_built || m_visualsBuilt) return;

    // ===================================================================================
    // R11 — THE RIFT-HUB GLOW-UP, APPLIED TO THE LIFT (2026-07-12)
    // ===================================================================================
    // WHAT WAS HERE: the car was a GRAYBOX KIT and, worse, a car with NO WALLS. The lift
    // was a physics platform (a thin box) plus nine floating flat-tinted boxes — a glass
    // slab, a "mirror" that was a 0.92 near-WHITE box, a strata plane with a fake 0.5
    // self-emissive on solid rock, doors with an emissive "seam", a terminal that was a
    // glowing blue box, a ceiling "light" that was a glowing box with no fixture, and an
    // indicator that was a flat glowing bar. Stand in the cab and you saw the SHAFT's
    // graybox through the gaps. Every one of those is one of the recipe's named crutches:
    // fake self-emissive on geometry (#1), flat colours instead of materials (#4), and
    // whole objects glowing instead of small lit cores (#7).
    //
    // WHAT IT IS NOW: a real industrial lift. A CAB SHELL (floor / four walls / ceiling)
    // made of actual PBR sets — brushed panels with rivets and wear in the NORMAL map,
    // a worn deck plate, dark trim — so the surfaces catch the light and read as metal.
    // ONE practical: a recessed ceiling fixture with a dark housing and a small hot core
    // inside it (never the whole object). Emissive is confined to that core, the floor
    // indicator's GLYPHS (per-texel, not a lit slab), and the OLEDs' text.
    // ===================================================================================
    // R11 — THE CAR IS THE SIZE OF THE CAR. The old visuals hard-coded "3.8 x 5.2 x 3.8 m"
    // from the Babylon CFG, but the actual cab platform this class builds is 2*m_halfX
    // (2.8 m) and the level1 SHAFT it rides in is ~3 m across. So a 3.8 m shell would be
    // built OUTSIDE the shaft — its walls hidden behind the shaft's own, which is exactly
    // what happened the first time this shell was built and screenshotted (you stood in
    // the cab and saw the shaft's graybox). And 5.2 m of headroom is not a lift, it is a
    // silo. The shell is now derived from the platform: it fits, and it is room-height.
    const float carW = m_halfX * 2.0f, carD = m_halfZ * 2.0f;
    const float carH = 2.60f;
    const float T = 0.10f;
    const float noEm[4] = {0,0,0,0};

    // The material sets. mw_metal_panels_a = riveted industrial panel (the rifthub's
    // ring plates use it), mw_metal_trim_a = true dark gunmetal, mw_metal_grate = deck.
    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& sPanel = m_surf.get(device, "mw_metal_panels_a");   // cab walls
    const SurfaceSet& sTrim  = m_surf.get(device, "mw_metal_trim_a");     // dark structure
    const SurfaceSet& sDeck  = m_surf.get(device, "mw_metal_grate");      // the floor plate

    // ---- THE SHELL. Thin textured slabs, one per face. The +X face is the doorway, so
    // it gets two jambs and a header instead of a wall (level-authoring law: nothing is
    // ever drawn across an opening).
    // The doors part to +/- kDoorHalf, so the jambs must start OUTSIDE that span or they
    // would be drawn across the opening (and the doors would slide into them).
    const float kDoorHalf = carD * 0.24f + carD * 0.25f * 0.5f;   // 1.39 m: door reach
    const float jambHZ    = (carD * 0.5f - kDoorHalf) * 0.5f;     // fills 1.39 .. 1.90
    const float jambCZ    = kDoorHalf + jambHZ;
    { const float tWall[3] = { 0.44f, 0.46f, 0.49f };   // brushed steel, honest albedo
      m_eWallBack = addKitTex(scene, device, carW*0.5f, carH*0.5f, T*0.5f, sPanel, tWall, 0.55f);
      m_eWallSide = addKitTex(scene, device, carW*0.5f, carH*0.5f, T*0.5f, sPanel, tWall, 0.55f);
      const float tJamb[3] = { 0.30f, 0.31f, 0.34f };   // darker at the doors
      m_eWallFrontL = addKitTex(scene, device, T*0.5f, carH*0.5f, jambHZ, sPanel, tJamb, 0.55f);
      m_eWallFrontR = addKitTex(scene, device, T*0.5f, carH*0.5f, jambHZ, sPanel, tJamb, 0.55f);
      // Header: from the top of the door slabs to the ceiling (never over the opening).
      m_eHeader     = addKitTex(scene, device, T*0.5f, 0.21f, carD*0.5f, sTrim, tJamb, 0.6f); }
    // Ceiling slab + a deck kick-plate skirt (the detail that says "this is a box someone
    // WELDED", not a room that happens to end).
    { const float tCeil[3] = { 0.24f, 0.25f, 0.27f };
      m_eCabCeil = addKitTex(scene, device, carW*0.5f, T*0.5f, carD*0.5f, sTrim, tCeil, 0.7f);
      const float tKick[3] = { 0.20f, 0.21f, 0.23f };
      m_eKick = addKitTex(scene, device, carW*0.5f, 0.12f, carD*0.5f, sTrim, tKick, 0.8f); }
    // THE DECK: the cab platform entity itself (it already exists as the physics body's
    // render mesh) gets a real worn floor plate instead of baseColor 0.40 flat grey.
    if (m_entity != kNoLink && m_entity < scene.size() && sDeck.ok) {
        Entity& deck = scene.get(m_entity);
        deck.tex = sDeck.albedo; deck.normalTex = sDeck.normal; deck.mrTex = sDeck.mr;
        deck.baseColor[0] = 0.42f; deck.baseColor[1] = 0.43f; deck.baseColor[2] = 0.45f;
    }

    // Glass observation wall (left, -X): translucent smoked slab, set INTO a framed
    // aperture. The window glass is carH*0.40 tall x carD*0.42 deep (half-extents), so
    // the frame is four panel strips filling the rest of the -X face around it.
    //
    // B7 — IT WAS NOT GLASS. IT WAS A BRIGHT, NOISY, OPAQUE FILL.
    // It was added via addKit with baseColor alpha 0.35 and NO MR texel — so Scene::render
    // (scene.cpp:150-177) sent it down drawMeshEmissive(), the NON-PBR path, which does not
    // read baseColor[3] AT ALL. The alpha was decorative. The result: a fully OPAQUE slab
    // shaded by the unnormalized-Lambert prim path (~pi x brighter than every GLB beside it
    // — R1), which also HID THE STRATA PLANE 0.4 m behind it. The window's entire purpose is
    // to show that plane. Same bug class as B11 / L4 (a surface that needs the PBR route and
    // silently doesn't get one because it has no MR map).
    //
    // THE MEDICINE, same as the -Z mirror below: a real MR texel (-> the PBR/IBL branch) and
    // an honest albedo. Smoked glass is DARK — it borrows its brightness from what it
    // reflects and what shows through it, exactly like the mirror.
    //
    // WHY NOT Entity::transparent (the dedicated glass pass): normal glass (additive == 0)
    // rides the OPAQUE record range and therefore REPLAYS IN THE DEPTH PRE-PASS
    // (vk_passes.cpp:878-886) — its depth would then reject the strata plane behind it, and
    // we would be back to a flat fill with a fancier name. That is L3, and the strata is the
    // one thing this window must not occlude. Entity::alphaBlend (scene.h:117-125) is the
    // route that does NOT write depth: it rides the BLEND tail (vk_passes.cpp:1771-1774),
    // recorded after opaque, blending over the already-lit scene. baseColor[3] is its blend
    // alpha. That is real, see-through, PBR-lit glass with no depth trap.
    { const uint8_t mrGlass[4] = { 0, 30, 0, 255 };   // glTF MR: G=rough(0.12) B=metal(0)
      // metal 0 = a DIELECTRIC. Glass is not metal: it keeps a (tiny, dark) diffuse lobe and
      // takes its shine from a fresnel specular, which is what makes a window read as a
      // window and not as chrome. Roughness 0.12, not 0: a polished pane, but a mirror-flat
      // one hands the frame to the SSR/IBL sheet (the -Z mirror's note below is the record of
      // that exact trap).
      x3::rhi::TextureHandle gmr = device.createTexture(mrGlass, 1, 1, false);
      x3::prims::PrimMesh geo = x3::prims::makeBox(T*0.5f, carH*0.40f, carD*0.42f, 0,0,0, 1.0f);
      Entity e;
      e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                 geo.index.data(), (uint32_t)geo.index.size());
      e.mrTex = gmr;
      // Smoked, near-black glass. alpha = the BLEND alpha (the see-through dial).
      e.baseColor[0] = 0.05f; e.baseColor[1] = 0.06f; e.baseColor[2] = 0.08f;
      e.baseColor[3] = 0.42f;
      e.alphaBlend = true;
      e.tag = (uint32_t)Tag::Prop; e.body.id = 0;
      m_eGlass = scene.add(e); }
    { const float tWin[3] = { 0.40f, 0.42f, 0.45f };
      const float gh = carH * 0.40f, gd = carD * 0.42f;      // glass half-extents
      const float topH = (carH * 0.5f - gh) * 0.5f;          // strip above the glass
      const float sideD = (carD * 0.5f - gd) * 0.5f;         // strips beside it
      m_eWinTop = addKitTex(scene, device, T*0.5f, topH,  carD*0.5f, sPanel, tWin, 0.55f);
      m_eWinBot = addKitTex(scene, device, T*0.5f, topH,  carD*0.5f, sPanel, tWin, 0.55f);
      m_eWinL   = addKitTex(scene, device, T*0.5f, gh,    sideD,     sPanel, tWin, 0.55f);
      m_eWinR   = addKitTex(scene, device, T*0.5f, gh,    sideD,     sPanel, tWin, 0.55f); }

    // Earth-strata scroll plane behind the glass (further -X). R11: the FAKE SELF-EMISSIVE
    // is gone for the non-glowing strata. Solid rock does not emit light; it was carrying
    // a 0.5-strength glow of its own base colour purely to be visible, which is crutch #1
    // in the recipe (and reads as glow-in-the-dark granite). The three GLOWING strata
    // (Crystal Veins / Magma / Alien Substrate) keep theirs — those are supposed to glow;
    // that is the whole point of the descent. Driven per frame in layoutVisuals().
    //
    // B7, SECOND ORDER — THE CRUTCH WAS HIDING A BLOWN-OUT SLAB. Nobody could see this plane
    // for its entire life: the -X window in front of it was accidentally OPAQUE (see B7
    // above). The moment the window became real glass, the rock face behind it rendered as a
    // FLAT CLIPPED WHITE SHEET (measured — the screenshot is the proof).
    // The cause is R1, not the albedo. This plane is a bare prim with NO MR texel, so
    // Scene::render sends it to drawMeshEmissive() — the UNNORMALIZED Lambert path, which is
    // ~pi x brighter than the PBR path every GLB in the cab uses. It sits 1.4 m from the
    // practical, so it took that pi x on the strongest light in the car and clipped.
    // The previous pass DID see the symptom (the "brightest thing in the cab" note in
    // layoutVisuals) and reached for the albedo — 0.55 -> 0.42. That is the VALUE-not-lumens
    // reflex, and here it was aimed at the wrong dial: no albedo below 1.0 can pay off a
    // factor of pi. Give it a real (matte, dielectric) MR texel and it takes the same
    // physically-normalized path as everything else. Rock is rough and it is NOT metal.
    { const uint8_t mrRock[4] = { 0, 235, 0, 255 };   // glTF MR: G=rough(0.92) B=metal(0)
      m_strataMr = device.createTexture(mrRock, 1, 1, false);
      const float c[4] = {0.30f, 0.28f, 0.32f, 1.0f};
      m_eStrata = addKit(scene, device, 0.02f, carH*0.45f, carD*0.40f, c, noEm);
      if (m_eStrata != kNoLink && m_eStrata < scene.size())
          scene.get(m_eStrata).mrTex = m_strataMr; }

    // Back-wall mirror (-Z). R11: was baseColor 0.92/0.92/0.95 — a NEAR-WHITE box. There
    // is no such thing as a white mirror: a mirror is DARK glass with a very low roughness
    // that borrows its brightness from what it reflects. Give it the PBR route with a
    // polished MR texel so the SSR/IBL path actually reflects the cab, and drop the albedo
    // to what polished steel really is.
    // Roughness 60 (not 18): a POLISHED PANEL, not a laboratory mirror. At near-zero
    // roughness the SSR/IBL path painted it as a huge, noisy, blown-out white sheet that
    // owned the whole car — the exact "brightest thing in frame drags everything else
    // down" trap the recipe warns about (auto-exposure meters it). A little roughness
    // scatters the reflection into a sheen and hands the frame back to the room.
    { const uint8_t mrMirror[4] = { 255, 60, 225, 255 };   // G=rough(60) B=metal(225)
      x3::rhi::TextureHandle mmr = device.createTexture(mrMirror, 1, 1, false);
      x3::prims::PrimMesh geo = x3::prims::makeBox(carW*0.30f, carH*0.26f, 0.02f, 0,0,0, 1.0f);
      Entity e;
      e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                 geo.index.data(), (uint32_t)geo.index.size());
      e.mrTex = mmr;
      e.baseColor[0] = 0.34f; e.baseColor[1] = 0.35f; e.baseColor[2] = 0.37f; e.baseColor[3] = 1.0f;
      e.tag = (uint32_t)Tag::Prop; e.body.id = 0;
      m_eMirror = scene.add(e); }

    // Twin OLED viewscreens: left = geo survey, right = floor directory. LIVE
    // text telemetry baked by update() (the graybox emissive is just the boot
    // frame). Capture the device + a glossy MR texel for the PBR screen route.
    m_oledDevice = &device;
    { const uint8_t mr1[4] = { 255, 45, 30, 255 };   // G=rough(45) B=metal(30): glossy panel
      m_oledMr = device.createTexture(mr1, 1, 1, false); }
    // B6: flipV — these panels carry a BAKED TEXT raster, so they need the box's
    // bottom-up V flipped to the image's top-down rows. See flipMeshV().
    { const float c[4] = {0.0f, 0.05f, 0.10f, 1.0f};
      const float em[4] = {0.0f, 0.40f, 0.80f, 1.2f};
      m_eOledL = addKit(scene, device, 0.30f, 0.19f, 0.02f, c, em, /*flipV*/true); }
    // B6-2: the directory hangs on the +X wall facing -X into the cab, so its WIDE
    // axis is Z and its THIN axis is X (it was 0.30/0.02 — a fin EDGE-ON to the
    // rider, the whole display face buried pointing into the door wall; all you saw
    // was a 4 cm glowing sliver of the bake's left edge. Same mount logic as the
    // terminal below it: thin X, wide Z.)
    { const float c[4] = {0.05f, 0.0f, 0.08f, 1.0f};
      const float em[4] = {0.30f, 0.10f, 0.60f, 1.0f};
      m_eOledR = addKit(scene, device, 0.02f, 0.19f, 0.30f, c, em, /*flipV*/true); }

    // Access terminal + keypad (right wall, +X). R11: was a GLOWING BLUE BOX (emissive
    // 0.0/0.30/0.90 over its whole body). A terminal is a dark metal housing with a lit
    // FACE; the glow belongs to the face, not the casing (recipe #7). The housing is now
    // gunmetal, and a small keypad lens inside it carries the light.
    { const float tHousing[3] = { 0.16f, 0.17f, 0.20f };
      m_eTerm = addKitTex(scene, device, 0.05f, 0.30f, 0.18f, sTrim, tHousing, 1.2f); }

    // T3 — THE GOLDEN BUTTON (annex rail). Authored DARK (emissive 0.05) beside
    // the terminal; code 4790 lights it to 3.0 (driven per frame in
    // layoutVisuals from m_unlocked). It exists from build so the unlock is a
    // light-up beat, not a pop-in.
    { const float c[4] = { 0.55f, 0.42f, 0.10f, 1.0f };
      const float em[4] = { 1.0f, 0.75f, 0.18f, 0.05f };
      m_eGoldBtn = addKit(scene, device, 0.03f, 0.05f, 0.05f, c, em); }

    // Ceiling disco ball (hidden until disco — emissive 0 until then). R11: SHRUNK from a
    // 0.6 m box to a 0.36 m one, and hung clear of the practical (see the light-position
    // note in layoutVisuals: the ball used to swallow the cab's only light source whole).
    // ...and DARK when it is off (0.90 albedo read as a white box floating in the car).
    { const float c[4] = {0.16f, 0.16f, 0.18f, 1.0f};
      m_eDiscoBall = addKit(scene, device, 0.18f, 0.18f, 0.18f, c, noEm); }

    // ---- THE PRACTICAL. R11: the old "ceiling light" was a self-emissive box with no
    // fixture — the object WAS the glow. A real lift has a recessed pan fixture: a dark
    // metal HOUSING with a small hot lit CORE inside it. The housing catches the light it
    // is emitting (it is textured metal), the core is the only emissive surface, and the
    // point light that actually lights the cab is anchored inside the housing.
    { const float tHouse[3] = { 0.18f, 0.19f, 0.21f };
      m_eFixture = addKitTex(scene, device, carW*0.32f, 0.09f, 0.22f, sTrim, tHouse, 1.0f);
      const float cCore[4] = { 0.85f, 0.92f, 1.00f, 1.0f };
      const float emCore[4] = { 0.90f, 0.95f, 1.00f, 2.2f };   // small + hot, not big + dim
      m_eLensCore = addKit(scene, device, carW*0.28f, 0.015f, 0.16f, cCore, emCore);
      m_eCeil = m_eLensCore; }   // legacy alias: layout/disco drive the lit core

    // FOUR STEEL CABLES rising from the car roof up the shaft (the JS builds 300 m
    // of them; 120 m reads identically from inside and keeps the graybox cheap).
    // Thin dark columns at the roof corners; they ride the cab in layoutVisuals().
    { const float c[4] = {0.16f, 0.17f, 0.19f, 1.0f};
      for (int i = 0; i < 4; ++i)
          m_eCable[i] = addKit(scene, device, 0.03f, 60.0f, 0.03f, c, noEm); }

    // Twin sliding DOOR panels on the front (+X) wall — two tall slabs that part along Z
    // as m_doorPct rises. R11: they were flat 0.34-grey boxes with a FAKE EMISSIVE SEAM
    // ("so it reads in the dark") — geometry lit from inside itself, crutch #1. They are
    // brushed panel now: the seam reads because the normal map and the practical's
    // grazing light give it a real highlight, which is what a seam actually is.
    { const float tDoor[3] = { 0.38f, 0.40f, 0.43f };
      m_eDoorL = addKitTex(scene, device, 0.06f, carH*0.42f, carD*0.24f, sPanel, tDoor, 0.5f);
      m_eDoorR = addKitTex(scene, device, 0.06f, carH*0.42f, carD*0.24f, sPanel, tDoor, 0.5f); }

    // ---- THE FLOOR INDICATOR. R11: was a flat glowing BAR (a box with emissive 1.4)
    // that changed colour. It told you nothing and it read as a neon strip. It is now a
    // real DISPLAY: the floor label is rastered into a texture with stb_truetype and the
    // plate glows PER TEXEL through the map, so the glyphs are light and the substrate
    // stays black glass (the club-OLED / rifthub-console move, f2d86bc).
    // FOOTGUN (cost us the club EQ once already): Scene::submit only forwards
    // Entity::emissiveTex on the mrTex.valid() PBR branch, so the plate MUST carry an MR
    // texel or the emissive map is silently dropped and you get a flat slab again.
    // B6: the indicator is the SAME baked-raster-on-a-box as the OLEDs, so it carries the
    // same bottom-up-V defect — flipV for the same reason (see flipMeshV()).
    { const float c[4] = {0.9f, 0.9f, 0.9f, 1.0f};
      const float em[4] = {1.0f, 1.0f, 1.0f, 1.6f};
      m_eIndicator = addKit(scene, device, 0.02f, 0.16f, carD*0.30f, c, em, /*flipV*/true);
      if (m_eIndicator != kNoLink && m_eIndicator < scene.size()) {
          Entity& e = scene.get(m_eIndicator);
          e.mrTex = m_oledMr;                       // the glossy panel texel (see above)
          m_indTex = bakeIndicator(device, floorLabel(m_curStop), "IDLE", 1.0f, 0.72f, 0.22f);
          e.tex = m_indTex; e.emissiveTex = m_indTex;
      } }

    // Point lights: [0] = ceiling interior fill, [1..4] = disco spots (off until
    // disco mode). The host pushes these via setPointLights; the disco cue animates
    // them in update().
    // R11 — THE KEY, honestly. The cab's ceiling fill was 0.60/0.73/0.87 while level1's
    // own fixtures run 3.6-4.2 and the rifthub's overheads 5.8: FIVE TIMES too dim. It
    // never mattered, because the 0.42 engine ambient was lighting the car for it — which
    // is the whole crutch in one line. With the ambient honest, the fixture has to
    // actually be a light: cool-white, bright, and short-ranged so it dies in the shaft
    // when the doors open (the cab is a lit box in a dark hole — that is the look).
    m_lights.clear();
    x3::rhi::PointLight ceil;
    ceil.color[0] = 3.10f; ceil.color[1] = 3.35f; ceil.color[2] = 3.80f;
    ceil.range = 7.5f;
    m_lights.push_back(ceil);
    const float spot[4][3] = {
        {1.0f, 0.2f, 0.4f}, {0.2f, 0.4f, 1.0f}, {0.2f, 1.0f, 0.4f}, {1.0f, 0.8f, 0.2f}
    };
    for (int i = 0; i < 4; ++i) {
        x3::rhi::PointLight l;
        l.color[0] = spot[i][0]; l.color[1] = spot[i][1]; l.color[2] = spot[i][2];
        l.range = 6.0f;
        m_lights.push_back(l);   // intensity baked into color; starts at 0 (disco off)
        for (int c = 0; c < 3; ++c) m_lights.back().color[c] = 0.0f;
    }

    m_visualsBuilt = true;
    layoutVisuals(scene);
    x3::logInfo("[elevator] visuals built: glass + strata + twin OLEDs + mirror + "
                "blue terminal/keypad + ceiling light + disco ball");
}

bool ElevatorSystem::applyCabAtmosphere(x3::rhi::IRenderDevice& device,
                                        const x3::phys::Vec3& feet) {
    if (!m_visualsBuilt) return false;
    // Aboard = standing on the cab OR in the doorway of it (the car is 3.8 m across;
    // playerRiding's window is exactly the "your weight is on this platform" test).
    const int want = playerRiding(feet) ? 1 : 0;
    if (want == m_cabAir) return false;         // only on the edge — no per-frame spam
    m_cabAir = want;
    if (want) {
        // INSIDE. The engine default ambient is {0.42,0.44,0.50} and nothing in level1 or
        // the canon facility ever changed it, so the cab interior — a sealed steel box —
        // was being flooded with omnidirectional light from a building it cannot see. The
        // fixture in the ceiling is the light in here. (Same floor the rifthub hall
        // settled on, 0.032; the cab is smaller and its practical is close, so it can
        // afford to be just as dark.)
        device.setAmbient(0.030f, 0.032f, 0.037f);
        device.setIblIntensity(0.22f);
    } else {
        // OUTSIDE. Hand the world back WHAT THE WORLD ACTUALLY RUNS AT — not the engine
        // defaults. B4/L6b/THE PATTERN: this used to hard-code {0.42, 0.44, 0.50} + IBL 1.0,
        // which meant the elevator RE-IMPOSED THE 0.42 WASH ON THE ENTIRE GAME. And because
        // m_cabAir starts at -1, `want == 0` on the very first frame is a CHANGE, so it fired
        // BEFORE THE PLAYER HAD EVER SEEN THE CAB — silently overwriting the host's honest
        // ambient in level1, the spire, the club, the perf shop and the show room. It is the
        // single reason those rooms still ran the wash after dfcb65d. The host owns the
        // world's air (setWorldAtmosphere); the elevator only owns the cab's.
        device.setAmbient(m_worldAmb[0], m_worldAmb[1], m_worldAmb[2]);
        device.setIblIntensity(m_worldIbl);
    }
    return true;
}

void ElevatorSystem::layoutVisuals(Scene& scene) {
    if (!m_visualsBuilt) return;
    // MUST match buildVisuals (the car is the size of the car — see the note there).
    const float carW = m_halfX * 2.0f, carD = m_halfZ * 2.0f;
    const float carH = 2.60f;
    const float T = 0.10f;
    const float cx = m_pos.x + m_shakeX, cz = m_pos.z;
    // The cab top is the car floor; the car interior rises +carH/2 above center.
    const float floorY = m_pos.y + m_halfY;
    const float midY = floorY + carH * 0.5f;

    auto place = [&](uint32_t id, float ox, float oy, float oz) {
        if (id == kNoLink || id >= scene.size()) return;
        Entity& e = scene.get(id);
        e.transform[12] = cx + ox;
        e.transform[13] = midY + oy;
        e.transform[14] = cz + oz;
    };
    // ---- THE SHELL (R11). Walls hug the car's faces; the +X face is the doorway, so it
    // gets two jambs flanking the opening + a header above it, never a slab across it.
    place(m_eWallBack,   0.0f,                   0.0f,          -carD * 0.5f);   // mirror wall
    place(m_eWallSide,   0.0f,                   0.0f,           carD * 0.5f);   // +Z wall
    place(m_eCabCeil,    0.0f,                   carH * 0.5f,    0.0f);
    place(m_eKick,       0.0f,                  -carH * 0.5f + 0.12f, 0.0f);
    {   // The +X doorway: two jambs OUTSIDE the doors' reach, a header above them.
        const float kDoorHalf = carD * 0.24f + carD * 0.25f * 0.5f;
        const float jambCZ    = kDoorHalf + (carD * 0.5f - kDoorHalf) * 0.5f;
        place(m_eWallFrontL, carW * 0.5f, 0.0f, -jambCZ);
        place(m_eWallFrontR, carW * 0.5f, 0.0f,  jambCZ);
        place(m_eHeader,     carW * 0.5f, 1.095f, 0.0f);  // door tops -> the ceiling slab
    }
    place(m_eGlass,     -carW * 0.5f + T,        0.0f,           0.0f);
    {   // The window frame: strips above / below / either side of the glass aperture.
        const float gh = carH * 0.40f, gd = carD * 0.42f;
        const float topH = (carH * 0.5f - gh) * 0.5f;
        const float sideD = (carD * 0.5f - gd) * 0.5f;
        const float wx = -carW * 0.5f;
        place(m_eWinTop, wx,  gh + topH,  0.0f);
        place(m_eWinBot, wx, -gh - topH,  0.0f);
        place(m_eWinL,   wx,  0.0f,      -gd - sideD);
        place(m_eWinR,   wx,  0.0f,       gd + sideD);
    }
    place(m_eStrata,    -carW * 0.5f - 0.30f,    0.0f,           0.0f);
    place(m_eMirror,     0.0f,                   0.15f,         -carD * 0.5f + T);
    place(m_eFixture,    0.0f,                   carH * 0.5f - 0.16f, 0.0f);
    place(m_eLensCore,   0.0f,                   carH * 0.5f - 0.22f, 0.0f);
    place(m_eOledL,     -0.9f,                   carH * 0.5f - 1.2f, carD * 0.5f - 0.18f);
    place(m_eOledR,      carW * 0.5f - 0.18f,    0.0f,          -carD * 0.25f);
    place(m_eTerm,       carW * 0.5f - 0.18f,   -0.30f,          carD * 0.25f);
    place(m_eGoldBtn,    carW * 0.5f - 0.16f,    0.08f,          carD * 0.25f);  // above the keypad
    place(m_eDiscoBall,  0.0f,                   carH * 0.5f - 0.75f, 0.0f);  // BELOW the key
    place(m_eCeil,       0.0f,                   carH * 0.5f - 0.1f, 0.0f);
    // Shaft cables: from the roof corners, 60 m half-height columns rising up.
    for (int ci = 0; ci < 4; ++ci) {
        const float sx = (ci & 1) ? 1.0f : -1.0f;
        const float sz = (ci & 2) ? 1.0f : -1.0f;
        place(m_eCable[ci], sx * carW * 0.30f, carH * 0.5f + 60.0f, sz * carD * 0.30f);
    }

    // Sliding doors on the +X wall: closed (doorPct=0) the two panels meet at z=0;
    // open (doorPct=1) they retract to +/- a quarter-depth. Each panel is a quarter
    // wide, so the slide distance is carD*0.25.
    {
        const float slide = m_doorPct * carD * 0.25f;
        const float doorX = carW * 0.5f - 0.06f;
        const float doorY = -0.2f;     // door slab spans the deck up to the header
        place(m_eDoorL, doorX, doorY, -carD * 0.25f * 0.5f - slide);
        place(m_eDoorR, doorX, doorY,  carD * 0.25f * 0.5f + slide);
    }
    // FLOOR INDICATOR — the display plate, inboard of the header so it faces the rider.
    place(m_eIndicator, carW * 0.5f - 0.05f, carH * 0.42f, 0.0f);
    // R11: the state still drives the COLOUR (green = safe to step out, amber = under
    // way, magenta = disco) — but it now colours the INK, not the slab. The plate carries
    // a baked text texture on emissiveMap duty, so only the GLYPHS are light and the
    // substrate behind them stays black glass. Change-gated on (stop, state, disco), so
    // it is a texture upload on transitions, not a per-frame bake.
    if (m_eIndicator != kNoLink && m_eIndicator < scene.size() && m_oledDevice) {
        const bool open = (m_state == ElevState::DoorsOpen) || (m_doorPct > 0.95f &&
                          m_state == ElevState::Idle);
        const bool moving = !open;
        if (m_curStop != m_indStop || m_disco != m_indDisco || moving != m_indMoving) {
            m_indStop = m_curStop; m_indDisco = m_disco; m_indMoving = moving;
            float r, g, b; const char* cap;
            if (m_disco)   { r = 1.00f; g = 0.14f; b = 0.85f; cap = "DISCO"; }
            else if (open) { r = 0.28f; g = 1.00f; b = 0.42f; cap = "DOORS OPEN"; }
            else           { r = 1.00f; g = 0.62f; b = 0.10f; cap = stateName(m_state); }
            x3::rhi::TextureHandle fresh =
                bakeIndicator(*m_oledDevice, floorLabel(m_curStop), cap, r, g, b);
            Entity& e = scene.get(m_eIndicator);
            e.tex = fresh; e.emissiveTex = fresh; e.mrTex = m_oledMr;
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.9f;
            e.emissive[0] = e.emissive[1] = e.emissive[2] = 1.0f;
            e.emissive[3] = 1.7f;
            if (m_indTex.valid()) m_oledDevice->destroyTexture(m_indTex);
            m_indTex = fresh;
        }
    }
    // T3 — the golden button's light rides the unlock state: dark ember until
    // code 4790, then HOT gold (0.05 -> 3.0). Color stays put; only the strength
    // moves, so the lit button reads as the same object waking up.
    if (m_eGoldBtn != kNoLink && m_eGoldBtn < scene.size()) {
        Entity& e = scene.get(m_eGoldBtn);
        e.emissive[0] = 1.0f; e.emissive[1] = 0.75f; e.emissive[2] = 0.18f;
        e.emissive[3] = m_unlocked ? 3.0f : 0.05f;
    }

    // Terminal glow: cool blue normally, MAGENTA while disco is live (JS parity).
    // R11: the housing is TEXTURED METAL now, and the glow strength is a fraction of what
    // it was (1.0 -> 0.28). It reads as a keypad with a lit face, not a lightbox. The
    // glow can never again eat the whole casing, because the casing is a material.
    if (m_eTerm != kNoLink && m_eTerm < scene.size()) {
        Entity& e = scene.get(m_eTerm);
        if (m_disco) { e.emissive[0] = 0.90f; e.emissive[1] = 0.08f; e.emissive[2] = 0.75f; }
        else         { e.emissive[0] = 0.10f; e.emissive[1] = 0.45f; e.emissive[2] = 0.90f; }
        e.emissive[3] = 0.28f;
    }

    // Position the point lights at the cab interior (ceiling), spots ringed.
    if (!m_lights.empty()) {
        // ===== THE BUG THAT KEPT THIS CAB IN THE DARK FOR ITS ENTIRE LIFE =====
        // The ceiling light was placed at `midY + carH*0.5 - 0.5`... and so was the DISCO
        // BALL, a SOLID 0.6 m box. The cab's only light source has been sitting INSIDE the
        // disco ball this whole time. With RT shadows on, the ball encloses the light and
        // the cab renders PITCH BLACK — which nobody ever saw, because the 0.42 engine
        // ambient was lighting the car instead. Kill the ambient crutch and the real bug
        // walks out into the open: the room went black, not dark.
        // (Proof: identical frame with `r_rtshadows 0` came back correctly lit.)
        // The key now hangs BELOW the fixture's lit core and ABOVE the (shrunken) ball,
        // with clear air both ways. A practical has to be able to SEE the room it lights.
        m_lights[0].pos[0] = cx; m_lights[0].pos[1] = midY + carH * 0.5f - 0.40f; m_lights[0].pos[2] = cz;
        for (int i = 1; i < (int)m_lights.size(); ++i) {
            const float a = (float)(i - 1) / 4.0f * 6.2831853f;
            m_lights[i].pos[0] = cx + std::cos(a) * 1.2f;
            m_lights[i].pos[1] = midY + 1.0f;
            m_lights[i].pos[2] = cz + std::sin(a) * 1.2f;
        }
    }

    // Disco-ball emissive: glows when disco mode is on, dark otherwise.
    if (m_eDiscoBall != kNoLink && m_eDiscoBall < scene.size()) {
        Entity& e = scene.get(m_eDiscoBall);
        const float g = m_disco ? 0.9f : 0.0f;
        e.emissive[0] = 0.8f * g; e.emissive[1] = 0.8f * g; e.emissive[2] = 0.9f * g;
        e.emissive[3] = m_disco ? 1.2f : 0.0f;
    }

    // Drive the strata-plane tint/glow from the cab's current stratum.
    if (m_eStrata != kNoLink && m_eStrata < scene.size()) {
        Entity& e = scene.get(m_eStrata);
        // T3 — ANNEX TRANSIT PANORAMA: on a LATERAL leg (|dir.y| < 0.3) there is
        // no geology rushing past vertically, so the display swaps its scroll
        // axis — the band slides through the strata palette by LEG PROGRESS
        // (scroll = m_s / m_segLen across the 9 layers) with a soft amber
        // transit glow, so the observation window reads as sideways MOTION.
        const bool lateralLeg = moving() && m_segLen > 1e-3f &&
                                std::fabs(m_segDir.y) < 0.3f && m_fsmSpeed > 0.05f;
        if (lateralLeg) {
            const float scroll = (m_s / m_segLen) * (float)(kStrata.size() - 1);
            int i0 = (int)scroll;
            if (i0 > (int)kStrata.size() - 1) i0 = (int)kStrata.size() - 1;
            const int i1 = (i0 + 1 <= (int)kStrata.size() - 1) ? i0 + 1 : i0;
            const float f = scroll - (float)i0;
            const float kRockValue = 0.42f;   // same VALUE renorm as the vertical path
            for (int c = 0; c < 3; ++c)
                e.baseColor[c] = (kStrata[i0].rgb[c] * (1.0f - f) +
                                  kStrata[i1].rgb[c] * f) * kRockValue;
            e.emissive[0] = 0.90f; e.emissive[1] = 0.65f; e.emissive[2] = 0.25f;
            e.emissive[3] = 0.50f + 0.30f * std::sin(m_s * 2.1f);
        }
        else for (const StrataLayer& s : kStrata) {
            if (m_pos.y >= s.yMin && m_pos.y <= s.yMax) {
                // VALUE, not lumens (recipe #3): the strata plane sits 1.4 m from the cab's
                // practical, so at its authored albedo (limestone = 0.55) it metered as the
                // BRIGHTEST THING IN THE CAB — a blown-out lightbox where a rock face
                // should be, dragging the whole car's exposure down with it. It is rock, in
                // a shaft, behind smoked glass. Renormalize.
                const float kRockValue = 0.42f;
                for (int c = 0; c < 3; ++c) e.baseColor[c] = s.rgb[c] * kRockValue;
                if (s.glow) {
                    // Crystal Veins / Magma / Alien Substrate: these ARE light sources.
                    for (int c = 0; c < 3; ++c) e.emissive[c] = s.glowRgb[c];
                    e.emissive[3] = 1.4f;
                } else {
                    // R11 — CRUTCH REMOVED: limestone, granite and basalt were self-lit
                    // at 0.5 strength in their OWN base colour, purely so they'd be
                    // visible through the glass. That is the recipe's crutch #1 (fake
                    // self-emissive on geometry) and it read as glow-in-the-dark rock.
                    // The strata wall is lit by the cab's practical spilling through the
                    // glass now — so the upper (dead) strata are DARK, which is exactly
                    // what makes the first glowing vein land when you descend into it.
                    e.emissive[0] = e.emissive[1] = e.emissive[2] = 0.0f;
                    e.emissive[3] = 0.0f;
                }
                break;
            }
        }
    }
}

void ElevatorSystem::applyDiscoCue(float /*dt*/, float t) {
    if (!m_visualsBuilt) return;
    // Disco-ball emissive + the spinning, pulsing colored spots (the disco cue).
    // The ceiling light dims to a dim purple while the 4 spots cycle hue/intensity,
    // and a 4 Hz STROBE (JS DISCO_STROBE_HZ) snaps the ceiling to hard white on a
    // short duty cycle — the flash IS the ceiling light, so no extra host wiring.
    // (The disco-ball entity emissive is driven in layoutVisuals() from m_disco.)
    if (!m_lights.empty()) {
        const bool strobeOn = std::fmod(t * 4.0f, 1.0f) < 0.12f;
        if (strobeOn) {
            m_lights[0].color[0] = 2.6f; m_lights[0].color[1] = 2.6f; m_lights[0].color[2] = 2.8f;
        } else {
            m_lights[0].color[0] = 0.20f; m_lights[0].color[1] = 0.05f; m_lights[0].color[2] = 0.40f;
        }
        const float baseSpot[4][3] = {
            {1.0f, 0.2f, 0.4f}, {0.2f, 0.4f, 1.0f}, {0.2f, 1.0f, 0.4f}, {1.0f, 0.8f, 0.2f}
        };
        for (int i = 1; i < (int)m_lights.size(); ++i) {
            const float pulse = 0.6f + 0.4f * std::sin(t * 3.0f + (float)i * 1.5f);
            for (int c = 0; c < 3; ++c) m_lights[i].color[c] = baseSpot[i - 1][c] * pulse;
        }
    }
}

} // namespace x3::game


// ===========================================================================
// Headless self-test (--test-elevatorfsm). Self-contained: uses the shared
// HeadlessRenderDevice + a fresh Jolt world; no window/Vulkan. Leak-clean.
// ===========================================================================
#include "headless_device.h"

namespace x3::game {

namespace {
int    s_pass = 0, s_fail = 0;
void   fcheck(bool cond, const char* name) {
    if (cond) { ++s_pass; x3::logInfo(std::string("  [PASS] ") + name); }
    else      { ++s_fail; x3::logError(std::string("  [FAIL] ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;
} // namespace

bool runElevatorFsmSelfTest() {
    s_pass = s_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessRenderDevice device;
    Scene scene;

    // A tall shaft: stop 0 at cab-center y=0.15 (room floor), stop 1 at +60 m up
    // (long enough that the cab actually reaches MAX_SPEED == CRUISING before the
    // decel window). Cab half-extents match the main wiring.
    const float cabHY = 0.15f;
    const float groundY = cabHY;            // cab CENTER at the room floor
    const float topY    = cabHY + 60.0f;    // cab CENTER 60 m up
    ElevatorSystem elev;
    bool built = elev.build(scene, device, *physics, 0.0f, 0.0f,
                            1.4f, cabHY, 1.4f, std::vector<float>{ groundY, topY }, 0);
    elev.enableFsm(true);
    elev.buildVisuals(scene, device);

    // ---- F1: builds, FSM enabled, idle at the ground stop, club stop = -200.
    fcheck(built && elev.built() && elev.fsmEnabled() &&
           elev.state() == ElevState::Idle &&
           std::fabs(elev.cabCenter().y - groundY) < 1e-3f,
           "F1 builds + FSM enabled, idle at ground stop");
    // Club descent target is Y=-200 floor + cab half-height (so the cab top sits
    // flush with the club floor at -200).
    fcheck(std::fabs(elev.clubStopY() - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 1e-3f,
           "F1b club stop Y = -200 (+ cab half-height)");

    // ---- F2: strata = 9 layers, named, with the bottom three glowing.
    {
        const auto& st = ElevatorSystem::strata();
        bool glows = st[6].glow && st[7].glow && st[8].glow && !st[0].glow;
        fcheck(st.size() == 9 &&
               std::string(st[0].name) == "Sky & Concrete" &&
               std::string(st[8].name) == "Alien Substrate" && glows,
               "F2 9 earth-strata layers, named, bottom-3 glow");
    }

    // ---- F3: a NORMAL ride up — drive the FSM and record the state sequence.
    // Expect IDLE -> (DOORS_CLOSING) -> ACCELERATING -> CRUISING -> DECELERATING
    // -> ARRIVING -> DOORS_OPENING -> DOORS_OPEN -> IDLE, the cab reaches topY, and
    // the speed ramps up to (clamps at) MAX_SPEED then ramps back down.
    {
        elev.callTo(1);   // call to the top stop
        bool sawClosing = false, sawAccel = false, sawCruise = false, sawDecel = false,
             sawArriving = false, sawOpening = false, sawOpen = false, backIdle = false;
        bool clampedMax = false, rampedDown = false;
        bool cruiseBeforeDecel = false;
        float maxSeen = 0.0f;
        ElevState prev = elev.state();
        float feetY = elev.cabTopY() + 0.05f;   // a simulated rider on the cab top
        float carried = 0.0f;

        for (int i = 0; i < 4000; ++i) {
            float edy = elev.update(kDt, scene, *physics);
            if (elev.playerRiding(x3::phys::Vec3{0.0f, feetY, 0.0f})) {
                feetY += edy; carried += edy;
            }
            ElevState s = elev.state();
            switch (s) {
                case ElevState::DoorsClosing: sawClosing = true; break;
                case ElevState::Accelerating: sawAccel = true; break;
                case ElevState::Cruising:     sawCruise = true; if (!sawDecel) cruiseBeforeDecel = true; break;
                case ElevState::Decelerating: sawDecel = true; break;
                case ElevState::Arriving:     sawArriving = true; break;
                case ElevState::DoorsOpening: sawOpening = true; break;
                case ElevState::DoorsOpen:    sawOpen = true; break;
                default: break;
            }
            // Track the FSM speed via the carried delta magnitude per frame.
            float frameSpeed = std::fabs(edy) / kDt;
            if (frameSpeed > maxSeen) maxSeen = frameSpeed;
            // Speed clamps at MAX_SPEED (within a frame's accel step tolerance).
            if (frameSpeed > elev.tuning().maxSpeed - 0.5f &&
                frameSpeed <= elev.tuning().maxSpeed + 0.5f) clampedMax = true;
            // Detect ramp-down: a slower frame after the max while decelerating.
            if (s == ElevState::Decelerating && frameSpeed < maxSeen - 1.0f) rampedDown = true;
            prev = s;
            if (s == ElevState::Idle && (sawArriving || sawOpen)) { backIdle = true; break; }
        }
        (void)prev;

        bool reached = std::fabs(elev.cabCenter().y - topY) < 0.05f;
        bool seq = sawClosing && sawAccel && sawCruise && sawDecel && sawArriving &&
                   sawOpening && sawOpen && backIdle;
        fcheck(seq, "F3 ride state sequence IDLE->CLOSING->ACCEL->CRUISE->DECEL->ARRIVING->DOORS->IDLE");
        fcheck(reached, "F3b cab reaches the target floor Y (60 m up)");
        fcheck(cruiseBeforeDecel && clampedMax && maxSeen <= elev.tuning().maxSpeed + 0.5f,
               "F3c speed ramps up + CLAMPS at MAX_SPEED (~14) before decel");
        fcheck(rampedDown, "F3d speed ramps DOWN during DECELERATING");
        fcheck(std::fabs(carried - (topY - groundY)) < 0.25f,
               "F3e rider carried up by the full ride height");
    }

    // ---- F4: the 1127 keypad code triggers DISCO + a descent to Y=-200.
    {
        ElevatorSystem e2;
        e2.build(scene, device, *physics, 10.0f, 10.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 1);   // start at the TOP stop
        e2.enableFsm(true);
        // Wrong code first: 9-9-9-9 must NOT enable disco.
        e2.keypadDigit(9); e2.keypadDigit(9); e2.keypadDigit(9);
        bool notYet = !e2.disco();
        bool wrong = e2.keypadDigit(9);
        fcheck(notYet && !wrong && !e2.disco(), "F4 wrong code (9999) does NOT enable disco");

        // Right code: 1-1-2-7 enables disco; the 4th digit returns true.
        e2.keypadDigit(1); e2.keypadDigit(1); e2.keypadDigit(2);
        bool completed = e2.keypadDigit(7);
        bool descending = (e2.state() == ElevState::DoorsClosing ||
                           e2.state() == ElevState::Accelerating ||
                           e2.state() == ElevState::Cruising ||
                           e2.state() == ElevState::Decelerating);
        fcheck(completed && e2.disco() && descending,
               "F4b code 1127 enables DISCO + starts a descent");
        fcheck(std::fabs(e2.clubStopY() - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 1e-3f,
               "F4c disco descent target is Club 1127 at Y=-200");

        // Drive it down and assert it actually reaches the club stop.
        for (int i = 0; i < 20000 && e2.state() != ElevState::DoorsOpen &&
                        e2.state() != ElevState::Idle; ++i)
            e2.update(kDt, scene, *physics);
        fcheck(std::fabs(e2.cabCenter().y - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 0.1f,
               "F4d cab descends all the way to the Club 1127 stop (Y=-200)");
    }

    // ---- F4.5: the 4455 code (taught by the chief engineer's log on F4) unlocks
    // the hidden half-floor stop + rides the cab to it; a wrong code does neither.
    {
        const float midY = cabHY + 30.0f;   // the hidden stop, mid-shaft
        ElevatorSystem e45;
        e45.build(scene, device, *physics, 30.0f, 30.0f, 1.4f, cabHY, 1.4f,
                  std::vector<float>{ groundY, midY, topY }, 0);   // start at ground
        e45.enableFsm(true);
        e45.setSecretStop(1);
        fcheck(e45.stopLocked(1) && !e45.secretUnlocked(),
               "F4.5a hidden stop starts LOCKED (dead directory row)");
        // NEGATIVE CONTROL: 4454 must not unlock, must not move the cab.
        e45.keypadDigit(4); e45.keypadDigit(4); e45.keypadDigit(5);
        bool wrongDone = e45.keypadDigit(4);
        fcheck(!wrongDone && !e45.secretUnlocked() && e45.stopLocked(1) &&
               e45.state() == ElevState::Idle,
               "F4.5b wrong code 4454 does NOT unlock the hidden stop");
        // The real code: 4-4-5-5 — "double the four, double the five".
        e45.keypadDigit(4); e45.keypadDigit(4); e45.keypadDigit(5);
        bool rightDone = e45.keypadDigit(5);
        fcheck(rightDone && e45.secretUnlocked() && !e45.stopLocked(1),
               "F4.5c code 4455 unlocks the hidden stop (one-way, on the panel now)");
        for (int i = 0; i < 20000 && e45.state() != ElevState::DoorsOpen &&
                        !(e45.state() == ElevState::Idle && i > 10); ++i)
            e45.update(kDt, scene, *physics);
        fcheck(std::fabs(e45.cabCenter().y - midY) < 0.1f,
               "F4.5d cab rides to the 4.5 stop after the code");

        // The owner's MASTER BACKUP (7762) also unlocks the 4.5 stop; the
        // off-by-one 7761 does not (negative control).
        ElevatorSystem eMk;
        eMk.build(scene, device, *physics, 40.0f, 40.0f, 1.4f, cabHY, 1.4f,
                  std::vector<float>{ groundY, midY, topY }, 0);
        eMk.enableFsm(true);
        eMk.setSecretStop(1);
        eMk.keypadDigit(7); eMk.keypadDigit(7); eMk.keypadDigit(6);
        bool nearMiss = eMk.keypadDigit(1);
        fcheck(!nearMiss && !eMk.secretUnlocked() && eMk.stopLocked(1),
               "F4.5e wrong code 7761 does NOT unlock the hidden stop");
        eMk.keypadDigit(7); eMk.keypadDigit(7); eMk.keypadDigit(6);
        bool masterDone = eMk.keypadDigit(2);
        fcheck(masterDone && eMk.secretUnlocked() && !eMk.stopLocked(1),
               "F4.5f master backup 7762 unlocks the hidden stop (owner key)");
    }

    // ---- F5: FREEFALL is reachable + drops the cab.
    {
        ElevatorSystem e3;
        e3.build(scene, device, *physics, 20.0f, 20.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 1);
        e3.enableFsm(true);
        float before = e3.cabCenter().y;
        e3.freefall();
        bool inState = e3.state() == ElevState::Freefall;
        for (int i = 0; i < 30; ++i) e3.update(kDt, scene, *physics);
        bool dropped = e3.cabCenter().y < before - 1.0f;
        fcheck(inState && dropped, "F5 FREEFALL reachable + drops the cab");
    }

    // ---- F6: EMERGENCY_STOP is reachable, halts, shakes, then recovers to IDLE.
    {
        ElevatorSystem e4;
        e4.build(scene, device, *physics, 30.0f, 30.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 0);
        e4.enableFsm(true);
        e4.callTo(1);
        for (int i = 0; i < 20; ++i) e4.update(kDt, scene, *physics);  // get it moving
        e4.emergencyStop();
        bool inState = e4.state() == ElevState::EmergencyStop;
        // Run past the 3 s shake window -> recovers to IDLE.
        for (int i = 0; i < 250; ++i) e4.update(kDt, scene, *physics);
        bool recovered = e4.state() == ElevState::Idle;
        fcheck(inState && recovered, "F6 EMERGENCY_STOP reachable, halts + recovers to IDLE");
    }

    // ---- F7: state-name table is complete (all 10 states named, distinct).
    {
        const ElevState all[] = {
            ElevState::Idle, ElevState::Accelerating, ElevState::Cruising,
            ElevState::Decelerating, ElevState::Arriving, ElevState::DoorsOpening,
            ElevState::DoorsOpen, ElevState::DoorsClosing, ElevState::EmergencyStop,
            ElevState::Freefall };
        bool ok = true;
        for (const ElevState s : all) {
            const char* n = ElevatorSystem::stateName(s);
            if (!n || n[0] == '?' || n[0] == '\0') ok = false;
        }
        fcheck(ok, "F7 all 10 FSM states have names");
    }

    // ---- F8: the legacy core path is UNTOUCHED (FSM off => linear snap arrive),
    // so --test-elevator stays green. Build a fresh non-FSM cab and ride it.
    {
        ElevatorSystem e5;
        e5.build(scene, device, *physics, 40.0f, 40.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, cabHY + 6.0f }, 0);
        e5.setSpeed(8.0f);
        e5.callNext();
        bool startedMoving = e5.moving() && e5.targetStop() == 1;
        for (int i = 0; i < 600 && e5.moving(); ++i) e5.update(kDt, scene, *physics);
        bool arrived = !e5.moving() && std::fabs(e5.cabCenter().y - (cabHY + 6.0f)) < 1e-3f;
        fcheck(startedMoving && arrived, "F8 legacy non-FSM linear lift still arrives (core unchanged)");
    }

    physics->shutdown();
    // ---- F9: THE CABLE SLIPS (armed) — on a long descent past halfway the cab
    // freefalls, the brakes CATCH (EmergencyStop), and the ride RESUMES to the
    // original stop; the scare is once-only and never drops through the pit. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p9(x3::phys::createPhysicsWorld());
        p9->init();
        HeadlessRenderDevice d9;
        Scene s9;
        ElevatorSystem e9;
        const float bot = 0.15f, top = 60.15f;
        e9.build(s9, d9, *p9, 0.0f, 0.0f, 1.4f, 0.15f, 1.4f,
                 std::vector<float>{ bot, top }, 1);       // start at the TOP
        e9.enableFsm(true);
        e9.armCableSlip();
        e9.callTo(0);                                      // the long descent
        bool sawFall = false, sawEmergency = false, minYOk = true;
        float minY = 1e9f;
        for (int i = 0; i < 60 * 60; ++i) {                // up to 60 sim-seconds
            e9.update(kDt, s9, *p9);
            sawFall      |= (e9.state() == ElevState::Freefall);
            sawEmergency |= (e9.state() == ElevState::EmergencyStop);
            minY = std::min(minY, e9.cabCenter().y);
            if (e9.state() == ElevState::DoorsOpen && sawEmergency) break;
        }
        minYOk = (minY > bot - 1.0f);
        const bool arrived = std::fabs(e9.cabCenter().y - bot) < 0.5f;
        fcheck(sawFall && sawEmergency && arrived && minYOk,
               "F9 CABLE SLIP: freefall -> brakes catch -> resumes -> arrives (never through the pit)");
        // Once-only: ride back up, then down again — no second scare.
        e9.callTo(1);
        for (int i = 0; i < 60 * 40 && e9.state() != ElevState::DoorsOpen; ++i) e9.update(kDt, s9, *p9);
        e9.callTo(0);
        bool secondFall = false;
        for (int i = 0; i < 60 * 40; ++i) {
            e9.update(kDt, s9, *p9);
            secondFall |= (e9.state() == ElevState::Freefall);
            if (e9.state() == ElevState::DoorsOpen) break;
        }
        fcheck(!secondFall, "F9b the slip is once-only (second descent rides clean)");
        p9->shutdown();
    }

    // ---- F10: rider craft — an idle SEALED car auto-opens for NEAR feet only.
    // (Doors are closed at Idle after a manual emergency stop mid-shaft.)
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p10(x3::phys::createPhysicsWorld());
        p10->init();
        Scene s10;
        ElevatorSystem e10;
        e10.build(s10, device, *p10, 0.0f, 0.0f, 1.4f, cabHY, 1.4f,
                  std::vector<float>{ groundY, topY }, 0);
        e10.enableFsm(true);
        e10.callTo(1);
        for (int i = 0; i < 60 * 3 && e10.state() != ElevState::Cruising; ++i) e10.update(kDt, s10, *p10);
        e10.emergencyStop();                    // halts mid-shaft, doors still sealed
        for (int i = 0; i < 60 * 5 && e10.state() != ElevState::Idle; ++i) e10.update(kDt, s10, *p10);
        const bool sealedIdle = (e10.state() == ElevState::Idle) && (e10.doorPct() < 0.05f);
        const x3::phys::Vec3 feetFar{ e10.cabCenter().x + 30.0f,
                                      e10.cabCenter().y + cabHY, e10.cabCenter().z };
        e10.autoOpenFor(feetFar);
        const bool stayedShut = (e10.state() == ElevState::Idle);
        const x3::phys::Vec3 feetNear{ e10.cabCenter().x + 2.0f,
                                       e10.cabCenter().y + cabHY, e10.cabCenter().z };
        e10.autoOpenFor(feetNear);
        const bool opening = (e10.state() == ElevState::DoorsOpening);
        fcheck(sealedIdle && stayedShut && opening,
               "F10 rider craft: idle sealed car auto-opens only for NEAR feet");
        p10->shutdown();
    }

    // ---- F11 (R11 art gate): THE FLOOR INDICATOR IS A DISPLAY, NOT A GLOWING BAR.
    // Two things must hold, and they are different assertions:
    //   (a) the plate SAYS SOMETHING — the baked readout carries real ink, and the probe
    //       is capable of failing (the negative control: a blank bake reads ~0), and
    //   (b) the plate is WIRED for per-texel emissive — Entity::emissiveTex is bound AND
    //       so is an MR texel, because Scene::submit only forwards emissiveTex on the
    //       mrTex.valid() PBR branch. Drop the MR map and the emissive map is SILENTLY
    //       ignored and you are back to a flat lit slab (this is exactly how the club's
    //       back-bar EQ shipped as a milky white rectangle).
    {
        const float inkIdle  = elevatorIndicatorInkFraction("B1", "IDLE");
        const float inkOpen  = elevatorIndicatorInkFraction("F7", "DOORS OPEN");
        const float inkBlank = elevatorIndicatorInkFraction("", "");     // negative control
        fcheck(inkIdle > 0.01f && inkOpen > 0.01f && inkBlank < 0.002f,
               "F11 floor indicator BAKES REAL INK (with a negative control)");

        Scene s11; std::unique_ptr<x3::phys::IPhysicsWorld> p11(x3::phys::createPhysicsWorld());
        p11->init();
        HeadlessRenderDevice d11;
        ElevatorSystem e11;
        e11.build(s11, d11, *p11, 0.0f, 0.0f, 1.4f, cabHY, 1.4f,
                  std::vector<float>{ groundY, topY }, 0);
        e11.enableFsm(true);
        e11.setFloorLabels(std::vector<std::string>{ "B1", "F7" });
        e11.buildVisuals(s11, d11);
        e11.update(kDt, s11, *p11);          // one tick: the layout bakes the plate
        bool wired = false, coreLit = false, strataHonest = true;
        for (const Entity& en : s11.entities()) {
            if (en.emissiveTex.valid() && en.mrTex.valid() && en.emissive[3] > 0.5f)
                wired = true;                // the indicator/OLED per-texel route is live
            if (en.emissive[3] > 1.5f) coreLit = true;   // the practical's lit core
        }
        fcheck(wired, "F11b indicator/OLED carry emissiveTex AND an MR texel (submit footgun)");
        fcheck(coreLit && strataHonest,
               "F11c the cab has a hot lit core (the practical is a fixture, not a wall)");
        p11->shutdown();
    }

    x3::logInfo("elevatorfsm: " + std::to_string(s_pass) + "/" +
                std::to_string(s_pass + s_fail) + " passed");
    return s_fail == 0;
}

} // namespace x3::game
