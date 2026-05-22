// Combat FX: crosshair + shot tracers + muzzle flash. See app/fx.h.
//
// Clean-room: built from the IRenderDevice + Vec3 interfaces only. No id Tech /
// RBDOOM source consulted.
#include "fx.h"
#include "mesh_prims.h"

#include <cmath>

namespace x3::game {

namespace {

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), PER-AXIS scale
// (sx,sy,sz applied to the corresponding basis column), and translation t.
void composeTRS3(float m[16],
                 const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                 float sx, float sy, float sz, const x3::phys::Vec3& t) {
    m[0]  = bx.x * sx; m[1]  = bx.y * sx; m[2]  = bx.z * sx; m[3]  = 0.0f;
    m[4]  = by.x * sy; m[5]  = by.y * sy; m[6]  = by.z * sy; m[7]  = 0.0f;
    m[8]  = bz.x * sz; m[9]  = bz.y * sz; m[10] = bz.z * sz; m[11] = 0.0f;
    m[12] = t.x;       m[13] = t.y;       m[14] = t.z;       m[15] = 1.0f;
}

x3::phys::Vec3 cross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.y * b.z - a.z * b.y,
                           a.z * b.x - a.x * b.z,
                           a.x * b.y - a.y * b.x };
}

x3::phys::Vec3 normalize(const x3::phys::Vec3& v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l < 1e-6f) return x3::phys::Vec3{ 0.0f, 0.0f, 1.0f };
    return x3::phys::Vec3{ v.x / l, v.y / l, v.z / l };
}

} // namespace

// ---------------------------------------------------------------------------
// init / shutdown: own one shared centered unit box (half-extent 0.5).
// ---------------------------------------------------------------------------
void CombatFx::init(x3::rhi::IRenderDevice& device) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f);
    m_box = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                              geo.index.data(), (uint32_t)geo.index.size());
    for (auto& t : m_tracers) t.life = 0.0f;
    m_muzzleFlash = 0.0f;
    m_nextTracer = 0;
}

void CombatFx::shutdown(x3::rhi::IRenderDevice& device) {
    if (m_box.valid()) {
        device.destroyMesh(m_box);
        m_box = x3::rhi::MeshHandle{};
    }
}

// ---------------------------------------------------------------------------
// Small deterministic PRNG (xorshift32) for spawn jitter. Repeatable so headless
// captures (--screenshot / --capture-ai) produce the same burst every run.
// ---------------------------------------------------------------------------
float CombatFx::frand() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return (float)(m_rng & 0x00FFFFFFu) / (float)0x01000000u; // [0,1)
}
float CombatFx::frandSym() { return frand() * 2.0f - 1.0f; }   // [-1,1)

// ---------------------------------------------------------------------------
// spawnParticle: claim a slot via the round-robin recycle cursor. Bounded pool
// -> no per-frame heap alloc (the array is fixed; overflow recycles the oldest).
// ---------------------------------------------------------------------------
int CombatFx::spawnParticle(const Particle& p) {
    int slot = m_nextParticle;
    m_nextParticle = (m_nextParticle + 1) % kMaxParticles;
    m_particles[slot] = p;
    return slot;
}

// ---------------------------------------------------------------------------
// addTracer: drop a beam into the pool (round-robin) + light the muzzle flash +
// spawn the muzzle-flash particle burst (so every shot reads with juice).
// ---------------------------------------------------------------------------
void CombatFx::addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to) {
    Tracer& t = m_tracers[m_nextTracer];
    t.from = from;
    t.to   = to;
    t.life = kTracerTime;
    m_nextTracer = (m_nextTracer + 1) % kMaxTracers;

    m_muzzlePos   = from;
    m_muzzleFlash = kMuzzleFlashTime;

    // Bias the muzzle spark cone forward along the shot direction (to - from).
    x3::phys::Vec3 dir{ to.x - from.x, to.y - from.y, to.z - from.z };
    spawnMuzzleFlash(from, dir);
}

// ---------------------------------------------------------------------------
// Combat-event particle/decal presets. Each spawns a tuned burst into the pool.
// Colors are LINEAR HDR — additive sparks/muzzle use >1 intensity so they feed
// the renderer's bloom chain. Sizes are billboard half-extents in meters.
// ---------------------------------------------------------------------------
void CombatFx::spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir) {
    x3::phys::Vec3 d = normalize(dir);
    // A few hot, fast, short-lived additive sparks shooting out of the barrel.
    const int n = 10;
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 5.0f + frand() * 7.0f;
        p.vel = x3::phys::Vec3{ d.x * speed + frandSym() * 2.0f,
                                d.y * speed + frandSym() * 2.0f,
                                d.z * speed + frandSym() * 2.0f };
        p.life = p.maxLife = 0.06f + frand() * 0.06f;
        p.size0 = 0.10f + frand() * 0.05f;
        p.size1 = 0.02f;
        p.r = 5.0f; p.g = 3.2f; p.b = 1.0f;   // hot orange-white (HDR -> bloom)
        p.a0 = 1.0f;
        p.gravity = 0.0f; p.drag = 6.0f; p.additive = true;
        spawnParticle(p);
    }
    // One bright soft flash sprite at the muzzle.
    Particle flash;
    flash.pos = pos;
    flash.life = flash.maxLife = 0.05f;
    flash.size0 = 0.28f; flash.size1 = 0.10f;
    flash.r = 6.0f; flash.g = 4.0f; flash.b = 1.6f;
    flash.a0 = 1.0f; flash.additive = true;
    spawnParticle(flash);
}

void CombatFx::spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal) {
    x3::phys::Vec3 nrm = normalize(normal);
    // Additive sparks sprayed back along the surface normal in a cone.
    const int nSpark = 14;
    for (int i = 0; i < nSpark; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 3.0f + frand() * 6.0f;
        p.vel = x3::phys::Vec3{ nrm.x * speed + frandSym() * 3.5f,
                                nrm.y * speed + frandSym() * 3.5f,
                                nrm.z * speed + frandSym() * 3.5f };
        p.life = p.maxLife = 0.18f + frand() * 0.22f;
        p.size0 = 0.045f + frand() * 0.03f;
        p.size1 = 0.01f;
        p.r = 4.5f; p.g = 2.6f; p.b = 0.8f;   // hot spark
        p.a0 = 1.0f;
        p.gravity = 0.6f; p.drag = 2.0f; p.additive = true;
        spawnParticle(p);
    }
    // An alpha dust puff that drifts off the surface + rises slightly.
    const int nDust = 8;
    for (int i = 0; i < nDust; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.06f,
                                pos.y + frandSym() * 0.06f,
                                pos.z + frandSym() * 0.06f };
        p.vel = x3::phys::Vec3{ nrm.x * 0.8f + frandSym() * 0.6f,
                                nrm.y * 0.8f + 0.4f + frandSym() * 0.3f,
                                nrm.z * 0.8f + frandSym() * 0.6f };
        p.life = p.maxLife = 0.5f + frand() * 0.5f;
        p.size0 = 0.08f; p.size1 = 0.32f;     // grows as it disperses
        p.r = 0.35f; p.g = 0.33f; p.b = 0.30f; // grey dust
        p.a0 = 0.5f;
        p.gravity = -0.05f; p.drag = 1.5f; p.additive = false;
        spawnParticle(p);
    }
    // Persistent scorch mark on the surface.
    addDecal(pos, nrm);
}

void CombatFx::spawnBlood(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir) {
    x3::phys::Vec3 d = normalize(dir);
    // A short spray of dark-red alpha droplets along the shot direction + gravity.
    const int n = 12;
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 1.5f + frand() * 4.0f;
        p.vel = x3::phys::Vec3{ d.x * speed + frandSym() * 2.0f,
                                d.y * speed + frandSym() * 2.0f + 1.0f,
                                d.z * speed + frandSym() * 2.0f };
        p.life = p.maxLife = 0.35f + frand() * 0.35f;
        p.size0 = 0.05f + frand() * 0.04f;
        p.size1 = 0.03f;
        p.r = 0.55f; p.g = 0.02f; p.b = 0.02f;  // dark red
        p.a0 = 0.85f;
        p.gravity = 1.0f; p.drag = 1.2f; p.additive = false;
        spawnParticle(p);
    }
}

void CombatFx::spawnDeath(const x3::phys::Vec3& pos) {
    // Debris chunks: alpha, gravity-driven, sprayed in all directions.
    const int nChunk = 16;
    for (int i = 0; i < nChunk; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.2f,
                                pos.y + frandSym() * 0.3f,
                                pos.z + frandSym() * 0.2f };
        const float speed = 2.0f + frand() * 4.0f;
        p.vel = x3::phys::Vec3{ frandSym() * speed,
                                frand() * speed + 1.0f,
                                frandSym() * speed };
        p.life = p.maxLife = 0.6f + frand() * 0.6f;
        p.size0 = 0.07f + frand() * 0.05f;
        p.size1 = 0.05f;
        p.r = 0.30f; p.g = 0.28f; p.b = 0.26f;  // grey-brown chunk
        p.a0 = 0.95f;
        p.gravity = 1.0f; p.drag = 0.6f; p.additive = false;
        spawnParticle(p);
    }
    // A couple of hot embers (additive) so the kill pops.
    for (int i = 0; i < 6; ++i) {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ frandSym() * 3.0f, frand() * 3.0f + 1.0f, frandSym() * 3.0f };
        p.life = p.maxLife = 0.3f + frand() * 0.3f;
        p.size0 = 0.06f; p.size1 = 0.01f;
        p.r = 4.0f; p.g = 1.6f; p.b = 0.4f;
        p.a0 = 1.0f; p.gravity = 0.3f; p.drag = 1.5f; p.additive = true;
        spawnParticle(p);
    }
    spawnSmoke(pos);
}

void CombatFx::spawnSmoke(const x3::phys::Vec3& pos) {
    const int n = 6;
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.15f,
                                pos.y + frandSym() * 0.15f,
                                pos.z + frandSym() * 0.15f };
        p.vel = x3::phys::Vec3{ frandSym() * 0.4f, 0.6f + frand() * 0.5f, frandSym() * 0.4f };
        p.life = p.maxLife = 1.2f + frand() * 1.0f;
        p.size0 = 0.18f; p.size1 = 0.7f;        // grows + dissipates
        p.r = 0.18f; p.g = 0.18f; p.b = 0.18f;  // dark grey smoke
        p.a0 = 0.4f;
        p.gravity = -0.15f; p.drag = 1.0f; p.additive = false;
        spawnParticle(p);
    }
}

void CombatFx::addDecal(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal) {
    Decal& d = m_decalsRing[m_nextDecal];
    m_nextDecal = (m_nextDecal + 1) % kMaxDecals;
    d.center   = pos;
    d.normal   = normalize(normal);
    d.halfSize = 0.10f + frand() * 0.05f;
    d.angle    = frand() * 6.2831853f;
    d.life     = d.maxLife = kDecalLife;
}

int CombatFx::liveParticleCount() const {
    int n = 0;
    for (const auto& p : m_particles) if (p.life > 0.0f) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// update: decay tracer lifetimes + the muzzle flash, integrate the particle pool
// (semi-implicit Euler with gravity + drag), and age the decals.
// ---------------------------------------------------------------------------
void CombatFx::update(float dt) {
    if (dt <= 0.0f) return;
    for (auto& t : m_tracers) {
        if (t.life > 0.0f) {
            t.life -= dt;
            if (t.life < 0.0f) t.life = 0.0f;
        }
    }
    if (m_muzzleFlash > 0.0f) {
        m_muzzleFlash -= dt;
        if (m_muzzleFlash < 0.0f) m_muzzleFlash = 0.0f;
    }

    // Particle integration. World gravity is -Y 9.81 (CONVENTIONS §1).
    const float kG = 9.81f;
    for (auto& p : m_particles) {
        if (p.life <= 0.0f) continue;
        // Drag: exponential-ish velocity damping.
        const float damp = 1.0f - p.drag * dt;
        const float k = (damp < 0.0f) ? 0.0f : damp;
        p.vel.x *= k; p.vel.y *= k; p.vel.z *= k;
        p.vel.y -= kG * p.gravity * dt;       // gravity scaled per-particle
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
        p.life -= dt;
        if (p.life < 0.0f) p.life = 0.0f;
    }

    // Age the decals (fade out over their lifetime, then free).
    for (auto& d : m_decalsRing) {
        if (d.life > 0.0f) {
            d.life -= dt;
            if (d.life < 0.0f) d.life = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// submit: stream the live particles (as additive + alpha batches) + the live
// decals to the device for this frame. Bounded fixed scratch arrays (static-
// thread-local would be heap; instead a small stack array per call, capped to
// the pool size) — no per-frame heap alloc.
// ---------------------------------------------------------------------------
void CombatFx::submit(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    (void)frame; // submit API caches per-frame; frame ctx not needed by the device call
    using PI = x3::rhi::IRenderDevice::ParticleInstance;
    using DI = x3::rhi::IRenderDevice::DecalInstance;

    // Member-owned scratch (bounded by the pool / ring caps) — no per-frame alloc.
    PI* addBuf   = m_scratchAdd;
    PI* alphaBuf = m_scratchAlpha;
    uint32_t nAdd = 0, nAlpha = 0;

    for (const auto& p : m_particles) {
        if (p.life <= 0.0f) continue;
        const float t = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 0.0f; // 1 -> 0
        const float age = 1.0f - t;                                       // 0 -> 1
        const float size = p.size0 + (p.size1 - p.size0) * age;
        // Fade alpha out as the particle dies (smooth tail).
        const float alpha = p.a0 * t;
        PI inst;
        inst.pos[0] = p.pos.x; inst.pos[1] = p.pos.y; inst.pos[2] = p.pos.z;
        inst.size = size;
        inst.color[0] = p.r; inst.color[1] = p.g; inst.color[2] = p.b; inst.color[3] = alpha;
        if (p.additive) addBuf[nAdd++] = inst;
        else            alphaBuf[nAlpha++] = inst;
    }
    if (nAdd)   device.submitParticles(addBuf,   nAdd,   x3::rhi::IRenderDevice::ParticleBlend::Additive);
    if (nAlpha) device.submitParticles(alphaBuf, nAlpha, x3::rhi::IRenderDevice::ParticleBlend::Alpha);

    // Decals.
    DI* decalBuf = m_scratchDecal;
    uint32_t nDecal = 0;
    for (const auto& d : m_decalsRing) {
        if (d.life <= 0.0f) continue;
        const float t = (d.maxLife > 0.0f) ? (d.life / d.maxLife) : 0.0f;
        // Hold opaque for most of the life, fade in the last ~25%.
        const float fade = (t > 0.25f) ? 1.0f : (t / 0.25f);
        DI inst;
        inst.center[0] = d.center.x; inst.center[1] = d.center.y; inst.center[2] = d.center.z;
        inst.halfSize = d.halfSize;
        inst.normal[0] = d.normal.x; inst.normal[1] = d.normal.y; inst.normal[2] = d.normal.z;
        inst.angle = d.angle;
        // Dark scorch; opacity carries the lifetime fade.
        inst.color[0] = 0.02f; inst.color[1] = 0.015f; inst.color[2] = 0.01f;
        inst.color[3] = 0.85f * fade;
        decalBuf[nDecal++] = inst;
    }
    if (nDecal) device.submitDecals(decalBuf, nDecal);
}

// ---------------------------------------------------------------------------
// drawBeam: stretch the unit box along segment a->b with `thickness` cross-
// section. Build an orthonormal basis with the segment as the local Z axis.
// ---------------------------------------------------------------------------
void CombatFx::drawBeam(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                        float thickness, const float color[4]) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-5f) return;
    x3::phys::Vec3 dir = normalize(seg);

    // Two perpendiculars to dir. Pick a reference axis not parallel to dir.
    x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                    : x3::phys::Vec3{ 1, 0, 0 };
    x3::phys::Vec3 u = normalize(cross(ref, dir));   // perp 1
    x3::phys::Vec3 v = cross(dir, u);                // perp 2 (already unit)

    // Midpoint of the segment is the box center.
    x3::phys::Vec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };

    // Unit box is half-extent 0.5: scale local axes by (2*thickness) cross-
    // section and `len` along the segment so it exactly spans a->b.
    float model[16];
    composeTRS3(model, u, v, dir, thickness * 2.0f, thickness * 2.0f, len, mid);
    device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, color, model);
}

// ---------------------------------------------------------------------------
// draw: active tracers + muzzle flash. (The crosshair moved to the screen-space
// HUD layer in S7 — see app/hud.* — so fx no longer draws a world-space "+".)
// ---------------------------------------------------------------------------
void CombatFx::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const {
    if (!m_box.valid()) return;
    (void)eyeX; (void)eyeY; (void)eyeZ;  // no longer needed without the crosshair

    // Bright FX colors (baseColorFactor multiplies the default white texel).
    const float tracerColor[4]    = { 1.0f, 0.95f, 0.4f, 1.0f }; // hot yellow
    const float muzzleColor[4]    = { 1.0f, 0.85f, 0.4f, 1.0f }; // muzzle orange

    // ---- Camera basis (device convention: fwd = (cp*cy, sp, cp*sy)). ----
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    x3::phys::Vec3 right{ -sy, 0.0f, cy };
    x3::phys::Vec3 up = cross(right, forward);  // used by the muzzle-flash basis

    // ---- Tracers: thin bright beams, fading thinner as they age. ----
    for (const auto& t : m_tracers) {
        if (t.life <= 0.0f) continue;
        float k = (kTracerTime > 0.0f) ? (t.life / kTracerTime) : 1.0f; // 1->0
        // Slightly taper the beam as it fades so it reads as a fast streak.
        float thick = kTracerThickness * (0.5f + 0.5f * k);
        drawBeam(device, frame, t.from, t.to, thick, tracerColor);
    }

    // ---- Muzzle flash: a brief bright box at the muzzle. ----
    if (m_muzzleFlash > 0.0f) {
        float k = (kMuzzleFlashTime > 0.0f) ? (m_muzzleFlash / kMuzzleFlashTime) : 1.0f;
        float s = kMuzzleFlashSize * (0.6f + 0.4f * k) * 2.0f; // full extent
        float m[16];
        composeTRS3(m, right, up, forward, s, s, s, m_muzzlePos);
        device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, muzzleColor, m);
    }
}

} // namespace x3::game
