// FISH — see app/fish.h. Kinematic schools in the world water.
#include "fish.h"

#include "mesh_prims.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265f;

// Append a local-space box to a compound mesh (render-only — fish carry no
// collision). Same helper the ecology's species meshes are built from.
void appendBox(x3::prims::PrimMesh& dst, float hx, float hy, float hz,
               float cx, float cy, float cz) {
    x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), b.verts.begin(), b.verts.end());
    for (uint32_t i : b.index) dst.index.push_back(base + i);
}

// THE FISH: a slim TAPERED body (nose -> shoulder -> flank -> peduncle), a
// forked TAIL FIN, a dorsal fin and a pectoral pair. Forward is +X, up is +Y;
// the whole thing is ~1 m long in local space and scaled by cfg.size * per-fish
// variance (so the default reads as a ~0.4 m river fish).
x3::prims::PrimMesh makeFishMesh() {
    x3::prims::PrimMesh m;
    // Body: four boxes tapering front (+X) to back (-X).
    appendBox(m, 0.14f, 0.055f, 0.035f,  0.36f, 0.0f, 0.0f);   // nose/head
    appendBox(m, 0.13f, 0.095f, 0.065f,  0.11f, 0.0f, 0.0f);   // shoulder (deepest)
    appendBox(m, 0.13f, 0.075f, 0.050f, -0.13f, 0.0f, 0.0f);   // flank
    appendBox(m, 0.09f, 0.040f, 0.025f, -0.33f, 0.0f, 0.0f);   // peduncle
    // Tail fin: a forked vertical blade behind the peduncle.
    appendBox(m, 0.08f, 0.10f, 0.010f, -0.48f,  0.05f, 0.0f);
    appendBox(m, 0.08f, 0.10f, 0.010f, -0.48f, -0.05f, 0.0f);
    // Dorsal fin + a pectoral pair (the read at a glance: it is a FISH).
    appendBox(m, 0.09f, 0.055f, 0.008f, -0.02f,  0.13f, 0.0f);
    appendBox(m, 0.05f, 0.008f, 0.055f,  0.10f, -0.03f,  0.08f);
    appendBox(m, 0.05f, 0.008f, 0.055f,  0.10f, -0.03f, -0.08f);
    return m;
}

inline float slew(float a, float target, float maxStep) {
    float d = target - a;
    while (d >  kPi) d -= 2.0f * kPi;
    while (d < -kPi) d += 2.0f * kPi;
    if (d >  maxStep) d =  maxStep;
    if (d < -maxStep) d = -maxStep;
    return a + d;
}

} // namespace

uint32_t FishSystem::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}
float FishSystem::frand() { return (float)(rng() % 10000u) * 0.0001f; }

float FishSystem::waterAt(float x, float z) const {
    return m_water ? m_water(x, z) : kFishDryTest * 2.0f;
}

float FishSystem::bedAt(float x, float z, float surface) const {
    return m_bed ? m_bed(x, z) : (surface - 30.0f);
}

void FishSystem::writeTransform(Fish& f, Scene& scene) {
    if (f.entity == kNoLink) return;
    Entity& e = scene.get(f.entity);
    const float s  = m_cfg.size * f.size;
    // Yaw about +Y (forward = local +X) composed with a ROLL about the forward
    // axis: a live fish rolls 0; a DEAD fish rolls pi — belly-up.
    const float roll = f.dead ? kPi : 0.0f;
    const float c = std::cos(f.yaw), sn = std::sin(f.yaw);
    const float cr = std::cos(roll), sr = std::sin(roll);
    float* t = e.transform;
    // M = Ry(yaw) * Rx(roll), column-major, uniformly scaled.
    t[0]  =  c * s;        t[1]  = 0.0f;      t[2]  = -sn * s;       t[3]  = 0;
    t[4]  =  sn * sr * s;  t[5]  = cr * s;    t[6]  =  c * sr * s;   t[7]  = 0;
    t[8]  =  sn * cr * s;  t[9]  = -sr * s;   t[10] =  c * cr * s;   t[11] = 0;
    t[12] = f.x; t[13] = f.y; t[14] = f.z; t[15] = 1;
}

void FishSystem::build(const FishConfig& cfg, Scene& scene,
                       x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_cfg = cfg;
    m_rngState = cfg.seed ? cfg.seed : 0xF15Fu;

    x3::prims::PrimMesh pm = makeFishMesh();
    m_mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                               pm.index.data(), (uint32_t)pm.index.size());

    uint32_t skipped = 0;
    for (const FishSchoolDesc& sd : m_cfg.schools) {
        const float surf = waterAt(sd.centerX, sd.centerZ);
        if (surf < kFishDryTest) {   // a school never spawns on land
            ++skipped;
            x3::logWarn("fish: school at (" + std::to_string(sd.centerX) + ", "
                        + std::to_string(sd.centerZ) + ") is DRY — skipped");
            continue;
        }
        FishSchool sc;
        sc.cx = sd.centerX; sc.cz = sd.centerZ; sc.heading = sd.heading;
        sc.speed = sd.speed;
        const uint32_t si = (uint32_t)m_schools.size();
        m_schools.push_back(sc);

        for (uint32_t k = 0; k < sd.count; ++k) {
            Fish f;
            f.school = si;
            // Slot: a jittered ring position in school-local space (+X = heading).
            const float ang = (float)k / (float)(sd.count ? sd.count : 1) * 2.0f * kPi
                            + frand() * 0.6f;
            const float rad = sd.spread * (0.35f + 0.65f * frand());
            f.slotX = std::cos(ang) * rad;
            f.slotZ = std::sin(ang) * rad;
            f.slotD = frand() * 1.4f;               // 0..1.4 m of extra depth
            f.size  = 0.82f + frand() * 0.42f;      // size variance
            f.phase = frand() * 6.2831853f;
            f.speed = sd.speed * (0.9f + frand() * 0.3f);
            const float ch = std::cos(sc.heading), sh = std::sin(sc.heading);
            f.x = sc.cx + f.slotX * ch - f.slotZ * sh;
            f.z = sc.cz + f.slotX * sh + f.slotZ * ch;
            const float ws = waterAt(f.x, f.z);
            const float surfHere = (ws < kFishDryTest) ? surf : ws;
            const float bed = bedAt(f.x, f.z, surfHere);
            f.y = surfHere - m_cfg.depthBelowSurf - f.slotD;
            if (f.y < bed + m_cfg.depthMin) f.y = bed + m_cfg.depthMin;
            f.yaw = sc.heading;

            Entity e;
            e.mesh = m_mesh;
            // Per-fish tint variance around the school's base color (silver /
            // olive / copper schools; each fish a shade off its neighbours).
            const float j = 0.82f + frand() * 0.36f;
            e.baseColor[0] = sd.tint[0] * j;
            e.baseColor[1] = sd.tint[1] * j;
            e.baseColor[2] = sd.tint[2] * j;
            e.baseColor[3] = 1.0f;
            // A wet flank catches the light. The river water is a 0.62-opacity
            // glass over a shadowed channel, so a purely lit fish reads as a black
            // blob from the bank (measured — the first proof shot failed exactly
            // that way). A real emissive term (the scale-flash) is what makes the
            // shoal READ as fish through the surface, above water and below it.
            e.emissive[0] = sd.tint[0] * 0.85f;
            e.emissive[1] = sd.tint[1] * 0.88f;
            e.emissive[2] = sd.tint[2] * 0.92f;
            e.emissive[3] = 1.15f;
            e.roomId  = m_cfg.roomId;
            e.visible = false;   // until the school is in range
            f.entity = scene.add(e);
            writeTransform(f, scene);
            m_fish.push_back(f);
        }
    }
    m_built = true;
    x3::logInfo("fish: built " + std::to_string(m_fish.size()) + " fish in "
                + std::to_string(m_schools.size()) + " schools ("
                + std::to_string(skipped) + " dry schools skipped)");
}

void FishSystem::update(float dt, Scene& scene, const x3::phys::Vec3& playerPos) {
    if (!m_built || dt <= 0.0f) return;
    m_time += dt;

    // ---- School centers: drift along the water, turning away from dry land ----
    for (FishSchool& sc : m_schools) {
        const float dx = playerPos.x - sc.cx, dz = playerPos.z - sc.cz;
        sc.active = (dx * dx + dz * dz) <= (m_cfg.activeRadius * m_cfg.activeRadius);
        if (!sc.active) continue;
        // Probe ahead: if the water ends there, fan out for a heading that stays
        // wet (deterministic sweep — no rng in update()).
        const float look = 8.0f;
        bool wet = waterAt(sc.cx + std::cos(sc.heading) * look,
                           sc.cz + std::sin(sc.heading) * look) > kFishDryTest;
        if (!wet) {
            for (int k = 1; k <= 10 && !wet; ++k) {
                const float step = (float)k * 0.32f;
                for (int sgn = -1; sgn <= 1 && !wet; sgn += 2) {
                    const float h = sc.heading + (float)sgn * step;
                    if (waterAt(sc.cx + std::cos(h) * look,
                                sc.cz + std::sin(h) * look) > kFishDryTest) {
                        sc.heading = h; wet = true;
                    }
                }
            }
            if (!wet) sc.heading += kPi;   // dead end: about-face
        }
        const float sp = sc.speed;
        sc.cx += std::cos(sc.heading) * sp * dt;
        sc.cz += std::sin(sc.heading) * sp * dt;
        // Never let the center itself leave the water (it would drag the school out).
        if (waterAt(sc.cx, sc.cz) < kFishDryTest) {
            sc.cx -= std::cos(sc.heading) * sp * dt;
            sc.cz -= std::sin(sc.heading) * sp * dt;
            sc.heading += kPi;
        }
    }

    // ---- Fish ----
    const uint32_t n = (uint32_t)m_fish.size();
    for (uint32_t i = 0; i < n; ++i) {
        Fish& f = m_fish[i];
        const FishSchool& sc = m_schools[f.school];
        if (f.gone) continue;
        if (!sc.active && !f.dead) {
            if (f.entity != kNoLink) scene.get(f.entity).visible = false;
            continue;
        }
        if (f.entity != kNoLink) scene.get(f.entity).visible = true;

        const float surf = waterAt(f.x, f.z);

        // ---- DEAD: belly-up, rise to the surface, drift, despawn ----
        if (f.dead) {
            f.deadT += dt;
            if (f.deadT >= m_cfg.deadLinger) {
                f.gone = true;
                if (f.entity != kNoLink) scene.get(f.entity).visible = false;
                continue;
            }
            if (surf > kFishDryTest) {
                // The pale BELLY rides PROUD of the water (a floating dead fish is
                // half out of it). It must: the water ribbon is normal glass, which
                // replays in the depth PRE-PASS (engine/rhi/vk/vk_passes.cpp — see
                // the drawMeshGlass comment), so anything strictly BELOW the surface
                // is depth-rejected from above. A corpse at surf + kDeadFloatProud
                // breaks the plane and reads from the bank, which is the whole point
                // of the aftermath.
                const float top = surf + 0.05f;
                if (f.y < top) f.y = std::min(top, f.y + m_cfg.deadRise * dt);
                const float nx = f.x + std::cos(sc.heading) * m_cfg.deadDrift * dt;
                const float nz = f.z + std::sin(sc.heading) * m_cfg.deadDrift * dt;
                if (waterAt(nx, nz) > kFishDryTest) { f.x = nx; f.z = nz; }
            }
            // A dead fish lolls: a slow yaw wander, no wiggle.
            f.yaw += 0.15f * dt;
            writeTransform(f, scene);
            continue;
        }

        // ---- ALIVE: flee / cohesion / separation / alignment ----
        const float pdx = f.x - playerPos.x, pdy = f.y - playerPos.y, pdz = f.z - playerPos.z;
        const float pd2 = pdx * pdx + pdy * pdy + pdz * pdz;
        if (pd2 < m_cfg.fleeRadius * m_cfg.fleeRadius) f.fleeT = m_cfg.fleeTime;
        else if (f.fleeT > 0.0f) f.fleeT -= dt;

        float dirX = 0.0f, dirZ = 0.0f, speed = f.speed;
        if (f.fleeT > 0.0f) {
            // BOLT: straight away from the player (planar), fast.
            const float len = std::sqrt(pdx * pdx + pdz * pdz);
            if (len > 1e-3f) { dirX = pdx / len; dirZ = pdz / len; }
            else { dirX = std::cos(f.yaw); dirZ = std::sin(f.yaw); }
            speed = m_cfg.fleeSpeed;
        } else {
            // COHESION: steer to my slot in the school (rotated by the heading).
            const float ch = std::cos(sc.heading), sh = std::sin(sc.heading);
            const float tx = sc.cx + f.slotX * ch - f.slotZ * sh;
            const float tz = sc.cz + f.slotX * sh + f.slotZ * ch;
            float ax = tx - f.x, az = tz - f.z;
            const float ad = std::sqrt(ax * ax + az * az);
            if (ad > 1e-3f) { ax /= ad; az /= ad; }
            // ALIGNMENT: blend the slot pull with the school heading (a school
            // swims together, it does not converge on a point).
            const float w = ad > 1.5f ? 0.75f : 0.25f;   // far from slot -> chase it
            dirX = ax * w + ch * (1.0f - w);
            dirZ = az * w + sh * (1.0f - w);
        }
        // SEPARATION: push off crowded schoolmates (same school only; n<=16).
        for (uint32_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const Fish& o = m_fish[j];
            if (o.school != f.school || o.dead || o.gone) continue;
            const float sx = f.x - o.x, sz = f.z - o.z;
            const float d2 = sx * sx + sz * sz;
            if (d2 < m_cfg.separation * m_cfg.separation && d2 > 1e-6f) {
                const float d = std::sqrt(d2);
                dirX += (sx / d) * 0.9f;
                dirZ += (sz / d) * 0.9f;
            }
        }
        const float dl = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (dl > 1e-3f) { dirX /= dl; dirZ /= dl; }
        else { dirX = std::cos(f.yaw); dirZ = std::sin(f.yaw); }

        // Advance — but a fish NEVER beaches: a step onto dry ground is refused.
        const float nx = f.x + dirX * speed * dt;
        const float nz = f.z + dirZ * speed * dt;
        if (waterAt(nx, nz) > kFishDryTest) { f.x = nx; f.z = nz; }

        // Depth: bounded between the bed and just under the surface.
        const float s2 = waterAt(f.x, f.z);
        if (s2 > kFishDryTest) {
            const float bed = bedAt(f.x, f.z, s2);
            float ty = s2 - m_cfg.depthBelowSurf - f.slotD;
            // A gentle vertical breathing so the shoal isn't a flat sheet.
            ty += std::sin(m_time * 0.7f + f.phase) * 0.12f;
            const float lo = bed + m_cfg.depthMin;
            const float hi = s2 - m_cfg.depthBelowSurf * 0.5f;
            if (ty < lo) ty = lo;
            if (ty > hi) ty = hi;
            f.y += (ty - f.y) * std::min(1.0f, 2.5f * dt);
        }

        // Facing + the TAIL WIGGLE (a sin sway on top of the travel yaw).
        const float want = std::atan2(dirZ, dirX);
        f.yaw = slew(f.yaw, want, m_cfg.turnRate * dt);
        const float wig = std::sin(m_time * m_cfg.wiggleHz * 6.2831853f + f.phase)
                        * m_cfg.wiggleAmp * (f.fleeT > 0.0f ? 1.6f : 1.0f);
        const float drawYaw = f.yaw + wig;
        const float keep = f.yaw;
        f.yaw = drawYaw;
        writeTransform(f, scene);
        f.yaw = keep;   // the wiggle is VISUAL only — it never steers the fish
    }
}

uint32_t FishSystem::killWithin(float cx, float cz, float radius) {
    if (!m_built) return 0;
    uint32_t killed = 0;
    const float r2 = radius * radius;
    for (Fish& f : m_fish) {
        if (f.dead || f.gone) continue;
        const float dx = f.x - cx, dz = f.z - cz;
        if (dx * dx + dz * dz <= r2) { f.dead = true; f.deadT = 0.0f; f.fleeT = 0.0f; ++killed; }
    }
    if (killed)
        x3::logInfo("fish: THE ZAP killed " + std::to_string(killed) + " fish");
    return killed;
}

uint32_t FishSystem::aliveCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (!f.dead) ++n;
    return n;
}
uint32_t FishSystem::deadCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (f.dead && !f.gone) ++n;
    return n;
}
uint32_t FishSystem::activeCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (m_schools[f.school].active && !f.gone) ++n;
    return n;
}

} // namespace x3::game
