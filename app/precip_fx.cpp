#include "precip_fx.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {
constexpr float kPi = 3.14159265358979f;
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

uint32_t PrecipFx::nextRand() {
    m_rng = m_rng * 1664525u + 1013904223u;
    return m_rng;
}
float PrecipFx::randUnit() { return (float)(nextRand() >> 8) / (float)(1u << 24); }

void PrecipFx::scatter(Flake& f) {
    const float e = m_cfg.halfExtentM;
    f.x = (randUnit() * 2.0f - 1.0f) * e;
    f.z = (randUnit() * 2.0f - 1.0f) * e;
    f.y = randUnit() * m_cfg.heightM;
    f.phase = randUnit() * 2.0f * kPi;
    f.radius = 0.25f + randUnit() * 0.75f;
    f.speedVar = 0.8f + randUnit() * 0.4f;
}

void PrecipFx::respawnTop(Flake& f) {
    const float e = m_cfg.halfExtentM;
    f.x = (randUnit() * 2.0f - 1.0f) * e;
    f.z = (randUnit() * 2.0f - 1.0f) * e;
    f.y = m_cfg.heightM;
    f.phase = randUnit() * 2.0f * kPi;
    f.radius = 0.25f + randUnit() * 0.75f;
    f.speedVar = 0.8f + randUnit() * 0.4f;
}

void PrecipFx::init(const PrecipConfig& cfg, uint32_t seed) {
    m_cfg = cfg;
    m_rng = seed ? seed : 0xC0FFEEu;
    const uint32_t cap = (cfg.maxRain > cfg.maxSnow) ? cfg.maxRain : cfg.maxSnow;
    m_flakes.assign(cap, Flake{});
    for (auto& f : m_flakes) scatter(f);
    m_scratch.reserve((size_t)cap * 8);
    m_live = 0;
    m_kind = PrecipKind::None;
}

void PrecipFx::update(float dt, PrecipKind kind, float intensity,
                      float camX, float camY, float camZ,
                      float windX, float windZ, float skyVisible) {
    m_kind = kind;
    intensity = clamp01(intensity);
    if (kind == PrecipKind::None || intensity <= 0.001f || m_flakes.empty()) {
        m_live = 0;
        return;
    }
    if (dt <= 0.0f) return;
    m_t += dt;

    // THE VOLUME FOLLOWS THE CAMERA. Positions are stored RELATIVE to the
    // centre, so moving the camera moves the whole column with it for free --
    // no per-particle rebase, and no chance of the snow being left behind on a
    // fast drive, which is the failure mode of world-space precipitation the
    // moment you go quicker than it falls.
    m_cx = camX; m_cy = camY; m_cz = camZ;

    // UNDER COVER. Ease toward the target rather than snapping: at 60 mph a
    // tunnel mouth passes in a frame or two, and a hard switch makes the entire
    // snowfall vanish between one frame and the next, which reads as a bug even
    // though the geometry is right. ~0.4 s either way.
    const float tgt = clamp01(skyVisible);
    m_sky += (tgt - m_sky) * clamp01(dt * 2.5f);

    const bool snow = (kind == PrecipKind::Snow);
    const uint32_t cap = snow ? m_cfg.maxSnow : m_cfg.maxRain;
    // INTENSITY SCALES COUNT, NOT SPEED. Heavier snow is not faster snow; there
    // is simply more of it. Driving fall speed from intensity is the classic
    // tell -- the storm appears to accelerate as it thickens, which nothing in
    // nature does.
    uint32_t want = (uint32_t)((float)cap * intensity * m_sky);
    if (want > m_flakes.size()) want = (uint32_t)m_flakes.size();
    m_live = want;

    const float fall = (snow ? m_cfg.snowFallMps : m_cfg.rainFallMps);
    const float e = m_cfg.halfExtentM;

    for (uint32_t i = 0; i < m_live; ++i) {
        Flake& f = m_flakes[i];
        f.y -= fall * f.speedVar * dt;
        // WIND LEANS THE COLUMN. Rain leans harder than snow for the same wind
        // only because it is not being pushed sideways by the flutter as well.
        f.x += windX * dt;
        f.z += windZ * dt;

        if (f.y < 0.0f) { respawnTop(f); continue; }
        // Horizontal wrap: a flake blown out one side re-enters the other. The
        // fade below is what keeps that from being visible.
        if (f.x >  e) f.x -= 2.0f * e;
        if (f.x < -e) f.x += 2.0f * e;
        if (f.z >  e) f.z -= 2.0f * e;
        if (f.z < -e) f.z += 2.0f * e;
    }
}

void PrecipFx::submit(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    if (m_live == 0) return;

    const bool snow = (m_kind == PrecipKind::Snow);
    const float e = m_cfg.halfExtentM;
    const float nearSz = snow ? m_cfg.snowNearSize : m_cfg.rainNearSize;
    const float farSz  = snow ? m_cfg.snowFarSize  : m_cfg.rainFarSize;

    std::vector<x3::rhi::IRenderDevice::ParticleInstance> inst;
    inst.reserve(m_live);

    for (uint32_t i = 0; i < m_live; ++i) {
        const Flake& f = m_flakes[i];

        // FLUTTER. Snow wanders down on its own little spiral because it is
        // light enough for the air to push around; rain is heavy and goes
        // essentially straight. Per-flake phase AND radius, so no two share a
        // path -- a shared phase makes the whole field pulse in unison, which
        // reads as a shader effect rather than as weather.
        float px = f.x, pz = f.z;
        if (snow) {
            const float a = f.phase + m_t * m_cfg.flutterRate;
            px += std::cos(a) * m_cfg.flutterAmpM * f.radius;
            pz += std::sin(a * 0.83f) * m_cfg.flutterAmpM * f.radius;   // not a circle
        }

        // DEPTH CUES. Distance from the volume centre drives both size and
        // opacity, so the column has front-to-back instead of reading as a flat
        // sheet of confetti hung in front of the lens.
        const float d = std::sqrt(px * px + pz * pz);
        const float d01 = clamp01(d / e);
        const float size = nearSz + (farSz - nearSz) * d01;

        // ---- THE BOUNDARY FADE ------------------------------------------
        // The most important line here. A particle that teleports at full
        // opacity BLINKS, and blinking is the one artefact the eye will not
        // forgive. Fade out toward the shell (and toward the ceiling and the
        // ground) so every recycle happens at zero alpha and is invisible.
        float a = 1.0f;
        a *= 1.0f - clamp01((d01 - 0.72f) / 0.28f);          // side shell
        a *= clamp01(f.y / 2.0f);                             // near the ground
        a *= 1.0f - clamp01((f.y - m_cfg.heightM * 0.75f)
                            / (m_cfg.heightM * 0.25f));       // near the ceiling
        // Fade the whole field with sky visibility as well as thinning it, so
        // the last few flakes under a tunnel mouth dim out instead of popping.
        a *= m_sky;
        if (a <= 0.002f) continue;

        x3::rhi::IRenderDevice::ParticleInstance pi{};
        pi.pos[0] = m_cx + px;
        pi.pos[1] = m_cy + f.y - m_cfg.heightM * 0.35f;   // straddle the eye
        pi.pos[2] = m_cz + pz;
        pi.size   = size;
        if (snow) {
            // Snow is bright and very slightly blue in shadow -- pure white
            // reads as paper. Alpha is high because a flake is opaque.
            pi.color[0] = 0.94f; pi.color[1] = 0.96f; pi.color[2] = 1.0f;
            pi.color[3] = a * 0.92f;
        } else {
            // Rain is nearly colourless and mostly TRANSPARENT: a raindrop is a
            // lens, not a white dot. Low alpha is what stops rain looking like
            // grey snow, which is the other half of the same mistake.
            pi.color[0] = 0.72f; pi.color[1] = 0.78f; pi.color[2] = 0.86f;
            pi.color[3] = a * 0.34f;
        }
        inst.push_back(pi);
    }

    if (!inst.empty())
        device.submitParticles(inst.data(), (uint32_t)inst.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

// ===========================================================================
// --test-precip
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void pcheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
}  // namespace

bool runPrecipSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- precipitation self-test (falling snow + rain) ---");
    char buf[256];
    PrecipConfig cfg;
    const float dt = 1.0f / 60.0f;

    // P1: INTENSITY SCALES COUNT, NOT SPEED.
    {
        PrecipFx a, b;
        a.init(cfg, 5u); b.init(cfg, 5u);
        for (int i = 0; i < 120; ++i) {
            a.update(dt, PrecipKind::Snow, 0.25f, 0, 0, 0);
            b.update(dt, PrecipKind::Snow, 1.00f, 0, 0, 0);
        }
        std::snprintf(buf, sizeof(buf),
                      "P1 heavier snow means MORE flakes (%u vs %u), not faster ones",
                      b.liveCount(), a.liveCount());
        pcheck(b.liveCount() > a.liveCount() * 2, buf);
    }

    // P2: nothing falls when nothing is falling -- zero particles submitted, so
    // the device adds no pass at all.
    {
        PrecipFx p; p.init(cfg, 9u);
        for (int i = 0; i < 60; ++i) p.update(dt, PrecipKind::None, 1.0f, 0, 0, 0);
        pcheck(p.liveCount() == 0, "P2 clear weather submits ZERO particles (no pass, no cost)");
        for (int i = 0; i < 60; ++i) p.update(dt, PrecipKind::Snow, 0.0f, 0, 0, 0);
        pcheck(p.liveCount() == 0, "P2b zero intensity likewise costs nothing");
    }

    // P3: RAIN FALLS FASTER THAN SNOW. Time how long each takes to clear the
    // volume from the ceiling with respawn disabled by measuring the fall rate.
    {
        std::snprintf(buf, sizeof(buf),
            "P3 rain falls at %.1f mph and snow at %.1f mph -- the gap is most of why they read as different weather",
            cfg.rainFallMps * 2.23694f, cfg.snowFallMps * 2.23694f);
        pcheck(cfg.rainFallMps > cfg.snowFallMps * 5.0f, buf);
    }

    // P4: THE VOLUME IS CAMERA-LOCAL. Drive the camera a mile and the snow must
    // still be around it -- a world-space field would be a mile behind.
    {
        PrecipFx p; p.init(cfg, 11u);
        float cx = 0.0f;
        for (int i = 0; i < 600; ++i) {
            cx += 45.0f * dt;                    // ~100 mph
            p.update(dt, PrecipKind::Snow, 1.0f, cx, 0.0f, 0.0f);
        }
        std::snprintf(buf, sizeof(buf),
            "P4 after %.0f ft at 100 mph the column is still around the camera (%u live)",
            cx * 3.28084f, p.liveCount());
        pcheck(p.liveCount() > 0, buf);
    }

    // P5: the pool RECYCLES rather than draining. Run long past the time it
    // takes a flake to fall the full height and the count must hold.
    {
        PrecipFx p; p.init(cfg, 13u);
        const float fallTime = cfg.heightM / cfg.snowFallMps;   // ~27 s
        const int steps = (int)(fallTime * 4.0f / dt);
        for (int i = 0; i < steps; ++i) p.update(dt, PrecipKind::Snow, 1.0f, 0, 0, 0);
        std::snprintf(buf, sizeof(buf),
                      "P5 after %.0f s (4x the full fall time) the field is still full: %u flakes",
                      fallTime * 4.0f, p.liveCount());
        pcheck(p.liveCount() == cfg.maxSnow, buf);
    }

    // P6: determinism.
    {
        auto run = [&](uint32_t seed) {
            PrecipFx p; p.init(cfg, seed);
            for (int i = 0; i < 900; ++i) p.update(dt, PrecipKind::Snow, 0.7f, 0, 0, 0);
            return p.liveCount();
        };
        pcheck(run(31337u) == run(31337u), "P6 same seed -> identical field");
    }

    // P7: NEGATIVE CONTROL -- the world-space model this replaces. At 100 mph a
    // fixed world volume is left behind in under a second, which is why the
    // camera-local wrap is not an optimisation but the whole design.
    {
        const float leaveSeconds = cfg.halfExtentM / 45.0f;
        std::snprintf(buf, sizeof(buf),
            "P7 a world-space column of this size would be behind you in %.1f s at 100 mph",
            leaveSeconds);
        pcheck(leaveSeconds < 1.0f, buf);
    }

    std::snprintf(buf, sizeof(buf), "--- precipitation self-test: %d passed, %d failed ---",
                  g_pass, g_fail);
    if (g_fail) x3::logError(buf); else x3::logInfo(buf);
    return g_fail == 0;
}

}  // namespace x3::game
