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
    for (auto& l : m_licks) l.life = 0.0f;
    m_muzzleFlash = 0.0f;
    m_nextTracer = 0;
    m_nextArc = 0;
    m_nextLick = 0;
}

// ---------------------------------------------------------------------------
// spawnFlameLick: claim a ring slot for one velocity-stretched fire tongue.
// Bounded (kMaxFlameLicks; oldest recycled). See the fx.h note: licks exist
// because the camera-facing particle billboards can only be CIRCLES, and a
// cone of circles reads as puffs — Tim: "fire should be flames not puffs".
// ---------------------------------------------------------------------------
void CombatFx::spawnFlameLick(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel,
                              float len, float width, float bright, float life) {
    FlameLick& l = m_licks[m_nextLick];
    m_nextLick = (m_nextLick + 1) % kMaxFlameLicks;
    l.pos = pos; l.vel = vel;
    l.len = len; l.width = width; l.bright = bright;
    l.life = l.maxLife = life;
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
void CombatFx::addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to, WeaponFxKind kind,
                         float widthOverride, const x3::phys::Vec3* carrierVel) {
    Tracer& t = m_tracers[m_nextTracer];
    t.from = from;
    t.to   = to;
    t.life = kTracerTime;
    t.age  = 0.0f;        // Lightning bolt grows from the muzzle over time
    t.width = widthOverride;
    t.kind = kind;
    // The beam rides the shooter's velocity (see addTracer's note in fx.h): zero
    // for every on-foot caller, the ship's velocity for space combat.
    t.vel  = carrierVel ? *carrierVel : x3::phys::Vec3{ 0.0f, 0.0f, 0.0f };
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
        case WeaponFxKind::Lightning: // electric crackle: LIGHT BLUE, twitchy fast (blue-forward,
                                      // fewer/smaller sparks so the muzzle doesn't read as white dots)
            return { 1.8f, 4.0f, 7.2f,  2.0f, 4.5f, 7.5f, 9, 0.55f, 1.5f,  3.2f, 0.26f };
        case WeaponFxKind::Flame:     // IGNITION CONE: a fat orange flash + slower, wide,
                                      // larger tongues leaving the nozzle (fuel catching,
                                      // not a gunshot crack)
            return { 5.0f, 2.0f, 0.45f, 6.0f, 2.6f, 0.7f, 14, 1.5f, 0.75f, 2.8f, 0.34f };
        case WeaponFxKind::Frost:     // icy discharge: cyan-white, small tight cone
            return { 1.5f, 3.5f, 6.5f,  2.0f, 4.2f, 7.0f,  8, 0.9f, 0.9f,  1.6f, 0.30f };
        case WeaponFxKind::Napalm:    // heavy launcher pop (rocket-class, warmer)
            return { 5.5f, 2.4f, 0.6f,  6.5f, 3.2f, 1.0f, 12, 1.3f, 1.0f,  2.4f, 0.40f };
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
    // FLAME ignition: short FAT licks leaving the nozzle every shot, so the fire
    // body is connected to the gun from its first centimetre (with the stream's
    // own licks overlapping just ahead, the cone reads as ONE tongue, not beads).
    if (kind == WeaponFxKind::Flame) {
        for (int i = 0; i < 3; ++i) {
            const x3::phys::Vec3 lv{ d.x * (3.2f + frand() * 1.4f) + frandSym() * 0.5f,
                                     d.y * (3.2f + frand() * 1.4f) + 0.3f + frandSym() * 0.4f,
                                     d.z * (3.2f + frand() * 1.4f) + frandSym() * 0.5f };
            spawnFlameLick(x3::phys::Vec3{ pos.x + d.x * 0.15f, pos.y + d.y * 0.15f,
                                           pos.z + d.z * 0.15f },
                           lv, 0.40f + frand() * 0.15f, 0.12f, 1.6f, 0.10f + frand() * 0.05f);
        }
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
        case WeaponFxKind::Lightning: // electric: a FEW tiny fast LIGHT-BLUE specks (arcs carry the
                                      // read). Dimmed + fewer so their additive centers don't bloom to
                                      // white dots at the strike (Tim: "the white dots — eliminate that").
            return { 0.7f, 1.8f, 4.2f, 4, 0.34f, false };
        case WeaponFxKind::Flame:     // fire splash: orange tongues, no metal dust
                                      // (the scorch DECAL carries the aftermath)
            return { 5.0f, 1.9f, 0.4f, 12, 1.1f,  false };
        case WeaponFxKind::Frost:     // CRYSTALLINE burst: cyan-white shards (extra
                                      // falling glints + the icy decal in spawnImpact)
            return { 1.6f, 3.6f, 6.5f, 14, 0.8f,  false };
        case WeaponFxKind::Napalm:    // burning splatter (the fireball + pool carry it)
            return { 5.0f, 2.0f, 0.5f, 16, 1.3f,  true  };
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
    // LIGHTNING impact = electric SIZZLE, not a white dot (Tim, 2026-07-26: "the white
    // dots — eliminate that .. add more sizzle"). The bright additive flash CORE that
    // used to sit on the strike WAS the white blob — REMOVED. The electric read now
    // comes entirely from the whipping ring of short crackling arc tendrils (crawling
    // off the hit as re-rolled mini zigzags) plus the few tiny fast blue specks above:
    // a small electric impact, no white cloud.
    if (kind == WeaponFxKind::Lightning) {
        spawnArcs(pos, nrm);
    }
    // FROST impact = a CRYSTALLINE burst (weapon-vfx lane): beyond the cyan spray
    // above, a handful of small bright glints that ARC OUT AND FALL (gravity) like
    // shattering ice, plus a pale frost RING on the surface instead of a black
    // scorch — a cryo hit must not leave a burn mark.
    if (kind == WeaponFxKind::Frost) {
        for (int i = 0; i < 8; ++i) {
            Particle p;
            p.pos = pos;
            const float speed = 1.5f + frand() * 3.0f;
            p.vel = x3::phys::Vec3{ nrm.x * speed + frandSym() * 2.0f,
                                    nrm.y * speed + 1.0f + frand() * 1.5f,
                                    nrm.z * speed + frandSym() * 2.0f };
            p.life = p.maxLife = 0.35f + frand() * 0.30f;
            p.size0 = 0.030f + frand() * 0.02f; p.size1 = 0.008f;
            p.r = 2.2f; p.g = 4.2f; p.b = 7.0f;             // icy glint (HDR -> bloom)
            p.a0 = 1.0f;
            p.gravity = 1.2f; p.drag = 0.8f; p.additive = true;
            spawnParticle(p);
        }
        Decal& dc = m_decalsRing[m_nextDecal];              // pale frost ring, not scorch
        m_nextDecal = (m_nextDecal + 1) % kMaxDecals;
        dc.center   = pos;
        dc.normal   = nrm;
        dc.halfSize = 0.14f + frand() * 0.06f;
        dc.angle    = frand() * 6.2831853f;
        dc.life     = dc.maxLife = kDecalLife * 0.5f;       // frost melts off faster
        dc.color[0] = 0.45f; dc.color[1] = 0.62f; dc.color[2] = 0.80f;   // pale ice blue
        return;                                             // no dark scorch for cryo
    }
    // FLAME impact: a couple of brief upward fire licks at the strike so a burst
    // hitting a wall reads as fire catching, then the dark scorch decal below.
    if (kind == WeaponFxKind::Flame) {
        for (int i = 0; i < 3; ++i) {
            Particle p;
            p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.10f,
                                    pos.y + frandSym() * 0.10f,
                                    pos.z + frandSym() * 0.10f };
            p.vel = x3::phys::Vec3{ nrm.x * 0.5f + frandSym() * 0.4f,
                                    0.9f + frand() * 0.9f,
                                    nrm.z * 0.5f + frandSym() * 0.4f };
            p.life = p.maxLife = 0.25f + frand() * 0.20f;
            p.size0 = 0.10f; p.size1 = 0.20f;               // a tongue that spreads
            p.r = 4.5f; p.g = 1.7f; p.b = 0.35f;
            p.r1 = 1.8f; p.g1 = 0.25f; p.b1 = 0.06f;        // cools to deep red
            p.a0 = 1.0f;
            p.gravity = -0.08f; p.drag = 1.4f; p.additive = true;
            spawnParticle(p);
        }
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
    const int n = 9 + (int)(frand() * 5.0f);   // 9-13 tendrils (SIZZLE pass: more crackle)
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
void CombatFx::boltFx(const x3::phys::Vec3& pos, const x3::phys::Vec3& vel, WeaponFxKind kind,
                      float streamPhase) {
    x3::phys::Vec3 v = normalize(vel);
    // Per-kind bolt look — now a queryable table row (fx.h boltStyleFor) so the
    // flame/frost params are testable headlessly instead of literals in a switch.
    const BoltStyle st = boltStyleFor(kind);
    // FIRE reads as FLAMES, not puffs (Tim iteration): the connected body comes
    // from VELOCITY-STRETCHED licks, so the round soft core underneath shrinks to
    // a volume-filler instead of being the silhouette.
    // (Second cut: the soft round core stays FULL size — head-on down the stream
    // axis every velocity-stretched lick is end-on and nearly vanishes, so the
    // cores are what fill the center of the FP view; the licks carry the shape
    // from every other angle.)
    const bool  flame    = (kind == WeaponFxKind::Flame);
    const float coreSize = st.coreSize;
    // Hot core at the bolt position (near-static: it just marks where the bolt is
    // THIS frame; a short life so a despawned bolt's cores fade instantly). FIRE
    // cores drift UPWARD (st.rise) and GROW; ICE cores shrink — the style says.
    {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ 0, st.rise, 0 };
        p.life = p.maxLife = st.life;
        p.size0 = coreSize; p.size1 = coreSize * st.endScale;
        p.r = st.r; p.g = st.g; p.b = st.b;
        p.r1 = st.r1; p.g1 = st.g1; p.b1 = st.b1;   // orange->red as fire cools (== birth for legacy)
        p.a0 = 1.0f;
        p.gravity = 0.0f; p.drag = 0.0f; p.additive = true;
        spawnParticle(p);
    }
    // Trail speck a little behind (fading tail). FLAME skips it — the stretched
    // licks ARE the trail; a second bead chain would re-introduce the puff read.
    if (!flame) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x - v.x * coreSize * 2.0f,
                                pos.y - v.y * coreSize * 2.0f,
                                pos.z - v.z * coreSize * 2.0f };
        p.vel = x3::phys::Vec3{ 0, st.rise * 0.5f, 0 };
        p.life = p.maxLife = 0.12f;
        p.size0 = coreSize * 0.7f; p.size1 = coreSize * 0.15f;
        p.r = st.r * 0.6f; p.g = st.g * 0.6f; p.b = st.b * 0.6f;
        p.r1 = st.r1 * 0.6f; p.g1 = st.g1 * 0.6f; p.b1 = st.b1 * 0.6f;
        p.a0 = 0.8f;
        p.gravity = 0.0f; p.drag = 0.0f; p.additive = true;
        spawnParticle(p);
    }
    // FIRE: a large, DIM volume-filler core — the connective tissue between the
    // licks. Same trick that made the ground pool read as burning ground rather
    // than fireflies: sharp elements carry the silhouette, a soft wide low-alpha
    // body fills the gaps so no individual element reads as its own puff. Kept
    // dim on purpose (VALUE, NOT LUMENS) — it must never become the silhouette.
    if (flame) {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ 0.0f, st.rise * 0.6f, 0.0f };
        p.life = p.maxLife = st.life * 1.15f;
        p.size0 = coreSize * 2.3f;
        p.size1 = coreSize * 3.4f;                 // spreads into its neighbours
        p.r = 1.5f; p.g = 0.52f; p.b = 0.10f;      // dim ember-orange wash
        p.r1 = 0.6f; p.g1 = 0.12f; p.b1 = 0.02f;   // cools to deep red
        p.a0 = 0.30f;
        p.gravity = 0.0f; p.drag = 1.0f; p.additive = true;
        spawnParticle(p);
    }
    // FIRE: velocity-stretched LICKS — the connected flame body. RATE-based
    // (kFlameLickRate * dt, probabilistic carry) so 60 Hz and 165 Hz lay the same
    // ~0.43 m lick spacing (< lick length -> overlapped into one tongue).
    // streamPhase shapes the cone: near the NOZZLE (0) fat/bright/tight — one
    // connected body across the first ~40% of range; at the TAIL (1) narrower,
    // dimmer, laterally jittered so the fire breaks into separate curling licks.
    if (flame) {
        const float t = (streamPhase >= 0.0f) ? streamPhase : 0.35f;
        const float expect = kFlameLickRate * m_lastDt;
        int n = (int)expect;
        if (frand() < expect - (float)n) ++n;
        for (int i = 0; i < n; ++i) {
            const float jit = 0.2f + 1.0f * t;        // turbulence grows down-stream
            const x3::phys::Vec3 lv{ v.x * 1.6f + frandSym() * jit,
                                     v.y * 1.6f + 0.5f + frand() * 0.8f + frandSym() * jit * 0.5f,
                                     v.z * 1.6f + frandSym() * jit };
            spawnFlameLick(x3::phys::Vec3{ pos.x + frandSym() * 0.05f,
                                           pos.y + frandSym() * 0.05f,
                                           pos.z + frandSym() * 0.05f },
                           lv,
                           0.45f + frand() * 0.30f,            // length (m)
                           0.10f - 0.045f * t,                 // fat at the nozzle, thin at the tail
                           1.4f - 0.8f * t,                    // bright at the nozzle
                           0.13f + frand() * 0.08f + 0.05f * t); // tail licks linger a touch
        }
    }
    // FIRE: an occasional hot ember popping off the puff (~1 in 4 frames) — the
    // ragged edge that separates a flame from a glowing bolt.
    if (kind == WeaponFxKind::Flame && frand() < 0.25f) {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ frandSym() * 1.2f, 0.8f + frand() * 1.4f, frandSym() * 1.2f };
        p.life = p.maxLife = 0.20f + frand() * 0.15f;
        p.size0 = 0.05f; p.size1 = 0.012f;
        p.r = 5.0f; p.g = 1.6f; p.b = 0.3f;
        p.a0 = 1.0f; p.gravity = -0.05f; p.drag = 1.5f; p.additive = true;
        spawnParticle(p);
    }
    // ICE: an occasional tiny glint shed behind the crystal (sparkle, falls).
    if (kind == WeaponFxKind::Frost && frand() < 0.20f) {
        Particle p;
        p.pos = pos;
        p.vel = x3::phys::Vec3{ frandSym() * 0.8f, frandSym() * 0.5f, frandSym() * 0.8f };
        p.life = p.maxLife = 0.25f + frand() * 0.15f;
        p.size0 = 0.035f; p.size1 = 0.01f;
        p.r = 2.2f; p.g = 4.2f; p.b = 7.0f;
        p.a0 = 1.0f; p.gravity = 0.5f; p.drag = 1.0f; p.additive = true;
        spawnParticle(p);
    }
    // Rocket/napalm: an alpha smoke puff so the exhaust trail lingers behind the warhead.
    if (kind == WeaponFxKind::Rocket || kind == WeaponFxKind::Napalm) {
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

// ---------------------------------------------------------------------------
// firePoolFx: render one live NAPALM ground pool for THIS frame (weapon-vfx
// lane). dt-SCALED probabilistic emission (house rule: rate*dt with a random
// carry, so 60 Hz and 165 Hz integrate to the same average flame density — a
// per-frame count would triple the fire at a high refresh rate). Three layers:
// licking flame billboards (orange->red gradient, grow + rise), popping embers,
// and low black smoke. Flicker is intrinsic — every count/size/lifetime is
// jittered, so the pool shimmers instead of pulsing.
// Pool LOGIC (damage/expiry/water) lives in FirePoolSystem; this is only the look.
// ---------------------------------------------------------------------------
void CombatFx::firePoolFx(const x3::phys::Vec3& center, float radius, float dt) {
    if (dt <= 0.0f) return;
    const float r = (radius > 0.3f) ? radius : 0.3f;
    const float area = (r / 3.0f);   // emission scales with the pool footprint (3 m = the napalm pool)
    // dt-scaled probabilistic rounding: rate*dt whole part + a frand() carry.
    auto emitCount = [&](float ratePerSec) {
        const float x = ratePerSec * dt;
        int n = (int)x;
        if (frand() < (x - (float)n)) ++n;
        return n;
    };
    // A random point in the pool disc (sqrt for uniform area density).
    auto discPoint = [&]() {
        const float a = frand() * 6.2831853f;
        const float d = std::sqrt(frand()) * r * 0.9f;
        return x3::phys::Vec3{ center.x + std::cos(a) * d, center.y,
                               center.z + std::sin(a) * d };
    };
    // (0) GROUND GLOW: a soft stack of large, DIM additive sprites hugging the
    // pool center — the "the floor itself is alight" base layer under the tongues
    // (bare tongues alone read as fireflies, not a burning pool). ~12/s at ~0.35 s
    // life keeps ~4 alive; each is dim so the stack sums to a glow, not a blob.
    const int nGlow = emitCount(12.0f * area);
    for (int i = 0; i < nGlow; ++i) {
        Particle p;
        p.pos = discPoint();
        p.pos.y += 0.12f;
        p.vel = x3::phys::Vec3{ 0.0f, 0.15f, 0.0f };
        p.life = p.maxLife = 0.30f + frand() * 0.15f;
        p.size0 = r * (0.35f + frand() * 0.20f);
        p.size1 = p.size0 * 1.25f;
        p.r = 1.1f; p.g = 0.40f; p.b = 0.09f;               // dim ember-orange wash
        p.r1 = 0.5f; p.g1 = 0.12f; p.b1 = 0.03f;
        p.a0 = 0.35f;
        p.gravity = 0.0f; p.drag = 0.5f; p.additive = true;
        spawnParticle(p);
    }
    // (1) FLAME TONGUES: velocity-stretched LICKS rising off the ground ("flames
    // not puffs" — same fix as the stream: the round particle billboards can't
    // make an elongated tongue). ~22/s on the 3 m pool at ~0.6 s life keeps ~13
    // licks standing at any instant, curling upward via the lick buoyancy.
    const int nLick = emitCount(30.0f * area);
    for (int i = 0; i < nLick; ++i) {
        x3::phys::Vec3 lp = discPoint();
        lp.y += 0.10f;
        // A clumped PAIR per point: a lone sliver reads as a blade; two lobes
        // rising off one base read as a flame tongue.
        for (int k = 0; k < 2; ++k) {
            const x3::phys::Vec3 lv{ frandSym() * 0.50f,
                                     0.9f + frand() * 0.9f,
                                     frandSym() * 0.50f };
            spawnFlameLick(x3::phys::Vec3{ lp.x + frandSym() * 0.06f, lp.y,
                                           lp.z + frandSym() * 0.06f },
                           lv,
                           0.40f + frand() * 0.35f,     // tongue length
                           0.075f + frand() * 0.04f,    // width (sharp thread; bloom fuses)
                           1.15f + frand() * 0.35f,     // brightness (VALUE, NOT LUMENS)
                           0.40f + frand() * 0.30f);    // life (flicker variance)
        }
    }
    // (1b) soft flame cores under the tongues — reduced to a volume-filler now
    // that the licks carry the silhouette (was 42/s when they WERE the read).
    const int nFlame = emitCount(34.0f * area);
    for (int i = 0; i < nFlame; ++i) {
        Particle p;
        p.pos = discPoint();
        p.pos.y += 0.05f;
        p.vel = x3::phys::Vec3{ frandSym() * 0.35f, 0.7f + frand() * 1.1f, frandSym() * 0.35f };
        p.life = p.maxLife = 0.40f + frand() * 0.35f;
        p.size0 = 0.17f + frand() * 0.09f;
        p.size1 = p.size0 * (1.7f + frand() * 0.5f);        // spreads as it climbs
        p.r = 4.5f; p.g = 1.7f; p.b = 0.35f;                // hot orange (HDR -> bloom)
        p.r1 = 1.8f; p.g1 = 0.25f; p.b1 = 0.06f;            // cools to deep red at the tip
        p.a0 = 0.75f;                                       // soft bed, not discrete balls
        p.gravity = -0.06f; p.drag = 1.2f; p.additive = true;
        spawnParticle(p);
    }
    // (2) EMBERS: fast hot specks popping up off the burning ground.
    const int nEmber = emitCount(8.0f * area);
    for (int i = 0; i < nEmber; ++i) {
        Particle p;
        p.pos = discPoint();
        p.vel = x3::phys::Vec3{ frandSym() * 0.8f, 2.0f + frand() * 1.6f, frandSym() * 0.8f };
        p.life = p.maxLife = 0.4f + frand() * 0.4f;
        p.size0 = 0.035f; p.size1 = 0.01f;
        p.r = 5.0f; p.g = 1.5f; p.b = 0.3f;
        p.a0 = 1.0f; p.gravity = 0.8f; p.drag = 0.8f; p.additive = true;
        spawnParticle(p);
    }
    // (3) SMOKE: low sooty alpha puffs drifting off the fire.
    const int nSmoke = emitCount(4.0f * area);
    for (int i = 0; i < nSmoke; ++i) {
        Particle p;
        p.pos = discPoint();
        p.pos.y += 0.3f;
        p.vel = x3::phys::Vec3{ frandSym() * 0.3f, 0.8f + frand() * 0.6f, frandSym() * 0.3f };
        p.life = p.maxLife = 0.9f + frand() * 0.8f;
        p.size0 = 0.16f; p.size1 = 0.55f;
        p.r = 0.10f; p.g = 0.09f; p.b = 0.08f;
        p.a0 = 0.40f;
        p.gravity = -0.14f; p.drag = 1.0f; p.additive = false;
        spawnParticle(p);
    }
}

// ---------------------------------------------------------------------------
// extinguishFx: the one-shot STEAM burst for a napalm shell that lands in WATER
// (SurfaceType rule: no pools on water). Pale alpha puffs, faster and whiter
// than smoke — a hiss, not a burn.
// ---------------------------------------------------------------------------
void CombatFx::extinguishFx(const x3::phys::Vec3& pos) {
    for (int i = 0; i < 8; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + frandSym() * 0.3f,
                                pos.y + 0.05f,
                                pos.z + frandSym() * 0.3f };
        p.vel = x3::phys::Vec3{ frandSym() * 0.5f, 1.2f + frand() * 1.0f, frandSym() * 0.5f };
        p.life = p.maxLife = 0.6f + frand() * 0.5f;
        p.size0 = 0.12f; p.size1 = 0.45f;
        p.r = 0.55f; p.g = 0.58f; p.b = 0.60f;   // pale steam grey
        p.a0 = 0.45f;
        p.gravity = -0.20f; p.drag = 1.2f; p.additive = false;
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

void CombatFx::spawnShipMuzzle(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir,
                               const x3::phys::Vec3* carrierVel) {
    x3::phys::Vec3 d = normalize(dir);
    const x3::phys::Vec3 cv = carrierVel ? *carrierVel : x3::phys::Vec3{ 0.0f, 0.0f, 0.0f };
    // One bright core flash at the hardpoint...
    Particle f;
    f.pos = pos;
    f.vel = x3::phys::Vec3{ d.x * 2.0f + cv.x, d.y * 2.0f + cv.y, d.z * 2.0f + cv.z };
    f.life = f.maxLife = 0.07f;
    f.size0 = 0.85f; f.size1 = 0.15f;
    f.r = 5.5f; f.g = 4.2f; f.b = 2.2f;        // hot yellow-white (HDR -> bloom)
    f.a0 = 1.0f; f.gravity = 0.0f; f.drag = 0.0f; f.additive = true;
    spawnParticle(f);
    // ...plus a short forward spray so the discharge reads directional.
    for (int i = 0; i < 3; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ pos.x + d.x * 0.8f, pos.y + d.y * 0.8f, pos.z + d.z * 0.8f };
        p.vel = x3::phys::Vec3{ d.x * (14.0f + frand() * 10.0f) + frandSym() * 2.0f + cv.x,
                                d.y * (14.0f + frand() * 10.0f) + frandSym() * 2.0f + cv.y,
                                d.z * (14.0f + frand() * 10.0f) + frandSym() * 2.0f + cv.z };
        p.life = p.maxLife = 0.10f + frand() * 0.08f;
        p.size0 = 0.25f; p.size1 = 0.05f;
        p.r = 4.5f; p.g = 3.0f; p.b = 1.2f;
        p.a0 = 1.0f; p.gravity = 0.0f; p.drag = 0.5f; p.additive = true;
        spawnParticle(p);
    }
}

// ---------------------------------------------------------------------------
// spawnShipDeathBlast: a MASSIVE ship-disintegration burst (space power fantasy).
// Distinctly bigger than a barrel/infantry pop: a huge white-hot flash that blooms
// for a couple frames, a dense hot fireball, and an expanding shockwave shell of
// fast bright specks flung radially outward. Zero-gravity. See fx.h.
// ---------------------------------------------------------------------------
void CombatFx::spawnShipDeathBlast(const x3::phys::Vec3& center, float radius) {
    const float r = (radius > 1.0f) ? radius : 1.0f;
    // (1) WHITE-HOT FLASH: a few big, near-stationary billboards that flash huge
    //     then collapse — the bloom-blowing heart of the blast (reads for ~2-3
    //     frames as a searing white ball before the fireball colours take over).
    for (int i = 0; i < 4; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ center.x + frandSym() * r * 0.15f,
                                center.y + frandSym() * r * 0.15f,
                                center.z + frandSym() * r * 0.15f };
        p.vel = x3::phys::Vec3{ frandSym() * 1.5f, frandSym() * 1.5f, frandSym() * 1.5f };
        p.life = p.maxLife = 0.30f + frand() * 0.10f;
        p.size0 = (1.0f + frand() * 0.5f) * r;   // huge searing ball
        p.size1 = 0.10f * r;
        p.r = 9.0f; p.g = 7.5f; p.b = 5.5f;      // white-hot (HDR -> hard bloom)
        p.a0 = 1.0f; p.gravity = 0.0f; p.drag = 3.0f; p.additive = true;
        spawnParticle(p);
    }
    // (2) FIREBALL CORE: a dense ball of hot additive orange/yellow puffs, scaled
    //     up hard vs spawnExplosion (more, bigger, hotter) so the fireball fills
    //     the ship's silhouette instead of peppering it.
    const int nCore = 40;
    for (int i = 0; i < nCore; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ center.x + frandSym() * r * 0.5f,
                                center.y + frandSym() * r * 0.5f,
                                center.z + frandSym() * r * 0.5f };
        const float speed = (2.5f + frand() * 5.0f) * r;
        p.vel = x3::phys::Vec3{ frandSym() * speed, frandSym() * speed, frandSym() * speed };
        p.life = p.maxLife = 0.35f + frand() * 0.40f;
        p.size0 = (0.30f + frand() * 0.25f) * r;   // big hot puff
        p.size1 = 0.05f * r;
        p.r = 6.0f; p.g = 2.6f + frand() * 1.2f; p.b = 0.5f;   // hot orange->yellow
        p.a0 = 1.0f; p.gravity = 0.0f; p.drag = 1.8f; p.additive = true;
        spawnParticle(p);
    }
    // (3) SHOCKWAVE SHELL: fast, bright specks flung radially outward at ~uniform
    //     speed so they read as an expanding blast ring/sphere sweeping past the
    //     fireball, then wink out. Thin + short-lived (the leading edge of the blast).
    const int nShock = 56;
    const float shockSpeed = 13.0f * r;   // ring lingers near the hull (was 22 = gone in a frame)
    for (int i = 0; i < nShock; ++i) {
        float dx = frandSym(), dy = frandSym(), dz = frandSym();
        float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dl < 1e-4f) { dx = 1.0f; dy = 0.0f; dz = 0.0f; dl = 1.0f; }
        dx /= dl; dy /= dl; dz /= dl;
        Particle p;
        p.pos = center;
        const float sp = shockSpeed * (0.85f + frand() * 0.30f);
        p.vel = x3::phys::Vec3{ dx * sp, dy * sp, dz * sp };
        p.life = p.maxLife = 0.28f + frand() * 0.14f;
        p.size0 = 0.10f * r; p.size1 = 0.02f * r;
        p.r = 5.5f; p.g = 3.2f; p.b = 1.0f;      // hot leading-edge spark
        p.a0 = 1.0f; p.gravity = 0.0f; p.drag = 0.8f; p.additive = true;
        spawnParticle(p);
    }
    // (4) Dark rolling SMOKE so the blast leaves a believable plume in its wake.
    const int nSmoke = 14;
    for (int i = 0; i < nSmoke; ++i) {
        Particle p;
        p.pos = x3::phys::Vec3{ center.x + frandSym() * r * 0.5f,
                                center.y + frandSym() * r * 0.5f,
                                center.z + frandSym() * r * 0.5f };
        p.vel = x3::phys::Vec3{ frandSym() * 2.0f, frandSym() * 2.0f, frandSym() * 2.0f };
        p.life = p.maxLife = 1.2f + frand() * 1.4f;
        p.size0 = 0.5f * r; p.size1 = 2.0f * r;   // grows + dissipates
        p.r = 0.09f; p.g = 0.08f; p.b = 0.07f;    // sooty dark smoke
        p.a0 = 0.5f; p.gravity = 0.0f; p.drag = 0.7f; p.additive = false;
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

int CombatFx::liveFlameLickCount() const {
    int n = 0;
    for (const auto& l : m_licks) if (l.life > 0.0f) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// update: decay tracer lifetimes + the muzzle flash, integrate the particle pool
// (semi-implicit Euler with gravity + drag), and age the decals.
// ---------------------------------------------------------------------------
void CombatFx::update(float dt) {
    if (dt <= 0.0f) return;
    m_lastDt = dt;   // boltFx's rate-based lick emission reads the frame dt here
    for (auto& t : m_tracers) {
        if (t.life > 0.0f) {
            t.life -= dt;
            t.age  += dt;   // drives the Lightning bolt's propagation reach
            // Carry the beam along with the shooter (dt-scaled, never per-frame).
            // vel is zero for stationary shooters, so this is a no-op there.
            t.from.x += t.vel.x * dt; t.from.y += t.vel.y * dt; t.from.z += t.vel.z * dt;
            t.to.x   += t.vel.x * dt; t.to.y   += t.vel.y * dt; t.to.z   += t.vel.z * dt;
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
    // Flame licks: drift + BUOYANCY (fire curls upward as it ages — the rising
    // stretch axis is what bends the tongue tips up) + light damping. dt-scaled.
    for (auto& l : m_licks) {
        if (l.life <= 0.0f) continue;
        const float damp = 1.0f - 1.5f * dt;
        const float k = (damp < 0.0f) ? 0.0f : damp;
        l.vel.x *= k; l.vel.y *= k; l.vel.z *= k;
        l.vel.y += 3.0f * dt;                      // buoyant rise
        l.pos.x += l.vel.x * dt;
        l.pos.y += l.vel.y * dt;
        l.pos.z += l.vel.z * dt;
        l.life -= dt;
        if (l.life < 0.0f) l.life = 0.0f;
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
        // Optional birth->death color gradient (fire cools orange->red). r1 < 0
        // (the default) keeps the legacy constant color bit-for-bit.
        const float cr = (p.r1 >= 0.0f) ? p.r + (p.r1 - p.r) * age : p.r;
        const float cg = (p.r1 >= 0.0f) ? p.g + (p.g1 - p.g) * age : p.g;
        const float cb = (p.r1 >= 0.0f) ? p.b + (p.b1 - p.b) * age : p.b;
        PI inst;
        inst.pos[0] = p.pos.x; inst.pos[1] = p.pos.y; inst.pos[2] = p.pos.z;
        inst.size = size;
        inst.color[0] = cr; inst.color[1] = cg; inst.color[2] = cb; inst.color[3] = alpha;
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
        const float emis[4] = { 0.14f, 0.52f, 1.25f, 0.85f * brightness };  // LIGHT ELECTRIC BLUE corona
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
        // LIGHT ELECTRIC BLUE core (Tim 2026-07-26, headline: "another one, a light
        // bluish color!!!"). Was {1.3,1.9,3.0} which, at 2.5x, saturated toward WHITE
        // at the center. Red pulled DOWN and blue pushed UP so the hue stays a bright
        // blue-forward arc even where it blooms hot — reads as electric blue, not white.
        const float emis[4] = { 0.55f, 1.5f, 3.7f, 2.6f * brightness };     // light electric blue core
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
    // It inherits the parent's direction rotated 8-22 deg off-axis, takes a fraction of
    // the remaining parent length, subdivides with its OWN fractal detail (so branches
    // branch), and inherits REDUCED brightness + thickness. It ends in open air — only
    // the trunk is required to terminate on the hit point.
    if (branchDepth >= kLightningMaxBranchDepth) return;
    float chance = kLightningBranchChance;
    for (int i = 0; i < branchDepth; ++i) chance *= kLightningBranchDecay;
    // Only fork off the upper subdivision levels — forking at the finest scale just adds
    // fuzz, not structure.
    if (depth < 2 || rng() >= chance) return;

    // Rotate the parent's direction 8-22 deg off-axis about a random perpendicular.
    // (was 15-40 deg — owner "STILL goes quite wide": tighter fork angle so branches
    // hug the trunk instead of fanning the beam out into a wide web.)
    const float ang = (8.0f + rng() * 14.0f) * 3.14159265f / 180.0f;
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
            const float baseW = (t.width > 0.0f) ? t.width : kTracerThickness;
            float width = baseW * (0.5f + 0.5f * k);
            if (t.width > 0.0f) {
                // SHIP-SCALE bolt: HDR-hot so it BLOOMS as an energy bolt under
                // the space exposure (the 1.0-range on-foot tint reads olive).
                const float hot[4] = { 3.2f, 2.9f, 1.3f, 1.0f };
                drawTracerBillboard(device, frame, t.from, t.to, eyePos, width, hot);
            } else {
                drawTracerBillboard(device, frame, t.from, t.to, eyePos, width, tracerColor);
            }
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

    // ---- FLAME LICKS: velocity-stretched emissive fire tongues. --------------
    // Each live lick is a camera-facing ribbon whose LONG axis runs along its
    // velocity (forward down the stream, curling upward with age via buoyancy) —
    // the elongated shape the round particle billboards cannot make. HDR emissive
    // (drawMeshEmissive, black base) so bloom fuses overlapping licks into ONE
    // connected flame body; color ramps hot orange -> deep red as the lick dies
    // (the cooling read Tim confirmed), width tapers with age.
    for (const auto& l : m_licks) {
        if (l.life <= 0.0f) continue;
        const float t = (l.maxLife > 0.0f) ? 1.0f - l.life / l.maxLife : 1.0f;  // age 0->1
        x3::phys::Vec3 dir = l.vel;
        float vlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (vlen < 1e-4f) dir = x3::phys::Vec3{ 0.0f, 1.0f, 0.0f };
        else dir = x3::phys::Vec3{ dir.x / vlen, dir.y / vlen, dir.z / vlen };
        const float half = l.len * 0.5f;
        const x3::phys::Vec3 a{ l.pos.x - dir.x * half, l.pos.y - dir.y * half,
                                l.pos.z - dir.z * half };
        const x3::phys::Vec3 b{ l.pos.x + dir.x * half, l.pos.y + dir.y * half,
                                l.pos.z + dir.z * half };
        // Camera-facing width axis (same construction as the tracer ribbon).
        x3::phys::Vec3 view{ l.pos.x - eyeX, l.pos.y - eyeY, l.pos.z - eyeZ };
        const float vl = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
        if (vl > 1e-4f) view = x3::phys::Vec3{ view.x / vl, view.y / vl, view.z / vl };
        x3::phys::Vec3 w = cross(dir, view);
        const float wl = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
        if (wl < 1e-3f) {
            const x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f)
                ? x3::phys::Vec3{ 0, 1, 0 } : x3::phys::Vec3{ 1, 0, 0 };
            w = normalize(cross(ref, dir));
        } else {
            w = x3::phys::Vec3{ w.x / wl, w.y / wl, w.z / wl };
        }
        const x3::phys::Vec3 nrm = cross(dir, w);          // right-handed (KNOWN_BUGS R3)
        const float width = l.width * (1.0f - 0.40f * t);  // narrows as it dies
        const float blackBase[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        // Hot orange young -> deep red old; intensity fades with age. VALUE, NOT
        // LUMENS (docs/DECISIONS.md, re-learned on the first cut of this ribbon):
        // wide bright slabs clip to flat yellow planks — the flame look is a
        // SHARP, MODEST ribbon whose halo comes from bloom.
        const float fade = 1.0f - t;
        const float emis[4]    = { 2.4f - 1.4f * t,
                                   0.85f - 0.73f * t,
                                   0.12f - 0.095f * t,
                                   l.bright * (0.35f + 0.65f * fade) };
        // TWO segments, TAPERED: base half at full width, tip half narrowed +
        // dimmed — a constant-width rectangle reads as a plank; flames taper.
        const x3::phys::Vec3 mB{ (a.x * 3.0f + b.x) * 0.25f, (a.y * 3.0f + b.y) * 0.25f,
                                 (a.z * 3.0f + b.z) * 0.25f };  // base-quarter center
        const x3::phys::Vec3 mT{ (a.x + b.x * 3.0f) * 0.25f, (a.y + b.y * 3.0f) * 0.25f,
                                 (a.z + b.z * 3.0f) * 0.25f };  // tip-quarter center
        float model[16];
        composeTRS3(model, w, nrm, dir, width, width * 0.10f, l.len * 0.5f, mB);
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emis, model);
        const float emisTip[4] = { emis[0], emis[1], emis[2], emis[3] * 0.7f };
        composeTRS3(model, w, nrm, dir, width * 0.45f, width * 0.10f, l.len * 0.5f, mT);
        device.drawMeshEmissive(frame, m_box, x3::rhi::TextureHandle{}, blackBase, emisTip, model);
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
