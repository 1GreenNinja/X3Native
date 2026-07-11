// Combat FX: crosshair + shot tracers + muzzle flash. See app/fx.h.
//
// Clean-room: built from the IRenderDevice + Vec3 interfaces only. No id Tech /
// RBDOOM source consulted.
#include "fx.h"
#include "mesh_prims.h"

#include <algorithm> // std::min (lightning bolt propagation clamp)
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
    for (auto& a : m_arcs)    a.life = 0.0f;
    m_muzzleFlash = 0.0f;
    m_nextTracer = 0;
    m_nextArc = 0;
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
void CombatFx::addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to, WeaponFxKind kind) {
    Tracer& t = m_tracers[m_nextTracer];
    t.from = from;
    t.to   = to;
    t.life = kTracerTime;
    t.age  = 0.0f;        // Lightning bolt grows from the muzzle over time
    t.kind = kind;
    m_nextTracer = (m_nextTracer + 1) % kMaxTracers;

    m_muzzlePos   = from;
    // LIGHTNING has NO box muzzle flash: the box flash + soft flash sprite read as an
    // ugly bright blob (a "snowball") in front of the beam. The jagged bolt IS the read
    // for the beam weapon; suppress the flash entirely. Other weapons are unchanged.
    m_muzzleFlash = (kind == WeaponFxKind::Lightning) ? 0.0f : kMuzzleFlashTime;

    // Bias the muzzle spark cone forward along the shot direction (to - from). Lightning
    // skips the addTracer spark burst (the bolt carries the read); every other weapon
    // keeps the original default muzzle-spark burst (byte-identical behavior).
    if (kind != WeaponFxKind::Lightning) {
        x3::phys::Vec3 dir{ to.x - from.x, to.y - from.y, to.z - from.z };
        spawnMuzzleFlash(from, dir);
    }
}

// ---------------------------------------------------------------------------
// Combat-event particle/decal presets. Each spawns a tuned burst into the pool.
// Colors are LINEAR HDR — additive sparks/muzzle use >1 intensity so they feed
// the renderer's bloom chain. Sizes are billboard half-extents in meters.
// ---------------------------------------------------------------------------
// Per-kind muzzle-flash tuning. Returns a tint (linear HDR), per-spark count + size
// + speed multipliers, and a soft-flash tint/scale, so each weapon's flash reads
// distinctly. Default/Pistol keep the original hot-orange ballistic look.
namespace {
struct MuzzleStyle {
    float sparkR, sparkG, sparkB;     // spark tint (linear HDR -> bloom)
    float flashR, flashG, flashB;     // soft-flash sprite tint
    int   sparkCount;                 // number of cone sparks
    float sizeMul;                    // spark + flash size multiplier
    float speedMul;                   // spark cone speed multiplier
    float coneJitter;                 // lateral spark spread (m/s)
    float flashSize;                  // soft-flash half-extent (m) at birth
};
MuzzleStyle muzzleStyleFor(WeaponFxKind k) {
    switch (k) {
        case WeaponFxKind::Smg:       // leaner/cooler, many small fast sparks
            return { 4.5f, 3.0f, 1.2f,  5.5f, 3.8f, 1.8f, 12, 0.8f, 1.15f, 2.0f, 0.22f };
        case WeaponFxKind::Shotgun:   // WIDE fat boom: big flash, broad spray
            return { 6.0f, 3.6f, 1.0f,  7.5f, 4.6f, 1.6f, 16, 1.6f, 1.0f,  3.6f, 0.52f };
        case WeaponFxKind::Chaingun:  // hot + extra-sparky (busy auto roar)
            return { 6.0f, 3.0f, 0.7f,  7.0f, 4.0f, 1.2f, 18, 0.95f,1.25f, 2.6f, 0.30f };
        case WeaponFxKind::Plasma:    // BLUE energy: soft round flash, no metal sparks
            return { 0.8f, 2.4f, 6.0f,  1.2f, 3.0f, 7.0f,  8, 1.2f, 0.85f, 1.4f, 0.40f };
        case WeaponFxKind::Lightning: // electric crackle: white-cyan, twitchy fast
            return { 3.5f, 6.0f, 6.5f,  4.0f, 6.5f, 7.0f, 14, 0.7f, 1.5f,  3.2f, 0.26f };
        case WeaponFxKind::Pistol:
        case WeaponFxKind::Default:
        default:                      // original hot orange-white ballistic look
            return { 5.0f, 3.2f, 1.0f,  6.0f, 4.0f, 1.6f, 10, 1.0f, 1.0f,  2.0f, 0.28f };
    }
}
} // namespace

void CombatFx::spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir) {
    spawnMuzzleFlash(pos, dir, WeaponFxKind::Default);
}

void CombatFx::spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir,
                                WeaponFxKind kind) {
    x3::phys::Vec3 d = normalize(dir);
    const MuzzleStyle st = muzzleStyleFor(kind);
    const bool isLightning = (kind == WeaponFxKind::Lightning);
    // LIGHTNING: the spark cone leaving the tip is the only per-frame muzzle motion —
    // slow it ~39% (x0.61) so the effect doesn't "travel too fast". Other weapons
    // unchanged (speedScale == 1).
    const float speedScale = isLightning ? 0.61f : 1.0f;
    // A few hot, fast, short-lived additive sparks shooting out of the barrel.
    for (int i = 0; i < st.sparkCount; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = (5.0f + frand() * 7.0f) * st.speedMul * speedScale;
        p.vel = x3::phys::Vec3{ d.x * speed + frandSym() * st.coneJitter * speedScale,
                                d.y * speed + frandSym() * st.coneJitter * speedScale,
                                d.z * speed + frandSym() * st.coneJitter * speedScale };
        p.life = p.maxLife = 0.06f + frand() * 0.06f;
        p.size0 = (0.10f + frand() * 0.05f) * st.sizeMul;
        p.size1 = 0.02f * st.sizeMul;
        p.r = st.sparkR; p.g = st.sparkG; p.b = st.sparkB;   // per-weapon tint
        p.a0 = 1.0f;
        p.gravity = 0.0f; p.drag = 6.0f; p.additive = true;
        spawnParticle(p);
    }
    // One bright soft flash sprite at the muzzle. LIGHTNING SKIPS THIS — the bright soft
    // blob in front of the beam was the "snowball puff" to be removed; the jagged bolt is
    // the read by itself. Every other weapon keeps its soft flash sprite (unchanged).
    if (!isLightning) {
        Particle flash;
        flash.pos = pos;
        flash.life = flash.maxLife = 0.05f;
        flash.size0 = st.flashSize; flash.size1 = st.flashSize * 0.36f;
        flash.r = st.flashR; flash.g = st.flashG; flash.b = st.flashB;
        flash.a0 = 1.0f; flash.additive = true;
        spawnParticle(flash);
    }
}

// Per-kind impact tuning: spark tint + count, and whether the dust puff is the grey
// metal-debris look (ballistic) or suppressed (energy weapons just splash light).
namespace {
struct ImpactStyle {
    float sparkR, sparkG, sparkB;  // spark tint
    int   sparkCount;
    float sizeMul;
    bool  dust;                    // grey alpha dust puff (metal hit) vs energy splash
};
ImpactStyle impactStyleFor(WeaponFxKind k) {
    switch (k) {
        case WeaponFxKind::Plasma:    // blue energy splash, no metal dust
            return { 0.7f, 2.2f, 6.0f, 16, 1.25f, false };
        case WeaponFxKind::Lightning: // electric: few tiny fast sparks (arc tendrils carry it)
            return { 3.0f, 5.5f, 6.5f, 6, 0.4f,  false };
        case WeaponFxKind::Shotgun:   // wide hot spark spray + dust
            return { 4.5f, 2.6f, 0.8f, 20, 1.2f,  true  };
        case WeaponFxKind::Chaingun:  // busy hot sparks + dust
            return { 5.0f, 2.6f, 0.7f, 18, 0.9f,  true  };
        case WeaponFxKind::Smg:
        case WeaponFxKind::Pistol:
        case WeaponFxKind::Default:
        default:                      // original hot spark + grey dust
            return { 4.5f, 2.6f, 0.8f, 14, 1.0f,  true  };
    }
}
} // namespace

void CombatFx::spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal) {
    spawnImpact(pos, normal, WeaponFxKind::Default);
}

void CombatFx::spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal,
                           WeaponFxKind kind) {
    x3::phys::Vec3 nrm = normalize(normal);
    const ImpactStyle st = impactStyleFor(kind);
    // Additive sparks sprayed back along the surface normal in a cone.
    for (int i = 0; i < st.sparkCount; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 3.0f + frand() * 6.0f;
        p.vel = x3::phys::Vec3{ nrm.x * speed + frandSym() * 3.5f,
                                nrm.y * speed + frandSym() * 3.5f,
                                nrm.z * speed + frandSym() * 3.5f };
        p.life = p.maxLife = 0.18f + frand() * 0.22f;
        p.size0 = (0.045f + frand() * 0.03f) * st.sizeMul;
        p.size1 = 0.01f * st.sizeMul;
        p.r = st.sparkR; p.g = st.sparkG; p.b = st.sparkB;   // per-weapon tint
        p.a0 = 1.0f;
        p.gravity = 0.6f; p.drag = 2.0f; p.additive = true;
        spawnParticle(p);
    }
    // An alpha dust puff that drifts off the surface + rises slightly. Energy weapons
    // (plasma/lightning) skip the grey metal dust — they just splash light.
    if (st.dust) {
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
    }
    // LIGHTNING impact = electric VIOLENCE, not white puffballs: a tight blue-white
    // flash core + a whipping ring of short crackling arc tendrils crawling off the hit
    // (drawn in draw() as re-rolled mini zigzags). The round sparks are already cut to a
    // few tiny fast specks above. Other weapons keep the metal-spark + dust look above.
    if (kind == WeaponFxKind::Lightning) {
        Particle f;                       // one tight blue-white flash core
        f.pos = pos;
        f.life = f.maxLife = 0.08f;
        f.size0 = 0.30f; f.size1 = 0.05f;
        f.r = 2.2f; f.g = 3.0f; f.b = 4.5f; f.a0 = 1.0f;
        f.gravity = 0.0f; f.drag = 0.0f; f.additive = true;
        spawnParticle(f);
        spawnArcs(pos, nrm);
    }
    // Persistent scorch mark on the surface.
    addDecal(pos, nrm);
}

// ---------------------------------------------------------------------------
// spawnArcs: whip a ring of short electric tendrils off a lightning hit point.
// Each is a tiny re-rolled zigzag (drawLightningBolt) leaning off the surface in
// a random hemisphere direction — sharp electric streaks, not round puffballs.
// ---------------------------------------------------------------------------
void CombatFx::spawnArcs(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal) {
    x3::phys::Vec3 nrm = normalize(normal);
    x3::phys::Vec3 ref = (std::fabs(nrm.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                    : x3::phys::Vec3{ 1, 0, 0 };
    x3::phys::Vec3 u = normalize(cross(ref, nrm));
    x3::phys::Vec3 v = cross(nrm, u);
    const int n = 6 + (int)(frand() * 4.0f);   // 6-9 tendrils
    for (int i = 0; i < n; ++i) {
        Arc& a = m_arcs[m_nextArc];
        m_nextArc = (m_nextArc + 1) % kMaxArcs;
        const float az = frand() * 6.2831853f;
        const float el = 0.15f + frand() * 0.85f;          // lean out from the surface
        x3::phys::Vec3 d{ nrm.x * el + (u.x * std::cos(az) + v.x * std::sin(az)),
                          nrm.y * el + (u.y * std::cos(az) + v.y * std::sin(az)),
                          nrm.z * el + (u.z * std::cos(az) + v.z * std::sin(az)) };
        a.base = pos;
        a.dir  = normalize(d);
        a.len  = 0.35f + frand() * 0.65f;
        a.life = a.maxLife = kArcLife * (0.6f + frand() * 0.7f);
        a.seed = m_rng ^ (uint32_t)(i * 2654435761u);
    }
}

void CombatFx::spawnBlood(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir) {
    x3::phys::Vec3 d = normalize(dir);
    // A DENSE spray of dark-red alpha droplets along the shot direction + gravity.
    // Bigger + longer-lived than the old spray so the hit reads clearly on-screen.
    const int n = 22;                       // was 12 — denser spray
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 2.0f + frand() * 5.0f;
        p.vel = x3::phys::Vec3{ d.x * speed + frandSym() * 2.5f,
                                d.y * speed + frandSym() * 2.5f + 1.5f,
                                d.z * speed + frandSym() * 2.5f };
        p.life = p.maxLife = 0.5f + frand() * 0.5f;     // longer
        p.size0 = 0.09f + frand() * 0.06f;              // ~2x bigger
        p.size1 = 0.05f;
        p.r = 0.6f; p.g = 0.02f; p.b = 0.02f;           // dark red
        p.a0 = 0.9f;
        p.gravity = 1.4f; p.drag = 1.1f; p.additive = false;
        spawnParticle(p);
    }
    // Ground pool: drop a dark-red, up-facing decal just below the hit so a kill
    // leaves a lasting mark. The decal ring has no color param, so we claim a ring
    // slot directly and stamp it red + bigger (radius ~0.4-0.7 m).
    {
        Decal& dc = m_decalsRing[m_nextDecal];
        m_nextDecal = (m_nextDecal + 1) % kMaxDecals;
        dc.center   = x3::phys::Vec3{ pos.x, pos.y - 0.4f, pos.z };
        dc.normal   = x3::phys::Vec3{ 0.0f, 1.0f, 0.0f };   // up-facing ground pool
        dc.halfSize = 0.4f + frand() * 0.3f;                // bigger than a bullet hole
        dc.angle    = frand() * 6.2831853f;
        dc.life     = dc.maxLife = kDecalLife;
        dc.color[0] = 0.35f; dc.color[1] = 0.02f; dc.color[2] = 0.02f;  // dark red
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
    d.color[0] = 0.02f; d.color[1] = 0.015f; d.color[2] = 0.01f;   // dark scorch (bullet hole)
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
            t.age  += dt;   // drives the Lightning bolt's propagation reach + re-roll
            t.life -= dt;
            if (t.life < 0.0f) t.life = 0.0f;
        }
    }
    // Age the electric arc tendrils (lightning impact crackle).
    for (auto& a : m_arcs) {
        if (a.life > 0.0f) {
            a.life -= dt;
            if (a.life < 0.0f) a.life = 0.0f;
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
        // Per-decal tint (scorch by default, dark red for blood pools); opacity
        // carries the lifetime fade.
        inst.color[0] = d.color[0]; inst.color[1] = d.color[1]; inst.color[2] = d.color[2];
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
// drawBoltSegment: one straight zigzag segment a->b as a camera-facing GLOW
// ribbon (wide, dim blue) + a thinner white-hot CORE ribbon inside it, both via
// drawMeshEmissive so the HDR emissive term drives the bloom halo (bright white
// core, tight blue-white glow). The ribbon WIDTH axis is perpendicular to both the
// segment and the eye->segment view dir so the flat side faces the camera (never a
// square rod, even when the bolt points near the eye).
// ---------------------------------------------------------------------------
void CombatFx::drawBoltSegment(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                               const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                               const x3::phys::Vec3& eye,
                               float coreThick, float brightness) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-5f) return;
    x3::phys::Vec3 dir = normalize(seg);
    x3::phys::Vec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    x3::phys::Vec3 view{ mid.x - eye.x, mid.y - eye.y, mid.z - eye.z };
    float vl = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
    if (vl < 1e-5f) view = dir; else view = x3::phys::Vec3{ view.x / vl, view.y / vl, view.z / vl };
    // Ribbon width axis (perp to segment + view). Head-on -> pick any perp.
    x3::phys::Vec3 w = cross(dir, view);
    float wl = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    if (wl < 1e-3f) {
        x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                        : x3::phys::Vec3{ 1, 0, 0 };
        w = normalize(cross(ref, dir));
    } else {
        w = x3::phys::Vec3{ w.x / wl, w.y / wl, w.z / wl };
    }
    x3::phys::Vec3 nrm = cross(w, dir);   // depth axis (kept thin)

    const float blackBase[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // pure emissive read
    // GLOW ribbon: wide soft blue halo (HDR emissive so bloom smears it).
    {
        float model[16];
        float gw = kLightningGlowThick;
        composeTRS3(model, w, nrm, dir, gw, gw * 0.35f, len, mid);
        const float emis[4] = { 0.12f, 0.45f, 1.0f, 2.4f * brightness };  // blue-electric
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emis, model);
    }
    // CORE ribbon: thin white-hot line (blue-white, very bright -> blooms to white).
    {
        float model[16];
        composeTRS3(model, w, nrm, dir, coreThick, coreThick * 0.5f, len, mid);
        const float emis[4] = { 1.8f, 2.4f, 3.4f, 3.4f * brightness };    // white-blue core
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emis, model);
    }
}

// ---------------------------------------------------------------------------
// drawLightningBolt: a HARD-ANGLE ZIGZAG bolt a->b. Split into straight runs
// (~kLightningSegLen each) whose interior vertices are kicked perpendicular by an
// alternating-azimuth offset sized to a random 15-45 deg kink, then hang 1-2 short
// thinner/dimmer BRANCH forks off random kink points. The pattern is DETERMINISTIC
// from `seed` (the caller buckets t.age by kLightningRerollPeriod), so the bolt holds
// a shape ~65 ms then JUMPS — a living crackling zigzag, not a per-frame strobe.
// Endpoints land exactly on a (muzzle) and b (hit point / propagation tip).
// ---------------------------------------------------------------------------
void CombatFx::drawLightningBolt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                                 const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                                 const x3::phys::Vec3& eye,
                                 float coreThick, uint32_t seed, float brightness) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-4f) { drawBoltSegment(device, frame, a, b, eye, coreThick, brightness); return; }
    x3::phys::Vec3 dir = normalize(seg);
    x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                    : x3::phys::Vec3{ 1, 0, 0 };
    x3::phys::Vec3 u = normalize(cross(ref, dir));   // perpendicular kink basis
    x3::phys::Vec3 v = cross(dir, u);

    // Deterministic per-bucket PRNG (xorshift32) — the crackling "re-roll".
    uint32_t s = seed * 2654435761u + 0x9E3779B9u; if (s == 0u) s = 1u;
    auto rnd01 = [&s]() -> float {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s & 0x00FFFFFFu) / (float)0x01000000u;
    };
    const float kPi = 3.14159265f;

    // Number of straight runs: ~1 per kLightningSegLen, clamped so a long beam stays a
    // legible zigzag (not noise) and a short one still kinks a few times.
    int n = (int)(len / kLightningSegLen + 0.5f);
    if (n < 3)  n = 3;
    if (n > 20) n = 20;
    const float segStep = len / (float)n;

    // Build the zigzag vertices. Endpoints fixed; interior vertices kicked perp.
    constexpr int kMaxV = 21;   // n<=20 -> n+1 vertices
    x3::phys::Vec3 pts[kMaxV + 1];
    pts[0] = a;
    pts[n] = b;
    for (int i = 1; i < n; ++i) {
        float t = (float)i / (float)n;
        x3::phys::Vec3 base{ a.x + seg.x * t, a.y + seg.y * t, a.z + seg.z * t };
        // Alternating azimuth (i&1 flips ~180 deg) + jitter + slow drift -> a sharp
        // back-and-forth zigzag that also twists in 3D instead of staying planar.
        float azi = ((i & 1) ? kPi : 0.0f) + (rnd01() * 2.0f - 1.0f) * 0.7f + (float)i * 0.6f;
        float kinkDeg = 15.0f + rnd01() * 30.0f;           // 15..45 deg hard kink
        float r = segStep * std::tan(kinkDeg * kPi / 180.0f) * 0.5f;
        float ox = std::cos(azi) * r, oy = std::sin(azi) * r;
        pts[i] = x3::phys::Vec3{ base.x + u.x * ox + v.x * oy,
                                 base.y + u.y * ox + v.y * oy,
                                 base.z + u.z * ox + v.z * oy };
    }
    // Draw the main zigzag.
    for (int i = 1; i <= n; ++i)
        drawBoltSegment(device, frame, pts[i - 1], pts[i], eye, coreThick, brightness);

    // 1-2 short BRANCH forks off random interior kink points (thinner + dimmer).
    int nForks = (rnd01() < 0.55f) ? 2 : 1;
    for (int f = 0; f < nForks && n > 2; ++f) {
        int ki = 1 + (int)(rnd01() * (float)(n - 1));
        if (ki >= n) ki = n - 1;
        x3::phys::Vec3 fp = pts[ki];
        // Fork heads off mostly perpendicular to the main path (a real branch).
        float fa = rnd01() * 2.0f * kPi;
        x3::phys::Vec3 fdir = normalize(x3::phys::Vec3{
            u.x * std::cos(fa) + v.x * std::sin(fa) + dir.x * 0.25f,
            u.y * std::cos(fa) + v.y * std::sin(fa) + dir.y * 0.25f,
            u.z * std::cos(fa) + v.z * std::sin(fa) + dir.z * 0.25f });
        int fsegs = 2 + (int)(rnd01() * 2.0f);             // 2-3 short segments
        float fstep = segStep * (0.45f + rnd01() * 0.5f);
        x3::phys::Vec3 prev = fp;
        for (int j = 0; j < fsegs; ++j) {
            float ka = rnd01() * 2.0f * kPi;
            float kr = fstep * 0.4f;
            x3::phys::Vec3 nxt{
                prev.x + fdir.x * fstep + (u.x * std::cos(ka) + v.x * std::sin(ka)) * kr,
                prev.y + fdir.y * fstep + (u.y * std::cos(ka) + v.y * std::sin(ka)) * kr,
                prev.z + fdir.z * fstep + (u.z * std::cos(ka) + v.z * std::sin(ka)) * kr };
            drawBoltSegment(device, frame, prev, nxt, eye, coreThick * 0.55f, brightness * 0.6f);
            prev = nxt;
        }
    }
}

// ---------------------------------------------------------------------------
// draw: active tracers + muzzle flash. (The crosshair moved to the screen-space
// HUD layer in S7 — see app/hud.* — so fx no longer draws a world-space "+".)
// ---------------------------------------------------------------------------
void CombatFx::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const {
    if (!m_box.valid()) return;
    const x3::phys::Vec3 eyePos{ eyeX, eyeY, eyeZ };  // camera-facing lightning ribbons

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
        if (t.kind == WeaponFxKind::Lightning) {
            // HARD-ANGLE ZIGZAG bolt: bright white-blue core + tight blue glow, sharp
            // 15-45 deg kinks + branch forks, re-rolled ~15x/s. The bolt PROPAGATES: its
            // leading tip extends from the muzzle toward the hit point at kLightningBolt-
            // Speed (m/s) over the tracer's age, so it visibly travels.
            x3::phys::Vec3 seg{ t.to.x - t.from.x, t.to.y - t.from.y, t.to.z - t.from.z };
            float fullLen = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
            float reach   = kLightningBoltSpeed * t.age;          // how far the tip has travelled
            float frac    = (fullLen > 1e-4f) ? std::min(1.0f, reach / fullLen) : 1.0f;
            x3::phys::Vec3 tip{ t.from.x + seg.x * frac,
                                t.from.y + seg.y * frac,
                                t.from.z + seg.z * frac };
            // Re-roll the kink pattern every kLightningRerollPeriod (living crackle),
            // salted per-tracer (from-position hash) so simultaneous bolts differ.
            uint32_t bucket = (uint32_t)(t.age / kLightningRerollPeriod);
            uint32_t salt   = (uint32_t)(t.from.x * 73.1f) * 2246822519u
                            ^ (uint32_t)(t.from.y * 91.7f) * 3266489917u
                            ^ (uint32_t)(t.from.z * 53.3f) * 668265263u;
            float coreThick = kLightningCoreThick * (0.85f + 0.25f * k);
            float brightness = 0.75f + 0.35f * k;   // stays bright while held (new tracer each tick)
            drawLightningBolt(device, frame, t.from, tip, eyePos, coreThick, bucket ^ salt, brightness);
        } else {
            // Slightly taper the beam as it fades so it reads as a fast streak.
            float thick = kTracerThickness * (0.5f + 0.5f * k);
            drawBeam(device, frame, t.from, t.to, thick, tracerColor);
        }
    }

    // ---- Electric arc tendrils (lightning impact violence). ----
    // Each live arc is a short re-rolled zigzag whipping off the hit point, fading +
    // retracting as it dies. Fast re-roll (~33/s) so they crackle violently.
    for (const auto& a : m_arcs) {
        if (a.life <= 0.0f) continue;
        const float k = (a.maxLife > 0.0f) ? (a.life / a.maxLife) : 0.0f;   // 1 -> 0
        const float reach = a.len * (0.55f + 0.45f * k);                    // retract as it dies
        x3::phys::Vec3 tip{ a.base.x + a.dir.x * reach,
                            a.base.y + a.dir.y * reach,
                            a.base.z + a.dir.z * reach };
        const uint32_t bucket = (uint32_t)((a.maxLife - a.life) / 0.03f);   // ~33 re-rolls/s
        drawLightningBolt(device, frame, a.base, tip, eyePos,
                          kLightningCoreThick * 0.5f, a.seed ^ bucket, 0.85f * k);
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
