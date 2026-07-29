// ECHO WATER V2 "LIVING BAY" — see echo_water.h for ownership/integration notes.
#include "echo_water.h"

#include <cmath>
#include <algorithm>

namespace x3::game {

namespace {
// Unit XZ direction from (dx,dz). water.vert normalizes its 4 wave directions
// at shader-compile-time constants via normalize(vec2(...)) — this reproduces
// that exactly for the same 4 literal pairs (water.vert L88-92).
struct Dir2 { float x, z; };
inline Dir2 normDir(float dx, float dz) {
    const float L = std::sqrt(dx * dx + dz * dz);
    return Dir2{ dx / L, dz / L };
}

// The exact 4-wave table from shaders/water.vert L87-94 (N, dirs[], lenMul[],
// ampMul[]). Kept as functions-of-nothing (not file-scope statics) so this TU
// has no static-init-order dependency; the compiler trivially constant-folds
// these at -O1+, and at -O0 it is 4 sqrt() calls per invocation which is fine
// for gameplay-rate (not per-vertex) sampling.
constexpr int kWaveN = 4;
inline void wave_dirs(Dir2 out[kWaveN]) {
    out[0] = normDir( 1.0f,  0.25f);
    out[1] = normDir(-0.6f,  0.8f);
    out[2] = normDir( 0.2f, -1.0f);
    out[3] = normDir(-0.9f, -0.35f);
}
constexpr float kLenMul[kWaveN] = { 1.0f, 0.55f, 0.32f, 0.18f };
constexpr float kAmpMul[kWaveN] = { 1.0f, 0.5f,  0.28f, 0.14f };

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

// ---------------------------------------------------------------------------
// echoWaveHeight — see echo_water.h. Mirrors water.vert L69-107's Y-only sum:
//   for each of the 4 waves: w = 2*pi/L; A = amp*ampMul[i];
//   phi = sqrt(9.81*w)*speed; y += A * sin(w*dot(dir,basePos) + phi*t)
// (the shader also accumulates disp.x/disp.z and dPdx/dPdz for the horizontal
// pinch + analytic normal; both are display-only and don't change the surface
// HEIGHT at a given XZ, so gameplay sampling skips them — see the header note).
float echoWaveHeight(float x, float z, float t, const WaterTuning& tune) {
    const float amp     = tune.amplitude;
    const float baseLen = std::max(tune.waveLength, 0.5f);   // water.vert L74: max(u.p1.x, 0.5)
    const float speed   = tune.speed;

    Dir2 dirs[kWaveN];
    wave_dirs(dirs);

    float y = 0.0f;
    for (int i = 0; i < kWaveN; ++i) {
        const float L    = baseLen * kLenMul[i];
        const float w    = 6.28318530718f / L;              // 2*pi / wavelength (water.vert L101)
        const float A    = amp * kAmpMul[i];
        const float phi  = std::sqrt(9.81f * w) * speed;     // deep-water dispersion (water.vert L104)
        const float dotd = dirs[i].x * x + dirs[i].z * z;
        const float ph   = w * dotd + phi * t;               // water.vert L50
        y += A * std::sin(ph);                               // water.vert L57 (disp.y += A*s)
    }
    return tune.seaLevel + y;
}

// ---------------------------------------------------------------------------
// echoShipPose — see echo_water.h for the full integration note.
void echoShipPose(float baseX, float baseZ, float headingRad,
                  float halfLength, float halfBeam,
                  float t, const WaterTuning& tune,
                  ShipWaveState& outState) {
    // Forward/right axes in world XZ. Matches echo_region_builders.cpp's
    // buildHarborBay heading convention: heading = atan2(dx, dz), and its
    // M[16] build uses ch=cos(heading) on the X column, sh=sin(heading) on the
    // Z column (i.e. forward = (sin(heading), cos(heading)) in XZ).
    const float fx = std::sin(headingRad), fz = std::cos(headingRad);
    const float rx = fz, rz = -fx;   // right/beam axis, perpendicular to forward

    const float bowX   = baseX + fx * halfLength, bowZ   = baseZ + fz * halfLength;
    const float sternX = baseX - fx * halfLength, sternZ = baseZ - fz * halfLength;
    const float stbdX  = baseX + rx * halfBeam,   stbdZ  = baseZ + rz * halfBeam;
    const float portX  = baseX - rx * halfBeam,   portZ  = baseZ - rz * halfBeam;

    const float yMid   = echoWaveHeight(baseX, baseZ, t, tune);
    const float yBow   = echoWaveHeight(bowX, bowZ, t, tune);
    const float yStern = echoWaveHeight(sternX, sternZ, t, tune);
    const float yStbd  = echoWaveHeight(stbdX, stbdZ, t, tune);
    const float yPort  = echoWaveHeight(portX, portZ, t, tune);

    outState.heaveY = yMid - tune.seaLevel;   // relative to still-water sea level
    // Small-angle tilt from the height difference across the hull footprint.
    // atan2 (not a plain division) so it stays well-behaved as halfLength/
    // halfBeam -> 0 (a degenerate/zero-length hull just returns 0 tilt).
    outState.pitchRad = (halfLength > 1e-4f)
        ? std::atan2(yBow - yStern, 2.0f * halfLength) : 0.0f;
    outState.rollRad = (halfBeam > 1e-4f)
        ? std::atan2(yStbd - yPort, 2.0f * halfBeam) : 0.0f;

    // Clamp to a sane range. A hull much longer than the dominant wavelength
    // (or a pathologically small halfLength/halfBeam) can otherwise produce a
    // tilt angle that reads as the boat flipping rather than rocking.
    constexpr float kMaxTiltRad = 0.35f;   // ~20 degrees
    outState.pitchRad = clampf(outState.pitchRad, -kMaxTiltRad, kMaxTiltRad);
    outState.rollRad  = clampf(outState.rollRad,  -kMaxTiltRad, kMaxTiltRad);
}

ShipWaveState echoShipPose(float baseX, float baseZ, float headingRad,
                           float halfLength, float halfBeam,
                           float t, const WaterTuning& tune) {
    ShipWaveState s;
    echoShipPose(baseX, baseZ, headingRad, halfLength, halfBeam, t, tune, s);
    return s;
}

// ---------------------------------------------------------------------------
// EchoSplashes
// ---------------------------------------------------------------------------
float EchoSplashes::frand() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return (float)(m_rng & 0x00FFFFFFu) / (float)0x01000000u;
}
float EchoSplashes::frandSym() { return frand() * 2.0f - 1.0f; }

int EchoSplashes::spawnParticle(const SplashParticle& p) {
    // Find a free slot; if the pool is full, recycle the round-robin cursor
    // (matches CombatFx's spawnParticle doctrine in app/fx.cpp: bounded ring,
    // no per-frame heap alloc, oldest recycled under pressure).
    for (int i = 0; i < kMaxSplashParticles; ++i) {
        const int idx = (m_nextParticle + i) % kMaxSplashParticles;
        if (m_particles[idx].life <= 0.0f) {
            m_particles[idx] = p;
            m_nextParticle = (idx + 1) % kMaxSplashParticles;
            return idx;
        }
    }
    const int idx = m_nextParticle;
    m_particles[idx] = p;
    m_nextParticle = (idx + 1) % kMaxSplashParticles;
    return idx;
}

void EchoSplashes::spawnBowSpray(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel) {
    const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    // Idle/dockside boats spawn nothing (zero GPU cost when the fleet is
    // still) — mirrors CombatFx's "spawned ONLY from events" doctrine.
    constexpr float kMinSprayShip = 0.6f;   // m/s
    if (speed < kMinSprayShip) return;

    // More spray the faster the boat moves, capped so a runaway velocity
    // doesn't blow the pool in one frame.
    const int count = std::min(10, 2 + (int)(speed * 0.6f));
    const float invSpeed = 1.0f / speed;
    const float nx = vel.x * invSpeed, ny = vel.y * invSpeed, nz = vel.z * invSpeed;
    for (int i = 0; i < count; ++i) {
        SplashParticle p;
        p.pos = pos;
        // Kick mostly UP + slightly BACKWARD along -vel (bow wake), with
        // lateral jitter for a spray fan rather than a single jet.
        const float upKick   = 1.2f + frand() * 1.6f;
        const float backKick = 0.3f + frand() * 0.5f;
        const float lat      = frandSym() * 0.8f;
        p.vel.x = -nx * speed * backKick + nz * lat * speed * 0.3f;
        p.vel.y = upKick + std::abs(speed) * 0.15f;
        p.vel.z = -nz * speed * backKick - nx * lat * speed * 0.3f;
        p.life = p.maxLife = 0.5f + frand() * 0.4f;
        p.size0 = 0.04f + frand() * 0.03f;
        p.size1 = p.size0 * 2.2f;
        p.r = 0.82f; p.g = 0.88f; p.b = 0.94f;
        p.a0 = 0.45f + frand() * 0.2f;
        p.gravity = 1.0f;
        p.drag = 1.4f;
        spawnParticle(p);
    }
}

void EchoSplashes::spawnShoreLapping(const Heightfield& hf, const x3::phys::Vec3& a,
                                     const x3::phys::Vec3& b, float t,
                                     const WaterTuning& tune) {
    if (!hf.ok()) return;
    constexpr int kProbes = kShoreProbesPerCall;
    constexpr float kTideBandM = 0.35f; // terrain within +/- this of the live wave height laps

    for (int i = 0; i < kProbes; ++i) {
        const float u = (kProbes > 1) ? (float)i / (float)(kProbes - 1) : 0.0f;
        const float px = a.x + (b.x - a.x) * u;
        const float pz = a.z + (b.z - a.z) * u;
        const float terrainY = hf.heightAt(px, pz);
        const float waveY = echoWaveHeight(px, pz, t, tune);
        if (std::abs(terrainY - waveY) > kTideBandM) continue;   // dry beach or open water

        // Sparse, jittered spawn (not every probe every frame) so lapping
        // reads as intermittent laps rather than a static foam fence.
        if (frand() > 0.30f) continue;

        SplashParticle p;
        p.pos = x3::phys::Vec3{ px, waveY, pz };
        p.vel = x3::phys::Vec3{ frandSym() * 0.3f, 0.5f + frand() * 0.4f, frandSym() * 0.3f };
        p.life = p.maxLife = 0.6f + frand() * 0.5f;
        p.size0 = 0.06f + frand() * 0.04f;
        p.size1 = p.size0 * 1.6f;
        p.r = 0.90f; p.g = 0.93f; p.b = 0.95f;   // paler than bow spray — sun-bleached foam lace
        p.a0 = 0.35f + frand() * 0.15f;
        p.gravity = 0.6f;   // lapping foam settles slower than a spray droplet
        p.drag = 1.8f;
        spawnParticle(p);
    }
}

void EchoSplashes::update(float dt) {
    if (dt <= 0.0f) return;
    for (auto& p : m_particles) {
        if (p.life <= 0.0f) continue;
        p.life -= dt;
        if (p.life <= 0.0f) continue;
        p.vel.y -= p.gravity * 9.81f * dt;
        const float dragK = std::max(0.0f, 1.0f - p.drag * dt);
        p.vel.x *= dragK; p.vel.y *= dragK; p.vel.z *= dragK;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
    }
}

void EchoSplashes::submit(x3::rhi::IRenderDevice& device) const {
    using PI = x3::rhi::IRenderDevice::ParticleInstance;
    PI* buf = m_scratch;
    uint32_t n = 0;
    for (const auto& p : m_particles) {
        if (p.life <= 0.0f) continue;
        const float tlife = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 0.0f;  // 1 -> 0
        const float age = 1.0f - tlife;
        const float size = p.size0 + (p.size1 - p.size0) * age;
        const float alpha = p.a0 * tlife;
        PI inst;
        inst.pos[0] = p.pos.x; inst.pos[1] = p.pos.y; inst.pos[2] = p.pos.z;
        inst.size = size;
        inst.color[0] = p.r; inst.color[1] = p.g; inst.color[2] = p.b; inst.color[3] = alpha;
        buf[n++] = inst;
        if (n >= kMaxSplashParticles) break;
    }
    if (n) device.submitParticles(buf, n, x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

int EchoSplashes::liveCount() const {
    int n = 0;
    for (const auto& p : m_particles) if (p.life > 0.0f) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Swell preset table (see echo_water.h for the ring-margin math per preset).
const SwellPreset kSwellPresets[3] = {
    { "calm",   kSwellCalm,   -0.0536f, 0.3464f },
    { "harbor", kSwellHarbor, -0.2072f, 0.1928f },
    { "storm",  kSwellStorm,  -0.3224f, 0.0776f },
};

} // namespace x3::game
