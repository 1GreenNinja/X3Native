// CAVE / TUNNEL ATMOSPHERE — see cave_atmosphere.h.
#include "cave_atmosphere.h"
#include "cave_river.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {

namespace {
constexpr float kPi = 3.14159265f;

// The crystal-only CAVE look (near-black, a whisper of blue) vs the club's own fill.
constexpr float kCaveAmbR = 0.006f, kCaveAmbG = 0.006f, kCaveAmbB = 0.013f;
constexpr float kCaveIbl  = 0.02f;

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ---- procedural tone WAV (16-bit PCM mono) so the caves SOUND without a pack asset.
// Written to the OS temp dir; loaded by the audio system. `N` samples looped seamlessly.
std::string writeToneWav(const char* name, int sampleRate, int N,
                         float (*gen)(int i, int N, int sr)) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = ".";
    std::string path = std::string(tmp) + "/x3cave_" + name + ".wav";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return std::string();
    auto w32 = [&](uint32_t v){ std::fputc(v&0xFF,f); std::fputc((v>>8)&0xFF,f); std::fputc((v>>16)&0xFF,f); std::fputc((v>>24)&0xFF,f); };
    auto w16 = [&](uint16_t v){ std::fputc(v&0xFF,f); std::fputc((v>>8)&0xFF,f); };
    const uint32_t dataBytes = (uint32_t)N * 2u;
    std::fputs("RIFF", f); w32(36 + dataBytes); std::fputs("WAVE", f);
    std::fputs("fmt ", f); w32(16); w16(1); w16(1);
    w32((uint32_t)sampleRate); w32((uint32_t)sampleRate * 2u); w16(2); w16(16);
    std::fputs("data", f); w32(dataBytes);
    for (int i = 0; i < N; ++i) {
        float s = gen(i, N, sampleRate);
        s = std::max(-1.0f, std::min(1.0f, s));
        w16((uint16_t)(int16_t)std::lround(s * 32000.0f));
    }
    std::fclose(f);
    return path;
}

// A deep, slightly-detuned SUB sine — the club bass felt through the rock.
float genBass(int i, int N, int sr) {
    const float t = (float)i / (float)sr;
    const float f0 = 50.0f;                         // ~50 Hz sub
    float s = 0.85f * std::sin(2.0f * kPi * f0 * t)
            + 0.30f * std::sin(2.0f * kPi * (f0 * 2.0f) * t + 0.6f);
    return s * 0.9f;
    (void)N;
}
// A crystalline HUM — a base tone + a fifth + a slow shimmer (the "singing" crystal).
float genHum(int i, int N, int sr) {
    const float t = (float)i / (float)sr;
    const float base = 196.0f;                      // G3
    const float shimmer = 0.5f + 0.5f * std::sin(2.0f * kPi * 3.0f * t);  // 3 Hz tremolo
    float s = 0.5f * std::sin(2.0f * kPi * base * t)
            + 0.32f * std::sin(2.0f * kPi * (base * 1.5f) * t)            // fifth
            + 0.18f * std::sin(2.0f * kPi * (base * 3.0f) * t + 1.1f);    // airy overtone
    return s * (0.35f + 0.35f * shimmer);
    (void)N;
}
} // namespace

void CaveAtmosphere::configure(const DescentFallLayout& L, float clubMaxX,
                               float clubFloorY, float clubCeilY, float mouthY) {
    m_clubMaxX  = clubMaxX;
    m_clubFloorY = clubFloorY;
    m_clubCeilY = clubCeilY;
    m_mouthY    = mouthY;
    // The landing-room ceiling = the top of the elevator/hall INFRASTRUCTURE; the cave
    // zone (shaft + side-shoots) is everything ABOVE it out east of the club.
    m_shaftBotY = L.roomCeilY;
    m_clubFloorY_a = clubFloorY;
}

void CaveAtmosphere::setClubLook(float ambR, float ambG, float ambB, float iblIntensity) {
    m_ambR = ambR; m_ambG = ambG; m_ambB = ambB; m_ibl = iblIntensity;
}

CaveAtmosphere::State CaveAtmosphere::classify(float x, float y) const {
    State st;
    st.inCave = (x > m_clubMaxX + 6.0f) &&
                (y > m_shaftBotY - 0.5f) &&
                (y < m_mouthY + 2.0f);
    if (st.inCave) {
        const float span = std::max(1.0f, m_mouthY - m_clubFloorY);
        st.depth01 = std::clamp((m_mouthY - y) / span, 0.0f, 1.0f);
    }
    return st;
}

CaveAtmosphere::State CaveAtmosphere::update(float dt, const x3::phys::Vec3& cam,
                                             x3::rhi::IRenderDevice& device) {
    State st = classify(cam.x, cam.y);
    const float target = st.inCave ? 1.0f : 0.0f;
    if (m_first) { m_blend = target; m_first = false; }
    else         { m_blend += (target - m_blend) * std::clamp(dt * 6.0f, 0.0f, 1.0f); }
    st.blend = m_blend;

    // #1 CRYSTAL-ONLY: fade the club ambient + IBL toward near-black in the caves.
    device.setAmbient(lerpf(m_ambR, kCaveAmbR, m_blend),
                      lerpf(m_ambG, kCaveAmbG, m_blend),
                      lerpf(m_ambB, kCaveAmbB, m_blend));
    device.setIblIntensity(lerpf(m_ibl, kCaveIbl, m_blend));

    // #5 FOG: a dark-blue cavern haze so the crystal light throws shafts through it.
    x3::rhi::IRenderDevice::FogParams fog;
    if (m_blend > 0.12f) {
        fog.enabled = true;
        fog.color[0] = 0.010f; fog.color[1] = 0.030f; fog.color[2] = 0.11f;   // deep crystal-blue haze
        fog.density  = 0.011f * m_blend;
        fog.start    = 1.2f;
        fog.maxOpacity = 0.72f;
    } else {
        fog.enabled = false;
    }
    device.setFog(fog);
    return st;
}

float CaveAtmosphere::crystalPulse(float beatThump, float depth01) {
    const float t = std::clamp(beatThump, 0.0f, 1.0f);
    const float d = std::clamp(depth01, 0.0f, 1.0f);
    return 1.0f + 0.55f * t * (0.35f + 0.65f * d);
}

bool CaveAtmosphere::isCrystal(const x3::rhi::PointLight& l) {
    return l.color[2] > 0.8f &&
           l.color[2] > l.color[0] * 2.0f &&
           l.color[2] > l.color[1] * 1.3f;
}

// ---------------------------------------------------------------------------
// AUDIO (live path).
// ---------------------------------------------------------------------------
void CaveAtmosphere::bindAudio(x3::audio::IAudioSystem* audio, float clubFloorY,
                               float shaftX, float shaftZ) {
    m_audio = audio;
    m_clubFloorY_a = clubFloorY; m_shaftX = shaftX; m_shaftZ = shaftZ;
    if (!m_audio) return;
    const int sr = 44100;
    const std::string bp = writeToneWav("bass", sr, sr, &genBass);   // 1 s seamless loop
    const std::string hp = writeToneWav("hum",  sr, sr, &genHum);
    if (!bp.empty()) m_bassSnd = m_audio->load(bp);
    if (!hp.empty()) m_humSnd  = m_audio->load(hp);
    x3::logInfo("[cave-atmos] audio bound (bass-from-below + singing-crystal hum tones generated)");
}

void CaveAtmosphere::updateAudio(const x3::phys::Vec3& cam, const State& st, float beatThump) {
    if (!m_audio) return;
    // #2 BASS FROM BELOW: a sub emitter at the club floor whose loudness rises with
    // DEPTH and PULSES on the beat — the party throbbing up through the rock. Only
    // while in the caves (in the club, the real music owns the low end).
    if (st.inCave && m_bassSnd.valid()) {
        const float vol = std::clamp(st.depth01, 0.0f, 1.0f) * (0.45f + 0.55f * beatThump);
        if (!m_bassLoop.valid())
            m_bassLoop = m_audio->startLoop3D(m_bassSnd, m_shaftX, m_clubFloorY_a + 2.0f, m_shaftZ, vol, 1.0f);
        else
            m_audio->setLoopParams(m_bassLoop, vol, 1.0f);
    } else if (m_bassLoop.valid()) {
        m_audio->stopLoop(m_bassLoop); m_bassLoop = x3::audio::LoopHandle{};
    }

    // SINGING CRYSTALS: a resonant hum whose PITCH shifts with depth (a different
    // resonance as you move down past the veins) + a soft breathe on the beat. One
    // shared emitter mid-shaft (an approximation of per-crystal resonance).
    if (st.inCave && m_humSnd.valid()) {
        const float pitch = 0.85f + 0.5f * st.depth01;
        const float vol   = 0.22f + 0.10f * beatThump;
        if (!m_humLoop.valid())
            m_humLoop = m_audio->startLoop3D(m_humSnd, m_shaftX, cam.y, m_shaftZ, vol, pitch);
        else
            m_audio->setLoopParams(m_humLoop, vol, pitch);
    } else if (m_humLoop.valid()) {
        m_audio->stopLoop(m_humLoop); m_humLoop = x3::audio::LoopHandle{};
    }

    // CAVE REVERB: a long RT60 while in the caves (the singing caves echo). Applies to
    // 3D one-shots routed through the reverb insert; ambient loops sit on the plain
    // spatializer path, so this is the echo for footsteps/impacts, not the hum bed.
    m_audio->setReverbParams(st.inCave ? 2.6f : 0.0f, st.inCave ? 0.34f : 0.0f);
}

void CaveAtmosphere::shutdownAudio() {
    if (!m_audio) return;
    if (m_bassLoop.valid()) m_audio->stopLoop(m_bassLoop);
    if (m_humLoop.valid())  m_audio->stopLoop(m_humLoop);
    m_bassLoop = x3::audio::LoopHandle{};
    m_humLoop  = x3::audio::LoopHandle{};
}

// ---------------------------------------------------------------------------
// Self-test (--test-caveatmos): classifier + pulse + fog band logic (no device sound).
// ---------------------------------------------------------------------------
namespace { int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("[cave-atmos-test] PASS ") + what); }
    else    { ++g_fail; x3::logError(std::string("[cave-atmos-test] FAIL ") + what); }
}}

bool CaveAtmosphere::runSelfTest() {
    g_pass = g_fail = 0;

    // A representative club/descent layout (club floor -800; landing room 12 m above).
    DescentFallLayout L;
    L.roomCeilY = -782.0f;
    CaveAtmosphere ca;
    ca.configure(L, /*clubMaxX*/15.24f, /*clubFloorY*/-800.0f, /*clubCeilY*/-790.86f, /*mouthY*/-3.0f);
    ca.setClubLook(0.024f, 0.019f, 0.040f, 0.20f);

    // A1: a point in the club (x=0) is NOT cave.
    check(!ca.classify(0.0f, -795.0f).inCave, "A1 inside the club is NOT the cave zone");
    // A2: mid-shaft (east of club, above the landing infra) IS cave, mid depth.
    {
        auto s = ca.classify(37.0f, -400.0f);
        check(s.inCave && s.depth01 > 0.3f && s.depth01 < 0.7f,
              "A2 mid-shaft is the cave zone at mid depth");
    }
    // A3: the shaft mouth is cave, shallow (depth ~0).
    check(ca.classify(37.0f, -5.0f).inCave && ca.classify(37.0f, -5.0f).depth01 < 0.05f,
          "A3 the mouth is the cave zone, near the surface (depth ~0)");
    // A4: deep near the club is cave, depth ~1.
    check(ca.classify(50.0f, -770.0f).depth01 > 0.9f,
          "A4 deep near the club is the DEEPEST cave (depth ~1 — where the bass is loudest)");
    // A5: the elevator/hall infra (below the landing ceiling) is NOT the crystal cave.
    check(!ca.classify(30.0f, -800.0f).inCave, "A5 the elevator/hall infra is NOT the crystal-only cave");

    // Pulse: monotone in beat + deeper = stronger; rest = 1.
    check(std::fabs(crystalPulse(0.0f, 1.0f) - 1.0f) < 1e-4f, "B1 crystals rest at 1.0 between beats");
    check(crystalPulse(1.0f, 1.0f) > crystalPulse(1.0f, 0.0f),
          "B2 the beat pulse is STRONGER the deeper you are (bass rising from below)");
    check(crystalPulse(1.0f, 0.5f) > crystalPulse(0.2f, 0.5f),
          "B3 the crystals brighten toward the KICK");

    // isCrystal: blue Salvari lights pulse; warm worklights don't.
    x3::rhi::PointLight blue; blue.color[0]=0.12f; blue.color[1]=0.34f; blue.color[2]=1.90f;
    x3::rhi::PointLight warm; warm.color[0]=1.25f; warm.color[1]=1.00f; warm.color[2]=0.75f;
    check(isCrystal(blue) && !isCrystal(warm),
          "B4 blue Salvari crystals pulse; warm worklights do not");

    // update(): ambient/IBL/fog blend snaps to the cave look in the cave, club out.
    HeadlessRenderDevice dev;
    auto sc = ca.update(1.0f / 60.0f, x3::phys::Vec3{37.0f, -400.0f, 4.0f}, dev);
    check(sc.inCave && sc.blend > 0.9f, "C1 first cave frame snaps to crystal-only (blend->1)");
    // Step back into the club: blend eases back toward the club look.
    for (int i = 0; i < 240; ++i) ca.update(1.0f / 60.0f, x3::phys::Vec3{0.0f, -795.0f, 0.0f}, dev);
    auto scl = ca.update(1.0f / 60.0f, x3::phys::Vec3{0.0f, -795.0f, 0.0f}, dev);
    check(!scl.inCave && scl.blend < 0.1f, "C2 back in the club the crystal-only fade RELEASES (ambient restored)");

    x3::logInfo("caveatmos: " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");

    // The UNDERGROUND RIVER folds its own build/emissive/flow assertions into this gate
    // (feat/cave-river) so --test-caveatmos green also proves the river geometry builds,
    // is MILD emissive-blue, and flows without blowing out.
    const bool riverOk = CaveRiver::runSelfTest();
    if (!riverOk) ++g_fail;

    return g_fail == 0;
}

// ===========================================================================
// UNDERGROUND RIVER (feat/cave-river) — the self-emissive blue stream. See
// cave_river.h. Implemented HERE (in the already-compiled cave-atmosphere TU) so no
// new build-system entry is needed; conceptually it is the water half of the cave
// atmosphere.
// ===========================================================================
namespace {

// A subtle blue water-mottle albedo so the surface catches the pool bank-lights with a
// hint of ripple detail (self-contained; no pack asset). Deep-blue, low contrast.
std::vector<uint8_t> makeWaterRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    auto hash = [](int x, int y) {
        uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            // two octaves of value noise -> a faint caustic-ish shimmer
            const float u = (float)x / n, v = (float)y / n;
            const float a = hash((int)(u * 8) & 7, (int)(v * 8) & 7);
            const float b = hash((int)(u * 24) & 23, (int)(v * 24) & 23);
            const float g = 0.6f * a + 0.4f * b;          // 0..1
            auto c8 = [](float f) { return (uint8_t)(f < 0 ? 0 : f > 1 ? 255 : f * 255.0f + 0.5f); };
            const uint32_t i = (y * n + x) * 4;
            px[i + 0] = c8(0.02f + 0.05f * g);            // R low
            px[i + 1] = c8(0.10f + 0.14f * g);            // G mid-low
            px[i + 2] = c8(0.45f + 0.40f * g);            // B dominant
            px[i + 3] = 255;
        }
    return px;
}

// One double-sided flat water-ribbon quad between two river nodes (world-space verts;
// the Y column bobs for ripple in update()).
x3::rhi::MeshHandle makeRibbonSeg(x3::rhi::IRenderDevice& device,
                                  const CaveRiverNode& a, const CaveRiverNode& b,
                                  float uA, float uB) {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> idx;
    v.reserve(8); idx.reserve(12);
    auto push = [&](float x, float y, float z, float u, float vv, float ny) {
        x3::rhi::MeshVertex mv;
        mv.pos[0] = x; mv.pos[1] = y; mv.pos[2] = z;
        mv.normal[0] = 0.0f; mv.normal[1] = ny; mv.normal[2] = 0.0f;
        mv.uv[0] = u; mv.uv[1] = vv;
        v.push_back(mv);
    };
    // Width runs PERPENDICULAR to the segment (W-UNDERRIVER: the club_bedrock
    // tubes all run along +X, where perp == the old z±halfWidth exactly — the
    // caves render byte-identically — but the open-world underground river
    // BENDS, and an axis-aligned ribbon leaves wedge gaps at every bend).
    float dx = b.x - a.x, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-4f) { dx /= len; dz /= len; } else { dx = 1.0f; dz = 0.0f; }
    const float px = -dz, pz = dx;   // left-hand perp in XZ
    // TOP face (normal +Y) — aL,aR,bL,bR.
    push(a.x - px * a.halfWidth, a.y, a.z - pz * a.halfWidth, uA, 0.0f, 1.0f);   // 0
    push(a.x + px * a.halfWidth, a.y, a.z + pz * a.halfWidth, uA, 1.0f, 1.0f);   // 1
    push(b.x - px * b.halfWidth, b.y, b.z - pz * b.halfWidth, uB, 0.0f, 1.0f);   // 2
    push(b.x + px * b.halfWidth, b.y, b.z + pz * b.halfWidth, uB, 1.0f, 1.0f);   // 3
    idx.insert(idx.end(), { 0, 2, 3,  0, 3, 1 });
    // BOTTOM copy (normal -Y, reversed winding) — visible from below too (noclip-proof).
    const uint32_t b2 = (uint32_t)v.size();
    push(a.x - px * a.halfWidth, a.y, a.z - pz * a.halfWidth, uA, 0.0f, -1.0f);
    push(a.x + px * a.halfWidth, a.y, a.z + pz * a.halfWidth, uA, 1.0f, -1.0f);
    push(b.x - px * b.halfWidth, b.y, b.z - pz * b.halfWidth, uB, 0.0f, -1.0f);
    push(b.x + px * b.halfWidth, b.y, b.z + pz * b.halfWidth, uB, 1.0f, -1.0f);
    idx.insert(idx.end(), { b2 + 0, b2 + 3, b2 + 2,  b2 + 0, b2 + 1, b2 + 3 });
    return device.createMesh(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
}

} // namespace

int CaveRiver::build(Scene& scene, x3::rhi::IRenderDevice& device,
                     const std::vector<CaveRiverNode>& nodes,
                     std::vector<x3::rhi::PointLight>* outLights) {
    m_segs.clear();
    m_flow = 0.0f;
    if (nodes.size() < 2) return 0;

    // One shared subtle water-mottle albedo (reused by every ribbon segment).
    const uint32_t N = 64;
    const std::vector<uint8_t> wpx = makeWaterRGBA(N);
    m_tex = device.createTexture(wpx.data(), N, N, true);

    // Cumulative along-river length (breaks contribute 0 — no bridge across two tubes),
    // for the flow phase s01 + the streamed UV.
    std::vector<float> cum(nodes.size(), 0.0f);
    float total = 0.0f;
    for (size_t i = 1; i < nodes.size(); ++i) {
        float d = 0.0f;
        if (!nodes[i - 1].breakSeg) {
            const float dx = nodes[i].x - nodes[i - 1].x;
            const float dz = nodes[i].z - nodes[i - 1].z;
            d = std::sqrt(dx * dx + dz * dz);
        }
        total += d;
        cum[i] = total;
    }
    if (total < 1e-3f) total = 1.0f;

    // MILD deep-electric-blue: emissive ratio is blue-dominant, and the animated strength
    // (below) keeps blue*strength well under 1.0 so ACES holds the hue (a gentle glow, not
    // a blinding white strip). baseColor is a deep blue the pool lights catch.
    const float blueBase[4] = { 0.03f, 0.10f, 0.42f, 1.0f };
    const float blueEmis[3] = { 0.05f, 0.14f, 1.00f };

    int added = 0;
    for (size_t i = 1; i < nodes.size(); ++i) {
        const CaveRiverNode& a = nodes[i - 1];
        const CaveRiverNode& b = nodes[i];
        if (a.breakSeg) continue;                        // gap between disconnected tubes
        const float uA = cum[i - 1] / total * 12.0f, uB = cum[i] / total * 12.0f;
        Entity e;
        e.mesh = makeRibbonSeg(device, a, b, uA, uB);
        e.tex  = m_tex;
        // RUSH (whitewater): blend colour + emissive toward churned foam.
        // rush 0 (every pre-existing cave river) is bit-identical blue.
        const float rush = 0.5f * (a.rush + b.rush);
        auto lerp = [](float p, float q, float t) { return p + (q - p) * t; };
        e.baseColor[0] = lerp(blueBase[0], 0.42f, rush);
        e.baseColor[1] = lerp(blueBase[1], 0.55f, rush);
        e.baseColor[2] = lerp(blueBase[2], 0.62f, rush);
        e.baseColor[3] = blueBase[3];
        const float em = 0.5f * (a.emissive + b.emissive);
        e.emissive[0] = lerp(blueEmis[0], 0.55f, rush);
        e.emissive[1] = lerp(blueEmis[1], 0.72f, rush);
        e.emissive[2] = lerp(blueEmis[2], 0.95f, rush);
        e.emissive[3] = em;
        e.tag  = (uint32_t)Tag::Static;
        e.body = x3::phys::BodyId{};                     // purely visual (water floats over the floor)
        const uint32_t id = scene.add(e);
        ++added;

        Seg sg;
        sg.id = id;
        sg.s01 = cum[i - 1] / total;
        sg.baseEmis = em;
        sg.pool = a.pool || b.pool;
        sg.rush = rush;
        m_segs.push_back(sg);

        // POOL bank light — a DIM blue glow over the pool that softly lights the rock
        // banks, pushed into the SAME distance-culled crystal/bedrock channel the Salvari
        // crystals use (host cull @50 m, 64-light cap). A handful total (2 per tube).
        if (b.pool && outLights) {
            x3::rhi::PointLight l;
            l.pos[0] = b.x; l.pos[1] = b.y + 0.5f; l.pos[2] = b.z;
            l.range  = 12.0f;
            l.color[0] = 0.10f; l.color[1] = 0.22f; l.color[2] = 0.85f;   // dim, cool, blue-dominant
            outLights->push_back(l);
        }
    }
    return added;
}

void CaveRiver::update(float dt, Scene& scene) {
    if (m_segs.empty()) return;
    m_flow += dt * 1.7f;                                 // dt-scaled downstream flow rate
    const float kTwoPi = 6.28318531f;
    for (const Seg& s : m_segs) {
        Entity& e = scene.get(s.id);
        const float phase = s.s01 * 3.0f * kTwoPi;       // ~3 crests spread along the river
        float strength;
        if (s.pool) {
            // Pools BREATHE slower + a touch brighter (a settled shimmer, not a crest).
            strength = s.baseEmis * (1.02f + 0.10f * std::sin(m_flow * 0.6f + s.s01 * 5.0f));
        } else {
            // A bright CREST travels DOWNSTREAM (increasing s01) as m_flow rises, + a
            // faster micro-shimmer so it reads as living water, not a sliding gradient.
            // RUSH segments run the crest ~3x faster with a deeper churn swing —
            // whitewater at the drops (rush 0 = the historic motion, bit-identical).
            const float rf      = 1.0f + 2.2f * s.rush;
            const float crest   = 0.5f + 0.5f * std::sin(phase - m_flow * rf);
            const float shimmer = 0.90f + (0.10f + 0.10f * s.rush) *
                                  std::sin(phase * (2.3f + 2.1f * s.rush) - m_flow * 1.7f * rf);
            strength = s.baseEmis * (0.80f + (0.34f + 0.30f * s.rush) * crest) * shimmer;
        }
        e.emissive[3] = strength;
        // Gentle ripple BOB on the surface Y (verts are world-space; the translation
        // column oscillates the whole segment). Kept LOW-amplitude + LOW-frequency so
        // adjacent segments (which share an edge) stay within ~2 mm of each other — a
        // living undulation with NO visible seam/step between ribbon quads.
        e.transform[13] = 0.012f * std::sin(s.s01 * 4.5f - m_flow * 1.1f);
    }
}

bool CaveRiver::runSelfTest() {
    int pass = 0, fail = 0;
    auto chk = [&](bool ok, const char* w) {
        if (ok) { ++pass; x3::logInfo(std::string("[cave-river-test] PASS ") + w); }
        else    { ++fail; x3::logError(std::string("[cave-river-test] FAIL ") + w); }
    };

    // A tiny river: one 5-node run (belly pool at 2, dead-end pool at 4, break at 4).
    std::vector<CaveRiverNode> nodes;
    for (int i = 0; i < 5; ++i) {
        CaveRiverNode n;
        n.x = 40.0f + (float)i * 2.2f;
        n.y = -560.0f + 0.05f;
        n.z = 4.0f;
        n.halfWidth = (i == 2) ? 3.0f : 0.9f;
        n.emissive  = (i == 2 || i == 4) ? 0.40f : 0.30f;
        n.pool      = (i == 2 || i == 4);
        n.breakSeg  = (i == 4);
        nodes.push_back(n);
    }

    HeadlessRenderDevice dev;
    Scene scene;
    std::vector<x3::rhi::PointLight> lights;
    CaveRiver river;
    const int added = river.build(scene, dev, nodes, &lights);
    chk(added >= 3 && river.built(), "R1 the water ribbon builds segments from the node polyline");

    // MILD emissive BLUE: blue-dominant, and the final blue add (ratio*strength) held
    // well under 1.0 so it's a gentle glow, not a blinding white strip.
    bool blueMild = (scene.size() > 0);
    for (uint32_t id = 0; id < scene.size(); ++id) {
        const Entity& e = scene.get(id);
        if (e.emissive[3] <= 0.0f) { blueMild = false; break; }
        if (!(e.emissive[2] > e.emissive[0] && e.emissive[2] > e.emissive[1])) blueMild = false;
        if (e.emissive[2] * e.emissive[3] > 0.7f) blueMild = false;   // mild peak
    }
    chk(blueMild, "R2 water is MILD emissive BLUE (blue-dominant, final add < ~0.7, not a white strip)");

    // A handful of DIM, short-range, blue bank lights over the pools.
    bool banks = !lights.empty() && lights.size() <= 8;
    for (const auto& l : lights) {
        if (l.range > 16.0f) banks = false;
        if (!(l.color[2] > l.color[0] && l.color[2] < 1.2f)) banks = false;
    }
    chk(banks, "R3 a HANDFUL of dim short-range blue bank lights light the pools");

    // FLOW animates the surface (a non-pool stream segment's emissive crest MOVES + the
    // ripple bobs) and stays MILD.
    const float e0 = scene.get(0).emissive[3];           // entity 0 = first (non-pool) stream seg
    for (int k = 0; k < 20; ++k) river.update(1.0f / 60.0f, scene);
    const float e1 = scene.get(0).emissive[3];
    const float bob = scene.get(0).transform[13];
    chk(std::fabs(e1 - e0) > 1e-4f && e1 > 0.0f && e1 < 0.7f && std::fabs(bob) < 0.06f,
        "R4 flow animates (crest moves, surface bobs) and stays mild");

    x3::logInfo("caveriver: " + std::to_string(pass) + "/" +
                std::to_string(pass + fail) + " passed");
    return fail == 0;
}

} // namespace x3::game
