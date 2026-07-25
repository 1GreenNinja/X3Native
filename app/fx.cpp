// Combat FX: crosshair + shot tracers + muzzle flash. See app/fx.h.
//
// Clean-room: built from the IRenderDevice + Vec3 interfaces only. No id Tech /
// RBDOOM source consulted.
#include "fx.h"
#include "mesh_prims.h"

#include <cmath>
#include <cstdlib>   // std::rand / RAND_MAX (jagged lightning bolt jitter)

namespace x3::game {

namespace {

// Small deterministic integer hash + a [0,1) mapping. Used by the Lightning draw path
// for the IRREGULAR re-roll cadence and the per-re-roll brightness flicker. Kept out of
// CombatFx::m_rng on purpose: draw() is const and must not perturb the spawn RNG that
// headless captures depend on being repeatable.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float unitFromHash(uint32_t h) {   // [0,1)
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}

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
    for (auto& a : m_arcs) a.life = 0.0f;
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
    // LIGHTNING has NO muzzle flash (director note): the box flash + the bright soft
    // flash sprite read as an ugly bright blob in front of the beam. The jagged bolt
    // IS the read; suppress the flash entirely for the beam weapon. Other weapons
    // light the brief box flash as before.
    m_muzzleFlash = (kind == WeaponFxKind::Lightning) ? 0.0f : kMuzzleFlashTime;

    // Bias the muzzle spark cone forward along the shot direction (to - from).
    x3::phys::Vec3 dir{ to.x - from.x, to.y - from.y, to.z - from.z };
    spawnMuzzleFlash(from, dir, kind);
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
    // LIGHTNING (director note): the bolt "travels too fast" — slow the visible
    // effect by ~39% (x0.61). The only per-frame "motion" the eye reads at the
    // muzzle is the spark cone leaving the tip, so scale its launch speed by 0.61
    // for the beam weapon (other weapons unchanged).
    const float kLightningFxSpeed = 0.61f;
    const float speedScale = isLightning ? kLightningFxSpeed : 1.0f;
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
    // One bright soft flash sprite at the muzzle. LIGHTNING SKIPS THIS — the bright
    // soft blob in front of the beam was the "big bright flash blob" the director
    // wants gone; the jagged bolt is the read by itself.
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
        case WeaponFxKind::Lightning: // electric: few tiny fast sparks (arcs carry it)
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
    // LIGHTNING impact = electric VIOLENCE, not white puffballs (Tim): a tight
    // blue-white flash + a whipping ring of short crackling arc tendrils crawling
    // off the hit (drawn in draw() as re-rolled mini zigzags). The round sparks are
    // already cut to a few tiny fast specks above.
    if (kind == WeaponFxKind::Lightning) {
        Particle f;                       // one tight blue-white flash core
        f.pos = pos;
        f.life = f.maxLife = 0.08f;
        // SMALL + brief. At 0.30 m this additive sprite was a white BLOB sitting on the
        // strike — the exact "snowball" the arc tendrils exist to replace. It should be
        // a spark of light at the contact point, not the subject of the frame.
        f.size0 = 0.13f; f.size1 = 0.02f;
        f.r = 1.8f; f.g = 2.5f; f.b = 3.8f; f.a0 = 1.0f;
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

// ---------------------------------------------------------------------------
// boltFx: make a travelling PROJECTILE visible in flight (plasma/rocket bolts
// were invisible — only the on-hit tracer showed). Called once per frame per
// live projectile: drops a hot additive CORE billboard at the bolt position + a
// dimmer TRAIL speck just behind it (along -vel). 60 fps of overlapping cores
// reads as a continuous glowing bolt with a fading tail. Rocket additionally
// puffs a little alpha smoke so the exhaust lingers.
// ---------------------------------------------------------------------------
void CombatFx::boltFx(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel, WeaponFxKind kind) {
    x3::phys::Vec3 v = normalize(vel);
    // Per-kind bolt tint (linear HDR -> feeds the additive bloom chain) + size.
    float cr, cg, cb, coreSize;
    switch (kind) {
        case WeaponFxKind::Plasma:    cr = 0.5f; cg = 1.9f; cb = 6.0f; coreSize = 0.16f; break; // blue-cyan
        case WeaponFxKind::Rocket:    cr = 6.0f; cg = 2.2f; cb = 0.5f; coreSize = 0.20f; break; // orange fire
        case WeaponFxKind::Lightning: cr = 2.0f; cg = 3.0f; cb = 4.0f; coreSize = 0.12f; break; // white-blue
        default:                      cr = 5.0f; cg = 3.4f; cb = 1.0f; coreSize = 0.13f; break; // hot yellow
    }
    // Hot core at the bolt position (near-static: it just marks where the bolt is
    // THIS frame; a short life so a despawned bolt's cores fade instantly).
    {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ 0, 0, 0 };
        p.life = p.maxLife = 0.06f;
        p.size0 = coreSize; p.size1 = coreSize * 0.7f;
        p.r = cr; p.g = cg; p.b = cb; p.a0 = 1.0f;
        p.gravity = 0.0f; p.drag = 0.0f; p.additive = true;
        spawnParticle(p);
    }
    // Trail speck a little behind (fading tail).
    {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x - v.x * coreSize * 2.0f,
                                pos.y - v.y * coreSize * 2.0f,
                                pos.z - v.z * coreSize * 2.0f };
        p.vel = x3::phys::Vec3{ 0, 0, 0 };
        p.life = p.maxLife = 0.12f;
        p.size0 = coreSize * 0.7f; p.size1 = coreSize * 0.15f;
        p.r = cr * 0.6f; p.g = cg * 0.6f; p.b = cb * 0.6f; p.a0 = 0.8f;
        p.gravity = 0.0f; p.drag = 0.0f; p.additive = true;
        spawnParticle(p);
    }
    // Rocket: an alpha smoke puff so the exhaust trail lingers behind the warhead.
    if (kind == WeaponFxKind::Rocket) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x - v.x * 0.3f, pos.y - v.y * 0.3f, pos.z - v.z * 0.3f };
        p.vel = x3::phys::Vec3{ frandSym() * 0.3f, 0.2f + frand() * 0.3f, frandSym() * 0.3f };
        p.life = p.maxLife = 0.7f + frand() * 0.4f;
        p.size0 = 0.12f; p.size1 = 0.5f;      // grows + dissipates
        p.r = 0.16f; p.g = 0.15f; p.b = 0.14f; // sooty grey exhaust
        p.a0 = 0.45f;
        p.gravity = -0.1f; p.drag = 1.2f; p.additive = false;
        spawnParticle(p);
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

// ---------------------------------------------------------------------------
// spawnExplosion: a bright ADDITIVE orange/yellow fireball + dark smoke at
// `center`, sized by `radius` (playtest "barrels look like red boxes" fix).
// Hot additive cores (HDR r/g/b > 1) feed the bloom chain so a shot barrel
// reads as a violent fireball instead of just scattered red chunks.
// ---------------------------------------------------------------------------
void CombatFx::spawnExplosion(const x3::phys::Vec3& center, float radius) {
    const float r = (radius > 0.2f) ? radius : 0.2f;
    // (1) Fireball CORE: a dense ball of hot additive orange/yellow puffs that flash
    // bright then shrink — the bloom-feeding heart of the blast.
    const int nCore = 26;
    for (int i = 0; i < nCore; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ center.x + frandSym() * r * 0.4f,
                                center.y + frandSym() * r * 0.4f,
                                center.z + frandSym() * r * 0.4f };
        const float speed = (2.0f + frand() * 5.0f) * r;
        p.vel = x3::phys::Vec3{ frandSym() * speed,
                                frand() * speed * 0.7f + 0.8f,
                                frandSym() * speed };
        p.life = p.maxLife = 0.25f + frand() * 0.30f;
        p.size0 = (0.18f + frand() * 0.18f) * r;   // big hot puff
        p.size1 = 0.03f * r;                        // collapse to a spark
        // HDR additive orange->yellow core (intensity > 1 feeds bloom).
        p.r = 5.0f; p.g = 2.2f + frand() * 1.0f; p.b = 0.45f;
        p.a0 = 1.0f;
        p.gravity = -0.2f; p.drag = 2.2f; p.additive = true;
        spawnParticle(p);
    }
    // (2) Outward EMBER spray: fast hot additive specks flung past the core radius.
    const int nEmber = 18;
    for (int i = 0; i < nEmber; ++i) {
        Particle p;
        p.pos = center;
        const float speed = (5.0f + frand() * 8.0f) * r;
        p.vel = x3::phys::Vec3{ frandSym() * speed,
                                frand() * speed + 1.0f,
                                frandSym() * speed };
        p.life = p.maxLife = 0.3f + frand() * 0.4f;
        p.size0 = 0.05f * r; p.size1 = 0.012f * r;
        p.r = 4.5f; p.g = 1.4f; p.b = 0.30f;        // hot orange ember
        p.a0 = 1.0f;
        p.gravity = 0.8f; p.drag = 1.4f; p.additive = true;
        spawnParticle(p);
    }
    // (3) Dark rolling SMOKE so the fireball leaves a believable plume.
    const int nSmoke = 10;
    for (int i = 0; i < nSmoke; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ center.x + frandSym() * r * 0.5f,
                                center.y + frandSym() * r * 0.3f,
                                center.z + frandSym() * r * 0.5f };
        p.vel = x3::phys::Vec3{ frandSym() * 0.8f, 0.8f + frand() * 1.0f, frandSym() * 0.8f };
        p.life = p.maxLife = 1.0f + frand() * 1.2f;
        p.size0 = 0.25f * r; p.size1 = 1.0f * r;    // grows + dissipates
        p.r = 0.10f; p.g = 0.09f; p.b = 0.08f;      // sooty dark smoke
        p.a0 = 0.5f;
        p.gravity = -0.18f; p.drag = 1.0f; p.additive = false;
        spawnParticle(p);
    }
    // A scorch decal under the blast (up-facing) so the ground reads burned.
    addDecal(x3::phys::Vec3{ center.x, center.y - r * 0.8f, center.z },
             x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });
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

// ===========================================================================
// SHIP-SCALE damage-state FX (space combat readability — see fx.h). Sizes are
// ~5x the on-foot presets so the read survives dogfight range (a 0.3 m puff is
// sub-pixel at 100 m; a 2.5 m churn is a plume). No gravity: this is space.
// ===========================================================================
void CombatFx::spawnShipSparks(const x3::phys::Vec3& pos) {
    const int n = 10;
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 1.2f,
                                pos.y + frandSym() * 1.2f,
                                pos.z + frandSym() * 1.2f };
        const float speed = 6.0f + frand() * 10.0f;
        p.vel = x3::phys::Vec3{ frandSym() * speed, frandSym() * speed, frandSym() * speed };
        p.life = p.maxLife = 0.20f + frand() * 0.25f;
        p.size0 = 0.22f + frand() * 0.14f;
        p.size1 = 0.05f;
        p.r = 4.5f; p.g = 2.4f; p.b = 0.7f;    // hot electrical-orange (HDR -> bloom)
        p.a0 = 1.0f;
        p.gravity = 0.0f; p.drag = 1.6f; p.additive = true;
        spawnParticle(p);
    }
}

void CombatFx::spawnShipSmoke(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel,
                              float heavy01) {
    const float h = heavy01 < 0.0f ? 0.0f : (heavy01 > 1.0f ? 1.0f : heavy01);
    const int n = 2 + (int)(2.0f * h);
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.8f,
                                pos.y + frandSym() * 0.8f,
                                pos.z + frandSym() * 0.8f };
        // Inherit a fraction of the ship's velocity + a slow random drift: the
        // puffs stream back along the flight path (a REAL trail, not a bead
        // chain of stationary blobs the ship leaves behind at 90 m/s).
        p.vel = x3::phys::Vec3{ vel.x * 0.35f + frandSym() * 1.5f,
                                vel.y * 0.35f + frandSym() * 1.5f,
                                vel.z * 0.35f + frandSym() * 1.5f };
        p.life = p.maxLife = 1.4f + frand() * 1.2f + 0.8f * h;
        p.size0 = 0.9f + 0.8f * h;
        p.size1 = 2.6f + 2.2f * h;             // grows into a dissipating plume
        const float g = 0.16f - 0.08f * h;     // heavy damage = darker smoke
        p.r = g; p.g = g; p.b = g;
        p.a0 = 0.34f + 0.28f * h;
        p.gravity = 0.0f; p.drag = 0.55f; p.additive = false;
        spawnParticle(p);
    }
}

void CombatFx::spawnShipEmber(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel) {
    // A licking fire glow at the wound + a couple of shed embers.
    Particle f;
    f.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.9f,
                            pos.y + frandSym() * 0.9f,
                            pos.z + frandSym() * 0.9f };
    f.vel = x3::phys::Vec3{ vel.x * 0.85f, vel.y * 0.85f, vel.z * 0.85f };
    f.life = f.maxLife = 0.22f + frand() * 0.18f;
    f.size0 = 0.9f + frand() * 0.5f;
    f.size1 = 0.25f;
    f.r = 5.0f; f.g = 1.9f; f.b = 0.45f;       // HDR fire orange -> bloom halo
    f.a0 = 1.0f; f.gravity = 0.0f; f.drag = 0.4f; f.additive = true;
    spawnParticle(f);
    for (int i = 0; i < 2; ++i) {
        Particle p;
        p.pos = f.pos;
        p.vel = x3::phys::Vec3{ vel.x * 0.3f + frandSym() * 5.0f,
                                vel.y * 0.3f + frandSym() * 5.0f,
                                vel.z * 0.3f + frandSym() * 5.0f };
        p.life = p.maxLife = 0.5f + frand() * 0.5f;
        p.size0 = 0.2f; p.size1 = 0.04f;
        p.r = 4.0f; p.g = 1.4f; p.b = 0.3f;
        p.a0 = 1.0f; p.gravity = 0.0f; p.drag = 0.8f; p.additive = true;
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
            t.life -= dt;
            t.age  += dt;   // drives the Lightning bolt's propagation reach
            if (t.life < 0.0f) t.life = 0.0f;
        }
    }
    if (m_muzzleFlash > 0.0f) {
        m_muzzleFlash -= dt;
        if (m_muzzleFlash < 0.0f) m_muzzleFlash = 0.0f;
    }
    for (auto& a : m_arcs) {
        if (a.life > 0.0f) { a.life -= dt; if (a.life < 0.0f) a.life = 0.0f; }
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
// drawTracerBillboard: a thin CAMERA-FACING ribbon a->b (playtest "chaingun
// fires a square rod" fix). The width axis = normalize(cross(dir, view)) so the
// flat side always faces the eye; the depth axis is collapsed to a sliver so it
// reads as a streak, not a box. Degenerate (segment ~parallel to the view) ->
// thin beam fallback so a head-on tracer still draws.
// ---------------------------------------------------------------------------
void CombatFx::drawTracerBillboard(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                                   const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                                   const x3::phys::Vec3& eye, float width,
                                   const float color[4]) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-5f) return;
    x3::phys::Vec3 dir = normalize(seg);

    // View direction from the eye to the segment midpoint.
    x3::phys::Vec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    x3::phys::Vec3 view{ mid.x - eye.x, mid.y - eye.y, mid.z - eye.z };
    float vl = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
    if (vl < 1e-5f) { drawBeam(device, frame, a, b, width * 0.5f, color); return; }
    view = x3::phys::Vec3{ view.x / vl, view.y / vl, view.z / vl };

    // Ribbon WIDTH axis = perpendicular to both the segment and the view dir. When
    // the segment points nearly along the view (head-on), this cross product is
    // tiny -> a thin beam reads fine there, so fall back.
    x3::phys::Vec3 w = cross(dir, view);
    float wl = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    if (wl < 1e-3f) { drawBeam(device, frame, a, b, width * 0.5f, color); return; }
    w = x3::phys::Vec3{ w.x / wl, w.y / wl, w.z / wl };
    // Thin axis = face normal toward the eye (so the flat quad faces the camera).
    // MIRROR (KNOWN_BUGS R3): this was cross(w, dir), which makes [w, nrm, dir] a
    // LEFT-handed basis (det -1) — a reflection. Harmless-looking on a symmetric
    // flat-shaded box, but it is the same bug class that ate the rift gate, and the
    // --test-basis invariant is total: no negative determinants, anywhere.
    // cross(dir, w) is the right-handed one: det[w, dir x w, dir] = +1.
    x3::phys::Vec3 nrm = cross(dir, w);

    // Unit box (half-extent 0.5): WIDTH across `w`, a sliver of depth along `nrm`,
    // full `len` along the segment. width is the full ribbon width.
    float model[16];
    composeTRS3(model, w, nrm, dir, width, width * 0.06f, len, mid);
    device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, color, model);
}

// ---------------------------------------------------------------------------
// drawBoltSegment: one straight zigzag segment a->b as a camera-facing GLOW
// ribbon (wide, dim blue) + a thinner white-hot CORE ribbon inside it, both via
// drawMeshEmissive so the HDR emissive term drives the bloom halo (bright white
// core, tight blue-white glow — Tim spec). The ribbon WIDTH axis is perpendicular
// to both the segment and the eye->segment view dir so the flat side faces the
// camera (never a square rod, even when the bolt points near the eye).
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
    // GLOW ribbon: a soft BLUE corona around the core. Its width is now PROPORTIONAL to
    // coreThick — the original pinned it to the kLightningGlowThick constant, so the
    // thin branch forks (coreThick*0.55) and the thin arc tendrils (coreThick*0.5) were
    // still wrapped in a FULL-WIDTH glow slab. That is why every strand read equally
    // fat and the impact ring looked like a shuriken of white planks. Now a thin strand
    // gets a thin corona.
    // It is also deliberately DIM (radiance ~ rgb*strength stays near 1): its job is to
    // tint the bloom blue around the core, NOT to be a second bright bar. Crank this and
    // the bolt degenerates back into a white slab.
    {
        float model[16];
        const float gw = kLightningGlowThick * (coreThick / kLightningCoreThick);
        composeTRS3(model, w, nrm, dir, gw, gw * 0.30f, len, mid);
        const float emis[4] = { 0.10f, 0.38f, 1.00f, 0.80f * brightness };  // blue corona
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emis, model);
    }
    // CORE ribbon: a THIN white-hot thread — the only genuinely hot surface, and ~4x
    // narrower than the glow. Dropped from 3.4 to 2.5 strength: at 3.4 the whole ribbon
    // saturated and CLIPPED to flat white (killing the hot-core / cool-falloff read, so
    // it looked like a painted white bar). Still comfortably HDR (>1), so bloom smears a
    // bright thread into a halo instead of the geometry BEING the halo.
    {
        float model[16];
        composeTRS3(model, w, nrm, dir, coreThick, coreThick * 0.5f, len, mid);
        const float emis[4] = { 1.3f, 1.9f, 3.0f, 2.5f * brightness };      // white-blue core
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emis, model);
    }
}

// ---------------------------------------------------------------------------
// NATURAL FRACTAL LIGHTNING (Tim: "we need natural lightning").
//
// The old builder WALKED muzzle->hit in fixed ~0.30 m runs and kicked every joint by a
// random 25-60 deg. A uniform step with a uniform kick is a REGULAR ZIGZAG — a stylized
// bolt icon — and no amount of tuning segLen/angle fixes that; it only makes a finer
// regular zigzag. The problem was the algorithm, not the constants.
//
// Real lightning is SELF-SIMILAR: big lazy bends, with smaller kinks riding on them, and
// smaller kinks on those. So build it the other way round — start from the single
// straight a->b segment and RECURSIVELY SUBDIVIDE, displacing each midpoint
// perpendicular to its OWN parent segment and decaying the displacement each level:
//
//     subdivide(A,B,d,n):  M = mid(A,B) + randPerp(d)
//                          subdivide(A,M, d*DECAY, n-1)
//                          subdivide(M,B, d*DECAY, n-1)
//
// DECAY (kLightningDecay) is the naturalness knob — it IS the fractal dimension. The
// initial displacement is a FRACTION OF LENGTH, so a 2 m bolt and a 25 m bolt look
// equally natural (the old fixed-metre step is exactly why short bolts read as coat
// hangers). Branches are recursive CHILD bolts that fork again, taper, and mostly die
// partway — a dead-end tendril is one of the strongest naturalness cues.
// ---------------------------------------------------------------------------
void CombatFx::boltSubdivide(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                             const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                             const x3::phys::Vec3& eye, BoltRng& rng,
                             float displace, int depth, int branchDepth,
                             float coreThick, float brightness, float t0, float t1) const {
    if (depth <= 0) {
        // LEAF: emit the actual ribbon pair. TAPER along the strand — thickness and
        // brightness fall off toward the tip (a strand that ends as thick as it started
        // reads fake; nature tapers). t is the strand-relative position of this leaf.
        const float t     = 0.5f * (t0 + t1);
        const float taper = 1.0f - 0.55f * t;            // 1.0 at the root -> 0.45 at the tip
        drawBoltSegment(device, frame, a, b, eye, coreThick * taper, brightness * taper);
        return;
    }

    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    const float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-5f) return;
    const x3::phys::Vec3 dir{ seg.x / len, seg.y / len, seg.z / len };

    // Perpendicular basis of THIS segment (not of the trunk) — that is what makes the
    // displacement follow the local bend instead of all wobbling in one shared plane.
    const x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                          : x3::phys::Vec3{ 1, 0, 0 };
    const x3::phys::Vec3 u = normalize(cross(ref, dir));
    const x3::phys::Vec3 v = cross(dir, u);

    // Midpoint, kicked perpendicular by a random azimuth at ~`displace` magnitude. The
    // magnitude is itself jittered so successive levels never share one amplitude (that
    // sameness is what read as a repeating motif).
    const float azi = rng() * 6.2831853f;
    const float mag = displace * (0.55f + rng() * 0.9f);
    const float ox  = std::cos(azi) * mag, oy = std::sin(azi) * mag;
    const x3::phys::Vec3 m{ (a.x + b.x) * 0.5f + u.x * ox + v.x * oy,
                            (a.y + b.y) * 0.5f + u.y * ox + v.y * oy,
                            (a.z + b.z) * 0.5f + u.z * ox + v.z * oy };
    const float tm = 0.5f * (t0 + t1);

    const float nextDisp = displace * kLightningDecay;
    boltSubdivide(device, frame, a, m, eye, rng, nextDisp, depth - 1, branchDepth,
                  coreThick, brightness, t0, tm);
    boltSubdivide(device, frame, m, b, eye, rng, nextDisp, depth - 1, branchDepth,
                  coreThick, brightness, tm, t1);

    // ---- BRANCH: a recursive CHILD bolt hung off this midpoint ----------------
    // It inherits the parent's direction rotated 15-40 deg off-axis, takes a fraction of
    // the remaining parent length, subdivides with its OWN fractal detail (so branches
    // branch), and inherits REDUCED brightness + thickness. It ends in open air — only
    // the trunk is required to terminate on the hit point.
    if (branchDepth >= kLightningMaxBranchDepth) return;
    float chance = kLightningBranchChance;
    for (int i = 0; i < branchDepth; ++i) chance *= kLightningBranchDecay;
    // Only fork off the upper subdivision levels — forking at the finest scale just adds
    // fuzz, not structure.
    if (depth < 2 || rng() >= chance) return;

    // Rotate the parent's direction 15-40 deg off-axis about a random perpendicular.
    const float ang = (15.0f + rng() * 25.0f) * 3.14159265f / 180.0f;
    const float ba  = rng() * 6.2831853f;
    const x3::phys::Vec3 perp{ u.x * std::cos(ba) + v.x * std::sin(ba),
                               u.y * std::cos(ba) + v.y * std::sin(ba),
                               u.z * std::cos(ba) + v.z * std::sin(ba) };
    const float cs = std::cos(ang), sn = std::sin(ang);
    const x3::phys::Vec3 bdir = normalize(x3::phys::Vec3{ dir.x * cs + perp.x * sn,
                                                          dir.y * cs + perp.y * sn,
                                                          dir.z * cs + perp.z * sn });
    // Length: a fraction of what's LEFT of the parent, jittered. Most branches die well
    // short of anything (the dead-end tendril).
    const float remain = len * 0.5f;                    // this midpoint -> b
    const float blen   = remain * kLightningBranchLenFrac * (0.5f + rng() * 1.0f);
    if (blen < 0.05f) return;
    const x3::phys::Vec3 bend{ m.x + bdir.x * blen, m.y + bdir.y * blen, m.z + bdir.z * blen };

    boltSubdivide(device, frame, m, bend, eye, rng,
                  blen * kLightningDisplaceFrac,        // scale-relative, like the trunk
                  depth - 1, branchDepth + 1,
                  coreThick * 0.55f,                    // children are THINNER
                  brightness * 0.55f,                   // ...and DIMMER
                  0.35f, 1.0f);                         // start part-tapered, fade to the tip
}

// ---------------------------------------------------------------------------
// drawLightningBolt: seed the fractal and emit the trunk. Endpoints are EXACT —
// `a` is the muzzle (weaponMuzzle(): the measured barrel tip), `b` the hit point /
// propagation tip. Deterministic from `seed`.
// ---------------------------------------------------------------------------
void CombatFx::drawLightningBolt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                                 const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                                 const x3::phys::Vec3& eye,
                                 float coreThick, uint32_t seed, float brightness) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    const float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-4f) { drawBoltSegment(device, frame, a, b, eye, coreThick, brightness); return; }

    BoltRng rng{ seed * 2654435761u + 0x9E3779B9u };
    if (rng.s == 0u) rng.s = 1u;

    // Short strands (the impact arc tendrils) don't need — and can't afford — the full
    // trunk depth; scale the subdivision to the strand's length so a 0.4 m tendril isn't
    // paying for 64 segments.
    int depth = kLightningFractalDepth;
    if (len < 1.0f) depth = 4;
    if (len < 0.4f) depth = 3;

    boltSubdivide(device, frame, a, b, eye, rng,
                  len * kLightningDisplaceFrac,   // initial kick scales with LENGTH
                  depth, 0, coreThick, brightness, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// draw: active tracers + muzzle flash. (The crosshair moved to the screen-space
// HUD layer in S7 — see app/hud.* — so fx no longer draws a world-space "+".)
// ---------------------------------------------------------------------------
void CombatFx::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const {
    if (!m_box.valid()) return;
    const x3::phys::Vec3 eyePos{ eyeX, eyeY, eyeZ };  // camera-facing tracer ribbons

    // Bright FX colors (baseColorFactor multiplies the default white texel).
    const float tracerColor[4]    = { 1.0f, 0.95f, 0.4f, 1.0f }; // hot yellow
    const float muzzleColor[4]    = { 1.0f, 0.85f, 0.4f, 1.0f }; // muzzle orange

    // ---- Camera basis (device convention: fwd = (cp*cy, sp, cp*sy)). ----
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    x3::phys::Vec3 right{ -sy, 0.0f, cy };
    x3::phys::Vec3 up = cross(right, forward);  // used by the muzzle-flash basis
    // A model matrix's local +Z is BACK, not forward (CONVENTIONS: an unrotated model
    // faces local -Z). [right, up, forward] has det -1 — a MIRROR (KNOWN_BUGS R3).
    // [right, up, back] is the rotation, det +1.
    const x3::phys::Vec3 back{ -forward.x, -forward.y, -forward.z };

    // ---- Tracers: thin bright beams, fading thinner as they age. ----
    for (const auto& t : m_tracers) {
        if (t.life <= 0.0f) continue;
        float k = (kTracerTime > 0.0f) ? (t.life / kTracerTime) : 1.0f; // 1->0
        if (t.kind == WeaponFxKind::Lightning) {
            // HARD-ANGLE ZIGZAG bolt (Tim spec): bright white-blue core + tight blue
            // glow, sharp 15-45 deg kinks + branch forks, re-rolled ~15x/s. The bolt
            // PROPAGATES: its leading tip extends from the muzzle toward the hit point
            // at kLightningBoltSpeed (m/s) over the tracer's age, so it visibly travels.
            x3::phys::Vec3 seg{ t.to.x - t.from.x, t.to.y - t.from.y, t.to.z - t.from.z };
            float fullLen = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
            float reach   = kLightningBoltSpeed * t.age;          // how far the tip has travelled
            float frac    = (fullLen > 1e-4f) ? std::min(1.0f, reach / fullLen) : 1.0f;
            x3::phys::Vec3 tip{ t.from.x + seg.x * frac,
                                t.from.y + seg.y * frac,
                                t.from.z + seg.z * frac };
            // Salt per-tracer (from-position hash) so simultaneous bolts differ.
            uint32_t salt   = (uint32_t)(t.from.x * 73.1f) * 2246822519u
                            ^ (uint32_t)(t.from.y * 91.7f) * 3266489917u
                            ^ (uint32_t)(t.from.z * 53.3f) * 668265263u;
            // IRREGULAR RE-ROLL. A fixed 65 ms bucket is a METRONOME, and a metronome
            // reads as a machine. Walk the buckets forward accumulating a JITTERED
            // duration each (~40-90 ms), so the bolt re-rolls at an uneven cadence. This
            // is deterministic from age+salt (headless captures stay repeatable) and
            // costs ~2 iterations at kTracerTime = 0.12 s.
            uint32_t bucket = 0;
            float    acc    = 0.0f;
            while (acc < t.age && bucket < 64u) {
                const uint32_t h = hash32(bucket ^ salt);
                acc += kLightningRerollPeriod * (0.6f + 0.8f * unitFromHash(h));
                ++bucket;
            }
            const uint32_t boltSeed = bucket ^ salt;
            // Real arcs pulse in INTENSITY, not just in shape — vary brightness per
            // re-roll instead of holding one steady value.
            const float flicker = 0.78f + 0.42f * unitFromHash(hash32(boltSeed ^ 0xA5A5A5A5u));
            float coreThick  = kLightningCoreThick * (0.85f + 0.25f * k);
            float brightness = (0.75f + 0.35f * k) * flicker;
            drawLightningBolt(device, frame, t.from, tip, eyePos, coreThick, boltSeed, brightness);
        } else {
            // Camera-facing ribbon (playtest "square rod" fix): a thin flat streak
            // that always faces the eye instead of drawBeam's world-fixed square
            // cross-section box. Taper the width as it fades for a fast-streak feel.
            float width = kTracerThickness * (0.5f + 0.5f * k);
            drawTracerBillboard(device, frame, t.from, t.to, eyePos, width, tracerColor);
        }
    }

    // ---- Electric arc tendrils (lightning impact violence). ----
    // Each live arc is a short re-rolled zigzag whipping off the hit point, fading
    // + retracting as it dies. Fast re-roll (~33/s) so they crackle violently.
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
        composeTRS3(m, right, up, back, s, s, s, m_muzzlePos);   // det +1 (was -1: forward)
        device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, muzzleColor, m);
    }
}

} // namespace x3::game
