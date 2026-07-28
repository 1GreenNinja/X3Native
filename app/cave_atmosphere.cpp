// CAVE / TUNNEL ATMOSPHERE — see cave_atmosphere.h.
#include "cave_atmosphere.h"
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
    return g_fail == 0;
}

} // namespace x3::game
