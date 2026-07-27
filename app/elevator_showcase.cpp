// THE CENTERPIECE — self-contained THICK / NICE / DARK-GLASS elevator showcase.
// See app/elevator_showcase.h for the design + decoupling contract.
#include "elevator_showcase.h"
#include "elevator_mesh.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/audio/IAudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Small carrier for an authored prim (render mesh + collision) we pass into the
// add* helpers. Keeps the helpers free of x3::prims/elevmesh template noise.
// ---------------------------------------------------------------------------
struct ElevPrim { x3::prims::PrimMesh mesh; };

namespace {
// Premium dark-luxury palette (linear-ish). Tim: THICK / NICE / DARK GLASS.
const float kBrushedSteel[4] = { 0.42f, 0.44f, 0.48f, 1.0f };   // frame / jambs (brushed)
const float kDarkSteel[4]    = { 0.16f, 0.17f, 0.20f, 1.0f };   // heavy structure
const float kDoorMetal[4]    = { 0.30f, 0.32f, 0.36f, 1.0f };   // sliding door slabs
const float kCabFloor[4]     = { 0.10f, 0.10f, 0.12f, 1.0f };   // dark cab deck
const float kDarkGlassTint[3]= { 0.10f, 0.12f, 0.16f };         // SMOKED dark glass tint
const float kNoEm[4]         = { 0, 0, 0, 0 };
// Accent strip glow (cool premium teal-white), holo cyan, warm vent amber.
const float kAccentEm[4]     = { 0.10f, 0.55f, 0.70f, 1.5f }; // gamma walk-back (was 2.2)
const float kWarmEm[4]       = { 0.95f, 0.78f, 0.45f, 1.4f };

constexpr float kPi2 = 6.2831853f;
constexpr float kPi  = 3.14159265f;

// ---- THE BEAT ---------------------------------------------------------------------
// ONE clock for the whole set piece. The elevator's own disco track (clubTrack, the
// loop the FSM starts on 1127) runs at the club tempo; the visualizer, the MV cuts,
// the PA cones and the concert-wash lights all derive from this so the cab pulses as a
// single instrument. (Matches the club lane's "ONE clock" doctrine — no free-running
// per-element rates.)
constexpr float kBeatBpm = 128.0f;                 // elevator disco-track tempo

// HSV -> linear-ish RGB (h,s,v in 0..1). Cheap 6-sector conversion; used to paint the
// Sphere's flowing colour fields and the concert wash without a palette table.
inline void hsv2rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default:r = v; g = p; b = q; break;
    }
}

// ===================================================================================
// MUSIC-VIDEO FRAME STRIP — procedural "MV playing on glass" content (goal #2).
// We bake a small strip of RGBA frames ONCE at build and flip through them on the beat
// in animateShow(), assigning the current frame as the panel's emissiveTex. Real video
// decode isn't required; a beat-cut strip of dancer silhouettes / equalizer / lyric
// cards / a scrolling cityscape reads unmistakably as a music video on the pane.
// ===================================================================================
constexpr int kMvW = 160, kMvH = 112;

inline void mvPx(std::vector<uint8_t>& px, int x, int y, float r, float g, float b) {
    if (x < 0 || x >= kMvW || y < 0 || y >= kMvH) return;
    uint8_t* p = &px[((size_t)y * kMvW + x) * 4];
    p[0] = (uint8_t)std::min(255.0f, r * 255.0f);
    p[1] = (uint8_t)std::min(255.0f, g * 255.0f);
    p[2] = (uint8_t)std::min(255.0f, b * 255.0f);
    p[3] = 255;
}
inline void mvRect(std::vector<uint8_t>& px, int x0, int y0, int x1, int y1,
                   float r, float g, float b) {
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) mvPx(px, x, y, r, g, b);
}
// A thick line (for silhouette limbs), integer Bresenham-ish with radius.
inline void mvLimb(std::vector<uint8_t>& px, float x0, float y0, float x1, float y1,
                   float rad, float r, float g, float b) {
    const int steps = (int)(std::fabs(x1 - x0) + std::fabs(y1 - y0)) + 1;
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / (float)steps;
        const float cx = x0 + (x1 - x0) * t, cy = y0 + (y1 - y0) * t;
        for (int dy = -(int)rad; dy <= (int)rad; ++dy)
            for (int dx = -(int)rad; dx <= (int)rad; ++dx)
                if (dx*dx + dy*dy <= (int)(rad*rad) + 1)
                    mvPx(px, (int)cx + dx, (int)cy + dy, r, g, b);
    }
}

// A concert-silhouette frame: a bright stage gradient + spotlight cones, a performer
// figure in near-black, in one of 4 dance poses. Reads as "singer on stage" MV.
inline std::vector<uint8_t> mvDancer(int pose, float hue) {
    std::vector<uint8_t> px((size_t)kMvW * kMvH * 4, 0);
    // Stage backdrop: a warm-to-hue vertical wash, brighter at the top (the lights).
    for (int y = 0; y < kMvH; ++y) {
        const float v = 1.0f - (float)y / kMvH;      // 1 at top
        float r, g, b; hsv2rgb(hue, 0.65f, 0.35f + 0.55f * v, r, g, b);
        for (int x = 0; x < kMvW; ++x) mvPx(px, x, y, r, g, b);
    }
    // Two spotlight cones splaying from the top toward the stage.
    for (int s = 0; s < 2; ++s) {
        const float ox = s ? kMvW * 0.72f : kMvW * 0.28f;
        for (int y = 0; y < kMvH; ++y) {
            const float spread = 3.0f + y * 0.45f;
            const int cxc = (int)(ox + (s ? 1 : -1) * y * 0.18f);
            for (int x = cxc - (int)spread; x <= cxc + (int)spread; ++x) {
                uint8_t* p = (x>=0&&x<kMvW) ? &px[((size_t)y*kMvW+x)*4] : nullptr;
                if (p) { p[0]=(uint8_t)std::min(255,p[0]+70); p[1]=(uint8_t)std::min(255,p[1]+70); p[2]=(uint8_t)std::min(255,p[2]+55); }
            }
        }
    }
    // The performer silhouette (near-black), centred, feet at the stage line.
    const float cx = kMvW * 0.5f, feet = kMvH * 0.92f, hip = kMvH * 0.60f, sh = kMvH * 0.36f;
    const float sr = 0.02f, sg = 0.02f, sb = 0.05f;             // near-black limbs
    mvLimb(px, cx, hip, cx, sh, 5.0f, sr, sg, sb);              // torso
    mvLimb(px, cx, sh - 6, cx, sh - 14, 6.0f, sr, sg, sb);      // head
    // Legs + arms vary by pose (the "dance").
    switch (pose & 3) {
        case 0: // arms up (both)
            mvLimb(px, cx, sh, cx - 14, sh - 16, 3.0f, sr, sg, sb);
            mvLimb(px, cx, sh, cx + 14, sh - 16, 3.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx - 8, feet, 4.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx + 8, feet, 4.0f, sr, sg, sb);
            break;
        case 1: // arms out (T)
            mvLimb(px, cx, sh + 4, cx - 20, sh + 2, 3.0f, sr, sg, sb);
            mvLimb(px, cx, sh + 4, cx + 20, sh + 2, 3.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx - 10, feet, 4.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx + 10, feet, 4.0f, sr, sg, sb);
            break;
        case 2: // lean, one arm up
            mvLimb(px, cx, sh + 4, cx - 18, sh - 14, 3.0f, sr, sg, sb);
            mvLimb(px, cx, sh + 4, cx + 12, sh + 12, 3.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx - 4, feet, 4.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx + 14, feet - 6, 4.0f, sr, sg, sb);
            break;
        default: // crouch / wide stance
            mvLimb(px, cx, sh + 4, cx - 12, sh + 16, 3.0f, sr, sg, sb);
            mvLimb(px, cx, sh + 4, cx + 12, sh + 16, 3.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx - 16, feet, 4.0f, sr, sg, sb);
            mvLimb(px, cx, hip, cx + 16, feet, 4.0f, sr, sg, sb);
            break;
    }
    return px;
}
// A neon equalizer frame: vertical bars from the floor, varying heights, hue ramp.
inline std::vector<uint8_t> mvEqualizer(float seed) {
    std::vector<uint8_t> px((size_t)kMvW * kMvH * 4, 0);
    mvRect(px, 0, 0, kMvW - 1, kMvH - 1, 0.02f, 0.02f, 0.05f);
    const int nbars = 16, bw = kMvW / nbars;
    for (int i = 0; i < nbars; ++i) {
        const float h = 0.35f + 0.6f * std::fabs(std::sin(seed * 3.1f + i * 0.9f));
        const int top = (int)(kMvH * (1.0f - h));
        float r, g, b; hsv2rgb(0.55f + 0.5f * (float)i / nbars, 0.85f, 1.0f, r, g, b);
        mvRect(px, i * bw + 1, top, i * bw + bw - 1, kMvH - 2, r, g, b);
    }
    return px;
}
// A "lyric card": dark frame + two/three bright text-like bars (karaoke read).
inline std::vector<uint8_t> mvLyric(int line, float hue) {
    std::vector<uint8_t> px((size_t)kMvW * kMvH * 4, 0);
    mvRect(px, 0, 0, kMvW - 1, kMvH - 1, 0.02f, 0.02f, 0.04f);
    float r, g, b; hsv2rgb(hue, 0.3f, 1.0f, r, g, b);
    // 3 stacked "text" rows built from short bright ticks (words).
    const int rows[3] = { 30, 55, 80 };
    const int nwords[3] = { 4, 3, 5 };
    for (int rrow = 0; rrow < 3; ++rrow) {
        const bool hot = (rrow == (line % 3));         // the "current line" glows brighter
        float rr = r, gg = g, bb = b;
        if (!hot) { rr *= 0.4f; gg *= 0.4f; bb *= 0.45f; }
        int x = 14;
        for (int w = 0; w < nwords[rrow]; ++w) {
            const int wl = 14 + ((w * 7 + rrow * 5) % 22);
            mvRect(px, x, rows[rrow], x + wl, rows[rrow] + 8, rr, gg, bb);
            x += wl + 8;
            if (x > kMvW - 20) break;
        }
    }
    return px;
}
// A scrolling neon cityscape silhouette (an MV "scene" shot).
inline std::vector<uint8_t> mvCity(float scroll) {
    std::vector<uint8_t> px((size_t)kMvW * kMvH * 4, 0);
    for (int y = 0; y < kMvH; ++y) {                   // dusk sky gradient
        const float v = (float)y / kMvH;
        float r, g, b; hsv2rgb(0.72f - 0.12f * v, 0.7f, 0.25f + 0.4f * v, r, g, b);
        for (int x = 0; x < kMvW; ++x) mvPx(px, x, y, r, g, b);
    }
    const int base = (int)(kMvH * 0.9f);
    for (int x = 0; x < kMvW; ++x) {                   // building blocks (silhouette + lit windows)
        const float s = std::sin((x + scroll) * 0.20f) + 0.6f * std::sin((x + scroll) * 0.07f);
        const int top = base - (int)(18 + 22 * (0.5f + 0.5f * s));
        for (int y = top; y < kMvH; ++y) mvPx(px, x, y, 0.03f, 0.03f, 0.07f);
        if ((x % 5) == 0)                               // lit windows
            for (int y = top + 3; y < base; y += 6)
                mvPx(px, x, y, 0.9f, 0.8f, 0.4f);
    }
    return px;
}
} // namespace

// ===========================================================================
// ADD HELPERS
// ===========================================================================
uint32_t ElevatorShowcase::addSolid(Scene& scene, x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& physics,
                                    const ElevPrim& prim, const float color[4],
                                    const float emissive[4], bool collide, uint32_t tag) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    if (collide && !prim.mesh.cverts.empty()) {
        e.body = physics.addStaticMesh(prim.mesh.cverts.data(), (uint32_t)(prim.mesh.cverts.size()/3),
                                       prim.mesh.cindex.data(), (uint32_t)prim.mesh.cindex.size());
    }
    e.tag = tag;
    uint32_t id = scene.add(e);
    if (e.body.id) scene.get(id).body = e.body;
    ++m_stats.entities;
    return id;
}

uint32_t ElevatorShowcase::addDecor(Scene& scene, x3::rhi::IRenderDevice& device,
                                    const ElevPrim& prim, const float color[4],
                                    const float emissive[4], uint32_t tag) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = tag;
    e.body.id = 0;
    ++m_stats.entities;
    return scene.add(e);
}

uint32_t ElevatorShowcase::addDarkGlass(Scene& scene, x3::rhi::IRenderDevice& device,
                                        const ElevPrim& prim, float opacity,
                                        const float tint[3], const float emissive[4]) {
    Entity e;
    e.mesh = device.createMesh(prim.mesh.verts.data(), (uint32_t)prim.mesh.verts.size(),
                               prim.mesh.index.data(), (uint32_t)prim.mesh.index.size());
    // Body color carries the dark tint too (so even with the scene-copy path off it
    // reads smoked, not clear). emissive honored for any glow baked behind the glass.
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = opacity;
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.transparent = true;
    e.glass.opacity    = opacity;        // DARK but still see-through
    e.glass.refraction = 0.025f;         // subtle bend (premium thick glass)
    e.glass.roughness  = 0.06f;          // near-polished, faint smoke
    e.glass.specular   = 0.9f;           // crisp reflections off the dark surface
    e.glass.tint[0] = tint[0]; e.glass.tint[1] = tint[1]; e.glass.tint[2] = tint[2];
    e.tag = (uint32_t)Tag::Prop;
    e.body.id = 0;
    ++m_stats.entities;
    return scene.add(e);
}

// ===========================================================================
// BUILD
// ===========================================================================
bool ElevatorShowcase::build(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics,
                             const PlacementSpec& spec, x3::audio::IAudioSystem* audio) {
    if (m_built) return true;
    m_spec = spec;
    m_audio = audio;
    m_shaftX = spec.shaftX;
    m_shaftZ = spec.shaftZ;

    // ---- Floor list: use the caller's, or synthesize a default showcase tower ----
    m_floors = spec.floors;
    if (m_floors.empty()) {
        // A premium tower: Club at the bottom (-200), then 6 above-ground floors.
        m_floors = {
            { ElevatorSystem::kDefaultClubFloorY + m_cabHY, "CLUB 1127  \xC2\xB7  THE DEEP", true },
            { m_cabHY + 0.0f,   "LOBBY  \xC2\xB7  GROUND",      false },
            { m_cabHY + 16.0f,  "F2  \xC2\xB7  DETENTION",      false },
            { m_cabHY + 32.0f,  "F3  \xC2\xB7  RESEARCH",       false },
            { m_cabHY + 48.0f,  "F4  \xC2\xB7  STRATA DECK",    false },
            { m_cabHY + 64.0f,  "F5  \xC2\xB7  THE CHORUS",     false },
            { m_cabHY + 80.0f,  "F6  \xC2\xB7  EXEC / SPIRE",   false },
        };
    }
    std::sort(m_floors.begin(), m_floors.end(),
              [](const ShowcaseFloor& a, const ShowcaseFloor& b){ return a.centerY < b.centerY; });

    std::vector<float> stopsY;
    std::vector<std::string> labels;
    m_clubStop = -1;
    for (int i = 0; i < (int)m_floors.size(); ++i) {
        stopsY.push_back(m_floors[i].centerY);
        labels.push_back(m_floors[i].label);
        if (m_floors[i].isClub) m_clubStop = i;
    }
    m_stats.floors = (int)m_floors.size();
    m_stats.hasClubStop = (m_clubStop >= 0);

    // Start at the lobby (first non-club stop) so the cab is at the human entrance.
    int start = spec.startStop;
    if (start <= 0) { start = (m_clubStop == 0 && m_floors.size() > 1) ? 1 : 0; }

    // ---- The core elevator (THICK cab platform) + FSM ----
    if (!m_elev.build(scene, device, physics, m_shaftX, m_shaftZ,
                      m_cabHX, m_cabHY, m_cabHZ, stopsY, start)) {
        x3::logError("[showcase] elevator core build failed");
        return false;
    }
    m_elev.enableFsm(true);
    m_elev.setAudio(audio);
    m_elev.setFloorLabels(labels);
    if (m_clubStop >= 0) m_elev.setClubStopY(m_floors[m_clubStop].centerY);
    m_stats.clubStopY = m_clubStop >= 0 ? m_floors[m_clubStop].centerY : 0.0f;

    // ---- The shell (shaft + per-floor doors + call panels) ----
    if (spec.buildShaftShell) {
        buildShaft(scene, device, physics);
        buildStrataLiner(scene, device);   // the descent's glowing geology (goal #3)
    }

    // ---- A SAVORABLE RIDE (Tim's north star: "an elevator you don't want to get off").
    //      Gentle ramps + a long doorHold so the descent LINGERS and the doors never rush
    //      you out — you get time to be wowed. (Only this showcase cab; the FSM self-test
    //      builds its own default-tuned ElevatorSystem, so it is unaffected.) ----
    { ElevTuning& tune = m_elev.tuning();
      tune.maxSpeed = 7.0f; tune.accel = 3.0f; tune.decel = 4.0f; tune.doorHold = 6.0f; }

    // ---- The thick dark-glass cab interior + holo control panel ----
    buildCabInterior(scene, device);
    buildHoloPanel(scene, device);

    // ---- THE HERO SET PIECE: 5-star luxury base, then the Sphere + MV glass + concert PA
    //      layered over it (an elevator you don't want to get off). ----
    buildLuxury(scene, device, physics);
    buildSphere(scene, device);
    buildMusicVideoGlass(scene, device);
    buildConcertPA(scene, device);

    // ---- Interior + accent point lights (host pushes these each frame) ----
    m_lights.clear();
    // Warm KEY light — intensity baked into the color magnitude (PointLight.color =
    // linear RGB * intensity). Bright enough to lift the dark-glass cab interior so
    // the smoked walls read rich (not black) + the accent strips/holo pop against it.
    // GAMMA WALK-BACK (integration/gamma-fold, 2026-07-25): these always-on base
    // lights were tuned on the 2x-dark engine; the sRGB fix lifts them ~2.4x so the
    // luxury cab reads over-bright (measured mean-luma ~140). Cast intensities cut
    // ~30% (matching the club's wash walk-back) — still a bright showcase, no blowout.
    { x3::rhi::PointLight ceil; ceil.color[0]=2.40f; ceil.color[1]=2.10f; ceil.color[2]=1.70f; ceil.range=8.0f; m_lights.push_back(ceil); } // warm key (was 3.4/3.0/2.4)
    { x3::rhi::PointLight holo; holo.color[0]=0.42f; holo.color[1]=1.55f; holo.color[2]=2.25f; holo.range=5.0f;  m_lights.push_back(holo); }  // holo cyan glow (was 0.6/2.2/3.2)
    // WAVE-2B (LD review #3): the cab read as a BLACK BOX between two beautiful vistas —
    // the exterior crown glow existed but the interior ceiling had no fill, so the coffer
    // + upper walls fell to black (captures/elevtrio/elevator_interior.png). Add ONE SOFT
    // ceiling fill high at cab centre — additive only (this is the 14900K showcase; the
    // warm key + holo mood are untouched). A gentle cool-neutral wash so the coffered
    // ceiling + rails read without flattening the smoked-glass richness. Placed at [2] so
    // the disco spots stay the TRAILING lights (layoutCab poses this + skips it in the
    // disco sweep). Low intensity: lift the black, don't wash the room.
    { x3::rhi::PointLight fill; fill.color[0]=1.70f; fill.color[1]=1.85f; fill.color[2]=2.10f; fill.range=6.5f; m_lights.push_back(fill); } // soft ceiling fill (gamma walk-back, was 2.4/2.6/3.0)
    // 4 disco spots (off until 1127); placed in layoutCab().
    for (int i = 0; i < 4; ++i) { x3::rhi::PointLight l; l.color[0]=l.color[1]=l.color[2]=0.0f; l.range=7.0f; m_lights.push_back(l); }

    // Player spawn just inside the cab, on the deck, facing -Z toward the holo panel.
    const float floorY = m_floors[start].centerY + m_cabHY;
    m_spawn = x3::phys::Vec3{ m_shaftX, floorY + 0.05f, m_shaftZ + m_cabHZ - 0.6f };

    m_built = true;
    layoutCab(scene);
    x3::logInfo("[showcase] THICK dark-glass elevator built: " + std::to_string(m_floors.size()) +
                " floors, club stop " + std::to_string(m_clubStop) +
                ", " + std::to_string(m_stats.entities) + " entities");
    return true;
}

// ===========================================================================
// SHAFT SHELL — a heavy structural tube + premium portal frames + sliding doors +
// realistic call-panel keypads on each served floor. THICK + NICE.
// ===========================================================================
void ElevatorShowcase::buildShaft(Scene& scene, x3::rhi::IRenderDevice& device,
                                  x3::phys::IPhysicsWorld& physics) {
    using namespace x3::elevmesh;
    const float lo = m_floors.front().centerY - 3.0f;
    const float hi = m_floors.back().centerY  + 4.0f;
    const float shaftH = hi - lo;
    const float midY = (lo + hi) * 0.5f;
    // Shaft inner half-extent: the cab plus generous clearance + THICK walls.
    const float inHX = m_cabHX + 0.55f, inHZ = m_cabHZ + 0.55f;
    const float wallT = 0.45f;                // THICK structural walls
    const float ox = inHX + wallT, oz = inHZ + wallT;

    auto box = [&](float hx,float hy,float hz,float cx,float cy,float cz,const float c[4],bool col){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,cx,cy,cz,0.04f);
        return addSolid(scene, device, physics, p, c, kNoEm, col, (uint32_t)Tag::Static);
    };

    // --- Back wall (-Z, behind the cab; this is the "spine" the strata reads against) ---
    box(ox, shaftH*0.5f, wallT*0.5f, m_shaftX, midY, m_shaftZ - inHZ - wallT*0.5f, kDarkSteel, true);
    // --- Side walls (+/-X) ---
    box(wallT*0.5f, shaftH*0.5f, oz, m_shaftX - inHX - wallT*0.5f, midY, m_shaftZ, kDarkSteel, true);
    box(wallT*0.5f, shaftH*0.5f, oz, m_shaftX + inHX + wallT*0.5f, midY, m_shaftZ, kDarkSteel, true);
    // The FRONT (+Z) is the doorway face — left open per-floor (the door frames sit there).
    // Front pillars flanking the doorway (full height, THICK).
    const float doorHalfW = m_cabHX - 0.05f;     // opening half-width
    box(wallT*0.55f, shaftH*0.5f, wallT*0.5f, m_shaftX - doorHalfW - wallT*0.55f, midY, m_shaftZ + inHZ + wallT*0.5f, kDarkSteel, true);
    box(wallT*0.55f, shaftH*0.5f, wallT*0.5f, m_shaftX + doorHalfW + wallT*0.55f, midY, m_shaftZ + inHZ + wallT*0.5f, kDarkSteel, true);

    // Shaft cap + base slabs (THICK).
    box(ox, 0.30f, oz, m_shaftX, hi, m_shaftZ, kDarkSteel, true);
    box(ox, 0.30f, oz, m_shaftX, lo, m_shaftZ, kDarkSteel, true);

    // --- Per-floor: a premium chamfered portal frame + 2 sliding door leaves + a
    //     realistic call-panel keypad beside the doorway. ---
    const float doorH = 2.35f;               // opening half-height ~ tall premium doors
    const float frontZ = m_shaftZ + inHZ + 0.02f;   // door plane (just outside the shaft front)
    m_shaftDoorL.clear(); m_shaftDoorR.clear(); m_shaftDoorY.clear();

    for (int f = 0; f < (int)m_floors.size(); ++f) {
        const float cy = m_floors[f].centerY + m_cabHY + doorH;   // doorway vertical center

        // Chamfered portal frame (THICK jambs).
        { ElevPrim p; p.mesh = doorFrame(doorHalfW + 0.06f, doorH, 0.22f, 0.16f,
                                         m_shaftX, cy, frontZ, 0.035f);
          addSolid(scene, device, physics, p, kBrushedSteel, kNoEm, true, (uint32_t)Tag::Static); }

        // 2 THICK sliding door leaves (heavy slabs, beveled), meeting at center.
        const float leafHW = doorHalfW * 0.5f - 0.01f;
        const float leafHD = 0.10f;          // THICK heavy door
        const float dz = frontZ + 0.10f;     // doors ride just proud of the frame face
        { ElevPrim p; p.mesh = beveledBox(leafHW, doorH - 0.05f, leafHD,
                                          m_shaftX - leafHW, cy, dz, 0.03f);
          uint32_t id = addSolid(scene, device, physics, p, kDoorMetal, kNoEm, false, (uint32_t)Tag::Door);
          m_shaftDoorL.push_back(id); m_stats.shaftDoors++; }
        { ElevPrim p; p.mesh = beveledBox(leafHW, doorH - 0.05f, leafHD,
                                          m_shaftX + leafHW, cy, dz, 0.03f);
          uint32_t id = addSolid(scene, device, physics, p, kDoorMetal, kNoEm, false, (uint32_t)Tag::Door);
          m_shaftDoorR.push_back(id); m_stats.shaftDoors++; }
        m_shaftDoorY.push_back(cy);
        // A glowing seam strip down the door meeting line (premium accent).
        { ElevPrim p; p.mesh = beveledBox(0.012f, doorH - 0.1f, 0.012f, m_shaftX, cy, dz + leafHD + 0.005f, 0.004f);
          addDecor(scene, device, p, kBrushedSteel, kAccentEm, (uint32_t)Tag::Prop); }

        // --- Realistic CALL PANEL keypad beside the door (+X jamb) ---
        const float panelX = m_shaftX + doorHalfW + 0.34f;
        const float panelY = cy - doorH + 1.3f;          // ~1.3 m up from floor
        const float panelZ = frontZ + 0.12f;
        // Panel plate (brushed, beveled, slightly proud of the wall).
        { ElevPrim p; p.mesh = beveledBox(0.13f, 0.20f, 0.03f, panelX, panelY, panelZ, 0.012f);
          addSolid(scene, device, physics, p, kBrushedSteel, kNoEm, false, (uint32_t)Tag::Button); }
        m_stats.callPanels++;
        // 2 real round call buttons (UP / DOWN) with a soft glow ring.
        { ElevPrim p; p.mesh = roundButton(panelX, panelY + 0.05f, panelZ + 0.03f, 0.035f, 0.018f, 12);
          float em[4] = {0.10f, 0.70f, 0.35f, 1.6f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button); m_stats.callButtons++; }
        { ElevPrim p; p.mesh = roundButton(panelX, panelY - 0.05f, panelZ + 0.03f, 0.035f, 0.018f, 12);
          float em[4] = {0.85f, 0.55f, 0.10f, 1.4f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button); m_stats.callButtons++; }
        // A small floor-indicator emissive chip above the buttons.
        { ElevPrim p; p.mesh = beveledBox(0.08f, 0.025f, 0.012f, panelX, panelY + 0.13f, panelZ + 0.02f, 0.004f);
          float em[4] = {0.20f, 0.80f, 1.0f, 2.0f};
          addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Prop); }

        // Floor lobby pad (a thick deck slab in front of the doors so you stand level).
        box(doorHalfW + 0.6f, 0.12f, 0.7f, m_shaftX, m_floors[f].centerY + m_cabHY - 0.12f,
            frontZ + 0.75f, kCabFloor, true);
    }
}

// ===========================================================================
// STRATA LINER — the DESCENT experience (goal #3). Band the shaft interior faces
// with depth-tinted GLOWING geology (limestone -> granite -> basalt -> obsidian ->
// the club's crystal glow near the bottom). World-FIXED thin panels hugging just
// inside the walls (outside the cab footprint, so they never clip the interior);
// as the cab descends they slide past the dark glass = "rock layers rushing past".
// update() scrolls a bright seam down them + swells the glow while travelling.
// ===========================================================================
void ElevatorShowcase::buildStrataLiner(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    // The visible descent shaft: from just under the top served floor down to the club.
    const float topY = m_floors.back().centerY + m_cabHY;      // ~surface
    const float botY = m_floors.front().centerY + m_cabHY - 2.0f; // just past the club
    // Panels hug the shaft interior faces (inHX/inHZ from buildShaft), just proud of the
    // dark-steel walls so their glow reads without z-fighting.
    const float inHX = m_cabHX + 0.55f, inHZ = m_cabHZ + 0.55f;
    const float seg = 5.0f;                                     // band segment height (m)
    const int   n = std::max(1, (int)((topY - botY) / seg));
    m_eStrataBands.clear(); m_eStrataBandY.clear(); m_eStrataBandEm.clear(); m_eStrataBandTint.clear();

    // STYLIZED NEON GEOLOGY palette (premium, club-UV direction): the raw survey rock
    // colours are desaturated greys/tans that wash to a flat haze behind the smoked glass,
    // so the liner uses SATURATED hues per depth band — the hue survives the glass even
    // when the band is kept dim, reading as coloured strata rather than grey fog. Ordered
    // surface -> deep (amber limestone -> teal granite -> indigo basalt -> deep blue-UV
    // -> the club's BLUE-UV crystal glow, matching Club 1127's blacklight so the seam is
    // seamless on arrival). glow flag marks the bright crystal/magma layers.
    auto stratumAt = [](float y, float rgb[3], bool& glow, float grgb[3]) {
        struct Band { float yMin; float rgb[3]; bool glow; };  // yMin = band's lower bound
        static const Band kNeon[] = {
            { -20.0f,  {0.55f, 0.42f, 0.18f}, false },  // surface amber (limestone/foundation)
            { -80.0f,  {0.12f, 0.55f, 0.52f}, false },  // teal granite
            {-140.0f,  {0.14f, 0.22f, 0.62f}, false },  // indigo basalt
            {-200.0f,  {0.12f, 0.03f, 0.85f}, true  },  // deep blue-UV obsidian (deep, glows — leads into the club seam)
            {-1e9f,    {0.10f, 0.00f, 1.00f}, true  },  // BLUE-UV crystal (the club approach — matches Club 1127 blacklight)
        };
        for (const Band& b : kNeon) {
            if (y >= b.yMin) { for(int k=0;k<3;++k){rgb[k]=b.rgb[k]; grgb[k]=b.rgb[k];} glow=b.glow; return; }
        }
        const Band& b = kNeon[4];
        for(int k=0;k<3;++k){rgb[k]=b.rgb[k]; grgb[k]=b.rgb[k];} glow=b.glow;
    };

    // A thin SELF-LIT geology panel on one interior face at band center cy. The baseColor
    // is kept near-BLACK so the cab's bright interior lights don't blow the saturated rock
    // hue into a flood — the colour comes purely from the emissive term (real geology reads
    // as glowing depth, not a lit wall). The hue is stored for the seam blend in update().
    auto face = [&](float hx,float hy,float hz,float cx,float cy,float cz,
                    const float rgb[3], float em) {
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,cx,cy,cz,0.01f);
        float c[4]  = { 0.03f, 0.03f, 0.035f, 1.0f };   // near-black: lighting must not amplify it
        float e[4]  = { rgb[0], rgb[1], rgb[2], em };    // colour lives in the emissive term
        uint32_t id = addDecor(scene, device, p, c, e, (uint32_t)Tag::Prop);   // world-FIXED, no layoutCab offset
        m_eStrataBands.push_back(id);
        m_eStrataBandY.push_back(cy);
        m_eStrataBandEm.push_back(em);
        m_eStrataBandTint.push_back(rgb[0]); m_eStrataBandTint.push_back(rgb[1]); m_eStrataBandTint.push_back(rgb[2]);
    };

    const float panelHY = seg * 0.5f - 0.06f;   // panel half-height (leaves a dark seam)
    for (int i = 0; i < n; ++i) {
        const float cy = topY - (i + 0.5f) * seg;
        float rgb[3]; bool glow=false; float grgb[3];
        stratumAt(cy, rgb, glow, grgb);
        // MOOD, not lightbox: a DARK shaft where distinct glowing bands + a bright moving
        // seam stream past. Crystal/magma layers glow; plain rock is a low self-lit band so
        // the geology reads without washing the premium dark-glass cab. The seam (update())
        // is the motion star.
        // The bands hug the cab closely, so even a modest emissive would FLOOD the premium
        // dark interior with colour through the glass. Keep the base glow LOW (a coloured
        // depth cue, not an area light); the bright moving scan-seam (update()) is the real
        // streaming tell, and it rides additively on top so it still pops.
        const float em = glow ? 0.50f : 0.12f;   // gamma walk-back (was 0.70/0.16)
        const float* tint = glow ? grgb : rgb;
        // Back face (-Z, the "spine" seen through the -Z glass wall + straight down the shaft).
        face(inHX - 0.02f, panelHY, 0.03f, m_shaftX, cy, m_shaftZ - inHZ + 0.05f, tint, em);
        // Two side faces (+/-X): dimmer still — they flank the camera closest of all.
        face(0.03f, panelHY, inHZ - 0.02f, m_shaftX - inHX + 0.05f, cy, m_shaftZ, tint, em * 0.35f);
        face(0.03f, panelHY, inHZ - 0.02f, m_shaftX + inHX - 0.05f, cy, m_shaftZ, tint, em * 0.35f);
    }
    x3::logInfo("[showcase] strata liner: " + std::to_string(m_eStrataBands.size()) +
                " glowing geology bands (" + std::to_string(topY) + " -> " + std::to_string(botY) + " m)");
}

// ===========================================================================
// CAB INTERIOR — THICK deck + ceiling + DARK SMOKED GLASS walls + handrail +
// glass floor (strata view) + accent light strips + entertainment screen + vent.
// All authored centered at the cab origin; layoutCab() offsets them each frame.
// ===========================================================================
void ElevatorShowcase::buildCabInterior(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    // Premium cab interior dims (THICK, generous: ~3.4 x 4.6 x 3.4 m clear).
    const float W = 1.55f, D = 1.55f;        // interior half-extents (match cab platform)
    const float H = 2.35f;                    // interior half-height
    const float wallT = 0.06f;                // glass pane thickness (THICK premium glass)

    auto decorBox = [&](float hx,float hy,float hz,float cx,float cy,float cz,const float c[4],const float em[4]){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,cx,cy,cz,0.015f);
        return addDecor(scene, device, p, c, em, (uint32_t)Tag::Prop);
    };

    // --- DARK SMOKED GLASS walls (3 sides: -Z back, +/-X). Front (+Z) is the door. ---
    // Authored as thin glass slabs just inside the cab. See-through but dark + rich.
    { ElevPrim p; p.mesh = beveledBox(W - 0.02f, H, wallT, 0, 0, -(D - wallT), 0.012f);
      m_eWall[0] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    { ElevPrim p; p.mesh = beveledBox(wallT, H, D - 0.02f, -(W - wallT), 0, 0, 0.012f);
      m_eWall[1] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    { ElevPrim p; p.mesh = beveledBox(wallT, H, D - 0.02f, (W - wallT), 0, 0, 0.012f);
      m_eWall[2] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    // Front upper transom glass (above the doorway) so the cab reads enclosed.
    { ElevPrim p; p.mesh = beveledBox(W - 0.02f, H - 1.9f, wallT, 0, H - (H - 1.9f), (D - wallT), 0.012f);
      m_eWall[3] = addDarkGlass(scene, device, p, 0.62f, kDarkGlassTint, kNoEm); }
    m_stats.hasDarkGlass = true;

    // --- THICK dark deck (the cab floor edge ring) + a GLASS center for the strata view ---
    // A solid dark frame ring around a transparent glass center panel (look down → strata).
    const float ringT = 0.28f;
    decorBox(W, 0.06f, ringT, 0, -H + 0.05f, -(D - ringT), kCabFloor, kNoEm);   // back ring
    decorBox(W, 0.06f, ringT, 0, -H + 0.05f,  (D - ringT), kCabFloor, kNoEm);   // front ring
    decorBox(ringT, 0.06f, D, -(W - ringT), -H + 0.05f, 0, kCabFloor, kNoEm);   // -X ring
    decorBox(ringT, 0.06f, D,  (W - ringT), -H + 0.05f, 0, kCabFloor, kNoEm);   // +X ring
    // GLASS FLOOR center (see the strata descend below you).
    { ElevPrim p; p.mesh = beveledBox(W - ringT, 0.025f, D - ringT, 0, -H + 0.04f, 0, 0.008f);
      float em[4] = {0,0,0,0};
      m_eGlassFloor = addDarkGlass(scene, device, p, 0.40f, kDarkGlassTint, em); }
    m_stats.hasGlassFloor = true;

    // --- The STRATA PLANE seen THROUGH the glass floor (driven by current stratum) ---
    { ElevPrim p; p.mesh = beveledBox(W - ringT - 0.05f, 0.02f, D - ringT - 0.05f, 0, -H - 1.2f, 0, 0.005f);
      float c[4] = {0.30f, 0.28f, 0.32f, 1.0f}; float em[4] = {0.30f, 0.28f, 0.32f, 0.8f};
      m_eStrata = addDecor(scene, device, p, c, em, (uint32_t)Tag::Prop); }

    // --- Coffered ceiling (THICK) with a recessed warm luminaire ---
    // WAVE-2B (LD review #3): the cab ceiling read pure black between the two vistas.
    // Brighten the recessed luminaire so the coffer is SELF-LIT (a soft ceiling fill),
    // paired with the new fill point light — additive, no restyle of the warm/holo mood.
    const float kCeilFill[4] = { 1.00f, 0.86f, 0.55f, 2.8f };   // brighter warm luminaire
    decorBox(W, 0.10f, D, 0, H - 0.05f, 0, kDarkSteel, kNoEm);
    m_eCeil = decorBox(W - 0.35f, 0.04f, D - 0.35f, 0, H - 0.14f, 0, kCabFloor, kCeilFill);

    // --- Octagonal brushed HANDRAIL around 3 walls at waist height ---
    // PREMIUM METAL (goal #1): the rail routes through the PBR path with a CLEARCOAT lobe
    // so it reads as a glossy clearcoated-steel rail (a lacquer highlight over the brushed
    // base) rather than a flat grey tube. A dielectric MR texel (metallic 0) keeps it from
    // going black in the cab's low-IBL interior; the clearcoat sheen rides on top.
    if (!m_mrSteel.valid()) {
        const uint8_t mr[4] = { 0, 80, 0, 255 };   // glTF MR: G=rough ~0.31, B=metal 0 (dielectric)
        m_mrSteel = device.createTexture(mr, 1, 1, false);
    }
    const float railY = -0.15f, railR = 0.035f;
    auto rail = [&](const x3::prims::PrimMesh& mesh) {
        ElevPrim p; p.mesh = mesh;
        uint32_t id = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop);
        Entity& e = scene.get(id);
        e.mrTex = m_mrSteel;            // -> PBR route
        e.clearcoat = 1.0f; e.clearcoatRough = 0.10f;   // premium lacquer sheen
        return id;
    };
    m_eRailEnts[0] = rail(tube(railR, W - 0.12f, 0, railY, -(D - 0.10f), 0, 8));   // back run (along X)
    m_eRailEnts[1] = rail(tube(railR, D - 0.12f, -(W - 0.10f), railY, 0, 2, 8));   // -X run (along Z)
    m_eRailEnts[2] = rail(tube(railR, D - 0.12f,  (W - 0.10f), railY, 0, 2, 8));   // +X run
    m_stats.hasHandrail = true;

    // --- Glowing ACCENT light strips (vertical, at the wall corners) ---
    for (int i = 0; i < 4; ++i) {
        float sx = (i & 1) ? (W - 0.04f) : -(W - 0.04f);
        float sz = (i & 2) ? (D - 0.04f) : -(D - 0.04f);
        ElevPrim p; p.mesh = beveledBox(0.02f, H - 0.3f, 0.02f, sx, 0, sz, 0.006f);
        m_eAccent[i] = addDecor(scene, device, p, kDarkSteel, kAccentEm, (uint32_t)Tag::Prop);
    }

    // --- ENTERTAINMENT SCREEN on the -X wall (a wall display looping visuals/ads) ---
    { ElevPrim p; p.mesh = beveledBox(0.02f, 0.42f, 0.62f, -(W - 0.05f), 0.5f, 0.2f, 0.01f);
      float em[4] = {0.20f, 0.45f, 0.85f, 1.8f};
      m_eEntScreen = addDecor(scene, device, p, kCabFloor, em, (uint32_t)Tag::Prop); }
    m_stats.hasEntScreen = true;

    // --- VENT grille on the ceiling edge (the "heat/cooling" feel) ---
    { ElevPrim p; p.mesh = beveledBox(0.30f, 0.03f, 0.14f, 0, H - 0.18f, D - 0.30f, 0.006f);
      float em[4] = {0.05f, 0.06f, 0.08f, 0.3f};
      m_eVent = addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Prop); }
    m_stats.hasVent = true;

    // --- Disco ball (hidden until 1127) hung from ceiling center ---
    { ElevPrim p; p.mesh = x3::elevmesh::cylinderY(0.22f, 0.22f, 0, H - 0.55f, 0, 12, true);
      m_eDiscoBall = addDecor(scene, device, p, kBrushedSteel, kNoEm, (uint32_t)Tag::Prop); }

    // --- INNER CAB DOOR leaves (slide with the FSM door %) ---
    const float cdHW = W * 0.5f - 0.02f, cdHD = 0.05f, cdH = 1.95f;
    { ElevPrim p; p.mesh = beveledBox(cdHW, cdH, cdHD, -cdHW, -H + cdH + 0.05f, D - 0.06f, 0.02f);
      m_eCabDoorL = addDecor(scene, device, p, kDoorMetal, kNoEm, (uint32_t)Tag::Door); }
    { ElevPrim p; p.mesh = beveledBox(cdHW, cdH, cdHD,  cdHW, -H + cdH + 0.05f, D - 0.06f, 0.02f);
      m_eCabDoorR = addDecor(scene, device, p, kDoorMetal, kNoEm, (uint32_t)Tag::Door); }
}

// ===========================================================================
// HOLO CONTROL PANEL — a transparent glowing glass panel on the -X wall with the
// building directory + an animated floor indicator + floor-select round buttons.
// ===========================================================================
void ElevatorShowcase::buildHoloPanel(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f;
    // Panel anchored on the +X wall, chest height, facing -X into the cab. The
    // HoloTerminal builds its own translucent emissive glass + ceiling arm. We place
    // it relative to the cab origin; layoutCab() does NOT move it (the holo manages
    // its own entities). We anchor it at the cab's START position; for the showcase
    // the cab + holo move together — but since the holo's entities aren't re-laid by
    // layoutCab, we anchor it at a FIXED interior spot and accept that the panel rides
    // with the cab via the SAME per-frame offset we apply to the buttons below.
    const float startFloorY = m_elev.cabCenter().y + m_cabHY;
    x3::phys::Vec3 holoPos{ m_shaftX + W - 0.10f, startFloorY + 1.35f, m_shaftZ + 0.2f };
    m_holo.build(scene, device, holoPos, /*yaw*/1.5708f, /*w*/0.7f, /*h*/0.95f,
                 holoPos.y + 0.9f);
    m_holo.setLines({
        std::string("X3  CORE LIFT  \xC2\xB7  DIRECTORY"),
        std::string("--------------------------------"),
    });
    for (int i = (int)m_floors.size() - 1; i >= 0; --i) {
        std::string row = (i == m_clubStop ? std::string("[*] ") : std::string("[ ] ")) + m_floors[i].label;
        m_holo.addLine(row);
    }
    m_holo.addLine(std::string("ENTER CODE 1127 -> DISCO DESCENT"));
    m_holo.addLine(std::string("--------------------------------"));
    // The LIVE status row (updated each frame via setLastLine): floor / depth-to-club /
    // stratum / motion. Seeded here; update() re-bakes it only when the text changes.
    m_holoStatusLine = "SURFACE  -  DEPTH 0 m  -  IDLE";
    m_holo.addLine(m_holoStatusLine);
    m_stats.hasHoloPanel = true;

    // Interior floor-select ROUND BUTTONS in a column beside the holo glass (real
    // raised buttons; pressing maps to callTo in the host). One per floor.
    const float bx = m_shaftX + W - 0.13f;
    const float bz = m_shaftZ - 0.55f;
    m_holoButtonCount = 0;
    for (int i = 0; i < (int)m_floors.size() && i < 16; ++i) {
        float by = startFloorY + 0.4f + (float)i * 0.16f;
        ElevPrim p; p.mesh = roundButton(0, 0, 0, 0.03f, 0.016f, 12);   // origin-authored; layoutCab offsets
        // Club stop marker glows BLUE-UV (matches Club 1127's blacklight, was magenta);
        // the other floors stay cool cyan and remain distinct from the pure-blue club.
        float em[4] = { (i == m_clubStop) ? 0.10f : 0.10f,
                        (i == m_clubStop) ? 0.00f : 0.55f,
                        (i == m_clubStop) ? 1.00f : 0.80f, 1.5f };
        uint32_t id = addDecor(scene, device, p, kDarkSteel, em, (uint32_t)Tag::Button);
        scene.get(id).link = (uint32_t)i;     // which stop this button calls
        m_eHoloButtons[m_holoButtonCount++] = id;
        m_stats.holoButtons++;
        (void)bx; (void)by; (void)bz;
    }
}

// ===================================================================================
// THE BEAT — one clock, shared by the whole set piece.
// ===================================================================================
float ElevatorShowcase::beatCount() const { return m_time * (kBeatBpm / 60.0f); }
float ElevatorShowcase::beatThump() const {
    const float s = std::sin(beatCount() * kPi);
    const float k = std::max(0.0f, s);
    return k * k * k * k * k * k;                 // pow(.,6): a sharp per-beat kick
}
// The show plays ALWAYS (idle/stopped/descending) — but 1127 disco ramps it to 11.
float ElevatorShowcase::discoBoost() const { return m_elev.disco() ? 1.0f : 0.55f; }

// ===================================================================================
// 4. FIVE-STAR LUXURY — the base the whole spectacle is layered over. Polished marble
//    floor border, brushed-gold trim, a plush bench, and a warm chandelier that breathes
//    UNDER the concert light show (the Ritz, not an office lift). Built first so the
//    Sphere / MV / PA read as a show playing over a refined room.
// ===================================================================================
void ElevatorShowcase::buildLuxury(Scene& scene, x3::rhi::IRenderDevice& device,
                                   x3::phys::IPhysicsWorld& physics) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f, H = 2.35f;
    (void)physics;
    // Shared premium material texels (glTF MR: B=metallic, G=roughness).
    if (!m_mrMarble.valid()) { const uint8_t mr[4] = {0,  40, 0, 255}; m_mrMarble = device.createTexture(mr,1,1,false); } // polished dielectric
    if (!m_mrGold.valid())   { const uint8_t mr[4] = {255,70,255,255}; m_mrGold   = device.createTexture(mr,1,1,false); } // metal, satin gold
    if (!m_mrPanel.valid())  { const uint8_t mr[4] = {0,  50, 0, 255}; m_mrPanel  = device.createTexture(mr,1,1,false); } // glossy screen glass

    auto premium = [&](const x3::prims::PrimMesh& mesh, const float c[4], const float em[4],
                       x3::rhi::TextureHandle mr, float cc, float ccr) {
        ElevPrim p; p.mesh = mesh;
        uint32_t id = addDecor(scene, device, p, c, em ? em : kNoEm, (uint32_t)Tag::Prop);
        Entity& e = scene.get(id);
        if (mr.valid()) e.mrTex = mr;
        e.clearcoat = cc; e.clearcoatRough = ccr;
        return id;
    };

    // --- POLISHED MARBLE floor border (a lacquered dark-stone frame around the glass
    //     strata view; clearcoat sheen = the wet-marble reflection). ---
    const float kMarble[4] = { 0.14f, 0.13f, 0.16f, 1.0f };   // dark warm marble
    const float ringT = 0.28f, my = -H + 0.085f;
    { uint32_t id = premium(beveledBox(W, 0.02f, ringT, 0,my,-(D-ringT), 0.01f), kMarble, kNoEm, m_mrMarble, 1.0f, 0.06f); addRide(id, 0, 0, 0); }
    { uint32_t id = premium(beveledBox(W, 0.02f, ringT, 0,my, (D-ringT), 0.01f), kMarble, kNoEm, m_mrMarble, 1.0f, 0.06f); addRide(id, 0, 0, 0); }
    { uint32_t id = premium(beveledBox(ringT, 0.02f, D, -(W-ringT),my,0, 0.01f), kMarble, kNoEm, m_mrMarble, 1.0f, 0.06f); addRide(id, 0, 0, 0); }
    { uint32_t id = premium(beveledBox(ringT, 0.02f, D,  (W-ringT),my,0, 0.01f), kMarble, kNoEm, m_mrMarble, 1.0f, 0.06f); addRide(id, 0, 0, 0); }

    // --- BRUSHED-GOLD trim: base molding along the wall feet + a coffer crown band. ---
    const float kGold[4] = { 0.83f, 0.66f, 0.32f, 1.0f };
    const float goldEm[4] = { 0.55f, 0.42f, 0.16f, 0.30f };   // a faint self-lustre so it never goes black
    auto goldStrip = [&](float hx,float hy,float hz,float ox,float oy,float oz){
        uint32_t id = premium(beveledBox(hx,hy,hz,ox,oy,oz,0.008f), kGold, goldEm, m_mrGold, 1.0f, 0.08f);
        addRide(id, 0, 0, 0);
    };
    goldStrip(W-0.05f, 0.03f, 0.03f, 0, -H+0.16f, -(D-0.04f));   // base molding, back
    goldStrip(0.03f, 0.03f, D-0.05f, -(W-0.04f), -H+0.16f, 0);   // -X
    goldStrip(0.03f, 0.03f, D-0.05f,  (W-0.04f), -H+0.16f, 0);   // +X
    goldStrip(W-0.30f, 0.02f, 0.02f, 0, H-0.20f, -(D-0.06f));    // coffer crown band, back
    goldStrip(0.02f, 0.02f, D-0.30f, -(W-0.06f), H-0.20f, 0);    // coffer crown, -X
    goldStrip(0.02f, 0.02f, D-0.30f,  (W-0.06f), H-0.20f, 0);    // coffer crown, +X

    // --- PLUSH BENCH along the -Z back wall (a padded banquette you'd want to sink into).
    //     Matte upholstery (high-rough, no clearcoat) so it reads as fabric, not plastic.
    //     Visual luxury (the deck carries the rider; a separate collider can't ride the
    //     moving cab, so this invites you to linger rather than being a sit mechanic). ---
    const float kUph[4]  = { 0.32f, 0.10f, 0.14f, 1.0f };       // deep wine velvet
    const float kUphEm[4]= { 0.05f, 0.015f,0.02f, 0.12f };
    { uint32_t id = premium(beveledBox(W-0.25f, 0.11f, 0.26f, 0, -H+0.50f, -(D-0.30f), 0.05f), kUph, kUphEm, {}, 0.0f, 0.5f); addRide(id, 0, 0, 0); } // seat cushion
    { uint32_t id = premium(beveledBox(W-0.25f, 0.34f, 0.08f, 0, -H+0.86f, -(D-0.12f), 0.04f), kUph, kUphEm, {}, 0.0f, 0.5f); addRide(id, 0, 0, 0); } // backrest
    // A brushed-gold bench rail under the seat lip.
    goldStrip(W-0.25f, 0.015f, 0.02f, 0, -H+0.40f, -(D-0.40f));

    // --- WARM CHANDELIER hung under the coffer: a central stem + a ring of warm beads +
    //     a teardrop drop. Breathes gently under the show so the luxury base always reads. ---
    const float kBrass[4] = { 0.60f, 0.48f, 0.24f, 1.0f };
    const float warm[4]   = { 1.0f, 0.80f, 0.45f, 2.4f };
    { ElevPrim p; p.mesh = cylinderY(0.015f, 0.16f, 0, H-0.42f, 0, 8, true);   // stem
      uint32_t id = addDecor(scene, device, p, kBrass, kNoEm, (uint32_t)Tag::Prop); addRide(id, 0, 0, 0); }
    for (int i = 0; i < 6; ++i) {
        const float a = (float)i / 6.0f * kPi2;
        ElevPrim p; p.mesh = cylinderY(0.032f, 0.05f, std::cos(a)*0.22f, H-0.62f, std::sin(a)*0.22f, 8, true);
        uint32_t id = addDecor(scene, device, p, kBrass, warm, (uint32_t)Tag::Prop);
        m_eChandelier.push_back(id); addRide(id, 0, 0, 0);
    }
    { ElevPrim p; p.mesh = cylinderY(0.05f, 0.07f, 0, H-0.66f, 0, 10, true);   // central teardrop
      uint32_t id = addDecor(scene, device, p, kBrass, warm, (uint32_t)Tag::Prop);
      m_eChandelier.push_back(id); addRide(id, 0, 0, 0); }
    x3::logInfo("[showcase] 5-star luxury: marble floor border + brushed-gold trim + plush bench + chandelier");
}

// ===================================================================================
// 1. THE VEGAS SPHERE — a wraparound faceted display. An upper-wall BAND wrapping the
//    cab 360 degrees + a stepped DOME overhead, all emissive facets whose colour is
//    painted every frame by a beat-synced visualizer (buildSphere lays the geometry +
//    records each facet's angle/height; animateShow paints the flowing scenes). Standing
//    in the cab you are ENVELOPED in light + motion — the "Sphere".
// ===================================================================================
void ElevatorShowcase::buildSphere(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f, H = 2.35f;
    const float T = 0.015f;                          // facet thickness (flat on the wall)
    auto facet = [&](const x3::prims::PrimMesh& mesh, float ox, float oy, float oz,
                     float ang, float v) {
        ElevPrim p; p.mesh = mesh;
        float c[4] = { 0.02f, 0.02f, 0.03f, 1.0f };   // dark substrate; colour lives in emissive
        uint32_t id = addDecor(scene, device, p, c, kNoEm, (uint32_t)Tag::Prop);
        m_eSphere.push_back(id); m_eSphereAng.push_back(ang); m_eSphereV.push_back(v);
        addRide(id, ox, oy, oz);
    };
    // --- UPPER-WALL WRAP BAND: two rows of facets on all four walls, above the glass /
    //     doorway, wrapping the cab. v spans 0.05..0.55 (the band sits below the dome). ---
    const int cols = 5;
    const float bandY0 = H * 0.30f, bandY1 = H * 0.66f;
    for (int wall = 0; wall < 4; ++wall) {
        for (int row = 0; row < 2; ++row) {
            const float oy = (row == 0) ? bandY0 : bandY1;
            const float v  = (row == 0) ? 0.12f : 0.42f;
            for (int cix = 0; cix < cols; ++cix) {
                const float u = ((float)cix + 0.5f) / cols;      // 0..1 along the wall
                const float span = (W - 0.14f);
                const float p = -span + 2.0f * span * u;         // position along the wall
                float ox, oz, ang;
                x3::prims::PrimMesh m;
                // INBOARD of the smoked-glass walls (glass inner face ~= W-0.12) so the
                // facets glow straight into the cab UNOBSTRUCTED — behind the glass they
                // metered grey. Taller facets (0.20) tile the band into a continuous wrap.
                const float inb = 0.16f;
                if (wall == 0)      { ox = p; oz = -(D - inb); ang = std::atan2(oz, ox);
                                      m = beveledBox(span/cols*0.90f, 0.20f, T, 0,0,0, 0.006f); }
                else if (wall == 1) { ox = p; oz =  (D - inb); ang = std::atan2(oz, ox);
                                      m = beveledBox(span/cols*0.90f, 0.20f, T, 0,0,0, 0.006f); }
                else if (wall == 2) { ox = -(W - inb); oz = p;  ang = std::atan2(oz, ox);
                                      m = beveledBox(T, 0.20f, span/cols*0.90f, 0,0,0, 0.006f); }
                else                { ox =  (W - inb); oz = p;  ang = std::atan2(oz, ox);
                                      m = beveledBox(T, 0.20f, span/cols*0.90f, 0,0,0, 0.006f); }
                facet(m, ox, oy, oz, ang, v);
            }
        }
    }
    // --- STEPPED DOME overhead: concentric rings of small horizontal facets rising toward
    //     a crown (a faceted dome of light). Radius shrinks + height rises per ring, so
    //     from below the rider sees an inverted bowl of animated light. v spans 0.6..1.0. ---
    const int rings = 4;
    const int perRing[4]  = { 16, 12, 8, 1 };
    const float ringR[4]  = { W * 0.80f, W * 0.56f, W * 0.30f, 0.0f };
    const float ringHW[4] = { 0.20f, 0.20f, 0.18f, 0.22f };   // facet half-width (tiled dome)
    const float ringY[4]  = { H - 0.50f, H - 0.40f, H - 0.30f, H - 0.22f };   // just below the coffer
    const float ringV[4]  = { 0.62f, 0.78f, 0.90f, 1.0f };
    for (int r = 0; r < rings; ++r) {
        for (int i = 0; i < perRing[r]; ++i) {
            const float a = (float)i / perRing[r] * kPi2;
            const float ox = std::cos(a) * ringR[r], oz = std::sin(a) * ringR[r];
            facet(beveledBox(ringHW[r], T, ringHW[r], 0,0,0, 0.006f), ox, ringY[r], oz, a, ringV[r]);
        }
    }
    x3::logInfo("[showcase] Vegas Sphere: " + std::to_string(m_eSphere.size()) +
                " wraparound emissive facets (wall band + stepped dome)");
}

// ===================================================================================
// 2. MUSIC-VIDEO GLASS — translucent holo panes playing a baked strip of procedural MV
//    frames, cut on the beat (animateShow swaps the emissiveTex). Reads as music videos
//    layered over the cab (goal #2).
// ===================================================================================
void ElevatorShowcase::buildMusicVideoGlass(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f;
    if (!m_mrPanel.valid()) { const uint8_t mr[4] = {0, 50, 0, 255}; m_mrPanel = device.createTexture(mr,1,1,false); }
    // --- Bake the MV strip ONCE: 4 dancer poses + 2 equalizers + a lyric card + a city. ---
    m_mvFrame[0] = device.createTexture(mvDancer(0, 0.92f).data(), kMvW, kMvH, true);  // magenta stage
    m_mvFrame[1] = device.createTexture(mvDancer(1, 0.58f).data(), kMvW, kMvH, true);  // cyan stage
    m_mvFrame[2] = device.createTexture(mvDancer(2, 0.78f).data(), kMvW, kMvH, true);  // violet stage
    m_mvFrame[3] = device.createTexture(mvDancer(3, 0.10f).data(), kMvW, kMvH, true);  // amber stage
    m_mvFrame[4] = device.createTexture(mvEqualizer(0.3f).data(),  kMvW, kMvH, true);
    m_mvFrame[5] = device.createTexture(mvEqualizer(1.7f).data(),  kMvW, kMvH, true);
    m_mvFrame[6] = device.createTexture(mvLyric(0, 0.55f).data(),  kMvW, kMvH, true);
    m_mvFrame[7] = device.createTexture(mvCity(0.0f).data(),       kMvW, kMvH, true);

    // A translucent holo MV pane: dark glass substrate + the MV as an emissive map on the
    // PBR/alpha-blend route (mirrors the cab's -X window alphaBlend + the OLED emissiveTex).
    auto pane = [&](float hx,float hy,float hz,float ox,float oy,float oz){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,0,0,0, 0.01f);
        Entity e;
        e.mesh = device.createMesh(p.mesh.verts.data(), (uint32_t)p.mesh.verts.size(),
                                   p.mesh.index.data(), (uint32_t)p.mesh.index.size());
        e.mrTex = m_mrPanel;
        e.emissiveTex = m_mvFrame[0];
        e.baseColor[0] = 0.05f; e.baseColor[1] = 0.05f; e.baseColor[2] = 0.07f; e.baseColor[3] = 0.82f;
        e.emissive[0] = 1.0f; e.emissive[1] = 1.0f; e.emissive[2] = 1.0f; e.emissive[3] = 1.05f; // gamma walk-back 2nd pass (2.2 -> 1.5 -> 1.05)
        e.alphaBlend = true;                         // see-through holo glass (blend tail, no depth trap)
        e.tag = (uint32_t)Tag::Prop; e.body.id = 0;
        uint32_t id = scene.add(e); ++m_stats.entities;
        m_eMvPanel.push_back(id); addRide(id, ox, oy, oz);
    };
    // Hero pane (-X wall, big), + a side pane (+Z front transom) + a back pane (above bench).
    pane(0.02f, 0.52f, 0.72f, -(W-0.04f), 0.28f, -0.10f);   // HERO -X (lower + bigger to frame)
    pane(0.44f, 0.30f, 0.02f, 0.10f, 0.95f,  (D-0.04f));    // +Z transom (over the doors)
    pane(0.40f, 0.28f, 0.02f, 0.0f,  0.55f, -(D-0.04f));    // -Z back (above the bench)
    x3::logInfo("[showcase] music-video glass: " + std::to_string(m_eMvPanel.size()) +
                " holo panes, 8-frame MV strip (dancer/equalizer/lyric/city), beat-cut");
}

// ===================================================================================
// 3. CONCERT PA — a real line-array + subwoofers + satellites that VISIBLY perform: the
//    sub cones PUMP on the bass (a per-beat ride-along translation) and the driver lenses
//    STROBE on the beat (animateShow). The cab reads as a private PA firing at you.
// ===================================================================================
void ElevatorShowcase::buildConcertPA(Scene& scene, x3::rhi::IRenderDevice& device) {
    using namespace x3::elevmesh;
    const float W = 1.55f, D = 1.55f, H = 2.35f;
    const float kCab[4] = { 0.05f, 0.05f, 0.06f, 1.0f };     // matte-black speaker box
    auto cabinet = [&](float hx,float hy,float hz,float ox,float oy,float oz){
        ElevPrim p; p.mesh = beveledBox(hx,hy,hz,0,0,0, 0.02f);
        uint32_t id = addDecor(scene, device, p, kCab, kNoEm, (uint32_t)Tag::Prop);
        addRide(id, ox, oy, oz);
    };
    // A driver lens (emissive baffle facing the room, -Z face of the front arrays) that
    // strobes on the beat. A thin square panel reads as a lit driver without any rotation.
    auto driver = [&](float r,float ox,float oy,float oz){
        ElevPrim p; p.mesh = beveledBox(r, r, 0.012f, 0,0,0, r*0.5f);   // round-ish (heavy bevel) baffle
        float em[4] = { 0.4f, 0.6f, 1.0f, 1.2f };
        uint32_t id = addDecor(scene, device, p, kCab, em, (uint32_t)Tag::Prop);
        m_eDriver.push_back(id); addRide(id, ox, oy, oz);
    };
    // A woofer/sub cone (bigger emissive baffle) that PUMPS out toward the rider on the
    // bass (the ride-along loop applies pumpZ * m_showPump every frame).
    auto cone = [&](float r,float ox,float oy,float oz,float pumpZ){
        ElevPrim p; p.mesh = beveledBox(r, r, 0.03f, 0,0,0, r*0.6f);    // recessed cone baffle
        float em[4] = { 0.10f, 0.12f, 0.20f, 0.35f };
        uint32_t id = addDecor(scene, device, p, kCab, em, (uint32_t)Tag::Prop);
        m_eSubCone.push_back(id); addRide(id, ox, oy, oz, 0, 0, pumpZ);   // pump along Z toward the rider
    };

    // --- 2 hanging LINE ARRAYS at the front (+Z) ceiling corners: 4 stacked cabinets,
    //     each face carrying two driver lenses aimed into the room. ---
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? (W - 0.16f) : -(W - 0.16f);
        for (int box = 0; box < 4; ++box) {
            const float hw = 0.15f - box * 0.012f;
            const float oy = H * 0.60f - box * 0.22f;
            const float oz = (D - 0.20f) - box * 0.02f;
            cabinet(hw, 0.10f, 0.12f, sx, oy, oz);
            driver(0.045f, sx, oy + 0.04f, oz - 0.13f);
            driver(0.045f, sx, oy - 0.04f, oz - 0.13f);
        }
    }
    // --- 2 SUBWOOFERS low in the back corners, big pumping cones facing +Z into the cab. ---
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? (W - 0.24f) : -(W - 0.24f);
        cabinet(0.22f, 0.28f, 0.22f, sx, -H + 0.42f, -(D - 0.26f));
        cone(0.16f, sx, -H + 0.42f, -(D - 0.42f), 0.045f);        // recessed sub cone (pumps)
    }
    // --- 2 SATELLITE speakers mid ±X walls (a cone + a tweeter lens). ---
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? (W - 0.08f) : -(W - 0.08f);
        cabinet(0.05f, 0.14f, 0.11f, sx, 0.25f, (D - 0.55f));
        cone(0.07f, sx - (s?0.06f:-0.06f), 0.20f, (D - 0.55f), 0.0f);
        driver(0.028f, sx - (s?0.06f:-0.06f), 0.36f, (D - 0.55f));
    }
    x3::logInfo("[showcase] concert PA: 2 line-arrays (" + std::to_string(m_eDriver.size()) +
                " drivers) + 2 subs + 2 satellites (" + std::to_string(m_eSubCone.size()) + " pumping cones)");
}

// ===================================================================================
// ANIMATE THE SHOW — every frame, off the shared beat clock. Paints the Sphere's flowing
// scenes, cuts the MV frames, strobes the PA + pumps the sub cones (the ride-along loop
// applies the pump translation), and drives the 4 repurposed concert-wash lights. Runs
// CONTINUOUSLY (idle, stopped, or descending) — the private show that never gets boring.
// ===================================================================================
void ElevatorShowcase::animateShow(float dt, Scene& scene) {
    (void)dt;
    const float t = m_time;
    const float bc = beatCount();
    const float thump = beatThump();
    const float boost = discoBoost();

    // ---- 1. VEGAS SPHERE: cycle through visualizer SCENES so it evolves + never repeats.
    //         A slow scene index crossfades every ~10 s; within a scene each facet's colour
    //         is a function of its wrap-angle + height + the beat. ----
    const float sceneDur = 10.0f;
    const int   nScenes = 5;
    const float sf = t / sceneDur;
    const int   sceneA = ((int)sf) % nScenes;
    const int   sceneB = (sceneA + 1) % nScenes;
    const float blend  = std::min(1.0f, std::max(0.0f, (sf - std::floor(sf) - 0.85f) / 0.15f)); // last 15% crossfades
    auto sceneColor = [&](int sc, float ang, float v, float& r, float& g, float& b) {
        const float u = ang / kPi2 + 0.5f;                    // 0..1 around the cab
        switch (sc) {
            case 0: { // spectrum rotor — a hue wheel sweeping around, beat-pumped brightness
                float val = 0.35f + 0.65f * thump;
                hsv2rgb(u + t * 0.08f, 0.9f, val, r, g, b); } break;
            case 1: { // vertical colour rain — bright bands travelling down the walls+dome
                float w = 0.5f + 0.5f * std::sin((v * 3.0f - t * 1.6f) * kPi2);
                float val = 0.15f + 0.9f * w * w;
                hsv2rgb(0.55f + 0.25f * std::sin(t * 0.3f), 0.85f, val, r, g, b); } break;
            case 2: { // radial pulse from the crown — a ring expands down on each beat
                float phase = std::fmod(bc, 4.0f) / 4.0f;     // 0..1 over 4 beats
                float ring = 1.0f - std::min(1.0f, std::fabs((1.0f - v) - phase) * 4.0f);
                float val = 0.12f + 0.95f * ring;
                hsv2rgb(0.75f + 0.15f * v, 0.8f, val, r, g, b); } break;
            case 3: { // equalizer — each column bounces; colour ramps by angle
                float col = std::floor(u * 12.0f);
                float h = 0.4f * std::fabs(std::sin(t * 2.2f + col * 0.8f));
                float val = (v < 0.2f + h) ? (0.3f + 0.7f * thump) : 0.10f;
                hsv2rgb(0.55f + 0.45f * (col / 12.0f), 0.9f, val, r, g, b); } break;
            default: { // aurora wash — slow overlapping sinusoids, dreamy
                float f = 0.5f + 0.5f * std::sin(u * 6.28f + t * 0.5f)
                                     * std::cos(v * 4.0f - t * 0.4f);
                float val = 0.2f + 0.6f * f + 0.25f * thump;
                hsv2rgb(0.45f + 0.4f * f + 0.1f * std::sin(t*0.2f), 0.7f, val, r, g, b); }
        }
    };
    for (size_t i = 0; i < m_eSphere.size(); ++i) {
        if (m_eSphere[i] == kNoLink || m_eSphere[i] >= scene.size()) continue;
        float rA,gA,bA, rB,gB,bB;
        sceneColor(sceneA, m_eSphereAng[i], m_eSphereV[i], rA,gA,bA);
        sceneColor(sceneB, m_eSphereAng[i], m_eSphereV[i], rB,gB,bB);
        Entity& e = scene.get(m_eSphere[i]);
        const float floorGlow = 0.06f;                        // never fully black, but keep the hue saturated
        e.emissive[0] = (rA + (rB-rA)*blend) + floorGlow;
        e.emissive[1] = (gA + (gB-gA)*blend) + floorGlow;
        e.emissive[2] = (bA + (bB-bA)*blend) + floorGlow;
        e.emissive[3] = (0.42f + 0.42f * boost);              // gamma walk-back 2nd pass (1.0+1.1 -> 0.70+0.75 -> 0.42+0.42): the facets were desaturating to milky pastel at full disco; lower so they read SATURATED colored light like the club beams
    }

    // ---- 2. MUSIC-VIDEO GLASS: cut frames on the beat. Mostly the 4 dancer poses on the
    //         eighth-note grid (so the silhouette DANCES), with equalizer / lyric / city
    //         "scenes" swapping in on phrase boundaries so the MV keeps changing. ----
    const int e8 = (int)(bc * 2.0f);                          // eighth-note index
    const int phrase = ((int)(bc / 8.0f)) % 4;                // which 8-beat segment
    int frame;
    switch (phrase) {
        case 0: frame = e8 & 3; break;                        // dance
        case 1: frame = 4 + (e8 & 1); break;                  // equalizer A/B
        case 2: frame = (e8 & 3); break;                      // dance again
        default: frame = (e8 & 1) ? 6 : 7; break;             // lyric / city
    }
    if (frame != m_mvLastFrame) {
        m_mvLastFrame = frame;
        for (uint32_t id : m_eMvPanel)
            if (id != kNoLink && id < scene.size()) scene.get(id).emissiveTex = m_mvFrame[frame];
    }
    for (uint32_t id : m_eMvPanel)                            // beat-swell the glow
        if (id != kNoLink && id < scene.size())
            scene.get(id).emissive[3] = (0.85f + 0.35f * thump) * (0.7f + 0.6f * boost); // gamma walk-back 2nd pass (1.9+0.8 -> 1.30+0.55 -> 0.85+0.35): panes were washing pale; keep the MV readable + saturated

    // ---- 3. CONCERT PA: strobe the driver lenses on the beat; the sub cones pump via the
    //         ride-along loop (it scales each RideEnt pump by m_showPump below). ----
    for (size_t i = 0; i < m_eDriver.size(); ++i) {
        if (m_eDriver[i] == kNoLink || m_eDriver[i] >= scene.size()) continue;
        Entity& e = scene.get(m_eDriver[i]);
        // A solid baseline glow (the rig is always lit) with a hard strobe punch on the
        // kick — so a still frame always shows lit drivers, and motion adds the flash.
        const float flash = 0.75f + 1.9f * thump;
        if (i & 1) { e.emissive[0]=0.5f*flash; e.emissive[1]=0.7f*flash; e.emissive[2]=1.0f*flash; }
        else       { e.emissive[0]=1.0f*flash; e.emissive[1]=0.5f*flash; e.emissive[2]=0.8f*flash; }
        e.emissive[3] = 1.10f * boost + 0.42f;   // gamma walk-back (was 1.6*boost+0.6)
    }
    for (uint32_t id : m_eSubCone)                            // the cone face glows as it pumps
        if (id != kNoLink && id < scene.size())
            scene.get(id).emissive[3] = 0.42f + 0.90f * thump * boost; // gamma walk-back (was 0.6+1.3*thump*boost)
    m_showPump = thump * (0.7f + 0.6f * boost);               // the ride-along loop reads this

    // ---- 4. CONCERT-WASH LIGHTS: reuse the 4 (otherwise-dead) disco spots [3..6] as a
    //         beat-driven concert wash so the luxury materials + the room are lit by the
    //         show — cohesion (the spectacle plays OVER the warm luxury base [0..2]). ----
    for (int i = 3; i < (int)m_lights.size(); ++i) {
        float r,g,b; hsv2rgb((float)(i-3)/4.0f + t*0.10f, 0.85f, 0.6f + 0.9f*thump, r, g, b);
        const float amp = (0.85f + 1.80f * boost);   // gamma walk-back (was 1.2 + 2.6*boost)
        m_lights[i].color[0] = r*amp; m_lights[i].color[1] = g*amp; m_lights[i].color[2] = b*amp;
    }

    // ---- LUXURY chandelier: a gentle warm breathe under the show (never strobes). ----
    for (uint32_t id : m_eChandelier)
        if (id != kNoLink && id < scene.size())
            scene.get(id).emissive[3] = 1.45f + 0.35f * std::sin(t * 1.5f); // gamma walk-back (was 2.1+0.5*sin)
}

// ===========================================================================
// PER-FRAME LAYOUT — offset all cab-child entities to the live cab center.
// ===========================================================================
void ElevatorShowcase::layoutCab(Scene& scene) {
    if (!m_built) return;
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float W = 1.55f;
    // Interior origin sits at the cab center + half-height (the cab platform top is
    // the deck; the interior box is centered ~H above the deck).
    const float ix = c.x, iz = c.z;
    const float iy = c.y + m_cabHY + 2.35f;   // interior vertical center

    auto place = [&](uint32_t id, float ox, float oy, float oz) {
        if (id == kNoLink || id >= scene.size()) return;
        Entity& e = scene.get(id);
        e.transform[12] = ix + ox; e.transform[13] = iy + oy; e.transform[14] = iz + oz;
    };
    for (int i = 0; i < kWallGlass; ++i) place(m_eWall[i], 0, 0, 0);
    place(m_eCeil, 0, 0, 0);
    place(m_eGlassFloor, 0, 0, 0);
    place(m_eStrata, 0, 0, 0);
    for (int i = 0; i < 3; ++i) place(m_eRailEnts[i], 0, 0, 0);
    for (int i = 0; i < 4; ++i) place(m_eAccent[i], 0, 0, 0);
    place(m_eEntScreen, 0, 0, 0);
    place(m_eVent, 0, 0, 0);
    place(m_eDiscoBall, 0, 0, 0);

    // HERO SET-PIECE ride-along: every Sphere facet / MV pane / PA cabinet / luxury trim
    // rides the interior origin at its authored offset, plus (for the sub cones) a per-beat
    // PUMP translation scaled by m_showPump (written by animateShow). ONE loop, so the whole
    // spectacle tracks the cab as it descends 200 m to the club.
    for (const RideEnt& r : m_ride) {
        if (r.id == kNoLink || r.id >= scene.size()) continue;
        Entity& e = scene.get(r.id);
        e.transform[12] = ix + r.ox + r.px * m_showPump;
        e.transform[13] = iy + r.oy + r.py * m_showPump;
        e.transform[14] = iz + r.oz + r.pz * m_showPump;
    }

    // Interior floor-select buttons (column on the +X wall near the holo).
    for (int i = 0; i < m_holoButtonCount; ++i) {
        float by = 0.4f + (float)i * 0.16f - 1.0f;
        place(m_eHoloButtons[i], W - 0.13f, by, -0.55f);
    }

    // THE HOLO PANEL RIDES THE CAB. It builds its own meshes (not offset by place()), so
    // without this the control panel + its ceiling pipe stayed at the boot floor while the
    // car descended 200 m — you'd lose your panel mid-ride. Track it to the live cab Y
    // (same anchor buildHoloPanel used: +X wall, chest height above the deck).
    if (m_holo.built()) {
        const float floorY = c.y + m_cabHY;
        m_holo.reposition(x3::phys::Vec3{ m_shaftX + W - 0.10f, floorY + 1.35f, m_shaftZ + 0.2f });
    }

    // Disco-ball glow when disco mode is on.
    if (m_eDiscoBall != kNoLink && m_eDiscoBall < scene.size()) {
        Entity& e = scene.get(m_eDiscoBall);
        float g = m_elev.disco() ? 1.0f : 0.0f;
        e.emissive[0] = 0.8f*g; e.emissive[1] = 0.8f*g; e.emissive[2] = 0.95f*g;
        e.emissive[3] = m_elev.disco() ? 1.10f : 0.0f;   // gamma walk-back (was 1.6)
    }

    // Drive the strata plane (seen through the glass floor) from the current stratum.
    if (m_eStrata != kNoLink && m_eStrata < scene.size()) {
        Entity& e = scene.get(m_eStrata);
        for (const StrataLayer& s : ElevatorSystem::strata()) {
            if (c.y >= s.yMin && c.y <= s.yMax) {
                for (int k = 0; k < 3; ++k) e.baseColor[k] = s.rgb[k];
                if (s.glow) { for (int k = 0; k < 3; ++k) e.emissive[k] = s.glowRgb[k]; e.emissive[3] = 1.10f; } // gamma walk-back (was 1.6)
                else        { for (int k = 0; k < 3; ++k) e.emissive[k] = s.rgb[k];     e.emissive[3] = 0.48f; } // gamma walk-back (was 0.7)
                break;
            }
        }
    }

    // Interior lights: warm ceiling key + holo glow + (disco) spots.
    if (m_lights.size() >= 2) {
        m_lights[0].pos[0]=ix;            m_lights[0].pos[1]=iy + 1.9f; m_lights[0].pos[2]=iz;
        m_lights[1].pos[0]=ix + W - 0.3f; m_lights[1].pos[1]=iy + 0.6f; m_lights[1].pos[2]=iz - 0.4f;
        // [2] = WAVE-2B soft ceiling fill: high at cab centre, just under the coffer.
        if (m_lights.size() >= 3) { m_lights[2].pos[0]=ix; m_lights[2].pos[1]=iy + 2.15f; m_lights[2].pos[2]=iz; }
        for (int i = 3; i < (int)m_lights.size(); ++i) {
            float a = (float)(i-3)/4.0f * kPi2 + m_time * (m_elev.disco() ? 2.5f : 0.0f);
            m_lights[i].pos[0]=ix + std::cos(a)*1.0f;
            m_lights[i].pos[1]=iy + 1.4f;
            m_lights[i].pos[2]=iz + std::sin(a)*1.0f;
        }
    }
}

// ===========================================================================
// DOOR ANIMATION — slide the cab + per-floor leaves to match the FSM door %.
// doorPct: 1 = fully open, 0 = closed (we read it via the FSM state proxy).
// ===========================================================================
void ElevatorShowcase::animateDoors(Scene& scene) {
    // Derive an open fraction from the FSM state: open when stopped, closed while
    // travelling. We approximate doorPct from the state (the FSM owns the real %,
    // but it isn't exposed; this matches the visible behavior 1:1).
    float openF = 0.0f;
    switch (m_elev.state()) {
        case ElevState::DoorsOpen:    openF = 1.0f; break;
        case ElevState::Idle:         openF = 1.0f; break;   // sits open at a stop
        case ElevState::DoorsOpening: openF = std::min(1.0f, m_time * 0.0f + 0.5f); break;
        case ElevState::DoorsClosing: openF = 0.5f; break;
        default:                      openF = 0.0f; break;    // travelling => shut
    }
    const float W = 1.55f;
    const float slide = openF * (W * 0.5f);     // leaves retract by up to half-width

    // Cab inner doors.
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float iy = c.y + m_cabHY + 2.35f;
    const float cdHW = W * 0.5f - 0.02f;
    if (m_eCabDoorL != kNoLink && m_eCabDoorL < scene.size()) {
        Entity& e = scene.get(m_eCabDoorL);
        e.transform[12] = c.x - cdHW - slide; e.transform[13] = iy - 2.35f + 1.95f + 0.05f; e.transform[14] = c.z + (W - 0.06f);
    }
    if (m_eCabDoorR != kNoLink && m_eCabDoorR < scene.size()) {
        Entity& e = scene.get(m_eCabDoorR);
        e.transform[12] = c.x + cdHW + slide; e.transform[13] = iy - 2.35f + 1.95f + 0.05f; e.transform[14] = c.z + (W - 0.06f);
    }

    // Per-floor shaft doors: only the floor the cab is AT opens; the rest stay shut.
    int atFloor = currentFloorIndex();
    bool stopped = (m_elev.state() == ElevState::DoorsOpen || m_elev.state() == ElevState::Idle ||
                    m_elev.state() == ElevState::DoorsOpening);
    const float doorHalfW = m_cabHX - 0.05f;
    const float leafHW = doorHalfW * 0.5f - 0.01f;
    for (int f = 0; f < (int)m_shaftDoorL.size(); ++f) {
        float of = (stopped && f == atFloor) ? openF : 0.0f;
        float sl = of * (doorHalfW * 0.5f);
        float cy = m_shaftDoorY[f];
        float dz = m_shaftZ + m_cabHZ + 0.55f + 0.02f + 0.10f;
        if (m_shaftDoorL[f] < scene.size()) {
            Entity& e = scene.get(m_shaftDoorL[f]);
            e.transform[12] = m_shaftX - leafHW - sl; e.transform[13] = cy; e.transform[14] = dz;
        }
        if (m_shaftDoorR[f] < scene.size()) {
            Entity& e = scene.get(m_shaftDoorR[f]);
            e.transform[12] = m_shaftX + leafHW + sl; e.transform[13] = cy; e.transform[14] = dz;
        }
    }
}

// ===========================================================================
// UPDATE
// ===========================================================================
float ElevatorShowcase::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return 0.0f;
    m_time += dt;
    float dy = m_elev.update(dt, scene, physics);
    // THE HERO SET-PIECE SHOW (beat-driven Sphere + MV glass + concert PA + wash lights).
    // Runs BEFORE layoutCab so the sub-cone pump (m_showPump) is current when the ride-along
    // loop poses the cones this frame. Plays continuously — idle, stopped, or descending.
    animateShow(dt, scene);
    layoutCab(scene);
    animateDoors(scene);

    // LIVE HOLO DEPTH READOUT (goal #2): the panel's last row counts the cab DOWN to the
    // club at Y=-200 with the live stratum + motion state. Re-baked only when the text
    // actually changes (depth quantized to 2 m so a full-speed descent doesn't rebake the
    // glass every frame).
    {
        const float y = m_elev.cabCenter().y;
        const int   depth = ((int)std::lround(-y / 2.0f)) * 2;   // metres below surface, 2 m steps
        std::string state;
        if (m_elev.disco())               state = "DISCO DESCENT";
        else if (!m_elev.moving())        state = (depth > 4 ? "ARRIVED" : "IDLE");
        else if (m_elev.targetStop() >= 0 &&
                 m_floors[m_elev.targetStop()].centerY < y) state = "DESCENDING v";
        else                              state = "ASCENDING ^";
        std::string line = (depth <= 0)
            ? std::string("SURFACE  -  DEPTH 0 m  -  ") + state
            : std::string("DEPTH -") + std::to_string(depth) + " m / -200  -  " +
              m_elev.currentStratum() + "  -  " + state;
        if (line != m_holoStatusLine) { m_holoStatusLine = line; m_holo.setLastLine(line); }
    }

    m_holo.update(dt);

    // Entertainment screen: a slow hue-cycling glow (looping ad visuals).
    if (m_eEntScreen != kNoLink && m_eEntScreen < scene.size()) {
        m_entScroll += dt * 0.4f;
        Entity& e = scene.get(m_eEntScreen);
        e.emissive[0] = 0.25f + 0.20f * std::sin(m_entScroll);
        e.emissive[1] = 0.40f + 0.20f * std::sin(m_entScroll + 2.1f);
        e.emissive[2] = 0.75f + 0.25f * std::sin(m_entScroll + 4.2f);
        e.emissive[3] = 1.25f;   // gamma walk-back (was 1.8)
    }
    // Vent hum visual flicker (very subtle) — feel of moving air.
    if (m_eVent != kNoLink && m_eVent < scene.size()) {
        Entity& e = scene.get(m_eVent);
        e.emissive[3] = 0.25f + 0.05f * std::sin(m_time * 9.0f);
    }
    // STRATA STREAMING (goal #3): the world-fixed geology bands swell + a bright seam
    // sweeps DOWN them while the cab travels, so looking through the dark glass the rock
    // layers read as rushing past. When the real streamed strata takes over (setStrataStreamed)
    // the placeholder liner eases its glow off so the streamed layers own the view.
    if (!m_eStrataBands.empty()) {
        const bool travelling = m_elev.moving();
        const float cabY = m_elev.cabCenter().y;
        // A downward-sweeping bright band centered near the cab's own depth (the "rush").
        const float sweepY = cabY - std::fmod(m_time * 22.0f, 12.0f);   // 22 m/s sweep, 12 m wrap
        const float yield = m_strataStreamed ? 0.25f : 1.0f;           // ease off if streamed
        for (size_t i = 0; i < m_eStrataBands.size(); ++i) {
            uint32_t id = m_eStrataBands[i];
            if (id == kNoLink || id >= scene.size()) continue;
            Entity& e = scene.get(id);
            const float base = m_eStrataBandEm[i];
            const float tr = m_eStrataBandTint[i*3], tg = m_eStrataBandTint[i*3+1], tb = m_eStrataBandTint[i*3+2];
            // Proximity of this band to the moving sweep -> a passing highlight.
            float d = std::fabs(m_eStrataBandY[i] - sweepY);
            float seam = travelling ? std::max(0.0f, 1.0f - d / 2.2f) : 0.0f;
            // Depth-swell: deeper bands (nearer the club) glow up as you descend into them.
            float depthLift = 0.5f + 0.5f * descentProgress();
            e.emissive[3] = (base * depthLift + seam * 1.5f) * yield;   // gamma walk-back (seam was *2.2)
            // Rests at the band's rock hue; the passing seam shifts it toward a bright cool
            // scan-line (the clear "rushing" tell), easing back to rock behind it.
            e.emissive[0] = tr + (0.45f - tr) * seam;
            e.emissive[1] = tg + (0.85f - tg) * seam;
            e.emissive[2] = tb + (1.00f - tb) * seam;
        }
    }

    // Accent strips brighten subtly while moving (the lift "comes alive").
    float pulse = m_elev.moving() ? (0.7f + 0.3f * std::sin(m_time * 5.0f)) : 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (m_eAccent[i] != kNoLink && m_eAccent[i] < scene.size()) {
            Entity& e = scene.get(m_eAccent[i]);
            e.emissive[3] = 1.5f * pulse;   // gamma walk-back (was 2.2*pulse; matches reduced kAccentEm)
            if (m_elev.disco()) { e.emissive[0] = 0.6f + 0.4f*std::sin(m_time*4.0f + i); e.emissive[1] = 0.2f; e.emissive[2] = 0.7f; }
        }
    }
    (void)device;
    return dy;
}

// ===========================================================================
// CALLS / DISPLAY READ-BACK
// ===========================================================================
void ElevatorShowcase::callClub() {
    if (m_clubStop >= 0) m_elev.callTo(m_clubStop);
}

int ElevatorShowcase::currentFloorIndex() const {
    const float y = m_elev.cabCenter().y;
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < (int)m_floors.size(); ++i) {
        float d = std::fabs(y - m_floors[i].centerY);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Descent progress 0..1: surface (Y>=0) -> Club 1127 (kDefaultClubFloorY = -200).
float ElevatorShowcase::descentProgress() const {
    const float y = m_elev.cabCenter().y;
    const float p = (0.0f - y) / (0.0f - ElevatorSystem::kDefaultClubFloorY);
    return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

void ElevatorShowcase::showcaseCamera(int variant, float out[5]) const {
    const x3::phys::Vec3 c = m_elev.cabCenter();
    const float floorY = c.y + m_cabHY;
    if (variant == 1) {
        // Exterior shaft: stand in the lobby looking at the doors.
        out[0] = m_shaftX; out[1] = floorY + 1.6f; out[2] = m_shaftZ + m_cabHZ + 2.6f;
        out[3] = -1.5708f; out[4] = 0.05f;
    } else if (variant == 2) {
        // Strata descent: stand at the FRONT of the cab and look back across it at the
        // -Z "spine" glass wall — the glowing geology bands stream vertically behind the
        // dark glass as the cab descends (the "rock layers rushing past" read). A slight
        // downward tilt lets the near strata + the glass-floor edge catch the eye too.
        out[0] = m_shaftX + 0.15f; out[1] = floorY + 1.55f; out[2] = m_shaftZ + m_cabHZ - 0.35f;
        out[3] = -1.5708f;   // face -Z (toward the strata spine)
        out[4] = -0.18f;
    } else if (variant == 3) {
        // Holo panel HERO: stand close in front of the +X control panel, looking straight
        // at it (the glowing directory + live depth readout fill the frame).
        out[0] = m_shaftX + 0.15f; out[1] = floorY + 1.42f; out[2] = m_shaftZ + 0.22f;
        out[3] = 0.0f;             // face +X (straight at the holo glass)
        out[4] = -0.04f;
    } else if (variant == 4) {
        // THE SPHERE: stand at cab centre and look UP into the wraparound dome + upper-wall
        // band — the rider ENVELOPED in the beat-driven light show.
        out[0] = m_shaftX - 0.10f; out[1] = floorY + 1.35f; out[2] = m_shaftZ + 0.05f;
        out[3] = -1.5708f;       // face -Z (across the cab)
        out[4] = 0.82f;          // tilt UP into the band + domed ceiling (tall cab)
    } else if (variant == 5) {
        // MUSIC-VIDEO GLASS: stand near the +X wall looking across + up at the HERO MV pane
        // on the -X wall (the "music video on glass" fills the frame, layered over the cab).
        out[0] = m_shaftX + m_cabHX - 0.55f; out[1] = floorY + 1.45f; out[2] = m_shaftZ - 0.05f;
        out[3] = 3.14159f;       // face -X (straight at the hero MV pane)
        out[4] = 0.30f;          // tilt up at the pane
    } else if (variant == 6) {
        // CONCERT PA: stand back-centre, low, looking toward the +Z front + up at a hanging
        // line-array firing into the cab (the "micro-concert" PA).
        out[0] = m_shaftX - 0.35f; out[1] = floorY + 1.15f; out[2] = m_shaftZ - m_cabHZ + 0.40f;
        out[3] = 1.15f;          // face +Z, angled toward the +X front line-array
        out[4] = 0.42f;          // tilt up at the hanging array
    } else {
        // Interior beauty: stand in a back corner of the cab looking across the dark-
        // glass interior toward the +X holo wall + accent strips (eye height, slight
        // downward so the glass floor + strata read at the bottom of frame).
        out[0] = m_shaftX - m_cabHX + 0.35f;
        out[1] = floorY + 1.60f;
        out[2] = m_shaftZ - m_cabHZ + 0.35f;
        out[3] = 0.55f;          // yaw toward +X / +Z (the holo + screen corner)
        out[4] = -0.12f;
    }
}

} // namespace x3::game

// ===========================================================================
// Headless self-test (--test-elevator-showcase). Uses the shared headless device +
// a fresh Jolt world; no window/Vulkan. Leak-clean.
// ===========================================================================
#include "headless_device.h"

namespace x3::game {
namespace {
int s_pass = 0, s_fail = 0;
void chk(bool c, const char* n) {
    if (c) { ++s_pass; x3::logInfo(std::string("  [PASS] ") + n); }
    else   { ++s_fail; x3::logError(std::string("  [FAIL] ") + n); }
}
constexpr float kDt = 1.0f/60.0f;
} // namespace

bool runElevatorShowcaseSelfTest() {
    s_pass = s_fail = 0;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessRenderDevice device;
    Scene scene;

    ElevatorShowcase show;
    PlacementSpec spec;   // default tower (club at the bottom)
    bool built = show.build(scene, device, *physics, spec, nullptr);
    const auto& st = show.stats();

    chk(built && show.built(), "S1 showcase builds");
    chk(st.floors >= 5, "S2 multi-floor tower (>=5 floors)");
    chk(st.hasClubStop && std::fabs(st.clubStopY - (ElevatorSystem::kDefaultClubFloorY + 0.18f)) < 0.5f,
        "S3 Club 1127 stop present at Y=-200");
    chk(st.hasDarkGlass && st.hasGlassFloor, "S4 DARK smoked-glass walls + glass floor");
    chk(st.hasHandrail && st.hasHoloPanel && st.hasEntScreen && st.hasVent,
        "S5 handrail + holo panel + entertainment screen + vent");
    chk(st.shaftDoors == 2 * st.floors, "S6 two sliding door leaves per floor");
    chk(st.callPanels == st.floors && st.callButtons == 2 * st.floors,
        "S7 a realistic call-panel keypad (2 buttons) on every floor");
    chk(st.holoButtons == st.floors, "S8 one interior floor-select button per floor");

    // Drive a normal ride up one floor: rider carried, doors animate.
    {
        float feetY = show.cabTopY() + 0.05f, carried = 0.0f;
        int target = show.currentFloorIndex() + 1;
        if (target >= show.stopCount()) target = show.currentFloorIndex() - 1;
        show.callTo(target);
        bool sawClosing=false, sawMoving=false, arrived=false;
        for (int i = 0; i < 6000; ++i) {
            float edy = show.update(kDt, scene, device, *physics);
            if (show.playerRiding(x3::phys::Vec3{0,feetY,0})) { feetY += edy; carried += edy; }
            if (show.state() == ElevState::DoorsClosing) sawClosing = true;
            if (show.moving()) sawMoving = true;
            if (!show.moving() && show.currentFloorIndex() == target && (sawMoving)) { arrived = true; break; }
        }
        chk(sawClosing && sawMoving && arrived, "S9 normal ride: doors close, travel, arrive");
        chk(std::fabs(carried) > 1.0f, "S10 rider carried by the cab");
    }

    // 1127 keypad -> DISCO + descend all the way to Club 1127.
    {
        ElevatorShowcase s2; PlacementSpec sp; s2.build(scene, device, *physics, sp, nullptr);
        s2.keypadDigit(1); s2.keypadDigit(1); s2.keypadDigit(2);
        bool done = s2.keypadDigit(7);
        chk(done && s2.disco(), "S11 code 1127 enables DISCO + queues club descent");
        for (int i = 0; i < 40000 && s2.state() != ElevState::DoorsOpen && s2.state() != ElevState::Idle; ++i)
            s2.update(kDt, scene, device, *physics);
        chk(std::fabs(s2.cabCenter().y - (ElevatorSystem::kDefaultClubFloorY + 0.18f)) < 0.2f,
            "S12 cab descends all the way to Club 1127 (Y=-200)");
    }

    physics->shutdown();
    x3::logInfo("elevshowcase: " + std::to_string(s_pass) + "/" + std::to_string(s_pass+s_fail) + " passed");
    return s_fail == 0;
}

} // namespace x3::game
